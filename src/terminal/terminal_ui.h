#pragma once

// The presentation and input half of the terminal viewer: raw-mode termios,
// the SIGWINCH/SIGINT flags, the halfblock and terminal-image renderers, the
// cell-to-page coordinate map and the input parser. Ported from
// tools/anoa-term/anoa_term.cpp; the transport it used to talk to lives behind
// FrameBackend now.
//
// POSIX only — termios, TIOCGWINSZ and SIGWINCH have no MSVC equivalent, so
// this header is never included on Windows (see CMakeLists.txt).

#include <cstddef>
#include <string>

#include <QByteArray>
#include <QObject>
#include <QString>

#include "terminal/frame_backend.h"

struct Config;

// Which renderer draws the page. Auto is resolved to one of the other three
// by sniffing the terminal at startup.
enum class GfxMode { Auto, Halfblock, Iterm, Kitty };

// Parses the --gfx spelling used by Config. Unknown values fall back to Auto;
// config.cpp already rejects them before we get here.
GfxMode gfxModeFromString(const QString &name);

// Extent of the displayed page image, in terminal cells, plus the browser's
// logical viewport size. Valid for both renderers: halfblock shows 1 px per
// column and 2 px per row; gfx stretches the PNG over dispCols x dispRows.
struct DisplayMap {
    int dispCols = 0;
    int dispRows = 0;
    int viewportW = 0;
    int viewportH = 0;
};

bool mapCellToPage(const DisplayMap &map, int cellX, int cellY, int &pageX, int &pageY);

// Process-wide terminal state. These wrap the parts that cannot belong to an
// object: the saved termios and the signal handler flags.
namespace term {

bool enterRawMode();
void restoreTerminal();
void installSignalHandlers();

// Signal flags, polled from the frame tick — the handlers themselves only
// touch a volatile sig_atomic_t.
bool quitSignalled();
bool takeResized(); // true once per SIGWINCH, then clears the flag

void terminalSize(int &cols, int &rows);
void queryCellSize(int &cellW, int &cellH);
GfxMode detectGfx();

} // namespace term

class TerminalUi : public QObject
{
    Q_OBJECT

public:
    TerminalUi(const Config &config, FrameBackend *backend, QObject *parent = nullptr);
    ~TerminalUi() override;

    // Resolve the gfx mode, enter raw mode, install the signal handlers and —
    // for the image protocols only — ask the terminal for its cell size.
    // False means raw mode failed and nothing has been drawn.
    bool begin();
    // Leave the alt screen and restore the terminal. Idempotent.
    void end();

    GfxMode gfxMode() const { return m_gfx; }
    int framePeriodMs() const { return 1000 / m_fps; }

    // One frame tick: service the pending signals, then ask the backend for
    // the frame kind the active renderer needs. False means a signal asked the
    // process to quit.
    bool tick();

    // Bytes read from stdin, in whatever chunks arrive — a partial escape
    // sequence is buffered until the rest turns up. False means the user asked
    // to quit (Ctrl-C / Ctrl-Q).
    bool feedInput(const char *data, size_t len);

public slots:
    void onFrame(const FrameData &frame);
    void onFrameFailed(const QString &reason);
    void onStatus(const QString &text);

private:
    void renderStatusBar();
    void renderHalfblock(const FrameData &frame);
    void renderGfx(const QByteArray &png);
    bool processInput(std::string &buf);

    FrameBackend *m_backend = nullptr;
    GfxMode m_gfx = GfxMode::Auto;
    int m_fps = 30;
    int m_cols = 80;
    int m_rows = 24;
    int m_cellW = 8;
    int m_cellH = 16;
    bool m_started = false;
    bool m_connected = false;
    DisplayMap m_map;
    QByteArray m_lastPng;  // last frame sent to a gfx terminal, to skip repeats
    std::string m_inputBuf;
    std::string m_lastInput; // last event forwarded, shown in the status bar
    std::string m_status;    // connection state reported by the backend
};
