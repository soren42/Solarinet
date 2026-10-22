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
var RW_INDEX = path.join(PUB, "rackwire", "index.html");
var SCREEN = "screens-rackwire.jsx";

var pass = 0, fail = 0;
var rackWireBabel = null;
function ok(label, cond, extra) {
  if (cond) { pass++; console.log("  ok   - " + label); }
  else { fail++; console.log("  FAIL - " + label + (extra ? "  [" + extra + "]" : "")); }
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
      ok("dashboard/public/rackwire/index.html missing -> token-bridge checks skipped for checkouts without RackWire submodule", true);
    } else {
      ok("dashboard/public/rackwire/index.html is readable", false, e.message);
    }
    html = null;
  }

  if (html !== null) {
    var lookupStart = html.indexOf('async lookupCode(code)');
    var lookupEnd = html.indexOf('\n  scanGo()', lookupStart);
    var lookupBlock = lookupStart >= 0 && lookupEnd > lookupStart ? html.slice(lookupStart, lookupEnd) : '';
    ok("SoR barcode lookup sends the session-bound CSRF token",
      /rwSessionCsrf\(false\)/.test(lookupBlock) && /X-Solari-CSRF/.test(lookupBlock));
    ok("SoR barcode lookup refreshes an expired CSRF token once",
      /r\.status === 403[\s\S]*?rwSessionCsrf\(true\)/.test(lookupBlock));
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
  var html = fs.readFileSync(RW_INDEX, "utf8");

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

  var invalidCsrf = { ok: false, error: { message: "Missing or invalid CSRF token." } };
  var planStart = html.indexOf("async function rwFetch(path, body) {");
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

  var lookupStart = html.indexOf("  async lookupCode(code) {");
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
    } catch (e) {
      ok("barcode lookup retry harness completes", false, String(e.message).split("\n")[0]);
    }
  }
}

testExpiredCsrfRetry().then(function () {
  console.log("\n" + pass + " passed, " + fail + " failed\n");
  process.exit(fail ? 1 : 0);
}).catch(function (e) {
  console.error("Unexpected test error:", e);
  process.exit(1);
});
