#pragma once

// The third FrameBackend: an AnoaBrowser living in this very process.
//
// This is what `anoa-browser terminal` uses when it is given no target at all
// — no --term-host, no --term-port, no --cdp. There is no socket, no port and
// no second process: frames come from QWidget::grab() and input goes straight
// into the view, so the /render/* and CDP transports are bypassed entirely
// rather than pointed at ourselves.
//
// The cost is the one main.cpp documents: hosting a browser needs QApplication
// and the WebEngine stack, so this mode alone cannot be the thin QCoreApplication
// client the other two transports are.
//
// POSIX only, like the rest of src/terminal (see CMakeLists.txt).

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>

#include "terminal/frame_backend.h"

class AnoaBrowser;

class InProcessFrameBackend : public FrameBackend
{
    Q_OBJECT

public:
    // `browser` is borrowed, not owned: main.cpp builds it on the stack because
    // WebEngine wants its view to outlive the event loop, and this class is
    // destroyed first.
    explicit InProcessFrameBackend(AnoaBrowser *browser, QObject *parent = nullptr);

    // Synchronous like RenderHttpClient: frameReady()/frameFailed() is emitted
    // before the call returns, which TerminalUi already tolerates.
    void requestRgbFrame(int width, int height) override;
    void requestPngFrame() override;

    void sendClick(int pageX, int pageY, MouseButton button) override;
    void sendScroll(int pageX, int pageY, int dy) override;
    void sendText(const QByteArray &utf8) override;
    void sendKey(const QString &namedKey) override;

    void navigate(const QString &url) override;
    void goBack() override;
    void goForward() override;
    void reloadPage() override;

    // Honoured here, and only here: this backend owns the browser, so it can
    // reshape the page to the terminal instead of letterboxing it.
    void resizeViewport(int width, int height) override;

    QString description() const override;

private:
    // grab() + the viewport size that goes with it, shared by both frame
    // requests. Returns a null image when the widget has no backing surface.
    QImage capture(int &viewportW, int &viewportH) const;

    AnoaBrowser *m_browser = nullptr;
};
