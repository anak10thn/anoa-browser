#pragma once

// The CDP FrameBackend: what `anoa-browser terminal --cdp URL` draws from.
//
// It owns a CdpClient and answers the seam's two frame requests with
// Page.captureScreenshot. Everything the /render/* server used to do on its
// side has to happen here instead:
//
//   * scaling      - there is no ?w=&h= on this path, so the PNG comes back at
//                    full size and the halfblock downscale is local.
//   * the viewport - /render/screenshot.ppm answered with X-Anoa-Viewport-*;
//                    here the numbers come from Page.getLayoutMetrics. They
//                    are CSS pixels: with deviceScaleFactor > 1 the PNG is
//                    that many times larger than the viewport a click
//                    coordinate lives in, so mapping terminal cells against
//                    image pixels would put every click off-target.
//
// Unlike RenderHttpClient nothing here blocks. A frame request is answered on
// a later turn of the event loop, and a tick that arrives while a capture is
// still outstanding is dropped rather than queued — the frame timer must never
// wait on the network, and a queue of stale captures would only make the
// viewer lag further behind the page.
//
// One consequence for callers: a dropped tick is answered by neither
// frameReady() nor frameFailed(), so it is the one case where the seam's
// "exactly one answer per request" rule does not hold. TerminalUi treats an
// unanswered tick as "no new frame", which is exactly right.
//
// The input half is Input.dispatchMouseEvent / insertText / dispatchKeyEvent,
// and every one of them is fire-and-forget: the command goes out, the frame
// loop carries on, and only the error half of the reply is ever read (into the
// status bar). Two things differ from what /render/* did on the server side and
// have to be paid for here — the wheel delta sign, which CDP defines the
// opposite way round from Qt's angleDelta, and the named-key table, which
// anoa_browser.cpp expressed as Qt::Key values that mean nothing to CDP.
//
// POSIX only, like the rest of src/terminal (see CMakeLists.txt).

#include <QByteArray>
#include <QObject>
#include <QString>

#include "terminal/cdp_client.h"
#include "terminal/frame_backend.h"

struct Config;

class CdpFrameBackend : public FrameBackend
{
    Q_OBJECT

public:
    // Builds the client from config.termToken and dials config.cdpUrl right
    // away; both URL forms are handled by CdpClient::connectToEndpoint().
    explicit CdpFrameBackend(const Config &config, QObject *parent = nullptr);

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

    QString description() const override;

    // The transport, for the wiring in terminal_app.cpp (discoveryFailed) and
    // for tests that need to drive the connection directly.
    CdpClient *client() const { return m_client; }

private:
    // Both frame requests funnel through here. `wantRgb` picks the halfblock
    // path (decode + downscale + packed RGB) over the gfx one (PNG verbatim);
    // targetW/targetH are the cell grid in pixels and are unused when false.
    void captureFrame(bool wantRgb, int targetW, int targetH);
    void onCaptureReply(const CdpResult &result, bool wantRgb, int targetW, int targetH);
    // Turns the decoded/undecoded frame into FrameData and emits it.
    void emitRgbFrame(const QByteArray &png, int targetW, int targetH);
    void emitPngFrame(const QByteArray &png);

    // Every Input.* command goes out through here: sent and forgotten, with
    // only the error half of the reply looked at. Nothing on the input path
    // may make the frame loop wait.
    void dispatchInput(const QString &method, const QJsonObject &params);

    // Shared by goBack()/goForward(): delta is -1 or +1 into the history list.
    void historyStep(int delta);

    void requestMetrics();
    void onMetricsReply(const CdpResult &result);
    // Image pixels -> the CSS-pixel viewport the click map runs in.
    void viewportForImage(int imageW, int imageH, int &viewportW, int &viewportH) const;
    // Refreshes the metrics when the page geometry moved under us.
    void noteImageSize(int imageW, int imageH);

    // Empty text means the last capture succeeded.
    void setCaptureError(const QString &text);
    // Empty text means the last input dispatch was accepted.
    void setInputError(const QString &text);
    void updateStatus();

    CdpClient *m_client = nullptr;

    bool m_captureInFlight = false;
    bool m_metricsInFlight = false;
    bool m_metricsValid = false;

    int m_cssViewportW = 0;    // CSS pixels, from Page.getLayoutMetrics
    int m_cssViewportH = 0;    //
    double m_deviceScale = 0.0; // image px per CSS px; 0 means "not known yet"

    int m_lastImageW = 0; // size of the previous screenshot, in image pixels
    int m_lastImageH = 0;
    int m_lastTargetW = 0; // previous halfblock cell grid, to spot a resize
    int m_lastTargetH = 0;

    QString m_captureError;
    QString m_inputError;
    QString m_status; // last text handed to statusChanged(), to skip repeats
};
