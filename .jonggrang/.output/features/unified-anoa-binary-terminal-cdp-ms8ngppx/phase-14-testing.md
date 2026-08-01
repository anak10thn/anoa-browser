# Phase 14 — Testing

Feature: `unified-anoa-binary-terminal-cdp` (`anoa-browser terminal [--cdp URL]`)
Executes the plan in `phase-13-test-plan.md`.

---

## 1. Result

| | |
|---|---|
| New test files | 4 (2 QTest targets, 1 vitest suite, 2 shell suites) |
| New cases | 38 automated checks + 6 CLI + 3 build-shape + 6 Qt-floor |
| Unit (`make test`) | **3/3 CTest targets pass**, zero compiler warnings under `-Wall -Wextra -Wpedantic` |
| Unit under ASan + UBSan | **clean** (both terminal targets) |
| Integration (`npx vitest run`) | **103/103 pass**, 8 files |
| Bugs closed | bug-001, bug-002 — both landed red first, then fixed |
| Bugs opened | bug-003 (the stalling-peer wedge, G9) |
| P0 gaps closed | G1, G2, G3, G4 — all four |
| P1 gaps closed | G5, G6, G7, G8; G9 covered by a committed red case + bug-003 |

---

## 2. What was built

### 2.1 A shared byte-parser module (closes G2, and code-quality finding #5)

`pngDimensions()` existed twice — byte for byte in `cdp_frame_backend.cpp:21` and
`render_http_client.cpp:67` — and `parsePpm()` was a file-local static nobody could
call. Both moved to **`src/terminal/frame_bytes.{h,cpp}`** taking `(const char *,
size_t)`, so the CDP backend passes a `QByteArray` and the HTTP backend a
`std::string` without either converting. The duplicate is gone and both parsers are
directly testable.

New CMake target **`anoa-terminal-bytes-lib`** (Qt6::Core only, POSIX-only, separate
from `anoa-config-lib` as AGENTS.md requires) plus `anoa-browser-test-frame-bytes`.

### 2.2 A TerminalUi unit target (closes G3 and G7)

New target **`anoa-terminal-ui-lib`** (`terminal_ui.cpp` + its two headers for
AUTOMOC, Qt6::Core) plus `anoa-browser-test-terminal-ui`. Driven through the public
`feedInput()` with a recording stub `FrameBackend`, `begin()` never called (so the
geometry stays at the 80x24 default and no terminal is taken), stdout redirected to
a temp file per test.

Two things this reaches that no pty suite can:

- **Chunk boundaries.** A pty delivers a keypress as one write. `feedInput()` takes
  the split as an argument, which is the only way bug-002 is reproducible at all.
- **`--gfx kitty` / `--gfx iterm`.** Under `script(1)` the viewer always resolves to
  halfblock, so `renderGfx()`, the 4096-byte kitty chunking and the hand-rolled
  base64 encoder ran in no test. The kitty payload is now reassembled from its
  chunks and compared against `QByteArray::toBase64()`, which is what actually
  validates the encoder.

### 2.3 `tests/integration/terminal_http.test.js` (closes G1, exercises G9)

The default `/render/*` backend had **zero** coverage — the plan's own "known-good
control to diff against" was the untested half. A fake `/render/*` endpoint, built
as the sibling of the fake CDP one: same pty harness, same geometry, same three
input assertions, so the two wires can be read side by side.

The pty harness (`launchViewer`, `waitUntil`, `viewerErrors`, `sgrPress`, the
geometry constants, the `script(1)` rationale) was lifted out of
`terminal_cdp.test.js` into `helpers.js` and both suites now share it.

### 2.4 CLI and build-shape suites (closes G4, G5, G6)

- **`port_layout.test.sh`** grew TERM-MODE-01…05: the non-tty refusal, the argv
  pre-scan not echoing `terminal` back in `--help`, both `--help` outputs listing
  the same terminal flags, `wss://` rejected by the shipped binary, and the accepted
  pre-scan limitation.
- **`tests/integration/build_shape.test.sh`** (new): `runTerminal` compiled out
  under `Q_OS_WIN`, and no `src/terminal/` reference escaping `if(NOT WIN32)` in
  either CMakeLists.txt.
- **`tests/integration/qt_floor.test.sh`** (new): syntax-only pass over
  `src/terminal/*.cpp` against the **declared** Qt floor rather than CI's 6.7.3.

### 2.5 CI

| Job | Change |
|---|---|
| `unit-tests` | builds all three test targets; new Linux-only ASan+UBSan step over the two terminal targets |
| `shell-tests` | new `build_shape.test.sh` step |
| `qt-floor` | **new job** — Qt 6.4.3 (`qtwebsockets` only, no WebEngine), runs `qt_floor.test.sh` |
| `integration-tests` | unchanged; picks up `terminal_http.test.js` automatically |

---

## 3. Case inventory

### 3.1 `tests/unit/test_frame_bytes.cpp` — 10 cases

| ID | Case | Status |
|---|---|---|
| TERM-BYTES-01 | `P6 1 1 255` + one pixel → 1x1, exact bytes | pass |
| TERM-BYTES-02 | body ends at the maxval → refused | **red first**, now pass |
| TERM-BYTES-03 | `P6 4000 4000 255` with no body, and with 10 bytes | **red first (SIGSEGV)**, now pass |
| — | body one byte short / exact / with trailing bytes | pass |
| TERM-BYTES-04 | `#` comments and whitespace runs in the header | pass |
| TERM-BYTES-05 | zero, negative, non-numeric, overflowing, wrong maxval, `P5`, empty, `P` | pass |
| TERM-BYTES-06 | PNG IHDR on the 4x2 fixture; truncated, JFIF magic, empty, signature-only | pass |
| — | PNG width 0, and width with the top bit set | pass |

Every buffer is an **exactly-sized heap allocation**, not a `QByteArray` or a string
literal — both keep a NUL terminator and round their allocation up, so an overread
off the end of one is invisible to both the assertion and ASan.

### 3.2 `tests/unit/test_terminal_ui.cpp` — 26 cases

| ID | Case | Status |
|---|---|---|
| TERM-INPUT-01 | `ESC [` then `A` → one arrow-up, no text | **red first**, now pass |
| TERM-INPUT-02 | `ESC` then `[A` → same (the already-working tail) | pass |
| — | all four arrows in one read, ESC[A..D order | pass |
| TERM-INPUT-03 | mouse report split after `ESC [` → one click, no leaked text | **red first**, now pass |
| — | mouse report split before the final byte (the branch that always worked) | pass |
| — | wheel +120 up / -120 down, and coordinates dropped off-page | pass |
| — | click outside the page image is dropped | pass |
| TERM-INPUT-04 | `abc` → one batched `sendText` | pass |
| — | a named key splits the batch; UTF-8 crosses as bytes | pass |
| TERM-INPUT-05 | DEL (127) and BS (8) both → backspace | pass |
| — | CR, LF, Tab | pass |
| TERM-INPUT-06 | Ctrl-C (3) and Ctrl-Q (17) → `feedInput` returns false | pass |
| — | the quit byte is consumed, text before it flushes | pass |
| TERM-INPUT-07 | `mapCellToPage` at both edges, one past each, negatives, empty map | pass |
| TERM-GFX-01 | kitty: chunked at 4096, payload reassembles to Qt's base64, one `m=0` | pass |
| TERM-GFX-02 | iTerm: `size=` is the image length, payload is the base64 | pass |
| TERM-GFX-03 | an identical frame is not retransmitted | pass |
| — | halfblock asks for `cols x (rows-1)*2`; gfx asks for a PNG | pass |
| — | status bar: complaint > link > frame-level fallback | pass |
| — | status bar: the link yields when the row is over budget | pass |
| TERM-INPUT-08 | status bar truncates on a **byte** boundary, splitting an em dash | pass (documents finding #7) |
| — | a row that fits is padded to the full width | pass |
| — | the last-input echo replaces the usage hint | pass |

### 3.3 `tests/integration/terminal_http.test.js` — 9 cases

| ID | Case | Status |
|---|---|---|
| THTTP-01 | repeated frames from `/render/screenshot.ppm`, status bar names the endpoint | pass |
| THTTP-02 | the 8x8 pre-flight probe, then `COLS x (ROWS-1)*2` on every frame | pass |
| THTTP-03 | click at a known cell → `/render/click` at the mapped page coords | pass |
| THTTP-04 | wheel-up → `/render/scroll?dy=120`; wheel-down → `-120` | pass |
| THTTP-05 | typed burst → one `/render/type`, no `/render/key` | pass |
| THTTP-06 | `--term-token` → `Authorization: Bearer` **and** `token=` on every request | pass |
| THTTP-07 | unreachable port → exit 1, one prefixed line, plus the unprefixed hint | pass |
| THTTP-08 | stalling endpoint → viewer stays interactive | **`it.fails`** — see §5 |
| THTTP-09 | `P6 4000 4000 255` truncated → refused at the probe, exit 1, "malformed PPM" | pass |

### 3.4 `tests/integration/terminal_cdp.test.js` — 12 cases (3 new)

TCDP-01…09 unchanged; the suite now imports its harness from `helpers.js`.

| ID | Case | Status |
|---|---|---|
| TCDP-10 | endpoint drops the socket after a successful attach → re-dials, resumes capturing, still exits cleanly on Ctrl-C | pass |
| TCDP-11 | two `type: "page"` targets → attaches to the **first** | pass |
| TCDP-12 | `/json/list` returns non-JSON → exit non-zero, one line, distinct from the empty-list message | pass |

### 3.5 Shell — 6 CLI + 3 build-shape + 6 Qt-floor

| ID | Case | Status |
|---|---|---|
| TERM-MODE-01 | `terminal` over a pipe → exit 1, "stdin/stdout must be a terminal" | pass |
| TERM-MODE-02a | `terminal --help` → exit 0, usage line ends at `[options]` | pass |
| TERM-MODE-02b | `terminal --help` lists all six terminal options | pass |
| TERM-MODE-03 | `--help` (browser mode) lists the same six | pass |
| TERM-MODE-04 | `terminal --cdp wss://…` → exit 1 naming the scheme | pass |
| TERM-MODE-05 | `--profile terminal` → exit 1, "Missing value" (the accepted limitation) | pass |
| TERM-BUILD-01 | `runTerminal` present on POSIX, absent under `-DQ_OS_WIN` | pass |
| TERM-BUILD-02 | every `src/terminal/` reference in CMakeLists.txt is inside `if(NOT WIN32)` | pass |
| TERM-BUILD-02b | the same for `tests/unit/CMakeLists.txt` | pass |
| TERM-BUILD-03 | all six `src/terminal/*.cpp` syntax-check against **Qt 6.4.3** | pass |

---

## 4. Bugs

### bug-001 — PPM guard underflow → unbounded OOB read. **Fixed.**

Landed red first and it is worth recording *how* red: TERM-BYTES-02 failed the
assertion, and TERM-BYTES-03 took **SIGSEGV** and killed the test binary mid-run.

The plan's TERM-BYTES-03 as written (`"P6 4000 4000 255"` + 10 bytes of body) does
**not** reproduce the defect — with a non-empty body `pos` stays inside the buffer
and the ordinary short-body check catches it. The body has to end exactly at the
maxval, which is the reproducer `bugs.md` actually names. The committed case asserts
both, and says why in a comment, so the distinction is not lost again.

Fix: `pos > len || (unsigned long long)(len - pos) < need`, `need` computed in
64-bit so a wire-chosen `w*h*3` cannot overflow the comparison, and `readInt()`
refuses a dimension past 10^8 rather than overflowing an `int` on the way in.
`pngDimensions()` was hardened at the same time — assembling a big-endian IHDR
field into a signed `int` shifts into the sign bit, which is undefined for a PNG
that legally declares a width with the top bit set.

### bug-002 — CSI sequence split after `ESC [` is dropped. **Fixed.**

Landed red first; the captured status bar in the failure replay shows the leak
directly (`typed "<`).

Fix: `if (i + 2 >= buf.size()) break;` immediately after the `buf[i + 1] == '['`
check. That also made the `i + 2 < buf.size()` conditions on the arrow and SGR
branches redundant, and both were dropped so the guard lives in one place.

### bug-003 — a stalling `/render/*` peer wedges the viewer. **Filed, open.**

New, found while writing THTTP-08. `RenderHttpClient::httpRequest()` sets no socket
timeouts, and since the frame loop became a `QTimer` callback that blocking `read()`
runs on the Qt event loop. A peer that accepts and then goes quiet takes everything
down with it: the `QSocketNotifier` never fires again so Ctrl-C is never read; ISIG
is off in raw mode so byte 3 is not a signal; and SIGTERM only sets a flag the
never-reached frame tick polls. **The process needs SIGKILL**, and SIGKILL does not
run the `atexit(restoreTerminal)` hook — so the user is left with a terminal in raw
mode on the alt screen.

Not fixed here: phase 14 is testing, and this is a transport change rather than a
regression in the code under test. It is committed as a running `it.fails()` case
(§5) so it cannot be forgotten.

---

## 5. The one deliberately-red case

`THTTP-08` is annotated `it.fails()`. That is not a skip — the case runs on every
push, and it passes **because** the viewer wedges. The day a socket timeout lands it
goes red, and the annotation plus the `describe` block above it are what gets
deleted along with bug-003. This is the plan's own sanctioned option for G9
("land it `.fails()`-annotated with the bug filed").

Everything else in the suite is green for the right reason.

---

## 6. Not covered, and why

Unchanged from the plan's §6, with one addition:

- **Resize / SIGWINCH** (G10) — needs `node-pty`; a committed pty harness is out of
  scope per the plan.
- **Static-artifact PNG decode** (G11) — release-gate check, not per-PR.
- **CDP request timeout and response reordering** (G12) — the fake endpoint would
  need scheduling knobs; low field-risk relative to cost.
- **`wss://` / TLS** — rejected at parse time by design; TERM-CFG-09 and
  TERM-MODE-04 cover the rejection and there is nothing beyond it.
- **Terminal mode on Windows** — compiled out; covered negatively by TERM-BUILD-01/02.
- **Tabs, URL input**, **`loadConfigFile()` with terminal options** — out of scope
  by decision.
- **New:** the status bar's optional **link field is not observable in the pty
  suites.** At the harness's 100 columns the row is already over budget before any
  link, so the link yields by design and is never painted. TCDP-10 and TCDP-11
  therefore read the wire (a re-dial, which target was chosen) rather than the bar,
  and the yielding rule itself is covered where it *is* observable — the TerminalUi
  unit suite at 80 columns.

---

## 7. Platform reality

Every pty-driven case is **Linux-only**. BSD `script` on macOS has no `-e` and takes
different arguments, so both `terminal_cdp.test.js` and `terminal_http.test.js` skip
there. macOS coverage of terminal mode is therefore:

- the two new QTest targets (the `unit-tests` job is already a Linux/macOS matrix),
- `test_config.cpp`'s TERM-CFG and TERM-GUI cases,
- TERM-MODE-01…05, which need no pty.

Worth stating in the phase-17 summary rather than leaving implied.

---

## 8. Sandbox notes for the next agent

- **`port_layout.test.sh`'s six PORT-\* cases fail in this sandbox and did before
  this phase.** `nc` is not installed, and `wait_for_port()` is built on `nc -z`, so
  every case that starts a browser reports "Browser failed to start" even when the
  binary comes up (its "DevTools listening" line is right there in the output).
  Baseline on an unmodified file: 2 passed / 6 failed. With TERM-MODE-01…05 added:
  8 passed / 6 failed — the same six. CI's ubuntu-22.04 has netcat.
- **`jonggrang bug --feature <id>` cannot file a bug here.** The global option parser
  consumes `--feature` into `WORK_FEATURE_ID` before `cmdBug()` sees it, so with more
  than one feature present it always exits "Multiple features found." bug-003 was
  appended to `bugs.md` by hand in the CLI's own format. Also: `parseBugsFile()`
  understands only `[open]` and `[task:<id>]` — there is no `[fixed]`, and an
  unrecognised header is dropped from the parse entirely, so bug-001 and bug-002 stay
  `[open]` with RESOLVED recorded in the body.
- **Qt 6.4.3 for the floor check** installs in ~20 s here:
  `aqt install-qt linux desktop 6.4.3 gcc_64 -m qtwebsockets -O /opt/Qt`.
  No WebEngine needed.
- **`make lint` skips** — clang-tidy is not installed in this sandbox.

---

## 9. Exit criteria

| Criterion | Status |
|---|---|
| 1. Every P0 case exists and passes; TERM-BYTES-02/03 and TERM-INPUT-01/03 fail first | **met** — all four confirmed red against the pre-fix tree (two assertion failures, one SIGSEGV, one leak visible in the captured bar), then landed green with the fix |
| 2. `make test` clean, zero warnings including the new targets | **met** — 3/3, `-Wall -Wextra -Wpedantic` silent |
| 3. `npx vitest run` green on Linux, green-with-skips on macOS | **met** on Linux (103/103); the macOS skip is structural (`describe.skipIf(!HAVE_UTIL_LINUX_SCRIPT)`) |
| 4. P1 cases exist or are re-filed with the reason | **met** — G5/G6/G7/G8 landed; G9 is a running red case plus bug-003 |

Phase 15 (coverage) is assessed as behavioural checklist completion against §3, per
the plan. If a numeric gate is wanted later, `anoa-terminal-bytes-lib` and
`anoa-terminal-ui-lib` are now the two targets where gcov is cheap — Qt6::Core only,
no WebEngine, and the sanitizer option in `tests/unit/CMakeLists.txt` shows the shape
a `--coverage` option would take.
