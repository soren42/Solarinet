/* Exercise the browser adapter with a real first fetch, outage, and recovery. */
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const source = fs.readFileSync(path.join(__dirname, "../../dashboard/public/api.jsx"), "utf8");
let failing = false;
let writes = 0;
const browser = {
  console,
  Promise,
  Date,
  URLSearchParams,
  setTimeout,
  clearTimeout,
  document: { querySelector: () => null },
  navigator: { serviceWorker: null },
  addEventListener: () => {},
  SOLARI_OFFLINE: false,
  SOLARI: { fmt: {}, nodes: [], rules: [], inventory: {} },
  fetch: async (_url, options) => {
    if (options && options.method && options.method !== "GET") writes++;
    if (failing) throw new Error("network unavailable");
    return { ok: true, status: 200, json: async () => ({ ok: true, data: {} }) };
  },
};
browser.window = browser;
vm.runInNewContext(source, browser, { filename: "api.jsx" });
(async () => {
  await browser.solariReady;
  if (browser.SOLARI.source !== "live" || !browser.SOLARI.lastLiveAt) {
    throw new Error("first successful fetch should mark live data with a timestamp");
  }
  failing = true;
  await browser.SOLARI.api.refresh().then(() => { throw new Error("refresh should fail"); }, () => {});
  if (browser.SOLARI.source !== "stale" || !browser.SOLARI.lastLiveAt) {
    throw new Error("failed refresh should preserve and label the last real snapshot");
  }
  await browser.SOLARI.api.survey("all").then(() => {
    throw new Error("mutation should be blocked while stale");
  }, () => {});
  if (writes !== 0) throw new Error("blocked mutation reached fetch");
  failing = false;
  await browser.SOLARI.api.refresh();
  if (browser.SOLARI.source !== "live" || browser.SOLARI.staleReason !== null) {
    throw new Error("recovery should restore live state");
  }
  console.log("API freshness and mutation gate passed");
})().catch((error) => { console.error(error); process.exitCode = 1; });
