#include <cstring>

#include <QtGlobal>

#include <QApplication>
#include <QCoreApplication>
#include <QTextStream>

#include "browser/anoa_browser.h"
#include "cdp/cdp_proxy.h"
#include "config/config.h"
#include "http/http_server.h"

#ifndef Q_OS_WIN
#include "terminal/terminal_app.h"
#endif

int main(int argc, char *argv[])
{
    // Pre-scan raw argv for the two things that must be known before any
    // application object exists: --headless (QT_QPA_PLATFORM has to be set
    // before QApplication) and the `terminal` subcommand (it selects the
    // application class itself). QCommandLineParser, used by parseArgs, needs a
    // live QCoreApplication, so neither decision can wait for the full parse —
    // and `terminal` is a positional word the parser never registers.
    bool terminalMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        } else if (std::strcmp(argv[i], "terminal") == 0) {
            terminalMode = true;
            // Drop the subcommand word so QCommandLineParser only ever sees
            // options: process() would otherwise leave it in
            // positionalArguments() and echo it back in --help.
            for (int j = i; j < argc - 1; ++j)
                argv[j] = argv[j + 1];
            argv[--argc] = nullptr;
            --i; // re-examine the argument shifted into this slot
        }
    }

    if (terminalMode) {
#ifdef Q_OS_WIN
        // No terminal source is compiled on Windows at all (see CMakeLists.txt),
        // so this has to be a clean runtime error rather than a link failure.
        QTextStream err(stderr);
        err << "Error: terminal mode is not supported on Windows" << Qt::endl;
        return 1;
#else
        // QCoreApplication, not QApplication: the primary use case is SSH with
        // no display, where QApplication aborts unless QT_QPA_PLATFORM is set.
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(QStringLiteral(ANOA_VERSION));

        Config config = parseArgs(argc, argv);
        config.terminalMode = true;

        // Terminal mode is a client: no AnoaBrowser, HttpServer or CdpProxy.
        return runTerminal(config);
#endif
    }

    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(ANOA_VERSION));

    Config config = parseArgs(argc, argv);

    AnoaBrowser browser(config);
    if (!config.profileName.isEmpty())
        browser.setupNamedProfile(config.profileName, config.profileDir);
    browser.loadExtensions(config.extensionPaths);
    browser.init();

    // Port layout:
    //   config.port     (e.g. 9222) – HTTP discovery (HttpServer)
    //   config.port + 1 (e.g. 9223) – Chromium DevTools internal (set via QTWEBENGINE_CHROMIUM_FLAGS)
    //   config.port + 2 (e.g. 9224) – WebSocket CDP proxy (CdpProxy)
    //
    // HttpServer rewrites webSocketDebuggerUrl from port+1 to port+2 so that
    // CDP clients connect through the authenticated proxy.
    const auto httpPort  = static_cast<quint16>(config.port);
    const auto debugPort = static_cast<quint16>(config.port + 1);
    const auto wsPort    = static_cast<quint16>(config.port + 2);

    HttpServer httpServer(httpPort, debugPort, wsPort, config.authToken, &browser, &app);
    if (!httpServer.start()) {
        qCritical("Failed to bind HTTP server to port %u (already in use?)", httpPort);
        return 1;
    }

    CdpProxy cdpProxy(wsPort, debugPort, config.authToken, &app);
    // Provide the initial page for commands handled locally (e.g. Page.printToPDF).
    cdpProxy.setPage(browser.page());
    if (!cdpProxy.start()) {
        qCritical("Failed to bind CDP proxy to port %u (already in use?)", wsPort);
        return 1;
    }

    return app.exec();
}
