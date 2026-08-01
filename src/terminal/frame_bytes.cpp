#include "terminal/frame_bytes.h"

#include <cctype>
#include <cstring>

namespace frame_bytes {

namespace {

// Refuse a dimension before it can overflow an int on the way in. Anything
// this large is a hostile or corrupt header, not a screenshot: at three bytes
// a pixel, a single 10^8-wide row already exceeds any buffer that could have
// carried it. The cap also keeps the accumulator below INT_MAX with a full
// decimal digit of headroom, so the multiply-add below stays defined rather
// than relying on wraparound.
constexpr int kMaxDimension = 100000000;

} // namespace

// /render/screenshot.ppm answers with the page already scaled server-side, so
// the only work here is stripping the header off the packed RGB the renderer
// wants.
bool parsePpm(const char *data, size_t len, int &width, int &height, QByteArray &pixels)
{
    size_t pos = 0;
    auto skipSpace = [&]() {
        while (pos < len) {
            if (isspace(static_cast<unsigned char>(data[pos]))) {
                ++pos;
            } else if (data[pos] == '#') { // comment to end of line
                while (pos < len && data[pos] != '\n')
                    ++pos;
            } else {
                break;
            }
        }
    };
    auto readInt = [&](int &value) {
        skipSpace();
        if (pos >= len || !isdigit(static_cast<unsigned char>(data[pos])))
            return false;
        value = 0;
        while (pos < len && isdigit(static_cast<unsigned char>(data[pos]))) {
            if (value > kMaxDimension)
                return false;
            value = value * 10 + (data[pos++] - '0');
        }
        return true;
    };

    if (len < 2 || data[0] != 'P' || data[1] != '6')
        return false;
    pos = 2;
    int w = 0, h = 0, maxval = 0;
    if (!readInt(w) || !readInt(h) || !readInt(maxval) || maxval != 255)
        return false;
    ++pos; // single whitespace byte after maxval
    if (w <= 0 || h <= 0 || w > kMaxDimension || h > kMaxDimension)
        return false;

    // `pos > len` is the whole of bug-001. A response that ends at the maxval
    // — "P6 4000 4000 255" and nothing else — leaves pos at len + 1 after the
    // step above, and `len - pos` in size_t arithmetic then wraps to SIZE_MAX
    // and passes any check put after it. The copy that followed read w*h*3
    // bytes, both of them chosen by the peer, out of a sixteen-byte buffer.
    //
    // 64-bit throughout for the same reason: w*h*3 is wire-controlled and
    // would overflow a 32-bit size_t on the way to being compared.
    const unsigned long long need = 3ULL * static_cast<unsigned long long>(w)
                                    * static_cast<unsigned long long>(h);
    if (pos > len || static_cast<unsigned long long>(len - pos) < need)
        return false;

    width = w;
    height = h;
    pixels = QByteArray(data + pos, static_cast<qsizetype>(need));
    return true;
}

bool pngDimensions(const char *data, size_t len, int &width, int &height)
{
    static const unsigned char sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (len < 24 || memcmp(data, sig, 8) != 0)
        return false;
    const auto *p = reinterpret_cast<const unsigned char *>(data);
    // Assembled unsigned: a PNG may legally declare a width with the top bit
    // set, and shifting that into the sign bit of an int is undefined.
    const auto be32 = [p](size_t at) {
        return (static_cast<unsigned>(p[at]) << 24) | (static_cast<unsigned>(p[at + 1]) << 16)
               | (static_cast<unsigned>(p[at + 2]) << 8) | static_cast<unsigned>(p[at + 3]);
    };
    const unsigned w = be32(16);
    const unsigned h = be32(20);
    if (w == 0 || h == 0 || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu)
        return false;
    width = static_cast<int>(w);
    height = static_cast<int>(h);
    return true;
}

} // namespace frame_bytes
