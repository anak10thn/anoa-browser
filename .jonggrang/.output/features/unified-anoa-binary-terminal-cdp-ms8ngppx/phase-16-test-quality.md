# Phase 16 — Test Quality

Reviewed every test this feature added: `tests/unit/test_config.cpp`,
`test_frame_bytes.cpp`, `test_terminal_ui.cpp`, `tests/integration/helpers.js`,
`terminal_cdp.test.js`, `terminal_http.test.js`, `build_shape.test.sh`,
`qt_floor.test.sh`, and the terminal cases in `port_layout.test.sh`.

The suites are in good shape. Nothing was deleted as worthless; the fixes below
are six weak or wrong assertions and two redundancies.

## Fixed — assertions that could not fail for the reason they claimed

**1. `build_shape.test.sh` TERM-BUILD-02b checked only one side of the guard.**
It searched for `src/terminal/` references *before* `if(NOT WIN32)` and never
looked past `endif()`, so a terminal source appended to the bottom of
`tests/unit/CMakeLists.txt` — the realistic regression — passed. It was also
vacuous: with zero `src/terminal/` references (a deleted target, a broken grep)
the stray list is empty and it reported PASS. TERM-BUILD-02 next door already
handled both, so the shared logic is now one `assert_guarded` function used by
both cases, checking both bounds and requiring count >= 1.

Verified negatively: appending a `src/terminal/oops.cpp` line after `endif()`
turns 02b red (`outside the guard (lines 83-137)`); it passed before the fix.

**2. `test_config.cpp` CFG-13 and CFG-14 asserted only the exit code.**
`loadConfigFile()` has three `exit(1)` branches — not found, cannot open,
invalid JSON — so `exitCode == 1` passes whichever one fires. CFG-23 drives the
same `bad_json` harness mode down the *cannot open* branch, which makes CFG-14
and CFG-23 mutually indistinguishable. Both now assert the message
(`config file not found` / `invalid JSON in config file`), matching CFG-23's
existing style, and both now check `waitForFinished()` rather than reading
`exitCode()` off a process that may not have finished.

**3. `test_terminal_ui.cpp` `testStatusBarPrecedence` under-asserted its third
block.** After clearing both the status and the link it checked only that
`connection lost` was absent — it would have passed just as happily with a
cleared `reconnecting` or `page 1` still painted, which is the actual failure
mode of a precedence chain. Now asserts the base row is back and all three
higher-precedence sources are gone.

## Fixed — redundant assertions

**4. `terminal_cdp.test.js` TCDP-04** —
`expect(Math.sign(deltaY)).toBe(-Math.sign(120))` is entailed by the
`toBe(-120)` on the line above it and can never fail independently. The
inversion is genuinely pinned by the opposite-notch assertion that follows.
Removed.

**5. `terminal_http.test.js` THTTP-04** — same shape
(`parseInt(dy) === -CDP_DELTA_Y` after `dy === '120'`). Removed, keeping the
cross-suite note as a comment so the CDP/HTTP pairing stays documented.

**6. `terminal_cdp.test.js` dead helper.** The fake endpoint's `inputs` getter
was referenced only by its own doc comment, and `inputsOf()`'s comment claimed
"Every Input.\* call" while every caller uses it for `Page.captureScreenshot`.
Getter removed, comments corrected.

## Fixed — test IDs

**7. `TERM-CFG-10` and `TERM-CFG-11` were each used twice** in
`test_config.cpp`: once for the `ws://` / `http://` acceptance cases (the
planned range is TERM-CFG-01…11) and again for the non-numeric and
unusable-scheme cases added in phase 15. The later pair is renumbered to
TERM-CFG-12 / TERM-CFG-13, and `phase-15-coverage.md` is synced. A stale
cross-reference in the same file ("TERM-CFG-04/05 already cover the range
refusals") named the wrong cases and now reads 05/06/07.

**8. `port_layout.test.sh` TERM-MODE-04 cited `CFG-09`**, which is
`testAuthToken`. The case it means is `TERM-CFG-09`. Corrected.

## Reviewed and deliberately kept

- **TERM-GUI-01** (`className() == "QCoreApplication"`) looks self-referential —
  it asserts what this file's own `main()` constructs. It stays: it is the
  premise guard for TERM-GUI-02/03, which prove nothing about headless
  operation if the harness quietly becomes a `QApplication`.
- **`testDestructionThroughBasePointerIsSafe`** has no explicit assertion. It is
  a real check under `-DANOA_TEST_SANITIZE=ON`, which CI runs.
- **THTTP-01's `toContain('[halfblock]')`** partly restates the `--gfx halfblock`
  the harness forces, but it also pins that the resolved mode reaches the status
  bar end to end.
- **The `it.fails()` block (THTTP-08)** is correct as committed — it documents
  open bug-003 and turns red when the socket-timeout fix lands.

## Reported, not changed

- **`helpers.js` sets `QPA_PLATFORM`, not `QT_QPA_PLATFORM`** (helpers.js:66) —
  a variable nothing reads. It is harmless because `startBrowser()` hardcodes
  `--headless`, and `main.cpp` sets the real variable itself. The same
  misspelling is in `port_layout.test.sh`, `extensions.test.sh` and `smoke.sh`
  and is already documented in AGENTS.md; fixing one of five would make the
  tree less consistent, not more.
- **`anoa-config-lib` / `anoa-browser-test-config` never call
  `anoa_test_target_flags()`**, so `-DANOA_TEST_SANITIZE=ON` does not actually
  sanitize `config.cpp` or its test binary — only the two terminal targets. This
  may be intentional (the ASan rationale in `tests/unit/CMakeLists.txt` is about
  the wire-facing byte parsers), but the option's description says "the unit test
  targets". Build config, not test quality; left for a decision rather than
  changed silently.

## Validation

| Suite | Result |
|---|---|
| `ctest` (ConfigTests, FrameBytesTests, TerminalUiTests) | 3/3 pass |
| `vitest terminal_cdp.test.js terminal_http.test.js` | 21/21 pass |
| `build_shape.test.sh` | 3/3 pass, and verified failing on an injected stray |
| `qt_floor.test.sh` (Qt 6.4.3) | 6/6 pass |
| `port_layout.test.sh` | 8 pass / 6 fail — all 6 are the WebEngine-launching PORT-0x cases; this sandbox has no GL or GPU so Chromium cannot come up. Pre-existing and environmental; every TERM-MODE-* case passes. |

Rebuilt warning-clean under `-Wall -Wextra -Wpedantic`.
