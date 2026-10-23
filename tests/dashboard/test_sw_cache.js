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

const oldPrivate = new Response('{"ok":true,"data":{"session":"previous"}}');
const shell = new Response("<html>offline shell</html>");
const cache = {
  add: async () => {},
  put: async () => { cacheWrites++; },
  match: async () => { cacheReads++; return shell; },
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
    keys: async () => ["solari-v6-api", "solari-v6-shell", "solari-v7-shell", "solari-v8-shell", "solari-v9-shell"],
    delete: async (key) => { deleted.push(key); return true; },
    match: async () => oldPrivate,
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
  assert.equal(cacheWrites, 0, "API responses must never enter Cache Storage");
  assert.equal(lastFetchOptions.cache, "no-store", "bypass HTTP cache too");

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
  assert.deepEqual(deleted.sort(), ["solari-v6-api", "solari-v6-shell", "solari-v7-shell", "solari-v8-shell", "solari-v9-shell"]);

  console.log("service worker API privacy and shell fallback passed");
})().catch((error) => { console.error(error); process.exitCode = 1; });
