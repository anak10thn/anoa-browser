#include "http/http_server.h"

#include <QEventLoop>
#include <QHostAddress>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

HttpServer::HttpServer(quint16 port, quint16 debuggingPort, const QString &authToken,
                       QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(port)
    , m_debugPort(debuggingPort)
    , m_authToken(authToken)
{
    connect(m_server, &QTcpServer::newConnection, this, &HttpServer::handleNewConnection);
}

bool HttpServer::start()
{
    return m_server->listen(QHostAddress::Any, m_port);
}

void HttpServer::stop()
{
    m_server->close();
}

static void sendResponse(QTcpSocket *socket, int statusCode, const QByteArray &statusText,
                         const QByteArray &body)
{
    QByteArray response =
        "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText + "\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    socket->write(response);
    socket->flush();
    socket->disconnectFromHost();
    socket->deleteLater();
}

void HttpServer::handleNewConnection()
{
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;

    // Accumulate data until the end of HTTP headers (\r\n\r\n).
    QByteArray requestData;
    while (!requestData.contains("\r\n\r\n")) {
        if (!socket->waitForReadyRead(5000)) {
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }
        requestData += socket->readAll();
    }

    // Parse request line.
    int firstLineEnd = requestData.indexOf("\r\n");
    QList<QByteArray> requestLineParts = requestData.left(firstLineEnd).split(' ');
    if (requestLineParts.size() < 2) {
        socket->disconnectFromHost();
        socket->deleteLater();
        return;
    }

    QString method = QString::fromUtf8(requestLineParts[0]);
    QString rawPath = QString::fromUtf8(requestLineParts[1]);

    // Parse headers into a lowercased map.
    QMap<QString, QString> headers;
    int headerEnd = requestData.indexOf("\r\n\r\n");
    QByteArray headerSection = requestData.mid(firstLineEnd + 2, headerEnd - firstLineEnd - 2);
    for (const QByteArray &line : headerSection.split('\n')) {
        QByteArray trimmed = line.trimmed();
        int colonPos = trimmed.indexOf(':');
        if (colonPos > 0) {
            QString key = QString::fromUtf8(trimmed.left(colonPos)).trimmed().toLower();
            QString value = QString::fromUtf8(trimmed.mid(colonPos + 1)).trimmed();
            headers[key] = value;
        }
    }

    // Decompose path and query string.
    QUrl url(rawPath);
    QString path = url.path();
    QUrlQuery query(url.query());
    QString hostHeader = headers.value(QStringLiteral("host"),
                                       QStringLiteral("127.0.0.1:%1").arg(m_port));

    // Auth check.
    if (!m_authToken.isEmpty()) {
        bool authorized = false;
        QString authHeader = headers.value(QStringLiteral("authorization"));
        if (authHeader.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)
            && authHeader.mid(7) == m_authToken) {
            authorized = true;
        }
        if (!authorized && query.queryItemValue(QStringLiteral("token")) == m_authToken) {
            authorized = true;
        }
        if (!authorized) {
            sendResponse(socket, 401, "Unauthorized", R"({"error":"unauthorized"})");
            return;
        }
    }

    // Route CDP discovery paths to the internal Chromium debugging port.
    bool isDiscovery = (path == QLatin1String("/json")
                        || path == QLatin1String("/json/list")
                        || path == QLatin1String("/json/version"));

    if (method == QLatin1String("GET") && isDiscovery) {
        QNetworkAccessManager nam;
        QUrl targetUrl(QString("http://127.0.0.1:%1%2").arg(m_debugPort).arg(path));
        QNetworkReply *reply = nam.get(QNetworkRequest(targetUrl));

        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(5000);
        loop.exec();

        QByteArray body;
        int statusCode = 200;
        if (reply->error() == QNetworkReply::NoError) {
            body = reply->readAll();
            // Rewrite 127.0.0.1 to the client-visible hostname so that
            // webSocketDebuggerUrl is reachable from outside the host.
            QString hostName = hostHeader;
            int colonIdx = hostName.lastIndexOf(':');
            if (colonIdx != -1) {
                bool ok = false;
                hostName.mid(colonIdx + 1).toUShort(&ok);
                if (ok)
                    hostName = hostName.left(colonIdx);
            }
            body.replace(QByteArrayLiteral("127.0.0.1"), hostName.toUtf8());
        } else {
            body = R"({"error":"upstream unavailable"})";
            statusCode = 503;
        }
        reply->deleteLater();

        QByteArray statusText = (statusCode == 200) ? "OK" : "Service Unavailable";
        sendResponse(socket, statusCode, statusText, body);
    } else {
        sendResponse(socket, 404, "Not Found", R"({"error":"not found"})");
    }
}
