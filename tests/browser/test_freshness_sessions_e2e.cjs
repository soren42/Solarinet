#!/usr/bin/env node
'use strict';

// Exercise dashboard freshness in two authenticated browser sessions against
// the real SPA and real PHP session implementation. API payloads are fixtures;
// no production service or database is contacted.
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
  throw new Error('PHP freshness fixture did not become ready.');
}

function phpRouter(stateFile) {
  return `<?php
require_once ${JSON.stringify(AUTH_LIB)};
function fixture_state($path, $write = null) {
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
    Auth::establishSession(['username' => $user, 'role' => 'operator', 'source' => 'test']);
    header('Content-Type: application/json'); echo json_encode(['ready' => true]); exit;
}
if ($path === '/__fixture_control') {
    Auth::bootSession();
    $principal = Auth::current();
    if ($principal === null || $principal['role'] !== 'operator') { http_response_code(401); exit; }
    $mode = (string)($_GET['mode'] ?? 'online');
    if (!in_array($mode, ['online', 'offline'], true)) { http_response_code(400); exit; }
    $key = session_id();
    fixture_state(${JSON.stringify(stateFile)}, static function ($state) use ($key, $mode) {
        $state['modes'][$key] = $mode; return $state;
    });
    header('Content-Type: application/json'); echo json_encode(['mode' => $mode]); exit;
}
if (is_string($path) && str_starts_with($path, '/api/')) {
    Auth::bootSession();
    $principal = Auth::current();
    if ($principal === null) { http_response_code(401); header('Content-Type: application/json'); echo json_encode(['ok'=>false,'error'=>['code'=>'unauthorized','message'=>'Not authenticated']]); exit; }
    $key = session_id();
    $state = fixture_state(${JSON.stringify(stateFile)});
    if (($state['modes'][$key] ?? 'online') === 'offline' && $path === '/api/nodes') {
        http_response_code(503); header('Content-Type: application/json');
        echo json_encode(['ok'=>false,'error'=>['code'=>'fixture_offline','message'=>'Fixture API unavailable']]); exit;
    }
    if ($_SERVER['REQUEST_METHOD'] === 'POST') {
        fixture_state(${JSON.stringify(stateFile)}, static function ($state) use ($key) {
            $state['writes'][$key] = ($state['writes'][$key] ?? 0) + 1; return $state;
        });
    }
    $data = match ($path) {
        '/api/summary' => ['counts'=>[], 'lease'=>[]],
        '/api/config' => [],
        '/api/auth/whoami' => ['operator'=>$principal['username'],'role'=>$principal['role'],'displayName'=>$principal['username'],'source'=>'oidc','csrfToken'=>Auth::csrfToken()],
        default => [],
    };
    if ($path !== '/api/summary' && $path !== '/api/config' && $path !== '/api/auth/whoami') $data = [];
    header('Content-Type: application/json'); header('Cache-Control: no-store');
    echo json_encode(['ok'=>true,'data'=>$data,'ts'=>gmdate('c')]); exit;
}
return false;
?>`;
}

async function main() {
  const temp = fs.mkdtempSync(path.join(os.tmpdir(), 'solarinet-freshness-browser-'));
  const sessions = path.join(temp, 'sessions');
  const stateFile = path.join(temp, 'state.json');
  fs.mkdirSync(sessions, { mode: 0o700 });
  fs.writeFileSync(stateFile, '{}', { mode: 0o600 });
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
    child.stderr.on('data', (data) => { phpErrors = (phpErrors + data.toString()).slice(-3000); });
    await waitForServer(`${base}/__health`, child);

    browser = await chromium.launch({
      headless: true,
      args: ['--no-sandbox'],
      ...(process.env.SOLARI_CHROMIUM_EXECUTABLE ? { executablePath: process.env.SOLARI_CHROMIUM_EXECUTABLE } : {}),
    });
    const contexts = [await browser.newContext(), await browser.newContext()];
    const pages = await Promise.all(contexts.map((context) => context.newPage()));
    const pageErrors = [];
    for (const page of pages) page.on('pageerror', (error) => pageErrors.push(error.message));

    for (let i = 0; i < pages.length; i += 1) {
      const user = `freshness-${i}-${crypto.randomBytes(4).toString('hex')}`;
      await pages[i].goto(`${base}/__fixture_session?user=${encodeURIComponent(user)}`, { waitUntil: 'domcontentloaded' });
      await pages[i].goto(`${base}/`, { waitUntil: 'load' });
      try {
        await pages[i].locator('.nav-item').filter({ hasText: /^Fleet Overview$/ }).waitFor({ timeout: 15000 });
      } catch (error) {
        error.message += `\nBrowser page errors: ${JSON.stringify(pageErrors)}\n` +
          `Rendered root: ${(await pages[i].locator('#root').innerText().catch(() => '')).slice(0, 1200)}`;
        throw error;
      }
      assert.equal(await pages[i].evaluate(() => window.SOLARI.source), 'live', 'authenticated browser should start with real-mode data');
      assert.equal(await pages[i].evaluate(() => window.SOLARI.operator.role), 'operator');
    }

    const [affected, unaffected] = contexts;
    await affected.request.get(`${base}/__fixture_control?mode=offline`);
    await pages[0].evaluate(async () => {
      await window.SOLARI.api.refresh().catch(() => {});
    });
    await pages[0].waitForFunction(() => window.SOLARI.source === 'stale', null, { timeout: 3000 });
    await pages[0].getByRole('status').filter({ hasText: 'DATA STALE' }).waitFor({ timeout: 15000 });
    await pages[0].evaluate(async () => {
      await window.SOLARI.api.survey('all').then(
        () => { throw new Error('stale session unexpectedly sent a mutation'); },
        (error) => { if (error.code !== 'DATA_STALE') throw error; }
      );
    });
    await pages[1].evaluate(async () => { await window.SOLARI.api.survey('all'); });
    assert.equal(await pages[0].evaluate(() => window.SOLARI.source), 'stale');
    assert.equal(await pages[1].evaluate(() => window.SOLARI.source), 'live', 'one browser session outage must not stale another session');

    await affected.request.get(`${base}/__fixture_control?mode=online`);
    await pages[0].evaluate(async () => { await window.SOLARI.api.refresh(); });
    assert.equal(await pages[0].evaluate(() => window.SOLARI.source), 'live', 'the affected session should recover after API service returns');

    const state = JSON.parse(fs.readFileSync(stateFile, 'utf8'));
    const keys = await Promise.all(contexts.map(async (context) => {
      const cookies = await context.cookies();
      const session = cookies.find((cookie) => cookie.name === 'solari_sess');
      assert(session, 'fixture must establish an independent authenticated PHP session');
      return session.value;
    }));
    assert.notEqual(keys[0], keys[1], 'the browser contexts must use distinct PHP sessions');
    assert.equal(state.writes[keys[0]] || 0, 0, 'the stale client must not send a write request');
    assert.equal(state.writes[keys[1]] || 0, 1, 'the live client should retain its independent write path');
    assert.deepEqual(pageErrors, [], 'browser reported no JavaScript errors');
    await Promise.all(contexts.map((context) => context.close()));
    console.log('Dashboard freshness browser E2E passed: two independent authenticated sessions, stale warning, stale-write gate, unaffected peer, and recovery.');
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
