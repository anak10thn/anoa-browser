# AGENTS.md — anoa-browser

> This file is human-curated project knowledge for AI agents.
> Agents may propose updates, but humans approve them.
> Research shows human-written AGENTS.md improves agent success ~4%.

---

## Project Overview

- **Name**: anoa-browser
- **Type**: C++ application
- **Stack**: Qt6/QWebEngine (C++17), CMake ≥ 3.16
- **Description**: Headless browser built on Qt6/QWebEngine with full Chrome DevTools Protocol (CDP) support via `--remote-debugging-port` passthrough. Distributed as a single self-contained binary — no Node.js or npm required. Supports both headless (offscreen) and visible window modes.

---

## Conventions

### File Structure
```
src/
├── main.cpp              # Qt application entry point; CLI parsing bootstrap
├── browser/              # QWebEngineView subclass, profile & extension management
│   ├── anoa_browser.cpp
│   └── anoa_browser.h
├── http/                 # QTcpServer-based HTTP discovery endpoints (/json, /json/version, /json/list)
│   ├── http_server.cpp
│   └── http_server.h
├── cdp/                  # WebSocket CDP proxy + domain extensions
│   ├── cdp_proxy.cpp     # QWebSocketServer bridge; session multiplexing; bearer token auth
│   ├── cdp_proxy.h
│   ├── cdp_extensions.cpp  # Profiler, HeapProfiler, Security domain adapter
│   └── cdp_extensions.h
├── pdf/                  # Page.printToPDF CDP command handler
│   ├── pdf_handler.cpp
│   └── pdf_handler.h
└── config/               # CLI flag parsing and config file support
    ├── config.cpp
    └── config.h
```

### Naming Conventions
- Files: `snake_case.cpp` / `snake_case.h`
- Qt QObject subclasses: `PascalCase` (e.g., `AnoaBrowser`, `HttpServer`, `CdpProxy`)
- Methods: `camelCase`
- Constants / `#define` macros: `UPPER_SNAKE_CASE`
- Local variables: `camelCase`
- CMake targets: `kebab-case` (matches the binary name: `anoa-browser`)

### Code Patterns
- All QObject subclasses declare `Q_OBJECT` and live in their own `.h`/`.cpp` pair
- Async Qt operations (signals/slots) are bridged to synchronous callers using `QEventLoop` with a timeout — see PDF generation and CDP response patterns
- CDP commands arrive as JSON; use `QJsonDocument::fromJson` + `QJsonObject` for parsing; never use a third-party JSON library
- CLI flags are parsed in `src/config/config.cpp` and passed as a `Config` struct to all subsystems; no global state
- Bearer token authentication is checked in `CdpProxy` before upgrading the WebSocket connection — reject with HTTP 401 if the token mismatches

### Testing Conventions
- Framework: none configured
- Command: `echo 'no test command configured'`
- Integration validation is manual (Phase 10): connect Playwright/Puppeteer to the running binary and exercise CDP domains

---

## Known Gotchas

- **Headless mode**: Set `QPA_PLATFORM=offscreen` *before* constructing `QApplication`. When `--headless` is passed, the binary sets this env var programmatically via `qputenv("QPA_PLATFORM", "offscreen")` at the very top of `main()`, before any Qt object is created.
- **Qt 6.4+ required**: `--remote-debugging-port` passthrough via `QTWEBENGINE_CHROMIUM_FLAGS` is stable only from Qt 6.4 onward. Earlier versions may silently ignore the flag.
- **`printToPdf` is asynchronous**: `QWebEnginePage::printToPdf` does not return the PDF bytes directly — it emits a callback. Wrap with `QEventLoop` and a `QTimer` timeout (default 30 s) to bridge to a synchronous CDP response.
- **`QWebEngineView` requires `QApplication`**: Do not use `QCoreApplication` — WebEngine requires the full GUI application even in headless/offscreen mode.
- **Windows MSVC build**: Requires MSVC 2022 toolset (`v143`) and Windows SDK 10.0.22000. Set `CMAKE_GENERATOR_PLATFORM=x64` explicitly in the CI pipeline.
- **macOS universal binary**: Build separately for `x86_64` and `arm64`, then combine with `lipo`. The Qt6 macOS installer provides a universal SDK but individual builds must target one arch at a time for GitHub Actions matrix.
- **CDP session multiplexing**: `targetId` in CDP messages must be tracked per WebSocket connection — do not share a single upstream DevTools connection across multiple client sockets.
- **Bearer token in URL**: The `?token=` query param may appear in proxy logs. Also support `Authorization: Bearer <token>` header for clients that allow custom WebSocket headers.
- **Extension manifest versions**: `QWebEngineProfile` supports loading unpacked extensions but has limited manifest v3 support in Qt 6.x. Test with manifest v2 extensions; document gaps for v3.
- **Software rasterizer for headless**: On CI runners without a GPU, pass `--disable-gpu` and `--no-sandbox` via `QTWEBENGINE_CHROMIUM_FLAGS` to avoid crashes in offscreen mode.
- **Port layout (3 ports)**: The binary requires 3 consecutive ports: HTTP discovery (port, e.g. 9222), Chromium DevTools internal (port+1, e.g. 9223), and WebSocket CDP proxy (port+2, e.g. 9224). `QTWEBENGINE_CHROMIUM_FLAGS=--remote-debugging-port=<port+1>` must use port+1, not port.
- **`Target.createTarget` not supported**: QtWebEngine's Chromium build does NOT support `Target.createTarget` via CDP. Calling `browser.newPage()` in Playwright will fail. Workaround: use `browser.contexts()[0].pages()[0]` to reuse the existing page created at startup. Document this as a protocol gap for users.
- **Browser context management not supported**: `Target.createBrowserContext`, `Browser.setDownloadBehavior`, `Browser.getWindowForTarget`, etc. are all unsupported by QtWebEngine. The proxy stubs these with `{}` responses so clients like Playwright don't abort connection setup. Playwright works in read/reuse-page mode.
- **`Page.printToPDF` not in Chromium DevTools for QtWebEngine**: The native `Page.printToPDF` CDP command returns `Method not found` (-32601) from this Chromium build. The proxy intercepts it and handles it via `QWebEnginePage::printToPdf` instead. The proxy must be initialized with `setPage()` — without a page reference the call will crash.
- **Page must navigate at startup to appear as CDP target**: `QWebEnginePage` does not register as a DevTools target in `/json/list` until it receives a navigation event. Call `load(QUrl("about:blank"))` in `AnoaBrowser::init()` to ensure the page appears immediately.
- **Playwright trailing slash on `/json/version/`**: Playwright 1.40+ requests `/json/version/` with a trailing slash. HttpServer must normalize trailing slashes or the 404 response causes `connectOverCDP` to fail.
- **`QJsonArray` include required**: `#include <QJsonArray>` must be explicit; forward declarations in `qmetatype.h` are not sufficient for calling `toArray()` in Qt 6.10.
- **macOS strip flag**: `-s` is a GNU ld linker flag and is not valid on macOS (Apple clang). Use `if(UNIX AND NOT APPLE)` guard in CMakeLists.txt.

### Protocol Gap Matrix (Phase 10 Validation, Qt 6.10.2, Chrome 134)

| CDP Command | Status | Notes |
|---|---|---|
| `Target.createTarget` | FAIL — Not Supported | QtWebEngine does not support creating tabs via CDP |
| `Target.createBrowserContext` | STUBBED → `{}` | Proxy returns synthetic context ID `__anoa_default__` |
| `Target.disposeBrowserContext` | STUBBED → `{}` | No-op; proxy returns success |
| `Browser.setDownloadBehavior` | STUBBED → `{}` | Context management not supported |
| `Browser.getWindowForTarget` | STUBBED → `{}` | Not supported |
| `Browser.getVersion` | PASS — passthrough | Chromium returns correct version info |
| `Page.printToPDF` | PASS — Qt API | Not in Chromium DevTools; handled via `QWebEnginePage::printToPdf` |
| `Profiler.enable` | PASS — stub | Returns `{}` (no V8 profiler exposure) |
| `HeapProfiler.enable` | PASS — stub | Returns `{}` (no heap profiler exposure) |
| `Security.enable` | PASS — stub | Returns `{}` |
| `Security.setIgnoreCertificateErrors` | PASS — stub | Returns `{}` |
| `Target.getTargets` | PASS — passthrough | Returns active pages after startup navigation |
| `Playwright connectOverCDP` | PASS (with workaround) | Use existing page; `newPage()` fails |
| `Puppeteer connect` | PASS | Works out-of-the-box |

---

## Architecture Decisions

- **Qt6/QWebEngine chosen over Electron/Playwright server**: Qt6 ships a bundled Chromium (via QtWebEngine) and exposes `--remote-debugging-port`, giving full native CDP support without maintaining a protocol implementation. Playwright/Puppeteer connect to it as they would to any Chrome instance.
- **Single self-contained C++ binary, no Node.js**: Users download one file per platform. No npm install, no Node.js version conflicts. All subsystems (HTTP, WebSocket proxy, CDP extensions, PDF) are implemented with Qt's built-in classes.
- **CDP via `--remote-debugging-port` passthrough**: QtWebEngine forwards Chromium's DevTools port transparently. Set via `QTWEBENGINE_CHROMIUM_FLAGS=--remote-debugging-port=<N>`. This provides full protocol compliance for free — no custom CDP implementation needed.
- **Default port 9222**: Matches Chrome's default, so all CDP clients that auto-detect Chrome work out of the box with zero configuration.
- **Both headless and headed modes**: `--headless` triggers `QPA_PLATFORM=offscreen`; without it the binary opens a visible window. Driven by user requirement to support both use cases from one binary.
- **CDP domain extensions as a C++ adapter**: Profiler, HeapProfiler, Security domains are layered on top of the passthrough in `src/cdp/cdp_extensions.cpp`. They route to Qt APIs where available and return capability stubs otherwise — clients don't break on unsupported commands.
- **Authentication via bearer token**: Stateless; compatible with Playwright's `browserWSEndpoint` URL (`?token=`). Also accepted via `Authorization` header.
- **Persistent profiles via `QWebEngineProfile`**: Named profiles with isolated cookie jars and localStorage. Stored in a user-configurable directory. No external profile manager needed.
- **GitHub Releases for binary distribution**: Standard pattern for compiled tools — no registry dependency. Versioned archives published per platform on `git tag` push via GitHub Actions matrix.

---

## Dependencies & Integrations

- **Qt6 ≥ 6.4** — modules required: `QtWebEngineWidgets`, `QtWebEngineCore`, `QtNetwork`, `QtWebSockets`, `QtWidgets`. Install via Qt online installer or system package manager. On Ubuntu: `apt install qt6-webengine-dev`.
- **CMake ≥ 3.16** — build system. All platform builds use CMake; no Qmake.
- **MSVC 2022 + Windows SDK 10.0.22000** — required for Windows Qt6 WebEngine builds. Set `CMAKE_GENERATOR "Visual Studio 17 2022"` and `CMAKE_GENERATOR_PLATFORM x64`.
- **`lipo`** (macOS) — combines separate `x86_64` and `arm64` builds into a universal binary for macOS distribution.
- **GitHub Actions** — matrix release builds: `ubuntu-latest` (x86_64), `ubuntu-arm64` or cross-compile (arm64), `macos-latest` (universal), `windows-latest` (MSVC x64). Triggered by `git tag` push.
- **`QPA_PLATFORM=offscreen`** — environment variable required for headless rendering without a display server. Set programmatically by the binary when `--headless` is passed.
- **`QTWEBENGINE_CHROMIUM_FLAGS`** — environment variable used to pass Chromium flags (e.g., `--remote-debugging-port=9222 --disable-gpu --no-sandbox`) to the embedded Chromium instance.

---

## Development Setup

```bash
# Prerequisites: Qt6 ≥ 6.4 with WebEngine, CMake ≥ 3.16

# Configure (Debug)
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Configure (Release with strip)
cmake -B build-release -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --parallel

# Run headless (offscreen, no display required)
./build/anoa-browser --headless --port 9222

# Run headed (visible window)
./build/anoa-browser --port 9222

# Install to system
sudo cmake --install build-release
```

---

## Jonggrang Workflow

Jonggrang uses a **two-phase planning** flow so humans can review and edit a plan before AI decomposes it into tasks.

### Full workflow

```bash
# Phase 1 — generate a human-readable draft plan
jonggrang plan "add JWT authentication"
# → AI writes .jonggrang/plan.md (high-level, no tasks yet)
# → Interactive options:
#     Approve           → run Phase 2 immediately
#     Edit with AI      → describe changes, AI revises plan, loop back
#     Edit in $EDITOR   → open editor, loop back
#     Save draft        → exit, run "jonggrang approve" later
#     Abort             → discard plan.md

# Resume after accidental close:
jonggrang plan
# → no description → shows list of pending + archived plans
# → pick one → shows plan + interactive options again

# Phase 2 — approve plan → decompose into tasks
jonggrang approve
# → AI reads .jonggrang/plan.md → runs `jonggrang task import` to create tasks
# → plan.md is archived to .jonggrang/.output/features/<featureId>/plan.md

# Execute tasks
jonggrang work
```

### Shorthand options

```bash
# Plan + auto-approve + tasks in one shot (skips human review)
jonggrang plan "add JWT auth" --yes

# Deep mode: 3-phase analysis (discovery + brainstorm + condense) → enriched plan
# Adds Affected Areas, Risks, and Alternatives Considered sections to plan.md
jonggrang plan "add JWT auth" --deep

# Deep mode + auto-approve in one shot
jonggrang plan "add JWT auth" --deep --yes

# Full pipeline: plan → approve → execute in one shot
jonggrang work "add JWT auth" --yes

# Execute existing tasks only (skip pending plan warning)
jonggrang work --ignore-plan
```

### Modifying an approved plan

| Situation | Command |
|-----------|---------|
| Add new scope on top of done work | `jonggrang plan "also add rate limiting"` |
| Change remaining pending work | `jonggrang plan "use Passport.js instead"` |
| Undo completed tasks | Not supported — create new tasks to override |

**Rule: completed tasks are immutable.** They reflect real code. Any correction must be a new task that fixes/replaces the previous implementation.

### Plan file format

```markdown
---
feature: jwt-auth
branch: feat/jwt-auth
work_type: MEDIUM
description: JWT authentication with login, register, refresh
created_at: 2026-04-16T10:30:00Z
---

# Plan: JWT Authentication

## Approach
...

## Phases
1. DB schema — users + refresh_tokens
2. Auth service — register/login/refresh
3. JWT middleware
...

## Key Decisions
- Token storage: httpOnly cookie

## Out of Scope
- OAuth, 2FA, email verification
```

---

## Bug Reporting

When you discover a defect **outside the scope of your current task**, report it immediately:

```bash
# Report a bug and create a BUGFIX task in one shot
jonggrang bug "description of what is broken" --feature <feature_id>
# When asked "Create a task now?" → y

# Or save for later (batch convert)
jonggrang bug "description" --feature <feature_id>
# When asked "Create a task now?" → n
jonggrang bug convert --feature <feature_id>   # converts all open bugs to tasks later
```

Get the `feature_id` by running: `jonggrang task show <id>` — look for the `feature_id` field in the output.

**Rules:**
- Do NOT fix out-of-scope bugs inline — stay focused on your current task
- Report real defects only (crashes, wrong return values, broken edge cases)
- Do NOT report style issues, TODOs, or future features — those go in the plan

Bug reports are saved to `.jonggrang/.output/features/<feature_id>/bugs.md` and can be viewed with:
```bash
jonggrang bug list
```

---

## Task Management CLI

Use the `jonggrang task` CLI to manage tasks instead of editing `.jonggrang/jonggrang-tasks.json` directly.

### Commands

```bash
# List & inspect
jonggrang task list                         # list all tasks (JSON output)
jonggrang task list pending                 # filter by status
jonggrang task show task-001                # show task detail
jonggrang task next                         # show next eligible task

# Create & modify
jonggrang task add --title "Add login page" --priority 1
jonggrang task add --title "Write tests" --blocked-by task-001
jonggrang task update task-001 --status in_progress
jonggrang task update task-001 --files src/login.ts,src/login.test.ts

# Complete & block
jonggrang task done task-001                # mark completed + passes=true
jonggrang task block task-002 --reason "Waiting for API spec"

# Remove (cleans up blocked_by refs)
jonggrang task remove task-003
```

### Output

- Default output is **JSON** (machine-readable for agents)
- Add `--pretty` for human-readable table format
- Add `--json` to force JSON when in a TTY

### Available flags for add/update

| Flag | Description |
|------|-------------|
| `--title` | Task title |
| `--desc` | Task description |
| `--priority` | Priority (1 = highest) |
| `--status` | pending, in_progress, completed, blocked, waiting, skipped |
| `--skill` | Skill name |
| `--blocked-by` | Comma-separated dependency task IDs |
| `--files` | Comma-separated file paths |
| `--reason` | Reason (used with `block`) |

---

## Jonggrang Notes

This section is updated by Jonggrang during work sessions.
Human should review and curate periodically.

### Patterns Discovered
<!-- Agent appends here, human curates -->

### Gotchas Discovered
<!-- Agent appends here, human curates -->
