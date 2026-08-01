#pragma once

// Terminal mode (`anoa-browser terminal`) is POSIX-only: the viewer is built on
// termios, select()/QSocketNotifier over STDIN and SIGWINCH, none of which exist
// on MSVC. This header is never included on Windows and terminal_app.cpp is not
// added to target_sources there — main.cpp guards both behind Q_OS_WIN.

struct Config;
class AnoaBrowser;

// Entry point for terminal mode. Returns the process exit code.
// The caller has already constructed an application object and parsed argv.
//
// `embedded` is the browser this process is hosting itself, passed only when
// config.termEmbedded is set — and then it is a QApplication above us, not a
// QCoreApplication, because a live QWebEngineView needs the widget stack. Null
// for the /render/* and CDP transports, which own no browser.
int runTerminal(const Config &config, AnoaBrowser *embedded = nullptr);
