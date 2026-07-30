/**
 * Suite 6 — Playwright Compatibility Tests
 *
 * Prerequisites:
 *   1. anoa-browser is running:  ./build/anoa-browser --headless --no-sandbox --port 9222
 *   2. npm install in this directory
 *
 * Key constraint: browser.newPage() is NOT supported by anoa-browser (QtWebEngine
 * limitation). Always use browser.contexts()[0].pages()[0] to get the existing page.
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

// PW-11: browser.newPage() is expected to fail — document the expected behavior
test('browser.newPage() throws (Target.createTarget not supported)', async () => {
  await expect(browser.newPage()).rejects.toThrow();
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
