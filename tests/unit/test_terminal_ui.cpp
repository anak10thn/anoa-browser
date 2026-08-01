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
    QString description() const override { return m_description; }

    QList<QSize> rgbRequests;
    int pngRequests = 0;
    QList<Click> clicks;
    QList<Scroll> scrolls;
    QList<QByteArray> texts;
    QList<QString> keys;

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

private slots:
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

    // The halfblock renderer asks for one pixel per column and two per row,
    // leaving the last row for the status bar. tick() is the only place that
    // contract is expressed.
    void testHalfblockFrameGeometry()
    {
        CapturedStdout capture;
        RecordingBackend backend;
        TerminalUi ui(terminalConfig("halfblock"), &backend);

        QVERIFY(ui.tick());
        QCOMPARE(backend.rgbRequests.size(), 1);
        QCOMPARE(backend.rgbRequests[0], QSize(80, (24 - 1) * 2)); // the 80x24 default
        QCOMPARE(backend.pngRequests, 0);

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
        // "connected" here because a frame did arrive.
        ui.onStatus(QString());
        ui.onLink(QString());
        QVERIFY(feed(ui, QByteArray()));
        row = statusRow(capture.take());
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

        // No input yet, so the tail is the 46-character usage hint and there
        // is no room left for a link of any length.
        ui.onLink(QStringLiteral("attached to page ABC"));
        QVERIFY(feed(ui, QByteArray()));
        const QByteArray row = statusRow(capture.take());
        QVERIFY2(!row.contains("attached to page ABC"), row.constData());
        QVERIFY2(row.contains("click/type/scroll drive the page"), row.constData());
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
        // 23 characters, chosen so the em dash in " connection lost <em> "
        // straddles column 80:
        //   " anoa-browser terminal "        23
        //   description                      23
        //   " 0x0 [halfblock]"               16   -> em dash begins at byte 79
        //   " connection lost "              17
        RecordingBackend backend(QStringLiteral("terminal.example.com:80"));
        QCOMPARE(backend.description().size(), qsizetype(23));

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
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    TestTerminalUi ttu;
    return QTest::qExec(&ttu, argc, argv);
}

#include "test_terminal_ui.moc"
