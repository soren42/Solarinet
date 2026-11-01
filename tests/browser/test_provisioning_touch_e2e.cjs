#!/usr/bin/env node
'use strict';

// Touch/viewport acceptance for the provisioning screens (Images & Stacks,
// software pool, editors, confirm sheets, log drawer). It drives the same
// write-counted PHP fixture as test_provisioning_images_e2e.cjs in emulated
// iPad and iPhone contexts (hasTouch + isMobile) and taps every key flow.
//
// Engine: WebKit when it is installed (SOLARI_TOUCH_ENGINE=webkit forces it),
// otherwise Chromium with iPad/iPhone viewports and user agents. Emulation is
// not Safari: on-screen keyboard and real safe-area insets are not modelled.
//
// Per viewport it asserts: no horizontal page scroll; no interactive element
// clipped by the viewport or by an overflow-hidden ancestor (a contained
// horizontal scroller is fine); every interactive hit area ≥44×44 CSS px;
// text-entry font ≥16px (no iOS focus zoom); machine-token inputs opt out of
// autocapitalize/autocorrect/spellcheck; icon-only buttons are labelled.
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const playwright = require('playwright');
const { freePort, waitForServer, phpRouter, fixtureState } = require('./test_provisioning_images_e2e.cjs');

const ROOT = path.resolve(__dirname, '../..');
const PUBLIC = path.join(ROOT, 'dashboard/public');
const SHOTS = process.env.SOLARI_TOUCH_SHOTS || '/tmp/prov-ui-touch-shots';
const PROBE = process.env.SOLARI_TOUCH_PROBE === '1'; // report every violation, never stop early

const IPAD_UA = playwright.devices['iPad Pro 11'].userAgent;
const IPHONE_UA = playwright.devices['iPhone 13'].userAgent;
const VIEWPORTS = [
  { name: 'ipad-portrait-820', width: 820, height: 1180, ua: IPAD_UA, scale: 2 },
  { name: 'ipad-portrait-768', width: 768, height: 1024, ua: IPAD_UA, scale: 2 },
  { name: 'ipad-landscape-1180', width: 1180, height: 820, ua: IPAD_UA, scale: 2 },
  { name: 'ipad-landscape-1024', width: 1024, height: 768, ua: IPAD_UA, scale: 2 },
  { name: 'iphone-390', width: 390, height: 844, ua: IPHONE_UA, scale: 3 },
  { name: 'iphone-375', width: 375, height: 667, ua: IPHONE_UA, scale: 2 },
].filter((v) => !process.env.SOLARI_TOUCH_ONLY || v.name.includes(process.env.SOLARI_TOUCH_ONLY));

// Runs in the page. `scope` is a CSS selector for the surface being audited.
function audit(scope) {
  const out = { width: innerWidth, overflow: [], clipped: [], small: [], font: [], tokens: [], unlabeled: [] };
  const se = document.scrollingElement;
  const vv = window.visualViewport;
  if (vv && (Math.abs(vv.offsetLeft) > 0.5 || Math.abs(vv.scale - 1) > 0.01)) out.overflow.push('visual viewport panned/zoomed: offsetLeft=' + vv.offsetLeft + ' scale=' + vv.scale);
  if (scrollX !== 0) out.overflow.push('window scrollX=' + scrollX);
  for (const el of document.querySelectorAll('.content, .page, .panel, .prov-page, .app, #root, body, html')) {
    if (el.scrollLeft) out.overflow.push((el.className || el.tagName) + ' scrollLeft=' + el.scrollLeft);
  }
  if (se.scrollWidth > innerWidth) out.overflow.push('document ' + se.scrollWidth + ' > ' + innerWidth);
  const content = document.querySelector('.content');
  if (content && content.scrollWidth - content.clientWidth > 0) out.overflow.push('.content ' + content.scrollWidth + ' > ' + content.clientWidth);
  const roots = Array.from(document.querySelectorAll(scope));
  const visible = (el) => {
    const r = el.getBoundingClientRect();
    if (r.width === 0 || r.height === 0) return false;
    const cs = getComputedStyle(el);
    return cs.visibility !== 'hidden' && cs.display !== 'none';
  };
  const desc = (el) => {
    const name = el.getAttribute('aria-label') || el.id || (el.textContent || '').trim().slice(0, 30);
    return el.tagName.toLowerCase() + (el.className && typeof el.className === 'string' ? '.' + el.className.split(/\s+/).join('.') : '') + ' "' + name + '"';
  };
  // A hit area is the element's box unioned with its label (implicit or for=).
  const hitBox = (el) => {
    let r = el.getBoundingClientRect();
    const boxes = [r];
    const labels = el.labels ? Array.from(el.labels) : [];
    for (const l of labels) boxes.push(l.getBoundingClientRect());
    // Row-tap surfaces declare themselves; the whole row selects the control.
    const row = el.closest('[data-tap-row]');
    if (row) boxes.push(row.getBoundingClientRect());
    return boxes.reduce((a, b) => ({ w: Math.max(a.w, b.width), h: Math.max(a.h, b.height) }), { w: 0, h: 0 });
  };
  const sel = 'button, a[href], input:not([type=hidden]), select, textarea, summary, [role=button], [role=radio]:not(input), [tabindex="0"]';
  const seen = new Set();
  for (const root of roots) {
    for (const el of root.querySelectorAll(sel)) {
      if (seen.has(el) || !visible(el)) continue;
      seen.add(el);
      const r = el.getBoundingClientRect();
      // viewport clip (horizontal): a control must be reachable without page panning
      let clippedBy = null;
      // overflow-hidden ancestors silently cut controls off. A contained
      // horizontal scroller is fine (the control is reachable by scrolling
      // it); a fixed-position ancestor escapes every clip above it.
      let contained = false;
      for (let p = el.parentElement; p && !clippedBy; p = p.parentElement) {
        const cs = getComputedStyle(p);
        if (cs.position === 'fixed') break;
        if (cs.overflowX === 'visible') continue;
        const pr = p.getBoundingClientRect();
        if (cs.overflowX === 'auto' || cs.overflowX === 'scroll') { contained = p.scrollWidth > p.clientWidth || r.right <= pr.right + 1; break; }
        if (r.right > pr.right + 1 || r.left < pr.left - 1) clippedBy = desc(p);
      }
      if (!clippedBy && !contained && (r.left < -1 || r.right > innerWidth + 1)) clippedBy = 'viewport';
      if (clippedBy) out.clipped.push(desc(el) + ' by ' + clippedBy + ' [' + Math.round(r.left) + '..' + Math.round(r.right) + ']');
      const hb = hitBox(el);
      if (el.tagName !== 'TEXTAREA' && el.getAttribute('tabindex') !== '0' && (hb.w < 43.5 || hb.h < 43.5)) {
        out.small.push(desc(el) + ' ' + Math.round(hb.w) + 'x' + Math.round(hb.h));
      }
      const textEntry = el.tagName === 'TEXTAREA' || el.tagName === 'SELECT'
        || (el.tagName === 'INPUT' && !['checkbox', 'radio', 'button', 'submit', 'range', 'color', 'file'].includes(el.type));
      if (textEntry && parseFloat(getComputedStyle(el).fontSize) < 16) out.font.push(desc(el) + ' ' + getComputedStyle(el).fontSize);
      if ((el.tagName === 'INPUT' && textEntry && el.type !== 'number') || el.tagName === 'TEXTAREA') {
        const token = el.classList.contains('mono') || ['url', 'search'].includes(el.type);
        if (token && !(el.getAttribute('autocapitalize') === 'off' && el.getAttribute('autocorrect') === 'off' && el.getAttribute('spellcheck') === 'false')) {
          out.tokens.push(desc(el));
        }
      }
      if (el.tagName === 'BUTTON' && !(el.getAttribute('aria-label') || (el.textContent || '').trim() || el.getAttribute('aria-labelledby'))) {
        out.unlabeled.push(desc(el));
      }
    }
  }
  return out;
}

async function main() {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'solarinet-provtouch-browser-'));
  const sessions = path.join(temp, 'sessions');
  const stateFile = path.join(temp, 'state.json');
  fs.mkdirSync(sessions, { mode: 0o700 });
  fs.mkdirSync(SHOTS, { recursive: true });
  const routerPath = path.join(temp, 'router.php');
  fs.writeFileSync(routerPath, phpRouter(stateFile), { mode: 0o600 });
  const reset = () => fs.writeFileSync(stateFile, JSON.stringify(fixtureState()), { mode: 0o600 });
  const writes = () => JSON.parse(fs.readFileSync(stateFile, 'utf8')).writes;
  reset();

  let engine = 'chromium';
  if (process.env.SOLARI_TOUCH_ENGINE !== 'chromium') {
    try { if (fs.existsSync(playwright.webkit.executablePath())) engine = 'webkit'; } catch (_) { /* not installed */ }
  }

  let child = null;
  let browser = null;
  let failure = null;
  const report = {};
  const shots = [];
  try {
    const port = await freePort();
    const base = `http://127.0.0.1:${port}`;
    child = spawn('php', ['-d', `session.save_path=${sessions}`, '-S', `127.0.0.1:${port}`,
      '-t', PUBLIC, routerPath], { cwd: ROOT, stdio: ['ignore', 'ignore', 'pipe'] });
    let phpErrors = '';
    child.stderr.on('data', (data) => { phpErrors = (phpErrors + data.toString()).slice(-4000); });
    await waitForServer(`${base}/__health`, child);

    browser = await playwright[engine].launch({
      headless: true,
      ...(engine === 'chromium' ? { args: ['--no-sandbox', '--disable-dev-shm-usage'] } : {}),
      ...(engine === 'chromium' && process.env.SOLARI_CHROMIUM_EXECUTABLE ? { executablePath: process.env.SOLARI_CHROMIUM_EXECUTABLE } : {}),
    });

    for (const vp of VIEWPORTS) {
      reset();
      const ctx = await browser.newContext({
        viewport: { width: vp.width, height: vp.height }, userAgent: vp.ua, deviceScaleFactor: vp.scale,
        hasTouch: true, isMobile: engine === 'chromium' ? true : undefined,
      });
      const page = await ctx.newPage();
      const pageErrors = [];
      page.on('pageerror', (error) => pageErrors.push(error.message));
      page.on('console', (message) => { if (message.type() === 'error') pageErrors.push(message.text()); });
      const findings = {};
      report[vp.name] = findings;
      const must = (cond, msg) => { if (cond) return; if (!PROBE) assert.fail(msg); findings['assert:' + msg] = true; };
      // In probe mode, record a failed new-behaviour step and carry on.
      const soft = async (label, fn) => {
        try { await fn(); } catch (e) { if (!PROBE) throw e; findings['step:' + label] = String(e.message).split('\n')[0]; }
      };
      const check = async (label, scope) => {
        await page.waitForTimeout(150);
        const a = await page.evaluate(audit, scope);
        const bad = Object.entries(a).filter(([k, v]) => Array.isArray(v) && v.length);
        if (bad.length) findings[label] = Object.fromEntries(bad);
        if (!PROBE) {
          for (const [k, v] of bad) assert.fail(`${vp.name} · ${label}: ${k}: ${JSON.stringify(v.slice(0, 6))}`);
        }
      };
      const shot = async (label) => {
        const file = path.join(SHOTS, `${vp.name}-${label}.png`);
        await page.screenshot({ path: file });
        shots.push(file);
      };

      const user = `provtouch-${crypto.randomBytes(4).toString('hex')}`;
      await page.goto(`${base}/__fixture_session?user=${encodeURIComponent(user)}`, { waitUntil: 'domcontentloaded' });
      await page.goto(`${base}/`, { waitUntil: 'load' });
      // Phones/narrow tablets show the nav behind the menu button.
      const nav = page.locator('.nav-item').filter({ hasText: /^Images & Stacks$/ });
      await page.locator('.nav-item').filter({ hasText: /^Fleet Overview$/ }).waitFor({ state: 'attached', timeout: 15000 });
      if (!(await nav.isVisible())) await page.getByRole('button', { name: /menu|navigation/i }).first().tap();
      await nav.tap();
      await page.getByRole('heading', { name: 'Images & Stacks', level: 1 }).waitFor();
      await page.getByRole('status', { name: 'Image store status' }).getByText('store mounted', { exact: true }).waitFor();

      // --- deploy tab: choose target, tap an image row, tap stacks ------------
      const deploy = page.getByRole('region', { name: 'Deploy' });
      const deployBtn = deploy.getByRole('button', { name: 'Deploy' });
      await page.getByLabel('Machine (from inventory)').selectOption({ label: 'neon — Raspberry Pi Pi 5 · arm64' });
      await page.getByLabel('Netboot interface').waitFor();
      // Hover-only facts have a visible equivalent: arch source is printed.
      await soft('arch-source', () => page.locator('.prov-facts').getByText(/via agent/).waitFor({ timeout: 2000 }));
      // A disabled image row explains itself on tap (no hover on iPad).
      const x86Row = page.locator('.prov-irow').filter({ has: page.getByRole('radio', { name: /^Select Debian 13 \(trixie\) x86_64/ }) });
      await soft('row-why', async () => { await x86Row.locator('.meta').tap(); await x86Row.getByText(/image is x86_64, target is arm64/).waitFor({ timeout: 2000 }); });
      // Tapping anywhere on a selectable row picks it.
      const armRow = page.locator('.prov-irow').filter({ has: page.getByRole('radio', { name: 'Select Debian 13 (trixie) arm64' }) });
      await soft('row-tap', () => armRow.locator('.meta').tap({ timeout: 2000 }));
      if (PROBE) await page.getByRole('radio', { name: 'Select Debian 13 (trixie) arm64' }).check({ force: true });
      assert.equal(await page.getByRole('radio', { name: 'Select Debian 13 (trixie) arm64' }).isChecked(), true, `${vp.name}: row tap selects the image`);
      await page.locator('.prov-stack').filter({ hasText: 'Monitoring' }).locator('label').first().tap();
      assert.equal(await page.getByRole('checkbox', { name: 'Monitoring' }).isChecked(), true, `${vp.name}: stack label tap toggles`);
      await deploy.getByText(/1·Core[\s\S]*2·Web[\s\S]*3·Monitoring/i).first().waitFor();
      await check('deploy', '.prov-page');
      // The deploy action is reachable: scrolled into view it sits inside the viewport.
      await deployBtn.scrollIntoViewIfNeeded();
      const db = await deployBtn.boundingBox();
      must(db && db.y >= 0 && db.y + db.height <= vp.height + 1, `${vp.name}: deploy button is on screen (${JSON.stringify(db)})`);
      const dbar = await page.locator('.prov-dbar').boundingBox();
      must(dbar.height <= vp.height * 0.45, `${vp.name}: sticky deploy bar leaves the page usable (${Math.round(dbar.height)}px of ${vp.height})`);
      await page.locator('.content').evaluate((el) => { el.scrollTop = 0; });
      await shot('deploy-top');
      await deployBtn.scrollIntoViewIfNeeded();
      await shot('deploy-bar');

      // --- reimage confirm sheet with the typed name ---------------------------
      await deployBtn.tap();
      let dialog = page.getByRole('alertdialog', { name: 'Stage reinstall of neon?' });
      await dialog.waitFor();
      const word = dialog.getByLabel('Type neon to confirm');
      await word.tap();
      await check('confirm-reimage', '[role=alertdialog]');
      await word.fill('neon');
      const stage = dialog.getByRole('button', { name: 'Stage reinstall' });
      const sb = await stage.boundingBox();
      must(sb && sb.y + sb.height <= vp.height + 1 && sb.x + sb.width <= vp.width + 1, `${vp.name}: confirm action on screen`);
      await shot('confirm-reimage');
      await stage.tap();
      await dialog.waitFor({ state: 'detached' });
      assert.equal(writes().length, 1, `${vp.name}: one stage write from the touch flow`);

      // --- run log drawer ---------------------------------------------------
      const runs = page.locator('section[aria-labelledby="prov-h-runs"]');
      await page.getByRole('radio', { name: 'Apply stacks only' }).tap();
      await deployBtn.tap();
      dialog = page.getByRole('alertdialog', { name: 'Dry-run stacks on neon?' });
      await dialog.waitFor();
      await check('confirm-apply', '[role=alertdialog]');
      await dialog.getByRole('button', { name: 'Run dry-run' }).tap();
      await dialog.waitFor({ state: 'detached' });
      await runs.getByText(/\(dry-run\)/).waitFor();
      await runs.getByRole('button', { name: /^Helper log for run / }).first().tap();
      const drawer = page.getByRole('dialog', { name: 'Apply log · neon' });
      await drawer.getByText(/PLAY RECAP neon ok=3/).waitFor();
      await check('log-drawer', '[role=dialog]');
      const tailFont = await drawer.locator('.prov-tail').evaluate((el) => parseFloat(getComputedStyle(el).fontSize));
      must(tailFont >= 12, `${vp.name}: log text is readable (${tailFont}px)`);
      await shot('log-drawer');
      await drawer.getByRole('button', { name: 'Close' }).first().tap();
      await drawer.waitFor({ state: 'detached' });
      await page.getByRole('radio', { name: 'Reimage (wipe + install)' }).tap();

      // --- hardware-model arch edit ---------------------------------------------
      await page.getByLabel('Machine (from inventory)').selectOption({ label: 'krypton — Dell R410' });
      await page.locator('.prov-facts').getByRole('button', { name: 'Set' }).tap();
      dialog = page.getByRole('alertdialog', { name: 'CPU architecture of Dell R410' });
      await dialog.waitFor();
      await check('model-arch', '[role=alertdialog]');
      await shot('model-arch');
      await dialog.getByRole('button', { name: 'Cancel' }).tap();
      await dialog.waitFor({ state: 'detached' });

      // --- delete confirm (destructive, touch) ---------------------------------
      await page.getByRole('button', { name: 'Delete stack Web' }).tap();
      dialog = page.getByRole('alertdialog', { name: 'Delete stack Web?' });
      await dialog.waitFor();
      await check('confirm-delete', '[role=alertdialog]');
      await dialog.getByRole('button', { name: 'Cancel' }).tap();
      await dialog.waitFor({ state: 'detached' });

      // --- editors ------------------------------------------------------------
      await page.getByRole('button', { name: 'Add image' }).tap();
      let ed = page.getByRole('dialog').last();
      await ed.getByLabel('Source URL').waitFor();
      await ed.getByLabel('Source URL').tap();
      await check('image-editor', '[role=dialog]');
      await shot('image-editor');
      await ed.getByRole('button', { name: 'Close' }).first().tap();
      await ed.waitFor({ state: 'detached' });

      await page.getByRole('button', { name: 'Edit stack Monitoring' }).tap();
      ed = page.getByRole('dialog').last();
      await ed.getByText('prom-tools').first().waitFor();
      await check('stack-editor', '[role=dialog]');
      await shot('stack-editor');
      await ed.getByRole('button', { name: 'Close' }).first().tap();
      await ed.waitFor({ state: 'detached' });

      // --- software pool --------------------------------------------------------
      await page.getByRole('button', { name: 'Software pool' }).tap();
      const pool = page.locator('section[aria-labelledby="prov-h-pool"]');
      await pool.getByText('node-exporter').first().waitFor();
      await check('pool', '.prov-page');
      await shot('pool');
      await pool.getByRole('button', { name: 'Edit node-exporter' }).tap();
      ed = page.getByRole('dialog').last();
      await ed.getByRole('heading', { name: 'Artifacts' }).waitFor();
      await check('software-editor', '[role=dialog]');
      await shot('software-editor');
      await ed.getByRole('button', { name: 'Close' }).first().tap();
      await ed.waitFor({ state: 'detached' });

      if (pageErrors.length) findings.pageErrors = pageErrors;
      if (!PROBE) assert.deepEqual(pageErrors, [], `${vp.name}: no JavaScript or console errors`);
      report[vp.name] = findings;
      await ctx.close();
    }

    // Desktop regression guard: the 1440 layout keeps its two columns.
    const ctx = await browser.newContext({ viewport: { width: 1440, height: 1000 } });
    const page = await ctx.newPage();
    await page.goto(`${base}/__fixture_session?user=provtouch-desk`, { waitUntil: 'domcontentloaded' });
    await page.goto(`${base}/`, { waitUntil: 'load' });
    await page.locator('.nav-item').filter({ hasText: /^Images & Stacks$/ }).click();
    await page.getByRole('heading', { name: 'Images & Stacks', level: 1 }).waitFor();
    const cols = await page.locator('.prov-cols').evaluate((el) => getComputedStyle(el).gridTemplateColumns.split(' ').length);
    assert.equal(cols, 2, 'desktop keeps the two-column images/stacks layout');
    const ib = await page.locator('.prov-ib').first().boundingBox();
    assert(ib.width <= 34, `desktop icon buttons keep their compact size (${ib.width})`);
    await page.getByRole('button', { name: 'Software pool' }).click();
    const display = await page.locator('table.prov-pool tbody tr').first().evaluate((el) => getComputedStyle(el).display);
    assert.equal(display, 'table-row', 'desktop pool stays a table');
    const file = path.join(SHOTS, 'desktop-1440-pool.png');
    await page.screenshot({ path: file });
    shots.push(file);
    await ctx.close();

    if (PROBE) console.log(JSON.stringify(report, null, 1));
    console.log(`Provisioning touch acceptance (${engine}${engine === 'chromium' ? ' with iPad/iPhone emulation' : ''}) passed at `
      + VIEWPORTS.map((v) => `${v.width}×${v.height}`).join(', ') + ' + desktop 1440 guard.');
    console.log('Screenshots:\n' + shots.join('\n'));
    if (phpErrors.trim() && /Fatal|Parse error/.test(phpErrors)) throw new Error('PHP fixture errors:\n' + phpErrors);
  } catch (error) {
    failure = error;
    if (PROBE) console.log(JSON.stringify(report, null, 1));
  } finally {
    if (browser) await browser.close();
    if (child && child.exitCode === null) child.kill('SIGTERM');
    if (child) await new Promise((resolve) => child.once('exit', resolve));
    fs.rmSync(temp, { recursive: true, force: true });
  }
  if (failure) {
    console.error(failure.stack || failure.message || failure);
    process.exit(1);
  }
}

main();
