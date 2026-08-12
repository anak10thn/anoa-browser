#pragma once

#include <QObject>
#include <QString>
#include <QTcpServer>

class AnoaBrowser;

class HttpServer : public QObject {
    Q_OBJECT
public:
    explicit HttpServer(quint16 port, quint16 debuggingPort, quint16 proxyPort,
                       const QString &authToken, AnoaBrowser *browser,
                       QObject *parent = nullptr);

    bool start();
    void stop();

private slots:
    void handleNewConnection();

private:
    // /json and /json/list, rebuilt from the tab registry: a tab id is ours and
    // appears nowhere in Chromium's answer, so the old byte rewrite had nothing
    // to carry it in. Falls back to the already-rewritten upstream body when no
    // tab has resolved its target id yet.
    QByteArray rebuildTargetList(const QByteArray &rewritten, const QString &hostName) const;

    QTcpServer *m_server;
    quint16 m_port;
    quint16 m_debugPort;
    quint16 m_proxyPort;
    QString m_authToken;
    AnoaBrowser *m_browser;
};
