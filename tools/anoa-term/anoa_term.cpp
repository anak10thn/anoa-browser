// anoa-term — terminal viewer/controller for a running anoa-browser.
//
// Renders the live browser view in the terminal and forwards terminal mouse
// clicks and wheel events back to the browser through the /render/* HTTP
// endpoints. Pure POSIX + C++17, no Qt dependency.
//
// Two rendering backends:
//   - gfx: full-resolution PNG via the iTerm2 inline-image (OSC 1337) or
//     kitty graphics (APC G) protocol — crisp, auto-detected where supported
//   - halfblock: ANSI truecolor ▀ cells (1 cell = 1×2 px) — works everywhere
//
// Usage:
//   anoa-term [--host 127.0.0.1] [--port 9222] [--token SECRET] [--fps 10]
//             [--gfx auto|halfblock|iterm|kitty]
//
// Mouse clicks, wheel scrolling and the keyboard (text, Enter, Backspace,
// Tab, arrows) are forwarded to the page. Ctrl-C / Ctrl-Q quits.

#include <arpa/inet.h>
#include <netdb.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

// ── Config ──────────────────────────────────────────────────────────────────

enum class GfxMode { Auto, Halfblock, Iterm, Kitty };

struct Options {
    std::string host = "127.0.0.1";
    int port = 9222;
    std::string token;
    int fps = 10;
    GfxMode gfx = GfxMode::Auto;
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

// ── PPM (P6) parsing — halfblock backend ────────────────────────────────────

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<unsigned char> rgb; // width*height*3
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

// ── PNG IHDR dimensions — gfx backend needs only width/height ──────────────

bool pngDimensions(const std::string &png, int &w, int &h)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (png.size() < 24 || memcmp(png.data(), sig, 8) != 0)
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(png.data());
    w = (p[16] << 24) | (p[17] << 16) | (p[18] << 8) | p[19];
    h = (p[20] << 24) | (p[21] << 16) | (p[22] << 8) | p[23];
    return w > 0 && h > 0;
}

std::string base64Encode(const std::string &in)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16)
                           | (static_cast<unsigned char>(in[i + 1]) << 8)
                           | static_cast<unsigned char>(in[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == in.size()) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16)
                           | (static_cast<unsigned char>(in[i + 1]) << 8);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
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

// Ask the terminal for its character-cell size in pixels (XTWINOPS 16).
// Must run in raw mode with no other pending input (i.e. right at startup).
// Falls back to 8×16 (the common 1:2 cell) when the terminal stays silent.
void queryCellSize(int &cellW, int &cellH)
{
    cellW = 8;
    cellH = 16;
    fputs("\033[16t", stdout);
    fflush(stdout);

    std::string buf;
    for (int attempts = 0; attempts < 10; ++attempts) {
        struct timeval tv = {0, 50000}; // 50 ms per poll, ≤500 ms total
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(STDIN_FILENO, &fds);
        if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
            continue;
        char tmp[64];
        const ssize_t n = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (n <= 0)
            continue;
        buf.append(tmp, static_cast<size_t>(n));
        int h = 0, w = 0;
        const size_t at = buf.find("\033[6;");
        if (at != std::string::npos
            && sscanf(buf.c_str() + at, "\033[6;%d;%dt", &h, &w) == 2 && w > 0 && h > 0) {
            cellW = w;
            cellH = h;
            return;
        }
    }
}

GfxMode detectGfx()
{
    const char *term = getenv("TERM");
    const char *prog = getenv("TERM_PROGRAM");
    if (getenv("KITTY_WINDOW_ID") || (term && strstr(term, "kitty"))
        || (prog && strcmp(prog, "ghostty") == 0))
        return GfxMode::Kitty;
    if (prog && (strcmp(prog, "iTerm.app") == 0 || strcmp(prog, "WezTerm") == 0))
        return GfxMode::Iterm;
    return GfxMode::Halfblock;
}

// ── Coordinate mapping: terminal cells → browser viewport pixels ───────────

// Extent of the displayed page image, in terminal cells, plus the browser's
// logical viewport size. Valid for both backends: halfblock shows 1 px per
// column and 2 px per row; gfx stretches the PNG over dispCols×dispRows.
struct DisplayMap {
    int dispCols = 0;
    int dispRows = 0;
    int viewportW = 0;
    int viewportH = 0;
};

bool mapCellToPage(const DisplayMap &map, int cellX, int cellY, int &pageX, int &pageY)
{
    if (map.dispCols <= 0 || map.dispRows <= 0 || map.viewportW <= 0 || map.viewportH <= 0)
        return false;
    if (cellX < 0 || cellY < 0 || cellX >= map.dispCols || cellY >= map.dispRows)
        return false;
    pageX = static_cast<int>((cellX + 0.5) * map.viewportW / map.dispCols);
    pageY = static_cast<int>((cellY + 0.5) * map.viewportH / map.dispRows);
    return true;
}

void sendClick(const Options &opt, const DisplayMap &map, int cellX, int cellY, int button)
{
    int pageX = 0, pageY = 0;
    if (!mapCellToPage(map, cellX, cellY, pageX, pageY))
        return;
    static const char *names[] = {"left", "middle", "right"};
    const std::string path = "/render/click?x=" + std::to_string(pageX)
                             + "&y=" + std::to_string(pageY)
                             + "&button=" + names[button % 3];
    HttpResponse resp;
    httpRequest(opt, "POST", withToken(opt, path), resp);
}

void sendScroll(const Options &opt, const DisplayMap &map, int cellX, int cellY, int dy)
{
    std::string path = "/render/scroll?dy=" + std::to_string(dy);
    int pageX = 0, pageY = 0;
    if (mapCellToPage(map, cellX, cellY, pageX, pageY))
        path += "&x=" + std::to_string(pageX) + "&y=" + std::to_string(pageY);
    HttpResponse resp;
    httpRequest(opt, "POST", withToken(opt, path), resp);
}

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

void sendText(const Options &opt, const std::string &text)
{
    HttpResponse resp;
    httpRequest(opt, "POST", withToken(opt, "/render/type?text=" + urlEncode(text)), resp);
}

void sendKey(const Options &opt, const std::string &keyName)
{
    HttpResponse resp;
    httpRequest(opt, "POST", withToken(opt, "/render/key?key=" + keyName), resp);
}

// ── Rendering ───────────────────────────────────────────────────────────────

void renderStatusBar(int cols, int rows, const std::string &statusText)
{
    std::string out = "\033[" + std::to_string(rows) + ";1H\033[7m";
    std::string status = statusText;
    if (static_cast<int>(status.size()) > cols)
        status.resize(static_cast<size_t>(cols));
    out += status;
    out.append(static_cast<size_t>(cols) - status.size(), ' ');
    out += "\033[0m";
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// Halfblock: 1 cell = 1×2 pixels via ▀ (fg = top px, bg = bottom px).
void renderHalfblock(const Frame &frame, int cols, int rows)
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

    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// Gfx: full-resolution PNG via terminal graphics protocol, drawn into an
// aspect-correct dispCols×dispRows cell rectangle at the top-left.
void renderGfx(GfxMode mode, const std::string &png, int dispCols, int dispRows)
{
    const std::string b64 = base64Encode(png);
    std::string out = "\033[H";

    if (mode == GfxMode::Iterm) {
        // preserveAspectRatio=0: we already sized the cell rect to the image
        // aspect, so let the terminal fill it exactly.
        out += "\033]1337;File=inline=1;size=" + std::to_string(png.size())
               + ";width=" + std::to_string(dispCols)
               + ";height=" + std::to_string(dispRows)
               + ";preserveAspectRatio=0:" + b64 + "\007";
    } else { // Kitty
        // Same image id (i=1) + placement id (p=1) each frame → the terminal
        // replaces the previous frame atomically, no delete needed. Payload
        // must be chunked to ≤4096 bytes; only the first chunk carries keys.
        const size_t chunkSize = 4096;
        size_t off = 0;
        bool first = true;
        while (off < b64.size()) {
            const size_t len = std::min(chunkSize, b64.size() - off);
            const bool last = off + len >= b64.size();
            out += "\033_G";
            if (first) {
                out += "a=T,f=100,i=1,p=1,q=2,C=1,c=" + std::to_string(dispCols)
                       + ",r=" + std::to_string(dispRows) + ",";
                first = false;
            }
            out += "m=" + std::string(last ? "0" : "1") + ";";
            out += b64.substr(off, len);
            out += "\033\\";
            off += len;
        }
    }

    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// ── Input handling ──────────────────────────────────────────────────────────

// Consume every complete escape sequence / key in `buf`; returns false on
// quit. `feedback` receives a short description of the last event forwarded,
// shown in the status bar so users can verify their terminal delivers input.
bool processInput(std::string &buf, const Options &opt, const DisplayMap &map,
                  std::string &feedback)
{
    // Batch consecutive printable bytes into one /render/type call so pasted
    // text is not sent one HTTP request per character. UTF-8 continuation
    // bytes (>= 0x80) pass straight through.
    std::string pending;
    auto flushPending = [&]() {
        if (pending.empty())
            return;
        sendText(opt, pending);
        feedback = "typed \"" + (pending.size() > 12 ? pending.substr(0, 12) + "…" : pending) + "\"";
        pending.clear();
    };

    size_t i = 0;
    while (i < buf.size()) {
        const char c = buf[i];
        const auto u = static_cast<unsigned char>(c);

        if (c == 3 || c == 17) { // Ctrl-C / Ctrl-Q (ISIG is off)
            flushPending();
            return false;
        }

        if (c != '\033') {
            if (c == '\r' || c == '\n') {
                flushPending();
                sendKey(opt, "enter");
                feedback = "key enter";
            } else if (c == 127 || c == 8) {
                flushPending();
                sendKey(opt, "backspace");
                feedback = "key backspace";
            } else if (c == '\t') {
                flushPending();
                sendKey(opt, "tab");
                feedback = "key tab";
            } else if (u >= 32 || u >= 0x80) { // printable ASCII or UTF-8 bytes
                pending += c;
            }
            ++i;
            continue;
        }

        flushPending();
        if (i + 1 >= buf.size())
            break; // incomplete sequence — wait for more bytes
        if (buf[i + 1] != '[') {
            i += 2;
            continue;
        }

        // Arrow keys → forwarded as key events. In a focused text field they
        // move the caret; otherwise Chromium scrolls the page — both natural.
        if (i + 2 < buf.size() && buf[i + 2] >= 'A' && buf[i + 2] <= 'D') {
            static const char *arrows[] = {"up", "down", "right", "left"};
            const char *name = arrows[buf[i + 2] - 'A'];
            sendKey(opt, name);
            feedback = std::string("key ") + name;
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
                    int pageX = 0, pageY = 0;
                    const bool inPage = mapCellToPage(map, cellX, cellY, pageX, pageY);
                    if (btn >= 0 && btn <= 2) {
                        sendClick(opt, map, cellX, cellY, btn);
                        feedback = inPage ? "click " + std::to_string(pageX) + ","
                                                + std::to_string(pageY)
                                          : "click outside page";
                    } else if (btn == 64) {
                        sendScroll(opt, map, cellX, cellY, 120);
                        feedback = "scroll up";
                    } else if (btn == 65) {
                        sendScroll(opt, map, cellX, cellY, -120);
                        feedback = "scroll down";
                    }
                }
            }
            i = j;
            continue;
        }
        i += 2;
    }
    flushPending();
    buf.erase(0, i);
    return true;
}

void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--host HOST] [--port PORT] [--token SECRET] [--fps N]\n"
            "          [--gfx auto|halfblock|iterm|kitty]\n"
            "Connects to a running anoa-browser and renders its view in the\n"
            "terminal. Click, scroll and type in the terminal to drive the page.\n"
            "Keys are forwarded to the page (text, Enter, Backspace, Tab,\n"
            "arrows); Ctrl-C or Ctrl-Q quits.\n"
            "--gfx auto (default) uses the iTerm2/kitty image protocol when the\n"
            "terminal supports it (crisp, full resolution) and falls back to\n"
            "ANSI halfblock rendering everywhere else.\n",
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
        } else if (arg == "--gfx" && hasValue) {
            const std::string mode = argv[++i];
            if (mode == "auto")
                opt.gfx = GfxMode::Auto;
            else if (mode == "halfblock")
                opt.gfx = GfxMode::Halfblock;
            else if (mode == "iterm")
                opt.gfx = GfxMode::Iterm;
            else if (mode == "kitty")
                opt.gfx = GfxMode::Kitty;
            else {
                usage(argv[0]);
                return 2;
            }
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

    GfxMode gfx = (opt.gfx == GfxMode::Auto) ? detectGfx() : opt.gfx;

    if (!enterRawMode()) {
        fprintf(stderr, "anoa-term: failed to set raw terminal mode\n");
        return 1;
    }
    signal(SIGINT, onSignalQuit);
    signal(SIGTERM, onSignalQuit);
    signal(SIGWINCH, onSignalResize);

    int cellW = 8, cellH = 16;
    if (gfx != GfxMode::Halfblock)
        queryCellSize(cellW, cellH);

    const char *modeName = (gfx == GfxMode::Iterm) ? "iterm"
                           : (gfx == GfxMode::Kitty) ? "kitty"
                                                     : "halfblock";

    Frame frame;
    DisplayMap map;
    std::string inputBuf;
    std::string lastPng;
    std::string lastInput;
    const long framePeriodUs = 1000000L / opt.fps;

    while (!g_quit) {
        if (g_resized) {
            g_resized = 0;
            lastPng.clear();          // force a redraw at the new size
            fputs("\033[2J", stdout); // clear stale cells
        }

        int cols = 0, rows = 0;
        terminalSize(cols, rows);
        bool fetched = false;
        std::string statusExtra;

        if (gfx == GfxMode::Halfblock) {
            const int imgW = cols;
            const int imgH = (rows - 1) * 2;
            HttpResponse resp;
            const std::string path = "/render/screenshot.ppm?w=" + std::to_string(imgW)
                                     + "&h=" + std::to_string(imgH);
            if (httpRequest(opt, "GET", withToken(opt, path), resp) && resp.status == 200
                && parsePpm(resp.body, frame)) {
                map.viewportW = atoi(resp.headers["x-anoa-viewport-width"].c_str());
                map.viewportH = atoi(resp.headers["x-anoa-viewport-height"].c_str());
                map.dispCols = frame.width;
                map.dispRows = (frame.height + 1) / 2;
                renderHalfblock(frame, cols, rows);
                fetched = true;
            }
        } else {
            HttpResponse resp;
            int imgW = 0, imgH = 0;
            if (httpRequest(opt, "GET", withToken(opt, "/render/screenshot.png"), resp)
                && resp.status == 200 && pngDimensions(resp.body, imgW, imgH)) {
                map.viewportW = atoi(resp.headers["x-anoa-viewport-width"].c_str());
                map.viewportH = atoi(resp.headers["x-anoa-viewport-height"].c_str());

                // Fit an aspect-correct cell rect into cols × (rows-1) cells.
                // colsPerRow = image aspect corrected by the cell pixel aspect.
                const double colsPerRow = (static_cast<double>(imgW) / imgH)
                                          * (static_cast<double>(cellH) / cellW);
                int r = rows - 1;
                int c = static_cast<int>(llround(r * colsPerRow));
                if (c > cols) {
                    c = cols;
                    r = std::max(1, static_cast<int>(llround(c / colsPerRow)));
                }
                map.dispCols = std::max(1, c);
                map.dispRows = std::max(1, r);

                if (resp.body != lastPng) { // skip identical frames — no flicker
                    renderGfx(gfx, resp.body, map.dispCols, map.dispRows);
                    lastPng = std::move(resp.body);
                }
                fetched = true;
            }
        }

        std::string status = " anoa-term " + opt.host + ":" + std::to_string(opt.port)
                             + " " + std::to_string(map.viewportW) + "x"
                             + std::to_string(map.viewportH) + " [" + modeName + "]";
        if (!fetched)
            status += " connection lost — retrying…";
        // Once the user has interacted, the last forwarded event replaces the
        // long usage hint so it survives narrow terminals.
        status += lastInput.empty() ? " click/type/scroll drive the page, ctrl-c quits"
                                    : " ctrl-c=quit | " + lastInput;
        renderStatusBar(cols, rows, status);

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
                if (!processInput(inputBuf, opt, map, lastInput))
                    break;
            }
        }
    }

    restoreTerminal();
    return 0;
}
