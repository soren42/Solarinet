#!/usr/bin/env node
"use strict";

// A CacheStorage enumeration failure must not prevent the current worker from
// activating. Cache cleanup is best-effort; the worker can retry on a later
// install and shell requests remain network-first.
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const vm = require("node:vm");

const handlers = {};
let claimed = false;
let warning = "";
const context = {
  self: {
    location: { origin: "https://dashboard.akoria.net" },
    addEventListener: (name, handler) => { handlers[name] = handler; },
    clients: { claim: async () => { claimed = true; } },
    skipWaiting: async () => {},
  },
  caches: {
    keys: async () => { throw new Error("simulated CacheStorage enumeration failure"); },
    open: async () => { throw new Error("not used by activation"); },
    delete: async () => true,
    match: async () => undefined,
  },
  Request,
  Response,
  URL,
  console: { warn: (...args) => { warning = args.join(" "); }, log() {} },
  fetch: async () => { throw new Error("offline"); },
};

vm.runInNewContext(
  fs.readFileSync(path.join(__dirname, "../../dashboard/public/sw.js"), "utf8"),
  context,
  { filename: "sw.js" },
);

(async () => {
  const activation = { waitUntil(promise) { this.done = promise; } };
  handlers.activate(activation);
  await activation.done;
  assert.equal(claimed, true, "activation should claim clients after cache enumeration fails");
  assert.match(warning, /cache activation cleanup incomplete/, "failure should be observable in worker logs");
  console.log("service worker cache failure contract passed: activation continues after CacheStorage enumeration failure");
})().catch((error) => { console.error(error.stack || error); process.exitCode = 1; });
