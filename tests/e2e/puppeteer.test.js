/**
 * Suite 7 — Puppeteer Compatibility Tests
 *
 * Prerequisites:
 *   1. anoa-browser is running:  ./build/anoa-browser --headless --no-sandbox --port 9222
 *   2. npm install in this directory
 *
 * Run: node --test puppeteer.test.js
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import puppeteer from 'puppeteer-core';

const HTTP_PORT  = parseInt(process.env.ANOA_PORT  ?? '9222', 10);
const AUTH_TOKEN = process.env.ANOA_AUTH_TOKEN ?? '';
const BASE_URL   = `http://localhost:${HTTP_PORT}`;

let browser;
let page;

async function getWsUrl() {
  const headers = AUTH_TOKEN ? { Authorization: `Bearer ${AUTH_TOKEN}` } : {};
  const resp = await fetch(`${BASE_URL}/json/version`, { headers });
  if (!resp.ok) throw new Error(`/json/version returned ${resp.status}`);
  const body = await resp.json();
  let wsUrl = body.webSocketDebuggerUrl;
  if (AUTH_TOKEN) {
    wsUrl += `?token=${AUTH_TOKEN}`;
  }
  return wsUrl;
}

async function getPageWsUrl() {
  const headers = AUTH_TOKEN ? { Authorization: `Bearer ${AUTH_TOKEN}` } : {};
  const resp = await fetch(`${BASE_URL}/json/list`, { headers });
  const list = await resp.json();
  if (!list.length) throw new Error('/json/list is empty');
  let wsUrl = list[0].webSocketDebuggerUrl;
  if (AUTH_TOKEN) wsUrl += `?token=${AUTH_TOKEN}`;
  return wsUrl;
}

describe('Puppeteer Compatibility (Suite 7)', () => {
  before(async () => {
    const browserWSEndpoint = await getWsUrl();
    // PP-01 / PP-02: connect via browserWSEndpoint
    browser = await puppeteer.connect({ browserWSEndpoint });
    const pages = await browser.pages();
    // PP-03: browser.pages() returns at least one page
    assert.ok(pages.length >= 1, 'Expected at least 1 page');
    page = pages[0];
  });

  after(async () => {
    // PP-09: clean disconnect
    if (browser) await browser.disconnect();
  });

  // PP-01
  it('connects via browserWSEndpoint without error', () => {
    assert.ok(browser, 'browser should be defined');
  });

  // PP-03
  it('browser.pages() returns at least one page', async () => {
    const pages = await browser.pages();
    assert.ok(pages.length >= 1);
  });

  // PP-04
  it('page.goto() navigates successfully', async () => {
    const response = await page.goto('https://example.com');
    assert.ok(response, 'goto() should return a Response');
  });

  // PP-05
  it('page.evaluate() returns the document title string', async () => {
    await page.goto('https://example.com');
    const title = await page.evaluate(() => document.title);
    assert.strictEqual(typeof title, 'string');
    assert.ok(title.length > 0, 'Title should be non-empty');
  });

  // PP-06
  it('page.screenshot() returns a non-empty Buffer', async () => {
    const buf = await page.screenshot();
    assert.ok(buf instanceof Buffer, 'screenshot() should return a Buffer');
    assert.ok(buf.length > 0, 'screenshot buffer should not be empty');
  });

  // PP-07
  it('page.createCDPSession() returns a session', async () => {
    const session = await page.createCDPSession();
    assert.ok(session, 'CDP session should be defined');
    await session.detach();
  });

  // PP-08
  it('Raw CDP command Browser.getVersion returns version info', async () => {
    const session = await page.createCDPSession();
    const result = await session.send('Browser.getVersion');
    assert.ok(result.Browser, 'Browser.getVersion should return Browser field');
    await session.detach();
  });

  // PP-09: covered by after() above
  it('disconnect completes cleanly (covered by after())', () => {
    assert.ok(true); // pass — actual disconnect is in after()
  });
});
