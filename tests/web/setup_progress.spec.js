// SPDX-License-Identifier: MIT
// The setup page's voice-recording progress card (VOICE_PROGRESS_HTML in setup_web.c).
const { test, expect } = require('@playwright/test');
const { PROGRESS_HTML, serve } = require('./helpers/device_pages');

test('after saving a new voice: progress bar, then finished with a link', async ({ page }) => {
  let n = 0;
  const { errors } = await serve(page, PROGRESS_HTML(), {
    'GET /voice/progress': () => (++n < 4
      ? { busy: true, pending: true, done: n * 5, total: 21, voice: 'Kore', active_voice: 'Puck', error: '', status: '' }
      : { busy: false, pending: false, done: 21, total: 21, voice: 'Kore', active_voice: 'Kore', error: '', status: '' }),
  });
  await page.goto('http://dev/?saved=1&rec=1');
  await expect(page.locator('#vp')).toBeVisible();
  await expect(page.locator('#vptitle')).toHaveText('Recording voice messages in Kore’s voice…');
  await expect(page.locator('#vptext')).toContainText('of 21 messages recorded');
  await expect(page.locator('#vptitle')).toHaveText('✔ Finished: everything now uses Kore’s voice', { timeout: 10000 });
  await expect(page.locator('#vptext a')).toHaveAttribute('href', '/diag#voice-messages');
  expect(errors).toEqual([]);
});

test('a failed recording says why', async ({ page }) => {
  await serve(page, PROGRESS_HTML(), {
    'GET /voice/progress': () => ({ busy: false, pending: false, done: 3, total: 21, voice: 'Kore', active_voice: 'Puck',
                                   error: "couldn't reach Gemini (API key not valid)",
                                   status: "couldn't record in Kore's voice: couldn't reach Gemini (API key not valid). Save the setup page to try again." }),
  });
  await page.goto('http://dev/?saved=1&rec=1');
  await expect(page.locator('#vptitle')).toHaveText('Voice recording stopped');
  await expect(page.locator('#vptext')).toContainText('API key not valid');
});

test('no card when nothing is being recorded', async ({ page }) => {
  await serve(page, PROGRESS_HTML(), {
    'GET /voice/progress': () => ({ busy: false, pending: false, done: 0, total: 0, voice: 'Puck', active_voice: 'Puck', error: '' }),
  });
  await page.goto('http://dev/?saved=1');
  await page.waitForTimeout(500);
  await expect(page.locator('#vp')).toBeHidden();
});

test('a paused recording (conversation in progress) keeps showing progress', async ({ page }) => {
  await serve(page, PROGRESS_HTML(), {
    'GET /voice/progress': () => ({ busy: false, pending: true, done: 8, total: 21, voice: 'Kore', active_voice: 'Puck',
                                   error: 'you were talking to me',
                                   status: 'recording in Kore\'s voice paused: you were talking to me; it continues when the board is free' }),
  });
  await page.goto('http://dev/?saved=1');
  await expect(page.locator('#vp')).toBeVisible();
  await expect(page.locator('#vptext')).toContainText('paused');
});
