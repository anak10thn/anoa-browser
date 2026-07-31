#pragma once

// Terminal mode (`anoa-browser terminal`) is POSIX-only: the viewer is built on
// termios, select()/QSocketNotifier over STDIN and SIGWINCH, none of which exist
// on MSVC. This header is never included on Windows and terminal_app.cpp is not
// added to target_sources there — main.cpp guards both behind Q_OS_WIN.

struct Config;

// Entry point for terminal mode. Returns the process exit code.
// The caller has already constructed a QCoreApplication and parsed argv.
int runTerminal(const Config &config);
