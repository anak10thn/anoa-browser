#include "config.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTextStream>

static void validatePort(int port)
{
    if (port < 1 || port > 65535) {
        QTextStream err(stderr);
        err << "Error: --port must be between 1 and 65535, got " << port << "\n";
        ::exit(1);
    }
}

static void validateExtensionPaths(const QStringList &paths)
{
    for (const QString &p : paths) {
        if (!QFileInfo(p).isDir()) {
            QTextStream err(stderr);
            err << "Error: extension path is not an existing directory: " << p << "\n";
            ::exit(1);
        }
    }
}

Config loadConfigFile(const QString &path)
{
    Config cfg;
    QFileInfo fi(path);
    if (!fi.exists()) {
        QTextStream err(stderr);
        err << "Error: config file not found: " << path << "\n";
        ::exit(1);
    }

    QString suffix = fi.suffix().toLower();
    if (suffix == "json") {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            QTextStream err(stderr);
            err << "Error: cannot open config file: " << path << "\n";
            ::exit(1);
        }
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &parseErr);
        if (doc.isNull()) {
            QTextStream err(stderr);
            err << "Error: invalid JSON in config file: " << parseErr.errorString() << "\n";
            ::exit(1);
        }
        QJsonObject obj = doc.object();
        if (obj.contains("port"))
            cfg.port = obj["port"].toInt(9222);
        if (obj.contains("headless"))
            cfg.headless = obj["headless"].toBool(false);
        if (obj.contains("noSandbox"))
            cfg.noSandbox = obj["noSandbox"].toBool(false);
        if (obj.contains("profileDir"))
            cfg.profileDir = obj["profileDir"].toString();
        if (obj.contains("profileName"))
            cfg.profileName = obj["profileName"].toString();
        if (obj.contains("authToken"))
            cfg.authToken = obj["authToken"].toString();
        if (obj.contains("extensionPaths")) {
            for (const auto &v : obj["extensionPaths"].toArray())
                cfg.extensionPaths << v.toString();
        }
    } else {
        // INI format via QSettings
        QSettings ini(path, QSettings::IniFormat);
        cfg.port = ini.value("port", 9222).toInt();
        cfg.headless = ini.value("headless", false).toBool();
        cfg.noSandbox = ini.value("noSandbox", false).toBool();
        cfg.profileDir = ini.value("profileDir").toString();
        cfg.profileName = ini.value("profileName").toString();
        cfg.authToken = ini.value("authToken").toString();
        cfg.extensionPaths = ini.value("extensionPaths").toStringList();
        cfg.extensionPaths.removeAll(QString());
    }
    return cfg;
}

Config parseArgs(int argc, char *argv[])
{
    // QCommandLineParser requires a QCoreApplication to exist.
    // Callers must construct QApplication before calling parseArgs.
    QCommandLineParser parser;
    parser.setApplicationDescription("Anoa headless browser with CDP support");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOpt({"p", "port"}, "CDP listen port (1-65535, default 9222)", "port", "9222");
    QCommandLineOption headlessOpt("headless", "Run in offscreen/headless mode");
    QCommandLineOption noSandboxOpt("no-sandbox", "Disable Chromium sandbox");
    QCommandLineOption profileDirOpt("profile-dir", "Base directory for browser profiles", "dir");
    QCommandLineOption profileNameOpt("profile", "Named profile to activate", "name");
    QCommandLineOption extensionOpt("extension", "Path to an unpacked extension directory (repeatable)", "path");
    QCommandLineOption authTokenOpt("auth-token", "Bearer token required for CDP WebSocket connections", "token");
    QCommandLineOption configOpt("config", "Path to JSON or INI config file", "file");

    parser.addOption(portOpt);
    parser.addOption(headlessOpt);
    parser.addOption(noSandboxOpt);
    parser.addOption(profileDirOpt);
    parser.addOption(profileNameOpt);
    parser.addOption(extensionOpt);
    parser.addOption(authTokenOpt);
    parser.addOption(configOpt);

    parser.process(*QCoreApplication::instance());

    // Start from file config (if given), then let CLI values override.
    Config cfg;
    if (parser.isSet(configOpt)) {
        cfg = loadConfigFile(parser.value(configOpt));
    }

    if (parser.isSet(portOpt))
        cfg.port = parser.value(portOpt).toInt();
    if (parser.isSet(headlessOpt))
        cfg.headless = true;
    if (parser.isSet(noSandboxOpt))
        cfg.noSandbox = true;
    if (parser.isSet(profileDirOpt))
        cfg.profileDir = parser.value(profileDirOpt);
    if (parser.isSet(profileNameOpt))
        cfg.profileName = parser.value(profileNameOpt);
    if (parser.isSet(authTokenOpt))
        cfg.authToken = parser.value(authTokenOpt);

    // --extension is repeatable; append CLI entries on top of file config entries.
    const QStringList cliExtensions = parser.values(extensionOpt);
    if (!cliExtensions.isEmpty())
        cfg.extensionPaths = cliExtensions;

    // Validate
    validatePort(cfg.port);
    validateExtensionPaths(cfg.extensionPaths);

    if (cfg.authToken.isEmpty()) {
        QTextStream err(stderr);
        err << "Warning: --auth-token is not set; CDP WebSocket will be unauthenticated\n";
    }

    return cfg;
}
