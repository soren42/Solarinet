/* ============================================================
   SolariNet — screens3: Discovery, Provisioning, Config & Rules
   ============================================================ */
(function () {
  const { useState } = React;
  const Icon = window.Icon;
  const { StatusDot } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;

  function toast(msg, icon) { if (window.__solariToast) window.__solariToast(msg, icon); }
  // adapter handle (live mutations); null/offline-safe
  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function refresh() { const a = api(); if (a && a.refresh) a.refresh().catch(function () {}); }

  /* ===================== DISCOVERY ===================== */
  function Discovery({ onOpenNode }) {
    const [staged, setStaged] = useState({});      // host -> "staged" | "ignored"
    const [auto, setAuto] = useState(S.config.autoDiscover);
    const [cidr, setCidr] = useState("");
    const [scanning, setScanning] = useState(false);
    const items = S.discovered;
    const active = items.filter((d) => staged[d.host] !== "ignored");
    const byVia = {};
    items.forEach((d) => { byVia[d.via] = (byVia[d.via] || 0) + 1; });

    // adopt (monitor) — POST /api/discovery/{discId}/adopt via the adapter
    function stage(d) {
      setStaged((s) => ({ ...s, [d.host]: "staged" }));   // optimistic
      const a = api();
      if (a && a.adoptDiscovered && d.discId != null) {
        a.adoptDiscovered(d.discId).then(function () {
          toast(`Adopting ${d.host} → solariCtl (probe target)`, "shield");
          refresh();
        }).catch(function (e) {
          setStaged((s) => { const n = { ...s }; delete n[d.host]; return n; });
          toast(`Adopt failed: ${e && e.message || "error"}`, "close");
        });
      } else {
        toast(`Enrollment token issued → ${d.host}`, "shield");
      }
    }
    // ignore — POST /api/discovery/{discId}/ignore via the adapter
    function ignore(d) {
      setStaged((s) => ({ ...s, [d.host]: "ignored" }));  // optimistic
      const a = api();
      if (a && a.ignoreDiscovered && d.discId != null) {
        a.ignoreDiscovered(d.discId).then(function () {
          toast(`${d.host} ignored`, "close");
          refresh();
        }).catch(function (e) {
          setStaged((s) => { const n = { ...s }; delete n[d.host]; return n; });
          toast(`Ignore failed: ${e && e.message || "error"}`, "close");
        });
      } else {
        toast(`${d.host} ignored`, "close");
      }
    }

    // active scan — POST /api/discovery/scan (TCP connect-scan via solariCtl)
    function scanNow() {
      const target = cidr.trim();
      if (!/^\d{1,3}(\.\d{1,3}){3}\/\d{1,2}$/.test(target)) {
        toast("Enter a CIDR, e.g. 192.168.1.0/24", "close"); return;
      }
      const a = api();
      if (!a || !a.discoverScan) { toast("Scan unavailable (offline)", "close"); return; }
      setScanning(true);
      a.discoverScan(target).then(function (r) {
        toast(`Scan complete — ${r && r.found != null ? r.found : "?"} host(s) found`, "discovery");
        refresh();
      }).catch(function (e) {
        toast(`Scan failed: ${e && e.message || "error"}`, "close");
      }).finally(function () { setScanning(false); });
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Discovery</h1><div className="page-sub">{items.length} candidates found · not yet monitored</div></div>
          <div className="page-head__right">
            <input value={cidr} onChange={(e) => setCidr(e.target.value)} placeholder="192.168.1.0/24"
              onKeyDown={(e) => { if (e.key === "Enter") scanNow(); }}
              style={{ padding: "7px 10px", borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13, width: 150 }} />
            <button className="btn-primary" disabled={scanning} onClick={scanNow}>
              <Icon name="discovery" size={14} />{scanning ? "Scanning…" : "Scan"}
            </button>
            <div className="chip" onClick={() => { setAuto((a) => !a); toast(`Auto-discovery ${auto ? "paused" : "resumed"}`, auto ? "close" : "check"); }}>
              <button className={"switch" + (auto ? " on" : "")} style={{ pointerEvents: "none" }}><i /></button>Auto-discover
            </div>
          </div>
        </div>

        <div className="kpis">
          <div className="kpi teal"><div className="kpi__k">Candidates</div><div className="kpi__v">{active.filter((d) => staged[d.host] !== "staged").length}</div><div className="kpi__sub">awaiting decision</div><div className="kpi__bar" /></div>
          <div className="kpi ok"><div className="kpi__k">Staged</div><div className="kpi__v">{Object.values(staged).filter((v) => v === "staged").length}</div><div className="kpi__sub">enrollment issued</div><div className="kpi__bar" /></div>
          {Object.entries(byVia).slice(0, 3).map(([via, n]) => (
            <div className="kpi" key={via}><div className="kpi__k">{via}</div><div className="kpi__v" style={{ color: "var(--violet)" }}>{n}</div><div className="kpi__sub">via this method</div><div className="kpi__bar" style={{ background: "var(--violet)" }} /></div>
          ))}
        </div>

        <div className="page-sub" style={{ margin: "4px 0 12px" }}>Discovered hosts & services</div>
        {active.map((d) => {
          const isStaged = staged[d.host] === "staged";
          return (
            <div key={d.host} className="disc-row">
              <Icon name={d.kind === "service" ? "link" : "host"} size={20} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
              <div className="disc-main">
                <div className="disc-host">{d.host}<span className="muted" style={{ fontSize: 11, marginLeft: 8 }}>{d.ip}</span></div>
                <div className="disc-svcs">
                  {d.services.map((s) => <span key={s} className="svc-chip">{s}</span>)}
                </div>
              </div>
              <div className="disc-meta">
                <span className="tag">{d.via}</span>
                <span className="td-mono muted">{d.seg} · {d.arch}</span>
                <span className="td-mono muted">{d.seen}m ago</span>
              </div>
              {isStaged ? (
                <span className="alert-sev info" style={{ flex: "0 0 auto" }}><Icon name="check" size={12} style={{ verticalAlign: "-2px" }} /> staged</span>
              ) : (
                <div className="disc-actions">
                  <button className="btn-ghost" onClick={() => ignore(d)}>Ignore</button>
                  <button className="btn-primary" onClick={() => stage(d)}><Icon name="plus" size={14} />Monitor</button>
                </div>
              )}
            </div>
          );
        })}
      </div>
    );
  }

  /* ===================== PROVISIONING ===================== */
  function Provisioning({ onOpenNode }) {
    const [enr, setEnr] = useState(S.enrollments);
    const [token, setToken] = useState(null);
    const drifted = S.nodes.filter((n) => !n.converged);
    const converged = S.nodes.length - drifted.length;

    // approve (sign CSR) — POST /api/enrollments/{enrId}/approve via the adapter.
    // Destructive: the adapter sends confirm:true; the bridge signs via the CA.
    function approve(e) {
      const a = api();
      if (a && a.approveEnrollment && e.enrId != null) {
        setEnr((list) => list.filter((x) => x.host !== e.host));   // optimistic
        a.approveEnrollment(e.enrId).then(function () {
          toast(`CSR signed — ${e.host} enrolled as ${e.role}`, "shield");
          refresh();
        }).catch(function (err) {
          setEnr(S.enrollments);   // revert from model
          toast(`Approve failed: ${err && err.message || "error"}`, "close");
        });
      } else {
        setEnr((list) => list.filter((x) => x.host !== e.host));
        toast(`CSR signed — ${e.host} enrolled as ${e.role}`, "shield");
      }
    }
    // reject — POST /api/enrollments/{enrId}/reject via the adapter
    function deny(e) {
      const a = api();
      if (a && a.rejectEnrollment && e.enrId != null) {
        setEnr((list) => list.filter((x) => x.host !== e.host));   // optimistic
        a.rejectEnrollment(e.enrId).then(function () {
          toast(`Enrollment denied — ${e.host}`, "close");
          refresh();
        }).catch(function (err) {
          setEnr(S.enrollments);
          toast(`Reject failed: ${err && err.message || "error"}`, "close");
        });
      } else {
        setEnr((list) => list.filter((x) => x.host !== e.host));
        toast(`Enrollment denied — ${e.host}`, "close");
      }
    }
    function issueToken() {
      const t = "SLR-" + Math.random().toString(36).slice(2, 8).toUpperCase() + "-" + Math.random().toString(36).slice(2, 6).toUpperCase();
      setToken({ value: t, ttl: 900 });
      toast("Single-use enrollment token generated (TTL 15m)", "shield");
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Provisioning</h1><div className="page-sub">enrollment · binary deploy · config convergence</div></div>
        </div>

        <div className="kpis">
          <div className="kpi warn"><div className="kpi__k">Pending CSRs</div><div className="kpi__v">{enr.length}</div><div className="kpi__sub">awaiting approval</div><div className="kpi__bar" /></div>
          <div className="kpi ok"><div className="kpi__k">Converged</div><div className="kpi__v">{converged}</div><div className="kpi__sub">at target epoch</div><div className="kpi__bar" /></div>
          <div className="kpi crit"><div className="kpi__k">Config drift</div><div className="kpi__v">{drifted.length}</div><div className="kpi__sub">need re-push</div><div className="kpi__bar" /></div>
          <div className="kpi teal"><div className="kpi__k">Build channels</div><div className="kpi__v">{S.builds.length}</div><div className="kpi__sub">arch × OS targets</div><div className="kpi__bar" /></div>
        </div>

        <div className="two-col" style={{ alignItems: "start" }}>
          <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
            {/* enrollment */}
            <div className="panel">
              <div className="panel__head"><Icon name="shield" size={16} /><h3>Enrollment queue</h3><div className="right"><button className="btn-primary" onClick={issueToken}><Icon name="plus" size={14} />Issue token</button></div></div>
              <div className="panel__body" style={{ padding: enr.length ? 0 : 16 }}>
                {token && (
                  <div className="token-banner">
                    <div><div className="kpi__k">One-time enrollment token</div><div className="token-val">{token.value}</div></div>
                    <div className="td-mono muted">TTL 15:00 · single-use</div>
                  </div>
                )}
                {enr.length === 0 && <div className="td-mono muted" style={{ fontSize: 13 }}>No pending enrollments.</div>}
                {enr.map((e) => (
                  <div key={e.host} className="enr-row">
                    <Icon name={e.role === "monitor" ? "monitor" : "host"} size={18} style={{ color: "var(--ink-dim)", flex: "0 0 auto" }} />
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <div className="td-mono" style={{ fontWeight: 600 }}>{e.host}</div>
                      <div className="td-mono muted" style={{ fontSize: 11 }}>{e.ip} · {e.role} · CSR {e.fp}</div>
                    </div>
                    <span className="tag">{e.status}</span>
                    <span className="td-mono muted" style={{ fontSize: 11 }}>{e.requestedMin}m</span>
                    <div className="disc-actions">
                      <button className="btn-ghost" onClick={() => deny(e)}>Deny</button>
                      <button className="btn-primary" onClick={() => approve(e)}><Icon name="check" size={14} />Sign</button>
                    </div>
                  </div>
                ))}
              </div>
            </div>

            {/* config convergence */}
            <div className="panel">
              <div className="panel__head"><Icon name="refresh" size={16} /><h3>Config convergence</h3><div className="right">{converged}/{S.nodes.length} ok</div></div>
              <div className="panel__body" style={{ padding: 0 }}>
                {drifted.slice(0, 8).map((n) => (
                  <div key={n.nodeId} className="enr-row" onClick={() => onOpenNode(n.nodeId)} style={{ cursor: "pointer" }}>
                    <span className="dot degraded" style={{ flex: "0 0 auto" }} />
                    <div style={{ flex: 1, minWidth: 0 }}>
                      <div className="td-mono" style={{ fontWeight: 600 }}>{n.name}<span className="muted" style={{ fontSize: 11 }}> · {n.segName}</span></div>
                      <div className="td-mono muted" style={{ fontSize: 11 }}>target epoch {n.configEpoch} · applied {n.configEpoch - 1} · drift</div>
                    </div>
                    <button className="btn-primary" onClick={(ev) => {
                      ev.stopPropagation();
                      const a = api();
                      if (a && a.provision && n.nodeId != null) {
                        a.provision({ nodeId: n.nodeId, configEpoch: n.configEpoch }).then(function () {
                          toast(`Config re-pushed → ${n.name} (solariCtl PROVISION)`, "settings"); refresh();
                        }).catch(function (e) { toast(`Re-push failed: ${e && e.message || "error"}`, "close"); });
                      } else { toast(`Config re-pushed → ${n.name} (SCP_MSG_CONTROL)`, "settings"); }
                    }}><Icon name="refresh" size={13} />Re-push</button>
                  </div>
                ))}
                {drifted.length === 0 && <div className="td-mono muted" style={{ fontSize: 13, padding: 16 }}>All nodes converged.</div>}
              </div>
            </div>
          </div>

          {/* binary builds */}
          <div className="panel">
            <div className="panel__head"><Icon name="arch" size={16} /><h3>Binary builds & deploy</h3></div>
            <div className="panel__body" style={{ padding: 0 }}>
              {S.builds.map((b, i) => (
                <div key={i} className="build-row">
                  <div style={{ flex: 1 }}>
                    <div className="td-mono" style={{ fontWeight: 600 }}>{b.os} · {b.arch}</div>
                    <div className="td-mono muted" style={{ fontSize: 11 }}>{b.nodes} nodes · {b.channel}</div>
                  </div>
                  <div className="td-mono" style={{ textAlign: "right" }}>
                    <div style={{ color: b.status === "current" ? "var(--ok)" : "var(--warn)", fontWeight: 600 }}>v{b.version}</div>
                    <div className="muted" style={{ fontSize: 10 }}>{b.status === "current" ? "up to date" : "update available"}</div>
                  </div>
                  {b.status === "update"
                    ? <button className="btn-primary" onClick={() => toast(`Rolling v1.0.3 → ${b.nodes} ${b.arch} nodes`, "arch")}><Icon name="refresh" size={13} />Push</button>
                    : <span className="alert-sev info" style={{ flex: "0 0 auto" }}>current</span>}
                </div>
              ))}
            </div>
          </div>
        </div>
      </div>
    );
  }

  /* ===================== CONFIG & RULES ===================== */
  function ConfigScreen({ onNav }) {
    const [tab, setTab] = useState("global");
    const [cfg, setCfg] = useState(JSON.parse(JSON.stringify(S.config)));
    const [dirty, setDirty] = useState(false);
    function set(path, val) {
      setCfg((c) => { const n = JSON.parse(JSON.stringify(c)); let o = n; const parts = path.split("."); for (let i = 0; i < parts.length - 1; i++) o = o[parts[i]]; o[parts[parts.length - 1]] = val; return n; });
      setDirty(true);
    }
    function Row({ label, path, min, max, step, unit }) {
      const parts = path.split("."); let v = cfg; parts.forEach((p) => v = v[p]);
      return (
        <div className="metric-row">
          <span className="lbl" style={{ width: 150, flex: "0 0 150px" }}>{label}</span>
          <input type="range" min={min} max={max} step={step || 1} value={v} onChange={(e) => set(path, +e.target.value)} style={{ flex: 1, accentColor: "var(--teal)" }} />
          <span className="val" style={{ width: 90, flex: "0 0 90px", color: "var(--teal)" }}>{v}{unit}</span>
        </div>
      );
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Config & Rules</h1><div className="page-sub">global tolerances, schedules & retention · pushed as SCP control frames</div></div>
          <div className="page-head__right">
            <div className="seg">
              <button className={tab === "global" ? "on" : ""} onClick={() => setTab("global")}><Icon name="settings" size={14} />Global</button>
              <button className={tab === "agents" ? "on" : ""} onClick={() => setTab("agents")}><Icon name="host" size={14} />Per-agent</button>
            </div>
            <button className="backbtn" onClick={() => onNav("alerts")}><Icon name="alerts" size={15} />Alert rules</button>
            {tab === "global" && <button className={"btn-primary" + (dirty ? "" : " disabled")} onClick={() => {
              if (!dirty) return;
              const a = api();
              if (a && a.saveConfig) {
                a.saveConfig(cfg).then(function () { toast("Config saved → pushed to fleet (solariCtl)", "settings"); setDirty(false); refresh(); })
                  .catch(function (e) { toast(`Save failed: ${e && e.message || "error"}`, "close"); });
              } else { toast("Config staged → pushed to fleet on next epoch", "settings"); setDirty(false); }
            }}><Icon name="check" size={14} />Save & push</button>}
          </div>
        </div>

        {tab === "global" && (
        <div className="three-col" style={{ alignItems: "start" }}>
          <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
            <div className="panel">
              <div className="panel__head"><Icon name="clock" size={16} /><h3>Sampling schedule</h3></div>
              <div className="panel__body">
                <Row label="Sample interval" path="schedule.sampleIntervalSec" min={5} max={120} unit="s" />
                <Row label="Watchdog interval" path="schedule.watchdogIntervalSec" min={1} max={30} unit="s" />
              </div>
            </div>
            <div className="panel">
              <div className="panel__head"><Icon name="disk" size={16} /><h3>Retention</h3></div>
              <div className="panel__body">
                <Row label="History" path="retention.historyDays" min={7} max={365} unit="d" />
                <div className="cfg-toggle"><span>Monthly partition pruning</span><button className={"switch" + (cfg.retention.partitionByMonth ? " on" : "")} onClick={() => set("retention.partitionByMonth", !cfg.retention.partitionByMonth)}><i /></button></div>
              </div>
            </div>
          </div>

          <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
            <div className="panel">
              <div className="panel__head"><Icon name="reachability" size={16} /><h3>Probe defaults</h3></div>
              <div className="panel__body">
                <Row label="Round interval" path="probe.roundIntervalSec" min={10} max={120} unit="s" />
                <Row label="Probes / round" path="probe.probesPerRound" min={1} max={20} unit="" />
                <Row label="Replication factor" path="probe.replFactor" min={1} max={5} unit="×" />
                <Row label="Peer gossip" path="probe.gossipIntervalSec" min={5} max={60} unit="s" />
              </div>
            </div>
            <div className="panel">
              <div className="panel__head"><Icon name="shield" size={16} /><h3>Failover lease</h3></div>
              <div className="panel__body">
                <Row label="Renew interval" path="lease.renewSec" min={1} max={30} unit="s" />
                <Row label="Lease TTL" path="lease.ttlSec" min={5} max={60} unit="s" />
              </div>
            </div>
          </div>

          <div style={{ display: "flex", flexDirection: "column", gap: 16 }}>
            <div className="panel">
              <div className="panel__head"><Icon name="settings" size={16} /><h3>Discovery & enrollment</h3></div>
              <div className="panel__body">
                <div className="cfg-toggle"><span>Auto-discovery</span><button className={"switch" + (cfg.autoDiscover ? " on" : "")} onClick={() => set("autoDiscover", !cfg.autoDiscover)}><i /></button></div>
                <div className="cfg-toggle"><span>Auto-enroll discovered <span className="muted" style={{ fontSize: 11 }}>(requires approval off)</span></span><button className={"switch" + (cfg.autoEnroll ? " on" : "")} onClick={() => set("autoEnroll", !cfg.autoEnroll)}><i /></button></div>
                <div className="divider" />
                <div className="cfg-info"><span className="kpi__k">Internal CA</span><span className="td-mono">{cfg.ca.issuer}</span></div>
                <div className="cfg-info"><span className="kpi__k">Cert TTL</span><span className="td-mono">{cfg.ca.certTtlDays} days</span></div>
                <div className="cfg-info"><span className="kpi__k">Enroll method</span><span className="td-mono">{cfg.ca.enroll}</span></div>
              </div>
            </div>
            <div className="panel">
              <div className="panel__head"><Icon name="network" size={16} /><h3>Transport</h3><div className="right">{cfg.ingest.tls}</div></div>
              <div className="panel__body">
                <div className="cfg-info"><span className="kpi__k">Ingest</span><span className="td-mono">:{cfg.ingest.ports.ingest} · PULL</span></div>
                <div className="cfg-info"><span className="kpi__k">Survey</span><span className="td-mono">:{cfg.ingest.ports.survey} · SURVEYOR</span></div>
                <div className="cfg-info"><span className="kpi__k">Publish</span><span className="td-mono">:{cfg.ingest.ports.pub} · PUB</span></div>
              </div>
            </div>
          </div>
        </div>
        )}
        {tab === "agents" && <AgentDirectory />}
      </div>
    );
  }

  /* per-agent directory + modal config editor */
  function AgentDirectory() {
    const [type, setType] = useState("client");
    const [q, setQ] = useState("");
    const [driftOnly, setDriftOnly] = useState(false);
    const [modalId, setModalId] = useState(null);
    const pool = S.nodes.filter((n) => n.role === type);
    const list = pool.filter((n) => (!q || n.name.includes(q.toLowerCase()) || (n.ip || "").includes(q)) && (!driftOnly || !n.converged));
    const drift = pool.filter((n) => !n.converged).length;
    const modalNode = modalId ? S.nodes.find((n) => n.nodeId === modalId) : null;
    const TYPE_ICON = { server: "server", monitor: "monitor", client: "host" };
    const TYPE_LABEL = { server: "Server", monitor: "Network", client: "Application" };

    return (
      <div>
        <div className="filters">
          {["server", "monitor", "client"].map((t) => (
            <button key={t} className={"chip" + (type === t ? " on" : "")} onClick={() => setType(t)}><Icon name={TYPE_ICON[t]} size={14} />{TYPE_LABEL[t]}<span className="chip__n">{S.nodes.filter((n) => n.role === t).length}</span></button>
          ))}
          <div style={{ width: 1, height: 24, background: "var(--line)", margin: "0 4px" }} />
          <button className={"chip" + (driftOnly ? " on" : "")} onClick={() => setDriftOnly((d) => !d)}><span className="dot degraded" />Drift only<span className="chip__n">{drift}</span></button>
          <div className="search" style={{ marginLeft: "auto", maxWidth: 250, height: 36 }}><Icon name="search" size={15} /><input value={q} onChange={(e) => setQ(e.target.value)} placeholder="Find agent…" /></div>
        </div>

        <div className="agent-dir">
          {list.map((n) => (
            <div key={n.nodeId} className="agent-card" onClick={() => setModalId(n.nodeId)}>
              <Icon name={TYPE_ICON[n.role]} size={18} style={{ color: "var(--ink-dim)", flex: "0 0 auto" }} />
              <div style={{ flex: 1, minWidth: 0 }}>
                <div className="td-mono" style={{ fontWeight: 600, fontSize: 13, display: "flex", alignItems: "center", gap: 7 }}><StatusDot state={n.state} size={8} />{n.name}</div>
                <div className="td-mono muted" style={{ fontSize: 10.5, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{n.segName} · {n.ip} · epoch {n.configEpoch}</div>
              </div>
              {!n.converged && <span className="tag" style={{ color: "var(--warn)", borderColor: "var(--warn)" }}>drift</span>}
              <Icon name="chevronRight" size={16} style={{ color: "var(--ink-faint)", flex: "0 0 auto" }} />
            </div>
          ))}
          {list.length === 0 && <div className="empty" style={{ gridColumn: "1 / -1" }}>No agents match.</div>}
        </div>

        {modalNode && <AgentModal node={modalNode} onClose={() => setModalId(null)} />}
      </div>
    );
  }

  function AgentModal({ node, onClose }) {
    function initDraft(n) {
      if (n.role === "client") return { sampleSec: 15, logPat: "(error|fail)", watched: Object.fromEntries(n.procs.map((p) => [p.name, true])) };
      if (n.role === "monitor") { const ts = S.probes.filter((p) => p.vantages.some((v) => v.monitorNode === n.nodeId)); return { roundSec: 30, perRound: 5, repl: 2, targets: Object.fromEntries(ts.map((t) => [t.targetId, true])) }; }
      return { renewSec: 5, ttlSec: 15, pool: 8 };
    }
    const [draft, setDraft] = useState(() => initDraft(node));
    const TYPE_ICON = { server: "server", monitor: "monitor", client: "host" };
    React.useEffect(() => {
      function onKey(e) { if (e.key === "Escape") onClose(); }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, []);
    // decommission (irreversible) — double-confirmed: a UI confirm here, plus the
    // adapter sends confirm:true and the PHP/bridge run the two-step token handshake.
    function decommission() {
      if (!window.confirm(`Decommission ${node.name}? This securely wipes config, certs, spool, logs, and the service unit, then retires the node. This cannot be undone.`)) return;
      const a = api();
      if (a && a.decommission && node.nodeId != null) {
        a.decommission(node.nodeId, ["config", "certs", "spool", "logs", "unit"]).then(function () {
          toast(`Decommissioned → ${node.name} (retired)`, "close"); refresh(); onClose();
        }).catch(function (e) { toast(`Decommission failed: ${e && e.message || "error"}`, "close"); });
      } else {
        toast(`Decommission requires the live control plane`, "close");
      }
    }
    function push() {
      const a = api();
      if (a && a.provision && node.nodeId != null) {
        a.provision({ nodeId: node.nodeId, configEpoch: node.configEpoch + 1, configBlob: draft }).then(function () {
          toast("Config pushed → " + node.name + " (solariCtl · epoch " + (node.configEpoch + 1) + ")", "settings"); refresh(); onClose();
        }).catch(function (e) { toast(`Push failed: ${e && e.message || "error"}`, "close"); });
      } else {
        toast("Config pushed → " + node.name + " (SCP_MSG_CONTROL · epoch " + (node.configEpoch + 1) + ")", "settings"); onClose();
      }
    }

    function Slider({ label, k, min, max, unit }) {
      return (
        <div className="metric-row">
          <span className="lbl" style={{ width: 150, flex: "0 0 150px" }}>{label}</span>
          <input type="range" min={min} max={max} value={draft[k] != null ? draft[k] : min} onChange={(e) => setDraft((d) => ({ ...d, [k]: +e.target.value }))} style={{ flex: 1, accentColor: "var(--teal)" }} />
          <span className="val" style={{ width: 84, flex: "0 0 84px", color: "var(--teal)" }}>{draft[k]}{unit}</span>
        </div>
      );
    }

    return (
      <div className="modal-overlay" onClick={onClose}>
        <div className="modal" onClick={(e) => e.stopPropagation()}>
          <div className="modal__head">
            <Icon name={TYPE_ICON[node.role]} size={20} style={{ color: "var(--teal)" }} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontFamily: "var(--mono)", fontWeight: 600, fontSize: 15 }}>{node.name}<span className="muted">.akoria.net</span></div>
              <div className="td-mono muted" style={{ fontSize: 11 }}>{node.role}{node.label ? " · " + node.label : ""} · {node.segName} · {node.osName} · {node.arch} · epoch {node.configEpoch}</div>
            </div>
            <button className="iconbtn" style={{ width: 36, height: 36, flex: "0 0 36px" }} onClick={onClose} aria-label="Close"><Icon name="close" size={16} /></button>
          </div>
          <div className="modal__body">
            {node.role === "client" && (
              <React.Fragment>
                <Slider label="Sample interval" k="sampleSec" min={5} max={120} unit="s" />
                <div className="agent-sect">Watched applications</div>
                <div className="agent-apps">
                  {node.procs.map((p) => (
                    <div key={p.name} className="agent-app">
                      <span className="td-mono" style={{ fontWeight: 600, flex: 1 }}>{p.name}</span>
                      <span className="td-mono muted" style={{ fontSize: 10 }}>{p.runState === "Z" ? "not running" : "pid " + p.pid}</span>
                      <button className={"switch" + (draft.watched && draft.watched[p.name] ? " on" : "")} onClick={() => setDraft((d) => ({ ...d, watched: { ...d.watched, [p.name]: !d.watched[p.name] } }))}><i /></button>
                    </div>
                  ))}
                </div>
                <div className="metric-row"><span className="lbl" style={{ width: 150, flex: "0 0 150px" }}>Log watch regex</span><input className="agent-text" value={draft.logPat || ""} onChange={(e) => setDraft((d) => ({ ...d, logPat: e.target.value }))} /></div>
              </React.Fragment>
            )}
            {node.role === "monitor" && (
              <React.Fragment>
                <Slider label="Round interval" k="roundSec" min={10} max={120} unit="s" />
                <Slider label="Probes / round" k="perRound" min={1} max={20} unit="" />
                <Slider label="Repl factor hint" k="repl" min={1} max={5} unit="×" />
                <div className="agent-sect">Assigned probe targets <span className="muted">(HRW)</span></div>
                <div className="agent-apps">
                  {(!draft.targets || Object.keys(draft.targets).length === 0) && <div className="td-mono muted" style={{ fontSize: 12 }}>No targets assigned to this vantage.</div>}
                  {S.probes.filter((p) => draft.targets && p.targetId in draft.targets).map((p) => (
                    <div key={p.targetId} className="agent-app">
                      <span className="td-mono" style={{ fontWeight: 600, flex: 1, minWidth: 0, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{p.targetId}</span>
                      <span className="td-mono muted" style={{ fontSize: 10 }}>{p.label}</span>
                      <button className={"switch" + (draft.targets[p.targetId] ? " on" : "")} onClick={() => setDraft((d) => ({ ...d, targets: { ...d.targets, [p.targetId]: !d.targets[p.targetId] } }))}><i /></button>
                    </div>
                  ))}
                </div>
              </React.Fragment>
            )}
            {node.role === "server" && (
              <React.Fragment>
                <Slider label="Lease renew" k="renewSec" min={1} max={30} unit="s" />
                <Slider label="Lease TTL" k="ttlSec" min={5} max={60} unit="s" />
                <Slider label="DB pool size" k="pool" min={2} max={32} unit="" />
                <div className="agent-sect">Bind addresses</div>
                <div className="cfg-info"><span className="kpi__k">Ingest</span><span className="td-mono">0.0.0.0:{S.config.ingest.ports.ingest} · PULL</span></div>
                <div className="cfg-info"><span className="kpi__k">Survey</span><span className="td-mono">0.0.0.0:{S.config.ingest.ports.survey} · SURVEYOR</span></div>
                <div className="cfg-info"><span className="kpi__k">Publish</span><span className="td-mono">0.0.0.0:{S.config.ingest.ports.pub} · PUB</span></div>
              </React.Fragment>
            )}
          </div>
          <div className="modal__foot">
            <button className="btn-ghost" onClick={decommission} style={{ marginRight: "auto", color: "var(--crit)", borderColor: "var(--crit)" }}><Icon name="close" size={14} />Decommission</button>
            <button className="btn-ghost" onClick={onClose}>Cancel</button>
            <button className="btn-primary" onClick={push}><Icon name="check" size={14} />Push to node</button>
          </div>
        </div>
      </div>
    );
  }

  Object.assign(window, { Discovery, Provisioning, ConfigScreen });
})();
