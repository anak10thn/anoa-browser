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
- **Terminal viewer (`anoa-browser terminal`)** — a mode of the same binary, not a second executable: watch and control the browser from any terminal with live ANSI rendering, mouse clicks and scrolling forwarded to the page. `--cdp` points it at any external Chrome/Chromium/Playwright endpoint instead (POSIX only)
- **Remote CDP friendly** — Chromium started with `--remote-allow-origins=*`, so clients behind tunnels/reverse proxies connect without Origin rejections (access control via `--auth-token`)
- **`Page.printToPDF`** — intercepted and handled via `QWebEnginePage::printToPdf`
- **Named browser profiles** — isolated cookie jars and localStorage per profile
- **Extension loading** — unpacked Chromium extensions (manifest v2)
- **CDP domain extensions** — Profiler, HeapProfiler, Security stubs so clients don't abort on unsupported commands
- **Static linking support** — optional `STATIC_BUILD` for self-contained deployment

---

## Install

### macOS (Homebrew) — Intel & Apple Silicon

One universal (x86_64 + arm64) build serves both architectures:

```bash
brew tap porcupine-md/tap
brew trust porcupine-md/tap        # tap ships a cask + formula
brew install --cask anoa-browser
```

Installs a single `anoa-browser` shim into your `PATH`, pointing at `anoa-browser.app/Contents/MacOS/anoa-browser`. That one executable is the whole product — the terminal viewer is the `anoa-browser terminal` subcommand of it, not a separate file in the bundle. The app is Developer ID signed and notarized by Apple, so it opens with no Gatekeeper warnings. (The cask still clears the quarantine flag on install — a harmless no-op safety net.)

**Upgrade** — `brew update` first, so the tap picks up the newest release:

```bash
brew update
brew upgrade --cask anoa-browser
```

### Linux (Homebrew)

```bash
brew tap porcupine-md/tap
brew install anoa-browser-linux
```

The formula creates exactly one symlink, `bin/anoa-browser` → `libexec/anoa-browser.sh`, and terminal mode is reached through it as `anoa-browser terminal`. If you are upgrading from a release that installed a second command, read the breaking-change note at the end of [Terminal Viewer](#terminal-viewer-anoa-browser-terminal).

**Upgrade:**

```bash
brew update
brew upgrade anoa-browser-linux
```

### Linux (portable tarball)

The release tarball is self-contained: one executable (`anoa-browser`), every Qt/WebEngine shared library under `lib/`, plugins, `resources/`, `translations/`, a `qt.conf`, plus a launcher script that wires them together (`LD_LIBRARY_PATH`, `QTWEBENGINEPROCESS_PATH`, …). Terminal mode is a subcommand of that executable, so the tarball contains no second binary.

```bash
tar xzf anoa-browser-linux-x86_64.tar.gz
./anoa-browser/anoa-browser.sh --headless --port 9222   # launcher, not the raw binary
./anoa-browser/anoa-browser.sh terminal                 # terminal viewer — same launcher
```

Always go through `anoa-browser.sh`: the raw `anoa-browser` next to it has no Qt environment set up, in terminal mode as much as in browser mode.

### Windows

Download `anoa-browser-windows-x86_64.zip` from [Releases](https://github.com/porcupine-md/anoa-browser/releases) and run `anoa-browser.exe`.

---

## Prerequisites (building from source)

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

## Terminal Viewer (`anoa-browser terminal`)

`anoa-browser terminal` renders a live browser view directly in your terminal and forwards terminal mouse and keyboard input back to the page — click a link in your terminal and the browser clicks it. It is a **mode of the `anoa-browser` binary**, not a separate program: the word `terminal` before any options selects it, and that mode never starts a browser window, an HTTP server, or a CDP proxy of its own.

By default it views a running `anoa-browser` over the [`/render/*` endpoints](#web-render-endpoints). With [`--cdp`](#attaching-to-an-external-cdp-endpoint---cdp) it attaches to any external Chrome/Chromium/Playwright endpoint instead.

**POSIX only.** The terminal sources are not compiled into the Windows build at all; there, `anoa-browser terminal` prints `Error: terminal mode is not supported on Windows` and exits non-zero.

Two rendering backends, auto-detected:

| Backend | Quality | Terminals |
|---|---|---|
| `iterm` / `kitty` | Full-resolution PNG (crisp) | iTerm2, WezTerm (`iterm`); kitty, Ghostty (`kitty`) |
| `halfblock` | ANSI truecolor ▀ cells (1 cell = 1×2 px, pixelated) | Everything else with truecolor support |

### Invocation

```
anoa-browser terminal [options]

Options:
  --term-host <host>     Host of the anoa-browser to view (default: 127.0.0.1)
  --term-port <N>        HTTP port of the anoa-browser to view (1-65535, default: 9222)
  --term-token <secret>  Bearer token, if the viewed endpoint requires one
  --fps <N>              Refresh rate, 1-120 (default: 30)
  --gfx <mode>           auto | halfblock | iterm | kitty (default: auto)
  --cdp <url>            Attach to an external CDP endpoint instead of /render/*
```

The connection flags are spelled `--term-*` deliberately. `--port` and `--auth-token` keep their browser meaning on the same shared parser (the port *this* process listens on, the token *it* demands), so no flag changes meaning between modes.

**Terminal options are CLI-only.** `--config` reads the browser options from a JSON or INI file, but nothing in that file is consulted for terminal mode — `--term-host`, `--term-port`, `--term-token`, `--fps`, `--gfx` and `--cdp` must be passed on the command line.

`--gfx auto` picks the image protocol from `TERM`/`TERM_PROGRAM`; pass `--gfx iterm` or `--gfx kitty` explicitly if detection misses (e.g. inside tmux, which hides the outer terminal — image protocols need tmux ≥ 3.4 with `allow-passthrough`, otherwise use `--gfx halfblock`).

### Controls

Mouse reporting uses the SGR extended protocol (`ESC [ < btn ; col ; row M`), which every modern terminal emits; cells are mapped back to page coordinates using the viewport size the endpoint reports, so clicks land where you see them even on a HiDPI page.

| Input | Action |
|---|---|
| Left/right/middle mouse click | Click at that position in the page |
| Mouse wheel | Scroll the page under the pointer |
| Typing (any text, incl. paste) | Typed into the focused element — a whole paste burst is forwarded as one event |
| `Enter` / `Backspace` / `Tab` | Forwarded as key events (`Backspace` accepts both DEL and BS) |
| Arrow keys | Forwarded to the page — they move the caret in a focused field, otherwise they scroll |
| `Ctrl-C` / `Ctrl-Q` | Quit and restore the terminal |

There is no single-letter quit: `q` is an ordinary printable character and is typed into the page, so you can fill in a search box without the viewer exiting. `Ctrl-C` is delivered as a keystroke rather than a signal (the terminal runs with `ISIG` off), so it quits immediately.

The status bar shows the last event forwarded to the browser (`click 640,360`, `typed "hello"`, `key backspace`, …) alongside the backend, endpoint and viewport size. If it doesn't change when you click, your terminal isn't delivering mouse reports — check its mouse-reporting setting, or in tmux enable `set -g mouse on`.

```bash
# 1. Start the browser (any machine, headless or headed)
./anoa-browser --headless --port 9222 --auth-token mysecret

# 2. Point it somewhere
curl -X POST "http://localhost:9222/render/navigate?url=https%3A%2F%2Fnews.ycombinator.com&token=mysecret"

# 3. Watch and control it from your terminal (works over SSH too)
./anoa-browser terminal --term-host localhost --term-port 9222 --term-token mysecret
```

Requires a terminal with SGR mouse support; the halfblock fallback additionally needs truecolor (iTerm2, kitty, Alacritty, WezTerm, GNOME Terminal, tmux ≥ 3.2, …). Both stdin and stdout must be a terminal — piping either one is refused, since there is nothing to drive and nothing to paint.

### Attaching to an external CDP endpoint (`--cdp`)

`--cdp <url>` replaces the `/render/*` transport with a CDP WebSocket client (`Page.captureScreenshot` for frames, `Input.dispatchMouseEvent` / `dispatchKeyEvent` / `insertText` for input), so the viewer can drive any Chrome, Chromium, Edge or Playwright/Puppeteer-launched browser — not just anoa-browser.

Two URL forms are accepted:

| Form | Behaviour |
|---|---|
| `http://host:port` or `https://host:port` | Fetches `/json/list` and attaches to the **first `type: "page"` target**, dialling that target's `webSocketDebuggerUrl`. A URL with a path keeps it verbatim (`http://host/proxy/devtools` fetches exactly that), which is what makes reverse-proxied endpoints work; an empty path or a bare `/` becomes `/json/list`. |
| `ws://host:port/devtools/page/<id>` | Dialled directly — no discovery request, and the target you name is the target you get. |

```bash
# Attach to a Chrome started with --remote-debugging-port=9222
anoa-browser terminal --cdp http://127.0.0.1:9222

# Attach to one specific page target, skipping discovery
anoa-browser terminal --cdp ws://127.0.0.1:9222/devtools/page/ABC123 --gfx kitty
```

**Auth token.** `--term-token` is *not* ignored under `--cdp` — it becomes the bearer token for the CDP endpoint, sent both as an `Authorization: Bearer <secret>` header and as a `?token=<secret>` query parameter, on the `/json/list` request and on the WebSocket dial. That is what anoa-browser's own `--auth-token` proxy expects, and endpoints that ignore an unexpected header or query parameter (plain Chrome) are unaffected. `--term-host` and `--term-port` *are* ignored under `--cdp`, and the viewer says so on stderr before it takes over the screen.

**`wss://` is not supported.** TLS CDP endpoints are rejected at argument-parsing time, because Qt is not built with OpenSSL here. Use `ws://`, or `http://` and let discovery hand you the right `ws://` URL. Tunnel it (SSH port-forward, stunnel) if the endpoint is only reachable over TLS.

Two things worth knowing when the endpoint is anoa-browser itself: it uses [three consecutive ports](#port-layout), so `--cdp http://127.0.0.1:9222` discovers on 9222 and then dials `ws://127.0.0.1:9224/…` — the port shown in the status bar changing mid-session is correct, not a fault. And a dropped connection is retried with an exponential backoff (250 ms doubling to 8 s) that the status bar reports as `connecting` / `reconnecting (attempt N)`; before the *first* successful connect the retries are capped, so a wrong URL fails with a message instead of spinning forever.

### Breaking change: `anoa-term` is gone

The standalone `anoa-term` binary no longer exists and ships **no compatibility shim, symlink, or wrapper** — a shim would either be the second binary this merge removed, or a symlink whose `argv[0]` sniffing outlives its usefulness. Type `anoa-browser terminal` instead; every flag it used has a `--term-*` equivalent listed above (`--host` → `--term-host`, `--port` → `--term-port`, `--token` → `--term-token`; `--fps` and `--gfx` are unchanged, though `--fps` now defaults to 30 and accepts up to 120).

Upgrading in place can leave a stale `anoa-term` on your `PATH` that no longer resolves. Two hazards:

- **Linux (Homebrew):** `brew upgrade` unlinks the old keg before linking the new one, which normally takes `bin/anoa-term` with it — but it does not do so reliably if the link was force-linked, hand-created, or left behind by a partially failed unlink. The symptom is a *dangling* symlink into the new keg's `libexec/anoa-term`, so you get "No such file or directory" rather than "command not found".
- **macOS (cask):** the cask's `binary` shim for `anoa-term` is removed on **reinstall**, not on upgrade, so the old shim can survive a `brew upgrade --cask`.

If `anoa-term` still appears on your `PATH` after upgrading, uninstall and reinstall:

```bash
brew uninstall anoa-browser-linux && brew install anoa-browser-linux   # Linux
brew uninstall --zap --cask anoa-browser && brew install --cask anoa-browser   # macOS
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

## Releasing (maintainers)

1. Create a tag `vX.Y.Z` — via GitHub (**Releases → Draft a new release → Choose a tag → Create new tag**) or CLI (`git tag vX.Y.Z && git push origin vX.Y.Z`). The tag name is the version: CI injects it into the build (`ANOA_VERSION_OVERRIDE`), so `CMakeLists.txt` never needs a manual bump.
2. CI builds Linux / macOS / Windows, signs + notarizes the macOS app, publishes the GitHub Release, and updates the Homebrew tap automatically.

Tag pushes require the macOS signing secrets (`MACOS_CERT_P12_BASE64`, `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_PASSWORD`, `APPLE_TEAM_ID`) — CI fails fast if any are missing, so an unsigned build can never be released. Without `HOMEBREW_TAP_TOKEN` the tap update is skipped (warning only). To smoke-test the pipeline without publishing, run the Release workflow manually (`workflow_dispatch`) — artifacts only, no release, no tap update.

If the tap update was skipped or failed, resync it without re-running the release: **Actions → Update Homebrew tap → Run workflow** (or `gh workflow run update-homebrew-tap.yml -f tag=vX.Y.Z`). Leave `tag` empty to sync the latest release; the run is idempotent and pushes nothing when the tap already matches.

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
├── terminal/                 # `anoa-browser terminal` — POSIX only, compiled out on Windows
│   ├── terminal_app          # QCoreApplication loop — QSocketNotifier(stdin) + frame QTimer
│   ├── terminal_ui           # termios raw mode, SIGWINCH, iTerm2/kitty image protocols or
│   │                         # ANSI half-block fallback, status bar, SGR mouse/key parsing
│   ├── frame_backend         # transport seam — frames out, input in
│   ├── render_http_client    # default backend — /render/screenshot.ppm + click/scroll/type/key
│   ├── cdp_client            # --cdp transport — QWebSocket, id/response correlation, discovery
│   └── cdp_frame_backend     # --cdp backend — Page.captureScreenshot / getLayoutMetrics / Input.*
└── pdf/                      # Page.printToPDF interceptor via QWebEnginePage::printToPdf
```

All subsystems are implemented with Qt built-in classes (no third-party dependencies beyond Qt6).

---

## License

MIT
