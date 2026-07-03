/* ============================================================
   SolariNet — screens: FleetOverview, NodeDetail, AlertsScreen
   ============================================================ */
(function () {
  const { useState, useMemo, useEffect } = React;
  const Icon = window.Icon;
  const { StatusDot, Sparkline, TimeSeries, BandwidthGauge, RadialGauge, HealthDonut, RTTBars, metricColor } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;

  const ROLE_ICON = { server: "server", monitor: "monitor", client: "host" };
  const STATE_COLOR = { up: "var(--ok)", degraded: "var(--warn)", down: "var(--crit)", unknown: "var(--unknown)" };

  /* ===================== FLEET OVERVIEW ===================== */
  function FleetOverview({ onOpenNode, view, setView, fleet }) {
    const [stateFilter, setStateFilter] = useState("all");
    const [roleFilter, setRoleFilter] = useState("all");
    const [dense, setDense] = useState(true);
    const [sort, setSort] = useState({ key: "name", dir: 1 });

    const filtered = useMemo(() => fleet.filter((n) =>
      (stateFilter === "all" || n.state === stateFilter) &&
      (roleFilter === "all" || n.role === roleFilter)
    ), [fleet, stateFilter, roleFilter]);

    const roll = S.fleetRoll;
    const cpuNodes = fleet.filter((n) => !n.isAsset && n.state !== "down");
    const avgCpu = Math.round(cpuNodes.reduce((a, n) => a + n.cpuPct, 0) / Math.max(1, cpuNodes.length));
    const monsUp = fleet.filter((n) => n.role === "monitor" && n.state === "up").length;
    const monsTotal = fleet.filter((n) => n.role === "monitor").length;

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Fleet Overview</h1>
            <div className="page-sub">{roll.total} systems · {S.summary.applications} applications · {S.segments.length} segments · live</div>
          </div>
          <div className="page-head__right">
            <div className="seg">
              <button className={view === "heat" ? "on" : ""} onClick={() => setView("heat")}><Icon name="grid" size={14} />Heatmap</button>
              <button className={view === "table" ? "on" : ""} onClick={() => setView("table")}><Icon name="table" size={14} />Table</button>
              <button className={view === "cards" ? "on" : ""} onClick={() => setView("cards")}><Icon name="cards" size={14} />Cards</button>
            </div>
          </div>
        </div>

        {/* KPIs */}
        <div className="kpis">
          <div className="kpi teal"><div className="kpi__k">Systems</div><div className="kpi__v">{roll.total}</div><div className="kpi__sub">monitored hosts</div><div className="kpi__bar" /></div>
          <div className="kpi ok"><div className="kpi__k">Operational</div><div className="kpi__v">{roll.up}</div><div className="kpi__sub">{Math.round(roll.up / roll.total * 100)}% healthy</div><div className="kpi__bar" /></div>
          <div className="kpi warn"><div className="kpi__k">Degraded</div><div className="kpi__v">{roll.degraded}</div><div className="kpi__sub">over tolerance</div><div className="kpi__bar" /></div>
          <div className="kpi crit"><div className="kpi__k">Down</div><div className="kpi__v">{roll.down}</div><div className="kpi__sub">{S.activeCrit} critical alerts</div><div className="kpi__bar" /></div>
          <div className="kpi"><div className="kpi__k">Avg CPU</div><div className="kpi__v" style={{ color: metricColor(avgCpu) }}>{avgCpu}%</div><div className="kpi__sub">across live hosts</div><div className="kpi__bar" style={{ background: metricColor(avgCpu) }} /></div>
          <div className="kpi violet"><div className="kpi__k">Monitors</div><div className="kpi__v">{monsUp}<span style={{ fontSize: 16, color: "var(--ink-faint)" }}>/{monsTotal}</span></div><div className="kpi__sub">vantages online</div><div className="kpi__bar" /></div>
        </div>

        {/* filters */}
        <div className="filters">
          {[["all", "All", roll.total], ["up", "Up", roll.up], ["degraded", "Degraded", roll.degraded], ["down", "Down", roll.down], ["unknown", "Unknown", roll.unknown]].map(([k, lbl, n]) => (
            <button key={k} className={"chip" + (stateFilter === k ? " on" : "")} onClick={() => setStateFilter(k)}>
              {k !== "all" && <span className={"dot " + k} />}{lbl}<span className="chip__n">{n}</span>
            </button>
          ))}
          <div style={{ width: 1, height: 24, background: "var(--line)", margin: "0 4px" }} />
          {[["all", "All roles"], ["client", "Clients"], ["monitor", "Monitors"], ["server", "Servers"]].map(([k, lbl]) => (
            <button key={k} className={"chip" + (roleFilter === k ? " on" : "")} onClick={() => setRoleFilter(k)}>
              {k !== "all" && <Icon name={ROLE_ICON[k]} size={14} />}{lbl}
            </button>
          ))}
          {view === "heat" && (
            <button className="chip" style={{ marginLeft: "auto" }} onClick={() => setDense((d) => !d)}>
              <Icon name={dense ? "cards" : "grid"} size={14} />{dense ? "Compact" : "Detailed"}
            </button>
          )}
        </div>

        {view === "heat" && <HeatView nodes={filtered} dense={dense} onOpenNode={onOpenNode} />}
        {view === "table" && <TableView nodes={filtered} sort={sort} setSort={setSort} onOpenNode={onOpenNode} />}
        {view === "cards" && <CardsView nodes={filtered} onOpenNode={onOpenNode} />}
      </div>
    );
  }

  function Cell({ n, dense, onOpenNode }) {
    const load = n.state === "down" ? 0 : n.cpuPct;
    return (
      <div className={"cell " + n.state + (dense ? "" : " cozy-cell")} onClick={() => onOpenNode(n)} title={`${n.hostFqdn} — ${n.state}`}>
        <div className="cell__top">
          <span className="cell__name">{n.name}</span>
          <span className="cell__dot" style={{ background: STATE_COLOR[n.state], boxShadow: n.state !== "unknown" ? `0 0 6px ${STATE_COLOR[n.state]}` : "none" }} />
        </div>
        <div className="cell__meta">{n.role === "client" ? n.ip : n.role.toUpperCase()}</div>
        {n.alertsCount > 0 && <span className="cell__badge">{n.alertsCount}</span>}
        <div className="cell__spark"><Sparkline data={n.hist.cpu} color={STATE_COLOR[n.state]} h={dense ? 20 : 26} fill={true} strokeW={1.5} /></div>
        <div className="cell__load"><i style={{ width: load + "%", background: metricColor(load), boxShadow: `0 0 5px ${metricColor(load)}` }} /></div>
      </div>
    );
  }

  function HeatView({ nodes, dense, onOpenNode }) {
    // One block (network segment, or a functional pool for agent-less systems).
    const block = (key, title, sub, segNodes) => {
      if (!segNodes.length) return null;
      const roll = { up: 0, degraded: 0, down: 0, unknown: 0 };
      segNodes.forEach((n) => roll[n.state]++);
      return (
        <div className="segment-block" key={key}>
          <div className="segment-head">
            <h3>{title}</h3>
            <span className="cidr">{sub}</span>
            <span className="rule" />
            <div className="roll">
              {roll.down > 0 && <span className="roll-pip"><span className="dot down" />{roll.down}</span>}
              {roll.degraded > 0 && <span className="roll-pip"><span className="dot degraded" />{roll.degraded}</span>}
              <span className="roll-pip"><span className="dot up" />{roll.up}</span>
            </div>
          </div>
          <div className={"heat" + (dense ? "" : " cozy")}>
            {segNodes.map((n) => <Cell key={n.nodeId} n={n} dense={dense} onOpenNode={onOpenNode} />)}
          </div>
        </div>
      );
    };
    // Adopted systems carry no network segment; group them by pool so they still
    // appear in the heatmap (not just the table).
    const knownSeg = {}; S.segments.forEach((s) => { knownSeg[s.id] = true; });
    const pools = {};
    nodes.forEach((n) => { if (!knownSeg[n.segId]) { const k = n.segName || "Other"; (pools[k] = pools[k] || []).push(n); } });
    return (
      <div>
        {S.segments.map((seg) => block(seg.id, seg.name, seg.cidr, nodes.filter((n) => n.segId === seg.id)))}
        {Object.keys(pools).map((k) => block("pool:" + k, k, "systems", pools[k]))}
      </div>
    );
  }

  function TableView({ nodes, sort, setSort, onOpenNode }) {
    const sorted = useMemo(() => {
      const arr = [...nodes];
      const k = sort.key;
      arr.sort((a, b) => {
        let va, vb;
        if (k === "state") { const ord = { down: 0, degraded: 1, unknown: 2, up: 3 }; va = ord[a.state]; vb = ord[b.state]; }
        else if (k === "name") { va = a.name; vb = b.name; }
        else if (k === "seg") { va = a.segName; vb = b.segName; }
        else if (k === "role") { va = a.role; vb = b.role; }
        else va = a[k], vb = b[k];
        if (va < vb) return -1 * sort.dir; if (va > vb) return 1 * sort.dir; return 0;
      });
      return arr;
    }, [nodes, sort]);
    function Th({ k, children, num }) {
      const on = sort.key === k;
      return <th onClick={() => setSort((s) => ({ key: k, dir: s.key === k ? -s.dir : 1 }))} style={{ textAlign: num ? "right" : "left" }}>
        {children}{on && <span className="arr">{sort.dir === 1 ? "▲" : "▼"}</span>}
      </th>;
    }
    return (
      <div className="tablewrap">
        <table className="grid">
          <thead><tr>
            <Th k="state">St</Th><Th k="name">Host</Th><Th k="role">Role</Th><Th k="seg">Segment</Th>
            <Th k="cpuPct" num>CPU</Th><Th k="ramPct" num>RAM</Th><Th k="diskMaxPct" num>Disk</Th>
            <Th k="netTotalMbps" num>Net</Th><Th k="lastSeenMin" num>Seen</Th><Th k="alertsCount" num>Alerts</Th>
          </tr></thead>
          <tbody>
            {sorted.map((n) => (
              <tr key={n.nodeId} onClick={() => onOpenNode(n)}>
                <td><StatusDot state={n.state} /></td>
                <td><div className="td-host"><Icon name={ROLE_ICON[n.role]} size={15} className="ico" />{n.name}<span className="td-mono" style={{ fontSize: 10, color: "var(--ink-faint)" }}>.akoria.net</span></div></td>
                <td><span className="tag">{n.role}</span></td>
                <td className="td-mono">{n.segName} <span style={{ color: "var(--ink-faint)" }}>{n.ip}</span></td>
                <td style={{ textAlign: "right" }}><Bar pct={n.cpuPct} /></td>
                <td style={{ textAlign: "right" }}><Bar pct={n.ramPct} /></td>
                <td style={{ textAlign: "right" }}><Bar pct={n.diskMaxPct} /></td>
                <td className="td-mono" style={{ textAlign: "right" }}>{n.state === "down" ? "—" : fmt.mbps(n.netTotalMbps)}</td>
                <td className="td-mono" style={{ textAlign: "right", color: n.lastSeenMin > 2 ? "var(--crit)" : "var(--ink-dim)" }}>{fmt.ago(n.lastSeenMin)}</td>
                <td style={{ textAlign: "right" }}>{n.alertsCount > 0 ? <span className="cell__badge" style={{ position: "static", display: "inline-flex" }}>{n.alertsCount}</span> : <span className="td-mono" style={{ color: "var(--ink-faint)" }}>0</span>}</td>
              </tr>
            ))}
          </tbody>
        </table>
      </div>
    );
  }
  function Bar({ pct }) {
    const c = metricColor(pct);
    return <span style={{ display: "inline-flex", alignItems: "center", gap: 8, justifyContent: "flex-end" }}>
      <span className="td-mono" style={{ width: 34, textAlign: "right", color: c }}>{pct}%</span>
      <span className="metricbar"><i style={{ width: pct + "%", background: c }} /></span>
    </span>;
  }

  function CardsView({ nodes, onOpenNode }) {
    const card = (key, title, sub, desc, segNodes) => {
      if (!segNodes.length) return null;
      const roll = { total: segNodes.length, up: 0, degraded: 0, down: 0, unknown: 0 };
      segNodes.forEach((n) => roll[n.state]++);
      const metricNodes = segNodes.filter((n) => !n.isAsset);  // agent-less systems have no CPU/RAM
      const avgCpu = metricNodes.length ? Math.round(metricNodes.reduce((a, n) => a + n.cpuPct, 0) / metricNodes.length) : 0;
      const avgRam = metricNodes.length ? Math.round(metricNodes.reduce((a, n) => a + n.ramPct, 0) / metricNodes.length) : 0;
      return (
        <div className="scard" key={key}>
          <div className="scard__head">
            <Icon name="arch" size={16} style={{ color: "var(--teal)" }} />
            <span className="scard__title">{title}</span>
            <span className="scard__cidr" style={{ marginLeft: "auto" }}>{sub}</span>
          </div>
          <div className="scard__body">
            <div className="donut-row">
              <HealthDonut roll={roll} />
              <div style={{ flex: 1 }}>
                <div className="scard__stats">
                  <div className="scard__stat"><div className="k">Up</div><div className="v" style={{ color: "var(--ok)" }}>{roll.up}</div></div>
                  <div className="scard__stat"><div className="k">Issues</div><div className="v" style={{ color: roll.down ? "var(--crit)" : "var(--warn)" }}>{roll.down + roll.degraded}</div></div>
                  <div className="scard__stat"><div className="k">Avg CPU</div><div className="v" style={{ color: metricColor(avgCpu) }}>{metricNodes.length ? avgCpu + "%" : "—"}</div></div>
                  <div className="scard__stat"><div className="k">Avg RAM</div><div className="v" style={{ color: metricColor(avgRam) }}>{metricNodes.length ? avgRam + "%" : "—"}</div></div>
                </div>
              </div>
            </div>
            <div className="minigrid">
              {segNodes.map((n) => (
                <div key={n.nodeId} className="minicell" title={`${n.name} — ${n.state}`} onClick={() => onOpenNode(n)}
                  style={{ background: STATE_COLOR[n.state], boxShadow: n.state === "down" ? "0 0 7px var(--crit)" : "none", opacity: n.state === "unknown" ? 0.5 : 1 }} />
              ))}
            </div>
            {desc && <div style={{ fontFamily: "var(--mono)", fontSize: 10.5, color: "var(--ink-faint)", marginTop: 10 }}>{desc}</div>}
          </div>
        </div>
      );
    };
    const knownSeg = {}; S.segments.forEach((s) => { knownSeg[s.id] = true; });
    const pools = {};
    nodes.forEach((n) => { if (!knownSeg[n.segId]) { const k = n.segName || "Other"; (pools[k] = pools[k] || []).push(n); } });
    return (
      <div className="cards">
        {S.segments.map((seg) => card(seg.id, seg.name, seg.cidr, seg.desc, nodes.filter((n) => n.segId === seg.id)))}
        {Object.keys(pools).map((k) => card("pool:" + k, k, "systems", "monitored systems", pools[k]))}
      </div>
    );
  }

  /* ===================== NODE DETAIL ===================== */
  function NodeDetail({ node, onBack, onSurvey }) {
    const [metric, setMetric] = useState("cpu");
    const [detail, setDetail] = useState(null);
    const [history, setHistory] = useState(null);
    // Fetch the full node detail (real current CPU cores / RAM / disks / ifaces /
    // procs from hostCurrent) and refresh on the poll interval. The list entry
    // only carries identity + state, so without this the gauges read zero.
    useEffect(function () {
      const api = S.api;
      if (!api || !api.node) return undefined;
      let live = true;
      const load = function () {
        api.node(node.nodeId).then(function (d) {
          if (!live) return;
          setDetail(d);
          if (!api.nodeHistory) return;
          Promise.all([
            api.nodeHistory(node.nodeId, "cpu"),
            api.nodeHistory(node.nodeId, "ram", { totalKb: d.ramTotalKb }),
            api.nodeHistory(node.nodeId, "disk"),
            api.nodeHistory(node.nodeId, "net"),
          ]).then(function (h) {
            if (live) setHistory({ cpu: h[0], ram: h[1], disk: h[2], net: h[3] });
          }).catch(function () {});
        }).catch(function () {});
      };
      load();
      const iv = setInterval(load, 10000);
      return function () { live = false; clearInterval(iv); };
    }, [node.nodeId]);
    // Prefer fetched detail; fall back to the live list entry, then the passed node.
    const n = detail || (S.nodes || S.fleet || []).find((x) => x.nodeId === node.nodeId) || node;
    const hist = history ? Object.assign({}, n.hist || {}, history) : (n.hist || { cpu: [], ram: [], net: [], disk: [] });
    const nodeAlerts = S.alerts.filter((a) => a.nodeId === n.nodeId && !a.cleared);
    const nodeProbes = S.probes.filter((p) => p.hostNode === n.nodeId);
    const metricMap = {
      cpu: { data: hist.cpu, color: "var(--teal)", label: "CPU %", max: 100 },
      ram: { data: hist.ram, color: "var(--violet)", label: "RAM %", max: 100 },
      net: { data: hist.net, color: "var(--ok)", label: "Net Mb/s", max: 100 },
      disk: { data: hist.disk, color: "var(--warn)", label: "Disk %", max: 100 },
    };
    const cur = metricMap[metric];

    return (
      <div className="page">
        <div style={{ display: "flex", alignItems: "center", gap: 12, marginBottom: 18, flexWrap: "wrap" }}>
          <button className="backbtn" onClick={onBack}><Icon name="chevronLeft" size={16} />Fleet</button>
          <span className="td-mono" style={{ color: "var(--ink-faint)", fontSize: 12 }}>/ {n.segName} / {n.hostFqdn}</span>
          <div style={{ marginLeft: "auto", display: "flex", gap: 9 }}>
            <button className="backbtn" onClick={() => onSurvey(n)}><Icon name="survey" size={15} />Survey now</button>
            <button className="backbtn" onClick={() => onSurvey(n, "config")}><Icon name="settings" size={15} />Push config</button>
          </div>
        </div>

        <div className="node-hero" style={{ marginBottom: 20 }}>
          <div className="node-hero__id">
            <div className="node-hero__mark"><Icon name={ROLE_ICON[n.role]} size={26} /></div>
            <div>
              <h1>{n.name}<span style={{ color: "var(--ink-faint)", fontWeight: 400 }}>.akoria.net</span></h1>
              <div className="meta">
                <span><StatusDot state={n.state} /> <span className={"statetext " + n.state} style={{ fontWeight: 600 }}>{window.STATE_LABEL[n.state]}</span></span>
                <span>{n.ip}</span><span>{n.role}{n.label ? ` · ${n.label}` : ""}</span>
                <span>{n.osName} · {n.arch}</span>
                <span>up {n.uptimeDays}d</span>
                <span style={{ color: n.lastSeenMin > 2 ? "var(--crit)" : "inherit" }}>seen {fmt.ago(n.lastSeenMin)}</span>
                <span>cfg epoch {n.configEpoch} {n.converged ? <span style={{ color: "var(--ok)" }}>✓</span> : <span style={{ color: "var(--warn)" }}>drift</span>}</span>
              </div>
            </div>
          </div>
        </div>

        {/* gauges */}
        <div className="panel" style={{ marginBottom: 16 }}>
          <div className="panel__body">
            <div className="gauge-grid">
              <RadialGauge value={n.state === "down" ? 0 : n.cpuPct} label="CPU LOAD" sub={`${n.cores.length} cores`} />
              <RadialGauge value={n.ramPct} label="MEMORY" sub={fmt.kb(n.ramTotalKb)} color="var(--violet)" />
              <RadialGauge value={n.diskMaxPct} label="DISK PEAK" sub={`${n.disks.length} vols`} />
              <RadialGauge value={n.swapPct} label="SWAP" sub={fmt.kb(n.swapTotalKb)} />
              <div className="gauge" style={{ width: 116 }}>
                <div style={{ height: 116, display: "grid", placeItems: "center" }}>
                  <div style={{ textAlign: "center" }}>
                    <Icon name="network" size={26} style={{ color: "var(--teal)" }} />
                    <div style={{ fontFamily: "var(--mono)", fontWeight: 600, fontSize: 18, marginTop: 6 }}>{n.state === "down" ? "—" : fmt.mbps(n.netTotalMbps)}</div>
                  </div>
                </div>
                <div className="lab">THROUGHPUT</div>
              </div>
            </div>
          </div>
        </div>

        <div className="two-col" style={{ marginBottom: 16 }}>
          {/* time series */}
          <div className="panel">
            <div className="panel__head">
              <Icon name="activity" size={16} />
              <h3>Trend — last 15 min</h3>
              <div className="right">
                <div className="seg" style={{ transform: "scale(.92)", transformOrigin: "right" }}>
                  {["cpu", "ram", "net", "disk"].map((m) => (
                    <button key={m} className={metric === m ? "on" : ""} onClick={() => setMetric(m)}>{m}</button>
                  ))}
                </div>
              </div>
            </div>
            <div className="panel__body">
              <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 8, fontFamily: "var(--mono)", fontSize: 12 }}>
                <span className="muted" style={{ textTransform: "uppercase", letterSpacing: ".08em", fontSize: 10 }}>{cur.label}</span>
                <span style={{ color: cur.color, fontWeight: 600, fontSize: 16 }}>{(cur.data && cur.data.length) ? cur.data[cur.data.length - 1].toFixed(0) + (metric === "net" ? "" : "%") : "—"}</span>
              </div>
              {(cur.data && cur.data.length)
                ? <TimeSeries data={cur.data} color={cur.color} max={cur.max} h={170} />
                : <div className="muted" style={{ height: 170, display: "flex", alignItems: "center", justifyContent: "center", fontSize: 12 }}>no history yet — collecting…</div>}
            </div>
          </div>

          {/* interfaces / bandwidth */}
          <div className="panel">
            <div className="panel__head"><Icon name="bandwidth" size={16} /><h3>Interfaces</h3><div className="right">{n.ifaces.length} NIC</div></div>
            <div className="panel__body" style={{ display: "flex", flexDirection: "column", gap: 16 }}>
              {n.ifaces.map((f, i) => (
                <div key={i}>
                  <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 8 }}>
                    <span className="td-mono" style={{ fontWeight: 600 }}>{f.name} <span className="tag">{fmt.mbps(f.capMbps)}</span></span>
                    {f.errs > 10 && <span className="td-mono" style={{ color: "var(--warn)" }}>{f.errs} err/s</span>}
                  </div>
                  <div style={{ display: "flex", flexDirection: "column", gap: 8 }}>
                    <BandwidthGauge label="RX" used={f.rxMbps} cap={f.capMbps} unit={fmt.mbps} />
                    <BandwidthGauge label="TX" used={f.txMbps} cap={f.capMbps} unit={fmt.mbps} color="var(--violet)" />
                  </div>
                </div>
              ))}
            </div>
          </div>
        </div>

        {/* per-core + memory + disks */}
        <div className="two-col" style={{ marginBottom: 16 }}>
          <div className="panel">
            <div className="panel__head"><Icon name="cpu" size={16} /><h3>Per-core utilisation</h3><div className="right">{n.cores.length} × {n.arch}</div></div>
            <div className="panel__body">
              <div className="cores">
                {n.cores.map((c, i) => (
                  <div className="core" key={i}>
                    <div className="n">c{i}</div>
                    <div className="bar"><i style={{ height: c + "%", background: metricColor(c), boxShadow: `0 0 5px ${metricColor(c)}` }} /></div>
                    <div className="pct" style={{ color: metricColor(c) }}>{c}</div>
                  </div>
                ))}
              </div>
            </div>
          </div>
          <div className="panel">
            <div className="panel__head"><Icon name="disk" size={16} /><h3>Storage</h3></div>
            <div className="panel__body">
              {n.disks.map((d, i) => (
                <div className="metric-row" key={i}>
                  <span className="lbl">{d.mount}</span>
                  <span className="track"><i style={{ width: d.usedPct + "%", background: metricColor(d.usedPct) }} /></span>
                  <span className="val" style={{ color: metricColor(d.usedPct) }}>{d.usedPct}% <span className="muted" style={{ fontSize: 11 }}>{d.totalGb}G {d.fs}</span></span>
                </div>
              ))}
              <div className="metric-row"><span className="lbl">Memory</span><span className="track"><i style={{ width: n.ramPct + "%", background: "var(--violet)" }} /></span><span className="val" style={{ color: "var(--violet)" }}>{fmt.kb(n.ramUsedKb)}</span></div>
              <div className="metric-row"><span className="lbl">Swap</span><span className="track"><i style={{ width: n.swapPct + "%", background: metricColor(n.swapPct) }} /></span><span className="val">{n.swapPct}%</span></div>
            </div>
          </div>
        </div>

        {/* processes + probes/alerts */}
        <div className="two-col" style={{ marginBottom: 16 }}>
          <div className="panel">
            <div className="panel__head"><Icon name="process" size={16} /><h3>Watched processes</h3><div className="right">{n.procs.length}</div></div>
            <div className="panel__body" style={{ padding: 0 }}>
              <table className="proc-table">
                <thead><tr><th>Process</th><th>PID</th><th>St</th><th>Files</th><th>Sock</th><th>RSS</th></tr></thead>
                <tbody>
                  {n.procs.map((p, i) => (
                    <tr key={i}>
                      <td style={{ fontWeight: 600 }}>{p.name}</td>
                      <td className="muted">{p.runState === "Z" ? "—" : p.pid}</td>
                      <td><span style={{ color: p.runState === "R" ? "var(--ok)" : p.runState === "Z" ? "var(--crit)" : "var(--warn)" }}>{p.runState}</span></td>
                      <td className="muted">{p.runState === "Z" ? "—" : p.nFiles}</td>
                      <td className="muted">{p.runState === "Z" ? "—" : p.nSockets}</td>
                      <td className="muted">{p.runState === "Z" ? "—" : fmt.kb(p.rssKb)}</td>
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          </div>
          <div className="panel">
            <div className="panel__head"><Icon name="reachability" size={16} /><h3>Reachability & alerts</h3></div>
            <div className="panel__body">
              {nodeProbes.length > 0 ? nodeProbes.map((p) => (
                <div key={p.targetId} style={{ marginBottom: 16 }}>
                  <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 8 }}>
                    <span className="td-mono" style={{ fontWeight: 600 }}><StatusDot state={p.state} size={8} /> {p.targetId}</span>
                    <span className="tag">{p.label}</span>
                  </div>
                  <RTTBars vantages={p.vantages} fmt={fmt} />
                </div>
              )) : <div className="td-mono muted" style={{ fontSize: 12, marginBottom: 14 }}>No probe targets assigned to this host.</div>}
              <div className="divider" />
              {nodeAlerts.length ? nodeAlerts.map((a) => (
                <div key={a.eventId} className={"alert-row " + a.severity} style={{ marginBottom: 8 }}>
                  <span className={"alert-sev " + a.severity}>{a.severity}</span>
                  <div className="alert-main"><div className="t">{a.ruleName}</div><div className="d">{a.detail}</div></div>
                  <div className="alert-meta">{fmt.ago(a.firedMinAgo)}</div>
                </div>
              )) : <div className="td-mono" style={{ color: "var(--ok)", fontSize: 12 }}><Icon name="check" size={14} style={{ verticalAlign: "-2px" }} /> No active alerts on this node.</div>}
            </div>
          </div>
        </div>
      </div>
    );
  }

  /* ===================== ALERTS + RULES ===================== */
  function AlertsScreen({ onOpenNode, rules, setRules, toast }) {
    const [tab, setTab] = useState("active");
    const list = S.alerts.filter((a) => tab === "all" ? true : !a.cleared);
    const crit = S.alerts.filter((a) => !a.cleared && a.severity === "crit").length;
    const warn = S.alerts.filter((a) => !a.cleared && a.severity === "warn").length;
    const info = S.alerts.filter((a) => !a.cleared && a.severity === "info").length;

    const api = () => (window.SOLARI && window.SOLARI.api) || null;
    // edit threshold locally; persist on commit (POST /api/rules/{id})
    function setThreshold(id, val) { setRules((rs) => rs.map((r) => r.ruleId === id ? { ...r, threshold: val } : r)); }
    function commitThreshold(id, val) {
      const a = api();
      if (a && a.saveRule) a.saveRule(id, { threshold: val }).catch((e) => toast(`Rule save failed: ${e && e.message || "error"}`, "close"));
    }
    // toggle armed/disarmed (POST /api/rules/{id} {enabled})
    function toggleRule(id) {
      const r = rules.find((x) => x.ruleId === id);
      const next = !(r && r.enabled);
      setRules((rs) => rs.map((x) => x.ruleId === id ? { ...x, enabled: next } : x));   // optimistic
      const label = (r && (r.name || r.metric)) || ("rule " + id);
      const a = api();
      if (a && a.toggleRule) {
        a.toggleRule(id, next).then(() => toast(`Rule "${label}" ${next ? "enabled" : "disabled"}`, next ? "check" : "close"))
          .catch((e) => { setRules((rs) => rs.map((x) => x.ruleId === id ? { ...x, enabled: !next } : x)); toast(`Toggle failed: ${e && e.message || "error"}`, "close"); });
      } else {
        toast(`Rule "${label}" ${next ? "enabled" : "disabled"}`, next ? "check" : "close");
      }
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Alerts & Tolerances</h1><div className="page-sub">{crit + warn + info} active · {rules.filter((r) => r.enabled).length}/{rules.length} rules armed</div></div>
          <div className="page-head__right">
            <div className="seg">
              <button className={tab === "active" ? "on" : ""} onClick={() => setTab("active")}>Active</button>
              <button className={tab === "all" ? "on" : ""} onClick={() => setTab("all")}>All</button>
            </div>
          </div>
        </div>

        <div className="kpis" style={{ gridTemplateColumns: "repeat(auto-fit,minmax(150px,1fr))" }}>
          <div className="kpi crit"><div className="kpi__k">Critical</div><div className="kpi__v">{crit}</div><div className="kpi__sub">need attention</div><div className="kpi__bar" /></div>
          <div className="kpi warn"><div className="kpi__k">Warning</div><div className="kpi__v">{warn}</div><div className="kpi__sub">over tolerance</div><div className="kpi__bar" /></div>
          <div className="kpi teal"><div className="kpi__k">Info</div><div className="kpi__v">{info}</div><div className="kpi__sub">advisory</div><div className="kpi__bar" /></div>
        </div>

        <div className="two-col" style={{ alignItems: "start" }}>
          <div>
            <div className="page-sub" style={{ marginBottom: 12 }}>{tab === "active" ? "Active events" : "All events (incl. cleared)"}</div>
            {list.length === 0 && <div className="empty">No alerts — all systems nominal.</div>}
            {list.map((a) => (
              <div key={a.eventId} className={"alert-row " + a.severity + (a.cleared ? " cleared" : "")} onClick={() => a.nodeId && onOpenNode(a.nodeId)}>
                <span className={"alert-sev " + a.severity}>{a.severity}</span>
                <div className="alert-main">
                  <div className="t">{a.ruleName}</div>
                  <div className="d">{a.detail}</div>
                </div>
                {a.node && <div className="alert-node">{a.node}<div style={{ color: "var(--ink-faint)", fontSize: 10 }}>{a.segName}</div></div>}
                <div className="alert-meta">{a.cleared ? "cleared" : ""}<div>{fmt.ago(a.firedMinAgo)}</div></div>
              </div>
            ))}
          </div>

          <div className="panel">
            <div className="panel__head"><Icon name="settings" size={16} /><h3>Alert rules & tolerances</h3></div>
            <div className="panel__body" style={{ padding: 0 }}>
              {rules.map((r) => (
                <div key={r.ruleId} style={{ padding: "14px 16px", borderBottom: "1px solid var(--line)", opacity: r.enabled ? 1 : 0.55 }}>
                  <div style={{ display: "flex", alignItems: "center", gap: 12 }}>
                    <span className={"alert-sev " + r.severity}>{r.severity}</span>
                    <div style={{ flex: 1 }}>
                      <div style={{ fontWeight: 600, fontSize: 13 }}>{r.name}</div>
                      <div className="rule-cond">{r.metric} <b>{({ gt: ">", lt: "<", eq: "=", transition: "Δ" })[r.op]}</b> {r.op !== "transition" ? `${r.threshold}${r.unit}` : "change"}{r.forSeconds ? ` for ${r.forSeconds}s` : ""}</div>
                    </div>
                    <span className="tag">{r.scope}</span>
                    <button className={"switch" + (r.enabled ? " on" : "")} onClick={() => toggleRule(r.ruleId)} aria-label="toggle rule"><i /></button>
                  </div>
                  {r.op !== "transition" && r.enabled && (
                    <div className="thr" style={{ marginTop: 12 }}>
                      <input type="range" min={r.metric === "rttMicros" ? 1000 : 0} max={r.metric === "rttMicros" ? 60000 : r.unit === "s" ? 300 : 100}
                        step={r.metric === "rttMicros" ? 1000 : 1} value={r.threshold} onChange={(e) => setThreshold(r.ruleId, +e.target.value)} onMouseUp={(e) => commitThreshold(r.ruleId, +e.target.value)} onTouchEnd={(e) => commitThreshold(r.ruleId, +e.target.value)} />
                      <span className="num">{r.threshold}{r.unit}</span>
                    </div>
                  )}
                </div>
              ))}
            </div>
          </div>
        </div>
      </div>
    );
  }

  Object.assign(window, { FleetOverview, NodeDetail, AlertsScreen });
})();
