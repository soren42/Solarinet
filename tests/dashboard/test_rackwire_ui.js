#!/usr/bin/env node
/* test_rackwire_ui.js — CONTRACT-RW.md §6.
 *
 * Two checks:
 *   1. Parse dashboard/public/screens-rackwire.jsx with the dashboard's own
 *      vendored Babel standalone (pattern: test_jsx_parse.js) — a pass means
 *      the SPA screen will not blank out on a syntax error.
 *   2. If the RackWire dependency submodule is checked out, sanity-check the
 *      CONTRACT-RW §0 D9 token bridge in dashboard/public/rackwire/index.html:
 *      every `--sn-*` custom property the app REFERENCES (CSS `var(--sn-x, ...)`,
 *      or the runtime `tok()` helper's
 *      `getComputedStyle(...).getPropertyValue('--sn-' + k)`) must be DEFINED
 *      somewhere in the bridge's `:root` blocks. A token used but never bridged
 *      would silently fall back to its hardcoded literal even when a dashboard
 *      theme value exists — exactly the regression D9 exists to prevent.
 *      When the submodule is absent (for example in a mirror checkout that
 *      cannot fetch the private dependency), assert the pin still exists in
 *      .gitmodules and skip the dependency-internal bridge inspection.
 *
 * Run: node tests/dashboard/test_rackwire_ui.js   — exits non-zero on failure.
 */
"use strict";

var fs = require("fs");
var vm = require("vm");
var path = require("path");

var PUB = path.join(__dirname, "..", "..", "dashboard", "public");
var GITMODULES = path.join(__dirname, "..", "..", ".gitmodules");
var RW_INDEX = path.join(PUB, "rackwire", "index.html");
var SCREEN = "screens-rackwire.jsx";

var pass = 0, fail = 0;
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
    try {
      var src = fs.readFileSync(path.join(PUB, SCREEN), "utf8");
      Babel.transform(src, { presets: ["react"], filename: SCREEN });
      ok(SCREEN + " parses clean", true);
    } catch (e) {
      ok(SCREEN + " parses clean", false, String(e.message).split("\n")[0]);
    }
  }
}

/* ---------------------------------------------------------------- [2] --- */
console.log("\n[2] RackWire dependency pin + token bridge");
{
  var gitmodules = "";
  try {
    gitmodules = fs.readFileSync(GITMODULES, "utf8");
  } catch (e) {
    ok(".gitmodules is readable", false, e.message);
  }
  ok(".gitmodules pins RackWire at dashboard/public/rackwire",
    /\[submodule "dashboard\/public\/rackwire"\][\s\S]*?path = dashboard\/public\/rackwire/.test(gitmodules));

  if (!fs.existsSync(RW_INDEX)) {
    ok("dashboard/public/rackwire/index.html is optional when the RackWire submodule is not checked out", true);
  } else {
    var html;
    try {
      html = fs.readFileSync(RW_INDEX, "utf8");
    } catch (e) {
      ok("dashboard/public/rackwire/index.html is readable", false, e.message);
      html = null;
    }

    if (html !== null) {
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

      // Two tokens are intentionally literal (D9) rather than var()-mapped —
      // still expected to be DEFINED in the :root block, just not to a var().
      var LITERAL_TOKENS = ["--sn-signal-other", "--sn-signal-open"];
      LITERAL_TOKENS.forEach(function (t) {
        ok(t + " is defined (literal by design, D9)", defined.has(t));
      });

      // Every other defined token must actually BRIDGE to a dashboard token —
      // var(--<dashboard-token>, <fallback>) — not just declare something.
      // A bare literal standing in for a real token (e.g. a hardcoded hex where
      // var(--accent, #22B8F0) belongs) would satisfy "is defined" but is
      // exactly the D9 regression this test exists to catch, so check the
      // declaration shape, not just its presence.
      var BRIDGE_RE = /^var\(\s*--[a-z][a-z0-9-]*\s*,\s*[\s\S]+\)$/;
      defined.forEach(function (vals, token) {
        if (LITERAL_TOKENS.indexOf(token) !== -1) return;
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
    }
  }
}

console.log("\n" + pass + " passed, " + fail + " failed\n");
process.exit(fail ? 1 : 0);
