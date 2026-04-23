#pragma once

#include <QObject>
#include <QString>
#include <QTcpServer>

class HttpServer : public QObject {
    Q_OBJECT
public:
    explicit HttpServer(quint16 port, quint16 debuggingPort, quint16 proxyPort,
                       const QString &authToken, QObject *parent = nullptr);

    bool start();
    void stop();

private slots:
    void handleNewConnection();

private:
    QTcpServer *m_server;
    quint16 m_port;
    quint16 m_debugPort;
    quint16 m_proxyPort;
    QString m_authToken;
};
