---
feature: multi-tab-support
branch: feat/multi-tab-support
base: "master"
work_type: LARGE
description: One anoa process hosts many tabs, each with a stable id every agent command, CDP client and viewer can target
created_at: 2026-08-12T08:17:32.575Z
---

# Plan: Multi-Tab Support

## Approach

`AnoaBrowser` stops being a `QWebEngineView` and becomes a container that owns a registry of N views, one per tab, each with a monotonic short id (`t1`, `t2`, …) minted by anoa rather than borrowed from Chromium. Everything that today reaches "the page" — `grab()`, `width()/height()`, `sendClick()`, `setPage()` on the proxy — instead reaches *a* tab, defaulting to the active one, so single-tab behaviour is byte-identical and no existing command changes meaning. Two seams already do most of the work and are the reason this is tractable: `CdpProxy` opens its upstream from the client's own dial path, so N tabs mean N working WebSocket routes with no routing change, and the agent CLI is a CDP client that already *selects* a target out of `/json/list` — `--tab <id>` is a filter at that one selection point, not a parameter threaded through thirty commands. Refs need no work at all: they live in the DOM as `data-anoa-ref`, so they scope per tab for free.

The two genuinely new problems are identity and creation. Identity: `/json/list` is currently produced by byte-replacing the host:port in Chromium's response, which cannot carry an anoa id, so it becomes a parse-and-rebuild that emits one entry per tab with both its stable id and its own `webSocketDebuggerUrl`. Creation: `Target.createTarget` is asynchronous (a page exists before its DevTools target does), and `CdpExtensions::processCommand` can only answer synchronously — that seam has to grow a deferred reply, in the same signals/slots-not-blocking-calls style the `FrameBackend` seam already uses.

## Phases

1. **Tab registry** — `AnoaBrowser` becomes a container owning N views and minting stable ids; `init()` creates exactly one tab so nothing observable changes yet. Rework the stack ownership in `main.cpp` and `BrowserWindow`'s reparent/release contract, and route every current view accessor through "the active tab" so `/render/*` and the input helpers keep describing the page they describe today.

2. **Tab identity over the wire** — `/json/list` and `/json` become parsed and rebuilt from the registry instead of byte-replaced: one entry per tab, each carrying its anoa tab id alongside its own proxy-pointing `webSocketDebuggerUrl`, which is the shape Playwright and other CDP clients already expect. Establish the tab-id ↔ Chromium-target-id mapping once per tab and cache it. `CdpProxy`'s single `m_page` becomes a per-connection lookup keyed on the target id in the dial path, so locally-handled commands (`Page.printToPDF`, Security) act on the tab the client actually attached to.

3. **CDP `Target.*` for real** — answer `Target.createTarget`, `closeTarget`, `getTargets` and `activateTarget` from the registry instead of passing them to an engine that rejects them. This is the phase that adds the deferred-reply path to `CdpExtensions`, since a created tab cannot report its target id in the same turn.

4. **Per-tab profiles** — the registry owns `QWebEngineProfile`s: shared by default, isolated on request. A profile outlives every page using it and is destroyed only with its last tab; two tabs naming the same profile share one object, because two Qt profiles over one on-disk path is a corruption risk, not a duplicate.

5. **CLI surface** — `anoa tab new [url]` (prints the new id), `anoa tab list`, `anoa tab close <id>`, `anoa tab select <id>`, plus `--tab <id>` on every existing agent command, resolved at the CDP target-selection seam and defaulting to the active tab. Teach `isAgentCommand()` the new verb; `--tab` is read by the agent argument parser, never by `QCommandLineParser`. This phase updates its own lines in `anoa help` and `skills get commands` rather than deferring them — two e2e tests assert those documents match the real command set, so they fail the moment the verb lands undocumented.

6. **Viewers** — `?tab=<id>` on the `/render/*` endpoints (which gives the terminal viewer's HTTP backend tab switching for free), a tab strip in `BrowserWindow` consistent with the auto-hiding nav bar, and a tab-switch shortcut plus status-bar field in the terminal viewer alongside Ctrl-L / Ctrl-R / Ctrl-B. The tab strip lives *outside* the view for the same load-bearing reason the toolbar does: anything inside it appears in every screenshot and shifts the coordinate space every click is measured in.

7. **Docs sweep** — README (including the "`Target.createTarget` — the embedded engine cannot create tabs over CDP" row, now false), `docs/`, the embedded core skill, `.claude/skills/anoa/SKILL.md`, a tabs help group, and the AGENTS.md architecture decisions. Delete the "Not implemented" claim about tabs.

8. **Tests** — extend `tests/e2e/agent_cli.test.js`: two tabs holding independent pages, refs from one tab not resolving in the other, closing a tab (including closing the active one), and every command with no `--tab` still hitting the active tab. Add a `/json/list`-reports-one-entry-per-tab case to the Playwright suite, and per-tab isolation checks for phase 4's cookie behaviour.

## Key Decisions

- **Ids are anoa's, not Chromium's**: short monotonic `t1`, `t2` minted by the registry. Chromium's target GUIDs change when a page is recreated, so an agent that captured one would hold a stale handle; a stable id is the whole point of the feature. The registry keeps the mapping and `/json/list` publishes both.
- **`--tab` resolves at the CDP target-selection seam**, not per command. The agent CLI already picks one `page` target out of `/json/list` and warns that you can attach elsewhere; `--tab` narrows that pick. Absent, it picks the active tab — so every existing invocation keeps working unchanged.
- **Shared profile by default, isolation opt-in per tab** (`anoa tab new --profile <name>` for a persistent named session, `--isolated` for an ephemeral one). Sharing preserves today's behaviour, where a login in one place is a login everywhere; isolation is what makes parallel agent work useful and must be asked for. Solved at the Qt profile layer, as the feature description requires — a `QWebEngineProfile` with a name is on-disk persistent, one without is off-the-record, and that distinction is exactly the two modes needed.
- **A per-tab profile is reported as a real `browserContextId`** in `Target.getTargets`, replacing the synthetic `__anoa_default__` for tabs that have their own profile. Tabs on the shared profile keep reporting one shared context id.
- **`CdpExtensions` grows an asynchronous answer** rather than blocking on page creation, matching the existing rule that a seam is crossed by signals/slots and never by a blocking call.
- **The tab strip goes in `BrowserWindow`, outside the view**, preserving the documented invariant that `HttpServer` captures the view alone and reports its geometry as the coordinate space for `/render/click`.
- **Tab logic is not unit-testable in the existing targets.** `anoa-config-lib` and the two terminal libs are Qt6::Core-only by design and the unit CI job builds no WebEngine, so registry coverage lands in the integration and e2e suites. Any Core-only helper (id minting, `/json/list` shaping) may get its own fourth unit lib — never a WebEngine source folded into an existing one.

## Out of Scope

- Making CDP `Target.createBrowserContext` mint real Qt profiles. Playwright's `newContext` expects far more than a tab-scoped profile provides; it keeps returning a synthetic id, documented as such.
- Tab groups, pinning, reordering, session restore, or persisting the tab set across restarts.
- Window management: one process still means one window. No `Browser.getWindowForTarget` / `setWindowBounds` beyond today's stubs.
- Making `Browser.close` work, and any change to how the process exits.
- Multi-tab support in the *embedded* terminal browser (`anoa terminal` with no target), which opens no debugging port and reaches its browser through a pointer.
- Per-tab viewport or device emulation independent of the process-wide setting.
- Extension loading per tab — extensions stay process-wide.

## Dependencies

Builds on existing code and patterns rather than anything new: `AnoaBrowser`'s profile setup (`setupNamedProfile`) as the seed of the profile registry; `CdpProxy`'s path-derived upstream routing, which already makes per-tab WebSocket URLs work; `CdpExtensions`'s local-handle-or-pass-through dispatch; `HttpServer`'s discovery proxy and `webSocketDebuggerUrl` port rewrite; `CdpClient`'s `/json/list` target picker, which `--tab` narrows; the `data-anoa-ref` DOM attributes that already scope refs per page; `BrowserWindow`'s wrapper-not-parent toolbar contract; the `FrameBackend` asynchronous-seam pattern for the deferred CDP reply; and `runAgentCommand`'s own `takeOption` argument parsing, which is where `--tab` belongs. No new third-party dependency, and no Qt API above the declared 6.4 floor without a `\since` check.
