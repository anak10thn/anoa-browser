#include "terminal/cdp_client.h"

#include <QAbstractSocket>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QList>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketProtocol>

namespace {

// Reconnect backoff: doubles from the base up to the cap and stays there, so a
// server that is down produces one attempt every 8 s rather than a tight loop.
constexpr int kBaseReconnectMs = 250;
constexpr int kMaxReconnectMs = 8000;

// How often expired requests are swept. Coarse on purpose: the deadline that
// matters is the 5 s one, and a timer that wakes 4 times a second costs
// nothing next to the frame timer running at 30 fps.
constexpr int kSweepIntervalMs = 250;

QString hostPort(const QUrl &url)
{
    if (url.host().isEmpty())
        return url.toString();
    if (url.port() < 0)
        return url.host();
    return QStringLiteral("%1:%2").arg(url.host()).arg(url.port());
}

} // namespace

CdpClient::CdpClient(const QString &token, QObject *parent)
    : QObject(parent)
    , m_socket(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this))
    , m_reconnectTimer(new QTimer(this))
    , m_timeoutTimer(new QTimer(this))
    , m_token(token)
    , m_stateText(QStringLiteral("disconnected"))
{
    m_clock.start();

    m_reconnectTimer->setSingleShot(true);
    m_timeoutTimer->setInterval(kSweepIntervalMs);

    connect(m_socket, &QWebSocket::connected, this, &CdpClient::onSocketConnected);
    connect(m_socket, &QWebSocket::disconnected, this, &CdpClient::onSocketDisconnected);
    // Qt drops trailing signal arguments, so the SocketError is not carried
    // into the slot; QWebSocket::errorString() has the text we want anyway.
    connect(m_socket, &QWebSocket::errorOccurred, this, &CdpClient::onSocketError);
    connect(m_socket, &QWebSocket::textMessageReceived, this,
            &CdpClient::onTextMessageReceived);
    connect(m_reconnectTimer, &QTimer::timeout, this, &CdpClient::onReconnectTimeout);
    connect(m_timeoutTimer, &QTimer::timeout, this, &CdpClient::onTimeoutSweep);
}

CdpClient::~CdpClient()
{
    // Not disconnectFromEndpoint(): handlers must not run from a destructor,
    // where anything they capture may already be gone.
    m_closing = true;
    m_reconnectTimer->stop();
    m_timeoutTimer->stop();
    m_pending.clear();
    m_socket->abort();
}

void CdpClient::connectToEndpoint(const QUrl &url)
{
    m_endpoint = url;
    m_closing = false;
    m_attempt = 0;
    openSocket();
}

void CdpClient::disconnectFromEndpoint()
{
    m_closing = true;
    m_reconnectTimer->stop();
    m_queued.clear();
    failAllPending(QStringLiteral("connection closed"), ErrorConnectionLost);
    m_socket->close();
    setState(State::Disconnected, QStringLiteral("disconnected"));
}

void CdpClient::setRequestTimeout(int ms)
{
    m_requestTimeoutMs = ms > 0 ? ms : 1;
}

QString CdpClient::description() const
{
    return hostPort(m_endpoint);
}

int CdpClient::send(const QString &method, const QJsonObject &params, Handler onReply)
{
    const int id = ++m_nextId;

    QJsonObject cmd;
    cmd[QStringLiteral("id")] = id;
    cmd[QStringLiteral("method")] = method;
    // CDP tolerates an absent "params" but not a null one; omit it when empty.
    if (!params.isEmpty())
        cmd[QStringLiteral("params")] = params;

    Pending pending;
    pending.id = id;
    pending.method = method;
    pending.handler = std::move(onReply);
    pending.deadline = m_clock.elapsed() + m_requestTimeoutMs;
    m_pending.insert(id, pending);
    armTimeoutSweep();

    const QString text =
        QString::fromUtf8(QJsonDocument(cmd).toJson(QJsonDocument::Compact));
    if (m_socket->state() == QAbstractSocket::ConnectedState)
        m_socket->sendTextMessage(text);
    else
        m_queued.append(text); // flushed by onSocketConnected()

    return id;
}

int CdpClient::navigate(const QString &url, Handler onReply)
{
    QJsonObject params;
    params[QStringLiteral("url")] = url;
    return send(QStringLiteral("Page.navigate"), params, std::move(onReply));
}

QUrl CdpClient::authorizedUrl(const QUrl &url) const
{
    if (m_token.isEmpty())
        return url;
    QUrl out = url;
    QUrlQuery query(out.query());
    query.removeAllQueryItems(QStringLiteral("token"));
    query.addQueryItem(QStringLiteral("token"), m_token);
    out.setQuery(query);
    return out;
}

void CdpClient::openSocket()
{
    if (m_endpoint.isEmpty())
        return;

    QNetworkRequest request(authorizedUrl(m_endpoint));
    if (!m_token.isEmpty()) {
        request.setRawHeader(QByteArrayLiteral("Authorization"),
                             QByteArrayLiteral("Bearer ") + m_token.toUtf8());
    }

    const QString target = hostPort(m_endpoint);
    if (m_attempt > 0) {
        setState(State::Reconnecting,
                 QStringLiteral("reconnecting to %1 (attempt %2)").arg(target).arg(m_attempt));
    } else {
        setState(State::Connecting, QStringLiteral("connecting to %1").arg(target));
    }
    m_socket->open(request);
}

void CdpClient::setState(State state, const QString &text)
{
    if (m_state == state && m_stateText == text)
        return;
    m_state = state;
    m_stateText = text;
    emit stateChanged(text);
}

void CdpClient::scheduleReconnect(const QString &reason)
{
    if (m_closing || m_endpoint.isEmpty() || m_reconnectTimer->isActive())
        return;

    ++m_attempt;
    int delay = kBaseReconnectMs;
    for (int i = 1; i < m_attempt && delay < kMaxReconnectMs; ++i)
        delay *= 2;
    if (delay > kMaxReconnectMs)
        delay = kMaxReconnectMs;

    setState(State::Reconnecting,
             QStringLiteral("%1 - retrying in %2 ms (attempt %3)")
                 .arg(reason)
                 .arg(delay)
                 .arg(m_attempt));
    emit retryScheduled(m_attempt, delay);
    m_reconnectTimer->start(delay);
}

void CdpClient::armTimeoutSweep()
{
    if (!m_pending.isEmpty() && !m_timeoutTimer->isActive())
        m_timeoutTimer->start();
}

void CdpClient::resolve(Pending &pending, const CdpResult &result)
{
    if (!pending.handler)
        return;
    // Moved out first: the handler may call send() again, and a re-entrant
    // rehash of m_pending must not be able to reach this entry.
    const Handler handler = std::move(pending.handler);
    pending.handler = Handler();
    handler(result);
}

void CdpClient::failAllPending(const QString &reason, int code)
{
    if (m_pending.isEmpty())
        return;

    QHash<int, Pending> taken;
    taken.swap(m_pending);
    m_timeoutTimer->stop();

    CdpResult failure;
    failure.errorCode = code;
    failure.errorMessage = reason;
    for (auto it = taken.begin(); it != taken.end(); ++it)
        resolve(*it, failure);

    // A handler may have issued a fresh request while unwinding.
    armTimeoutSweep();
}

void CdpClient::onSocketConnected()
{
    m_attempt = 0;
    setState(State::Connected, QStringLiteral("connected to %1").arg(hostPort(m_endpoint)));

    const QStringList queued = m_queued;
    m_queued.clear();
    for (const QString &text : queued)
        m_socket->sendTextMessage(text);

    emit connected();
}

void CdpClient::onSocketDisconnected()
{
    if (m_closing)
        return;

    m_queued.clear(); // ids are already allocated; those requests fail below
    failAllPending(QStringLiteral("connection lost"), ErrorConnectionLost);
    emit disconnected(QStringLiteral("connection lost"));
    scheduleReconnect(QStringLiteral("connection lost"));
}

void CdpClient::onSocketError()
{
    if (m_closing)
        return;

    QString reason = m_socket->errorString();
    if (reason.isEmpty())
        reason = QStringLiteral("connection failed");

    // A handshake that never completed emits errorOccurred() without a
    // preceding disconnected(), so the pending work is failed here too.
    m_queued.clear();
    failAllPending(reason, ErrorConnectionLost);
    scheduleReconnect(reason);
}

void CdpClient::onReconnectTimeout()
{
    if (m_closing)
        return;
    openSocket();
}

void CdpClient::onTextMessageReceived(const QString &message)
{
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit protocolError(QStringLiteral("malformed CDP frame: %1")
                               .arg(parseError.errorString()));
        return;
    }
    const QJsonObject frame = doc.object();

    // No "id" means an unsolicited event, not an orphan response.
    if (!frame.contains(QStringLiteral("id"))) {
        const QString method = frame.value(QStringLiteral("method")).toString();
        if (method.isEmpty()) {
            emit protocolError(QStringLiteral("CDP frame with neither id nor method"));
            return;
        }
        emit eventReceived(method, frame.value(QStringLiteral("params")).toObject());
        return;
    }

    const int id = frame.value(QStringLiteral("id")).toInt();
    const auto it = m_pending.find(id);
    if (it == m_pending.end()) {
        // Almost always a reply that arrived after its own timeout fired.
        emit protocolError(QStringLiteral("CDP reply for unknown id %1").arg(id));
        return;
    }

    Pending pending = std::move(*it);
    m_pending.erase(it);
    if (m_pending.isEmpty())
        m_timeoutTimer->stop();

    CdpResult result;
    if (frame.contains(QStringLiteral("error"))) {
        const QJsonObject error = frame.value(QStringLiteral("error")).toObject();
        result.errorCode = error.value(QStringLiteral("code")).toInt();
        result.errorMessage = error.value(QStringLiteral("message")).toString();
        if (result.errorMessage.isEmpty())
            result.errorMessage = QStringLiteral("%1 failed").arg(pending.method);
        const QString data = error.value(QStringLiteral("data")).toString();
        if (!data.isEmpty())
            result.errorMessage += QStringLiteral(": ") + data;
    } else {
        result.ok = true;
        result.result = frame.value(QStringLiteral("result")).toObject();
    }
    resolve(pending, result);
}

void CdpClient::onTimeoutSweep()
{
    const qint64 now = m_clock.elapsed();

    // Collect first, resolve second: a handler is free to call send(), which
    // would invalidate an iterator held across the callback.
    QList<int> expired;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it) {
        if (it->deadline <= now)
            expired.append(it.key());
    }

    for (const int id : expired) {
        Pending pending = m_pending.take(id);
        CdpResult result;
        result.errorCode = ErrorTimeout;
        result.errorMessage = QStringLiteral("%1 timed out after %2 ms")
                                  .arg(pending.method)
                                  .arg(m_requestTimeoutMs);
        resolve(pending, result);
    }

    if (m_pending.isEmpty())
        m_timeoutTimer->stop();
}
