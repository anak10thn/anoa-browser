One binary: fold `anoa-term` into `anoa-browser terminal`, and give it a CDP transport

## What this does

`anoa-term` is gone. The terminal viewer is now a mode of the single
`anoa-browser` executable, reached as the bare positional word `terminal`, and it
can drive the page over **either** the existing `/render/*` HTTP endpoints or an
external CDP endpoint selected with `--cdp`.

```bash
anoa-browser --headless --no-sandbox --port 9222   # host the page
anoa-browser terminal --term-port 9222             # view it, over /render/*
anoa-browser terminal --cdp http://host:9222       # view it, over CDP
```

23 commits, ~10,300 insertions / ~970 deletions across 49 files, of which
`tools/anoa-term/anoa_term.cpp` (838 lines) is deleted outright.

## How it fits together

- **`terminal` is detected by a raw-argv pre-scan in `main.cpp`, before any
  application object exists.** It cannot be a `QCommandLineParser` positional,
  because the parser needs a live `QCoreApplication` and *which application class
  to construct* is exactly what the word decides — terminal mode builds a
  `QCoreApplication` (the primary use case is SSH with no display, where
  `QApplication` aborts), browser mode a `QApplication`. The scan also removes the
  word from argv so `--help` does not echo it back.
- **Transports sit behind a `FrameBackend` seam**, so `terminal_ui.cpp` never
  names HTTP or CDP and the two are genuinely swappable. The seam is asynchronous
  by necessity — a WebSocket transport cannot answer a frame request
  synchronously without a nested event loop — so it is `frameReady` /
  `frameFailed` signals, not a blocking call.
- **The frame loop is a Qt timer now, not `select()`.** This is what let the CDP
  transport exist at all, and it is also what makes the open bug below matter
  more than it used to.
- **No flag changes meaning by mode.** `--port` and `--auth-token` keep their
  browser meaning in both modes; the terminal's *connection* settings get their
  own `--term-host` / `--term-port` / `--term-token` names. Both modes share one
  parser, so every flag appears in both `--help` outputs — that is the price of
  never overloading one.
- **Terminal sources are POSIX-only and compiled out on Windows** (`if(NOT WIN32)`
  in CMake, plus a runtime message under `Q_OS_WIN`). `build_shape.test.sh`
  verifies this negatively — a syntax check would prove nothing.

## Verification

Every suite in the repo was run. All pass.

| Suite | Result |
|---|---|
| Unit (QTest + CTest) | **3/3** — ConfigTests, FrameBytesTests, TerminalUiTests |
| Coverage gate (`make coverage`, min 80) | **95.61%** — config.cpp 100%, frame_bytes.cpp 100%, terminal_ui.cpp 92.68% |
| Compiler warnings (`-Wall -Wextra -Wpedantic`) | **0** |
| Integration (vitest, 8 files) | **103/103** |
| Integration (bash: port_layout, extensions, build_shape, qt_floor) | **27/27** |
| Regression (`smoke.sh`) | **4/4** |
| Qt 6.4.3 floor | **6/6** — all six `src/terminal` sources compile at the declared floor, which CI's 6.7.3 would not catch |

E2E (Playwright + Puppeteer) was not run — it needs a chromium download the
build environment cannot reach. It is unchanged by this branch.

## Two bugs found and fixed on the way

Both were confirmed **red first** against the pre-fix parser.

- **bug-001 — unbounded out-of-bounds read in the PPM parser.** The pixel-length
  guard was `size_t` arithmetic that wrapped to `SIZE_MAX` and therefore always
  passed, so a hostile or truncated `P6 4000 4000 255` from the `--term-host`
  endpoint copied ~48 MB from past the end of the buffer. Inherited unchanged
  from `anoa_term.cpp`. The parser moved to `src/terminal/frame_bytes.cpp`, the
  guard now tests `pos > len` first, `need` is computed in 64-bit so a
  wire-chosen `w*h*3` cannot overflow the comparison, and dimensions past 10^8
  are refused on the way in.
- **bug-002 — split escape sequences were typed into the page as literal text.**
  A CSI sequence arriving as exactly `ESC [` on one read and the remainder on the
  next fell through to `i += 2` and dropped the prefix, so an up-arrow typed the
  character `A` and a mouse report leaked `<0;12;7M` — contradicting the
  documented contract that partial sequences are buffered.

## Known follow-ups — please read before merging

**bug-003 is open and is the one with teeth.** `RenderHttpClient::httpRequest()`
sets no socket timeouts, so a `/render/*` peer that accepts and then goes quiet
blocks *on the Qt event loop*. Everything that would let a user out depends on
that loop still turning: the stdin `QSocketNotifier` never fires so Ctrl-C is
never read, `ISIG` is off in raw mode so byte 3 is not a signal either, and
SIGTERM only sets a flag the never-reached frame tick polls. The process survives
Ctrl-C, SIGINT and SIGTERM alike and needs SIGKILL — which skips
`atexit(restoreTerminal)` and leaves the terminal in raw mode on the alt screen.

This was survivable when the frame loop was a blocking `select()`; it became
serious when the loop became a `QTimer` callback. `THTTP-08` is committed as
`it.fails()` and passes *because* the wedge happens, so it turns red the moment a
timeout lands. The fix is `SO_RCVTIMEO`/`SO_SNDTIMEO` plus a bounded `connect()`,
reported through `frameFailed()`.

**Three code-quality findings carried, not fixed** — deliberately, because
applying them at the completion phase would put unreviewed code behind a review
phase that has already closed:

1. `Config::terminalMode` is written at `main.cpp:56` and never read — a mode gate
   that gates nothing.
2. `terminal_ui.cpp:28` hand-rolls 30 lines of base64 that `QByteArray::toBase64()`
   already provides. It predates the binary merge, when that file deliberately had
   no Qt dependency. Note this path is *not* covered by the pty suites, which
   force `--gfx halfblock`.
3. `kErrPrefix` is defined identically in `cdp_client.cpp:36` and
   `terminal_app.cpp:50`, with a comment on one saying it must match the other.

## Accepted limitations

- **`--profile-name terminal` cannot work.** The pre-scan has no option-arity
  knowledge, so the word is taken as the subcommand wherever it appears. Pinned by
  `TERM-MODE-05` so it is not rediscovered as a regression.
- **Terminal options are CLI-only** — `loadConfigFile()` is deliberately not
  extended, so nothing in `--config` can set them.
- **The status bar's link field is invisible at 100 columns.** The row is over
  budget before any link, so the link yields by design.
- `wss://` CDP endpoints are refused with an explicit message; Qt is not built
  with OpenSSL here.

## A correction to the branch's own record

Phases 14–16 recorded that `port_layout.test.sh` fails 6/14 in the build sandbox
because it has "no GL, no GPU, so WebEngine cannot start", and left the next agent
the note **"Do not chase these here."** That was wrong. The three bash suites wait
on readiness with `while ! nc -z ...` and `nc` was not installed, so every
browser-launching case failed identically — and a headless start prints
`QRhiGles2: Failed to create context` and `Unable to detect GPU vendor` to stderr,
which made the GPU story look obvious. Curling the port by hand answered in two
seconds. With netcat installed: port_layout **14/14**, smoke.sh **4/4**.

The note is corrected and the `nc` requirement is now in AGENTS.md under both
*Dependencies* and *Known Gotchas*. Flagging it here because a false
environmental excuse in a progress log is where the next real regression hides.
