#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>

class QWebEnginePage;

class CdpExtensions : public QObject {
    Q_OBJECT
public:
    // Returns a JSON response string if the domain is handled locally; empty string = pass through.
    static QString processCommand(const QJsonObject &cmd, QWebEnginePage *page);

private:
    static QString handleProfiler(const QJsonObject &cmd);
    static QString handleHeapProfiler(const QJsonObject &cmd);
    static QString handleSecurity(const QJsonObject &cmd, QWebEnginePage *page);
};
