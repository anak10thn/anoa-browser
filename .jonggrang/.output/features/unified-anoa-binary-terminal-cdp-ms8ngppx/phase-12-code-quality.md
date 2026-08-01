# Phase 12 — Code Quality Review

Scope: `master...HEAD` on `feat/unified-anoa-binary-terminal-cdp` — 19 commits,
~5,100 insertions, of which ~2,400 lines are the new `src/terminal/` subsystem.

Validation run: `make test QT_PREFIX=/opt/Qt/6.7.3/gcc_64` — builds clean under
`-Wall -Wextra -Wpedantic` (0 warnings), `ConfigTests` passes.

## Overall

The branch is above the bar for maintainability. The `FrameBackend` seam holds:
`terminal_ui.cpp` never names HTTP or CDP, `terminal_app.cpp` makes the one mode
decision, and the two transports are genuinely swappable. Comments consistently
explain *why* rather than restating *what* — the wheel-delta sign inversion, the
CSS-pixel vs device-pixel viewport derivation, the `rawKeyDown`/`keyDown` split
and the discovery generation counter are each documented at the point where a
future reader would otherwise get them wrong. Nothing below argues with the
architecture; these are cleanups.

Two genuine defects were found and filed separately in `bugs.md` (bug-001,
bug-002). They are correctness issues, not code-quality ones, and are excluded
from the list below.

## Findings

### 1. `Config::terminalMode` is written but never read — dead field
`src/config/config.h:20`, set at `src/main.cpp:56`. No call site reads it;
`runTerminal()` receives the whole `Config` and branches on `config.cdpUrl`
instead. A `bool` in a config struct that nothing consumes reads as a mode gate
and will mislead. Remove it, or have `runTerminal()` assert on it.

### 2. Hand-rolled base64 duplicates `QByteArray::toBase64()`
`src/terminal/terminal_ui.cpp:28-58` — 30 lines of table-driven base64. It came
from `tools/anoa-term/anoa_term.cpp`, which deliberately had no Qt dependency;
that constraint disappeared when the binary was merged. The file already
includes `<QByteArray>` and `renderGfx()` takes one, so
`png.toBase64()` replaces the function outright. Unused padding/encoding paths
are exactly where a rarely-exercised bug hides.

### 3. `RenderHttpClient` sets no socket timeouts
`src/terminal/render_http_client.cpp:134-216` — `getaddrinfo()`, `::connect()`
and the `read()` drain loop all block indefinitely; there is no `SO_RCVTIMEO`,
`SO_SNDTIMEO` or non-blocking path anywhere in the file. Under `anoa-term` this
stalled a `select()` loop. It now stalls the **Qt event loop**, so a peer that
accepts and then goes quiet freezes the frame timer and the stdin
`QSocketNotifier` together — Ctrl-C stops working until the peer closes.
`frame_backend.h:39-41` explicitly permits a synchronous transport, so this is
within contract, but the cost changed with the architecture and neither the
header nor the class comment says the HTTP backend can wedge the loop.
Recommend a receive/send timeout pair plus one line in
`render_http_client.h` stating the blocking behaviour.

### 4. Unused public API surface on the new classes
Zero call sites in `src/` or `tests/`:
- `CdpClient::navigate()` (`cdp_client.h:92`, impl `cdp_client.cpp:214`)
- `CdpClient::pendingCount()` (`cdp_client.h:104`)
- `CdpClient::setRequestTimeout()` / `requestTimeout()` (`cdp_client.h:110-111`)
  — the 5000 ms default is the only value the program ever uses
- `CdpClient::exitOnDiscoveryFailure()` getter (`cdp_client.h:119`); the
  *setter* is used, at `terminal_app.cpp:306`
- `TerminalUi::gfxMode()` (`terminal_ui.h:77`)
- the whole `CdpModifier` enum — `ModAlt`/`ModCtrl`/`ModMeta`/`ModShift`
  (`cdp_frame_backend.cpp:50-55`)

Each is documented as deliberate ("the one navigation primitive this iteration
ships", "the names exist so the day it grows one there is a single place to read
the convention off"), which is a defensible call for the modifier enum and the
timeout knob. But this is untested, uncompiled-against surface that will drift
from the wire format it claims to encode. Recommend keeping `CdpModifier` and
`setRequestTimeout()` (both are real documentation) and dropping the rest.

### 5. `pngDimensions()` is duplicated across the two backends
`cdp_frame_backend.cpp:21` and `render_http_client.cpp:67` — the same 24-byte
IHDR walk and the same signature check, differing only in `QByteArray` vs
`std::string`. The comment on the first copy points at the second and justifies
it as backend independence. That reasoning is sound for transport logic; it is
weak for a byte-format parser, and the two copies must now stay in sync by hand.
A shared helper taking `(const char *, size_t)` serves both without coupling the
backends to each other.

### 6. `kErrPrefix` is defined twice
`cdp_client.cpp:36` and `terminal_app.cpp:50`, with a comment on the first
saying "Same wording as terminal_app.cpp, because to the user it is one
program." A constant whose correctness is defined as *being equal to the other
copy* belongs in one header.

### 7. Status-bar truncation and echo are byte-wise, not character-wise
`terminal_ui.cpp:373-376`: `status.resize(m_cols)` truncates a UTF-8
`std::string` that can contain the em dash and ellipsis at `kEmDash`/`kEllipsis`,
so a cut landing mid-character emits a partial UTF-8 sequence to the terminal.
The padding on the next line is computed from byte length, so a bar carrying
multibyte characters renders short of `m_cols` columns. Same class of issue at
line 502, where `pending.substr(0, 12)` can split a pasted multibyte character
in the "typed ..." echo. Cosmetic only — no memory-safety consequence — but it
is the kind of thing that looks like terminal corruption when reported.

## Tooling note (not a code finding)

`jonggrang bug --feature <id> "..."` cannot work non-interactively in this
multi-feature repo: the global option parser at `/usr/bin/jonggrang:4990`
consumes `--feature` into `WORK_FEATURE_ID` before the `bug` subcommand's own
parser (line 2372) can see it, so the handler falls through to "Multiple
features found. Use --feature <featureId>." — the exact flag that was passed.
bug-001 and bug-002 were therefore written to `bugs.md` directly, in
`appendBug()`'s format; `jonggrang bug list` parses them correctly. They are
**not** converted to tasks. Run
`jonggrang bug convert --feature unified-anoa-binary-terminal-cdp-ms8ngppx`
from an interactive terminal to queue them.
