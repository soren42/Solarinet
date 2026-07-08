/* ============================================================
   SolariNet — screens: FleetOverview, NodeDetail, AlertsScreen
   ============================================================ */
(function () {
  const { useState, useMemo, useEffect } = React;
  const Icon = window.Icon;
  const { StatusDot, Sparkline, TimeSeries, BandwidthGauge, RadialGauge, HealthDonut, RTTBars, metricColor, DangerModal, MaintenanceBadge, maintenanceFor } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;

  const ROLE_ICON = { server: "server", monitor: "monitor", client: "host" };
  const STATE_COLOR = { up: "var(--ok)", degraded: "var(--warn)", down: "var(--crit)", unknown: "var(--unknown)", retired: "var(--unknown)" };
  const STATE_ORD = { down: 0, degraded: 1, unknown: 2, up: 3, retired: 5 };

  // Staleness: a node is stale when it has been silent for more than 2× the
  // configured sample interval (floor 2 min so the UI never flaps on jitter).
  function staleThresholdMin() {
    const sec = (S.config && S.config.schedule && S.config.schedule.sampleIntervalSec) || 15;
    return Math.max(2, Math.ceil((2 * sec) / 60));
  }
  function isStale(n) {
    return !n.isAsset && n.state !== "down" && n.state !== "retired" && n.lastSeenMin != null && n.lastSeenMin > staleThresholdMin();
  }

  /* ===================== ATTENTION PANEL =====================
     The operator's first read: everything that currently needs a human,
     ranked crit → warn → info, each row a click-through to the owning
     screen/item. Aggregates nodes, alerts, probes, drift, staleness and
     pending enrollments without duplicating the same incident twice
     (a down node's row absorbs its own alerts). */
  function buildAttention(fleet, { onOpenNode, onNav }) {
    const items = [];
    const active = S.alerts.filter((a) => !a.cleared);
    const alertsByNode = {};
    active.forEach((a) => { if (a.nodeId != null) (alertsByNode[a.nodeId] = alertsByNode[a.nodeId] || []).push(a); });
    const listed = {};  // nodeId -> true once a node already owns a row

    const nodeOf = (nid) => fleet.find((n) => String(n.nodeId) === String(nid)) || null;
    const openNodeOrAlerts = (nid) => { const n = nodeOf(nid); if (n) onOpenNode(n); else onNav("alerts"); };
    const alertNote = (nodeId) => {
      const as = alertsByNode[nodeId] || [];
      if (!as.length) return null;
      const c = as.filter((a) => a.severity === "crit").length;
      const w = as.filter((a) => a.severity === "warn").length;
      const bits = [];
      if (c) bits.push(c + " crit");
      if (w) bits.push(w + " warn");
      return bits.length ? bits.join(" · ") + (as.length === 1 ? " alert" : " alerts") : null;
    };

    // 1 — down systems (crit)
    fleet.filter((n) => n.state === "down").forEach((n) => {
      listed[n.nodeId] = true;
      const seen = n.lastSeenMin != null ? "last seen " + fmt.ago(n.lastSeenMin) : "reachability lost";
      items.push({
        key: "down-" + n.nodeId, sev: "crit", icon: n.isAsset ? "host" : (ROLE_ICON[n.role] || "host"),
        text: `${n.name} is down`,
        sub: [n.segName, seen, alertNote(n.nodeId)].filter(Boolean).join(" · "),
        onClick: () => onOpenNode(n),
      });
    });

    // 2 — probes unreachable from every vantage (crit)
    S.probes.filter((p) => p.state === "down").forEach((p) => {
      items.push({
        key: "probe-down-" + p.targetId, sev: "crit", icon: "reachability",
        text: p.vantages.length > 1
          ? `${p.targetId} unreachable from all ${p.vantages.length} vantages`
          : `${p.targetId} is unreachable`,
        sub: [p.label, p.host].filter(Boolean).join(" · "),
        onClick: () => onNav("reachability"),
      });
    });

    // 3 — critical alerts on systems that are not already listed as down (crit)
    Object.keys(alertsByNode).forEach((nid) => {
      if (listed[nid]) return;
      const crits = alertsByNode[nid].filter((a) => a.severity === "crit");
      if (!crits.length) return;
      const first = crits[0];
      items.push({
        key: "crit-" + nid, sev: "crit", icon: "alerts",
        text: crits.length === 1 ? `${first.ruleName} — ${first.node}` : `${crits.length} critical alerts — ${first.node}`,
        sub: first.detail,
        onClick: () => openNodeOrAlerts(nid),
      });
    });
    // nodeless critical alerts (fleet / probe scope) → Alerts screen
    active.filter((a) => a.severity === "crit" && a.nodeId == null).forEach((a) => {
      items.push({ key: "crit-ev-" + a.eventId, sev: "crit", icon: "alerts", text: a.ruleName, sub: a.detail, onClick: () => onNav("alerts") });
    });

    // 4 — degraded systems (warn); their own warn alerts fold into the sub-line
    fleet.filter((n) => n.state === "degraded").forEach((n) => {
      listed[n.nodeId] = true;
      items.push({
        key: "deg-" + n.nodeId, sev: "warn", icon: n.isAsset ? "host" : (ROLE_ICON[n.role] || "host"),
        text: `${n.name} is degraded`,
        sub: [n.segName, alertNote(n.nodeId) || "over tolerance"].filter(Boolean).join(" · "),
        onClick: () => onOpenNode(n),
      });
    });

    // 5 — split-vantage probes (warn)
    S.probes.filter((p) => p.state === "degraded").forEach((p) => {
      const bad = p.vantages.filter((v) => v.outcome !== "ok").length;
      items.push({
        key: "probe-split-" + p.targetId, sev: "warn", icon: "reachability",
        text: `${p.targetId} split vantage — failing from ${bad}/${p.vantages.length}`,
        sub: [p.label, p.host].filter(Boolean).join(" · "),
        onClick: () => onNav("reachability"),
      });
    });

    // 6 — warn-level alerts on otherwise-healthy systems (warn)
    Object.keys(alertsByNode).forEach((nid) => {
      if (listed[nid]) return;
      const warns = alertsByNode[nid].filter((a) => a.severity === "warn");
      if (!warns.length) return;
      const first = warns[0];
      listed[nid] = true;
      items.push({
        key: "warn-" + nid, sev: "warn", icon: "alerts",
        text: warns.length === 1 ? `${first.ruleName} — ${first.node}` : `${warns.length} warnings — ${first.node}`,
        sub: first.detail,
        onClick: () => openNodeOrAlerts(nid),
      });
    });

    // 7 — stale (silent) nodes (warn)
    fleet.filter((n) => n.state !== "down" && n.state !== "degraded" && isStale(n)).forEach((n) => {
      items.push({
        key: "stale-" + n.nodeId, sev: "warn", icon: "clock",
        text: `${n.name} silent for ${fmt.ago(n.lastSeenMin).replace(" ago", "")}`,
        sub: `${n.segName} · expected every ${staleThresholdMin() / 2}m`,
        onClick: () => onOpenNode(n),
      });
    });

    // 8 — config drift (warn, aggregated)
    const drifted = S.nodes.filter((n) => !n.converged);
    if (drifted.length) {
      items.push({
        key: "drift", sev: "warn", icon: "refresh",
        text: drifted.length === 1 ? `${drifted[0].name} has config drift` : `${drifted.length} nodes have config drift`,
        sub: drifted.slice(0, 3).map((n) => n.name).join(", ") + (drifted.length > 3 ? ` +${drifted.length - 3}` : "") + " · re-push from Provisioning",
        onClick: () => onNav("provision"),
      });
    }

    // 9 — pending enrollments (info)
    const pend = (S.enrollments || []).length;
    if (pend) {
      items.push({
        key: "enroll", sev: "info", icon: "shield",
        text: pend === 1 ? "1 CSR awaiting approval" : `${pend} CSRs awaiting approval`,
        sub: (S.enrollments || []).slice(0, 3).map((e) => e.host).join(", "),
        onClick: () => onNav("provision"),
      });
    }

    return items;
  }

  function AttentionPanel({ fleet, onOpenNode, onNav }) {
    const [expanded, setExpanded] = useState(false);
    const items = useMemo(() => buildAttention(fleet, { onOpenNode, onNav }),
      [fleet, S.alerts, S.probes, S.enrollments]);

    if (!items.length) {
      return (
        <div className="attn nominal">
          <span className="dot up glow" style={{ width: 9, height: 9 }} />
          <span className="attn__ok">All systems nominal</span>
          <span className="attn__oksub">{fleet.length} systems · {S.probes.length} probes · {S.rules.filter((r) => r.enabled).length} rules armed — nothing needs attention</span>
        </div>
      );
    }

    const crit = items.filter((i) => i.sev === "crit").length;
    const warn = items.filter((i) => i.sev === "warn").length;
    const CAP = 7;
    const shown = expanded ? items : items.slice(0, CAP);
    const hidden = items.length - shown.length;

    return (
      <div className="attn panel">
        <div className="attn__head">
          <Icon name="pulse" size={16} style={{ color: crit ? "var(--crit)" : "var(--warn)" }} />
          <h3>Needs attention</h3>
          <div className="attn__counts">
            {crit > 0 && <span className="attn__pill crit">{crit} crit</span>}
            {warn > 0 && <span className="attn__pill warn">{warn} warn</span>}
            {items.length - crit - warn > 0 && <span className="attn__pill info">{items.length - crit - warn} info</span>}
          </div>
        </div>
        <div className="attn__list">
          {shown.map((it) => (
            <div key={it.key} className={"attn__row " + it.sev} onClick={it.onClick} role="button" tabIndex={0}
              onKeyDown={(e) => { if (e.key === "Enter") it.onClick(); }}>
              <span className={"dot glow " + (it.sev === "crit" ? "down" : it.sev === "warn" ? "degraded" : "up")} style={{ width: 8, height: 8 }} />
              <Icon name={it.icon} size={16} className="attn__ico" />
              <span className="attn__text">{it.text}</span>
              <span className="attn__sub">{it.sub}</span>
              <Icon name="chevronRight" size={14} className="attn__go" />
            </div>
          ))}
        </div>
        {(hidden > 0 || expanded) && (
          <button className="attn__more" onClick={() => setExpanded((v) => !v)}>
            {expanded ? "Show fewer" : `+${hidden} more issue${hidden === 1 ? "" : "s"}`}
          </button>
        )}
      </div>
    );
  }

  /* ===================== FLEET OVERVIEW ===================== */
  function FleetOverview({ onOpenNode, onNav, view, setView, fleet }) {
    const [stateFilter, setStateFilter] = useState("all");
    const [roleFilter, setRoleFilter] = useState("all");
    const [showRetired, setShowRetired] = useState(false);   // retired nodes hidden by default
    const [dense, setDense] = useState(true);
    // problem-first default: worst state floats to the top until re-sorted
    const [sort, setSort] = useState({ key: "state", dir: 1 });

    const retiredCount = useMemo(() => fleet.filter((n) => n.state === "retired").length, [fleet]);
    const filtered = useMemo(() => fleet.filter((n) =>
      (showRetired || n.state !== "retired") &&
      (stateFilter === "all" || n.state === stateFilter) &&
      (roleFilter === "all" || n.role === roleFilter)
    ), [fleet, stateFilter, roleFilter, showRetired]);

    const roll = S.fleetRoll;
    const active = Math.max(1, roll.total - (roll.retired || 0));   // KPI denominators exclude retired
    const cpuNodes = fleet.filter((n) => !n.isAsset && n.state !== "down" && n.state !== "retired");
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

        {/* what needs attention — ranked, click-through */}
        <AttentionPanel fleet={fleet} onOpenNode={onOpenNode} onNav={onNav || (() => {})} />

        {/* KPIs — the state tiles double as filters */}
        <div className="kpis">
          <div className={"kpi teal clickable" + (stateFilter === "all" ? " on" : "")} role="button" tabIndex={0} onClick={() => setStateFilter("all")}><div className="kpi__k">Systems</div><div className="kpi__v">{roll.total - (roll.retired || 0)}</div><div className="kpi__sub">monitored hosts</div><div className="kpi__bar" /></div>
          <div className={"kpi ok clickable" + (stateFilter === "up" ? " on" : "")} role="button" tabIndex={0} onClick={() => setStateFilter("up")}><div className="kpi__k">Operational</div><div className="kpi__v">{roll.up}</div><div className="kpi__sub">{Math.round(roll.up / active * 100)}% healthy</div><div className="kpi__bar" /></div>
          <div className={"kpi warn clickable" + (stateFilter === "degraded" ? " on" : "")} role="button" tabIndex={0} onClick={() => setStateFilter("degraded")}><div className="kpi__k">Degraded</div><div className="kpi__v">{roll.degraded}</div><div className="kpi__sub">over tolerance</div><div className="kpi__bar" /></div>
          <div className={"kpi crit clickable" + (stateFilter === "down" ? " on" : "")} role="button" tabIndex={0} onClick={() => setStateFilter("down")}><div className="kpi__k">Down</div><div className="kpi__v">{roll.down}</div><div className="kpi__sub">{S.activeCrit} critical alerts</div><div className="kpi__bar" /></div>
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
          {retiredCount > 0 && (
            <React.Fragment>
              <div style={{ width: 1, height: 24, background: "var(--line)", margin: "0 4px" }} />
              <button className={"chip" + (showRetired ? " on" : "")} onClick={() => setShowRetired((v) => !v)}
                title={showRetired ? "Hide retired nodes" : "Show retired nodes"}>
                <span className="dot unknown" />Retired<span className="chip__n">{retiredCount}</span>
              </button>
            </React.Fragment>
          )}
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
    const stale = isStale(n);
    const maint = maintenanceFor && maintenanceFor(n);
    return (
      <div className={"cell " + n.state + (dense ? "" : " cozy-cell")} onClick={() => onOpenNode(n)}
        title={`${n.hostFqdn} — ${maint ? "in maintenance" + (maint.reason ? " (" + maint.reason + ")" : "") : n.state}${stale ? " · stale (last seen " + fmt.ago(n.lastSeenMin) + ")" : ""}`}>
        <div className="cell__top">
          <span className="cell__name">{n.name}</span>
          {maint ? <Icon name="wrench" size={11} style={{ color: "var(--violet)" }} />
            : <span className="cell__dot" style={{ background: STATE_COLOR[n.state], boxShadow: n.state !== "unknown" ? `0 0 6px ${STATE_COLOR[n.state]}` : "none" }} />}
        </div>
        <div className="cell__meta">{n.role === "client" ? n.ip : n.role.toUpperCase()}{stale && <span className="stale-tag">stale</span>}</div>
        {n.alertsCount > 0 && <span className="cell__badge">{n.alertsCount}</span>}
        <div className="cell__spark"><Sparkline data={n.hist.cpu} color={STATE_COLOR[n.state]} h={dense ? 20 : 26} fill={true} strokeW={1.5} /></div>
        <div className="cell__load"><i style={{ width: load + "%", background: metricColor(load), boxShadow: `0 0 5px ${metricColor(load)}` }} /></div>
      </div>
    );
  }

  function HeatView({ nodes, dense, onOpenNode }) {
    // One block (network segment, or a functional pool for agent-less systems).
    const block = (key, title, sub, segNodesIn) => {
      if (!segNodesIn.length) return null;
      // problem-first: worst state leads each segment block
      const segNodes = [...segNodesIn].sort((a, b) =>
        (STATE_ORD[a.state] ?? 4) - (STATE_ORD[b.state] ?? 4) || String(a.name).localeCompare(String(b.name)));
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
        if (k === "state") { va = STATE_ORD[a.state] ?? 4; vb = STATE_ORD[b.state] ?? 4; }
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
                <td><div className="td-host"><Icon name={ROLE_ICON[n.role]} size={15} className="ico" />{n.name}<span className="td-mono" style={{ fontSize: 10, color: "var(--ink-faint)" }}>.akoria.net</span>{MaintenanceBadge && <MaintenanceBadge entity={n} compact style={{ marginLeft: 6 }} />}</div></td>
                <td><span className="tag">{n.role}</span></td>
                <td className="td-mono">{n.segName} <span style={{ color: "var(--ink-faint)" }}>{n.ip}</span></td>
                <td style={{ textAlign: "right" }}><Bar pct={n.cpuPct} /></td>
                <td style={{ textAlign: "right" }}><Bar pct={n.ramPct} /></td>
                <td style={{ textAlign: "right" }}><Bar pct={n.diskMaxPct} /></td>
                <td className="td-mono" style={{ textAlign: "right" }}>{n.state === "down" ? "—" : fmt.mbps(n.netTotalMbps)}</td>
                <td className="td-mono" style={{ textAlign: "right", color: n.lastSeenMin != null && n.lastSeenMin > staleThresholdMin() ? "var(--crit)" : "var(--ink-dim)" }}>{n.lastSeenMin == null ? "—" : fmt.ago(n.lastSeenMin)}</td>
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
    const card = (key, title, sub, desc, segNodesIn) => {
      if (!segNodesIn.length) return null;
      const segNodes = [...segNodesIn].sort((a, b) =>
        (STATE_ORD[a.state] ?? 4) - (STATE_ORD[b.state] ?? 4) || String(a.name).localeCompare(String(b.name)));
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
              {segNodes.map((n) => {
                const stale = isStale(n);
                return (
                  <div key={n.nodeId} className={"minicell" + (stale ? " stale" : "")}
                    title={`${n.name} — ${n.state}${stale ? " · stale (last seen " + fmt.ago(n.lastSeenMin) + ")" : ""}`} onClick={() => onOpenNode(n)}
                    style={{ background: STATE_COLOR[n.state], boxShadow: n.state === "down" ? "0 0 7px var(--crit)" : "none", opacity: n.state === "unknown" ? 0.5 : 1 }} />
                );
              })}
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
  // metric taxonomy for the rule editor — the wire metric names the C server
  // evaluates (§ rules), grouped by scope, with their display units.
  const RULE_METRICS = {
    host: [
      { id: "cpuAvgMilli", label: "CPU average", unit: "%" },
      { id: "ramUsedPct", label: "RAM used", unit: "%" },
      { id: "swapUsedPct", label: "Swap used", unit: "%" },
      { id: "diskUsedPct", label: "Disk used", unit: "%" },
      { id: "lastSeenSec", label: "Silence (last seen)", unit: "s" },
      { id: "ifErrs", label: "NIC errors", unit: "/s" },
      { id: "procRunState", label: "Watched process state", unit: "" },
    ],
    probe: [
      { id: "lossPermille", label: "Packet loss", unit: "‰" },
      { id: "rttMicros", label: "Round-trip time", unit: "µs" },
      { id: "outcome", label: "Probe outcome", unit: "" },
    ],
  };

  // Create-rule modal — same .modal design system as AgentModal/DangerModal.
  // onSave receives the full rule body; the caller assigns the fresh ruleId
  // and POSTs it via the existing saveRule (RULE_SET) endpoint.
  function RuleModal({ onSave, onClose }) {
    const [name, setName] = useState("");
    const [scope, setScope] = useState("host");
    const [metric, setMetric] = useState(RULE_METRICS.host[0].id);
    const [op, setOp] = useState("gt");
    const [threshold, setThreshold] = useState(90);
    const [forSeconds, setForSeconds] = useState(60);
    const [severity, setSeverity] = useState("warn");
    const [busy, setBusy] = useState(false);
    useEffect(() => {
      function onKey(e) { if (e.key === "Escape") onClose(); }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, [onClose]);
    const metrics = RULE_METRICS[scope];
    const mdef = metrics.find((m) => m.id === metric) || metrics[0];
    const fld = { width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 4, borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };
    const lbl = { fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, marginTop: 14, display: "block" };
    const valid = name.trim().length > 0;
    function submit() {
      if (!valid || busy) return;
      setBusy(true);
      Promise.resolve(onSave({
        name: name.trim(), scope, metric: mdef.id, op,
        threshold: op === "transition" ? 0 : (Number(threshold) || 0),
        unit: mdef.unit,
        forSeconds: op === "transition" ? 0 : (Number(forSeconds) || 0),
        severity, enabled: true,
      })).then(() => setBusy(false));
    }
    return (
      <div className="modal-overlay" onClick={onClose}>
        <div className="modal" style={{ width: "min(460px, 94vw)" }} onClick={(e) => e.stopPropagation()}>
          <div className="modal__head">
            <Icon name="alerts" size={20} style={{ color: "var(--teal)" }} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontWeight: 700, fontSize: 15 }}>New alert rule</div>
              <div className="td-mono muted" style={{ fontSize: 11 }}>armed on save · pushed to the fleet</div>
            </div>
            <button className="iconbtn" style={{ width: 36, height: 36, flex: "0 0 36px" }} onClick={onClose} aria-label="Close"><Icon name="close" size={16} /></button>
          </div>
          <div className="modal__body">
            <label style={{ ...lbl, marginTop: 2 }}>Rule name</label>
            <input autoFocus style={fld} value={name} onChange={(e) => setName(e.target.value)} placeholder="e.g. Disk almost full" />
            <div style={{ display: "flex", gap: 12 }}>
              <div style={{ flex: 1 }}>
                <label style={lbl}>Scope</label>
                <select style={fld} value={scope} onChange={(e) => { const s = e.target.value; setScope(s); setMetric(RULE_METRICS[s][0].id); }}>
                  <option value="host">host</option><option value="probe">probe</option>
                </select>
              </div>
              <div style={{ flex: 2 }}>
                <label style={lbl}>Metric</label>
                <select style={fld} value={mdef.id} onChange={(e) => setMetric(e.target.value)}>
                  {metrics.map((m) => <option key={m.id} value={m.id}>{m.label} ({m.id})</option>)}
                </select>
              </div>
            </div>
            <div style={{ display: "flex", gap: 12 }}>
              <div style={{ flex: 1 }}>
                <label style={lbl}>Condition</label>
                <select style={fld} value={op} onChange={(e) => setOp(e.target.value)}>
                  <option value="gt">&gt; greater than</option>
                  <option value="lt">&lt; less than</option>
                  <option value="eq">= equals</option>
                  <option value="transition">Δ on change</option>
                </select>
              </div>
              {op !== "transition" && (
                <React.Fragment>
                  <div style={{ flex: 1 }}>
                    <label style={lbl}>Threshold{mdef.unit ? ` (${mdef.unit})` : ""}</label>
                    <input type="number" style={fld} value={threshold} onChange={(e) => setThreshold(e.target.value)} />
                  </div>
                  <div style={{ flex: 1 }}>
                    <label style={lbl}>Sustained for (s)</label>
                    <input type="number" style={fld} value={forSeconds} onChange={(e) => setForSeconds(e.target.value)} />
                  </div>
                </React.Fragment>
              )}
            </div>
            <label style={lbl}>Severity</label>
            <div style={{ display: "flex", gap: 8, marginTop: 6 }}>
              {["info", "warn", "crit"].map((sv) => (
                <button key={sv} className={"chip" + (severity === sv ? " on" : "")} onClick={() => setSeverity(sv)}>
                  <span className={"alert-sev " + sv}>{sv}</span>
                </button>
              ))}
            </div>
          </div>
          <div className="modal__foot">
            <button className="btn-ghost" onClick={onClose} disabled={busy}>Cancel</button>
            <button className={"btn-primary" + (valid ? "" : " disabled")} onClick={submit} disabled={busy}>
              <Icon name="plus" size={14} />{busy ? "Creating…" : "Create rule"}
            </button>
          </div>
        </div>
      </div>
    );
  }

  function AlertsScreen({ onOpenNode, onOpenOpie, rules, setRules, toast }) {
    const [tab, setTab] = useState("active");            // "active" | "history"
    const [acked, setAcked] = useState({});   // eventId -> true (optimistic, until refresh)
    const [confirmAll, setConfirmAll] = useState(false); // arm the Ack-All confirm
    const [ackingAll, setAckingAll] = useState(false);

    // ---- lifecycle derivation (mirrors the API's status rule) ----
    const isAcked = (a) => !!(a.ackedAt || acked[a.eventId]);
    const isCleared = (a) => !!(a.cleared || a.clearedAt);
    const rowStatus = (a) => isAcked(a) ? "acked" : isCleared(a) ? "cleared" : "active";
    // ACTIVE board = unacked AND (never cleared OR cleared within the last 60 min).
    const inActive = (a) => !isAcked(a) && (!isCleared(a) || (a.clearedMinAgo != null ? a.clearedMinAgo : 0) < 60);
    // "unhandled" = still firing (drives the KPI tiles + nav-badge parity).
    const unhandled = (a) => !isAcked(a) && !isCleared(a);

    const list = S.alerts.filter((a) => tab === "history" ? !inActive(a) : inActive(a));
    // group events by owning node (nodeless events group under "Fleet / probes");
    // S.alerts is pre-sorted uncleared+crit first, so group order inherits severity.
    const groups = [];
    {
      const byKey = new Map();
      list.forEach((a) => {
        const k = a.node || "Fleet / probes";
        if (!byKey.has(k)) { const g = { key: k, node: a.node, nodeId: a.nodeId, segName: a.segName, events: [] }; byKey.set(k, g); groups.push(g); }
        byKey.get(k).events.push(a);
      });
    }
    // POST /api/alerts/ack {eventId}, then refresh the model
    function ackAlert(a) {
      const x = api();
      if (x && x.ackAlert) {
        x.ackAlert(a.eventId).then(() => {
          setAcked((m) => ({ ...m, [a.eventId]: true }));
          toast(`Acknowledged — ${a.ruleName}`, "check");
          if (x.refresh) x.refresh().catch(() => {});
        }).catch((e) => toast(`Ack failed: ${e && e.message || "error"}`, "close"));
      } else {
        setAcked((m) => ({ ...m, [a.eventId]: true }));
        toast("Acknowledged (offline — not persisted)", "check");
      }
    }
    // POST /api/alerts/ack-all — ack every currently-ACTIVE alert; report the count.
    function ackAll() {
      const x = api();
      const localIds = S.alerts.filter(inActive).map((a) => a.eventId);
      const finish = (n) => {
        setAcked((m) => { const c = { ...m }; localIds.forEach((id) => { c[id] = true; }); return c; });
        setConfirmAll(false); setAckingAll(false);
        toast(`Acknowledged ${n} alert${n === 1 ? "" : "s"}`, "check");
      };
      if (x && x.ackAllAlerts) {
        setAckingAll(true);
        x.ackAllAlerts().then((r) => {
          const n = (r && (r.count != null ? r.count : r.acked)) != null ? (r.count != null ? r.count : r.acked) : localIds.length;
          if (x.refresh) x.refresh().catch(() => {});
          finish(n);
        }).catch((e) => { setAckingAll(false); toast(`Ack-all failed: ${e && e.message || "error"}`, "close"); });
      } else {
        finish(localIds.length);   // offline optimistic
      }
    }

    const crit = S.alerts.filter((a) => unhandled(a) && a.severity === "crit").length;
    const warn = S.alerts.filter((a) => unhandled(a) && a.severity === "warn").length;
    const info = S.alerts.filter((a) => unhandled(a) && a.severity === "info").length;
    const activeCount = S.alerts.filter(inActive).length;   // what Ack-All targets

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

    // ---- rule CRUD (create via existing RULE_SET; delete via RULE_DEL) ----
    const [ruleModal, setRuleModal] = useState(false);
    const [delRule, setDelRule] = useState(null);   // rule pending delete confirmation
    function createRule(fields) {
      const a = api();
      if (!a || !a.saveRule) { toast("Rule editing unavailable (offline)", "close"); setRuleModal(false); return; }
      const ruleId = rules.reduce((m, r) => Math.max(m, Number(r.ruleId) || 0), 0) + 1;
      return a.saveRule(ruleId, fields).then(() => {
        setRules((rs) => [...rs, Object.assign({ ruleId }, fields)]);
        toast(`Rule created: ${fields.name}`, "check");
        setRuleModal(false);
        if (a.refresh) a.refresh().catch(() => {});
      }).catch((e) => toast(`Rule create failed: ${e && e.message || "error"}`, "close"));
    }
    function deleteRuleGo() {
      const a = api();
      if (!a || !a.deleteRule) { toast("Rule deletion unavailable (offline)", "close"); setDelRule(null); return; }
      const r = delRule;
      return a.deleteRule(r.ruleId).then(() => {
        setRules((rs) => rs.filter((x) => x.ruleId !== r.ruleId));
        toast(`Rule deleted: ${r.name}`, "close");
        setDelRule(null);
        if (a.refresh) a.refresh().catch(() => {});
      }).catch((e) => toast(`Delete failed: ${e && e.message || "error"}`, "close"));
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Alerts & Tolerances</h1><div className="page-sub">{crit + warn + info} active · {rules.filter((r) => r.enabled).length}/{rules.length} rules armed</div></div>
          <div className="page-head__right">
            {tab === "active" && (
              confirmAll ? (
                <div className="ackall-confirm">
                  <span className="ackall-confirm__q">Ack {activeCount} active alert{activeCount === 1 ? "" : "s"}?</span>
                  <button className="btn-primary" disabled={ackingAll} onClick={ackAll}>
                    <Icon name="check" size={14} />{ackingAll ? "Acking…" : "Confirm"}
                  </button>
                  <button className="btn-ghost" disabled={ackingAll} onClick={() => setConfirmAll(false)}>Cancel</button>
                </div>
              ) : (
                <button className={"btn-ghost btn-ackall" + (activeCount === 0 ? " disabled" : "")} disabled={activeCount === 0}
                  onClick={() => setConfirmAll(true)} title="Acknowledge every active alert">
                  <Icon name="check" size={14} />Ack all{activeCount > 0 ? ` · ${activeCount}` : ""}
                </button>
              )
            )}
            <div className="seg">
              <button className={tab === "active" ? "on" : ""} onClick={() => { setTab("active"); setConfirmAll(false); }}>Active</button>
              <button className={tab === "history" ? "on" : ""} onClick={() => { setTab("history"); setConfirmAll(false); }}>History</button>
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
            <div className="page-sub" style={{ marginBottom: 12 }}>{tab === "active" ? "Live board · by node" : "Retired events (acked or auto-cleared) · by node"}</div>
            {list.length === 0 && <div className="empty">{tab === "active" ? "No active alerts — all systems nominal." : "No retired alerts yet."}</div>}
            {groups.map((g) => {
              const open = g.events.filter((a) => unhandled(a));
              const gc = open.filter((a) => a.severity === "crit").length;
              const gw = open.filter((a) => a.severity === "warn").length;
              return (
                <div key={g.key} className="alert-group">
                  <div className={"alert-group__head" + (g.nodeId != null ? " link" : "")}
                    onClick={() => g.nodeId != null && onOpenNode(g.nodeId)}>
                    <Icon name={g.node ? "host" : "reachability"} size={14} className="ico" />
                    <span className="alert-group__name">{g.key}</span>
                    {g.segName && <span className="alert-group__seg">{g.segName}</span>}
                    <span className="alert-group__counts">
                      {gc > 0 && <span className="attn__pill crit">{gc} crit</span>}
                      {gw > 0 && <span className="attn__pill warn">{gw} warn</span>}
                      <span className="alert-group__n">{g.events.length} event{g.events.length === 1 ? "" : "s"}</span>
                    </span>
                  </div>
                  {g.events.map((a) => {
                    const st = rowStatus(a);
                    return (
                    <div key={a.eventId} className={"alert-row " + a.severity + (st !== "active" ? " cleared" : "")} onClick={() => a.nodeId && onOpenNode(a.nodeId)}>
                      <span className={"alert-sev " + a.severity}>{a.severity}</span>
                      <div className="alert-main">
                        <div className="t">{a.ruleName}</div>
                        <div className="d">{a.detail}</div>
                      </div>
                      {MaintenanceBadge && <MaintenanceBadge entity={a} compact />}
                      <span className={"alert-stat " + st} title={st === "acked" && a.ackedBy ? "Acknowledged by " + a.ackedBy : ("Status: " + st)}>
                        {st === "acked" ? "Acked" : st === "cleared" ? "Cleared" : "Active"}
                      </span>
                      {a.opieReportId != null && onOpenOpie && (
                        <button className="btn-ghost btn-opie" onClick={(ev) => { ev.stopPropagation(); onOpenOpie(a.opieReportId); }}
                          title="Open Opie's analysis of this incident"><Icon name="pulse" size={13} />Opie</button>
                      )}
                      {isAcked(a)
                        ? <span className="alert-ackby" title={a.ackedBy ? "Acknowledged by " + a.ackedBy : "Acknowledged"}><Icon name="check" size={12} />{a.ackedBy || "ack’d"}</span>
                        : <button className="btn-ghost btn-ack" onClick={(ev) => { ev.stopPropagation(); ackAlert(a); }}
                            title="Acknowledge this event"><Icon name="check" size={13} />Ack</button>}
                      <div className="alert-meta">
                        {st === "cleared" ? <div>cleared {a.clearedMinAgo != null ? fmt.ago(a.clearedMinAgo) : ""}</div>
                          : st === "acked" && a.ackedMinAgo != null ? <div>acked {fmt.ago(a.ackedMinAgo)}</div> : null}
                        <div>{fmt.ago(a.firedMinAgo)}</div>
                      </div>
                    </div>
                    );
                  })}
                </div>
              );
            })}
          </div>

          <div className="panel">
            <div className="panel__head">
              <Icon name="settings" size={16} /><h3>Alert rules & tolerances</h3>
              <div className="right"><button className="btn-primary" onClick={() => setRuleModal(true)}><Icon name="plus" size={14} />New rule</button></div>
            </div>
            <div className="panel__body" style={{ padding: 0 }}>
              {rules.length === 0 && <div className="td-mono muted" style={{ fontSize: 13, padding: 16 }}>No rules defined — create one to start alerting.</div>}
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
                    <button className="rowx" title={`Delete rule "${r.name}"`} aria-label={`Delete rule ${r.name}`} onClick={() => setDelRule(r)}>
                      <Icon name="close" size={13} />
                    </button>
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

        {ruleModal && <RuleModal onSave={createRule} onClose={() => setRuleModal(false)} />}
        {delRule && (
          <DangerModal title={`Delete rule "${delRule.name}"?`}
            sub={`${delRule.metric} ${({ gt: ">", lt: "<", eq: "=", transition: "Δ" })[delRule.op] || delRule.op} ${delRule.op !== "transition" ? delRule.threshold + (delRule.unit || "") : "change"} · ${delRule.scope}`}
            verb="Delete rule" busyVerb="Deleting…" onConfirm={deleteRuleGo} onClose={() => setDelRule(null)}>
            <p style={{ margin: 0 }}>
              The rule stops firing immediately and is removed from every node's evaluation set.
              Alert history already recorded by this rule is kept.
            </p>
          </DangerModal>
        )}
      </div>
    );
  }

  Object.assign(window, { FleetOverview, NodeDetail, AlertsScreen, AttentionPanel });
})();
