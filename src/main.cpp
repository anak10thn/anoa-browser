#include <cstring>
#include <memory>

#include <QtGlobal>

#include <QApplication>
#include <QCoreApplication>
#include <QTextStream>

#include "browser/anoa_browser.h"
#include "browser/browser_window.h"
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
    // Whether the viewer was given a target to connect to. Also a pre-scan
    // question, and for the same reason: with no target the viewer hosts its
    // own browser, which decides the application class just as `terminal` does.
    // Presence is all that matters, so the values are not parsed here — this
    // only has to agree with QCommandLineParser about which words are options,
    // including the --opt=value spelling it accepts.
    bool hasTarget = false;
    const auto isOption = [](const char *arg, const char *name) {
        const size_t n = std::strlen(name);
        return std::strncmp(arg, name, n) == 0 && (arg[n] == '\0' || arg[n] == '=');
    };
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
        } else if (std::strcmp(argv[i], "--version") == 0 || std::strcmp(argv[i], "-v") == 0
                   || std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            // QCommandLineParser handles both of these, and it needs a live
            // application object — which on a machine with no display aborts
            // before it can print anything. `anoa-browser --version` therefore
            // died with SIGABRT on any headless Linux box, which is also the
            // one command the Homebrew Linux formula runs as its test.
            //
            // Neither path ever puts a pixel on a screen, so the offscreen
            // platform is not a compromise here: it is simply the honest
            // description of what the process is about to do.
            qputenv("QT_QPA_PLATFORM", "offscreen");
        } else if (isOption(argv[i], "--term-host") || isOption(argv[i], "--term-port")
                   || isOption(argv[i], "--cdp")) {
            hasTarget = true;
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
    const bool embeddedTerminal = terminalMode && !hasTarget;

    if (terminalMode) {
#ifdef Q_OS_WIN
        // No terminal source is compiled on Windows at all (see CMakeLists.txt),
        // so this has to be a clean runtime error rather than a link failure.
        // The embedded decision is POSIX-only for the same reason, and would
        // otherwise be an unused variable in a build that treats warnings as
        // something to keep at zero.
        Q_UNUSED(embeddedTerminal)
        QTextStream err(stderr);
        err << "Error: terminal mode is not supported on Windows" << Qt::endl;
        return 1;
#else
        if (embeddedTerminal) {
            // The one terminal case that is not a thin client, and so the one
            // that cannot use QCoreApplication: it hosts a QWebEngineView, and
            // that needs the widget stack. Offscreen keeps the "works over SSH
            // with no display" property that motivated QCoreApplication in the
            // first place — QApplication only aborts on a missing display when
            // it is left to pick a platform itself.
            qputenv("QT_QPA_PLATFORM", "offscreen");
            QApplication app(argc, argv);
            app.setApplicationVersion(QStringLiteral(ANOA_VERSION));

            Config config = parseArgs(argc, argv, /*terminalMode=*/true);
            config.termEmbedded = true;
            // Not a copy of the --headless flag: there is no window either way,
            // and AnoaBrowser reads this to add --disable-gpu and to create an
            // offscreen surface rather than look for a display.
            config.headless = true;

            AnoaBrowser browser(config);
            if (!config.profileName.isEmpty())
                browser.setupNamedProfile(config.profileName, config.profileDir);
            browser.loadExtensions(config.extensionPaths);
            browser.init();

            // Still no HttpServer and no CdpProxy: nothing outside this process
            // is meant to reach this browser, and binding ports for a viewer
            // that talks to it through a pointer would be surface for nothing.
            return runTerminal(config, &browser);
        }

        // QCoreApplication, not QApplication: the primary use case is SSH with
        // no display, where QApplication aborts unless QT_QPA_PLATFORM is set.
        QCoreApplication app(argc, argv);
        app.setApplicationVersion(QStringLiteral(ANOA_VERSION));

        Config config = parseArgs(argc, argv, /*terminalMode=*/true);

        // Pointed at a browser elsewhere: no AnoaBrowser, HttpServer or CdpProxy.
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

    // Declared after `browser` so it is destroyed first: its destructor
    // releases the view it borrowed. Headless mode gets no window at all —
    // there is nothing to show chrome on, and wrapping the view would change
    // the geometry that /render/* reports as the viewport.
    std::unique_ptr<BrowserWindow> window;
    if (!config.headless)
        window = std::make_unique<BrowserWindow>(&browser, config);

    browser.init();
    if (window)
        window->show();

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
