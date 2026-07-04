/* ============================================================
   SolariNet — screens2: Reachability matrix, Topology map
   ============================================================ */
(function () {
  const { useState, useMemo, useEffect } = React;
  const Icon = window.Icon;
  const { StatusDot, Sparkline, RTTBars, metricColor } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;

  const STATE_COLOR = { up: "var(--ok)", degraded: "var(--warn)", down: "var(--crit)", unknown: "var(--unknown)" };
  const OUTCOME_ABBR = { ok: "OK", timeout: "T/O", refused: "RST", unreachable: "UNR", dns_fail: "DNS", tls_fail: "TLS", proto_err: "ERR" };
  const GEAR_ICON = { gateway: "gateway", switch: "netswitch", ap: "wifi" };
  const GEAR_FILL = { gateway: "var(--violet)", switch: "var(--teal)", ap: "var(--warn)" };

  /* ===================== REACHABILITY MATRIX ===================== */
  function Reachability({ onOpenNode }) {
    const [proto, setProto] = useState("all");
    const [stateF, setStateF] = useState("all");
    const [selId, setSelId] = useState(null);
    const probes = S.probes.filter((p) => (proto === "all" || p.proto === proto) && (stateF === "all" || p.state === stateF));
    const sel = selId ? S.probes.find((p) => p.targetId === selId) : null;

    const monCols = useMemo(() => {
      const map = new Map();
      S.probes.forEach((p) => p.vantages.forEach((v) => map.set(v.monitorNode, v.monitorName)));
      return [...map.entries()].map(([id, name]) => ({ id, name })).sort((a, b) => a.name.localeCompare(b.name));
    }, []);

    const roll = { total: S.probes.length, up: 0, degraded: 0, down: 0 };
    S.probes.forEach((p) => roll[p.state]++);
    const rtts = S.probes.flatMap((p) => p.vantages.filter((v) => v.outcome === "ok").map((v) => v.rttMicros));
    const avgRtt = rtts.length ? Math.round(rtts.reduce((a, b) => a + b, 0) / rtts.length) : 0;

    function cellFor(p, monId) {
      const v = p.vantages.find((x) => x.monitorNode === monId);
      if (!v) return <td key={monId} className="mx-cell empty"><span className="mx-empty" /></td>;
      const ok = v.outcome === "ok";
      const c = !ok ? "var(--crit)" : v.lossPermille > 0 ? "var(--warn)" : "var(--ok)";
      return (
        <td key={monId} className="mx-cell">
          <div className="mx-blk" style={{ background: ok ? `color-mix(in srgb, ${c} 18%, transparent)` : "var(--crit-bg)", borderColor: c, color: c }}>
            <span className="mx-rtt">{ok ? fmt.rtt(v.rttMicros) : OUTCOME_ABBR[v.outcome]}</span>
            {ok && v.lossPermille > 0 && <span className="mx-loss">{(v.lossPermille / 10).toFixed(0)}% loss</span>}
          </div>
        </td>
      );
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Reachability Matrix</h1><div className="page-sub">{S.probes.length} probe targets × {monCols.length} monitor vantages · HRW-assigned</div></div>
          <div className="page-head__right">
            <button className="backbtn" onClick={() => {
              const a = (window.SOLARI && window.SOLARI.api) || null;
              if (a && a.survey) {
                a.survey("all").then(() => { window.__solariToast && window.__solariToast("Survey dispatched to monitor fleet", "survey"); a.refresh && a.refresh().catch(() => {}); })
                  .catch((e) => window.__solariToast && window.__solariToast(`Survey failed: ${e && e.message || "error"}`, "close"));
              } else { window.__solariToast && window.__solariToast("Survey dispatched to monitor fleet", "survey"); }
            }}><Icon name="survey" size={15} />Survey now</button>
          </div>
        </div>

        <div className="kpis">
          <div className="kpi teal"><div className="kpi__k">Targets</div><div className="kpi__v">{roll.total}</div><div className="kpi__sub">under probe</div><div className="kpi__bar" /></div>
          <div className="kpi ok"><div className="kpi__k">Reachable</div><div className="kpi__v">{roll.up}</div><div className="kpi__sub">all vantages OK</div><div className="kpi__bar" /></div>
          <div className="kpi warn"><div className="kpi__k">Split vantage</div><div className="kpi__v">{roll.degraded}</div><div className="kpi__sub">partial reachability</div><div className="kpi__bar" /></div>
          <div className="kpi crit"><div className="kpi__k">Unreachable</div><div className="kpi__v">{roll.down}</div><div className="kpi__sub">from every vantage</div><div className="kpi__bar" /></div>
          <div className="kpi"><div className="kpi__k">Avg RTT</div><div className="kpi__v" style={{ color: "var(--teal)" }}>{fmt.rtt(avgRtt)}</div><div className="kpi__sub">across OK probes</div><div className="kpi__bar" style={{ background: "var(--teal)" }} /></div>
        </div>

        <div className="filters">
          {[["all", "All protos"], ["tcp", "TCP"], ["udp", "UDP"], ["icmp", "ICMP"]].map(([k, l]) => (
            <button key={k} className={"chip" + (proto === k ? " on" : "")} onClick={() => setProto(k)}>{l}</button>
          ))}
          <div style={{ width: 1, height: 24, background: "var(--line)", margin: "0 4px" }} />
          {[["all", "All"], ["up", "Reachable"], ["degraded", "Split"], ["down", "Down"]].map(([k, l]) => (
            <button key={k} className={"chip" + (stateF === k ? " on" : "")} onClick={() => setStateF(k)}>{k !== "all" && <span className={"dot " + k} />}{l}</button>
          ))}
          {!sel && <span className="td-mono muted" style={{ marginLeft: "auto", fontSize: 11 }}>tap a row for per-vantage detail</span>}
        </div>

        <div style={sel ? { display: "grid", gridTemplateColumns: "minmax(0, 1.55fr) minmax(0, 1fr)", gap: 16, alignItems: "start" } : undefined}>
          <div className="tablewrap" style={{ overflowX: "auto" }}>
            <table className="mx">
              <thead>
                <tr>
                  <th className="mx-corner">Target</th>
                  {monCols.map((m) => <th key={m.id} className="mx-mon"><span><Icon name="monitor" size={12} />{m.name}</span></th>)}
                </tr>
              </thead>
              <tbody>
                {probes.map((p) => (
                  <tr key={p.targetId} className={(selId === p.targetId ? "sel " : "") + (p.state === "degraded" ? "diverge" : "")} onClick={() => setSelId(selId === p.targetId ? null : p.targetId)}>
                    <td className="mx-target">
                      <StatusDot state={p.state} size={8} />
                      <div className="mx-target__main">
                        <span className="mx-target__id">{p.targetId}</span>
                        <span className="mx-target__lbl">{p.label}{p.state === "degraded" && <span className="diverge-tag">split</span>}</span>
                      </div>
                    </td>
                    {monCols.map((m) => cellFor(p, m.id))}
                  </tr>
                ))}
              </tbody>
            </table>
          </div>

          {sel && (
            <div className="panel">
              <div className="panel__head">
                <Icon name="reachability" size={16} />
                <h3 style={{ flex: 1, minWidth: 0, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{sel.targetId}</h3>
                <span className="tag">{sel.proto}</span>
                <button className="iconbtn" style={{ width: 32, height: 32, flex: "0 0 32px" }} onClick={() => setSelId(null)} aria-label="Close detail"><Icon name="close" size={15} /></button>
              </div>
              <div className="panel__body">
                <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: "12px 16px", marginBottom: 16 }}>
                  <div><div className="kpi__k">Service</div><div className="td-mono" style={{ fontWeight: 600 }}>{sel.label}</div></div>
                  <div style={{ minWidth: 0 }}><div className="kpi__k">Host</div><div className="td-mono" style={{ whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{sel.host}</div></div>
                  <div><div className="kpi__k">Repl factor</div><div className="td-mono">×{sel.replFactor}</div></div>
                  <div><div className="kpi__k">State</div><div className={"td-mono statetext " + sel.state} style={{ fontWeight: 600 }}>{sel.state}</div></div>
                </div>
                <div className="kpi__k" style={{ marginBottom: 8 }}>RTT per vantage</div>
                <RTTBars vantages={sel.vantages} fmt={fmt} />
                <div className="divider" />
                <table className="proc-table" style={{ width: "100%" }}>
                  <thead><tr><th>Vantage</th><th>Outcome</th><th>RTT</th><th>Jitter</th><th>Loss</th><th>Thrpt</th></tr></thead>
                  <tbody>
                    {sel.vantages.map((v, i) => (
                      <tr key={i}>
                        <td style={{ fontWeight: 600 }}>{v.monitorName}</td>
                        <td><span style={{ color: v.outcome === "ok" ? "var(--ok)" : "var(--crit)" }}>{OUTCOME_ABBR[v.outcome]}</span></td>
                        <td className="muted">{fmt.rtt(v.rttMicros)}</td>
                        <td className="muted">{v.outcome === "ok" ? fmt.rtt(v.jitterMicros) : "—"}</td>
                        <td className="muted" style={{ color: v.lossPermille > 0 ? "var(--warn)" : "inherit" }}>{v.outcome === "ok" ? (v.lossPermille / 10).toFixed(1) + "%" : "100%"}</td>
                        <td className="muted">{v.outcome === "ok" ? (v.throughputKbps / 1000).toFixed(1) + "M" : "—"}</td>
                      </tr>
                    ))}
                  </tbody>
                </table>
                <button className="backbtn" style={{ marginTop: 14 }} onClick={() => onOpenNode(sel.hostNode)}><Icon name="host" size={14} />Open host {sel.host.split(".")[0]}</button>
              </div>
            </div>
          )}
        </div>
      </div>
    );
  }

  /* ============ MANAGED GEAR — SNMP interface throughput ============ */
  /* Read-only surface over the /api/gear tier: lists each managed device's
     interfaces with an inline Sparkline of recent inRateKbps. Fetches lazily
     via the live API; renders nothing when the SNMP tier is unavailable
     (offline fixture, or no gear polled yet), so it never disrupts Topology. */
  function GearThroughput() {
    const api = window.SolariAPI;
    const [gears, setGears] = useState([]);
    const [ifaces, setIfaces] = useState({}); // gearId -> [interface rows]
    const [sparks, setSparks] = useState({}); // "gearId:ifIndex" -> [numbers]

    useEffect(() => {
      if (!api || !api.gearList) return;
      let alive = true;
      api.gearList().then((gs) => {
        if (!alive) return;
        setGears(gs || []);
        (gs || []).forEach((g) => {
          api.gearInterfaces(g.gearId).then((rows) => {
            if (!alive) return;
            setIfaces((prev) => Object.assign({}, prev, { [g.gearId]: rows }));
            (rows || []).forEach((f) => {
              api.gearHistory(g.gearId, f.ifIndex, "inRateKbps").then((data) => {
                if (!alive) return;
                setSparks((prev) => Object.assign({}, prev, { [g.gearId + ":" + f.ifIndex]: data }));
              }).catch(() => {});
            });
          }).catch(() => {});
        });
      }).catch(() => {});
      return () => { alive = false; };
    }, []);

    if (!gears.length) return null;

    return (
      <div className="panel" style={{ marginTop: 16 }}>
        <div className="panel__head">
          <Icon name="bandwidth" size={16} /><h3>Managed gear — interface throughput</h3>
          <div className="right">{gears.length} SNMP device{gears.length === 1 ? "" : "s"}</div>
        </div>
        <div className="panel__body" style={{ display: "flex", flexDirection: "column", gap: 14 }}>
          {gears.map((g) => {
            const rows = ifaces[g.gearId] || [];
            return (
              <div key={g.gearId}>
                <div style={{ display: "flex", justifyContent: "space-between", alignItems: "baseline", marginBottom: 6 }}>
                  <span className="td-mono" style={{ fontWeight: 600 }}>
                    <Icon name={GEAR_ICON[g.kind] || "netswitch"} size={13} style={{ color: GEAR_FILL[g.kind] || "var(--teal)", marginRight: 6 }} />
                    {g.name}<span className="muted" style={{ marginLeft: 6, fontWeight: 400 }}>{g.model || g.kind}</span>
                  </span>
                  <span className="td-mono muted" style={{ fontSize: 11 }}>{g.upCount}/{g.ifCount} up · ↓{fmt.mbps(Math.round((g.totalInRateKbps || 0) / 1000))} ↑{fmt.mbps(Math.round((g.totalOutRateKbps || 0) / 1000))}</span>
                </div>
                {rows.length === 0
                  ? <div className="td-mono muted" style={{ fontSize: 11, padding: "4px 0" }}>no interface samples</div>
                  : rows.map((f) => (
                    <div key={f.ifIndex} style={{ display: "flex", alignItems: "center", gap: 10, padding: "3px 0" }}>
                      <span className="dot" style={{ background: f.operStatus === 1 ? "var(--ok)" : "var(--crit)", flex: "0 0 auto" }} />
                      <span className="td-mono" style={{ flex: "0 0 96px", fontSize: 12 }}>{f.ifName || ("if" + f.ifIndex)}</span>
                      <Sparkline data={sparks[g.gearId + ":" + f.ifIndex] || []} w={140} h={24} />
                      <span className="td-mono muted" style={{ marginLeft: "auto", fontSize: 11 }}>↓{fmt.mbps(Math.round((f.inRateKbps || 0) / 1000))} ↑{fmt.mbps(Math.round((f.outRateKbps || 0) / 1000))}</span>
                    </div>
                  ))}
              </div>
            );
          })}
        </div>
      </div>
    );
  }

  /* ===================== TOPOLOGY MAP ===================== */
  /* Live, data-driven dual view. Fetches the frozen /api/topology?view=...
     contract ({view,nodes,edges} with per-node + per-edge `health` + `label`),
     lays it out with window.SolariTopoLayout, and colours every glyph/edge by
     health. When the API is offline or the fetch fails, an identically-shaped
     payload is synthesized from the S fixture (no gear ids are hardcoded —
     roots/hierarchy come from uplink fields inside the layout engine). */

  const HEALTH_COLOR = { up: "var(--ok)", degraded: "var(--warn)", down: "var(--crit)", unknown: "var(--unknown)" };

  // ---- health + label helpers (mirror dashboard/api/routes/topology.php) ----
  function healthRank(h) { return h === "down" ? 3 : h === "degraded" ? 2 : h === "up" ? 1 : 0; }
  function worseHealth(a, b) { return healthRank(b) > healthRank(a) ? b : a; }
  function probeHealth(outcome, loss) {
    let h;
    if (outcome === "ok") h = "up";
    else if (outcome === "timeout" || outcome === "unreachable" || outcome === "dns_fail") h = "down";
    else if (outcome === "refused" || outcome === "tls_fail" || outcome === "proto_err") h = "degraded";
    else h = "unknown";
    if (loss != null && loss >= 100 && h === "up") h = "degraded";
    return h;
  }
  function trimNum(v) { return (Math.round(v * 10) / 10).toString(); }
  function speedLabel(mbps) {
    if (mbps == null || mbps <= 0) return null;
    if (mbps % 1000 === 0) return (mbps / 1000) + "G";
    if (mbps >= 1000) return trimNum(mbps / 1000) + "G";
    return mbps + "M";
  }
  function monitorLabel(proto, rtt, loss) {
    const parts = [];
    if (proto) parts.push(String(proto).toUpperCase());
    parts.push(rtt != null ? trimNum(rtt / 1000) + "ms" : "—ms");
    parts.push((loss != null ? trimNum(loss / 10) : "—") + "% loss");
    return parts.join(" · ");
  }
  function attachLabel(linkType, speed, rssi, ifName, util) {
    const parts = [];
    if (linkType === "wireless") { parts.push("wireless"); if (rssi != null) parts.push(rssi + "dBm"); }
    else { const sp = speedLabel(speed); if (sp) parts.push(sp); if (ifName) parts.push(ifName); if (util != null) parts.push(util + "%"); }
    if (!parts.length) return linkType === "wireless" ? "wireless" : "wired";
    return parts.join(" · ");
  }
  function fmtStale(s) {
    if (s == null) return "—";
    if (s < 120) return s + "s";
    if (s < 7200) return Math.round(s / 60) + "m";
    return Math.round(s / 3600) + "h";
  }

  // ---- offline synthesizers: build the SAME contract shape from the fixture -
  function synthMonitoring() {
    const F = window.SOLARI || {};
    const segByNode = {};
    (F.nodes || []).forEach((n) => { segByNode[n.nodeId] = n.segId; });
    const nodes = (F.nodes || []).map((n) => ({
      id: n.nodeId, kind: "node", role: n.role, hostFqdn: n.hostFqdn, state: n.state,
      segId: n.segId, health: n.state || "unknown",
      lastSeenAt: null, staleSec: n.lastSeenMin != null ? n.lastSeenMin * 60 : null,
    }));
    const targetHealth = {};
    const edges = [];
    (F.probes || []).forEach((p) => {
      (p.vantages || []).forEach((v) => {
        const h = probeHealth(v.outcome, v.lossPermille);
        targetHealth[p.targetId] = worseHealth(targetHealth[p.targetId] || "unknown", h);
        edges.push({
          from: v.monitorNode, to: "target:" + p.targetId, kind: "monitors",
          outcome: v.outcome, rttMicros: v.rttMicros, lossPermille: v.lossPermille,
          jitterMicros: v.jitterMicros, sampledAt: null, health: h,
          label: monitorLabel(p.proto, v.rttMicros, v.lossPermille),
        });
      });
    });
    const targets = (F.probes || []).map((p) => ({
      id: "target:" + p.targetId, kind: "target", label: p.label, proto: p.proto,
      host: p.host, port: p.port, segId: segByNode[p.hostNode] || null,
      health: targetHealth[p.targetId] || "unknown",
    }));
    return { view: "monitoring", nodes: nodes.concat(targets), edges: edges };
  }

  function synthNetwork() {
    const F = window.SOLARI || {};
    const hosts = F.nodes || [];
    // gear health derived from attached node states (no SNMP interface data offline).
    const attachedStates = {};
    hosts.forEach((n) => { if (n.uplink) { (attachedStates[n.uplink] = attachedStates[n.uplink] || []).push(n.state); } });
    function gearHealth(gid) {
      const st = attachedStates[gid] || [];
      if (!st.length) return "up";
      if (st.every((s) => s === "up")) return "up";
      if (st.every((s) => s === "down")) return "down";
      return "degraded";
    }
    const gearNodes = (F.netgear || []).map((g) => ({
      id: "gear:" + g.id, kind: "gear", gearKind: g.kind, name: g.name, model: g.model,
      segId: g.seg, uplinkGearId: g.uplink || null, wireless: !!g.wireless, mgmtIp: g.mgmtIp || null,
      health: gearHealth(g.id), lastSeenAt: null, staleSec: null,
      ifUp: 0, ifDown: 0, ifTotal: 0, attached: g.attached,
    }));
    const hostNodes = hosts.map((n) => ({
      id: n.nodeId, kind: "node", role: n.role, hostFqdn: n.hostFqdn, state: n.state,
      segId: n.segId, uplinkGearId: n.uplink || null, health: n.state || "unknown",
      lastSeenAt: null, staleSec: n.lastSeenMin != null ? n.lastSeenMin * 60 : null,
    }));
    const edges = [];
    (F.netgear || []).forEach((g) => {
      if (!g.uplink) return;
      edges.push({
        from: "gear:" + g.uplink, to: "gear:" + g.id, kind: "uplink",
        linkType: g.wireless ? "wireless" : "wired", health: "up",
        operStatus: null, inRateKbps: null, outRateKbps: null, utilPct: null,
        speedMbps: null, ifName: null, label: g.wireless ? "wireless" : "wired",
      });
    });
    hosts.forEach((n) => {
      if (!n.uplink) return;
      edges.push({
        from: n.nodeId, to: "gear:" + n.uplink, kind: "attaches",
        localIf: n.uplinkPort || null, peerPort: n.uplinkPort || null,
        linkType: n.linkType || null, speedMbps: n.linkSpeedMbps || null,
        rssi: n.rssi != null ? n.rssi : null, viaLldp: !!n.lldp, health: n.state || "unknown",
        operStatus: null, inRateKbps: null, outRateKbps: null, utilPct: null,
        label: attachLabel(n.linkType, n.linkSpeedMbps, n.rssi, n.uplinkPort, null),
      });
    });
    return { view: "network", nodes: gearNodes.concat(hostNodes), edges: edges };
  }

  function synthTopology(view) { return view === "network" ? synthNetwork() : synthMonitoring(); }

  // ---- glyphs: shape by kind/role, FILL by health ---------------------------
  function radiusOf(nd) {
    if (nd.kind === "gear") return nd.gearKind === "gateway" ? 16 : 13;
    if (nd.kind === "target") return 8;
    if (nd.role === "server") return 13;
    if (nd.role === "monitor") return 9;
    return 6;
  }
  function TopoGlyph({ nd, r, fill }) {
    if (nd.kind === "gear") return <rect x={-r} y={-r * 0.72} width={r * 2} height={r * 1.44} rx="3" fill={fill} />;
    if (nd.kind === "target") return <g><circle r={r} fill="none" stroke={fill} strokeWidth="2" /><circle r={r * 0.42} fill={fill} /></g>;
    if (nd.role === "server") return <rect x={-r} y={-r} width={r * 2} height={r * 2} rx="3" fill={fill} />;
    if (nd.role === "monitor") return <rect x={-r} y={-r} width={r * 2} height={r * 2} transform="rotate(45)" fill={fill} />;
    return <circle r={r} fill={fill} />;
  }

  function Topology({ onOpenNode }) {
    const [view, setView] = useState("monitoring");   // monitoring ≈ old "infra"; network ≈ old "lan"
    const [payload, setPayload] = useState(null);      // frozen {view,nodes,edges} contract
    const [status, setStatus] = useState("loading");   // loading | ready
    const [sel, setSel] = useState(null);              // {kind:"node"|"edge", id|i}
    const [live, setLive] = useState(false);

    // fetch (or synthesize) the contract on mount + every view change.
    useEffect(() => {
      let alive = true;
      setSel(null); setPayload(null); setStatus("loading");
      const api = window.SolariAPI;
      const offline = !!(window.SOLARI && window.SOLARI.source === "offline");
      const fallback = () => { if (alive) { setPayload(synthTopology(view)); setLive(false); setStatus("ready"); } };
      if (offline || !api || !api.topology) { fallback(); return () => { alive = false; }; }
      api.topology(view).then((p) => {
        if (!alive) return;
        if (p && Array.isArray(p.nodes)) { setPayload(p); setLive(true); setStatus("ready"); }
        else fallback();
      }).catch(() => fallback());
      return () => { alive = false; };
    }, [view]);

    const nodeById = useMemo(() => {
      const m = {};
      ((payload && payload.nodes) || []).forEach((nd) => { m[nd.id] = nd; });
      return m;
    }, [payload]);

    // run the layout engine, then normalize its arbitrary coords into the viewBox.
    const layout = useMemo(() => {
      if (!payload || !payload.nodes || !payload.nodes.length) return null;
      const engine = window.SolariTopoLayout;
      if (!engine || !engine.compute) return null;
      const raw = engine.compute(payload.nodes, payload.edges || [], { view: payload.view || view });
      const b = raw.bounds || { minX: 0, minY: 0, maxX: 0, maxY: 0 };
      const VBW = 1000, margin = 74;
      const bw = Math.max(1, b.maxX - b.minX), bh = Math.max(1, b.maxY - b.minY);
      const scale = (VBW - margin * 2) / bw;
      const VBH = Math.max(420, bh * scale + margin * 2);
      const pos = {};
      payload.nodes.forEach((nd) => {
        const p = raw.positions[nd.id];
        if (!p) return;
        pos[nd.id] = { x: margin + (p.x - b.minX) * scale, y: margin + (p.y - b.minY) * scale };
      });
      const edges = (payload.edges || []).filter((e) => pos[e.from] && pos[e.to]);
      return { pos, edges, VBW, VBH };
    }, [payload, view]);

    const { activeNodes, activeEdge } = useMemo(() => {
      if (!sel || !layout) return { activeNodes: null, activeEdge: null };
      if (sel.kind === "edge") {
        const e = layout.edges[sel.i];
        return e ? { activeNodes: new Set([e.from, e.to]), activeEdge: sel.i } : { activeNodes: null, activeEdge: null };
      }
      const set = new Set([sel.id]);
      layout.edges.forEach((e) => { if (e.from === sel.id) set.add(e.to); if (e.to === sel.id) set.add(e.from); });
      return { activeNodes: set, activeEdge: null };
    }, [sel, layout]);

    function edgePath(e) {
      const a = layout.pos[e.from], b = layout.pos[e.to]; if (!a || !b) return null;
      const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2 - 16;
      return `M${a.x} ${a.y} Q ${mx} ${my} ${b.x} ${b.y}`;
    }
    function labelOf(nd) {
      if (!nd) return "";
      if (nd.kind === "gear") return nd.name || String(nd.id);
      if (nd.kind === "target") return nd.label || nd.host || String(nd.id);
      return nd.hostFqdn ? nd.hostFqdn.split(".")[0] : String(nd.id);
    }
    const nm = (id) => labelOf(nodeById[id]) || String(id);
    const isNodeRef = (id) => typeof id === "number" || (typeof id === "string" && /^\d+$/.test(id));

    const selNode = sel && sel.kind === "node" ? nodeById[sel.id] : null;
    const selEdge = sel && sel.kind === "edge" && layout ? layout.edges[sel.i] : null;

    const nCount = payload ? payload.nodes.length : 0;
    const eCount = payload ? (payload.edges || []).length : 0;
    const empty = status === "ready" && nCount === 0;
    const srcTag = live ? "live" : "offline fixture";

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Topology Map</h1>
            <div className="page-sub">
              {view === "monitoring"
                ? `monitoring view · ${nCount} nodes · ${eCount} probe edges`
                : `network view · ${nCount} devices + hosts · ${eCount} links`} · {srcTag}
            </div>
          </div>
          <div className="page-head__right">
            <div className="seg">
              <button className={view === "network" ? "on" : ""} onClick={() => { setView("network"); }}><Icon name="netswitch" size={14} />Network</button>
              <button className={view === "monitoring" ? "on" : ""} onClick={() => { setView("monitoring"); }}><Icon name="topology" size={14} />Monitoring</button>
            </div>
          </div>
        </div>

        <div className="topo-legend" style={{ marginBottom: 14 }}>
          <span><span className="dot up" />Up</span>
          <span><span className="dot degraded" />Degraded</span>
          <span><span className="dot down" />Down</span>
          <span><span className="dot unknown" />Unknown</span>
          {view === "monitoring"
            ? <span><i className="leg-line monitors" />monitors</span>
            : <>
                <span><i className="leg-line uplink" />uplink</span>
                <span><i className="leg-line wired" />wired</span>
                <span><i className="leg-line wireless" />wireless</span>
                <span><Icon name="gateway" size={13} style={{ color: "var(--violet)" }} />gateway</span>
                <span><Icon name="netswitch" size={13} style={{ color: "var(--teal)" }} />switch</span>
                <span><Icon name="wifi" size={13} style={{ color: "var(--warn)" }} />AP</span>
              </>}
        </div>

        <div className="panel topo-panel">
          {status === "loading" && <div className="topo-hint" style={{ position: "static", margin: 24 }}><Icon name="topology" size={14} />Loading topology…</div>}

          {empty && (
            <div style={{ padding: "48px 24px", textAlign: "center" }}>
              <div className="td-mono muted" style={{ fontSize: 13 }}><Icon name="topology" size={16} style={{ marginRight: 8, verticalAlign: "middle" }} />No topology data yet for this view.</div>
            </div>
          )}

          {!empty && layout && (
            <svg viewBox={`0 0 ${layout.VBW} ${layout.VBH}`} className="topo-svg" onClick={() => setSel(null)} preserveAspectRatio="xMidYMid meet">
              {/* visible edges — stroke coloured by health */}
              <g>
                {layout.edges.map((e, i) => {
                  const d = edgePath(e); if (!d) return null;
                  const inActive = activeNodes && activeNodes.has(e.from) && activeNodes.has(e.to);
                  const active = activeEdge === i || (sel && sel.kind === "node" && inActive);
                  const dim = sel && !active;
                  const color = HEALTH_COLOR[e.health] || HEALTH_COLOR.unknown;
                  const dash = e.linkType === "wireless" ? "3 4"
                    : (e.kind === "monitors" && (e.health === "down" || e.health === "degraded")) ? "4 4" : undefined;
                  return <path key={i} d={d} fill="none" className={"topo-edge " + e.kind + (dim ? " dim" : "") + (active ? " active" : "")} style={{ stroke: color, color: color, strokeDasharray: dash }} />;
                })}
              </g>
              {/* invisible wide hit-paths for click/touch selection */}
              <g>
                {layout.edges.map((e, i) => {
                  const d = edgePath(e); if (!d) return null;
                  return <path key={i} d={d} className="topo-hit" onClick={(ev) => { ev.stopPropagation(); setSel({ kind: "edge", i }); }} />;
                })}
              </g>
              {/* nodes + gear — glyph by kind/role, FILL by health */}
              <g>
                {payload.nodes.map((nd) => {
                  const p = layout.pos[nd.id]; if (!p) return null;
                  const r = radiusOf(nd);
                  const fill = HEALTH_COLOR[nd.health] || HEALTH_COLOR.unknown;
                  const dim = sel && activeNodes && !activeNodes.has(nd.id);
                  const isSel = sel && sel.kind === "node" && sel.id === nd.id;
                  const kindCls = nd.kind === "gear" ? "gear " + (nd.gearKind || "other") : nd.kind === "target" ? "target" : (nd.role || "client");
                  const showLabel = nd.kind === "gear" || nd.kind === "target" || nd.role === "server" || nd.role === "monitor" || isSel;
                  return (
                    <g key={nd.id} className={"topo-node " + kindCls + (dim ? " dim" : "") + (isSel ? " sel" : "")} transform={`translate(${p.x},${p.y})`}
                      onClick={(ev) => { ev.stopPropagation(); setSel(isSel ? null : { kind: "node", id: nd.id }); }}>
                      <TopoGlyph nd={nd} r={r} fill={fill} />
                      {showLabel && <text y={-r - 5} className={"topo-nodelabel" + (nd.kind === "gear" ? " strong" : "")} textAnchor="middle">{labelOf(nd)}</text>}
                    </g>
                  );
                })}
              </g>
            </svg>
          )}

          {/* NODE / GEAR / TARGET card */}
          {selNode && (
            <div className="topo-card">
              <button className="topo-card__close" onClick={() => setSel(null)} aria-label="Close"><Icon name="close" size={14} /></button>
              {selNode.kind === "gear" ? (
                <>
                  <div className="topo-card__head">
                    <Icon name={GEAR_ICON[selNode.gearKind] || "netswitch"} size={18} style={{ color: GEAR_FILL[selNode.gearKind] || "var(--teal)" }} />
                    <div style={{ minWidth: 0 }}><div style={{ fontFamily: "var(--mono)", fontWeight: 600 }}>{selNode.name}</div>
                      <div className="td-mono muted" style={{ fontSize: 11 }}>{selNode.gearKind}{selNode.model ? " · " + selNode.model : ""}</div></div>
                    <span className={"alert-sev " + (selNode.health === "down" ? "crit" : selNode.health === "degraded" ? "warn" : "info")} style={{ marginLeft: "auto" }}>{selNode.health}</span>
                  </div>
                  <div className="topo-card__stats">
                    <div><div className="kpi__k">Health</div><div className={"td-mono statetext " + selNode.health} style={{ fontWeight: 600 }}>{selNode.health}</div></div>
                    <div><div className="kpi__k">Interfaces</div><div className="td-mono">{selNode.ifTotal ? `${selNode.ifUp}/${selNode.ifTotal} up` : "—"}</div></div>
                    <div><div className="kpi__k">Segment</div><div className="td-mono">{selNode.segId || "—"}</div></div>
                    <div><div className="kpi__k">Uplink</div><div className="td-mono">{selNode.uplinkGearId || "WAN"}</div></div>
                  </div>
                </>
              ) : selNode.kind === "target" ? (
                <>
                  <div className="topo-card__head">
                    <Icon name="reachability" size={18} style={{ color: HEALTH_COLOR[selNode.health] }} />
                    <div style={{ minWidth: 0 }}><div style={{ fontFamily: "var(--mono)", fontWeight: 600 }}>{selNode.label}</div>
                      <div className="td-mono muted" style={{ fontSize: 11 }}>{selNode.proto} · {selNode.host}{selNode.port ? ":" + selNode.port : ""}</div></div>
                    <span className={"alert-sev " + (selNode.health === "down" ? "crit" : selNode.health === "degraded" ? "warn" : "info")} style={{ marginLeft: "auto" }}>{selNode.health}</span>
                  </div>
                  <div className="topo-card__stats">
                    <div><div className="kpi__k">Health</div><div className={"td-mono statetext " + selNode.health} style={{ fontWeight: 600 }}>{selNode.health}</div></div>
                    <div><div className="kpi__k">Proto</div><div className="td-mono">{selNode.proto || "—"}</div></div>
                    <div><div className="kpi__k">Port</div><div className="td-mono">{selNode.port || "—"}</div></div>
                    <div><div className="kpi__k">Segment</div><div className="td-mono">{selNode.segId || "—"}</div></div>
                  </div>
                </>
              ) : (
                <>
                  <div className="topo-card__head">
                    <Icon name={selNode.role === "server" ? "server" : selNode.role === "monitor" ? "monitor" : "host"} size={18} style={{ color: HEALTH_COLOR[selNode.health] }} />
                    <div style={{ minWidth: 0 }}><div style={{ fontFamily: "var(--mono)", fontWeight: 600, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{labelOf(selNode)}</div>
                      <div className="td-mono muted" style={{ fontSize: 11 }}>{selNode.role}{selNode.segId ? " · " + selNode.segId : ""}</div></div>
                    <span className={"alert-sev " + (selNode.health === "down" ? "crit" : selNode.health === "degraded" ? "warn" : "info")} style={{ marginLeft: "auto" }}>{selNode.health}</span>
                  </div>
                  <div className="topo-card__stats">
                    <div><div className="kpi__k">Health</div><div className={"td-mono statetext " + selNode.health} style={{ fontWeight: 600 }}>{selNode.health}</div></div>
                    <div><div className="kpi__k">Role</div><div className="td-mono">{selNode.role || "—"}</div></div>
                    <div><div className="kpi__k">Last seen</div><div className="td-mono">{fmtStale(selNode.staleSec)}</div></div>
                    <div><div className="kpi__k">Uplink</div><div className="td-mono">{selNode.uplinkGearId || "—"}</div></div>
                  </div>
                  <button className="backbtn" style={{ width: "100%", justifyContent: "center" }} onClick={() => onOpenNode(selNode.id)}><Icon name="enter" size={14} />Open node detail</button>
                </>
              )}
            </div>
          )}

          {/* EDGE card */}
          {selEdge && (
            <div className="topo-card">
              <button className="topo-card__close" onClick={() => setSel(null)} aria-label="Close"><Icon name="close" size={14} /></button>
              <div className="topo-card__head">
                <Icon name={selEdge.kind === "monitors" ? "reachability" : selEdge.linkType === "wireless" ? "wifi" : selEdge.kind === "uplink" ? "activity" : "link"} size={18} style={{ color: HEALTH_COLOR[selEdge.health] }} />
                <div style={{ minWidth: 0 }}><div style={{ fontFamily: "var(--mono)", fontWeight: 600, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{selEdge.label || selEdge.kind}</div>
                  <div className="td-mono muted" style={{ fontSize: 11 }}>connection · {selEdge.kind}</div></div>
                <span className={"alert-sev " + (selEdge.health === "down" ? "crit" : selEdge.health === "degraded" ? "warn" : "info")} style={{ marginLeft: "auto" }}>{selEdge.health}</span>
              </div>
              <div className="edge-link">
                <span className="td-mono">{nm(selEdge.from)}</span>
                <span className="edge-arrow"><Icon name="chevronRight" size={14} /></span>
                <span className="td-mono">{nm(selEdge.to)}</span>
              </div>
              <div className="td-mono muted" style={{ fontSize: 12, margin: "10px 0 12px" }}>
                {selEdge.kind === "monitors"
                  ? `${selEdge.outcome || "—"} · ${selEdge.rttMicros != null ? fmt.rtt(selEdge.rttMicros) : "—"}${selEdge.lossPermille ? ` · ${(selEdge.lossPermille / 10).toFixed(0)}% loss` : ""}`
                  : `${selEdge.linkType || "wired"}${selEdge.speedMbps ? ` · ${fmt.mbps(selEdge.speedMbps)}` : ""}${selEdge.utilPct != null ? ` · ${selEdge.utilPct}% util` : ""}${selEdge.rssi != null ? ` · ${selEdge.rssi} dBm` : ""}`}
              </div>
              {isNodeRef(selEdge.from) && <button className="backbtn" style={{ width: "100%", justifyContent: "center" }} onClick={() => onOpenNode(typeof selEdge.from === "number" ? selEdge.from : Number(selEdge.from))}><Icon name="enter" size={14} />Open endpoint host</button>}
            </div>
          )}

          {!sel && !empty && status === "ready" && <div className="topo-hint"><Icon name="topology" size={14} />Tap any node, device, or connection to inspect</div>}
        </div>

        {/* SNMP-managed gear throughput (read-only) under the Network view. */}
        {view === "network" && <GearThroughput />}
      </div>
    );
  }

  Object.assign(window, { Reachability, Topology, GearThroughput });
})();
