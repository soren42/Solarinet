#!/usr/bin/env node
'use strict';

// Exercise RackWire's dashboard handoff into the real Discovery UI, PHP scan
// routes, privileged C queue, MariaDB-backed worker, and candidate list. CI
// remains loopback-only; an explicit test flag enables one route-checked LAN
// /24 acceptance on the operator workstation.
const assert = require('node:assert/strict');
const { spawn, spawnSync } = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');
const { chromium } = require('playwright');

const ROOT = path.resolve(__dirname, '../..');
const PUBLIC = path.join(ROOT, 'dashboard/public');
const API_INDEX = path.join(ROOT, 'dashboard/api/index.php');
const AUTH_LIB = path.join(ROOT, 'dashboard/api/lib/Auth.php');
const BRIDGE_NODE_ID = '138915363029153'; // test_ctl_adopt_bridge's fixed node id
const REQUESTER = 'scan-worker-browser-e2e';
const NETWORK_SCAN = process.env.SOLARI_TEST_NETWORK_SCAN === '1';
const SCAN_CIDR = NETWORK_SCAN ? '10.0.0.0/24' : '127.0.0.1/32';
const SCAN_TARGET = NETWORK_SCAN ? '10.0.0.20' : '127.0.0.1';
const SCAN_HOSTS = NETWORK_SCAN ? 254 : 1;

function assertNetworkScanIsDirect() {
  if (!NETWORK_SCAN) return;
  const interfaces = os.networkInterfaces();
  const iface = Object.entries(interfaces).find(([, addresses]) =>
    (addresses || []).some((entry) => entry.address === SCAN_TARGET &&
      (entry.family === 'IPv4' || entry.family === 4)));
  assert(iface, `Opt-in LAN scan requires ${SCAN_TARGET} to be assigned locally.`);
  const route = spawnSync('ip', ['-4', 'route', 'get', '10.0.0.254'], { encoding: 'utf8' });
  assert.equal(route.status, 0, 'Opt-in LAN scan requires the ip route utility.');
  assert(route.stdout.includes(`dev ${iface[0]}`) && !/\bvia\b/.test(route.stdout),
    'Opt-in /24 must route directly through the interface that owns the listener address.');
}

function databaseConfig() {
  const host = process.env.SOLARI_DB_HOST || '';
  const name = process.env.SOLARI_DB_NAME || '';
  if (process.env.SOLARI_TEST_DB !== '1' || !['127.0.0.1', 'localhost'].includes(host) ||
      !/^solarinet_scan_worker_[a-z0-9_]+$/.test(name)) {
    throw new Error('Set SOLARI_TEST_DB=1 and loopback solarinet_scan_worker_* DB settings.');
  }
  return {
    host, port: process.env.SOLARI_DB_PORT || '3306', name,
    user: process.env.SOLARI_DB_USER || 'root', pass: process.env.SOLARI_DB_PASS || '',
  };
}

function sql(db, query) {
  const result = spawnSync('mariadb', [
    '--no-defaults', '--protocol=tcp', `--host=${db.host}`, `--port=${db.port}`,
    `--user=${db.user}`, '--batch', '--raw', '--skip-column-names', db.name, '-e', query,
  ], { encoding: 'utf8', env: { ...process.env, MYSQL_PWD: db.pass } });
  if (result.status !== 0) throw new Error('Scratch monitoring SQL failed: ' + (result.stderr || 'MariaDB client error').trim());
  return result.stdout.trim();
}

function quote(value) { return "'" + String(value).replaceAll("'", "''") + "'"; }

async function reservePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      server.close((error) => error ? reject(error) : resolve(port));
    });
  });
}

async function openScanListener(host = '127.0.0.1') {
  // These are all in the C scanner's fixed default port set. The UI exercises
  // the same no-custom-ports path an operator uses in the dashboard.
  for (const port of [8080, 9090, 9100, 7701, 7702]) {
    const server = net.createServer((socket) => socket.end());
    try {
      await new Promise((resolve, reject) => {
        server.once('error', reject);
        server.listen(port, host, resolve);
      });
      return { server, port };
    } catch (_) { server.close(); }
  }
  throw new Error(`No free default scan port is available on ${host}.`);
}

async function waitFor(predicate, description, processes = [], timeout = 10000) {
  const until = Date.now() + timeout;
  while (Date.now() < until) {
    for (const item of processes) {
      if (item.process && item.process.exitCode !== null)
        throw new Error(`${item.name} exited early (${item.process.exitCode}): ${item.logs().slice(-2500)}`);
    }
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  const details = processes.map((p) => `${p.name}:\n${p.logs().slice(-2000)}`).join('\n');
  throw new Error(`Timed out waiting for ${description}.${details ? `\n${details}` : ''}`);
}

function phpRouter(user, ctlSock) {
  return `<?php
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH);
if ($path === '/__health') { header('Content-Type: text/plain'); echo 'ok'; exit; }
if ($path === '/__fixture_session') {
  require_once ${JSON.stringify(AUTH_LIB)};
  Auth::establishSession(['username'=>${JSON.stringify(user)},'role'=>'operator','source'=>'test']);
  header('Content-Type: application/json'); echo '{"ready":true}'; exit;
}
if (is_string($path) && ($path === '/api/auth/whoami' || str_starts_with($path, '/api/discovery'))) {
  require ${JSON.stringify(API_INDEX)}; exit;
}
if (is_string($path) && str_starts_with($path, '/api/')) {
  header('Content-Type: application/json'); echo json_encode(['ok'=>true,'data'=>[],'ts'=>time()]); exit;
}
return false;
?>`;
}

function startLogged(command, args, options) {
  const proc = spawn(command, args, { ...options, stdio: ['ignore', 'pipe', 'pipe'] });
  let logs = '';
  proc.stdout.on('data', (chunk) => { logs = (logs + chunk.toString()).slice(-12000); });
  proc.stderr.on('data', (chunk) => { logs = (logs + chunk.toString()).slice(-12000); });
  return { process: proc, logs: () => logs };
}

function stop(proc) {
  if (!proc || proc.exitCode !== null) return Promise.resolve();
  proc.kill('SIGTERM');
  return new Promise((resolve) => {
    const timer = setTimeout(() => { if (proc.exitCode === null) proc.kill('SIGKILL'); }, 5000);
    proc.once('exit', () => { clearTimeout(timer); resolve(); });
  });
}

async function main() {
  if (process.argv.length !== 4) {
    throw new Error('usage: test_scan_worker_browser_e2e.cjs /path/to/solariScanWorker /path/to/test_ctl_adopt_bridge');
  }
  assertNetworkScanIsDirect();
  const db = databaseConfig();
  const discoveredBefore = new Set(sql(db, 'SELECT discId FROM discovered').split('\n').filter(Boolean));
  const workerBin = path.resolve(process.argv[2]);
  const bridgeBin = path.resolve(process.argv[3]);
  for (const bin of [workerBin, bridgeBin]) {
    if (!fs.existsSync(bin) || !(fs.statSync(bin).mode & 0o111)) throw new Error(`Missing executable: ${bin}`);
  }
  const listener = await openScanListener(SCAN_TARGET);
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'solari-scan-browser-e2e-'));
  const sessionDir = path.join(temp, 'sessions');
  const toolDir = path.join(temp, 'tools');
  fs.mkdirSync(sessionDir, { mode: 0o700 });
  fs.mkdirSync(toolDir, { mode: 0o700 });
  const ctlSock = path.join(temp, 'solariCtl.sock');
  const routerPath = path.join(temp, 'router.php');
  const configPath = path.join(temp, 'worker.conf');
  const lockPath = path.join(temp, 'worker.lock');
  const webPort = await reservePort();
  const user = `${REQUESTER}-${process.pid}`;
  const operator = user;
  const host = db.host;
  const port = db.port;
  const sessionUser = user;
  fs.writeFileSync(routerPath, phpRouter(sessionUser, ctlSock), { mode: 0o600 });
  fs.writeFileSync(configPath, `[identity]\nhostFqdn = scan-worker-browser.invalid\nnodeId = ${BRIDGE_NODE_ID}\n\n[db]\nhost = ${host}\nport = ${port}\nname = ${db.name}\nuser = ${db.user}\npoolSize = 1\n\n[lease]\nleaseRenewSec = 1\nleaseTtlSec = 6\n`, { mode: 0o600 });
  fs.writeFileSync(path.join(toolDir, 'nmap'), '#!/bin/sh\n/bin/sleep 2\nprintf \'%s\\n\' \'<nmaprun><host><status state="up"/></host></nmaprun>\'\n', { mode: 0o700 });
  for (const tool of ['avahi-resolve', 'avahi-browse', 'snmpget'])
    fs.writeFileSync(path.join(toolDir, tool), '#!/bin/sh\nexit 1\n', { mode: 0o700 });

  let bridge, worker, php, browser, listenerClosed = false;
  const serverLog = { name: 'php', process: null, logs: () => phpLogs };
  let phpLogs = '';
  const childEnv = {
    ...process.env,
    SOLARI_DB_HOST: db.host, SOLARI_DB_PORT: db.port, SOLARI_DB_NAME: db.name,
    SOLARI_DB_USER: db.user, SOLARI_DB_PASS: db.pass,
    SOLARI_CTL_SOCK: ctlSock, SOLARI_TEST_DRAIN_CTL_QUEUE: '1',
  };
  childEnv.PATH = toolDir + path.delimiter + (process.env.PATH || '/usr/bin:/bin');
  try {
    if (sql(db, "SELECT COUNT(*) FROM discoveryScanJob WHERE status IN ('queued','running')") !== '0' ||
        sql(db, `SELECT COUNT(*) FROM discovered WHERE ip=${quote(SCAN_TARGET)} AND kind='host'`) !== '0')
      throw new Error('Scratch DB already contains an active scan or target candidate row.');
    sql(db, `INSERT INTO serverLease (id,leaseHolder,leaseEpoch,expiresAt) VALUES ` +
      `(1,${BRIDGE_NODE_ID},1,UTC_TIMESTAMP(3)+INTERVAL 120 SECOND) ` +
      `ON DUPLICATE KEY UPDATE leaseHolder=VALUES(leaseHolder),leaseEpoch=VALUES(leaseEpoch),expiresAt=VALUES(expiresAt)`);

    bridge = startLogged(bridgeBin, [ctlSock], { cwd: ROOT, env: childEnv });
    await waitFor(() => fs.existsSync(ctlSock), 'C bridge socket', [
      { name: 'C bridge', process: bridge.process, logs: bridge.logs },
    ]);
    worker = startLogged(workerBin, ['--config', configPath, '--lock-file', lockPath],
      { cwd: ROOT, env: childEnv });
    await waitFor(() => worker.logs().includes('ready'), 'scan worker readiness', [
      { name: 'scan worker', process: worker.process, logs: worker.logs },
    ]);
    php = spawn('php', ['-d', `session.save_path=${sessionDir}`, '-S', `127.0.0.1:${webPort}`,
      '-t', PUBLIC, routerPath], { cwd: ROOT, env: childEnv, stdio: ['ignore', 'ignore', 'pipe'] });
    php.stderr.on('data', (chunk) => { phpLogs = (phpLogs + chunk.toString()).slice(-8000); });
    serverLog.process = php;
    await waitFor(async () => {
      try { return (await fetch(`http://127.0.0.1:${webPort}/__health`)).ok; }
      catch (_) { return false; }
    }, 'PHP fixture readiness', [serverLog]);

    browser = await chromium.launch({ headless: true, args: ['--no-sandbox'],
      ...(process.env.SOLARI_CHROMIUM_EXECUTABLE ? { executablePath: process.env.SOLARI_CHROMIUM_EXECUTABLE } : {}) });
    const context = await browser.newContext();
    const page = await context.newPage();
    const errors = [];
    const external = [];
    const discoveryResponses = [];
    page.on('pageerror', (error) => errors.push(error.message));
    page.on('response', async (response) => {
      if (response.url().includes('/api/discovery')) {
        let body = '';
        try { body = (await response.text()).slice(0, 1200); } catch (_) { /* body unavailable */ }
        discoveryResponses.push(`${response.status()} ${response.url()} ${body}`);
      }
    });
    page.on('request', (req) => {
      const url = req.url();
      if (!url.startsWith(`http://127.0.0.1:${webPort}/`) && !url.startsWith('file://')) external.push(url);
    });
    const base = `http://127.0.0.1:${webPort}`;
    await page.goto(`${base}/__fixture_session`, { waitUntil: 'domcontentloaded' });
    await page.goto(`${base}/`, { waitUntil: 'load' });
    try {
      await page.locator('.nav-item').filter({ hasText: /^Inventory$/ }).waitFor({ timeout: 15000 });
    } catch (error) {
      throw new Error(`Dashboard boot failed: ${error.message}\nURL: ${page.url()}\n` +
        `body: ${(await page.locator('body').innerText()).slice(0, 1500)}\n` +
        `page errors: ${errors.join(' | ')}\nPHP: ${phpLogs}`);
    }
    assert.equal(await page.evaluate(() => window.SOLARI && window.SOLARI.source), 'live',
      'dashboard must boot in live mode');
    await page.locator('.nav-item').filter({ hasText: /^RackWire$/ }).click();
    await page.getByRole('heading', { name: 'RackWire', exact: true }).waitFor();
    await page.getByText('Network, power & interconnect planning', { exact: false }).waitFor();
    await page.waitForFunction(() => {
      const frame = document.querySelector('iframe[title="RackWire network, power, and interconnect planner"]');
      return !!(frame && frame.contentWindow && frame.contentWindow.RackWire);
    }, { timeout: 15000 });
    await page.getByRole('button', { name: 'Network discovery', exact: true }).click();
    await page.getByRole('heading', { name: 'Discovery', exact: true }).waitFor();
    await page.getByPlaceholder('192.168.1.0/24').fill(SCAN_CIDR);
    const acceptedResponse = page.waitForResponse((response) =>
      response.url().endsWith('/api/discovery/scan') && response.request().method() === 'POST');
    await page.getByRole('button', { name: 'Scan', exact: true }).click();
    const accepted = await acceptedResponse;
    assert.equal(accepted.status(), 202, 'Discovery UI must submit through the real scan API');
    const csrf = (await accepted.request().allHeaders())['x-solari-csrf'];
    assert(typeof csrf === 'string' && csrf.length > 20, 'real scan admission must carry CSRF');

    const firstObservedStatus = await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      return job && job.scanId > 0 && ['running', 'completed'].includes(job.status) ? job.status : false;
    }, { timeout: 15000 });
    if (firstObservedStatus === 'running') {
      await page.getByRole('status').filter({ hasText: 'Scan running' }).waitFor();
      assert(await page.getByRole('button', { name: 'Scan active…', exact: true }).isDisabled(),
        'Discovery must prevent duplicate scan submission while work is running');
    }

    await page.locator('.nav-item').filter({ hasText: /^Inventory$/ }).click();
    await page.getByRole('heading', { name: 'Inventory', exact: true }).waitFor();
    await page.locator('.nav-item').filter({
      has: page.locator('.navlabel').filter({ hasText: /^Discovery$/ }),
    }).click();
    await page.getByRole('heading', { name: 'Discovery', exact: true }).waitFor();
    await page.waitForFunction(() => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      return job && job.scanId > 0 && ['running', 'completed'].includes(job.status);
    });

    if (NETWORK_SCAN) {
      await page.waitForFunction((target) => {
        const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
        const candidateVisible = Array.from(document.querySelectorAll('tr'))
          .some((row) => row.innerText.includes(target));
        return job && job.status === 'running' && candidateVisible;
      }, SCAN_TARGET, { timeout: 45000 });
    }

    await page.waitForFunction((expectedHosts) => {
      const job = JSON.parse(sessionStorage.getItem('solari.activeScan') || 'null');
      return job && job.status === 'completed' && job.hostsTotal === expectedHosts && job.hostsFound > 0;
    }, SCAN_HOSTS, { timeout: NETWORK_SCAN ? 45000 : 20000 });
    try {
      await page.locator('tr').filter({ hasText: SCAN_TARGET }).first().waitFor({ timeout: 10000 });
    } catch (error) {
      throw new Error(`Completed scan candidate not visible: ${error.message}\n` +
        `body: ${(await page.locator('body').innerText()).slice(0, 2500)}\n` +
        `discovery responses: ${discoveryResponses.join('\n')}\n` +
        `browser errors: ${errors.join(' | ')}\nPHP: ${phpLogs}`);
    }
    assert.deepEqual(errors, [], 'browser has no JavaScript errors');
    assert.deepEqual(external, [], 'browser makes no external requests');

    const scanRows = sql(db, `SELECT CONCAT(status,'|',hostsDone,'|',hostsTotal,'|',hostsFound) ` +
      `FROM discoveryScanJob WHERE requestedBy=${quote(operator)} ORDER BY scanId DESC LIMIT 1`);
    const [scanStatus, scanDone, scanTotal, scanFound] = scanRows.split('|');
    assert.equal(scanStatus, 'completed', 'native worker must persist terminal completion');
    assert.equal(Number(scanDone), SCAN_HOSTS, 'native worker must process the entire selected CIDR');
    assert.equal(Number(scanTotal), SCAN_HOSTS, 'admission must preserve the selected host count');
    assert(Number(scanFound) > 0, 'native worker must find at least one host');
    const candidates = sql(db, `SELECT COUNT(*) FROM discovered WHERE ip=${quote(SCAN_TARGET)} AND kind='host' AND via='portscan' AND status='new'`);
    assert.equal(candidates, '1', 'native worker must create one reviewable discovery candidate');
    console.log(`RackWire-to-native-worker browser E2E passed: ${SCAN_CIDR}, ${scanDone}/${scanTotal} hosts, ${scanFound} candidates, embedded discovery handoff, live status polling, navigation recovery, and candidate ingest${NETWORK_SCAN ? ' before terminal completion' : ''}.`);
  } finally {
    if (browser) await browser.close();
    await stop(php);
    await stop(worker && worker.process);
    await stop(bridge && bridge.process);
    if (!listenerClosed) await new Promise((resolve) => listener.server.close(resolve));
    try {
      sql(db, `DELETE FROM ctlRequest WHERE requestedBy=${quote(operator)} AND verb='DISCOVER'`);
      sql(db, `DELETE FROM discoveryScanJob WHERE requestedBy=${quote(operator)}`);
      const discoveredAfter = sql(db, 'SELECT discId FROM discovered').split('\n').filter(Boolean);
      const createdIds = discoveredAfter.filter((id) => !discoveredBefore.has(id) && /^\d+$/.test(id));
      if (createdIds.length) sql(db, `DELETE FROM discovered WHERE discId IN (${createdIds.join(',')})`);
      sql(db, 'DELETE FROM serverLease WHERE id=1');
    } catch (_) { /* the isolated MariaDB schema is discarded by the test harness */ }
    fs.rmSync(temp, { recursive: true, force: true });
  }
}

main().catch((error) => {
  console.error(`FAIL: ${error.stack || error}`);
  process.exitCode = 1;
});
