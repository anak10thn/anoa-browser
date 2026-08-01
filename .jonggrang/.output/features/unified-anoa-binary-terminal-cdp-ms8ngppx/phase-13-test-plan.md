# Phase 13 — Test Plan

Feature: `unified-anoa-binary-terminal-cdp` (`anoa-browser terminal [--cdp URL]`)
Scope of the plan: `master...HEAD`, 19 commits, ~2,400 new lines under `src/terminal/`.

This is a plan, not an implementation. Phase 14 executes it.

---

## 1. What the branch already tests

| Suite | Where | Cases |
|---|---|---|
| Config / CLI | `tests/unit/test_config.cpp` | TERM-CFG-01…11 — every terminal flag parses into its own field, defaults hold, `--port`/`--auth-token` are not aliased either way, `--term-port 0`/`70000`/`--fps 0`/`--gfx bogus`/`wss://` are rejected, `ws://` and `http://` accepted |
| Headless assumption | `tests/unit/test_config.cpp` | TERM-GUI-01…03 — the app object is a plain `QCoreApplication`, `QImage` decodes PNG under it, downscale + `Format_RGB888` works. Now runs on Linux **and** macOS (`ci.yml` unit job is a matrix) |
| CDP transport | `tests/integration/terminal_cdp.test.js` (fake CDP endpoint, pty via `script(1)`) | 9 cases: `http://` discovery via `/json/list`, `ws://` direct dial with no discovery, CSS-viewport in the status bar, wheel `deltaY = -120`, click in CSS pixels under `deviceScaleFactor 2`, named keys carrying `key`+`code`+`windowsVirtualKeyCode`, one `Input.insertText` per burst, unreachable endpoint, empty `/json/list` |

That is genuinely good coverage of the *four silent capability gaps* the plan called out. The gaps below are everything else.

---

## 2. Gaps, ranked

### P0 — must land in phase 14

**G1. The default `/render/*` backend in terminal mode has zero automated coverage.**
The plan's own argument for shipping both backends was that the HTTP path is *"a known-good control to diff against"*. Nothing tests the control. `terminal_cdp.test.js` covers only `--cdp`; no suite ever starts `anoa-browser terminal` without it. The whole port from `select()` to `QSocketNotifier` + `QTimer` (commit `eb3e2c1`) is unverified end-to-end.

**G2. bug-001 (`parsePpm` unsigned underflow) has no regression test.**
`render_http_client.cpp` — `data.size() - pos < need` wraps to `SIZE_MAX`, so a hostile/truncated `P6 4000 4000 255` reads ~48 MB out of bounds. Filed in `bugs.md`, not fixed, not covered.

**G3. bug-002 (CSI escape split across two stdin reads) has no regression test.**
`terminal_ui.cpp processInput()` — an up-arrow split as `ESC [` then `A` types a literal `A` into the page. Directly contradicts the contract at `terminal_ui.h:91`.

**G4. Mode dispatch and the argv pre-scan are untested.**
Nothing checks that the bare `terminal` word is consumed, that `--help` does not echo it back, that the non-tty refusal fires with the documented message, or that the accepted limitation (`--profile-name terminal` is read as the subcommand) behaves as documented rather than silently drifting.

### P1 — should land in phase 14

**G5. Windows compile-out is asserted, never checked.** AGENTS.md states the verification (`g++ -E -DQ_OS_WIN … | grep -c runTerminal` must be 0) and no job runs it. A syntax check proves nothing here; this is the only cheap negative check available.

**G6. The Qt 6.4 API floor is unverified.** `find_package(Qt6 6.4)` is the floor; CI pins 6.7.3. The new subsystem is the largest block of Qt API added to this repo, `QWebSocket::errorOccurred` (6.5+) already needed a `QT_VERSION_CHECK` fallback once, and nothing prevents the next one.

**G7. `--gfx kitty` / `--gfx iterm` renderers are never exercised.** Under `script(1)` the viewer resolves to halfblock, so `renderGfx()`, the cell-size query and the hand-rolled base64 (code-quality finding #2) run in no test.

**G8. CDP connection loss mid-session is untested.** The endpoint closing after a successful attach is the common real-world case (Chrome quits, tab closes). Untested: does the status bar clear the link, does the process survive, does it exit cleanly.

**G9. `RenderHttpClient` has no socket timeouts (code-quality finding #3) and the cost changed.** A peer that accepts and goes quiet now wedges the *Qt event loop*, so Ctrl-C stops working. There is no test that the viewer stays interactive against a stalling peer.

### P2 — deferred, with the reason recorded

**G10. SIGWINCH / resize.** `script(1)` gives no way to resize the pty after launch; a real test needs `node-pty`, which the plan explicitly put out of scope ("a committed pty test harness … is its own piece of work"). Deferred, not silently dropped.

**G11. Static-artifact PNG decode.** `make release-static` excludes `imageformats` plugins; PNG is built into QtGui so it should work and was verified manually once (commit `ab0db6a`), but `release.yml` only runs on tags. Covering it in CI means a full static build per PR — not worth the minutes. Keep as a release-gate check.

**G12. CDP request timeout / out-of-order responses.** `CdpClient`'s 5 s timeout and id-correlation under reordering are real logic, but the fake endpoint would have to grow scheduling knobs. Low field-risk relative to cost.

---

## 3. Planned test cases

### 3.1 New unit target — pure byte parsers (closes G2, and finding #5)

`parsePpm()` and `pngDimensions()` are file-local statics, and `pngDimensions()` exists twice (`cdp_frame_backend.cpp:21`, `render_http_client.cpp:67`) — code-quality finding #5. One refactor makes both testable and removes the duplication:

- extract into `src/terminal/frame_bytes.{h,cpp}` taking `(const char *, size_t)`, used by both backends;
- add `anoa-terminal-bytes-lib` in `tests/unit/CMakeLists.txt`, linking **`Qt6::Core` + `Qt6::Gui` only**.

> **Constraint, load-bearing:** this must be a *separate* target. `anoa-config-lib` is `config.cpp` alone against `Qt6::Core`, and AGENTS.md forbids adding terminal sources to it.

| ID | Case | Why |
|---|---|---|
| TERM-BYTES-01 | `parsePpm("P6 1 1 255\n\xFF\x00\x00")` → 1×1, correct pixel | happy path |
| TERM-BYTES-02 | `parsePpm("P6 1 1 255")` (body ends at maxval) → **false**, no read past end | **bug-001**; today `pos == size+1` and the guard wraps |
| TERM-BYTES-03 | `"P6 4000 4000 255"` + 10 bytes of body → false | bug-001 at the reported magnitude; run under ASan to make the OOB fail loudly |
| TERM-BYTES-04 | header with `#` comments, multiple whitespace runs | the skipSpace path inherited unchanged |
| TERM-BYTES-05 | `w`/`h` of 0, negative, and non-numeric → false | wire-controlled dimensions |
| TERM-BYTES-06 | `pngDimensions()` on the real 4×2 fixture → 4×2; on truncated (<24 B) and on non-PNG magic → false | the parser both backends now share |

Phase 14 should build this target with `-fsanitize=address,undefined` in one CI step — an unsigned wrap plus an OOB read is exactly what UBSan/ASan exist for, and the guard is the bug.

### 3.2 New unit target — input parser (closes G3)

`TerminalUi::feedInput()` is already public; `processInput()` is the private worker. Add `anoa-terminal-ui-lib` (`terminal_ui.cpp` + `frame_bytes.cpp`, `Qt6::Core` + `Qt6::Gui`), drive it with a stub `FrameBackend` that records calls, and redirect stdout to a temp file so the paint does not corrupt the CTest log.

| ID | Case | Why |
|---|---|---|
| TERM-INPUT-01 | `feedInput("\x1b[")` then `feedInput("A")` → one arrow-up dispatch, no text | **bug-002** |
| TERM-INPUT-02 | `feedInput("\x1b")` alone then `"[A"` → same | the one-byte-tail case, documented as already working — pin it |
| TERM-INPUT-03 | SGR mouse report split as `"\x1b[<0;12;7"` + `"M"` → one click | bug-002's second symptom (`<0;12;7M` leaking as typed text) |
| TERM-INPUT-04 | `"abc"` in one read → one batched text dispatch, not three | the hard-won batching behaviour the plan flagged |
| TERM-INPUT-05 | `0x7F` and `0x08` both → Backspace | "Backspace-as-DEL(127)", one of the four regressions already paid for |
| TERM-INPUT-06 | `0x03` and `0x11` → `feedInput` returns false | Ctrl-C / Ctrl-Q quit |
| TERM-INPUT-07 | `mapCellToPage` outside the display map → false | already public, free to test |
| TERM-INPUT-08 | status-bar truncation with a multibyte em dash at `m_cols` | code-quality finding #7 — pins the fix if taken, documents it if not |

### 3.3 New integration suite — `terminal_http.test.js` (closes G1, G9)

A fake `/render/*` endpoint, structured as the sibling of the fake CDP one in `terminal_cdp.test.js`: same `script(1)` pty harness, same fixed `COLS`/`ROWS`, `freePort()` from `helpers.js` (already added on this branch), records every request. Reuse the harness by lifting `runViewer`/`waitForFrames`/`errorLines` out of `terminal_cdp.test.js` into `helpers.js`.

| ID | Case | Why |
|---|---|---|
| TERM-HTTP-01 | viewer paints from `/render/screenshot.ppm`, ≥2 frames arrive | the event-loop port (`select()` → `QTimer`) end-to-end |
| TERM-HTTP-02 | requested size is `COLS × (ROWS-1)*2` | the halfblock geometry contract |
| TERM-HTTP-03 | click at a known cell → `/render/click` at the expected page coords | the control that TERM-CDP's CSS-pixel test is diffed against |
| TERM-HTTP-04 | wheel-up → `/render/scroll` with **`+120`** | the other half of the sign story; asserting both sides in two suites is what makes the inversion a fact rather than a comment |
| TERM-HTTP-05 | typed burst → one batched `/render/type` | parity with TERM-INPUT-04 at the wire |
| TERM-HTTP-06 | `--term-token` → `Authorization: Bearer …` on every request | never verified anywhere |
| TERM-HTTP-07 | unreachable `--term-port` → exit non-zero, one prefixed stderr line | mirrors the CDP failure case |
| TERM-HTTP-08 | endpoint accepts then sends nothing; Ctrl-C within 5 s | **G9** — proves whether the loop wedges. Expected to **fail** until finding #3 is addressed; land it as the failing case that justifies the timeout, or land it `.fails()`-annotated with the bug filed |
| TERM-HTTP-09 | endpoint replies `P6 4000 4000 255` truncated → no crash, error surfaced | bug-001 at the integration level, complementing TERM-BYTES-03 |

### 3.4 Extensions to `terminal_cdp.test.js` (closes G8)

| ID | Case |
|---|---|
| TERM-CDP-10 | endpoint closes the socket after a successful attach → link cleared from the status bar, process still alive or exits non-zero with one prefixed line (pin whichever the code actually does, then state it in the header comment) |
| TERM-CDP-11 | `/json/list` with two `type: "page"` targets → attaches to the first and prints the list, per the plan's "printing the list when ambiguous" |
| TERM-CDP-12 | `/json/list` returns malformed JSON → one prefixed error line, non-zero exit (distinct from the empty-list case already covered) |

### 3.5 Mode dispatch — shell (closes G4)

Extend `tests/integration/port_layout.test.sh`, which already owns "the binary's CLI behaves" and needs no pty:

| ID | Case | Why |
|---|---|---|
| TERM-MODE-01 | `anoa-browser terminal` over a **pipe** → exit 1, stderr contains `stdin/stdout must be a terminal` | the guard every pty test depends on; cheap and needs no pty |
| TERM-MODE-02 | `anoa-browser terminal --help` → exit 0, does **not** contain the word `terminal` as a positional, **does** list `--cdp`, `--term-host`, `--fps`, `--gfx` | the pre-scan's argv shift-left, plus the shared-parser decision |
| TERM-MODE-03 | `anoa-browser --help` lists the same terminal flags | "both modes share one parser — that is the price of never overloading one" |
| TERM-MODE-04 | `anoa-browser terminal --cdp wss://x` → exit 1 with the TLS message, *through the real binary* | TERM-CFG-09 covers the harness path; this covers the shipped one |
| TERM-MODE-05 | `anoa-browser --profile-name terminal --help` → documents the accepted limitation | a known limitation with no test is indistinguishable from a regression later |

### 3.6 Build-shape checks (closes G5, G6)

| ID | Case | Where |
|---|---|---|
| TERM-BUILD-01 | `g++ -E -DQ_OS_WIN -std=c++17 src/main.cpp \| grep -c runTerminal` → 0 | new CI step in the shell-tests job; no Qt link needed |
| TERM-BUILD-02 | `src/terminal/*` absent from the Windows target: configure with `-DCMAKE_SYSTEM_NAME=Windows` or grep the generated build system | same job |
| TERM-BUILD-03 | Qt **6.4.3** syntax-only pass over `src/terminal/*.cpp` (`aqt` install + `-fsyntax-only`, no WebEngine needed) | new CI job, or a documented pre-release manual gate. This is the check that catches a `\since 6.5` API before a distro user does |

---

## 4. CI wiring

| Job | Change |
|---|---|
| `unit-tests` (matrix, exists) | add the two new CTest targets; one Linux-only ASan/UBSan run of `anoa-terminal-bytes-lib` |
| `integration-tests` (exists) | picks up `terminal_http.test.js` automatically — `fileParallelism: false` already holds, and the new suite binds only ephemeral ports via `freePort()` |
| `shell-tests` (exists) | TERM-MODE-01…05 in `port_layout.test.sh`; TERM-BUILD-01/02 as steps |
| new `qt-floor` job | TERM-BUILD-03 |
| `e2e`, `regression` | unchanged — both attach to a running browser and know nothing about terminal mode |

**Platform reality:** every pty-driven case is Linux-only. BSD `script` on macOS has no `-e` and takes different arguments, so `terminal_cdp.test.js` already skips there and `terminal_http.test.js` will too. macOS coverage of terminal mode is therefore the unit suites plus TERM-MODE-01…05 — worth stating in the phase-17 summary rather than leaving implied.

---

## 5. Exit criteria

**Phase 14 (testing) is done when:**
1. Every P0 case above exists and passes, *except* TERM-BYTES-02/03 and TERM-INPUT-01/03, which must **fail first** against current `master...HEAD` — they are regression tests for two filed, unfixed bugs. Land them red (`.fails()` / `QEXPECT_FAIL`) or land them green with the fix; do not land them green without one.
2. `make test QT_PREFIX=…` is clean and `-Wall -Wextra -Wpedantic` stays at zero warnings, including the new test targets.
3. `npx vitest run` in `tests/integration` is green on Linux and green-with-skips on macOS.
4. P1 cases exist or are re-filed as tasks with the reason.

**Phase 15 (coverage):** this repo has no gcov/lcov wiring and adding it for a Qt/WebEngine binary is its own piece of work. Coverage is therefore assessed as **behavioural checklist completion** against §3, not a line percentage. If a numeric gate is wanted, the two new unit libs are the only targets where gcov is cheap (`Qt6::Core`/`Gui` only, no WebEngine) — propose `--coverage` on those alone, ≥85 % of `frame_bytes.cpp` and the `processInput`/`mapCellToPage` region of `terminal_ui.cpp`.

**Blocking dependency:** bug-001 and bug-002 are open in `bugs.md` and were never converted to tasks — `jonggrang bug --feature` cannot run non-interactively here (phase-12 tooling note). Run `jonggrang bug convert --feature unified-anoa-binary-terminal-cdp-ms8ngppx` from an interactive terminal before phase 14, or phase 14 will be writing tests for bugs nothing is assigned to fix.

---

## 6. Explicitly not tested, and why

- **Resize / SIGWINCH** (G10) — needs `node-pty`; a committed pty harness is out of scope per the plan.
- **Static-artifact PNG decode** (G11) — release-gate check, not per-PR.
- **CDP request timeout and response reordering** (G12) — cost of the endpoint knobs exceeds the field risk.
- **`wss://` / TLS** — rejected at parse time by design; TERM-CFG-09 and TERM-MODE-04 cover the rejection, and there is nothing beyond it to test.
- **Terminal mode on Windows** — compiled out; covered negatively by TERM-BUILD-01/02.
- **Tabs, URL input** — out of scope for the feature.
- **`loadConfigFile()` with terminal options** — CLI-only in v1 by decision; a test would encode the opposite.
