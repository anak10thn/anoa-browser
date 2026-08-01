# AGENTS.md — anoa

> This file is human-curated project knowledge for AI agents.
> Agents may propose updates, but humans approve them.
> Research shows human-written AGENTS.md improves agent success ~4%.

---

## Project Overview

- **Name**: anoa (binary: `anoa-browser`)
- **Type**: native desktop/CLI binary — a browser automation host, not a Node service
- **Stack**: C++17 + Qt 6 (Widgets, WebEngineWidgets, WebEngineCore, Network, WebSockets), built with CMake ≥ 3.16. `find_package(Qt6 6.4 REQUIRED ...)` is the floor; CI pins 6.7.3. Node is used **only** for the test suites under `tests/`.
- **Description**: A Qt WebEngine browser that exposes the live page over an HTTP `/render/*` + screenshot API and an authenticated CDP WebSocket proxy, and can render that page into a terminal via `anoa-browser terminal`.

---

## Conventions

### File Structure
```
src/
├── main.cpp        # entry point + the raw-argv pre-scan that selects the mode
├── browser/        # AnoaBrowser — the QWebEngineView host, profiles, extensions
├── cdp/            # CdpProxy (authenticated WebSocket proxy) + CDP extension methods
├── config/         # Config struct, parseArgs(), loadConfigFile() — shared by both modes
├── http/           # HttpServer — /json/*, /render/*, screenshot and input endpoints
├── pdf/            # PdfHandler — print-to-PDF
└── terminal/       # `anoa-browser terminal`: viewer UI, HTTP transport, CDP transport
resources/          # .desktop file, SVG icon, anoa-browser.sh bundle launcher
tests/
├── unit/           # QTest + CTest (test_config.cpp, its own CMakeLists.txt)
├── integration/    # vitest (*.test.js) + two bash suites (*.test.sh)
├── e2e/            # Playwright (TS) and Puppeteer (JS) against a running binary
└── regression/     # smoke.sh — fast post-commit check of the 5 critical paths
.github/
├── workflows/      # ci.yml, release.yml, update-homebrew-tap.yml
├── homebrew/       # anoa-browser.rb.tpl (cask), anoa-browser-linux.rb.tpl (formula)
└── entitlements/   # anoa-browser.entitlements for macOS codesigning
```

### Naming Conventions
- Source files: `snake_case.cpp` / `snake_case.h`, one directory per subsystem
- Classes / structs: `PascalCase` — Qt classes keep their `Q` prefix
- Methods and locals: `camelCase`
- Member variables: `m_camelCase`
- Constants: `kCamelCase` for file-local `constexpr` values, `UPPER_SNAKE_CASE` for macros
- CLI flags: `kebab-case`; HTTP endpoints: `kebab-case` under `/render/*`, `/json/*`
- **String literals in `src/` must be ASCII-only** (this also builds on MSVC). Write
  UTF-8 bytes as escapes (`"\xE2\x80\x94"`). Non-ASCII in comments is fine.

### Code Patterns
- Every subsystem is a `QObject` owning its own Qt resources; parent-child ownership,
  not manual `delete`.
- Signals/slots across a seam, never a blocking call: `FrameBackend` requests a frame
  and is answered by `frameReady` / `frameFailed`, because a WebSocket transport
  cannot answer synchronously without a nested event loop.
- Config flows one way: `parseArgs()` → `Config` struct → constructors. Nothing
  re-reads argv.
- Validation failures in `parseArgs()` print one line to stderr and `::exit(1)`.
  Runtime failures inside terminal mode instead ask `exec()` to return non-zero so
  the termios/alt-screen restore still runs.
- `-Wall -Wextra -Wpedantic` is on for the main target and the tree is warning-clean;
  keep it that way.
- **Any POSIX call whose name collides with a `QObject` member must be `::`-qualified
  inside a `QObject` subclass** (`::connect`, `::bind`, `::listen`, `::accept`) —
  unqualified lookup finds `QObject::connect` and stops.

### Testing Conventions
- **Unit (QTest + CTest)**: `make test` — it re-configures with `-DBUILD_TESTS=ON`,
  builds and runs `ctest --output-on-failure`. A plain `make build` leaves
  `BUILD_TESTS` OFF, so the test target does not exist. Sources live in `tests/unit/`.
  `parseArgs()` cannot be tested in-process (it reads argv off the live
  `QCoreApplication` and exits on error), so those cases re-invoke the test binary
  through `QProcess` with `ANOA_TEST_HARNESS=parse_args` and parse a JSON line
  off stdout.
- **Integration (vitest)**: `cd tests/integration && npm install && npx vitest run`.
  `vitest.config.js` sets `fileParallelism: false` on purpose — each file spawns its
  own `anoa-browser` on the same port triplet, so concurrent files would bind-clash
  or silently talk to the wrong instance. Binary and port come from `ANOA_BINARY` /
  `ANOA_PORT`.
- **Integration (bash)**: `bash tests/integration/port_layout.test.sh` and
  `bash tests/integration/extensions.test.sh`, both taking
  `ANOA_BINARY=./build/anoa-browser`.
- **E2E**: `cd tests/e2e && npm install`, then `npx playwright test` (needs
  `npx playwright install --with-deps chromium`) and `node --test puppeteer.test.js`.
  Both attach to an already-running `anoa-browser`; the test does not start it.
- **Regression**: `bash tests/regression/smoke.sh` — same `ANOA_BINARY`/`ANOA_PORT`
  contract.
- Tests run against a **real browser process**, never a mock. Each vitest and bash
  file starts and kills its own instance; the e2e suites are the exception and attach
  to one started outside them.
- Terminal-mode tests need a **pty**: `terminal_app.cpp` refuses to start unless both
  stdin and stdout are a terminal, so a pipe exercises nothing. `terminal_cdp.test.js`
  uses `script -q -e -c` from util-linux (skipped on macOS, whose `script` has no
  `-e`) and kills the process *group*, not just `script`.

---

## Known Gotchas

- **Terminal options are CLI-only.** `--term-host`, `--term-port`, `--term-token`,
  `--fps`, `--gfx` and `--cdp` are read by `parseArgs()` only; `loadConfigFile()` is
  deliberately *not* extended with them, so nothing in `--config` can set them.
- **The CDP `Input.dispatchMouseEvent` `deltaY` sign is inverted relative to
  `/render/scroll`.** Qt's `angleDelta` says which way the wheel turned (+120 = up);
  the DOM/CDP `deltaY` says how far the content should move (+120 = down). The seam
  carries the Qt convention, so `cdp_frame_backend.cpp` sends `deltaY = -dy`. That
  conversion happens at that boundary and nowhere else.
- **CDP coordinates are CSS pixels, screenshots are device pixels.** Every
  `Input.*` x/y must be divided by the deviceScaleFactor. `Page.getLayoutMetrics`
  does *not* return that factor — it is the ratio of `layoutViewport` to
  `cssLayoutViewport`, and endpoints predating Chrome's 2020 `css*` fields report
  both halves in CSS pixels, so the ratio is 1 while the screenshot is still 2x.
  See `viewportForImage()` before touching any of this.
- **`tests/unit` builds `anoa-config-lib` from `src/config/config.cpp` alone,
  against `Qt6::Core` only.** Never add terminal (or browser, or http) sources to
  that target, and never make `config.cpp` depend on QtGui/QtNetwork/QtWebSockets —
  the unit test job builds no WebEngine.
- **`anoa-browser` uses three ports, not one.** `--port 9222` means HTTP on 9222,
  Chromium's internal DevTools on 9223 and the `CdpProxy` on 9224; `HttpServer`
  rewrites `webSocketDebuggerUrl` from +1 to +2 so clients land on the authenticated
  proxy. A terminal session started with `--cdp http://host:9222` therefore ends up
  dialling `ws://host:9224/...` — the host:port changing mid-session is correct.
- **Check `\since` before using any Qt API newer than 6.4.** The floor is
  `find_package(Qt6 6.4)` and distro builds really do use 6.4; CI's 6.7.3 will not
  catch the regression. `QWebSocket::errorOccurred` (6.5+) already needed a
  `QT_VERSION_CHECK` fallback.
- `parseArgs()` prints `Warning: --auth-token is not set; ...` unconditionally, so
  `anoa-browser terminal` prints it too even though terminal mode starts no CDP
  server. Known noise — tests filter stderr on the `anoa-browser terminal: ` prefix
  rather than counting lines.

---

## Architecture Decisions

### One binary, two modes (the merged-binary convention)

- **There is exactly one executable, `anoa-browser`.** The terminal viewer used to be
  a second binary (`anoa-term`); it was merged in and the separate target is gone.
  `add_executable` appears once in the top-level `CMakeLists.txt`.
- **`terminal` is a bare positional word, detected by a raw-argv pre-scan in
  `src/main.cpp` before any application object exists.** It cannot be a
  `QCommandLineParser` positional: the parser needs a live `QCoreApplication`, and
  which application class to construct is precisely what the word decides. The
  pre-scan also removes the word from argv (shift left, `argv[--argc] = nullptr`,
  then re-examine the current slot) so the parser only ever sees options and
  `--help` does not echo it back. `--headless` is scanned in the same loop for the
  same reason — `QT_QPA_PLATFORM` must be set before `QApplication`.
  Known limitation, accepted: the scan has no option-arity knowledge, so
  `--profile-name terminal` is read as the subcommand.
- **Terminal mode constructs a `QCoreApplication`, browser mode a `QApplication`.**
  The primary use case is SSH with no display, where `QApplication` aborts unless
  `QT_QPA_PLATFORM` is set. `QImage` decode/scale/convert under a plain
  `QCoreApplication` is verified by unit tests, not assumed.
- **Terminal sources are POSIX-only and conditionally compiled.** termios, `select()`
  on stdin and SIGWINCH have no MSVC equivalent, so `src/terminal/*` sits behind
  `if(NOT WIN32)` in `CMakeLists.txt` and `main.cpp` reports the unsupported platform
  at runtime under `#ifdef Q_OS_WIN`. Verify a Windows change negatively
  (`g++ -E -DQ_OS_WIN ... | grep -c runTerminal` must be 0) — a syntax check proves
  nothing.
- **No flag changes meaning by mode.** `--port` and `--auth-token` keep their browser
  meaning (the ports this process serves, the token it requires) in both modes.
  Terminal *connection* settings, which would otherwise collide with them, get their
  own `--term-host` / `--term-port` / `--term-token` names for the endpoint being
  viewed. Terminal-only options with nothing to collide with (`--fps`, `--gfx`,
  `--cdp`) keep plain names. Both modes share one `QCommandLineParser`, so every flag
  is listed in both `--help` outputs — that is the price of never overloading one.

### Other decisions

- **Transports sit behind the `FrameBackend` seam**, so `--cdp` selects an external
  CDP endpoint while the default path talks to `/render/*` over HTTP, and the viewer
  UI knows about neither. The seam is asynchronous by necessity (see Code Patterns).
- **CDP clients connect through `CdpProxy`, not Chromium directly**, which is what
  makes bearer-token auth possible at all.
- Version comes from the git tag: CI passes `-DANOA_VERSION_OVERRIDE=$TAG`, so
  `CMakeLists.txt`'s `project(... VERSION)` is only the local default.

---

## Dependencies & Integrations

- **Qt 6.4+** (6.7.3 in CI) — WebEngineWidgets, WebEngineCore, Network, WebSockets,
  Widgets; `Test`, `Core`, `Gui` additionally for the unit test target. Point CMake at
  it with `QT_PREFIX=` / `-DCMAKE_PREFIX_PATH=`.
- **libcups2-dev** is required on Linux: `Qt6PrintSupport` is a WebEngineWidgets
  dependency, and without CUPS headers `find_package` fails with the misleading
  *"Failed to find WebEngineWidgets"*.
- **Chromium**, embedded via QtWebEngine — no external browser is downloaded.
- **Node ≥ 20** for the test suites only (vitest, ws, node-fetch, @playwright/test,
  puppeteer-core). Each `tests/*` directory has its own `package.json`.
- **Homebrew** is the distribution channel: `.github/homebrew/*.tpl` are rendered by
  `update-homebrew-tap.yml` on release. macOS builds are codesigned/notarized using
  `.github/entitlements/anoa-browser.entitlements`.

---

## Development Setup

```bash
# Qt 6 + CMake must be installed first (Homebrew on macOS, aqtinstall or the
# distro packages on Linux). On Linux also: sudo apt-get install libcups2-dev

make build                       # Debug build -> build/anoa-browser
make release                     # Release build -> build-release/
make release-static QT_PREFIX=/opt/Qt/6.7.3/gcc_64   # what the release job runs
make test                        # re-configures with -DBUILD_TESTS=ON, runs ctest
make lint                        # clang-tidy over src/ (skipped if not installed)
make help                        # every target and the QT_PREFIX in effect

./build/anoa-browser --headless --no-sandbox --port 9222   # run the browser
./build/anoa-browser terminal --term-port 9222             # view it in a terminal
```

---

## Jonggrang Workflow

Jonggrang uses a **two-phase planning** flow so humans can review and edit a plan before AI decomposes it into tasks.

### Full workflow

```bash
# Phase 1 — generate a human-readable draft plan
jonggrang plan "add JWT authentication"
# → AI writes .jonggrang/.drafts/<session>/plan.md (high-level, no tasks yet)
# → Interactive options:
#     Approve           → run Phase 2 immediately
#     Edit with AI      → describe changes, AI revises plan, loop back
#     Edit in $EDITOR   → open editor, loop back
#     Save draft        → exit, run "jonggrang approve" later
#     Abort             → discard the draft plan.md

# Resume after accidental close:
jonggrang plan
# → no description → shows list of pending + archived plans
# → pick one → shows plan + interactive options again

# Phase 2 — approve plan → decompose into tasks
jonggrang approve
# → AI reads .jonggrang/.drafts/<session>/plan.md → runs `jonggrang task import` to create tasks
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
