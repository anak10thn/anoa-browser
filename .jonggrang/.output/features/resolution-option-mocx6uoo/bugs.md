# Bug Reports — resolution-option

## [open] bug-001 · 2026-04-24T13:07:25.962Z
QCommandLineOption initializer-list constructor {"width"}/{"height"} is ambiguous on macOS Qt — call to constructor of 'QCommandLineOption' is ambiguous between QString and QStringList overloads. Fix: use QStringLiteral("width") or explicit QStringList{"width"} instead of braced-init-list.
