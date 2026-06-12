import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import fetch from 'node-fetch';
import { startBrowser, stopBrowser, BASE_URL } from './helpers.js';

const AUTH_TOKEN = 'inttest-render-abc';

describe('Render endpoints (no auth)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser();
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // RND-01
  it('GET /render/screenshot.png returns 200 with image/png', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/image\/png/);
  });

  // RND-02
  it('GET /render/screenshot.png body starts with PNG magic bytes', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    const buf = Buffer.from(await resp.arrayBuffer());
    // PNG magic: 89 50 4E 47
    expect(buf[0]).toBe(0x89);
    expect(buf[1]).toBe(0x50); // 'P'
    expect(buf[2]).toBe(0x4e); // 'N'
    expect(buf[3]).toBe(0x47); // 'G'
  });

  // RND-03
  it('GET /render/screenshot.png has Cache-Control: no-cache', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    expect(resp.headers.get('cache-control')).toBe('no-cache');
  });

  // RND-05
  it('GET /render/html returns 200 with text/html', async () => {
    const resp = await fetch(`${BASE_URL}/render/html`);
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/text\/html/);
  });

  // RND-06
  it('GET /render/html body contains HTML structure', async () => {
    const body = await fetch(`${BASE_URL}/render/html`).then(r => r.text());
    expect(body).toMatch(/<html/i);
    expect(body).toMatch(/<\/html>/i);
  });

  // RND-07
  it('GET /render/html has Cache-Control: no-cache', async () => {
    const resp = await fetch(`${BASE_URL}/render/html`);
    expect(resp.headers.get('cache-control')).toBe('no-cache');
  });

  // RND-09
  it('POST /render/navigate with valid HTTP URL (query param) returns 200 "navigating"', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate?url=http://example.com`, {
      method: 'POST',
    });
    expect(resp.status).toBe(200);
    const body = await resp.text();
    expect(body).toBe('navigating');
  });

  // RND-10
  it('POST /render/navigate with valid HTTPS URL (query param) returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate?url=https://example.com`, {
      method: 'POST',
    });
    expect(resp.status).toBe(200);
  });

  // RND-11
  it('POST /render/navigate with URL in body returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: 'https://example.com',
    });
    expect(resp.status).toBe(200);
    const body = await resp.text();
    expect(body).toBe('navigating');
  });

  // RND-12
  it('POST /render/navigate with no URL returns 400 "invalid url"', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate`, {
      method: 'POST',
      headers: { 'Content-Type': 'text/plain' },
      body: '',
    });
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('invalid url');
  });

  // RND-13
  it('POST /render/navigate with relative URL returns 400', async () => {
    const resp = await fetch(`${BASE_URL}/render/navigate?url=relative/path`, {
      method: 'POST',
    });
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('invalid url');
  });

  // RND-14
  it('POST /render/navigate with javascript: scheme returns 400 "scheme not allowed"', async () => {
    const resp = await fetch(
      `${BASE_URL}/render/navigate?url=${encodeURIComponent('javascript:alert(1)')}`,
      { method: 'POST' },
    );
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('scheme not allowed');
  });

  // RND-15
  it('POST /render/navigate with ftp: scheme returns 400 "scheme not allowed"', async () => {
    const resp = await fetch(
      `${BASE_URL}/render/navigate?url=${encodeURIComponent('ftp://example.com')}`,
      { method: 'POST' },
    );
    expect(resp.status).toBe(400);
    const body = await resp.text();
    expect(body).toBe('scheme not allowed');
  });

  // RND-16
  it('GET /render returns 200 with text/html', async () => {
    const resp = await fetch(`${BASE_URL}/render`);
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/text\/html/);
  });

  // RND-17
  it('GET /render page body references /render/screenshot.png', async () => {
    const body = await fetch(`${BASE_URL}/render`).then(r => r.text());
    expect(body).toContain('/render/screenshot.png');
  });

  // RND-19
  it('GET /render has Cache-Control: no-cache', async () => {
    const resp = await fetch(`${BASE_URL}/render`);
    expect(resp.headers.get('cache-control')).toBe('no-cache');
  });

  // RND-20
  it('GET /render/ (trailing slash) returns 301 to /render', async () => {
    const resp = await fetch(`${BASE_URL}/render/`, { redirect: 'manual' });
    expect(resp.status).toBe(301);
    expect(resp.headers.get('location')).toBe('/render');
  });

  // RND-21
  it('GET /render/?token=abc preserves query string in redirect Location', async () => {
    const resp = await fetch(`${BASE_URL}/render/?token=abc`, { redirect: 'manual' });
    expect(resp.status).toBe(301);
    expect(resp.headers.get('location')).toBe('/render?token=abc');
  });

  // RND-22
  it('GET /render/stream.mjpeg returns 200 with multipart/x-mixed-replace content type', async () => {
    const controller = new AbortController();
    const resp = await fetch(`${BASE_URL}/render/stream.mjpeg`, {
      signal: controller.signal,
    });
    expect(resp.status).toBe(200);
    expect(resp.headers.get('content-type')).toMatch(/multipart\/x-mixed-replace.*boundary=frame/);
    controller.abort();
  });

  // RND-24
  it('GET /render/stream.mjpeg has no Content-Length header', async () => {
    const controller = new AbortController();
    const resp = await fetch(`${BASE_URL}/render/stream.mjpeg`, {
      signal: controller.signal,
    });
    expect(resp.headers.get('content-length')).toBeNull();
    controller.abort();
  });

  // RND-23
  it('GET /render/stream.mjpeg stream begins with MJPEG boundary marker', async () => {
    await new Promise((resolve, reject) => {
      const controller = new AbortController();
      const timeout = setTimeout(() => {
        controller.abort();
        reject(new Error('No MJPEG boundary received within 3s'));
      }, 3000);

      fetch(`${BASE_URL}/render/stream.mjpeg`, { signal: controller.signal })
        .then(resp => {
          const chunks = [];
          resp.body.on('data', chunk => {
            chunks.push(chunk);
            const text = Buffer.concat(chunks).toString('binary');
            if (text.includes('--frame')) {
              clearTimeout(timeout);
              controller.abort();
              resolve();
            }
          });
          resp.body.on('error', (err) => {
            if (err.name !== 'AbortError') reject(err);
          });
        })
        .catch(err => {
          if (err.name !== 'AbortError') reject(err);
        });
    });
  });
});

describe('Render endpoints (with auth token)', () => {
  let proc;

  beforeAll(async () => {
    proc = await startBrowser([`--auth-token=${AUTH_TOKEN}`]);
  }, 20000);

  afterAll(() => stopBrowser(proc));

  // RND-25
  it('GET /render/screenshot.png without token returns 401', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`);
    expect(resp.status).toBe(401);
  });

  // RND-26
  it('GET /render/screenshot.png with Bearer token returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`, {
      headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
    });
    expect(resp.status).toBe(200);
  });

  // RND-27
  it('GET /render/screenshot.png with ?token= returns 200', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png?token=${AUTH_TOKEN}`);
    expect(resp.status).toBe(200);
  });

  // RND-28
  it('GET /render/screenshot.png with wrong token returns 401', async () => {
    const resp = await fetch(`${BASE_URL}/render/screenshot.png`, {
      headers: { Authorization: 'Bearer wrongtoken' },
    });
    expect(resp.status).toBe(401);
  });

  // RND-18
  it('GET /render with auth configured embeds token in screenshot URL', async () => {
    const resp = await fetch(`${BASE_URL}/render`, {
      headers: { Authorization: `Bearer ${AUTH_TOKEN}` },
    });
    const body = await resp.text();
    expect(body).toContain(`token=${AUTH_TOKEN}`);
  });
});
