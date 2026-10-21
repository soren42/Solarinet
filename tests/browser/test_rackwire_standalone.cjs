#!/usr/bin/env node
'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.resolve(__dirname, '../..');
const app = path.join(root, 'dashboard/public/rackwire/index.html');

(async () => {
  const executablePath = process.env.RW_CHROME || undefined;
  const browser = await chromium.launch({
    headless: true,
    ...(executablePath ? { executablePath } : {}),
    args: ['--no-sandbox', '--disable-dev-shm-usage'],
  });
  const context = await browser.newContext({ viewport: { width: 1280, height: 900 } });
  const page = await context.newPage();
  const failures = [];
  const networkRequests = [];
  page.on('pageerror', (error) => failures.push(`page: ${error.message}`));
  page.on('console', (message) => {
    if (message.type() === 'error') failures.push(`console: ${message.text()}`);
  });
  page.on('request', (request) => {
    if (/^https?:/i.test(request.url())) networkRequests.push(request.url());
  });

  try {
    await page.goto(`file://${app}`, { waitUntil: 'load', timeout: 30000 });
    await page.getByRole('button', { name: 'Map' }).waitFor();
    assert.equal(await page.title(), 'RackWire');
    assert.equal(networkRequests.length, 0, 'file-origin planner must not require network access');

    const opener = page.getByRole('button', { name: 'Add gear' });
    await opener.click();
    const dialog = page.getByRole('dialog', { name: 'Gear library' });
    await dialog.waitFor();
    const search = dialog.getByRole('textbox', { name: 'Search gear library' });
    assert.equal(await search.count(), 1, 'gear search has an accessible name');
    assert.equal(await page.evaluate(() => document.querySelector('[role="dialog"]').contains(document.activeElement)), true,
      'opening a modal moves focus inside it');

    const focusable = dialog.locator('button:not([disabled]),input:not([disabled]),select:not([disabled]),textarea:not([disabled]),a[href]');
    await focusable.first().focus();
    await page.keyboard.press('Shift+Tab');
    assert.equal(await focusable.last().evaluate((el) => el === document.activeElement), true,
      'Shift+Tab wraps to the last modal control');
    await page.keyboard.press('Tab');
    assert.equal(await focusable.first().evaluate((el) => el === document.activeElement), true,
      'Tab wraps to the first modal control');

    await search.focus();
    await page.keyboard.press('Escape');
    await dialog.waitFor({ state: 'detached' });
    assert.equal(await opener.evaluate((el) => el === document.activeElement), true,
      'closing a modal restores focus to its opener');

    await opener.click();
    const library = page.getByRole('dialog', { name: 'Gear library' });
    await library.getByRole('textbox', { name: 'Search gear library' }).fill('Patch panel');
    await library.getByRole('button', { name: /Patch panel · 24 port/ }).click();
    await library.waitFor({ state: 'detached' });
    const saved = await page.evaluate(() => JSON.parse(localStorage.getItem('rackwire.project.v2') || '{}'));
    assert(saved.devices.some((device) => device.name === 'Patch panel · 24 port'),
      'placing library gear persists it in the local plan');
    await page.reload({ waitUntil: 'load' });
    await page.getByText('Patch panel · 24 port', { exact: true }).first().waitFor();

    const searchTrigger = page.getByRole('button', { name: 'Find a device or port' });
    await searchTrigger.click();
    const searchDialog = page.getByRole('dialog', { name: 'Find a device or port' });
    await searchDialog.getByRole('textbox', { name: 'Find a device or port' }).waitFor();
    await page.keyboard.press('Escape');
    await searchDialog.waitFor({ state: 'detached' });
    assert.equal(await searchTrigger.evaluate((el) => el === document.activeElement), true,
      'device search restores focus on close');

    const scanTrigger = page.getByRole('button', { name: 'SCAN' });
    await scanTrigger.click();
    const scanDialog = page.getByRole('dialog', { name: 'Scan or type a code' });
    await scanDialog.getByRole('textbox', { name: 'UPC, EAN, or model code' }).waitFor();
    await page.keyboard.press('Escape');
    await scanDialog.waitFor({ state: 'detached' });
    assert.equal(await scanTrigger.evaluate((el) => el === document.activeElement), true,
      'code scan restores focus on close');

    const mapTrigger = page.getByRole('button', { name: 'Map' });
    await mapTrigger.focus();
    await page.keyboard.press('?');
    const helpDialog = page.getByRole('dialog', { name: 'Keyboard shortcuts' });
    await helpDialog.waitFor();
    await page.keyboard.press('Escape');
    await helpDialog.waitFor({ state: 'detached' });
    assert.equal(await mapTrigger.evaluate((el) => el === document.activeElement), true,
      'keyboard help restores focus on close');

    await page.getByRole('button', { name: 'Data' }).click();
    await page.getByRole('button', { name: 'Connections' }).waitFor();
    await page.getByRole('button', { name: 'Map' }).click();

    await page.setViewportSize({ width: 390, height: 844 });
    const responsive = await page.evaluate(() => ({
      width: innerWidth,
      documentWidth: document.documentElement.scrollWidth,
      toolbarWidth: document.querySelector('.rw-toolbar').getBoundingClientRect().width,
    }));
    assert.equal(responsive.documentWidth, responsive.width, 'mobile layout has no horizontal page overflow');
    for (const name of ['Map', 'Data', 'Add gear', 'Undo', 'Redo', 'Find a device or port', 'GO LIVE', 'SCAN', 'Menu']) {
      const control = page.getByRole('button', { name, exact: true });
      await control.waitFor();
      const box = await control.boundingBox();
      assert(box && box.x >= 0 && box.x + box.width <= responsive.width,
        `mobile toolbar keeps ${name} visible`);
    }

    assert.deepEqual(failures, [], 'browser reports no script or console errors');
    console.log('RackWire standalone browser acceptance passed: offline boot, accessible modal focus, local save/reload, map/data navigation, and mobile toolbar.');
  } finally {
    await browser.close();
  }
})().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
