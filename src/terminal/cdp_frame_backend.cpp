#include "terminal/cdp_frame_backend.h"

#include <cmath>
#include <cstring>

#include <QImage>
#include <QJsonObject>
#include <QJsonValue>
#include <QUrl>
#include <QtGlobal>

#include "config/config.h"
#include "terminal/frame_bytes.h"

namespace {

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

// CDP packs modifiers into one integer on every Input.* event, and the bit
// order is its own — not Qt's, not the terminal's. Nothing sets them today:
// terminal_ui.cpp drops an SGR mouse report whose button field carries the
// shift/meta/ctrl bits (it only accepts 0-2, 64 and 65) and a modified key
// reaches feedInput() as a bare control byte, so the seam has no modifier to
// pass on. The names exist so the day it grows one there is a single place to
// read the convention off.
enum CdpModifier : int {
    ModAlt = 1,
    ModCtrl = 2,
    ModMeta = 4,
    ModShift = 8,
};
constexpr int kNoModifiers = 0;

// CDP's "button" string. Same three the /render/click query string used, which
// is not a coincidence — http_server.cpp took its spelling from the DOM too.
const char *cdpButtonName(MouseButton button)
{
    switch (button) {
    case MouseButton::Left:
        break;
    case MouseButton::Middle:
        return "middle";
    case MouseButton::Right:
        return "right";
    }
    // Left, and the only path here for an out-of-range value. No default label:
    // -Wswitch then turns a fourth button into a compile error instead of a
    // silent "left", exactly as buttonName() in render_http_client.cpp does.
    return "left";
}

// CDP's "buttons" field is the DOM MouseEvent.buttons bitmask — which is *not*
// the same numbering as "button" (there right is 2 and middle is 1).
int cdpButtonMask(MouseButton button)
{
    switch (button) {
    case MouseButton::Left:
        break;
    case MouseButton::Middle:
        return 4;
    case MouseButton::Right:
        return 2;
    }
    return 1;
}

// The 14 keys anoa_browser.cpp:220-258 accepts on /render/key, restated in
// CDP's terms. Qt::Key values do not survive the trip: Chromium wants the DOM
// triple (key, code, windowsVirtualKeyCode) and, for the keys that produce a
// character, the character itself.
//
// A partial entry is the dangerous failure here — Chromium does not reject a
// dispatchKeyEvent with a missing windowsVirtualKeyCode or an unknown code, it
// accepts it and the page sees nothing. So every row is filled in, and `text`
// is empty only for the keys that genuinely insert nothing.
struct NamedKey {
    const char *name; // what TerminalUi calls it, lowercase
    const char *key;  // DOM KeyboardEvent.key
    const char *code; // DOM KeyboardEvent.code, i.e. the physical key
    int windowsVirtualKeyCode;
    const char *text; // "" where the key produces no character
};

const NamedKey kNamedKeys[] = {
    // "backspace" is also where the DEL byte (127) the terminal delivers ends
    // up: terminal_ui.cpp maps both 127 and 8 onto this name.
    {"enter", "Enter", "Enter", 13, "\r"},
    {"tab", "Tab", "Tab", 9, "\t"},
    {"backspace", "Backspace", "Backspace", 8, ""},
    {"delete", "Delete", "Delete", 46, ""},
    {"escape", "Escape", "Escape", 27, ""},
    {"space", " ", "Space", 32, " "},
    {"up", "ArrowUp", "ArrowUp", 38, ""},
    {"down", "ArrowDown", "ArrowDown", 40, ""},
    {"left", "ArrowLeft", "ArrowLeft", 37, ""},
    {"right", "ArrowRight", "ArrowRight", 39, ""},
    {"home", "Home", "Home", 36, ""},
    {"end", "End", "End", 35, ""},
    {"pageup", "PageUp", "PageUp", 33, ""},
    {"pagedown", "PageDown", "PageDown", 34, ""},
};

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
        m_inputError.clear();
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

void CdpFrameBackend::setInputError(const QString &text)
{
    m_inputError = text;
    updateStatus();
}

void CdpFrameBackend::updateStatus()
{
    // While the transport is down its own words are the more urgent truth
    // ("reconnecting to host:port (attempt 3)"); a capture error only means
    // anything once there is a connection for it to have failed on.
    //
    // An input error outranks a capture error because it is the rarer and more
    // actionable one, and because it would otherwise be invisible: captures run
    // 30 times a second and each success clears m_captureError, so the two
    // sharing one slot would blink a rejected keystroke away inside 33 ms.
    QString text;
    if (!m_client->isConnected())
        text = m_client->stateText();
    else if (!m_inputError.isEmpty())
        text = m_inputError;
    else
        text = m_captureError;
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
    if (!frame_bytes::pngDimensions(png.constData(), static_cast<size_t>(png.size()), frame.width,
                                    frame.height)) {
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

void CdpFrameBackend::dispatchInput(const QString &method, const QJsonObject &params)
{
    // Dropped rather than queued while the socket is down, for the same reason
    // captureFrame() drops a tick: CdpClient would park the frame in its
    // pre-open queue and fail it on the 5 s deadline, so the user would get an
    // error about a keystroke made five seconds ago on a bar that is already
    // saying "reconnecting". Silence is the honest answer — the transport words
    // in the status bar already explain why nothing is happening.
    if (!m_client->isConnected())
        return;

    m_client->send(method, params, [this, method](const CdpResult &result) {
        // Fire-and-forget as far as the frame loop is concerned: nothing waited
        // for this, and all it can do is change what the status bar says. The
        // next accepted event clears it again.
        if (result.ok)
            setInputError(QString());
        else
            setInputError(QStringLiteral("%1 failed: %2").arg(method, result.errorMessage));
    });
}

void CdpFrameBackend::sendClick(int pageX, int pageY, MouseButton button)
{
    // CDP has no "click" — press and release are two events, and Chromium
    // synthesises the click from the pair. They must agree on the position, so
    // both are built from one object.
    QJsonObject params;
    params[QStringLiteral("x")] = pageX;
    params[QStringLiteral("y")] = pageY;
    params[QStringLiteral("button")] = QString::fromLatin1(cdpButtonName(button));
    params[QStringLiteral("clickCount")] = 1;
    params[QStringLiteral("modifiers")] = kNoModifiers;

    params[QStringLiteral("type")] = QStringLiteral("mousePressed");
    params[QStringLiteral("buttons")] = cdpButtonMask(button);
    dispatchInput(QStringLiteral("Input.dispatchMouseEvent"), params);

    // "buttons" is the set of buttons held *after* the event, so it is empty on
    // the release. Sending the mask again leaves Chromium believing the button
    // is still down, which turns the next move into a drag.
    params[QStringLiteral("type")] = QStringLiteral("mouseReleased");
    params[QStringLiteral("buttons")] = 0;
    dispatchInput(QStringLiteral("Input.dispatchMouseEvent"), params);
}

void CdpFrameBackend::sendScroll(int pageX, int pageY, int dy)
{
    QJsonObject params;
    params[QStringLiteral("type")] = QStringLiteral("mouseWheel");

    // Negative coordinates mean the pointer was off the page image. /render/
    // scroll expressed that by omitting x and y and http_server.cpp:433 filled
    // in the viewport centre; CDP has no way to omit a position, so the same
    // substitution happens here. Falling back to 0,0 when the metrics have not
    // arrived yet is not equivalent — a wheel at the top-left corner lands in
    // whatever pane happens to be there — but it is the best guess available
    // and only applies for the first frame or two.
    const bool onPage = pageX >= 0 && pageY >= 0;
    params[QStringLiteral("x")] = onPage ? pageX : m_cssViewportW / 2;
    params[QStringLiteral("y")] = onPage ? pageY : m_cssViewportH / 2;

    // THE SIGN. `dy` is a Qt angleDelta, as /render/scroll takes it: +120 means
    // one notch *up*, because Qt reports which way the wheel turned. CDP's
    // deltaY is the DOM WheelEvent one, which reports how far the content
    // should move — so +120 there scrolls *down*. Negating here is the whole
    // difference, and getting it wrong produces no error anywhere: the page
    // just scrolls the wrong way. task-016 pins it with an integration test.
    params[QStringLiteral("deltaX")] = 0;
    params[QStringLiteral("deltaY")] = -dy;
    params[QStringLiteral("modifiers")] = kNoModifiers;
    dispatchInput(QStringLiteral("Input.dispatchMouseEvent"), params);
}

void CdpFrameBackend::sendText(const QByteArray &utf8)
{
    if (utf8.isEmpty())
        return;
    // Input.insertText, not a key event per character: the burst is already
    // batched by TerminalUi and this is the one CDP command that takes text
    // without needing a key code for every character in it. It is also what
    // makes pasted CJK and emoji work, which a virtual-key table cannot.
    QJsonObject params;
    params[QStringLiteral("text")] = QString::fromUtf8(utf8);
    dispatchInput(QStringLiteral("Input.insertText"), params);
}

void CdpFrameBackend::sendKey(const QString &namedKey)
{
    const QString wanted = namedKey.toLower();
    const NamedKey *entry = nullptr;
    for (const NamedKey &candidate : kNamedKeys) {
        if (wanted == QLatin1String(candidate.name)) {
            entry = &candidate;
            break;
        }
    }
    if (!entry) {
        // /render/key answered an unknown name with 400 and anoa_browser.cpp's
        // sendKey() returned false; the equivalent here is one line in the
        // status bar. Nothing is dispatched — a made-up key code is worse than
        // no key at all.
        setInputError(QStringLiteral("unknown key: %1").arg(namedKey));
        return;
    }

    const QString text = QString::fromLatin1(entry->text);

    QJsonObject base;
    base[QStringLiteral("key")] = QString::fromLatin1(entry->key);
    base[QStringLiteral("code")] = QString::fromLatin1(entry->code);
    base[QStringLiteral("windowsVirtualKeyCode")] = entry->windowsVirtualKeyCode;
    base[QStringLiteral("modifiers")] = kNoModifiers;

    // "keyDown" carries a character and is what makes Chromium insert one;
    // "rawKeyDown" is the same event without one, and is what the keys that
    // insert nothing must use. Sending keyDown with empty text instead is the
    // quiet way to make Backspace and the arrows do nothing at all.
    QJsonObject down = base;
    if (text.isEmpty()) {
        down[QStringLiteral("type")] = QStringLiteral("rawKeyDown");
    } else {
        down[QStringLiteral("type")] = QStringLiteral("keyDown");
        down[QStringLiteral("text")] = text;
        // With no modifiers held the unmodified character is the character.
        down[QStringLiteral("unmodifiedText")] = text;
    }
    dispatchInput(QStringLiteral("Input.dispatchKeyEvent"), down);

    // The release carries no text — the character was already delivered by the
    // keyDown, and CDP has no "rawKeyUp" to distinguish the two cases with.
    QJsonObject up = base;
    up[QStringLiteral("type")] = QStringLiteral("keyUp");
    dispatchInput(QStringLiteral("Input.dispatchKeyEvent"), up);
}
