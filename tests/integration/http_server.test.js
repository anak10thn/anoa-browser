import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fetch from 'node-fetch';
import { startBrowser, stopBrowser, BASE_URL, HTTP_PORT, WS_PORT } from './helpers.js';

const AUTH_TOKEN = 'inttest-http-abc';

describe('HTTP Discovery Server (no auth)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser();
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // HTTP-01
  it('GET /json/list returns HTTP 200 with a JSON array', async () => {
    const resp = await fetch(`${BASE_URL}/json/list`);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(Array.isArray(body)).toBe(true);
  });

  // HTTP-02
  it('GET /json returns same content as /json/list', async () => {
    const [r1, r2] = await Promise.all([
      fetch(`${BASE_URL}/json`).then(r => r.json()),
      fetch(`${BASE_URL}/json/list`).then(r => r.json()),
    ]);
    // `title` is volatile during startup (empty ↔ "about:blank" race between
    // the two upstream fetches) — compare everything else.
    const stripTitle = (targets) => targets.map(({ title, ...rest }) => rest);
    expect(stripTitle(r1)).toEqual(stripTitle(r2));
  });

  // HTTP-03
  it('GET /json/version returns Browser (non-empty string) and valid webSocketDebuggerUrl', async () => {
    const resp = await fetch(`${BASE_URL}/json/version`);
    expect(resp.status).toBe(200);
    const body = await resp.json();
    expect(typeof body.Browser).toBe('string');
    expect(body.Browser.length).toBeGreaterThan(0);
    expect(body.webSocketDebuggerUrl).toMatch(/^ws:\/\//);
  });

  // HTTP-04
  it('webSocketDebuggerUrl uses proxy port (port+2)', async () => {
    const body = await fetch(`${BASE_URL}/json/version`).then(r => r.json());
    expect(body.webSocketDebuggerUrl).toMatch(new RegExp(`:${WS_PORT}`));
  });

  // HTTP-05
  it('GET /json/version/ (trailing slash) returns HTTP 200', async () => {
    const resp = await fetch(`${BASE_URL}/json/version/`);
    expect(resp.status).toBe(200);
  });

  // HTTP-06
  it('GET /unknown returns HTTP 404', async () => {
    const resp = await fetch(`${BASE_URL}/unknown`);
    expect(resp.status).toBe(404);
  });

  // HTTP-11
  it('No auth configured — request succeeds without Authorization header', async () => {
    const resp = await fetch(`${BASE_URL}/json`);
    expect(resp.status).toBe(200);
  });

  // HTTP-12
  it('GET /json/version returns Content-Type: application/json', async () => {
    const resp = await fetch(`${BASE_URL}/json/version`);
    expect(resp.headers.get('content-type')).toMatch(/application\/json/);
  });
});

describe('HTTP Discovery Server (with auth token)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser([`--auth-token=${AUTH_TOKEN}`]);
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // HTTP-07
  it('Request without token returns HTTP 401', async () => {
    const resp = await fetch(`${BASE_URL}/json`);
    expect(resp.status).toBe(401);
  });

  // HTTP-08
  it('Request with correct Authorization header returns HTTP 200', async () => {
    const resp = await fetch(`${BASE_URL}/json`, {
      headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
    });
    expect(resp.status).toBe(200);
  });

  // HTTP-09
  it('Request with correct ?token= query param returns HTTP 200', async () => {
    const resp = await fetch(`${BASE_URL}/json?token=${AUTH_TOKEN}`);
    expect(resp.status).toBe(200);
  });

  // HTTP-10
  it('Request with wrong token returns HTTP 401', async () => {
    const resp = await fetch(`${BASE_URL}/json`, {
      headers: { Authorization: `Bearer wrongtoken` },
    });
    expect(resp.status).toBe(401);
  });
});
