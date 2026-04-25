# Phase 11 – Code Quality Audit: resolution-option

## Scope

Files modified for the `--width` / `--height` resolution feature:

| File | Role |
|------|------|
| `src/config/config.h` | `Config` struct gains `width = 1280`, `height = 720` fields |
| `src/config/config.cpp` | `--width`/`--height` CLI option registration; JSON/INI file parsing; validation (positive integer) |
| `src/browser/anoa_browser.cpp` | `resize(w, h)` called before `show()` / `load()` in `init()` |
| `src/browser/anoa_browser.h` | No change |
| `src/main.cpp` | No change |

---

## Findings

### 1. Positive-Integer Validation

**Location:** `src/config/config.cpp:149-168`

Both `--width` and `--height` reject zero and negative values (`<= 0`), and reject non-integer input (via `QCommandLineParser::value().toInt(&ok)`). Strings like `"abc"` and `"0"` cause `exit(1)` — correct.

**Severity:** None — behavior is correct.

---

### 2. Config Precedence

**Location:** `src/config/config.cpp:130-168`

The precedence chain is:

1. Config file loaded first (if `--config` given)
2. CLI args parsed second; when set, they overwrite the file values

This is explicitly documented and matches the intended design. No bug.

**Severity:** None.

---

### 3. Viewport Application Order

**Location:** `src/browser/anoa_browser.cpp:41-48`

```cpp
resize(m_config.width, m_config.height);
show();
load(QUrl(QStringLiteral("about:blank")));
```

This is the **correct order** — `resize()` before `load()` ensures the initial render uses the configured surface size (per AGENTS.md "Known Gotchas"). `show()` is called before navigation, which is required for headless (offscreen) mode to create a backing surface. No issue.

**Severity:** None.

---

### 4. Headless `--disable-gpu` Flag

**Location:** `src/browser/anoa_browser.cpp:28-29`

```cpp
if (m_config.headless)
    flags += " --disable-gpu";
```

When `--headless` is active, `--disable-gpu` is appended to `QTWEBENGINE_CHROMIUM_FLAGS`. This prevents the Skia rasterizer crash on GPU-less hosts (CI, dev Macs). Correct and consistent with the project gotcha documented in AGENTS.md.

**Severity:** None.

---

### 5. Default Values Are Sensible

**Location:** `src/config/config.h:14-15`

```cpp
int width = 1280;
int height = 720;
```

Defaults match Chrome's default viewport. Both `config.cpp` parsing and the struct initializer agree on these values.

**Severity:** None.

---

### 6. JSON Config Parsing – Missing Validation on File Load

**Location:** `src/config/config.cpp:76-79`

```cpp
if (obj.contains("width"))
    cfg.width = obj["width"].toInt(1280);
if (obj.contains("height"))
    cfg.height = obj["height"].toInt(720);
```

When loading from a JSON config file, there is **no validation** that `width`/`height` are positive. A config file containing `{"width": -10}` will set `cfg.width = -10` without error. The CLI path goes through validation (`src/config/config.cpp:149-168`), but the file path does not.

This is a **Low-severity** issue: the invalid value propagates to `resize(-10, 720)`, which Qt handles gracefully (likely clamping to valid bounds), and `--width abc` on CLI is caught. However, a user who writes `-10` in their config file silently gets unexpected behavior.

**Recommendation:** Apply the same positive-integer validation to file-loaded `width`/`height` values. Add a `validateResolution()` helper and call it after loading a config file.

**Severity:** Low.

---

### 7. No Bounds on Upper Limit

`--width 99999` or `--width 0` (CLI) is correctly rejected, but `--width 99999` passes validation because there's no upper bound. Very large viewports may cause resource issues or renderer instability. No critical impact, but worth noting.

**Severity:** Low (informational).

---

### 8. QCommandLineOption Disambiguation — Correct

**Location:** `src/config/config.cpp:114-115`

```cpp
QCommandLineOption widthOpt(QStringList{"width"}, ...);
QCommandLineOption heightOpt(QStringList{"height"}, ...);
```

`QStringList{"width"}` is used instead of `{"width"}` to disambiguate from the `QString` overload on macOS Qt 6.10. Consistent with AGENTS.md convention.

**Severity:** None.

---

### 9. `show()` Before `load()` in Both Modes

`show()` is called unconditionally, which is correct for both headed (visible window) and headless (`QPA_PLATFORM=offscreen`, invisible surface) modes. The comment at `anoa_browser.cpp:42-44` explains why this is necessary. Well-documented.

**Severity:** None.

---

### 10. No Test Coverage

There are no unit or integration tests for the resolution option feature. The project has no test framework configured (`AGENTS.md`: "Test command: echo 'no test command configured'"). Validation was performed manually (Phase 10). This is a project-level gap, not a code-quality bug in this feature.

**Severity:** Informational.

---

## Summary

| Category | Count |
|----------|-------|
| Critical | 0 |
| High | 0 |
| Medium | 0 |
| Low | 2 (JSON config unvalidated input; no upper bound) |
| Informational | 1 (no test coverage) |

**Overall:** Code quality is good. The implementation correctly applies resolution, validates CLI input, follows the established patterns, and is well-commented. The only actionable issue is that JSON/INI config file values for `width`/`height` bypass the positive-integer validation that CLI values go through.

**Recommendation:** Add a `validateResolution()` call after `loadConfigFile()` in the CLI override path, or directly in `loadConfigFile()` before returning. This closes the only meaningful gap between the safe CLI path and the file config path.

---

## Phase Completion

`phase: 11`
`status: completed`
`output: { "issues_found": 2, "critical": 0, "high": 0, "medium": 0, "low": 2, "informational": 1, "recommendation": "Add positive-integer validation to width/height values loaded from config files" }`
