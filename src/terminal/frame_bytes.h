#pragma once

// The two byte-level parsers both frame backends need: the PPM header the
// halfblock renderer is fed through, and the PNG IHDR the image protocols size
// their cell rectangle from. They used to be file-local statics, one copy per
// backend (pngDimensions() existed twice, byte for byte).
//
// They are here for two reasons. The duplicate is gone, and — the load-bearing
// one — both parsers read length-prefixed bytes that arrived over the wire, so
// they are the part of the terminal viewer most worth testing directly. A
// hostile or truncated response reaches them before it reaches anything else.
// Everything here is pure: no sockets, no Qt event loop, no QObject, so the
// unit target that links it needs Qt6::Core alone.
//
// Both take (data, len) rather than a container so the CDP backend can pass a
// QByteArray and the HTTP backend a std::string without either converting.

#include <cstddef>

#include <QByteArray>

namespace frame_bytes {

// Parse a binary PPM (P6, maxval 255) into packed RGB.
//
// `pixels` receives exactly width*height*3 bytes and is only written on
// success. False means the buffer is not a P6 image, is truncated, or carries
// dimensions this refuses to believe — in every case nothing was read past
// `data + len`.
bool parsePpm(const char *data, size_t len, int &width, int &height, QByteArray &pixels);

// Read width/height out of a PNG's IHDR without decoding the image.
//
// The gfx renderers hand the PNG to the terminal untouched, so it is never
// decoded on that path; the aspect-correct cell rect only needs these numbers.
// False means the signature is wrong, the header is short, or a dimension is
// zero or too large to be an int.
bool pngDimensions(const char *data, size_t len, int &width, int &height);

} // namespace frame_bytes
