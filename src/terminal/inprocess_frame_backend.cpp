#include "terminal/inprocess_frame_backend.h"

#include <QBuffer>
#include <QPixmap>
#include <QPoint>
#include <QUrl>

#include "browser/anoa_browser.h"

namespace {

Qt::MouseButton qtButton(MouseButton button)
{
    switch (button) {
    case MouseButton::Middle:
        return Qt::MiddleButton;
    case MouseButton::Right:
        return Qt::RightButton;
    case MouseButton::Left:
        break;
    }
    return Qt::LeftButton;
}

} // namespace

InProcessFrameBackend::InProcessFrameBackend(AnoaBrowser *browser, QObject *parent)
    : FrameBackend(parent)
    , m_browser(browser)
{
}

// ── Frames ──────────────────────────────────────────────────────────────────

QImage InProcessFrameBackend::capture(int &viewportW, int &viewportH) const
{
    viewportW = 0;
    viewportH = 0;
    if (!m_browser)
        return QImage();

    const QPixmap pixmap = m_browser->grab();
    if (pixmap.isNull())
        return QImage();

    // The logical widget size, not the pixmap's — on a HiDPI screen the grab
    // comes back in device pixels while clicks are delivered in logical ones,
    // and the click map runs on what is reported here. This is the same pair
    // /render/screenshot.ppm sends as its X-Anoa-Viewport-* headers.
    viewportW = m_browser->width();
    viewportH = m_browser->height();
    return pixmap.toImage();
}

void InProcessFrameBackend::requestRgbFrame(int width, int height)
{
    int viewportW = 0, viewportH = 0;
    QImage img = capture(viewportW, viewportH);
    if (img.isNull()) {
        emit frameFailed(QStringLiteral("capture failed"));
        return;
    }

    if (width > 0 && height > 0)
        img = img.scaled(width, height, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    img = img.convertToFormat(QImage::Format_RGB888);

    FrameData frame;
    frame.width = img.width();
    frame.height = img.height();
    frame.viewportW = viewportW;
    frame.viewportH = viewportH;
    // Packed RGB, no row padding: scanlines are 4-byte aligned, so copying
    // bytesPerLine() would hand the halfblock renderer the padding as pixels.
    frame.bytes.reserve(img.width() * img.height() * 3);
    for (int y = 0; y < img.height(); ++y)
        frame.bytes.append(reinterpret_cast<const char *>(img.constScanLine(y)), img.width() * 3);

    emit frameReady(frame);
}

void InProcessFrameBackend::requestPngFrame()
{
    int viewportW = 0, viewportH = 0;
    const QImage img = capture(viewportW, viewportH);
    if (img.isNull()) {
        emit frameFailed(QStringLiteral("capture failed"));
        return;
    }

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    if (!img.save(&buffer, "PNG")) {
        emit frameFailed(QStringLiteral("PNG encode failed"));
        return;
    }

    FrameData frame;
    frame.bytes = png;
    frame.width = img.width();
    frame.height = img.height();
    frame.viewportW = viewportW;
    frame.viewportH = viewportH;
    emit frameReady(frame);
}

// ── Input ───────────────────────────────────────────────────────────────────

void InProcessFrameBackend::sendClick(int pageX, int pageY, MouseButton button)
{
    if (m_browser)
        m_browser->sendClick(QPoint(pageX, pageY), qtButton(button));
}

void InProcessFrameBackend::sendScroll(int pageX, int pageY, int dy)
{
    if (!m_browser)
        return;
    // Negative coordinates mean the pointer was off the page image. The HTTP
    // backend drops x/y in that case and /render/scroll substitutes the
    // viewport centre; do the same here so the two transports scroll the same
    // place rather than only agreeing when the pointer is over the page.
    const QPoint pos(pageX >= 0 ? pageX : m_browser->width() / 2,
                     pageY >= 0 ? pageY : m_browser->height() / 2);
    m_browser->sendScroll(pos, dy);
}

void InProcessFrameBackend::sendText(const QByteArray &utf8)
{
    if (m_browser)
        m_browser->sendText(QString::fromUtf8(utf8));
}

void InProcessFrameBackend::sendKey(const QString &namedKey)
{
    if (m_browser)
        m_browser->sendKey(namedKey);
}

// ── Navigation ──────────────────────────────────────────────────────────────

void InProcessFrameBackend::navigate(const QString &url)
{
    if (m_browser)
        m_browser->load(QUrl(url));
}

void InProcessFrameBackend::goBack()
{
    if (m_browser)
        m_browser->back();
}

void InProcessFrameBackend::goForward()
{
    if (m_browser)
        m_browser->forward();
}

void InProcessFrameBackend::reloadPage()
{
    if (m_browser)
        m_browser->reload();
}

QString InProcessFrameBackend::description() const
{
    // The other two backends name a host:port here. There is no endpoint to
    // name, and saying "127.0.0.1:9222" would be a lie that sends someone
    // looking for a server that was never started.
    return QStringLiteral("embedded");
}
