---
feature: anoa-headless-browser-cdp
branch: feat/anoa-headless-browser-cdp
work_type: LARGE
description: Headless browser built on Qt6/QWebEngine with full CDP support via remote debugging passthrough and a Node.js/TypeScript process manager
created_at: 2026-04-23T10:21:22.832Z
depth: deep
phase: 11
---

# Code Quality Report — Phase 11
Feature: anoa-headless-browser-cdp  
Date: 2026-04-23

## Critical (Security / Crash Risk)

### C1 — Null pointer dereference on failed JSON parse (`cdp_proxy.cpp`)
`QJsonDocument::fromJson(...).object()` silently returns empty object on invalid JSON. The `id` and `method` fields are then read from an empty object — this won't crash but will mismatch all method routing and produce confusing behavior. Add a parse-success check before using the object.

### C2 — Race condition in `AnoaBrowser::getCookies()` (`anoa_browser.cpp`)
A hardcoded 500 ms `QTimer::singleShot` fires before the cookie-load signal regardless of whether the async load has finished. Under load or on slow storage, the list will be silently truncated. Use a connection to the actual `allCookiesLoaded` signal instead.

### C3 — TOCTOU race in `PdfHandler::handlePrintToPdf()` (`pdf_handler.cpp`)
`QTemporaryFile` is opened, the path is read, then the file is closed so `printToPdf()` can overwrite it. Between close and overwrite, another process can create a file at the same path. Use `QTemporaryFile::fileName()` with `setAutoRemove(false)` and pass the open fd path, or keep the file open and truncate after write.

### C4 — No request-size limit in `HttpServer::handleNewConnection()` (`http_server.cpp`)
The read loop accumulates bytes until `\r\n\r\n` is found with no upper bound. A malicious or buggy client can cause OOM. Add a max header size guard (e.g. 8 KiB).

### C5 — `setupNamedProfile()` use-after-deleteLater (`anoa_browser.cpp`)
`deleteLater()` schedules deletion on the next event loop turn. If any synchronous code accesses `m_page` or `m_profile` after the call returns but before the event loop runs, it touches a freed object. Null-assign both pointers immediately after `deleteLater()`.

---

## High (Reliability / Maintainability)

### H1 — Duplicated bearer token authentication logic (`cdp_proxy.cpp`, `http_server.cpp`)
The `Authorization: Bearer` + `?token=` extraction pattern appears in both files. Extract to a free function `QString extractBearerToken(const QHttpHeaders &headers, const QUrl &url)` in a shared utility header.

### H2 — Blocking event loop in `HttpServer` (`http_server.cpp`)
`socket->waitForReadyRead(5000)` and the upstream `QNetworkAccessManager` + `QEventLoop` both block the main thread. Any slow Chromium response stalls all other Qt event processing. Convert to fully async signal/slot pattern with a per-request timeout `QTimer`.

### H3 — No upstream WebSocket connection timeout in `CdpProxy` (`cdp_proxy.cpp`)
After `upstream->open(upstreamUrl)`, there is no timeout. If Chromium DevTools is not listening, client connections succeed against the proxy but all CDP commands silently disappear. Add a `QTimer` that disconnects the client with an error if the upstream does not connect within 5 s.

### H4 — Bidirectional map has no encapsulation (`cdp_proxy.cpp`)
`m_clientToUpstream` and `m_upstreamToClient` must stay consistent but are updated at 4+ call sites. One missing update will silently route messages to the wrong socket. Wrap in a small `SessionMap` helper with `insert`, `remove`, and `lookup` methods that always update both sides.

### H5 — Magic timeouts and constants throughout
At minimum, define these as `constexpr` in the relevant header or a `constants.h`:
- `500` ms cookie-load timeout (`anoa_browser.cpp`)
- `30000` ms PDF timeout (`pdf_handler.cpp`)
- `5000` ms HTTP read timeout (`http_server.cpp`)
- `9222` default port (`config.h`, `config.cpp`)
- `"__anoa_default__"` synthetic context ID (`cdp_extensions.cpp`)

### H6 — `exit(1)` in `config.cpp` bypasses Qt cleanup
`QApplication` destructors and pending `deleteLater()` calls are skipped. Replace with structured error return (`std::optional<Config>` or exception) so `main()` can call `return 1` after proper cleanup.

### H7 — `CdpExtensions` inherits `QObject` but has only static methods (`cdp_extensions.h`)
The `Q_OBJECT` macro is unused (no signals or slots), and there is no instance state. Remove `QObject` inheritance and the `Q_OBJECT` macro. If signals are added later, re-add them then.

---

## Medium (Code Quality)

### M1 — Unused parameters in `AnoaBrowser` public API (`anoa_browser.h`)
`getCookies(const QUrl &origin)` and `clearStorage(const QUrl &origin)` both ignore the `origin` parameter. Either implement per-origin filtering (the expected semantics) or remove the parameter from the signature.

### M2 — Extension manifest parsing without key-existence checks (`anoa_browser.cpp`)
`manifest["manifest_version"].toInt()` and similar accesses silently return 0/empty on missing keys. Use `manifest.contains("manifest_version")` guards or the `.value()` overload with an explicit default to make failures visible.

### M3 — Fragile JSON rewriting in `HttpServer` (`http_server.cpp`)
String-replacing `"127.0.0.1"` in the raw JSON body will corrupt any field whose *value* contains that string. Parse the JSON, mutate the relevant fields, and re-serialize instead.

### M4 — Upstream connection state not verified before sending (`cdp_proxy.cpp`)
Messages from clients are forwarded to `upstream` without checking `upstream->state() == QAbstractSocket::ConnectedState`. If the upstream dropped, messages are silently discarded. Add a state check and return an error response to the client.

### M5 — Bearer token minimum-length validation absent (`config.cpp`)
The code warns on empty token but accepts any non-empty string. A 1-character token is trivially guessable. Add a minimum length check (suggest ≥ 16 characters) and log a warning for short tokens.

---

## Low (Style / Clarity)

### L1 — Port offset calculation undocumented in `main.cpp`
The three-port layout (HTTP = port, Chromium DevTools = port+1, WS proxy = port+2) is mentioned in AGENTS.md but not in the code. Add a single comment block at the top of `main()` explaining the layout.

### L2 — Inconsistent null-checking patterns across files
Some call sites check `m_page != nullptr` before use; others do not. Establish a pattern: assert in debug builds (`Q_ASSERT(m_page)`), check in release builds only where the condition can legitimately be false.

### L3 — No domain-level fallthrough logging in `CdpExtensions::processCommand()` (`cdp_extensions.cpp`)
Unknown domains return an empty string (triggering passthrough), with no debug log. A `qDebug()` line for unrecognized domains would make protocol debugging faster.

### L4 — `8.5, 11.0, 0.4` paper/margin defaults in `pdf_handler.cpp`
These are US Letter inches. They are not self-documenting. Extract as named constants: `PDF_DEFAULT_WIDTH_INCH`, `PDF_DEFAULT_HEIGHT_INCH`, `PDF_DEFAULT_MARGIN_INCH`.

---

## Verdict

The implementation is functionally complete and passes Phase 10 integration tests. The critical and high items above should be addressed before a production release — most are low-effort (add a constant, extract a function, add a guard). No architectural changes are needed; all findings are localized fixes.

**Recommended next tasks (priority order):**
1. Fix C2 (cookie race) — real data loss risk
2. Fix C4 (HTTP request size limit) — security boundary
3. Fix H1 (dedup auth logic) — reduces maintenance surface
4. Fix H3 (upstream WS timeout) — user-visible silent failure
5. Fix H4 (SessionMap encapsulation) — prevents subtle routing bugs
6. Address all H5 magic constants in one pass
