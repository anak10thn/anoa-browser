# anoa-browser

Headless browser built on Qt6/QWebEngine with full [Chrome DevTools Protocol (CDP)](https://chromedevtools.github.io/devtools-protocol/) support. Distributed as a single self-contained binary — no Node.js or npm required.

Works with Playwright, Puppeteer, and any other CDP client that connects to a Chrome-compatible endpoint.

---

## Features

- **Full CDP support** via `--remote-debugging-port` passthrough to embedded Chromium
- **Headless and headed modes** from the same binary
- **HTTP discovery endpoints** — `/json`, `/json/version`, `/json/list` (Chrome-compatible)
- **WebSocket CDP proxy** with session multiplexing and optional bearer token auth
- **`Page.printToPDF`** — intercepted and handled via `QWebEnginePage::printToPdf`
- **Named browser profiles** — isolated cookie jars and localStorage per profile
- **Extension loading** — unpacked Chromium extensions (manifest v2)
- **CDP domain extensions** — Profiler, HeapProfiler, Security stubs so clients don't abort on unsupported commands
- **Static linking support** — optional `STATIC_BUILD` for self-contained deployment

---

## Prerequisites

| Dependency | Version | Notes |
|---|---|---|
| Qt6 | ≥ 6.4 | Modules: WebEngineWidgets, WebEngineCore, Network, WebSockets, Widgets |
| CMake | ≥ 3.16 | Build system |
| C++ compiler | C++17 | GCC ≥ 10, Clang ≥ 12, MSVC 2022 |

**Install Qt6 on Ubuntu:**
```bash
apt install qt6-webengine-dev qt6-websockets-dev libqt6network6-dev
```

**Install Qt6 on macOS (Homebrew):**
```bash
brew install qt
```

**Install Qt6 on Windows:**
Use the [Qt Online Installer](https://www.qt.io/download). Select Qt 6.4+ with WebEngine and MSVC 2022 components.

---

## Build

```bash
# Clone
git clone git@github.com:porcupine-md/anoa-browser.git
cd anoa-browser

# Debug build (dynamic linking)
make

# Release build (dynamic linking)
make release

# Release build (static linking)
make release-static

# Install to dist/
make install
```

All available targets:

```
make                        # Debug build (dynamic)
make static                 # Debug build (static)
make release                # Release build (dynamic)
make release-static         # Release build (static)
make install                # Install release to INSTALL_PREFIX (default: dist/)
make install-static         # Install static release to INSTALL_PREFIX
make test                   # Build and run tests
make clean-all              # Remove all build dirs and dist/
make help                   # Show all targets
```

Override variables:
```bash
make release-static QT_PREFIX=/path/to/qt JOBS=8 INSTALL_PREFIX=/opt/anoa
```

---

## Usage

```
anoa-browser [options]

Options:
  --port <N>           CDP HTTP/WebSocket port (default: 9222)
  --headless           Run in offscreen/headless mode (no display required)
  --profile <name>     Named browser profile (isolated cookies/storage)
  --token <secret>     Require Bearer token for WebSocket connections
  --load-extension <path>  Load unpacked Chromium extension from directory
  --url <url>          Navigate to URL at startup (default: about:blank)
```

### Examples

```bash
# Headless on port 9222 (default)
./anoa-browser --headless --port 9222

# Headed with a named profile
./anoa-browser --port 9222 --profile myprofile

# With bearer token auth
./anoa-browser --headless --port 9222 --token mysecret

# Connect Playwright
node -e "
const { chromium } = require('playwright');
(async () => {
  const browser = await chromium.connectOverCDP('http://localhost:9222');
  const page = browser.contexts()[0].pages()[0];
  await page.goto('https://example.com');
  console.log(await page.title());
  await browser.close();
})();
"

# Connect Puppeteer
node -e "
const puppeteer = require('puppeteer-core');
(async () => {
  const browser = await puppeteer.connect({ browserURL: 'http://localhost:9222' });
  const page = await browser.newPage();
  await page.goto('https://example.com');
  console.log(await page.title());
  await browser.close();
})();
"
```

### Port layout

The binary uses 3 consecutive ports:

| Port | Purpose |
|---|---|
| `N` (e.g. 9222) | HTTP discovery + WebSocket CDP proxy |
| `N+1` (e.g. 9223) | Chromium internal DevTools (set via `QTWEBENGINE_CHROMIUM_FLAGS`) |
| `N+2` (e.g. 9224) | Internal WebSocket proxy upstream |

---

## Web Render Endpoints

The HTTP server exposes a `/render/*` family for inspecting the live browser view from any web browser or CLI tool — no CDP client required.

All endpoints share the same `--auth-token` auth as the CDP endpoints: pass the secret as a `Bearer` header or `?token=` query parameter.

### Endpoints

| Method | Path | Response | Description |
|---|---|---|---|
| `GET` | `/render` | `text/html` | Live viewer page — auto-refreshing screenshot in the browser |
| `GET` | `/render/screenshot.png` | `image/png` | Current frame as a PNG snapshot |
| `GET` | `/render/html` | `text/html` | Rendered DOM source (`page()->toHtml()`) |
| `POST` | `/render/navigate?url=<url>` | `text/plain` | Load a URL into the embedded browser |
| `GET` | `/render/stream.mjpeg` | `multipart/x-mixed-replace` | MJPEG live stream (~10 fps) |

### Usage example

```bash
# 1. Start anoa with a token
./anoa-browser --headless --port 9222 --auth-token mysecret

# 2. Navigate the browser to a page
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fexample.com&token=mysecret"

# 3. Open the live viewer in any browser
open "http://localhost:9222/render?token=mysecret"

# 4. Fetch a PNG screenshot with curl
curl -H "Authorization: Bearer mysecret" \
     http://localhost:9222/render/screenshot.png \
     -o screenshot.png

# 5. Navigate the browser to a new URL
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fnews.ycombinator.com&token=mysecret"

# 6. Stream live MJPEG (e.g. in VLC or ffplay)
ffplay "http://localhost:9222/render/stream.mjpeg?token=mysecret"
```

---

## CDP Protocol Support

### Supported / Passing

| Command | Status |
|---|---|
| `Browser.getVersion` | Pass — Chromium passthrough |
| `Target.getTargets` | Pass — returns active pages |
| `Page.navigate` | Pass |
| `Page.printToPDF` | Pass — handled via Qt API |
| `Profiler.enable` | Pass — stub `{}` |
| `HeapProfiler.enable` | Pass — stub `{}` |
| `Security.enable` | Pass — stub `{}` |
| `Security.setIgnoreCertificateErrors` | Pass — stub `{}` |
| `Target.createBrowserContext` | Stubbed → synthetic context ID |
| `Target.disposeBrowserContext` | Stubbed → no-op |
| `Browser.setDownloadBehavior` | Stubbed → `{}` |
| `Browser.getWindowForTarget` | Stubbed → `{}` |

### Not Supported

| Command | Reason |
|---|---|
| `Target.createTarget` | QtWebEngine does not support creating tabs via CDP |

**Playwright workaround:** use the existing page instead of `browser.newPage()`:
```js
const page = browser.contexts()[0].pages()[0];
```

---

## Headless / CI notes

On CI runners without a GPU, set these flags:

```bash
export QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu --no-sandbox"
./anoa-browser --headless --port 9222
```

On macOS, `DISPLAY` is not required. On Linux without a display server, `QPA_PLATFORM=offscreen` is set automatically when `--headless` is passed.

---

## Architecture

```
anoa-browser
├── main.cpp                  # CLI parsing, QApplication bootstrap
├── config/                   # Config struct from CLI flags + env vars
├── browser/                  # QWebEngineView subclass, profiles, extensions
├── http/                     # QTcpServer — /json, /json/version, /json/list
├── cdp/
│   ├── cdp_proxy             # QWebSocketServer bridge, session multiplexing, auth
│   └── cdp_extensions        # Profiler / HeapProfiler / Security domain stubs
└── pdf/                      # Page.printToPDF interceptor via QWebEnginePage::printToPdf
```

All subsystems are implemented with Qt built-in classes (no third-party dependencies beyond Qt6).

---

## License

MIT
