# Phase 17 — Completion

Branch `feat/unified-anoa-binary-terminal-cdp`, 23 commits ahead of
`origin/master`. All 18 tasks completed.

## Verification

Every suite in the repository was run. All of them pass.

| Suite | Command | Result |
|---|---|---|
| Unit (QTest + CTest) | `make test QT_PREFIX=/opt/Qt/6.7.3/gcc_64` | **3/3** — ConfigTests, FrameBytesTests, TerminalUiTests |
| Coverage gate | `make coverage` | **95.61%** total, gate is 80 — config.cpp 100%, frame_bytes.cpp 100%, terminal_ui.cpp 92.68% |
| Compiler warnings | same build, `-Wall -Wextra -Wpedantic` | **0** |
| Integration (vitest) | `npx vitest run` | **103/103** across 8 files |
| Integration (bash) | `port_layout.test.sh` | **14/14** |
| Integration (bash) | `extensions.test.sh` | **4/4** |
| Build shape | `build_shape.test.sh` | **3/3** — Windows compile-out holds |
| Qt floor | `qt_floor.test.sh` (Qt 6.4.3) | **6/6** — all six `src/terminal` sources compile at the declared floor |
| Regression | `tests/regression/smoke.sh` | **4/4** |

E2E (`tests/e2e`, Playwright + Puppeteer) was not run: it needs
`npx playwright install --with-deps chromium`, a network download this
environment does not have. It is unchanged by this branch.

## A correction to the record

Phases 14–16 recorded that `port_layout.test.sh` fails 6/14 here because the
sandbox has "no GL, no GPU, so WebEngine cannot start", and `.jonggrang/progress.txt`
told the next agent **"Do not chase these here."** That was wrong, and the advice
was the dangerous kind — it labels a real failure as environmental and would have
absorbed a genuine regression silently.

The actual cause is that `nc` was not installed. `wait_for_port()` in all three
bash suites is `while ! nc -z 127.0.0.1 "$port"`, so with no netcat the probe can
never succeed and every browser-launching case reports "Browser failed to start".
The misreading was easy to make because a headless run prints `QRhiGles2: Failed
to create context`, `QVulkanInstance: Failed to initialize Vulkan` and `Unable to
detect GPU vendor` to stderr — which makes the GPU story look obvious.

Disproved directly: launched the binary by hand and `curl`ed it — `/json/version`
answered in 2 seconds with `anoa-browser/0.3.1`. After
`apt-get install -y netcat-openbsd`, port_layout went 6-fail → **14/14**, and
`smoke.sh` went from a fatal start failure → **4/4**.

Corrected in `.jonggrang/progress.txt`; the `nc` requirement is now recorded in
AGENTS.md under both *Dependencies* and *Known Gotchas*.

## Cleanup

- Deleted two misplaced jonggrang artifact directories created by running the CLI
  from the wrong cwd: `tests/integration/.jonggrang/` and
  `.jonggrang/.output/features/.jonggrang/`. The canonical copies at
  `.jonggrang/.ephemeral/` and `.jonggrang/codemap/` were confirmed present first.
- Reverted `aqtinstall.log`, a tracked debug log this branch had appended 158
  lines of aqt download chatter to.

Left alone deliberately: the uncommitted jonggrang tooling self-update
(`.jonggrang/lib/*`, `hooks/*`, `.opencode/plugins/*`, `CLAUDE.md`,
`.jonggrang/jonggrang.json`) and other features' output files. None of it belongs
to this feature and folding it into this PR would obscure the diff.

## Known follow-ups, carried not fixed

These are stated in the PR body rather than fixed here, because fixing them at
the completion phase means re-opening a closed review phase and re-running the
whole suite against unreviewed code.

**bug-003 — open, and the one with teeth.** `RenderHttpClient::httpRequest()`
sets no socket timeouts, so a `/render/*` peer that accepts and then goes quiet
wedges the Qt event loop; Ctrl-C, SIGINT and SIGTERM are all unreachable and the
process needs SIGKILL, which skips `atexit(restoreTerminal)` and leaves the
terminal in raw mode on the alt screen. `THTTP-08` is committed as `it.fails()`
and passes *because* of the wedge — it turns red the moment a timeout lands.

**Three phase-12 code-quality findings not applied** (findings 1, 2 and 6; 3 is
bug-003, 5 was resolved when phase 14 extracted `frame_bytes.cpp`):

1. `Config::terminalMode` is written at `main.cpp:56` and never read — a mode
   gate that gates nothing.
2. `terminal_ui.cpp:28` hand-rolls 30 lines of base64 that `QByteArray::toBase64()`
   already provides. It predates the binary merge, when the file had no Qt
   dependency. Note this path is *not* covered by the pty suites, which force
   `--gfx halfblock`.
6. `kErrPrefix` is defined identically in `cdp_client.cpp:36` and
   `terminal_app.cpp:50`, with a comment on one saying it must match the other.

Findings 4 (unused public API) and 7 (byte-wise vs character-wise status-bar
truncation) were reviewed in phase 12 and judged acceptable as documented.

**bug-001 and bug-002 are fixed** (phase 14, both confirmed red first) but remain
`[open]` in `bugs.md` — the jonggrang bug parser recognises only `[open]` and
`[task:<id>]`, so a resolved-but-never-tasked bug has to stay `[open]` or vanish
from `jonggrang bug list`. Resolution is recorded in each body, prefixed RESOLVED.
