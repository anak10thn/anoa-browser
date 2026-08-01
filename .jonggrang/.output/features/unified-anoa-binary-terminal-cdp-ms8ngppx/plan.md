---
feature: unified-anoa-binary-terminal-cdp
branch: feat/unified-anoa-binary-terminal-cdp
base: "master"
work_type: LARGE
description: Merge anoa-term into the anoa-browser binary as `anoa-browser terminal`, with an opt-in --cdp option to attach the terminal viewer to any external CDP endpoint
created_at: 2026-07-31T07:55:26.965Z
depth: deep
---

# Plan: Single Binary — `anoa-browser terminal [--cdp URL]`

## Approach
Ship one binary: `anoa-browser` gains a `terminal` positional subcommand, detected by the same raw-argv pre-scan in `src/main.cpp` that already handles `--headless` (it must run before `QApplication` exists, since `QCommandLineParser` needs a live application object). Terminal mode constructs `QCoreApplication` instead of `QApplication` — the primary use case is SSH with no display — and branches into a `runTerminal(config)` path that never builds `AnoaBrowser`/`HttpServer`/`CdpProxy`. The 838 lines of `tools/anoa-term/anoa_term.cpp` move into `src/terminal/`, split along their existing seams, with the blocking `select()` frame loop converted to `QCoreApplication::exec()` + `QSocketNotifier(STDIN_FILENO)` + a `QTimer` at `1000/fps`, because `QWebSocket` requires a live Qt event loop. Behind a small backend seam, the existing `/render/*` HTTP path stays the default (byte-identical behaviour when `--cdp` is omitted) and a new request/response-correlating CDP client — genuinely new code, since `CdpProxy` is a blind relay that never parses replies — becomes the opt-in path for attaching to any Chrome/Chromium/Playwright endpoint.

## Phases
1. **Entry point & config plumbing** — Pre-scan argv in `src/main.cpp` for the `terminal` positional alongside the existing `--headless` scan, choose `QCoreApplication` vs `QApplication` from it, and branch to a `runTerminal(config)` path. Add terminal fields to `Config` and register the terminal options on the shared parser so `parser.process()` stops rejecting them. Windows gets a clean "terminal mode is not supported on Windows" error, not a link failure. Covered by `tests/unit` (QTest) — the cheapest verification in the project, so it goes first.
2. **Terminal core port (HTTP backend, behaviour parity)** — Move `anoa_term.cpp` into `src/terminal/` split along its seams (termios/signals/render/input vs. transport), convert the `select()` frame loop to `QSocketNotifier` + `QTimer`, and route `SIGWINCH`/`SIGINT` into the Qt thread by polling the existing `volatile sig_atomic_t` flags from the frame timer. Exit criterion: `anoa-browser terminal` against a local `anoa-browser` is indistinguishable from `anoa-term`, including the four hard-won input behaviours (Ctrl-C/Ctrl-Q quit, Backspace-as-DEL(127), printable-byte batching per burst, last-event-before-hint status bar). Delete the standalone target and `tools/anoa-term/`.
3. **CDP client** — New `src/terminal/cdp_client.{h,cpp}`: `QWebSocket`, monotonic `id` allocation with response correlation, bearer auth via both `Authorization` header and `?token=`, `http://` discovery via `/json/list` before dialling `ws://`, and connection-loss/retry surfaced in the status bar. Modelled on the `QWebSocket` + pending-message-queue shape in `cdp_proxy.cpp:86–156`.
4. **CDP frame backend** — Implement the backend seam against CDP: `Page.captureScreenshot` → `QImage::loadFromData` → aspect-correct downscale for halfblock (PNG passed through untouched for kitty/iTerm), `Page.getLayoutMetrics` for the CSS-pixel display map including `deviceScaleFactor`, `Input.dispatchMouseEvent` press+release, `mouseWheel` with the inverted delta sign, `Input.insertText`, and a `dispatchKeyEvent` table covering the full 14-key set from `anoa_browser.cpp:220–258`.
5. **Build, packaging & release pipeline** — CMake: terminal sources conditionally added to `anoa-browser` (POSIX only), both `anoa-term` blocks removed. Then every downstream reference: 8 sites in `release.yml` (Linux `cp`/`chmod`/`patchelf`/smoke, macOS `lipo`/move-into-bundle/`codesign`/smoke), the Homebrew cask `binary` stanza and the Linux formula `bin.install_symlink`, and `resources/anoa-browser.sh`'s layout comment. Verify `make release-static` explicitly.
6. **Tests & documentation** — A fake CDP endpoint in `tests/integration` (`ws` is already a dependency, `sendCdp` already exists) to exercise `--cdp` without a real Chrome; config unit tests for the new options and mode dispatch; both `--help` smoke invocations in `release.yml` retargeted to `anoa-browser terminal --help`. README's `anoa-term` section rewritten around `anoa-browser terminal`, install/tarball text corrected, breaking change called out. Fix `AGENTS.md`'s wrong stack metadata and record the merged-binary convention.

## Key Decisions
- **Subcommand spelling is `terminal`**, not `term` — the user's final constraint overrides the earlier `--term` answer. A bare positional word detected in the raw argv scan before `QApplication`; `QCommandLineParser` has no positional registered today, so it is dispatched before `process()` rather than through it.
- **Application class in terminal mode: `QCoreApplication`** — the primary use case is SSH with no display, and `QApplication` aborts on a display-less box unless `QT_QPA_PLATFORM` is pre-set. `QImage` decoding works without a GUI application object. Must be confirmed in CI on Linux and macOS, not assumed; fallback is `QApplication` with a forced `QT_QPA_PLATFORM=offscreen` from the same pre-scan.
- **Both backends ship; `/render/*` stays the default** — omitting `--cdp` preserves the exact current behaviour, host and port. `--cdp` opts into the WebSocket path. This makes the feature purely additive and gives every CDP-mode bug a known-good control to diff against, which matters more than usual because nothing compiles locally.
- **`--cdp` accepts both `http(s)://host:port` and `ws://host:port/devtools/page/<id>`** — the `http://` form triggers a `/json/list` fetch and attaches to the first `type: "page"` target, printing the list when ambiguous. `wss://` requires Qt built with OpenSSL and is explicitly unsupported.
- **Option-name collisions resolved by distinct names** — `--port` and `--auth-token` keep their browser meaning (server listen port / server token) on the shared parser; terminal connection settings get unambiguous names (`--term-host`, `--term-port`, `--term-token`). `--fps` and `--gfx` are terminal-only and collision-free. No flag changes meaning based on mode.
- **Terminal options are CLI-only in v1** — `loadConfigFile()` is not extended. JSON and INI mirror every browser option today; adding a second mode's options to both doubles the drift surface for no demonstrated need.
- **Event loop: `QCoreApplication::exec()` + `QSocketNotifier` + `QTimer`** — signal handlers keep their `volatile sig_atomic_t` flags and are polled from the frame timer, the simplest correct hop to the Qt thread at 30 fps; no self-pipe needed.
- **Coordinates are CSS pixels on the CDP path** — `deviceScaleFactor` means the returned PNG is routinely larger than the viewport, so the display map must scale from terminal cells to CSS pixels, not image pixels, or every click lands off-target on a HiDPI endpoint.
- **Scroll sign is inverted between backends** — `/render/scroll` uses Qt `angleDelta` (`+120` = up, per `processInput`'s `btn==64`); CDP `mouseWheel` `deltaY` is the opposite convention. A silent UX bug with no error message, so it needs an explicit test rather than care.
- **Windows: terminal mode is compiled out and fails cleanly** — `termios.h`/`sys/select.h`/`TIOCGWINSZ` do not exist on MSVC, so terminal sources are conditionally added to `target_sources` and `anoa-browser terminal` on Windows prints an unsupported-platform message and exits non-zero. The Windows release job packages `build/Release/*` wholesale and needs no change.
- **The CDP client attaches to exactly one target in v1** — no `sessionId` routing or multi-target plumbing, since tab UI is out of scope.
- **Breaking change accepted and documented** — `anoa-term` disappears rather than shipping a deprecation shim; a shim would be either a second binary (defeating the merge) or a symlink whose `argv[0]` sniffing outlives its usefulness.

## Affected Areas
**Build / entry point**
- `CMakeLists.txt` — delete `add_executable(anoa-term …)` (126–130) and `install(TARGETS anoa-term …)` (163–167); add terminal sources to `target_sources(anoa-browser …)` under a POSIX guard
- `src/main.cpp` — argv pre-scan for `terminal`, application-class choice, `runTerminal(config)` branch
- `src/config/config.h`, `src/config/config.cpp` — terminal-mode fields, option registration, validation

**New code**
- `src/terminal/terminal_ui.{h,cpp}` — termios/raw mode, SIGWINCH, halfblock/iTerm/kitty rendering, status bar, SGR mouse + key input parsing
- `src/terminal/cdp_client.{h,cpp}` — `QWebSocket` CDP client with id/response correlation
- `src/terminal/render_http_client.{h,cpp}` — the existing `/render/*` path behind the backend seam

**Deleted**
- `tools/anoa-term/anoa_term.cpp` and the now-empty `tools/anoa-term/`

**Packaging / CI (every one names `anoa-term` today)**
- `.github/workflows/release.yml` — Linux L55 `cp`, L56 `chmod`, L58 `patchelf --set-rpath`, L152 bundle smoke; macOS L219–220 `lipo -archs`, L225–230 move-into-bundle step, L311 `codesign --entitlements`, L349 smoke
- `.github/homebrew/anoa-browser-linux.rb.tpl:15` — `bin.install_symlink libexec/"anoa-term"`
- `.github/homebrew/anoa-browser.rb.tpl:18` — `binary "#{appdir}/…/MacOS/anoa-term"`
- `resources/anoa-browser.sh:6` — bundle-layout comment

**Docs / tests**
- `README.md` — features bullet (L16), macOS install text (L38), Linux tarball example (L68), the whole "Terminal Viewer (`anoa-term`)" section (L265–320+)
- `AGENTS.md` — wrong stack metadata (claims node-typescript for a C++17/Qt6/CMake project) + merged-binary convention
- `tests/unit/` — config option and mode-dispatch tests
- `tests/integration/` — fake CDP endpoint exercising `--cdp`

**Referenced, not modified**
- `src/cdp/cdp_proxy.cpp` (QWebSocket template), `src/browser/anoa_browser.cpp:220–258` (named-key table), `src/http/http_server.cpp:342–388` (QImage scaling recipe)

## Risks
- **Event-loop model conflict (highest risk)** — the ported code is a blocking `select()` loop with synchronous HTTP; `QWebSocket` needs a live Qt event loop. Mitigation: restructure to `exec()` + `QSocketNotifier` + `QTimer` in phase 2 *before* any CDP work lands, so the loop is proven against the known-good HTTP backend first.
- **Nothing compiles or runs in this sandbox** — Qt6 is absent (`qmake6` missing, no `Qt6Core.pc`) and there is no `build/`. Every phase is a CI round-trip. Mitigation: each phase must land in a state where the existing `/render/*` path still demonstrably works, and phase 1 is chosen first precisely because `tests/unit` is the cheapest signal.
- **`QCoreApplication` may not suffice for `QImage` PNG decode on some platforms** — asserted, not verified. Mitigation: confirm in CI on Linux and macOS in phase 1; fallback is `QApplication` + forced `QT_QPA_PLATFORM=offscreen` from the same pre-scan.
- **`QCommandLineParser::process()` hard-exits on unknown options** — any unregistered terminal flag kills `anoa-browser terminal --gfx kitty` with "Unknown option". Mitigation: register every terminal flag on the shared parser in phase 1, covered by unit tests.
- **CDP/`/render/*` capability gap produces silent wrong behaviour** — inverted scroll sign, CSS-pixel vs image-pixel coordinates under `deviceScaleFactor`, PNG-only screenshots with no server-side scaling, and named keys needing `key`+`code`+`windowsVirtualKeyCode`+`text` per entry. None of these fail loudly. Mitigation: explicit integration tests against the fake CDP endpoint for scroll direction and click coordinates, not code review alone.
- **Static build excludes `imageformats` plugins** (`CMakeLists.txt:92`) — PNG is built into QtGui rather than a plugin so decode should work, but a regression only surfaces in the static artifact. Mitigation: `make release-static` verification is an explicit phase-5 exit criterion.
- **Linux packaging behaviour change** — `anoa-term` is a bare, Qt-free binary symlinked into `bin/`; terminal mode now needs `resources/anoa-browser.sh` to set `LD_LIBRARY_PATH`/`QT_PLUGIN_PATH`/`QTWEBENGINE_*`. The launcher already `exec`s with `"$@"`, so only docs and symlinks change — but the raw `libexec/anoa-browser` must never be invoked directly.
- **Breaking change for existing users** — `brew upgrade` will not necessarily remove a stale `bin/anoa-term` symlink, and the cask's `binary` shim only disappears on reinstall. Mitigation: state both in release notes.
- **POSIX-only sources inside a binary that also ships on Windows** — mitigated by conditional `target_sources` plus a clean unsupported-platform error path.
- **Regressing hard-won input behaviour during the port** — the `web-render-endpoint-*` progress.txt documents four bugs already paid for. Mitigation: read it before touching input handling; phase 2's exit criterion is parity with `anoa-term`, not just "it compiles".

## Alternatives Considered
- **Option 2 — Minimal-diff transplant, keep the `select()` loop and pump Qt with `processEvents()`**: not chosen. It delays every inbound CDP response by up to a full frame period and runs WebSocket housekeeping (ping/pong, reconnect backoff) on that same jittery schedule. Worse, it breaks the moment a CDP call must await a response inside an input handler — the natural fix there is a nested event loop, exactly the failure mode the option was picked to avoid. It converts a one-time restructuring cost into a permanent latency floor, and no other part of this codebase drives Qt from a foreign loop.
- **Option 3 — Server-side attach bridge, terminal stays HTTP-only while `anoa-browser` proxies to remote CDP**: not chosen. It fits `HttpServer`'s patterns beautifully and the deployment story not at all — pointing the terminal at someone else's Chrome would require running a second `anoa-browser` process as a bridge, i.e. more moving parts than today, and on an SSH box that bridge drags in the full Qt/WebEngine runtime just to relay screenshots. It also adds a network hop per frame on top of the existing per-frame TCP connection cost.

## Out of Scope
- **Tabs / a tab bar in the terminal** — investigated in discovery but not part of the merge-plus-option request, and it cannot work against `anoa-browser`'s own CDP (QtWebEngine has one `QWebEnginePage`; `Target.createTarget` is unsupported), so it would ship asymmetric. Deferred as its own feature.
- **An in-terminal URL input line** — same reasoning. `Page.navigate` lands in the CDP client as a primitive, but no UI is built on it.
- **`wss://` / TLS CDP endpoints** — requires Qt built with OpenSSL; rejected with a clear message.
- **Terminal mode on Windows** — compiled out; a port means replacing the entire termios/select/SIGWINCH layer.
- **Multi-view / real tab support inside `anoa-browser` itself** — a single-`QWebEngineView` architecture change unrelated to this merge.
- **Rewriting the hand-rolled HTTP/1.1 layer** — `http_server.cpp`'s 475-line `handleNewConnection()` chain stays exactly as it is.
- **Any `anoa-term` compatibility shim, symlink, or wrapper script** — users type `anoa-browser terminal`.
- **Terminal options in the JSON/INI config file** — CLI-only in v1.
- **A committed pty test harness** — CDP-mode coverage comes from the fake-CDP integration test plus retargeted `--help` smoke checks. A general pty harness is worthwhile but is its own piece of work.

## Dependencies
- **Qt6 components already linked into `anoa-browser`** (user explicitly approved Qt use in terminal mode, so these are free): `Qt6::WebSockets` (`QWebSocket`, plus `QNetworkRequest` raw headers for Bearer auth), `Qt6::Network` (`QNetworkAccessManager` for `/json/list` discovery, `QTcpSocket`), `Qt6::Core` (`QJson*`, `QUrl`, `QUrlQuery`, `QTimer`, `QSocketNotifier`, `QCommandLineParser`), `Qt6::Gui` via Widgets (`QImage`, `QByteArray::fromBase64/toBase64`), `Qt6::Test` for unit tests. No new `find_package(Qt6 …)` component is required, though listing `Gui` explicitly would be cleaner.
- **Existing patterns to build on**: the two-stage argv handling in `main.cpp:12–23`; the `QWebSocket` + `m_pendingMessages` queue in `cdp_proxy.cpp:86–97, 131–156`; CDP JSON via `QJsonDocument::fromJson(...)` / `toJson(Compact)` in `cdp_extensions.cpp`; the `QImage` → `Format_RGB888` scanline-copy scaling recipe in `http_server.cpp:342–388`; dual bearer-token auth (header or `?token=`) in `http_server.cpp:113–122` and `cdp_proxy.cpp:56–80`; config precedence and `QTextStream(stderr) << … << Qt::endl` + `::exit(1)` validation style in `config.cpp`.
- **Reusable source**: `tools/anoa-term/anoa_term.cpp` — termios/SIGWINCH/`detectGfx` (244–340), display map and cell→page coordinate mapping (342–414), `renderStatusBar`/`renderHalfblock`/`renderGfx` (416–514) and `processInput` (516–640) all port largely unchanged; the named-key set to reach parity is the 14-entry table in `anoa_browser.cpp:220–258`.
- **Test infrastructure**: `tests/unit` QTest against `anoa-config-lib` (a STATIC lib of `config.cpp` alone, so terminal sources must stay out of it), gated on `-DBUILD_TESTS=ON`, run via `make test` → `ctest`; `tests/integration` vitest with `fileParallelism: false`, `helpers.js` (`BINARY`, `startBrowser`, `waitForPort`, `openWs`, `sendCdp`) and the `ws` dependency for standing up a fake CDP endpoint; shell suites `port_layout.test.sh` / `extensions.test.sh`; `tests/e2e` Playwright + Puppeteer; `tests/regression/smoke.sh`; CI's 5 sequential jobs with Qt 6.7.3 and `QPA_PLATFORM=offscreen`.
- **Prior-session learnings** in `.jonggrang/.output/features/web-render-endpoint-*/progress.txt` (not `.jonggrang/progress.txt`, which is deleted) — required reading before touching input handling.

<!-- jonggrang:clarifications -->
## Clarifications
_Captured from the planning Q&A:_

Goal: User wants anoa-browser and anoa-term shipped as a single binary, with an option that lets the terminal viewer attach to a different/external CDP endpoint instead of only the anoa-browser instance it was launched with.

- **Yang dimaksud "konek ke CDP URL lain" itu yang mana?** → CDP beneran — attach ke Chrome/Chromium/Playwright mana pun lewat ws://host:port/devtools/... (Page.captureScreenshot + Input.dispatchMouseEvent/dispatchKeyEvent)
- **Bagaimana satu binary itu memilih mode browser vs terminal?** → Flag: `anoa-browser --term [--cdp URL]`
- **Setelah digabung, apakah kode mode terminal boleh memakai Qt (QWebSocket, QImage untuk decode PNG, QJson)?** → Boleh pakai Qt — WebSocket, JSON, dan decode PNG langsung ambil dari Qt yang sudah di-link
- **Binary `anoa-term` terpisah masih perlu di-ship atau dihapus sepenuhnya?** → Hapus total — mulai rilis berikutnya hanya ada `anoa-browser term`
- **Untuk CDP eksternal, seberapa lengkap kontrolnya harus jalan di iterasi ini?** → Setara sekarang: screenshot + klik + scroll + ketik + tombol khusus (Enter/Backspace/Tab/panah)
- **Ada constraint lain? (mis. target platform Windows untuk mode terminal, format --cdp URL yang diinginkan, auth/header untuk endpoint remote, atau pemilihan target/tab kalau CDP punya banyak page)** → posix only dulu aja, tambahin tab dan di term tambahin untuk input url dan namanya jangan term tapi terminal
