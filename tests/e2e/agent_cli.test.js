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
    for (const group of ['browser', 'navigate', 'inspect', 'interact',
                         'state', 'debug', 'capture', 'agents']) {
      const one = run(['help', group]);
      assert.equal(one.code, 0, `help ${group} failed`);
      assert.ok(one.out.length > 0, `help ${group} printed nothing`);
    }
    assert.equal(run(['help', 'nope']).code, 2);
  });

  // AGENT-12b: --help and -h must be the grouped help, not the flag dump.
  // QCommandLineParser's own --help cannot mention a subcommand, because the
  // parser never sees one — so someone typing --help was shown --profile-dir
  // and left with no idea `click` existed.
  it('--help and -h print the same grouped help as `help`', () => {
    const grouped = run(['help']);
    for (const flag of ['--help', '-h']) {
      const r = run([flag]);
      assert.equal(r.code, 0, `${flag} failed`);
      assert.equal(r.out, grouped.out, `${flag} differs from \`help\``);
    }
    // And it carries the browser flags, so nothing was lost by replacing the
    // parser's output with this.
    for (const flag of ['--headless', '--auth-token', '--gfx', '--term-port']) {
      assert.match(grouped.out, new RegExp(flag.replace(/-/g, '\\-')),
                   `${flag} missing from help`);
    }
  });

  // AGENT-13: find returns refs, so its output feeds every other command.
  it('find locates by role, text and selector, and returns usable refs', () => {
    anoa('open', 'example.com');
    const byRole = anoa('find', 'role', 'link', '--json');
    assert.equal(byRole.code, 0, byRole.err);
    const first = JSON.parse(byRole.out).matches[0];
    assert.ok(first, 'no link found on example.com');
    assert.match(first.ref, /^@e\d+$/);

    // The ref find minted must work in the next process, like snapshot's do.
    assert.match(anoa('get', 'attr', first.ref, 'href').out, /^https?:\/\//);

    assert.equal(anoa('find', 'text', 'Learn more').code, 0);
    assert.equal(anoa('find', 'selector', 'a').code, 0);
    // No match is a failure, not an empty success — an agent branches on this.
    assert.equal(anoa('find', 'text', 'definitely-not-on-this-page').code, 1);
  });

  // AGENT-14: cookies and storage survive between processes, which is the
  // whole reason they are worth having as commands.
  it('cookies and storage round-trip across processes', () => {
    anoa('open', 'example.com');

    assert.equal(anoa('cookies', 'set', 'sid', 'abc123').code, 0);
    assert.match(anoa('cookies').out, /sid=abc123/);
    assert.equal(anoa('cookies', 'clear').code, 0);

    assert.equal(anoa('storage', 'local', 'set', 'token', 'xyz789').code, 0);
    assert.equal(anoa('storage', 'local', 'token').out, 'xyz789');
    assert.equal(anoa('storage', 'local', 'clear').code, 0);
    assert.match(anoa('storage', 'local').out, /empty/);
  });

  // AGENT-15: emulation actually reaches the page, checked by asking the page.
  it('set viewport and device change what the page sees', () => {
    assert.equal(anoa('set', 'viewport', '800', '600').code, 0);
    assert.equal(anoa('eval', 'window.innerWidth').out, '800');

    assert.equal(anoa('set', 'device', 'iphone-14').code, 0);
    assert.equal(anoa('eval', 'window.innerWidth').out, '390');

    // No name lists the presets rather than erroring.
    assert.match(anoa('set', 'device').out, /iphone-14/);
    assert.equal(anoa('set', 'device', 'no-such-phone').code, 1);
  });

  // AGENT-16: the recorders. Written by one process, read by another — a
  // one-shot command cannot subscribe to CDP events in time, so this is the
  // only shape that can report what already happened.
  it('console, errors and network report what happened before the command ran', () => {
    anoa('open', 'example.com');
    anoa('console', '--clear');

    anoa('eval', "console.log('recorded-marker'); console.warn('warn-marker'); 'ok'");
    const log = anoa('console');
    assert.equal(log.code, 0, log.err);
    assert.match(log.out, /recorded-marker/);
    assert.match(log.out, /warn-marker/);
    assert.match(anoa('console', '--level', 'warn').out, /warn-marker/);

    anoa('eval', "setTimeout(function(){ null.boom; }, 0); 'armed'");
    anoa('wait', '--ms', '600');
    assert.match(anoa('errors').out, /TypeError/);

    anoa('eval', 'fetch(location.href).then(function(){return 0;}); "fired"');
    anoa('wait', '--ms', '1200');
    assert.match(anoa('network').out, /GET\s+200/);

    anoa('console', '--clear');
    assert.match(anoa('console').out, /nothing recorded/);
  });

  // AGENT-17: the richer waits, including the one that must not mistake a
  // throwing expression for a failure.
  it('wait handles text, url, fn and hidden', () => {
    anoa('open', 'example.com');
    assert.equal(anoa('wait', '--text', 'Example Domain', '--timeout', '5000').code, 0);
    assert.equal(anoa('wait', '--url', 'example.com', '--timeout', '5000').code, 0);
    // Throws until it does not — `window.__late` is undefined at first.
    anoa('eval', 'setTimeout(function(){ window.__late = { ready: true }; }, 300); "armed"');
    assert.equal(anoa('wait', '--fn', 'window.__late.ready', '--timeout', '5000').code, 0);
    assert.equal(anoa('wait', '#nothing-here', '--state', 'hidden', '--timeout', '3000').code, 0);
    // And a real timeout is a failure with a reason.
    const late = anoa('wait', '--text', 'not-on-this-page', '--timeout', '1000');
    assert.equal(late.code, 1);
    assert.match(late.err, /timed out/);
  });

  // AGENT-19: `wait --load` straight after a click that navigates.
  //
  // Found by pointing an agent at the skill and watching it work. The obvious
  // probe returns instantly here — the *old* document is still `complete` while
  // the new one is in flight — so the wait passed, the next command read the
  // page the agent was trying to leave, and it reported the wrong answer with
  // no sign anything had gone wrong.
  it('wait --load waits for a navigation the click started', () => {
    anoa('open', 'example.com');
    anoa('snapshot', '-i');
    const before = anoa('eval', 'location.href').out;

    assert.equal(anoa('click', '@e1').code, 0);
    assert.equal(anoa('wait', '--load').code, 0);

    const after = anoa('eval', 'location.href').out;
    assert.notEqual(after, before,
                    'wait --load returned while the navigation was still in flight');
    assert.match(after, /iana\.org/);
  });

  // AGENT-19b: and it must not hang on a page that is genuinely idle — the
  // settle window is what bounds that, so a regression there shows up as a
  // wait that never returns rather than one that returns too early.
  it('wait --load returns promptly when nothing is loading', () => {
    anoa('open', 'example.com');
    const started = Date.now();
    assert.equal(anoa('wait', '--load').code, 0);
    const took = Date.now() - started;
    assert.ok(took < 6000, `wait --load on an idle page took ${took}ms`);
  });

  // AGENT-18: the reference an agent is told to read must exist and describe
  // the commands that exist.
  it('skills get commands documents the real command set', () => {
    const r = run(['skills', 'get', 'commands']);
    assert.equal(r.code, 0);
    for (const verb of ['snapshot', 'click', 'find', 'cookies', 'storage',
                        'console', 'network', 'wait', 'screenshot']) {
      assert.match(r.out, new RegExp(`anoa ${verb}`), `${verb} missing from the reference`);
    }
    assert.match(run(['skills', 'list']).out, /commands/);
  });

  // AGENT-19: a mistyped subcommand has to be reported as one. Nothing reads
  // positionalArguments(), so the word used to be discarded and a browser
  // started in its place — a stray window on a desktop, and SIGABRT under
  // "Could not load the Qt platform plugin xcb" over SSH.
  it('an unknown subcommand is an error, not a browser', () => {
    const r = run(['blahblah']);
    assert.equal(r.code, 2);
    assert.match(r.err, /unknown command 'blahblah'/);
    assert.match(r.err, /anoa help/);
    // The real verbs, and the words that are not verbs but are still
    // legitimate first arguments, must not be caught by it.
    assert.equal(anoa('status').code, 0);
    assert.equal(run(['--version']).code, 0);
    assert.equal(run(['help']).code, 0);
  });
});
