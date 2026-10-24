#!/usr/bin/env node
/* Exercise the browser service worker's privacy boundary without a server. */
"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const handlers = {};
const deleted = [];
let cacheReads = 0;
let cacheWrites = 0;
let networkOk = false;
let lastFetchOptions;
let lastCacheMatchOptions;
let lastGlobalMatchOptions;
let globalCacheReads = 0;

const oldPrivate = new Response('{"ok":true,"data":{"session":"previous"}}');
const cache = {
  add: async () => {},
  put: async () => { cacheWrites++; },
  match: async (_request, options) => { cacheReads++; lastCacheMatchOptions = options; return new Response("<html>offline shell</html>"); },
};
const context = {
  self: {
    location: { origin: "https://dashboard.akoria.net" },
    addEventListener: (name, fn) => { handlers[name] = fn; },
    clients: { claim: async () => {} },
    skipWaiting: async () => {},
  },
  caches: {
    open: async () => cache,
    keys: async () => ["solari-v6-api", "solari-v6-shell", "solari-v7-shell", "solari-v8-shell", "solari-v9-shell", "solari-v10-shell", "solari-v11-shell", "solari-v12-shell", "solari-v13-shell", "solari-jsx-transform-v2"],
    delete: async (key) => { deleted.push(key); return true; },
    match: async (request, options) => {
      globalCacheReads++;
      lastGlobalMatchOptions = options;
      return new URL(request.url).pathname.startsWith("/api/") ? oldPrivate : new Response("cached vendor");
    },
  },
  fetch: async (_request, options) => {
    lastFetchOptions = options;
    if (!networkOk) throw new Error("network offline");
    return new Response('{"ok":true,"data":{"session":"current"}}');
  },
  Request, Response, URL, console,
};

vm.runInNewContext(
  fs.readFileSync(path.join(__dirname, "../../dashboard/public/sw.js"), "utf8"),
  context,
  { filename: "sw.js" }
);

const shellAssets = vm.runInContext("SHELL_ASSETS", context);
const indexHtml = fs.readFileSync(path.join(__dirname, "../../dashboard/public/index.html"), "utf8");
const bootAsset = (indexHtml.match(/<script\s+defer\s+src="([^"]*boot\.js\?v=[^"]+)"/) || [])[1];
const bootSource = fs.readFileSync(path.join(__dirname, "../../dashboard/public/boot.js"), "utf8");
const workerAsset = (bootSource.match(/new URL\("([^"]*transform-worker\.js\?v=[^"]+)"/) || [])[1];
assert.ok(shellAssets.includes(bootAsset), "the service-worker shell must pin the exact boot.js URL used by index.html");
assert.ok(shellAssets.includes(workerAsset), "the service-worker shell must pin the exact transform-worker URL used by boot.js");
assert.equal(new URL(bootAsset, "https://dashboard.akoria.net/").searchParams.get("v"),
  new URL(workerAsset, "https://dashboard.akoria.net/").searchParams.get("v"),
  "boot.js and its worker must use the same shell asset revision");

function dispatchFetch(url) {
  const event = {
    request: new Request(url),
    respondWith(promise) { this.response = promise; },
  };
  handlers.fetch(event);
  assert.ok(event.response, "worker must handle same-origin GET");
  return event.response;
}

(async () => {
  // A prior authenticated response is present, but an offline API read must
  // reject. It must not be returned to a later session or called live.
  await assert.rejects(dispatchFetch("https://dashboard.akoria.net/api/nodes"));
  assert.equal(cacheReads, 0, "offline API must never read cached data");
  assert.equal(globalCacheReads, 0, "offline API must never use global CacheStorage.match");
  assert.equal(lastFetchOptions.cache, "no-store", "bypass HTTP cache too");

  // Versioned application URLs must fall back to their canonical shell paths.
  networkOk = false;
  const offlineShell = await dispatchFetch("https://dashboard.akoria.net/styles.css?v=20261024a");
  assert.equal(await offlineShell.text(), "<html>offline shell</html>");
  assert.equal(lastCacheMatchOptions.ignoreSearch, true, "versioned URLs use the canonical precached shell response");
  const offlineVendor = await dispatchFetch("https://dashboard.akoria.net/vendor/react.production.min.js?v=20260809a");
  assert.equal(await offlineVendor.text(), "cached vendor");
  assert.equal(lastGlobalMatchOptions.ignoreSearch, true, "versioned vendor URLs use the canonical precached response");
  assert.equal(cacheWrites, 0, "API responses must never enter Cache Storage");

  networkOk = true;
  const fresh = await dispatchFetch("https://dashboard.akoria.net/api/nodes");
  assert.match(await fresh.text(), /current/);
  assert.equal(cacheWrites, 0);

  networkOk = false;
  const cachedShell = await dispatchFetch("https://dashboard.akoria.net/api.jsx");
  assert.match(await cachedShell.text(), /offline shell/,
    "app shell remains available offline");

  const activate = { waitUntil(promise) { this.done = promise; } };
  handlers.activate(activate);
  await activate.done;
  assert.deepEqual(deleted.sort(), ["solari-v6-api", "solari-v6-shell", "solari-v7-shell", "solari-v8-shell", "solari-v9-shell", "solari-v10-shell", "solari-v11-shell", "solari-v12-shell", "solari-v13-shell"].sort());
  assert.equal(deleted.includes("solari-jsx-transform-v2"), false, "source-addressed JSX transforms survive shell upgrades");

  console.log("service worker API privacy and shell fallback passed");
})().catch((error) => { console.error(error); process.exitCode = 1; });
