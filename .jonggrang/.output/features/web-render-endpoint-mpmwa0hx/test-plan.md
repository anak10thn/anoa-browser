# Test Plan: Web Render Endpoints

**Feature**: web-render-endpoint-mpmwa0hx  
**Phase**: 13 — test-planning  
**Date**: 2026-05-27

---

## Scope

Tests cover the six render-family routes added to `src/http/http_server.cpp`:

| ID | Route | Method | Description |
|----|-------|--------|-------------|
| RND-01–04 | `GET /render/screenshot.png` | GET | PNG frame capture |
| RND-05–08 | `GET /render/html` | GET | Live DOM HTML |
| RND-09–15 | `POST /render/navigate` | POST | URL navigation |
| RND-16–19 | `GET /render` | GET | Polling viewer page |
| RND-20–21 | `GET /render/` | GET | Trailing-slash redirect |
| RND-22–24 | `GET /render/stream.mjpeg` | GET | MJPEG stream |
| RND-25–28 | Auth on render routes | — | Token enforcement |

---

## Test Infrastructure

Reuse `tests/integration/helpers.js` (`startBrowser`, `stopBrowser`, `BASE_URL`).  
Test file: `tests/integration/render_endpoints.test.js`  
Framework: **vitest** (same as rest of integration suite).

---

## Test Cases

### Screenshot endpoint — GET /render/screenshot.png

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-01 | Returns 200 with PNG content | `GET /render/screenshot.png` (no auth) | Status 200, `Content-Type: image/png` |
| RND-02 | Response body starts with PNG magic bytes | `GET /render/screenshot.png` | Body[0..3] === `\x89PNG` |
| RND-03 | No-cache header present | `GET /render/screenshot.png` | `Cache-Control: no-cache` |
| RND-04 | Two successive requests return different sizes (live content) | Two sequential GETs after navigation | Both bodies > 0 bytes; test is non-deterministic, just assert both > 0 |

### HTML endpoint — GET /render/html

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-05 | Returns 200 with HTML content | `GET /render/html` | Status 200, `Content-Type: text/html; charset=utf-8` |
| RND-06 | Body contains valid HTML structure | Body text | Contains `<html` and `</html>` |
| RND-07 | No-cache header present | Response headers | `Cache-Control: no-cache` |
| RND-08 | Body is non-empty | Body length | > 0 |

### Navigate endpoint — POST /render/navigate

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-09 | Valid HTTP URL via query string returns 200 | `POST /render/navigate?url=http://example.com` | Status 200, body `navigating` |
| RND-10 | Valid HTTPS URL via query string returns 200 | `POST /render/navigate?url=https://example.com` | Status 200 |
| RND-11 | Valid URL via request body returns 200 | POST body: `https://example.com`, Content-Type text/plain | Status 200, body `navigating` |
| RND-12 | Missing URL returns 400 | `POST /render/navigate` (no url param, empty body) | Status 400, body `invalid url` |
| RND-13 | Relative URL returns 400 | `POST /render/navigate?url=relative/path` | Status 400, body `invalid url` |
| RND-14 | Disallowed scheme (javascript:) returns 400 | `POST /render/navigate?url=javascript:alert(1)` | Status 400, body `scheme not allowed` |
| RND-15 | Disallowed scheme (ftp:) returns 400 | `POST /render/navigate?url=ftp://example.com` | Status 400, body `scheme not allowed` |

### Live viewer — GET /render

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-16 | Returns 200 with HTML page | `GET /render` | Status 200, `Content-Type: text/html` |
| RND-17 | Page references screenshot URL | Body text | Contains `/render/screenshot.png` |
| RND-18 | When auth configured, screenshot URL includes token | `GET /render` with auth token | Body contains `token=<authToken>` in screenshot URL |
| RND-19 | No-cache header present | Response headers | `Cache-Control: no-cache` |

### Trailing-slash redirect — GET /render/

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-20 | Redirects to /render | `GET /render/` | Status 301, `Location: /render` |
| RND-21 | Redirect preserves query string | `GET /render/?token=abc` | `Location: /render?token=abc` |

### MJPEG stream — GET /render/stream.mjpeg

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-22 | Returns 200 with multipart content type | `GET /render/stream.mjpeg` | Status 200, `Content-Type: multipart/x-mixed-replace; boundary=frame` |
| RND-23 | Stream begins with MJPEG boundary marker | First bytes received before disconnect | Chunk contains `--frame` |
| RND-24 | No Content-Length in stream headers | Response headers | No `Content-Length` header |

### Auth enforcement on render routes

| ID | Title | Input | Expected |
|----|-------|-------|----------|
| RND-25 | Unauthenticated request to /render/screenshot.png returns 401 | `GET /render/screenshot.png` (token configured, no token sent) | Status 401 |
| RND-26 | Bearer token on render route returns 200 | `GET /render/screenshot.png` with `Authorization: Bearer <token>` | Status 200 |
| RND-27 | ?token= on render route returns 200 | `GET /render/screenshot.png?token=<token>` | Status 200 |
| RND-28 | Wrong token on render route returns 401 | `GET /render/screenshot.png`, Authorization: Bearer wrong | Status 401 |

---

## Non-Goals / Out of Scope

- Load testing / throughput benchmarks
- Multi-tab or multi-browser scenarios
- Interactive control (clicks, scrolls) — render endpoints are read-only
- Viewport size negotiation
- MJPEG decode correctness (only checks boundary framing)

---

## Test File

`tests/integration/render_endpoints.test.js`
