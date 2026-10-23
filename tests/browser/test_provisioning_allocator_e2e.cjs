#!/usr/bin/env node
'use strict';

// Browser acceptance for the SoR-backed static-address review surface. It uses
// the real dashboard bundle, PHP session/auth code, and a write-counted API
// fixture; no production service or database is contacted.
const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const crypto = require('node:crypto');
const { chromium } = require('playwright');

const ROOT = path.resolve(__dirname, '../..');
const PUBLIC = path.join(ROOT, 'dashboard/public');
const AUTH_LIB = path.join(ROOT, 'dashboard/api/lib/Auth.php');

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
    if (child.exitCode !== null) throw new Error(`PHP fixture exited early (${child.exitCode}).`);
    try {
      const response = await fetch(url, { signal: AbortSignal.timeout(500) });
      if (response.ok) return;
    } catch (_) { /* wait for the local PHP listener */ }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error('Provisioning allocator fixture did not become ready.');
}

function phpRouter(stateFile) {
  return `<?php
require_once ${JSON.stringify(AUTH_LIB)};
function allocator_fixture_state($path, $write = null) {
    $fp = fopen($path, 'c+');
    if ($fp === false) { http_response_code(500); exit; }
    flock($fp, LOCK_EX);
    $raw = stream_get_contents($fp);
    $state = is_string($raw) && $raw !== '' ? json_decode($raw, true) : [];
    if (!is_array($state)) $state = [];
    if ($write !== null) {
        $state = $write($state);
        rewind($fp); ftruncate($fp, 0);
        fwrite($fp, json_encode($state)); fflush($fp);
    }
    flock($fp, LOCK_UN); fclose($fp);
    return $state;
}
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
if ($path === '/__health') { echo 'ok'; exit; }
if ($path === '/__fixture_session') {
    $user = (string)($_GET['user'] ?? '');
    if (!preg_match('/\\A[a-z0-9-]{1,64}\\z/', $user)) { http_response_code(400); exit; }
    Auth::establishSession(['username' => $user, 'role' => 'operator', 'source' => 'oidc']);
    header('Content-Type: application/json'); echo json_encode(['ready' => true]); exit;
}
if (is_string($path) && str_starts_with($path, '/api/')) {
    Auth::bootSession();
    $principal = Auth::current();
    if ($principal === null) { http_response_code(401); header('Content-Type: application/json'); echo json_encode(['ok'=>false,'error'=>['code'=>'unauthorized','message'=>'Not authenticated']]); exit; }
    $statePath = ${JSON.stringify(stateFile)};
    if ($path === '/api/networks/allocator-eligibility' && $_SERVER['REQUEST_METHOD'] === 'GET') {
        $state = allocator_fixture_state($statePath);
        header('Content-Type: application/json'); header('Cache-Control: no-store');
        echo json_encode(['ok'=>true,'data'=>$state['networks'],'ts'=>gmdate('c')]); exit;
    }
    if (preg_match('~\\A/api/networks/(\\d+)/allocator-eligibility\\z~', (string)$path, $match)
        && $_SERVER['REQUEST_METHOD'] === 'POST') {
        $body = json_decode(file_get_contents('php://input') ?: '', true);
        if (!is_array($body) || !isset($body['eligible']) || !is_bool($body['eligible']) || ($body['confirm'] ?? false) !== true) {
            http_response_code(400); header('Content-Type: application/json'); echo json_encode(['ok'=>false]); exit;
        }
        $id = (int)$match[1];
        $state = allocator_fixture_state($statePath, static function ($state) use ($id, $body, $principal) {
            foreach ($state['networks'] as &$network) {
                if ((int)$network['id'] === $id) {
                    $network['allocator_eligible'] = $body['eligible'];
                    $network['allocator_approved_by'] = $body['eligible'] ? $principal['username'] : null;
                    $network['allocator_approved_at'] = $body['eligible'] ? '2026-10-22 12:00:00' : null;
                }
            }
            unset($network);
            $state['writes'] = ($state['writes'] ?? 0) + 1;
            return $state;
        });
        header('Content-Type: application/json'); header('Cache-Control: no-store');
        echo json_encode(['ok'=>true,'data'=>['id'=>$id,'allocator_eligible'=>$body['eligible']],'ts'=>gmdate('c')]); exit;
    }
    $data = match ($path) {
        '/api/summary' => ['counts'=>[], 'lease'=>[]],
        '/api/config' => [],
        '/api/auth/whoami' => ['operator'=>$principal['username'],'role'=>$principal['role'],'displayName'=>$principal['username'],'source'=>'oidc','csrfToken'=>Auth::csrfToken()],
        default => [],
    };
    header('Content-Type: application/json'); header('Cache-Control: no-store');
    echo json_encode(['ok'=>true,'data'=>$data,'ts'=>gmdate('c')]); exit;
}
return false;
?>`;
}

function fixtureNetworks() {
  return [
    {
      id: 1, name: 'Default', purpose: 'Infrastructure', enabled: true, is_default: true,
      allocator_eligible: false, allocator_approved_by: null, allocator_approved_at: null,
      vlan_tag: 1, vlan_name: 'Default', ipv4_subnet_count: 1, overlapping_networks: [],
      subnets: [{ id: 101, cidr: '10.0.0.0/8', gateway_ip: '10.0.0.1', dhcp_pools: '10.1.0.26–10.7.161.117' }],
    },
    {
      id: 2, name: 'Lab isolated', purpose: 'Test equipment', enabled: true, is_default: false,
      allocator_eligible: true, allocator_approved_by: 'jason', allocator_approved_at: '2026-10-21 18:00:00',
      vlan_tag: 42, vlan_name: 'Lab', ipv4_subnet_count: 1, overlapping_networks: [],
      subnets: [{ id: 102, cidr: '10.42.0.0/24', gateway_ip: '10.42.0.1', dhcp_pools: '10.42.0.100–10.42.0.199' }],
    },
    {
      id: 3, name: 'Staging incomplete', purpose: 'Staging', enabled: true, is_default: false,
      allocator_eligible: false, allocator_approved_by: null, allocator_approved_at: null,
      vlan_tag: 73, vlan_name: 'Staging', ipv4_subnet_count: 0, overlapping_networks: [], subnets: [],
    },
    {
      id: 4, name: 'Overlap lab', purpose: 'Needs reconciliation', enabled: true, is_default: false,
      allocator_eligible: false, allocator_approved_by: null, allocator_approved_at: null,
      vlan_tag: 4094, vlan_name: 'Quarantine', ipv4_subnet_count: 1, overlapping_networks: ['Default (10.0.0.0/8)'],
      subnets: [{ id: 104, cidr: '10.255.0.0/16', gateway_ip: '10.255.0.1', dhcp_pools: '' }],
    },
  ];
}

async function main() {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'solarinet-provisioning-browser-'));
  const sessions = path.join(temp, 'sessions');
  const stateFile = path.join(temp, 'state.json');
  fs.mkdirSync(sessions, { mode: 0o700 });
  fs.writeFileSync(stateFile, JSON.stringify({ networks: fixtureNetworks(), writes: 0 }), { mode: 0o600 });
  const routerPath = path.join(temp, 'router.php');
  fs.writeFileSync(routerPath, phpRouter(stateFile), { mode: 0o600 });

  let child = null;
  let browser = null;
  let failure = null;
  let phpErrors = '';
  try {
    const port = await freePort();
    const base = `http://127.0.0.1:${port}`;
    child = spawn('php', ['-d', `session.save_path=${sessions}`, '-S', `127.0.0.1:${port}`,
      '-t', PUBLIC, routerPath], { cwd: ROOT, stdio: ['ignore', 'ignore', 'pipe'] });
    child.stderr.on('data', (data) => { phpErrors = (phpErrors + data.toString()).slice(-4000); });
    await waitForServer(`${base}/__health`, child);

    browser = await chromium.launch({
      headless: true,
      args: ['--no-sandbox', '--disable-dev-shm-usage'],
      ...(process.env.SOLARI_CHROMIUM_EXECUTABLE ? { executablePath: process.env.SOLARI_CHROMIUM_EXECUTABLE } : {}),
    });
    const page = await browser.newPage({ viewport: { width: 1440, height: 1100 } });
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(error.message));
    page.on('console', (message) => { if (message.type() === 'error') pageErrors.push(message.text()); });
    const user = `allocator-${crypto.randomBytes(4).toString('hex')}`;
    await page.goto(`${base}/__fixture_session?user=${encodeURIComponent(user)}`, { waitUntil: 'domcontentloaded' });
    await page.goto(`${base}/`, { waitUntil: 'load' });
    await page.locator('.nav-item').filter({ hasText: /^Fleet Overview$/ }).waitFor({ timeout: 15000 });
    const serverStatus = page.locator('.topbar .statuschip');
    assert.equal(await serverStatus.getAttribute('role'), 'status');
    assert.match(await serverStatus.getAttribute('aria-label'), /^Active control server:/,
      'compact server indicator keeps its full status accessible');
    await page.setViewportSize({ width: 390, height: 844 });
    await page.locator('.nav-item').filter({ hasText: /^Provisioning$/ }).click();

    await page.getByRole('heading', { name: 'Static address allocation' }).waitFor();
    const search = page.getByRole('searchbox', {
      name: 'Search networks by name, VLAN, subnet, gateway, or DHCP pool',
    });
    await search.waitFor();
    const summary = page.getByRole('status').filter({ hasText: 'approved' });
    await page.locator('.allocator-counts').waitFor();
    assert.match(await summary.innerText(), /1 approved[\s\S]*1 ready[\s\S]*2 blocked[\s\S]*4 of 4/,
      'status summary groups approved, ready, blocked, and total counts');
    assert.equal(await page.getByText('10.0.0.0/8', { exact: true }).count(), 1,
      'network row shows the subnet');
    assert.equal(await page.getByText('gateway 10.0.0.1', { exact: false }).count(), 1,
      'network row shows its gateway');
    assert.equal(await page.getByText('DHCP 10.1.0.26–10.7.161.117', { exact: true }).count(), 1,
      'network row preserves the controller DHCP range');
    const overlapRow = page.locator('.enr-row').filter({ hasText: 'Overlap lab' });
    await overlapRow.getByText(/Prefix overlap:/).waitFor();
    assert.equal(await overlapRow.getByRole('button', { name: 'Approve' }).isDisabled(), true,
      'overlapping network cannot be approved');
    const incompleteRow = page.locator('.enr-row').filter({ hasText: 'Staging incomplete' });
    await incompleteRow.getByText('Approval requires exactly one live IPv4 subnet; this network has 0.').waitFor();
    assert.equal(await incompleteRow.getByRole('button', { name: 'Approve' }).isDisabled(), true,
      'network without a live IPv4 subnet cannot be approved');
    if (process.env.SOLARI_PROVISIONING_SCREENSHOT) {
      await page.screenshot({ path: process.env.SOLARI_PROVISIONING_SCREENSHOT, fullPage: true });
    }

    await search.fill('10.0.0.0/8');
    await page.getByText('1 of 4', { exact: true }).waitFor();
    assert.equal(await page.getByText('Overlap lab', { exact: true }).count(), 0,
      'prefix search filters unrelated networks');
    await search.fill('4094');
    const vlanMatch = page.locator('.enr-row').filter({ hasText: 'Overlap lab' });
    await vlanMatch.waitFor();
    await search.fill('no-such-network');
    await page.getByText('No networks match “no-such-network”.').waitFor();
    await search.clear();

    const defaultRow = page.locator('.enr-row').filter({ hasText: /^Default\b/ });
    page.once('dialog', (dialog) => {
      assert.match(dialog.message(), /Approve Default for static IP allocation/);
      dialog.accept();
    });
    await defaultRow.getByRole('button', { name: 'Approve' }).click();
    await defaultRow.getByRole('button', { name: 'Revoke' }).waitFor();
    assert.match(await page.getByRole('status').filter({ hasText: 'approved' }).innerText(), /2 approved/,
      'approval refreshes the state and count');
    await defaultRow.getByText('Approved by ' + user).waitFor();

    page.once('dialog', (dialog) => dialog.dismiss());
    await defaultRow.getByRole('button', { name: 'Revoke' }).click();
    await defaultRow.getByRole('button', { name: 'Revoke' }).waitFor();
    assert.match(await page.getByRole('status').filter({ hasText: 'approved' }).innerText(), /2 approved/,
      'cancelled revocation leaves the approved state unchanged');

    page.once('dialog', (dialog) => dialog.accept());
    await defaultRow.getByRole('button', { name: 'Revoke' }).click();
    await defaultRow.getByRole('button', { name: 'Approve' }).waitFor();
    assert.match(await page.getByRole('status').filter({ hasText: 'approved' }).innerText(), /1 approved/,
      'confirmed revocation refreshes the state and count');
    assert.equal(JSON.parse(fs.readFileSync(stateFile, 'utf8')).writes, 2,
      'cancelled action did not send a write request');

    const mobile = await page.evaluate(() => ({
      width: innerWidth,
      documentWidth: document.documentElement.scrollWidth,
      bodyWidth: document.body.scrollWidth,
      contentWidth: document.querySelector('.content').clientWidth,
      contentScrollWidth: document.querySelector('.content').scrollWidth,
      topbarWidth: document.querySelector('.topbar').clientWidth,
      topbarScrollWidth: document.querySelector('.topbar').scrollWidth,
      overflowing: Array.from(document.body.querySelectorAll('*')).map((element) => {
        const rect = element.getBoundingClientRect();
        return { tag: element.tagName, className: String(element.className || '').slice(0, 90),
          left: Math.round(rect.left), right: Math.round(rect.right), width: Math.round(rect.width) };
      }).filter((item) => item.right > innerWidth + 1 || item.left < -1)
        .sort((a, b) => b.right - a.right).slice(0, 12),
    }));
    assert.equal(mobile.documentWidth, mobile.width, 'mobile provisioning page has no horizontal overflow');
    assert.equal(mobile.bodyWidth, mobile.width,
      `mobile body has no hidden horizontal overflow: ${JSON.stringify(mobile.overflowing)}`);
    assert.equal(mobile.topbarScrollWidth, mobile.topbarWidth,
      `mobile topbar keeps every action in its viewport: ${JSON.stringify(mobile)}`);
    const topbarControls = await page.locator('.topbar .iconbtn, .topbar .profile-chip, .topbar .search, .topbar .statuschip')
      .evaluateAll((elements) => elements.map((element) => {
        const rect = element.getBoundingClientRect();
        return { label: element.getAttribute('aria-label') || element.className,
          left: Math.round(rect.left), right: Math.round(rect.right), width: Math.round(rect.width) };
      }));
    assert(topbarControls.every((control) => control.left >= 0 && control.right <= mobile.width),
      `every topbar control is visible on mobile: ${JSON.stringify(topbarControls)}`);
    assert(topbarControls.every((control) => control.width >= (control.label.startsWith('Active control server:') ? 20 : 40)),
      `mobile keeps its search and touch controls usable: ${JSON.stringify(topbarControls)}`);
    assert.equal(mobile.contentScrollWidth, mobile.contentWidth,
      `mobile content has no clipped horizontal overflow: ${JSON.stringify(mobile)}`);
    assert(await defaultRow.getByRole('button', { name: 'Approve' }).isVisible(),
      'network actions remain visible on mobile');
    await page.setViewportSize({ width: 320, height: 720 });
    assert.equal(await page.evaluate(() => document.body.scrollWidth), 320,
      'compact phone layout has no hidden toolbar overflow');
    const compactControls = await page.locator('.topbar .iconbtn, .topbar .profile-chip, .topbar .search, .topbar .statuschip')
      .evaluateAll((elements) => elements.map((element) => {
        const rect = element.getBoundingClientRect();
        return { label: element.getAttribute('aria-label') || element.className,
          left: Math.round(rect.left), right: Math.round(rect.right), width: Math.round(rect.width) };
      }));
    assert(compactControls.every((control) => control.left >= 0 && control.right <= 320
      && control.width >= (control.label.startsWith('Active control server:') ? 20 : 40)),
    `compact phone keeps its search and touch controls usable: ${JSON.stringify(compactControls)}`);

    await page.setViewportSize({ width: 1440, height: 1000 });
    await page.reload({ waitUntil: 'load' });
    await page.locator('.nav-item').filter({ hasText: /^Fleet Overview$/ }).waitFor({ timeout: 15000 });
    await page.locator('.nav-item').filter({ hasText: /^Provisioning$/ }).click();
    await page.getByRole('heading', { name: 'Static address allocation' }).waitFor();
    assert.equal(await page.evaluate(() => document.body.scrollWidth), 1440,
      'desktop provisioning page fits its viewport');
    if (process.env.SOLARI_PROVISIONING_DESKTOP_SCREENSHOT) {
      await page.screenshot({ path: process.env.SOLARI_PROVISIONING_DESKTOP_SCREENSHOT, fullPage: true });
    }
    assert.deepEqual(pageErrors, [], 'browser reported no JavaScript or console errors');
    console.log('Provisioning browser acceptance passed: status summary, SoR facts, blocked reasons, search, confirmed/cancelled approval, and mobile layout.');
  } catch (error) {
    failure = error;
  } finally {
    if (browser) await browser.close();
    if (child && child.exitCode === null) child.kill('SIGTERM');
    if (child) await new Promise((resolve) => child.once('exit', resolve));
    fs.rmSync(temp, { recursive: true, force: true });
  }
  if (failure) {
    failure.message += phpErrors ? `\nPHP fixture diagnostics:\n${phpErrors}` : '';
    throw failure;
  }
}

main().catch((error) => {
  console.error(error && error.message ? error.message : String(error));
  if (error && error.stack) console.error(error.stack);
  process.exitCode = 1;
});
