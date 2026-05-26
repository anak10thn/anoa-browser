#include "http/http_server.h"

#include "browser/anoa_browser.h"

#include <QBuffer>
#include <QEventLoop>
#include <QHostAddress>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

HttpServer::HttpServer(quint16 port, quint16 debuggingPort, quint16 proxyPort,
                       const QString &authToken, AnoaBrowser *browser, QObject *parent)
    : QObject(parent)
    , m_server(new QTcpServer(this))
    , m_port(port)
    , m_debugPort(debuggingPort)
    , m_proxyPort(proxyPort)
    , m_authToken(authToken)
    , m_browser(browser)
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
    // Normalize trailing slash: Playwright requests /json/version/ with a slash.
    if (path.endsWith('/') && path.size() > 1)
        path.chop(1);
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
            // Strip port from Host header to get bare hostname.
            QString hostName = hostHeader;
            int colonIdx = hostName.lastIndexOf(':');
            if (colonIdx != -1) {
                bool ok = false;
                hostName.mid(colonIdx + 1).toUShort(&ok);
                if (ok)
                    hostName = hostName.left(colonIdx);
            }
            // Rewrite "127.0.0.1:<debugPort>" to "hostname:<proxyPort>" first so
            // that webSocketDebuggerUrl points at the CDP proxy (port+2) rather than
            // the raw Chromium DevTools port (port+1).
            body.replace(
                QByteArrayLiteral("127.0.0.1:") + QByteArray::number(m_debugPort),
                hostName.toUtf8() + ":" + QByteArray::number(m_proxyPort)
            );
            // Rewrite any remaining bare 127.0.0.1 references.
            body.replace(QByteArrayLiteral("127.0.0.1"), hostName.toUtf8());
        } else {
            body = R"({"error":"upstream unavailable"})";
            statusCode = 503;
        }
        reply->deleteLater();

        QByteArray statusText = (statusCode == 200) ? "OK" : "Service Unavailable";
        sendResponse(socket, statusCode, statusText, body);
    } else if (method == QLatin1String("GET")
               && path == QLatin1String("/render/screenshot.png")) {
        QByteArray pngBytes;
        bool ok = false;
        if (m_browser) {
            QPixmap pixmap = m_browser->grab();
            if (!pixmap.isNull()) {
                QBuffer buf(&pngBytes);
                buf.open(QIODevice::WriteOnly);
                ok = pixmap.save(&buf, "PNG");
            }
        }
        if (!ok) {
            QByteArray body = "capture failed";
            QByteArray response =
                "HTTP/1.1 503 Service Unavailable\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
        } else {
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/png\r\n"
                "Cache-Control: no-cache\r\n"
                "Content-Length: " + QByteArray::number(pngBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += pngBytes;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
        }
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render/html")) {
        QString htmlResult;
        bool timedOut = true;

        if (m_browser && m_browser->page()) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(5000);
            m_browser->page()->toHtml([&](const QString &html) {
                htmlResult = html;
                timedOut = false;
                loop.quit();
            });
            loop.exec();
        }

        if (timedOut) {
            QByteArray body = "html capture timeout";
            QByteArray response =
                "HTTP/1.1 504 Gateway Timeout\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
        } else {
            QByteArray htmlBytes = htmlResult.toUtf8();
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-cache\r\n"
                "Content-Length: " + QByteArray::number(htmlBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += htmlBytes;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
        }
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/navigate")) {
        // Prefer url from query string; fall back to plain-text request body.
        QString navUrl = query.queryItemValue(QStringLiteral("url"), QUrl::FullyDecoded);
        if (navUrl.isEmpty()) {
            QByteArray bodyBytes = requestData.mid(headerEnd + 4);
            bool lengthOk = false;
            int contentLength = headers.value(QStringLiteral("content-length")).toInt(&lengthOk);
            if (lengthOk && contentLength > bodyBytes.size()) {
                while (bodyBytes.size() < contentLength) {
                    if (!socket->waitForReadyRead(5000))
                        break;
                    bodyBytes += socket->readAll();
                }
            }
            navUrl = QString::fromUtf8(bodyBytes.trimmed());
        }

        QUrl parsedUrl(navUrl);
        if (navUrl.isEmpty() || !parsedUrl.isValid() || parsedUrl.isRelative()) {
            QByteArray body = "invalid url";
            QByteArray response =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }

        QString scheme = parsedUrl.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
            && scheme != QLatin1String("file")) {
            QByteArray body = "scheme not allowed";
            QByteArray response =
                "HTTP/1.1 400 Bad Request\r\n"
                "Content-Type: text/plain\r\n"
                "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
            socket->write(response);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }

        if (m_browser)
            m_browser->load(parsedUrl);

        QByteArray body = "navigating";
        QByteArray response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body;
        socket->write(response);
        socket->flush();
        socket->disconnectFromHost();
        socket->deleteLater();
    } else {
        sendResponse(socket, 404, "Not Found", R"({"error":"not found"})");
    }
}
