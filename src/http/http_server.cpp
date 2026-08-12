#include "http/http_server.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QWebEnginePage>
#include <QWebEngineView>

#include "browser/anoa_browser.h"
#include "browser/tab_ids.h"

#include <QBuffer>
#include <QEventLoop>
#include <QHostAddress>
#include <QImage>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <memory>

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

// Close a response socket without losing what was just written to it.
//
// The old sequence — write, flush, disconnectFromHost, deleteLater — dropped
// anything that did not fit in the kernel's send buffer. deleteLater() destroys
// the socket on the next return to the event loop, which for a large body is
// long before the write buffer has drained; the peer got a truncated response
// while Content-Length still promised the whole thing. A 1280x720 PPM declared
// 2.7 MB and delivered nothing at all. (bug-004.)
//
// disconnectFromHost() already defers the close until pending writes are done.
// All that was missing is keeping the object alive that long, so deletion is
// driven by `disconnected` instead of by the next event loop turn.
static void closeWhenSent(QTcpSocket *socket)
{
    socket->flush();
    QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    // A peer that vanishes mid-write would otherwise leak the socket, since
    // `disconnected` is not guaranteed to arrive.
    QObject::connect(socket, &QAbstractSocket::errorOccurred, socket,
                     [socket](QAbstractSocket::SocketError) { socket->deleteLater(); });
    socket->disconnectFromHost();
}

static void sendResponse(QTcpSocket *socket, int statusCode, const QByteArray &statusText,
                         const QByteArray &body,
                         const QByteArray &contentType = "application/json")
{
    QByteArray response =
        "HTTP/1.1 " + QByteArray::number(statusCode) + " " + statusText + "\r\n"
        "Content-Type: " + contentType + "\r\n"
        "Content-Length: " + QByteArray::number(body.size()) + "\r\n"
        "Connection: close\r\n"
        "\r\n" + body;
    socket->write(response);
    closeWhenSent(socket);
}

struct HtmlCaptureState {
    QString html;
    bool timedOut = true;
    bool waiting = true;
    QEventLoop *loop = nullptr;
};

QByteArray HttpServer::rebuildTargetList(const QByteArray &rewritten,
                                         const QString &hostName) const
{
    if (!m_browser)
        return rewritten;

    const QJsonArray upstream = QJsonDocument::fromJson(rewritten).array();

    // Title and url come from Chromium where it has them: it knows the live
    // document, while the view's own properties lag a navigation slightly.
    QHash<QString, QJsonObject> byTargetId;
    for (const QJsonValue &value : upstream) {
        const QJsonObject target = value.toObject();
        byTargetId.insert(target.value(QStringLiteral("id")).toString(), target);
    }

    QList<TabTargetInfo> tabs;
    const QString activeId = m_browser->activeTabId();
    for (const QString &tabId : m_browser->tabIds()) {
        TabTargetInfo info;
        info.tabId = tabId;
        info.chromiumTargetId = m_browser->chromiumTargetId(tabId);
        info.active = (tabId == activeId);

        const QJsonObject target = byTargetId.value(info.chromiumTargetId);
        info.title = target.value(QStringLiteral("title")).toString();
        info.url = target.value(QStringLiteral("url")).toString();
        if (info.url.isEmpty()) {
            if (QWebEngineView *view = m_browser->viewFor(tabId)) {
                info.url = view->url().toString();
                info.title = view->title();
            }
        }
        tabs.append(info);
    }

    QJsonArray out = buildTargetList(tabs, hostName, m_proxyPort);
    // Nothing resolved yet — task-004's lookup is still in flight. Answering an
    // empty array here would show a client zero targets where it sees one
    // today, which reads as "the browser has no pages" rather than "ask again".
    if (out.isEmpty())
        return rewritten;

    // Everything that is not a page — service workers, iframes — keeps its
    // upstream entry, already host-rewritten, so no client loses a target it
    // can see today.
    for (const QJsonValue &value : upstream) {
        const QJsonObject target = value.toObject();
        if (target.value(QStringLiteral("type")).toString() != QLatin1String("page"))
            out.append(target);
    }

    return QJsonDocument(out).toJson(QJsonDocument::Compact);
}

void HttpServer::handleNewConnection()
{
    QTcpSocket *socket = m_server->nextPendingConnection();
    if (!socket)
        return;

    QByteArray requestData;
    while (!requestData.contains("\r\n\r\n")) {
        if (!socket->waitForReadyRead(5000)) {
            socket->disconnectFromHost();
            socket->deleteLater();
            return;
        }
        requestData += socket->readAll();
    }

    int firstLineEnd = requestData.indexOf("\r\n");
    QList<QByteArray> requestLineParts = requestData.left(firstLineEnd).split(' ');
    if (requestLineParts.size() < 2) {
        socket->disconnectFromHost();
        socket->deleteLater();
        return;
    }

    QString method = QString::fromUtf8(requestLineParts[0]);
    QString rawPath = QString::fromUtf8(requestLineParts[1]);

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

    QUrl url(rawPath);
    QString path = url.path();
    QUrlQuery query(url.query());
    QString hostHeader = headers.value(QStringLiteral("host"),
                                       QStringLiteral("127.0.0.1:%1").arg(m_port));

    if (!m_authToken.isEmpty()) {
        QString authHeader = headers.value(QStringLiteral("authorization"));
        bool viaBearer = authHeader.startsWith(QStringLiteral("Bearer "), Qt::CaseInsensitive)
                         && authHeader.mid(7) == m_authToken;
        bool viaQuery = query.queryItemValue(QStringLiteral("token")) == m_authToken;
        if (!viaBearer && !viaQuery) {
            sendResponse(socket, 401, "Unauthorized", R"({"error":"unauthorized"})");
            return;
        }
    }

    // Redirect /render/ (trailing slash) to /render, preserving query string.
    if (method == QLatin1String("GET") && path == QLatin1String("/render/")) {
        QString location = QStringLiteral("/render");
        QString origQuery = url.query();
        if (!origQuery.isEmpty())
            location += QStringLiteral("?") + origQuery;
        QByteArray response =
            "HTTP/1.1 301 Moved Permanently\r\n"
            "Location: " + location.toUtf8() + "\r\n"
            "Content-Length: 0\r\n"
            "Connection: close\r\n"
            "\r\n";
        socket->write(response);
        closeWhenSent(socket);
        return;
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

            // The target list is rebuilt from the registry rather than
            // byte-patched, because a tab id is ours and appears nowhere in
            // Chromium's answer. /json/version is left alone: its
            // webSocketDebuggerUrl is the browser endpoint, not a page.
            if (path != QLatin1String("/json/version"))
                body = rebuildTargetList(body, hostName);
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
            sendResponse(socket, 503, "Service Unavailable", "capture failed", "text/plain");
        } else {
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/png\r\n"
                "Cache-Control: no-cache\r\n"
                "X-Anoa-Viewport-Width: " + QByteArray::number(m_browser->width()) + "\r\n"
                "X-Anoa-Viewport-Height: " + QByteArray::number(m_browser->height()) + "\r\n"
                "Content-Length: " + QByteArray::number(pngBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += pngBytes;
            socket->write(response);
            closeWhenSent(socket);
        }
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render/html")) {
        auto state = std::make_shared<HtmlCaptureState>();

        if (m_browser && m_browser->page()) {
            QEventLoop loop;
            QTimer timer;
            state->loop = &loop;
            timer.setSingleShot(true);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(5000);
            m_browser->page()->toHtml([state](const QString &html) {
                if (!state->waiting)
                    return;
                state->html = html;
                state->timedOut = false;
                if (state->loop)
                    state->loop->quit();
            });
            loop.exec();
            state->waiting = false;
            state->loop = nullptr;
        }

        if (state->timedOut) {
            sendResponse(socket, 504, "Gateway Timeout", "html capture timeout", "text/plain");
        } else {
            QByteArray htmlBytes = state->html.toUtf8();
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Cache-Control: no-cache\r\n"
                "Content-Length: " + QByteArray::number(htmlBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += htmlBytes;
            socket->write(response);
            closeWhenSent(socket);
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
            sendResponse(socket, 400, "Bad Request", "invalid url", "text/plain");
            return;
        }

        QString scheme = parsedUrl.scheme().toLower();
        if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
            && scheme != QLatin1String("file")) {
            sendResponse(socket, 400, "Bad Request", "scheme not allowed", "text/plain");
            return;
        }

        if (m_browser)
            m_browser->load(parsedUrl);

        sendResponse(socket, 200, "OK", "navigating", "text/plain");
    } else if (method == QLatin1String("POST")
               && (path == QLatin1String("/render/back")
                   || path == QLatin1String("/render/forward")
                   || path == QLatin1String("/render/reload"))) {
        // History control. Deliberately not one endpoint with a direction
        // parameter: these are three verbs a caller either has or does not,
        // and a typo in a parameter would silently do the wrong one.
        //
        // back/forward at the end of history are no-ops in Chromium, so there
        // is nothing to report but acceptance — answering 200 for a request
        // that changed nothing is the honest description of what happened.
        if (m_browser) {
            if (path.endsWith(QLatin1String("back")))
                m_browser->back();
            else if (path.endsWith(QLatin1String("forward")))
                m_browser->forward();
            else
                m_browser->reload();
        }
        sendResponse(socket, 200, "OK", "ok", "text/plain");
    } else if (method == QLatin1String("GET") && path == QLatin1String("/render")) {
        QString screenshotUrl = QStringLiteral("/render/screenshot.png");
        if (!m_authToken.isEmpty()) {
            QUrlQuery screenshotQuery;
            screenshotQuery.addQueryItem(QStringLiteral("token"), m_authToken);
            screenshotUrl += QStringLiteral("?") + screenshotQuery.toString(QUrl::FullyEncoded);
        }

        static const char htmlTmpl[] = R"(<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<title>Anoa Live View</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%;background:#000;overflow:hidden}
#frame{display:block;width:100%;height:100%;object-fit:contain}
</style>
</head>
<body>
<img id="frame">
<script>
var src="%1";
var img=document.getElementById("frame");
function refresh(){img.src=src+(src.indexOf("?")>=0?"&":"?")+"_t="+Date.now();}
refresh();
setInterval(refresh,500);
</script>
</body>
</html>)";

        QByteArray htmlBytes = QString::fromUtf8(htmlTmpl).arg(screenshotUrl).toUtf8();
        QByteArray response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Content-Length: " + QByteArray::number(htmlBytes.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n";
        response += htmlBytes;
        socket->write(response);
        closeWhenSent(socket);
    } else if (method == QLatin1String("GET")
               && path == QLatin1String("/render/screenshot.ppm")) {
        // Binary PPM (P6) frame, optionally scaled server-side via ?w=&h=.
        // Terminal clients parse PPM with ~20 lines of code — no image decoder
        // needed. X-Anoa-Viewport-* headers carry the logical widget size so
        // clients can map image pixels back to click coordinates.
        QByteArray ppmBytes;
        int viewportW = 0, viewportH = 0;
        if (m_browser) {
            QPixmap pixmap = m_browser->grab();
            if (!pixmap.isNull()) {
                viewportW = m_browser->width();
                viewportH = m_browser->height();
                QImage img = pixmap.toImage();
                int reqW = query.queryItemValue(QStringLiteral("w")).toInt();
                int reqH = query.queryItemValue(QStringLiteral("h")).toInt();
                if (reqW > 0 && reqH > 0)
                    img = img.scaled(reqW, reqH, Qt::KeepAspectRatio,
                                     Qt::SmoothTransformation);
                img = img.convertToFormat(QImage::Format_RGB888);
                ppmBytes = "P6\n" + QByteArray::number(img.width()) + " "
                           + QByteArray::number(img.height()) + "\n255\n";
                // Copy width*3 bytes per row — scanlines are 4-byte aligned so
                // bytesPerLine() may include padding that must not be emitted.
                for (int y = 0; y < img.height(); ++y)
                    ppmBytes.append(reinterpret_cast<const char *>(img.constScanLine(y)),
                                    img.width() * 3);
            }
        }
        if (ppmBytes.isEmpty()) {
            sendResponse(socket, 503, "Service Unavailable", "capture failed", "text/plain");
        } else {
            QByteArray response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: image/x-portable-pixmap\r\n"
                "Cache-Control: no-cache\r\n"
                "X-Anoa-Viewport-Width: " + QByteArray::number(viewportW) + "\r\n"
                "X-Anoa-Viewport-Height: " + QByteArray::number(viewportH) + "\r\n"
                "Content-Length: " + QByteArray::number(ppmBytes.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n";
            response += ppmBytes;
            socket->write(response);
            closeWhenSent(socket);
        }
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/click")) {
        bool okX = false, okY = false;
        int x = query.queryItemValue(QStringLiteral("x")).toInt(&okX);
        int y = query.queryItemValue(QStringLiteral("y")).toInt(&okY);
        if (!okX || !okY || x < 0 || y < 0) {
            sendResponse(socket, 400, "Bad Request", "invalid coordinates", "text/plain");
            return;
        }

        QString buttonStr = query.queryItemValue(QStringLiteral("button")).toLower();
        Qt::MouseButton button = Qt::LeftButton;
        if (buttonStr == QLatin1String("right"))
            button = Qt::RightButton;
        else if (buttonStr == QLatin1String("middle"))
            button = Qt::MiddleButton;
        else if (!buttonStr.isEmpty() && buttonStr != QLatin1String("left")) {
            sendResponse(socket, 400, "Bad Request", "invalid button", "text/plain");
            return;
        }

        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        m_browser->sendClick(QPoint(x, y), button);
        sendResponse(socket, 200, "OK", "clicked", "text/plain");
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/scroll")) {
        bool okDy = false;
        int dy = query.queryItemValue(QStringLiteral("dy")).toInt(&okDy);
        if (!okDy || dy == 0) {
            sendResponse(socket, 400, "Bad Request", "invalid dy", "text/plain");
            return;
        }

        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        // x/y optional — default to the viewport center.
        bool okX = false, okY = false;
        int x = query.queryItemValue(QStringLiteral("x")).toInt(&okX);
        int y = query.queryItemValue(QStringLiteral("y")).toInt(&okY);
        QPoint pos(okX && x >= 0 ? x : m_browser->width() / 2,
                   okY && y >= 0 ? y : m_browser->height() / 2);

        m_browser->sendScroll(pos, dy);
        sendResponse(socket, 200, "OK", "scrolled", "text/plain");
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/type")) {
        // Prefer text from the query string; fall back to the request body
        // (same pattern as /render/navigate) for long payloads.
        QString text = query.queryItemValue(QStringLiteral("text"), QUrl::FullyDecoded);
        if (text.isEmpty()) {
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
            text = QString::fromUtf8(bodyBytes);
        }

        if (text.isEmpty()) {
            sendResponse(socket, 400, "Bad Request", "empty text", "text/plain");
            return;
        }
        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        m_browser->sendText(text);
        sendResponse(socket, 200, "OK", "typed", "text/plain");
    } else if (method == QLatin1String("POST") && path == QLatin1String("/render/key")) {
        const QString keyName = query.queryItemValue(QStringLiteral("key"));
        if (keyName.isEmpty()) {
            sendResponse(socket, 400, "Bad Request", "missing key", "text/plain");
            return;
        }
        if (!m_browser) {
            sendResponse(socket, 503, "Service Unavailable", "no browser", "text/plain");
            return;
        }

        if (!m_browser->sendKey(keyName)) {
            sendResponse(socket, 400, "Bad Request", "unknown key", "text/plain");
            return;
        }
        sendResponse(socket, 200, "OK", "key sent", "text/plain");
    } else if (method == QLatin1String("GET")
               && path == QLatin1String("/render/stream.mjpeg")) {
        // Send MJPEG stream headers — keep socket open, no Content-Length.
        QByteArray streamHeader =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        socket->write(streamHeader);
        socket->flush();

        // Timer fires every 100 ms (~10 fps). Parented to socket so it is
        // cleaned up automatically if socket is deleted before disconnected fires.
        QTimer *frameTimer = new QTimer(socket);
        frameTimer->setInterval(100);

        AnoaBrowser *browser = m_browser;
        connect(frameTimer, &QTimer::timeout, socket, [browser, socket]() {
            // Skip this frame if the write buffer is backed up beyond 512 KB.
            if (socket->bytesToWrite() > 512 * 1024)
                return;
            if (!browser)
                return;

            QPixmap pixmap = browser->grab();
            if (pixmap.isNull())
                return;

            QByteArray jpegBytes;
            QBuffer buf(&jpegBytes);
            buf.open(QIODevice::WriteOnly);
            if (!pixmap.save(&buf, "JPEG", 70))
                return;
            buf.close();

            QByteArray part;
            part += "--frame\r\n";
            part += "Content-Type: image/jpeg\r\n";
            part += "Content-Length: " + QByteArray::number(jpegBytes.size()) + "\r\n";
            part += "\r\n";
            part += jpegBytes;
            part += "\r\n";

            socket->write(part);
            socket->flush();
        });

        connect(socket, &QTcpSocket::disconnected, socket, [socket, frameTimer]() {
            frameTimer->stop();
            socket->deleteLater();
        });

        frameTimer->start();
        // Do not close socket — stream runs until client disconnects.
    } else {
        sendResponse(socket, 404, "Not Found", R"({"error":"not found"})");
    }
}
