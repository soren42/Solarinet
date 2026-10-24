#!/usr/bin/env node
'use strict';

// Cold/warm Chromium profile for the no-build dashboard. API calls are
// intercepted as 401; the warm navigation reuses compiled JSX in CacheStorage.
const { spawn } = require('node:child_process');
const path = require('node:path');
const { chromium } = require('playwright');

const root = path.resolve(__dirname, '../../dashboard/public');
const declaredJsxModules = (require('node:fs').readFileSync(path.join(root, 'index.html'), 'utf8')
  .match(/<script\b[^>]*type=["']application\/x-solarinet-jsx["'][^>]*>/g) || []).length;

async function startServer() {
  const child = spawn(process.env.PYTHON || 'python3', [
    '-u', '-m', 'http.server', '0', '--bind', '127.0.0.1', '--directory', root,
  ], { stdio: ['ignore', 'pipe', 'ignore'] });
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => { child.kill('SIGTERM'); reject(new Error('static server startup timed out')); }, 10000);
    let output = '';
    child.stdout.on('data', (chunk) => {
      output += chunk.toString();
      const match = output.match(/Serving HTTP on 127\.0\.0\.1 port (\d+)/);
      if (match) { clearTimeout(timeout); resolve({ child, origin: `http://127.0.0.1:${match[1]}` }); }
    });
    child.once('error', (error) => { clearTimeout(timeout); reject(error); });
    child.once('exit', (code) => { clearTimeout(timeout); reject(new Error(`static server exited (${code})`)); });
  });
}

async function profile(page, origin, label) {
  const errors = [];
  page.on('pageerror', (error) => errors.push(String(error)));
  await page.goto(`${origin}/index.html`, { waitUntil: 'load', timeout: 45000 });
  await page.waitForFunction(() => window.__solariBootReady === true, { timeout: 45000 });
  await page.locator('.app').waitFor({ timeout: 15000 });
  const result = await page.evaluate(async (profileName) => ({
    profile: profileName,
    navigation: performance.getEntriesByType('navigation').map((entry) => ({
      dcl: Math.round(entry.domContentLoadedEventEnd), load: Math.round(entry.loadEventEnd),
    })),
    paints: performance.getEntriesByType('paint').map((entry) => ({ name: entry.name, ms: Math.round(entry.startTime) })),
    appReadyMs: Math.round((performance.getEntriesByName('solari-app-ready')[0] || {}).startTime || 0),
    longTasks: window.__longTasks,
    babelRequests: performance.getEntriesByType('resource').filter((entry) => entry.name.includes('/vendor/babel.')).length,
    babelBytes: performance.getEntriesByType('resource').filter((entry) => entry.name.includes('/vendor/babel.'))
      .reduce((total, entry) => total + entry.transferSize, 0),
    cacheEntries: window.caches ? await window.caches.open('solari-jsx-transform-v2').then((cache) => cache.keys().then((keys) => keys.length)).catch(() => 0) : 0,
    cacheKeyParams: window.caches ? await window.caches.open('solari-jsx-transform-v2').then((cache) => cache.keys().then((keys) => keys.map((key) => Array.from(new URL(key.url).searchParams.keys())))).catch(() => []) : [],
    workerTransform: window.__solariTransformWorkerUsed === true,
    renderedOfflineFixture: (document.querySelector('#root')?.innerText || '').includes('DEMO DATA'),
  }), label);
  if (errors.length) throw new Error(`${label} boot produced page errors: ${errors.join(' | ')}`);
  if (!result.renderedOfflineFixture) throw new Error(`${label} did not render the offline fixture`);
  return result;
}

function summary(result) {
  return {
    appReadyMs: result.appReadyMs,
    firstPaintMs: result.paints.find((paint) => paint.name === 'first-contentful-paint')?.ms || null,
    longestMainTaskMs: Math.max(0, ...(result.longTasks || []).map((task) => task.duration)),
    babelRequests: result.babelRequests,
    cacheEntries: result.cacheEntries,
    workerTransform: result.workerTransform,
  };
}

async function main() {
  const server = await startServer();
  let browser;
  try {
    browser = await chromium.launch({
      headless: true,
      args: ['--no-sandbox', '--disable-dev-shm-usage'],
      ...(process.env.SOLARI_CHROMIUM_EXECUTABLE ? { executablePath: process.env.SOLARI_CHROMIUM_EXECUTABLE } : {}),
    });
    const context = await browser.newContext({ viewport: { width: 1440, height: 900 } });
    await context.addInitScript(() => {
      window.__longTasks = [];
      new PerformanceObserver((list) => list.getEntries().forEach((entry) => {
        window.__longTasks.push({ start: Math.round(entry.startTime), duration: Math.round(entry.duration) });
      })).observe({ type: 'longtask', buffered: true });
    });
    let cspShellResponses = 0;
    await context.route('**/index.html', async (route) => {
      const response = await route.fetch();
      cspShellResponses += 1;
      await route.fulfill({
        response,
        headers: {
          ...response.headers(),
          'content-security-policy': "default-src 'self'; base-uri 'self'; object-src 'none'; frame-ancestors 'self'; form-action 'self'; script-src 'self' 'unsafe-eval'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; font-src 'self'; connect-src 'self'; worker-src 'self'; frame-src 'self'",
        },
      });
    });
    await context.route('**/api/**', (route) => route.fulfill({ status: 401, body: 'offline profile fixture' }));

    const coldPage = await context.newPage();
    await coldPage.goto(`${server.origin}/manifest.webmanifest`, { waitUntil: 'load' });
    await coldPage.evaluate(async () => {
      const oldShell = await caches.open('solari-v10-shell');
      await oldShell.put(new Request(`${location.origin}/old-shell-fixture`), new Response('old shell'));
    });
    const cold = await profile(coldPage, server.origin, 'cold transform cache');
    if (cspShellResponses === 0) throw new Error('dashboard profile did not exercise the deployed Content-Security-Policy');
    const globalScriptScope = await coldPage.evaluate(() => {
      (0, window.eval)('var __solariGlobalScopeProbe = 814;');
      return window.__solariGlobalScopeProbe === 814 && (0, window.eval)('__solariGlobalScopeProbe') === 814;
    });
    if (!globalScriptScope) throw new Error('compiled classic scripts did not retain global declaration scope');
    if (cold.cacheEntries !== declaredJsxModules) throw new Error(`expected ${declaredJsxModules} cached JSX transforms after cold boot, got ${cold.cacheEntries}`);
    if (cold.cacheKeyParams.some((params) => params.length !== 1 || params[0] !== '__solari_compiled')) throw new Error('compiled cache keys must exclude source cache-buster query parameters');
    if (!cold.workerTransform) throw new Error('cold transforms did not run in the Web Worker');

    // Registration occurs during source execution. Wait for activation so the
    // second navigation exercises the same service-worker-controlled path.
    await coldPage.evaluate(async () => { if ('serviceWorker' in navigator) await navigator.serviceWorker.ready; });
    await coldPage.waitForFunction(async () => {
      const names = await caches.keys();
      return !names.includes('solari-v10-shell') && names.includes('solari-jsx-transform-v2');
    }, { timeout: 15000 });
    const serviceWorkerCaches = await coldPage.evaluate(async () => {
      const names = await caches.keys();
      return {
        oldShellCacheRemoved: !names.includes('solari-v10-shell'),
        transformCachePreserved: names.includes('solari-jsx-transform-v2'),
      };
    });
    if (!serviceWorkerCaches.oldShellCacheRemoved || !serviceWorkerCaches.transformCachePreserved) {
      throw new Error(`service worker cache upgrade failed: ${JSON.stringify(serviceWorkerCaches)}`);
    }
    const warmPage = await context.newPage();
    const warm = await profile(warmPage, server.origin, 'warm transform cache');
    if (warm.babelRequests !== 0) throw new Error('warm boot unexpectedly loaded Babel');
    if (warm.cacheEntries !== declaredJsxModules) throw new Error(`expected ${declaredJsxModules} cached JSX transforms on warm boot, got ${warm.cacheEntries}`);
    if (warm.cacheKeyParams.some((params) => params.length !== 1 || params[0] !== '__solari_compiled')) throw new Error('warm compiled cache keys must exclude source cache-buster query parameters');
    if (warm.workerTransform) throw new Error('warm boot unexpectedly started the transform worker');

    const routeErrors = [];
    warmPage.on('pageerror', (error) => routeErrors.push(String(error)));
    const interactionLatencyMs = {};
    let actionStarted = Date.now();
    await warmPage.locator('.nav-item').filter({ hasText: 'RackWire' }).click();
    await warmPage.locator('.rw-app, iframe').first().waitFor({ state: 'visible', timeout: 10000 });
    interactionLatencyMs.rackWire = Date.now() - actionStarted;
    actionStarted = Date.now();
    await warmPage.locator('.nav-item').filter({ hasText: 'Panel' }).click();
    await warmPage.waitForFunction(() => Array.from(document.querySelectorAll('.page-title')).some((node) => node.textContent.trim() === 'Panel'), { timeout: 10000 });
    interactionLatencyMs.panel = Date.now() - actionStarted;
    if (routeErrors.length) throw new Error(`dashboard route smoke produced page errors: ${routeErrors.join(' | ')}`);

    // A present cache entry whose response body cannot be read is still a
    // transform miss: Babel must load before the fallback transform executes.
    const unreadableCachePage = await context.newPage();
    await unreadableCachePage.addInitScript(() => {
      const store = window.caches;
      const originalOpen = store.open.bind(store);
      store.open = async function (name) {
        const cache = await originalOpen(name);
        if (name !== 'solari-jsx-transform-v2') return cache;
        return {
          keys: () => cache.keys(),
          put: (...args) => cache.put(...args),
          delete: (...args) => cache.delete(...args),
          match: async (...args) => {
            const response = await cache.match(...args);
            return response ? { text: () => Promise.reject(new Error('simulated cached response body failure')) } : null;
          },
        };
      };
    });
    const unreadableCache = await profile(unreadableCachePage, server.origin, 'unreadable transform cache');
    if (!unreadableCache.workerTransform) throw new Error('unreadable transform cache did not use the worker transform fallback');

    const unsupportedWorkerPage = await context.newPage();
    await unsupportedWorkerPage.addInitScript(() => {
      Object.defineProperty(window, 'Worker', { configurable: true, value: undefined });
      const store = window.caches;
      store.open = () => Promise.reject(new Error('simulated CacheStorage failure'));
    });
    const unsupportedWorker = await profile(unsupportedWorkerPage, server.origin, 'worker and cache unavailable');
    if (unsupportedWorker.workerTransform || unsupportedWorker.babelRequests === 0) {
      throw new Error('unavailable Web Worker/CacheStorage did not fall back to main-thread Babel');
    }

    const failedWorkerPage = await context.newPage();
    await failedWorkerPage.addInitScript(() => {
      const originalSetTimeout = window.setTimeout.bind(window);
      window.setTimeout = (callback, delay, ...args) => originalSetTimeout(callback, delay === 45000 ? 10 : delay, ...args);
      Object.defineProperty(window, 'Worker', {
        configurable: true,
        value: function () {
          return {
            terminate() {},
            postMessage() {},
          };
        },
      });
      window.caches.open = () => Promise.reject(new Error('simulated CacheStorage failure'));
    });
    const failedWorker = await profile(failedWorkerPage, server.origin, 'transform worker timeout');
    if (failedWorker.workerTransform || failedWorker.babelRequests === 0) {
      throw new Error('transform worker timeout did not fall back to main-thread Babel');
    }
    console.log(JSON.stringify({
      cold: summary(cold),
      warm: summary(warm),
      unreadableCache: summary(unreadableCache),
      unsupportedWorker: summary(unsupportedWorker),
      failedWorker: summary(failedWorker),
      serviceWorkerCaches,
      routesSmoke: 'RackWire and Panel mounted',
      interactionLatencyMs,
      routeErrors,
    }, null, 2));
  } finally {
    if (browser) await browser.close();
    server.child.kill('SIGTERM');
  }
}

main().catch((error) => { console.error(error); process.exitCode = 1; });
