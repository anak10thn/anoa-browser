#pragma once

// A real CDP client: it parses replies and hands each one back to the call
// site that produced it. That is the whole difference from src/cdp/cdp_proxy.*,
// which is a blind relay — it forwards frames in both directions and never
// looks at an "id". The QWebSocket handling and the send-before-open queue are
// modelled on that file; the correlation, the error/timeout resolution and the
// reconnect backoff are new here.
//
// Everything is asynchronous. The terminal frame loop is already a Qt event
// loop (see terminal_app.cpp), so a reply is delivered through a callback on a
// later turn of that loop and never through a nested QEventLoop.
//
// v1 attaches to exactly one target: no "sessionId" is ever sent or read, and
// no Target.* plumbing lives here.

#include <functional>

#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QTimer;
class QWebSocket;

// The outcome of one CDP command. `ok` picks which half is meaningful:
// `result` on success, `errorMessage`/`errorCode` on a protocol error, a
// dropped connection or a request timeout.
struct CdpResult {
    bool ok = false;
    QJsonObject result;
    QString errorMessage;
    int errorCode = 0;
};

class CdpClient : public QObject
{
    Q_OBJECT

public:
    // Called with the reply for exactly one send(). Guaranteed to run at most
    // once, and — barring an explicit disconnectFromEndpoint() — to run at all:
    // a lost reply is resolved by the request timeout rather than leaking.
    using Handler = std::function<void(const CdpResult &)>;

    // Failure codes that do not come from the wire. CDP's own codes are
    // negative but small (-32000 and friends), so these stay well clear.
    enum : int {
        ErrorTimeout = -1000,
        ErrorConnectionLost = -1001,
    };

    enum class State { Disconnected, Connecting, Connected, Reconnecting };

    // `token` is Config::termToken; empty means no authentication.
    explicit CdpClient(const QString &token = QString(), QObject *parent = nullptr);
    ~CdpClient() override;

    // Opens the socket. Safe to call before or after send() — anything sent
    // while the handshake is outstanding is queued and flushed on connected().
    void connectToEndpoint(const QUrl &url);
    // Deliberate shutdown: stops the reconnect backoff and fails every
    // outstanding request. The client does not reconnect afterwards.
    void disconnectFromEndpoint();

    // Allocates a monotonically increasing id, records the pending entry and
    // either writes the frame or queues it. Returns the id, which is useful
    // for logging; correlation itself needs nothing from the caller.
    int send(const QString &method, const QJsonObject &params = QJsonObject(),
             Handler onReply = Handler());

    // The one navigation primitive this iteration ships. No UI is built on it
    // yet — the in-terminal URL line is out of scope.
    int navigate(const QString &url, Handler onReply = Handler());

    State state() const { return m_state; }
    bool isConnected() const { return m_state == State::Connected; }
    // Human-readable form of state(), already worded for the status bar.
    QString stateText() const { return m_stateText; }
    QUrl endpoint() const { return m_endpoint; }
    // "host:port", for FrameBackend::description() on the CDP path.
    QString description() const;
    int pendingCount() const { return static_cast<int>(m_pending.size()); }

    // Per-request deadline. Applies from send() — including to a frame still
    // sitting in the pre-open queue, so an endpoint that never completes its
    // handshake fails its requests instead of accumulating them.
    void setRequestTimeout(int ms);
    int requestTimeout() const { return m_requestTimeoutMs; }

signals:
    void connected();
    void disconnected(const QString &reason);
    // Every state change, worded for the status bar. Same text as stateText().
    void stateChanged(const QString &text);
    // A reconnect has been scheduled, not yet attempted.
    void retryScheduled(int attempt, int delayMs);
    // Unsolicited CDP events — a frame with no "id". These are never treated
    // as orphan responses.
    void eventReceived(const QString &method, const QJsonObject &params);
    // A frame that is not a JSON object, or a reply for an id nobody is
    // waiting for. Informational: the connection is left alone.
    void protocolError(const QString &message);

private slots:
    void onSocketConnected();
    void onSocketDisconnected();
    void onSocketError();
    void onTextMessageReceived(const QString &message);
    void onReconnectTimeout();
    void onTimeoutSweep();

private:
    struct Pending {
        int id = 0;
        QString method;
        Handler handler;
        qint64 deadline = 0; // m_clock.elapsed() value past which it has failed
    };

    // Bearer auth on both channels, matching what http_server.cpp and
    // cdp_proxy.cpp accept on the receiving side: the header for servers that
    // read headers, ?token= for servers that read the query.
    QUrl authorizedUrl(const QUrl &url) const;
    void openSocket();
    void setState(State state, const QString &text);
    void scheduleReconnect(const QString &reason);
    void armTimeoutSweep();
    void resolve(Pending &pending, const CdpResult &result);
    void failAllPending(const QString &reason, int code);

    QWebSocket *m_socket = nullptr;
    QTimer *m_reconnectTimer = nullptr;
    QTimer *m_timeoutTimer = nullptr;
    QElapsedTimer m_clock; // monotonic; only differences are ever used

    QString m_token;
    QUrl m_endpoint; // as handed to connectToEndpoint(), without ?token=

    // Frames written before the handshake completed, flushed in order by
    // onSocketConnected(). Same role as CdpProxy::m_pendingMessages.
    QStringList m_queued;
    QHash<int, Pending> m_pending;
    int m_nextId = 0;

    State m_state = State::Disconnected;
    QString m_stateText;
    int m_attempt = 0;      // consecutive failed connections, 0 once connected
    bool m_closing = false; // disconnectFromEndpoint() was called
    int m_requestTimeoutMs = 5000;
};
