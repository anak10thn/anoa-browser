---
feature: web-render-endpoint
feature_id: web-render-endpoint-mpmwa0hx
branch: feat/web-render-endpoint
work_type: MEDIUM
description: HTTP endpoints to view the browser's rendered page output (screenshot, HTML, live view) in any web browser
created_at: 2026-05-26T15:58:43.724Z
---

# Plan: Web Render Endpoint

## Approach
Extend `HttpServer` with a new `/render/*` route family that exposes the live `AnoaBrowser`/`QWebEngineView` output over plain HTTP. The HTTP server gets a non-owning pointer to the `AnoaBrowser` instance, then uses Qt's existing APIs (`QWebEngineView::grab()` or `QWidget::grab()` for PNG, `page()->toHtml()` for HTML, `page()->printToPdf()` for PDF) to capture frames on demand. A small static HTML viewer page (`/render` or `/view`) polls `/render/screenshot.png` on an interval — or consumes a multipart MJPEG stream — so the rendered page can be embedded in any browser without a CDP client.

## Phases
1. Wire `AnoaBrowser` into `HttpServer` — pass pointer from `main.cpp`, add accessor methods for capture
2. PNG screenshot endpoint — `GET /render/screenshot.png` returning current frame as PNG (sync via `QWebEngineView::grab`)
3. HTML source endpoint — `GET /render/html` returning rendered DOM via `page()->toHtml()` (async with callback bridging)
4. Navigation control — `POST /render/navigate?url=...` to load a URL into the embedded view
5. Live viewer page — `GET /render` serves an HTML page that auto-refreshes the screenshot (img tag with polling JS)
6. Optional MJPEG stream — `GET /render/stream.mjpeg` multipart frame stream for smoother live view
7. Documentation — update README with new endpoints and a usage example

## Key Decisions
- **Polling over WebSocket for v1**: simpler to implement and consume from any browser via plain `<img>` tag. MJPEG stream is optional follow-up.
- **Reuse `HttpServer`, not a new server**: keeps port layout (`N`, `N+1`, `N+2`) intact; auth token + CORS handling stay centralized.
- **Async HTML via QEventLoop blocking pattern**: matches the existing pattern used for CDP discovery passthrough in `http_server.cpp`.
- **Render endpoints honor `--token`**: same Bearer/`?token=` auth as the CDP endpoints — no separate auth path.
- **Capture from the existing `QWebEngineView`, not a headless render-only pipeline**: avoids spinning up a second profile and reuses whatever the user has loaded via CDP/`--url`.

## Out of Scope
- Multi-tab rendering (Qt+CDP limitation already documented — only one page exists)
- Recording/replay of render sessions
- Interactive control from the viewer page (clicks, scrolls) — read-only view first
- Authentication UI / login form for the viewer page (token still required via query string)
- Resolution/viewport size negotiation per request (uses current view size)
- Caching headers / ETag — endpoints are explicitly no-cache

## Dependencies
Builds on existing components:
- `HttpServer` (`src/http/http_server.cpp`) — request parsing, auth, routing skeleton already in place
- `AnoaBrowser` / `QWebEngineView` (`src/browser/anoa_browser.h`) — provides the rendered surface and `QWebEnginePage`
- `pdf/` subsystem — pattern for async page capture via Qt callback already used for `Page.printToPDF`
- Qt6 modules already linked: WebEngineWidgets, WebEngineCore, Network — no new dependencies required
