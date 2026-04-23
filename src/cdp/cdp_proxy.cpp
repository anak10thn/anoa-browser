#include "cdp/cdp_proxy.h"
#include "cdp/cdp_extensions.h"

#include <QDebug>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QWebSocketProtocol>

CdpProxy::CdpProxy(quint16 listenPort, quint16 debuggingPort,
                   const QString &authToken, QObject *parent)
    : QObject(parent)
    , m_server(new QWebSocketServer(QStringLiteral("CdpProxy"),
                                    QWebSocketServer::NonSecureMode, this))
    , m_listenPort(listenPort)
    , m_debugPort(debuggingPort)
    , m_authToken(authToken)
{}

bool CdpProxy::start()
{
    if (!m_server->listen(QHostAddress::Any, m_listenPort)) {
        qWarning() << "CdpProxy: failed to listen on port" << m_listenPort
                   << m_server->errorString();
        return false;
    }
    connect(m_server, &QWebSocketServer::newConnection,
            this, &CdpProxy::onNewConnection);
    qInfo() << "CdpProxy: listening on ws://0.0.0.0:" << m_listenPort;
    return true;
}

void CdpProxy::stop()
{
    m_server->close();
    for (auto *client : m_clientToUpstream.keys()) {
        client->close();
    }
    m_clientToUpstream.clear();
    m_upstreamToClient.clear();
}

void CdpProxy::onNewConnection()
{
    QWebSocket *client = m_server->nextPendingConnection();
    if (!client)
        return;

    // Auth check: ?token= query param or Authorization: Bearer <token> header.
    if (!m_authToken.isEmpty()) {
        bool authorized = false;
        QUrlQuery query(client->requestUrl());
        if (query.queryItemValue(QStringLiteral("token")) == m_authToken)
            authorized = true;
        if (!authorized) {
            QByteArray authHeader = client->request().rawHeader("Authorization");
            if (!authHeader.isEmpty()) {
                QString authStr = QString::fromUtf8(authHeader);
                if (authStr.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)
                    && authStr.mid(7) == m_authToken)
                    authorized = true;
            }
        }
        if (!authorized) {
            client->close(QWebSocketProtocol::CloseCodeNormal,
                          QStringLiteral("Unauthorized"));
            client->deleteLater();
            return;
        }
    }

    // Extract target path and open an upstream connection to Chromium DevTools.
    QString path = client->requestUrl().path();
    QUrl upstreamUrl(QStringLiteral("ws://127.0.0.1:%1%2").arg(m_debugPort).arg(path));

    QWebSocket *upstream = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    m_clientToUpstream.insert(client, upstream);
    m_upstreamToClient.insert(upstream, client);

    connect(client, &QWebSocket::textMessageReceived, this, &CdpProxy::onClientMessage);
    connect(client, &QWebSocket::disconnected, this, &CdpProxy::onClientDisconnected);
    connect(upstream, &QWebSocket::textMessageReceived, this, &CdpProxy::onUpstreamMessage);
    connect(upstream, &QWebSocket::disconnected, this, &CdpProxy::onUpstreamDisconnected);

    upstream->open(upstreamUrl);
}

void CdpProxy::onClientMessage(const QString &message)
{
    QWebSocket *client = qobject_cast<QWebSocket *>(sender());
    if (!client)
        return;
    QWebSocket *upstream = m_clientToUpstream.value(client);
    if (!upstream)
        return;
    QJsonObject cmd = QJsonDocument::fromJson(message.toUtf8()).object();
    const QString handled = CdpExtensions::processCommand(cmd, nullptr);
    if (!handled.isEmpty()) {
        client->sendTextMessage(handled);
        return;
    }
    upstream->sendTextMessage(message);
}

void CdpProxy::onClientDisconnected()
{
    QWebSocket *client = qobject_cast<QWebSocket *>(sender());
    if (!client)
        return;
    QWebSocket *upstream = m_clientToUpstream.take(client);
    if (upstream) {
        m_upstreamToClient.remove(upstream);
        upstream->close();
        upstream->deleteLater();
    }
    client->deleteLater();
}

void CdpProxy::onUpstreamMessage(const QString &message)
{
    QWebSocket *upstream = qobject_cast<QWebSocket *>(sender());
    if (!upstream)
        return;
    QWebSocket *client = m_upstreamToClient.value(upstream);
    if (!client)
        return;
    client->sendTextMessage(message);
}

void CdpProxy::onUpstreamDisconnected()
{
    QWebSocket *upstream = qobject_cast<QWebSocket *>(sender());
    if (!upstream)
        return;
    QWebSocket *client = m_upstreamToClient.take(upstream);
    if (client) {
        m_clientToUpstream.remove(client);
        client->close();
        client->deleteLater();
    }
    upstream->deleteLater();
}
