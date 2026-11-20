// SPDX-License-Identifier: MIT
// The firmware's diagnostics page (components/setup_web/diag_page.h).
const { test, expect } = require('@playwright/test');
const { DIAG_HTML, serve } = require('./helpers/device_pages');

const IDLE = { current: 'off', states: [{ name: 'connecting', description: 'Blue pulse' }], events: [] };
const MESSAGES = { voice: 'Puck', status: "messages and conversations use Puck's voice",
                   messages: [{ id: 'ready', text: 'Ready. Say Jarvis when you need me.', source: "Puck's voice" },
                              { id: 'wake_ack_1', text: 'Yes?', source: 'built-in clip' }] };

function snapshot(battery) {
  return { battery, wifi: { connected: true, ssid: 'Home<script>alert(1)</script>', rssi_dbm: -60, signal: 'good' },
           modules: { gemini: 'idle' } };
}

test('battery headline: level, status and time left', async ({ page }) => {
  const { errors } = await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => snapshot({ level: '78%', status: 'discharging', time_left: 'about 5.2 hours' }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
    'GET /voice/progress': () => ({ busy: false, pending: false }),
  });
  await page.goto('http://dev/diag');
  await expect(page.locator('#batline')).toHaveText('Battery 78% · discharging · about 5.2 hours left');
  await expect(page.locator('#batline')).not.toHaveClass('bad');
  expect(errors).toEqual([]);
});

test('low battery is highlighted; unknown battery says so', async ({ page }) => {
  let level = '9%';
  await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => snapshot(level === '?' ? { level: 'unknown' } : { level, status: 'discharging' }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
  });
  await page.goto('http://dev/diag');
  await expect(page.locator('#batline')).toHaveClass('bad');
  level = '?';
  await page.reload();
  await expect(page.locator('#batline')).toHaveText('Battery level unknown');
});

test('values from the board are shown as text, never as HTML', async ({ page }) => {
  const { errors } = await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => snapshot({ level: '50%' }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
  });
  await page.goto('http://dev/diag');
  await expect(page.getByText('Home<script>alert(1)</script>')).toBeVisible();
  expect(await page.locator('script', { hasText: 'alert(1)' }).count()).toBe(0);
  expect(errors).toEqual([]);
});

test('actions send the cross-site protection header', async ({ page }) => {
  const { calls } = await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => snapshot({ level: '50%' }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
    'POST /diag/speaker-test': () => ({ ok: true, message: 'Playing a test message.' }),
  });
  await page.goto('http://dev/diag');
  await page.click('button[data-act="speaker-test"]');
  await expect(page.locator('#result')).toHaveText('Playing a test message.');
  const post = calls.find((c) => c.key === 'POST /diag/speaker-test');
  expect(post.headers['x-assistant']).toBe('1');
});

test('microphone test follows the board until it heard you', async ({ page }) => {
  let polls = 0;
  await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => snapshot({ level: '50%' }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
    'POST /diag/mic-test': () => ({ state: 'running', seconds: 0 }),
    'GET /diag/mic-test': () => (++polls < 3 ? { state: 'running', seconds: 0 } : { state: 'heard', seconds: 1.3 }),
  });
  await page.goto('http://dev/diag');
  await page.click('button[data-act="mic-test"]');
  await expect(page.locator('#result')).toContainText('say hi');
  await expect(page.locator('#result')).toHaveText(/heard 1\.3 s of speech/, { timeout: 8000 });
});

test('messages list: play one, and re-record with a progress bar', async ({ page }) => {
  let n = 0;
  const { calls, errors } = await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => snapshot({ level: '50%' }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
    'POST /diag/voice-record': () => ({ ok: true, message: "Recording every message again in Puck's voice." }),
    'GET /voice/progress': () => {
      if (!calls.some((c) => c.key === 'POST /diag/voice-record')) return { busy: false, pending: false };
      n++;
      return n < 4 ? { busy: true, pending: true, done: n * 5, total: 21, voice: 'Puck', active_voice: 'Puck', error: '' }
                   : { busy: false, pending: false, done: 21, total: 21, voice: 'Puck', active_voice: 'Puck', error: '' };
    },
  });
  await page.goto('http://dev/diag');
  await expect(page.locator('#messages .light')).toHaveCount(2);
  await page.locator('#messages .light').nth(1).getByRole('button', { name: 'Play' }).click();
  await expect.poll(() => calls.some((c) => c.key === 'POST /diag/play' && c.search === '?id=wake_ack_1')).toBe(true);

  await page.click('#voicerecord');
  await expect(page.locator('#dvp')).toBeVisible();
  await expect(page.locator('#dvptext')).toContainText(/Recording in Puck’s voice: \d+ of 21/);
  await expect(page.locator('#dvptext')).toHaveText(/Finished/, { timeout: 10000 });
  expect(errors).toEqual([]);
});

test('alarms & timers card, and Stop ringing', async ({ page }) => {
  let clock = { ringing: 'timer: pasta', 'timer 1': 'tea, 4:59 left', 'alarm 1': '07:30 weekdays (next Tuesday 07:30)' };
  const { calls } = await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => ({ ...snapshot({ level: '50%' }), clock }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
    'POST /diag/stop-ringing': () => { clock = {}; return { ok: true, message: 'Stopped.' }; },
  });
  await page.goto('http://dev/diag');
  const card = page.locator('.card', { has: page.getByRole('heading', { name: 'Alarms & timers' }) });
  await expect(card).toContainText('tea, 4:59 left');
  await expect(card).toContainText('07:30 weekdays');
  await page.click('button[data-act="stop-ringing"]');
  await expect(page.locator('#result')).toHaveText('Stopped.');
  expect(calls.find((c) => c.key === 'POST /diag/stop-ringing').headers['x-assistant']).toBe('1');
  await page.reload();
  await expect(card).toContainText('No timers, alarms or stopwatch.');
});

test('software update: check, then install only after confirming', async ({ page }) => {
  const { calls } = await serve(page, DIAG_HTML(), {
    'GET /diag.json': () => ({ ...snapshot({ level: '80%' }),
      updates: { this_version: '2026.11.17-1111111', latest_version: '2026.11.20-2222222', status: 'update available' } }),
    'GET /diag/leds.json': () => IDLE, 'GET /diag/messages.json': () => MESSAGES,
    'POST /diag/update-check': () => ({ ok: true, message: 'Version 2026.11.20-2222222 is available.' }),
    'POST /diag/update-install': () => ({ ok: true, message: 'Installing.' }),
  });
  await page.goto('http://dev/diag');
  const card = page.locator('.card', { has: page.getByRole('heading', { name: 'Software update' }) });
  await expect(card).toContainText('2026.11.20-2222222');
  await page.click('button[data-act="update-check"]');
  await expect(page.locator('#result')).toHaveText('Version 2026.11.20-2222222 is available.');
  page.removeAllListeners('dialog');                          // answer the confirmation ourselves
  page.once('dialog', (d) => d.dismiss());                     // "Cancel": nothing installed
  await page.click('button[data-act="update-install"]');
  await page.waitForTimeout(300);
  expect(calls.some((c) => c.key === 'POST /diag/update-install')).toBe(false);
  page.once('dialog', (d) => d.accept());
  await page.click('button[data-act="update-install"]');
  await expect(page.locator('#result')).toHaveText('Installing.');
  expect(calls.find((c) => c.key === 'POST /diag/update-install').headers['x-assistant']).toBe('1');
});
