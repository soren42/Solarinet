#!/usr/bin/env node
'use strict';

// Real dashboard → authenticated PHP routes → scratch SoR browser acceptance.
// This test refuses a non-loopback or non-scratch database by construction.
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { pathToFileURL } = require('node:url');
const { chromium } = require('playwright');

const ROOT = process.env.SOLARI_TEST_ROOT
  ? path.resolve(process.env.SOLARI_TEST_ROOT)
  : path.resolve(__dirname, '../..');
const PUBLIC = path.join(ROOT, 'dashboard/public');
const API_INDEX = path.join(ROOT, 'dashboard/api/index.php');
const AUTH_LIB = path.join(ROOT, 'dashboard/api/lib/Auth.php');
const DB_HOST = process.env.SOR_DB_HOST || '';
const DB_PORT = process.env.SOR_DB_PORT || '3306';
const DB_NAME = process.env.SOR_DB_NAME || '';
const DB_USER = process.env.SOR_DB_USER || 'root';
const DB_PASS = process.env.SOR_DB_PASS || '';

function requireScratchDatabase() {
  if (process.env.SOLARI_TEST_SOR_DB !== '1' ||
      !['127.0.0.1', 'localhost'].includes(DB_HOST) ||
      !/^solarinet_[a-z0-9_]+$/.test(DB_NAME) || !/^\d+$/.test(DB_PORT)) {
    throw new Error('Set SOLARI_TEST_SOR_DB=1 and a loopback solarinet_* SOR_DB_* test database.');
  }
  if (!fs.existsSync(PUBLIC) || !fs.existsSync(API_INDEX) || !fs.existsSync(AUTH_LIB)) {
    throw new Error('Dashboard browser-test root must contain the public app and PHP API.');
  }
  if (process.env.SOLARI_TEST_ROOT && !fs.existsSync(path.join(ROOT, 'RELEASE.json'))) {
    throw new Error('SOLARI_TEST_ROOT must point at an installed release containing RELEASE.json.');
  }
  if (sql("SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'") !== 'solarinet-ci-scratch-v1') {
    throw new Error('Refusing browser fixtures: scratch SoR sentinel is missing.');
  }
}

function sql(query) {
  const result = spawnSync('mariadb', [
    '--no-defaults', '--protocol=tcp', `--host=${DB_HOST}`, `--port=${DB_PORT}`,
    `--user=${DB_USER}`, '--batch', '--raw', '--skip-column-names', DB_NAME,
    '-e', query,
  ], { encoding: 'utf8', env: { ...process.env, MYSQL_PWD: DB_PASS } });
  if (result.status !== 0) throw new Error('Scratch SoR fixture SQL failed: ' + (result.stderr || 'MariaDB client error').trim());
  return result.stdout.trim();
}

function quote(value) {
  return "'" + String(value).replaceAll("'", "''") + "'";
}

function freePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const { port } = server.address();
      server.close((error) => error ? reject(error) : resolve(port));
    });
  });
}

async function waitForServer(url, child) {
  for (let attempt = 0; attempt < 60; attempt += 1) {
    if (child.exitCode !== null) throw new Error(`PHP fixture exited before ready (${child.exitCode}).`);
    try {
      const response = await fetch(url, { signal: AbortSignal.timeout(500) });
      if (response.ok) return;
    } catch (_) { /* wait for PHP's local listener */ }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('PHP dashboard fixture did not become ready.');
}

function phpRouter(sessionUser, scanStatePath) {
  return `<?php
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
if ($path === '/__health') { header('Content-Type: text/plain'); echo 'ok'; exit; }
if ($path === '/__fixture_session') {
    require_once ${JSON.stringify(AUTH_LIB)};
    Auth::establishSession(['username' => ${JSON.stringify(sessionUser)}, 'role' => 'operator', 'source' => 'oidc']);
    header('Content-Type: application/json'); echo json_encode(['ready' => true]); exit;
}
if ($path === '/api/discovery/scan' && ($_SERVER['REQUEST_METHOD'] ?? '') === 'POST') {
    $body = json_decode(file_get_contents('php://input') ?: '[]', true) ?: [];
    http_response_code(202); header('Content-Type: application/json');
    echo json_encode(['ok' => true, 'data' => ['cidr' => $body['cidr'] ?? '', 'requestId' => 940, 'status' => 'admission_pending'], 'ts' => time()]); exit;
}
if ($path === '/__finish_scan') {
    file_put_contents(${JSON.stringify(scanStatePath)}, 'scan-done');
    header('Content-Type: application/json'); echo '{"ok":true}'; exit;
}
if ($path === '/__finish_sweep') {
    file_put_contents(${JSON.stringify(scanStatePath)}, 'sweep-done');
    header('Content-Type: application/json'); echo '{"ok":true}'; exit;
}
if ($path === '/__finish_candidate') {
    file_put_contents(${JSON.stringify(scanStatePath)}, 'candidate-done');
    header('Content-Type: application/json'); echo '{"ok":true}'; exit;
}
if ($path === '/__finish_admission') {
    file_put_contents(${JSON.stringify(scanStatePath)}, 'admission-done');
    header('Content-Type: application/json'); echo '{"ok":true}'; exit;
}
if ($path === '/api/discovery/scan-requests/940') {
    $state = is_file(${JSON.stringify(scanStatePath)})
        && in_array(file_get_contents(${JSON.stringify(scanStatePath)}), ['admission-done', 'sweep-done', 'candidate-done', 'scan-done'], true)
        ? 'done' : 'pending';
    header('Content-Type: application/json');
    echo json_encode(['ok' => true, 'data' => ['requestId' => 940, 'state' => $state,
        'scanId' => $state === 'done' ? 941 : null, 'hostsTotal' => $state === 'done' ? 16 : null],
        'ts' => time()]); exit;
}
if ($path === '/api/discovery/scan-jobs') {
    $phase = is_file(${JSON.stringify(scanStatePath)})
        ? file_get_contents(${JSON.stringify(scanStatePath)}) : '';
    header('Content-Type: application/json');
    $jobs = in_array($phase, ['admission-done', 'sweep-done', 'candidate-done', 'scan-done'], true) ? [[
        'scanId' => 941, 'cidr' => '127.0.0.1/32', 'ports' => '', 'status' => 'running',
        'hostsTotal' => 16, 'hostsDone' => in_array($phase, ['sweep-done', 'candidate-done', 'scan-done'], true) ? 16 : 4,
        'hostsFound' => in_array($phase, ['candidate-done', 'scan-done'], true) ? 1 : 0,
        'error' => '', 'createdAt' => date('Y-m-d H:i:s.000'),
    ]] : [];
    echo json_encode(['ok' => true, 'data' => $jobs, 'ts' => time()]); exit;
}
if ($path === '/api/discovery/scans/941') {
    $scanPhase = is_file(${JSON.stringify(scanStatePath)})
        ? file_get_contents(${JSON.stringify(scanStatePath)}) : '';
    $finished = $scanPhase === 'scan-done';
    $sweepComplete = in_array($scanPhase, ['sweep-done', 'candidate-done', 'scan-done'], true);
    header('Content-Type: application/json');
    echo json_encode(['ok' => true, 'data' => ['scanId' => 941,
        'status' => $finished ? 'completed' : 'running',
        'hostsDone' => $sweepComplete ? 16 : 4, 'hostsTotal' => 16,
        'hostsFound' => in_array($scanPhase, ['candidate-done', 'scan-done'], true) ? 1 : 0],
        'ts' => time()]); exit;
}
if ($path === '/api/discovery' && ($_GET['status'] ?? '') === 'all') {
    $scanPhase = is_file(${JSON.stringify(scanStatePath)})
        ? file_get_contents(${JSON.stringify(scanStatePath)}) : '';
    $rows = in_array($scanPhase, ['candidate-done', 'scan-done'], true)
        ? [['discId' => 1901, 'host' => 'live-scan-fixture', 'ip' => '192.0.2.7',
            'kind' => 'host', 'via' => 'portscan', 'services' => ['http:80'],
            'status' => 'new', 'seenCount' => 1]] : [];
    header('Content-Type: application/json');
    echo json_encode(['ok' => true, 'data' => $rows, 'ts' => time()]); exit;
}
if (is_string($path) && str_starts_with($path, '/api/')) {
    $actual = $path === '/api/auth/whoami' || $path === '/api/inventory'
        || (is_string($path) && (str_starts_with($path, '/api/inventory/')
            || str_starts_with($path, '/api/rackwire/')));
    if ($actual) { require ${JSON.stringify(API_INDEX)}; exit; }
    header('Content-Type: application/json');
    echo json_encode(['ok' => true, 'data' => [], 'ts' => time()]); exit;
}
return false;
?>`;
}

async function main() {
  requireScratchDatabase();
  const suffix = crypto.randomBytes(6).toString('hex');
  const exportPath = process.env.SOLARI_SCANNER_EXPORT_FILE
    ? path.resolve(process.env.SOLARI_SCANNER_EXPORT_FILE) : '';
  const snapshot = exportPath
    ? JSON.parse(fs.readFileSync(exportPath, 'utf8'))
    : {
      schema_version: 1,
      exported_at: new Date().toISOString(),
      items: [{ id: crypto.randomUUID(), name: `Temporary unit ${suffix}`, manufacturer: `Browser Scanner ${suffix}`, model: `Temporary unit ${suffix}`, category: 'scanner', barcode: `PRODUCT-${suffix}`, notes: '', created_at: new Date().toISOString(), updated_at: new Date().toISOString(), version: 1, archived: 0 }],
      assets: [{ id: crypto.randomUUID(), item_id: '', tag: `BROWSER-SCAN-${suffix.toUpperCase()}`, serial: `BROWSER-SER-${suffix.toUpperCase()}`, location: `Browser Fixture ${suffix}`, status: 'in_stock', notes: '', created_at: new Date().toISOString(), updated_at: new Date().toISOString(), version: 1, archived: 0 }],
    };
  if (!exportPath) snapshot.assets[0].item_id = snapshot.items[0].id;
  assert.equal(snapshot.schema_version, 1, 'scanner export must use supported schema version 1');
  assert(Array.isArray(snapshot.items) && Array.isArray(snapshot.assets), 'scanner export must contain catalog and asset arrays');
  const activeItems = snapshot.items.filter((item) => item && item.archived === 0);
  const activeAssets = snapshot.assets.filter((asset) => asset && asset.archived === 0);
  assert(activeItems.length > 0 && activeAssets.length > 0, 'scanner export must include active catalog items and assets');
  const itemIdSet = new Set(activeItems.map((item) => item.id));
  const importAssets = activeAssets.filter((asset) => itemIdSet.has(asset.item_id));
  assert.equal(importAssets.length, activeAssets.length, 'every active scanner asset must reference an active catalog item');
  assert(importAssets.every((asset) => typeof asset.tag === 'string' && asset.tag.trim()), 'every active scanner asset must have a tag');
  const focusAsset = importAssets[0];
  const focusItem = activeItems.find((item) => item.id === focusAsset.item_id);
  const itemIds = activeItems.map((item) => item.id);
  const assetIds = importAssets.map((asset) => asset.id);
  const importedTags = importAssets.map((asset) => asset.tag);
  assert.equal(new Set(importedTags).size, importedTags.length, 'active scanner asset tags must be unique');
  const serial = focusAsset.serial || '';
  const tag = focusAsset.tag;
  const make = focusItem.manufacturer || 'Unknown manufacturer';
  const model = focusItem.model || focusItem.name;
  assert(typeof model === 'string' && model.trim(), 'selected scanner item must identify a hardware model');
  const sourceLocations = Array.from(new Set(importAssets
    .map((asset) => typeof asset.location === 'string' ? asset.location.trim() : '')
    .filter(Boolean)));
  const locationChoices = Object.create(null);
  const sessionUser = `scanner-browser-${suffix}`;
  const actor = `dashboard:${sessionUser}`;
  const sourceExistedBefore = sql(`SELECT id FROM sources WHERE slug='scanner-ingest' LIMIT 1`);
  const manualSource = sql("SELECT id FROM sources WHERE slug='manual' LIMIT 1");
  const rackwireSource = sql("SELECT id FROM sources WHERE slug='rackwire' AND deleted_at IS NULL LIMIT 1");
  assert(manualSource, 'scratch SoR schema must contain the manual provenance source');
  assert(rackwireSource, 'scratch SoR schema must contain the RackWire provenance source');
  const rackwirePlanName = `RackWire Browser ${suffix}`;
  const rackwirePlanNote = `CSRF persistence proof ${suffix}`;
  const linkedEntityName = `scanner-node-${suffix}`;
  const peerEntityName = `scanner-peer-${suffix}`;
  const unknownPeerEntityName = `scanner-unknown-peer-${suffix}`;
  const committedCableLabel = `scanner-cable-${suffix}`;
  const mappedCableLabel = `scanner-cable-port-${suffix}`;
  const unknownPortCableLabel = `scanner-cable-unknown-port-${suffix}`;
  const scannerInterfaceName = `scanner-uplink-${suffix}`;
  let rackwirePlanId = null;
  let linkedEntityId = null;
  let peerEntityId = null;
  let unknownPeerEntityId = null;
  let scannerInterfaceId = null;

  let locationIds = [];
  let child = null;
  let browser = null;
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'solarinet-scanner-browser-'));
  const sessions = path.join(temp, 'sessions');
  fs.mkdirSync(sessions, { mode: 0o700 });
  const routerPath = path.join(temp, 'router.php');
  const scanStatePath = path.join(temp, 'scan-state');
  let failure = null;
  let phpErrors = '';
  let browserDiagnostics = '';

  try {
    for (const locationName of sourceLocations) {
      sql(`INSERT INTO locations (name,loc_type,source_id,asserted_kind,asserted_at)
           VALUES (${quote(locationName)},'site',${manualSource},'human',NOW()),
                  (${quote(locationName)},'site',${manualSource},'human',NOW())`);
      const choices = sql(`SELECT id FROM locations WHERE name=${quote(locationName)} AND deleted_at IS NULL ORDER BY id`).split(/\s+/).filter(Boolean);
      assert.equal(choices.length, 2, `scratch SoR must contain two duplicate choices for source location ${locationName}`);
      locationChoices[locationName] = choices.map(Number);
      locationIds.push(...choices);
    }
    const selectedLocation = focusAsset.location ? locationChoices[focusAsset.location.trim()]?.[1] : null;

    sql(`INSERT INTO entities (name,entity_type,source_id,asserted_kind)
         VALUES (${quote(linkedEntityName)},'network_gear',${Number(manualSource)},'human'),
                (${quote(peerEntityName)},'server',${Number(manualSource)},'human'),
                (${quote(unknownPeerEntityName)},'server',${Number(manualSource)},'human')`);
    linkedEntityId = Number(sql(`SELECT id FROM entities WHERE name=${quote(linkedEntityName)} AND deleted_at IS NULL LIMIT 1`));
    peerEntityId = Number(sql(`SELECT id FROM entities WHERE name=${quote(peerEntityName)} AND deleted_at IS NULL LIMIT 1`));
    unknownPeerEntityId = Number(sql(`SELECT id FROM entities WHERE name=${quote(unknownPeerEntityName)} AND deleted_at IS NULL LIMIT 1`));
    assert(linkedEntityId > 0 && peerEntityId > 0 && unknownPeerEntityId > 0,
      'scratch SoR must include canonical entities for mapped, entity-level, and unknown-port cable cases');
    sql(`INSERT INTO interfaces (entity_id,name,if_kind,source_id,asserted_kind,asserted_at)
         VALUES (${linkedEntityId},${quote(scannerInterfaceName)},'physical',${Number(manualSource)},'human',NOW())`);
    scannerInterfaceId = Number(sql(`SELECT id FROM interfaces WHERE entity_id=${linkedEntityId}
      AND name=${quote(scannerInterfaceName)} AND deleted_at IS NULL LIMIT 1`));
    assert(scannerInterfaceId > 0, 'scratch SoR must include the scanner device interface');

    const seedNote = `seed ${suffix}`;
    const initialRackwirePlan = JSON.stringify({ devices: [], cables: [], racks: [], notes: [{ uid: `seed-${suffix}`, text: seedNote, x: 0, y: 0 }], codeMap: {} });
    sql(`INSERT INTO rw_plans (name,slug,plan_json,device_count,cable_count,updated_by,source_id,asserted_kind,asserted_at)
         VALUES (${quote(rackwirePlanName)},${quote(`rackwire-browser-${suffix}`)},${quote(initialRackwirePlan)},0,0,${quote(actor)},${Number(rackwireSource)},'human',NOW())`);
    rackwirePlanId = Number(sql(`SELECT id FROM rw_plans WHERE slug=${quote(`rackwire-browser-${suffix}`)} AND deleted_at IS NULL LIMIT 1`));
    assert(Number.isSafeInteger(rackwirePlanId) && rackwirePlanId > 0, 'scratch RackWire plan must be created');

    fs.writeFileSync(routerPath, phpRouter(sessionUser, scanStatePath), { mode: 0o600 });
    const port = await freePort();
    const base = `http://127.0.0.1:${port}`;
    const childEnv = {
      ...process.env,
      SOR_DB_HOST: DB_HOST, SOR_DB_PORT: DB_PORT, SOR_DB_NAME: DB_NAME,
      SOR_DB_USER: DB_USER, SOR_DB_PASS: DB_PASS,
    };
    child = spawn('php', ['-d', `session.save_path=${sessions}`, '-S', `127.0.0.1:${port}`,
      '-t', PUBLIC, routerPath], { cwd: ROOT, env: childEnv, stdio: ['ignore', 'ignore', 'pipe'] });
    child.stderr.on('data', (data) => { phpErrors = (phpErrors + data.toString()).slice(-4000); });
    await waitForServer(`${base}/__health`, child);

    browser = await chromium.launch({
      headless: true,
      args: ['--no-sandbox'],
      ...(process.env.SOLARI_CHROMIUM_EXECUTABLE ? { executablePath: process.env.SOLARI_CHROMIUM_EXECUTABLE } : {}),
    });
    const context = await browser.newContext();
    const page = await context.newPage();
    const pageErrors = [];
    const browserConsole = [];
    const externalRequests = [];
    const scanJobsResponses = [];
    page.on('pageerror', (error) => pageErrors.push(error.message));
    page.on('console', (message) => {
      if (message.type() === 'error') browserConsole.push(message.text());
    });
    page.on('request', (request) => {
      if (!request.url().startsWith(base + '/') && !request.url().startsWith('file://')) externalRequests.push(request.url());
    });
    page.on('response', async (response) => {
      if (response.url().includes('/api/discovery/scan-jobs')) {
        let body = '';
        try { body = await response.text(); } catch (_) { /* response body may be unavailable */ }
        scanJobsResponses.push(`${response.status()} ${body.slice(0, 500)}`);
      }
    });

    await page.goto(`${base}/__fixture_session`, { waitUntil: 'domcontentloaded' });
    await page.goto(`${base}/`, { waitUntil: 'load' });
    const inventoryNav = page.locator('.nav-item').filter({ hasText: /^Inventory$/ });
    try {
      await inventoryNav.waitFor({ timeout: 15000 });
    } catch (error) {
      browserDiagnostics = `\nURL: ${page.url()}\nbody: ${(await page.locator('body').innerText()).slice(0, 1200)}\npage errors: ${pageErrors.join(' | ')}\nPHP: ${phpErrors}`;
      throw error;
    }
    assert.equal(await page.evaluate(() => window.SOLARI && window.SOLARI.source), 'live', 'dashboard must boot in live mode');
    await inventoryNav.click();
    await page.getByRole('button', { name: 'Scanner import', exact: true }).click();
    await page.getByRole('heading', { name: 'Import a scanner-ingest export' }).waitFor();

    const scannerExportName = exportPath ? path.basename(exportPath) : 'scanner-fixture.json';
    await page.getByLabel('Scanner JSON export').setInputFiles({
      name: scannerExportName, mimeType: 'application/json',
      buffer: exportPath ? fs.readFileSync(exportPath) : Buffer.from(JSON.stringify(snapshot)),
    });
    await page.getByText(`${scannerExportName}: ${snapshot.items.length} catalog items, ${snapshot.assets.length} assets.`).waitFor();
    assert(await page.getByRole('note').getByText('Some SoR locations share a name').isVisible(),
      'duplicate SoR names should be called out');
    for (const locationName of sourceLocations) {
      await page.getByLabel(`SoR destination for source location ${locationName}`).selectOption(String(locationChoices[locationName][1]));
    }

    const before = await context.request.get(`${base}/api/inventory`);
    assert.equal(before.status(), 200);
    const beforeData = (await before.json()).data;
    assert(!beforeData.units.some((unit) => importedTags.includes(unit.assetTag)), 'preview fixture assets must not exist before apply');

    await page.getByRole('button', { name: 'Preview import' }).click();
    const preview = page.getByRole('status').filter({ hasText: 'Preview ready' });
    await preview.waitFor();
    await preview.getByText(`insert hardware units`, { exact: false }).waitFor();
    for (const locationName of sourceLocations) {
      await preview.getByText(`#${locationChoices[locationName][1]} · ${locationName}`, { exact: false }).waitFor();
    }
    const afterPreview = await context.request.get(`${base}/api/inventory`);
    const afterPreviewData = (await afterPreview.json()).data;
    assert(!afterPreviewData.units.some((unit) => importedTags.includes(unit.assetTag)), 'preview must leave SoR inventory unchanged');

    await page.getByRole('button', { name: 'Apply import' }).click();
    await page.getByRole('status').filter({ hasText: 'Import complete' }).waitFor();
    const afterApply = await context.request.get(`${base}/api/inventory`);
    assert.equal(afterApply.status(), 200);
    const appliedData = (await afterApply.json()).data;
    const imported = appliedData.units.find((unit) => unit.assetTag === tag);
    assert(imported, 'applied scanner asset must be returned by the real SoR inventory route');
    assert.equal(Number(imported.locationId), selectedLocation, 'mapped SoR location ID must be committed');
    const importedBatch = appliedData.units.filter((unit) => importedTags.includes(unit.assetTag));
    assert.equal(importedBatch.length, importAssets.length, 'every active scanner export asset must be imported');

    // Reconcile the imported physical unit to its canonical network entity in
    // the operator UI before asking RackWire to resolve cable endpoints.
    await page.getByRole('button', { name: 'Components', exact: true }).click();
    await page.getByPlaceholder('Find component…').fill(tag);
    const importedRow = page.locator('tr').filter({ hasText: tag });
    await importedRow.getByRole('button', { name: 'Link entity', exact: true }).click();
    await page.getByRole('heading', { name: 'Physical identity ↔ SoR entity', exact: true }).waitFor();
    await page.locator('.inv-entity-link__search input').fill(linkedEntityName);
    await page.getByRole('button', { name: 'Search', exact: true }).click();
    await page.locator('.inv-entity-link__form select').selectOption(String(linkedEntityId));
    const linkResponseWait = page.waitForResponse((response) =>
      response.url().includes(`/api/inventory/components/${imported.id}/entity-link`) && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Link entity', exact: true }).last().click();
    const entityLinkResponse = await linkResponseWait;
    assert.equal(entityLinkResponse.status(), 200, 'operator-reviewed inventory association must pass the real SoR API');
    const entityLinkBody = entityLinkResponse.request().postDataJSON();
    assert.equal(Number(entityLinkBody.entity_id), linkedEntityId, 'the association request must submit the chosen entity ID');
    assert.equal(Number(sql(`SELECT entity_id FROM entity_hardware_unit_links WHERE hardware_unit_id=${Number(imported.id)}`)), linkedEntityId,
      'the entity association registry must reflect the operator selection');
    await page.getByRole('status').filter({ hasText: 'Entity association saved' }).waitFor();
    await page.getByRole('button', { name: 'Close', exact: true }).last().click();

    await page.getByRole('button', { name: 'RackWire device profiles', exact: true }).click();
    if (focusAsset.location) await importedRow.getByText(focusAsset.location, { exact: true }).waitFor();
    const profileModel = page.getByLabel('SoR hardware model');
    await profileModel.waitFor();
    await page.waitForFunction((id) => Array.from(document.querySelectorAll('select[aria-label="SoR hardware model"] option')).some((option) => option.value === String(id)), imported.modelId);
    await profileModel.selectOption(String(imported.modelId));
    await page.getByLabel('Planner label').fill(`${make} ${model}`);
    await page.getByLabel('Port group 1 label').fill('Ethernet');
    await page.getByLabel('Port group 1 count').fill('8');
    const profileResponseWait = page.waitForResponse((response) =>
      response.url().includes(`/api/rackwire/library/${imported.modelId}/groups`) && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Save RackWire profile', exact: true }).click();
    const profileResponse = await profileResponseWait;
    assert.equal(profileResponse.status(), 201, 'the inventory profile editor must save through the guarded RackWire SoR API');
    const profileBody = profileResponse.request().postDataJSON();
    assert.equal(profileBody.groups[0].count, 8, 'profile editor must persist the reviewed port count');
    const profileHeaders = await profileResponse.request().allHeaders();
    assert(typeof profileHeaders['x-solari-csrf'] === 'string' && profileHeaders['x-solari-csrf'].length > 20,
      'RackWire profile write must carry the dashboard CSRF token');
    await page.getByRole('status').filter({ hasText: 'Profile saved' }).waitFor();
    await page.getByRole('button', { name: 'Open RackWire planner', exact: true }).click();

    // The embedded planner has a separate document and its own CSRF cache.
    // Drive the real same-origin frame against the real guarded RackWire route;
    // a missing X-Solari-CSRF header previously made every autosave fail 403.
    await page.evaluate(({ id, name }) => {
      localStorage.setItem('rackwire.project.v2.planId', String(id));
      localStorage.setItem('rackwire.project.v2.planName', name);
    }, { id: rackwirePlanId, name: rackwirePlanName });
    const frameSelector = 'iframe[title="RackWire network, power, and interconnect planner"]';
    await page.locator(frameSelector).waitFor();
    await page.waitForFunction(({ model, count }) => {
      const frame = document.querySelector('iframe[title="RackWire network, power, and interconnect planner"]');
      try {
        return frame && frame.contentWindow.RW && frame.contentWindow.RW.DEVICES.some((device) =>
          device.model === model && device.groups.some((group) => group.glyph === 'rj45' && group.count === count));
      } catch (_) { return false; }
    }, { model, count: 8 }, { timeout: 15000 });
    await page.waitForFunction((note) => {
      const frame = document.querySelector('iframe[title="RackWire network, power, and interconnect planner"]');
      try { return frame && frame.contentWindow.RackWire && frame.contentWindow.RackWire.getPlan().notes.some((item) => item.text === note); }
      catch (_) { return false; }
    }, seedNote, { timeout: 15000 });
    const rackwireFrame = page.frameLocator(frameSelector);
    try {
      await page.getByRole('button', { name: 'Scan inventory barcode', exact: true }).click({ timeout: 5000 });
    } catch (error) {
      throw new Error('RackWire SCAN control was unavailable. Frame text: ' + (await rackwireFrame.locator('body').innerText()).slice(0, 1600) + '\nPage: ' + (await page.locator('body').innerText()).slice(0, 800) + '\nConsole: ' + browserConsole.join(' | ') + '\n' + error.message);
    }
    const barcodeInput = rackwireFrame.getByLabel('UPC, EAN, or model code');
    try {
      await barcodeInput.waitFor({ timeout: 5000 });
    } catch (_) {
      throw new Error('RackWire scan dialog did not open after SCAN. Frame text: ' + (await rackwireFrame.locator('body').innerText()).slice(0, 1600) + '\nConsole: ' + browserConsole.join(' | '));
    }
    await barcodeInput.fill(tag);
    // Simulate one expiry-shaped 403, then keep the identity refresh and retry
    // on the real same-origin PHP/SoR routes to exercise the full iframe path.
    const csrfRetry = {
      active: false,
      lookupAttempts: 0,
      whoamiRefreshes: 0,
      firstToken: '',
      retryToken: '',
      firstExpired: false,
    };
    await page.route('**/api/auth/whoami', async (route) => {
      let frameUrl = '';
      try { frameUrl = route.request().frame().url(); } catch (_) {}
      if (csrfRetry.active && frameUrl.includes('/rackwire/')) csrfRetry.whoamiRefreshes += 1;
      await route.continue();
    });
    await page.route('**/api/inventory/codes/lookup', async (route) => {
      if (!csrfRetry.active) return route.continue();
      csrfRetry.lookupAttempts += 1;
      const token = route.request().headers()['x-solari-csrf'] || '';
      if (csrfRetry.lookupAttempts === 1) {
        csrfRetry.firstToken = token;
        csrfRetry.firstExpired = true;
        return route.fulfill({
          status: 403,
          contentType: 'application/json',
          body: JSON.stringify({
            ok: false,
            error: { code: 'forbidden', message: 'Missing or invalid CSRF token.' },
            ts: Date.now(),
          }),
        });
      }
      if (csrfRetry.lookupAttempts === 2) csrfRetry.retryToken = token;
      return route.continue();
    });
    await page.locator(frameSelector).evaluate((element) => {
      element.contentWindow.eval('rwPlanCsrf = ""; rwPlanCsrfPromise = null;');
    });
    csrfRetry.active = true;
    const barcodeLookupWait = page.waitForResponse((response) =>
      response.url().includes('/api/inventory/codes/lookup') && response.request().method() === 'POST'
        && response.status() === 200);
    await rackwireFrame.getByRole('button', { name: 'Look up', exact: true }).click();
    const barcodeLookup = await barcodeLookupWait;
    assert.equal(barcodeLookup.status(), 200, 'RackWire barcode scan must resolve a scanner-imported inventory unit');
    assert.equal(csrfRetry.firstExpired, true, 'the first barcode request must receive a simulated expired-token rejection');
    assert.equal(csrfRetry.lookupAttempts, 2, 'RackWire must issue exactly one lookup retry');
    assert.equal(csrfRetry.whoamiRefreshes, 2, 'RackWire must fetch identity once for the stale token and once to refresh it');
    assert(csrfRetry.firstToken.length > 20 && csrfRetry.retryToken.length > 20,
      'both the initial and refreshed barcode requests must carry a session CSRF token');
    const lookupHeaders = await barcodeLookup.request().allHeaders();
    assert(typeof lookupHeaders['x-solari-csrf'] === 'string' && lookupHeaders['x-solari-csrf'].length > 20,
      'RackWire barcode lookup must carry the dashboard CSRF token');
    await page.waitForFunction(({ assetTag, serial }) => {
      const frame = document.querySelector('iframe[title="RackWire network, power, and interconnect planner"]');
      try { return frame && frame.contentWindow.RackWire && frame.contentWindow.RackWire.getPlan().devices.some((device) => device.assetId === assetTag && device.serial === serial); }
      catch (_) { return false; }
    }, { assetTag: tag, serial }, { timeout: 10000 });
    let scannerPlanSaved = false;
    for (let attempt = 0; attempt < 40 && !scannerPlanSaved; attempt += 1) {
      const saved = JSON.parse(sql(`SELECT plan_json FROM rw_plans WHERE id=${rackwirePlanId}`));
      scannerPlanSaved = saved.devices.some((device) => device.assetId === tag && device.serial === serial);
      if (!scannerPlanSaved) await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert(scannerPlanSaved, 'RackWire should persist the scanned SoR asset tag and serial on the placed device');
    const savedResponse = page.waitForResponse((response) =>
      response.url().includes('/api/rackwire/plans') && response.request().method() === 'POST');
    await page.locator(frameSelector).evaluate((element, { note }) => {
      const plan = element.contentWindow.RackWire.getPlan();
      plan.notes = [{ uid: 'browser-note', text: note, x: 0, y: 0 }];
      element.contentWindow.RackWire.setPlan(plan);
    }, { note: rackwirePlanNote });
    const rackwireResponse = await savedResponse;
    assert.equal(rackwireResponse.status(), 201, 'embedded RackWire autosave must pass the real API policy');
    const rwHeaders = await rackwireResponse.request().allHeaders();
    assert.equal(rwHeaders.origin, base, 'RackWire save must remain same-origin');
    assert(typeof rwHeaders['x-solari-csrf'] === 'string' && rwHeaders['x-solari-csrf'].length > 20,
      'embedded RackWire save must carry the authenticated dashboard CSRF token');
    let storedPlan = null;
    for (let attempt = 0; attempt < 40; attempt += 1) {
      storedPlan = JSON.parse(sql(`SELECT plan_json FROM rw_plans WHERE id=${rackwirePlanId}`));
      if (storedPlan.notes.some((item) => item.text === rackwirePlanNote)) break;
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert.deepEqual(storedPlan.notes, [{ uid: 'browser-note', text: rackwirePlanNote, x: 0, y: 0 }], 'RackWire autosave must persist its edited plan to the SoR');
    assert(storedPlan.devices.some((device) => device.assetId === tag && device.serial === serial),
      'RackWire plan persistence must retain the SoR-linked scanned asset');

    const scannerDevice = storedPlan.devices.find((device) => device.assetId === tag);
    assert(scannerDevice && scannerDevice.uid, 'the scanned physical unit must remain a plan device');
    // Scanner lookup may leave the newly placed device selected. Clear that
    // state so the following pointer click always exercises the UI path.
    await page.locator(frameSelector).evaluate((element) => {
      element.contentWindow.RackWire.select([]);
    });
    await rackwireFrame.locator(`[data-kind="device"][data-uid="${scannerDevice.uid}"]`).click();
    const portMap = rackwireFrame.locator('input[placeholder="GigabitEthernet0/3=ge-3, Te1/0/1=sfp-1"]');
    await portMap.waitFor({ state: 'visible' });
    const scannerPortId = await page.locator(frameSelector).evaluate((element, uid) => {
      const frame = element.contentWindow;
      const device = frame.RackWire.getPlan().devices.find((item) => item.uid === uid);
      if (!device) return '';
      const geometry = frame.RW.layout({ groups: device.groups, name: device.name, model: device.model });
      const networkPort = geometry.ports.find((port) => port.domain === 'net');
      return networkPort ? networkPort.id : '';
    }, scannerDevice.uid);
    assert(scannerPortId, 'the reviewed RackWire profile must expose a physical port ID');
    const mappedSaveWait = page.waitForResponse((response) =>
      response.url().includes('/api/rackwire/plans') && response.request().method() === 'POST'
        && (response.request().postData() || '').includes(scannerInterfaceName)
        && (response.request().postData() || '').includes(scannerPortId));
    await portMap.fill(`${scannerInterfaceName}=${scannerPortId}`);
    const mappedSave = await mappedSaveWait;
    assert([200, 201].includes(mappedSave.status()), 'the operator port map must save through the RackWire SoR API');
    let mappedPlan = null;
    let scannerMapPersisted = false;
    for (let attempt = 0; attempt < 40; attempt += 1) {
      mappedPlan = JSON.parse(sql(`SELECT plan_json FROM rw_plans WHERE id=${rackwirePlanId}`));
      const mappedDevice = mappedPlan.devices.find((device) => device.uid === scannerDevice.uid);
      if (mappedDevice && mappedDevice.ifMap && mappedDevice.ifMap[scannerInterfaceName] === scannerPortId) {
        scannerMapPersisted = true;
        break;
      }
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert(scannerMapPersisted,
      `RackWire autosave must persist the operator-selected SoR interface-to-port mapping; last saved plan: ${JSON.stringify(mappedPlan)}`);
    assert(mappedPlan && Array.isArray(mappedPlan.devices), 'RackWire plan should remain persisted after port mapping');
    const persistedMappedDevice = mappedPlan.devices.find((device) => device.uid === scannerDevice.uid);
    assert(persistedMappedDevice && persistedMappedDevice.ifMap, 'the scanned device should retain its persisted port map');
    scannerDevice.ifMap = persistedMappedDevice.ifMap;
    assert.equal(scannerDevice.ifMap[scannerInterfaceName], scannerPortId,
      'the SoR interface name must persist against the reviewed RackWire port ID');
    const peerUid = `peer-${suffix}`;
    const cablePlan = {
      devices: [scannerDevice, { uid: peerUid, name: peerEntityName, assetId: peerEntityName, groups: [] }],
      cables: [{ uid: `cable-${suffix}`, a: { dev: scannerDevice.uid }, b: { dev: peerUid },
        standard: 'cat6', label: committedCableLabel, lengthM: 1.25 }],
      racks: [], notes: [], codeMap: {},
    };
    const cableCommit = await context.request.post(`${base}/api/rackwire/commit`, {
      data: { dryRun: false, plan: cablePlan },
      headers: { Origin: base, 'X-Solari-CSRF': profileHeaders['x-solari-csrf'] },
    });
    assert.equal(cableCommit.status(), 200, 'RackWire must commit a cable using the operator-linked scanner unit');
    const cableCommitData = (await cableCommit.json()).data;
    assert.equal(cableCommitData.created, 1, 'RackWire commit must create the integration cable');
    const storedCable = JSON.parse(sql(`SELECT JSON_ARRAY(id,a_entity_id,b_entity_id,a_interface_id,b_interface_id)
      FROM interconnects WHERE label=${quote(committedCableLabel)} AND deleted_at IS NULL`));
    assert.equal(Number(storedCable[1]), linkedEntityId,
      'scanner asset tag must resolve through the operator-reviewed SoR entity association');
    assert.equal(Number(storedCable[2]), peerEntityId,
      'the second cable endpoint must resolve to its canonical SoR entity');
    assert.equal(storedCable[3], null, 'a cable with no port specified must remain entity-level');
    assert.equal(storedCable[4], null, 'a cable with no port specified must retain its peer entity');

    const mappedCablePlan = {
      ...cablePlan,
      cables: [{ uid: `mapped-cable-${suffix}`, a: { dev: scannerDevice.uid, port: scannerPortId },
        b: { dev: peerUid }, standard: 'cat6', label: mappedCableLabel, lengthM: 1.25 }],
    };
    const mappedCableCommit = await context.request.post(`${base}/api/rackwire/commit`, {
      data: { dryRun: false, plan: mappedCablePlan },
      headers: { Origin: base, 'X-Solari-CSRF': profileHeaders['x-solari-csrf'] },
    });
    assert.equal(mappedCableCommit.status(), 200, 'RackWire must commit the explicitly mapped scanner interface');
    assert.equal((await mappedCableCommit.json()).data.created, 1, 'mapped-port cable commit must create one link');
    const mappedStoredCable = JSON.parse(sql(`SELECT JSON_ARRAY(id,a_interface_id,a_entity_id,b_interface_id,b_entity_id)
      FROM interconnects WHERE label=${quote(mappedCableLabel)} AND deleted_at IS NULL`));
    assert.equal(Number(mappedStoredCable[1]), scannerInterfaceId,
      'the scanner endpoint must resolve to the exact SoR interface selected by ifMap');
    assert.equal(mappedStoredCable[2], null, 'an interface endpoint must not duplicate its entity-level reference');
    assert.equal(mappedStoredCable[3], null, 'an unmapped peer endpoint must remain entity-level');
    assert.equal(Number(mappedStoredCable[4]), peerEntityId, 'the peer endpoint must retain its canonical SoR entity');

    const repeatedMappedCommit = await context.request.post(`${base}/api/rackwire/commit`, {
      data: { dryRun: false, plan: mappedCablePlan },
      headers: { Origin: base, 'X-Solari-CSRF': profileHeaders['x-solari-csrf'] },
    });
    assert.equal(repeatedMappedCommit.status(), 200, 'replaying an interface-level cable plan must remain safe');
    const replayResult = (await repeatedMappedCommit.json()).data;
    assert.equal(replayResult.created, 0, 'replaying the mapped cable must not create a duplicate');
    assert.equal(replayResult.updated, 1, 'replaying the mapped cable must update the existing interface-level link');

    const unknownPeerUid = `unknown-peer-${suffix}`;
    const unknownPortPlan = {
      ...cablePlan,
      devices: [...cablePlan.devices, { uid: unknownPeerUid, name: unknownPeerEntityName,
        assetId: unknownPeerEntityName, groups: [] }],
      cables: [{ uid: `unknown-port-${suffix}`, a: { dev: scannerDevice.uid, port: `missing-${suffix}` },
        b: { dev: unknownPeerUid }, standard: 'cat6', label: unknownPortCableLabel, lengthM: 1.25 }],
    };
    const unknownPortCommit = await context.request.post(`${base}/api/rackwire/commit`, {
      data: { dryRun: false, plan: unknownPortPlan },
      headers: { Origin: base, 'X-Solari-CSRF': profileHeaders['x-solari-csrf'] },
    });
    assert.equal(unknownPortCommit.status(), 200, 'an explicitly unmapped port must commit conservatively');
    assert.equal((await unknownPortCommit.json()).data.created, 1, 'unknown-port fallback must create one entity-level link');
    const unknownStoredCable = JSON.parse(sql(`SELECT JSON_ARRAY(id,a_interface_id,a_entity_id,b_interface_id,b_entity_id)
      FROM interconnects WHERE label=${quote(unknownPortCableLabel)} AND deleted_at IS NULL`));
    assert.equal(unknownStoredCable[1], null, 'an unknown port must not bind to a SoR interface');
    assert.equal(Number(unknownStoredCable[2]), linkedEntityId, 'an unknown port must retain the reviewed entity association');
    assert.equal(unknownStoredCable[3], null, 'the unmapped peer remains at entity level');
    assert.equal(Number(unknownStoredCable[4]), unknownPeerEntityId, 'the peer entity remains canonical');
    const repeatedUnknownCommit = await context.request.post(`${base}/api/rackwire/commit`, {
      data: { dryRun: false, plan: unknownPortPlan },
      headers: { Origin: base, 'X-Solari-CSRF': profileHeaders['x-solari-csrf'] },
    });
    assert.equal(repeatedUnknownCommit.status(), 200, 'replaying an unknown-port cable must remain safe');
    const unknownReplayResult = (await repeatedUnknownCommit.json()).data;
    assert.equal(unknownReplayResult.created, 0, 'replaying an unknown-port link must not create a duplicate');
    assert.equal(unknownReplayResult.updated, 1, 'replaying an unknown-port link must update its entity-level record');
    const finalEntityCable = JSON.parse(sql(`SELECT JSON_ARRAY(id,a_entity_id,b_entity_id,a_interface_id,b_interface_id)
      FROM interconnects WHERE label=${quote(committedCableLabel)} AND deleted_at IS NULL`));
    assert.deepEqual(finalEntityCable, storedCable, 'the later interface-level commits must not alter the original entity-level cable');
    assert.equal(Number(sql(`SELECT COUNT(*) FROM interconnects
      WHERE label IN (${quote(committedCableLabel)},${quote(mappedCableLabel)},${quote(unknownPortCableLabel)})
        AND deleted_at IS NULL`)), 3, 'missing-port, mapped-port, and unknown-port cases must remain three distinct links');

    // RackWire's embedded network-discovery action returns to the dashboard's
    // durable scan queue rather than calling the standalone RackWire backend.
    await page.getByRole('button', { name: 'Network discovery', exact: true }).click();
    await page.getByRole('heading', { name: 'Discovery', exact: true }).waitFor();
    await page.getByPlaceholder('192.168.1.0/24').fill('10.0.0.0/24');
    const activeStatusRequests = new Set();
    const statusPollEvents = [];
    let maxActiveStatusPolls = 0;
    let statusPollCount = 0;
    let failedStatusPollCount = 0;
    let delayStatusResponses = true;
    page.on('request', (request) => {
      if (!request.url().includes('/api/discovery/scans/941')) return;
      statusPollCount += 1;
      activeStatusRequests.add(request);
      maxActiveStatusPolls = Math.max(maxActiveStatusPolls, activeStatusRequests.size);
      statusPollEvents.push(`start ${Date.now()} active=${activeStatusRequests.size}`);
    });
    function finishStatusRequest(request) {
      if (activeStatusRequests.delete(request)) statusPollEvents.push(`finish ${Date.now()} active=${activeStatusRequests.size}`);
    }
    page.on('requestfinished', finishStatusRequest);
    page.on('requestfailed', (request) => {
      if (activeStatusRequests.has(request)) failedStatusPollCount += 1;
      finishStatusRequest(request);
    });
    await page.route('**/api/discovery/scans/941', async (route) => {
      // Hold the response longer than Discovery's one-second interval to
      // prove slow status reads never overlap or race their own results.
      if (delayStatusResponses) {
        const aborted = new Promise((resolve) => route.request().once('requestfailed', () => resolve(true)));
        const delayed = new Promise((resolve) => setTimeout(() => resolve(false), 1500));
        if (await Promise.race([aborted, delayed])) return;
      }
      if (route.request().failure()) return;
      try {
        const response = await route.fetch();
        await route.fulfill({ response });
      } catch (error) {
        if (!route.request().failure() && !page.isClosed()) throw error;
      }
    });
    const pendingAdmissionResponse = page.waitForResponse((response) =>
      response.url().includes('/api/discovery/scan-requests/940') && response.request().method() === 'GET');
    const scanResponse = page.waitForResponse((response) =>
      response.url().includes('/api/discovery/scan') && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Scan', exact: true }).click();
    const scanAccepted = await scanResponse;
    assert.equal(scanAccepted.status(), 202, 'the embedded RackWire path must queue a bounded dashboard scan');
    const scanHeaders = await scanAccepted.request().allHeaders();
    assert(typeof scanHeaders['x-solari-csrf'] === 'string' && scanHeaders['x-solari-csrf'].length > 20,
      'network scan admission must carry the dashboard CSRF token');
    const pendingAdmission = await pendingAdmissionResponse;
    assert.equal((await pendingAdmission.json()).data.state, 'pending',
      'the UI must poll and retain a pending durable admission request');
    try {
      await page.waitForFunction(() => {
        const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
        const status = document.querySelector('.page-sub[role="status"]');
        return job && job.requestId === 940 && job.status === 'admission_pending'
          && status && status.textContent.includes('Waiting for scan admission');
      }, null, { timeout: 5000 });
    } catch (error) {
      const state = await page.evaluate(() => ({
        scan: sessionStorage.getItem('solari.activeScan'),
        status: document.querySelector('.page-sub[role="status"]')?.textContent || null,
        toast: document.querySelector('[role="alert"]')?.textContent || null,
      }));
      throw new Error(`${error.message}; scan admission UI state: ${JSON.stringify(state)}`);
    }
    assert(await page.getByRole('button', { name: 'Scan active…', exact: true }).isDisabled(),
      'the scan action must stay disabled while admission is pending');
    await context.request.get(`${base}/__finish_admission`);
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      const status = document.querySelector('.page-sub[role="status"]');
      return job && job.scanId === 941 && job.status === 'running' && status
        && status.textContent.includes('4/16 hosts checked') && status.textContent.includes('0 candidates saved');
    });
    assert(await page.getByRole('button', { name: 'Scan active…', exact: true }).isDisabled(),
      'the scan action must stay disabled while the worker is running');
    const pollCountBeforeNavigation = statusPollCount;
    for (let attempt = 0; attempt < 50 && activeStatusRequests.size === 0; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert(activeStatusRequests.size > 0, 'the slow status poll must be active before navigating away');
    await page.locator('.nav-item').filter({ hasText: /^Inventory$/ }).click();
    await page.getByRole('heading', { name: 'Inventory', exact: true }).waitFor();
    for (let attempt = 0; attempt < 30 && activeStatusRequests.size > 0; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert.equal(activeStatusRequests.size, 0, 'leaving Discovery must abort its in-flight status request');
    assert(failedStatusPollCount > 0, 'navigation must cancel the browser status request');
    await page.locator('.nav-item').filter({ hasText: /^Discovery$/ }).click();
    await page.getByRole('heading', { name: 'Discovery', exact: true }).waitFor();
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      return job && job.scanId === 941 && job.status === 'running';
    });
    for (let attempt = 0; attempt < 110 && (statusPollCount < 2 || activeStatusRequests.size > 0); attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert(statusPollCount > pollCountBeforeNavigation,
      'returning to Discovery must resume status polling for the active scan');
    for (let attempt = 0; attempt < 50 && activeStatusRequests.size > 0; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
    assert.equal(maxActiveStatusPolls, 1,
      `slow Discovery status requests must never overlap; events: ${statusPollEvents.join(' | ')}`);
    assert.equal(activeStatusRequests.size, 0, 'slow scan status request must settle before navigation');
    delayStatusResponses = false;
    await page.unroute('**/api/discovery/scans/941');

    // Durable scan state must recover from the server if browser session state
    // disappears, rather than requiring the original tab storage to survive.
    await page.evaluate(() => sessionStorage.removeItem('solari.activeScan'));
    await page.reload();
    await page.locator('.nav-item').filter({ hasText: /^Discovery$/ }).click();
    await page.getByRole('heading', { name: 'Discovery', exact: true }).waitFor();
    try {
      await page.getByRole('status').filter({ hasText: 'Resumed saved scan' }).waitFor({ timeout: 5000 });
    } catch (error) {
      throw new Error(`Server scan recovery did not render: ${error.message}\n` +
        `stored job: ${await page.evaluate(() => sessionStorage.getItem('solari.activeScan'))}\n` +
        `scan-jobs responses: ${scanJobsResponses.join(' | ')}\n` +
        `page errors: ${pageErrors.join(' | ')}\nconsole: ${browserConsole.join(' | ')}`);
    }
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      return job && job.scanId === 941 && job.recoveredFromServer === true && job.status === 'running';
    });
    assert(await page.getByRole('button', { name: 'Scan active…', exact: true }).isDisabled(),
      'server-recovered scan must keep duplicate admission disabled');

    // The progress card is component state backed by sessionStorage. Leaving
    // Discovery and returning should restore the same job and resume polling.
    await page.locator('.nav-item').filter({ hasText: /^Inventory$/ }).click();
    await page.getByRole('heading', { name: 'Inventory', exact: true }).waitFor();
    await page.locator('.nav-item').filter({ hasText: /^Discovery$/ }).click();
    await page.getByRole('heading', { name: 'Discovery', exact: true }).waitFor();
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      const status = document.querySelector('.page-sub[role="status"]');
      return job && job.scanId === 941 && job.status === 'running' && status
        && status.textContent.includes('4/16 hosts checked');
    });
    await context.request.get(`${base}/__finish_sweep`);
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      const status = document.querySelector('.page-sub[role="status"]');
      return job && job.status === 'running' && job.hostsDone === 16 && job.hostsFound === 0
        && status && status.textContent.includes('Host sweep complete')
        && status.textContent.includes('0 candidates saved');
    });
    await context.request.get(`${base}/__finish_candidate`);
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      const status = document.querySelector('.page-sub[role="status"]');
      return job && job.status === 'running' && job.hostsFound === 1
        && status && status.textContent.includes('1 candidate saved');
    });
    await page.getByText('live-scan-fixture', { exact: true }).waitFor({ timeout: 5000 });
    assert.equal(await page.locator('.page-head').getByText('Last scan completed', { exact: false }).count(), 0,
      'a candidate must appear in Discovery before the scan reaches its terminal state');
    await context.request.get(`${base}/__finish_scan`);
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      const status = document.querySelector('.page-sub[role="status"]');
      return job && job.status === 'completed' && status
        && status.textContent.includes('Last scan completed');
    });
    assert.equal(activeStatusRequests.size, 0, 'all scan status requests must settle when the scan completes');

    assert.deepEqual(pageErrors, [], 'browser reported no JavaScript errors');
    assert.deepEqual(externalRequests, [], 'browser made no external requests');
    console.log('Dashboard integration browser E2E passed: scanner import, operator-reviewed SoR entity association, RackWire profile creation, barcode resolution, UI port mapping and interface-level cable commit with replay, missing/unknown-port entity fallback, plan persistence, and Discovery scan lifecycle.');
  } catch (error) {
    failure = error;
  } finally {
    if (browser) await browser.close();
    if (child && child.exitCode === null) child.kill('SIGTERM');
    if (child) await new Promise((resolve) => child.once('exit', resolve));
    try {
      const fixtureEntityIds = [linkedEntityId, peerEntityId, unknownPeerEntityId]
        .filter((id) => Number.isSafeInteger(id) && id > 0);
      const entityCleanup = fixtureEntityIds.length
        ? `DELETE FROM entities WHERE id IN (${fixtureEntityIds.join(',')});`
        : '';
      const cleanup = `SET @source_id=(SELECT id FROM sources WHERE slug='scanner-ingest' LIMIT 1);
        DELETE FROM audit_log WHERE actor_label=${quote(actor)};
        DELETE FROM interconnects WHERE label IN (${quote(committedCableLabel)},${quote(mappedCableLabel)},${quote(unknownPortCableLabel)});
        ${entityCleanup}
        DELETE FROM barcodes WHERE subject_table='hardware_units' AND subject_id IN (SELECT subject_id FROM external_refs WHERE source_id=@source_id AND subject_table='hardware_units' AND external_id IN (${assetIds.map(quote).join(',')}));
        DELETE FROM hardware_units WHERE id IN (SELECT subject_id FROM external_refs WHERE source_id=@source_id AND subject_table='hardware_units' AND external_id IN (${assetIds.map(quote).join(',')}));
        DELETE FROM hardware_models WHERE id IN (SELECT subject_id FROM external_refs WHERE source_id=@source_id AND subject_table='hardware_models' AND external_id IN (${itemIds.map(quote).join(',')}));
        DELETE FROM external_refs WHERE source_id=@source_id AND external_id IN (${[...itemIds, ...assetIds].map(quote).join(',')});
        DELETE FROM locations WHERE id IN (${locationIds.map(Number).join(',') || '0'});
        ${rackwirePlanId ? `DELETE FROM rw_plan_versions WHERE plan_id=${rackwirePlanId}; DELETE FROM rw_plans WHERE id=${rackwirePlanId};` : ''}
        ${sourceExistedBefore ? '' : "DELETE FROM sources WHERE id=@source_id AND NOT EXISTS (SELECT 1 FROM external_refs WHERE source_id=@source_id) AND NOT EXISTS (SELECT 1 FROM hardware_models WHERE source_id=@source_id) AND NOT EXISTS (SELECT 1 FROM hardware_units WHERE source_id=@source_id);"}`;
      sql(cleanup);
    } catch (cleanupError) {
      failure = failure || cleanupError;
    }
    fs.rmSync(temp, { recursive: true, force: true });
  }
  if (failure) {
    // Include only the local PHP diagnostic tail; never print environment/config values.
    failure.message += browserDiagnostics;
    throw failure;
  }
}

main().catch((error) => {
  console.error(error && error.message ? error.message : String(error));
  if (error && error.stack) console.error(error.stack);
  process.exitCode = 1;
});
