#include <cstring>

#include <QApplication>

#include "browser/anoa_browser.h"
#include "cdp/cdp_proxy.h"
#include "config/config.h"
#include "http/http_server.h"

int main(int argc, char *argv[])
{
    // Pre-scan argv for --headless to set QT_QPA_PLATFORM before QApplication.
    // QCommandLineParser (used by parseArgs) requires QCoreApplication, so the
    // full config parse must come after app construction. This raw scan handles
    // the env var that must be set before QApplication is constructed.
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--headless") == 0) {
            qputenv("QT_QPA_PLATFORM", "offscreen");
            break;
        }
    }

    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral("0.1.0"));

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
    httpServer.start();

    CdpProxy cdpProxy(wsPort, debugPort, config.authToken, &app);
    // Provide the initial page for commands handled locally (e.g. Page.printToPDF).
    cdpProxy.setPage(browser.page());
    cdpProxy.start();

    return app.exec();
}
