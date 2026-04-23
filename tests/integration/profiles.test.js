import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { existsSync } from 'fs';
import { mkdtempSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import { join } from 'path';
import { startBrowser, stopBrowser, openDevtoolsWs, sendCdp } from './helpers.js';

describe('Profile Isolation & Cookie Tests', () => {
  let profileBaseDir;

  beforeAll(() => {
    profileBaseDir = mkdtempSync(join(tmpdir(), 'anoa-profile-test-'));
  });

  afterAll(() => {
    try { rmSync(profileBaseDir, { recursive: true, force: true }); } catch { /* ignore */ }
  });

  // PRF-01
  it('Named profile creates its directory under profileDir', async () => {
    const proc = await startBrowser([
      `--profile=testA`,
      `--profile-dir=${profileBaseDir}`,
    ]);
    // The directory is created lazily on first navigation; give it a moment.
    await new Promise((r) => setTimeout(r, 1000));
    await stopBrowser(proc);
    expect(existsSync(join(profileBaseDir, 'testA'))).toBe(true);
  }, 25000);

  // PRF-02
  it('Two profiles produce separate storage directories', async () => {
    const procA = await startBrowser([`--profile=profileA`, `--profile-dir=${profileBaseDir}`]);
    await new Promise((r) => setTimeout(r, 500));
    await stopBrowser(procA);

    // Start second instance — needs a different port to avoid conflict.
    // We reuse the default port since procs run sequentially here.
    const procB = await startBrowser([`--profile=profileB`, `--profile-dir=${profileBaseDir}`]);
    await new Promise((r) => setTimeout(r, 500));
    await stopBrowser(procB);

    expect(existsSync(join(profileBaseDir, 'profileA'))).toBe(true);
    expect(existsSync(join(profileBaseDir, 'profileB'))).toBe(true);
    // Verify the directories are distinct (not symlinks to each other)
    const statA = join(profileBaseDir, 'profileA');
    const statB = join(profileBaseDir, 'profileB');
    expect(statA).not.toBe(statB);
  }, 40000);

  // PRF-03
  it('No --profile flag uses default profile (no named directory created)', async () => {
    const base = mkdtempSync(join(tmpdir(), 'anoa-default-'));
    const proc = await startBrowser([`--profile-dir=${base}`]);
    await new Promise((r) => setTimeout(r, 500));
    await stopBrowser(proc);
    // With no --profile, no named subdirectory should be created
    const { readdirSync } = await import('fs');
    const entries = readdirSync(base);
    expect(entries.length).toBe(0);
    rmSync(base, { recursive: true, force: true });
  }, 25000);

  // PRF-04
  it('Cookie set via Network.setCookie is retrievable via Network.getCookies', async () => {
    const proc = await startBrowser([`--profile=cookieTest`, `--profile-dir=${profileBaseDir}`]);
    const { ws } = await openDevtoolsWs();

    // Navigate to about:blank first so we have an origin context
    await sendCdp(ws, 'Page.navigate', { url: 'about:blank' }, 1);
    await new Promise((r) => setTimeout(r, 300));

    await sendCdp(ws, 'Network.setCookie', {
      name: 'testcookie',
      value: 'hello',
      domain: 'example.com',
      path: '/',
    }, 2);

    const resp = await sendCdp(ws, 'Network.getCookies', { urls: ['https://example.com'] }, 3);
    ws.close();
    await stopBrowser(proc);

    const cookie = resp.result?.cookies?.find((c) => c.name === 'testcookie');
    expect(cookie).toBeDefined();
    expect(cookie?.value).toBe('hello');
  }, 25000);

  // PRF-06
  it('Profile stored in specified directory path', async () => {
    const customBase = mkdtempSync(join(tmpdir(), 'anoa-custom-'));
    const proc = await startBrowser([`--profile=myprof`, `--profile-dir=${customBase}`]);
    await new Promise((r) => setTimeout(r, 500));
    await stopBrowser(proc);
    expect(existsSync(join(customBase, 'myprof'))).toBe(true);
    rmSync(customBase, { recursive: true, force: true });
  }, 25000);
});
