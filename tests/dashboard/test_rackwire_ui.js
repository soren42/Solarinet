#!/usr/bin/env node
/* test_rackwire_ui.js — CONTRACT-RW.md §6.
 *
 * Two checks:
 *   1. Parse dashboard/public/screens-rackwire.jsx with the dashboard's own
 *      vendored Babel standalone (pattern: test_jsx_parse.js) — a pass means
 *      the SPA screen will not blank out on a syntax error.
 *   2. Sanity-check the CONTRACT-RW §0 D9 token bridge in
 *      dashboard/public/rackwire/index.html: every `--sn-*` custom property
 *      the app REFERENCES (CSS `var(--sn-x, ...)`, or the runtime `tok()`
 *      helper's `getComputedStyle(...).getPropertyValue('--sn-' + k)`) must
 *      be DEFINED somewhere in the bridge's `:root` blocks. A token used but
 *      never bridged would silently fall back to its hardcoded literal even
 *      when a dashboard theme value exists — exactly the regression D9 exists
 *      to prevent. Cable colors use separate `--rw-cable-*` brand literals and
 *      must not be rethemed by the host.
 *
 * Run: node tests/dashboard/test_rackwire_ui.js   — exits non-zero on failure.
 */
"use strict";

var fs = require("fs");
var vm = require("vm");
var path = require("path");

var PUB = path.join(__dirname, "..", "..", "dashboard", "public");
var DASHBOARD_INDEX = path.join(PUB, "index.html");
var RW_INDEX = path.join(PUB, "rackwire", "index.html");
var SCREEN = "screens-rackwire.jsx";

var pass = 0, fail = 0;
var rackWireBabel = null;
function ok(label, cond, extra) {
  if (cond) { pass++; console.log("  ok   - " + label); }
  else { fail++; console.log("  FAIL - " + label + (extra ? "  [" + extra + "]" : "")); }
}
function readRackWireIndex(section) {
  try { return fs.readFileSync(RW_INDEX, "utf8"); }
  catch (e) {
    if (e && e.code === "ENOENT") {
      const inCI = !!process.env.CI && process.env.CI !== "0";
      ok(section + (inCI ? " requires the RackWire submodule in CI" : " skipped because RackWire submodule is absent"), !inCI);
      return null;
    }
    ok(section + " can read RackWire index", false, String(e.message).split("\n")[0]);
    return null;
  }
}

/* ---------------------------------------------------------------- [1] --- */
console.log("[1] Babel parse — " + SCREEN);
{
  var sandbox = { console: console, process: process, setTimeout: setTimeout, clearTimeout: clearTimeout };
  sandbox.window = sandbox;
  sandbox.self = sandbox;
  sandbox.global = sandbox;
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(PUB, "vendor/babel.min.js"), "utf8"), sandbox, { filename: "babel.min.js" });
  var Babel = sandbox.Babel;
  if (!Babel) {
    ok("vendor/babel.min.js exposes Babel", false);
  } else {
    rackWireBabel = Babel;
    var dashboardSource = "";
    var rackwireSource = readRackWireIndex("RackWire cache revision check");
    try { dashboardSource = fs.readFileSync(DASHBOARD_INDEX, "utf8"); }
    catch (e) { ok("dashboard index is readable", false, String(e.message).split("\n")[0]); }
    try {
      var src = fs.readFileSync(path.join(PUB, SCREEN), "utf8");
      Babel.transform(src, { presets: ["react"], filename: SCREEN });
      ok(SCREEN + " parses clean", true);
      ok("view toggle is a named button group",
        /className="seg" role="group" aria-label="Planner view"/.test(src));
      ok("Map and Table expose their pressed state",
        /aria-pressed=\{view === "map"\}/.test(src) &&
        /aria-pressed=\{view === "table"\}/.test(src));
      ok("planner readiness is announced as a polite status",
        /role="status" aria-live="polite" aria-atomic="true"/.test(src) &&
        /Planner ready\./.test(src));
      ok("host requests the current plan and consumes its reply",
        /send\("getPlan"\)/.test(src) &&
        /m\.event === "reply" && m\.payload && m\.payload\.cmd === "getPlan"/.test(src));
      ok("selected device chips show a readable name and preserve the ID",
        /devNames\[uid\] \|\| uid/.test(src) &&
        /title=\{`Device ID: \$\{uid\}`\}/.test(src));
      ok("embedded planner is labeled for network, power, and interconnect work",
        /title="RackWire network, power, and interconnect planner"/.test(src));
      var revisionMatch = /const RW_V = "([^"]+)";/.exec(src);
      var dashboardRevision = /<script type="application\/x-solarinet-jsx" data-presets="[^"]+" data-plugins="[^"]+" src="screens-rackwire\.jsx\?v=([^"]+)"/.exec(dashboardSource);
      var expectedRuntimeAssets = ["./support.js", "device-library.js", "devices/generic.js", "devices/ubiquiti.js", "devices/netgear.js", "devices/netool.js", "devices/apple.js", "devices/samsung.js", "devices/raspberry-pi.js", "devices/qualcomm-arduino.js", "devices/m5stack.js", "devices/eos.js", "devices/minisforum.js", "live-adapter.js"];
      if (rackwireSource !== null) {
        ok("pinned RackWire client sends the compare-and-save hash required by the API",
          /if \(this\._planServerHash\) body\.expectedHash = this\._planServerHash/.test(rackwireSource));
        ok("the embedded RackWire source pin contains the current compare-and-save protocol",
          !!revisionMatch && revisionMatch[1] === "20261023a" && rackwireSource.indexOf("support.js?v=20261023a") !== -1);
        var rackwireMarkup = rackwireSource.replace(/<!--[\s\S]*?-->/g, "");
        var runtimeAssets = Array.from(rackwireMarkup.matchAll(/<script\b([^>]*)>/gi)).map(function (m) {
          var source = /(?:^|\s)src="([^"]+)"/i.exec(m[1]);
          if (!source) return null;
          var htmlSrc = source[1].replace(/&amp;/g, "&");
          var scriptUrl = new URL(htmlSrc, "https://rackwire.invalid/");
          return [htmlSrc.split(/[?#]/)[0], scriptUrl.searchParams.get("v")];
        }).filter(Boolean);
        ok("dashboard and RackWire runtime assets share one cache revision",
          !!revisionMatch && !!dashboardRevision && dashboardRevision[1] === revisionMatch[1] &&
          runtimeAssets.length === expectedRuntimeAssets.length &&
          expectedRuntimeAssets.every(function (asset) { return runtimeAssets.some(function (entry) { return entry[0] === asset && entry[1] === revisionMatch[1]; }); }) &&
          runtimeAssets.every(function (entry) { return expectedRuntimeAssets.indexOf(entry[0]) !== -1 && entry[1] === revisionMatch[1]; }),
          JSON.stringify({ planner: revisionMatch && revisionMatch[1], dashboard: dashboardRevision && dashboardRevision[1], runtime: runtimeAssets }));
      }
      ok("embedded frame and standalone link share the RackWire cache revision",
        /const RW_V = "[^"]+";/.test(src) &&
        /href=\{`rackwire\/index\.html\?v=\$\{RW_V\}`\}/.test(src) &&
        /src=\{`rackwire\/index\.html\?embed=1&solarinet=1&v=\$\{RW_V\}`\}/.test(src));
    } catch (e) {
      ok(SCREEN + " parses clean", false, String(e.message).split("\n")[0]);
    }
  }
}

/* ------------------------------------------------------ [1b] bridge smoke --- */
console.log("\n[1b] embedded selection labels — existing getPlan request/reply bridge");
if (!rackWireBabel) {
  ok("RackWire screen bridge loads", false, "Babel is unavailable");
} else {
  var slots = [], hookIndex = 0, effects = [], listeners = {}, sent = [];
  var frameWindow = { postMessage: function (message) { sent.push(message); } };
  var React = {
    createElement: function (type, props) {
      return { type: type, props: props || {}, children: Array.prototype.slice.call(arguments, 2) };
    },
    useRef: function (initial) {
      var i = hookIndex++;
      if (!slots[i]) slots[i] = { current: initial };
      return slots[i];
    },
    useState: function (initial) {
      var i = hookIndex++;
      if (!(i in slots)) slots[i] = initial;
      return [slots[i], function (next) { slots[i] = typeof next === "function" ? next(slots[i]) : next; }];
    },
    useCallback: function (callback) { hookIndex++; return callback; },
    useEffect: function (effect) { hookIndex++; effects.push(effect); }
  };
  var win = {
    location: { origin: "https://solarinet.test" },
    addEventListener: function (name, callback) { listeners[name] = callback; },
    removeEventListener: function (name) { delete listeners[name]; }
  };
  var doc = { documentElement: { getAttribute: function () { return "dark"; } } };
  var bridge = { React: React, window: win, document: doc, console: console,
    getComputedStyle: function () { return { getPropertyValue: function () { return ""; } }; },
    MutationObserver: function () { this.observe = function () {}; this.disconnect = function () {}; } };
  vm.createContext(bridge);
  try {
    var transformed = rackWireBabel.transform(fs.readFileSync(path.join(PUB, SCREEN), "utf8"),
      { presets: ["react"], filename: SCREEN }).code;
    vm.runInContext(transformed, bridge, { filename: SCREEN });
    hookIndex = 0;
    var navRequests = [];
    var screenProps = { onNavigate: function (route) { navRequests.push(route); } };
    win.RackWireScreen(screenProps);
    slots[0].current = { contentWindow: frameWindow };
    effects.forEach(function (effect) { effect(); });
    listeners.message({ origin: win.location.origin, source: frameWindow,
      data: { source: "rackwire", event: "ready", payload: { version: "1.0" } } });
    ok("ready handshake requests the current plan", sent.some(function (m) { return m.cmd === "getPlan"; }));
    listeners.message({ origin: win.location.origin, source: frameWindow,
      data: { source: "rackwire", event: "reply", payload: { cmd: "getPlan", value: {
        devices: [{ uid: "dev-17", name: "Laboratory switch" }]
      } } } });
    listeners.message({ origin: win.location.origin, source: frameWindow,
      data: { source: "rackwire", event: "select", payload: ["dev-17"] } });
    listeners.message({ origin: win.location.origin, source: frameWindow,
      data: { source: "rackwire", event: "navigate", payload: { route: "discovery" } } });
    hookIndex = 0;
    var tree = win.RackWireScreen(screenProps);
    function findChip(node) {
      if (!node) return null;
      if (Array.isArray(node)) {
        for (var i = 0; i < node.length; i++) { var hit = findChip(node[i]); if (hit) return hit; }
        return null;
      }
      if (typeof node !== "object") return null;
      if (node.type === "button" && node.props.className === "chip") return node;
      return findChip(node.children);
    }
    var chip = findChip(tree);
    ok("selection chip displays the plan device name", !!chip && chip.children[0] === "Laboratory switch");
    ok("selection chip retains its UID in the title", !!chip && chip.props.title === "Device ID: dev-17");
    ok("embedded discovery navigation reaches the dashboard route", navRequests.indexOf("discovery") >= 0);
    var navButtons = [];
    var scanButton = null;
    (function walk(node) {
      if (!node) return;
      if (Array.isArray(node)) return node.forEach(walk);
      if (typeof node !== "object") return;
      if (node.type === "button" && (node.children || []).some(function (child) { return child === "Network discovery" || child === "Inventory"; })) navButtons.push(node);
      if (node.type === "button" && (node.children || []).some(function (child) { return child === "Scan inventory barcode"; })) scanButton = node;
      walk(node.children);
    })(tree);
    ok("RackWire exposes cross-links to Network discovery and Inventory", navButtons.length === 2);
    ok("embedded RackWire exposes a barcode scan action", !!scanButton);
    if (scanButton) {
      scanButton.props.onClick();
      ok("barcode scan action uses the guarded iframe bridge command", sent.some(function (m) { return m.cmd === "openScan"; }));
    }
    navButtons.forEach(function (button) { button.props.onClick(); });
    ok("RackWire cross-links navigate only to the intended dashboard sections", navRequests.indexOf("inventory") >= 0 && navRequests.filter(function (route) { return route === "discovery" || route === "inventory"; }).length === 3);
    ok("dashboard embed explicitly selects the SolariNet discovery bridge", /rackwire\/index\.html\?embed=1&solarinet=1/.test(fs.readFileSync(path.join(PUB, SCREEN), "utf8")));
  } catch (e) {
    ok("embedded selection label bridge completes", false, String(e.message).split("\n")[0]);
  }
}

/* ---------------------------------------------------------------- [2] --- */
console.log("\n[2] token bridge — every --sn-* token used is bridged to a dashboard token (dashboard/public/rackwire/index.html)");
{
  var html;
  try {
    html = fs.readFileSync(RW_INDEX, "utf8");
  ok("standalone toolbar states RackWire's broad planning scope",
    /NETWORK · POWER · INTERCONNECT PLANNING/.test(html));
  } catch (e) {
    if (e && e.code === "ENOENT") {
      const inCI = !!process.env.CI && process.env.CI !== "0";
      ok("RackWire token-bridge submodule is present" + (inCI ? " in CI" : " for this checkout"), !inCI);
    } else {
      ok("dashboard/public/rackwire/index.html is readable", false, e.message);
    }
    html = null;
  }

  if (html !== null) {
    var lookupStart = html.indexOf('async lookupCode(code, timeoutMs)');
    var lookupEnd = html.indexOf('\n  scanGo()', lookupStart);
    var lookupBlock = lookupStart >= 0 && lookupEnd > lookupStart ? html.slice(lookupStart, lookupEnd) : '';
    ok("SoR barcode lookup sends the session-bound CSRF token",
      /rwSessionCsrf\(false(?:,\s*signal)?\)/.test(lookupBlock) && /X-Solari-CSRF/.test(lookupBlock));
    ok("SoR barcode lookup refreshes an expired CSRF token once",
      /r\.status === 403[\s\S]*?rwSessionCsrf\(true(?:,\s*signal)?\)/.test(lookupBlock));
    // Isolate the token-bridge <style> block's :root sections (D9's home),
    // so "defined" only counts real custom-property declarations, not every
    // var(--sn-x) usage further down the same block.
    var styleMatch = html.match(/<style>([\s\S]*?)<\/style>/);
    ok("index.html has a <style> block", !!styleMatch);
    var styleBlock = styleMatch ? styleMatch[1] : "";

    var rootBlocks = styleBlock.match(/:root(?:\[[^\]]*\])?\s*\{[^}]*\}/g) || [];
    ok("token bridge defines at least one :root block", rootBlocks.length >= 1, "found " + rootBlocks.length);

    // defined: token name -> every declaration VALUE seen for it across the
    // :root blocks (e.g. "var(--bg, #070A0F)"), so bridging can be checked
    // per-declaration, not just presence.
    var defined = new Map();
    rootBlocks.forEach(function (block) {
      var re = /(--sn-[a-z][a-z0-9-]*)\s*:\s*([^;]+);/g;
      var m;
      while ((m = re.exec(block)) !== null) {
        var vals = defined.get(m[1]) || [];
        vals.push(m[2].trim());
        defined.set(m[1], vals);
      }
    });
    ok("bridge defines a non-trivial token set", defined.size >= 15, "got " + defined.size);

    // Every --sn-* token must actually BRIDGE to a dashboard token —
    // var(--<dashboard-token>, <fallback>) — not just declare something.
    // A bare literal standing in for a real token (e.g. a hardcoded hex where
    // var(--accent, #22B8F0) belongs) would satisfy "is defined" but is
    // exactly the D9 regression this test exists to catch, so check the
    // declaration shape, not just its presence.
    var BRIDGE_RE = /^var\(\s*--[a-z][a-z0-9-]*\s*,\s*[\s\S]+\)$/;
    defined.forEach(function (vals, token) {
      var unbridged = vals.filter(function (v) { return !BRIDGE_RE.test(v); });
      ok(token + " every declaration bridges via var(--<dashboard-token>, <fallback>)",
        unbridged.length === 0, unbridged.join(" | "));
    });

    // Referenced set: every --sn-* the file actually reads, from both static
    // var(--sn-x, ...) usage and the runtime tok() helper's dynamic
    // getPropertyValue('--sn-' + k) construction (g('key', fallback) calls).
    var referenced = new Set();
    var varRe = /--sn-[a-z][a-z0-9-]*/g;
    var vm2;
    while ((vm2 = varRe.exec(html)) !== null) referenced.add(vm2[0]);

    var gCallRe = /\bg\(\s*'([a-z0-9-]+)'/g;
    var gm;
    while ((gm = gCallRe.exec(html)) !== null) referenced.add("--sn-" + gm[1]);

    ok("found --sn-* references to check", referenced.size >= 15, "got " + referenced.size);

    var missing = [];
    referenced.forEach(function (t) {
      if (!defined.has(t)) missing.push(t);
    });
    ok("every referenced --sn-* token is bridged", missing.length === 0, missing.join(", "));
    ok("shared-plan saves have a bounded fetch timeout",
      /controller = new AbortController\(\)[\s\S]{0,220}controller\.abort\(\)/.test(html) &&
      /finally \{ if \(timer\) clearTimeout\(timer\); \}/.test(html));
    ok("unsynced local edits are stored with the recoverable plan snapshot",
      /remoteDirty: !!this\._planRemoteDirty/.test(html) && !/localStorage\.setItem\(PLAN_DIRTY_KEY/.test(html));
    ok("existing shared-plan updates carry the server plan hash", /body\.expectedHash = this\._planServerHash/.test(html) && /planHash/.test(html));
    ok("boot retries persisted dirty local plans before reading the shared snapshot",
      /if \(this\._planRemoteDirty && !forceRemote\) \{[\s\S]*?this\.pushPlan\(\);\s*return;\s*\}[\s\S]*?const list = await rwFetch\('\/plans'\)/.test(html));
    ok("server hydration cannot create a false unsynced-local marker",
      /this\.persist\(planChanged && !this\._planHold\)/.test(html));
    ok("initial planner status does not claim an unverified server sync",
      /Autosaved locally · shared sync pending/.test(html));
    ok("failed shared-plan saves show status and use capped retry backoff",
      /shared save failed; retrying/.test(html) && /Math\.min\(60000, 3000 \* Math\.pow\(2/.test(html));
    ok("plan conflicts expose separate-copy and archive-then-reload actions",
      /Save local edits as a separate shared plan/.test(html) && /Load shared plan · archive local edits/.test(html) && /this\._planHashConflict \|\|/.test(html));

    // Brand Standards v1.1 and CONTRACT-RW D9 moved cable meaning out of the
    // host bridge. Pin the literals so a dashboard theme cannot repaint them.
    var cablePalette = {
      "--rw-cable-net": "#22D3EE",
      "--rw-cable-power": "#FB923C",
      "--rw-cable-usb": "#2E6BE6",
      "--rw-cable-other": "#74889D",
      "--rw-cable-lighting": "#E879F9",
      "--rw-cable-audio": "#A3E635",
      "--rw-cable-open": "#FFE04D",
    };
    Object.keys(cablePalette).forEach(function (token) {
      var match = styleBlock.match(new RegExp(token + "\\s*:\\s*([^;]+);"));
      ok(token + " keeps the RackWire brand literal", !!match &&
        match[1].trim().toUpperCase() === cablePalette[token].toUpperCase());
    });
  }
}

/* --------------------------------------------------- [3] stale CSRF retry --- */
async function testExpiredCsrfRetry() {
  console.log("\n[3] stale dashboard CSRF token retries once with a refreshed token");
  var html = readRackWireIndex("CSRF retry check");
  if (html === null) return;

  function makeResponse(status, body) {
    return {
      status: status,
      ok: status >= 200 && status < 300,
      json: async function () { return body; },
      clone: function () { return { json: async function () { return body; } }; }
    };
  }
  function makeHarness(responses) {
    var calls = [], refreshes = 0;
    var sandbox = {
      API: "/api",
      AbortController: AbortController,
      setTimeout: setTimeout,
      clearTimeout: clearTimeout,
      rwOnline: function () { return true; },
      rwSessionCsrf: async function (refresh) {
        if (refresh) { refreshes++; return "fresh-token"; }
        return "stale-token";
      },
      fetch: async function (url, options) {
        calls.push({ url: url, options: {
          method: options.method,
          headers: Object.assign({}, options.headers),
          body: options.body,
          credentials: options.credentials
        } });
        var response = responses.shift();
        if (!response) throw new Error("unexpected extra request");
        return response;
      },
      window: { RW: { DEVICES: [{ id: "model-1", name: "Lab switch" }] } }
    };
    vm.createContext(sandbox);
    return { sandbox: sandbox, calls: calls, refreshes: function () { return refreshes; } };
  }

  var csrfStart = html.indexOf("function rwAwaitCsrf(pending, signal) {");
  var csrfEnd = html.indexOf("\n\n/* ── postMessage peer pinning", csrfStart);
  ok("shared CSRF helper source is available for cancellation tests", csrfStart >= 0 && csrfEnd > csrfStart);
  if (csrfStart >= 0 && csrfEnd > csrfStart) {
    var identityResolve = null, identityCalls = 0, identityOptions = null;
    var csrfSandbox = {
      AbortController: AbortController,
      setTimeout: setTimeout, clearTimeout: clearTimeout,
      rwOnline: function () { return true; },
      fetch: function (url, options) {
        identityCalls++;
        identityOptions = options;
        return new Promise(function (resolve, reject) {
          identityResolve = resolve;
          options.signal.addEventListener('abort', function () { reject(new Error('shared identity timeout')); });
        });
      }
    };
    vm.createContext(csrfSandbox);
    vm.runInContext("var rwPlanCsrf = '', rwPlanCsrfPromise = null;\n" +
      html.slice(csrfStart, csrfEnd), csrfSandbox, { filename: "rackwire-csrf-coalescing.js" });
    var cancelOne = new AbortController();
    var cancelledWait = csrfSandbox.rwSessionCsrf(false, cancelOne.signal);
    var sharedWait = csrfSandbox.rwSessionCsrf(false);
    cancelOne.abort();
    var cancelledToken = await cancelledWait;
    identityResolve({ ok: true, json: async function () { return { ok: true, data: { csrfToken: "shared-token" } }; } });
    var sharedToken = await sharedWait;
    ok("aborting one CSRF waiter does not cancel a coalesced caller", cancelledToken === "" && sharedToken === "shared-token");
    ok("coalesced CSRF callers use one bounded identity request independent of caller signals",
      identityCalls === 1 && identityOptions.signal && identityOptions.signal !== cancelOne.signal && !identityOptions.signal.aborted);

    var identityResolvers = [], raceCsrfSandbox = {
      AbortController: AbortController, setTimeout: setTimeout, clearTimeout: clearTimeout,
      rwOnline: function () { return true; },
      fetch: function () { return new Promise(function (resolve) { identityResolvers.push(resolve); }); }
    };
    vm.createContext(raceCsrfSandbox);
    vm.runInContext("var rwPlanCsrf = '', rwPlanCsrfPromise = null, rwPlanCsrfFailureStatus = 401;\n" + html.slice(csrfStart, csrfEnd), raceCsrfSandbox, { filename: "rackwire-csrf-refresh-race.js" });
    var olderIdentity = raceCsrfSandbox.rwSessionCsrf(false);
    ok("starting a fresh identity request clears a stale logged-out status", raceCsrfSandbox.rwPlanCsrfFailureStatus === 0);
    var refreshedIdentity = raceCsrfSandbox.rwSessionCsrf(true);
    identityResolvers[1]({ ok: true, json: async function () { return { ok: true, data: { csrfToken: "new-token" } }; } });
    await refreshedIdentity;
    identityResolvers[0]({ ok: true, json: async function () { return { ok: true, data: { csrfToken: "old-token" } }; } });
    await olderIdentity;
    ok("a late identity response cannot replace the refreshed CSRF token", raceCsrfSandbox.rwPlanCsrf === "new-token");
  }

  var invalidCsrf = { ok: false, error: { message: "Missing or invalid CSRF token." } };
  var planStart = html.indexOf("async function rwFetch(path, body, timeoutMs, onFailure) {");
  var planEnd = html.indexOf("\n\n/* ── RackWire discovery seam", planStart);
  ok("rwFetch helper source is available for behavioral test", planStart >= 0 && planEnd > planStart);
  if (planStart >= 0 && planEnd > planStart) {
    var planHarness = makeHarness([
      makeResponse(403, invalidCsrf),
      makeResponse(200, { ok: true, data: { id: 71 } })
    ]);
    try {
      vm.runInContext(html.slice(planStart, planEnd), planHarness.sandbox, { filename: "rackwire-rwFetch.js" });
      var plan = await planHarness.sandbox.rwFetch("/rackwire/plans", { name: "retry" });
      ok("plan POST succeeds after one expired-token retry", !!plan && plan.id === 71);
      ok("plan POST uses stale then refreshed CSRF tokens exactly once",
        planHarness.calls.length === 2 && planHarness.refreshes() === 1 &&
        planHarness.calls[0].options.headers["X-Solari-CSRF"] === "stale-token" &&
        planHarness.calls[1].options.headers["X-Solari-CSRF"] === "fresh-token");
      ok("plan retry preserves the original request body",
        planHarness.calls[0].options.body === planHarness.calls[1].options.body &&
        JSON.parse(planHarness.calls[1].options.body).name === "retry");
    } catch (e) {
      ok("plan POST retry harness completes", false, String(e.message).split("\n")[0]);
    }

    var timeoutHarness = makeHarness([]), timeoutCallback = null, timeoutSignal = null;
    timeoutHarness.sandbox.AbortController = AbortController;
    timeoutHarness.sandbox.setTimeout = function (fn) { timeoutCallback = fn; return 9; };
    timeoutHarness.sandbox.clearTimeout = function () {};
    timeoutHarness.sandbox.fetch = function (url, options) {
      timeoutSignal = options.signal;
      return new Promise(function (resolve, reject) {
        options.signal.addEventListener("abort", function () { reject(new Error("aborted")); });
      });
    };
    try {
      vm.runInContext(html.slice(planStart, planEnd), timeoutHarness.sandbox, { filename: "rackwire-rwFetch-timeout.js" });
      var timed = timeoutHarness.sandbox.rwFetch("/rackwire/plans", undefined, 25);
      timeoutCallback();
      var timeoutGuard;
      var timedResult = await Promise.race([timed, new Promise(function (resolve) { timeoutGuard = setTimeout(function () { resolve("timeout"); }, 250); })]);
      clearTimeout(timeoutGuard);
      ok("hung shared-plan fetch aborts at its timeout and returns offline", timedResult === null && timeoutSignal && timeoutSignal.aborted);
    } catch (e) {
      ok("fetch timeout harness completes", false, String(e.message).split("\n")[0]);
    }

    var deniedHarness = makeHarness([
      makeResponse(403, invalidCsrf), makeResponse(403, invalidCsrf)
    ]);
    try {
      vm.runInContext(html.slice(planStart, planEnd), deniedHarness.sandbox, { filename: "rackwire-rwFetch-denied.js" });
      var denied = await deniedHarness.sandbox.rwFetch("/rackwire/plans", { name: "retry" });
      ok("plan POST stops after one refreshed-token rejection",
        denied === null && deniedHarness.calls.length === 2 && deniedHarness.refreshes() === 1);
    } catch (e) {
      ok("plan POST bounded retry harness completes", false, String(e.message).split("\n")[0]);
    }
  }

  var lookupStart = html.indexOf("  async lookupCode(code, timeoutMs) {");
  var lookupEnd = html.indexOf("\n  scanGo()", lookupStart);
  ok("barcode lookup method is available for behavioral test", lookupStart >= 0 && lookupEnd > lookupStart);
  if (lookupStart >= 0 && lookupEnd > lookupStart) {
    var lookupHarness = makeHarness([
      makeResponse(403, invalidCsrf),
      makeResponse(200, { ok: true, data: {
        found: true, subjectTable: "hardware_units",
        subject: { modelId: "model-1", assetTag: "ASSET-71", serial: "SER-71" }
      } })
    ]);
    try {
      var lookup = vm.runInContext("({" + html.slice(lookupStart, lookupEnd).trim() + "})",
        lookupHarness.sandbox, { filename: "rackwire-lookupCode.js" });
      var receiver = { _codeExtra: null };
      var device = await lookup.lookupCode.call(receiver, "ASSET-71");
      ok("barcode lookup succeeds after one expired-token retry", !!device && device.id === "model-1");
      ok("barcode retry uses fresh CSRF once and preserves request body",
        lookupHarness.calls.length === 2 && lookupHarness.refreshes() === 1 &&
        lookupHarness.calls[0].options.headers["X-Solari-CSRF"] === "stale-token" &&
        lookupHarness.calls[1].options.headers["X-Solari-CSRF"] === "fresh-token" &&
        lookupHarness.calls[0].options.body === lookupHarness.calls[1].options.body);
      ok("barcode result retains asset tag and serial on the placed device",
        !!receiver._codeExtra && receiver._codeExtra.assetId === "ASSET-71" &&
        receiver._codeExtra.serial === "SER-71");

      var lookupSignal = null;
      var timedSandbox = {
        AbortController: AbortController, setTimeout: setTimeout, clearTimeout: clearTimeout,
        rwOnline: function () { return true; },
        rwSessionCsrf: function (refresh, signal) {
          lookupSignal = signal;
          return new Promise(function (resolve) { signal.addEventListener("abort", function () { resolve(""); }); });
        },
        fetch: async function () { throw new Error("lookup should stop at CSRF identity timeout"); },
        window: { RW: { DEVICES: [] } }
      };
      vm.createContext(timedSandbox);
      var timedLookup = vm.runInContext("({" + html.slice(lookupStart, lookupEnd).trim() + "})",
        timedSandbox, { filename: "rackwire-lookup-timeout.js" });
      var started = Date.now();
      var lookupTimeoutGuard;
      var timedResult = await Promise.race([
        timedLookup.lookupCode.call({ _codeExtra: null }, "ASSET-72", 10),
        new Promise(function (resolve) { lookupTimeoutGuard = setTimeout(function () { resolve("timeout"); }, 250); })
      ]);
      clearTimeout(lookupTimeoutGuard);
      ok("barcode lookup bounds a hung CSRF identity request", timedResult === null && lookupSignal && lookupSignal.aborted && Date.now() - started < 1000);
    } catch (e) {
      ok("barcode lookup retry harness completes", false, String(e.message).split("\n")[0]);
    }
  }
}

async function testAutosaveRecovery() {
  console.log("\n[4] RackWire local save recovery after shared API failure");
  var html = readRackWireIndex("autosave recovery check");
  if (html === null) return;
  var inline = html.match(/<script[^>]*data-dc-script[^>]*>([\s\S]*?)<\/script>/);
  ok("inline RackWire component is available for autosave behavior tests", !!inline);
  if (!inline) return;

  function TestLogic() { this.state = {}; }
  TestLogic.prototype.setState = function (patch, done) {
    this.state = Object.assign({}, this.state, patch);
    if (done) done();
  };
  var saved = new Map(), timers = new Map(), nextTimer = 0, failPlanWrite = true, planFailureStatus = 503, planFailureCode = "", failWhoami = false, failWhoamiTransport = false, delayPlanWrite = false, delayedPlanWriteResolve = null, delayedSecondaryPlanWriteResolve = null, delayedPlanWriteStatus = 409, delayFullPlan = false, delayedFullPlanResolve = null, failFullPlan = false, alternatePlanList = false, calls = [], remoteBootPlan = null, createReplayPlan = null, failPlanList = false, delayPlanList = false, delayedPlanListResolve = null, delayRestore = false, delayedRestoreResolve = null, restorePlanConflict = false, failRemoteSnapshot = false, remoteSnapshotStatus = 503;
  var sandbox = {
    console: console, DCLogic: TestLogic,
    React: { createElement: function () { return {}; } },
    location: { protocol: "http:", origin: "https://solarinet.test", search: "" },
    URLSearchParams: URLSearchParams,
    localStorage: {
      getItem: function (key) { return saved.has(key) ? saved.get(key) : null; },
      setItem: function (key, value) { saved.set(key, String(value)); },
      removeItem: function (key) { saved.delete(key); }
    },
    setTimeout: function (fn, ms) { nextTimer += 1; timers.set(nextTimer, { fn: fn, ms: ms }); return nextTimer; },
    clearTimeout: function (id) { timers.delete(id); },
    fetch: null, prompt: function () { return "Recovered copy"; }, confirm: function () { return true; }, alert: function () {},
    addEventListener: function () {},
    document: { querySelector: function () { return null; } },
    removeEventListener: function () {},
    RWLive: { stop: function () {} },
    window: null
  };
  sandbox.window = sandbox;
  sandbox.parent = sandbox;
  sandbox.RW = { DEVICES: [], CABLES: [] };
  function response(status, data) {
    return { ok: status >= 200 && status < 300, status: status,
      json: async function () { return data; } };
  }
  sandbox.fetch = async function (url, options) {
    calls.push({ url: url, method: options.method || "GET", body: options.body ? JSON.parse(options.body) : null });
    if (url === "/api/auth/whoami") {
      if (failWhoamiTransport) return Promise.reject(new Error("identity network failure"));
      return response(200, { ok: true, data: { csrfToken: failWhoami ? "" : "test-csrf" } });
    }
    if (url === "/api/rackwire/plans" && options.method === "POST") {
      if (delayPlanWrite) return new Promise(function (resolve) {
        var finish = function () { resolve(delayedPlanWriteStatus === 200
          ? response(200, { ok: true, data: { id: 73, name: "Recovered plan", planHash: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" } })
          : response(409, { ok: false, error: { code: "plan_conflict" } })); };
        if (!delayedPlanWriteResolve) delayedPlanWriteResolve = finish;
        else delayedSecondaryPlanWriteResolve = finish;
      });
      if (failPlanWrite) return response(planFailureStatus, {
        ok: false, error: { code: planFailureStatus === 409 ? (planFailureCode || "plan_conflict") : "forbidden" }
      });
      return response(200, { ok: true, data: { id: 73, name: "Recovered plan", planHash: "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa" } });
    }
    if (url === "/api/rackwire/plans" && delayPlanList) {
      return new Promise(function (resolve) {
        delayedPlanListResolve = function () { resolve(response(200, { ok: true, data: [{ id: 73, name: "Recovered plan" }] })); };
      });
    }
    if (url === "/api/rackwire/plans/73") {
      if (failFullPlan) return response(503, { ok: false, error: { code: "service_unavailable" } });
      var fullResponse = function () { return response(200, { ok: true, data: {
        id: 73, name: "Recovered plan", plan: remoteBootPlan || { devices: [], cables: [], racks: [], notes: [], codeMap: {} },
        planHash: "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
      } }); };
      if (delayFullPlan) return new Promise(function (resolve) { delayedFullPlanResolve = function () { resolve(fullResponse()); }; });
      return fullResponse();
    }
    if (url === "/api/rackwire/plans/74" && createReplayPlan) return response(200, { ok: true, data: createReplayPlan });
    if (url === "/api/rackwire/plans/73/restore") {
      if (restorePlanConflict) return response(409, { ok: false, error: { code: "plan_conflict" } });
      var restoreResponse = function () { return response(200, { ok: true, data: {
        id: 73, name: "Recovered plan", plan: { devices: [{ uid: "restored-device" }], cables: [], racks: [], notes: [], codeMap: {} },
        planHash: "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
      } }); };
      if (delayRestore) return new Promise(function (resolve) { delayedRestoreResolve = function () { resolve(restoreResponse()); }; });
      return restoreResponse();
    }
    if (/\/api\/rackwire\/plans\/73\/versions$/.test(url) && options.method === "POST") {
      return failRemoteSnapshot ? response(remoteSnapshotStatus, { ok: false, error: { code: remoteSnapshotStatus === 409 ? "conflict" : "service_unavailable" } }) : response(201, { ok: true, data: { id: 88 } });
    }
    if (url === "/api/rackwire/plans" && failPlanList) return response(503, { ok: false });
    if (url === "/api/rackwire/plans") return response(200, { ok: true, data: createReplayPlan ? [{ id: 74, name: createReplayPlan.name }] : alternatePlanList ? [{ id: 74, name: "Different plan" }] : [{ id: 73, name: "Older server plan" }] });
    return response(404, { ok: false });
  };

  try {
    vm.createContext(sandbox);
    vm.runInContext(inline[1] + "\nwindow.TestComponent = Component;", sandbox, { filename: "rackwire-autosave.js" });
    sandbox.RW.DEVICES = ["udm-pro-max", "usw-pro-max-16-poe", "macmini-m4pro", "pdu-8"].map(function (id) {
      return { id: id, name: id, model: id, vendor: "fixture", kind: "other", groups: [] };
    });
    var freshBoot = new sandbox.TestComponent({}), seedDone = null, bootDirty = null;
    var bootSetState = freshBoot.setState.bind(freshBoot);
    freshBoot.setState = function (patch, done) { bootSetState(patch); if (done) seedDone = done; };
    freshBoot.bootTheme = function () {};
    freshBoot.exposeApi = function () {};
    freshBoot.mergeLibrary = async function () {};
    freshBoot.mergeDiscoveryLibrary = async function () {};
    freshBoot.loadRemote = async function () { bootDirty = freshBoot._planRemoteDirty; };
    freshBoot.componentDidMount();
    freshBoot.componentDidUpdate();
    if (seedDone) seedDone();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    ok("fresh-browser demo seed stays local while boot loads the shared plan",
      freshBoot.state.devices.length === 4 && bootDirty === false && saved.get("rackwire.project.v2.remoteDirty") !== "1" &&
      !calls.some(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }));
    saved.clear();
    calls.length = 0;
    sandbox.RW.DEVICES = [];

    var first = new sandbox.TestComponent({});
    first.state.devices = [{ uid: "local-device", name: "Local edit" }];
    first.persist(true);
    sandbox.clearTimeout(first._planT);
    await first.flushPlan();
    ok("failed shared save retains dirty state inside the local recovery snapshot",
      JSON.parse(saved.get("rackwire.project.v2")).remoteDirty === true && !saved.has("rackwire.project.v2.remoteDirty"));
    ok("failed shared save reports local-only status", first.state.saved === "Local copy · shared save failed; retrying");
    var retry = timers.get(first._planT);
    ok("failed shared save schedules a delayed retry", !!retry && retry.ms >= 3000);

    var requestCount = calls.length;
    var recovered = new sandbox.TestComponent({});
    await recovered.loadRemote();
    ok("reload retries dirty local plan without fetching older server state",
      calls.length === requestCount && !!timers.get(recovered._planT));
    var local = JSON.parse(saved.get("rackwire.project.v2"));
    ok("reloaded component retains the locally edited device", local.devices[0].uid === "local-device");

    failPlanWrite = false;
    sandbox.clearTimeout(recovered._planT);
    await recovered.flushPlan();
    ok("successful recovery clears snapshot dirty state and confirms shared save",
      JSON.parse(saved.get("rackwire.project.v2")).remoteDirty === false && !saved.has("rackwire.project.v2.remoteDirty") && recovered.state.saved === "Shared plan saved");

    saved.set("rackwire.project.v2.planId", "73");
    var restored = new sandbox.TestComponent({});
    restored.state.devices = [{ uid: "before-restore" }];
    restored.state.codeMap = { existing: "fixture-model" };
    await restored.restoreVersionRemote({ remoteId: 88 });
    var restoreRequest = calls.filter(function (call) { return call.url === "/api/rackwire/plans/73/restore"; }).slice(-1)[0];
    var restoreHash = restored._planServerHash;
    restored.undo();
    var undoRestoredDevice = restored.state.devices[0].uid;
    var undoRestoredCodeMap = restored.state.codeMap.existing;
    restored.redo();
    var redoRestoredDevice = restored.state.devices[0].uid;
    var redoRestoredCodeMap = restored.state.codeMap.existing;
    restored.state.devices = [{ uid: "after-restore" }];
    restored.componentDidUpdate();
    sandbox.clearTimeout(restored._planT);
    await restored.flushPlan();
    ok("remote snapshot restore refreshes the CAS hash before the next edit",
      restoreHash === "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc" &&
      restored.state.saved === "Shared plan saved" && !JSON.parse(saved.get("rackwire.project.v2")).remoteDirty &&
      undoRestoredDevice === "before-restore" && undoRestoredCodeMap === "fixture-model" &&
      redoRestoredDevice === "restored-device" && redoRestoredCodeMap === undefined &&
      !!restoreRequest && typeof restoreRequest.body.expectedHash === "string");

    restorePlanConflict = true;
    var conflictedRestore = new sandbox.TestComponent({});
    conflictedRestore._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    conflictedRestore._planServerBaselineKnown = true;
    conflictedRestore.state.devices = [{ uid: "keep-local-after-conflict" }];
    await conflictedRestore.restoreVersionRemote({ remoteId: 88 });
    ok("a stale remote snapshot restore enters conflict recovery without replacing the local plan",
      conflictedRestore._planHashConflict === true && /changed elsewhere/i.test(conflictedRestore.state.saved) && conflictedRestore.state.devices[0].uid === "keep-local-after-conflict");
    restorePlanConflict = false;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    saved.delete("rackwire.project.v2.remoteDirty");
    var restorePreflightOutage = new sandbox.TestComponent({});
    restorePreflightOutage._planRemoteDirty = false;
    restorePreflightOutage._planServerHash = "";
    restorePreflightOutage._planServerBaselineKnown = true;
    restorePreflightOutage._planServerBaseline = restorePreflightOutage.planComparable(restorePreflightOutage.plan());
    failFullPlan = true;
    await restorePreflightOutage.restoreVersionRemote({ remoteId: 88 });
    failFullPlan = false;
    ok("a temporary restore preflight failure stays retryable instead of latching conflict",
      !restorePreflightOutage._planHashConflict && /could not verify.*retry/i.test(restorePreflightOutage.state.saved),
      JSON.stringify({ conflict: restorePreflightOutage._planHashConflict, status: restorePreflightOutage.state.saved, hashFailure: restorePreflightOutage._planHashFailureStatus, dirty: restorePreflightOutage._planRemoteDirty }));

    delayRestore = true;
    var restoreRace = new sandbox.TestComponent({});
    var restoreRaceCallStart = calls.length;
    var restoreRaceRequest = restoreRace.restoreVersionRemote({ remoteId: 89 });
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    restoreRace.state.devices = [{ uid: "edit-during-restore" }];
    restoreRace.componentDidUpdate();
    await restoreRace.flushPlan();
    delayedRestoreResolve();
    await restoreRaceRequest;
    delayRestore = false;
    ok("an edit during remote restore is retained and held for conflict review",
      restoreRace.state.devices[0].uid === "edit-during-restore" && restoreRace._planServerHash === "" &&
      restoreRace._planHashConflict === true && /review the shared version/i.test(restoreRace.state.saved));
    ok("autosave stays out of flight while a remote restore is pending",
      !calls.slice(restoreRaceCallStart).some(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }));
    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.delete("rackwire.project.v2.remoteDirty");

    delayPlanWrite = true;
    delayedPlanWriteStatus = 200;
    delayedPlanWriteResolve = null;
    delayedSecondaryPlanWriteResolve = null;
    var pendingWriteRestore = new sandbox.TestComponent({});
    pendingWriteRestore.setPlanId(73, "Recovered plan");
    pendingWriteRestore._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    pendingWriteRestore._planServerBaselineKnown = true;
    pendingWriteRestore.state.devices = [{ uid: "pending-local-write" }];
    pendingWriteRestore.persist(true);
    sandbox.clearTimeout(pendingWriteRestore._planT);
    pendingWriteRestore.waitForPlanFlush = async function () {
      while (this._planBusy) await new Promise(function (resolve) { setTimeout(resolve, 1); });
      return true;
    };
    var pendingRestoreCallStart = calls.length;
    var writeThenRestore = pendingWriteRestore.restoreVersionRemote({ remoteId: 90 });
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    delayedPlanWriteResolve();
    await writeThenRestore;
    delayPlanWrite = false;
    delayedPlanWriteStatus = 409;
    delayedPlanWriteResolve = null;
    ok("snapshot restore waits for a pending local save and uses its refreshed base hash",
      pendingWriteRestore._planHashConflict === false && pendingWriteRestore.state.devices[0].uid === "restored-device" &&
      calls.slice(pendingRestoreCallStart).findIndex(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST" && call.body.plan.devices[0].uid === "pending-local-write"; }) <
      calls.slice(pendingRestoreCallStart).findIndex(function (call) { return call.url === "/api/rackwire/plans/73/restore"; }),
      JSON.stringify({ conflict: pendingWriteRestore._planHashConflict, device: pendingWriteRestore.state.devices[0] && pendingWriteRestore.state.devices[0].uid,
        status: pendingWriteRestore.state.saved, busy: pendingWriteRestore._planBusy, dirty: pendingWriteRestore._planRemoteDirty,
        history: calls.filter(function (call) { return call.url.indexOf("/api/rackwire/plans") === 0 && call.method === "POST"; }).map(function (call) { return call.url + " " + JSON.stringify(call.body); }) }));

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "before-restore-preflight" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [{ uid: "reloaded-shared-plan" }], cables: [], racks: [], notes: [], codeMap: {} };
    var reloadDuringRestorePreflight = new sandbox.TestComponent({});
    reloadDuringRestorePreflight._planServerBaselineKnown = true;
    reloadDuringRestorePreflight._planServerBaseline = reloadDuringRestorePreflight.planComparable(reloadDuringRestorePreflight.plan());
    delayFullPlan = true;
    var staleRestorePreflight = reloadDuringRestorePreflight.restoreVersionRemote({ remoteId: 91 });
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    delayFullPlan = false;
    await reloadDuringRestorePreflight.discardPlanConflictAndReload();
    delayedFullPlanResolve();
    await staleRestorePreflight;
    ok("a stale restore preflight cannot add conflict state after a forced shared-plan reload",
      reloadDuringRestorePreflight.state.devices[0].uid === "reloaded-shared-plan" &&
      reloadDuringRestorePreflight._planHashConflict === false && reloadDuringRestorePreflight.state.saved === "Shared plan loaded");
    sandbox.clearTimeout(reloadDuringRestorePreflight._planT);
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "reload-will-be-interrupted" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    var interruptedReload = new sandbox.TestComponent({});
    interruptedReload._planHashConflict = true;
    delayPlanList = true;
    var pendingForcedReload = interruptedReload.discardPlanConflictAndReload();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    interruptedReload.state.devices = [{ uid: "edit-during-forced-reload" }];
    interruptedReload.componentDidUpdate();
    delayedPlanListResolve();
    await pendingForcedReload;
    delayPlanList = false;
    sandbox.clearTimeout(interruptedReload._planT);
    ok("a local edit that interrupts a forced reload gets a clear recovery status",
      interruptedReload.state.devices[0].uid === "edit-during-forced-reload" && /reload interrupted by a local edit/i.test(interruptedReload.state.saved));

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.delete("rackwire.project.v2.remoteDirty");
    recovered._planHold = true;
    recovered.state.devices = [{ uid: "server-device", name: "Server snapshot" }];
    recovered.componentDidUpdate();
    ok("server hydration persists locally without creating an unsynced-edit marker",
      !saved.has("rackwire.project.v2.remoteDirty") &&
      JSON.parse(saved.get("rackwire.project.v2")).devices[0].uid === "server-device");
    ok("server hydration does not add the previous local snapshot to undo history",
      !recovered._past || recovered._past.length === 0);

    delayPlanList = true;
    var hydration = new sandbox.TestComponent({});
    var hydrationLoad = hydration.loadRemote();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    hydration.state.devices = [{ uid: "edit-during-hydration" }];
    hydration.componentDidUpdate();
    delayedPlanListResolve();
    await hydrationLoad;
    ok("an edit made during remote boot hydration is retained and stays dirty",
      hydration.state.devices[0].uid === "edit-during-hydration" && hydration._planRemoteDirty === true);
    sandbox.clearTimeout(hydration._planT);
    delayPlanList = false;

    saved.delete("rackwire.project.v2");
    saved.set("rackwire.project.v2.planId", "73");
    saved.set("rackwire.project.v2.remoteDirty", "1");
    remoteBootPlan = { devices: [{ uid: "server-after-orphan-marker" }], cables: [], racks: [], notes: [], codeMap: {} };
    var orphanMarkerPostsBefore = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    var orphanMarker = new sandbox.TestComponent({});
    await orphanMarker.loadRemote();
    var orphanMarkerPostsAfter = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    ok("an orphan dirty marker hydrates the shared plan without uploading an unverified local default",
      orphanMarker.state.devices[0].uid === "server-after-orphan-marker" && orphanMarkerPostsAfter === orphanMarkerPostsBefore &&
      !saved.has("rackwire.project.v2.remoteDirty"));
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [], cables: [], racks: [], notes: [], codeMap: {} };
    delayPlanList = true;
    var baselineHydration = new sandbox.TestComponent({});
    var baselineLoad = baselineHydration.loadRemote();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    baselineHydration.state.devices = [{ uid: "edit-after-baseline" }];
    baselineHydration.componentDidUpdate();
    delayedPlanListResolve();
    await baselineLoad;
    delayPlanList = false;
    sandbox.clearTimeout(baselineHydration._planT);
    var baselinePostsBefore = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    await baselineHydration.flushPlan();
    var baselinePostsAfter = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    ok("a legacy local edit after baseline capture syncs against the pre-edit server plan",
      baselinePostsAfter === baselinePostsBefore + 1 && baselineHydration.state.devices[0].uid === "edit-after-baseline" && baselineHydration.state.saved === "Shared plan saved");
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "legacy-device", ifMap: {} }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [{ uid: "legacy-device", ifMap: [] }], cables: [], racks: [], notes: [], codeMap: [] };
    failPlanList = true;
    var legacy = new sandbox.TestComponent({});
    await legacy.loadRemote();
    failPlanList = false;
    legacy.persist(true);
    sandbox.clearTimeout(legacy._planT);
    await legacy.flushPlan();
    ok("legacy equal plans recover after transient failure with nested empty-map normalization", legacy.planId() === "73" && !!legacy._planServerHash && legacy.state.saved === "Shared plan saved");
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "legacy-baseline-device" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [{ uid: "legacy-baseline-device" }], cables: [], racks: [], notes: [], codeMap: {} };
    var unknownLegacyBaseline = new sandbox.TestComponent({});
    unknownLegacyBaseline._planServerHash = "";
    unknownLegacyBaseline._planServerBaselineKnown = false;
    sandbox.clearTimeout(unknownLegacyBaseline._planT);
    await unknownLegacyBaseline.flushPlan();
    ok("legacy dirty copies with no saved baseline can compare to the current server plan before conflict",
      unknownLegacyBaseline._planHashConflict === false && !!unknownLegacyBaseline._planServerHash && unknownLegacyBaseline.state.saved === "Shared plan saved");
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "legacy-catalog-device", libId: "legacy-catalog-model" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    sandbox.RW.DEVICES = [{ id: "legacy-catalog-model", ifMap: { lan: "catalog-port" }, groups: [] }];
    remoteBootPlan = { devices: [{ uid: "legacy-catalog-device", libId: "legacy-catalog-model" }], cables: [], racks: [], notes: [], codeMap: {} };
    var unknownCatalogBaseline = new sandbox.TestComponent({});
    unknownCatalogBaseline._planServerHash = "";
    unknownCatalogBaseline._planServerBaselineKnown = false;
    sandbox.clearTimeout(unknownCatalogBaseline._planT);
    await unknownCatalogBaseline.flushPlan();
    ok("legacy baseline comparison applies current catalog migration to server and local devices",
      unknownCatalogBaseline._planHashConflict === false && unknownCatalogBaseline.state.devices[0].ifMap.lan === "catalog-port" && unknownCatalogBaseline.state.saved === "Shared plan saved");
    sandbox.RW.DEVICES = [];
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "catalog-device", libId: "catalog-switch" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    sandbox.RW.DEVICES = [{ id: "catalog-switch", ifMap: { wan: "port-1" }, groups: [] }];
    remoteBootPlan = { devices: [{ uid: "catalog-device", libId: "catalog-switch" }], cables: [], racks: [], notes: [], codeMap: {} };
    failPlanList = true;
    var catalogBaseline = new sandbox.TestComponent({});
    await catalogBaseline.loadRemote();
    failPlanList = false;
    catalogBaseline.persist(true);
    sandbox.clearTimeout(catalogBaseline._planT);
    await catalogBaseline.flushPlan();
    ok("legacy baseline comparison ignores fields newly backfilled from the device catalog",
      !!catalogBaseline._planServerHash && catalogBaseline.state.devices[0].ifMap.wan === "port-1" && catalogBaseline.state.saved === "Shared plan saved");
    sandbox.RW.DEVICES = [];
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "offline-edit" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [{ uid: "other-browser-edit" }], cables: [], racks: [], notes: [], codeMap: {} };
    var beforeStaleBoot = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    var staleBoot = new sandbox.TestComponent({});
    await staleBoot.loadRemote();
    sandbox.clearTimeout(staleBoot._planT);
    await staleBoot.flushPlan();
    var afterStaleBoot = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    ok("legacy dirty copy that differs from the shared plan is held for review", afterStaleBoot === beforeStaleBoot && /differs from shared copy/i.test(staleBoot.state.saved));
    remoteBootPlan = null;

    planFailureStatus = 403;
    failPlanWrite = true;
    var denied = new sandbox.TestComponent({});
    denied._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    denied.state.devices = [{ uid: "denied-device", name: "Denied edit" }];
    denied.persist(true);
    sandbox.clearTimeout(denied._planT);
    await denied.flushPlan();
    ok("permission failures stop autosave retries but retain the dirty local snapshot",
      !timers.has(denied._planT) && /operator session/i.test(denied.state.saved) &&
      JSON.parse(saved.get("rackwire.project.v2")).remoteDirty === true);

    failWhoami = true;
    vm.runInContext("rwPlanCsrf = ''; rwPlanCsrfPromise = null;", sandbox);
    var noSessionPosts = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    var loggedOut = new sandbox.TestComponent({});
    loggedOut._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    loggedOut.state.devices = [{ uid: "logged-out-device" }];
    loggedOut.persist(true);
    sandbox.clearTimeout(loggedOut._planT);
    await loggedOut.flushPlan();
    var noSessionPostsAfter = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    ok("missing operator CSRF identity stops retries with a visible sign-in status",
      noSessionPostsAfter === noSessionPosts && !timers.has(loggedOut._planT) && /operator session/i.test(loggedOut.state.saved));
    failWhoami = false;
    failWhoamiTransport = true;
    vm.runInContext("rwPlanCsrf = ''; rwPlanCsrfPromise = null; rwPlanCsrfFailureStatus = 0;", sandbox);
    var networkIdentity = new sandbox.TestComponent({});
    networkIdentity._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    networkIdentity.state.devices = [{ uid: "identity-retry-device" }];
    networkIdentity.persist(true);
    sandbox.clearTimeout(networkIdentity._planT);
    await networkIdentity.flushPlan();
    ok("transient identity network failures retain the bounded autosave retry", !!timers.get(networkIdentity._planT) && timers.get(networkIdentity._planT).ms >= 3000 && /retrying/i.test(networkIdentity.state.saved));
    failWhoamiTransport = false;

    planFailureStatus = 409;
    planFailureCode = "plan_hash_required";
    var staleUi = new sandbox.TestComponent({});
    staleUi._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    staleUi.state.devices = [{ uid: "stale-client-device" }];
    staleUi.persist(true);
    sandbox.clearTimeout(staleUi._planT);
    await staleUi.flushPlan();
    ok("cached clients receive a visible reload instruction when the API requires a plan hash",
      !timers.has(staleUi._planT) && /reload rackwire/i.test(staleUi.state.saved));

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "preflight-local" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    var preflightSwitch = new sandbox.TestComponent({});
    preflightSwitch._planServerBaselineKnown = true;
    preflightSwitch._planServerBaseline = preflightSwitch.planComparable(preflightSwitch.plan());
    delayFullPlan = true;
    var pendingPreflight = preflightSwitch.flushPlan();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    preflightSwitch.savePlanConflictAsCopy();
    delayedFullPlanResolve();
    await pendingPreflight;
    delayFullPlan = false;
    sandbox.clearTimeout(preflightSwitch._planT);
    ok("a late legacy hash preflight cannot affect a newly bound local copy",
      preflightSwitch.planId() === "" && preflightSwitch._planServerHash === "" && !preflightSwitch._planHashConflict);

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "preflight-reload-local" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    var reloadDuringPreflight = new sandbox.TestComponent({});
    reloadDuringPreflight._planServerBaselineKnown = true;
    reloadDuringPreflight._planServerBaseline = reloadDuringPreflight.planComparable(reloadDuringPreflight.plan());
    remoteBootPlan = { devices: [{ uid: "shared-current" }], cables: [], racks: [], notes: [], codeMap: {} };
    delayFullPlan = true;
    var preflightWrite = reloadDuringPreflight.flushPlan();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    delayFullPlan = false;
    await reloadDuringPreflight.discardPlanConflictAndReload();
    delayedFullPlanResolve();
    await preflightWrite;
    ok("forced reload fences a save still waiting on its hash preflight",
      reloadDuringPreflight.state.devices[0].uid === "shared-current" && reloadDuringPreflight.state.saved === "Shared plan loaded" &&
      calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).every(function (call) { return !call.body || call.body.plan.devices[0].uid !== "preflight-reload-local"; }));
    sandbox.clearTimeout(reloadDuringPreflight._planT);

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "orphaned-local" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "74");
    remoteBootPlan = { devices: [{ uid: "newest-unrelated-plan" }], cables: [], racks: [], notes: [], codeMap: {} };
    var removedBinding = new sandbox.TestComponent({});
    await removedBinding.loadRemote();
    ok("boot with a deleted plan binding keeps the local copy and records a tombstone",
      removedBinding.planId() === "" && removedBinding._planDeleted === true && removedBinding.state.devices[0].uid === "orphaned-local" && /plan was removed/i.test(removedBinding.state.saved));
    var removedBindingPosts = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    await removedBinding.loadRemote();
    ok("a deleted binding does not silently adopt or recreate another shared plan",
      removedBinding.planId() === "" && removedBinding.state.devices[0].uid === "orphaned-local" &&
      calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length === removedBindingPosts);
    remoteBootPlan = null;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, deletedPlan: false }));
    saved.delete("rackwire.project.v2.planId");
    planFailureCode = "conflict";
    var nameCollision = new sandbox.TestComponent({});
    nameCollision.setPlanId("", "Duplicate shared name");
    nameCollision.state.devices = [{ uid: "name-collision-device" }];
    nameCollision.persist(true);
    sandbox.clearTimeout(nameCollision._planT);
    await nameCollision.flushPlan();
    ok("a create-name collision exposes the separate-copy recovery action", nameCollision._planHashConflict === true && /rejected/i.test(nameCollision.state.saved));
    failPlanWrite = false;
    nameCollision.savePlanConflictAsCopy();
    var renamedTimer = timers.get(nameCollision._planT);
    sandbox.clearTimeout(nameCollision._planT);
    var renamedSave = null, originalRenamedFlush = nameCollision.flushPlan.bind(nameCollision);
    nameCollision.flushPlan = function () { renamedSave = originalRenamedFlush(); return renamedSave; };
    if (renamedTimer) renamedTimer.fn();
    if (renamedSave) await renamedSave;
    var renamedRequest = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).slice(-1)[0];
    ok("create-name collision can be retried under a new name", !!renamedRequest && renamedRequest.body.name === "Recovered copy" && !Object.prototype.hasOwnProperty.call(renamedRequest.body, "id"));

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, deletedPlan: false }));
    saved.delete("rackwire.project.v2.planId");
    planFailureStatus = 409;
    planFailureCode = "conflict";
    failPlanWrite = true;
    var uncertainCreate = new sandbox.TestComponent({});
    uncertainCreate.setPlanId("", "Created before response loss");
    uncertainCreate.state.devices = [{ uid: "lost-create-device" }];
    var uncertainPlan = uncertainCreate.plan();
    createReplayPlan = { id: 74, name: "Created before response loss", plan: uncertainPlan, planHash: "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd" };
    uncertainCreate.persist(true);
    sandbox.clearTimeout(uncertainCreate._planT);
    await uncertainCreate.flushPlan();
    ok("a lost create response reconciles an exact shared row without posting a duplicate",
      uncertainCreate.planId() === "74" && uncertainCreate._planHashConflict === false &&
      calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).slice(-1)[0].body.name === "Created before response loss" &&
      /Shared plan saved/i.test(uncertainCreate.state.saved));
    createReplayPlan = null;
    failPlanWrite = false;
    planFailureStatus = 503;
    planFailureCode = "";

    failPlanWrite = true;
    planFailureStatus = 409;
    planFailureCode = "plan_conflict";
    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, deletedPlan: false }));
    saved.delete("rackwire.project.v2.planId");
    var conflict = new sandbox.TestComponent({});
    conflict._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    conflict.state.devices = [{ uid: "conflict-device", name: "Concurrent edit" }];
    conflict.persist(true);
    sandbox.clearTimeout(conflict._planT);
    await conflict.flushPlan();
    ok("concurrent plan conflicts stop retries and preserve the local dirty copy",
      !timers.has(conflict._planT) && /changed elsewhere/i.test(conflict.state.saved) &&
      JSON.parse(saved.get("rackwire.project.v2")).remoteDirty === true);
    var conflictPostCount = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length;
    await conflict.flushPlan();
    ok("a known conflicted plan does not issue redundant writes", calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).length === conflictPostCount);
    var conflictStatusBeforeEdit = conflict.state.saved;
    conflict.persist(true);
    ok("editing a conflicted plan does not claim that a shared save is running",
      conflict.state.saved === conflictStatusBeforeEdit && !timers.has(conflict._planT));

    delayPlanWrite = true;
    delayedPlanWriteResolve = null;
    delayedSecondaryPlanWriteResolve = null;
    var switching = new sandbox.TestComponent({});
    switching.setPlanId(73, "Old shared plan");
    switching._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    switching._planHashConflict = false;
    switching.state.devices = [{ uid: "switching-device" }];
    switching.persist(true);
    sandbox.clearTimeout(switching._planT);
    var switchingRequest = switching.flushPlan();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    switching.savePlanConflictAsCopy();
    delayedPlanWriteResolve();
    await switchingRequest;
    delayedPlanWriteResolve = null;
    delayedSecondaryPlanWriteResolve = null;
    delayPlanWrite = false;
    sandbox.clearTimeout(switching._planT);
    ok("a late conflict response cannot rebind or re-open conflict on a new local copy",
      switching.planId() === "" && switching._planHashConflict === false && !/changed elsewhere/i.test(switching.state.saved));

    delayPlanWrite = true;
    delayedPlanWriteResolve = null;
    delayedSecondaryPlanWriteResolve = null;
    var copiedDuringSave = new sandbox.TestComponent({});
    copiedDuringSave.setPlanId(73, "Old shared plan");
    copiedDuringSave._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    copiedDuringSave.state.devices = [{ uid: "duplicated-on-purpose" }];
    copiedDuringSave.persist(true);
    sandbox.clearTimeout(copiedDuringSave._planT);
    delayedPlanWriteStatus = 200;
    var earlierSave = copiedDuringSave.flushPlan();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    failPlanWrite = false;
    copiedDuringSave.savePlanConflictAsCopy();
    copiedDuringSave._planRequestGeneration++;
    var separateCopyTimer = timers.get(copiedDuringSave._planT);
    sandbox.clearTimeout(copiedDuringSave._planT);
    var separateCopySave = null, copyFlush = copiedDuringSave.flushPlan.bind(copiedDuringSave);
    copiedDuringSave.flushPlan = function () { separateCopySave = copyFlush(); return separateCopySave; };
    delayedPlanWriteResolve();
    await earlierSave;
    ok("a prior-plan write is disclosed even when its response generation is stale",
      copiedDuringSave._planPriorWriteNotice === true && /checking the separate copy/i.test(copiedDuringSave.state.saved));
    delayPlanWrite = false;
    delayedPlanWriteResolve = null;
    if (separateCopyTimer) separateCopyTimer.fn();
    if (separateCopySave) await separateCopySave;
    delayedPlanWriteStatus = 409;
    delayedSecondaryPlanWriteResolve = null;
    ok("a successful write to the prior plan is disclosed after saving a separate copy",
      copiedDuringSave.planId() === "73" && /previous shared plan/i.test(copiedDuringSave.state.saved));
    ok("the previous-plan disclosure remains visible after the separate copy saves",
      /earlier edits also reached the previous shared plan/i.test(copiedDuringSave.state.saved));

    failPlanWrite = false;
    conflict.savePlanConflictAsCopy();
    var copyTimer = timers.get(conflict._planT);
    ok("saving conflicted edits schedules an immediate shared-copy attempt", !!copyTimer && copyTimer.ms === 1000);
    sandbox.clearTimeout(conflict._planT);
    var copySave = null, originalCopyFlush = conflict.flushPlan.bind(conflict);
    conflict.flushPlan = function () { copySave = originalCopyFlush(); return copySave; };
    if (copyTimer) copyTimer.fn();
    if (copySave) await copySave;
    var copyRequest = calls.filter(function (call) { return call.url === "/api/rackwire/plans" && call.method === "POST"; }).slice(-1)[0];
    ok("conflicted local edits can be saved as a separate shared plan",
      !!copyRequest && !Object.prototype.hasOwnProperty.call(copyRequest.body, "id") &&
      copyRequest.body.name === "Recovered copy" && copyRequest.body.plan.devices[0].uid === "conflict-device" && conflict.planId() === "73");

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "conflicted-local" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [{ uid: "shared-current" }], cables: [], racks: [], notes: [], codeMap: {} };
    var conflictReload = new sandbox.TestComponent({});
    conflictReload._planHashConflict = true;
    var archivesBeforeConflictReload = conflictReload.localVersions().filter(function (version) { return version.name === "Before shared plan reload"; }).length;
    failPlanList = true;
    await conflictReload.discardPlanConflictAndReload();
    var archivedBeforeRetry = conflictReload.localVersions().filter(function (version) { return version.name === "Before shared plan reload"; }).length;
    ok("a failed conflict reload keeps its local archive, dirty marker, and retry action available",
      conflictReload.state.devices[0].uid === "conflicted-local" && conflictReload._planRemoteDirty === true && archivedBeforeRetry === archivesBeforeConflictReload + 1 && conflictReload._planHashConflict === true,
      JSON.stringify({ device: conflictReload.state.devices[0].uid, dirty: conflictReload._planRemoteDirty, archives: archivedBeforeRetry, conflict: conflictReload._planHashConflict, status: conflictReload.state.saved }));
    failPlanList = false;
    alternatePlanList = true;
    await conflictReload.discardPlanConflictAndReload();
    ok("forced conflict reload does not switch silently to a different shared plan",
      conflictReload.planId() === "" && conflictReload._planDeleted === true && conflictReload.state.devices[0].uid === "conflicted-local" && /shared plan was removed/i.test(conflictReload.state.saved));
    alternatePlanList = false;
    conflictReload._planDeleted = false;
    conflictReload._planHashConflict = true;
    conflictReload._planRemoteDirty = true;
    conflictReload.setPlanId("73", "Recovered plan");
    await conflictReload.discardPlanConflictAndReload();
    ok("conflict reload archives local edits before loading the shared version",
      conflictReload.state.devices[0].uid === "shared-current" &&
      conflictReload.localVersions().filter(function (version) { return version.name === "Before shared plan reload"; }).length === archivesBeforeConflictReload + 1 && conflictReload._planRemoteDirty === false,
      JSON.stringify({ device: conflictReload.state.devices[0].uid, dirty: conflictReload._planRemoteDirty, status: conflictReload.state.saved }));

    var snapshotGuard = new sandbox.TestComponent({});
    snapshotGuard.setPlanId(73, "Conflicted plan");
    snapshotGuard._planHashConflict = true;
    snapshotGuard._planRemoteDirty = true;
    snapshotGuard.forceUpdate = function () {};
    var remoteSnapshotRequests = calls.filter(function (call) { return /\/versions$/.test(call.url) && call.method === "POST"; }).length;
    snapshotGuard.saveVersion("Conflict-local snapshot");
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    ok("a remote snapshot is not mislabeled when local edits could not sync",
      snapshotGuard.localVersions().some(function (version) { return version.name === "Conflict-local snapshot"; }) &&
      calls.filter(function (call) { return /\/versions$/.test(call.url) && call.method === "POST"; }).length === remoteSnapshotRequests &&
      /shared snapshot blocked by a plan conflict/i.test(snapshotGuard.state.saved));

    var snapshotDuringFlush = new sandbox.TestComponent({});
    saved.set("rackwire.project.v2", JSON.stringify({ devices: [], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: false, deletedPlan: false }));
    saved.delete("rackwire.project.v2.planId");
    snapshotDuringFlush = new sandbox.TestComponent({});
    snapshotDuringFlush.setPlanId(73, "Snapshot after flush");
    snapshotDuringFlush._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    snapshotDuringFlush.state.devices = [{ uid: "snapshot-after-save" }];
    snapshotDuringFlush.persist(true);
    sandbox.clearTimeout(snapshotDuringFlush._planT);
    delayPlanWrite = true;
    delayedPlanWriteStatus = 200;
    delayedPlanWriteResolve = null;
    var firstSnapshotFlush = snapshotDuringFlush.flushPlan();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    snapshotDuringFlush.waitForPlanFlush = async function () {
      while (this._planBusy) await new Promise(function (resolve) { setTimeout(resolve, 1); });
      return true;
    };
    var snapshotAfterFlush = snapshotDuringFlush.saveVersionRemote("Saved plan snapshot");
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    delayedPlanWriteResolve();
    await Promise.all([firstSnapshotFlush, snapshotAfterFlush]);
    delayPlanWrite = false;
    delayedPlanWriteStatus = 409;
    delayedPlanWriteResolve = null;
    sandbox.clearTimeout(snapshotDuringFlush._planT);
    ok("remote snapshot waits for an in-flight successful save instead of requiring a manual retry",
      calls.some(function (call) { return call.url === "/api/rackwire/plans/73/versions" && call.method === "POST" && call.body.name === "Saved plan snapshot"; }) &&
      snapshotDuringFlush._planRemoteDirty === false);

    failRemoteSnapshot = true;
    var snapshotFailure = new sandbox.TestComponent({});
    snapshotFailure.setPlanId(73, "Snapshot failure");
    snapshotFailure._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    await snapshotFailure.saveVersionRemote("Unavailable snapshot");
    ok("failed remote snapshot creation reports that only the local snapshot was saved",
      /local snapshot saved · shared snapshot unavailable/i.test(snapshotFailure.state.saved));
    remoteSnapshotStatus = 409;
    var rejectedSnapshot = new sandbox.TestComponent({});
    rejectedSnapshot.setPlanId(73, "Snapshot rejected");
    rejectedSnapshot._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    await rejectedSnapshot.saveVersionRemote("Rejected snapshot");
    ok("snapshot ownership conflicts are described as rejection rather than plan-version change",
      /shared snapshot rejected.*ownership/i.test(rejectedSnapshot.state.saved));
    remoteSnapshotStatus = 503;
    failRemoteSnapshot = false;

    saved.set("rackwire.project.v2", JSON.stringify({ devices: [{ uid: "late-local" }], cables: [], racks: [], notes: [], codeMap: {}, remoteDirty: true, serverHash: "" }));
    saved.set("rackwire.project.v2.planId", "73");
    remoteBootPlan = { devices: [{ uid: "shared-after-reload" }], cables: [], racks: [], notes: [], codeMap: {} };
    var reloadDuringWrite = new sandbox.TestComponent({});
    reloadDuringWrite._planHashConflict = false;
    reloadDuringWrite._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    reloadDuringWrite.state.devices = [{ uid: "late-local" }];
    reloadDuringWrite.persist(true);
    sandbox.clearTimeout(reloadDuringWrite._planT);
    delayPlanWrite = true;
    delayedPlanWriteResolve = null;
    delayedSecondaryPlanWriteResolve = null;
    var lateWrite = reloadDuringWrite.flushPlan();
    await new Promise(function (resolve) { setTimeout(resolve, 0); });
    await reloadDuringWrite.discardPlanConflictAndReload();
    delayedPlanWriteResolve();
    await lateWrite;
    delayPlanWrite = false;
    ok("a stale save response cannot overwrite the forced shared-plan reload in the UI",
      reloadDuringWrite.state.devices[0].uid === "shared-after-reload" && reloadDuringWrite.state.saved === "Shared plan loaded" &&
      reloadDuringWrite._planServerHash === "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    remoteBootPlan = null;

    planFailureStatus = 404;
    failPlanWrite = true;
    var missing = new sandbox.TestComponent({});
    missing._planServerHash = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    missing.setPlanId(999, "Removed plan");
    missing.state.devices = [{ uid: "missing-device", name: "Recovered edit" }];
    missing.persist(true);
    sandbox.clearTimeout(missing._planT);
    await missing.flushPlan();
    ok("a deleted shared-plan binding is retained for explicit recovery",
      missing.planId() === "" && missing._planDeleted === true && /local copy retained/i.test(missing.state.saved) &&
      !timers.has(missing._planT) && JSON.parse(saved.get("rackwire.project.v2")).deletedPlan === true);
    missing.savePlanConflictAsCopy();
    ok("deleted-plan recovery requires an explicit save-as-new action",
      missing._planDeleted === false && missing._planRemoteDirty === true && missing.planName() === "Recovered copy");
    failPlanWrite = false;
    sandbox.clearTimeout(missing._planT);

    var beforeUnmountCalls = calls.length;
    missing.persist(true);
    var unmountTimer = missing._planT;
    missing.componentWillUnmount();
    await missing.flushPlan();
    ok("unmount clears a pending save timer and prevents later requests",
      !timers.has(unmountTimer) && calls.length === beforeUnmountCalls);

    sandbox.localStorage = {
      getItem: function () { throw new Error("storage blocked"); },
      setItem: function () { throw new Error("storage blocked"); },
      removeItem: function () { throw new Error("storage blocked"); }
    };
    var blocked = new sandbox.TestComponent({});
    ok("blocked browser storage is surfaced during startup", /local recovery unavailable/i.test(blocked.state.saved));
    blocked._planHold = true;
    await blocked.flushPlan();
    var holdTimer = timers.get(blocked._planT);
    ok("a shared-save timer that fires during server hydration is deferred", !!holdTimer && holdTimer.ms === 250);
    blocked._planHold = false;
    blocked.persist(true);
    sandbox.clearTimeout(blocked._planT);
    await blocked.flushPlan();
    ok("shared-save status keeps the local recovery warning visible", /shared plan saved/i.test(blocked.state.saved) && /local recovery unavailable/i.test(blocked.state.saved));
    ok("shared plan identity stays available in memory when browser storage is blocked", blocked.planId() === "73");

    sandbox.location.protocol = "file:";
    var offline = new sandbox.TestComponent({});
    offline.persist(true);
    ok("standalone file mode keeps the plan local without a permanent retry timer", !timers.has(offline._planT));
  } catch (e) {
    ok("autosave failure/reload/recovery behavior completes", false, String(e.stack || e));
  }
}

testExpiredCsrfRetry().then(testAutosaveRecovery).then(function () {
  console.log("\n" + pass + " passed, " + fail + " failed\n");
  process.exit(fail ? 1 : 0);
}).catch(function (e) {
  console.error("Unexpected test error:", e);
  process.exit(1);
});
