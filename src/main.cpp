#include <cstring>

#include <QApplication>

#include "browser/anoa_browser.h"
#include "config/config.h"

// Forward declarations for future modules (wired up once implemented).
// class HttpServer;
// class CdpProxy;

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
    browser.init();

    // HttpServer and CdpProxy are wired here once those modules are implemented:
    // HttpServer httpServer(config, &app);
    // CdpProxy cdpProxy(config, &app);

    return app.exec();
}
