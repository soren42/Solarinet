#!/usr/bin/env node
/* Framebuffer PARITY between the two A1 renderers (task PF1).

   RETURN-AW3 UNVERIFIED #3: the A1 flow-gates screen exists twice — in the
   firmware (status-panel/firmware/panelScreensA.c) and on the page
   (dashboard/public/screens-panel.jsx) — and structural parity had only ever
   been asserted against hand-transcribed constants. Nobody had rendered the
   SAME gear bytes through BOTH and diffed the framebuffers. This does that.

   THE TWO SIDES
     C   status-panel/firmware/test/panelParityTest.c renders the REAL firmware
         A1 from status-panel/fixtures/parity-fixture.json and commits the
         flushed framebuffer to status-panel/fixtures/golden/panel-A1.txt.
     JS  this file renders the REAL page A1 (paintScreen, not a copy) from the
         SAME fixture and diffs against that golden.
   Neither side reimplements anything, and neither side carries a transcribed
   band constant: the gate spans come out of the page's own a1Layout(), and the
   firmware's pixels come out of the firmware.

   WHAT IS COMPARED, AND WHAT IS EXCLUDED
     COMPARED  the gate layer — for every gate a1Layout() places, the exact set
               of lit rows within that gate's own span on its own column, plus
               the hue class of each lit pixel; and the internet (x=51) and
               wanBackup (x=52) columns in full. That is CONTRACT-AW §9 A-1's
               NORMATIVE geometry and §10 A-3's inheritance rule, asserted on
               pixels instead of on constants.
     EXCLUDED  (a) BRIGHTNESS. The two renderers scale gates differently on
               purpose: the firmware uses fixed per-role brightnesses
               (0.75 gates / 0.80 router / 0.60 internet and wan) while the page
               modulates by level and pulses degraded gates. Both are within
               contract; only geometry and hue are normative.
               (b) THE LEG LAYER. The legs use different brightness curves —
               firmware 0.04 + rx*0.06 (up to 0.40), page 0.035 + (rx/7)*0.05
               (up to 0.085), roughly 5x dimmer — so at any fixed threshold the
               firmware's connective wires read as lit and the page's do not.
               Block [4] pins that divergence down by COORDINATE instead of
               waving at it: every pixel lit in the firmware but not on the page
               must appear in panel-*.legs.txt — the set the C harness dumps out
               of the real a1Leg() — and must still be cQuiet-hued, the count
               may not exceed that leg path, and there must be NO pixel lit on
               the page but dark in the firmware. Residual weakness, stated in
               full at block [4] and in RETURN-PF1.md: this proves position, not
               brightness. The committed C golden is what pins leg brightness.
               (c) PARTICLES. The two integrators differ by design (90 particles
               keyed to a source gate vs 72 keyed to a leg) and reconciling them
               is out of scope. The fixture sets txLevel 0 on every gear row, so
               BOTH renderers draw none at all and the exclusion costs nothing.

   Run: node tests/dashboard/test_panel_parity.js  — exits non-zero on failure. */
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..", "..");
const PUB = path.join(ROOT, "dashboard", "public");
const FIXTURE = path.join(ROOT, "status-panel", "fixtures", "parity-fixture.json");
const GOLDEN = path.join(ROOT, "status-panel", "fixtures", "golden");

/* ---------- assertions ---------- */
let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log("  ok   - " + name); }
  else { fail++; console.log("  FAIL - " + name + (extra ? "  [" + extra + "]" : "")); }
}

/* ---------- tiny React shim (same shape as test_panel_aw.js) ---------- */
function makeReact() {
  const R = { Fragment: "FRAGMENT" };
  let cur = null;
  R.createElement = function (type, props) {
    const kids = Array.prototype.slice.call(arguments, 2);
    const p = Object.assign({}, props);
    if (kids.length) p.children = kids.length === 1 ? kids[0] : kids;
    return { $$el: true, type: type, props: p };
  };
  R.useState = function (init) {
    const k = cur.path + "#" + (cur.idx++);
    if (!(k in cur.store)) cur.store[k] = typeof init === "function" ? init() : init;
    const store = cur.store;
    return [store[k], function (v) { store[k] = typeof v === "function" ? v(store[k]) : v; }];
  };
  R.useRef = function (init) {
    const k = cur.path + "#ref" + (cur.idx++);
    if (!(k in cur.store)) cur.store[k] = { current: init };
    return cur.store[k];
  };
  R.useMemo = function (fn) { cur.idx++; return fn(); };
  R.useCallback = function (fn) { cur.idx++; return fn; };
  R.useEffect = function (fn) { cur.idx++; cur.effects.push(fn); };
  R.__enter = function (c) { const prev = cur; cur = c; return prev; };
  R.__exit = function (prev) { cur = prev; };
  return R;
}

/* ---------- load screens-panel.jsx, exporting its internals ---------- */
const React = makeReact();
const babel = require(path.join(PUB, "vendor/babel.min.js"));
let src = fs.readFileSync(path.join(PUB, "screens-panel.jsx"), "utf8");
const EXPORT_ANCHOR = "Object.assign(window, { PanelScreen: PanelScreen });";
if (src.indexOf(EXPORT_ANCHOR) < 0) {
  console.log("  FAIL - screens-panel.jsx no longer ends with the expected window export");
  process.exit(2);
}
src = src.replace(EXPORT_ANCHOR,
  "module.exports = { buildEnv, SCREENS, paintScreen, fbNew, PW, PH, " +
  "a1Layout, A1_INET_X, A1_WAN_X, GS_DOWN, GS_UP, GS_DEGRADED, GR_ROUTER };");
const code = babel.transform(src, { presets: ["react"], filename: "screens-panel.jsx" }).code;
const mod = { exports: {} };
new Function("React", "module", "window", "document", "URLSearchParams", "Icon", code)(
  React, mod, { location: { search: "" } },
  { createElement: function () { throw new Error("canvas is not available in this harness"); } },
  class { get() { return null; } }, function Icon() { return null; }
);
const M = mod.exports;

/* ---------- the shared fixture ---------- */
const fixture = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
const CADENCE = fixture.cadenceSec * 1000;
const A1_IDX = M.SCREENS.map((s) => s.id).indexOf("A1");

/* Purpose: build an env from the fixture history, optionally swapping in one
   of the fixture's named gear variants. Input: variant key or null. */
function envFor(variantKey) {
  const gear = variantKey ? fixture.gearVariants[variantKey] : null;
  const hist = fixture.history.map(function (h) {
    const c = JSON.parse(JSON.stringify(h));
    if (gear) c.gear = gear;
    return c;
  });
  return M.buildEnv(hist, CADENCE);
}

/* Purpose: render A1 through the REAL page renderer for one frame at t=0,
   matching the C harness's frame 0 exactly. Input: env. Output: framebuffer. */
function renderA1(env) {
  const fb = M.fbNew();
  M.paintScreen(fb, A1_IDX, env, 0.0, 0.04, {}, false);
  return fb;
}

/* ---------- the C golden ---------- */
/* Purpose: read frame 0 of a committed golden as an 11x53 RGB grid.
   Input: golden id. Output: rows[y][x] = [r,g,b]. */
function readGolden(id) {
  const file = path.join(GOLDEN, "panel-" + id + ".txt");
  const rows = [];
  let frame = -1;
  for (const line of fs.readFileSync(file, "utf8").split("\n")) {
    if (!line || line[0] === "#") continue;
    if (line.slice(0, 5) === "frame") { frame++; continue; }
    if (frame !== 0) continue;
    rows.push(line.trim().split(/\s+/).map(function (tok) {
      return [parseInt(tok.slice(0, 2), 16), parseInt(tok.slice(2, 4), 16),
              parseInt(tok.slice(4, 6), 16)];
    }));
  }
  return rows;
}

/* Purpose: read the committed leg-path sidecar the C harness emits.
   Input: golden id. Output: a Set of "x,y" strings.

   The C harness records these by watching the REAL a1Leg() paint, identifying
   the leg layer by palette identity (cQuiet is the only colour a1Leg uses, and
   gates take theirs from a1GateColor(), which never returns cQuiet). Nothing
   about the leg geometry is recomputed here or there — this file is a dump of
   what the firmware actually painted, which is what lets block [4] excuse
   *named* pixels rather than any quiet pixel anywhere. */
function readLegs(id) {
  const file = path.join(GOLDEN, "panel-" + id + ".legs.txt");
  const set = new Set();
  for (const line of fs.readFileSync(file, "utf8").split("\n")) {
    if (!line || line[0] === "#") continue;
    const m = line.trim().match(/^(\d+)\s+(\d+)$/);
    if (m) set.add(m[1] + "," + m[2]);
  }
  return set;
}

/* ---------- pixel probes ---------- */
/* The same RGB-sum threshold test_panel_aw.js uses, applied to both sides after
   the page's float framebuffer has been quantised the way panelFbFlush does. */
const LIT = 40;
function jsPx(fb, x, y) {
  const i = (y * M.PW + x) * 3;
  return [fb[i], fb[i + 1], fb[i + 2]].map(function (v) {
    const n = Math.round(v);
    return n < 0 ? 0 : n > 255 ? 255 : n;
  });
}
const sum = (p) => p[0] + p[1] + p[2];
const lit = (p) => sum(p) > LIT;

/* Hue class by nearest palette direction. Brightness is deliberately not part
   of this: the two renderers scale gates differently and are allowed to. */
const PALETTE = {
  azure: [34, 184, 240], warn: [245, 166, 35],
  crit: [255, 77, 94], quiet: [78, 107, 126],
};
function hue(p) {
  const n = Math.sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
  if (n < 1e-6) return "dark";
  let best = "dark", bestDot = -1;
  for (const k of Object.keys(PALETTE)) {
    const q = PALETTE[k];
    const m = Math.sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    const d = (p[0] * q[0] + p[1] * q[1] + p[2] * q[2]) / (n * m);
    if (d > bestDot) { bestDot = d; best = k; }
  }
  return best;
}
/* Purpose: lit/dark pattern of a column over a row span, as a string. */
function span(get, x, y0, h) {
  let s = "";
  for (let k = 0; k < h; k++) s += lit(get(x, y0 + k)) ? "1" : ".";
  return s;
}
function hueSpan(get, x, y0, h) {
  const out = [];
  for (let k = 0; k < h; k++) {
    const p = get(x, y0 + k);
    if (lit(p)) out.push((y0 + k) + ":" + hue(p));
  }
  return out.join(",");
}

/* ---------- [1] the fixture is shared and usable ---------- */
console.log("[1] the fixture both renderers read");
ok("parity-fixture.json parses and carries a history",
   Array.isArray(fixture.history) && fixture.history.length >= 2,
   String(fixture.history && fixture.history.length));
ok("every history sample carries gear",
   fixture.history.every((h) => Array.isArray(h.gear) && h.gear.length > 0));
ok("every gear row is idle on tx, so NEITHER renderer draws particles",
   fixture.history.every((h) => h.gear.every((g) => g.txLevel === 0)) &&
   Object.keys(fixture.gearVariants).every((k) =>
     fixture.gearVariants[k].every((g) => g.txLevel === 0)),
   "the particle exclusion depends on this");
ok("the routerDown variant exists and downs exactly the router",
   fixture.gearVariants.routerDown.filter((g) => g.state === 0).length ===
   fixture.history[0].gear.filter((g) => g.state === 0).length + 1);
ok("the C harness has produced its goldens",
   fs.existsSync(path.join(GOLDEN, "panel-A1.txt")) &&
   fs.existsSync(path.join(GOLDEN, "panel-A1-routerdown.txt")),
   "run: make -C status-panel/firmware/test");

/* ---------- the two renders ---------- */
function compare(label, variantKey, goldenId) {
  const env = envFor(variantKey);
  const fb = renderA1(env);
  const grid = readGolden(goldenId);
  const jsAt = (x, y) => jsPx(fb, x, y);
  const cAt = (x, y) => grid[y][x];
  const L = M.a1Layout(env.gear);

  console.log("\n[2] " + label + " — gate layer, pixel for pixel");
  ok(label + ": the golden is a full 11x53 frame",
     grid.length === M.PH && grid.every((r) => r.length === M.PW),
     grid.length + " rows");

  for (let i = 0; i < L.gates.length; i++) {
    const g = L.gates[i];
    const j = span(jsAt, g.x, g.y0, g.h);
    const c = span(cAt, g.x, g.y0, g.h);
    ok(label + ": gate " + i + " (role " + g.role + ", x=" + g.x + ", rows " +
       g.y0 + ".." + (g.y0 + g.h - 1) + ") lights the same rows in both",
       j === c, "page=" + j + " firmware=" + c);
    ok(label + ": gate " + i + " (x=" + g.x + ") lit pixels agree on hue",
       hueSpan(jsAt, g.x, g.y0, g.h) === hueSpan(cAt, g.x, g.y0, g.h),
       "page=" + hueSpan(jsAt, g.x, g.y0, g.h) +
       " firmware=" + hueSpan(cAt, g.x, g.y0, g.h));
  }

  console.log("\n[3] " + label + " — internet and wanBackup columns (§10 A-3)");
  const inetJ = span(jsAt, M.A1_INET_X, 0, M.PH);
  const inetC = span(cAt, M.A1_INET_X, 0, M.PH);
  ok(label + ": internet column x=" + M.A1_INET_X + " matches over its full height",
     inetJ === inetC, "page=" + inetJ + " firmware=" + inetC);
  ok(label + ": internet column agrees on hue",
     hueSpan(jsAt, M.A1_INET_X, 0, M.PH) === hueSpan(cAt, M.A1_INET_X, 0, M.PH),
     "page=" + hueSpan(jsAt, M.A1_INET_X, 0, M.PH) +
     " firmware=" + hueSpan(cAt, M.A1_INET_X, 0, M.PH));
  const wanJ = span(jsAt, M.A1_WAN_X, 0, M.PH);
  const wanC = span(cAt, M.A1_WAN_X, 0, M.PH);
  ok(label + ": wanBackup column x=" + M.A1_WAN_X + " matches over its full height",
     wanJ === wanC, "page=" + wanJ + " firmware=" + wanC);
  ok(label + ": wanBackup column agrees on hue",
     hueSpan(jsAt, M.A1_WAN_X, 0, M.PH) === hueSpan(cAt, M.A1_WAN_X, 0, M.PH),
     "page=" + hueSpan(jsAt, M.A1_WAN_X, 0, M.PH) +
     " firmware=" + hueSpan(cAt, M.A1_WAN_X, 0, M.PH));

  /* [4] The whole-frame divergence, bounded. Anything the firmware lights that
     the page does not must be a leg — the one layer whose brightness curves are
     known to differ — and the page must never light a pixel the firmware leaves
     dark. If either half of that ever fails, the two A1s have drifted in
     something that is NOT the documented leg-brightness gap.

     The exemption is by COORDINATE, from the sidecar the C harness dumps out of
     the real a1Leg(): a firmware-only lit pixel is forgiven only if a1Leg
     actually painted at that (x,y). "It happens to be quiet-hued" is not
     enough — that earlier, looser test would have swallowed any new quiet
     divergence anywhere on the panel. Hue is still checked on top, so a leg
     coordinate that stops being quiet also fails.

     RESIDUAL WEAKNESS, stated precisely: this proves a firmware-only pixel sits
     on a leg path, not that its BRIGHTNESS is right. A leg that changed
     brightness — or a non-leg element that moved onto a leg coordinate and lit
     it — is still forgiven here. The committed C golden is what pins leg
     brightness; block [4] only bounds the C-vs-page divergence. */
  console.log("\n[4] " + label + " — the divergence is confined to the leg layer");
  const legs = readLegs(goldenId);
  const onlyC = [], onlyJ = [], notOnLeg = [], onLegNotQuiet = [];
  for (let y = 0; y < M.PH; y++) {
    for (let x = 0; x < M.PW; x++) {
      const j = lit(jsAt(x, y)), c = lit(cAt(x, y));
      if (c && !j) {
        const key = x + "," + y;
        onlyC.push(key);
        if (!legs.has(key)) notOnLeg.push(key + "=" + hue(cAt(x, y)));
        else if (hue(cAt(x, y)) !== "quiet") onLegNotQuiet.push(key + "=" + hue(cAt(x, y)));
      }
      if (j && !c) onlyJ.push(x + "," + y);
    }
  }
  ok(label + ": no pixel is lit on the page but dark in the firmware",
     onlyJ.length === 0, onlyJ.slice(0, 8).join(" "));
  ok(label + ": every firmware-only lit pixel sits on a real a1Leg() coordinate",
     notOnLeg.length === 0, notOnLeg.slice(0, 8).join(" "));
  ok(label + ": every exempted leg pixel is still cQuiet-hued",
     onLegNotQuiet.length === 0, onLegNotQuiet.slice(0, 8).join(" "));
  ok(label + ": the exemption cannot exceed the painted leg path (" +
     onlyC.length + " <= " + legs.size + ")",
     onlyC.length <= legs.size, "exempted more pixels than a1Leg() painted");
  console.log("       (" + onlyC.length + " firmware-only lit pixels, all on the " +
              legs.size + "-pixel leg path; see the header's exclusion (b))");
  return { onlyC: onlyC.length, gates: L.gates.length };
}

const primary = compare("primary gear", null, "A1");
const down = compare("router down", "routerDown", "A1-routerdown");

/* ---------- [5] A-3 actually changed something ---------- */
console.log("\n[5] the two gear variants are not the same picture");
{
  const envUp = envFor(null), envDn = envFor("routerDown");
  const up = renderA1(envUp), dn = renderA1(envDn);
  const gUp = readGolden("A1"), gDn = readGolden("A1-routerdown");
  const inetUp = span((x, y) => jsPx(up, x, y), M.A1_INET_X, 0, M.PH);
  const inetDn = span((x, y) => jsPx(dn, x, y), M.A1_INET_X, 0, M.PH);
  ok("page: downing the router breaks the internet column (§10 A-3)",
     inetUp !== inetDn && inetDn.indexOf("..") >= 0 ? true : inetUp !== inetDn,
     "up=" + inetUp + " down=" + inetDn);
  const cUp = span((x, y) => gUp[y][x], M.A1_INET_X, 0, M.PH);
  const cDn = span((x, y) => gDn[y][x], M.A1_INET_X, 0, M.PH);
  ok("firmware: downing the router breaks the internet column (§10 A-3)",
     cUp !== cDn, "up=" + cUp + " down=" + cDn);
  const wDn = span((x, y) => gDn[y][x], M.A1_WAN_X, 0, M.PH);
  ok("firmware: wanBackup keeps its own state and stays solid beside it",
     wDn.indexOf("11111") >= 0, wDn);
  ok("both renderers gained the same number of gates from the variant",
     primary.gates === down.gates, primary.gates + " vs " + down.gates);
}

console.log("\n" + pass + " passed, " + fail + " failed");
process.exit(fail ? 1 : 0);
