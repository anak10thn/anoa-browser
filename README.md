# anoa-browser

Headless browser built on Qt6/QWebEngine with full [Chrome DevTools Protocol (CDP)](https://chromedevtools.github.io/devtools-protocol/) support. Distributed as a single self-contained binary — no Node.js or npm required.

Works with Playwright, Puppeteer, and any other CDP client that connects to a Chrome-compatible endpoint.

---

## Features

- **Full CDP support** via `--remote-debugging-port` passthrough to embedded Chromium
- **Headless and headed modes** from the same binary
- **HTTP discovery endpoints** — `/json`, `/json/version`, `/json/list` (Chrome-compatible)
- **WebSocket CDP proxy** with session multiplexing and optional bearer token auth
- **Web render endpoints** — live viewer, PNG screenshots, MJPEG stream, navigation, click/scroll injection over plain HTTP (`/render/*`)
- **Terminal viewer (`anoa-term`)** — watch and control the browser from any terminal: live ANSI rendering, mouse clicks and scrolling forwarded to the page
- **Remote CDP friendly** — Chromium started with `--remote-allow-origins=*`, so clients behind tunnels/reverse proxies connect without Origin rejections (access control via `--auth-token`)
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
  -p, --port <N>        CDP HTTP/WebSocket port (default: 9222)
  --headless            Run in offscreen/headless mode (no display required)
  --no-sandbox          Disable Chromium sandbox
  --profile <name>      Named browser profile (isolated cookies/storage)
  --profile-dir <dir>   Base directory for browser profiles
  --auth-token <secret> Require Bearer token for CDP WebSocket connections
  --extension <path>    Load unpacked Chromium extension directory (repeatable)
  --config <file>       Path to JSON or INI config file
  --width <px>          Browser viewport/window width (default: 1280)
  --height <px>         Browser viewport/window height (default: 720)
```

### Examples

```bash
# Headless on port 9222 (default)
./anoa-browser --headless --port 9222

# Headed with a named profile
./anoa-browser --port 9222 --profile myprofile

# With bearer token auth
./anoa-browser --headless --port 9222 --auth-token mysecret

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

### Remote CDP access

Chromium 111+ rejects DevTools WebSocket connections whose `Origin` header is not allowlisted. anoa-browser starts Chromium with `--remote-allow-origins=*` so remote CDP clients (tunnels, reverse proxies, browser-based frontends) can connect from arbitrary origins. Access control is enforced by the proxy layer via `--auth-token` instead.

---

## Web Render Endpoints

The HTTP server exposes a `/render/*` family for inspecting the live browser view from any web browser or CLI tool — no CDP client required.

All endpoints share the same `--auth-token` auth as the CDP endpoints: pass the secret as a `Bearer` header or `?token=` query parameter.

### Endpoints

| Method | Path | Response | Description |
|---|---|---|---|
| `GET` | `/render` | `text/html` | Live viewer page — auto-refreshing screenshot in the browser |
| `GET` | `/render/screenshot.png` | `image/png` | Current frame as a PNG snapshot; `X-Anoa-Viewport-Width/Height` headers carry the logical viewport size |
| `GET` | `/render/screenshot.ppm?w=<px>&h=<px>` | `image/x-portable-pixmap` | Current frame as binary PPM (P6), scaled server-side (aspect ratio kept); `X-Anoa-Viewport-Width/Height` headers carry the logical viewport size for coordinate mapping |
| `GET` | `/render/html` | `text/html` | Rendered DOM source (`page()->toHtml()`) |
| `POST` | `/render/navigate?url=<url>` | `text/plain` | Load a URL into the embedded browser |
| `POST` | `/render/click?x=<px>&y=<px>&button=left\|right\|middle` | `text/plain` | Synthesize a mouse click at viewport coordinates (button defaults to `left`) |
| `POST` | `/render/scroll?dy=<delta>&x=<px>&y=<px>` | `text/plain` | Synthesize a mouse wheel event; `dy` in angle-delta units (±120 per notch, positive scrolls up), `x`/`y` default to the viewport center |
| `POST` | `/render/type?text=<text>` | `text/plain` | Type text into the focused element (URL-encoded query param, or raw request body) |
| `POST` | `/render/key?key=<name>` | `text/plain` | Press a named key: `enter`, `tab`, `backspace`, `delete`, `escape`, `space`, `up`, `down`, `left`, `right`, `home`, `end`, `pageup`, `pagedown` |
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

# 7. Click at viewport coordinates (640, 360)
curl -X POST "http://localhost:9222/render/click?x=640&y=360&token=mysecret"

# 8. Scroll down one wheel notch
curl -X POST "http://localhost:9222/render/scroll?dy=-120&token=mysecret"
```

---

## Terminal Viewer (`anoa-term`)

`anoa-term` renders the live browser view directly in your terminal and forwards terminal mouse input back to the page — click a link in your terminal and the browser clicks it. Built automatically alongside `anoa-browser` on Linux/macOS (POSIX only, no Qt dependency).

Two rendering backends, auto-detected:

| Backend | Quality | Terminals |
|---|---|---|
| `iterm` / `kitty` | Full-resolution PNG (crisp) | iTerm2, WezTerm (`iterm`); kitty, Ghostty (`kitty`) |
| `halfblock` | ANSI truecolor ▀ cells (1 cell = 1×2 px, pixelated) | Everything else with truecolor support |

```
anoa-term [options]

Options:
  --host <host>     anoa-browser host (default: 127.0.0.1)
  --port <N>        anoa-browser HTTP port (default: 9222)
  --token <secret>  Bearer token if the server was started with --auth-token
  --fps <N>         Refresh rate, 1-30 (default: 10)
  --gfx <mode>      auto | halfblock | iterm | kitty (default: auto)
```

`--gfx auto` picks the image protocol from `TERM`/`TERM_PROGRAM`; pass `--gfx iterm` or `--gfx kitty` explicitly if detection misses (e.g. inside tmux, which hides the outer terminal — image protocols need tmux ≥ 3.4 with `allow-passthrough`, otherwise use `--gfx halfblock`).

Controls:

| Input | Action |
|---|---|
| Left/right/middle mouse click | Click at that position in the page |
| Mouse wheel | Scroll the page under the pointer |
| Typing (any text, incl. paste) | Typed into the focused element |
| `Enter` / `Backspace` / `Tab` / arrows | Forwarded to the page (arrows scroll when no field is focused) |
| `Ctrl-C` / `Ctrl-Q` | Quit and restore the terminal |

The status bar shows the last event forwarded to the browser (`click 640,360`, `typed "hello"`, …). If it doesn't change when you click, your terminal isn't delivering mouse reports — check its mouse-reporting setting, or in tmux enable `set -g mouse on`.

```bash
# 1. Start the browser (any machine, headless or headed)
./anoa-browser --headless --port 9222 --auth-token mysecret

# 2. Point it somewhere
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fnews.ycombinator.com&token=mysecret"

# 3. Watch and control it from your terminal (works over SSH too)
./anoa-term --host localhost --port 9222 --token mysecret
```

Requires a terminal with SGR mouse support; the halfblock fallback additionally needs truecolor (iTerm2, kitty, Alacritty, WezTerm, GNOME Terminal, tmux ≥ 3.2, …).

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

anoa-term (tools/anoa-term/)  # POSIX terminal viewer — iTerm2/kitty image protocols
                              # or ANSI half-block fallback; SGR mouse →
                              # /render/click + /render/scroll
```

All subsystems are implemented with Qt built-in classes (no third-party dependencies beyond Qt6).

---

## License

MIT
