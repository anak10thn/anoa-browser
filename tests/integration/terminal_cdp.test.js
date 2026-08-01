// `anoa-browser terminal --cdp <url>` against a fake CDP endpoint.
//
// Why a fake endpoint: the four capability gaps between /render/* and CDP
// (inverted wheel sign, CSS-pixel vs image-pixel click coordinates under a
// device scale factor, the key triple, and text batching) all fail *silently*
// against a real Chrome — the page just scrolls the wrong way or the click
// lands somewhere else. Nothing errors, so only a test that reads the wire can
// catch them. A fake endpoint also makes the device scale factor a knob rather
// than a property of whatever machine CI runs on.
//
// ── A pty is required, and here is exactly why ──────────────────────────────
// terminal_app.cpp:191 refuses to start unless both stdin and stdout are a
// terminal, and terminal_ui.cpp takes the terminal size from TIOCGWINSZ and
// reads input in termios raw mode. Over a pipe the binary exits 1 with
// "stdin/stdout must be a terminal" before it has looked at --cdp at all, so a
// pipe exercises *nothing* on this path — not even the failure cases, which
// would pass for the wrong reason.
//
// A committed general pty harness is out of scope for this feature, so the pty
// here is `script(1)` from util-linux: one process that allocates a pty, runs
// the viewer on it, forwards our stdin into it and returns the child's exit
// status (-e). Two details make it enough rather than a harness:
//   * `stty rows N cols M` inside the same shell sets the window size on the
//     pty slave, which is what makes the cell -> CSS-pixel arithmetic below
//     predictable instead of dependent on the runner's terminal.
//   * `2><file>` inside the same shell keeps stderr off the pty, so the
//     one-line failure messages are readable without the alt-screen paint
//     mixed into them.
// BSD `script` (macOS) takes different arguments and has no -e; the pty-driven
// tests skip there rather than pretend. CI's integration job is ubuntu-22.04,
// where util-linux script is part of the base image.

import { describe, it, expect, afterEach } from 'vitest';
import { spawn, execFileSync } from 'child_process';
import { createServer } from 'http';
import { WebSocketServer } from 'ws';
import { deflateSync } from 'zlib';
import { mkdtempSync, readFileSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import { join } from 'path';

import { BINARY, waitForPort, freePort } from './helpers.js';

// ── pty availability ────────────────────────────────────────────────────────

const HAVE_UTIL_LINUX_SCRIPT = (() => {
  try {
    return /util-linux/.test(execFileSync('script', ['--version'], { encoding: 'utf8' }));
  } catch {
    return false;
  }
})();

// ── The terminal geometry every pty-driven test runs at ─────────────────────
//
// Fixed on purpose: the click-coordinate assertions are arithmetic over these
// numbers, so they are constants of the suite rather than per-test knobs.
const COLS = 100;
const ROWS = 30;
const FPS = 10; // slower than the 30 fps default: fewer frames, same behaviour

// The page as the fake endpoint reports it. deviceScaleFactor 2 means the
// screenshot comes back at twice the CSS viewport, which is the whole point of
// the click test.
const CSS_W = 200;
const CSS_H = 100;
const DSF = 2;
const IMAGE_W = CSS_W * DSF; // 400
const IMAGE_H = CSS_H * DSF; // 200

// What TerminalUi ends up with, derived rather than guessed:
//
//   tick()          -> requestRgbFrame(COLS, (ROWS - 1) * 2)
//   emitRgbFrame()  -> QImage::scaled(target, Qt::KeepAspectRatio)
//   onFrame()       -> dispCols = scaled.width, dispRows = (scaled.height+1)/2
//
// qsize.cpp's KeepAspectRatio is integer arithmetic, reproduced here.
function keepAspectRatio(srcW, srcH, boxW, boxH) {
  const rw = Math.trunc((boxH * srcW) / srcH);
  return rw <= boxW
    ? { w: rw, h: boxH }
    : { w: boxW, h: Math.trunc((boxW * srcH) / srcW) };
}
const SCALED = keepAspectRatio(IMAGE_W, IMAGE_H, COLS, (ROWS - 1) * 2);
const DISP_COLS = SCALED.w;
const DISP_ROWS = Math.trunc((SCALED.h + 1) / 2);

// mapCellToPage(), terminal_ui.cpp:183. The viewport it divides by is the CSS
// one when the backend got the coordinate space right, and the image one when
// it did not — which is what the click test turns into an assertion.
function cellToPage(cellX, cellY, viewportW, viewportH) {
  return {
    x: Math.trunc(((cellX + 0.5) * viewportW) / DISP_COLS),
    y: Math.trunc(((cellY + 0.5) * viewportH) / DISP_ROWS),
  };
}

// ── A real, decodable PNG ───────────────────────────────────────────────────
//
// It has to survive QImage::loadFromData on the halfblock path, so this builds
// an actual truecolor PNG rather than a stand-in: signature, IHDR, one deflated
// IDAT of unfiltered scanlines, IEND. A single flat colour keeps it a few
// hundred bytes and keeps the viewer's per-frame paint small.

const CRC_TABLE = (() => {
  const table = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c;
  }
  return table;
})();

function crc32(buf) {
  let crc = -1;
  for (let i = 0; i < buf.length; i++) crc = CRC_TABLE[(crc ^ buf[i]) & 0xff] ^ (crc >>> 8);
  return (crc ^ -1) >>> 0;
}

function pngChunk(type, data) {
  const head = Buffer.alloc(8);
  head.writeUInt32BE(data.length, 0);
  head.write(type, 4, 'ascii');
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(Buffer.concat([head.subarray(4), data])), 0);
  return Buffer.concat([head, data, crc]);
}

function makePng(width, height, [r, g, b] = [0x33, 0x66, 0x99]) {
  const ihdr = Buffer.alloc(13);
  ihdr.writeUInt32BE(width, 0);
  ihdr.writeUInt32BE(height, 4);
  ihdr[8] = 8; // bit depth
  ihdr[9] = 2; // colour type: truecolor
  // 10..12 stay 0: deflate, adaptive filtering, no interlace.

  const stride = width * 3;
  const raw = Buffer.alloc(height * (stride + 1));
  for (let y = 0; y < height; y++) {
    const row = y * (stride + 1);
    raw[row] = 0; // filter type None
    for (let x = 0; x < width; x++) {
      raw[row + 1 + x * 3] = r;
      raw[row + 2 + x * 3] = g;
      raw[row + 3 + x * 3] = b;
    }
  }

  return Buffer.concat([
    Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
    pngChunk('IHDR', ihdr),
    pngChunk('IDAT', deflateSync(raw)),
    pngChunk('IEND', Buffer.alloc(0)),
  ]);
}

const SCREENSHOT = makePng(IMAGE_W, IMAGE_H).toString('base64');

// ── The fake CDP endpoint ───────────────────────────────────────────────────

/**
 * HTTP + WebSocket on one port, answering just enough of CDP for the viewer.
 *
 * `targets` is what /json/list returns; pass [] for the empty-list failure
 * path. Every frame the client sends is recorded, so a test asserts on
 * `endpoint.calls` (all of them) or `endpoint.inputs` (the Input.* subset).
 */
async function startFakeCdp({ port, targets, metrics = true } = {}) {
  const httpPaths = [];
  const wsPaths = [];
  const calls = [];

  const http = createServer((req, res) => {
    httpPaths.push(req.url);
    if (req.url.split('?')[0] === '/json/list') {
      const body = JSON.stringify(targets);
      res.writeHead(200, { 'Content-Type': 'application/json', 'Content-Length': body.length });
      res.end(body);
      return;
    }
    res.writeHead(404).end();
  });

  const wss = new WebSocketServer({ server: http });
  wss.on('connection', (ws, req) => {
    wsPaths.push(req.url);
    ws.on('message', (data) => {
      let msg;
      try {
        msg = JSON.parse(data.toString());
      } catch {
        return; // not our problem to diagnose; the assertions read `calls`
      }
      calls.push(msg);
      ws.send(JSON.stringify({ id: msg.id, result: resultFor(msg.method) }));
    });
  });

  function resultFor(method) {
    if (method === 'Page.captureScreenshot') return { data: SCREENSHOT };
    if (method === 'Page.getLayoutMetrics') {
      if (!metrics) return {};
      // Chrome reports the deprecated viewports in device pixels and the css*
      // ones in CSS pixels; their ratio *is* the device scale factor, which is
      // how cdp_frame_backend.cpp recovers it (getLayoutMetrics has no such
      // field). Both spellings are filled in so the reply is realistic even
      // though the backend reads cssLayoutViewport/layoutViewport.
      const css = { clientWidth: CSS_W, clientHeight: CSS_H, pageX: 0, pageY: 0 };
      const device = { clientWidth: IMAGE_W, clientHeight: IMAGE_H, pageX: 0, pageY: 0 };
      return {
        layoutViewport: device,
        cssLayoutViewport: css,
        visualViewport: { ...device, scale: 1 },
        cssVisualViewport: { ...css, scale: 1 },
        contentSize: { x: 0, y: 0, width: IMAGE_W, height: IMAGE_H },
        cssContentSize: { x: 0, y: 0, width: CSS_W, height: CSS_H },
      };
    }
    return {};
  }

  await new Promise((resolve, reject) => {
    http.once('error', reject);
    http.listen(port, '127.0.0.1', resolve);
  });
  await waitForPort(port, 5000);

  return {
    port,
    httpPaths,
    wsPaths,
    calls,
    get inputs() {
      return calls.filter((c) => c.method.startsWith('Input.'));
    },
    /** Every Input.* call of one method, most recent last. */
    inputsOf(method) {
      return calls.filter((c) => c.method === method);
    },
    async close() {
      for (const client of wss.clients) client.terminate();
      await new Promise((resolve) => wss.close(resolve));
      await new Promise((resolve) => http.close(resolve));
    },
  };
}

function pageTarget(port, id = 'FAKEPAGE00000000000000000000000A') {
  return {
    id,
    type: 'page',
    title: 'fake target',
    url: 'about:blank',
    webSocketDebuggerUrl: `ws://127.0.0.1:${port}/devtools/page/${id}`,
  };
}

// ── Driving the viewer ──────────────────────────────────────────────────────

const shq = (s) => `'${String(s).replace(/'/g, `'\\''`)}'`;

/**
 * Run `anoa-browser terminal --cdp <url>` on a pty at COLS x ROWS.
 *
 * Returns a handle whose `send()` writes raw bytes to the viewer's stdin (they
 * reach it through the pty exactly as a terminal would deliver them, i.e. as
 * the escape sequences a real keypress or SGR mouse report produces), and
 * whose `exited` resolves with { code, stderr } once it is gone.
 */
function launchTerminal(cdpUrl, extraArgs = []) {
  const dir = mkdtempSync(join(tmpdir(), 'anoa-term-cdp-'));
  const errPath = join(dir, 'stderr.txt');

  const argv = [
    'terminal',
    '--gfx',
    'halfblock', // never 'auto': kitty/iterm would probe the pty for a cell size
    '--fps',
    String(FPS),
    '--cdp',
    cdpUrl,
    ...extraArgs,
  ];
  const inner = `stty rows ${ROWS} cols ${COLS}; exec ${shq(BINARY)} ${argv.map(shq).join(' ')} 2>${shq(errPath)}`;

  // detached: script leads its own process group, so a test that has to give
  // up can take the whole tree (script -> sh -> anoa-browser) down at once.
  // Killing script alone would orphan the viewer, which would then sit in its
  // reconnect loop for the rest of the run.
  const proc = spawn('script', ['-q', '-e', '-c', inner, '/dev/null'], {
    stdio: ['pipe', 'pipe', 'pipe'],
    detached: true,
    env: { ...process.env, TERM: 'xterm-256color' },
  });

  // The viewer repaints the whole grid every frame, so only a tail is kept —
  // enough for the status bar, which is the last thing written each frame.
  let tail = '';
  proc.stdout.on('data', (d) => {
    tail = (tail + d.toString('latin1')).slice(-16384);
  });
  proc.stderr.on('data', () => {}); // script's own diagnostics; the viewer's go to errPath

  let exit = null;
  const exited = new Promise((resolve) => {
    proc.once('close', (code) => {
      let stderr = '';
      try {
        stderr = readFileSync(errPath, 'utf8');
      } catch { /* the shell never got far enough to create it */ }
      exit = { code, stderr };
      resolve(exit);
    });
  });

  return {
    proc,
    get exit() { return exit; },
    exited,
    get tail() { return tail; },
    send(bytes) {
      proc.stdin.write(Buffer.from(bytes, 'latin1'));
    },
    /** Ctrl-C: ISIG is off in raw mode, so this is byte 3, not a signal. */
    async quit() {
      if (exit) return exit;
      try {
        proc.stdin.write(Buffer.from([3]));
      } catch { /* already gone */ }
      const result = await Promise.race([
        exited,
        new Promise((resolve) => setTimeout(() => resolve(null), 5000)),
      ]);
      if (result) return result;
      try {
        process.kill(-proc.pid, 'SIGKILL'); // the group, not just script
      } catch { /* already reaped */ }
      return exited;
    },
    async cleanup() {
      await this.quit();
      rmSync(dir, { recursive: true, force: true });
    },
  };
}

/** Poll until `fn()` is truthy, failing loudly (never silently) on timeout. */
async function waitUntil(fn, what, timeout = 15000) {
  const deadline = Date.now() + timeout;
  for (;;) {
    const value = fn();
    if (value) return value;
    if (Date.now() >= deadline) throw new Error(`timed out after ${timeout}ms waiting for ${what}`);
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
}

/**
 * Wait until the viewer has painted at least two frames from the endpoint.
 *
 * Two, not one: the first capture is what fills TerminalUi's display map, and
 * input dispatched before that map exists is dropped as "click outside page".
 * The second reply proves the first was consumed.
 */
async function waitForFrames(endpoint, term, count = 2) {
  await waitUntil(
    () => !term.exit && endpoint.inputsOf('Page.captureScreenshot').length >= count,
    `${count} screenshot captures (viewer stderr: ${term.exit?.stderr ?? 'still running'})`,
  );
  // The reply is answered inside the viewer's event loop; give the frame it
  // triggers time to land before anything depends on the display map.
  await new Promise((resolve) => setTimeout(resolve, 200));
}

const ERR_PREFIX = 'anoa-browser terminal: ';

/**
 * The distinct viewer error lines in a captured stderr.
 *
 * Two filters, both deliberate. Lines without the prefix are dropped because
 * parseArgs() emits an unrelated "--auth-token is not set" warning on every
 * invocation, terminal mode included — pre-existing, not this path's, and
 * asserting it away either way would encode it. Duplicates are collapsed
 * because a discovery failure is written twice by design: once by
 * CdpClient::failDiscovery() while the alt screen is still up (where a user
 * would never see it) and once by runTerminal() after the terminal has been
 * handed back. Redirecting stderr to a file is what makes both copies visible
 * here; what the user reads is one line.
 */
function viewerErrors(stderr) {
  return [...new Set(stderr.split('\n').filter((line) => line.startsWith(ERR_PREFIX)))];
}

// SGR mouse reports, exactly what a terminal writes for a press: the button
// field is 0-2 for left/middle/right and 64/65 for wheel up/down, and the
// coordinates are 1-based.
const sgrPress = (btn, cellX, cellY) => `\x1b[<${btn};${cellX + 1};${cellY + 1}M`;

// ── Tests ───────────────────────────────────────────────────────────────────

describe.skipIf(!HAVE_UTIL_LINUX_SCRIPT)('terminal --cdp against a fake CDP endpoint', () => {
  let endpoint;
  let term;

  afterEach(async () => {
    if (term) await term.cleanup();
    if (endpoint) await endpoint.close();
    term = undefined;
    endpoint = undefined;
  });

  // ── Discovery ─────────────────────────────────────────────────────────────

  // TCDP-01
  it('http:// discovers via /json/list and dials the advertised webSocketDebuggerUrl', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitUntil(() => endpoint.wsPaths.length > 0, 'a WebSocket connection');

    expect(endpoint.httpPaths).toContain('/json/list');
    expect(endpoint.wsPaths).toEqual([
      '/devtools/page/FAKEPAGE00000000000000000000000A',
    ]);
  }, 30000);

  // TCDP-02
  it('ws:// connects directly, with no /json/list request at all', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`ws://127.0.0.1:${port}/devtools/page/DIRECT`);
    await waitUntil(() => endpoint.wsPaths.length > 0, 'a WebSocket connection');
    await waitForFrames(endpoint, term, 1);

    expect(endpoint.wsPaths).toEqual(['/devtools/page/DIRECT']);
    expect(endpoint.httpPaths).toEqual([]);
  }, 30000);

  // TCDP-03
  it('paints the CSS viewport, not the screenshot size, in the status bar', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);

    expect(term.tail).toContain(`cdp 127.0.0.1:${port} ${CSS_W}x${CSS_H}`);
    expect(term.tail).not.toContain(`${IMAGE_W}x${IMAGE_H}`);
  }, 30000);

  // ── The four silent capability gaps ───────────────────────────────────────

  // TCDP-04 — the wheel sign. /render/scroll takes a Qt angleDelta, where +120
  // means the wheel turned up; CDP's deltaY is the DOM one, where positive
  // moves the *content* down. The viewer sends +120 for a wheel-up either way,
  // so the CDP backend has to negate it, and nothing anywhere reports it if it
  // does not — the page just scrolls backwards.
  it('a wheel-up produces mouseWheel deltaY = -120, inverted from the +120 /render/scroll sends', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);

    term.send(sgrPress(64, 10, 5)); // wheel up, over the page
    const wheels = await waitUntil(() => {
      const found = endpoint
        .inputsOf('Input.dispatchMouseEvent')
        .filter((c) => c.params.type === 'mouseWheel');
      return found.length ? found : null;
    }, 'a mouseWheel event');

    const RENDER_SCROLL_ANGLE_DELTA = 120; // what terminal_ui.cpp hands the seam
    expect(wheels[0].params.deltaY).toBe(-RENDER_SCROLL_ANGLE_DELTA);
    expect(Math.sign(wheels[0].params.deltaY)).toBe(-Math.sign(RENDER_SCROLL_ANGLE_DELTA));
    expect(wheels[0].params.deltaX).toBe(0);

    // And the opposite notch is the opposite sign, so the assertion above is
    // pinning an inversion rather than a constant.
    term.send(sgrPress(65, 10, 5)); // wheel down
    const both = await waitUntil(() => {
      const found = endpoint
        .inputsOf('Input.dispatchMouseEvent')
        .filter((c) => c.params.type === 'mouseWheel');
      return found.length >= 2 ? found : null;
    }, 'a second mouseWheel event');
    expect(both[1].params.deltaY).toBe(RENDER_SCROLL_ANGLE_DELTA);
  }, 30000);

  // TCDP-05 — the HiDPI off-target bug. With deviceScaleFactor 2 the
  // screenshot is 400x200 while the page is 200x100 CSS pixels, so a click
  // mapped against the image lands at twice the intended offset and hits the
  // wrong element. Nothing errors; the click just goes somewhere else.
  it('a click at a known cell arrives in CSS pixels, not image pixels', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);

    const CELL_X = 10;
    const CELL_Y = 5;
    const css = cellToPage(CELL_X, CELL_Y, CSS_W, CSS_H);
    const image = cellToPage(CELL_X, CELL_Y, IMAGE_W, IMAGE_H);
    expect(css).not.toEqual(image); // the test can tell the two apart

    term.send(sgrPress(0, CELL_X, CELL_Y));
    const events = await waitUntil(() => {
      const found = endpoint
        .inputsOf('Input.dispatchMouseEvent')
        .filter((c) => c.params.type === 'mousePressed' || c.params.type === 'mouseReleased');
      return found.length >= 2 ? found : null;
    }, 'a press/release pair');

    const [pressed, released] = events;
    expect(pressed.params.type).toBe('mousePressed');
    expect({ x: pressed.params.x, y: pressed.params.y }).toEqual(css);
    expect({ x: pressed.params.x, y: pressed.params.y }).not.toEqual(image);
    expect(pressed.params.button).toBe('left');
    expect(pressed.params.buttons).toBe(1);
    expect(pressed.params.clickCount).toBe(1);

    // The release has to agree on the position or Chromium synthesises no
    // click, and it has to drop the mask or the next move becomes a drag.
    expect(released.params.type).toBe('mouseReleased');
    expect({ x: released.params.x, y: released.params.y }).toEqual(css);
    expect(released.params.buttons).toBe(0);
  }, 30000);

  // TCDP-06 — the key triple. Chromium accepts a dispatchKeyEvent with a
  // missing windowsVirtualKeyCode or an unknown code and simply does nothing
  // with it, so a half-filled row is invisible without this assertion.
  it('named keys carry key, code and windowsVirtualKeyCode', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);

    term.send('\r'); // Enter
    term.send('\x7f'); // Backspace (the terminal sends DEL, not BS)
    term.send('\t'); // Tab
    term.send('\x1b[A'); // Arrow up

    const events = await waitUntil(() => {
      const found = endpoint.inputsOf('Input.dispatchKeyEvent');
      return found.length >= 8 ? found : null; // four keys, down + up each
    }, 'four key down/up pairs');

    const downs = events.filter((e) => e.params.type !== 'keyUp');
    expect(downs.map((e) => e.params.key)).toEqual([
      'Enter',
      'Backspace',
      'Tab',
      'ArrowUp',
    ]);
    expect(downs.map((e) => e.params.code)).toEqual([
      'Enter',
      'Backspace',
      'Tab',
      'ArrowUp',
    ]);
    expect(downs.map((e) => e.params.windowsVirtualKeyCode)).toEqual([13, 8, 9, 38]);

    // A key that inserts a character is keyDown + text; one that inserts
    // nothing must be rawKeyDown with no text member at all, because keyDown
    // with an empty text is accepted and does nothing.
    const byKey = Object.fromEntries(downs.map((e) => [e.params.key, e.params]));
    expect(byKey.Enter.type).toBe('keyDown');
    expect(byKey.Enter.text).toBe('\r');
    expect(byKey.Tab.type).toBe('keyDown');
    expect(byKey.Tab.text).toBe('\t');
    expect(byKey.Backspace.type).toBe('rawKeyDown');
    expect(byKey.Backspace).not.toHaveProperty('text');
    expect(byKey.ArrowUp.type).toBe('rawKeyDown');
    expect(byKey.ArrowUp).not.toHaveProperty('text');

    // Every down is matched by a keyUp, and no keyUp carries text.
    const ups = events.filter((e) => e.params.type === 'keyUp');
    expect(ups).toHaveLength(4);
    for (const up of ups) expect(up.params).not.toHaveProperty('text');
  }, 30000);

  // TCDP-07 — typing. A burst is batched into one insertText rather than one
  // key event per character, which is also what makes pasted non-Latin text
  // work at all: a virtual-key table has nothing to say about it.
  it('typing produces one Input.insertText with the whole batched burst', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);

    term.send('hello');
    const inserts = await waitUntil(
      () => {
        const found = endpoint.inputsOf('Input.insertText');
        return found.length ? found : null;
      },
      'an Input.insertText',
    );

    expect(inserts[0].params.text).toBe('hello');
    // Not one dispatchKeyEvent per character on the way past.
    expect(endpoint.inputsOf('Input.dispatchKeyEvent')).toHaveLength(0);
  }, 30000);

  // ── Failure paths ─────────────────────────────────────────────────────────

  // TCDP-08
  it('an unreachable endpoint exits non-zero with one line naming it', async () => {
    const port = await freePort(); // nothing is listening on it
    term = launchTerminal(`http://127.0.0.1:${port}`);

    const { code, stderr } = await term.exited;
    expect(code).not.toBe(0);

    const lines = viewerErrors(stderr);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toMatch(
      new RegExp(`^${ERR_PREFIX}cannot reach http://127\\.0\\.0\\.1:${port}/json/list: `),
    );
  }, 30000);

  // TCDP-09
  it('an empty /json/list exits non-zero with one line saying so', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [] });

    term = launchTerminal(`http://127.0.0.1:${port}`);

    const { code, stderr } = await term.exited;
    expect(code).not.toBe(0);

    const lines = viewerErrors(stderr);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toMatch(
      new RegExp(
        `^${ERR_PREFIX}http://127\\.0\\.0\\.1:${port}/json/list lists no debuggable targets`,
      ),
    );
  }, 30000);
});
