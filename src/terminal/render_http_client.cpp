#include "terminal/render_http_client.h"

#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config/config.h"
#include "terminal/frame_bytes.h"

namespace {

std::string urlEncode(const std::string &in)
{
    std::string out;
    for (const char c : in) {
        const auto u = static_cast<unsigned char>(c);
        if (isalnum(u) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char hex[4];
            snprintf(hex, sizeof(hex), "%%%02X", u);
            out += hex;
        }
    }
    return out;
}

// A missing header reads as 0, which is what the old operator[] + atoi() pair
// produced for the same input.
int headerInt(const std::map<std::string, std::string> &headers, const char *key)
{
    const auto it = headers.find(key);
    return it == headers.end() ? 0 : atoi(it->second.c_str());
}

const char *buttonName(MouseButton button)
{
    switch (button) {
    case MouseButton::Middle:
        return "middle";
    case MouseButton::Right:
        return "right";
    case MouseButton::Left:
        break;
    }
    return "left";
}

} // namespace

// ── Lifecycle ───────────────────────────────────────────────────────────────

RenderHttpClient::RenderHttpClient(const Config &config, QObject *parent)
    : FrameBackend(parent)
    , m_host(config.termHost.toStdString())
    , m_port(config.termPort)
    , m_token(config.termToken.toStdString())
{
}

QString RenderHttpClient::description() const
{
    return QString::fromStdString(m_host) + QLatin1Char(':') + QString::number(m_port);
}

// ── Minimal HTTP/1.1 client (Connection: close per request) ────────────────

bool RenderHttpClient::httpRequest(const char *method, const std::string &pathWithQuery,
                                   Response &out) const
{
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    const std::string portStr = std::to_string(m_port);
    if (getaddrinfo(m_host.c_str(), portStr.c_str(), &hints, &res) != 0)
        return false;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        // Qualified: this is a member function of a QObject subclass, so an
        // unqualified connect() resolves to the static QObject::connect
        // overloads and never reaches the one in <sys/socket.h>.
        if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return false;

    std::string req = std::string(method) + " " + pathWithQuery + " HTTP/1.1\r\n"
                      "Host: " + m_host + ":" + portStr + "\r\n";
    if (!m_token.empty())
        req += "Authorization: Bearer " + m_token + "\r\n";
    req += "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < req.size()) {
        const ssize_t n = write(fd, req.data() + sent, req.size() - sent);
        if (n <= 0) {
            close(fd);
            return false;
        }
        sent += static_cast<size_t>(n);
    }

    std::string raw;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        raw.append(buf, static_cast<size_t>(n));
    close(fd);

    const size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos)
        return false;

    out.status = 0;
    out.headers.clear();
    const size_t lineEnd = raw.find("\r\n");
    if (sscanf(raw.c_str(), "HTTP/%*d.%*d %d", &out.status) != 1)
        return false;

    size_t pos = lineEnd + 2;
    while (pos < headerEnd) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd)
            eol = headerEnd;
        const std::string line = raw.substr(pos, eol - pos);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = line.substr(0, colon);
            for (char &c : key)
                c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
            size_t vstart = colon + 1;
            while (vstart < line.size() && line[vstart] == ' ')
                ++vstart;
            out.headers[key] = line.substr(vstart);
        }
        pos = eol + 2;
    }

    out.body = raw.substr(headerEnd + 4);
    return true;
}

std::string RenderHttpClient::withToken(std::string path) const
{
    if (!m_token.empty())
        path += (path.find('?') == std::string::npos ? "?" : "&") + std::string("token=") + m_token;
    return path;
}

void RenderHttpClient::post(const std::string &path) const
{
    Response resp;
    httpRequest("POST", withToken(path), resp);
}

// ── Frames ──────────────────────────────────────────────────────────────────

void RenderHttpClient::requestRgbFrame(int width, int height)
{
    const std::string path = "/render/screenshot.ppm?w=" + std::to_string(width)
                             + "&h=" + std::to_string(height);
    Response resp;
    if (!httpRequest("GET", withToken(path), resp)) {
        emit frameFailed(QStringLiteral("no response"));
        return;
    }
    if (resp.status != 200) {
        emit frameFailed(QStringLiteral("HTTP %1").arg(resp.status));
        return;
    }

    FrameData frame;
    if (!frame_bytes::parsePpm(resp.body.data(), resp.body.size(), frame.width, frame.height,
                               frame.bytes)) {
        emit frameFailed(QStringLiteral("malformed PPM"));
        return;
    }
    frame.viewportW = headerInt(resp.headers, "x-anoa-viewport-width");
    frame.viewportH = headerInt(resp.headers, "x-anoa-viewport-height");
    emit frameReady(frame);
}

void RenderHttpClient::requestPngFrame()
{
    Response resp;
    if (!httpRequest("GET", withToken("/render/screenshot.png"), resp)) {
        emit frameFailed(QStringLiteral("no response"));
        return;
    }
    if (resp.status != 200) {
        emit frameFailed(QStringLiteral("HTTP %1").arg(resp.status));
        return;
    }

    FrameData frame;
    if (!frame_bytes::pngDimensions(resp.body.data(), resp.body.size(), frame.width,
                                    frame.height)) {
        emit frameFailed(QStringLiteral("malformed PNG"));
        return;
    }
    frame.bytes = QByteArray(resp.body.data(), static_cast<qsizetype>(resp.body.size()));
    frame.viewportW = headerInt(resp.headers, "x-anoa-viewport-width");
    frame.viewportH = headerInt(resp.headers, "x-anoa-viewport-height");
    emit frameReady(frame);
}

// ── Input ───────────────────────────────────────────────────────────────────

void RenderHttpClient::sendClick(int pageX, int pageY, MouseButton button)
{
    post("/render/click?x=" + std::to_string(pageX) + "&y=" + std::to_string(pageY)
         + "&button=" + buttonName(button));
}

void RenderHttpClient::sendScroll(int pageX, int pageY, int dy)
{
    // dy is a Qt angleDelta: +120 scrolls up. Left as-is on purpose - the
    // opposite CDP convention is handled in the CDP backend, not here.
    std::string path = "/render/scroll?dy=" + std::to_string(dy);
    // Negative coordinates mean the pointer was off the page image; the old
    // query-string builder simply omitted x and y in that case.
    if (pageX >= 0 && pageY >= 0)
        path += "&x=" + std::to_string(pageX) + "&y=" + std::to_string(pageY);
    post(path);
}

void RenderHttpClient::sendText(const QByteArray &utf8)
{
    post("/render/type?text="
         + urlEncode(std::string(utf8.constData(), static_cast<size_t>(utf8.size()))));
}

void RenderHttpClient::sendKey(const QString &namedKey)
{
    post("/render/key?key=" + namedKey.toStdString());
}

// ── Navigation ──────────────────────────────────────────────────────────────

void RenderHttpClient::navigate(const QString &url)
{
    // The url rides in the query string rather than the body: post() sends no
    // body at all, and /render/navigate accepts either.
    post("/render/navigate?url=" + urlEncode(url.toStdString()));
}

void RenderHttpClient::goBack()
{
    post("/render/back");
}

void RenderHttpClient::goForward()
{
    post("/render/forward");
}

void RenderHttpClient::reloadPage()
{
    post("/render/reload");
}
