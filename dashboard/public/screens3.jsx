/* ============================================================
   SolariNet — screens3: Discovery, Provisioning, Config & Rules
   ============================================================ */
(function () {
  const { useState, useEffect } = React;
  const Icon = window.Icon;
  const { StatusDot } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;
  const STATE_DOT = { up: "var(--ok)", degraded: "var(--warn)", down: "var(--crit)", unknown: "var(--unknown)" };
  const dotStyle = (state, sz) => ({ background: STATE_DOT[state] || "var(--unknown)", width: sz || 9, height: sz || 9, display: "inline-block", borderRadius: "50%" });

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
    const [adoptDisc, setAdoptDisc] = useState(null);   // discovered entity in the adopt dialog
    const items = S.discovered;
    const active = items.filter((d) => staged[d.host] !== "ignored");
    const byVia = {};
    items.forEach((d) => { byVia[d.via] = (byVia[d.via] || 0) + 1; });

    // "+ Monitor" opens the adopt dialog (pick services, heartbeat, name, pool).
    function stage(d) { setAdoptDisc(d); }
    // called by the dialog on a successful adopt
    function onAdopted(host) {
      setStaged((s) => ({ ...s, [host]: "staged" }));   // optimistic remove
      setAdoptDisc(null);
      refresh();
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

        {adoptDisc && <AdoptModal disc={adoptDisc} onClose={() => setAdoptDisc(null)}
                                  onAdopted={onAdopted} />}
      </div>
    );
  }

  /* ===================== ADOPT DIALOG ===================== */
  // Turn a discovered host into a monitored asset: choose which services to probe,
  // toggle the ICMP host heartbeat, name + classify it, and assign a pool.
  function AdoptModal({ disc, onClose, onAdopted }) {
    const pools = S.pools || [];
    const svcList = (disc.services || []).map(function (s) {
      const parts = String(s).split(":");
      return { label: String(s), port: parseInt(parts[1] || parts[0], 10) };
    }).filter(function (x) { return x.port > 0 && x.port <= 65535; });

    const [name, setName] = useState(disc.host || disc.ip || "");
    const [cls, setCls] = useState("server");
    const [poolId, setPoolId] = useState(pools.length ? String(pools[0].poolId) : "1");
    const [heartbeat, setHeartbeat] = useState(true);
    const [sel, setSel] = useState(function () { const m = {}; svcList.forEach(function (s) { m[s.port] = true; }); return m; });
    const [busy, setBusy] = useState(false);

    const fld = { width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 4, borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };
    const lbl = { fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, marginTop: 14, display: "block" };

    function submit() {
      const a = api();
      if (!a || !a.adoptDiscovered) { toast("Adopt unavailable (offline)", "close"); return; }
      const ports = svcList.filter(function (s) { return sel[s.port]; }).map(function (s) { return s.port; });
      setBusy(true);
      a.adoptDiscovered(disc.discId, {
        displayName: name.trim() || disc.ip,
        class: cls,
        poolId: Number(poolId),
        heartbeat: heartbeat,
        services: ports,
      }).then(function () {
        toast(`Adopted ${name.trim() || disc.ip} — ${ports.length + (heartbeat ? 1 : 0)} target(s)`, "shield");
        onAdopted(disc.host);
      }).catch(function (e) {
        toast(`Adopt failed: ${e && e.message || "error"}`, "close");
        setBusy(false);
      });
    }

    return (
      <div onClick={onClose} style={{ position: "fixed", inset: 0, zIndex: 1000, background: "rgba(0,0,0,0.55)", display: "flex", alignItems: "center", justifyContent: "center", padding: 20 }}>
        <div onClick={(e) => e.stopPropagation()} style={{ width: 460, maxHeight: "86vh", overflowY: "auto", background: "var(--panel, #0c1118)", border: "1px solid var(--line-glow, rgba(255,255,255,0.12))", borderRadius: 14, padding: 22, boxShadow: "0 16px 56px rgba(0,0,0,0.5)" }}>
          <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: 4 }}>
            <Icon name="shield" size={22} style={{ color: "var(--teal)" }} />
            <div style={{ fontWeight: 700, fontSize: 16 }}>Monitor system</div>
          </div>
          <div className="muted" style={{ fontSize: 12 }}>{disc.host} · {disc.ip}</div>

          <label style={lbl}>Display name</label>
          <input style={fld} value={name} onChange={(e) => setName(e.target.value)} placeholder={disc.ip} />

          <div style={{ display: "flex", gap: 12 }}>
            <div style={{ flex: 1 }}>
              <label style={lbl}>Classification</label>
              <select style={fld} value={cls} onChange={(e) => setCls(e.target.value)}>
                {["server", "appliance", "network", "iot", "host", "other"].map((c) => <option key={c} value={c}>{c}</option>)}
              </select>
            </div>
            <div style={{ flex: 1 }}>
              <label style={lbl}>Pool</label>
              <select style={fld} value={poolId} onChange={(e) => setPoolId(e.target.value)}>
                {pools.length === 0 && <option value="1">Unassigned</option>}
                {pools.map((p) => <option key={p.poolId} value={String(p.poolId)}>{p.name}</option>)}
              </select>
            </div>
          </div>

          <label style={lbl}>Services to monitor</label>
          {svcList.length === 0 && <div className="muted" style={{ fontSize: 12, marginTop: 4 }}>No TCP services discovered — heartbeat only.</div>}
          <div style={{ display: "flex", flexWrap: "wrap", gap: 8, marginTop: 6 }}>
            {svcList.map((s) => (
              <label key={s.port} style={{ display: "flex", alignItems: "center", gap: 6, padding: "5px 9px", borderRadius: 8, cursor: "pointer", border: "1px solid " + (sel[s.port] ? "var(--teal)" : "var(--line-glow, rgba(255,255,255,0.14))"), background: sel[s.port] ? "rgba(53,224,208,0.10)" : "transparent", fontSize: 12 }}>
                <input type="checkbox" checked={!!sel[s.port]} onChange={(e) => setSel(Object.assign({}, sel, { [s.port]: e.target.checked }))} />
                {s.label}
              </label>
            ))}
          </div>

          <label style={{ display: "flex", alignItems: "center", gap: 8, marginTop: 16, cursor: "pointer", fontSize: 13 }}>
            <input type="checkbox" checked={heartbeat} onChange={(e) => setHeartbeat(e.target.checked)} />
            Host heartbeat (ICMP ping) — monitor the system itself
          </label>

          <div style={{ display: "flex", justifyContent: "flex-end", gap: 10, marginTop: 22 }}>
            <button className="btn-ghost" onClick={onClose} disabled={busy}>Cancel</button>
            <button className="btn-primary" onClick={submit} disabled={busy}>
              <Icon name="plus" size={14} />{busy ? "Adopting…" : "Monitor system"}
            </button>
          </div>
        </div>
      </div>
    );
  }

  /* ===================== PROVISIONING ===================== */
  function Provisioning({ onOpenNode }) {
    const [enr, setEnr] = useState(S.enrollments);
    const [token, setToken] = useState(null);
    const drifted = S.nodes.filter((n) => !n.converged);
    const converged = S.nodes.length - drifted.length;

    // ---- remote client deployment ----
    const [dHost, setDHost] = useState("");
    const [dServer, setDServer] = useState("");
    const [dLog, setDLog] = useState("");
    const [dBusy, setDBusy] = useState(false);
    const stripAnsi = (s) => (s || "").replace(/\x1b\[[0-9;]*m/g, "");
    function pollLog(host, tries) {
      const a = api();
      if (!a || !a.deployLog) { setDBusy(false); return; }
      a.deployLog(host).then(function (r) {
        setDLog(stripAnsi(r.log || ""));
        if (r.done || r.failed || tries > 60) {
          setDBusy(false);
          if (r.done) { toast("Deploy complete — " + host, "check"); refresh(); }
          else if (r.failed) toast("Deploy failed — " + host, "close");
        } else {
          setTimeout(function () { pollLog(host, tries + 1); }, 3000);
        }
      }).catch(function () { setDBusy(false); });
    }
    function deploy() {
      const host = dHost.trim();
      if (!/^[A-Za-z0-9._@-]{1,120}$/.test(host)) { toast("Enter a target as user@host", "close"); return; }
      const a = api();
      if (!a || !a.deployClient) { toast("Deploy unavailable (offline)", "close"); return; }
      const body = { host: host };
      if (dServer.trim()) body.server = dServer.trim();
      setDBusy(true); setDLog("starting deploy…");
      a.deployClient(body).then(function () {
        toast("Deploying client to " + host, "provision");
        pollLog(host, 0);
      }).catch(function (e) {
        setDBusy(false); setDLog("");
        toast("Deploy failed: " + (e && e.message || "error"), "close");
      });
    }

    // ---- fleet provisioning (bare-metal OS install + Pi imaging) ----
    const [cat, setCat] = useState(null);
    const [fMode, setFMode] = useState("install");     // "install" | "image"
    const [fDistro, setFDistro] = useState("debian");
    const [fArch, setFArch] = useState("x86_64");
    const [fTarget, setFTarget] = useState("");
    const [fHostname, setFHostname] = useState("");
    const [fRole, setFRole] = useState("sensor");
    const [fProfile, setFProfile] = useState("standard");
    const [fLog, setFLog] = useState("");
    const [fBusy, setFBusy] = useState(false);
    useEffect(function () {
      const a = api();
      if (!a || !a.fleetCatalog) return undefined;
      let live = true;
      a.fleetCatalog().then(function (c) { if (live) setCat(c); }).catch(function () {});
      return function () { live = false; };
    }, []);
    // distros available for the chosen mode: image builds are Pi (raspios) only
    const fDistros = (cat && cat.distros ? cat.distros : []).filter(function (d) {
      return fMode === "image" ? d.install === "image" : d.install !== "image";
    });
    const fArches = (function () {
      const d = (cat && cat.distros || []).find(function (x) { return x.id === fDistro; });
      return (d && d.arches) || (fMode === "image" ? ["arm64", "arm32"] : ["x86_64", "arm64", "arm32"]);
    })();
    function pollProvision(hostname, isImage, tries) {
      const a = api();
      if (!a || !a.provisionLog) { setFBusy(false); return; }
      a.provisionLog(hostname, isImage).then(function (r) {
        setFLog(stripAnsi(r.log || ""));
        if (r.done || r.failed || tries > 200) {
          setFBusy(false);
          if (r.done) { toast((isImage ? "Image built — " : "Provisioning staged — ") + hostname, "check"); refresh(); }
          else if (r.failed) toast("Provisioning failed — " + hostname, "close");
        } else {
          setTimeout(function () { pollProvision(hostname, isImage, tries + 1); }, 3000);
        }
      }).catch(function () { setFBusy(false); });
    }
    function provisionSubmit() {
      const host = fHostname.trim();
      if (!/^[A-Za-z0-9._-]{1,160}$/.test(host)) { toast("Enter a valid hostname", "close"); return; }
      const a = api();
      const isImage = fMode === "image";
      if (!a || (isImage ? !a.buildImage : !a.provision)) { toast("Provisioning unavailable (offline)", "close"); return; }
      const body = { hostname: host, arch: fArch, distro: fDistro, role: fRole, profile: fProfile };
      if (!isImage) {
        if (!/^[A-Za-z0-9._:@-]{1,120}$/.test(fTarget.trim())) { toast("Enter the target MAC or host", "close"); return; }
        body.target = fTarget.trim();
      }
      setFBusy(true); setFLog(isImage ? "building image…" : "staging install…");
      (isImage ? a.buildImage(body) : a.provision(body)).then(function () {
        toast((isImage ? "Building image for " : "Staging install for ") + host, "provision");
        pollProvision(host, isImage, 0);
      }).catch(function (e) {
        setFBusy(false); setFLog("");
        toast("Failed: " + (e && e.message || "error"), "close");
      });
    }
    const fieldStyle = { flex: "1 1 160px", minWidth: 140, padding: "8px 10px", borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };

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

        <div className="panel" style={{ marginBottom: 18 }}>
          <div className="panel__head"><Icon name="provision" size={16} /><h3>Deploy client to a host</h3></div>
          <div className="panel__body">
            <div style={{ display: "flex", gap: 10, flexWrap: "wrap", alignItems: "center" }}>
              <input value={dHost} onChange={(e) => setDHost(e.target.value)} placeholder="user@host (e.g. pi@10.0.0.254)"
                onKeyDown={(e) => { if (e.key === "Enter") deploy(); }}
                style={{ flex: "1 1 220px", minWidth: 200, padding: "8px 10px", borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 }} />
              <input value={dServer} onChange={(e) => setDServer(e.target.value)} placeholder="server URL (optional)"
                style={{ flex: "1 1 240px", minWidth: 200, padding: "8px 10px", borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 }} />
              <button className="btn-primary" disabled={dBusy} onClick={deploy}>
                <Icon name="provision" size={14} />{dBusy ? "Deploying…" : "Deploy"}
              </button>
            </div>
            {dLog ? <pre style={{ marginTop: 12, maxHeight: 220, overflow: "auto", background: "rgba(0,0,0,0.30)", padding: 10, borderRadius: 8, fontSize: 11, lineHeight: 1.5, whiteSpace: "pre-wrap", fontFamily: "var(--mono)" }}>{dLog}</pre> : null}
            <div className="muted" style={{ fontSize: 11, marginTop: 8 }}>
              Needs key-based SSH + passwordless sudo on the target. The CA signs the client cert; arm hosts need a binary built via <code>deploy/cross-build-client.sh</code> first.
            </div>
          </div>
        </div>

        <div className="panel" style={{ marginBottom: 18 }}>
          <div className="panel__head">
            <Icon name="provision" size={16} /><h3>Provision a new system</h3>
            <div className="right" style={{ display: "flex", gap: 6 }}>
              <button className={fMode === "install" ? "chip on" : "chip"} onClick={() => setFMode("install")}>OS install</button>
              <button className={fMode === "image" ? "chip on" : "chip"} onClick={() => setFMode("image")}>Pi image</button>
            </div>
          </div>
          <div className="panel__body">
            <div style={{ display: "flex", gap: 10, flexWrap: "wrap", alignItems: "center" }}>
              <select value={fDistro} onChange={(e) => setFDistro(e.target.value)} style={fieldStyle}>
                {fDistros.length
                  ? fDistros.map((d) => <option key={d.id} value={d.id}>{d.label}</option>)
                  : <option value={fDistro}>{fDistro}</option>}
              </select>
              <select value={fArch} onChange={(e) => setFArch(e.target.value)} style={fieldStyle}>
                {fArches.map((a) => <option key={a} value={a}>{a}</option>)}
              </select>
              <select value={fRole} onChange={(e) => setFRole(e.target.value)} style={fieldStyle}>
                {(cat && cat.roles ? cat.roles : [{ id: "sensor", label: "Sensor" }]).map((r) => <option key={r.id} value={r.id}>{r.label}</option>)}
              </select>
              <select value={fProfile} onChange={(e) => setFProfile(e.target.value)} style={fieldStyle}>
                {(cat && cat.profiles ? cat.profiles : [{ id: "standard", label: "Standard" }]).map((p) => <option key={p.id} value={p.id}>{p.label}</option>)}
              </select>
            </div>
            <div style={{ display: "flex", gap: 10, flexWrap: "wrap", alignItems: "center", marginTop: 10 }}>
              {fMode === "install"
                ? <input value={fTarget} onChange={(e) => setFTarget(e.target.value)} placeholder="target MAC (aa:bb:cc:..) or host" style={fieldStyle} />
                : null}
              <input value={fHostname} onChange={(e) => setFHostname(e.target.value)} placeholder="new hostname"
                onKeyDown={(e) => { if (e.key === "Enter") provisionSubmit(); }} style={fieldStyle} />
              <button className="btn-primary" disabled={fBusy} onClick={provisionSubmit}>
                <Icon name="provision" size={14} />{fBusy ? (fMode === "image" ? "Building…" : "Staging…") : (fMode === "image" ? "Build image" : "Stage install")}
              </button>
            </div>
            {fLog ? <pre style={{ marginTop: 12, maxHeight: 220, overflow: "auto", background: "rgba(0,0,0,0.30)", padding: 10, borderRadius: 8, fontSize: 11, lineHeight: 1.5, whiteSpace: "pre-wrap", fontFamily: "var(--mono)" }}>{fLog}</pre> : null}
            <div className="muted" style={{ fontSize: 11, marginTop: 8 }}>
              {fMode === "install"
                ? <span>Renders an unattended install (same core package stack across distros) + stages a per-MAC netboot entry on <code>benzene</code>. The machine installs on its next network boot. <b>Live PXE activates once the DHCP switch is flipped</b> — until then, entries are staged.</span>
                : <span>Builds a customized Raspberry Pi OS image (hostname, SSH key, core stack, SolariNet enrollment) on <code>benzene</code> for flashing to SD/USB. Pi 5 can also native-netboot once PXE is live.</span>}
            </div>
          </div>
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

  /* ===================== POOL CARDS (top-level dashboard) ===================== */
  // Operator-defined functional pools with per-pool health rollups.
  function PoolCards({ onOpen }) {
    const pools = S.pools || [];
    if (!pools.length) return null;
    return (
      <div className="kpis" style={{ marginBottom: 18 }}>
        {pools.map(function (p) {
          const h = p.health || {};
          const total = p.assetCount || 0;
          const accent = p.color ||
            (h.down > 0 ? "var(--red)" : h.degraded > 0 ? "var(--amber)" : h.up > 0 ? "var(--ok)" : "var(--muted)");
          return (
            <div className="kpi" key={p.poolId}
                 style={{ cursor: onOpen ? "pointer" : "default", borderLeft: "3px solid " + accent }}
                 onClick={() => onOpen && onOpen(p)}>
              <div className="kpi__k">{p.name}</div>
              <div className="kpi__v">{total}</div>
              <div className="kpi__sub">
                {h.up > 0 && <span style={{ color: "var(--ok)" }}>{h.up} up </span>}
                {h.degraded > 0 && <span style={{ color: "var(--amber)" }}>{h.degraded} deg </span>}
                {h.down > 0 && <span style={{ color: "var(--red)" }}>{h.down} down </span>}
                {h.unknown > 0 && <span className="muted">{h.unknown} unknown</span>}
                {total === 0 && <span className="muted">no systems yet</span>}
              </div>
              <div className="kpi__bar" style={{ background: accent }} />
            </div>
          );
        })}
      </div>
    );
  }

  /* ===================== ASSETS (monitored systems) ===================== */
  function Assets({ onOpenNode }) {
    const [assets, setAssets] = useState(S.assets || []);
    const [, force] = useState(0);
    const pools = S.pools || [];
    const dot = { up: "var(--ok)", degraded: "var(--amber)", down: "var(--red)", unknown: "var(--muted)" };

    function newPool() {
      const name = window.prompt("New pool name (e.g. DNS, Core infra):", "");
      if (name == null || name.trim() === "") return;
      const a = api();
      if (!a || !a.createPool) { toast("Pool creation unavailable (offline)", "close"); return; }
      a.createPool({ name: name.trim() })
        .then(function () { toast("Pool created: " + name.trim(), "check"); return a.refresh ? a.refresh() : null; })
        .then(function () { force(function (n) { return n + 1; }); })
        .catch(function (e) { toast("Create failed: " + (e && e.message || "error"), "close"); });
    }

    function patch(asset, body, msg) {
      const a = api();
      if (!a || !a.updateAsset) { toast("Edit unavailable (offline)", "close"); return; }
      a.updateAsset(asset.assetId, body).then(function () {
        setAssets(function (prev) {
          return prev.map(function (x) { return x.assetId === asset.assetId ? Object.assign({}, x, body) : x; });
        });
        if (msg) toast(msg, "check");
      }).catch(function (e) { toast("Update failed: " + (e && e.message || "error"), "close"); });
    }
    function reassign(asset, poolId) {
      const pn = (pools.find(function (p) { return p.poolId === Number(poolId); }) || {}).name || null;
      patch(asset, { poolId: Number(poolId), poolName: pn }, `Moved ${asset.displayName} → ${pn}`);
    }
    function rename(asset) {
      const v = window.prompt("Rename system", asset.displayName);
      if (v != null && v.trim() !== "") patch(asset, { displayName: v.trim() }, "Renamed");
    }

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Systems</h1><div className="page-sub">{assets.length} monitored · {pools.length} pool(s)</div></div>
          <div className="page-head__right">
            <button className="btn-primary" onClick={newPool}><Icon name="plus" size={14} />New pool</button>
          </div>
        </div>
        <PoolCards />
        {assets.length === 0 && (
          <div className="muted" style={{ padding: "24px 4px" }}>No systems yet. Adopt hosts from <b>Discovery</b> to monitor them.</div>
        )}
        {assets.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr>
              <th>System</th><th>IP</th><th>Class</th><th>State</th><th>Targets</th><th>Pool</th><th>Heartbeat</th>
            </tr></thead>
            <tbody>
              {assets.map(function (as) {
                return (
                  <tr key={as.assetId}>
                    <td><span style={{ cursor: "pointer" }} onClick={() => rename(as)} title="Rename">{as.displayName} <Icon name="settings" size={11} style={{ opacity: 0.4, verticalAlign: "-1px" }} /></span></td>
                    <td className="td-mono muted">{as.ip}</td>
                    <td>{as.class}</td>
                    <td><span className="dot" style={{ background: dot[as.state] || "var(--muted)", width: 9, height: 9, display: "inline-block", borderRadius: "50%" }} /> {as.state}</td>
                    <td className="td-mono">{as.targetCount}</td>
                    <td>
                      <select value={String(as.poolId || "")} onChange={(e) => reassign(as, e.target.value)}
                              style={{ background: "rgba(255,255,255,0.04)", color: "inherit", border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", borderRadius: 6, padding: "3px 6px", fontFamily: "inherit", fontSize: 12 }}>
                        {pools.map((p) => <option key={p.poolId} value={String(p.poolId)}>{p.name}</option>)}
                      </select>
                    </td>
                    <td>
                      <button className={"switch" + (as.monitorHost ? " on" : "")} onClick={() => patch(as, { monitorHost: !as.monitorHost }, (as.monitorHost ? "Heartbeat off" : "Heartbeat on") + " — " + as.displayName)}><i /></button>
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table></div>
        )}
      </div>
    );
  }

  /* ===================== PER-SERVER (ASSET) DETAIL ===================== */
  function AssetDetail({ assetId, onBack, onOpenService, toast }) {
    const [data, setData] = useState(null);
    const [err, setErr] = useState(null);
    const pools = S.pools || [];

    function load() {
      const a = api();
      if (a && a.asset) {
        a.asset(assetId).then(setData).catch((e) => setErr((e && e.message) || "load failed"));
      } else {
        const found = (S.assets || []).find((x) => x.assetId === assetId);
        setData(found ? Object.assign({ targets: [] }, found) : null);
      }
    }
    useEffect(load, [assetId]);

    function patch(body, msg) {
      const a = api();
      if (!a || !a.updateAsset) { toast && toast("Edit unavailable (offline)", "close"); return; }
      a.updateAsset(assetId, body).then(() => { toast && toast(msg || "Updated", "check"); load(); if (a.refresh) a.refresh(); })
        .catch((e) => toast && toast("Update failed: " + (e && e.message || "error"), "close"));
    }
    function rename() { const v = window.prompt("Rename system", data.displayName); if (v != null && v.trim() !== "") patch({ displayName: v.trim() }, "Renamed"); }

    if (err) return (<div className="page"><button className="btn-ghost" onClick={onBack}><Icon name="chevronLeft" size={14} />Back</button><div className="muted" style={{ marginTop: 16 }}>Couldn't load system: {err}</div></div>);
    if (!data) return (<div className="page"><div className="muted">Loading…</div></div>);
    const targets = data.targets || [];
    const sel = { background: "rgba(255,255,255,0.04)", color: "inherit", border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", borderRadius: 6, padding: "3px 6px", fontFamily: "inherit", fontSize: 13 };

    return (
      <div className="page">
        <button className="btn-ghost" onClick={onBack} style={{ marginBottom: 12 }}><Icon name="chevronLeft" size={14} />Back</button>
        <div className="page-head">
          <div>
            <h1 className="page-title"><span style={dotStyle(data.state, 11)} />&nbsp;{data.displayName} <Icon name="settings" size={13} style={{ opacity: 0.4, cursor: "pointer" }} onClick={rename} /></h1>
            <div className="page-sub">{data.ip}{data.host ? " · " + data.host : ""} · {data.class} · {data.poolName || "Unassigned"}</div>
          </div>
        </div>

        <div className="kpis" style={{ marginBottom: 18 }}>
          <div className="kpi"><div className="kpi__k">State</div><div className="kpi__v" style={{ color: STATE_DOT[data.state] }}>{data.state}</div></div>
          <div className="kpi"><div className="kpi__k">Targets</div><div className="kpi__v">{targets.length}</div></div>
          <div className="kpi"><div className="kpi__k">Class</div><div className="kpi__v" style={{ fontSize: 15 }}>
            <select value={data.class} onChange={(e) => patch({ class: e.target.value }, "Reclassified")} style={sel}>
              {["server", "appliance", "network", "iot", "host", "other"].map((c) => <option key={c} value={c}>{c}</option>)}
            </select></div></div>
          <div className="kpi"><div className="kpi__k">Pool</div><div className="kpi__v" style={{ fontSize: 15 }}>
            <select value={String(data.poolId || "")} onChange={(e) => patch({ poolId: Number(e.target.value) }, "Moved pool")} style={sel}>
              {pools.map((p) => <option key={p.poolId} value={String(p.poolId)}>{p.name}</option>)}
            </select></div></div>
          <div className="kpi"><div className="kpi__k">Heartbeat</div><div className="kpi__v">
            <button className={"switch" + (data.monitorHost ? " on" : "")} onClick={() => patch({ monitorHost: !data.monitorHost }, data.monitorHost ? "Heartbeat off" : "Heartbeat on")}><i /></button></div></div>
        </div>

        <div className="page-sub" style={{ margin: "4px 0 10px" }}>Monitored targets — click for per-service detail</div>
        {targets.length === 0 && <div className="muted">No targets yet. Enable the heartbeat or adopt services for this system.</div>}
        {targets.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr><th>Target</th><th>Proto</th><th>Port</th><th>State</th><th>Vantages</th></tr></thead>
            <tbody>
              {targets.map((t) => (
                <tr key={t.targetId} style={{ cursor: "pointer" }} onClick={() => onOpenService(t.targetId)}>
                  <td>{t.label || t.targetId}</td>
                  <td>{t.proto}</td>
                  <td className="td-mono">{t.port || "—"}</td>
                  <td><span style={dotStyle(t.state)} /> {t.state}</td>
                  <td className="td-mono">{t.vantages}</td>
                </tr>
              ))}
            </tbody>
          </table></div>
        )}
        <div className="muted" style={{ marginTop: 18, fontSize: 12 }}>
          {data.nodeId ? "Agent linked — host metrics available." : "No agent deployed on this system — reachability only. Deploy a SolariNet client to collect CPU / RAM / disk metrics."}
        </div>
      </div>
    );
  }

  /* ===================== PER-SERVICE (TARGET) DETAIL ===================== */
  function ServiceDetail({ targetId, onBack }) {
    const probe = (S.probes || []).find((p) => p.targetId === targetId);
    if (!probe) return (<div className="page"><button className="btn-ghost" onClick={onBack}><Icon name="chevronLeft" size={14} />Back</button><div className="muted" style={{ marginTop: 16 }}>Target not found.</div></div>);
    const rtt = (us) => (fmt && fmt.rtt) ? fmt.rtt(us) : (us ? (us / 1000).toFixed(1) + " ms" : "—");
    return (
      <div className="page">
        <button className="btn-ghost" onClick={onBack} style={{ marginBottom: 12 }}><Icon name="chevronLeft" size={14} />Back</button>
        <div className="page-head">
          <div>
            <h1 className="page-title"><span style={dotStyle(probe.state, 11)} />&nbsp;{probe.label || probe.targetId}</h1>
            <div className="page-sub">{probe.proto}{probe.port ? ":" + probe.port : ""} · {probe.host} · state {probe.state}</div>
          </div>
        </div>
        <div className="page-sub" style={{ margin: "4px 0 10px" }}>Vantages ({probe.vantages.length})</div>
        {probe.vantages.length === 0 && <div className="muted">No monitor has probed this target yet. Deploy or assign a monitor to collect reachability, RTT, and loss.</div>}
        {probe.vantages.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr><th>Monitor</th><th>Outcome</th><th>RTT</th><th>Jitter</th><th>Loss</th><th>Seen</th></tr></thead>
            <tbody>
              {probe.vantages.map((v, i) => (
                <tr key={i}>
                  <td>{v.monitorName || v.monitorNode}</td>
                  <td><span style={{ background: v.outcome === "ok" ? "var(--ok)" : "var(--crit)", width: 9, height: 9, display: "inline-block", borderRadius: "50%" }} /> {v.outcome}</td>
                  <td className="td-mono">{rtt(v.rttMicros)}</td>
                  <td className="td-mono">{(v.jitterMicros / 1000).toFixed(1)} ms</td>
                  <td className="td-mono">{(v.lossPermille / 10).toFixed(1)}%</td>
                  <td className="td-mono muted">{v.sampledMin}m ago</td>
                </tr>
              ))}
            </tbody>
          </table></div>
        )}
      </div>
    );
  }

  Object.assign(window, { Discovery, Provisioning, ConfigScreen, PoolCards, Assets, AssetDetail, ServiceDetail });
})();
