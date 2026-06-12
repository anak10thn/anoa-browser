# Code Quality Report — web-render-endpoint

**File reviewed**: `src/http/http_server.cpp`
**Phase**: 12 (code-quality)

---

## Issues

### HIGH — `handleNewConnection` is monolithic

`handleNewConnection` is ~390 lines, containing six route handlers in a single function body connected by nested if/else chains. Each handler should be a private method (`handleScreenshot`, `handleHtml`, `handleNavigate`, `handleLiveView`, `handleMjpeg`, `handleCdpProxy`). The routing logic remains in `handleNewConnection` but reduces to a dispatch table.

**Impact**: any future route addition grows an already-unreadable function; untestable in isolation.

---

### HIGH — Duplicated response-sending code

`sendResponse` exists but only handles `application/json` responses. Every other content type (text/plain, text/html, image/png, image/jpeg) manually replicates the same `socket->write / flush / disconnectFromHost / deleteLater` pattern — 8+ times inline. The fix is a generic helper:

```cpp
static void sendRawResponse(QTcpSocket *socket, int statusCode,
                             const QByteArray &statusText,
                             const QByteArray &contentType,
                             const QByteArray &body);
```

The redirect at line 138–148 and both error branches in the screenshot handler (lines 216–239) are the most obvious duplications.

---

### MEDIUM — Inconsistent error response format

Some 4xx/5xx errors return JSON (`{"error":"..."}`), others return bare plain text (`"capture failed"`, `"invalid url"`, `"scheme not allowed"`, `"html capture timeout"`). Pick one format — JSON is preferred since callers likely machine-parse errors.

---

### MEDIUM — Magic numbers without names

| Location | Value | Meaning |
|---|---|---|
| Lines 74, 169, 299 | `5000` | Socket/network read timeout (ms) |
| Line 414 | `100` | MJPEG frame interval (ms) |
| Line 418 | `512 * 1024` | MJPEG backpressure threshold (bytes) |
| Line 430 | `70` | JPEG quality (0–100) |
| Line 120 | `7` (implicit in `mid(7)`) | Length of "Bearer " prefix |

Named constants (`constexpr int kReadTimeoutMs = 5000`) at the top of the file would make each value self-documenting and easy to tune.

---

### MEDIUM — `QNetworkAccessManager` created per-request (line 160)

`QNetworkAccessManager nam` is stack-allocated inside the CDP proxy branch. NAM is designed to be long-lived (it manages connection pools and DNS cache). Creating one per request is correct but wasteful. Move it to a `m_nam` member variable.

---

### LOW — `HtmlCaptureState` sentinel convention is implicit

`HtmlCaptureState::timedOut` defaults to `true`, which means "no result yet" until the async callback fires. This is a double-duty flag (initial state AND actual timeout). The `waiting` flag is a second guard for the same concern. One clear `enum class CaptureStatus { Pending, Ok, TimedOut }` would be less surprising.

---

### LOW — Large HTML template embedded inside function body (line 362)

`static const char htmlTmpl[]` is a 22-line string literal inside `handleNewConnection`. It should be at file scope (between the includes and the constructor) to separate data from logic.

---

### LOW — Bearer prefix length as magic offset

`authHeader.mid(7)` at line 120 silently assumes `"Bearer "` is exactly 7 chars. Either add a `static const QLatin1String kBearerPrefix("Bearer ")` and use `.length()`, or at minimum add a brief comment.

---

## Summary

| Severity | Count |
|---|---|
| HIGH | 2 |
| MEDIUM | 3 |
| LOW | 3 |

None of the issues are correctness bugs — the implementation is functionally sound. The two HIGH findings (monolithic function + duplicated response code) are the only ones worth addressing before the next feature iteration, as they will compound with every new endpoint added.
