import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import { startBrowser, stopBrowser, openDevtoolsWs, sendCdp } from './helpers.js';

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
