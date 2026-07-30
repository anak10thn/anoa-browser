import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startBrowser, stopBrowser, openDevtoolsWs, sendCdp } from './helpers.js';

describe('Page.printToPDF', () => {
  let proc;
  let ws;
  let cmdId = 100;

  beforeAll(async () => {
    proc = await startBrowser();
    ({ ws } = await openDevtoolsWs());
  }, 20000);

  afterAll(async () => {
    ws?.close();
    await stopBrowser(proc);
  });

  function nextId() { return ++cmdId; }

  function isPdfBase64(data) {
    const bytes = Buffer.from(data, 'base64');
    // %PDF- header
    return bytes[0] === 0x25 && bytes[1] === 0x50 &&
           bytes[2] === 0x44 && bytes[3] === 0x46 && bytes[4] === 0x2D;
  }

  // PDF-01
  it('Default params: result.data is a non-empty base64 string', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF', {}, nextId());
    expect(resp.result?.data).toBeTruthy();
    expect(typeof resp.result.data).toBe('string');
    expect(resp.result.data.length).toBeGreaterThan(0);
  });

  // PDF-02
  it('Decoded base64 starts with %PDF-', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF', {}, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
  });

  // PDF-03
  it('Landscape mode returns valid PDF', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF', { landscape: true }, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
  });

  // PDF-04
  it('A4 paper size returns valid PDF', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF',
      { paperWidth: 8.27, paperHeight: 11.69 }, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
  });

  // PDF-05
  it('printBackground:true returns valid PDF', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF',
      { printBackground: true }, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
  });

  // PDF-06
  it('Custom margins return valid PDF', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF', {
      marginTop: 0.5, marginBottom: 0.5, marginLeft: 0.5, marginRight: 0.5,
    }, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
  });

  // PDF-07
  it('Zero margins return valid PDF', async () => {
    const resp = await sendCdp(ws, 'Page.printToPDF', {
      marginTop: 0, marginBottom: 0, marginLeft: 0, marginRight: 0,
    }, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
  });

  // PDF-08
  it('Response id matches the command id', async () => {
    const id = 42;
    const resp = await sendCdp(ws, 'Page.printToPDF', {}, id);
    expect(resp.id).toBe(id);
  });

  // PDF-09
  it('PDF after page navigation contains rendered content', async () => {
    // Navigate to example.com then print; PDF should be non-trivial in size.
    await sendCdp(ws, 'Page.navigate', { url: 'https://example.com' }, nextId());
    // Poll until the page has actually loaded — a fixed sleep is flaky on
    // cold CI runners and prints the still-blank page.
    const deadline = Date.now() + 20000;
    let loaded = false;
    while (Date.now() < deadline && !loaded) {
      const evalResp = await sendCdp(ws, 'Runtime.evaluate', {
        expression: "document.readyState === 'complete' && location.hostname === 'example.com'",
        returnByValue: true,
      }, nextId());
      loaded = evalResp.result?.result?.value === true;
      if (!loaded) await new Promise((r) => setTimeout(r, 500));
    }
    expect(loaded).toBe(true);
    const resp = await sendCdp(ws, 'Page.printToPDF', {}, nextId());
    expect(isPdfBase64(resp.result.data)).toBe(true);
    // Rendered page PDF should be meaningfully larger than an empty-page PDF (~4KB)
    const bytes = Buffer.from(resp.result.data, 'base64');
    expect(bytes.length).toBeGreaterThan(4096);
  }, 40000);

  // PDF-10 (stretch goal)
  it('Concurrent PDF requests both return valid PDFs (no race)', async () => {
    const [r1, r2] = await Promise.all([
      sendCdp(ws, 'Page.printToPDF', {}, nextId()),
      sendCdp(ws, 'Page.printToPDF', {}, nextId()),
    ]);
    expect(isPdfBase64(r1.result.data)).toBe(true);
    expect(isPdfBase64(r2.result.data)).toBe(true);
  }, 60000);
});
