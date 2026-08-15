import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startBrowser, stopBrowser, openDevtoolsWs, sendCdp, listTabs } from './helpers.js';

describe('CDP Extension Stubs', () => {
  let proc;
  let ws;
  let cmdId = 0;

  beforeAll(async () => {
    proc = await startBrowser();
    ({ ws } = await openDevtoolsWs());
  }, 20000);

  afterAll(async () => {
    ws?.close();
    await stopBrowser(proc);
  });

  function nextId() { return ++cmdId; }

  async function expectStub(method, params = {}) {
    const id = nextId();
    const resp = await sendCdp(ws, method, params, id);
    expect(resp.id).toBe(id);
    expect(resp).toHaveProperty('result');
    expect(resp.error).toBeUndefined();
    return resp;
  }

  // EXT-01
  it('Profiler.enable returns stub {}', () => expectStub('Profiler.enable'));
  // EXT-02
  it('Profiler.disable returns stub {}', () => expectStub('Profiler.disable'));
  // EXT-03
  it('Profiler.start returns stub {}', () => expectStub('Profiler.start'));
  // EXT-04
  it('Profiler.stop returns stub {}', () => expectStub('Profiler.stop'));
  // EXT-05
  it('Profiler.setSamplingInterval returns stub {}', () =>
    expectStub('Profiler.setSamplingInterval', { interval: 100 }));

  // EXT-06
  it('HeapProfiler.enable returns stub {}', () => expectStub('HeapProfiler.enable'));
  // EXT-07
  it('HeapProfiler.disable returns stub {}', () => expectStub('HeapProfiler.disable'));
  // EXT-08
  it('HeapProfiler.startTrackingHeapObjects returns stub {}', () =>
    expectStub('HeapProfiler.startTrackingHeapObjects'));
  // EXT-09
  it('HeapProfiler.stopTrackingHeapObjects returns stub {}', () =>
    expectStub('HeapProfiler.stopTrackingHeapObjects'));
  // EXT-10
  it('HeapProfiler.takeHeapSnapshot returns stub {}', () =>
    expectStub('HeapProfiler.takeHeapSnapshot'));

  // EXT-11
  it('Security.enable returns stub {}', () => expectStub('Security.enable'));
  // EXT-12
  it('Security.disable returns stub {}', () => expectStub('Security.disable'));
  // EXT-13
  it('Security.setIgnoreCertificateErrors returns stub {}', () =>
    expectStub('Security.setIgnoreCertificateErrors', { ignore: true }));

  // EXT-14
  it('Browser.setDownloadBehavior returns stub {}', () =>
    expectStub('Browser.setDownloadBehavior', { behavior: 'allow', downloadPath: '/tmp' }));
  // EXT-15
  it('Browser.getWindowForTarget returns stub {}', () =>
    expectStub('Browser.getWindowForTarget', { targetId: 'x' }));

  // EXT-16
  it('Target.createBrowserContext returns synthetic context ID', async () => {
    const id = nextId();
    const resp = await sendCdp(ws, 'Target.createBrowserContext', {}, id);
    expect(resp.id).toBe(id);
    expect(resp.result?.browserContextId).toBe('__anoa_default__');
  });

  // EXT-17
  it('Target.disposeBrowserContext returns stub {}', () =>
    expectStub('Target.disposeBrowserContext', { browserContextId: '__anoa_default__' }));

  // EXT-18
  it('Each response carries the correct matching id', async () => {
    const ids = [100, 200, 300];
    const responses = await Promise.all(
      ids.map((id) => sendCdp(ws, 'Profiler.enable', {}, id)),
    );
    for (let i = 0; i < ids.length; i++) {
      expect(responses[i].id).toBe(ids[i]);
    }
  });

  // EXT-19
  it('Unknown domain (DOM.getDocument) is forwarded to Chromium and returns result or error', async () => {
    const id = nextId();
    const resp = await sendCdp(ws, 'DOM.getDocument', {}, id);
    expect(resp.id).toBe(id);
    // Proxy must not swallow the response: must carry either result or error from Chromium
    expect(resp.result !== undefined || resp.error !== undefined).toBe(true);
  });
});

describe('Target domain against the tab registry', () => {
  let proc;
  let ws;
  let id = 9000;
  const call = (method, params = {}) => sendCdp(ws, method, params, ++id);

  beforeAll(async () => {
    proc = await startBrowser();
    ({ ws } = await openDevtoolsWs());
  }, 20000);

  afterAll(() => { if (ws) ws.close(); stopBrowser(proc); });

  // TGT-01: every tab, with both ids — ours and the engine's — and a browser
  // context per profile object.
  it('getTargets lists each tab, with a context per profile', async () => {
    const shared = await call('Target.createTarget', { url: 'about:blank' });
    const isolated = await call('Target.createTarget',
                                { url: 'about:blank', anoaIsolated: true });
    expect(shared.result.targetId).toBeTruthy();
    expect(isolated.result.targetId).toBeTruthy();

    const infos = (await call('Target.getTargets')).result.targetInfos;
    expect(infos.length).toBeGreaterThanOrEqual(3);
    for (const info of infos) {
      expect(info.type).toBe('page');
      expect(info.targetId).toBeTruthy();
      expect(info.anoaTabId).toMatch(/^t[1-9][0-9]*$/);
      expect(info.browserContextId).toBeTruthy();
    }

    // The two tabs on the shared profile agree on a context; the isolated one
    // has its own.
    const isolatedInfo = infos.find((i) => i.targetId === isolated.result.targetId);
    const others = infos.filter((i) => i.targetId !== isolated.result.targetId);
    expect(new Set(others.map((i) => i.browserContextId)).size).toBe(1);
    expect(isolatedInfo.browserContextId)
      .not.toBe(others[0].browserContextId);
  }, 30000);

  // TGT-02: createTarget answers with an id the discovery document then reports,
  // which is what a CDP client dials next.
  it('a created target appears in /json/list with a fresh tab id', async () => {
    const before = (await listTabs()).map((t) => t.anoaTabId);
    const created = await call('Target.createTarget', { url: 'about:blank' });
    const after = await listTabs();

    const entry = after.find((t) => t.id === created.result.targetId);
    expect(entry).toBeTruthy();
    expect(before).not.toContain(entry.anoaTabId);
  }, 20000);

  // TGT-03
  it('activateTarget moves the active marker', async () => {
    const tabs = await listTabs();
    const target = tabs.find((t) => !t.anoaActive);
    expect(target).toBeTruthy();

    const r = await call('Target.activateTarget', { targetId: target.id });
    expect(r.error).toBeUndefined();

    const after = await listTabs();
    expect(after.find((t) => t.anoaActive).anoaTabId).toBe(target.anoaTabId);
    expect(after.filter((t) => t.anoaActive).length).toBe(1);
  }, 20000);

  // TGT-04: closing answers true until the registry refuses the last tab, and
  // then false — not an error, because the client asked a fair question.
  it('closeTarget succeeds until the last tab, which is refused', async () => {
    let tabs = await listTabs();
    expect(tabs.length).toBeGreaterThan(1);

    while (tabs.length > 1) {
      const victim = tabs[tabs.length - 1];
      const r = await call('Target.closeTarget', { targetId: victim.id });
      expect(r.result.success).toBe(true);
      tabs = await listTabs();
    }

    const last = await call('Target.closeTarget', { targetId: tabs[0].id });
    expect(last.error).toBeUndefined();
    expect(last.result.success).toBe(false);
    expect((await listTabs()).length).toBe(1);
  }, 30000);

  // TGT-05: an id we never issued is refused rather than quietly opening the
  // tab somewhere else.
  it('an unknown browser context is refused', async () => {
    const r = await call('Target.createTarget',
                         { url: 'about:blank', browserContextId: 'never-issued' });
    expect(r.error).toBeTruthy();
    expect(r.error.message).toMatch(/browser context/i);
  }, 20000);
});
