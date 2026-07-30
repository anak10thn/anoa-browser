import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import WebSocket from 'ws';
import fetch from 'node-fetch';
import {
  startBrowser, stopBrowser, openDevtoolsWs, sendCdp,
  BASE_URL, WS_BASE, WS_PORT,
} from './helpers.js';

const AUTH_TOKEN = 'inttest-ws-abc';

// Helper: connect to the WS proxy via the devtools URL returned by /json/list.
async function getPageWsUrl() {
  const resp = await fetch(`${BASE_URL}/json/list`);
  const list = await resp.json();
  if (!list.length) throw new Error('/json/list is empty');
  return list[0].webSocketDebuggerUrl;
}

describe('WebSocket CDP Proxy (no auth)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser();
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // WS-01
  it('Client connects successfully to proxy via devtools URL', async () => {
    const wsUrl = await getPageWsUrl();
    await new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl);
      ws.once('open', () => { ws.close(); resolve(); });
      ws.once('error', reject);
    });
  });

  // WS-08
  it('Malformed JSON message leaves connection functional for subsequent commands', async () => {
    const { ws } = await openDevtoolsWs();
    ws.send('not json at all');
    await new Promise((r) => setTimeout(r, 200));
    // Connection must be usable after the malformed frame
    const resp = await sendCdp(ws, 'Browser.getVersion', {}, 888);
    ws.close();
    expect(resp.id).toBe(888);
    expect(resp.result).toHaveProperty('product');
  });

  // WS-09
  it('Message without id field leaves proxy state intact for subsequent commands', async () => {
    const { ws } = await openDevtoolsWs();
    ws.send(JSON.stringify({ method: 'Browser.getVersion' }));
    await new Promise((r) => setTimeout(r, 200));
    // Proxy must route the next properly-formed command correctly
    const resp = await sendCdp(ws, 'Browser.getVersion', {}, 777);
    ws.close();
    expect(resp.id).toBe(777);
    expect(resp.result).toHaveProperty('product');
  });

  // WS-12
  it('Browser.getVersion passthrough returns version info', async () => {
    const { ws } = await openDevtoolsWs();
    const response = await sendCdp(ws, 'Browser.getVersion', {}, 1);
    ws.close();
    expect(response).toHaveProperty('id', 1);
    expect(response.result).toHaveProperty('product');
  });

  // WS-06
  it('Two concurrent clients can both connect', async () => {
    const wsUrl = await getPageWsUrl();
    const [ws1, ws2] = await Promise.all([
      new Promise((res, rej) => {
        const w = new WebSocket(wsUrl); w.once('open', () => res(w)); w.once('error', rej);
      }),
      new Promise((res, rej) => {
        const w = new WebSocket(wsUrl); w.once('open', () => res(w)); w.once('error', rej);
      }),
    ]);
    ws1.close();
    ws2.close();
  });

  // WS-11
  it('Large message (>64 KB) is forwarded and response contains the correct string result', async () => {
    const { ws } = await openDevtoolsWs();
    const bigString = 'x'.repeat(70 * 1024);
    const response = await sendCdp(ws, 'Runtime.evaluate', {
      expression: JSON.stringify(bigString),
    }, 42);
    ws.close();
    expect(response).toHaveProperty('id', 42);
    expect(response.result?.result?.type).toBe('string');
    expect(response.result?.result?.value?.length).toBe(bigString.length);
  });
});

describe('WebSocket CDP Proxy (with auth token)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser([`--auth-token=${AUTH_TOKEN}`]);
  }, 20000);

  afterAll(() => stopBrowser(proc));

  async function getProtectedWsUrl() {
    const resp = await fetch(`${BASE_URL}/json/list`, {
      headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
    });
    const list = await resp.json();
    return list[0].webSocketDebuggerUrl;
  }

  // WS-02
  it('Client with correct token in URL is accepted', async () => {
    const wsUrl = await getProtectedWsUrl();
    const tokenUrl = `${wsUrl}?token=${AUTH_TOKEN}`;
    await new Promise((resolve, reject) => {
      const ws = new WebSocket(tokenUrl);
      ws.once('open', () => { ws.close(); resolve(); });
      ws.once('error', reject);
      ws.once('unexpected-response', (_req, resp) =>
        reject(new Error(`HTTP ${resp.statusCode}`)));
    });
  });

  // WS-03
  it('Client with correct Authorization header is accepted', async () => {
    const wsUrl = await getProtectedWsUrl();
    await new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl, {
        headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
      });
      ws.once('open', () => { ws.close(); resolve(); });
      ws.once('error', reject);
      ws.once('unexpected-response', (_req, resp) =>
        reject(new Error(`HTTP ${resp.statusCode}`)));
    });
  });

  // QWebSocketServer cannot reject a client during the HTTP upgrade, so the
  // proxy completes the handshake and immediately closes unauthorized
  // connections with 1008 (policy violation) before any CDP traffic flows.
  // "Rejected" therefore means: error, non-101 response, or close(1008)
  // without ever receiving a message.
  function expectRejected(wsUrl) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(wsUrl);
      ws.once('unexpected-response', (_req, resp) => {
        expect(resp.statusCode).toBe(401);
        resolve();
      });
      ws.once('error', () => resolve());
      ws.once('close', (code) => {
        try {
          expect(code).toBe(1008);
          resolve();
        } catch (e) {
          reject(e);
        }
      });
      ws.once('message', () => {
        ws.close();
        reject(new Error('Unauthorized client received CDP data'));
      });
    });
  }

  // WS-04
  it('Client with wrong token is rejected', async () => {
    const wsUrl = await getProtectedWsUrl();
    await expectRejected(`${wsUrl}?token=wrongtoken`);
  });

  // WS-05
  it('Client with no token is rejected when auth is required', async () => {
    const wsUrl = await getProtectedWsUrl();
    await expectRejected(wsUrl);
  });
});
