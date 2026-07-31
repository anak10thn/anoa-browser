#include "terminal/terminal_app.h"

#include <errno.h>
#include <unistd.h>

#include <cstddef>
#include <string>

#include <QCoreApplication>
#include <QObject>
#include <QSocketNotifier>
#include <QString>
#include <QTextStream>
#include <QTimer>

#include "config/config.h"
#include "terminal/frame_backend.h"
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

    err << "anoa-browser terminal: cannot fetch " << backend->description()
        << "/render/screenshot.ppm (" << failure << ")" << Qt::endl
        << "Is anoa-browser running? Does it need --term-token?" << Qt::endl;
    return false;
}

} // namespace

int runTerminal(const Config &config)
{
    QTextStream err(stderr);

    if (!config.cdpUrl.isEmpty()) {
        // The CDP backend is a later phase; falling through to the default
        // /render/* endpoint would silently ignore the flag.
        err << "Error: --cdp is not implemented yet in this build" << Qt::endl;
        return 1;
    }

    // Both directions are used: stdin for raw-mode key and mouse reports,
    // stdout for the escape sequences that draw the page.
    if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
        err << "anoa-browser terminal: stdin/stdout must be a terminal" << Qt::endl;
        return 1;
    }

    RenderHttpClient backend(config);
    if (!probeBackend(&backend, err))
        return 1;

    TerminalUi ui(config, &backend);
    QObject::connect(&backend, &FrameBackend::frameReady, &ui, &TerminalUi::onFrame);
    QObject::connect(&backend, &FrameBackend::frameFailed, &ui, &TerminalUi::onFrameFailed);
    QObject::connect(&backend, &FrameBackend::statusChanged, &ui, &TerminalUi::onStatus);

    // Past this point the terminal is on the alt screen in raw mode, so every
    // exit path has to hand it back. ui.end() does that and is idempotent; it
    // is reached from shutdown(), from the explicit call after exec() and from
    // ~TerminalUi. enterRawMode() also registered restoreTerminal() with
    // atexit(), which covers anything that leaves via ::exit().
    if (!ui.begin()) {
        err << "anoa-browser terminal: failed to set raw terminal mode" << Qt::endl;
        return 1;
    }

    QSocketNotifier stdinNotifier(STDIN_FILENO, QSocketNotifier::Read);
    QTimer frameTimer;

    auto shutdown = [&]() {
        // Silence both event sources first: the notifier is level triggered
        // and would keep firing on whatever is still queued while the event
        // loop unwinds.
        stdinNotifier.setEnabled(false);
        frameTimer.stop();
        ui.end();
        QCoreApplication::quit();
    };

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
    return rc;
}
