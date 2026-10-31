#!/usr/bin/env node
'use strict';

// Browser acceptance for Images & Stacks (Provisioning P4). It drives the real
// dashboard bundle and PHP session/auth code against a write-recording
// /api/provisioning fixture; no production service, socket, or database is
// contacted.
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
  throw new Error('Images & Stacks fixture did not become ready.');
}

// The fixture answers the /api/provisioning routes the screen reads from a JSON
// state file and records every write (method, path, body, CSRF + idempotency
// headers) so the test can assert exactly what the browser sent.
function phpRouter(stateFile) {
  return `<?php
require_once ${JSON.stringify(AUTH_LIB)};
function fx_state($path, $write = null) {
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
function fx_out($data, $code = 200) {
    http_response_code($code);
    header('Content-Type: application/json'); header('Cache-Control: no-store');
    echo json_encode($code < 400 ? ['ok'=>true,'data'=>$data,'ts'=>gmdate('c')]
        : ['ok'=>false,'error'=>['code'=>'fixture','message'=>(string)$data]]);
    exit;
}
$path = (string) parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
if ($path === '/__health') { echo 'ok'; exit; }
if ($path === '/__fixture_session') {
    $user = (string)($_GET['user'] ?? '');
    if (!preg_match('/\\A[a-z0-9-]{1,64}\\z/', $user)) { http_response_code(400); exit; }
    Auth::establishSession(['username' => $user, 'role' => 'operator', 'source' => 'test']);
    header('Content-Type: application/json'); echo json_encode(['ready' => true]); exit;
}
if (str_starts_with($path, '/api/')) {
    Auth::bootSession();
    $principal = Auth::current();
    if ($principal === null) fx_out('Not authenticated', 401);
    $statePath = ${JSON.stringify(stateFile)};
    $P = '/api/provisioning/';
    if (str_starts_with($path, $P)) {
        $rest = substr($path, strlen($P));
        if ($method === 'GET') {
            $s = fx_state($statePath);
            if ($rest === 'images') fx_out(array_values($s['images']));
            if ($rest === 'software') fx_out(array_values($s['software']));
            if ($rest === 'targets') fx_out($s['targets']);
            if ($rest === 'runs') fx_out(array_reverse($s['runs']));
            if ($rest === 'log') {
                $k = (string)($_GET['kind'] ?? '') . '-' . (string)($_GET['id'] ?? '');
                $log = $s['logs'][$k] ?? null;
                fx_out(['kind'=>$_GET['kind'] ?? '', 'id'=>(int)($_GET['id'] ?? 0), 'exists'=>$log !== null,
                    'log'=>$log ?? '', 'truncated'=>false, 'size'=>strlen($log ?? ''),
                    'updated_at'=>$log === null ? null : '2026-10-31T12:00:00Z']);
            }
            if ($rest === 'stacks') {
                fx_out(array_values(array_map(static function ($st) {
                    $o = $st; unset($o['members']);
                    $o['member_count'] = count($st['members']);
                    return $o;
                }, $s['stacks'])));
            }
            if (preg_match('~\\Astacks/(\\d+)\\z~', $rest, $m) && isset($s['stacks'][$m[1]])) {
                $st = $s['stacks'][$m[1]];
                $st['members'] = array_map(static function ($swId, $i) use ($s) {
                    $sw = $s['software'][(string)$swId];
                    return ['software_id'=>$swId, 'logical_name'=>$sw['logical_name'], 'item_type'=>$sw['item_type'],
                        'item_order'=>null, 'effective_order'=>($i + 1) * 100, 'sort_order'=>100, 'notes'=>null];
                }, $st['members'], array_keys($st['members']));
                $st['depends_on'] = array_values(array_map(static fn ($slug) => ['stack_id'=>0, 'slug'=>$slug], $st['depends_on']));
                $st['depended_on_by'] = [];
                fx_out($st);
            }
            fx_out('no such fixture route', 404);
        }
        $body = json_decode(file_get_contents('php://input') ?: '', true);
        $rec = ['method'=>$method, 'path'=>$rest, 'body'=>$body,
            'csrf'=>($_SERVER['HTTP_X_SOLARI_CSRF'] ?? '') === Auth::csrfToken(),
            'idem'=>(string)($_SERVER['HTTP_IDEMPOTENCY_KEY'] ?? '')];
        $s = fx_state($statePath, static function ($s) use ($rec, $principal) {
            $s['writes'][] = $rec;
            if ($rec['method'] === 'POST' && in_array($rec['path'], ['stage', 'apply'], true)) {
                $s['runs'][] = ['id'=>(string)(count($s['runs']) + 20), 'mode'=>$rec['path'] === 'stage' ? 'reimage' : 'apply',
                    'entity_id'=>(string)$rec['body']['entity'], 'entity_name'=>'neon',
                    'image_id'=>$rec['body']['image'] ?? null, 'image_label'=>isset($rec['body']['image']) ? 'debian 13 arm64' : null,
                    'stack_slugs'=>$rec['body']['stacks'], 'requested_by'=>$principal['username'], 'status'=>'queued',
                    'dry_run'=>(bool)($rec['body']['dry_run'] ?? false), 'detail'=>null,
                    'requested_at'=>gmdate('Y-m-d\\TH:i:s\\Z'), 'started_at'=>null, 'finished_at'=>null];
            }
            if ($rec['method'] === 'POST' && $rec['path'] === 'image-refresh') {
                $s['runs'][] = ['id'=>(string)(count($s['runs']) + 20), 'mode'=>'refresh', 'entity_id'=>null, 'entity_name'=>null,
                    'image_id'=>(string)$rec['body']['image'], 'image_label'=>'debian 12 arm64', 'stack_slugs'=>[],
                    'requested_by'=>$principal['username'], 'status'=>'running', 'dry_run'=>false, 'detail'=>null,
                    'requested_at'=>gmdate('Y-m-d\\TH:i:s\\Z'), 'started_at'=>null, 'finished_at'=>null];
            }
            if ($rec['method'] === 'DELETE' && preg_match('~\\Astacks/(\\d+)\\z~', $rec['path'], $m)) {
                unset($s['stacks'][$m[1]]);
            }
            return $s;
        });
        if ($method === 'DELETE') fx_out(['deleted'=>true]);
        fx_out(['status'=>'queued', 'request'=>(string)(100 + count($s['writes'])), 'log'=>null]);
    }
    if ($method === 'PUT' && preg_match('~\\A/api/inventory/models/(\\d+)\\z~', $path, $m)) {
        $body = json_decode(file_get_contents('php://input') ?: '', true);
        $id = (int) $m[1];
        fx_state($statePath, static function ($s) use ($id, $body) {
            $s['writes'][] = ['method'=>'PUT', 'path'=>'inventory/models/' . $id, 'body'=>$body,
                'csrf'=>($_SERVER['HTTP_X_SOLARI_CSRF'] ?? '') === Auth::csrfToken(), 'idem'=>''];
            foreach ($s['targets'] as &$t) {
                if (($t['hw_model_id'] ?? null) === $id) {
                    $t['arch'] = $body['arch'] ?: null;
                    $t['arch_source'] = $body['arch'] ? 'hardware_model' : null;
                }
            }
            unset($t);
            return $s;
        });
        fx_out(['id'=>$id, 'arch'=>$body['arch'] ?: null]);
    }
    $data = match ($path) {
        '/api/summary' => ['counts'=>[], 'lease'=>[]],
        '/api/config' => [],
        '/api/auth/whoami' => ['operator'=>$principal['username'],'role'=>$principal['role'],'displayName'=>$principal['username'],'source'=>$principal['source'],'csrfToken'=>Auth::csrfToken()],
        default => [],
    };
    fx_out($data);
}
return false;
?>`;
}

function image(id, release, label, arch, boot, fetched, extra = {}) {
  return Object.assign({
    id, distro: 'debian', family: 'debian', release_name: release, release_label: label, arch, boot_method: boot,
    source_url: 'https://deb.example/netboot/', checksum_url: null, version: fetched ? release + '.1' : null,
    sha256: fetched ? 'ab'.repeat(32) : null, fetched_at: fetched ? new Date(Date.now() - 2 * 86400000).toISOString() : null,
    fetched, install_template: 'debian.preseed.tmpl', refresh_policy: 'point_release', netboot_verified: false,
    notes: null, updated_at: '2026-10-30T12:00:00Z',
  }, extra);
}

function sw(id, logical, type, names, extra = {}) {
  return Object.assign({
    id, logical_name: logical, item_type: type, name_suse: names[0], name_debian: names[1], name_fedora: names[2],
    applies_families: null, applies_arches: null, ansible_role: null, source_url: null, version_pin: null,
    spec: null, notes: null, updated_at: '2026-10-30T12:00:00Z',
  }, extra);
}

function fixtureState() {
  return {
    writes: [],
    images: [
      image(4, '13', '13 (trixie)', 'x86_64', 'ipxe', true),
      image(5, '13', '13 (trixie)', 'arm64', 'ipxe', true),
      image(6, '12', '12 (bookworm)', 'arm64', 'ipxe', false),
      image(7, '12', '12 (bookworm)', 'armhf', 'pi_netboot', true, { distro: 'raspios' }),
    ],
    software: {
      10: sw(10, 'chrony', 'pkg', ['chrony', 'chrony', 'chrony']),
      11: sw(11, 'nginx', 'pkg', ['nginx', 'nginx', 'nginx'], { conflicts_with: [14] }),
      12: sw(12, 'node-exporter', 'artifact', ['=', '=', '='], { version_pin: '1.8.2' }),
      13: sw(13, 'prom-tools', 'pkg', ['golang-github-prometheus', null, 'golang-github-prometheus']),
      14: sw(14, 'apache2', 'pkg', ['apache2', 'apache2', 'httpd'], { conflicts_with: [11] }),
    },
    stacks: {
      1: { id: 1, slug: 'core', name: 'Core', description: 'Baseline every host gets', locked: true, sort_order: 0,
        depends_on: [], members: [10], updated_at: '2026-10-30T12:00:00Z' },
      3: { id: 3, slug: 'web', name: 'Web', description: null, locked: false, sort_order: 100,
        depends_on: [], members: [11], updated_at: '2026-10-30T12:00:00Z' },
      2: { id: 2, slug: 'monitoring', name: 'Monitoring', description: null, locked: false, sort_order: 200,
        depends_on: ['web'], members: [12, 13], updated_at: '2026-10-30T12:00:00Z' },
      4: { id: 4, slug: 'apache', name: 'Apache', description: null, locked: false, sort_order: 300,
        depends_on: [], members: [14], updated_at: '2026-10-30T12:00:00Z' },
    },
    targets: [
      { id: '9', name: 'neon', entity_type: 'server', lifecycle: 'active', role: 'compute', hw_make: 'Raspberry Pi',
        hw_model: 'Pi 5', hw_type: 'sbc', hw_model_id: null, arch: 'arm64', arch_source: 'agent', agent_arch: 'arm64',
        interfaces: [{ name: 'eth0', mac: 'aa:bb:cc:00:00:01', management: true },
          { name: 'wlan0', mac: 'aa:bb:cc:00:00:02', management: false }] },
      { id: '12', name: 'argon', entity_type: 'vm', lifecycle: 'active', role: null, hw_make: null, hw_model: null,
        hw_type: null, hw_model_id: null, arch: null, arch_source: null, agent_arch: null,
        interfaces: [{ name: 'ens18', mac: 'aa:bb:cc:00:00:03', management: false }] },
      { id: '14', name: 'krypton', entity_type: 'server', lifecycle: 'active', role: null, hw_make: 'Dell', hw_model: 'R410',
        hw_type: 'server', hw_model_id: 3, arch: null, arch_source: null, agent_arch: null,
        interfaces: [{ name: 'eno1', mac: 'aa:bb:cc:00:00:04', management: true }] },
    ],
    runs: [],
    logs: { 'stack-apply-9': 'PLAY [neon] ****\nTASK [chrony] ok\nPLAY RECAP neon ok=3 changed=0\n',
      'image-refresh-6': 'fetch debian 12 arm64 netboot\nsha256 ok\n' },
  };
}

async function main() {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'solarinet-provimages-browser-'));
  const sessions = path.join(temp, 'sessions');
  const stateFile = path.join(temp, 'state.json');
  fs.mkdirSync(sessions, { mode: 0o700 });
  fs.writeFileSync(stateFile, JSON.stringify(fixtureState()), { mode: 0o600 });
  const routerPath = path.join(temp, 'router.php');
  fs.writeFileSync(routerPath, phpRouter(stateFile), { mode: 0o600 });
  const writes = () => JSON.parse(fs.readFileSync(stateFile, 'utf8')).writes;
  // The page holds no lock on the state file; the test edits it between polls.
  const mutate = (fn) => { const st = JSON.parse(fs.readFileSync(stateFile, 'utf8')); fn(st); fs.writeFileSync(stateFile, JSON.stringify(st)); };

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
    const page = await browser.newPage({ viewport: { width: 1440, height: 1000 } });
    const pageErrors = [];
    page.on('pageerror', (error) => pageErrors.push(error.message));
    page.on('console', (message) => { if (message.type() === 'error') pageErrors.push(message.text()); });
    const user = `provimg-${crypto.randomBytes(4).toString('hex')}`;
    await page.goto(`${base}/__fixture_session?user=${encodeURIComponent(user)}`, { waitUntil: 'domcontentloaded' });
    await page.goto(`${base}/`, { waitUntil: 'load' });
    await page.locator('.nav-item').filter({ hasText: /^Fleet Overview$/ }).waitFor({ timeout: 15000 });
    await page.locator('.nav-item').filter({ hasText: /^Images & Stacks$/ }).click();
    await page.getByRole('heading', { name: 'Images & Stacks', level: 1 }).waitFor();
    assert.equal(await page.getByText('Live API unavailable').count(), 0, 'live session shows no offline banner');

    // --- target + arch filtering ------------------------------------------
    const deploy = page.getByRole('region', { name: 'Deploy' });
    const deployBtn = deploy.getByRole('button', { name: 'Deploy' });
    await deploy.getByText('Choose a target machine.').waitFor();
    assert.equal(await deployBtn.isDisabled(), true, 'deploy is blocked until a target is chosen');
    const targetSel = page.getByLabel('Machine (from inventory)');
    await targetSel.selectOption({ label: 'neon — Raspberry Pi Pi 5 · arm64' });
    const facts = page.locator('.prov-facts');
    const ifaceSel = page.getByLabel('Netboot interface');
    await ifaceSel.waitFor();
    assert.equal(await ifaceSel.inputValue(), 'aa:bb:cc:00:00:01', 'netboot interface defaults to the management MAC');
    assert.equal(await ifaceSel.isDisabled(), false, 'a multi-NIC target offers an explicit interface choice');
    assert.deepEqual(await ifaceSel.locator('option').allInnerTexts(),
      ['eth0 · aa:bb:cc:00:00:01 (mgmt)', 'wlan0 · aa:bb:cc:00:00:02'], 'every interface is listed, management marked');
    assert.match(await facts.innerText(), /arm64[\s\S]*Raspberry Pi Pi 5[\s\S]*active/i,
      'target facts show arch, model, lifecycle: ' + JSON.stringify(await facts.innerText()));
    const x86 = page.getByRole('radio', { name: /^Select Debian 13 \(trixie\) x86_64/ });
    const arm = page.getByRole('radio', { name: 'Select Debian 13 (trixie) arm64' });
    const unfetched = page.getByRole('radio', { name: /^Select Debian 12 \(bookworm\) arm64/ });
    const pi = page.getByRole('radio', { name: /^Select Raspberry Pi OS 12 \(bookworm\) armhf/ });
    assert.equal(await x86.isDisabled(), true, 'x86_64 image is not selectable for an arm64 target');
    assert.match(await x86.getAttribute('aria-label'), /image is x86_64, target is arm64/, 'mismatch reason is announced');
    assert.match(await unfetched.getAttribute('aria-label'), /not fetched/, 'unfetched image explains why it is disabled');
    assert.equal(await unfetched.isDisabled(), true, 'unfetched image cannot be staged');
    assert.equal(await pi.isDisabled(), true, 'armhf Pi image is greyed out for arm64');
    assert.equal(await arm.isDisabled(), false, 'matching fetched iPXE image is selectable');
    await page.getByRole('button', { name: 'Compatible only' }).click();
    assert.equal(await x86.count(), 0, 'compatible-only hides other architectures');
    await page.getByRole('button', { name: 'Compatible only' }).click();
    await x86.waitFor();

    // --- stacks: core locked, deps auto-pulled, run order -----------------
    const core = page.getByRole('checkbox', { name: 'Core' });
    const web = page.getByRole('checkbox', { name: 'Web' });
    const mon = page.getByRole('checkbox', { name: 'Monitoring' });
    assert.equal(await core.isChecked() && await core.isDisabled(), true, 'core is locked on');
    assert.equal(await web.isChecked(), false, 'web starts unselected');
    await mon.check();
    assert.equal(await web.isChecked() && await web.isDisabled(), true, 'web is auto-pulled as a dependency of monitoring');
    assert.match(await deploy.innerText(), /1·Core[\s\S]*2·Web[\s\S]*3·Monitoring/i,
      'deploy bar shows the resolved run order: ' + JSON.stringify(await deploy.innerText()));
    await deploy.getByText('Select an image.').waitFor();
    assert.equal(await deployBtn.isDisabled(), true, 'reimage needs an image');

    // --- reimage → PROV_STAGE via type-to-confirm --------------------------
    await arm.check();
    await deploy.getByText(/Unmapped on debian: Monitoring: prom-tools/).waitFor();
    await deploy.getByText(/Wipes the install disk on neon's next network boot from eth0 \(aa:bb:cc:00:00:01\)/).waitFor();
    await ifaceSel.selectOption('aa:bb:cc:00:00:02');
    await deploy.getByText(/next network boot from wlan0 \(aa:bb:cc:00:00:02\)/).waitFor();
    await deployBtn.click();
    let dialog = page.getByRole('alertdialog', { name: 'Stage reinstall of neon?' });
    await dialog.waitFor();
    assert.match(await dialog.innerText(), /Core → Web → Monitoring/, 'confirm lists the run order');
    assert.match(await dialog.innerText(), /PROV_STAGE · wlan0 aa:bb:cc:00:00:02 · arm64/, 'confirm names the chosen interface + arch');
    const stageBtn = dialog.getByRole('button', { name: 'Stage reinstall' });
    assert.equal(await stageBtn.isDisabled(), true, 'stage is disarmed until the target name is typed');
    await dialog.getByRole('button', { name: 'Cancel' }).click();
    await dialog.waitFor({ state: 'detached' });
    assert.equal(writes().length, 0, 'cancelled stage sent nothing');
    await deployBtn.click();
    dialog = page.getByRole('alertdialog', { name: 'Stage reinstall of neon?' });
    await dialog.getByLabel('Type neon to confirm').fill('neo');
    assert.equal(await stageBtn.isDisabled(), true, 'a partial name does not arm the action');
    await dialog.getByLabel('Type neon to confirm').fill('neon');
    await dialog.getByRole('button', { name: 'Stage reinstall' }).click();
    await dialog.waitFor({ state: 'detached' });
    let w = writes();
    assert.equal(w.length, 1, 'one stage write');
    assert.deepEqual([w[0].method, w[0].path, w[0].body], ['POST', 'stage',
      { entity: '9', image: 5, stacks: ['core', 'web', 'monitoring'], mac: 'aa:bb:cc:00:00:02' }],
      'stage body: entity, image, ordered stacks, the operator-chosen MAC (never a client arch)');
    assert.equal(w[0].csrf, true, 'stage carries the session CSRF token');
    assert.match(w[0].idem, /^prov-/, 'stage carries an idempotency key');
    const runs = page.locator('section[aria-labelledby="prov-h-runs"]');
    await runs.getByText('stage  debian 13 arm64 + core+web+monitoring → neon').waitFor();

    // --- apply-only: dry-run first, then a real apply ----------------------
    await page.getByRole('radio', { name: 'Apply stacks only' }).click();
    assert.match(await arm.getAttribute('aria-label'), /not used in apply-only mode/, 'images are disabled in apply mode');
    await deploy.getByText(/No monitoring agent matches this target/).waitFor();
    await deployBtn.click();
    dialog = page.getByRole('alertdialog', { name: 'Dry-run stacks on neon?' });
    await dialog.waitFor();
    const dry = dialog.getByRole('checkbox', { name: /Dry-run first/ });
    assert.equal(await dry.isChecked(), true, 'dry-run is offered first and on by default');
    await dialog.getByRole('button', { name: 'Run dry-run' }).click();
    await dialog.waitFor({ state: 'detached' });
    await deployBtn.click();
    dialog = page.getByRole('alertdialog', { name: 'Dry-run stacks on neon?' });
    await dialog.getByRole('checkbox', { name: /Dry-run first/ }).uncheck();
    dialog = page.getByRole('alertdialog', { name: 'Apply stacks to neon?' });
    const applyBtn = dialog.getByRole('button', { name: 'Apply now' });
    assert.equal(await applyBtn.isDisabled(), true, 'a real apply needs the typed target name');
    await dialog.getByLabel('Type neon to confirm').fill('neon');
    await applyBtn.click();
    await dialog.waitFor({ state: 'detached' });
    w = writes();
    assert.deepEqual(w.slice(1).map((x) => [x.path, x.body]), [
      ['apply', { entity: '9', stacks: ['core', 'web', 'monitoring'], dry_run: true }],
      ['apply', { entity: '9', stacks: ['core', 'web', 'monitoring'], dry_run: false }],
    ], 'dry-run then apply, both with the ordered stacks');
    assert.notEqual(w[1].idem, w[2].idem, 'dry-run and apply use distinct idempotency keys');
    await runs.getByText(/\(dry-run\)/).waitFor();

    // --- helper log tail --------------------------------------------------
    await runs.getByRole('button', { name: /^Helper log for run / }).first().click();
    const drawer = page.getByRole('dialog', { name: 'Apply log · neon' });
    await drawer.getByText(/PLAY RECAP neon ok=3/).waitFor();
    await page.keyboard.press('Escape');
    await drawer.waitFor({ state: 'detached' });

    // --- image refresh + delete confirm ------------------------------------
    await page.getByRole('radio', { name: 'Reimage (wipe + install)' }).click();
    await page.getByRole('button', { name: 'Refresh Debian 12 (bookworm) arm64 from upstream' }).click();
    await page.locator('.toast').filter({ hasText: /Refresh of Debian 12 \(bookworm\) arm64 queued \(request \d+\) — follow it in Recent runs/ }).waitFor();
    w = writes();
    assert.deepEqual([w[3].path, w[3].body, w[3].csrf], ['image-refresh', { image: 6 }, true], 'IMAGE_REFRESH for the row');
    await runs.getByText('refresh  debian 12 arm64').waitFor();
    await page.locator('.prov-irow').filter({ hasText: 'refresh running…' }).waitFor();
    await runs.getByRole('button', { name: /^Helper log for run / }).first().click();
    const refreshLog = page.getByRole('dialog', { name: 'Refresh log · debian 12 arm64' });
    await refreshLog.getByText(/sha256 ok/).waitFor();
    await page.keyboard.press('Escape');
    await refreshLog.waitFor({ state: 'detached' });
    mutate((st) => { st.runs.find((r) => r.mode === 'refresh').status = 'succeeded'; });
    await page.locator('.toast').filter({ hasText: 'Refresh of debian 12 arm64 succeeded' }).waitFor({ timeout: 15000 });
    await page.locator('.prov-irow').filter({ hasText: /refresh(ing| running)…/ }).waitFor({ state: 'detached' });

    await page.getByRole('button', { name: 'Delete stack Web' }).click();
    dialog = page.getByRole('alertdialog', { name: 'Delete stack Web?' });
    await dialog.getByRole('button', { name: 'Cancel' }).click();
    await dialog.waitFor({ state: 'detached' });
    assert.equal(writes().length, 4, 'cancelled delete sent nothing');
    assert.equal(await page.getByRole('button', { name: 'Delete stack Core' }).isDisabled(), true,
      'locked stack cannot be deleted');

    // --- pool conflicts block before confirm -------------------------------
    const alt = page.getByRole('checkbox', { name: 'Apache' });
    await alt.check();
    await deploy.getByText('Conflicting entries selected: nginx (Web) ✕ apache2 (Apache).').waitFor();
    assert.equal(await deployBtn.isDisabled(), true, 'a conflicting selection blocks deploy before confirm');
    await alt.uncheck();
    assert.equal(await deployBtn.isDisabled(), false, 'clearing the conflict re-arms deploy');
    assert.equal(writes().length, 4, 'a blocked conflict sent nothing');

    // --- unknown target arch fails closed for reimage ------------------------
    await targetSel.selectOption({ label: 'argon' });
    await deploy.getByText('Target CPU architecture unknown — give argon a hardware model with a CPU architecture (Inventory) or let the agent report it.').waitFor();
    assert.equal(await deployBtn.isDisabled(), true, 'reimage is blocked when the arch is unknown');
    assert.equal(await ifaceSel.isDisabled(), true, 'a single-NIC target shows its interface without a choice');
    assert.equal(await facts.getByRole('button', { name: 'Set' }).count(), 0, 'no model → no arch edit');
    await page.getByRole('radio', { name: 'Apply stacks only' }).click();
    await deploy.getByText(/Target CPU architecture unknown/).waitFor({ state: 'detached' });
    await page.getByRole('radio', { name: 'Reimage (wipe + install)' }).click();
    await targetSel.selectOption({ label: 'krypton — Dell R410' });
    await deploy.getByText('Target CPU architecture unknown — set the CPU architecture on hardware model Dell R410 (Inventory) or let the agent report it.').waitFor();
    await facts.getByRole('button', { name: 'Set' }).click();
    dialog = page.getByRole('alertdialog', { name: 'CPU architecture of Dell R410' });
    await dialog.getByLabel('CPU architecture').selectOption('x86_64');
    await dialog.getByRole('button', { name: 'Save' }).click();
    await dialog.waitFor({ state: 'detached' });
    w = writes();
    assert.deepEqual([w[4].method, w[4].path, w[4].body, w[4].csrf], ['PUT', 'inventory/models/3', { arch: 'x86_64' }, true],
      'arch edit PUTs the hardware model with CSRF');
    await deploy.getByText(/Target CPU architecture unknown/).waitFor({ state: 'detached' });
    assert.match(await facts.getByText('x86_64', { exact: true }).getAttribute('title'), /From hardware model Dell R410/,
      'resolved arch names its source');
    await deploy.getByText('Select an image.').waitFor();
    await targetSel.selectOption({ label: 'neon — Raspberry Pi Pi 5 · arm64' });

    if (process.env.SOLARI_PROVIMAGES_SCREENSHOT) {
      await page.screenshot({ path: process.env.SOLARI_PROVIMAGES_SCREENSHOT, fullPage: true });
    }

    // --- software pool ----------------------------------------------------
    await page.getByRole('button', { name: 'Software pool' }).click();
    const pool = page.locator('section[aria-labelledby="prov-h-pool"]');
    await pool.getByText('node-exporter').waitFor();
    assert.equal(await pool.getByText('prom-tools').count() > 0, true, 'pool lists every entry');
    if (process.env.SOLARI_PROVIMAGES_POOL_SCREENSHOT) {
      await page.screenshot({ path: process.env.SOLARI_PROVIMAGES_POOL_SCREENSHOT, fullPage: true });
    }

    // --- layout -----------------------------------------------------------
    await page.getByRole('button', { name: 'Deploy', exact: true }).first().click();
    await page.setViewportSize({ width: 390, height: 844 });
    const navItem = page.locator('.nav-item').filter({ hasText: /^Images & Stacks$/ });
    if (await navItem.isVisible()) await navItem.click();
    await page.getByRole('heading', { name: 'Images & Stacks', level: 1 }).waitFor();
    await page.waitForTimeout(400);
    const mobile = await page.evaluate(() => ({
      width: innerWidth,
      body: document.body.scrollWidth,
      content: document.querySelector('.content').scrollWidth - document.querySelector('.content').clientWidth,
      overflowing: Array.from(document.querySelectorAll('.prov-page *')).map((el) => {
        const r = el.getBoundingClientRect();
        return { cls: String(el.className || el.tagName).slice(0, 60), right: Math.round(r.right) };
      }).filter((x) => x.right > innerWidth + 1).slice(0, 8),
    }));
    assert.equal(mobile.body, mobile.width, `mobile page has no horizontal overflow: ${JSON.stringify(mobile.overflowing)}`);
    assert.equal(mobile.content, 0, `mobile content is not clipped: ${JSON.stringify(mobile.overflowing)}`);
    assert(await deployBtn.isVisible(), 'deploy action stays visible on mobile');
    if (process.env.SOLARI_PROVIMAGES_MOBILE_SCREENSHOT) {
      await page.screenshot({ path: process.env.SOLARI_PROVIMAGES_MOBILE_SCREENSHOT });
    }

    assert.deepEqual(pageErrors, [], 'browser reported no JavaScript or console errors');
    console.log('Images & Stacks browser acceptance passed: arch filtering, netboot interface choice, locked/auto stacks + run order, '
      + 'typed-confirm stage, dry-run then apply, log tail, refresh run tracking, delete confirm, unknown-arch block + '
      + 'hardware-model arch edit, pool, mobile layout.');
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
