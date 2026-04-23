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

    // debuggingPort = port+1: Qt/Chromium opens its DevTools HTTP endpoint on a
    // separate port from the one we expose to clients. We listen on config.port
    // and forward discovery requests to config.port+1 where Chromium runs.
    HttpServer httpServer(static_cast<quint16>(config.port),
                          static_cast<quint16>(config.port + 1),
                          config.authToken,
                          &app);
    httpServer.start();

    CdpProxy cdpProxy(static_cast<quint16>(config.port),
                      static_cast<quint16>(config.port + 1),
                      config.authToken,
                      &app);
    cdpProxy.start();

    return app.exec();
}
