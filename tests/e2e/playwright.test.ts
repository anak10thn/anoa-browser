/**
 * Suite 6 — Playwright Compatibility Tests
 *
 * Prerequisites:
 *   1. anoa is running:  ./build/anoa --headless --no-sandbox --port 9222
 *   2. npm install in this directory
 *
 * browser.newPage() works: Target.* is answered from anoa's own tab registry,
 * so a new page is a new tab. The existing page is still
 * browser.contexts()[0].pages()[0].
 */
import { chromium, type Browser, type Page, type BrowserContext } from '@playwright/test';
import { test, expect } from '@playwright/test';

const HTTP_PORT  = parseInt(process.env.ANOA_PORT  ?? '9222', 10);
const AUTH_TOKEN = process.env.ANOA_AUTH_TOKEN ?? '';
const CDP_URL    = `http://localhost:${HTTP_PORT}`;

let browser: Browser;
let context: BrowserContext;
let page: Page;

// PW-01 / PW-02 are tested in the beforeAll connection.
test.beforeAll(async () => {
  const connectOpts = AUTH_TOKEN
    ? { headers: { Authorization: `Bearer ${AUTH_TOKEN}` } }
    : {};
  browser = await chromium.connectOverCDP(CDP_URL, connectOpts);
  const contexts = browser.contexts();
  expect(contexts.length).toBeGreaterThan(0);
  context = contexts[0];
  const pages = context.pages();
  expect(pages.length).toBeGreaterThan(0);
  page = pages[0];
});

test.afterAll(async () => {
  // PW-13: disconnect gracefully
  await browser.close();
});

// PW-01
test('connectOverCDP succeeds and returns a browser object', () => {
  expect(browser).toBeTruthy();
});

// PW-03
test('Existing page is retrievable via contexts()[0].pages()[0]', () => {
  expect(page).toBeTruthy();
});

// PW-04
test('Navigate to a URL changes page.url()', async () => {
  await page.goto('https://example.com');
  expect(page.url()).toMatch(/example\.com/);
});

// PW-05
test('page.title() returns non-empty string after navigation', async () => {
  await page.goto('https://example.com');
  const title = await page.title();
  expect(title.length).toBeGreaterThan(0);
});

// PW-06
test('JavaScript evaluation: 1 + 1 = 2', async () => {
  const result = await page.evaluate(() => 1 + 1);
  expect(result).toBe(2);
});

// PW-07
test('DOM query: body element count is 1', async () => {
  await page.goto('https://example.com');
  const count = await page.locator('body').count();
  expect(count).toBe(1);
});

// PW-08
test('Screenshot returns non-empty Buffer', async () => {
  const buf = await page.screenshot();
  expect(buf.length).toBeGreaterThan(0);
});

// PW-09
test('Navigate back/forward works', async () => {
  const url1 = 'https://example.com';
  const url2 = 'about:blank';
  await page.goto(url1);
  await page.goto(url2);
  await page.goBack();
  expect(page.url()).toMatch(/example\.com/);
});

// PW-10: context.cookies() needs Storage.getCookies, which QtWebEngine's CDP
// backend does not implement ("Browser context management is not supported").
// Like PW-11, this documents the expected limitation.
test('context.cookies() rejects (Storage.getCookies not supported)', async () => {
  await expect(context.cookies()).rejects.toThrow();
});

// PW-11: browser.newPage() works — Target.createTarget is answered from the tab
// registry. This assertion used to be the opposite, and documented the
// limitation as permanent; it fails now because the limitation is gone.
test('browser.newPage() opens a real second page', async () => {
  const before = context.pages().length;
  const opened = await browser.newPage();
  expect(opened).toBeTruthy();
  expect(context.pages().length).toBe(before + 1);
  await opened.close();
});

// PW-12
test('5 concurrent page.evaluate() calls all return correct results', async () => {
  const results = await Promise.all(
    [1, 2, 3, 4, 5].map((n) => page.evaluate((x: number) => x * x, n)),
  );
  expect(results).toEqual([1, 4, 9, 16, 25]);
});

// PW-13: afterAll handles graceful disconnect (covered by afterAll above).
// Explicit test that browser is alive before close:
test('Browser is connected before close', () => {
  expect(browser.isConnected()).toBe(true);
});

// PW-14: /json/list is rebuilt from the tab registry rather than byte-patched,
// so this is the shape every CDP client actually dials.
test('/json/list reports one entry per tab, aimed at the proxy port', async () => {
  const res = await fetch(`${CDP_URL}/json/list`);
  const targets = await res.json();
  const pages = targets.filter((t: any) => t.type === 'page');
  expect(pages.length).toBeGreaterThan(0);

  for (const target of pages) {
    expect(target.anoaTabId).toMatch(/^t[1-9][0-9]*$/);
    expect(typeof target.anoaActive).toBe('boolean');
    // The proxy, not Chromium's own debugging port: a client handed the latter
    // would bypass every command anoa answers itself.
    expect(target.webSocketDebuggerUrl).toContain(`:${HTTP_PORT + 2}/devtools/page/`);
    expect(target.webSocketDebuggerUrl).not.toContain(`:${HTTP_PORT + 1}/`);
  }
  expect(pages.filter((t: any) => t.anoaActive).length).toBe(1);
});

// PW-15: the row the README used to list as unsupported. A new page is a new
// tab, and the discovery document has to agree with the client about it.
test('browser.newPage() adds a tab that /json/list reports', async () => {
  const before = await (await fetch(`${CDP_URL}/json/list`)).json();
  const beforeIds = before.filter((t: any) => t.type === 'page')
                          .map((t: any) => t.anoaTabId);

  const opened = await browser.newPage();
  try {
    const after = await (await fetch(`${CDP_URL}/json/list`)).json();
    const afterPages = after.filter((t: any) => t.type === 'page');
    expect(afterPages.length).toBe(beforeIds.length + 1);

    const fresh = afterPages.map((t: any) => t.anoaTabId)
                            .filter((id: string) => !beforeIds.includes(id));
    expect(fresh.length).toBe(1);
    expect(fresh[0]).toMatch(/^t[1-9][0-9]*$/);
  } finally {
    await opened.close();
  }
});
