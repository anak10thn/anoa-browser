---
feature: resolution-option
branch: feat/resolution-option
work_type: SMALL
description: Add --width/--height CLI flags (default 1280x720) and apply window/viewport size in browser core
created_at: 2026-04-24T12:59:55.744Z
---

# Plan: Resolution Option

## Approach
Extend the `Config` struct with `width` and `height` integer fields defaulting to 1280 and 720. Parse `--width` and `--height` CLI flags (and config-file equivalents) in `config.cpp` following existing flag patterns. Apply the resolution in the browser core by calling `resize()` on the `QWebEngineView` widget for headed mode, and setting the off-screen surface size via `QWebEngineView::resize()` for headless mode so that screenshots and PDF output respect the configured viewport.

## Phases
1. Config extension — add `width`/`height` fields to `Config` struct with defaults 1280×720, parse `--width`/`--height` CLI flags, read from JSON/INI config file
2. Browser core wiring — use `config.width`/`config.height` to call `resize()` on the view at startup (both headed and headless paths)
3. Validation — manually verify headed window opens at 1280×720, headless screenshot reflects correct dimensions, and `--width`/`--height` overrides work end-to-end

## Key Decisions
- Two separate flags (`--width`, `--height`) rather than one `--resolution WxH` string: simpler parsing, consistent with QCommandLineParser patterns already in use
- Default 1280×720: HD baseline, reasonable for most automation use cases
- Apply resize before `load(about:blank)` so the initial render uses the correct viewport

## Out of Scope
- DPI / device-pixel-ratio scaling option
- Per-tab or dynamic resolution changes via CDP
- Saving resolution back to config file

## Dependencies
- `src/config/config.h` / `src/config/config.cpp` — existing CLI flag and config-file parsing patterns
- `src/browser/anoa_browser.cpp` — `AnoBrowser` constructor where `show()` and `load()` are called
