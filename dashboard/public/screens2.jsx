/* ============================================================
   SolariNet — screens2: Reachability matrix, Topology map
   ============================================================ */
(function () {
  const { useState, useMemo } = React;
  const Icon = window.Icon;
  const { StatusDot, RTTBars, metricColor } = window;
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

  /* ===================== TOPOLOGY MAP ===================== */
  function NodeGlyph({ p }) {
    const fill = STATE_COLOR[p.n.state];
    const role = p.n.role;
    if (role === "server") return <rect x={-p.r} y={-p.r} width={p.r * 2} height={p.r * 2} rx="3" fill={fill} />;
    if (role === "monitor") return <rect x={-p.r} y={-p.r} width={p.r * 2} height={p.r * 2} transform="rotate(45)" fill={fill} />;
    return <circle r={p.r} fill={fill} />;
  }

  function Topology({ onOpenNode }) {
    const [view, setView] = useState("infra");
    const [sel, setSel] = useState(null);
    const W = 1000;

    const infra = useMemo(() => {
      const cx = W / 2, cy = 360, H = 720;
      const pos = {}; const anchors = [];
      const servers = S.nodes.filter((n) => n.role === "server");
      const monitors = S.nodes.filter((n) => n.role === "monitor");
      servers.forEach((s, i) => { pos[s.nodeId] = { x: cx + (i === 0 ? -26 : 26), y: cy, r: 13, n: s }; });
      monitors.forEach((m, i) => { const a = (i / monitors.length) * Math.PI * 2 - Math.PI / 2; pos[m.nodeId] = { x: cx + Math.cos(a) * 132, y: cy + Math.sin(a) * 132, r: 8, n: m }; });
      S.segments.forEach((seg, si) => {
        const clients = S.nodes.filter((n) => n.role === "client" && n.segId === seg.id);
        const baseA = (si / S.segments.length) * Math.PI * 2 - Math.PI / 2;
        const cols = Math.ceil(Math.sqrt(clients.length * 1.6)) || 1;
        clients.forEach((cn, ci) => {
          const col = ci % cols, row = Math.floor(ci / cols);
          const a = baseA + (col / Math.max(1, cols - 1) - 0.5) * 0.52;
          const rad = 235 + row * 26;
          pos[cn.nodeId] = { x: cx + Math.cos(a) * rad, y: cy + Math.sin(a) * rad * 0.92, r: 5, n: cn };
        });
        anchors.push({ id: seg.id, name: seg.name, x: cx + Math.cos(baseA) * 200, y: cy + Math.sin(baseA) * 188 });
      });
      const edges = [];
      monitors.forEach((m) => { if (pos[m.nodeId] && servers[0]) edges.push({ from: m.nodeId, to: servers[0].nodeId, kind: "report", label: "Telemetry report", sub: "SCP/TLS · PUSH :7701" }); });
      S.probes.forEach((p) => p.vantages.forEach((v) => {
        if (pos[v.monitorNode] && pos[p.hostNode]) edges.push({ from: v.monitorNode, to: p.hostNode, kind: "probe", ok: v.outcome === "ok", label: `${p.proto.toUpperCase()} probe · ${p.label}`, sub: `${v.outcome} · ${fmt.rtt(v.rttMicros)}${v.lossPermille ? ` · ${(v.lossPermille / 10).toFixed(0)}% loss` : ""}`, openId: p.hostNode });
      }));
      if (servers[1]) edges.push({ from: servers[0].nodeId, to: servers[1].nodeId, kind: "lease", label: "Failover lease", sub: "DB-mediated mutex · TTL 15s" });
      return { pos, edges, anchors, hub: { x: cx, y: cy }, H, curve: 18 };
    }, []);

    const lan = useMemo(() => {
      const pos = {}; const edges = [];
      const gw = S.netgear.find((g) => g.kind === "gateway");
      const core = S.netgear.find((g) => g.id === "sw-core");
      const others = S.netgear.filter((g) => g.id !== gw.id && g.id !== core.id);
      const mid = Math.floor(others.length / 2);
      const gearRow = [...others.slice(0, mid), core, ...others.slice(mid)]; // core centered as spine
      const yGw = 60, yGear = 198, ySys = 288, n = gearRow.length;
      const margin = 78, span = W - margin * 2;
      pos["gear:" + gw.id] = { x: W / 2, y: yGw, gear: gw, r: 16 };
      gearRow.forEach((g, i) => {
        const x = margin + (n > 1 ? span * (i / (n - 1)) : span / 2);
        pos["gear:" + g.id] = { x, y: yGear, gear: g, r: g.id === "sw-core" ? 13 : 11 };
      });
      gearRow.forEach((g) => {
        edges.push({ from: "gear:" + g.id, to: "gear:" + g.uplink, kind: "uplink", label: g.id === "sw-core" ? "Core uplink" : g.wireless ? "AP uplink" : "Switch uplink", sub: g.id === "sw-core" ? "40G fiber · LACP" : g.wireless ? "1G PoE+" : "10G SFP+" });
        const sys = S.nodes.filter((nd) => nd.uplink === g.id);
        const cols = Math.max(3, Math.round(Math.sqrt(sys.length * 1.3)));
        const gx = pos["gear:" + g.id].x;
        const cellW = Math.min(19, (span / n - 8) / cols);
        sys.forEach((nd, si) => {
          const col = si % cols, row = Math.floor(si / cols);
          const sx = gx - (cols - 1) * cellW / 2 + col * cellW;
          const sy = ySys + row * 21;
          pos[nd.nodeId] = { x: sx, y: sy, n: nd, r: 5 };
          edges.push({ from: nd.nodeId, to: "gear:" + g.id, kind: nd.linkType === "wireless" ? "wireless" : "wired", label: nd.linkType === "wireless" ? "Wireless association" : "Wired link", sub: `${fmt.mbps(nd.linkSpeedMbps)} · ${nd.uplinkPort}${nd.linkType === "wireless" ? ` · ${nd.rssi} dBm` : " · LLDP"}`, openId: nd.nodeId });
        });
      });
      let maxY = ySys; Object.values(pos).forEach((p) => { if (p.y > maxY) maxY = p.y; });
      return { pos, edges, anchors: [], H: Math.max(560, maxY + 50), curve: 0 };
    }, []);

    const L = view === "infra" ? infra : lan;
    const H = L.H;

    const { activeNodes, activeEdge } = useMemo(() => {
      if (!sel) return { activeNodes: null, activeEdge: null };
      if (sel.kind === "edge") { const e = L.edges[sel.i]; return e ? { activeNodes: new Set([e.from, e.to]), activeEdge: sel.i } : { activeNodes: null, activeEdge: null }; }
      const set = new Set([sel.id]);
      L.edges.forEach((e) => { if (e.from === sel.id) set.add(e.to); if (e.to === sel.id) set.add(e.from); });
      return { activeNodes: set, activeEdge: null };
    }, [sel, L]);

    function edgePath(e) {
      const a = L.pos[e.from], b = L.pos[e.to]; if (!a || !b) return null;
      const mx = (a.x + b.x) / 2, my = (a.y + b.y) / 2 - L.curve;
      return `M${a.x} ${a.y} Q ${mx} ${my} ${b.x} ${b.y}`;
    }
    function nm(id) { const p = L.pos[id]; return p ? (p.n ? p.n.name : p.gear ? p.gear.name : id) : id; }

    const selNode = sel && sel.kind === "node" ? L.pos[sel.id] : null;
    const selGear = sel && sel.kind === "gear" ? L.pos[sel.id] : null;
    const selEdge = sel && sel.kind === "edge" ? L.edges[sel.i] : null;
    const probeEdges = infra.edges.filter((e) => e.kind === "probe").length;

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Topology Map</h1>
            <div className="page-sub">{view === "infra" ? `monitoring C2 view · ${S.nodes.length} nodes · ${probeEdges} probe edges` : `LAN hierarchy · ${S.netgear.length} network devices · LLDP + agent-derived`} · live</div>
          </div>
          <div className="page-head__right">
            <div className="seg">
              <button className={view === "infra" ? "on" : ""} onClick={() => { setView("infra"); setSel(null); }}><Icon name="topology" size={14} />Infrastructure</button>
              <button className={view === "lan" ? "on" : ""} onClick={() => { setView("lan"); setSel(null); }}><Icon name="netswitch" size={14} />LAN hierarchy</button>
            </div>
          </div>
        </div>

        <div className="topo-legend" style={{ marginBottom: 14 }}>
          <span><span className="dot up" />Up</span><span><span className="dot degraded" />Degraded</span><span><span className="dot down" />Down</span>
          {view === "infra"
            ? <><span><i className="leg-line report" />report</span><span><i className="leg-line probe" />probe</span><span><i className="leg-line lease" />lease</span></>
            : <><span><i className="leg-line wired" />wired</span><span><i className="leg-line wireless" />wireless</span><span><Icon name="gateway" size={13} style={{ color: "var(--violet)" }} />gateway</span><span><Icon name="netswitch" size={13} style={{ color: "var(--teal)" }} />switch</span><span><Icon name="wifi" size={13} style={{ color: "var(--warn)" }} />AP</span></>}
        </div>

        <div className="panel topo-panel">
          <svg viewBox={`0 0 ${W} ${H}`} className="topo-svg" onClick={() => setSel(null)} preserveAspectRatio="xMidYMid meet">
            {L.anchors.map((seg) => <text key={seg.id} x={seg.x} y={seg.y} className="topo-seglabel" textAnchor="middle">{seg.name.toUpperCase()}</text>)}

            {/* visible edges */}
            <g>
              {L.edges.map((e, i) => {
                const d = edgePath(e); if (!d) return null;
                const inActive = activeNodes && activeNodes.has(e.from) && activeNodes.has(e.to);
                const active = activeEdge === i || (sel && sel.kind === "node" && inActive);
                const dim = sel && !active;
                return <path key={i} d={d} fill="none" className={"topo-edge " + e.kind + (e.ok === false ? " bad" : "") + (dim ? " dim" : "") + (active ? " active" : "")} />;
              })}
            </g>
            {/* invisible wide hit-paths for touch */}
            <g>
              {L.edges.map((e, i) => {
                const d = edgePath(e); if (!d) return null;
                return <path key={i} d={d} className="topo-hit" onClick={(ev) => { ev.stopPropagation(); setSel({ kind: "edge", i }); }} />;
              })}
            </g>

            {view === "infra" && <circle cx={L.hub.x} cy={L.hub.y} r="46" className="topo-hub-glow" />}

            {/* nodes + gear */}
            <g>
              {Object.entries(L.pos).map(([id, p]) => {
                const dim = sel && activeNodes && !activeNodes.has(id);
                const isSel = (selNode && sel.id === id) || (selGear && sel.id === id);
                if (p.gear) {
                  return (
                    <g key={id} className={"topo-node gear " + p.gear.kind + (dim ? " dim" : "") + (isSel ? " sel" : "")} transform={`translate(${p.x},${p.y})`}
                      onClick={(ev) => { ev.stopPropagation(); setSel(isSel ? null : { kind: "gear", id }); }}>
                      <rect x={-p.r} y={-p.r * 0.72} width={p.r * 2} height={p.r * 1.44} rx="3" fill={GEAR_FILL[p.gear.kind]} />
                      <text y={-p.r - 5} className="topo-nodelabel strong" textAnchor="middle">{p.gear.name}</text>
                    </g>
                  );
                }
                return (
                  <g key={id} className={"topo-node " + p.n.role + (dim ? " dim" : "") + (isSel ? " sel" : "")} transform={`translate(${p.x},${p.y})`}
                    onClick={(ev) => { ev.stopPropagation(); setSel(isSel ? null : { kind: "node", id }); }}>
                    <NodeGlyph p={p} />
                    {((view === "infra" && p.n.role !== "client") || isSel) && <text y={-p.r - 5} className="topo-nodelabel" textAnchor="middle">{p.n.name}</text>}
                  </g>
                );
              })}
            </g>
          </svg>

          {/* NODE card */}
          {selNode && (
            <div className="topo-card">
              <button className="topo-card__close" onClick={() => setSel(null)} aria-label="Close"><Icon name="close" size={14} /></button>
              <div className="topo-card__head">
                <Icon name={selNode.n.role === "server" ? "server" : selNode.n.role === "monitor" ? "monitor" : "host"} size={18} style={{ color: "var(--teal)" }} />
                <div><div style={{ fontFamily: "var(--mono)", fontWeight: 600 }}>{selNode.n.name}<span className="muted">.akoria.net</span></div>
                  <div className="td-mono muted" style={{ fontSize: 11 }}>{selNode.n.role}{selNode.n.label ? " · " + selNode.n.label : ""} · {selNode.n.segName} · {selNode.n.ip}</div></div>
              </div>
              <div className="topo-card__stats">
                <div><div className="kpi__k">CPU</div><div className="td-mono" style={{ color: metricColor(selNode.n.cpuPct) }}>{selNode.n.state === "down" ? "—" : selNode.n.cpuPct + "%"}</div></div>
                <div><div className="kpi__k">RAM</div><div className="td-mono" style={{ color: metricColor(selNode.n.ramPct) }}>{selNode.n.ramPct}%</div></div>
                <div><div className="kpi__k">Link</div><div className="td-mono">{fmt.mbps(selNode.n.linkSpeedMbps)}</div></div>
                <div><div className="kpi__k">Uplink</div><div className="td-mono">{selNode.n.uplink}</div></div>
              </div>
              <button className="backbtn" style={{ width: "100%", justifyContent: "center" }} onClick={() => onOpenNode(selNode.n.nodeId)}><Icon name="enter" size={14} />Open node detail</button>
            </div>
          )}

          {/* GEAR card */}
          {selGear && (
            <div className="topo-card">
              <button className="topo-card__close" onClick={() => setSel(null)} aria-label="Close"><Icon name="close" size={14} /></button>
              <div className="topo-card__head">
                <Icon name={GEAR_ICON[selGear.gear.kind]} size={18} style={{ color: GEAR_FILL[selGear.gear.kind] }} />
                <div><div style={{ fontFamily: "var(--mono)", fontWeight: 600 }}>{selGear.gear.name}</div>
                  <div className="td-mono muted" style={{ fontSize: 11 }}>{selGear.gear.kind} · {selGear.gear.model}</div></div>
              </div>
              <div className="topo-card__stats">
                <div><div className="kpi__k">Attached</div><div className="td-mono" style={{ color: "var(--teal)" }}>{selGear.gear.attached}</div></div>
                <div><div className="kpi__k">Ports</div><div className="td-mono">{selGear.gear.ports || "—"}</div></div>
                <div><div className="kpi__k">Uplink</div><div className="td-mono">{selGear.gear.uplink || "WAN"}</div></div>
                <div><div className="kpi__k">Segment</div><div className="td-mono">{selGear.gear.seg}</div></div>
              </div>
              <button className="backbtn" style={{ width: "100%", justifyContent: "center" }} onClick={() => window.__solariToast && window.__solariToast(`Port map for ${selGear.gear.name} — ${selGear.gear.attached} active LLDP neighbours`, "netswitch")}><Icon name="link" size={14} />View LLDP neighbours</button>
            </div>
          )}

          {/* EDGE card */}
          {selEdge && (
            <div className="topo-card">
              <button className="topo-card__close" onClick={() => setSel(null)} aria-label="Close"><Icon name="close" size={14} /></button>
              <div className="topo-card__head">
                <Icon name={selEdge.kind === "probe" ? "reachability" : selEdge.kind === "wireless" ? "wifi" : selEdge.kind === "report" ? "activity" : "link"} size={18} style={{ color: selEdge.ok === false ? "var(--crit)" : "var(--teal)" }} />
                <div><div style={{ fontFamily: "var(--mono)", fontWeight: 600 }}>{selEdge.label}</div>
                  <div className="td-mono muted" style={{ fontSize: 11 }}>connection · {selEdge.kind}</div></div>
                <span className={"alert-sev " + (selEdge.ok === false ? "crit" : "info")} style={{ marginLeft: "auto" }}>{selEdge.ok === false ? "fault" : "ok"}</span>
              </div>
              <div className="edge-link">
                <span className="td-mono">{nm(selEdge.from)}</span>
                <span className="edge-arrow"><Icon name="chevronRight" size={14} /></span>
                <span className="td-mono">{nm(selEdge.to)}</span>
              </div>
              <div className="td-mono muted" style={{ fontSize: 12, margin: "10px 0 12px" }}>{selEdge.sub}</div>
              {selEdge.openId && <button className="backbtn" style={{ width: "100%", justifyContent: "center" }} onClick={() => onOpenNode(selEdge.openId)}><Icon name="enter" size={14} />Open endpoint host</button>}
            </div>
          )}

          {!sel && <div className="topo-hint"><Icon name="topology" size={14} />Tap any node, device, or connection to inspect</div>}
        </div>
      </div>
    );
  }

  Object.assign(window, { Reachability, Topology });
})();
