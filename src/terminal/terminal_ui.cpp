#include "terminal/terminal_ui.h"

#include <signal.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "common/url_input.h"
#include "config/config.h"

namespace {

// UTF-8 for the em dash and the ellipsis. Spelled as escapes because every
// string literal under src/ stays ASCII (this file is POSIX-only, but the rule
// is the project's, not the platform's) — the bytes on the wire are the same
// ones anoa-term wrote.
const char kEmDash[] = "\xE2\x80\x94";
const char kEllipsis[] = "\xE2\x80\xA6";

std::string base64Encode(const char *in, size_t len)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < len) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16)
                           | (static_cast<unsigned char>(in[i + 1]) << 8)
                           | static_cast<unsigned char>(in[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
        i += 3;
    }
    if (i + 1 == len) {
        const unsigned v = static_cast<unsigned char>(in[i]) << 16;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == len) {
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

void onSignalQuit(int) { g_quit = 1; }
void onSignalResize(int) { g_resized = 1; }

} // namespace

namespace term {

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

void installSignalHandlers()
{
    signal(SIGINT, onSignalQuit);
    signal(SIGTERM, onSignalQuit);
    signal(SIGWINCH, onSignalResize);
}

bool quitSignalled() { return g_quit != 0; }

bool takeResized()
{
    if (!g_resized)
        return false;
    g_resized = 0;
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
// Falls back to 8x16 (the common 1:2 cell) when the terminal stays silent.
void queryCellSize(int &cellW, int &cellH)
{
    cellW = 8;
    cellH = 16;
    fputs("\033[16t", stdout);
    fflush(stdout);

    std::string buf;
    for (int attempts = 0; attempts < 10; ++attempts) {
        struct timeval tv = {0, 50000}; // 50 ms per poll, <=500 ms total
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
    const char *terminal = getenv("TERM");
    const char *prog = getenv("TERM_PROGRAM");
    if (getenv("KITTY_WINDOW_ID") || (terminal && strstr(terminal, "kitty"))
        || (prog && strcmp(prog, "ghostty") == 0))
        return GfxMode::Kitty;
    if (prog && (strcmp(prog, "iTerm.app") == 0 || strcmp(prog, "WezTerm") == 0))
        return GfxMode::Iterm;
    return GfxMode::Halfblock;
}

} // namespace term

// ── Coordinate mapping: terminal cells -> browser viewport pixels ──────────

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

GfxMode gfxModeFromString(const QString &name)
{
    if (name == QLatin1String("halfblock"))
        return GfxMode::Halfblock;
    if (name == QLatin1String("iterm"))
        return GfxMode::Iterm;
    if (name == QLatin1String("kitty"))
        return GfxMode::Kitty;
    return GfxMode::Auto;
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

TerminalUi::TerminalUi(const Config &config, FrameBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_gfx(gfxModeFromString(config.gfxMode))
    , m_fps(config.fps)
{
    // config.cpp validates the range (1-120); clamp anyway so framePeriodMs()
    // can never divide by zero.
    if (m_fps < 1)
        m_fps = 1;

    // --width is the page width in browser mode, and means the same here for
    // the backends that can be resized. Only the height is derived from the
    // terminal.
    if (config.width > 0)
        m_viewportWidth = config.width;
}

TerminalUi::~TerminalUi() { end(); }

bool TerminalUi::begin()
{
    if (m_gfx == GfxMode::Auto)
        m_gfx = term::detectGfx();

    if (!term::enterRawMode())
        return false;
    m_started = true;
    term::installSignalHandlers();

    // The XTWINOPS reply has to be read before any other input arrives, so it
    // happens here rather than lazily at the first gfx frame.
    if (m_gfx != GfxMode::Halfblock)
        term::queryCellSize(m_cellW, m_cellH);

    term::terminalSize(m_cols, m_rows);
    return true;
}

void TerminalUi::end()
{
    if (!m_started)
        return;
    m_started = false;
    term::restoreTerminal();
}

// ── Frame cycle ─────────────────────────────────────────────────────────────

bool TerminalUi::tick()
{
    if (term::quitSignalled())
        return false;
    if (term::takeResized()) {
        m_lastPng.clear();        // force a redraw at the new size
        fputs("\033[2J", stdout); // clear stale cells
    }

    term::terminalSize(m_cols, m_rows);

    // Ask the page to take the shape of the space it is about to be drawn in.
    //
    // The image renderers fit the frame into the cell rect with its aspect
    // ratio kept, so a 16:9 page in a taller terminal leaves the unused rows
    // black — the gap is the letterbox, not a drawing bug. A backend that owns
    // its browser can remove the gap at the source by reshaping the page;
    // /render/* and CDP ignore this and keep letterboxing, which is the honest
    // outcome when the page belongs to somebody else.
    //
    // Halfblock needs none of it: it asks for exactly the grid it has.
    if (m_gfx != GfxMode::Halfblock) {
        const int pxW = m_cols * m_cellW;
        const int pxH = pageRows() * m_cellH;
        if (pxW > 0 && pxH > 0) {
            // Width is pinned so the page keeps a sane CSS width and only the
            // height follows the terminal; deriving both from cell counts would
            // hand the page a viewport of a few hundred pixels on a small
            // window and trigger every mobile breakpoint on the web.
            const int targetW = m_viewportWidth;
            const int targetH = std::max(1, static_cast<int>(llround(
                                    static_cast<double>(targetW) * pxH / pxW)));
            m_backend->resizeViewport(targetW, targetH);
        }
    }

    if (m_gfx == GfxMode::Halfblock)
        m_backend->requestRgbFrame(m_cols, pageRows() * 2);
    else
        m_backend->requestPngFrame();
    return true;
}

// Rows available to the page: every row except the status bar's, and all of
// them when the bar is hidden.
int TerminalUi::pageRows() const
{
    return m_statusVisible ? std::max(1, m_rows - 1) : std::max(1, m_rows);
}

void TerminalUi::onFrame(const FrameData &frame)
{
    m_map.viewportW = frame.viewportW;
    m_map.viewportH = frame.viewportH;

    if (m_gfx == GfxMode::Halfblock) {
        m_map.dispCols = frame.width;
        m_map.dispRows = (frame.height + 1) / 2;
        renderHalfblock(frame);
    } else {
        // Fit an aspect-correct cell rect into cols x (rows-1) cells.
        // colsPerRow = image aspect corrected by the cell pixel aspect.
        if (frame.width > 0 && frame.height > 0) {
            const double colsPerRow = (static_cast<double>(frame.width) / frame.height)
                                      * (static_cast<double>(m_cellH) / m_cellW);
            int r = pageRows();
            int c = static_cast<int>(llround(r * colsPerRow));
            if (c > m_cols) {
                c = m_cols;
                r = std::max(1, static_cast<int>(llround(c / colsPerRow)));
            }
            m_map.dispCols = std::max(1, c);
            m_map.dispRows = std::max(1, r);

            if (frame.bytes != m_lastPng) { // skip identical frames — no flicker
                renderGfx(frame.bytes);
                m_lastPng = frame.bytes;
            }
        }
    }

    m_connected = true;
    renderStatusBar();
}

void TerminalUi::onFrameFailed(const QString &reason)
{
    Q_UNUSED(reason)
    m_connected = false;
    renderStatusBar();
}

void TerminalUi::onStatus(const QString &text)
{
    m_status = text.toStdString();
    // A backend can report "connecting" before begin() has taken the terminal;
    // that text has to wait for the first frame rather than scribble on the
    // user's shell.
    if (m_started)
        renderStatusBar();
}

void TerminalUi::onLink(const QString &text)
{
    m_link = text.toStdString();
    if (m_started)
        renderStatusBar();
}

void TerminalUi::setBackendLabel(const QString &label)
{
    m_backendLabel = label.toStdString();
    if (m_started)
        renderStatusBar();
}

// ── Rendering ───────────────────────────────────────────────────────────────

void TerminalUi::feedUrlPrompt(char c)
{
    const auto u = static_cast<unsigned char>(c);

    if (c == 3 || c == 7) { // Ctrl-C / Ctrl-G — abandon the line, stay in the viewer
        m_urlPrompt = false;
        m_urlInput.clear();
        m_lastInput = "url cancelled";
    } else if (c == '\r' || c == '\n') {
        const std::string url =
            normalizeUserUrl(QString::fromStdString(m_urlInput)).toStdString();
        m_urlPrompt = false;
        m_urlInput.clear();
        if (url.empty()) {
            m_lastInput = "url cancelled";
        } else {
            m_backend->navigate(QString::fromStdString(url));
            m_lastInput = "navigate " + (url.size() > 32 ? url.substr(0, 32) + kEllipsis : url);
        }
    } else if (c == 127 || c == 8) { // DEL and BS both mean Backspace
        if (!m_urlInput.empty()) {
            // Step back over a whole UTF-8 character, not a byte: erasing one
            // byte of a multi-byte character leaves the row unprintable.
            size_t n = m_urlInput.size() - 1;
            while (n > 0 && (static_cast<unsigned char>(m_urlInput[n]) & 0xC0) == 0x80)
                --n;
            m_urlInput.resize(n);
        }
    } else if (c == 21) { // Ctrl-U — clear the line, as in a shell
        m_urlInput.clear();
    } else if (u >= 32 || u >= 0x80) {
        m_urlInput += c;
    }
    renderStatusBar();
}

void TerminalUi::renderUrlPrompt()
{
    static const char kPrompt[] = " URL: ";
    static const char kHint[] = "  enter=go  ctrl-c=cancel";

    // The caret is drawn rather than moved: the alt screen was entered with the
    // real cursor hidden, and showing it here would leave it visible on every
    // frame the renderer paints underneath.
    std::string line = std::string(kPrompt) + m_urlInput + "_";
    const auto cols = static_cast<size_t>(m_cols);
    if (line.size() + sizeof(kHint) - 1 <= cols)
        line += kHint;
    // A url longer than the row keeps its tail visible, because the tail is
    // what is being typed. Truncating the front is the only option that lets
    // someone see the character they just pressed.
    if (line.size() > cols)
        line = line.substr(line.size() - cols);

    std::string out = "\033[" + std::to_string(m_rows) + ";1H\033[7m" + line;
    out.append(cols - line.size(), ' ');
    out += "\033[0m";
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

void TerminalUi::switchToNextTab()
{
    const QStringList ids = m_backend->tabIds();
    if (ids.size() < 2) {
        // Said rather than swallowed: a binding that appears to do nothing is
        // indistinguishable from one that is broken.
        m_lastInput = ids.isEmpty() ? "no tabs here" : "only one tab";
        return;
    }

    int index = ids.indexOf(m_currentTab);
    if (index < 0)
        index = 0; // nothing selected yet: Ctrl-N moves off the first
    const int next = (index + 1) % ids.size();

    m_currentTab = ids.at(next);
    m_backend->setTab(m_currentTab);
    m_tabLabel = m_currentTab.toStdString() + "/" + std::to_string(ids.size());
    m_lastInput = "tab " + m_currentTab.toStdString();
    // The page under the viewer changed entirely, so the identical-frame skip
    // would otherwise hold the old tab on screen.
    m_lastPng.clear();
}

void TerminalUi::renderStatusBar()
{
    // The prompt outranks the setting: it is a reply the user is waiting to
    // see, and it borrows the row whether or not the bar lives there.
    if (m_urlPrompt) {
        renderUrlPrompt();
        return;
    }
    if (!m_statusVisible)
        return;

    std::string status = " anoa terminal ";
    if (!m_backendLabel.empty())
        status += m_backendLabel + " ";
    status += m_backend->description().toStdString() + " " + std::to_string(m_map.viewportW)
              + "x" + std::to_string(m_map.viewportH) + " ["
              + (m_gfx == GfxMode::Iterm    ? "iterm"
                 : m_gfx == GfxMode::Kitty  ? "kitty"
                                            : "halfblock")
              + "]";
    // Part of the header, not an optional field: with several tabs open, which
    // one is on screen is not something a user can infer from the picture. It
    // therefore sits ahead of the link, which yields below.
    if (!m_tabLabel.empty())
        status += " " + m_tabLabel;
    // Once the user has interacted, the last forwarded event replaces the
    // long usage hint so it survives narrow terminals. Built before the middle
    // field because it is what the middle field has to make room for.
    // The long form is the only place the shortcuts are discoverable from
    // inside the viewer, so it lists them; it is also the state that ends at
    // the first keystroke, which is why the row can afford it. On a narrow
    // terminal the truncation below eats the tail of the list rather than the
    // header, and README carries the full table either way.
    const std::string tail =
        m_lastInput.empty()
            ? " click/type drive the page | ctrl-l=url ctrl-r=reload ctrl-n=tab alt-arrows=history ctrl-c=quit"
            : " ctrl-c=quit | " + m_lastInput;

    // One slot, three sources, worst news first: a backend complaint (which on
    // the CDP path is also where "connecting"/"reconnecting" arrives) beats
    // naming a link that is up, and both beat the frame-level fallback.
    if (!m_status.empty()) {
        status += " " + m_status;
    } else if (!m_link.empty()) {
        // The link is the one optional field on this row. It is steady state —
        // "we are still attached to the thing the header already names" — and
        // the row is routinely over budget at 80 columns, so letting it push
        // the last-input echo off the end would cost the one signal that tells
        // a user their terminal is delivering input at all. It yields instead.
        if (status.size() + 1 + m_link.size() + tail.size() <= static_cast<size_t>(m_cols))
            status += " " + m_link;
    } else if (!m_connected) {
        status += std::string(" connection lost ") + kEmDash + " retrying" + kEllipsis;
    }
    status += tail;

    std::string out = "\033[" + std::to_string(m_rows) + ";1H\033[7m";
    if (static_cast<int>(status.size()) > m_cols)
        status.resize(static_cast<size_t>(m_cols));
    out += status;
    out.append(static_cast<size_t>(m_cols) - status.size(), ' ');
    out += "\033[0m";
    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// Halfblock: 1 cell = 1x2 pixels via the upper half block (fg = top px,
// bg = bottom px).
void TerminalUi::renderHalfblock(const FrameData &frame)
{
    const size_t need = static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3;
    const bool havePixels = frame.width > 0 && frame.height > 0
                            && static_cast<size_t>(frame.bytes.size()) >= need;
    const auto *rgb = reinterpret_cast<const unsigned char *>(frame.bytes.constData());
    const int frameW = havePixels ? frame.width : 0;
    const int frameH = havePixels ? frame.height : 0;

    std::string out;
    out.reserve(static_cast<size_t>(m_cols) * static_cast<size_t>(m_rows) * 24);
    out += "\033[H";

    const int cellRows = pageRows(); // one row less when the status bar is shown
    int lastFr = -1, lastFg = -1, lastFb = -1;
    int lastBr = -1, lastBg = -1, lastBb = -1;

    for (int cy = 0; cy < cellRows; ++cy) {
        for (int cx = 0; cx < m_cols; ++cx) {
            const int topY = cy * 2, botY = cy * 2 + 1;
            int tr = 0, tg = 0, tb = 0, br = 0, bg = 0, bb = 0;
            if (cx < frameW && topY < frameH) {
                const unsigned char *p = &rgb[(static_cast<size_t>(topY) * frameW + cx) * 3];
                tr = p[0]; tg = p[1]; tb = p[2];
            }
            if (cx < frameW && botY < frameH) {
                const unsigned char *p = &rgb[(static_cast<size_t>(botY) * frameW + cx) * 3];
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
            out += "\xE2\x96\x80"; // U+2580 UPPER HALF BLOCK
        }
        out += "\033[0m\r\n";
        lastFr = lastFg = lastFb = lastBr = lastBg = lastBb = -1;
    }

    fwrite(out.data(), 1, out.size(), stdout);
    fflush(stdout);
}

// Gfx: full-resolution PNG via terminal graphics protocol, drawn into an
// aspect-correct dispCols x dispRows cell rectangle at the top-left.
void TerminalUi::renderGfx(const QByteArray &png)
{
    const std::string b64 = base64Encode(png.constData(), static_cast<size_t>(png.size()));
    const int dispCols = m_map.dispCols;
    const int dispRows = m_map.dispRows;
    std::string out = "\033[H";

    if (m_gfx == GfxMode::Iterm) {
        // preserveAspectRatio=0: we already sized the cell rect to the image
        // aspect, so let the terminal fill it exactly.
        out += "\033]1337;File=inline=1;size=" + std::to_string(png.size())
               + ";width=" + std::to_string(dispCols)
               + ";height=" + std::to_string(dispRows)
               + ";preserveAspectRatio=0:" + b64 + "\007";
    } else { // Kitty
        // Same image id (i=1) + placement id (p=1) each frame -> the terminal
        // replaces the previous frame atomically, no delete needed. Payload
        // must be chunked to <=4096 bytes; only the first chunk carries keys.
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

bool TerminalUi::feedInput(const char *data, size_t len)
{
    if (len > 0)
        m_inputBuf.append(data, len);
    const bool alive = processInput(m_inputBuf);
    renderStatusBar();
    return alive;
}

// Consume every complete escape sequence / key in `buf`; returns false on
// quit. m_lastInput receives a short description of the last event forwarded,
// shown in the status bar so users can verify their terminal delivers input.
bool TerminalUi::processInput(std::string &buf)
{
    // Batch consecutive printable bytes into one sendText() call so pasted
    // text is not sent one request per character. UTF-8 continuation bytes
    // (>= 0x80) pass straight through.
    std::string pending;
    auto flushPending = [&]() {
        if (pending.empty())
            return;
        m_backend->sendText(QByteArray(pending.data(), static_cast<int>(pending.size())));
        m_lastInput = "typed \""
                      + (pending.size() > 12 ? pending.substr(0, 12) + kEllipsis : pending) + "\"";
        pending.clear();
    };

    size_t i = 0;
    while (i < buf.size()) {
        const char c = buf[i];
        const auto u = static_cast<unsigned char>(c);

        // The URL prompt owns the keyboard while it is open, so this runs
        // before the quit check: Ctrl-C cancels the prompt there rather than
        // ending the session, the way it abandons a half-typed shell line.
        if (m_urlPrompt) {
            if (c == '\033') {
                // Esc is deliberately *not* a cancel key. Telling a lone Esc
                // from the start of an arrow or mouse report needs a timeout,
                // and guessing is what made a split sequence type its letters
                // into the page. The prompt swallows whole sequences instead
                // and buffers a partial one, exactly like the main path.
                if (i + 1 >= buf.size())
                    break;
                if (buf[i + 1] != '[') {
                    i += 2;
                    continue;
                }
                size_t j = i + 2;
                while (j < buf.size()
                       && !(static_cast<unsigned char>(buf[j]) >= 0x40
                            && static_cast<unsigned char>(buf[j]) <= 0x7E))
                    ++j;
                if (j >= buf.size())
                    break; // final byte has not arrived yet
                i = j + 1;
                continue;
            }
            feedUrlPrompt(c);
            ++i;
            continue;
        }

        if (c == 12) { // Ctrl-L — open the URL prompt
            flushPending();
            m_urlPrompt = true;
            m_urlInput.clear();
            renderStatusBar();
            ++i;
            continue;
        }

        if (c == 18) { // Ctrl-R — reload
            flushPending();
            m_backend->reloadPage();
            m_lastInput = "reload";
            renderStatusBar();
            ++i;
            continue;
        }

        if (c == 14) { // Ctrl-N — next tab
            flushPending();
            switchToNextTab();
            renderStatusBar();
            ++i;
            continue;
        }

        if (c == 2) { // Ctrl-B — show or hide the status bar
            flushPending();
            m_statusVisible = !m_statusVisible;
            // The row changes owner either way: hiding it leaves the bar's
            // reverse-video text sitting on a row the page now wants, and
            // showing it costs the page a row it had. Both need the frame
            // redrawn rather than the cheap skip an identical PNG gets.
            m_lastPng.clear();
            fputs("\033[2J", stdout);
            renderStatusBar();
            ++i;
            continue;
        }

        if (c == 3 || c == 17) { // Ctrl-C / Ctrl-Q (ISIG is off)
            flushPending();
            // Consume the quit byte too. anoa-term left it in the buffer and
            // broke out of the loop immediately; the buffer outlives the call
            // here, so a stale Ctrl-C would re-trigger on every later read.
            buf.erase(0, i + 1);
            return false;
        }

        if (c != '\033') {
            if (c == '\r' || c == '\n') {
                flushPending();
                m_backend->sendKey(QStringLiteral("enter"));
                m_lastInput = "key enter";
            } else if (c == 127 || c == 8) { // DEL and BS both mean Backspace
                flushPending();
                m_backend->sendKey(QStringLiteral("backspace"));
                m_lastInput = "key backspace";
            } else if (c == '\t') {
                flushPending();
                m_backend->sendKey(QStringLiteral("tab"));
                m_lastInput = "key tab";
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
        // A buffer ending on exactly "ESC [" is a sequence whose body has not
        // arrived yet, not one to resync past. Falling through to the i += 2
        // at the bottom dropped both bytes and left the rest to be read as
        // ordinary input on the next pass, so an up arrow split here typed a
        // literal "A" into the page and a split mouse report leaked
        // "<0;12;7M" as text. The SGR branch below already breaks the same way
        // for its own longer partial; this is the two-byte tail it cannot see.
        if (i + 2 >= buf.size())
            break;

        // Modified arrows arrive as ESC [ 1 ; <mod> <A-D>, so they reach here
        // with '1' where a plain arrow has its letter. Alt-Left / Alt-Right are
        // the viewer's history keys — the pair every browser binds — and are
        // consumed rather than forwarded.
        //
        // The prefix is matched byte by byte against what has actually arrived,
        // because "wait for six bytes" would stall the buffer on every other
        // sequence that starts with '1' (Home is ESC [ 1 ~, four bytes): those
        // must fall through to the resync below, not block input until the user
        // happens to press something else.
        if (buf[i + 2] == '1') {
            static const char kAltPrefix[] = {'1', ';', '3'};
            size_t k = 0;
            while (k < sizeof(kAltPrefix) && i + 2 + k < buf.size()
                   && buf[i + 2 + k] == kAltPrefix[k])
                ++k;
            if (k < sizeof(kAltPrefix)) {
                if (i + 2 + k >= buf.size())
                    break; // everything so far matches; the rest may still come
                // A real mismatch: some other CSI. Fall through to the resync.
            } else if (i + 5 >= buf.size()) {
                break; // final letter has not arrived yet
            } else if (buf[i + 5] == 'C' || buf[i + 5] == 'D') {
                if (buf[i + 5] == 'D') {
                    m_backend->goBack();
                    m_lastInput = "back";
                } else {
                    m_backend->goForward();
                    m_lastInput = "forward";
                }
                renderStatusBar();
                i += 6;
                continue;
            }
        }

        // Arrow keys → forwarded as key events. In a focused text field they
        // move the caret; otherwise Chromium scrolls the page — both natural.
        if (buf[i + 2] >= 'A' && buf[i + 2] <= 'D') {
            static const char *arrows[] = {"up", "down", "right", "left"};
            const char *name = arrows[buf[i + 2] - 'A'];
            m_backend->sendKey(QString::fromLatin1(name));
            m_lastInput = std::string("key ") + name;
            i += 3;
            continue;
        }

        // SGR mouse: ESC [ < btn ; col ; row (M=press, m=release), 1-based.
        if (buf[i + 2] == '<') {
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
                    const bool inPage = mapCellToPage(m_map, cellX, cellY, pageX, pageY);
                    if (btn >= 0 && btn <= 2) {
                        static const MouseButton buttons[] = {MouseButton::Left,
                                                              MouseButton::Middle,
                                                              MouseButton::Right};
                        if (inPage) {
                            m_backend->sendClick(pageX, pageY, buttons[btn % 3]);
                            m_lastInput = "click " + std::to_string(pageX) + ","
                                          + std::to_string(pageY);
                        } else {
                            m_lastInput = "click outside page";
                        }
                    } else if (btn == 64) {
                        // Wheel events keep their coordinates when the pointer
                        // is over the page and lose them (-1) when it is not,
                        // exactly as the old query-string builder did.
                        m_backend->sendScroll(inPage ? pageX : -1, inPage ? pageY : -1, 120);
                        m_lastInput = "scroll up";
                    } else if (btn == 65) {
                        m_backend->sendScroll(inPage ? pageX : -1, inPage ? pageY : -1, -120);
                        m_lastInput = "scroll down";
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
