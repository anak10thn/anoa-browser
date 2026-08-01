/**
 * Suite 8 — the agent command layer, end to end.
 *
 * These drive the real binary against a real browser, because that is the only
 * place the interesting failures live: the refs have to survive between
 * separate *processes*, which no in-process test can check. Everything here
 * runs the CLI exactly as an agent would, through argv and exit codes.
 *
 * Prerequisites: a build at ../../build (see docs/BUILDING.md).
 */
import { describe, it, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { spawn, spawnSync } from 'node:child_process';
import { existsSync, rmSync } from 'node:fs';
import { resolve, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const root = resolve(here, '../..');

// macOS builds into an app bundle; Linux and Windows leave the binary in place.
const CANDIDATES = [
  resolve(root, 'build/anoa.app/Contents/MacOS/anoa'),
  resolve(root, 'build/anoa'),
];
const BIN = CANDIDATES.find(existsSync);
const PORT = Number(process.env.ANOA_E2E_PORT ?? 9455);

let browser;

/** Run the CLI. Never throws on a non-zero exit — the exit code is the result. */
function run(args) {
  const r = spawnSync(BIN, args, { encoding: 'utf8' });
  return { code: r.status, out: (r.stdout || '').trim(), err: (r.stderr || '').trim() };
}

/** Run one agent command against the browser this suite started. */
function anoa(...args) {
  return run([...args, '--port', String(PORT)]);
}

describe('Agent CLI (Suite 8)', () => {
  before(async () => {
    assert.ok(BIN, `no built binary — looked in:\n  ${CANDIDATES.join('\n  ')}`);
    browser = spawn(BIN, ['--headless', '--no-sandbox', '--port', String(PORT)],
                    { stdio: 'ignore' });
    // Poll rather than sleep: startup time is a property of the machine.
    const deadline = Date.now() + 30000;
    for (;;) {
      const r = anoa('status');
      if (r.code === 0) break;
      assert.ok(Date.now() < deadline, `browser never came up on ${PORT}: ${r.err}`);
      await new Promise(r2 => setTimeout(r2, 500));
    }
  });

  after(() => {
    if (browser) browser.kill('SIGKILL');
  });

  // AGENT-01: the contract that lets an agent decide whether to start a browser
  // or retry a command. A wrong code here sends it into the wrong recovery.
  it('reports exit 3 when nothing is listening', () => {
    const r = run(['status', '--port', '9', '--json']);
    assert.equal(r.code, 3);
    assert.match(r.err, /no browser/i);
  });

  // AGENT-02
  it('open navigates and reports the resolved page', () => {
    const r = anoa('open', 'example.com');
    assert.equal(r.code, 0, r.err);
    assert.match(r.out, /example\.com/);
  });

  // AGENT-03: refs are the whole interface. They must come back with a role and
  // an accessible name, or an agent cannot tell two buttons apart.
  it('snapshot returns refs with roles and names', () => {
    const r = anoa('snapshot', '-i');
    assert.equal(r.code, 0, r.err);
    assert.match(r.out, /@e\d+\s+link\s+/);
  });

  // AGENT-04: the point of the whole design — a ref minted by one process is
  // resolvable by the next, because it lives on the DOM node, not in memory.
  it('a ref from one process is usable by another', () => {
    const snap = anoa('snapshot', '-i', '--json');
    assert.equal(snap.code, 0, snap.err);
    const first = JSON.parse(snap.out).elements[0];
    assert.ok(first, 'no interactive elements to test with');

    const got = anoa('get', 'attr', first.ref, 'href');
    assert.equal(got.code, 0, got.err);
    assert.match(got.out, /^https?:\/\//);
  });

  // AGENT-05
  it('get text reads the page, eval runs in it', () => {
    const text = anoa('get', 'text');
    assert.equal(text.code, 0, text.err);
    assert.match(text.out, /Example Domain/);

    const ev = anoa('eval', 'document.querySelectorAll("p").length');
    assert.equal(ev.code, 0, ev.err);
    assert.equal(ev.out, '2');
  });

  // AGENT-06: a click that would land on an overlay must be refused, with the
  // covering element named. Clicking through it is the failure mode that makes
  // an agent believe it dismissed a consent banner when it did not.
  it('refuses a click that something covers, and names the cover', () => {
    const build = anoa('eval',
      `document.body.innerHTML = '<button id=go>Go</button>' +
       '<div id=veil style="position:fixed;inset:0;z-index:99">Accept cookies</div>'; 'ok'`);
    assert.equal(build.code, 0, build.err);

    const snap = JSON.parse(anoa('snapshot', '-i', '--json').out);
    const button = snap.elements.find(e => e.role === 'button');
    assert.ok(button, 'no button in the built page');

    const blocked = anoa('click', button.ref);
    assert.equal(blocked.code, 1);
    assert.match(blocked.err, /covered by/i);
    assert.match(blocked.err, /Accept cookies/);

    anoa('eval', "document.getElementById('veil').remove(); 'gone'");
    const ok = anoa('click', button.ref);
    assert.equal(ok.code, 0, ok.err);
  });

  // AGENT-07: fill must go through the native setter, or a React-style page
  // ignores the value it was given.
  it('fill sets a value the page can see', () => {
    anoa('eval', `document.body.innerHTML = '<input id=q>'; 'ok'`);
    const snap = JSON.parse(anoa('snapshot', '-i', '--json').out);
    const box = snap.elements.find(e => e.role === 'textbox');
    assert.ok(box, 'no textbox in the built page');

    assert.equal(anoa('fill', box.ref, 'hello world').code, 0);
    assert.equal(anoa('eval', "document.getElementById('q').value").out, 'hello world');
  });

  // AGENT-08: a stale ref is a normal outcome, and has to read as one.
  it('reports a stale ref as a missing element, not a crash', () => {
    const r = anoa('click', '@e9999');
    assert.equal(r.code, 1);
    assert.match(r.err, /no element/i);
  });

  // AGENT-09
  it('screenshot writes a real PNG', () => {
    const path = resolve(here, 'test-results', 'agent-e2e.png');
    const r = anoa('screenshot', path);
    assert.equal(r.code, 0, r.err);
    assert.ok(existsSync(path));
    rmSync(path, { force: true });
  });

  // AGENT-10: history, which goes through getNavigationHistory rather than any
  // back() verb CDP does not have.
  it('back and forward move exactly one entry', () => {
    anoa('open', 'example.com');
    anoa('open', 'example.net');
    assert.equal(anoa('back').code, 0);
    assert.match(anoa('eval', 'location.hostname').out, /example\.com/);
    assert.equal(anoa('forward').code, 0);
    assert.match(anoa('eval', 'location.hostname').out, /example\.net/);
  });

  // AGENT-11: the skill has to be readable without a browser at all — an agent
  // asks for it before it has started one.
  it('skills get core works with no browser involved', () => {
    const r = run(['skills', 'get', 'core']);
    assert.equal(r.code, 0);
    assert.match(r.out, /snapshot/);
    assert.match(r.out, /@e\d/);
  });

  // AGENT-12: grouped help, and the group names it advertises.
  it('help is grouped and every advertised group exists', () => {
    assert.equal(run(['help']).code, 0);
    for (const group of ['browser', 'navigate', 'inspect', 'interact', 'capture', 'agents']) {
      const one = run(['help', group]);
      assert.equal(one.code, 0, `help ${group} failed`);
      assert.ok(one.out.length > 0, `help ${group} printed nothing`);
    }
    assert.equal(run(['help', 'nope']).code, 2);
  });
});
