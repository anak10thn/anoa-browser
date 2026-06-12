---
feature: anoa-headless-browser-cdp
branch: feat/anoa-headless-browser-cdp
work_type: LARGE
description: Headless browser built on Qt6/QWebEngine with full CDP support via remote debugging passthrough and a Node.js/TypeScript process manager
created_at: 2026-05-27T00:00:00.000Z
depth: thorough
phase: 11
---

# Code Quality Review — http_server.cpp

## Executive Summary

Reviewed `src/http/http_server.cpp` and `src/http/http_server.h` for code organization, duplication, error handling, maintainability, and correctness. **15 distinct issues identified** across critical, high, and medium priority tiers. Primary concern: **monolithic 391-line function** containing 7 route handlers; significant duplication in socket cleanup (12 instances) and response construction (7+ patterns); **security gap** in request body validation.

---

## Critical Issues

### C1 — Monolithic `handleNewConnection()` Function (Lines 65–456)
**Severity**: CRITICAL  
**Impact**: Untestable, unmaintainable, difficult to debug

- Function contains **391 lines** with 7 distinct route handlers:
  - CDP discovery proxy (lines 155–201)
  - Screenshot PNG response (lines 202–239)
  - HTML capture (lines 240–289)
  - Navigation handler (lines 290–353)
  - Live view HTML (lines 354–397)
  - MJPEG stream (lines 398–451)
  - 404 Not Found (lines 453–455)
- Cyclomatic complexity **≈ 12+**; violates single responsibility principle
- **Fix**: Extract each route into separate private method

### C2 — Unbounded Request Body Read (Lines 296–303)
**Severity**: CRITICAL (Security/DOS)  
**Impact**: Denial of service; unbounded memory allocation

- `Content-Length` header parsed without validation
- **No maximum size enforcement** — malicious client can request 1GB+ Content-Length
- No check for **negative values**
- **Fix**: Add validation with MAX_BODY_SIZE constant (recommend 1MB)

### C3 — Null Pointer Dereference (Line 341)
**Severity**: CRITICAL (Crash Risk)  
**Impact**: Crash on navigation when `m_browser == nullptr`

- Line 341 calls `m_browser->load(parsedUrl)` without null check
- All other routes guard with `if (m_browser)` before dereference
- **Fix**: Add `if (m_browser)` guard before `load()` call

---

## High Priority Issues

### H1 — Socket Cleanup Pattern Duplication (12 instances)
**Severity**: HIGH  
**Impact**: Maintenance burden; error-prone if cleanup needs change

**Lines**: 54–55, 75–77, 86–88, 146–147, 224–225, 237–238, 273–274, 287–288, 318–319, 335–336, 352–353, 396–397

Every error/completion path manually calls:
```cpp
socket->disconnectFromHost();
socket->deleteLater();
```

**Fix**: Create `void closeSocket(QTcpSocket *socket)` helper and replace all 12 instances

### H2 — Response Construction Duplication (7+ patterns)
**Severity**: HIGH  
**Impact**: Inconsistent error handling; hard to maintain

| Pattern | Lines | Count |
|---------|-------|-------|
| `sendResponse()` helper | 54, 201, 454 | 3 |
| Inline 503 Service Unavailable | 216–225, 265–274 | 2 |
| Inline 400 Bad Request | 309–320, 326–337 | 2 |
| Inline 504 Gateway Timeout | 265–274 | 1 |
| Inline 200 OK + PNG | 227–234 | 1 |
| Inline 200 OK + HTML | 386–393 | 1 |

**Fix**: Consolidate into `sendResponse()` overloads covering all content types

### H3 — Bearer Token Parsing with Magic Number (Line 120)
**Severity**: HIGH (Logic Bug)  
**Impact**: Fragile; assumes "Bearer " is exactly 7 chars; no whitespace handling

- Magic number `7` = strlen("Bearer ")
- Doesn't trim whitespace: `"Bearer  token"` fails
- **Fix**: Extract token and trim before comparison

### H4 — Duplicate 400 Bad Request Handlers (Lines 309–320, 326–337)
**Severity**: HIGH  
**Impact**: Code duplication; maintenance burden

Two nearly-identical error responses for different validation failures.

**Fix**: Use `sendResponse()` helper for both cases

### H5 — Port Parsing Edge Cases (Lines 177–183)
**Severity**: HIGH (Robustness)  
**Impact**: Breaks on IPv6 addresses; no port range validation

- **IPv6 addresses**: `"[::1]:8080"` breaks; `lastIndexOf(':')` finds wrong position
- **No port range validation**: Should verify 0–65535
- **Fix**: Use QUrl parsing for robust host/port extraction

---

## Medium Priority Issues

### M1 — Magic Numbers Without Named Constants
**Severity**: MEDIUM  
**Impact**: Unclear intent; difficult to configure

| Value | Lines | Purpose |
|-------|-------|---------|
| `5000` | 74, 169, 249, 299 | Timeout (ms) |
| `100` | 413 | MJPEG frame interval (ms) |
| `512 * 1024` | 418 | Write buffer backpressure threshold |
| `70` | 430 | JPEG quality (0–100) |
| `500` | 380 | Live view refresh interval (ms) |

**Fix**: Create `HttpServerConfig.h` with named constants

### M2 — Hardcoded Route Paths (Lines 155–157, 203, 240, 290, 354, 399)
**Severity**: MEDIUM  
**Impact**: Scattered string literals; no single source of truth

Routes: `/json`, `/json/list`, `/json/version`, `/render/screenshot.png`, `/render/html`, `/render/navigate`, `/render`, `/render/stream.mjpeg`

**Fix**: Create route path constants/enum

### M3 — String Literal Inconsistency
**Severity**: MEDIUM  
**Impact**: No clear pattern; inconsistent with Qt best practices

Mixed use of `QLatin1String`, `QStringLiteral`, `QString::fromUtf8` with no consistent pattern.

**Fix**: Adopt single pattern for each use case

### M4 — Incomplete URL Scheme Validation (Lines 324–325)
**Severity**: MEDIUM (Security)  
**Impact**: Whitelist approach incomplete; doesn't reject all dangerous schemes

**Fix**: Use strict whitelist with only http, https, file

### M5 — Under-Commented Magic Values
**Severity**: MEDIUM  
**Impact**: Future maintainers don't understand rationale

Missing comments explaining timeout values, JPEG quality, MJPEG interval choices, and error handling strategy.

**Fix**: Add justification comments for all configuration constants

### M6 — MJPEG Lambda Race Condition (Line 416)
**Severity**: MEDIUM (Race Condition)  
**Impact**: Potential null pointer if `m_browser` deleted during stream

**Fix**: Capture weak reference or document lifetime assumptions

### M7 — HtmlCaptureState Struct Scope (Lines 58–63)
**Severity**: MEDIUM (Code Organization)  
**Impact**: File-scope struct only used once; not reusable

**Fix**: Move into function scope or consolidate state handling

### M8 — Hardcoded HTML Template (Lines 362–383)
**Severity**: MEDIUM (Maintainability)  
**Impact**: Embedded HTML makes styling/localization difficult

**Fix**: Move to separate resource file

---

## Summary by Count

| Priority | Category | Count |
|----------|----------|-------|
| **CRITICAL** | Monolithic function, unbounded body read, null dereference | 3 |
| **HIGH** | Socket cleanup (12), response duplication (7+), token parsing, duplicate handlers, port parsing | 5 |
| **MEDIUM** | Magic numbers (6), hardcoded paths (8), string inconsistency, validation, comments, lambda race, struct scope, template | 8 |

**Total: 15 distinct issues identified**

---

## Recommended Refactoring Priority

### Phase 1: Security & Stability (Critical)
1. Add request body size validation (C2)
2. Add null check for `m_browser->load()` (C3)
3. Extract socket cleanup helper (H1)
4. Fix Bearer token parsing (H3)

### Phase 2: Code Quality (High)
5. Extract route handlers into separate methods (C1)
6. Consolidate response construction (H2)
7. Fix port parsing (H5)

### Phase 3: Maintainability (Medium)
8. Define constants and enums (M1, M2)
9. Standardize string literals (M3)
10. Add missing comments (M5)
11. Refactor code organization (M7, M8)

