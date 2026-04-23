# Test Plan — anoa-browser (anoa-headless-browser-cdp)

**Feature**: anoa-headless-browser-cdp  
**Phase**: 12 — test-planning  
**Date**: 2026-04-23  
**Stack**: Qt6/QWebEngine (C++17), CMake ≥ 3.16  
**Binary**: `anoa-browser` — single self-contained executable, no Node.js required

---

## 1. Scope

This plan covers validation of all 10 completed implementation tasks for the anoa-browser headless browser with full CDP support. It defines test suites, test cases, tooling, and CI/CD integration required for production-level confidence.

**In scope:**
- Config parsing & validation
- HTTP discovery endpoints (`/json`, `/json/version`, `/json/list`)
- WebSocket CDP proxy (session multiplexing, bearer token auth)
- CDP domain extensions (Profiler, HeapProfiler, Security, Browser, Target stubs)
- PDF generation via `Page.printToPDF`
- Browser profile isolation & cookie/storage CRUD
- Extension loading (manifest v2)
- Port layout enforcement (3-port: HTTP, Chromium internal, WS proxy)
- Playwright / Puppeteer connectivity (compatibility)
- GitHub Actions release pipeline

**Out of scope:**
- Manifest v3 extension support (documented gap)
- `Target.createTarget` (unsupported by QtWebEngine, documented gap)
- V8 Profiler / HeapProfiler actual data (stubs return `{}`, not real profiles)
- Download management (stubbed)

---

## 2. Test Strategy

### Approach

Given the C++ Qt binary with no formal test framework configured, the strategy is layered:

| Layer | Tool | When | Purpose |
|-------|------|------|---------|
| Unit | Qt Test Framework (`QTest`) | CI (every PR) | Isolate C++ logic, no Qt event loop required |
| Integration | Node.js (`ws` + `node-fetch`) | CI (every PR) | Verify HTTP + WebSocket protocol |
| Compatibility | Playwright + Puppeteer | CI (every PR, nightly) | End-to-end browser automation |
| Regression | Shell scripts | CI (tag builds) | Smoke test the release binary before publishing |
| Manual | Developer | Before release | Edge cases, platform-specific behavior |

### Key Constraints

- **No Node.js dependency in the binary** — test drivers (Node.js scripts) are dev-only and never bundled
- **3-port layout**: tests must use port 9222 (HTTP), 9223 (internal), 9224 (WS)
- **`Target.createTarget` not supported**: Playwright tests must use `browser.contexts()[0].pages()[0]` — never `browser.newPage()`
- **Startup navigation required**: binary calls `load(about:blank)` in `init()` — tests must wait for the page to be ready before sending CDP commands
- **GPU not available on CI**: always run with `--no-sandbox` on headless CI
- **macOS strip**: use Release build for binary size tests; Debug builds retain symbols

---

## 3. Test Suites

### Suite 1 — Config Parsing Unit Tests

**Framework**: `QTest` (C++, no event loop needed)  
**File**: `tests/unit/test_config.cpp`  
**Build target**: `anoa-browser-test-config`

| ID | Test Case | Input | Expected |
|----|-----------|-------|----------|
| CFG-01 | Default port | (no args) | port == 9222 |
| CFG-02 | Custom port | `--port 8080` | port == 8080 |
| CFG-03 | Invalid port — zero | `--port 0` | exit code 1, error to stderr |
| CFG-04 | Invalid port — too large | `--port 99999` | exit code 1, error to stderr |
| CFG-05 | Headless flag | `--headless` | headless == true |
| CFG-06 | No-sandbox flag | `--no-sandbox` | noSandbox == true |
| CFG-07 | Profile name | `--profile myprofile` | profile == "myprofile" |
| CFG-08 | Profile directory | `--profile-dir /tmp/p` | profileDir == "/tmp/p" |
| CFG-09 | Auth token | `--auth-token abc123` | authToken == "abc123" |
| CFG-10 | Multiple extensions | `--extension /p/a --extension /p/b` | extensions == ["/p/a", "/p/b"] |
| CFG-11 | JSON config file | `--config test.json` with `{"port":8081}` | port == 8081 |
| CFG-12 | CLI overrides config file | `--config test.json --port 9000` with `{"port":8081}` | port == 9000 |
| CFG-13 | Missing config file | `--config nonexistent.json` | exit code 1, error to stderr |
| CFG-14 | Malformed JSON config | `--config bad.json` with `{invalid}` | exit code 1 |

### Suite 2 — HTTP Server Integration Tests

**Framework**: Node.js + `node-fetch`  
**File**: `tests/integration/http_server.test.js`  
**Requires**: Running binary on port 9222

| ID | Test Case | Request | Expected |
|----|-----------|---------|----------|
| HTTP-01 | `/json/list` returns array | `GET /json/list` | HTTP 200, JSON array |
| HTTP-02 | `/json` same as `/json/list` | `GET /json` | HTTP 200, JSON array (same content) |
| HTTP-03 | `/json/version` structure | `GET /json/version` | HTTP 200, `Browser` + `webSocketDebuggerUrl` fields present |
| HTTP-04 | `webSocketDebuggerUrl` uses proxy port | `GET /json/version` | `webSocketDebuggerUrl` contains `localhost:9224` (not 9223) |
| HTTP-05 | Trailing slash `/json/version/` | `GET /json/version/` | HTTP 200 (same as without slash) |
| HTTP-06 | Unknown path | `GET /unknown` | HTTP 404 |
| HTTP-07 | Auth required — no token | `GET /json` (no header) with `--auth-token abc` | HTTP 401 |
| HTTP-08 | Auth via header | `GET /json` + `Authorization: Bearer abc` | HTTP 200 |
| HTTP-09 | Auth via query param | `GET /json?token=abc` | HTTP 200 |
| HTTP-10 | Wrong token rejected | `GET /json` + `Authorization: Bearer wrong` | HTTP 401 |
| HTTP-11 | No auth configured — no header needed | `GET /json` (no `--auth-token` flag) | HTTP 200 (no auth enforcement) |
| HTTP-12 | Content-Type JSON | `GET /json/version` | `Content-Type: application/json` header |

### Suite 3 — WebSocket CDP Proxy Tests

**Framework**: Node.js + `ws` library  
**File**: `tests/integration/cdp_proxy.test.js`  
**Requires**: Running binary on ports 9222–9224

| ID | Test Case | Action | Expected |
|----|-----------|--------|----------|
| WS-01 | Connect to proxy | `ws://localhost:9224` | Connection accepted |
| WS-02 | Auth via URL token | `ws://localhost:9224?token=abc` with `--auth-token abc` | Connection accepted |
| WS-03 | Auth via header | `ws://localhost:9224` + `Authorization: Bearer abc` | Connection accepted |
| WS-04 | Wrong token rejected | `ws://localhost:9224?token=wrong` | HTTP 401, connection refused |
| WS-05 | No token when required | `ws://localhost:9224` with `--auth-token abc` | HTTP 401 |
| WS-06 | Concurrent clients — 2 connections | Open 2 WS connections | Both accepted |
| WS-07 | Session isolation | Send `Runtime.evaluate` on client A + B simultaneously | Each receives its own response (no cross-contamination) |
| WS-08 | Malformed JSON ignored | Send `"not json"` | No crash, no response (or error response) |
| WS-09 | Missing `id` field | Send `{"method":"Browser.getVersion"}` | No crash; proxy forwards (Chromium may return error) |
| WS-10 | Upstream disconnect recovery | Kill Chromium port (simulate) | Proxy closes client cleanly, no crash |
| WS-11 | Large message forwarding | Send message > 64 KB | Correctly forwarded (no truncation) |
| WS-12 | Passthrough forwarding | `Browser.getVersion` | Response contains `{"id":1,"result":{"Browser":...}}` |

### Suite 4 — CDP Extension Stub Tests

**Framework**: Node.js + `ws`  
**File**: `tests/integration/cdp_extensions.test.js`

| ID | Test Case | Command Sent | Expected Response |
|----|-----------|-------------|-------------------|
| EXT-01 | Profiler.enable | `{"id":1,"method":"Profiler.enable","params":{}}` | `{"id":1,"result":{}}` |
| EXT-02 | Profiler.disable | `{"id":2,"method":"Profiler.disable","params":{}}` | `{"id":2,"result":{}}` |
| EXT-03 | Profiler.start | `{"id":3,"method":"Profiler.start","params":{}}` | `{"id":3,"result":{}}` |
| EXT-04 | Profiler.stop | `{"id":4,"method":"Profiler.stop","params":{}}` | `{"id":4,"result":{}}` |
| EXT-05 | Profiler.setSamplingInterval | `{"id":5,"method":"Profiler.setSamplingInterval","params":{"interval":100}}` | `{"id":5,"result":{}}` |
| EXT-06 | HeapProfiler.enable | `{"id":6,"method":"HeapProfiler.enable","params":{}}` | `{"id":6,"result":{}}` |
| EXT-07 | HeapProfiler.disable | `{"id":7,"method":"HeapProfiler.disable","params":{}}` | `{"id":7,"result":{}}` |
| EXT-08 | HeapProfiler.startTrackingHeapObjects | `{"id":8,"method":"HeapProfiler.startTrackingHeapObjects","params":{}}` | `{"id":8,"result":{}}` |
| EXT-09 | HeapProfiler.stopTrackingHeapObjects | `{"id":9,"method":"HeapProfiler.stopTrackingHeapObjects","params":{}}` | `{"id":9,"result":{}}` |
| EXT-10 | HeapProfiler.takeHeapSnapshot | `{"id":10,"method":"HeapProfiler.takeHeapSnapshot","params":{}}` | `{"id":10,"result":{}}` |
| EXT-11 | Security.enable | `{"id":11,"method":"Security.enable","params":{}}` | `{"id":11,"result":{}}` |
| EXT-12 | Security.disable | `{"id":12,"method":"Security.disable","params":{}}` | `{"id":12,"result":{}}` |
| EXT-13 | Security.setIgnoreCertificateErrors | `{"id":13,"method":"Security.setIgnoreCertificateErrors","params":{"ignore":true}}` | `{"id":13,"result":{}}` |
| EXT-14 | Browser.setDownloadBehavior | `{"id":14,"method":"Browser.setDownloadBehavior","params":{"behavior":"allow","downloadPath":"/tmp"}}` | `{"id":14,"result":{}}` |
| EXT-15 | Browser.getWindowForTarget | `{"id":15,"method":"Browser.getWindowForTarget","params":{"targetId":"x"}}` | `{"id":15,"result":{}}` |
| EXT-16 | Target.createBrowserContext | `{"id":16,"method":"Target.createBrowserContext","params":{}}` | `{"id":16,"result":{"browserContextId":"__anoa_default__"}}` |
| EXT-17 | Target.disposeBrowserContext | `{"id":17,"method":"Target.disposeBrowserContext","params":{"browserContextId":"__anoa_default__"}}` | `{"id":17,"result":{}}` |
| EXT-18 | Response contains correct `id` | Send multiple commands with different IDs | Each response has matching `id` |
| EXT-19 | Unknown domain passthrough | `{"id":19,"method":"DOM.getDocument","params":{}}` | Response forwarded from Chromium (not stubbed) |

### Suite 5 — Page.printToPDF Tests

**Framework**: Node.js + `ws`  
**File**: `tests/integration/pdf_handler.test.js`

| ID | Test Case | Params | Expected |
|----|-----------|--------|----------|
| PDF-01 | Default params | `{}` | `result.data` is non-empty base64 string |
| PDF-02 | Valid PDF header | Decode base64 → binary | Starts with `%PDF-` |
| PDF-03 | Landscape mode | `{"landscape":true}` | Valid PDF returned |
| PDF-04 | Custom paper size | `{"paperWidth":8.27,"paperHeight":11.69}` (A4 in inches) | Valid PDF returned |
| PDF-05 | Print background | `{"printBackground":true}` | Valid PDF returned |
| PDF-06 | Custom margins | `{"marginTop":0.5,"marginBottom":0.5,"marginLeft":0.5,"marginRight":0.5}` | Valid PDF returned |
| PDF-07 | Zero margins | `{"marginTop":0,"marginBottom":0,"marginLeft":0,"marginRight":0}` | Valid PDF returned |
| PDF-08 | Response has correct `id` | Send with `id: 42` | Response `id` == 42 |
| PDF-09 | PDF after page navigation | Navigate to a URL, then print | PDF contains rendered content (non-trivial file size) |
| PDF-10 | Concurrent PDF requests | Send 2 PDF requests concurrently | Both return valid PDFs (no race condition) |

### Suite 6 — Playwright Compatibility Tests

**Framework**: Playwright (`@playwright/test`)  
**File**: `tests/e2e/playwright.test.ts`  
**Note**: `browser.newPage()` is NOT supported — must use `browser.contexts()[0].pages()[0]`

| ID | Test Case | Steps | Expected |
|----|-----------|-------|----------|
| PW-01 | connectOverCDP succeeds | `chromium.connectOverCDP('http://localhost:9222')` | Connected, no error |
| PW-02 | connectOverCDP with auth token | `connectOverCDP('http://localhost:9222', {headers:{Authorization:'Bearer abc'}})` | Connected |
| PW-03 | Get existing page | `browser.contexts()[0].pages()[0]` | Returns page object |
| PW-04 | Navigate to URL | `page.goto('https://example.com')` | `page.url()` changes |
| PW-05 | Page title evaluation | `page.title()` after navigate to example.com | Returns non-empty string |
| PW-06 | JavaScript evaluation | `page.evaluate(() => 1 + 1)` | Returns `2` |
| PW-07 | DOM query | `page.locator('body').count()` | Returns `1` |
| PW-08 | Screenshot capture | `page.screenshot()` | Returns non-empty Buffer |
| PW-09 | Navigate back/forward | `page.goto(url1)`, `page.goto(url2)`, `page.goBack()` | `page.url()` is url1 |
| PW-10 | Cookie access | `context.cookies()` | Returns array |
| PW-11 | newPage() fails gracefully | `browser.newPage()` | Throws — document this expected failure |
| PW-12 | Multiple concurrent evaluations | 5 `page.evaluate()` in parallel | All return correct results |
| PW-13 | disconnect graceful | `browser.close()` after operations | No crash, process continues serving |

### Suite 7 — Puppeteer Compatibility Tests

**Framework**: Puppeteer  
**File**: `tests/e2e/puppeteer.test.js`

| ID | Test Case | Steps | Expected |
|----|-----------|-------|----------|
| PP-01 | connect via browserWSEndpoint | `puppeteer.connect({browserWSEndpoint: wsUrl})` | Connected |
| PP-02 | connect with auth token in URL | `wsUrl` contains `?token=abc` | Connected |
| PP-03 | Get existing page | `browser.pages()` returns array | Array has at least 1 page |
| PP-04 | Navigate | `page.goto('https://example.com')` | Success, no error |
| PP-05 | Evaluate | `page.evaluate(() => document.title)` | String result |
| PP-06 | Screenshot | `page.screenshot()` | Buffer returned |
| PP-07 | CDP session | `page.createCDPSession()` | Returns session |
| PP-08 | Raw CDP command via session | `session.send('Browser.getVersion')` | Returns version info |
| PP-09 | Disconnect | `browser.disconnect()` | Clean disconnect |

### Suite 8 — Profile Isolation & Cookie Tests

**Framework**: Node.js + CDP raw WebSocket  
**File**: `tests/integration/profiles.test.js`

| ID | Test Case | Steps | Expected |
|----|-----------|-------|----------|
| PRF-01 | Named profile creates directory | Start with `--profile testA --profile-dir /tmp/anoa-test` | `/tmp/anoa-test/testA/` directory created |
| PRF-02 | Two profiles are isolated | Start two instances with profiles testA and testB | Separate storage directories, no shared state |
| PRF-03 | Default profile (no --profile) | Start without `--profile` | Default QWebEngineProfile used, no directory created |
| PRF-04 | Cookie persistence within session | Set cookie, retrieve cookie | Cookie is returned |
| PRF-05 | Storage clear | `clearStorage()` CDP extension | Returns success, storage cleared |
| PRF-06 | Profile directory path | `--profile-dir /custom/dir` | Profile stored in `/custom/dir/<profile-name>/` |

### Suite 9 — Extension Loading Tests

**Framework**: Shell / process launch test  
**File**: `tests/integration/extensions.test.sh`

| ID | Test Case | Command | Expected |
|----|-----------|---------|----------|
| EXT-LOAD-01 | Valid manifest v2 extension | `--extension ./test-ext` | Process starts, no crash, no error in log |
| EXT-LOAD-02 | Nonexistent extension path | `--extension /nonexistent/path` | Warning logged, browser continues (does not exit) |
| EXT-LOAD-03 | Multiple extensions | `--extension ./test-ext --extension ./test-ext2` | Both loaded, no crash |
| EXT-LOAD-04 | Extension does not block CDP | Load extension, then connect via Playwright | CDP connection succeeds normally |

### Suite 10 — Port Layout & Startup Tests

**Framework**: Shell  
**File**: `tests/integration/port_layout.test.sh`

| ID | Test Case | Command / Check | Expected |
|----|-----------|----------------|----------|
| PORT-01 | Default ports | Start with `--port 9222` | `ss -tlnp` shows 9222 (HTTP) + 9224 (WS) listening |
| PORT-02 | Custom port | Start with `--port 8000` | HTTP on 8000, WS on 8002 (8001 is Chromium internal) |
| PORT-03 | Port conflict detection | Pre-bind 9222, then start | Error: port already in use, exit code 1 |
| PORT-04 | `/json/list` non-empty after startup | Start + `curl /json/list` | Returns array with at least 1 target (due to `load(about:blank)`) |
| PORT-05 | Headless mode no display needed | `--headless` on CI (no $DISPLAY) | Process starts successfully, HTTP responds |
| PORT-06 | Headed mode opens window | Start without `--headless` on display | Window appears (manual verification on dev machine) |
| PORT-07 | `--no-sandbox` flag accepted | `--no-sandbox` | No crash; suitable for CI environments |

### Suite 11 — Regression & Smoke Tests

**Framework**: Shell + Node.js  
**File**: `tests/regression/smoke.sh`  
**Purpose**: Fast check after any commit, verifies the 5 most critical paths

| ID | Regression Case | Verification |
|----|----------------|-------------|
| REG-01 | Binary starts + serves HTTP | `curl /json/version` returns 200 with JSON |
| REG-02 | CDP proxy accepts WS connection | `ws` client connects to port 9224 |
| REG-03 | Profiler.enable returns `{}` | Raw WS send/receive |
| REG-04 | Page.printToPDF returns `%PDF-` | Raw WS send/receive + base64 decode |
| REG-05 | Playwright connectOverCDP succeeds | Full Playwright connection + page title check |

---

## 4. Test Infrastructure Setup

### Directory Structure

```
tests/
├── unit/                         # QTest C++ unit tests
│   ├── test_config.cpp
│   └── CMakeLists.txt
├── integration/                  # Node.js integration tests
│   ├── http_server.test.js
│   ├── cdp_proxy.test.js
│   ├── cdp_extensions.test.js
│   ├── pdf_handler.test.js
│   ├── profiles.test.js
│   ├── extensions.test.sh
│   ├── port_layout.test.sh
│   └── package.json             # ws, node-fetch, vitest
├── e2e/                          # Playwright & Puppeteer
│   ├── playwright.test.ts
│   ├── puppeteer.test.js
│   └── package.json             # @playwright/test, puppeteer
├── regression/                   # Smoke tests
│   └── smoke.sh
└── fixtures/
    ├── test-ext/                 # Minimal manifest v2 extension
    │   └── manifest.json
    └── config.test.json          # Test config file
```

### Node.js Test Helpers

```javascript
// tests/integration/helpers.js
import { spawn } from 'child_process';
import WebSocket from 'ws';
import fetch from 'node-fetch';

export async function startBrowser(args = []) {
  const proc = spawn('./build/anoa-browser', ['--headless', '--no-sandbox', ...args]);
  await waitForPort(9222, 5000); // wait up to 5s
  return proc;
}

export function stopBrowser(proc) {
  proc.kill('SIGTERM');
}

export async function sendCdp(ws, method, params = {}, id = 1) {
  return new Promise((resolve) => {
    ws.once('message', (data) => resolve(JSON.parse(data)));
    ws.send(JSON.stringify({ id, method, params }));
  });
}
```

### CMake Test Integration

```cmake
# tests/unit/CMakeLists.txt
find_package(Qt6 REQUIRED COMPONENTS Test)

add_executable(anoa-browser-test-config test_config.cpp)
target_link_libraries(anoa-browser-test-config
  Qt6::Test
  anoa-config-lib   # extracted static lib from config.cpp
)
add_test(NAME ConfigTests COMMAND anoa-browser-test-config)
```

---

## 5. CI/CD Integration

### GitHub Actions — PR Validation Workflow

**File**: `.github/workflows/ci.yml`  
**Trigger**: Push to any branch, Pull Request to main/master

```yaml
jobs:
  unit-tests:
    runs-on: ubuntu-latest
    steps:
      - cmake -B build -DBUILD_TESTS=ON
      - cmake --build build --target anoa-browser-test-config
      - ctest --test-dir build -V

  integration-tests:
    runs-on: ubuntu-latest
    needs: unit-tests
    steps:
      - cmake -B build && cmake --build build
      - ./build/anoa-browser --headless --no-sandbox --port 9222 &
      - cd tests/integration && npm install && npx vitest run

  e2e-tests:
    runs-on: ubuntu-latest
    needs: integration-tests
    steps:
      - cmake -B build && cmake --build build
      - ./build/anoa-browser --headless --no-sandbox --port 9222 &
      - cd tests/e2e && npm install && npx playwright install chromium
      - npx playwright test

  smoke-tests:
    runs-on: ubuntu-latest
    needs: e2e-tests
    steps:
      - cmake -B build && cmake --build build
      - bash tests/regression/smoke.sh
```

### Environment Variables for CI

```yaml
env:
  QT_VERSION: "6.7.0"
  QPA_PLATFORM: offscreen
  QTWEBENGINE_CHROMIUM_FLAGS: "--disable-gpu --no-sandbox"
```

---

## 6. Protocol Gap Documentation (Non-Regression)

The following CDP commands are **known to fail** and must NOT be included in regression tests. They are documented as expected failures:

| Command | Expected Behavior | Test Assertion |
|---------|-------------------|----------------|
| `Target.createTarget` | Error response (not supported) | `result.error.message` contains "not supported" or similar |
| `browser.newPage()` in Playwright | Throws an error | Error thrown, message logged |
| Actual V8 profiling data | `result == {}` (stub only) | No real profiling data expected |
| Download path management | `result == {}` (stub) | No actual download routing |

These should be tested as **expected gap tests** — the test asserts the failure behavior so regressions in gap handling are also caught.

---

## 7. Test Execution Order

For a full validation run:

```
1. cmake build (Release)
2. unit tests (QTest, no binary needed)
3. integration/http_server tests (binary required)
4. integration/cdp_proxy tests
5. integration/cdp_extensions tests
6. integration/pdf_handler tests
7. integration/profiles tests
8. e2e/playwright tests
9. e2e/puppeteer tests
10. regression/smoke tests
```

Estimated total runtime: **~5 minutes** on a modern CI runner.

---

## 8. Acceptance Criteria

The feature is considered production-ready when:

- [ ] All Suite 1 (CFG) tests pass (config parsing)
- [ ] All Suite 2 (HTTP) tests pass (discovery endpoints)
- [ ] All Suite 3 (WS) tests pass (WebSocket proxy)
- [ ] All Suite 4 (EXT) tests pass (CDP stubs)
- [ ] Suite 5 PDF-01 through PDF-09 pass (PDF-10 concurrent is a stretch goal)
- [ ] Suite 6 PW-01 through PW-10, PW-12, PW-13 pass (PW-11 asserts expected failure)
- [ ] Suite 7 PP-01 through PP-09 pass
- [ ] Suite 10 PORT-01 through PORT-05, PORT-07 pass (PORT-06 is manual)
- [ ] Suite 11 REG-01 through REG-05 pass
- [ ] No memory leaks detected on repeated connect/disconnect cycles (Valgrind on Linux)
- [ ] CI pipeline runs green on all 3 platforms (Linux x86_64, macOS, Windows)

---

## 9. Known Risks

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| Chromium internal port (9223) race on slow CI | Medium | Integration test flakiness | Add startup wait with retry (5s max) before first test |
| `QWebEnginePage::printToPdf` async timing | Low | PDF tests fail intermittently | 30s timeout in `PdfHandler`; fail fast if QEventLoop times out |
| macOS `QPA_PLATFORM=offscreen` not recognized | Low | CI crash on macOS | Pre-test env check; fallback to `--platform offscreen` |
| Windows path separators in config tests | Medium | CFG tests fail on Windows | Use `QDir::fromNativeSeparators()` in assertions |
| Playwright version compatibility | Low | PW tests fail after Playwright update | Pin Playwright version in `package.json` and update intentionally |
| Qt 6.4 vs 6.7 API differences | Low | Build failure on older Qt | Document minimum Qt version clearly; test on Qt 6.4 in CI matrix |

---

## 10. Summary

**Total test cases**: 110  
**Automated**: 104 (94%)  
**Manual-only**: 6 (PORT-06 headed mode, platform-specific visual checks)

| Suite | Count | Framework | Priority |
|-------|-------|-----------|----------|
| Config Unit | 14 | QTest | P1 |
| HTTP Server | 12 | Node.js | P1 |
| WS Proxy | 12 | Node.js | P1 |
| CDP Extensions | 19 | Node.js | P1 |
| PDF Handler | 10 | Node.js | P1 |
| Playwright | 13 | Playwright | P1 |
| Puppeteer | 9 | Puppeteer | P2 |
| Profiles | 6 | Node.js | P2 |
| Extensions | 4 | Shell | P2 |
| Port Layout | 7 | Shell | P1 |
| Regression | 5 | Shell | P1 |
| **Total** | **111** | — | — |
