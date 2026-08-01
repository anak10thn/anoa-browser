#include "terminal/terminal_app.h"

#include <errno.h>
#include <unistd.h>

#include <cstddef>
#include <memory>
#include <string>

#include <QCoreApplication>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include "config/config.h"
#include "cdp/cdp_client.h"
#include "terminal/cdp_frame_backend.h"
#include "terminal/frame_backend.h"
#include "terminal/inprocess_frame_backend.h"
#include "terminal/render_http_client.h"
#include "terminal/terminal_ui.h"

// The frame loop. anoa-term drove itself from a blocking select() on stdin
// with the frame period as the timeout; the same shape here would leave no
// room for QWebSocket, which only makes progress inside a Qt event loop. So
// the two things select() interleaved become two event sources:
//
//   stdin  -> QSocketNotifier(STDIN_FILENO, Read)
//   frames -> QTimer at 1000/fps ms
//
// and QCoreApplication::exec() does the waiting. The signal handlers keep the
// volatile sig_atomic_t flags they always had and the frame tick polls them,
// which costs at most one frame period of latency (33 ms at the default 30
// fps) and saves a self-pipe.

namespace {

// A stdin burst is drained in full and dispatched once, so a paste arrives as
// one sendText() rather than one request per 512 bytes. read() can do this
// without blocking because raw mode sets VMIN=0/VTIME=0: on a tty that makes a
// drained buffer return 0 immediately instead of waiting for a byte.
//
// The cap keeps a firehose on stdin from starving the frame timer; anything
// left over stays in the tty buffer and re-arms the notifier at once.
const size_t kMaxBurstBytes = 64 * 1024;

const char kErrPrefix[] = "anoa terminal: ";

// How many failed dials to accept from an endpoint that has never once
// answered. CdpClient's backoff is 250 ms doubling to an 8 s cap, so six
// attempts is about sixteen seconds of trying — long enough to ride out a
// headless Chrome that is still starting, short enough that a typo'd port does
// not leave the user staring at a blank alt screen with no way to tell the two
// apart. There is deliberately no budget once a connection has succeeded: a
// session that worked and then blipped should heal, not exit.
const int kMaxConnectAttempts = 6;

// The wording scheduleReconnect() puts between the failure reason and the
// backoff numbers. Splitting on it recovers the reason on its own, which is
// the only part worth repeating after the terminal has been restored. If that
// format ever changes the whole state line is used instead: verbose, never
// wrong.
const char kRetryMarker[] = " - retrying in ";

// False means stdin is gone for good (the far end of a pty hung up, which
// reports EIO on Linux rather than end-of-file) and the viewer should stop.
// A short read is normal and says nothing about the descriptor.
bool readStdinBurst(std::string &burst)
{
    char buf[512];
    while (burst.size() < kMaxBurstBytes) {
        const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            burst.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0)
            break; // buffer drained (VMIN=0), not end-of-file
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            break; // a signal landed mid-read; the bytes are still queued
        return false;
    }
    return true;
}

// anoa-term probed the endpoint before touching the terminal so that "is it
// even running?" printed on a normal screen instead of flashing the alt
// screen and then sitting on "connection lost" forever.
//
// The seam is asynchronous, so this can only conclude something when the
// backend answers before the call returns. The HTTP client does (it blocks on
// the socket); a WebSocket backend will not, and for it the probe is a no-op
// by construction -- connection state reaches the user through statusChanged()
// in the status bar instead. Silence is therefore treated as success.
bool probeBackend(FrameBackend *backend, QTextStream &err)
{
    QString failure;
    bool failed = false;
    const QMetaObject::Connection conn =
        QObject::connect(backend, &FrameBackend::frameFailed, backend,
                         [&](const QString &reason) {
                             failed = true;
                             failure = reason;
                         });

    // The same 8x8 request anoa-term used: cheap for the server, and it
    // exercises the endpoint and the token rather than the renderer.
    backend->requestRgbFrame(8, 8);
    QObject::disconnect(conn);

    if (!failed)
        return true;

    err << kErrPrefix << "cannot fetch " << backend->description()
        << "/render/screenshot.ppm (" << failure << ")" << Qt::endl
        << "Is anoa running? Does it need --term-token?" << Qt::endl;
    return false;
}

// What the status bar names after "attached to". The precise answer is the
// ws:// URL, but its host:port half is already in the status bar header and
// its page id is the part that actually distinguishes one tab of an endpoint
// from another — so a /devtools/page/<id> URL is reduced to that id, and
// anything else is shown as the user typed it.
QString attachedTarget(const CdpClient *client)
{
    const QUrl endpoint = client->endpoint();
    if (endpoint.isEmpty())
        return client->requestedUrl().toString(); // discovery has not resolved one

    const QString path = endpoint.path();
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    const QString id = slash >= 0 ? path.mid(slash + 1) : path;
    if (id.isEmpty())
        return endpoint.toString();

    // Chrome's target ids are 32 hex characters; the first eight are as unique
    // as anyone reading a one-row status bar needs, and every character saved
    // is one the status bar does not have to drop this field to afford. "..."
    // rather than the ellipsis used elsewhere in the bar keeps this file ASCII.
    const QString shortId = id.size() > 8 ? id.left(8) + QStringLiteral("...") : id;
    return QStringLiteral("page ") + shortId;
}

// --term-host and --term-port describe the /render/* endpoint of a local
// anoa. On the CDP path the target comes from --cdp alone, so they are
// not applied — and saying so beats letting someone believe the viewer is
// pointed where they asked. Config carries no "was this set?" flag, so the
// test is against the defaults declared in config.h.
//
// --term-token is deliberately absent from that list, because it is *not*
// ignored here: CdpClient sends it as the endpoint's bearer token, on both the
// /json/list GET and the WebSocket dial, which is what anoa's own CDP
// proxy requires. This line says that out loud for the same reason.
void warnIgnoredRenderOptions(const Config &config, QTextStream &err)
{
    QStringList ignored;
    if (config.termHost != QLatin1String("127.0.0.1"))
        ignored << QStringLiteral("--term-host");
    if (config.termPort != 9222)
        ignored << QStringLiteral("--term-port");

    if (!ignored.isEmpty()) {
        err << kErrPrefix << "--cdp is set, ignoring " << ignored.join(QStringLiteral(", "))
            << " (they address the /render/* backend)" << Qt::endl;
    }
    if (!config.termToken.isEmpty()) {
        err << kErrPrefix
            << "--term-token is still used, as the bearer token for the CDP endpoint"
            << Qt::endl;
    }
}

} // namespace

int runTerminal(const Config &config, AnoaBrowser *embedded)
{
    QTextStream err(stderr);

    // The one mode decision in the whole viewer. Everything below this block
    // holds a FrameBackend * and cannot tell the three apart; the CDP-only
    // wiring further down reaches through `cdp`, never through a branch in
    // TerminalUi.
    const bool useCdp = !config.cdpUrl.isEmpty();
    // main.cpp decided this from raw argv and built the browser to match, so a
    // set flag without a browser is a wiring bug here, not a user error.
    const bool useEmbedded = config.termEmbedded && embedded != nullptr;

    // Both directions are used: stdin for raw-mode key and mouse reports,
    // stdout for the escape sequences that draw the page.
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        err << kErrPrefix << "stdin/stdout must be a terminal" << Qt::endl;
        return 1;
    }

    std::unique_ptr<FrameBackend> owned;
    CdpFrameBackend *cdp = nullptr;
    if (useEmbedded) {
        // No probe and no retry budget: the browser is a pointer away, so the
        // failures those guard against — wrong port, nothing listening, an
        // endpoint that never answers — cannot happen here.
        owned.reset(new InProcessFrameBackend(embedded));
    } else if (useCdp) {
        warnIgnoredRenderOptions(config, err);
        cdp = new CdpFrameBackend(config); // dials config.cdpUrl in its ctor
        owned.reset(cdp);
        // connectToEndpoint() can fail before it returns, on a scheme
        // config.cpp is supposed to have rejected already. It has printed its
        // own line by then; the only thing left to get right is not entering
        // raw mode on the way out. Every path that did start reports
        // Discovering, Connecting or Reconnecting.
        if (cdp->client()->state() == CdpClient::State::Disconnected)
            return 1;
    } else {
        owned.reset(new RenderHttpClient(config));
        // anoa-term's pre-flight probe, and HTTP-only by nature: it concludes
        // something because that backend answers before the call returns, and
        // its message names a /render/* path. The CDP path reports the same
        // class of failure through the status bar and the retry budget below.
        if (!probeBackend(owned.get(), err))
            return 1;
    }
    FrameBackend *backend = owned.get();

    TerminalUi ui(config, backend);
    // The label names the transport and description() names the target. For the
    // embedded backend those are one thing, so the label is left empty (which
    // hides the field) rather than printing "embedded embedded".
    ui.setBackendLabel(useEmbedded ? QString()
                       : useCdp    ? QStringLiteral("cdp")
                                   : QStringLiteral("http"));
    QObject::connect(backend, &FrameBackend::frameReady, &ui, &TerminalUi::onFrame);
    QObject::connect(backend, &FrameBackend::frameFailed, &ui, &TerminalUi::onFrameFailed);
    QObject::connect(backend, &FrameBackend::statusChanged, &ui, &TerminalUi::onStatus);

    // Past this point the terminal is on the alt screen in raw mode, so every
    // exit path has to hand it back. ui.end() does that and is idempotent; it
    // is reached from shutdown(), from the explicit call after exec() and from
    // ~TerminalUi. enterRawMode() also registered restoreTerminal() with
    // atexit(), which covers anything that leaves via ::exit().
    if (!ui.begin()) {
        err << kErrPrefix << "failed to set raw terminal mode" << Qt::endl;
        return 1;
    }

    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QTimer frameTimer;

    // Set when the viewer is being torn down because the connection can never
    // work, rather than because the user asked to leave. Printed after ui.end()
    // has handed the terminal back — anything written before that lands on the
    // alt screen and is thrown away with it.
    QString fatal;

    // CDP retry-budget state. Function scope, not the block below, because the
    // signal connections that read it outlive that block — they live as long
    // as `ui` does.
    bool everConnected = false;
    QString lastReason;

    auto shutdown = [&]() {
        // Silence both event sources first: the notifier is level triggered
        // and would keep firing on whatever is still queued while the event
        // loop unwinds.
        stdinNotifier.setEnabled(false);
        frameTimer.stop();
        ui.end();
        QCoreApplication::quit();
    };

    if (cdp) {
        CdpClient *client = cdp->client();

        // CdpFrameBackend::statusChanged() already carries the transport's own
        // words while the socket is down, and deliberately goes quiet once it
        // is up so a capture or input error can have the slot. What nobody
        // says in the connected case is *what we are attached to*, so that
        // half is read straight off the client. It arrives at TerminalUi as
        // plain text, like every other status line.
        auto refreshLink = [&ui, client]() {
            ui.onLink(client->isConnected()
                          ? QStringLiteral("attached to %1").arg(attachedTarget(client))
                          : QString());
        };

        QObject::connect(client, &CdpClient::connected, &ui,
                         [&everConnected]() { everConnected = true; });

        QObject::connect(client, &CdpClient::stateChanged, &ui,
                         [&lastReason, refreshLink](const QString &text) {
                             const int cut = text.indexOf(QLatin1String(kRetryMarker));
                             if (cut > 0)
                                 lastReason = text.left(cut);
                             refreshLink();
                         });

        // The retry budget. CdpClient retries without limit on purpose, which
        // is what a working session wants; an endpoint that has never answered
        // is a different situation, and without this the viewer would sit on a
        // blank alt screen forever with "reconnecting" as its only clue.
        QObject::connect(client, &CdpClient::retryScheduled, &ui, [&](int attempt, int) {
            if (everConnected || attempt <= kMaxConnectAttempts)
                return;
            fatal = QStringLiteral("cannot connect to %1 after %2 attempts (%3)")
                        .arg(config.cdpUrl)
                        .arg(kMaxConnectAttempts)
                        .arg(lastReason.isEmpty() ? QStringLiteral("no answer from the endpoint")
                                                  : lastReason);
            shutdown();
        });

        // /json/list produced no ws:// URL: terminal by definition, there is no
        // second endpoint to fall back to. The client would exit(1) by itself,
        // but this file owns the terminal, so it owns the exit too — otherwise
        // the message is written to the alt screen and discarded with it.
        client->setExitOnDiscoveryFailure(false);
        QObject::connect(client, &CdpClient::discoveryFailed, &ui, [&](const QString &message) {
            fatal = message;
            shutdown();
        });

        // The backend dialled in its constructor, before any of this was
        // connected, so its first statusChanged() is already gone. Prime both
        // slots from the client so the very first frame says "connecting to
        // host:port" rather than nothing.
        ui.onStatus(client->isConnected() ? QString() : client->stateText());
        refreshLink();
    }

    QObject::connect(&stdinNotifier, &QSocketNotifier::activated, &ui, [&]() {
        std::string burst;
        const bool alive = readStdinBurst(burst);
        if (!burst.empty() && !ui.feedInput(burst.data(), burst.size())) {
            shutdown(); // Ctrl-C / Ctrl-Q
            return;
        }
        if (!alive)
            shutdown();
    });

    frameTimer.setInterval(ui.framePeriodMs());
    QObject::connect(&frameTimer, &QTimer::timeout, &ui, [&]() {
        if (!ui.tick())
            shutdown(); // SIGINT or SIGTERM, seen by the poll inside tick()
    });
    frameTimer.start();

    // Draw once up front rather than making the user look at an empty alt
    // screen for the first frame period.
    if (!ui.tick()) {
        ui.end();
        return 0;
    }

    const int rc = QCoreApplication::exec();
    ui.end();

    // Now that the alt screen is gone, one line the user can actually read.
    // shutdown() left exec() with 0, so the status has to be forced non-zero.
    if (!fatal.isEmpty()) {
        err << kErrPrefix << fatal << Qt::endl;
        return rc != 0 ? rc : 1;
    }
    return rc;
}
