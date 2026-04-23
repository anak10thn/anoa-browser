---
feature: anoa-headless-browser-cdp
branch: feat/anoa-headless-browser-cdp
work_type: LARGE
description: Headless browser built on Qt6/QWebEngine with full CDP support via remote debugging passthrough and a Node.js/TypeScript process manager
created_at: 2026-04-23T10:21:22.832Z
depth: deep
---

# Plan: Anoa Headless Browser with CDP

## Approach
Launch a Qt6/QWebEngine C++ binary supporting both offscreen (headless) and visible window modes with `--remote-debugging-port`, exposing Chromium's native DevTools Protocol for free. The entire stack — HTTP discovery endpoints (`/json`, `/json/version`, `/json/list`), WebSocket CDP proxy with session multiplexing, bearer token authentication, CDP domain extensions for Profiler, HeapProfiler, and Security, browser extension loading, persistent browser profiles, cookie/storage management, and PDF generation via `Page.printToPDF` — is implemented directly in C++ within the single Qt binary. No Node.js runtime or npm packaging is used. The binary is built per target platform (Linux x86_64/arm64, macOS universal, Windows MSVC x86_64) via CMake, and GitHub Actions automates multi-platform release builds that publish versioned binary archives to GitHub Releases. This architecture delivers full CDP compliance while keeping distribution simple: users download a single self-contained binary.

## Phases
1. Project bootstrap — Initialize `CMakeLists.txt` and `AGENTS.md`; fill `AGENTS.md` with architecture decisions, file structure conventions, and stack choices; define multi-platform CMake toolchain setup
2. C++ Qt6 binary (headless + headed) — `src/main.cpp` + `src/browser/anoa_browser.cpp/.h`: QWebEngineView supporting both offscreen mode (`--headless`) and visible window mode, launched with `--remote-debugging-port`, accepting `--port`, `--headless`, `--profile-dir`, `--load-extension`, and `--auth-token` flags; multi-architecture CMake configuration (x86_64, arm64, Windows MSVC)
3. HTTP discovery server (C++) — `src/http/http_server.cpp/.h`: Qt-based TCP HTTP server (QTcpServer) serving `/json`, `/json/version`, `/json/list` endpoints in Chromium-compatible format by proxying Qt's own discovery port responses
4. WebSocket CDP proxy (C++) — `src/cdp/cdp_proxy.cpp/.h`: QWebSocketServer-based proxy bridging external CDP clients to Qt's native DevTools WebSocket, with session multiplexing for multiple targets; bearer token authentication middleware (`?token=` query param or `Authorization` header)
5. CDP domain extensions (C++) — `src/cdp/cdp_extensions.cpp/.h`: C++ adapter implementing Profiler, HeapProfiler, and Security CDP domains on top of the passthrough; routes domain commands to Qt APIs where available, returns capability stubs otherwise; does not break clients on unsupported commands
6. Browser extension and profile management (C++) — CLI flags and C++ API for loading unpacked extensions via `QWebEngineProfile`, managing named profiles with isolated cookie jars and localStorage, and cookie/storage CRUD via `QWebEngineCookieStore`
7. PDF generation (C++) — `src/pdf/pdf_handler.cpp/.h`: `Page.printToPDF` CDP command handler using `QWebEnginePage::printToPdf`; exposes the CDP command, validates parameters, bridges the asynchronous Qt callback to the synchronous CDP response with timeout handling
8. CLI argument parsing and configuration — `src/config/config.cpp/.h`: full CLI flag parsing (`--port`, `--headless`, `--no-sandbox`, `--profile`, `--extension`, `--auth-token`), config file support (JSON or INI), launch validation and error reporting
9. GitHub Actions release pipeline — `.github/workflows/release.yml`: matrix build for Linux (x86_64, arm64 via cross-compile or native runner), macOS (universal binary via `lipo` combining x86_64 + arm64 builds), and Windows (MSVC x86_64); strips and compresses binaries; creates a versioned GitHub Release with per-platform binary archives on `git tag` push
10. Integration validation — Manual and scripted validation against Playwright and Puppeteer using the downloaded binary; validate Profiler, HeapProfiler, Security domain commands; validate PDF output; validate extension loading; validate profile isolation; document known protocol gaps vs. full Chrome

## Key Decisions
- Decision: Use Qt's `--remote-debugging-port` passthrough instead of custom CDP — full protocol compliance for free; Playwright/Puppeteer compatibility without manual protocol maintenance
- Decision: Single self-contained C++ binary with no Node.js runtime dependency — simpler distribution; users download one file; no npm install required; all subsystems (HTTP, WebSocket proxy, CDP extensions) implemented with Qt's built-in networking classes (QTcpServer, QWebSocketServer)
- Decision: Support both offscreen (`--headless`) and visible window mode — driven by user requirement; `QPA_PLATFORM=offscreen` used only when `--headless` is set, otherwise standard QPA platform
- Decision: Default port 9222 (Chromium standard) — zero-config compatibility with all CDP clients that default to Chrome's port
- Decision: Single C++ binary compiled per target platform — multi-arch CMake config; macOS universal binary via `lipo`; Windows MSVC build; distributed as versioned archives on GitHub Releases
- Decision: GitHub Actions matrix build for all platforms — reproducible release builds; triggered by git tag push; no manual build steps required for releases
- Decision: CDP domain extensions (Profiler, HeapProfiler, Security) implemented as a C++ adapter layer — routes to Qt APIs where exposed, stubs with capability responses otherwise; incrementally improvable without introducing a second runtime
- Decision: Authentication via bearer token on the CDP WebSocket port — simple, stateless, compatible with Playwright's `browserWSEndpoint` URL; token passed as `?token=` query param or `Authorization` header
- Decision: Persistent profiles managed in C++ via `QWebEngineProfile` named profiles, stored in user-configurable directory — isolates sessions; no external profile manager needed
- Decision: GitHub Releases for binary distribution — standard pattern for compiled tools; users can download directly or via install scripts; no registry dependency

## Affected Areas
- `CMakeLists.txt` — C++ Qt6 multi-arch build definition (new)
- `src/main.cpp` — Qt application entry point with headless/headed mode switch and CLI parsing (new)
- `src/browser/anoa_browser.cpp` / `src/browser/anoa_browser.h` — QWebEngineView subclass, offscreen + visible init, extension loading, named profile support (new)
- `src/http/http_server.cpp` / `src/http/http_server.h` — QTcpServer-based HTTP discovery endpoint server (new)
- `src/cdp/cdp_proxy.cpp` / `src/cdp/cdp_proxy.h` — QWebSocketServer-based CDP proxy with session multiplexing and bearer token auth (new)
- `src/cdp/cdp_extensions.cpp` / `src/cdp/cdp_extensions.h` — C++ CDP domain adapter for Profiler, HeapProfiler, Security (new)
- `src/pdf/pdf_handler.cpp` / `src/pdf/pdf_handler.h` — `Page.printToPDF` CDP command handler (new)
- `src/config/config.cpp` / `src/config/config.h` — CLI flag parsing, config file support, launch validation (new)
- `.github/workflows/release.yml` — GitHub Actions matrix build and GitHub Releases publish workflow (new)
- `AGENTS.md` — Fill in all TODO placeholders with architecture decisions

## Risks
- Risk: QtWebEngine binary size (~200–500MB) conflicts with "small size" requirement — mitigation: strip symbols in release builds, exclude unnecessary Qt modules (no widgets, multimedia, SQL), document that "small" means minimal Qt footprint, not minimal Chromium
- Risk: `--remote-debugging-port` flag behavior may differ across Qt6 minor versions — mitigation: pin Qt6 minimum version in CMakeLists.txt; test on Qt 6.4+ where the flag is stable
- Risk: Qt's DevTools passthrough may not expose every CDP Profiler/HeapProfiler/Security command — mitigation: C++ adapter returns partial/stub responses and documents gaps; does not break clients
- Risk: Network domain via Qt's `QWebEngineUrlRequestInterceptor` is not full CDP Network — mitigation: rely on passthrough for what Chromium exposes; document delta from full Chrome Network domain
- Risk: Headless offscreen mode (`QPA_PLATFORM=offscreen`) requires a valid GPU or software renderer — mitigation: test with `--disable-gpu` and software rasterizer fallback; document environment requirements
- Risk: Session multiplexing across multiple targets adds WebSocket proxy complexity in C++ — mitigation: use Qt's QWebSocketServer; implement one-target-per-session first, then extend
- Risk: Playwright/Puppeteer CDP handshake requires specific `/json/version` fields (Browser, Protocol-Version, webSocketDebuggerUrl) — mitigation: validate response format against Playwright source in Phase 10
- Risk: Windows MSVC Qt6 WebEngine build requires specific MSVC version and Windows SDK — mitigation: document exact toolchain versions; pin MSVC 2022 + Windows SDK 10.0.22000 in the GitHub Actions workflow
- Risk: macOS universal binary (`lipo`) requires separate arm64 + x86_64 Qt builds — mitigation: use Qt's official macOS universal installer or two-runner matrix then `lipo`; document build matrix
- Risk: GitHub Actions macOS arm64 runner availability and Qt installation time may increase CI duration — mitigation: cache Qt installation via `actions/cache`; use self-hosted runner if build time is unacceptable
- Risk: `QWebEnginePage::printToPdf` is asynchronous and callback-based; bridging to a synchronous CDP response requires careful C++ promise/signal management — mitigation: wrap in a QEventLoop with timeout; handle partial/failed renders
- Risk: Bearer token in WebSocket URL query param may be logged by proxies — mitigation: also support `Authorization` header; document security tradeoff
- Risk: Extension loading via `QWebEngineProfile` has limited manifest v3 support in Qt 6.x — mitigation: document supported manifest versions; test with common extensions

## Alternatives Considered
- Node.js/TypeScript wrapper + npm distribution: rejected because it requires users to have Node.js installed and adds npm packaging complexity; a pure C++ binary is simpler to install and distribute
- Native CDP implementation in C++ without Qt passthrough: rejected because QWebEngine API does not cover all CDP domains, requires 3–5× more C++ code, and carries high protocol drift risk
- Docker image distribution: rejected because it adds container runtime dependency; single binary on GitHub Releases is sufficient

## Out of Scope
- Node.js runtime or npm packaging
- Docker container setup
- npm registry publishing

## Dependencies
- **Qt6** (≥ 6.4) with `QtWebEngine`, `QtWebEngineCore`, `QtNetwork`, `QtWebSockets` modules — system-level installation required; multi-platform installers for Linux, macOS (universal), Windows
- **CMake** (≥ 3.16) — C++ build system; multi-arch toolchain configuration
- **Chromium/Blink engine** — bundled automatically via QtWebEngine; no separate installation
- **`QPA_PLATFORM=offscreen`** environment variable — required for headless rendering without a display server (set automatically when `--headless` flag is used)
- **MSVC 2022 + Windows SDK 10.0.22000** — required for Windows Qt6 WebEngine build
- **`lipo`** (macOS) — universal binary assembly for macOS arm64 + x86_64 targets
- **GitHub Actions** — CI/CD platform for matrix release builds; `ubuntu-latest`, `macos-latest`, `windows-latest` runners; `actions/cache` for Qt installation caching
- **GitHub Releases** — binary distribution; versioned archives uploaded per platform on tag push
