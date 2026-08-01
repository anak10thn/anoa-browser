#include "cdp/cdp_client.h"

#include <cstdio> // stderr, for the QTextStream error lines

#include <QAbstractSocket>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTextStream>
#include <QTimer>
#include <QUrlQuery>
#include <QWebSocket>
#include <QWebSocketProtocol>
#include <QtGlobal>

namespace {

// Reconnect backoff: doubles from the base up to the cap and stays there, so a
// server that is down produces one attempt every 8 s rather than a tight loop.
constexpr int kBaseReconnectMs = 250;
constexpr int kMaxReconnectMs = 8000;

// How often expired requests are swept. Coarse on purpose: the deadline that
// matters is the 5 s one, and a timer that wakes 4 times a second costs
// nothing next to the frame timer running at 30 fps.
constexpr int kSweepIntervalMs = 250;

// Prefix for the handful of messages this class writes straight to stderr.
// Same wording as terminal_app.cpp, because to the user it is one program.
const char kErrPrefix[] = "anoa terminal: ";

QString hostPort(const QUrl &url)
{
    if (url.host().isEmpty())
        return url.toString();
    if (url.port() < 0)
        return url.host();
    return QStringLiteral("%1:%2").arg(url.host()).arg(url.port());
}

// One target row of /json/list, reduced to what the picker needs.
struct PageTarget {
    QString title;
    QString url;
    QString socketUrl;
};

// Target titles are page <title> text: arbitrary length, and arbitrary bytes
// once a page is hostile. Trim it to something that fits a listing line.
QString shortTitle(const QString &title)
{
    QString out = title.simplified();
    if (out.isEmpty())
        return QStringLiteral("(untitled)");
    const int kMax = 60;
    if (out.size() > kMax)
        out = out.left(kMax - 3) + QStringLiteral("...");
    return out;
}

// http://host:port -> http://host:port/json/list. A path that is already
// spelled out is kept verbatim, so pointing --cdp at a proxy that serves the
// target list somewhere else still works.
QUrl discoveryUrlFor(const QUrl &base)
{
    QUrl out = base;
    out.setFragment(QString());

    QString path = base.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    out.setPath(path.isEmpty() ? QStringLiteral("/json/list") : path);
    return out;
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
    // QWebSocket::errorOccurred only exists from 6.5; on 6.4 (what Ubuntu 24.04
    // and Debian bookworm ship, and what find_package above still allows) the
    // signal is error(), which needs the overload spelled out because a
    // same-named getter shares the address.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(m_socket, &QWebSocket::errorOccurred, this, &CdpClient::onSocketError);
#else
    connect(m_socket, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error), this,
            &CdpClient::onSocketError);
#endif
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
    if (m_discoveryReply)
        m_discoveryReply->abort(); // onDiscoveryFinished() returns at m_closing
    m_socket->abort();
}

void CdpClient::connectToEndpoint(const QUrl &url)
{
    m_requestedUrl = url;
    m_closing = false;
    m_attempt = 0;

    const QString scheme = url.scheme().toLower();

    if (scheme == QLatin1String("ws")) {
        // Already a page target: no discovery, dial it.
        m_endpoint = url;
        openSocket();
        return;
    }

    if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) {
        m_endpoint = QUrl(); // resolved by discovery
        startDiscovery(url);
        return;
    }

    // config.cpp's validateCdpUrl() has already turned wss:// (and every other
    // scheme) into a parse-time exit, so this is a programming error rather
    // than user input. The runtime branch exists because Q_ASSERT compiles out
    // of a release build and a silent no-op here would look like a hang.
    Q_ASSERT_X(scheme != QLatin1String("wss"), "CdpClient::connectToEndpoint",
               "wss:// must be rejected by config validation before it reaches the client");
    failDiscovery(QStringLiteral("--cdp scheme '%1' is not supported; use ws://, http:// or https://")
                      .arg(url.scheme()));
}

void CdpClient::disconnectFromEndpoint()
{
    m_closing = true;
    m_reconnectTimer->stop();
    m_queued.clear();
    if (m_discoveryReply)
        m_discoveryReply->abort(); // finished() fires, and m_closing swallows it
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
    // Before discovery resolves, the ws:// endpoint is not known yet — but the
    // host:port the user typed is, and that is what the status bar wants.
    return hostPort(m_endpoint.isEmpty() ? m_requestedUrl : m_endpoint);
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

void CdpClient::applyAuth(QNetworkRequest &request) const
{
    if (m_token.isEmpty())
        return;
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bearer ") + m_token.toUtf8());
}

void CdpClient::startDiscovery(const QUrl &base)
{
    if (!m_network)
        m_network = new QNetworkAccessManager(this);

    m_discoveryUrl = discoveryUrlFor(base);
    m_discoveryTimedOut = false;

    QNetworkRequest request(authorizedUrl(m_discoveryUrl));
    applyAuth(request);

    setState(State::Discovering,
             QStringLiteral("looking up CDP targets at %1").arg(hostPort(base)));

    QNetworkReply *reply = m_network->get(request);
    m_discoveryReply = reply;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply]() { onDiscoveryFinished(reply); });

    // Without this a host that accepts the connection and then says nothing
    // leaves the viewer on "looking up CDP targets" forever. abort() turns
    // into a finished() with OperationCanceledError, which the flag relabels.
    //
    // The generation counter, rather than the reply pointer, is what tells a
    // late timer that its request is already done: deleteLater() may well have
    // freed that reply, and a fresh one can be handed the same address.
    const int generation = ++m_discoveryGeneration;
    QTimer::singleShot(m_requestTimeoutMs, this, [this, generation]() {
        if (m_discoveryGeneration != generation || !m_discoveryReply)
            return; // already answered, or superseded
        m_discoveryTimedOut = true;
        m_discoveryReply->abort();
    });
}

void CdpClient::onDiscoveryFinished(QNetworkReply *reply)
{
    reply->deleteLater();
    if (m_discoveryReply == reply)
        m_discoveryReply = nullptr;
    if (m_closing)
        return; // disconnectFromEndpoint() aborted it on purpose

    // m_discoveryUrl, never reply->url(): the latter carries ?token=<secret>.
    const QString where = m_discoveryUrl.toString();

    if (reply->error() != QNetworkReply::NoError) {
        if (m_discoveryTimedOut) {
            failDiscovery(QStringLiteral("%1 did not answer within %2 ms")
                              .arg(where)
                              .arg(m_requestTimeoutMs));
        } else {
            failDiscovery(
                QStringLiteral("cannot reach %1: %2").arg(where, reply->errorString()));
        }
        return;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        failDiscovery(QStringLiteral("%1 returned malformed JSON: %2")
                          .arg(where, parseError.errorString()));
        return;
    }
    if (!doc.isArray()) {
        failDiscovery(QStringLiteral("%1 did not return a JSON array of targets").arg(where));
        return;
    }

    const QJsonArray targets = doc.array();
    if (targets.isEmpty()) {
        failDiscovery(QStringLiteral("%1 lists no debuggable targets; is a page open?").arg(where));
        return;
    }

    QList<PageTarget> pages;
    QStringList otherTypes;
    int attached = 0; // page targets some other debugger already holds
    for (const QJsonValue &value : targets) {
        const QJsonObject target = value.toObject();
        const QString type = target.value(QStringLiteral("type")).toString();
        if (type != QLatin1String("page")) {
            // Deduplicated so "saw: service_worker" does not become a wall of
            // the same word once a page registers a dozen workers.
            const QString label = type.isEmpty() ? QStringLiteral("(no type)") : type;
            if (!otherTypes.contains(label))
                otherTypes.append(label);
            continue;
        }
        const QString socketUrl =
            target.value(QStringLiteral("webSocketDebuggerUrl")).toString();
        if (socketUrl.isEmpty()) {
            // Chrome omits the field entirely once a client is attached.
            ++attached;
            continue;
        }
        PageTarget page;
        page.title = target.value(QStringLiteral("title")).toString();
        page.url = target.value(QStringLiteral("url")).toString();
        page.socketUrl = socketUrl;
        pages.append(page);
    }

    if (pages.isEmpty()) {
        if (attached > 0) {
            failDiscovery(QStringLiteral("%1 has %2 page target(s) but every one is already "
                                         "attached by another debugger")
                              .arg(where)
                              .arg(attached));
        } else {
            failDiscovery(QStringLiteral("%1 has no target of type \"page\" (saw: %2)")
                              .arg(where, otherTypes.join(QStringLiteral(", "))));
        }
        return;
    }

    if (pages.size() > 1) {
        // Ambiguous, so say what was picked and what the alternatives were.
        // Printed before the dial, on stderr, so it survives the alt screen.
        QTextStream err(stderr);
        err << kErrPrefix << pages.size() << " page targets at " << hostPort(m_requestedUrl)
            << "; attaching to [0]" << Qt::endl;
        for (int i = 0; i < pages.size(); ++i) {
            err << "  [" << i << "] " << shortTitle(pages.at(i).title) << " - "
                << pages.at(i).url << Qt::endl;
        }
        err << "  re-run with --cdp <webSocketDebuggerUrl> to attach to another one"
            << Qt::endl;
    }

    const QUrl socketUrl(pages.first().socketUrl);
    if (socketUrl.scheme().toLower() != QLatin1String("ws")) {
        failDiscovery(QStringLiteral("%1 offered a %2:// debugger URL; only ws:// is supported")
                          .arg(where, socketUrl.scheme()));
        return;
    }

    m_endpoint = socketUrl;
    openSocket();
}

void CdpClient::failDiscovery(const QString &message)
{
    // Terminal by definition: there is no second endpoint to fall back to, so
    // the reconnect machinery stays off and the queued work is failed rather
    // than left waiting for its 5 s deadline.
    m_closing = true;
    m_reconnectTimer->stop();
    m_queued.clear();

    QTextStream err(stderr);
    err << kErrPrefix << message << Qt::endl;

    setState(State::Disconnected, message);
    failAllPending(message, ErrorConnectionLost);
    emit discoveryFailed(message);

    // exec() returns 1 and runTerminal() unwinds normally — ::exit() here
    // would skip the termios restore if the viewer were already in raw mode.
    if (m_exitOnDiscoveryFailure)
        QCoreApplication::exit(1);
}

void CdpClient::openSocket()
{
    if (m_endpoint.isEmpty())
        return;

    QNetworkRequest request(authorizedUrl(m_endpoint));
    applyAuth(request);

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
