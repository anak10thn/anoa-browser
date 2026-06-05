# Phase 14 — Testing: Web Render Endpoints

**Date**: 2026-05-27  
**Feature**: web-render-endpoint-mpmwa0hx  
**Status**: COMPLETED — all tests pass

---

## Summary

All 27 integration tests in `tests/integration/render_endpoints.test.js` passed.  
Test IDs covered: RND-01–03, RND-05–28 (RND-04 intentionally excluded as non-deterministic).

---

## Test Run

**Command**:
```bash
ANOA_BINARY=build/anoa-browser.app/Contents/MacOS/anoa-browser \
  npx vitest run --reporter=verbose render_endpoints
```

**Result**: 27 passed, 0 failed, duration ~764 ms

---

## Test Cases Passed

### No-auth suite (22 tests)
| ID | Test | Result |
|----|------|--------|
| RND-01 | GET /render/screenshot.png → 200 image/png | ✓ |
| RND-02 | PNG magic bytes \x89PNG | ✓ |
| RND-03 | Cache-Control: no-cache on screenshot | ✓ |
| RND-05 | GET /render/html → 200 text/html | ✓ |
| RND-06 | HTML body contains `<html>` structure | ✓ |
| RND-07 | Cache-Control: no-cache on html | ✓ |
| RND-08 | HTML body non-empty | ✓ |
| RND-09 | POST /render/navigate?url=http://… → 200 "navigating" | ✓ |
| RND-10 | POST /render/navigate?url=https://… → 200 | ✓ |
| RND-11 | POST /render/navigate with URL in body → 200 | ✓ |
| RND-12 | POST /render/navigate no URL → 400 "invalid url" | ✓ |
| RND-13 | POST /render/navigate relative URL → 400 | ✓ |
| RND-14 | POST /render/navigate javascript: → 400 "scheme not allowed" | ✓ |
| RND-15 | POST /render/navigate ftp: → 400 "scheme not allowed" | ✓ |
| RND-16 | GET /render → 200 text/html | ✓ |
| RND-17 | /render body references /render/screenshot.png | ✓ |
| RND-19 | Cache-Control: no-cache on /render | ✓ |
| RND-20 | GET /render/ → 301 Location: /render | ✓ |
| RND-21 | GET /render/?token=abc → 301 Location: /render?token=abc | ✓ |
| RND-22 | GET /render/stream.mjpeg → 200 multipart/x-mixed-replace | ✓ |
| RND-23 | Stream starts with --frame boundary | ✓ |
| RND-24 | No Content-Length on stream | ✓ |

### With-auth suite (5 tests)
| ID | Test | Result |
|----|------|--------|
| RND-25 | No token → 401 | ✓ |
| RND-26 | Bearer token → 200 | ✓ |
| RND-27 | ?token= → 200 | ✓ |
| RND-28 | Wrong token → 401 | ✓ |
| RND-18 | /render with auth embeds token in screenshot URL | ✓ |

---

## Infrastructure Notes

- On macOS the build produces `build/anoa-browser.app/Contents/MacOS/anoa-browser` (app bundle), not a flat `build/anoa-browser`. Set `ANOA_BINARY` env var to override the default path in `helpers.js`.
- The `Skia Graphite backend not found` errors in stderr are benign warnings from the Chromium engine; they do not affect functionality.
- `node-fetch` deprecation warning for `response.buffer()` is cosmetic; tests still pass. Migrate to `response.arrayBuffer()` in a future cleanup.
