// TERM-INPUT-01..08 and TERM-GFX-01..03 — TerminalUi's input parser, cell map
// and the two image-protocol renderers.
//
// This suite exists because the pty-driven integration suites cannot reach any
// of it. `script(1)` always resolves the viewer to the halfblock renderer, so
// renderGfx() and the hand-rolled base64 under it run in no test there; and a
// pty delivers a keypress as one write, so the split-escape-sequence cases
// (bug-002) are unreachable by construction. Both are reachable here, because
// feedInput() is public and takes the chunk boundary as an argument.
//
// Two things make driving TerminalUi outside a terminal safe:
//   * begin() is never called. It is what enters raw mode and takes the alt
//     screen, and it fails on a non-tty anyway. Skipping it leaves m_cols/
//     m_rows at their 80x24 defaults, which is what makes the status-bar
//     arithmetic below a constant rather than a property of the CI runner.
//   * stdout is redirected to a temp file for the lifetime of each test. The
//     renderers write escape sequences unconditionally, and CTest's log is
//     also stdout — without this the two interleave and the log is unreadable.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QList>
#include <QSize>
#include <QString>
#include <QtTest/QtTest>

#include "config/config.h"
#include "terminal/frame_backend.h"
#include "terminal/terminal_ui.h"

namespace {

// ── A backend that answers nothing and records everything ───────────────────
//
// The seam is asynchronous, so a stub that never emits frameReady() is a valid
// backend: the UI simply never gets a frame it did not ask for. Frames are
// injected by calling onFrame() directly instead, which is also the only way
// to make the display map a known quantity.
class RecordingBackend : public FrameBackend
{
public:
    struct Click {
        int x;
        int y;
        MouseButton button;
    };
    struct Scroll {
        int x;
        int y;
        int dy;
    };

    explicit RecordingBackend(QString description = QStringLiteral("127.0.0.1:9222"))
        : m_description(std::move(description))
    {
    }

    void requestRgbFrame(int width, int height) override { rgbRequests.append(QSize(width, height)); }
    void requestPngFrame() override { ++pngRequests; }
    void sendClick(int pageX, int pageY, MouseButton button) override
    {
        clicks.append({pageX, pageY, button});
    }
    void sendScroll(int pageX, int pageY, int dy) override { scrolls.append({pageX, pageY, dy}); }
    void sendText(const QByteArray &utf8) override { texts.append(utf8); }
    void sendKey(const QString &namedKey) override { keys.append(namedKey); }
    // Navigation is recorded as one ordered list rather than four counters:
    // several of the tests below care that a key produced *only* the action it
    // names, and an ordered log is the only shape that can show that.
    void navigate(const QString &url) override { navActions.append(QStringLiteral("navigate ") + url); }
    void goBack() override { navActions.append(QStringLiteral("back")); }
    void goForward() override { navActions.append(QStringLiteral("forward")); }
    void reloadPage() override { navActions.append(QStringLiteral("reload")); }
    void resizeViewport(int width, int height) override
    {
        resizes.append(QSize(width, height));
    }
    QString description() const override { return m_description; }

    QList<QSize> rgbRequests;
    int pngRequests = 0;
    QList<Click> clicks;
    QList<Scroll> scrolls;
    QList<QByteArray> texts;
    QList<QString> keys;
    QList<QString> navActions;
    QList<QSize> resizes;

    // Tab support, off unless a case turns it on: a backend with no tabs is
    // the shape the CDP and in-process paths keep.
    QStringList tabs;
    QList<QString> tabSelections;
    QStringList tabIds() override { return tabs; }
    void setTab(const QString &tabId) override { tabSelections.append(tabId); }

private:
    QString m_description;
};

// ── stdout capture ──────────────────────────────────────────────────────────

// Redirects fd 1 to a temp file for its lifetime. take() returns everything
// written since the previous take(); reads go through pread() so they do not
// move the write offset fd 1 shares with the file.
//
// QTest's own logger writes to stdout too, so a failing QVERIFY inside the
// capture would otherwise report into the temp file and vanish. The destructor
// replays the whole capture to stderr when the test failed — escape bytes
// blanked out, because the replay includes a screen's worth of raw terminal
// paint and CTest's log is not a terminal.
class CapturedStdout
{
public:
    CapturedStdout()
    {
        fflush(stdout);
        m_saved = dup(STDOUT_FILENO);
        m_file = tmpfile();
        if (m_file)
            dup2(fileno(m_file), STDOUT_FILENO);
    }
    ~CapturedStdout()
    {
        fflush(stdout);
        const bool failed = QTest::currentTestFailed();
        QByteArray replay;
        if (failed && m_file) {
            m_read = 0;
            replay = take();
        }
        if (m_saved >= 0) {
            dup2(m_saved, STDOUT_FILENO);
            close(m_saved);
        }
        if (m_file)
            fclose(m_file);
        if (failed) {
            for (char &c : replay) {
                if ((c >= 0 && c < 0x20 && c != '\n') || c == 0x7F)
                    c = '.';
            }
            fprintf(stderr, "--- captured stdout ---\n%s\n--- end ---\n", replay.constData());
        }
    }
    CapturedStdout(const CapturedStdout &) = delete;
    CapturedStdout &operator=(const CapturedStdout &) = delete;

    QByteArray take()
    {
        fflush(stdout);
        if (!m_file)
            return {};
        const int fd = fileno(m_file);
        const off_t end = lseek(fd, 0, SEEK_END);
        if (end <= m_read)
            return {};
        QByteArray out(static_cast<qsizetype>(end - m_read), '\0');
        const ssize_t n = pread(fd, out.data(), static_cast<size_t>(out.size()), m_read);
        m_read = end;
        out.resize(n > 0 ? static_cast<qsizetype>(n) : 0);
        return out;
    }

private:
    int m_saved = -1;
    FILE *m_file = nullptr;
    off_t m_read = 0;
};

// ── stdin redirection ───────────────────────────────────────────────────────

// Replaces fd 0 with a temp file holding `contents` for its lifetime.
//
// The term:: helpers read stdin directly, and a regular file is the one input
// that makes them deterministic: select() always reports a regular file ready,
// and read() returns the bytes and then 0 at EOF. An inherited tty would make
// the result a property of the runner, and a pipe would leave select() blocking
// for the full 500 ms budget. It also guarantees the non-tty that
// enterRawMode()'s tcgetattr() failure path needs, rather than assuming CTest
// hands the test one.
class RedirectedStdin
{
public:
    explicit RedirectedStdin(const QByteArray &contents = QByteArray())
    {
        m_saved = dup(STDIN_FILENO);
        m_file = tmpfile();
        if (!m_file)
            return;
        if (!contents.isEmpty())
            fwrite(contents.constData(), 1, static_cast<size_t>(contents.size()), m_file);
        fflush(m_file);
        rewind(m_file);
        dup2(fileno(m_file), STDIN_FILENO);
    }
    ~RedirectedStdin()
    {
        if (m_saved >= 0) {
            dup2(m_saved, STDIN_FILENO);
            close(m_saved);
        }
        if (m_file)
            fclose(m_file);
    }
    RedirectedStdin(const RedirectedStdin &) = delete;
    RedirectedStdin &operator=(const RedirectedStdin &) = delete;

private:
    int m_saved = -1;
    FILE *m_file = nullptr;
};

// ── gfx environment ─────────────────────────────────────────────────────────

// Clears the three variables detectGfx() reads and puts the originals back
// afterwards. Without the restore the matrix below would leak a fabricated
// TERM into every test that runs after it.
class ScopedGfxEnv
{
public:
    ScopedGfxEnv()
        : m_term(qgetenv("TERM"))
        , m_prog(qgetenv("TERM_PROGRAM"))
        , m_kitty(qgetenv("KITTY_WINDOW_ID"))
        , m_hadTerm(qEnvironmentVariableIsSet("TERM"))
        , m_hadProg(qEnvironmentVariableIsSet("TERM_PROGRAM"))
        , m_hadKitty(qEnvironmentVariableIsSet("KITTY_WINDOW_ID"))
    {
        clear();
    }
    ~ScopedGfxEnv()
    {
        restore("TERM", m_hadTerm, m_term);
        restore("TERM_PROGRAM", m_hadProg, m_prog);
        restore("KITTY_WINDOW_ID", m_hadKitty, m_kitty);
    }
    ScopedGfxEnv(const ScopedGfxEnv &) = delete;
    ScopedGfxEnv &operator=(const ScopedGfxEnv &) = delete;

    void clear()
    {
        qunsetenv("TERM");
        qunsetenv("TERM_PROGRAM");
        qunsetenv("KITTY_WINDOW_ID");
    }

private:
    static void restore(const char *name, bool had, const QByteArray &value)
    {
        if (had)
            qputenv(name, value);
        else
            qunsetenv(name);
    }

    QByteArray m_term;
    QByteArray m_prog;
    QByteArray m_kitty;
    bool m_hadTerm;
    bool m_hadProg;
    bool m_hadKitty;
};

Config terminalConfig(const char *gfx)
{
    Config cfg;
    cfg.terminalMode = true;
    cfg.gfxMode = QString::fromLatin1(gfx);
    cfg.fps = 10;
    return cfg;
}

// A halfblock frame with a body the renderer will actually walk: the display
// map it produces is dispCols = width, dispRows = (height + 1) / 2.
FrameData rgbFrame(int width, int height, int viewportW, int viewportH)
{
    FrameData frame;
    frame.width = width;
    frame.height = height;
    frame.viewportW = viewportW;
    frame.viewportH = viewportH;
    frame.bytes = QByteArray(static_cast<qsizetype>(width) * height * 3, '\x40');
    return frame;
}

// The last status row in a captured paint: renderStatusBar() writes
// ESC[<rows>;1H ESC[7m <row> ESC[0m and nothing else touches that cursor
// position.
QByteArray statusRow(const QByteArray &painted)
{
    const QByteArray head("\x1b[24;1H\x1b[7m");
    const qsizetype at = painted.lastIndexOf(head);
    if (at < 0)
        return {};
    const qsizetype from = at + head.size();
    const qsizetype end = painted.indexOf(QByteArray("\x1b[0m"), from);
    return end < 0 ? painted.mid(from) : painted.mid(from, end - from);
}

// SGR mouse press, as a terminal writes it: 1-based coordinates.
QByteArray sgrPress(int btn, int cellX, int cellY)
{
    return QByteArray("\x1b[<") + QByteArray::number(btn) + ';' + QByteArray::number(cellX + 1)
           + ';' + QByteArray::number(cellY + 1) + 'M';
}

} // namespace

class TestTerminalUi : public QObject
{
    Q_OBJECT

private:
    // Feed a byte string as one read.
    static bool feed(TerminalUi &ui, const QByteArray &bytes)
    {
        return ui.feedInput(bytes.constData(), static_cast<size_t>(bytes.size()));
    }

    // Ctrl-B. The bar is shown by default, so this hides it on the first call
    // and brings it back on the next.
    static void toggleStatusBar(TerminalUi &ui)
    {
        feed(ui, QByteArray("\x02"));
    }

private slots:
    // ── Viewer shortcuts and the URL prompt ─────────────────────────────────

    // TERM-NAV-01: Ctrl-R and Alt-Left/Right drive history, and nothing else.
    // The "nothing else" half is the point: they used to reach the page, so a
    // regression here is silent — the page just receives keystrokes again.
    void testHistoryShortcuts()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray("\x12")));           // Ctrl-R
        QVERIFY(feed(ui, QByteArray("\x1b[1;3D")));      // Alt-Left
        QVERIFY(feed(ui, QByteArray("\x1b[1;3C")));      // Alt-Right

        QCOMPARE(backend.navActions,
                 QList<QString>({QStringLiteral("reload"), QStringLiteral("back"),
                                 QStringLiteral("forward")}));
        QVERIFY2(backend.texts.isEmpty(), "a shortcut leaked into the page as text");
        QVERIFY2(backend.keys.isEmpty(), "a shortcut leaked into the page as a key event");
    }

    // TERM-NAV-02: the Alt-arrow prefix arriving in pieces. Same hazard as
    // bug-002 — the half-matched sequence must be held, not resynced past and
    // typed into the page.
    void testAltArrowSplitAcrossReads()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray("\x1b[1")));   // nothing decidable yet
        QVERIFY(backend.navActions.isEmpty());
        QVERIFY(feed(ui, QByteArray(";3")));       // still only a prefix
        QVERIFY(backend.navActions.isEmpty());
        QVERIFY(feed(ui, QByteArray("D")));        // now it is Alt-Left

        QCOMPARE(backend.navActions, QList<QString>({QStringLiteral("back")}));
        QVERIFY2(backend.texts.isEmpty(), "part of the sequence was typed into the page");
    }

    // TERM-NAV-03: a CSI that merely starts like Alt-arrow must not stall the
    // buffer. Home is ESC [ 1 ~ and is complete in four bytes; the Alt-arrow
    // matcher must not sit waiting for a fifth and freeze input until the user
    // presses something else.
    //
    // What it resyncs *to* is the pre-existing behaviour TERM-INPUT-09 pins for
    // every unknown CSI: "ESC [" is dropped and the body is typed into the page,
    // so this expects "1~x" rather than "x". That is a defect of its own — it is
    // how ESC [ H already behaves — and it is deliberately not changed here, so
    // this case is asserted as it is rather than as it should be.
    void testUnknownCsiStartingWithOneDoesNotStall()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray("\x1b[1~x")));
        QVERIFY2(backend.navActions.isEmpty(), "Home was mistaken for a history key");
        // The trailing "x" is what proves the parser moved on rather than
        // holding the buffer for a sixth byte that was never coming.
        QCOMPARE(backend.texts, QList<QByteArray>({QByteArray("1~x")}));
    }

    // TERM-URL-01: Ctrl-L opens the prompt, typing goes to it instead of the
    // page, and Enter navigates with the scheme filled in.
    void testUrlPromptNavigates()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray("\x0c")));            // Ctrl-L
        QVERIFY(feed(ui, QByteArray("example.com")));
        QVERIFY2(backend.texts.isEmpty(), "the prompt let keystrokes through to the page");
        QVERIFY(feed(ui, QByteArray("\r")));

        QCOMPARE(backend.navActions,
                 QList<QString>({QStringLiteral("navigate https://example.com")}));
        QVERIFY2(backend.keys.isEmpty(), "Enter reached the page as a key event");
    }

    // TERM-URL-02: Ctrl-C inside the prompt abandons the line and stays in the
    // viewer; outside it, it still quits. Getting this backwards would either
    // trap the user in the prompt or drop the session on a typo.
    void testUrlPromptCancelDoesNotQuit()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray("\x0c")));
        QVERIFY(feed(ui, QByteArray("example.com")));
        QVERIFY2(feed(ui, QByteArray("\x03")), "Ctrl-C in the prompt ended the session");
        QVERIFY(backend.navActions.isEmpty());

        // Prompt is closed, so the next Ctrl-C is a quit again.
        QVERIFY(!feed(ui, QByteArray("\x03")));
    }

    // TERM-URL-03: backspace steps over a whole UTF-8 character. Erasing one
    // byte of a multi-byte character leaves the status row unprintable.
    void testUrlPromptBackspaceIsCharacterWise()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray("\x0c")));
        QVERIFY(feed(ui, QByteArray("a\xC3\xA9")));  // "aé"
        QVERIFY(feed(ui, QByteArray("\x7f")));       // one Backspace
        QVERIFY(feed(ui, QByteArray("\r")));

        QCOMPARE(backend.navActions, QList<QString>({QStringLiteral("navigate https://a")}));
    }

    // TERM-URL-04: an address that already names a scheme is left alone, and a
    // host:port is not mistaken for one.
    void testUrlPromptSchemeHandling()
    {
        for (const auto &pair : QList<QPair<QByteArray, QString>>{
                 {"http://x.test", QStringLiteral("navigate http://x.test")},
                 {"about:blank", QStringLiteral("navigate about:blank")},
                 {"localhost:8080", QStringLiteral("navigate https://localhost:8080")}}) {
            CapturedStdout capture;
            RecordingBackend backend;
            TerminalUi ui(terminalConfig("halfblock"), &backend);
    
            QVERIFY(feed(ui, QByteArray("\x0c")));
            QVERIFY(feed(ui, pair.first));
            QVERIFY(feed(ui, QByteArray("\r")));
            QCOMPARE(backend.navActions, QList<QString>({pair.second}));
        }
    }

    // TERM-BAR-01: the status bar is on by default, and Ctrl-B toggles it.
    // Hidden means *nothing drawn on that row* — leaving a blanked row behind
    // would spend the row without showing anything.
    void testStatusBarStartsVisibleAndToggles()
    {
        CapturedStdout capture;
        RecordingBackend backend(QStringLiteral("h:1"));
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 40, 20));
        capture.take();

        QVERIFY(feed(ui, QByteArray()));
        QVERIFY2(statusRow(capture.take()).contains("anoa terminal"),
                 "the bar was missing on a fresh viewer");

        toggleStatusBar(ui); // away
        capture.take();
        QVERIFY(feed(ui, QByteArray()));
        QVERIFY2(statusRow(capture.take()).isEmpty(), "Ctrl-B did not hide the bar");

        toggleStatusBar(ui); // and back
        QVERIFY(feed(ui, QByteArray()));
        QVERIFY(statusRow(capture.take()).contains("anoa terminal"));
    }

    // TERM-BAR-02: the URL prompt outranks the setting. It is a reply the user
    // is waiting on, so it takes the row even with the bar switched off —
    // otherwise Ctrl-L after a Ctrl-B types into something invisible.
    void testUrlPromptDrawsEvenWhenTheBarIsHidden()
    {
        CapturedStdout capture;
        RecordingBackend backend(QStringLiteral("h:1"));
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 40, 20));
        toggleStatusBar(ui); // hide it first — that is the case at issue
        capture.take();

        QVERIFY(feed(ui, QByteArray("\x0c")));
        QVERIFY(feed(ui, QByteArray("abc")));
        QVERIFY2(statusRow(capture.take()).contains("URL: abc"), "the prompt was invisible");
    }

    // TERM-BAR-03: in an image-protocol mode the viewer asks the page to take
    // the terminal's shape, so the aspect fit has nothing left to letterbox.
    // The width is pinned and only the height follows the terminal — deriving
    // both from cell counts would hand the page a few hundred CSS pixels and
    // trip every mobile breakpoint on the web.
    //
    // Halfblock asks for exactly its grid and so never letterboxes; it must not
    // resize the page at all.
    void testGfxModeAsksThePageToMatchTheTerminalShape()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi gfx(terminalConfig("kitty"), &backend);
        QVERIFY(gfx.tick());

        QCOMPARE(backend.resizes.size(), 1);
        // 80x24 cells at the 8x16 cell default, less the status bar's row:
        // 640x368 px, so the page is asked for 1280 x round(1280 * 368/640).
        QCOMPARE(backend.resizes[0], QSize(1280, 736));

        RecordingBackend halfblock;
        TerminalUi hb(terminalConfig("halfblock"), &halfblock);
        QVERIFY(hb.tick());
        QVERIFY2(halfblock.resizes.isEmpty(), "halfblock resized a page it fits exactly");
    }

    // ── The input parser ────────────────────────────────────────────────────

    // TERM-INPUT-01: bug-002. An up arrow is three bytes; a terminal is free to
    // deliver them in any grouping, and read() splitting after "ESC [" is the
    // one grouping that used to lose the sequence — the two bytes were dropped
    // and the 'A' that followed was typed into the page as a letter.
    void testArrowSplitAfterCsiIntroducer()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "\x1b["));
        QVERIFY(feed(ui, "A"));

        QCOMPARE(backend.keys, QList<QString>{QStringLiteral("up")});
        QVERIFY2(backend.texts.isEmpty(), "the arrow's final byte was typed into the page");
    }

    // TERM-INPUT-02: the one-byte tail. Documented as already working; pinned
    // so the fix for the two-byte tail cannot regress it.
    void testArrowSplitAfterEscape()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "\x1b"));
        QVERIFY(feed(ui, "[A"));

        QCOMPARE(backend.keys, QList<QString>{QStringLiteral("up")});
        QVERIFY(backend.texts.isEmpty());
    }

    // All four arrows in one read, to pin the ESC[A..D order against the
    // up/down/right/left table it indexes.
    void testArrowNames()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "\x1b[A\x1b[B\x1b[C\x1b[D"));

        const QList<QString> expected{QStringLiteral("up"), QStringLiteral("down"),
                                      QStringLiteral("right"), QStringLiteral("left")};
        QCOMPARE(backend.keys, expected);
    }

    // TERM-INPUT-03: bug-002's second symptom. A mouse report split the same
    // way leaked "<0;12;7M" into the page as typed text.
    void testMouseReportSplitAfterCsiIntroducer()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 400, 200)); // dispCols 40, dispRows 10

        QVERIFY(feed(ui, "\x1b["));
        QVERIFY(feed(ui, "<0;12;7M"));

        QCOMPARE(backend.clicks.size(), 1);
        // mapCellToPage: ((12-1)+0.5)*400/40 = 115, ((7-1)+0.5)*200/10 = 130.
        QCOMPARE(backend.clicks[0].x, 115);
        QCOMPARE(backend.clicks[0].y, 130);
        QVERIFY2(backend.texts.isEmpty(), "the mouse report leaked into the page as text");
    }

    // The longer partial split, which the SGR branch has always handled: it
    // breaks out and waits rather than resyncing. Kept alongside the case
    // above so a fix to one is visibly not a fix to the other.
    void testMouseReportSplitBeforeFinalByte()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 400, 200));

        QVERIFY(feed(ui, "\x1b[<0;12;7"));
        QCOMPARE(backend.clicks.size(), 0);
        QVERIFY(feed(ui, "M"));

        QCOMPARE(backend.clicks.size(), 1);
        QCOMPARE(backend.clicks[0].x, 115);
        QCOMPARE(backend.clicks[0].y, 130);
        QVERIFY(backend.texts.isEmpty());
    }

    // A wheel notch keeps the Qt angleDelta convention on the seam: +120 for
    // up. cdp_frame_backend.cpp is the only place that inverts it, and
    // terminal_cdp.test.js asserts the inverted half.
    void testWheelKeepsAngleDeltaConvention()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 400, 200));

        QVERIFY(feed(ui, sgrPress(64, 11, 6)));
        QVERIFY(feed(ui, sgrPress(65, 11, 6)));

        QCOMPARE(backend.scrolls.size(), 2);
        QCOMPARE(backend.scrolls[0].dy, 120);
        QCOMPARE(backend.scrolls[1].dy, -120);
        QCOMPARE(backend.scrolls[0].x, 115);
        QCOMPARE(backend.scrolls[0].y, 130);
    }

    // A wheel notch outside the page image keeps the event and drops the
    // coordinates, which is what the /render/scroll query builder did.
    void testWheelOutsidePageLosesCoordinates()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 400, 200));

        QVERIFY(feed(ui, sgrPress(64, 60, 6))); // column 60 > dispCols 40

        QCOMPARE(backend.scrolls.size(), 1);
        QCOMPARE(backend.scrolls[0].x, -1);
        QCOMPARE(backend.scrolls[0].y, -1);
        QCOMPARE(backend.scrolls[0].dy, 120);
    }

    // A click outside the page image is dropped entirely — there is no page
    // coordinate to send it to.
    void testClickOutsidePageIsDropped()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 400, 200));

        QVERIFY(feed(ui, sgrPress(0, 60, 6)));
        QCOMPARE(backend.clicks.size(), 0);
    }

    // TERM-INPUT-04: a burst is one sendText, not one per character. This is
    // what keeps a paste from turning into N requests.
    void testTypedBurstIsBatched()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "abc"));

        QCOMPARE(backend.texts.size(), 1);
        QCOMPARE(backend.texts[0], QByteArray("abc"));
        QVERIFY(backend.keys.isEmpty());
    }

    // A named key inside a burst flushes the text before it and starts a new
    // batch after it, so the page sees them in the order they were typed.
    void testNamedKeySplitsTheBatch()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "ab\rcd"));

        QCOMPARE(backend.texts.size(), 2);
        QCOMPARE(backend.texts[0], QByteArray("ab"));
        QCOMPARE(backend.texts[1], QByteArray("cd"));
        QCOMPARE(backend.keys, QList<QString>{QStringLiteral("enter")});
    }

    // Multibyte UTF-8 crosses the seam as bytes, not as a QString: a burst may
    // end mid-character and the tail has to survive unmangled.
    void testUtf8PassesThroughAsBytes()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        const QByteArray euro("\xE2\x82\xAC"); // U+20AC
        QVERIFY(feed(ui, euro));

        QCOMPARE(backend.texts.size(), 1);
        QCOMPARE(backend.texts[0], euro);
    }

    // TERM-INPUT-05: terminals send DEL (127) for the Backspace key and BS (8)
    // for Ctrl-H; both mean the same thing to a page.
    void testBackspaceFromDelAndBs()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, QByteArray(1, '\x7f')));
        QVERIFY(feed(ui, QByteArray(1, '\x08')));

        const QList<QString> expected{QStringLiteral("backspace"), QStringLiteral("backspace")};
        QCOMPARE(backend.keys, expected);
        QVERIFY(backend.texts.isEmpty());
    }

    void testEnterAndTab()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "\r\n\t"));

        const QList<QString> expected{QStringLiteral("enter"), QStringLiteral("enter"),
                                      QStringLiteral("tab")};
        QCOMPARE(backend.keys, expected);
    }

    // TERM-INPUT-06: ISIG is off in raw mode, so Ctrl-C and Ctrl-Q arrive as
    // bytes 3 and 17 and quitting is this return value, not a signal.
    void testCtrlCAndCtrlQQuit()
    {
        CapturedStdout capture;
        RecordingBackend backendC;
        TerminalUi uiC(terminalConfig("halfblock"), &backendC);
        QVERIFY(!feed(uiC, QByteArray(1, '\x03')));

        RecordingBackend backendQ;
        TerminalUi uiQ(terminalConfig("halfblock"), &backendQ);
        QVERIFY(!feed(uiQ, QByteArray(1, '\x11')));
    }

    // The quit byte is consumed. It used to be left in the buffer, which meant
    // a viewer that ignored the first quit re-triggered on every later read.
    void testQuitByteIsConsumedAndTextBeforeItFlushes()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(!feed(ui, QByteArray("hi\x03", 3)));
        QCOMPARE(backend.texts.size(), 1);
        QCOMPARE(backend.texts[0], QByteArray("hi"));

        // Nothing stale is left behind: a later ordinary read is alive again.
        QVERIFY(feed(ui, "x"));
    }

    // ── The cell map ────────────────────────────────────────────────────────

    // TERM-INPUT-07
    void testMapCellToPageBounds()
    {
        DisplayMap map;
        map.dispCols = 40;
        map.dispRows = 10;
        map.viewportW = 400;
        map.viewportH = 200;

        int x = -1, y = -1;
        QVERIFY(mapCellToPage(map, 0, 0, x, y));
        QCOMPARE(x, 5);
        QCOMPARE(y, 10);
        QVERIFY(mapCellToPage(map, 39, 9, x, y));
        QCOMPARE(x, 395);
        QCOMPARE(y, 190);

        // One past each edge, and negatives.
        QVERIFY(!mapCellToPage(map, 40, 0, x, y));
        QVERIFY(!mapCellToPage(map, 0, 10, x, y));
        QVERIFY(!mapCellToPage(map, -1, 0, x, y));
        QVERIFY(!mapCellToPage(map, 0, -1, x, y));

        // An empty map — no frame has arrived yet — maps nothing at all.
        QVERIFY(!mapCellToPage(DisplayMap(), 0, 0, x, y));
    }

    // ── The renderers ───────────────────────────────────────────────────────

    // TERM-GFX-01: kitty. `script(1)` always resolves the viewer to halfblock,
    // so this protocol runs in no integration test. The payload is reassembled
    // from its chunks and compared against Qt's base64 — which is the check
    // that matters, because the encoder under it is hand-rolled.
    void testKittyPayloadIsChunkedBase64()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("kitty"), &backend);
        QCOMPARE(ui.gfxMode(), GfxMode::Kitty);

        // Long enough that the base64 exceeds kitty's 4096-byte chunk limit.
        QByteArray png(5000, '\0');
        for (qsizetype i = 0; i < png.size(); ++i)
            png[i] = static_cast<char>((i * 37) & 0xFF);

        FrameData frame;
        frame.width = 400;
        frame.height = 200;
        frame.viewportW = 400;
        frame.viewportH = 200;
        frame.bytes = png;
        ui.onFrame(frame);

        const QByteArray painted = capture.take();
        QVERIFY2(painted.contains("\x1b_G"), "no kitty graphics escape was written");
        QVERIFY(painted.contains("a=T,f=100,i=1,p=1,q=2,C=1,c="));

        // Reassemble: every chunk is ESC _ G <keys> ; <payload> ESC \.
        QByteArray payload;
        int chunks = 0;
        qsizetype at = 0;
        for (;;) {
            const qsizetype start = painted.indexOf(QByteArray("\x1b_G"), at);
            if (start < 0)
                break;
            const qsizetype semi = painted.indexOf(';', start);
            const qsizetype end = painted.indexOf(QByteArray("\x1b\\"), semi);
            QVERIFY(semi > 0 && end > semi);
            payload += painted.mid(semi + 1, end - semi - 1);
            ++chunks;
            at = end + 2;
        }
        QVERIFY2(chunks >= 2, "a payload past 4096 bytes was not chunked");
        QCOMPARE(payload, png.toBase64());

        // The last chunk closes the transmission and no earlier one does.
        QCOMPARE(painted.count(QByteArray("m=0;")), qsizetype(1));
        QCOMPARE(painted.count(QByteArray("m=1;")), qsizetype(chunks - 1));
    }

    // TERM-GFX-02: iTerm2's inline-image escape. size= must be the byte count
    // of the *image*, not of the base64, or the terminal waits for more.
    void testItermPayloadAndSize()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("iterm"), &backend);
        QCOMPARE(ui.gfxMode(), GfxMode::Iterm);

        const QByteArray png(300, '\x5A');
        FrameData frame;
        frame.width = 400;
        frame.height = 200;
        frame.viewportW = 400;
        frame.viewportH = 200;
        frame.bytes = png;
        ui.onFrame(frame);

        const QByteArray painted = capture.take();
        const QByteArray head = "\x1b]1337;File=inline=1;size=" + QByteArray::number(png.size());
        QVERIFY2(painted.contains(head), "iTerm header missing or size is not the image length");
        QVERIFY(painted.contains("preserveAspectRatio=0:" + png.toBase64() + "\x07"));
    }

    // TERM-GFX-03: an identical frame is not re-sent. Terminals redraw the
    // image on every transmission, so repainting an unchanged page flickers.
    void testIdenticalGfxFrameIsNotResent()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("kitty"), &backend);

        FrameData frame;
        frame.width = 400;
        frame.height = 200;
        frame.viewportW = 400;
        frame.viewportH = 200;
        frame.bytes = QByteArray(64, '\x11');

        ui.onFrame(frame);
        QVERIFY(capture.take().contains("\x1b_G"));

        ui.onFrame(frame);
        QVERIFY2(!capture.take().contains("\x1b_G"), "an unchanged frame was retransmitted");

        frame.bytes = QByteArray(64, '\x22');
        ui.onFrame(frame);
        QVERIFY(capture.take().contains("\x1b_G"));
    }

    // The halfblock renderer asks for one pixel per column and two per row.
    // How many rows it may use depends on the status bar: shown — the default —
    // it costs the page one, and hiding it hands that row back. tick() is the
    // only place that contract is expressed.
    void testHalfblockFrameGeometry()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(ui.tick());
        QCOMPARE(backend.rgbRequests.size(), 1);
        QCOMPARE(backend.rgbRequests[0], QSize(80, (24 - 1) * 2)); // 80x24 default, bar shown
        QCOMPARE(backend.pngRequests, 0);

        toggleStatusBar(ui); // hidden — the page gets the row back
        QVERIFY(ui.tick());
        QCOMPARE(backend.rgbRequests.size(), 2);
        QCOMPARE(backend.rgbRequests[1], QSize(80, 24 * 2));

        RecordingBackend gfxBackend;
        TerminalUi gfxUi(terminalConfig("kitty"), &gfxBackend);
        QVERIFY(gfxUi.tick());
        QCOMPARE(gfxBackend.pngRequests, 1);
        QVERIFY(gfxBackend.rgbRequests.isEmpty());
    }

    // ── The status bar ──────────────────────────────────────────────────────

    // The three-source precedence: a backend complaint beats a named link,
    // and both beat the frame-level "connection lost" fallback.
    //
    // The short description and the typed byte are not decoration. At 80
    // columns the row only has room for the optional link once the long usage
    // hint has been replaced by the last-input echo — which is the yielding
    // rule the next test covers from the other side.
    void testStatusBarPrecedence()
    {
        CapturedStdout capture;
        RecordingBackend backend(QStringLiteral("h:1"));
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.setBackendLabel(QStringLiteral("cdp"));
        ui.onFrame(rgbFrame(40, 20, 40, 20));
        QVERIFY(feed(ui, "a"));
        capture.take();

        ui.onLink(QStringLiteral("page 1"));
        QVERIFY(feed(ui, QByteArray()));
        QByteArray row = statusRow(capture.take());
        QVERIFY2(row.contains("cdp h:1 40x20 [halfblock]"), row.constData());
        QVERIFY2(row.contains("page 1"), row.constData());

        ui.onStatus(QStringLiteral("reconnecting"));
        QVERIFY(feed(ui, QByteArray()));
        row = statusRow(capture.take());
        QVERIFY2(row.contains("reconnecting"), row.constData());
        QVERIFY2(!row.contains("page 1"), row.constData());

        // Clearing both falls back to the frame-level state, which is still
        // "connected" here because a frame did arrive. All three sources have
        // to be checked gone, not just the fallback: asserting only the absence
        // of "connection lost" would pass just as happily if a cleared status
        // or link were still being painted.
        ui.onStatus(QString());
        ui.onLink(QString());
        QVERIFY(feed(ui, QByteArray()));
        row = statusRow(capture.take());
        QVERIFY2(row.contains("cdp h:1 40x20 [halfblock]"), row.constData());
        QVERIFY2(!row.contains("reconnecting"), row.constData());
        QVERIFY2(!row.contains("page 1"), row.constData());
        QVERIFY2(!row.contains("connection lost"), row.constData());
    }

    // The link is the one field that yields. It is steady state, and dropping
    // the last-input echo to make room for it would cost the only signal that
    // says the terminal is delivering input at all.
    void testStatusBarLinkYieldsWhenItDoesNotFit()
    {
        CapturedStdout capture;
        RecordingBackend backend(QStringLiteral("h:1"));
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 40, 20));
        capture.take();

        // No input yet, so the tail is the usage hint — which now also lists the
        // viewer's shortcuts, making it longer than it was and leaving even less
        // room for a link.
        ui.onLink(QStringLiteral("attached to page ABC"));
        QVERIFY(feed(ui, QByteArray()));
        const QByteArray row = statusRow(capture.take());
        QVERIFY2(!row.contains("attached to page ABC"), row.constData());
        QVERIFY2(row.contains("click/type drive the page"), row.constData());
    }

    // TERM-INPUT-08: the row is truncated to m_cols *bytes*, so a multibyte
    // character straddling the last column is cut in half and the terminal is
    // handed a lone UTF-8 lead byte.
    //
    // This documents code-quality finding #7 rather than fixing it: the row is
    // reverse-video padding at the bottom of the screen, and a split trailing
    // character shows up as one replacement glyph on the terminals that render
    // it at all. If the fix is taken (truncate on a character boundary), this
    // test is the one that has to change, and the numbers below say exactly
    // what to change it to.
    void testStatusBarTruncatesOnByteBoundary()
    {
        CapturedStdout capture;
        // 31 characters, chosen so the em dash in " connection lost <em> "
        // straddles column 80:
        //   " anoa terminal "                15
        //   description                      31
        //   " 0x0 [halfblock]"               16   -> em dash begins at byte 79
        //   " connection lost "              17
        //
        // The description carries the slack because it is the only field here
        // whose length the test controls; the header shrank when the command
        // was renamed from anoa-browser to anoa, and this number absorbed it.
        RecordingBackend backend(QStringLiteral("terminal-host.example.com:18080"));
        QCOMPARE(backend.description().size(), qsizetype(31));

        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrameFailed(QStringLiteral("no response")); // m_connected = false
        capture.take();

        QVERIFY(feed(ui, QByteArray()));
        const QByteArray row = statusRow(capture.take());

        QCOMPARE(row.size(), qsizetype(80)); // the 80-column default
        QVERIFY2(row.endsWith("connection lost \xE2"), row.toHex(' ').constData());
        // Which is the first byte of the em dash and nothing else: the two
        // continuation bytes were cut off.
        QVERIFY(!row.contains("\xE2\x80\x94"));
    }

    // A row that fits is padded to the full width instead — the reverse-video
    // bar has to span the terminal or it looks like a stray highlight.
    void testStatusBarPadsToWidth()
    {
        CapturedStdout capture;
        RecordingBackend backend(QStringLiteral("h:1"));
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 40, 20));
        QVERIFY(feed(ui, "a")); // the short tail, so the row fits at all
        capture.take();

        QVERIFY(feed(ui, QByteArray()));
        const QByteArray row = statusRow(capture.take());
        QCOMPARE(row.size(), qsizetype(80));
        QVERIFY(row.endsWith(" "));
    }

    // The last forwarded event replaces the usage hint, which is the only
    // signal a user gets that their terminal is delivering input at all.
    void testStatusBarEchoesTheLastInput()
    {
        CapturedStdout capture;
        RecordingBackend backend(QStringLiteral("h:1"));
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 40, 20));
        capture.take();

        QVERIFY(feed(ui, "hello"));
        const QByteArray row = statusRow(capture.take());
        QVERIFY2(row.contains("ctrl-c=quit | typed \"hello\""), row.constData());
        QVERIFY(!row.contains("click/type/scroll drive the page"));
    }

    // ── Escape-sequence resync ──────────────────────────────────────────────
    //
    // The three branches below are the parser's "this is not a sequence I
    // handle" exits. Each one advances the cursor by a different amount, and
    // getting that wrong does not throw — it silently types control bytes into
    // the page, which is exactly how bug-002 presented.

    // TERM-INPUT-09: ESC followed by anything other than '[' (SS3 "ESC O P" is
    // the common one — F1 on a vt100-style keyboard). Both bytes are skipped;
    // the byte after them is ordinary input.
    void testEscapeFollowedByNonCsiIsSkipped()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "\x1bOP"));

        QCOMPARE(backend.texts, QList<QByteArray>{QByteArray("P")});
        QVERIFY2(backend.keys.isEmpty(), "SS3 was decoded as a named key");
    }

    // A CSI whose final byte is neither an arrow nor the SGR mouse introducer
    // (ESC [ H, cursor home) drops only the two-byte introducer, so the 'H'
    // itself is typed. Pinned as behaviour rather than endorsed: it is the
    // fallthrough at the bottom of the loop, and a future arity-aware parser
    // would change it.
    void testUnknownCsiFinalBytePassesThrough()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(feed(ui, "\x1b[Hx"));

        QCOMPARE(backend.texts, QList<QByteArray>{QByteArray("Hx")});
        QVERIFY(backend.keys.isEmpty());
    }

    // TERM-INPUT-10: an SGR mouse report interrupted by a byte that is neither
    // a digit, a separator nor a final M/m. The parser skips the offending byte
    // and resumes at the next one; it must not report a click from the digits
    // it had already accumulated.
    void testMalformedSgrMouseReportResyncs()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        ui.onFrame(rgbFrame(40, 20, 40, 20));

        // The trailing 'z' matters: without a byte after the malformed one the
        // parser treats the report as incomplete and waits for more input
        // instead of resyncing.
        QVERIFY(feed(ui, "\x1b[<0;5;5Xz"));

        QVERIFY2(backend.clicks.isEmpty(), "a half-parsed mouse report produced a click");
        QCOMPARE(backend.texts, QList<QByteArray>{QByteArray("z")});
    }

    // ── Construction edges ──────────────────────────────────────────────────

    // An unrecognised --gfx name resolves to Auto, which is what defers the
    // choice to detectGfx() in begin(). config.cpp rejects unknown names up
    // front, so this is the second line of defence rather than a reachable CLI
    // state — but it is what makes gfxModeFromString total.
    void testUnknownGfxNameResolvesToAuto()
    {
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("bogus"), &backend);

        QCOMPARE(ui.gfxMode(), GfxMode::Auto);
    }

    // fps is clamped to >= 1 in the constructor even though config.cpp already
    // validates the 1-120 range, because framePeriodMs() divides by it.
    void testZeroFpsIsClampedRatherThanDividingByZero()
    {
        RecordingBackend backend;
        Config cfg = terminalConfig("halfblock");
        cfg.fps = 0;
        TerminalUi ui(cfg, &backend);

        QCOMPARE(ui.framePeriodMs(), 1000);
    }

    // Destroying through a QObject* is how a parented TerminalUi actually dies
    // (terminal_app.cpp parents it), and it reaches the virtual deleting
    // destructor that a stack instance never does. end() must be a no-op when
    // begin() was never called, or this would try to restore a terminal the
    // process does not own.
    void testDestructionThroughBasePointerIsSafe()
    {
        RecordingBackend backend;
        QObject *ui = new TerminalUi(terminalConfig("halfblock"), &backend);

        delete ui; // must not touch termios — begin() was never called
    }

    // ── term:: terminal helpers ─────────────────────────────────────────────

    // TERM-TERM-01: the gfx auto-detection matrix. Every branch is a getenv
    // string compare, so this is the whole function — and it is unreachable
    // from the pty suites, where script(1) fixes TERM to something unrelated.
    void testDetectGfxEnvironmentMatrix()
    {
        ScopedGfxEnv env;

        QCOMPARE(term::detectGfx(), GfxMode::Halfblock);

        qputenv("KITTY_WINDOW_ID", "1");
        QCOMPARE(term::detectGfx(), GfxMode::Kitty);
        env.clear();

        qputenv("TERM", "xterm-kitty");
        QCOMPARE(term::detectGfx(), GfxMode::Kitty);
        env.clear();

        qputenv("TERM_PROGRAM", "ghostty");
        QCOMPARE(term::detectGfx(), GfxMode::Kitty);
        env.clear();

        qputenv("TERM_PROGRAM", "iTerm.app");
        QCOMPARE(term::detectGfx(), GfxMode::Iterm);
        env.clear();

        qputenv("TERM_PROGRAM", "WezTerm");
        QCOMPARE(term::detectGfx(), GfxMode::Iterm);
        env.clear();

        // A terminal that says nothing recognisable gets the renderer that
        // works everywhere, not an image protocol it would print as garbage.
        qputenv("TERM", "xterm-256color");
        QCOMPARE(term::detectGfx(), GfxMode::Halfblock);
    }

    // TERM-TERM-02: restoreTerminal() before enterRawMode() has succeeded is a
    // no-op. It is registered with atexit(), so it runs on every exit path
    // including the ones where raw mode was never entered.
    void testRestoreTerminalWithoutRawModeIsANoOp()
    {
        CapturedStdout capture;

        term::restoreTerminal();

        QVERIFY2(capture.take().isEmpty(),
                 "restoreTerminal wrote escape codes without owning the terminal");
    }

    // TERM-TERM-03: enterRawMode() on a non-tty fails at tcgetattr() and
    // reports it rather than proceeding. This is the check behind terminal
    // mode's "stdin/stdout must be a terminal" refusal.
    void testEnterRawModeFailsOnANonTty()
    {
        CapturedStdout capture;
        RedirectedStdin stdinIsAFile;

        QVERIFY2(!term::enterRawMode(), "raw mode was entered on a regular file");
        QVERIFY2(capture.take().isEmpty(), "the alt screen was taken despite the failure");
    }

    // TERM-TERM-04: terminalSize() falls back to 80x24 when the ioctl fails,
    // which is the geometry every other test in this file assumes.
    void testTerminalSizeFallsBackTo80x24()
    {
        CapturedStdout capture; // fd 1 is now a temp file, so TIOCGWINSZ fails
        int cols = 0;
        int rows = 0;

        term::terminalSize(cols, rows);

        QCOMPARE(cols, 80);
        QCOMPARE(rows, 24);
    }

    // TERM-TERM-05: the XTWINOPS cell-size probe parses a well-formed reply.
    // The reply order is ESC [ 6 ; <height> ; <width> t — height first — and
    // swapping it silently halves or doubles every image the gfx renderers
    // scale, which is why the two numbers here are deliberately different.
    void testQueryCellSizeParsesTheXtwinopsReply()
    {
        CapturedStdout capture;
        RedirectedStdin reply("\x1b[6;32;14t");
        int cellW = 0;
        int cellH = 0;

        term::queryCellSize(cellW, cellH);

        QCOMPARE(cellH, 32);
        QCOMPARE(cellW, 14);
        QVERIFY2(capture.take().contains("\x1b[16t"), "the probe itself was never sent");
    }

    // A terminal that never answers leaves the common 1:2 cell in place. The
    // poll budget is bounded (10 x 50 ms), so a silent endpoint cannot hang
    // startup.
    void testQueryCellSizeFallsBackWhenTheTerminalStaysSilent()
    {
        CapturedStdout capture;
        RedirectedStdin noReply;
        int cellW = 0;
        int cellH = 0;

        term::queryCellSize(cellW, cellH);

        QCOMPARE(cellW, 8);
        QCOMPARE(cellH, 16);
    }

    // TERM-TERM-06: begin() on a non-tty resolves the gfx mode and then fails
    // at raw mode, reporting false without having drawn anything. The caller
    // relies on that ordering — a half-started viewer would leave the terminal
    // on the alt screen with no way back.
    void testBeginResolvesGfxThenFailsOnANonTty()
    {
        CapturedStdout capture;
        RedirectedStdin stdinIsAFile;
        ScopedGfxEnv env; // no image protocol -> the resolved mode is halfblock
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("auto"), &backend);
        QCOMPARE(ui.gfxMode(), GfxMode::Auto);

        QVERIFY2(!ui.begin(), "begin() succeeded without a terminal");

        QCOMPARE(ui.gfxMode(), GfxMode::Halfblock);
        QVERIFY2(capture.take().isEmpty(), "begin() painted before raw mode was established");
    }

    // TERM-TERM-07: SIGWINCH is latched by the handler and consumed once by the
    // frame tick, which clears the screen so the previous size's cells do not
    // survive under the new one.
    void testResizeSignalClearsTheScreenOnce()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        term::installSignalHandlers();
        QVERIFY2(!term::takeResized(), "a resize was pending before one was raised");

        raise(SIGWINCH);
        QVERIFY(ui.tick());

        QVERIFY2(capture.take().contains("\x1b[2J"), "stale cells were not cleared");
        QVERIFY2(!term::takeResized(), "the resize flag was not one-shot");
    }

    // TERM-TERM-09: Ctrl-N cycles tabs and wraps, and the status row names the
    // one on screen.
    void testCtrlNCyclesTabs()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        backend.tabs = QStringList{QStringLiteral("t1"), QStringLiteral("t2"),
                                   QStringLiteral("t3")};
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        // Nothing is selected yet, so the first press moves off the first tab
        // rather than re-selecting it.
        QVERIFY(ui.feedInput("\x0E", 1));
        QCOMPARE(backend.tabSelections.size(), 1);
        QCOMPARE(backend.tabSelections.at(0), QStringLiteral("t2"));
        QVERIFY2(capture.take().contains("t2/3"), "the status row did not name the tab");

        QVERIFY(ui.feedInput("\x0E", 1));
        QCOMPARE(backend.tabSelections.at(1), QStringLiteral("t3"));

        // Wraps rather than stopping at the end.
        QVERIFY(ui.feedInput("\x0E", 1));
        QCOMPARE(backend.tabSelections.at(2), QStringLiteral("t1"));
        QVERIFY2(capture.take().contains("t1/3"), "the wrapped tab was not shown");
    }

    // TERM-TERM-10: with one tab or none, Ctrl-N selects nothing.
    //
    // The viewer also says so in the status row, but this case does not assert
    // the words: without begin() the UI never wires the backend's signals, so
    // it renders as permanently disconnected, and "connection lost — retrying"
    // is 28 columns of an 80-column row that the tail yields to by design. That
    // budget is TERM-TERM-03's subject, not this one's. What matters here is
    // that no tab is selected.
    void testCtrlNWithoutTabsSelectsNothing()
    {
        CapturedStdout capture;
        RecordingBackend backend; // tabs stays empty
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(ui.feedInput("\x0E", 1));
        QVERIFY2(backend.tabSelections.isEmpty(), "a tab was selected where there are none");

        backend.tabs = QStringList{QStringLiteral("t1")};
        QVERIFY(ui.feedInput("\x0E", 1));
        QVERIFY2(backend.tabSelections.isEmpty(), "the only tab was re-selected");
    }

    // TERM-TERM-08: a quit signal stops the frame loop without asking the
    // backend for anything further.
    //
    // This is deliberately the LAST slot in the class: the quit flag has no
    // reset, by design — nothing should be able to un-quit a viewer — so every
    // tick() after this one would return false. Add new cases above it.
    void testQuitSignalStopsTheFrameLoop()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);
        term::installSignalHandlers();
        QVERIFY2(!term::quitSignalled(), "a quit was pending before one was raised");

        raise(SIGTERM);

        QVERIFY(term::quitSignalled());
        QVERIFY2(!ui.tick(), "the frame loop kept running after a quit signal");
        QVERIFY2(backend.rgbRequests.isEmpty(), "a frame was requested after quitting");
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    TestTerminalUi ttu;
    return QTest::qExec(&ttu, argc, argv);
}

#include "test_terminal_ui.moc"
