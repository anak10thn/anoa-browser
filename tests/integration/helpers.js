import { spawn } from 'child_process';
import { createConnection } from 'net';
import WebSocket from 'ws';
import fetch from 'node-fetch';
import { resolve, dirname } from 'path';
import { fileURLToPath } from 'url';

const __dirname = dirname(fileURLToPath(import.meta.url));

export const BINARY = process.env.ANOA_BINARY
  ?? resolve(__dirname, '../../build/anoa-browser');

export const HTTP_PORT = parseInt(process.env.ANOA_PORT ?? '9222', 10);
export const WS_PORT   = HTTP_PORT + 2;
export const BASE_URL  = `http://localhost:${HTTP_PORT}`;
export const WS_BASE   = `ws://localhost:${WS_PORT}`;

/** Wait until TCP port is accepting connections or timeout (ms). */
export function waitForPort(port, timeout = 10000) {
  return new Promise((resolve, reject) => {
    const deadline = Date.now() + timeout;
    function attempt() {
      const sock = createConnection(port, '127.0.0.1');
      sock.once('connect', () => { sock.destroy(); resolve(); });
      sock.once('error', () => {
        sock.destroy();
        if (Date.now() >= deadline) return reject(new Error(`Port ${port} not ready after ${timeout}ms`));
        setTimeout(attempt, 100);
      });
    }
    attempt();
  });
}

/**
 * Start the anoa-browser binary and wait for HTTP port to be ready.
 * Returns the child process.
 */
export async function startBrowser(extraArgs = []) {
  const args = ['--headless', '--no-sandbox', `--port=${HTTP_PORT}`, ...extraArgs];
  const proc = spawn(BINARY, args, {
    env: { ...process.env, QPA_PLATFORM: 'offscreen' },
    stdio: ['ignore', 'pipe', 'pipe'],
  });
  proc.stdout.on('data', (d) => process.env.ANOA_VERBOSE && process.stdout.write(d));
  proc.stderr.on('data', (d) => process.env.ANOA_VERBOSE && process.stderr.write(d));
  await waitForPort(HTTP_PORT, 15000);
  return proc;
}

/** Send SIGTERM to the browser process and wait for it to exit. */
export function stopBrowser(proc) {
  return new Promise((resolve) => {
    if (!proc || proc.exitCode !== null) return resolve();
    proc.once('exit', resolve);
    proc.kill('SIGTERM');
  });
}

/**
 * Open a WebSocket to the proxy, optionally with an auth token in the URL.
 * Resolves once the connection is open.
 */
export function openWs(path = '', token = null) {
  const url = token
    ? `${WS_BASE}${path}?token=${token}`
    : `${WS_BASE}${path}`;
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    ws.once('open', () => resolve(ws));
    ws.once('error', reject);
  });
}

/**
 * Open a WS to the actual devtools page URL from /json/list.
 * Returns { ws, targetId }.
 */
export async function openDevtoolsWs(token = null) {
  const resp = await fetch(`${BASE_URL}/json/list`);
  const list = await resp.json();
  if (!list.length) throw new Error('/json/list returned empty array');
  const target = list[0];
  // The URL returned in webSocketDebuggerUrl already points to our proxy port.
  const wsUrl = token
    ? `${target.webSocketDebuggerUrl}?token=${token}`
    : target.webSocketDebuggerUrl;
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(wsUrl);
    ws.once('open', () => resolve({ ws, targetId: target.id }));
    ws.once('error', reject);
  });
}

/**
 * Send a CDP command on an open WebSocket and wait for the matching response.
 */
export function sendCdp(ws, method, params = {}, id = 1) {
  return new Promise((resolve, reject) => {
    const handler = (data) => {
      try {
        const msg = JSON.parse(data.toString());
        if (msg.id === id) {
          ws.off('message', handler);
          resolve(msg);
        }
      } catch { /* ignore non-JSON */ }
    };
    ws.on('message', handler);
    ws.send(JSON.stringify({ id, method, params }));
    // Timeout safety
    setTimeout(() => {
      ws.off('message', handler);
      reject(new Error(`CDP response for id=${id} (${method}) timed out`));
    }, 10000);
  });
}

/** Get the webSocketDebuggerUrl from /json/version. */
export async function getWsDebuggerUrl(token = null) {
  const headers = token ? { Authorization: `Bearer ${token}` } : {};
  const resp = await fetch(`${BASE_URL}/json/version`, { headers });
  const json = await resp.json();
  return json.webSocketDebuggerUrl;
}
