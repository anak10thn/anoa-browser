# Phase 16 — Test Quality: Web Render Endpoints

**Feature**: web-render-endpoint-mpmwa0hx  
**Phase**: 16 — test-quality  
**Date**: 2026-05-27

---

## Summary

**Status**: PASSED — 3 issues found and fixed.

---

## Issues Found and Fixed

### 1. RND-08 removed — redundant low-value test

**File**: `tests/integration/render_endpoints.test.js`

**Problem**: `expect(body.length).toBeGreaterThan(0)` is trivially subsumed by RND-06
(`body contains <html>` and `</html>`). A body that passes RND-06 is necessarily
non-empty. RND-08 has no independent failure mode and adds noise without coverage value.

**Fix**: Removed the RND-08 `it(...)` block entirely.

---

### 2. RND-22 assertion strengthened — boundary parameter was unchecked

**File**: `tests/integration/render_endpoints.test.js`, RND-22

**Problem**: The original assertion was:
```js
expect(resp.headers.get('content-type')).toMatch(/multipart\/x-mixed-replace/);
```
This only verifies the MIME type, not the `boundary=frame` parameter required by the
MJPEG protocol. A response with `Content-Type: multipart/x-mixed-replace; boundary=WRONG`
would pass silently. The test plan explicitly required `boundary=frame`.

**Fix**:
```js
expect(resp.headers.get('content-type')).toMatch(/multipart\/x-mixed-replace.*boundary=frame/);
```
The implementation (`http_server.cpp:402`) sends exactly `multipart/x-mixed-replace; boundary=frame`,
so this assertion is correct.

---

### 3. Deprecated `response.buffer()` replaced — RND-02

**File**: `tests/integration/render_endpoints.test.js`, RND-02

**Problem**: `node-fetch`'s `response.buffer()` is deprecated (flagged in phase 14 notes).
It is not part of the Fetch API standard and will be removed in a future release.

**Fix**:
```js
// Before (deprecated):
const buf = await resp.buffer();

// After (standard):
const buf = Buffer.from(await resp.arrayBuffer());
```
`Buffer.from(arrayBuffer)` produces a Node.js `Buffer` with the same byte-indexing
semantics, so the `buf[0]` through `buf[3]` assertions are unchanged.

---

## Test Count After Changes

| Before | After | Delta |
|--------|-------|-------|
| 27 tests | 26 tests | −1 (RND-08 removed) |

The 26 remaining tests cover all planned, deterministic scenarios. Coverage is unaffected
because RND-08 was a subset of RND-06.

---

## No Other Issues Found

All remaining assertions were verified against the implementation in `src/http/http_server.cpp`:

| Check | Assertion type | Implementation match |
|-------|---------------|---------------------|
| Status codes (200, 301, 400, 401, 503) | `.toBe(N)` | Exact |
| Content-Type headers | `.toMatch(regex)` | Regex matches server-sent values |
| Cache-Control headers | `.toBe('no-cache')` | Exact string sent by server |
| PNG magic bytes | `buf[N].toBe(0xNN)` | PNG format guarantee |
| Redirect Location header | `.toBe('/render')` / `.toBe('/render?token=abc')` | Exact, verified vs C++ logic |
| Navigate body text | `.toBe('navigating')` / `.toBe('invalid url')` / `.toBe('scheme not allowed')` | Exact strings from server |
| Token embedding in viewer HTML | `.toContain('token=<token>')` | QUrlQuery produces matching output |
| MJPEG boundary marker | `text.includes('--frame')` | Matches frame part format |
| No Content-Length on stream | `.toBeNull()` | Confirmed absent in stream headers |
