#include "terminal/cdp_frame_backend.h"

#include <cmath>
#include <cstring>

#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QtGlobal>

#include "config/config.h"

namespace {

// The gfx renderers hand the PNG to the terminal untouched, so it is never
// decoded on that path; the aspect-correct cell rect only needs the IHDR
// numbers. Same reasoning (and the same 24-byte header walk) as the copy in
// render_http_client.cpp — kept local rather than shared because the two
// backends are deliberately independent of each other.
bool pngDimensions(const QByteArray &png, int &w, int &h)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 24 || memcmp(png.constData(), sig, 8) != 0)
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(png.constData());
    w = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
    h = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
    return w > 0 && h > 0;
}

// clientWidth/clientHeight of one Page.getLayoutMetrics viewport object.
bool viewportSize(const QJsonObject &metrics, const char *key, int &w, int &h)
{
    const QJsonObject viewport = metrics.value(QLatin1String(key)).toObject();
    if (viewport.isEmpty())
        return false;
    w = viewport.value(QStringLiteral("clientWidth")).toInt();
    h = viewport.value(QStringLiteral("clientHeight")).toInt();
    return w > 0 && h > 0;
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

CdpFrameBackend::CdpFrameBackend(const Config &config, QObject *parent)
    : FrameBackend(parent)
    , m_client(new CdpClient(config.termToken, this))
{
    // Every state change the transport reports is status-bar material: the
    // viewer stays up through a dropped connection and the words in the bar
    // are the only thing that says so.
    connect(m_client, &CdpClient::stateChanged, this,
            [this](const QString &) { updateStatus(); });

    connect(m_client, &CdpClient::connected, this, [this]() {
        // A reconnect may land on a different target, or on the same one after
        // a resize, so nothing learned before it survives.
        m_metricsValid = false;
        m_cssViewportW = 0;
        m_cssViewportH = 0;
        m_deviceScale = 0.0;
        m_lastImageW = 0;
        m_lastImageH = 0;
        m_captureError.clear();
        requestMetrics(); // a round trip earlier than the first frame would
        updateStatus();
    });

    m_client->connectToEndpoint(QUrl(config.cdpUrl));
    updateStatus();
}

QString CdpFrameBackend::description() const
{
    return m_client->description();
}

// ── Status ──────────────────────────────────────────────────────────────────

void CdpFrameBackend::setCaptureError(const QString &text)
{
    m_captureError = text;
    updateStatus();
}

void CdpFrameBackend::updateStatus()
{
    // While the transport is down its own words are the more urgent truth
    // ("reconnecting to host:port (attempt 3)"); a capture error only means
    // anything once there is a connection for it to have failed on.
    const QString text = m_client->isConnected() ? m_captureError : m_client->stateText();
    if (text == m_status)
        return;
    m_status = text;
    emit statusChanged(text);
}

// ── Frames ──────────────────────────────────────────────────────────────────

void CdpFrameBackend::requestRgbFrame(int width, int height)
{
    if (width != m_lastTargetW || height != m_lastTargetH) {
        m_lastTargetW = width;
        m_lastTargetH = height;
        // The terminal was resized. The page viewport is independent of it, so
        // this is belt and braces — but a resize is rare and a stale click map
        // is invisible until someone clicks, which is exactly the kind of bug
        // worth one extra CDP call to avoid.
        requestMetrics();
    }
    captureFrame(true, width, height);
}

void CdpFrameBackend::requestPngFrame()
{
    captureFrame(false, 0, 0);
}

void CdpFrameBackend::captureFrame(bool wantRgb, int targetW, int targetH)
{
    // Skip the tick rather than queueing: at 30 fps an endpoint that answers
    // slower than the frame period would otherwise accumulate captures it can
    // never catch up on, and every one of them would be stale on arrival.
    if (m_captureInFlight)
        return;
    // Sending while the socket is down would only park the request in
    // CdpClient's pre-open queue until its 5 s deadline expired, blocking
    // every tick in between. The next tick is 33 ms away; the status bar
    // already carries the connection state.
    if (!m_client->isConnected())
        return;

    m_captureInFlight = true;
    QJsonObject params;
    params[QStringLiteral("format")] = QStringLiteral("png");
    m_client->send(QStringLiteral("Page.captureScreenshot"), params,
                   [this, wantRgb, targetW, targetH](const CdpResult &result) {
                       // Cleared first: onCaptureReply() renders synchronously
                       // through frameReady(), and the flag must already
                       // describe the next tick by the time it returns.
                       m_captureInFlight = false;
                       onCaptureReply(result, wantRgb, targetW, targetH);
                   });
}

void CdpFrameBackend::onCaptureReply(const CdpResult &result, bool wantRgb, int targetW,
                                     int targetH)
{
    if (!result.ok) {
        // Includes the connection being dropped mid-capture: the UI keeps its
        // last frame, the status bar says what happened, nothing tears down.
        setCaptureError(QStringLiteral("capture failed: %1").arg(result.errorMessage));
        emit frameFailed(result.errorMessage);
        return;
    }

    const QByteArray png =
        QByteArray::fromBase64(result.result.value(QStringLiteral("data")).toString().toLatin1());
    if (png.isEmpty()) {
        setCaptureError(QStringLiteral("capture failed: empty screenshot"));
        emit frameFailed(QStringLiteral("empty screenshot"));
        return;
    }

    if (wantRgb)
        emitRgbFrame(png, targetW, targetH);
    else
        emitPngFrame(png);
}

// Halfblock: the page has to arrive as packed RGB already scaled to the cell
// grid, which /render/screenshot.ppm did server-side via ?w=&h=. Same recipe as
// http_server.cpp:342-388, run here instead.
void CdpFrameBackend::emitRgbFrame(const QByteArray &png, int targetW, int targetH)
{
    QImage image;
    if (!image.loadFromData(png, "PNG") || image.isNull()) {
        setCaptureError(QStringLiteral("capture failed: undecodable PNG"));
        emit frameFailed(QStringLiteral("undecodable PNG"));
        return;
    }

    noteImageSize(image.width(), image.height());

    QImage scaled = image;
    if (targetW > 0 && targetH > 0) {
        scaled = image.scaled(targetW, targetH, Qt::KeepAspectRatio,
                              Qt::SmoothTransformation);
    }
    scaled = scaled.convertToFormat(QImage::Format_RGB888);
    if (scaled.isNull() || scaled.width() <= 0 || scaled.height() <= 0) {
        setCaptureError(QStringLiteral("capture failed: cannot scale screenshot"));
        emit frameFailed(QStringLiteral("cannot scale screenshot"));
        return;
    }

    FrameData frame;
    frame.width = scaled.width();
    frame.height = scaled.height();
    frame.bytes.reserve(static_cast<qsizetype>(frame.width) * frame.height * 3);
    // Copy width*3 bytes per row: scanlines are 4-byte aligned, so
    // bytesPerLine() may include padding the renderer must not see.
    for (int y = 0; y < scaled.height(); ++y) {
        frame.bytes.append(reinterpret_cast<const char *>(scaled.constScanLine(y)),
                           static_cast<qsizetype>(scaled.width()) * 3);
    }
    // The viewport comes from the *decoded* image, not the downscaled one:
    // it describes the page, not what fits in the terminal.
    viewportForImage(image.width(), image.height(), frame.viewportW, frame.viewportH);

    setCaptureError(QString());
    emit frameReady(frame);
}

// iTerm/kitty: the PNG goes to the terminal byte for byte. Decoding it here
// only to re-encode it would cost a full image round trip per frame and change
// nothing the terminal can see.
void CdpFrameBackend::emitPngFrame(const QByteArray &png)
{
    FrameData frame;
    if (!pngDimensions(png, frame.width, frame.height)) {
        setCaptureError(QStringLiteral("capture failed: malformed PNG"));
        emit frameFailed(QStringLiteral("malformed PNG"));
        return;
    }

    noteImageSize(frame.width, frame.height);

    frame.bytes = png;
    viewportForImage(frame.width, frame.height, frame.viewportW, frame.viewportH);

    setCaptureError(QString());
    emit frameReady(frame);
}

// ── Layout metrics and the CSS-pixel display map ────────────────────────────

void CdpFrameBackend::requestMetrics()
{
    if (m_metricsInFlight || !m_client->isConnected())
        return;

    m_metricsInFlight = true;
    m_client->send(QStringLiteral("Page.getLayoutMetrics"), QJsonObject(),
                   [this](const CdpResult &result) {
                       m_metricsInFlight = false;
                       onMetricsReply(result);
                   });
}

void CdpFrameBackend::onMetricsReply(const CdpResult &result)
{
    // A failure is not worth a status line of its own: it is either the
    // connection (already reported) or an endpoint without the command, and
    // viewportForImage()'s 1:1 fallback keeps clicks working on the endpoints
    // that is true of. The next resize, page-size change or reconnect asks
    // again.
    if (!result.ok)
        return;

    int cssW = 0, cssH = 0;
    if (!viewportSize(result.result, "cssLayoutViewport", cssW, cssH)) {
        // Chrome only grew the css* fields in 2020; before that layoutViewport
        // itself was CSS pixels.
        if (!viewportSize(result.result, "layoutViewport", cssW, cssH))
            return;
    }

    // deviceScaleFactor is not a field of getLayoutMetrics — it is the ratio
    // between the two viewports it reports, since the deprecated ones are in
    // device pixels once the css* ones exist. Where only one form is present
    // the ratio is 1 by construction and says nothing; viewportForImage()
    // compares the screenshot against the CSS viewport instead, and only
    // falls back to this factor to bridge a resize.
    int deviceW = 0, deviceH = 0;
    if (viewportSize(result.result, "layoutViewport", deviceW, deviceH) && cssW > 0)
        m_deviceScale = static_cast<double>(deviceW) / cssW;
    else
        m_deviceScale = 0.0;

    m_cssViewportW = cssW;
    m_cssViewportH = cssH;
    m_metricsValid = true;
}

void CdpFrameBackend::noteImageSize(int imageW, int imageH)
{
    if (imageW == m_lastImageW && imageH == m_lastImageH)
        return;
    m_lastImageW = imageW;
    m_lastImageH = imageH;
    // The screenshot is the CSS viewport times deviceScaleFactor, so any move
    // in either one shows up here. This is also what fetches the very first
    // metrics if the request made on connected() was lost.
    requestMetrics();
}

void CdpFrameBackend::viewportForImage(int imageW, int imageH, int &viewportW,
                                       int &viewportH) const
{
    // Nothing to scale by yet. Assuming 1:1 is right for most endpoints and
    // keeps clicks usable on them instead of dead everywhere; the metrics are
    // already on their way (see noteImageSize()).
    if (!m_metricsValid || m_cssViewportW <= 0 || m_cssViewportH <= 0 || imageW <= 0
        || imageH <= 0) {
        viewportW = imageW;
        viewportH = imageH;
        return;
    }

    // The screenshot is the CSS viewport times deviceScaleFactor, so a page
    // that has not changed size since the last reply shows the same ratio on
    // both axes — whatever that ratio is. Taking the reported numbers on that
    // test rather than on m_deviceScale also covers the endpoints that report
    // both viewports in CSS pixels (Chrome only grew the css* fields in 2020):
    // there the ratio is the deviceScaleFactor and m_deviceScale is 1.
    const double ratioW = static_cast<double>(imageW) / m_cssViewportW;
    const double ratioH = static_cast<double>(imageH) / m_cssViewportH;
    if (std::fabs(ratioW - ratioH) < 0.02) {
        viewportW = m_cssViewportW;
        viewportH = m_cssViewportH;
        return;
    }

    // The axes disagree, so the page was resized after the last reply and the
    // cached width belongs to the previous size. Bridge the round trip with
    // the scale factor instead, which changes far less often than the size.
    const double scale = m_deviceScale > 0.0 ? m_deviceScale : 1.0;
    viewportW = static_cast<int>(std::llround(imageW / scale));
    viewportH = static_cast<int>(std::llround(imageH / scale));
}

// ── Input ───────────────────────────────────────────────────────────────────

// Not implemented yet: Input.dispatchMouseEvent, Input.insertText and the
// named-key table are the next task. They are no-ops rather than absent
// because FrameBackend declares them pure virtual — until they land, --cdp
// shows the page and forwards nothing.

void CdpFrameBackend::sendClick(int pageX, int pageY, MouseButton button)
{
    Q_UNUSED(pageX)
    Q_UNUSED(pageY)
    Q_UNUSED(button)
}

void CdpFrameBackend::sendScroll(int pageX, int pageY, int dy)
{
    Q_UNUSED(pageX)
    Q_UNUSED(pageY)
    Q_UNUSED(dy)
}

void CdpFrameBackend::sendText(const QByteArray &utf8)
{
    Q_UNUSED(utf8)
}

void CdpFrameBackend::sendKey(const QString &namedKey)
{
    Q_UNUSED(namedKey)
}
