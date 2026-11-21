// SPDX-License-Identifier: MIT
// web/index.html: the one-click installer, against a simulated board.
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');
const { ROOT } = require('./helpers/c_html');
const { fakeSerial } = require('./helpers/fake_board');

const MANIFEST = { name: 'ESP32 Voice Assistant', version: '2026.11.15-abc1234', build: 'abc1234',
                   builds: [{ chipFamily: 'ESP32-S3', parts: [] }] };

async function open(page, board) {
  await page.addInitScript(fakeSerial, board);
  await page.route('http://dev/**', (r) => {
    const u = new URL(r.request().url());
    if (u.pathname === '/manifest.json') return r.fulfill({ json: MANIFEST });
    const f = path.join(ROOT, 'web', u.pathname === '/' ? 'index.html' : u.pathname);
    return fs.existsSync(f) ? r.fulfill({ path: f }) : r.fulfill({ status: 404 });
  });
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto('http://dev/');
  return errors;
}

test('up-to-date board on Wi-Fi: shows status, no Wi-Fi form', async ({ page }) => {
  const errors = await open(page, { ssid: 'HomeNet' });
  await expect(page.locator('#connect')).toHaveText('Install');
  await page.click('#connect');
  await expect(page.locator('#step')).toHaveText('The board already has the latest firmware.');
  await expect(page.locator('#sFw')).toHaveText('abc1234 (latest)');
  await expect(page.locator('#sWifi')).toHaveText('HomeNet · strong signal');
  await expect(page.locator('#sUrl a')).toHaveAttribute('href', 'http://192.168.1.77/');
  await expect(page.locator('#wifiCard')).toBeHidden();
  expect(errors).toEqual([]);
});

test('board reporting the full release version is recognised as the latest', async ({ page }) => {
  // Firmware now reports "2026.11.15-abc1234", not just the commit (regression: it was
  // compared with the commit only, so a successful install was reported as an error).
  const errors = await open(page, { ssid: 'HomeNet', deviceVersion: '2026.11.15-abc1234' });
  await page.click('#connect');
  await expect(page.locator('#step')).toHaveText('The board already has the latest firmware.');
  await expect(page.locator('#sFw')).toHaveText('2026.11.15-abc1234 (latest)');
  expect(errors).toEqual([]);
});

test('an older release version is not mistaken for the latest', async ({ page }) => {
  await open(page, { ssid: 'HomeNet', deviceVersion: '2026.11.14-abc1234' });
  await page.click('#connect');
  await expect(page.locator('#step')).toContainText('2026.11.14-abc1234');   // "Updating from ..."
  await expect(page.locator('#step')).not.toHaveText('The board already has the latest firmware.');
});

test('board without Wi-Fi: network list, wrong then right password', async ({ page }) => {
  const errors = await open(page, {});
  await page.click('#connect');
  await expect(page.locator('#wifiCard')).toBeVisible();
  await expect(page.locator('#sWifi')).toHaveText('not set up yet');
  await expect(page.locator('#ssid')).toHaveValue('HomeNet');          // strongest preselected
  await expect(page.locator('#ssid option')).toHaveCount(4);           // 3 networks + Other
  await page.fill('#pass', 'wrong');
  await page.click('#saveWifi');
  await expect(page.locator('#wifiMsg')).toContainText("couldn't connect to that network");
  await page.fill('#pass', 'correct horse');
  await page.click('#saveWifi');
  await expect(page.locator('#wifiCard')).toBeHidden();
  await expect(page.locator('#sWifi')).toHaveText('HomeNet · strong signal');
  expect(await page.evaluate(() => localStorage.getItem('va.lastSsid'))).toBe('HomeNet');
  expect(errors).toEqual([]);
});

test('after a restart the page waits for the board to rejoin its network', async ({ page }) => {
  const errors = await open(page, { saved: 'HomeNet', rejoinAfter: 2 });
  await page.click('#connect');
  await expect(page.locator('#sWifi')).toHaveText('HomeNet · strong signal', { timeout: 15000 });
  await expect(page.locator('#wifiCard')).toBeHidden();
  expect(errors).toEqual([]);
});

test('log output containing "IMPROV" does not swallow replies (regression)', async ({ page }) => {
  const errors = await open(page, { ssid: 'HomeNet', noise: 'W (5) net: IMPIMPROV text in a log line\n' });
  await page.click('#connect');
  await expect(page.locator('#sWifi')).toHaveText('HomeNet · strong signal');
  expect(errors).toEqual([]);
});

test('browsers without Web Serial get a clear message', async ({ page }) => {
  await page.addInitScript(() => { delete Navigator.prototype.serial; });
  await page.route('http://dev/**', (r) => {
    const u = new URL(r.request().url());
    if (u.pathname === '/manifest.json') return r.fulfill({ json: MANIFEST });
    return r.fulfill({ path: path.join(ROOT, 'web/index.html') });
  });
  await page.goto('http://dev/');
  await expect(page.locator('#unsupported')).toBeVisible();
  await expect(page.locator('#connect')).toBeDisabled();
});
