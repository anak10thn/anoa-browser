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
// The pty harness (`script(1)`), the fixed terminal geometry and the viewer
// error-line filter live in helpers.js — terminal_http.test.js is the same
// suite pointed at the other transport and shares all of it. What stays here
// is the fake CDP endpoint and the assertions about the CDP wire.

import { describe, it, expect, afterEach } from 'vitest';
import { createServer } from 'http';
import { WebSocketServer } from 'ws';
import { deflateSync } from 'zlib';

import {
  HAVE_UTIL_LINUX_SCRIPT,
  TERM_COLS as COLS,
  TERM_ROWS as ROWS,
  VIEWER_ERR_PREFIX as ERR_PREFIX,
  freePort,
  launchViewer,
  sgrPress,
  viewerErrors,
  waitForPort,
  waitUntil,
} from './helpers.js';

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
 * path, or `listBody` to answer with a byte string that is not JSON at all.
 * Every frame the client sends is recorded, so a test asserts on
 * `endpoint.calls` (all of them) or `endpoint.inputs` (the Input.* subset).
 */
async function startFakeCdp({ port, targets, metrics = true, listBody = null } = {}) {
  const httpPaths = [];
  const wsPaths = [];
  const calls = [];

  const http = createServer((req, res) => {
    httpPaths.push(req.url);
    if (req.url.split('?')[0] === '/json/list') {
      const body = listBody ?? JSON.stringify(targets);
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
    /**
     * Drop every attached CDP socket without shutting the endpoint down —
     * what a Chrome that quits, or a tab that closes, looks like to a client
     * that had already attached successfully.
     */
    dropConnections() {
      const dropped = wss.clients.size;
      for (const client of wss.clients) client.close();
      return dropped;
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

const launchTerminal = (cdpUrl, extraArgs = []) =>
  launchViewer(['--cdp', cdpUrl, ...extraArgs]);

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

  // TCDP-10 — the common real-world loss: Chrome quits, or the tab closes,
  // after the viewer had attached successfully. CdpClient carries no retry
  // budget once a connection has worked ("a session that worked and then
  // blipped should heal, not exit"), and this pins which of the two documented
  // outcomes the code actually takes: it stays up, re-dials, and resumes
  // capturing — it does not exit, and it does not sit there silently.
  //
  // The status bar's link field is not what this reads. At the 100 columns the
  // harness runs at, the row is already over budget before any link, so the
  // link yields by design and is never painted; that rule is covered where it
  // is observable, in the TerminalUi unit suite at 80 columns.
  it('survives the endpoint dropping the socket after a successful attach', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [pageTarget(port)] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);
    const capturesBefore = endpoint.inputsOf('Page.captureScreenshot').length;

    expect(endpoint.dropConnections()).toBe(1);

    // A second dial, and frames flowing again through it.
    await waitUntil(
      () => !term.exit && endpoint.wsPaths.length >= 2,
      `a re-dial (viewer stderr: ${term.exit?.stderr ?? 'still running'})`,
    );
    await waitUntil(
      () => !term.exit && endpoint.inputsOf('Page.captureScreenshot').length > capturesBefore,
      'a capture after the reconnect',
    );
    expect(term.exit).toBeNull();

    // And it is still interactive: Ctrl-C leaves cleanly rather than needing
    // the process group killed, and nothing was reported as fatal.
    const result = await term.quit();
    expect(result).not.toBeNull();
    expect(viewerErrors(result.stderr)).toEqual([]);
  }, 30000);

  // TCDP-11 — more than one debuggable target. The viewer is a one-page
  // viewer by design, so "ambiguous" resolves to the first entry rather than
  // to a prompt. Which entry it picks is the whole contract: picking the last
  // one would attach to a different tab than the user expects, and nothing
  // about the resulting session would look wrong.
  it('attaches to the first page target when /json/list offers several', async () => {
    const port = await freePort();
    const first = pageTarget(port, 'FIRSTPAGE0000000000000000000000A');
    const second = pageTarget(port, 'SECONDPAGE000000000000000000000B');
    endpoint = await startFakeCdp({ port, targets: [first, second] });

    term = launchTerminal(`http://127.0.0.1:${port}`);
    await waitForFrames(endpoint, term);

    expect(endpoint.wsPaths).toEqual(['/devtools/page/FIRSTPAGE0000000000000000000000A']);
    expect(term.exit).toBeNull();
  }, 30000);

  // TCDP-12 — a body that is not JSON. Distinct from the empty-list case:
  // that one is a well-formed answer with nothing in it, this one never
  // parses, and the two used to be indistinguishable from the outside.
  it('a malformed /json/list exits non-zero with one line saying so', async () => {
    const port = await freePort();
    endpoint = await startFakeCdp({ port, targets: [], listBody: '<html>not json</html>' });

    term = launchTerminal(`http://127.0.0.1:${port}`);

    const { code, stderr } = await term.exited;
    expect(code).not.toBe(0);

    const lines = viewerErrors(stderr);
    expect(lines).toHaveLength(1);
    expect(lines[0]).toContain(`http://127.0.0.1:${port}/json/list`);
    // Whatever the wording, it must not be the empty-list one — that would
    // send someone looking for a page that never existed instead of at the
    // thing answering on the port.
    expect(lines[0]).not.toContain('lists no debuggable targets');
  }, 30000);
});
