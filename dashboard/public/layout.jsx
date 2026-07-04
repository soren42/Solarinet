/* ============================================================
   SolariNet — topology layout engine  →  window.SolariTopoLayout

   Pure, deterministic (NO Math.random), framework-free graph layout.
   Ports network-map's radial + Fruchterman–Reingold algorithm
   (src/output/layout.c + src/core/graph.c) to a generic id-keyed
   graph where node ids are arbitrary strings or numbers.

   Public API:
     window.SolariTopoLayout.compute(nodes, edges, opts)
       nodes: [{ id, kind, gearKind?, role? }, ...]
       edges: [{ from, to, kind }, ...]
       opts : { width?, height?, rootHint?, view? }   (all optional)
       ->   { positions: { <id>: {x,y} },
              bounds:    { minX, minY, maxX, maxY } }

   No build step / no JSX in this file: plain JS hung off a global,
   same idiom as the other dashboard *.jsx modules. Also require()-able
   in Node (headless tests) via module.exports.
   ============================================================ */
(function (root) {
  "use strict";

  var TWO_PI = Math.PI * 2;
  var GOLDEN = 2.399963229728653; // golden angle (rad) — deterministic fan
  var RADIUS_STEP = 150.0; // mirrors layout.c radius_step
  var FR_ITERS = 120; // force-refine iterations
  var FR_AREA = 500.0 * 500.0; // mirrors layout.c area

  // ---- edge-kind → MST weight (L2 physical links preferred tree edges) --
  function edgeWeight(kind) {
    if (kind === "uplink" || kind === "attaches") return 1;
    if (kind === "monitors") return 2;
    return 3;
  }

  // ---- total, deterministic id ordering (ids may be str OR num, mixed) --
  function idKey(id) {
    return String(id);
  }
  function cmpId(a, b) {
    var ka = idKey(a), kb = idKey(b);
    if (ka < kb) return -1;
    if (ka > kb) return 1;
    return 0;
  }

  // ---- deterministic per-index micro-jitter (breaks exact overlaps) -----
  // Seeds from the node index only — no Math.random anywhere.
  function jitterX(i) { return Math.cos(i * GOLDEN) * 0.5; }
  function jitterY(i) { return Math.sin(i * GOLDEN) * 0.5; }

  function emptyResult() {
    return { positions: {}, bounds: { minX: 0, minY: 0, maxX: 0, maxY: 0 } };
  }

  function compute(nodes, edges, opts) {
    opts = opts || {};
    nodes = nodes || [];
    edges = edges || [];

    var n = nodes.length;
    if (n === 0) return emptyResult();

    // ---- index map (id -> dense index), stable node order ---------------
    var idOf = new Array(n); // index -> original id
    var idxOf = {}; // idKey -> index
    var i, j;
    for (i = 0; i < n; i++) {
      var nid = nodes[i].id;
      idOf[i] = nid;
      idxOf[idKey(nid)] = i;
    }

    // Single node → centered at origin.
    if (n === 1) {
      var res1 = { positions: {}, bounds: { minX: 0, minY: 0, maxX: 0, maxY: 0 } };
      res1.positions[idOf[0]] = { x: 0, y: 0 };
      return res1;
    }

    // ---- build dedup'd undirected edge list (ignore self-loops) ---------
    // Keep the MIN-weight kind seen for a given undirected pair.
    var edgeMap = {}; // "lo|hi" -> { a, b, w }
    for (i = 0; i < edges.length; i++) {
      var e = edges[i];
      if (!e) continue;
      var fk = idKey(e.from), tk = idKey(e.to);
      if (!(fk in idxOf) || !(tk in idxOf)) continue; // endpoint not a node
      var a = idxOf[fk], b = idxOf[tk];
      if (a === b) continue; // self-loop → ignore
      var lo = a < b ? a : b, hi = a < b ? b : a;
      var key = lo + "|" + hi;
      var w = edgeWeight(e.kind);
      var prev = edgeMap[key];
      if (!prev) edgeMap[key] = { a: lo, b: hi, w: w };
      else if (w < prev.w) prev.w = w;
    }

    var uEdges = [];
    for (var k in edgeMap) {
      if (Object.prototype.hasOwnProperty.call(edgeMap, k)) uEdges.push(edgeMap[k]);
    }

    // ---- adjacency (full graph, from dedup'd edges) ---------------------
    var adj = new Array(n);
    for (i = 0; i < n; i++) adj[i] = [];
    var degree = new Array(n);
    for (i = 0; i < n; i++) degree[i] = 0;
    for (i = 0; i < uEdges.length; i++) {
      var ue = uEdges[i];
      adj[ue.a].push(ue.b);
      adj[ue.b].push(ue.a);
      degree[ue.a]++;
      degree[ue.b]++;
    }

    // ---- Kruskal MST (union-find), deterministic tie-break -------------
    // Sort by (weight asc, then sorted (from,to) ids asc) for stable output.
    var order = uEdges.slice();
    order.sort(function (x, y) {
      if (x.w !== y.w) return x.w - y.w;
      var c = cmpId(idOf[x.a], idOf[y.a]);
      if (c !== 0) return c;
      return cmpId(idOf[x.b], idOf[y.b]);
    });

    var ufParent = new Array(n);
    var ufRank = new Array(n);
    for (i = 0; i < n; i++) { ufParent[i] = i; ufRank[i] = 0; }
    function ufFind(x) {
      while (ufParent[x] !== x) {
        ufParent[x] = ufParent[ufParent[x]]; // path compression
        x = ufParent[x];
      }
      return x;
    }
    function ufUnion(pa, pb) {
      var ra = ufFind(pa), rb = ufFind(pb);
      if (ra === rb) return false;
      if (ufRank[ra] < ufRank[rb]) ufParent[ra] = rb;
      else if (ufRank[ra] > ufRank[rb]) ufParent[rb] = ra;
      else { ufParent[rb] = ra; ufRank[ra]++; }
      return true;
    }

    var mstAdj = new Array(n);
    for (i = 0; i < n; i++) mstAdj[i] = [];
    var mstCount = 0;
    for (i = 0; i < order.length && mstCount < n - 1; i++) {
      var oe = order[i];
      if (ufUnion(oe.a, oe.b)) {
        mstAdj[oe.a].push(oe.b);
        mstAdj[oe.b].push(oe.a);
        mstCount++;
      }
    }

    // ---- decide view (network vs monitoring) ---------------------------
    var view = opts.view;
    if (view !== "network" && view !== "monitoring") {
      view = "monitoring";
      for (i = 0; i < n; i++) {
        if (nodes[i].gearKind != null && nodes[i].gearKind !== "") { view = "network"; break; }
      }
    }

    // ---- root selection ------------------------------------------------
    var rootIdx = -1;
    if (opts.rootHint != null && (idKey(opts.rootHint) in idxOf)) {
      rootIdx = idxOf[idKey(opts.rootHint)];
    } else if (view === "network") {
      // 1) gateway gear
      var bestGear = -1, bestGearDeg = -1;
      for (i = 0; i < n; i++) {
        var nd = nodes[i];
        if (nd.kind === "gear" && nd.gearKind === "gateway") { rootIdx = i; break; }
        if (nd.kind === "gear") {
          // track most-connected gear (tie → smaller id)
          if (degree[i] > bestGearDeg ||
              (degree[i] === bestGearDeg && bestGear >= 0 && cmpId(idOf[i], idOf[bestGear]) < 0)) {
            bestGearDeg = degree[i];
            bestGear = i;
          }
        }
      }
      if (rootIdx < 0 && bestGear >= 0) rootIdx = bestGear;
    } else {
      // monitoring: server, then monitor
      var firstServer = -1, firstMonitor = -1;
      for (i = 0; i < n; i++) {
        var r = nodes[i].role;
        if (r === "server" && (firstServer < 0 || cmpId(idOf[i], idOf[firstServer]) < 0)) firstServer = i;
        if (r === "monitor" && (firstMonitor < 0 || cmpId(idOf[i], idOf[firstMonitor]) < 0)) firstMonitor = i;
      }
      if (firstServer >= 0) rootIdx = firstServer;
      else if (firstMonitor >= 0) rootIdx = firstMonitor;
    }
    if (rootIdx < 0) {
      // fallback: first node by sorted id
      var sortedIdx = [];
      for (i = 0; i < n; i++) sortedIdx.push(i);
      sortedIdx.sort(function (x, y) { return cmpId(idOf[x], idOf[y]); });
      rootIdx = sortedIdx[0];
    }

    // ---- BFS depth over MST from root (fallback to full graph if the ----
    //      MST left the root's neighbourhood empty, e.g. no MST edges) ---
    function bfs(useAdj) {
      var depth = new Array(n), parent = new Array(n);
      for (i = 0; i < n; i++) { depth[i] = -1; parent[i] = -1; }
      var queue = [rootIdx];
      depth[rootIdx] = 0;
      parent[rootIdx] = rootIdx;
      var qh = 0;
      while (qh < queue.length) {
        var cur = queue[qh++];
        var nbrs = useAdj[cur];
        for (var t = 0; t < nbrs.length; t++) {
          var nb = nbrs[t];
          if (depth[nb] === -1) {
            depth[nb] = depth[cur] + 1;
            parent[nb] = cur;
            queue.push(nb);
          }
        }
      }
      return depth;
    }

    var depth = bfs(mstAdj);
    // If MST reached nothing beyond the root but the full graph would,
    // fall back to full-graph BFS so we still get a real tree structure.
    if (mstCount === 0) depth = bfs(adj);

    // ---- radial placement by depth -------------------------------------
    var xs = new Array(n), ys = new Array(n);
    var maxDepth = 0;
    for (i = 0; i < n; i++) if (depth[i] > maxDepth) maxDepth = depth[i];

    // group node indices by depth, sorted by id for a deterministic fan
    var byDepth = {}; // depth -> [idx...]
    var disconnected = [];
    for (i = 0; i < n; i++) {
      if (depth[i] < 0) { disconnected.push(i); continue; }
      var d = depth[i];
      if (!byDepth[d]) byDepth[d] = [];
      byDepth[d].push(i);
    }
    disconnected.sort(function (x, y) { return cmpId(idOf[x], idOf[y]); });

    for (var dd in byDepth) {
      if (!Object.prototype.hasOwnProperty.call(byDepth, dd)) continue;
      var group = byDepth[dd];
      group.sort(function (x, y) { return cmpId(idOf[x], idOf[y]); });
      var di = parseInt(dd, 10);
      var cnt = group.length;
      var rr = di * RADIUS_STEP;
      var phase = di * 0.5; // per-depth phase offset → rings don't align
      for (var g = 0; g < cnt; g++) {
        var idx = group[g];
        if (di === 0) {
          xs[idx] = 0;
          ys[idx] = 0;
        } else {
          var ang = phase + (cnt > 0 ? (TWO_PI * g / cnt) : 0);
          xs[idx] = rr * Math.cos(ang) + jitterX(idx);
          ys[idx] = rr * Math.sin(ang) + jitterY(idx);
        }
      }
    }

    // disconnected nodes → outer ring
    var mdisc = disconnected.length;
    if (mdisc > 0) {
      var ringR = (maxDepth + 2) * RADIUS_STEP;
      for (i = 0; i < mdisc; i++) {
        var didx = disconnected[i];
        var da = TWO_PI * i / mdisc;
        xs[didx] = ringR * Math.cos(da) + jitterX(didx);
        ys[didx] = ringR * Math.sin(da) + jitterY(didx);
      }
    }

    // ---- Fruchterman–Reingold force refine (2D, root pinned) -----------
    forceRefine(n, rootIdx, xs, ys, uEdges, FR_ITERS);

    // ---- assemble output ------------------------------------------------
    var positions = {};
    var minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    for (i = 0; i < n; i++) {
      var x = xs[i], y = ys[i];
      if (!isFinite(x)) x = 0;
      if (!isFinite(y)) y = 0;
      positions[idOf[i]] = { x: x, y: y };
      if (x < minX) minX = x;
      if (y < minY) minY = y;
      if (x > maxX) maxX = x;
      if (y > maxY) maxY = y;
    }
    if (!isFinite(minX)) { minX = 0; minY = 0; maxX = 0; maxY = 0; }

    return { positions: positions, bounds: { minX: minX, minY: minY, maxX: maxX, maxY: maxY } };
  }

  // Mirrors nm_layout_force_refine (layout.c) collapsed to 2D. Root pinned.
  function forceRefine(n, rootIdx, xs, ys, uEdges, iterations) {
    if (n < 2) return;
    var kk = Math.sqrt(FR_AREA / n);
    var dx = new Array(n), dy = new Array(n);
    var i, j;

    for (var iter = 0; iter < iterations; iter++) {
      var temp = kk * (1.0 - iter / iterations);
      for (i = 0; i < n; i++) { dx[i] = 0; dy[i] = 0; }

      // repulsive forces (all pairs)
      for (i = 0; i < n; i++) {
        for (j = i + 1; j < n; j++) {
          var ex = xs[i] - xs[j];
          var ey = ys[i] - ys[j];
          var dist = Math.sqrt(ex * ex + ey * ey);
          if (dist < 0.01) dist = 0.01;
          var force = (kk * kk) / dist;
          var fx = (ex / dist) * force;
          var fy = (ey / dist) * force;
          dx[i] += fx; dy[i] += fy;
          dx[j] -= fx; dy[j] -= fy;
        }
      }

      // attractive forces along edges
      for (var e = 0; e < uEdges.length; e++) {
        var si = uEdges[e].a, di = uEdges[e].b;
        var aex = xs[si] - xs[di];
        var aey = ys[si] - ys[di];
        var adist = Math.sqrt(aex * aex + aey * aey);
        if (adist < 0.01) adist = 0.01;
        var aforce = (adist * adist) / kk;
        var afx = (aex / adist) * aforce;
        var afy = (aey / adist) * aforce;
        dx[si] -= afx; dy[si] -= afy;
        dx[di] += afx; dy[di] += afy;
      }

      // apply with temperature limiting; root pinned
      for (i = 0; i < n; i++) {
        if (i === rootIdx) continue;
        var mdist = Math.sqrt(dx[i] * dx[i] + dy[i] * dy[i]);
        if (mdist < 0.01) continue;
        var scale = (mdist < temp) ? 1.0 : (temp / mdist);
        xs[i] += dx[i] * scale * 0.1;
        ys[i] += dy[i] * scale * 0.1;
      }
    }
  }

  var api = { compute: compute };

  root.SolariTopoLayout = api;
  if (typeof module !== "undefined" && module.exports) module.exports = api;
})(typeof window !== "undefined" ? window : global);
