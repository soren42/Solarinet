#!/usr/bin/env node
/* Headless tests for CONTRACT-AW's dashboard lane (AW3) in
   dashboard/public/screens-panel.jsx:

     [1] A1 flow-gate GEOMETRY, asserted on the framebuffer the renderer
         actually produces — §9 A-1's heights, stagger and AP distribution.
     [2] A1 state treatments: broken down-gates, §10 A-3's internet column,
         wanBackup as the surviving path, no-gear falling back to NO DATA.
     [3] every fixture scenario renders every screen, nothing empty, nothing
         negative.
     [4] §3.2 command packing — kind 8 (screenIdx<<1)|enabled and kind 9
         (screenIdx<<3)|weightCode, over §10 D2's screenIdx = theme*3 + slot —
         asserted from the rendered controls, not from the helpers alone.
     [5] §3.3 panelScreenConfig decoding across every wire shape, §10 D3's
         reserved bits and cfgPersisted, and the doctrine that matters most:
         absent or malformed reads as UNKNOWN, never as the factory default.
     [6] alignment with the shapes AW1's daemon and API actually emit.
     [7] conformance to §9 A-1, the parity oracle both renderers implement.
     [8] the fixture file itself.

   No DOM, no canvas, no network: the module is loaded into a sandbox with a
   tiny React shim, so this exercises what the page RENDERS and SENDS.
   Run: node tests/dashboard/test_panel_aw.js   — exits non-zero on failure. */
"use strict";

const fs = require("fs");
const path = require("path");

const ROOT = path.join(__dirname, "..", "..");
const PUB = path.join(ROOT, "dashboard", "public");
const FIXTURE = path.join(ROOT, "status-panel", "fixtures", "panel-snapshot.json");

/* ---------- assertions ---------- */
let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log("  ok   - " + name); }
  else { fail++; console.log("  FAIL - " + name + (extra ? "  [" + extra + "]" : "")); }
}

/* ---------- tiny React (same shape as test_lifecycle_ui.js, plus useRef,
   which the panel page needs for its canvas and clock refs) ---------- */
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
function render(React, el, ctx, p0) {
  if (el == null || el === false || el === true) return null;
  if (Array.isArray(el)) return el.map((c, i) => render(React, c, ctx, p0 + "[" + i + "]")).filter((x) => x != null);
  if (typeof el !== "object") return String(el);
  if (!el.$$el) return null;
  if (typeof el.type === "function") {
    const p = p0 + "/" + (el.type.name || "anon");
    const frame = { store: ctx.store, path: p, idx: 0, effects: ctx.effects };
    const prev = React.__enter(frame);
    let out;
    try { out = el.type(el.props); } finally { React.__exit(prev); }
    return render(React, out, ctx, p);
  }
  const node = { tag: String(el.type), props: el.props, children: [] };
  if (el.props && el.props.children != null) {
    const kids = render(React, el.props.children, ctx, p0 + ">");
    node.children = Array.isArray(kids) ? kids : (kids == null ? [] : [kids]);
  }
  return node;
}
function walk(node, fn) {
  if (node == null || typeof node !== "object") return;
  if (Array.isArray(node)) return node.forEach((n) => walk(n, fn));
  fn(node);
  (node.children || []).forEach((c) => walk(c, fn));
}
function findAll(tree, pred) { const out = []; walk(tree, (n) => { if (pred(n)) out.push(n); }); return out; }
const textOf = (n) => { let s = ""; walk(n, (x) => { (x.children || []).forEach((c) => { if (typeof c === "string") s += c; }); }); return s; };
function mount(React, el) {
  const ctx = { store: {}, effects: [] };
  return { tree: render(React, el, ctx, ""), ctx: ctx, rerender: () => render(React, el, ctx, "") };
}
const buttons = (tree) => findAll(tree, (n) => n.tag === "button");
const pressed = (tree) => buttons(tree).filter((b) => b.props["aria-pressed"] === true).map(textOf);

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
  "a1Layout, a1Spread, a1GateGeom, A1_BAND, A1_BANDS, A1_INET_X, A1_WAN_X, " +
  "readScreenCfg, readCfgPersisted, packScreenEn, packScreenWt, cmdGroup, cmdLabel, WEIGHTS, " +
  "weightLabel, N_SCREENS, CMD_KIND, ScreenTileCfg, VirtualPanel, " +
  "GR_ROUTER, GR_SWITCH, GR_HUB, GR_AP, GR_WANBK, GS_DOWN, GS_UP, GS_DEGRADED };");
const code = babel.transform(src, { presets: ["react"], filename: "screens-panel.jsx" }).code;

const win = { location: { search: "" } };
const doc = { createElement: function () { throw new Error("canvas is not available in this harness"); } };
const mod = { exports: {} };
new Function("React", "module", "window", "document", "URLSearchParams", "Icon", code)(
  React, mod, win, doc, class { get() { return null; } }, function Icon() { return null; }
);
const M = mod.exports;

const fixture = JSON.parse(fs.readFileSync(FIXTURE, "utf8"));
const CADENCE = fixture.cadenceSec * 1000;
const scenarioOf = (id) => fixture.scenarios.filter((s) => s.id === id)[0];

/* ---------- framebuffer probes ---------- */
const LIT = 40;                    // RGB sum; see [1]'s note on the threshold
function sum(fb, x, y) { const i = (y * M.PW + x) * 3; return fb[i] + fb[i + 1] + fb[i + 2]; }
/* Longest contiguous lit run in a column, as { len, y0 }. A gate is a solid
   vertical line, so the run IS the gate; stray particle pixels elsewhere in
   the column cannot lengthen it. */
function colRun(fb, x) {
  let best = 0, bestY = -1, len = 0, start = -1;
  for (let y = 0; y < M.PH; y++) {
    if (sum(fb, x, y) > LIT) {
      if (len === 0) start = y;
      len++;
      if (len > best) { best = len; bestY = start; }
    } else len = 0;
  }
  return { len: best, y0: bestY };
}
function colLit(fb, x) { let n = 0; for (let y = 0; y < M.PH; y++) if (sum(fb, x, y) > LIT) n++; return n; }
function fbStats(fb) {
  let lit = 0, neg = 0, over = 0;
  for (let i = 0; i < fb.length; i += 3) {
    const l = (fb[i] + fb[i + 1] + fb[i + 2]) / 3;
    if (l > 0.5) lit++;
    if (l < -0.001) neg++;
    if (fb[i] > 255 || fb[i + 1] > 255 || fb[i + 2] > 255) over++;
  }
  return { lit: lit, neg: neg, over: over };
}
const A1_IDX = M.SCREENS.map((s) => s.id).indexOf("A1");

/* Build an env carrying an arbitrary gear array, on top of the fixture's
   newest normal sample so everything else in env is real. */
function envWithGear(gear) {
  const base = scenarioOf("normal").history;
  const hist = base.map(function (h, i) {
    const c = JSON.parse(JSON.stringify(h));
    if (gear === null) delete c.gear; else c.gear = gear;
    return c;
  });
  return M.buildEnv(hist, CADENCE);
}
const g = (role, state, rx, tx) => ({ role: role, state: state, rxLevel: rx, txLevel: tx });
/* The real 9-device inventory (CONTRACT-AW §1) in §3.1 wire order, held at
   level 0 so NO particles are drawn: the contract makes 0 idle, so the frame
   contains gates and leg wires only and every lit pixel above the threshold is
   structural. That is what makes an exact geometry assertion possible. */
const QUIET_9 = [
  g(M.GR_ROUTER, M.GS_UP, 0, 0),
  g(M.GR_SWITCH, M.GS_UP, 0, 0),
  g(M.GR_HUB, M.GS_UP, 0, 0), g(M.GR_HUB, M.GS_UP, 0, 0), g(M.GR_HUB, M.GS_UP, 0, 0),
  g(M.GR_AP, M.GS_UP, 0, 0), g(M.GR_AP, M.GS_UP, 0, 0), g(M.GR_AP, M.GS_UP, 0, 0),
  g(M.GR_WANBK, M.GS_UP, 0, 0),
];
function renderA1(env, frames) {
  const fb = M.fbNew();
  const st = {};
  const n = frames || 1;
  for (let k = 1; k <= n; k++) M.paintScreen(fb, A1_IDX, env, k * 0.04, 0.04, st, false);
  return fb;
}

console.log("\n[1] A1 flow-gate geometry — §9 A-1 heights, stagger and AP distribution");
{
  const env = envWithGear(QUIET_9);
  ok("the 9-device inventory decodes to 9 gear rows", env.gearCount === 9, String(env.gearCount));
  const L = M.a1Layout(env.gear);
  const fb = renderA1(env);

  const byRole = function (r) { return L.gates.filter((x) => x.role === r); };
  const router = byRole(M.GR_ROUTER)[0];
  const sw = byRole(M.GR_SWITCH)[0];
  const hubs = byRole(M.GR_HUB);
  const aps = byRole(M.GR_AP);

  ok("bands are ordered APs → hubs → switch → router, left to right",
    aps[0].x < hubs[0].x && hubs[hubs.length - 1].x < sw.x && sw.x < router.x,
    aps.map((a) => a.x).join(",") + " | " + hubs.map((h) => h.x).join(",") + " | " + sw.x + " | " + router.x);

  // ---- §1: the router is the FULL display height ----
  const r = colRun(fb, router.x);
  ok("router gate is 11 rows (full height)", r.len === 11 && r.y0 === 0, JSON.stringify(r));
  ok("router column is lit edge to edge", colLit(fb, router.x) === 11, String(colLit(fb, router.x)));

  // ---- §1: the switch is 75% ----
  const s = colRun(fb, sw.x);
  ok("switch gate is 8 rows", s.len === 8, JSON.stringify(s));
  ok("switch gate is vertically centred", s.y0 === Math.floor((11 - 8) / 2), JSON.stringify(s));

  // ---- §1: hubs are 50% (5-6 rows), §4: STAGGERED ----
  const hr = hubs.map((h) => colRun(fb, h.x));
  ok("all three hub gates are 5 or 6 rows", hr.every((h) => h.len === 5 || h.len === 6),
    JSON.stringify(hr));
  ok("hub gates stagger top/bottom, alternating",
    hr[0].y0 === 0 && hr[1].y0 === 11 - hr[1].len && hr[2].y0 === 0, JSON.stringify(hr));
  ok("staggered hubs do not all share a row band",
    hr[0].y0 !== hr[1].y0 && hr[1].y0 !== hr[2].y0, JSON.stringify(hr));

  // ---- §1: APs are 25% ----
  /* §9 A-1: h=3, y0 = round(i*(11-3)/(n-1)) — distributed down the height. */
  const ar = aps.map((a) => colRun(fb, a.x));
  ok("all three AP gates are 3 rows", ar.every((a) => a.len === 3), JSON.stringify(ar));
  ok("AP gates are distributed down the height per §9 A-1 (rows 0, 4, 8)",
    ar.map((a) => a.y0).join(",") === "0,4,8", JSON.stringify(ar));
  ok("a lone AP is centred at y0=4 (§9 A-1's n==1 case)",
    M.a1GateGeom(M.GR_AP, 0, 1).y0 === 4 && M.a1GateGeom(M.GR_AP, 0, 1).h === 3,
    JSON.stringify(M.a1GateGeom(M.GR_AP, 0, 1)));

  // ---- §4: the internet is the rightmost column, RIGHT of the router ----
  ok("internet column sits right of the router gate", M.A1_INET_X > router.x,
    M.A1_INET_X + " vs " + router.x);
  ok("wanBackup is drawn beside the internet, further right",
    M.A1_WAN_X === M.A1_INET_X + 1 && M.A1_WAN_X <= M.PW - 1, M.A1_WAN_X + "/" + M.PW);
  ok("with the router up the internet column is a full-height marker",
    colRun(fb, M.A1_INET_X).len === 11 && colRun(fb, M.A1_INET_X).y0 === 0,
    JSON.stringify(colRun(fb, M.A1_INET_X)));
  ok("wanBackup occupies rows 3..7 (panelScreensA.c:154)",
    colRun(fb, M.A1_WAN_X).len === 5 && colRun(fb, M.A1_WAN_X).y0 === 3,
    JSON.stringify(colRun(fb, M.A1_WAN_X)));
  ok("the wanBackup gear row is recognised", !!L.wan && L.wan.role === M.GR_WANBK);
  ok("wanBackup is NOT laid out as a gate in the chain",
    L.gates.every((x) => x.role !== M.GR_WANBK));

  // ---- legs connect every band, so the picture is a path and not columns ----
  ok("every gate feeds forward (legs = gates, one per gate)",
    L.legs.length === L.gates.length, L.legs.length + " legs / " + L.gates.length + " gates");
  ok("the last leg terminates at the internet column",
    L.legs[L.legs.length - 1].x1 === M.A1_INET_X);
  ok("no leg runs backwards", L.legs.every((l) => l.x1 > l.x0));
  ok("no gate is off-grid", L.gates.every((x) => x.x >= 0 && x.x < M.PW
    && x.y0 >= 0 && x.y0 + x.h <= M.PH));
}

console.log("\n[2] A1 state treatments — broken gates, §10 A-3 internet/wanBackup, no-gear fallback");
{
  // A DOWN gate is "rendered broken" (§4): the column keeps its extent but is
  // no longer solid, so it reads as severed rather than merely red.
  const gear = QUIET_9.slice();
  gear[0] = g(M.GR_ROUTER, M.GS_DOWN, 0, 0);
  const env = envWithGear(gear);
  const L = M.a1Layout(env.gear);
  const router = L.gates.filter((x) => x.role === M.GR_ROUTER)[0];
  const fb = renderA1(env);
  const lit = colLit(fb, router.x);
  ok("a down router gate is broken, not solid", lit > 0 && lit < 11, String(lit));
  ok("the broken gate still spans the full height",
    sum(fb, router.x, 0) > LIT && sum(fb, router.x, 10) > LIT);
  /* §10 A-3, the ruling on this lane's flag: the internet is only meaningful
     THROUGH the router, so the marker takes the router's state treatment and is
     never a healthy column over a dead gateway. */
  const inet = colLit(fb, M.A1_INET_X);
  ok("a down router breaks the internet column too (§10 A-3)",
    inet > 0 && inet < 11, String(inet));
  ok("...and it is still full-extent, so it reads as severed and not absent",
    colRun(fb, M.A1_INET_X).y0 === 0 && sum(fb, M.A1_INET_X, 10) > LIT);
}
{
  // A degraded router tints the internet warn WITHOUT breaking it: degraded is
  // "still passing traffic", and a broken column would overstate the outage.
  const gear = QUIET_9.slice();
  gear[0] = g(M.GR_ROUTER, M.GS_DEGRADED, 0, 0);
  const fb = renderA1(envWithGear(gear));
  ok("a degraded router leaves the internet column solid (§10 A-3)",
    colLit(fb, M.A1_INET_X) === 11, String(colLit(fb, M.A1_INET_X)));
  const up = renderA1(envWithGear(QUIET_9));
  const hue = (b, x) => { const i = (5 * M.PW + x) * 3; return [b[i], b[i + 1], b[i + 2]]; };
  ok("...in a different hue from the healthy treatment",
    hue(fb, M.A1_INET_X).join() !== hue(up, M.A1_INET_X).join(),
    hue(fb, M.A1_INET_X).join() + " vs " + hue(up, M.A1_INET_X).join());
}
{
  /* §10 A-3's payoff: with the router down, wanBackup keeps its OWN state and
     is the only unbroken path on screen. This is the whole reason the screen
     exists, so it gets an explicit test rather than riding on the gate loop. */
  const gear = QUIET_9.slice();
  gear[0] = g(M.GR_ROUTER, M.GS_DOWN, 0, 0);
  gear[8] = g(M.GR_WANBK, M.GS_UP, 3, 3);
  const fb = renderA1(envWithGear(gear));
  ok("wanBackup stays solid while the internet marker is broken",
    colRun(fb, M.A1_WAN_X).len === 5 && colLit(fb, M.A1_INET_X) < 11,
    JSON.stringify(colRun(fb, M.A1_WAN_X)) + " inet=" + colLit(fb, M.A1_INET_X));
  const gear2 = gear.slice();
  gear2[8] = g(M.GR_WANBK, M.GS_DOWN, 0, 0);
  const fb2 = renderA1(envWithGear(gear2));
  ok("a down wanBackup breaks too — it is not pinned to the router's state",
    colLit(fb2, M.A1_WAN_X) < 5 && colLit(fb2, M.A1_WAN_X) > 0,
    String(colLit(fb2, M.A1_WAN_X)));
}
{
  const env = envWithGear(null);
  ok("no gear array → gearCount 0", env.gearCount === 0, String(env.gearCount));
  const fb = renderA1(env);
  ok("gearCount 0 renders the no-data treatment, not an empty screen",
    fbStats(fb).lit > 0, JSON.stringify(fbStats(fb)));
  // NO DATA is centred text on row 3; there is no gate at the router column.
  ok("...and paints no gates", colRun(fb, 47).len <= 1 && colRun(fb, 41).len <= 1);
}
{
  // Rows the server should never send, but a mapper bug would: an unknown role
  // must be DROPPED, because a mis-roled device moves a gate into the wrong
  // band and makes the picture lie about the network.
  const env = envWithGear([g(9, M.GS_UP, 3, 3), g(M.GR_ROUTER, M.GS_UP, 3, 3), { role: "switch", state: "up", rxLevel: 2, txLevel: 2 }]);
  ok("an unknown role is dropped, not coerced to router", env.gearCount === 2, String(env.gearCount));
  ok("string roles from a text mapper still decode",
    env.gear[1].role === M.GR_SWITCH && env.gear[1].state === M.GS_UP, JSON.stringify(env.gear[1]));
  const fb = renderA1(env);
  ok("a two-band inventory still renders a connected path", fbStats(fb).lit > 0);
  ok("...with no negative pixels", fbStats(fb).neg === 0);
}
{
  const gear = [];
  for (let i = 0; i < 20; i++) gear.push(g(M.GR_AP, M.GS_UP, 4, 4));
  const env = envWithGear(gear);
  ok("more than 12 gear rows are capped at 12 (§3.1 gearCount 0..12)",
    env.gearCount === 12, String(env.gearCount));
  const L = M.a1Layout(env.gear);
  ok("12 APs all stay inside the AP band and on-grid",
    L.gates.every((x) => x.x >= M.A1_BAND[M.GR_AP][0] && x.x <= M.A1_BAND[M.GR_AP][1]
      && x.y0 >= 0 && x.y0 + x.h <= M.PH));
  ok("a single-gate band is centred, not flush left",
    M.a1Spread(1, 38, 44)[0] === 41, JSON.stringify(M.a1Spread(1, 38, 44)));
}

console.log("\n[3] every fixture scenario renders every screen");
{
  fixture.scenarios.forEach(function (sc) {
    const env = M.buildEnv(sc.history, CADENCE);
    const head = sc.history[sc.history.length - 1];
    const ps = head.panelState;
    const armed = !!(env.alarmActive && !(ps && ps.alarmAcked
      && Number(ps.ackedEpisodeId) === Number(env.episodeId)));
    let empty = [], neg = [];
    const fb = M.fbNew();
    M.SCREENS.forEach(function (s, i) {
      const st = {};
      for (let k = 1; k <= 75; k++) M.paintScreen(fb, i, env, k * 0.04, 0.04, st, armed);
      const a = fbStats(fb);
      if (a.lit === 0) empty.push(s.id);
      if (a.neg > 0) neg.push(s.id);
    });
    ok(sc.id + ": no screen renders an empty framebuffer", empty.length === 0, empty.join(","));
    ok(sc.id + ": no screen renders negative pixels", neg.length === 0, neg.join(","));
  });
}
{
  // The scenarios that carry gear must actually reach the gate renderer, or
  // block [3] would pass on the no-data fallback and prove nothing.
  ["normal", "alarm", "stale", "emptyHistory"].forEach(function (id) {
    const env = M.buildEnv(scenarioOf(id).history, CADENCE);
    ok(id + ": A1 has gear and draws gates", env.gearCount > 0
      && M.a1Layout(env.gear).gates.length === env.gearCount
        - env.gear.filter((x) => x.role === M.GR_WANBK).length,
      "gearCount=" + env.gearCount);
  });
  const nd = M.buildEnv(scenarioOf("noData").history, CADENCE);
  ok("noData: A1 has no gear, exercising the §E2 fallback", nd.gearCount === 0);
  const st = M.buildEnv(scenarioOf("stale").history, CADENCE);
  ok("stale: gear is present but idle (level 0 everywhere but the router)",
    st.gear.every((x) => x.tx === 0), JSON.stringify(st.gear.map((x) => x.tx)));
}

console.log("\n[4] §3.2 command packing, asserted from the rendered controls");
{
  ok("packScreenEn is (screenIdx<<1)|enabled",
    M.packScreenEn(0, true) === 1 && M.packScreenEn(0, false) === 0
    && M.packScreenEn(1, true) === 3 && M.packScreenEn(11, false) === 22
    && M.packScreenEn(11, true) === 23,
    [M.packScreenEn(0, true), M.packScreenEn(1, true), M.packScreenEn(11, false)].join(","));
  ok("packScreenWt is (screenIdx<<3)|weightCode",
    M.packScreenWt(0, 0) === 0 && M.packScreenWt(0, 5) === 5
    && M.packScreenWt(1, 4) === 12 && M.packScreenWt(11, 5) === 93,
    [M.packScreenWt(1, 4), M.packScreenWt(11, 5)].join(","));
  ok("the two kinds use DIFFERENT shifts (the easy thing to get wrong)",
    M.packScreenEn(3, false) === 6 && M.packScreenWt(3, 0) === 24);
  ok("weight codes are the contract's six, in order",
    M.WEIGHTS.map((w) => w.code).join(",") === "0,1,2,3,4,5"
    && M.WEIGHTS.map((w) => w.mul).join(",") === "0.25,0.5,1,2,5,10",
    M.WEIGHTS.map((w) => w.label).join(","));
  ok("a 30 s base with 5x / half / 1x is 150 / 15 / 30 s (§1)",
    30 * M.WEIGHTS[4].mul === 150 && 30 * M.WEIGHTS[1].mul === 15 && 30 * M.WEIGHTS[2].mul === 30);
}
{
  const sent = [];
  const send = function (kind, arg) { sent.push({ kind: kind, arg: arg }); };
  const cfg = { enabled: true, weightCode: 2 };
  const m = mount(React, React.createElement(M.ScreenTileCfg, {
    idx: 1, cfg: cfg, pend: {}, send: send, disabled: false,
  }));
  const btns = buttons(m.tree);
  ok("a tile offers On/Off plus the six weights", btns.length === 8, String(btns.length));

  btns.filter((b) => textOf(b) === "Off")[0].props.onClick();
  ok("turning A1 off posts kind 8 with (1<<1)|0 = 2",
    sent.length === 1 && sent[0].kind === 8 && sent[0].arg === 2, JSON.stringify(sent));

  sent.length = 0;
  btns.filter((b) => textOf(b) === "On")[0].props.onClick();
  ok("turning A1 on posts kind 8 with (1<<1)|1 = 3",
    sent[0].kind === 8 && sent[0].arg === 3, JSON.stringify(sent));

  sent.length = 0;
  btns.filter((b) => textOf(b) === "5×")[0].props.onClick();
  ok("selecting 5x on A1 posts kind 9 with (1<<3)|4 = 12",
    sent[0].kind === 9 && sent[0].arg === 12, JSON.stringify(sent));

  sent.length = 0;
  btns.filter((b) => textOf(b) === "¼×")[0].props.onClick();
  ok("selecting quarter on A1 posts kind 9 with (1<<3)|0 = 8",
    sent[0].kind === 9 && sent[0].arg === 8, JSON.stringify(sent));

  // ...and the packing round-trips back to the screen it addressed.
  sent.length = 0;
  const m11 = mount(React, React.createElement(M.ScreenTileCfg, {
    idx: 11, cfg: cfg, pend: {}, send: send, disabled: false,
  }));
  buttons(m11.tree).filter((b) => textOf(b) === "10×")[0].props.onClick();
  ok("D2 at 10x posts (11<<3)|5 = 93, decoding back to screen 11",
    sent[0].arg === 93 && (sent[0].arg >> 3) === 11 && (sent[0].arg & 7) === 5, JSON.stringify(sent));
}
{
  /* §10 D2, NORMATIVE: screenIdx = theme*3 + slot, themes 0..3 × slots 0..2.
     The page's SCREENS array must BE that mapping, or a control addresses one
     screen and the panel changes another. */
  ok("SCREENS is laid out as theme*3 + slot",
    M.SCREENS.map((s) => s.id).join(",") === "A0,A1,A2,B0,B1,B2,C0,C1,C2,D0,D1,D2",
    M.SCREENS.map((s) => s.id).join(","));
  {
    let bad = [];
    "ABCD".split("").forEach(function (th, theme) {
      for (let slot = 0; slot < 3; slot++) {
        const idx = theme * 3 + slot;
        if (M.SCREENS[idx].id !== th + slot) bad.push(th + slot + " != idx " + idx);
        if ((M.packScreenWt(idx, 3) >> 3) !== idx) bad.push("wt idx " + idx);
        if ((M.packScreenEn(idx, true) >> 1) !== idx) bad.push("en idx " + idx);
      }
    });
    ok("every theme/slot pair round-trips through both packings (§10 D2)",
      bad.length === 0, bad.slice(0, 4).join("; "));
  }
  ok("C1 is theme 2 slot 1 = screenIdx 7, and kind 9 packs it as 59",
    M.SCREENS[2 * 3 + 1].id === "C1" && M.packScreenWt(7, 3) === 59,
    String(M.packScreenWt(7, 3)));
  ok("§10 D2 validation bounds hold: kind 8 arg <= 23, kind 9 screenIdx <= 11",
    M.packScreenEn(11, true) === 23 && (M.packScreenWt(11, 5) >> 3) === 11);

  ok("kinds 8/9 are keyed PER SCREEN so one tile cannot spin another",
    M.cmdGroup(8, M.packScreenEn(1, true)) === "screenEn:1"
    && M.cmdGroup(9, M.packScreenWt(1, 4)) === "screenWt:1"
    && M.cmdGroup(8, M.packScreenEn(7, true)) === "screenEn:7",
    M.cmdGroup(8, M.packScreenEn(7, true)));
  ok("enable and weight are separate keys on the same screen",
    M.cmdGroup(8, M.packScreenEn(1, true)) !== M.cmdGroup(9, M.packScreenWt(1, 2)));
  ok("the CP-era kinds keep their original keys",
    M.cmdGroup(1, 0) === "theme" && M.cmdGroup(3, 50) === "brightness"
    && M.cmdGroup(4, 0) === "brightness" && M.cmdGroup(6, 6) === "dwell");
  ok("a failure names the screen it belongs to, not a bare number",
    M.cmdLabel(8, M.packScreenEn(1, false)) === "A1 enable"
    && M.cmdLabel(9, M.packScreenWt(11, 5)) === "D2 weight",
    M.cmdLabel(8, M.packScreenEn(1, false)) + " / " + M.cmdLabel(9, M.packScreenWt(11, 5)));
  ok("the CP-era labels are unchanged", M.cmdLabel(5, 0) === "acknowledge"
    && M.cmdLabel(4, 0) === "auto-brightness");
}
{
  const send = function () {};
  const busy = { "screenEn:1": { kind: 8, arg: 3, status: "pending", at: 0 } };
  const t1 = mount(React, React.createElement(M.ScreenTileCfg, {
    idx: 1, cfg: { enabled: true, weightCode: 2 }, pend: busy, send: send, disabled: false,
  })).tree;
  const t2 = mount(React, React.createElement(M.ScreenTileCfg, {
    idx: 2, cfg: { enabled: true, weightCode: 2 }, pend: busy, send: send, disabled: false,
  })).tree;
  ok("a pending toggle on A1 shows pending on A1", textOf(t1).indexOf("pending") >= 0);
  ok("...and NOT on A2", textOf(t2).indexOf("pending") < 0, textOf(t2).slice(0, 60));
  ok("a pending toggle withholds the old On/Off selection",
    pressed(t1).indexOf("On") < 0 && pressed(t1).indexOf("Off") < 0, pressed(t1).join(","));
  ok("...but does not disturb the weight selection", pressed(t1).join(",") === "1×", pressed(t1).join(","));
  ok("A2, untouched, still shows its confirmed state",
    pressed(t2).join(",") === "On,1×", pressed(t2).join(","));
}
{
  const t = mount(React, React.createElement(M.ScreenTileCfg, {
    idx: 0, cfg: { enabled: true, weightCode: 2 }, pend: {}, send: function () {}, disabled: true,
  })).tree;
  ok("a read-only viewer gets no live controls",
    buttons(t).every((b) => b.props.disabled === true), String(buttons(t).length));
  ok("...but still reads what the panel reports", pressed(t).join(",") === "On,1×", pressed(t).join(","));
}

console.log("\n[5] §3.3 panelScreenConfig — absent reads as UNKNOWN, never as a default");
{
  const CFG_BYTES = scenarioOf("normal").history[11].panelScreenConfig;
  const decoded = M.readScreenCfg(CFG_BYTES);
  ok("the fixture's packed-byte config decodes to 12 entries",
    Array.isArray(decoded) && decoded.length === 12, JSON.stringify(decoded && decoded.length));
  ok("bit0 is enabled and bits1..3 are the weight code",
    decoded[0].enabled === true && decoded[0].weightCode === 2
    && decoded[1].enabled === true && decoded[1].weightCode === 4
    && decoded[5].enabled === false && decoded[11].weightCode === 0,
    JSON.stringify(decoded));

  const asObjects = M.readScreenCfg(scenarioOf("alarm").history[11].panelScreenConfig);
  const asHex = M.readScreenCfg(scenarioOf("stale").history[11].panelScreenConfig);
  ok("the object shape decodes identically", JSON.stringify(asObjects) === JSON.stringify(decoded));
  ok("the hex shape decodes identically", JSON.stringify(asHex) === JSON.stringify(decoded));
  ok("the §3.3 field name, wrapped, is also accepted",
    JSON.stringify(M.readScreenCfg({ screenCfg: CFG_BYTES })) === JSON.stringify(decoded));

  // --- the doctrine. Every one of these is "we have not been told". ---
  [
    ["absent", undefined],
    ["null", null],
    ["an empty object", {}],
    ["the wrong length", [5, 5, 5]],
    ["the noData scenario's malformed relay", scenarioOf("noData").history[0].panelScreenConfig],
    ["a short hex string", "0505"],
    ["a number", 5],
  ].forEach(function (c) {
    ok(c[0] + " decodes to unknown, not to a default", M.readScreenCfg(c[1]) === null,
      JSON.stringify(M.readScreenCfg(c[1])));
  });
  /* A garbled ENTRY is unknown on its own. The alternative — discarding the
     whole packet — would blank eleven tiles the panel did report. */
  const partial = M.readScreenCfg([5, null, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]);
  ok("one unreadable entry is unknown on its own, not fatal to the other eleven",
    partial && partial[1] === null && partial[0].enabled === true, JSON.stringify(partial));
  const over = M.readScreenCfg([13, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5]);   // 13 → code 6
  ok("a weight code above 5 is unknown for that entry, and never clamped to a real weight",
    over && over[0] === null && over[1].weightCode === 2, JSON.stringify(over && over[0]));

  /* §10 D3: screenCfg bits 4..7 are reserved, sent zero, and receivers IGNORE
     them. Ignoring is not the same as rejecting — a sender that sets one must
     not blank the tile. */
  const reserved = M.readScreenCfg([0xf5, 0xf9, 5, 5, 5, 4, 5, 7, 5, 5, 5, 1]);
  ok("reserved screenCfg bits 4..7 are masked off, not treated as malformed",
    reserved && reserved[0].enabled === true && reserved[0].weightCode === 2
    && reserved[1].weightCode === 4, JSON.stringify(reserved && reserved.slice(0, 2)));
  ok("...and the rest of the packet decodes as if they were zero",
    JSON.stringify(reserved) === JSON.stringify(M.readScreenCfg(
      scenarioOf("normal").history[11].panelScreenConfig)));
}
{
  /* §10 D3: CONFIG flags bit0 is cfgPersisted, set only after a successful
     flash write. Reported and persisted are different claims. D4 permits
     persistence to ship deferred, so a missing or clear bit must read as
     silence — never as a standing "unsaved" warning about a feature that may
     never arrive. */
  ok("cfgPersisted true only on a positive report",
    M.readCfgPersisted({ screenCfg: [], flags: 1 }) === true
    && M.readCfgPersisted({ screenCfg: [], flags: 3 }) === true);
  ok("a clear bit0 is not persisted", M.readCfgPersisted({ screenCfg: [], flags: 2 }) === false);
  ok("reserved flag bits alone never imply persistence",
    M.readCfgPersisted({ flags: 0xfe }) === false, String(M.readCfgPersisted({ flags: 0xfe })));
  [undefined, null, {}, [], "1", { flags: "yes" }, { flags: -1 }, { flags: 999 }].forEach(function (v) {
    ok("no usable flags reads as not-persisted: " + String(JSON.stringify(v)),
      M.readCfgPersisted(v) === false);
  });
  ok("the packed-byte fixture shape carries no flags, so it claims nothing",
    M.readCfgPersisted(scenarioOf("normal").history[11].panelScreenConfig) === false);
}
{
  const t = mount(React, React.createElement(M.ScreenTileCfg, {
    idx: 1, cfg: null, pend: {}, send: function () {}, disabled: false,
  })).tree;
  ok("with no reported config NOTHING is shown as selected", pressed(t).length === 0, pressed(t).join(","));
  ok("...specifically not 'On', which is the factory default's value",
    pressed(t).indexOf("On") < 0);
  ok("...and specifically not '1×', the factory default weight",
    pressed(t).indexOf("1×") < 0);
  ok("the page says so in words rather than leaving it blank",
    textOf(t).indexOf("has not reported") >= 0, textOf(t).slice(0, 90));
  ok("the controls are still usable — unknown is not the same as locked",
    buttons(t).every((b) => !b.props.disabled));
}
{
  // A tile whose screen the panel reports as disabled is marked and dimmed,
  // but only ever on the panel's word.
  const env = M.buildEnv(scenarioOf("normal").history, CADENCE);
  const cfg = M.readScreenCfg(scenarioOf("normal").history[11].panelScreenConfig);
  const vp = mount(React, React.createElement(M.VirtualPanel, {
    env: env, armed: false, gain: 1, currentIdx: 7, sleeping: false, still: true,
    screenCfg: cfg, pend: {}, send: function () {}, ctlDisabled: false,
  }));
  const figs = findAll(vp.tree, (n) => n.tag === "figure");
  ok("the grid still renders twelve tiles", figs.length === 12, String(figs.length));
  const cls = figs.map((f) => f.props.className);
  ok("only the screen the panel reports disabled is marked skipped",
    cls.filter((c) => c.indexOf("vp-tile--skipped") >= 0).length === 1
    && cls[5].indexOf("vp-tile--skipped") >= 0, cls.join(" | "));
  ok("a skipped tile says SKIPPED", textOf(figs[5]).indexOf("SKIPPED") >= 0, textOf(figs[5]).slice(0, 80));
  const wtBadge = (f) => findAll(f, (n) => String(n.props.className || "")
    .indexOf("vp-tile__badge--wt") >= 0).map(textOf);
  ok("a non-default weight is badged on the tile", wtBadge(figs[1]).join(",") === "5×", wtBadge(figs[1]).join(","));
  ok("the 2x and quarter screens are badged too",
    wtBadge(figs[7]).join(",") === "2×" && wtBadge(figs[11]).join(",") === "¼×",
    wtBadge(figs[7]) + "/" + wtBadge(figs[11]));
  ok("a 1x weight is NOT badged — a badge on every tile would say nothing",
    wtBadge(figs[0]).length === 0, wtBadge(figs[0]).join(","));
  ok("with no reported config nothing is badged at all",
    figs.length === 12 && findAll(mount(React, React.createElement(M.VirtualPanel, {
      env: env, armed: false, gain: 1, currentIdx: 7, sleeping: false, still: true,
      screenCfg: null, pend: {}, send: function () {}, ctlDisabled: false,
    })).tree, (n) => String(n.props.className || "").indexOf("vp-tile__badge--wt") >= 0).length === 0);

  const unknown = mount(React, React.createElement(M.VirtualPanel, {
    env: env, armed: false, gain: 1, currentIdx: 7, sleeping: false, still: true,
    screenCfg: null, pend: {}, send: function () {}, ctlDisabled: false,
  }));
  const ufigs = findAll(unknown.tree, (n) => n.tag === "figure");
  ok("with no reported config NO tile is drawn as skipped",
    ufigs.every((f) => f.props.className.indexOf("vp-tile--skipped") < 0));
  ok("...and no tile claims a weight", textOf(unknown.tree).indexOf("SKIPPED") < 0);
  ok("...and every tile still offers its controls",
    findAll(unknown.tree, (n) => n.tag === "button").length === 12 * 8,
    String(findAll(unknown.tree, (n) => n.tag === "button").length));
}

console.log("\n[6] alignment with the shapes AW1 actually emits");
{
  /* solariPanel.c postConfig() (RETURN-AW1) decodes the 16-byte 0x85 CONFIG
     frame itself and POSTs {screenCfg:[{enabled:0|1, weightCode:0..7} x12],
     flags:N} to /api/panel/config, landing in migration 019's
     panelScreenConfig.screenCfg JSON column. That is a REAL shape now, not a
     guess, so it gets a real test: enabled arrives as a NUMBER, not a bool. */
  const fromDaemon = { flags: 3, screenCfg: [] };
  const CFG = [[1, 2], [1, 4], [1, 2], [1, 2], [1, 2], [0, 2],
               [1, 2], [1, 3], [1, 2], [1, 2], [1, 2], [1, 0]];
  CFG.forEach(function (c) { fromDaemon.screenCfg.push({ enabled: c[0], weightCode: c[1] }); });
  const d = M.readScreenCfg(fromDaemon);
  ok("the daemon's own POST body decodes", Array.isArray(d) && d.length === 12, JSON.stringify(d && d.length));
  ok("numeric enabled from cJSON becomes a real boolean",
    d[0].enabled === true && d[5].enabled === false, JSON.stringify([d[0], d[5]]));
  ok("it agrees byte-for-byte with the packed form the fixture carries",
    JSON.stringify(d) === JSON.stringify(M.readScreenCfg(
      scenarioOf("normal").history[11].panelScreenConfig)));
  ok("the daemon's bit layout is §3.3's — bit0 enabled, bits1..3 weight",
    ((5 & 1) === 1 && ((5 >> 1) & 7) === 2) && d[0].weightCode === 2);
  ok("the extra `flags` field is ignored, not mistaken for a screen entry", d.length === 12);
}
{
  /* Migration 019 widens networkGear.kind to
     ('gateway','switch','ap','router','hub','wanBackup','other'). Every value
     that has a gate geometry must map; 'other' deliberately does not, and is
     dropped rather than guessed into a band. */
  const kinds = ["gateway", "switch", "ap", "router", "hub", "wanBackup"];
  const env = M.buildEnv([Object.assign({}, scenarioOf("normal").history[11], {
    gear: kinds.concat(["other"]).map((k) => ({ role: k, state: "up", rxLevel: 2, txLevel: 2 })),
  })], CADENCE);
  ok("every migration-019 kind with a gate geometry maps", env.gearCount === 6, String(env.gearCount));
  ok("gateway and router both land in the router band",
    env.gear[0].role === M.GR_ROUTER && env.gear[3].role === M.GR_ROUTER,
    env.gear.map((x) => x.role).join(","));
  ok("'other' is dropped, not filed into an arbitrary band",
    env.gear.every((x) => x.role <= M.GR_WANBK) && env.gearCount === kinds.length);
}

console.log("\n[7] conformance to §9 A-1, the parity oracle (acceptance A1)");
{
  /* §9 A-1 is NORMATIVE and both renderers implement it; the firmware is not
     the authority here and is cross-checked only where it already agrees.
     (At the time of writing, AW2's committed panelScreensA.c still fixes APs
     at y0=4 and holds the internet column healthy over a dead router — both
     superseded by §9 A-1 and §10 A-3. Those are AW2's to land; flagged to
     Lead.)

     This is a direct port of that firmware's a1BandX(): biased integer
     arithmetic against the dashboard's floats and Math.round. They must agree
     on every count the panel can display, or the two renderers put the same
     device in different columns. */
  const a1BandX = (low, high, count, ord) => count <= 1
    ? Math.floor((low + high) / 2)
    : low + Math.floor((ord * (high - low) + Math.floor((count - 1) / 2)) / (count - 1));
  const bands = [[2, 16], [20, 34], [38, 44]];
  let mismatch = [];
  bands.forEach(function (b) {
    for (let n = 1; n <= 12; n++) {
      const mine = M.a1Spread(n, b[0], b[1]);
      for (let i = 0; i < n; i++) {
        const theirs = a1BandX(b[0], b[1], n, i);
        if (mine[i] !== theirs) mismatch.push(b.join("..") + " n=" + n + " i=" + i
          + ": js " + mine[i] + " != c " + theirs);
      }
    }
  });
  ok("a1Spread agrees with the firmware's a1BandX for every band and count 1..12",
    mismatch.length === 0, mismatch.slice(0, 5).join("; "));

  // The fixed columns and heights, transcribed from panelScreensA.c:142-154.
  const env = envWithGear(QUIET_9);
  const L = M.a1Layout(env.gear);
  const at = (r) => L.gates.filter((x) => x.role === r);
  ok("router gate is x=47, y0=0, h=11 (§9 A-1)",
    at(M.GR_ROUTER)[0].x === 47 && at(M.GR_ROUTER)[0].y0 === 0 && at(M.GR_ROUTER)[0].h === 11,
    JSON.stringify(at(M.GR_ROUTER)[0]));
  ok("switch gate is y0=1, h=8 (§9 A-1)",
    at(M.GR_SWITCH)[0].y0 === 1 && at(M.GR_SWITCH)[0].h === 8, JSON.stringify(at(M.GR_SWITCH)[0]));
  ok("hubs alternate h=6 flush top / h=5 flush bottom (§9 A-1)",
    at(M.GR_HUB).every((h, i) => (i % 2 === 0)
      ? (h.h === 6 && h.y0 === 0) : (h.h === 5 && h.y0 === 11 - 5)),
    JSON.stringify(at(M.GR_HUB)));
  ok("APs are h=3 distributed 0/4/8 (§9 A-1)",
    at(M.GR_AP).every((a) => a.h === 3) && at(M.GR_AP).map((a) => a.y0).join() === "0,4,8",
    JSON.stringify(at(M.GR_AP)));
  ok("the 9-device inventory lands on §9 A-1's stated columns: APs 2/9/16, "
    + "hubs 20/27/34, switch 41, router 47",
    at(M.GR_AP).map((a) => a.x).join() === "2,9,16"
    && at(M.GR_HUB).map((h) => h.x).join() === "20,27,34"
    && at(M.GR_SWITCH)[0].x === 41 && at(M.GR_ROUTER)[0].x === 47,
    at(M.GR_AP).map((a) => a.x) + " | " + at(M.GR_HUB).map((h) => h.x));
  ok("the three bands are 2..16, 20..34, 38..44 (§9 A-1)",
    M.A1_BAND[M.GR_AP].join() === "2,16" && M.A1_BAND[M.GR_HUB].join() === "20,34"
    && M.A1_BAND[M.GR_SWITCH].join() === "38,44");
  ok("internet x=51 and wanBackup x=52 (§9 A-1)",
    M.A1_INET_X === 51 && M.A1_WAN_X === 52);
  ok("both renderers seed the same particle RNG (77)",
    /rngNew\(77\)/.test(fs.readFileSync(path.join(PUB, "screens-panel.jsx"), "utf8"))
    && /panelRngSeed\(&r, 77u\)/.test(fs.readFileSync(
      path.join(ROOT, "status-panel/firmware/panelScreensA.c"), "utf8")));
}

console.log("\n[8] the fixture file");
{
  ok("fixture is valid JSON and declares fixtureVersion 2", fixture.fixtureVersion === 2, String(fixture.fixtureVersion));
  ok("every scenario is still present",
    fixture.scenarios.map((s) => s.id).join(",") === "normal,alarm,stale,emptyHistory,noData",
    fixture.scenarios.map((s) => s.id).join(","));
  let rows = 0, bad = [];
  fixture.scenarios.forEach(function (sc) {
    sc.history.forEach(function (h, i) {
      if (!("gear" in h)) return;
      if (!Array.isArray(h.gear) || h.gear.length > 12) { bad.push(sc.id + "[" + i + "] shape"); return; }
      h.gear.forEach(function (x) {
        rows++;
        if (!(x.role >= 0 && x.role <= 4)) bad.push(sc.id + "[" + i + "] role " + x.role);
        if (!(x.state >= 0 && x.state <= 2)) bad.push(sc.id + "[" + i + "] state " + x.state);
        if (!(x.rxLevel >= 0 && x.rxLevel <= 7)) bad.push(sc.id + "[" + i + "] rx " + x.rxLevel);
        if (!(x.txLevel >= 0 && x.txLevel <= 7)) bad.push(sc.id + "[" + i + "] tx " + x.txLevel);
      });
    });
  });
  ok("every gear row is inside the §3.1 ranges", bad.length === 0, bad.slice(0, 4).join("; "));
  ok("the gear section is populated", rows > 300, String(rows));
  const n = scenarioOf("normal").history[0].gear;
  ok("the normal scenario carries the real 9-device inventory in §3.1 order",
    n.length === 9 && n.map((x) => x.role).join(",") === "0,1,2,2,2,3,3,3,4",
    n.map((x) => x.role).join(","));
  ok("the inventory is documented in the file itself",
    /chemistry/.test(fixture._gearInventory) && /covalent/.test(fixture._gearInventory));
  ok("the config shapes are documented too", /factory default/.test(fixture._screenConfig));
}

console.log("\n" + pass + " passed, " + fail + " failed\n");
process.exit(fail ? 1 : 0);
