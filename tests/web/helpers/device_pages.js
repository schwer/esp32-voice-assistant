// SPDX-License-Identifier: MIT
// Serve the firmware's diagnostics page / setup progress card with a scripted board API.
const { cString } = require('./c_html');

const DIAG_HTML = () => cString('components/setup_web/diag_page.h', 'DIAG_PAGE');
const INTERCOM_HTML = () => cString('components/setup_web/intercom_page.h', 'INTERCOM_PAGE');
const PROGRESS_HTML = () =>
  '<!doctype html><html><head><meta charset=utf-8></head><body><p class=ok>Saved.</p>' +
  cString('components/setup_web/setup_web.c', 'VOICE_PROGRESS_HTML') + '</body></html>';

/**
 * api: { 'GET /diag.json': () => json, 'POST /diag/play': (url) => json, ... }
 * Records every request in `calls`.
 */
async function serve(page, html, api) {
  const calls = [];
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  page.on('dialog', (d) => d.accept());
  await page.route('http://dev/**', (r) => {
    const req = r.request();
    const u = new URL(req.url());
    const key = `${req.method()} ${u.pathname}`;
    calls.push({ key, search: u.search, headers: req.headers(), body: req.postData() });
    if (req.method() === 'GET' && ['/', '/diag', '/intercom'].includes(u.pathname)) {
      return r.fulfill({ body: html, contentType: 'text/html' });
    }
    const fn = api[key];
    if (fn) return r.fulfill({ json: fn(u) });
    if (u.pathname === '/diag/logs') return r.fulfill({ body: '' });
    if (req.method() === 'POST') return r.fulfill({ json: { ok: true } });
    return r.fulfill({ json: {} });
  });
  return { calls, errors };
}

module.exports = { DIAG_HTML, INTERCOM_HTML, PROGRESS_HTML, serve };
