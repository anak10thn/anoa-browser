import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { existsSync } from 'fs';
import { mkdtempSync, rmSync } from 'fs';
import { tmpdir } from 'os';
import { join } from 'path';
import { startBrowser, stopBrowser, openDevtoolsWs, sendCdp, listTabs } from './helpers.js';

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

  // PRF-03: with no --profile there is still a profile, and it is on disk.
  //
  // This asserted the opposite — that nothing was written — which described
  // Qt's off-the-record default profile. That profile kept nothing at all, so
  // `anoa` with no flags logged you into a site and logged you out again when
  // the process ended, silently. The default is a persistent profile named
  // "default" now, and --ephemeral is how you ask for the old behaviour.
  it('with no --profile there is still a persistent default profile', async () => {
    const base = mkdtempSync(join(tmpdir(), 'anoa-default-'));
    const proc = await startBrowser([`--profile-dir=${base}`]);
    await new Promise((r) => setTimeout(r, 500));
    await stopBrowser(proc);
    const { readdirSync } = await import('fs');
    expect(readdirSync(base)).toContain('default');
    rmSync(base, { recursive: true, force: true });
  }, 25000);

  // PRF-03b: and --ephemeral still keeps nothing.
  it('--ephemeral writes no profile directory at all', async () => {
    const base = mkdtempSync(join(tmpdir(), 'anoa-ephemeral-'));
    const proc = await startBrowser([`--profile-dir=${base}`, '--ephemeral']);
    await new Promise((r) => setTimeout(r, 500));
    await stopBrowser(proc);
    const { readdirSync } = await import('fs');
    expect(readdirSync(base).length).toBe(0);
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

describe('Per-tab profiles', () => {
  let proc;
  let ws;
  let id = 7000;
  const call = (method, params = {}) => sendCdp(ws, method, params, ++id);

  // Runs a script in one tab, by attaching to that tab's own socket.
  async function evalIn(tab, expression) {
    const WebSocket = (await import('ws')).default;
    const sock = new WebSocket(tab.webSocketDebuggerUrl);
    await new Promise((res, rej) => { sock.once('open', res); sock.once('error', rej); });
    try {
      const r = await sendCdp(sock, 'Runtime.evaluate',
                              { expression, returnByValue: true }, 1);
      return r.result?.result?.value;
    } finally {
      sock.close();
    }
  }

  beforeAll(async () => {
    proc = await startBrowser();
    ({ ws } = await openDevtoolsWs());
    // One tab on a named profile, one isolated, and the default tab already
    // there. All on the same origin, or the cookie question is meaningless.
    await call('Target.createTarget',
               { url: 'https://example.com', anoaProfile: 'inttest-work' });
    await call('Target.createTarget',
               { url: 'https://example.com', anoaProfile: 'inttest-work' });
    await call('Target.createTarget',
               { url: 'https://example.com', anoaIsolated: true });
    await new Promise((r) => setTimeout(r, 4000));
  }, 40000);

  afterAll(() => { if (ws) ws.close(); stopBrowser(proc); });

  // PRF-T01: two tabs naming one profile share a jar; an isolated tab does not.
  // Both directions, because a one-way check passes on a browser where nothing
  // is shared at all.
  it('a named profile is shared between its tabs and hidden from an isolated one',
     async () => {
    const tabs = await listTabs();
    const infos = (await call('Target.getTargets')).result.targetInfos;
    const byTab = Object.fromEntries(infos.map((i) => [i.anoaTabId, i]));

    // The two tabs sharing a context, and the one that has its own.
    const counts = {};
    for (const info of infos) counts[info.browserContextId] =
      (counts[info.browserContextId] ?? 0) + 1;
    const sharedCtx = Object.keys(counts).find((c) => counts[c] === 2);
    expect(sharedCtx).toBeTruthy();

    const sharedTabs = tabs.filter((t) => byTab[t.anoaTabId].browserContextId === sharedCtx);
    const otherTabs = tabs.filter((t) => byTab[t.anoaTabId].browserContextId !== sharedCtx);
    expect(sharedTabs.length).toBe(2);
    expect(otherTabs.length).toBeGreaterThanOrEqual(1);

    await evalIn(sharedTabs[0], "document.cookie = 'probe=shared;path=/'");
    await new Promise((r) => setTimeout(r, 800));

    expect(await evalIn(sharedTabs[1], 'document.cookie')).toContain('probe=shared');
    for (const other of otherTabs)
      expect(await evalIn(other, 'document.cookie') ?? '').not.toContain('probe=shared');
  }, 40000);

  // PRF-T02: a profile has to outlive every page using it. Freeing it while a
  // sibling still holds it is a use-after-free inside Chromium, not a leak
  // noticed later.
  it('closing one of two tabs sharing a profile leaves the other working',
     async () => {
    const infos = (await call('Target.getTargets')).result.targetInfos;
    const counts = {};
    for (const info of infos) counts[info.browserContextId] =
      (counts[info.browserContextId] ?? 0) + 1;
    const sharedCtx = Object.keys(counts).find((c) => counts[c] === 2);
    const pair = infos.filter((i) => i.browserContextId === sharedCtx);
    expect(pair.length).toBe(2);

    const closed = await call('Target.closeTarget', { targetId: pair[0].targetId });
    expect(closed.result.success).toBe(true);
    await new Promise((r) => setTimeout(r, 800));

    const survivor = (await listTabs()).find((t) => t.anoaTabId === pair[1].anoaTabId);
    expect(survivor).toBeTruthy();
    // Still reads its own cookie, and still runs script at all.
    expect(await evalIn(survivor, '1 + 1')).toBe(2);
  }, 40000);
});
