---
feature: resolution-option
branch: feat/resolution-option
work_type: SMALL
description: Add --width/--height CLI flags (default 1280x720) and apply window/viewport size in browser core
created_at: 2026-04-24T12:59:55.744Z
---

# Bug Reports — resolution-option

## [open] bug-001 · 2026-04-24T13:07:25.962Z
QCommandLineOption initializer-list constructor {"width"}/{"height"} is ambiguous on macOS Qt — call to constructor of 'QCommandLineOption' is ambiguous between QString and QStringList overloads. Fix: use QStringLiteral("width") or explicit QStringList{"width"} instead of braced-init-list.
