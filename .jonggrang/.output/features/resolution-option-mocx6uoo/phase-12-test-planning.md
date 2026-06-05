---
feature: resolution-option
branch: feat/resolution-option
work_type: SMALL
description: Add --width/--height CLI flags (default 1280x720) and apply window/viewport size in browser core
created_at: 2026-04-24T12:59:55.744Z
---

# Phase 12 – Test Planning: resolution-option

## Feature Summary

The `resolution-option` feature adds `--width` and `--height` CLI flags (default 1280×720) and applies the configured viewport dimensions in both headed and headless modes.

**Files involved:**
- `src/config/config.h` — `Config` struct gains `width`, `height` fields
- `src/config/config.cpp` — `--width`/`--height` CLI parsing, JSON/INI file loading, positive-integer validation
- `src/browser/anoa_browser.cpp` — `resize(w, h)` called before `show()` / `load()` in `init()`

**Quality findings from Phase 11:**
- 1 Low issue: JSON/INI config file `width`/`height` values bypass positive-integer validation (CLI path is safe)
- 1 Low issue: No upper bound on `width`/`height` values
- 1 Informational: No automated test framework configured (manual validation only, per AGENTS.md)

---

## Test Strategy

### Constraints

- **No test framework configured** — `AGENTS.md`: "Test command: echo 'no test command configured'"
- **Manual validation only** — Integration testing done in Phase 10 with Playwright/CDP
- **C++ binary** — No unit-test mocking infrastructure present; testing requires a running binary

### Test Categories

#### 1. CLI Flag Parsing — Positive Validation (automated, shell)
Tests that valid `--width` and `--height` values are accepted and produce the expected binary behavior.

| Test | Command | Expected | Pass Criteria |
|------|---------|----------|---------------|
| TC-01 | `./build/anoa-browser --help` | Help text includes `--width` and `--height` | Help output contains both flags with descriptions |
| TC-02 | `./build/anoa-browser --width 1920 --height 1080 --help` | Help or version runs without error | No exit(1), no crash |
| TC-03 | `./build/anoa-browser --width 1280 --height 720 --port 19229 &` | Binary starts | HTTP discovery endpoint responds on port 19229 |

#### 2. CLI Flag Parsing — Negative Validation (automated, shell)
Tests that invalid `--width` and `--height` values cause exit(1) with descriptive messages.

| Test | Command | Expected | Pass Criteria |
|------|---------|----------|---------------|
| TC-04 | `./build/anoa-browser --width 0 --port 19230` | Exit code 1, stderr: "positive integer" error | Exit code = 1, stderr contains error text |
| TC-05 | `./build/anoa-browser --width -10 --port 19231` | Exit code 1, stderr: "positive integer" error | Exit code = 1, stderr contains error text |
| TC-06 | `./build/anoa-browser --width abc --port 19232` | Exit code 1, stderr: "positive integer" error | Exit code = 1, stderr contains error text |
| TC-07 | `./build/anoa-browser --height 0 --port 19233` | Exit code 1, stderr: "positive integer" error | Exit code = 1, stderr contains error text |
| TC-08 | `./build/anoa-browser --height -5 --port 19234` | Exit code 1, stderr: "positive integer" error | Exit code = 1, stderr contains error text |
| TC-09 | `./build/anoa-browser --height xyz --port 19235` | Exit code 1, stderr: "positive integer" error | Exit code = 1, stderr contains error text |

#### 3. Config File — Valid Values (automated, shell)
Tests that JSON config files with valid `width`/`height` are loaded and applied.

| Test | Command | Expected | Pass Criteria |
|------|---------|----------|---------------|
| TC-10 | Write `{"width":640,"height":480}` to config.json; launch with `--config config.json --port 19236` | Binary starts | HTTP discovery responds; CDP `Page.captureScreenshot` returns 640×480 PNG |
| TC-11 | INI file with `width=800\nheight=600`; launch with `--config config.ini --port 19237` | Binary starts | HTTP discovery responds; CDP `Page.captureScreenshot` returns 800×600 PNG |

#### 4. Config File — Invalid Values (automated, shell)
Tests that config file values pass through without validation (known gap from Phase 11).

| Test | Command | Expected | Pass Criteria |
|------|---------|----------|---------------|
| TC-12 | JSON config `{"width":-10,"height":480}`; launch with `--config config.json --port 19238` | Binary starts (no validation on file path) | HTTP responds; Qt clamps or accepts negative without crash |

*Note: TC-12 documents the known Phase 11 low-severity gap — config file values bypass positive-integer validation. This test confirms current (undesired) behavior.*

#### 5. Config Precedence — CLI Overrides File (automated, shell)
Tests that CLI `--width` and `--height` values override config file values.

| Test | Command | Expected | Pass Criteria |
|------|---------|----------|---------------|
| TC-13 | Config file `{"width":640,"height":480}`, CLI `--width 1024 --port 19239` | Width=1024, height=480 | CDP screenshot shows 1024×480 |

#### 6. Headless Viewport Resolution (CDP integration)
Tests that `Page.captureScreenshot` returns correctly-sized images in headless mode.

| Test | Scenario | Expected | Pass Criteria |
|------|----------|----------|---------------|
| TC-14 | `--headless --width 1280 --height 720 --port 19240` | 1280×720 PNG | PNG dimensions match |
| TC-15 | `--headless --width 1920 --height 1080 --port 19241` | 1920×1080 PNG | PNG dimensions match |
| TC-16 | `--headless --width 800 --height 600 --port 19242` | 800×600 PNG | PNG dimensions match |

#### 7. Config File + Headless (CDP integration)
Tests that config file resolution applies correctly in headless mode.

| Test | Scenario | Expected | Pass Criteria |
|------|----------|----------|---------------|
| TC-17 | JSON config `{"width":640,"height":480}` + `--headless --config config.json --port 19243` | 640×480 PNG | PNG dimensions match |

#### 8. Edge Cases (automated, shell)
Tests boundary conditions and extreme values.

| Test | Command | Expected | Pass Criteria |
|------|---------|----------|---------------|
| TC-18 | `--width 1 --height 1 --port 19244` | Binary starts | HTTP responds; screenshot 1×1 |
| TC-19 | `--width 99999 --height 99999 --port 19245` | Binary starts (no upper bound) | HTTP responds |
| TC-20 | Only `--width` set, `--height` omitted | Default height=720 used | Screenshot shows CLI width × 720 |
| TC-21 | Only `--height` set, `--width` omitted | Default width=1280 used | Screenshot shows 1280 × CLI height |

---

## Test Execution Plan

### Environment Requirements
- Built binary: `./build/anoa-browser`
- CDP client: Node.js with `ws` package or `wscat`
- Port availability: 19229–19245 (16 consecutive ports for parallel-safe testing)
- Kill strategy: Launched with `&` + `disown`, killed via `kill -TERM` to the entire process group

### Execution Order
1. TC-01 to TC-03 — smoke tests (binary must start successfully)
2. TC-04 to TC-09 — negative validation (exit code checks)
3. TC-10 to TC-11 — config file positive
4. TC-12 — config file invalid (gap confirmation)
5. TC-13 — CLI precedence
6. TC-14 to TC-17 — headless viewport tests (CDP required)
7. TC-18 to TC-21 — edge cases

### Pass Criteria Per Test
Each test in the table above defines pass/fail criteria. Overall feature test status is:
- **PASS** if all TC-01 through TC-21 pass
- **PARTIAL** if TC-04 through TC-21 pass but TC-01 through TC-03 smoke fails
- **FAIL** if any exit-code validation (TC-04 to TC-09) fails

---

## Known Limitations

1. **No automated test framework** — all tests run via shell commands; no unit test infrastructure
2. **Phase 11 gap not fixed** — config file invalid values (TC-12) are not rejected; this is documented as a low-severity issue to address in a future task
3. **Visual confirmation not automated** — headed mode window size cannot be programmatically verified without image processing; TC-02 is smoke-only
4. **Upper bound not enforced** — TC-19 confirms that very large values are accepted (informational gap, not a bug)

---

## Phase Completion

```json
{
  "phase": 12,
  "status": "completed",
  "output": {
    "test_categories": ["CLI positive", "CLI negative", "config file positive", "config file invalid gap", "CLI precedence", "headless viewport CDP", "edge cases"],
    "total_test_cases": 21,
    "automated_shell": 13,
    "cdp_integration": 8,
    "known_gaps": [
      "config file width/height bypasses positive-integer validation (Phase 11 Low #1)",
      "no upper bound on width/height (Phase 11 Low #2, informational)",
      "no automated test framework (Phase 11 Informational)"
    ],
    "execution_order": "smoke → negative → config → precedence → headless → edge",
    "port_range": "19229–19245 (16 ports for parallel-safe testing)",
    "recommendation": "Run TC-01 through TC-21 as shell/CDP tests; create tracking issue for Phase 11 Low #1 (config file validation)"
  }
}