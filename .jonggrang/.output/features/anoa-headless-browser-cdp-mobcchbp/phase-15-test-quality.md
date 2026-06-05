---
feature: anoa-headless-browser-cdp
branch: feat/anoa-headless-browser-cdp
work_type: LARGE
description: Headless browser built on Qt6/QWebEngine with full CDP support via remote debugging passthrough and a Node.js/TypeScript process manager
created_at: 2026-04-23T10:21:22.832Z
depth: deep
phase: 15
---

# Phase 15 — Test Quality Report

## Summary

Audited all 11 test suites (111 test cases). Identified and fixed 5 low-value or under-specified tests. No tests were deleted — all changes strengthen existing tests by replacing "no crash" waits or field-existence checks with behaviorally meaningful assertions.

## Changes Made

### cdp_proxy.test.js

**WS-08** — "Malformed JSON message does not crash the proxy"
- **Before**: Sent malformed JSON, waited 500 ms, resolved. Only verified the process didn't crash.
- **After**: After the malformed frame, sends a valid `Browser.getVersion` command and asserts the response returns `id: 888` and `result.Browser`. Verifies the connection is functionally intact, not just alive.

**WS-09** — "Message without id field is forwarded without crash"
- **Before**: Sent `{ method: 'Browser.getVersion' }` (no id), waited 500 ms. Only verified no crash.
- **After**: After the id-less message, sends a properly formed `Browser.getVersion` command and asserts the response is correct. Verifies proxy routing state is not corrupted.

**WS-11** — "Large message (>64 KB) is forwarded correctly"
- **Before**: Comment said "we just need no crash" and only asserted `response.id === 42`.
- **After**: Asserts `result.result.type === 'string'` and `result.result.value.length === bigString.length`. Verifies the full payload round-trips correctly through the proxy, not just that it doesn't hang.

### http_server.test.js

**HTTP-03** — "GET /json/version returns Browser and webSocketDebuggerUrl fields"
- **Before**: `toHaveProperty('Browser')` and `toHaveProperty('webSocketDebuggerUrl')` — only checks key existence.
- **After**: `typeof body.Browser === 'string'` + `.length > 0` + `webSocketDebuggerUrl.match(/^ws:\/\//)`. Verifies the Browser field is a real string and the URL uses the correct WebSocket scheme.

### cdp_extensions.test.js

**EXT-19** — "Unknown domain (DOM.getDocument) is forwarded to Chromium (not stubbed)"
- **Before**: Only asserted `resp.id === id` — no check that Chromium actually replied.
- **After**: Added `resp.result !== undefined || resp.error !== undefined` assertion. Verifies the proxy does not swallow passthrough responses from Chromium.

## Tests Not Changed

- **EXT-LOAD-01 / EXT-LOAD-03** (extensions.test.sh): "No crash" pattern retained because these shell tests run sequentially and lack a WebSocket helper. EXT-LOAD-04 already covers the functional-after-load assertion in the same suite.
- All other tests: assertions were already meaningful (PDF header bytes, exact JS eval values, port listening via nc -z, cookie value round-trip, etc.).

## Verdict

No low-value tests remain in the integration layer. The 5 modified tests now verify observable behavior rather than process survival or key existence.
