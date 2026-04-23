#pragma once

#include <QObject>
#include <QJsonObject>
#include <QString>

class QWebEnginePage;

class CdpExtensions : public QObject {
    Q_OBJECT
public:
    // Returns a JSON response string if the domain is handled locally; empty = pass through.
    static QString processCommand(const QJsonObject &cmd, QWebEnginePage *page);

    // Returns a possibly-modified command for upstream forwarding; null object = no change.
    static QJsonObject rewritePassthrough(const QJsonObject &cmd);

private:
    static QString handleProfiler(const QJsonObject &cmd);
    static QString handleHeapProfiler(const QJsonObject &cmd);
    static QString handleSecurity(const QJsonObject &cmd, QWebEnginePage *page);
    static QString handleBrowser(const QJsonObject &cmd);
    static QString handleTarget(const QJsonObject &cmd);
};
