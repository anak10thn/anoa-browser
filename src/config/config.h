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
    int width = 1280;
    int height = 720;
};

Config parseArgs(int argc, char *argv[]);
Config loadConfigFile(const QString &path);
