#!/usr/bin/env node
/* Headless test for the CONTRACT-LC lifecycle + criticality UI in
   dashboard/public/screens.jsx. Loads the real file into a sandbox with a
   minimal React shim (createElement + hooks), renders the components, and
   asserts the affordances, the role gating, and the wire bodies. No DOM and no
   network — window.SOLARI.api.post is a spy, so this exercises what the UI
   SENDS, not what the server does with it.
   Run: node tests/dashboard/test_lifecycle_ui.js   — exits non-zero on failure. */
"use strict";

const fs = require("fs");
const vm = require("vm");
const path = require("path");

const PUB = path.join(__dirname, "..", "..", "dashboard", "public");

/* ---------- tiny React ---------- */
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
  R.useMemo = function (fn) { cur.idx++; return fn(); };
  R.useCallback = function (fn) { cur.idx++; return fn; };
  R.useEffect = function (fn) { cur.idx++; cur.effects.push(fn); };
  R.__enter = function (c) { const prev = cur; cur = c; return prev; };
  R.__exit = function (prev) { cur = prev; };
  return R;
}

function render(React, el, ctx, path) {
  if (el == null || el === false || el === true) return null;
  if (Array.isArray(el)) return el.map((c, i) => render(React, c, ctx, path + "[" + i + "]")).filter((x) => x != null);
  if (typeof el !== "object") return String(el);
  if (!el.$$el) return null;
  if (typeof el.type === "function") {
    const p = path + "/" + (el.type.name || "anon");
    const frame = { store: ctx.store, path: p, idx: 0, effects: ctx.effects };
    const prev = React.__enter(frame);
    let out;
    try { out = el.type(el.props); } finally { React.__exit(prev); }
    return render(React, out, ctx, p);
  }
  const node = { tag: String(el.type), props: el.props, children: [] };
  if (el.props && el.props.children != null) {
    const kids = render(React, el.props.children, ctx, path + ">");
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
const textOf = (n) => {
  let s = "";
  walk(n, (x) => { (x.children || []).forEach((c) => { if (typeof c === "string") s += c; }); });
  return s;
};

/* ---------- sandbox ---------- */
/* load(role) builds a sandbox holding screens.jsx. Pass an operator object
   instead of a role string to model a malformed or half-loaded whoami; pass
   { screens3: true } as the second argument to also load screens3.jsx, which
   needs screens.jsx on window first (it reads the shared LC surface from
   there). */
function load(operatorRole, opts) {
  const React = makeReact();
  const posts = [];
  const toasts = [];
  const sandbox = { console, setTimeout, clearTimeout, Promise, Date, Number, Math, JSON, encodeURIComponent, String, Object, Array };
  sandbox.window = sandbox;
  sandbox.global = sandbox;
  sandbox.React = React;
  sandbox.__solariToast = function (m, i) { toasts.push({ msg: m, icon: i }); };
  sandbox.Icon = function Icon() { return null; };
  sandbox.StatusDot = function StatusDot() { return null; };
  sandbox.Sparkline = sandbox.TimeSeries = sandbox.BandwidthGauge = function () { return null; };
  sandbox.RadialGauge = sandbox.HealthDonut = sandbox.RTTBars = function () { return null; };
  sandbox.metricColor = function () { return "#000"; };
  // Stub DangerModal: the real one is unchanged by this lane, so the harness
  // asserts the PROPS contract (confirm grade + what onConfirm sends).
  sandbox.DangerModal = function DangerModal(props) {
    return React.createElement("danger-modal", {
      title: props.title, verb: props.verb, confirmName: props.confirmName, onConfirm: props.onConfirm,
    });
  };
  sandbox.SOLARI = {
    fmt: { ago: (m) => m + "m", kb: () => "0", mbps: () => "0", rtt: () => "0" },
    operator: (operatorRole && typeof operatorRole === "object") ? operatorRole
      : (operatorRole ? { name: "t", role: operatorRole } : null),
    nodes: [], fleet: [], alerts: [], probes: [], assets: [], segments: [], pools: [],
    rules: [], enrollments: [], discovered: [], maintenance: [], activeCrit: 0, activeWarn: 0,
    summary: { systems: 0, applications: 0 }, fleetRoll: { total: 0, up: 0, degraded: 0, down: 0, unknown: 0, retired: 0 },
    config: {},
    api: {
      post: function (p, b) { posts.push({ path: p, body: b }); return Promise.resolve({ ok: true }); },
      get: function () { return Promise.resolve([]); },
      refresh: function () { return Promise.resolve(); },
      retireNode: function () { posts.push({ path: "retireNode" }); return Promise.resolve({}); },
    },
  };
  vm.createContext(sandbox);
  vm.runInContext(fs.readFileSync(path.join(PUB, "vendor/babel.min.js"), "utf8"), sandbox, { filename: "babel.min.js" });
  const Babel = sandbox.Babel;
  const run = function (file) {
    const src = fs.readFileSync(path.join(PUB, file), "utf8");
    vm.runInContext(Babel.transform(src, { presets: ["react"], filename: file }).code, sandbox, { filename: file });
  };
  run("screens.jsx");
  if (opts && opts.screens3) run("screens3.jsx");
  return { React, win: sandbox, posts, toasts };
}

/* ---------- assertions ---------- */
let pass = 0, fail = 0;
function ok(name, cond, extra) {
  if (cond) { pass++; console.log("  ok   - " + name); }
  else { fail++; console.log("  FAIL - " + name + (extra ? "  [" + extra + "]" : "")); }
}
function mount(env, el) {
  const ctx = { store: {}, effects: [] };
  const tree = render(env.React, el, ctx, "");
  return { tree, ctx, rerender: () => render(env.React, el, ctx, "") };
}

const ASSET = { assetId: 7, displayName: "pihole-01", ip: "10.0.0.254", host: "pihole-01.akoria.net", lifecycle: "active", criticality: 2 };

console.log("\n[1] role gating — LifecycleActions");
{
  const viewer = load("viewer");
  const m = mount(viewer, viewer.React.createElement(viewer.win.LifecycleActions, { asset: ASSET }));
  ok("viewer sees no lifecycle buttons", findAll(m.tree, (n) => n.tag === "button").length === 0);

  const op = load("operator");
  const mo = mount(op, op.React.createElement(op.win.LifecycleActions, { asset: ASSET }));
  const btns = findAll(mo.tree, (n) => n.tag === "button").map(textOf);
  ok("operator sees Decommission + Delete", btns.join("|").includes("Decommission") && btns.join("|").includes("Delete"), btns.join("|"));
  ok("operator sees no Purge on an active asset", !btns.join("|").includes("Purge"), btns.join("|"));

  const opDel = load("operator");
  const md = mount(opDel, opDel.React.createElement(opDel.win.LifecycleActions, { asset: Object.assign({}, ASSET, { lifecycle: "deleted" }) }));
  const dbtns = findAll(md.tree, (n) => n.tag === "button").map(textOf).join("|");
  ok("purge hidden for operator even on a deleted asset", !dbtns.includes("Purge"), dbtns);
  ok("deleted asset offers Restore", dbtns.includes("Restore"), dbtns);

  const admin = load("admin");
  const ma = mount(admin, admin.React.createElement(admin.win.LifecycleActions, { asset: Object.assign({}, ASSET, { lifecycle: "deleted" }) }));
  const abtns = findAll(ma.tree, (n) => n.tag === "button").map(textOf).join("|");
  ok("admin sees Purge on a deleted asset", abtns.includes("Purge"), abtns);
}

console.log("\n[2] DangerModal flow — confirm grade + wire body");
{
  const env = load("operator");
  const el = env.React.createElement(env.win.LifecycleActions, { asset: ASSET });
  const m = mount(env, el);
  const del = findAll(m.tree, (n) => n.tag === "button" && textOf(n).includes("Delete"))[0];
  ok("no modal before the button is pressed", findAll(m.tree, (n) => n.tag === "danger-modal").length === 0);
  del.props.onClick();
  const t2 = m.rerender();
  const modals = findAll(t2, (n) => n.tag === "danger-modal");
  ok("delete opens exactly one DangerModal", modals.length === 1);
  ok("delete is a plain confirm (no typed name)", modals[0] && !modals[0].props.confirmName);
  ok("nothing posted until the modal confirms", env.posts.length === 0, JSON.stringify(env.posts));
  modals[0].props.onConfirm();
  ok("confirm posts the §3.3 lifecycle body", env.posts.length === 1
    && env.posts[0].path === "/api/assets/7/lifecycle"
    && env.posts[0].body.action === "delete"
    && env.posts[0].body.confirm === true, JSON.stringify(env.posts));
}
{
  const env = load("admin");
  const el = env.React.createElement(env.win.LifecycleActions, { asset: Object.assign({}, ASSET, { lifecycle: "deleted" }) });
  const m = mount(env, el);
  findAll(m.tree, (n) => n.tag === "button" && textOf(n).includes("Purge"))[0].props.onClick();
  const modal = findAll(m.rerender(), (n) => n.tag === "danger-modal")[0];
  ok("purge demands the name typed back", modal && modal.props.confirmName === "pihole-01", modal && modal.props.confirmName);
  modal.props.onConfirm();
  ok("purge posts confirmName", env.posts.length === 1 && env.posts[0].path === "/api/assets/7/purge"
    && env.posts[0].body.confirmName === "pihole-01", JSON.stringify(env.posts));
}

console.log("\n[3] criticality selector");
{
  const env = load("operator");
  const el = env.React.createElement(env.win.CriticalityControl, {
    tier: 2, onCommit: (t) => env.win.SolariLC.assetCriticality(7, t),
  });
  const m = mount(env, el);
  const btns = findAll(m.tree, (n) => n.tag === "button");
  ok("renders 5 tiers", btns.length === 5, "got " + btns.length);
  ok("tier labels are the contract's", btns.map(textOf).join(",") === "Ignore,Low,Normal,High,Vital", btns.map(textOf).join(","));
  ok("current tier is the pressed one", btns[2].props["aria-pressed"] === true && btns[0].props["aria-pressed"] === false);
  ok("behaviour text is shown inline", textOf(m.tree).includes("Standard alerting"), textOf(m.tree).slice(0, 90));
  btns[4].props.onClick();
  ok("picking vital posts tier 4 to the asset route", env.posts.length === 1
    && env.posts[0].path === "/api/assets/7/criticality" && env.posts[0].body.tier === 4, JSON.stringify(env.posts));
  const t2 = m.rerender();
  ok("optimistic: vital shows selected before the server answers",
    findAll(t2, (n) => n.tag === "button")[4].props["aria-pressed"] === true);

  const nodeEnv = load("operator");
  const nm = mount(nodeEnv, nodeEnv.React.createElement(nodeEnv.win.CriticalityControl, {
    tier: 2, onCommit: (t) => nodeEnv.win.SolariLC.nodeCriticality(12, t),
  }));
  findAll(nm.tree, (n) => n.tag === "button")[0].props.onClick();
  ok("node route carries tier 0", nodeEnv.posts.length === 1
    && nodeEnv.posts[0].path === "/api/nodes/12/criticality" && nodeEnv.posts[0].body.tier === 0, JSON.stringify(nodeEnv.posts));
}
{
  const env = load("viewer");
  const m = mount(env, env.React.createElement(env.win.CriticalityControl, { tier: 3, readOnly: true, onCommit: () => Promise.resolve() }));
  ok("viewer gets no tier buttons", findAll(m.tree, (n) => n.tag === "button").length === 0);
  ok("viewer still sees which tier is set", textOf(m.tree).includes("High"), textOf(m.tree).slice(0, 80));
}
{
  const env = load("operator");
  const m = mount(env, env.React.createElement(env.win.CriticalityControl, { tier: null, onCommit: () => Promise.resolve() }));
  ok("absent column reads as unset, never as Ignore", textOf(m.tree).includes("hasn't reported"), textOf(m.tree).slice(0, 120));
  ok("no tier is pressed when the column is absent",
    findAll(m.tree, (n) => n.tag === "button").every((b) => b.props["aria-pressed"] === false));
}

console.log("\n[4] chips");
{
  const env = load("operator");
  const chip = (t) => mount(env, env.React.createElement(env.win.TierChip, { tier: t })).tree;
  ok("default tier renders no chip", chip(2) === null);
  ok("absent tier renders no chip", chip(null) === null);
  ok("vital renders an accent chip", chip(4) && chip(4).props.className === "tierchip t4", chip(4) && chip(4).props.className);
  ok("ignore renders a dimmed chip", chip(0) && chip(0).props.className === "tierchip t0");
  const lcc = (l) => mount(env, env.React.createElement(env.win.LifecycleChip, { lifecycle: l })).tree;
  ok("active renders no lifecycle chip", lcc("active") === null);
  ok("decommissioned renders a chip", lcc("decommissioned") && textOf(lcc("decommissioned")) === "Decommissioned");
  ok("deleted renders a chip", lcc("deleted") && textOf(lcc("deleted")) === "Deleted");
}

console.log("\n[5] failure handling — endpoints not live yet");
function failureBlock() {
  const env = load("operator");
  env.win.SOLARI.api.post = function () { return Promise.reject(new Error("HTTP_404")); };
  const m = mount(env, env.React.createElement(env.win.CriticalityControl, { tier: 2, onCommit: (t) => env.win.SolariLC.assetCriticality(7, t) }));
  findAll(m.tree, (n) => n.tag === "button")[4].props.onClick();
  return new Promise((r) => setTimeout(r, 20)).then(function () {
    const t2 = m.rerender();
    const b = findAll(t2, (n) => n.tag === "button");
    ok("a 404 rolls the selection back to the server's tier",
      b[2].props["aria-pressed"] === true && b[4].props["aria-pressed"] === false);
    ok("the failure is toasted, not swallowed", env.toasts.some((t) => /failed/i.test(t.msg)), JSON.stringify(env.toasts));
    ok("the failure is shown in the control itself", textOf(t2).includes("last change failed"));
  });
}
console.log("\n[6] FleetOverview list filtering + chips");
{
  const env = load("operator");
  const S = env.win.SOLARI;
  S.assets = [
    { assetId: 1, displayName: "keep", lifecycle: "active", criticality: 4 },
    { assetId: 2, displayName: "gone", lifecycle: "decommissioned", criticality: 2 },
    { assetId: 3, displayName: "dead", lifecycle: "deleted", criticality: 2 },
  ];
  const mk = (id, name, extra) => Object.assign({
    nodeId: id, name: name, hostFqdn: name + ".akoria.net", ip: "10.0.0." + id, state: "up",
    role: "client", segId: 1, segName: "core", cpuPct: 1, ramPct: 1, diskMaxPct: 1,
    netTotalMbps: 0, lastSeenMin: 0, alertsCount: 0, hist: { cpu: [] },
  }, extra || {});
  const fleet = [
    mk("n1", "xenon"),
    mk("n2", "old", { state: "retired" }),
    // criticality/lifecycle ride on the row itself, the way mapAssetNode now
    // passes them through (CONTRACT-LC A-1).
    mk("asset-1", "keep", { isAsset: true, assetId: 1, lifecycle: "active", criticality: 4 }),
    mk("asset-2", "gone", { isAsset: true, assetId: 2, lifecycle: "decommissioned", criticality: 2 }),
    mk("asset-3", "dead", { isAsset: true, assetId: 3, lifecycle: "deleted", criticality: 2 }),
  ];
  S.summary = { systems: 5, applications: 0 };
  S.fleetRoll = { total: 5, up: 4, degraded: 0, down: 0, unknown: 0, retired: 1 };
  S.segments = [{ id: 1, name: "core", cidr: "10.0.0.0/24" }];
  const m = mount(env, env.React.createElement(env.win.FleetOverview, {
    fleet: fleet, view: "table", setView: () => {}, onOpenNode: () => {}, onNav: () => {},
  }));
  const hostText = findAll(m.tree, (n) => n.tag === "tr").map(textOf).join(" | ");
  ok("active rows render", hostText.includes("xenon") && hostText.includes("keep"), hostText);
  ok("retired node hidden by default", !hostText.includes("old"), hostText);
  ok("decommissioned asset hidden by default", !hostText.includes("gone"), hostText);
  ok("deleted asset hidden by default", !hostText.includes("dead"), hostText);
  ok("vital asset carries a tier chip",
    findAll(m.tree, (n) => n.props && n.props.className === "tierchip t4").length === 1);
  const chips = findAll(m.tree, (n) => n.tag === "button" && n.props.className && n.props.className.indexOf("chip") === 0).map(textOf).join("|");
  ok("a Deleted filter chip is offered", chips.includes("Deleted"), chips);
  ok("a Retired filter chip is offered", chips.includes("Retired"), chips);
  // switch the Deleted chip on
  findAll(m.tree, (n) => n.tag === "button" && textOf(n).indexOf("Deleted") === 0)[0].props.onClick();
  const t2 = m.rerender();
  const rows2 = findAll(t2, (n) => n.tag === "tr").map(textOf).join(" | ");
  ok("Deleted filter reveals the deleted row", rows2.includes("dead"), rows2);
  ok("the revealed row is greyed, not normal",
    findAll(t2, (n) => n.tag === "tr" && n.props.className === "lc-dormant").length === 1);
}

console.log("\n[7] role gating FAILS CLOSED on an unresolved identity");
{
  // The defect this guards: a viewer whose whoami failed, or has not answered
  // yet, must not be shown a write control. The server would refuse the write,
  // but by then the button has already been offered.
  const cases = [
    ["no operator profile at all (whoami failed / no session)", null],
    ["profile present but role missing", { name: "t" }],
    ["role of an unexpected type", { name: "t", role: 7 }],
    ["an unrecognised role string", { name: "t", role: "auditor" }],
  ];
  cases.forEach(function (c) {
    const env = load(c[1]);
    const m = mount(env, env.React.createElement(env.win.LifecycleActions, { asset: ASSET }));
    ok(c[0] + " → no lifecycle buttons", findAll(m.tree, (n) => n.tag === "button").length === 0);
    ok(c[0] + " → canOperate() is false", env.win.solariCanOperate() === false);
  });
  const env = load({ name: "t", role: "ADMIN" });
  ok("a confirmed role still works when the server shouts it", env.win.solariCanOperate() === true);
  const m = mount(env, env.React.createElement(env.win.LifecycleActions, { asset: Object.assign({}, ASSET, { lifecycle: "deleted" }) }));
  ok("...and case does not hide Purge from an admin",
    findAll(m.tree, (n) => n.tag === "button").map(textOf).join("|").includes("Purge"));
  // The tier selector defaults to read-only rather than to pressable.
  const un = load(null);
  const mc = mount(un, un.React.createElement(un.win.CriticalityControl, { tier: 2, onCommit: () => Promise.resolve() }));
  ok("criticality control defaults read-only when the role is unresolved",
    findAll(mc.tree, (n) => n.tag === "button").length === 0);
}

console.log("\n[8] Discovery reappearance notice — tombstone shapes (screens3.jsx)");
{
  const disc = (tombstone) => {
    const env = load("operator", { screens3: true });
    env.win.SOLARI.discovered = [{
      discId: 1, host: "pihole-01", ip: "10.0.0.254", via: "arp", kind: "host",
      services: [], seen: 0, seenCount: 1, status: "ignored", tombstone: tombstone,
    }];
    const m = mount(env, env.React.createElement(env.win.Discovery, { onOpenNode: () => {} }));
    return textOf(m.tree);
  };
  ok("no tombstone → no notice", disc(undefined).indexOf("not re-adopted") === -1);
  ok("null tombstone → no notice", disc(null).indexOf("not re-adopted") === -1);
  const full = disc({ assetId: 7, lifecycle: "deleted", displayName: "pihole-01" });
  ok("full tombstone names the state and the system",
    full.includes("matches a deleted system (pihole-01)"), full.slice(0, 160));
  const noName = disc({ assetId: 7, lifecycle: "decommissioned" });
  ok("tombstone without displayName still reads cleanly",
    noName.includes("matches a decommissioned system") && !/undefined|\(\)/.test(noName), noName.slice(0, 160));
  const partial = disc({ assetId: 7 });
  ok("partial tombstone raises the notice", partial.includes("not re-adopted"), partial.slice(0, 160));
  ok("...but never claims a lifecycle the server did not send",
    !partial.includes("deleted") && !partial.includes("decommissioned"), partial.slice(0, 160));
}

console.log("\n[9] Systems list — mutating affordances are gated too (screens3.jsx)");
{
  const systems = (role) => {
    const env = load(role, { screens3: true });
    env.win.SOLARI.assets = [{
      assetId: 1, displayName: "pihole-01", ip: "10.0.0.254", class: "appliance",
      state: "up", poolId: 2, poolName: "Core", targetCount: 2, monitorHost: 1, lifecycle: "active",
    }];
    env.win.SOLARI.pools = [{ poolId: 1, name: "Unassigned" }, { poolId: 2, name: "Core" }];
    return mount(env, env.React.createElement(env.win.Assets, { onOpenNode: () => {} }));
  };
  const opTree = systems("operator").tree;
  ok("operator is offered New pool", findAll(opTree, (n) => n.tag === "button").map(textOf).join("|").includes("New pool"));
  ok("operator's pool select is live", findAll(opTree, (n) => n.tag === "select").every((s) => !s.props.disabled));
  ok("operator can click the name to rename",
    findAll(opTree, (n) => n.props && n.props.title === "Rename").length === 1);

  const vTree = systems("viewer").tree;
  ok("viewer is not offered New pool", !findAll(vTree, (n) => n.tag === "button").map(textOf).join("|").includes("New pool"));
  ok("viewer's pool select is disabled", findAll(vTree, (n) => n.tag === "select").every((s) => s.props.disabled === true));
  ok("viewer gets no rename affordance", findAll(vTree, (n) => n.props && n.props.title === "Rename").length === 0);
  ok("viewer still reads the system's name", textOf(vTree).includes("pihole-01"));
  ok("viewer gets no pool-delete buttons",
    findAll(vTree, (n) => n.props && /^Delete pool/.test(String(n.props.title || ""))).length === 0);
  ok("unresolved identity is treated as a viewer here too",
    !findAll(systems(null).tree, (n) => n.tag === "button").map(textOf).join("|").includes("New pool"));
}

/* AssetDetail loads its record asynchronously, so this block runs the mount's
   effects and waits a tick before asserting. */
function assetDetailBlock() {
  console.log("\n[10] AssetDetail — class / pool / heartbeat / target removal (screens3.jsx)");
  const build = function (role) {
    const env = load(role, { screens3: true });
    env.win.SOLARI.pools = [{ poolId: 1, name: "Unassigned" }, { poolId: 2, name: "Core" }];
    env.win.SOLARI.api.asset = function () {
      return Promise.resolve({
        assetId: 7, displayName: "pihole-01", ip: "10.0.0.254", host: "pihole-01.akoria.net",
        class: "appliance", poolId: 2, poolName: "Core", state: "up", monitorHost: 1,
        lifecycle: "active", criticality: 2,
        targets: [{ targetId: "t1", label: "dns", proto: "udp", port: 53, state: "up", vantages: 1 }],
      });
    };
    // AssetDetail takes toast as a PROP (it shadows the module-level one), so
    // the fixture has to hand it in the same way app.jsx:250 does — otherwise
    // every `toast && toast(...)` in the component is a silent no-op and the
    // harness would score a failure notice it never actually rendered.
    const el = env.React.createElement(env.win.AssetDetail, {
      assetId: 7, onBack: () => {}, onOpenService: () => {}, toast: env.win.__solariToast,
    });
    const m = mount(env, el);
    m.ctx.effects.forEach((f) => f());
    return new Promise((r) => setTimeout(r, 20)).then(() => ({ env: env, m: m, tree: m.rerender() }));
  };
  return build("operator").then(function (o) {
    ok("operator's class + pool selects are live", findAll(o.tree, (n) => n.tag === "select").every((s) => !s.props.disabled));
    ok("operator's heartbeat switch is live",
      findAll(o.tree, (n) => n.props && /switch/.test(String(n.props.className || ""))).every((b) => !b.props.disabled));
    ok("operator can remove a target",
      findAll(o.tree, (n) => n.props && /^Stop monitoring/.test(String(n.props.title || ""))).length === 1);
    // Removal must reject, not resolve, when the call fails — DangerModal
    // drops its busy state either way, but a swallowed rejection would close
    // the modal as if the removal had worked.
    o.env.win.SOLARI.api.removeTarget = function () { return Promise.reject(new Error("HTTP_500")); };
    findAll(o.tree, (n) => n.props && /^Stop monitoring/.test(String(n.props.title || "")))[0]
      .props.onClick({ stopPropagation: function () {} });
    const confirm = findAll(o.m.rerender(), (n) => n.tag === "danger-modal")[0];
    ok("removing a target opens a confirm modal", !!confirm);
    return Promise.resolve(confirm.props.onConfirm()).then(
      function () { ok("a failed target removal rejects rather than resolving", false, "resolved"); },
      function () {
        ok("a failed target removal rejects rather than resolving", true);
                ok("...and says so", o.env.toasts.some((t) => /Remove failed/.test(t.msg)), JSON.stringify(o.env.toasts));
      }
    );
  }).then(function () {
    return build("viewer");
  }).then(function (v) {
    ok("viewer's class + pool selects are disabled",
      findAll(v.tree, (n) => n.tag === "select").length > 0
      && findAll(v.tree, (n) => n.tag === "select").every((s) => s.props.disabled === true));
    ok("viewer's heartbeat switch is disabled",
      findAll(v.tree, (n) => n.props && /switch/.test(String(n.props.className || ""))).every((b) => b.props.disabled === true));
    ok("viewer gets no target-removal button",
      findAll(v.tree, (n) => n.props && /^Stop monitoring/.test(String(n.props.title || ""))).length === 0);
    ok("viewer gets no lifecycle actions on the detail page",
      !textOf(v.tree).includes("Decommission") && !textOf(v.tree).includes("Delete"), textOf(v.tree).slice(0, 120));
    ok("viewer still sees the system's state and targets",
      textOf(v.tree).includes("pihole-01") && textOf(v.tree).includes("dns"));
  });
}

console.log("\n[11] give-up timer — withGiveUp (LC_GIVE_UP_MS=15s) fires without a real sleep");
/* screens.jsx's withGiveUp wraps every SolariLC call in a 15 s window.setTimeout
   race. Nothing so far ever let that timer actually fire — every prior failure
   test rejects the underlying post immediately. Here the post is held open
   FOREVER (a Promise that never settles), so the only way this control's
   promise can ever resolve is the give-up firing. window.setTimeout/clearTimeout
   are swapped for a manual fake queue so the 15 s is virtual: advance() moves a
   simulated clock and fires due callbacks synchronously, so this block runs in
   milliseconds of real wall time. */
function giveUpBlock() {
  const env = load("operator");
  let vclock = 0;
  const timers = [];
  const fired = [];   // { id, delay } for every callback advance() has actually invoked, in order
  let nextId = 1;
  env.win.setTimeout = function (fn, ms) {
    const id = nextId++;
    timers.push({ id: id, fn: fn, fireAt: vclock + (ms || 0), delay: (ms || 0) });
    return id;
  };
  env.win.clearTimeout = function (id) {
    const t = timers.filter((t) => t.id === id)[0];
    if (t) t.fn = null;
  };
  function advance(ms) {
    vclock += ms;
    timers.filter((t) => t.fn && t.fireAt <= vclock).forEach((t) => {
      const fn = t.fn; t.fn = null;
      fired.push({ id: t.id, delay: t.delay });
      fn();
    });
  }
  // Real (short) wait, purely to let a fired timer's .then/.catch microtasks
  // run before the next assertion — not a 15 s sleep, the same idiom
  // failureBlock() above already uses. Needed because advance() firing a
  // callback synchronously does NOT make its promise chain observable
  // synchronously: reject() inside withGiveUp still schedules a real
  // microtask for pick()'s .catch.
  function flush() { return new Promise((r) => setTimeout(r, 20)); }

  // Hold the wrapped promise open: the api.post stub never resolves or rejects.
  env.win.SOLARI.api.post = function () { return new Promise(function () {}); };

  const el = env.React.createElement(env.win.CriticalityControl, {
    tier: 2, onCommit: (t) => env.win.SolariLC.assetCriticality(7, t),
  });
  const m = mount(env, el);
  findAll(m.tree, (n) => n.tag === "button")[4].props.onClick();   // pick "Vital"

  /* Pin the delay directly, not just the eventual effect — the mutation this
     guards against (reviewer-caught on 52ce2f2: shortening the production
     delay to 1 ms while keeping the "15 s" error text) is invisible to an
     assertion that only checks "nothing visible yet" at some later point,
     because a 1 ms timer would already have fired DURING advance(14999)
     below and this pins it before that ambiguity can even arise. */
  ok("exactly one give-up timer was scheduled, with its delay pinned at 15000 ms",
    timers.length === 1 && timers[0].delay === 15000, JSON.stringify(timers.map((t) => t.delay)));

  advance(14999);
  ok("no timer callback has fired 1 ms before the pinned 15000 ms delay",
    fired.length === 0, JSON.stringify(fired));

  return flush().then(function () {
    // Flushed BEFORE asserting "nothing happened yet": if the production
    // delay were mutated shorter (the reviewer's 1 ms case), the timer would
    // already be in `fired` and its rejection's toast would be visible here
    // — this is what makes the earlier "no toast yet" check non-vacuous.
    const t1 = m.rerender();
    ok("...and no toast or rollback is visible either, confirmed after a flush",
      env.toasts.length === 0
      && findAll(t1, (n) => n.tag === "button")[4].props["aria-pressed"] === true,
      JSON.stringify(env.toasts));

    advance(1);   // crosses 15000 ms total — the one give-up timer fires here, not before
    ok("the give-up timer fires exactly once, and only at the 15000 ms mark",
      fired.length === 1 && fired[0].delay === 15000, JSON.stringify(fired));

    return flush();
  }).then(function () {
    const t2 = m.rerender();
    const btns = findAll(t2, (n) => n.tag === "button");
    ok("the caller's promise rejects: the optimistic pick rolls back to the server's tier",
      btns[2].props["aria-pressed"] === true && btns[4].props["aria-pressed"] === false,
      btns.map((b) => b.props["aria-pressed"]).join(","));
    ok("the give-up failure is toasted, not swallowed",
      env.toasts.some((t) => /failed/i.test(t.msg) && /15 s/.test(t.msg)), JSON.stringify(env.toasts));
    ok("the control shows the give-up's own reason (15 s), not a generic error",
      textOf(t2).indexOf("15 s") >= 0, textOf(t2).slice(0, 160));
  });
}

failureBlock().then(giveUpBlock).then(assetDetailBlock).then(done);

function done() {
  console.log("\n" + pass + " passed, " + fail + " failed\n");
  process.exit(fail ? 1 : 0);
}
