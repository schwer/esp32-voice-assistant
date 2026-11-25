// SPDX-License-Identifier: MIT
// Captive Wi-Fi setup page served on the board's setup hotspot (components/captive).
const { test, expect } = require('@playwright/test');
const { CAPTIVE_HTML, serve } = require('./helpers/device_pages');

const NETS = { networks: [
  { ssid: 'HomeNet', rssi: -50, secure: true },
  { ssid: 'Cafe <b>free</b>', rssi: -80, secure: false },
] };

test('pick a network, enter the password, see where Jarvis is now', async ({ page }) => {
  let polls = 0;
  const { calls, errors } = await serve(page, CAPTIVE_HTML(), {
    'GET /scan.json': () => NETS,
    'POST /connect': () => ({ ok: true }),
    'GET /status.json': () => (++polls < 2 ? { state: 'connecting', ssid: 'HomeNet' }
                                           : { state: 'connected', ssid: 'HomeNet', url: 'http://192.168.1.77/' }),
  });
  await page.goto('http://dev/');
  await expect(page.locator('#net option')).toHaveCount(3);                 // two networks + "Other"
  await expect(page.locator('#net option').nth(1)).toHaveText(/Cafe <b>free<\/b> \(open\)/);   // text, not HTML
  await page.fill('#pass', 'correct horse');
  await page.click('#go');
  await expect(page.locator('#status')).toContainText('Connected to HomeNet', { timeout: 10000 });
  await expect(page.locator('#status')).toContainText('http://192.168.1.77/');
  const body = new URLSearchParams(calls.find((c) => c.key === 'POST /connect').body);
  expect(body.get('ssid')).toBe('HomeNet');
  expect(body.get('pass')).toBe('correct horse');
  expect(errors).toEqual([]);
});

test('other network by name; a failed join says why and can be retried', async ({ page }) => {
  const { calls } = await serve(page, CAPTIVE_HTML(), {
    'GET /scan.json': () => NETS,
    'POST /connect': () => ({ ok: true }),
    'GET /status.json': () => ({ state: 'failed', ssid: 'Hidden', message: "Couldn't join that network. Check the password." }),
  });
  await page.goto('http://dev/');
  await expect(page.locator('#otherbox')).toBeHidden();
  await page.selectOption('#net', '__other__');
  await expect(page.locator('#otherbox')).toBeVisible();
  await page.click('#go');
  await expect(page.locator('#status')).toHaveText('Choose a network.');
  await page.fill('#other', 'Hidden');
  await page.click('#go');
  await expect(page.locator('#status')).toHaveText("Couldn't join that network. Check the password.", { timeout: 10000 });
  await expect(page.locator('#go')).toBeEnabled();
  expect(new URLSearchParams(calls.find((c) => c.key === 'POST /connect').body).get('ssid')).toBe('Hidden');
});

test('a scan that fails still allows typing a network name', async ({ page }) => {
  await serve(page, CAPTIVE_HTML(), { 'GET /scan.json': () => { throw new Error('down'); } });
  await page.route('http://dev/scan.json', (r) => r.fulfill({ status: 500, body: 'x' }));
  await page.goto('http://dev/');
  await expect(page.locator('#net option')).toHaveCount(1);
  await expect(page.locator('#otherbox')).toBeVisible();
});
