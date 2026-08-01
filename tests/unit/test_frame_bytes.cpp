// TERM-BYTES-01..06 — the two wire-facing byte parsers in
// src/terminal/frame_bytes.cpp.
//
// Why these get a target of their own: parsePpm() and pngDimensions() are the
// first code in the terminal viewer to touch bytes that came off a socket, and
// every dimension they act on is chosen by the peer. The rest of the viewer is
// only reachable through a pty and a live endpoint; these are reachable
// directly, so this is where a truncated or hostile response gets tested.
//
// Every buffer under test is an exactly-sized heap allocation rather than a
// QByteArray or a string literal. Both of those keep a NUL terminator and
// round their allocation up, so a short read off the end of one lands inside
// memory the process owns and neither the assertion nor ASan sees anything.
// `Bytes` below has nothing after the last byte, which is what makes
// TERM-BYTES-02/03 fail loudly (with -DANOA_TEST_SANITIZE=ON) instead of
// quietly returning garbage.

#include <cstring>
#include <memory>

#include <QByteArray>
#include <QCoreApplication>
#include <QtTest/QtTest>

#include "terminal/frame_bytes.h"

namespace {

// An exactly-sized, unterminated heap copy of `n` bytes.
class Bytes
{
public:
    Bytes(const char *src, size_t n)
        : m_data(new char[n])
        , m_size(n)
    {
        memcpy(m_data.get(), src, n);
    }
    explicit Bytes(const QByteArray &src)
        : Bytes(src.constData(), static_cast<size_t>(src.size()))
    {
    }

    const char *data() const { return m_data.get(); }
    size_t size() const { return m_size; }

private:
    std::unique_ptr<char[]> m_data;
    size_t m_size;
};

// A PPM whose header says w x h and whose body is `bodyLen` bytes long — the
// two are decoupled on purpose so a test can under-fill the body.
Bytes ppm(const char *header, size_t bodyLen, char fill = '\x7F')
{
    QByteArray buf(header);
    buf.append(QByteArray(static_cast<qsizetype>(bodyLen), fill));
    return Bytes(buf);
}

// The same 4x2 PNG test_config.cpp decodes, reused here so the two suites
// agree on what a valid PNG is. Row 0 = red/green/blue/white.
const char kTinyPngBase64[] =
    "iVBORw0KGgoAAAANSUhEUgAAAAQAAAACCAIAAADwyuo0AAAAFklEQVR42mP4z8DA"
    "AMZAwACmwWyG/wCJkAv1L5VDcAAAAABJRU5ErkJggg==";

QByteArray tinyPng()
{
    return QByteArray::fromBase64(QByteArray(kTinyPngBase64));
}

} // namespace

class TestFrameBytes : public QObject
{
    Q_OBJECT

private slots:
    // ── parsePpm ────────────────────────────────────────────────────────────

    // TERM-BYTES-01: the happy path, down to the pixel.
    void testParsePpmSinglePixel()
    {
        const char kImage[] = "P6 1 1 255\n\xFF\x00\x00";
        // sizeof - 1 would stop at the embedded NUL, which is a legitimate
        // pixel byte here; the length is spelled out instead.
        const Bytes buf(kImage, 10 + 1 + 3);

        int w = 0, h = 0;
        QByteArray pixels;
        QVERIFY(frame_bytes::parsePpm(buf.data(), buf.size(), w, h, pixels));
        QCOMPARE(w, 1);
        QCOMPARE(h, 1);
        QCOMPARE(pixels.size(), qsizetype(3));
        QCOMPARE(int(static_cast<unsigned char>(pixels[0])), 0xFF);
        QCOMPARE(int(static_cast<unsigned char>(pixels[1])), 0x00);
        QCOMPARE(int(static_cast<unsigned char>(pixels[2])), 0x00);
    }

    // TERM-BYTES-02: bug-001. The body ends at the maxval, so the "single
    // whitespace byte after maxval" step walks pos one *past* the end. The
    // guard was `len - pos < need` in size_t arithmetic, which wraps to
    // SIZE_MAX and always passes, and the copy that follows reads three bytes
    // the process never allocated. w and h are 1 here, so the damage is small
    // — TERM-BYTES-03 is the same defect at wire-chosen scale.
    void testParsePpmRejectsBodyEndingAtMaxval()
    {
        const Bytes buf = ppm("P6 1 1 255", 0);
        QCOMPARE(buf.size(), size_t(10));

        int w = 0, h = 0;
        QByteArray pixels;
        QVERIFY(!frame_bytes::parsePpm(buf.data(), buf.size(), w, h, pixels));
        QVERIFY(pixels.isEmpty());
    }

    // TERM-BYTES-03: bug-001 at the magnitude the report gives — a peer that
    // answers exactly "P6 4000 4000 255" and then stops. The body has to end
    // at the maxval for the defect to fire: that is the one input that leaves
    // pos past the end of the buffer, and it turns the guard into a 48 MB
    // copy out of a sixteen-byte allocation. With ten bytes of body (the
    // second case) pos stays inside and the ordinary short-body check catches
    // it, which is why the truncated-but-nonempty response was never the
    // reproducer.
    void testParsePpmRejectsHugeHeaderWithNoBody()
    {
        int w = 0, h = 0;
        QByteArray pixels;

        const Bytes empty = ppm("P6 4000 4000 255", 0);
        QVERIFY(!frame_bytes::parsePpm(empty.data(), empty.size(), w, h, pixels));
        QVERIFY(pixels.isEmpty());

        const Bytes tiny = ppm("P6 4000 4000 255", 10);
        QVERIFY(!frame_bytes::parsePpm(tiny.data(), tiny.size(), w, h, pixels));
        QVERIFY(pixels.isEmpty());
    }

    // A body one byte short of complete is still rejected — the boundary
    // either side of it, so the guard is pinned rather than merely present.
    void testParsePpmBodyBoundary()
    {
        int w = 0, h = 0;
        QByteArray pixels;

        const Bytes shortBody = ppm("P6 2 2 255\n", 2 * 2 * 3 - 1);
        QVERIFY(!frame_bytes::parsePpm(shortBody.data(), shortBody.size(), w, h, pixels));

        const Bytes exact = ppm("P6 2 2 255\n", 2 * 2 * 3);
        QVERIFY(frame_bytes::parsePpm(exact.data(), exact.size(), w, h, pixels));
        QCOMPARE(pixels.size(), qsizetype(12));

        // Trailing bytes past the declared image are ignored, not an error.
        const Bytes extra = ppm("P6 2 2 255\n", 2 * 2 * 3 + 9);
        QVERIFY(frame_bytes::parsePpm(extra.data(), extra.size(), w, h, pixels));
        QCOMPARE(pixels.size(), qsizetype(12));
    }

    // TERM-BYTES-04: the skipSpace path, inherited unchanged from anoa-term —
    // '#' comments and runs of whitespace between every field.
    void testParsePpmHeaderCommentsAndWhitespace()
    {
        const Bytes buf = ppm("P6\n# generated by anoa\n  2\t\n 1 \n# another\n255\n", 6);

        int w = 0, h = 0;
        QByteArray pixels;
        QVERIFY(frame_bytes::parsePpm(buf.data(), buf.size(), w, h, pixels));
        QCOMPARE(w, 2);
        QCOMPARE(h, 1);
        QCOMPARE(pixels.size(), qsizetype(6));
    }

    // TERM-BYTES-05: dimensions are wire-controlled, so every unusable
    // spelling of one has to be refused rather than believed.
    void testParsePpmRejectsBadDimensions()
    {
        // header, body length
        const struct {
            const char *header;
            size_t body;
        } cases[] = {
            {"P6 0 4 255\n", 64},           // zero width
            {"P6 4 0 255\n", 64},           // zero height
            {"P6 -1 1 255\n", 64},          // negative: '-' is not a digit
            {"P6 x 1 255\n", 64},           // not a number at all
            {"P6 1 1 128\n", 64},           // maxval other than 255
            {"P6 99999999999 1 255\n", 64}, // wide enough to overflow an int
            {"P5 1 1 255\n", 64},           // ASCII PPM, not binary
            {"", 0},                        // empty
            {"P", 0},                       // magic truncated mid-way
        };

        for (const auto &c : cases) {
            const Bytes buf = ppm(c.header, c.body);
            int w = 0, h = 0;
            QByteArray pixels;
            QVERIFY2(!frame_bytes::parsePpm(buf.data(), buf.size(), w, h, pixels), c.header);
        }
    }

    // ── pngDimensions ───────────────────────────────────────────────────────

    // TERM-BYTES-06
    void testPngDimensions()
    {
        const Bytes png(tinyPng());
        int w = 0, h = 0;
        QVERIFY(frame_bytes::pngDimensions(png.data(), png.size(), w, h));
        QCOMPARE(w, 4);
        QCOMPARE(h, 2);
    }

    void testPngDimensionsRejectsTruncatedAndForeign()
    {
        const QByteArray valid = tinyPng();
        int w = 0, h = 0;

        // One byte short of the 24 the IHDR walk needs.
        const Bytes truncated(valid.constData(), 23);
        QVERIFY(!frame_bytes::pngDimensions(truncated.data(), truncated.size(), w, h));

        // Right length, wrong magic: a JFIF header is the realistic confusion.
        QByteArray jpeg = valid;
        memcpy(jpeg.data(), "\xFF\xD8\xFF\xE0\x00\x10JF", 8);
        const Bytes foreign(jpeg);
        QVERIFY(!frame_bytes::pngDimensions(foreign.data(), foreign.size(), w, h));

        // Empty, and a buffer that is nothing but the signature.
        const Bytes empty(valid.constData(), 0);
        QVERIFY(!frame_bytes::pngDimensions(empty.data(), empty.size(), w, h));
        const Bytes sigOnly(valid.constData(), 8);
        QVERIFY(!frame_bytes::pngDimensions(sigOnly.data(), sigOnly.size(), w, h));
    }

    // A zero dimension is not a usable image, and a dimension with the top bit
    // set must not be shifted into the sign bit of an int on the way out.
    void testPngDimensionsRejectsZeroAndOversizeFields()
    {
        QByteArray png = tinyPng();
        int w = 0, h = 0;

        QByteArray zeroWidth = png;
        memset(zeroWidth.data() + 16, 0, 4);
        const Bytes zero(zeroWidth);
        QVERIFY(!frame_bytes::pngDimensions(zero.data(), zero.size(), w, h));

        QByteArray topBit = png;
        memcpy(topBit.data() + 16, "\xFF\xFF\xFF\xFF", 4);
        const Bytes huge(topBit);
        QVERIFY(!frame_bytes::pngDimensions(huge.data(), huge.size(), w, h));
    }
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    TestFrameBytes tfb;
    return QTest::qExec(&tfb, argc, argv);
}

#include "test_frame_bytes.moc"
