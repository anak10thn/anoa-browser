#pragma once

// The default FrameBackend: the /render/* HTTP endpoints of a local
// anoa-browser, carried over verbatim from tools/anoa-term/anoa_term.cpp.
//
// This path is deliberately the known-good control for CDP mode — endpoints,
// query strings, headers and the token handling are unchanged, so a behaviour
// difference between the two backends is always the CDP side's fault. The
// hand-rolled blocking HTTP/1.1 client comes along for the same reason: it is
// the code that shipped, and swapping it for QNetworkAccessManager would put a
// second variable in every diff.
//
// POSIX only — the socket layer is getaddrinfo/socket/read/write and this
// header is never included on Windows (see CMakeLists.txt).

#include <map>
#include <string>

#include <QByteArray>
#include <QObject>
#include <QString>

#include "terminal/frame_backend.h"

struct Config;

class RenderHttpClient : public FrameBackend
{
    Q_OBJECT

public:
    explicit RenderHttpClient(const Config &config, QObject *parent = nullptr);

    // Both requests are synchronous: frameReady()/frameFailed() is emitted
    // before the call returns. TerminalUi is written to tolerate that.
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

private:
    // One HTTP/1.1 exchange over a connection that is closed straight after —
    // no keep-alive, no pipelining, one socket per frame, exactly as before.
    struct Response {
        int status = 0;
        std::map<std::string, std::string> headers; // keys lowercased
        std::string body;
    };

    bool httpRequest(const char *method, const std::string &pathWithQuery, Response &out) const;
    // Appends ?token=/&token= when a token is configured. The Bearer header is
    // sent too; the server accepts either, and both are kept so the request on
    // the wire is unchanged.
    std::string withToken(std::string path) const;
    // Fire-and-forget POST for the input endpoints — the reply carries nothing
    // the viewer needs and a failure shows up as the next lost frame.
    void post(const std::string &path) const;

    std::string m_host;
    int m_port = 9222;
    std::string m_token;
};
