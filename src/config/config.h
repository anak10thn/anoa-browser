#pragma once

#include <QString>
#include <QStringList>

struct Config {
    int port = 9222;
    bool headless = false;
    bool noSandbox = false;
    QString profileDir;
    QString profileName;
    QStringList extensionPaths;
    QString authToken;
};

Config parseArgs(int argc, char *argv[]);
Config loadConfigFile(const QString &path);
