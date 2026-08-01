# Phase 15 — Coverage

Feature: `unified-anoa-binary-terminal-cdp` (`anoa-browser terminal [--cdp URL]`)
Threshold: **80%** (`.jonggrang/jonggrang.json` → `testing.coverage_threshold`)

---

## 1. Result

| | |
|---|---|
| Verdict | **PASS** — every measured file is at or above 80% |
| Aggregate | **631 / 660 source lines = 95.61%** |
| Unit suite | **3/3 CTest targets pass**, 91 cases (was 68) |
| New cases | 23 (11 TerminalUi, 12 config) |
| Warnings | zero, under `-Wall -Wextra -Wpedantic` |
| ASan + UBSan | still clean with the new cases |
| Bugs found | none — every new case passed on first run against the current tree |

Phase 14 left this phase a numeric gate as an option rather than a plan
("`anoa-terminal-bytes-lib` and `anoa-terminal-ui-lib` are now the two targets
where gcov is cheap"). It is now built, wired into CI, and green.

### Before and after

| File | Before | After | Δ |
|---|---|---|---|
| `src/config/config.cpp` | 70.09% | **100.00%** (210/210) | +29.9 |
| `src/terminal/frame_bytes.cpp` | 100.00% | **100.00%** (54/54) | — |
| `src/terminal/terminal_ui.cpp` | 76.32% | **92.68%** (367/396) | +16.4 |
| **Aggregate** | **76.1%** | **95.61%** | **+19.5** |

Two of the three files were **under threshold** on the first measurement. That
is the finding of this phase: phase 14's behavioural checklist was complete, and
the code underneath it still had two files below the gate.

---

## 2. What was built

### 2.1 `ANOA_TEST_COVERAGE` (`tests/unit/CMakeLists.txt`)

A gcov option shaped like the existing `ANOA_TEST_SANITIZE`, applied through a
new `anoa_test_target_coverage()` helper to all six unit targets — the three
libraries *and* the three executables that link them, because `--coverage` is
both a compile flag (emit the `.gcno` arc graph) and a link flag (pull in
libgcov, which writes the counts at exit).

The two options are **mutually exclusive** and CMake says so with a
`FATAL_ERROR` rather than producing a build whose arc counts are quietly
distorted by the sanitizer's instrumentation.

### 2.2 `tests/coverage.sh` — the gate

Configure → build the three targets → delete stale `.gcda` → `ctest` → `gcov` →
per-file table → non-zero exit under `COVERAGE_MIN` (default 80).

Two things it gets right that a hand-run `gcov` does not:

- **It deletes `.gcda` first.** libgcov *merges* into an existing file, so
  without this the reported coverage is a function of how many times the suite
  has been run since the last clean.
- **It counts the per-line listing, not gcov's file summary.** gcov's
  `Lines executed:N% of M` inflates `M` with Qt template instantiations
  (`QArrayDataPointer<QString>`, `QGenericArrayOps<QString>::moveAppend`, …)
  that land in the translation unit but are not lines of the source file. For
  `config.cpp` that is 14 phantom lines — the difference between the honest
  **100.00%** and gcov's headline **93.75%**. The script header records this so
  the next reader does not "fix" it back.

### 2.3 Wiring

| Where | Change |
|---|---|
| `make coverage` | new target, own build dir (`build-coverage`), `COVERAGE_MIN` overridable |
| `make clean-all` | now removes `build-coverage` too |
| `.gitignore` | `build-coverage/`, `*.gcov`, `*.gcda`, `*.gcno` |
| CI `unit-tests` | new Linux-only **Check unit-test line coverage** step + `.gcov` artifact upload (7-day retention) |

The gate was verified in both directions: green at `COVERAGE_MIN=80`, and
**exits 1** at `COVERAGE_MIN=95` naming `terminal_ui.cpp`. A gate that has never
been seen to fail is not a gate.

---

## 3. New cases

### 3.1 `tests/unit/test_terminal_ui.cpp` — 11 cases (26 → 37)

Two new fixtures make the previously-unreachable code reachable without a
terminal, which is what preserves phase 14's rule that this suite never calls
`begin()` on a real tty:

- **`RedirectedStdin`** — puts a temp file on fd 0. A regular file is the one
  input that makes the `term::` helpers deterministic: `select()` always reports
  it ready and `read()` returns the bytes then 0. An inherited tty would make
  the result a property of the runner; a pipe would block for the full 500 ms
  poll budget. It also *guarantees* the non-tty that `enterRawMode()`'s failure
  path needs rather than assuming CTest supplies one.
- **`ScopedGfxEnv`** — clears and restores `TERM` / `TERM_PROGRAM` /
  `KITTY_WINDOW_ID` so the detection matrix cannot leak a fabricated `TERM`
  into any test that runs after it.

| ID | Case |
|---|---|
| TERM-INPUT-09 | `ESC` + non-`[` (SS3 `ESC O P`) → both bytes skipped, `P` typed |
| — | unknown CSI final byte (`ESC [ H`) → introducer dropped, `Hx` typed |
| TERM-INPUT-10 | malformed SGR mouse report resyncs — no click from half-parsed digits |
| — | unknown `--gfx` name → `Auto` |
| — | `fps = 0` → clamped, `framePeriodMs()` does not divide by zero |
| — | destruction through a `QObject*` (the virtual deleting dtor) is safe |
| TERM-TERM-01 | `detectGfx()` — the whole env matrix, all six branches |
| TERM-TERM-02 | `restoreTerminal()` before raw mode is a no-op |
| TERM-TERM-03 | `enterRawMode()` on a non-tty fails at `tcgetattr` |
| TERM-TERM-04 | `terminalSize()` falls back to 80x24 |
| TERM-TERM-05 | `queryCellSize()` parses a well-formed XTWINOPS reply, and falls back on silence |
| TERM-TERM-06 | `begin()` resolves gfx *then* fails on a non-tty, having painted nothing |
| TERM-TERM-07 | SIGWINCH is latched, consumed once, and clears the screen |
| TERM-TERM-08 | a quit signal stops the frame loop and asks for no further frames |

Three of these are worth calling out as more than coverage:

- **`TERM-TERM-05` pins the reply field order.** XTWINOPS answers
  `ESC [ 6 ; <height> ; <width> t` — **height first**. The two numbers in the
  fixture are deliberately different (32 and 14), so a swap fails loudly instead
  of silently halving or doubling every image the gfx renderers scale.
- **`TERM-TERM-06` pins the ordering inside `begin()`**: gfx resolves, *then*
  raw mode is attempted, and nothing is painted if raw mode fails. A
  half-started viewer would leave the terminal on the alt screen with no way
  back.
- **`TERM-TERM-08` must stay the last slot in the class.** The quit flag has no
  reset — by design, nothing should be able to un-quit a viewer — so every
  `tick()` after it returns false. There is a comment on the slot saying so.

### 3.2 `tests/unit/test_config.cpp` — 12 cases (32 → 44)

The `parse_args` harness now also reports `headless`, `noSandbox`, `profileDir`,
`profileName`, `width`, `height` and `extensionPaths`, so these cases can assert
on the value that survived rather than only on the exit code.

| ID | Case |
|---|---|
| CFG-15 | `--port` 0 / 70000 / -1 refused; 1 and 65535 accepted |
| CFG-16 | `--width` / `--height`: non-numeric and zero refused, valid pair lands |
| CFG-17 | `--extension` must be an existing directory (a file is refused) |
| CFG-18 | `--headless`, `--no-sandbox`, `--profile-dir`, `--profile` reach Config |
| CFG-19 | `--config` is read first, CLI layers on top |
| CFG-20 | `width` / `height` from a JSON config file |
| CFG-21 | **the INI branch** — a whole second parser that had no test at all |
| CFG-22 | an empty INI file yields the documented defaults, not zeroes |
| CFG-23 | a path that exists but cannot be opened ≠ one that does not exist |
| TERM-CFG-12 | `--term-port` / `--fps` reject a non-numeric value |
| TERM-CFG-13 | `--cdp` with an unusable scheme, and a non-URL, are refused |

Notes on two of them:

- **CFG-21 is the largest single gap this phase closed.** Any `--config` suffix
  that is not `.json` is read as INI via `QSettings` — twelve lines of a second
  parser with zero coverage. Its defaults differ *in kind* from the JSON branch:
  `QSettings` substitutes a default for an absent key, where the JSON branch
  leaves the `Config` member untouched.
- **CFG-23 uses a directory named `adirectory.json`**, not a `chmod 000` file.
  `loadConfigFile()` checks `exists()` first and exits, so the "cannot open"
  branch needs a path that exists but will not open — and a mode-000 file still
  opens as root, which is how these tests run in the sandbox.

One case failed on first run and it was the test, not the code: the named-profile
flag is `--profile`, not `--profile-name` (the *Config field* is `profileName`).
Corrected, with a comment.

---

## 4. What is still uncovered, and why

**29 lines, all in `terminal_ui.cpp`, all of them tty-only** — reachable only
from a process that owns a real terminal:

| Lines | What |
|---|---|
| 78–82 | `restoreTerminal()` body — the actual `tcsetattr` restore |
| 89–101 | `enterRawMode()` success path — termios flags, `atexit`, alt screen |
| 125–126 | `terminalSize()` success branch (needs `TIOCGWINSZ` to answer) |
| 228–237 | `begin()` after raw mode succeeds |
| 244–245 | `end()` when `m_started` is true |
| 317, 324, 331 | `onStatus` / `onLink` / `setBackendLabel` repainting once started |

These are **covered by the pty integration suites** (`terminal_cdp.test.js`,
`terminal_http.test.js` under `script -q -e -c`), which drive the real binary —
an uninstrumented process, so its execution reports nothing back to gcov.

Reaching them at the unit level would mean opening a pty inside the QTest binary
(`posix_openpt` + `dup2` onto fds 0 and 1). **Deliberately not done**: phase 14's
stated invariant for this suite is that `begin()` is never called and the
geometry stays a constant 80x24 rather than becoming a property of the runner.
Trading that for 29 lines of an already-covered path is a bad trade, and it would
make every existing status-bar assertion in the file fragile.

### Scope of the gate

The gate measures the three Qt6::Core-only libraries the unit targets link. It
does **not** cover `src/browser/`, `src/http/`, `src/cdp/`, `src/pdf/`,
`src/main.cpp` or `src/terminal/{cdp_client,cdp_frame_backend,render_http_client,
terminal_app}.cpp` — all of which are exercised only by the vitest and e2e
suites, against a *separate* `anoa-browser` process that is not instrumented.
Including them would report ~0% for code those suites cover well, which is worse
than not reporting on them. Widening the gate means instrumenting the main binary
and teaching the integration suites to preserve its `.gcda` — a real piece of
work, and a candidate for a follow-up plan rather than something to fake here.
This is stated in the header of `tests/coverage.sh` and in the CI step comment.

---

## 5. Reproducing

```bash
make coverage                                    # gate at 80%
make coverage COVERAGE_MIN=90                    # tighter
QT_PREFIX=/opt/Qt/6.7.3/gcc_64 bash tests/coverage.sh
```

Per-line listings land in `build-coverage/.coverage-report/*.gcov`; CI uploads
them as the `coverage-gcov` artifact.

---

## 6. Verification performed

| Check | Result |
|---|---|
| `ctest` (plain Debug build, no instrumentation) | 3/3 pass |
| Build warnings, `-Wall -Wextra -Wpedantic` | zero |
| ASan + UBSan, both terminal targets, with the new cases | clean |
| Coverage gate at 80 | pass (95.61%) |
| Coverage gate at 95 | **fails**, naming `terminal_ui.cpp` — gate proven live |
| `-DANOA_TEST_SANITIZE=ON -DANOA_TEST_COVERAGE=ON` | rejected with a `FATAL_ERROR` |
| `ci.yml` | parses; new steps in the `unit-tests` job |

Not re-run: the vitest and shell integration suites. Nothing in their path
changed — `src/` has **no** changes in this phase, and the diff is tests, CMake,
the Makefile, `.gitignore` and CI only. Their last known state is phase 14's
103/103 (and `port_layout.test.sh`'s six pre-existing `nc`-related sandbox
failures, unchanged and documented there).

---

## 7. Exit criteria

| Criterion | Status |
|---|---|
| Coverage measured against the configured threshold | **met** — 80%, measured per file and in aggregate |
| Every measured file at or above threshold | **met** — 100% / 100% / 92.68% |
| The measurement is reproducible, not a one-off | **met** — `make coverage`, `tests/coverage.sh`, CI step |
| Gaps that remain are explained, not merely listed | **met** — §4 |
| No regression in the existing suite | **met** — 3/3, zero warnings, ASan clean |
