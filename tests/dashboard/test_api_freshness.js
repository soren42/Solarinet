/* Exercise the browser adapter with a real first fetch, outage, and recovery. */
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const source = fs.readFileSync(path.join(__dirname, "../../dashboard/public/api.jsx"), "utf8");
let failing = false;
let failInventory = false;
let oversizedInventory = false;
let changingInventory = false;
let inventoryRequests = 0;
let writes = 0;
const browser = {
  console,
  Promise,
  Date,
  URL,
  URLSearchParams,
  setTimeout,
  clearTimeout,
  document: { querySelector: () => null },
  navigator: { serviceWorker: null },
  addEventListener: () => {},
  SOLARI_OFFLINE: false,
  SOLARI: { fmt: {}, nodes: [], rules: [], inventory: {} },
  fetch: async (url, options) => {
    if (options && options.method && options.method !== "GET") writes++;
    if (failing) throw new Error("network unavailable");
    if (failInventory && String(url).includes("/api/inventory")) throw new Error("inventory unavailable");
    const parsed = new URL(String(url), "http://dashboard.test");
    if (parsed.pathname.startsWith("/api/inventory/")) inventoryRequests++;
    let data = {};
    if (parsed.pathname.startsWith("/api/inventory/")) {
      const offset = Number(parsed.searchParams.get("offset") || 0);
      let all = [];
      if (parsed.pathname === "/api/inventory/models") all = [{ id: 7, modelId: 7, name: "Retained model" }];
      if (parsed.pathname === "/api/inventory/components") {
        all = Array.from({ length: 1001 }, (_, index) => ({ id: index + 12, unitId: index + 12, modelId: 7 }));
      }
      const total = parsed.pathname === "/api/inventory/components" && oversizedInventory
        ? 100001
        : parsed.pathname === "/api/inventory/components" && changingInventory && offset > 0
          ? 1002
          : all.length;
      const items = all.slice(offset, offset + 1000);
      const hasMore = offset + items.length < total;
      data = { items, page: { limit: 1000, offset, total, hasMore, nextOffset: hasMore ? offset + items.length : null } };
    } else if (parsed.pathname === "/api/inventory/locations") {
      data = { items: [], page: { limit: 1000, offset: 0, total: 0, hasMore: false, nextOffset: null } };
    }
    return { ok: true, status: 200, json: async () => ({ ok: true, data }) };
  },
};
browser.window = browser;
vm.runInNewContext(source, browser, { filename: "api.jsx" });
(async () => {
  await browser.solariReady;
  if (browser.SOLARI.source !== "live" || !browser.SOLARI.lastLiveAt) {
    throw new Error("first successful fetch should mark live data with a timestamp");
  }
  const inventoryFreshAt = browser.SOLARI.freshness.inventory.lastSuccess;
  if (browser.SOLARI.freshness.inventory.state !== "fresh" || !inventoryFreshAt) {
    throw new Error("successful optional endpoint should expose its freshness");
  }
  failInventory = true;
  await browser.SOLARI.api.refresh();
  const inventoryStale = browser.SOLARI.freshness.inventory;
  if (browser.SOLARI.source !== "live" || inventoryStale.state !== "stale" || inventoryStale.lastSuccess !== inventoryFreshAt) {
    throw new Error("optional endpoint failure should preserve core live state and last success");
  }
  if (browser.SOLARI.inventory.models[0].name !== "Retained model" || browser.SOLARI.inventory.units[0].unitId !== 12) {
    throw new Error("optional endpoint failure should retain the last successful payload");
  }
  if (browser.SOLARI.inventory.units.length !== 1001 || browser.SOLARI.inventory.units[1000].unitId !== 1012) {
    throw new Error("inventory adapter should combine validated pages without dropping rows");
  }
  failInventory = false;
  await browser.SOLARI.api.refresh();
  if (browser.SOLARI.freshness.inventory.state !== "fresh" || browser.SOLARI.freshness.inventory.lastSuccess < inventoryFreshAt) {
    throw new Error("optional endpoint recovery should report fresh data");
  }
  const beforeCacheRead = inventoryRequests;
  await browser.SOLARI.api.inventory();
  if (inventoryRequests !== beforeCacheRead) throw new Error("inventory snapshot cache should serve a recent read");
  await browser.SOLARI.api.createUnit({});
  const afterMutation = inventoryRequests;
  await browser.SOLARI.api.inventory();
  if (inventoryRequests <= afterMutation) throw new Error("inventory writes should invalidate the shared snapshot");
  writes = 0;
  oversizedInventory = true;
  await browser.SOLARI.api.inventory(true).then(() => { throw new Error("oversized inventory should be rejected"); }, (error) => {
    if (error.code !== "INVENTORY_TOO_LARGE") throw error;
  });
  oversizedInventory = false;
  changingInventory = true;
  await browser.SOLARI.api.inventory(true).then(() => { throw new Error("inventory changed between pages should be rejected"); }, (error) => {
    if (error.code !== "INVENTORY_CHANGED") throw error;
  });
  changingInventory = false;
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
