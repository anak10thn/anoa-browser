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

    // Terminal mode (`anoa-browser terminal`). CLI-only — never read from the
    // JSON/INI config file. terminalMode is set by the argv pre-scan in
    // main.cpp, not by parseArgs().
    bool terminalMode = false;
    QString termHost = "127.0.0.1";
    int termPort = 9222;
    QString termToken;
    int fps = 30;
    QString gfxMode = "auto";
    QString cdpUrl; // empty = use the default /render/* HTTP backend
};

Config parseArgs(int argc, char *argv[]);
Config loadConfigFile(const QString &path);
