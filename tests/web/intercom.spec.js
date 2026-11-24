// SPDX-License-Identifier: MIT
// The firmware's Intercom page (components/setup_web/intercom_page.h).
const { test, expect } = require('@playwright/test');
const { INTERCOM_HTML, DIAG_HTML, serve } = require('./helpers/device_pages');

const SAVED = { messages: [
  { id: 2, text: 'Dinner is ready! <b>now</b>', time: 1795000000, seconds: 2.1 },
  { id: 1, text: 'Leaving in five minutes.', time: 1794990000, seconds: 1.8 },
] };

test('type a message: sent with the protection header, then progress until said', async ({ page }) => {
  let polls = 0;
  const { calls, errors } = await serve(page, INTERCOM_HTML(), {
    'GET /intercom/messages.json': () => SAVED,
    'POST /intercom/say': () => ({ ok: true }),
    'GET /intercom/status': () => (++polls < 3 ? { state: 'preparing' } : { state: 'done', message: 'Said it (2.1 s).' }),
  });
  await page.goto('http://dev/intercom');
  await page.fill('#text', 'Time for bed & "lights out"');
  await expect(page.locator('#count')).toHaveText('27 / 300');
  await page.click('#say');
  await expect(page.locator('#status')).toHaveText('Said it (2.1 s).', { timeout: 8000 });
  const say = calls.find((c) => c.key === 'POST /intercom/say');
  expect(say.headers['x-assistant']).toBe('1');
  expect(new URLSearchParams(say.body).get('text')).toBe('Time for bed & "lights out"');
  await expect(page.locator('#text')).toHaveValue('');
  expect(errors).toEqual([]);
});

test('an empty message is not sent; a failure says why', async ({ page }) => {
  const { calls } = await serve(page, INTERCOM_HTML(), {
    'GET /intercom/messages.json': () => ({ messages: [] }),
    'POST /intercom/say': () => ({ ok: true }),
    'GET /intercom/status': () => ({ state: 'failed', message: 'Couldn\'t reach Gemini: credits depleted' }),
  });
  await page.goto('http://dev/intercom');
  await expect(page.locator('#list')).toHaveText('None yet.');
  await page.click('#say');
  await expect(page.locator('#status')).toHaveText('Type a message first.');
  expect(calls.some((c) => c.key === 'POST /intercom/say')).toBe(false);
  await page.fill('#text', 'Hello');
  await page.click('#say');
  await expect(page.locator('#status')).toHaveText("Couldn't reach Gemini: credits depleted");
  await expect(page.locator('#say')).toBeEnabled();
});

test('saved messages: shown as text, say again, delete after confirming', async ({ page }) => {
  const { calls, errors } = await serve(page, INTERCOM_HTML(), {
    'GET /intercom/messages.json': () => SAVED,
    'POST /intercom/replay': () => ({ ok: true }),
    'GET /intercom/status': () => ({ state: 'done', message: 'Said it again.' }),
  });
  await page.goto('http://dev/intercom');
  await expect(page.locator('.msg')).toHaveCount(2);
  await expect(page.locator('.msg').first()).toContainText('Dinner is ready! <b>now</b>');   // not HTML
  expect(await page.locator('.msg b').count()).toBe(0);
  await page.locator('.msg').first().getByRole('button', { name: 'Say again' }).click();
  await expect(page.locator('#status')).toHaveText('Said it again.');
  expect(calls.find((c) => c.key === 'POST /intercom/replay').body).toBe('id=2');
  await page.locator('.msg').nth(1).getByRole('button', { name: 'Delete' }).click();
  await expect.poll(() => calls.find((c) => c.key === 'POST /intercom/delete')?.body).toBe('id=1');
  expect(errors).toEqual([]);
});

test('diagnostics links to the intercom', async ({ page }) => {
  await serve(page, DIAG_HTML(), { 'GET /diag.json': () => ({}) });
  await page.goto('http://dev/diag');
  await expect(page.getByRole('link', { name: 'Intercom' })).toHaveAttribute('href', '/intercom');
});
