#pragma once

#include <QMap>
#include <QObject>
#include <QString>
#include <QWebSocket>
#include <QWebSocketServer>

class QWebEnginePage;

class CdpProxy : public QObject {
    Q_OBJECT
public:
    explicit CdpProxy(quint16 listenPort, quint16 debuggingPort,
                      const QString &authToken, QObject *parent = nullptr);

    bool start();
    void stop();
    // Optional: provide a page for handling commands that require a local page
    // reference (e.g. Page.printToPDF via Qt's QWebEnginePage::printToPdf).
    void setPage(QWebEnginePage *page);

private slots:
    void onNewConnection();
    void onClientMessage(const QString &message);
    void onClientDisconnected();
    void onUpstreamMessage(const QString &message);
    void onUpstreamDisconnected();

private:
    QWebSocketServer *m_server;
    QMap<QWebSocket *, QWebSocket *> m_clientToUpstream;
    QMap<QWebSocket *, QWebSocket *> m_upstreamToClient;
    quint16 m_listenPort;
    quint16 m_debugPort;
    QString m_authToken;
    QWebEnginePage *m_page = nullptr;
};
