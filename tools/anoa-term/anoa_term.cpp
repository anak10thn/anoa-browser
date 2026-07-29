// anoa-term — terminal viewer/controller for a running anoa-browser.
//
// Renders the live browser view as ANSI truecolor half-blocks and forwards
// terminal mouse clicks and wheel events back to the browser through the
// /render/* HTTP endpoints. Pure POSIX + C++17, no Qt dependency.
//
// Usage:
//   anoa-term [--host 127.0.0.1] [--port 9222] [--token SECRET] [--fps 10]
//
// Keys: q quit · arrow Up/Down scroll · mouse click = click in the page.

#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ── Config ──────────────────────────────────────────────────────────────────

struct Options {
    std::string host = "127.0.0.1";
    int port = 9222;
    std::string token;
    int fps = 10;
};

// ── Minimal HTTP/1.1 client (Connection: close per request) ────────────────

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers; // keys lowercased
    std::string body;
};

bool httpRequest(const Options &opt, const std::string &method,
                 const std::string &pathWithQuery, HttpResponse &out)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    const std::string portStr = std::to_string(opt.port);
    if (getaddrinfo(opt.host.c_str(), portStr.c_str(), &hints, &res) != 0)
        return false;

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
        return false;

    std::string req = method + " " + pathWithQuery + " HTTP/1.1\r\n"
                      "Host: " + opt.host + ":" + portStr + "\r\n";
    if (!opt.token.empty())
        req += "Authorization: Bearer " + opt.token + "\r\n";
    req += "Connection: close\r\n\r\n";

    size_t sent = 0;
    while (sent < req.size()) {
        ssize_t n = write(fd, req.data() + sent, req.size() - sent);
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

std::string withToken(const Options &opt, std::string path)
{
    if (!opt.token.empty())
        path += (path.find('?') == std::string::npos ? "?" : "&") + std::string("token=") + opt.token;
    return path;
}

// ── PPM (P6) parsing ────────────────────────────────────────────────────────

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb; // width*height*3
    int viewportW = 0;              // browser logical viewport, for click mapping
    int viewportH = 0;
};

bool parsePpm(const std::string &data, Frame &frame)
{
    size_t pos = 0;
    auto skipSpace = [&]() {
        while (pos < data.size()) {
            if (isspace(static_cast<unsigned char>(data[pos]))) {
                ++pos;
            } else if (data[pos] == '#') { // comment to end of line
                while (pos < data.size() && data[pos] != '\n')
                    ++pos;
            } else {
                break;
            }
        }
    };
    auto readInt = [&](int &value) {
        skipSpace();
        if (pos >= data.size() || !isdigit(static_cast<unsigned char>(data[pos])))
            return false;
        value = 0;
        while (pos < data.size() && isdigit(static_cast<unsigned char>(data[pos])))
            value = value * 10 + (data[pos++] - '0');
        return true;
    };

    if (data.size() < 2 || data[0] != 'P' || data[1] != '6')
        return false;
    pos = 2;
    int w = 0, h = 0, maxval = 0;
    if (!readInt(w) || !readInt(h) || !readInt(maxval) || maxval != 255)
        return false;
    ++pos; // single whitespace byte after maxval
    const size_t need = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;
    if (w <= 0 || h <= 0 || data.size() - pos < need)
        return false;

    frame.width = w;
    frame.height = h;
    frame.rgb.assign(data.begin() + static_cast<long>(pos),
                     data.begin() + static_cast<long>(pos + need));
    return true;
}

// ── Terminal state ──────────────────────────────────────────────────────────

struct termios g_origTermios;
bool g_rawActive = false;
volatile sig_atomic_t g_resized = 0;
volatile sig_atomic_t g_quit = 0;

void restoreTerminal()
{
    if (!g_rawActive)
        return;
    g_rawActive = false;
    // Mouse off, main screen, cursor visible, colors reset.
    fputs("\033[?1000;1006l\033[?1049l\033[?25h\033[0m", stdout);
    fflush(stdout);
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_origTermios);
}

void onSignalQuit(int) { g_quit = 1; }
void onSignalResize(int) { g_resized = 1; }

bool enterRawMode()
{
    if (tcgetattr(STDIN_FILENO, &g_origTermios) != 0)
        return false;
    struct termios raw = g_origTermios;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON | ISIG);
    raw.c_iflag &= ~static_cast<tcflag_t>(IXON | ICRNL);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0)
        return false;
    g_rawActive = true;
    atexit(restoreTerminal);
    // Alt screen, hide cursor, mouse press + SGR extended reporting.
    fputs("\033[?1049h\033[?25l\033[?1000;1006h", stdout);
    fflush(stdout);
    return true;
}

void terminalSize(int &cols, int &rows)
{
    struct winsize ws = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    } else {
        cols = 80;
        rows = 24;
    }
}

// ── Rendering: 1 cell = 1×2 pixels via ▀ (fg = top px, bg = bottom px) ─────

void renderFrame(const Frame &frame, int cols, int rows, const std::string &statusText)
{
    std::string out;
    out.reserve(static_cast<size_t>(cols) * static_cast<size_t>(rows) * 24);
    out += "\033[H";

    const int cellRows = rows - 1; // last row reserved for the status bar
    int lastFr = -1, lastFg = -1, lastFb = -1;
    int lastBr = -1, lastBg = -1, lastBb = -1;

    for (int cy = 0; cy < cellRows; ++cy) {
        for (int cx = 0; cx < cols; ++cx) {
            const int topY = cy * 2, botY = cy * 2 + 1;
            int tr = 0, tg = 0, tb = 0, br = 0, bg = 0, bb = 0;
            if (cx < frame.width && topY < frame.height) {
                const unsigned char *p = &frame.rgb[(static_cast<size_t>(topY) * frame.width + cx) * 3];
                tr = p[0]; tg = p[1]; tb = p[2];
            }
            if (cx < frame.width && botY < frame.height) {
                const unsigned char *p = &frame.rgb[(static_cast<size_t>(botY) * frame.width + cx) * 3];
                br = p[0]; bg = p[1]; bb = p[2];
            }
            char seq[48];
            if (tr != lastFr || tg != lastFg || tb != lastFb) {
                snprintf(seq, sizeof(seq), "\033[38;2;%d;%d;%dm", tr, tg, tb);
                out += seq;
                lastFr = tr; lastFg = tg; lastFb = tb;
            }
            if (br != lastBr || bg != lastBg || bb != lastBb) {
                snprintf(seq, sizeof(seq), "\033[48;2;%d;%d;%dm", br, bg, bb);
                out += seq;
                lastBr = br; lastBg = bg; lastBb = bb;
            }
            out += "\xE2\x96\x80"; // ▀ UPPER HALF BLOCK
        }
        out += "\033[0m\r\n";
        lastFr = lastFg = lastFb = lastBr = lastBg = lastBb = -1;
    }

    out += "\033[7m"; // status bar: inverse video, padded/truncated to width
    std::string status = statusText;
    if (static_cast<int>(status.size()) > cols)
        status.resize(static_cast<size_t>(cols));
    out += status;
    out.append(static_cast<size_t>(cols) - status.size(), ' ');
    out += "\033[0m";

    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// ── Input: map terminal cells back to browser viewport coordinates ─────────

void sendClick(const Options &opt, const Frame &frame, int cellX, int cellY, int button)
{
    if (frame.width <= 0 || frame.viewportW <= 0)
        return;
    const int px = cellX;         // cell (cx,cy) shows pixels (cx, cy*2..cy*2+1)
    const int py = cellY * 2 + 1; // aim at the cell's vertical center
    if (px >= frame.width || py >= frame.height)
        return;
    const int pageX = px * frame.viewportW / frame.width;
    const int pageY = py * frame.viewportH / frame.height;
    static const char *names[] = {"left", "middle", "right"};
    const std::string path = "/render/click?x=" + std::to_string(pageX)
                             + "&y=" + std::to_string(pageY)
                             + "&button=" + names[button % 3];
    HttpResponse resp;
    httpRequest(opt, "POST", withToken(opt, path), resp);
}

void sendScroll(const Options &opt, const Frame &frame, int cellX, int cellY, int dy)
{
    std::string path = "/render/scroll?dy=" + std::to_string(dy);
    if (frame.width > 0 && frame.viewportW > 0 && cellX >= 0
        && cellX < frame.width && cellY * 2 < frame.height) {
        path += "&x=" + std::to_string(cellX * frame.viewportW / frame.width)
                + "&y=" + std::to_string((cellY * 2 + 1) * frame.viewportH / frame.height);
    }
    HttpResponse resp;
    httpRequest(opt, "POST", withToken(opt, path), resp);
}

// Consume every complete escape sequence / key in `buf`; returns false on quit.
bool processInput(std::string &buf, const Options &opt, const Frame &frame)
{
    size_t i = 0;
    while (i < buf.size()) {
        const char c = buf[i];
        if (c == 'q' || c == 'Q' || c == 3) // 3 = Ctrl-C (ISIG is off)
            return false;

        if (c != '\033') {
            ++i;
            continue;
        }
        if (i + 1 >= buf.size())
            break; // incomplete sequence — wait for more bytes
        if (buf[i + 1] != '[') {
            i += 2;
            continue;
        }

        // Arrow keys: ESC [ A (up) / ESC [ B (down) → scroll one wheel notch.
        if (i + 2 < buf.size() && (buf[i + 2] == 'A' || buf[i + 2] == 'B')) {
            sendScroll(opt, frame, -1, -1, buf[i + 2] == 'A' ? 120 : -120);
            i += 3;
            continue;
        }

        // SGR mouse: ESC [ < btn ; col ; row (M=press, m=release), 1-based.
        if (i + 2 < buf.size() && buf[i + 2] == '<') {
            size_t j = i + 3;
            int nums[3] = {0, 0, 0};
            int nIdx = 0;
            bool complete = false;
            char final = 0;
            while (j < buf.size()) {
                const char d = buf[j];
                if (isdigit(static_cast<unsigned char>(d))) {
                    nums[nIdx] = nums[nIdx] * 10 + (d - '0');
                } else if (d == ';' && nIdx < 2) {
                    ++nIdx;
                } else if (d == 'M' || d == 'm') {
                    final = d;
                    complete = true;
                    ++j;
                    break;
                } else {
                    ++j; // malformed — skip byte and resync
                    break;
                }
                ++j;
            }
            if (!complete && j >= buf.size())
                break; // incomplete — wait for more bytes
            if (complete) {
                const int btn = nums[0];
                const int cellX = nums[1] - 1;
                const int cellY = nums[2] - 1;
                if (final == 'M') {
                    if (btn >= 0 && btn <= 2)
                        sendClick(opt, frame, cellX, cellY, btn);
                    else if (btn == 64)
                        sendScroll(opt, frame, cellX, cellY, 120);
                    else if (btn == 65)
                        sendScroll(opt, frame, cellX, cellY, -120);
                }
            }
            i = j;
            continue;
        }
        i += 2;
    }
    buf.erase(0, i);
    return true;
}

void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--host HOST] [--port PORT] [--token SECRET] [--fps N]\n"
            "Connects to a running anoa-browser and renders its view in the\n"
            "terminal. Click and scroll in the terminal to drive the page.\n"
            "Keys: q quit, Up/Down scroll.\n",
            argv0);
}

} // namespace

int main(int argc, char *argv[])
{
    Options opt;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--host" && hasValue) {
            opt.host = argv[++i];
        } else if (arg == "--port" && hasValue) {
            opt.port = atoi(argv[++i]);
        } else if (arg == "--token" && hasValue) {
            opt.token = argv[++i];
        } else if (arg == "--fps" && hasValue) {
            opt.fps = atoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    if (opt.port <= 0 || opt.port > 65535) {
        fprintf(stderr, "anoa-term: invalid port\n");
        return 2;
    }
    if (opt.fps < 1)
        opt.fps = 1;
    if (opt.fps > 30)
        opt.fps = 30;

    // Probe the endpoint before touching the terminal so a connection error
    // prints as a normal message instead of flashing the alt screen.
    {
        HttpResponse probe;
        if (!httpRequest(opt, "GET", withToken(opt, "/render/screenshot.ppm?w=8&h=8"), probe)
            || probe.status != 200) {
            fprintf(stderr,
                    "anoa-term: cannot fetch %s:%d/render/screenshot.ppm (%s)\n"
                    "Is anoa-browser running? Does it need --token?\n",
                    opt.host.c_str(), opt.port,
                    probe.status ? ("HTTP " + std::to_string(probe.status)).c_str()
                                 : "no response");
            return 1;
        }
    }

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "anoa-term: stdin/stdout must be a terminal\n");
        return 1;
    }
    if (!enterRawMode()) {
        fprintf(stderr, "anoa-term: failed to set raw terminal mode\n");
        return 1;
    }
    signal(SIGINT, onSignalQuit);
    signal(SIGTERM, onSignalQuit);
    signal(SIGWINCH, onSignalResize);

    Frame frame;
    std::string inputBuf;
    const long framePeriodUs = 1000000L / opt.fps;

    while (!g_quit) {
        if (g_resized) {
            g_resized = 0;
            fputs("\033[2J", stdout); // clear stale cells after resize
        }

        int cols = 0, rows = 0;
        terminalSize(cols, rows);
        const int imgW = cols;
        const int imgH = (rows - 1) * 2;

        HttpResponse resp;
        const std::string path = "/render/screenshot.ppm?w=" + std::to_string(imgW)
                                 + "&h=" + std::to_string(imgH);
        if (httpRequest(opt, "GET", withToken(opt, path), resp) && resp.status == 200) {
            Frame next;
            if (parsePpm(resp.body, next)) {
                next.viewportW = atoi(resp.headers["x-anoa-viewport-width"].c_str());
                next.viewportH = atoi(resp.headers["x-anoa-viewport-height"].c_str());
                frame = std::move(next);
            }
            const std::string status = " anoa-term  " + opt.host + ":"
                                       + std::to_string(opt.port) + "  "
                                       + std::to_string(frame.viewportW) + "x"
                                       + std::to_string(frame.viewportH)
                                       + "  click=click  wheel/arrows=scroll  q=quit";
            renderFrame(frame, cols, rows, status);
        } else {
            renderFrame(frame, cols, rows, " anoa-term  connection lost — retrying…  q=quit");
        }

        // Wait out the frame period on stdin so input is handled immediately.
        struct timeval tv;
        tv.tv_sec = framePeriodUs / 1000000L;
        tv.tv_usec = framePeriodUs % 1000000L;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        const int ready = select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv);
        if (ready > 0) {
            char buf[512];
            const ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n > 0) {
                inputBuf.append(buf, static_cast<size_t>(n));
                if (!processInput(inputBuf, opt, frame))
                    break;
            }
        }
    }

    restoreTerminal();
    return 0;
}
