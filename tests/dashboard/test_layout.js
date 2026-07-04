#!/usr/bin/env node
/* Headless test for dashboard/public/layout.jsx (window.SolariTopoLayout).
   Run: node tests/dashboard/test_layout.js   — exits non-zero on failure. */
"use strict";

var Layout = require("../../dashboard/public/layout.jsx");

var failures = 0;
function ok(cond, msg) {
  if (cond) { console.log("  ok   - " + msg); }
  else { console.log("  FAIL - " + msg); failures++; }
}
function isFiniteNum(v) { return typeof v === "number" && isFinite(v); }
function finitePos(p) { return p && isFiniteNum(p.x) && isFiniteNum(p.y); }

// -------- (a) empty graph → empty positions -----------------------------
(function () {
  console.log("[a] empty graph");
  var r = Layout.compute([], []);
  ok(r && r.positions && Object.keys(r.positions).length === 0, "empty positions");
  ok(r.bounds && r.bounds.minX === 0 && r.bounds.maxX === 0, "zeroed bounds");
})();

// -------- (b) small network graph: gateway → switch → 3 nodes -----------
// gateway gear -> switch gear (uplink); switch -> 3 client nodes (attaches)
var nodes = [
  { id: "gear:1", kind: "gear", gearKind: "gateway" },
  { id: "gear:2", kind: "gear", gearKind: "switch" },
  { id: 101, kind: "node", role: "client" },
  { id: 102, kind: "node", role: "client" },
  { id: 103, kind: "node", role: "client" }
];
var edges = [
  { from: "gear:1", to: "gear:2", kind: "uplink" },
  { from: 101, to: "gear:2", kind: "attaches" },
  { from: 102, to: "gear:2", kind: "attaches" },
  { from: 103, to: "gear:2", kind: "attaches" },
  // duplicate + self-loop + reversed dupe → must be tolerated
  { from: "gear:2", to: "gear:1", kind: "uplink" },
  { from: 101, to: 101, kind: "attaches" }
];

(function () {
  console.log("[b] gateway→switch→3 nodes");
  var r = Layout.compute(nodes, edges, { view: "network" });
  var ids = nodes.map(function (nd) { return nd.id; });

  // every id present + finite
  var allFinite = true, allDistinct = true, seen = {};
  ids.forEach(function (id) {
    var p = r.positions[id];
    if (!finitePos(p)) allFinite = false;
    var key = p ? p.x.toFixed(4) + "," + p.y.toFixed(4) : "missing";
    if (seen[key]) allDistinct = false;
    seen[key] = true;
  });
  ok(Object.keys(r.positions).length === ids.length, "position for every id");
  ok(allFinite, "all positions finite");
  ok(allDistinct, "all positions distinct");

  // root (gateway gear:1) pinned near origin
  var rootP = r.positions["gear:1"];
  ok(finitePos(rootP) && Math.abs(rootP.x) < 1e-6 && Math.abs(rootP.y) < 1e-6,
     "root gateway at origin");

  // bounds sanity
  ok(r.bounds && isFiniteNum(r.bounds.minX) && isFiniteNum(r.bounds.maxX) &&
     r.bounds.maxX >= r.bounds.minX && r.bounds.maxY >= r.bounds.minY,
     "finite, ordered bounds");

  // determinism: two calls → byte-identical positions
  var r2 = Layout.compute(nodes, edges, { view: "network" });
  var det = JSON.stringify(r.positions) === JSON.stringify(r2.positions);
  ok(det, "deterministic across calls");
})();

// -------- (c) disconnected node still gets a finite position -----------
(function () {
  console.log("[c] disconnected node");
  var nodes2 = nodes.concat([{ id: "gear:99", kind: "gear", gearKind: "other" }]);
  var r = Layout.compute(nodes2, edges, { view: "network" });
  var p = r.positions["gear:99"];
  ok(finitePos(p), "disconnected node has finite position");
  // it should sit outside the connected cluster (outer ring)
  var rad = Math.sqrt(p.x * p.x + p.y * p.y);
  ok(rad > 0, "disconnected node placed away from origin");
})();

// -------- (d) single node → centered --------------------------------------
(function () {
  console.log("[d] single node");
  var r = Layout.compute([{ id: 7, kind: "node", role: "server" }], []);
  var p = r.positions[7];
  ok(finitePos(p) && p.x === 0 && p.y === 0, "single node centered at origin");
})();

// -------- (e) monitoring view root heuristic (server) ---------------------
(function () {
  console.log("[e] monitoring view root = server");
  var mnodes = [
    { id: 1, kind: "node", role: "monitor" },
    { id: 2, kind: "node", role: "server" },
    { id: "target:5", kind: "target" }
  ];
  var medges = [
    { from: 1, to: "target:5", kind: "monitors" },
    { from: 2, to: 1, kind: "monitors" }
  ];
  var r = Layout.compute(mnodes, medges, { view: "monitoring" });
  var sp = r.positions[2];
  ok(finitePos(sp) && Math.abs(sp.x) < 1e-6 && Math.abs(sp.y) < 1e-6,
     "server node pinned at origin");
  var allF = mnodes.every(function (nd) { return finitePos(r.positions[nd.id]); });
  ok(allF, "all monitoring positions finite");
})();

console.log("");
if (failures > 0) {
  console.log(failures + " assertion(s) FAILED");
  process.exit(1);
}
console.log("all assertions passed");
process.exit(0);
