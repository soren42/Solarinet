/* ============================================================
   SolariNet — screens3: Discovery, Provisioning, Config & Rules
   ============================================================ */
(function () {
  const { useState, useEffect } = React;
  const Icon = window.Icon;
  const { StatusDot, DangerModal } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;
  const STATE_DOT = { up: "var(--ok)", degraded: "var(--warn)", down: "var(--crit)", unknown: "var(--unknown)" };
  const dotStyle = (state, sz) => ({ background: STATE_DOT[state] || "var(--unknown)", width: sz || 9, height: sz || 9, display: "inline-block", borderRadius: "50%" });

  function toast(msg, icon) { if (window.__solariToast) window.__solariToast(msg, icon); }
  // adapter handle (live mutations); null/offline-safe
  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function refresh() { const a = api(); if (a && a.refresh) a.refresh().catch(function () {}); }

  /* ===================== DISCOVERY ===================== */
  // A discovered service entry may be the legacy string form ("ssh:22") or the
  // enriched object form ({port,name,product,version}) once the C fingerprinter
  // lands. These read either shape without throwing.
  function svcName(s) {
    if (s && typeof s === "object") return s.name || s.service || ("port " + (s.port || "?"));
    var str = String(s), c = str.lastIndexOf(":");
    return c > 0 ? str.slice(0, c) : str;
  }
  function svcPort(s) {
    if (s && typeof s === "object") return parseInt(s.port, 10) || 0;
    var parts = String(s).split(":");
    return parseInt(parts[1] || parts[0], 10) || 0;
  }
  // product/version fingerprint (object form only) — shown as a tooltip + sublabel.
  function svcDetail(s) {
    if (!s || typeof s !== "object") return "";
    return [s.product, s.version].filter(Boolean).join(" ");
  }
  // A host learned over mDNS/zeroconf (or with a *.local name, an mDNS record, or
  // advertised service types) gets an origin chip. Matches "mdns"/"mDNS" via.
  function isMdns(d) {
    return /mdns/i.test(String(d.via || ""))
      || !!d.mdnsName
      || (Array.isArray(d.mdnsServices) && d.mdnsServices.length > 0)
      || (typeof d.host === "string" && /\.local$/i.test(d.host));
  }

  // Normalise seenCount + mac + mdns into small render helpers used by the table.
  const numOr0 = (v) => (typeof v === "number" && isFinite(v) ? v : 0);
  // "3m ago" / "2h 04m ago" / "—" from a minutes-ago integer.
  function seenAgo(mins) {
    const m = numOr0(mins);
    if (!m) return "just now";
    if (m < 60) return m + "m ago";
    const h = Math.floor(m / 60);
    return h + "h " + String(m % 60).padStart(2, "0") + "m ago";
  }
  // A compact "sw-core-01 · Gi1/0/12" neighbor label, or null.
  function neighborLabel(n) {
    if (!n) return null;
    const parts = [];
    if (n.gearName) parts.push(n.gearName);
    if (n.peerPort) parts.push(n.peerPort);
    else if (n.localIf) parts.push(n.localIf);
    return parts.length ? parts.join(" · ") : null;
  }
  const ROLE_TONE = {
    server: "var(--teal)", nas: "var(--violet)", printer: "var(--warn)",
    camera: "var(--warn)", iot: "var(--warn)", network: "var(--violet)",
    ap: "var(--violet)", gateway: "var(--violet)", workstation: "var(--teal)",
    host: "var(--teal)",
  };

  function Discovery({ onOpenNode }) {
    const [staged, setStaged] = useState({});      // host -> "staged" | "ignored"
    const [auto, setAuto] = useState(!!S.config.autoDiscover);

    // Full candidate list. Boot with the fixture/boot slice (S.discovered), then
    // pull the complete set (every status) from /api/discovery so the screen can
    // filter across new/adopted/ignored without a reload. Offline → keep S.
    const [rows, setRows] = useState(S.discovered || []);
    useEffect(function () {
      const a = api();
      if (!a || !a.listDiscovered) return;
      let live = true;
      a.listDiscovered("all").then(function (list) {
        if (live && Array.isArray(list) && list.length) setRows(list);
      }).catch(function () { /* keep the boot slice */ });
      return function () { live = false; };
    }, []);

    // Persist the toggle: POST /api/config with the full effective global config
    // (same body shape ConfigScreen saves) so it survives reloads. On load the
    // useState above reads the persisted value back from S.config.
    function toggleAuto() {
      const next = !auto;
      setAuto(next);   // optimistic
      const a = api();
      if (a && a.saveConfig) {
        a.saveConfig(Object.assign({}, S.config, { autoDiscover: next })).then(function () {
          toast(`Auto-discovery ${next ? "resumed" : "paused"}`, next ? "check" : "close");
          refresh();
        }).catch(function (e) {
          setAuto(!next);   // revert — it didn't stick
          toast(`Auto-discovery save failed: ${e && e.message || "error"}`, "close");
        });
      } else {
        toast(`Auto-discovery ${next ? "resumed" : "paused"} (offline — not persisted)`, next ? "check" : "close");
      }
    }
    const [cidr, setCidr] = useState("");
    const [scanning, setScanning] = useState(false);
    const [adoptDisc, setAdoptDisc] = useState(null);   // discovered entity in the adopt dialog

    // ---- filter / search / sort state ----
    const [q, setQ] = useState("");
    const [statusFilter, setStatusFilter] = useState("new");  // new | adopted | ignored | all
    const [viaFilter, setViaFilter] = useState("all");
    const [sort, setSort] = useState({ key: "seen", dir: 1 });

    // Effective status after the local optimistic overlay (ignore/stage).
    function effStatus(d) {
      const ov = staged[d.host];
      if (ov === "ignored") return "ignored";
      if (ov === "staged") return "adopted";
      return d.status || "new";
    }

    // via-method breakdown (raw via string is consistent within one data source).
    const byVia = {};
    rows.forEach((d) => { if (d.via) byVia[d.via] = (byVia[d.via] || 0) + 1; });
    const viaKeys = Object.keys(byVia).sort();

    const statusCounts = { new: 0, adopted: 0, ignored: 0, all: rows.length };
    rows.forEach((d) => { const s = effStatus(d); if (statusCounts[s] != null) statusCounts[s]++; });

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

    // ---- filter → search → sort pipeline ----
    const ql = q.trim().toLowerCase();
    let view = rows.filter(function (d) {
      if (statusFilter !== "all" && effStatus(d) !== statusFilter) return false;
      if (viaFilter !== "all" && d.via !== viaFilter) return false;
      if (!ql) return true;
      // Search covers vendor / service / role / segment plus identity + SNMP.
      const svcHay = (d.services || []).map(function (s) { return svcName(s); }).join(" ");
      const hay = [
        d.host, d.ip, d.mac, d.vendor, d.osName, d.deviceRole, d.sysDescr,
        d.mdnsName, (d.mdnsServices || []).join(" "), svcHay, d.seg, d.via,
        d.neighbor && d.neighbor.gearName, d.neighbor && d.neighbor.peerPort,
      ].filter(Boolean).join(" ").toLowerCase();
      return hay.indexOf(ql) >= 0;
    });
    function keyOf(d) {
      switch (sort.key) {
        case "host": return (d.host || d.ip || "").toLowerCase();
        case "vendor": return (d.vendor || "").toLowerCase();
        case "os": return (d.osName || "").toLowerCase();
        case "role": return (d.deviceRole || "").toLowerCase();
        case "seg": return (d.seg || "").toLowerCase();
        case "via": return (d.via || "").toLowerCase();
        case "neighbor": return ((d.neighbor && d.neighbor.gearName) || "").toLowerCase();
        case "seen": return numOr0(d.seen);
        default: return 0;
      }
    }
    view = view.slice().sort(function (a, b) {
      const ka = keyOf(a), kb = keyOf(b);
      if (ka < kb) return -sort.dir;
      if (ka > kb) return sort.dir;
      return 0;
    });
    function th(key, label, right) {
      const on = sort.key === key;
      return <th style={{ cursor: "pointer", textAlign: right ? "right" : undefined, whiteSpace: "nowrap" }}
        onClick={function () { setSort(function (s) { return { key: key, dir: s.key === key ? -s.dir : 1 }; }); }}>
        {label}{on ? <span style={{ color: "var(--teal)", marginLeft: 4 }}>{sort.dir > 0 ? "▲" : "▼"}</span> : null}
      </th>;
    }

    const STATUS_CHIPS = [
      { k: "new", label: "New" },
      { k: "adopted", label: "Adopted" },
      { k: "ignored", label: "Ignored" },
      { k: "all", label: "All" },
    ];

    return (
      <div className="page">
        <div className="page-head">
          <div><h1 className="page-title">Discovery</h1><div className="page-sub">{statusCounts.new} candidates · {rows.length} assets seen</div></div>
          <div className="page-head__right">
            <input value={cidr} onChange={(e) => setCidr(e.target.value)} placeholder="192.168.1.0/24"
              onKeyDown={(e) => { if (e.key === "Enter") scanNow(); }}
              style={{ padding: "7px 10px", borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13, width: 150 }} />
            <button className="btn-primary" disabled={scanning} onClick={scanNow}>
              <Icon name="discovery" size={14} />{scanning ? "Scanning…" : "Scan"}
            </button>
            <div className="chip" onClick={toggleAuto}>
              <button className={"switch" + (auto ? " on" : "")} style={{ pointerEvents: "none" }}><i /></button>Auto-discover
            </div>
          </div>
        </div>

        <div className="kpis">
          <div className="kpi teal"><div className="kpi__k">Candidates</div><div className="kpi__v">{statusCounts.new}</div><div className="kpi__sub">awaiting decision</div><div className="kpi__bar" /></div>
          <div className="kpi ok"><div className="kpi__k">Adopted</div><div className="kpi__v">{statusCounts.adopted}</div><div className="kpi__sub">now monitored</div><div className="kpi__bar" /></div>
          {viaKeys.slice(0, 3).map((via) => (
            <div className="kpi" key={via}><div className="kpi__k">{via}</div><div className="kpi__v" style={{ color: "var(--violet)" }}>{byVia[via]}</div><div className="kpi__sub">via this method</div><div className="kpi__bar" style={{ background: "var(--violet)" }} /></div>
          ))}
        </div>

        {/* status + via filters, then search */}
        <div className="filters">
          {STATUS_CHIPS.map((c) => (
            <button key={c.k} className={"chip" + (statusFilter === c.k ? " on" : "")} onClick={() => setStatusFilter(c.k)}>
              {c.label}<span className="chip__n">{statusCounts[c.k]}</span>
            </button>
          ))}
          <span style={{ width: 1, height: 20, background: "var(--line)", margin: "0 2px" }} />
          <button className={"chip" + (viaFilter === "all" ? " on" : "")} onClick={() => setViaFilter("all")}>All vias</button>
          {viaKeys.map((via) => (
            <button key={via} className={"chip" + (viaFilter === via ? " on" : "")} onClick={() => setViaFilter(via)}>
              {via}<span className="chip__n">{byVia[via]}</span>
            </button>
          ))}
          <div className="search" style={{ marginLeft: "auto", maxWidth: 280, height: 36 }}>
            <Icon name="search" size={15} />
            <input value={q} onChange={(e) => setQ(e.target.value)} placeholder="vendor · service · role · segment" />
          </div>
        </div>

        {view.length === 0 && <div className="empty">No assets match these filters.</div>}
        {view.length > 0 && (
          <div className="tablewrap" style={{ overflowX: "auto" }}>
            <table className="grid">
              <thead><tr>
                {th("host", "Host / IP")}
                {th("vendor", "Vendor · MAC")}
                {th("os", "OS · Role")}
                <th style={{ whiteSpace: "nowrap" }}>mDNS &amp; services</th>
                <th style={{ whiteSpace: "nowrap" }}>SNMP sysDescr</th>
                {th("neighbor", "Neighbor")}
                {th("via", "Via · Seg")}
                {th("seen", "Seen", false)}
                <th aria-label="actions" />
              </tr></thead>
              <tbody>
                {view.map((d) => {
                  const st = effStatus(d);
                  const nbr = neighborLabel(d.neighbor);
                  const mdns = isMdns(d);
                  return (
                    <tr key={d.discId != null ? "d" + d.discId : d.host} style={{ cursor: "default" }}>
                      {/* Host / IP + kind + mDNS origin */}
                      <td>
                        <div style={{ display: "flex", alignItems: "center", gap: 7 }}>
                          <Icon name={d.kind === "service" ? "link" : "host"} size={15} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                          <span className="td-mono" style={{ fontWeight: 600 }}>{d.host}</span>
                          {mdns && <span className="svc-chip" style={{ borderColor: "var(--violet)", color: "var(--violet)" }}>mDNS</span>}
                        </div>
                        <div className="td-mono muted" style={{ fontSize: 11, marginTop: 2, marginLeft: 22 }}>{d.ip}{d.kind ? " · " + d.kind : ""}</div>
                      </td>

                      {/* Vendor · MAC */}
                      <td>
                        <div style={{ fontSize: 12.5 }}>{d.vendor || <span className="muted">—</span>}</div>
                        {d.mac && <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 2 }}>{d.mac}</div>}
                      </td>

                      {/* OS · Role */}
                      <td>
                        <div style={{ fontSize: 12.5 }}>{d.osName || <span className="muted">—</span>}</div>
                        {d.deviceRole && <span className="svc-chip" style={{ borderColor: ROLE_TONE[d.deviceRole] || "var(--teal)", color: ROLE_TONE[d.deviceRole] || "var(--teal)", marginTop: 3, display: "inline-block" }}>{d.deviceRole}</span>}
                      </td>

                      {/* mDNS name + mdns service chips + TCP service chips */}
                      <td>
                        {d.mdnsName && <div className="td-mono muted" style={{ fontSize: 10.5, marginBottom: 3 }}>{d.mdnsName}</div>}
                        <div style={{ display: "flex", gap: 5, flexWrap: "wrap", maxWidth: 320 }}>
                          {(d.mdnsServices || []).map((s, i) => (
                            <span key={"m" + i} className="svc-chip" style={{ borderColor: "var(--violet)", color: "var(--violet)" }} title={"mDNS service · " + s}>{s}</span>
                          ))}
                          {(d.services || []).map((s, i) => {
                            const det = svcDetail(s);
                            return (
                              <span key={"s" + i} className="svc-chip" title={det ? svcName(s) + " · " + det : undefined}>
                                {svcName(s)}{svcPort(s) ? ":" + svcPort(s) : ""}
                              </span>
                            );
                          })}
                          {(!d.mdnsServices || !d.mdnsServices.length) && (!d.services || !d.services.length) && <span className="muted">—</span>}
                        </div>
                      </td>

                      {/* SNMP sysDescr (truncated, full on hover) */}
                      <td style={{ maxWidth: 260, whiteSpace: "normal" }}>
                        {d.sysDescr
                          ? <div className="td-mono muted" style={{ fontSize: 10.5, display: "-webkit-box", WebkitLineClamp: 2, WebkitBoxOrient: "vertical", overflow: "hidden" }} title={d.sysDescr}>{d.sysDescr}</div>
                          : <span className="muted">—</span>}
                      </td>

                      {/* LLDP / topology neighbor */}
                      <td>
                        {nbr
                          ? <span title={d.neighbor && d.neighbor.viaLldp ? "via LLDP" : "via uplink"}>
                              <span className="td-mono" style={{ fontSize: 11.5 }}>{nbr}</span>
                              {d.neighbor && d.neighbor.viaLldp && <span className="svc-chip" style={{ borderColor: "var(--teal)", color: "var(--teal)", marginLeft: 6 }}>LLDP</span>}
                            </span>
                          : <span className="muted">—</span>}
                      </td>

                      {/* Via · Seg */}
                      <td>
                        <span className="tag">{d.via}</span>
                        {d.seg && <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 3 }}>{d.seg}{d.arch ? " · " + d.arch : ""}</div>}
                      </td>

                      {/* Seen */}
                      <td className="td-mono muted" style={{ fontSize: 11.5 }}>{seenAgo(d.seen)}</td>

                      {/* actions / status */}
                      <td style={{ textAlign: "right" }}>
                        {st === "new" ? (
                          <div className="disc-actions" style={{ justifyContent: "flex-end" }}>
                            <button className="btn-ghost" onClick={() => ignore(d)}>Ignore</button>
                            <button className="btn-primary" onClick={() => stage(d)}><Icon name="plus" size={14} />Monitor</button>
                          </div>
                        ) : st === "ignored" ? (
                          <span className="alert-sev" style={{ opacity: 0.7 }}>ignored</span>
                        ) : (
                          <span className="alert-sev info"><Icon name="check" size={12} style={{ verticalAlign: "-2px" }} /> adopted</span>
                        )}
                      </td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
          </div>
        )}

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
    // Accept both the legacy "ssh:22" string form and the enriched object form
    // ({port,name,product,version}); svcName/svcPort/svcDetail normalise either.
    const svcList = (disc.services || []).map(function (s) {
      const det = svcDetail(s);
      return { label: svcName(s) + (svcPort(s) ? ":" + svcPort(s) : "") + (det ? " (" + det + ")" : ""), port: svcPort(s) };
    }).filter(function (x) { return x.port > 0 && x.port <= 65535; });

    // Map the discovered deviceRole (nmap/SNMP/mDNS inference) onto the adopt
    // classification taxonomy so the operator gets a sensible pre-selection.
    const ROLE_TO_CLASS = {
      server: "server", workstation: "host", host: "host",
      printer: "appliance", nas: "appliance",
      camera: "iot", iot: "iot",
      network: "network", ap: "network", gateway: "network",
    };
    const [name, setName] = useState(disc.host || disc.ip || "");
    const [cls, setCls] = useState(ROLE_TO_CLASS[disc.deviceRole] || "server");
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
    const [driftAll, setDriftAll] = useState(false);   // "+N more" expander for the drift list

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
      if (!a || (isImage ? !a.buildImage : !a.fleetProvision)) { toast("Provisioning unavailable (offline)", "close"); return; }
      const body = { hostname: host, arch: fArch, distro: fDistro, role: fRole, profile: fProfile };
      if (!isImage) {
        if (!/^[A-Za-z0-9._:@-]{1,120}$/.test(fTarget.trim())) { toast("Enter the target MAC or host", "close"); return; }
        body.target = fTarget.trim();
      }
      setFBusy(true); setFLog(isImage ? "building image…" : "staging install…");
      (isImage ? a.buildImage(body) : a.fleetProvision(body)).then(function () {
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
                {(driftAll ? drifted : drifted.slice(0, 8)).map((n) => (
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
                {drifted.length > 8 && (
                  <button className="btn-ghost" style={{ margin: 12 }} onClick={() => setDriftAll((v) => !v)}>
                    {driftAll ? "Show fewer" : `+${drifted.length - 8} more drifted node${drifted.length - 8 === 1 ? "" : "s"}`}
                  </button>
                )}
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
  /* Identity & SSO — read-only surface over /api/auth/config (OIDC) + the current
     session's directory state. Gated: renders a muted note if the config read
     fails, so it never disrupts the Config screen. No secrets are shown. */
  function IdentityPanel() {
    const [cfg, setCfg] = useState(null);
    const [err, setErr] = useState(false);
    useEffect(function () {
      const a = api();
      if (!a || !a.authConfig) { setErr(true); return; }
      a.authConfig().then(function (c) { setCfg(c || {}); }).catch(function () { setErr(true); });
    }, []);
    const op = (S.operator) || null;
    const dir = (cfg && cfg.directory) || {};
    const Chip = function (props) {
      const on = props.on;
      return <span className="tag" style={{ color: on ? "var(--ok)" : "var(--unknown)", borderColor: on ? "var(--ok)" : "var(--line)" }}>{on ? "enabled" : "disabled"}</span>;
    };
    const Info = function (props) {
      return <div className="cfg-info"><span className="kpi__k">{props.k}</span><span className="td-mono" style={{ maxWidth: 190, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={props.v}>{props.v}</span></div>;
    };
    return (
      <div className="panel">
        <div className="panel__head"><Icon name="shield" size={16} /><h3>Identity & SSO</h3><span className="tag">read-only</span></div>
        <div className="panel__body">
          {err && <div className="muted" style={{ fontSize: 12 }}>Identity config unavailable.</div>}
          {!err && (
            <>
              <div className="kpi__k" style={{ margin: "2px 0 6px", display: "flex", alignItems: "center", gap: 8 }}>
                Single sign-on (OIDC) <Chip on={cfg && cfg.oidcEnabled} />
              </div>
              {cfg && cfg.oidcEnabled ? (
                <>
                  <Info k="Provider" v={cfg.oidcProvider || "OpenID Connect"} />
                  {cfg.oidcIssuer ? <Info k="Issuer" v={cfg.oidcIssuer} /> : null}
                  {cfg.oidcClientId ? <Info k="Client" v={cfg.oidcClientId} /> : null}
                  {cfg.oidcScopes ? <Info k="Scopes" v={cfg.oidcScopes} /> : null}
                  <a href={cfg.oidcLoginUrl || "/api/auth/oidc/login"} className="backbtn" style={{ marginTop: 10, display: "inline-flex" }}>
                    <Icon name="enter" size={13} />Test sign-in
                  </a>
                </>
              ) : (
                <div className="muted" style={{ fontSize: 11 }}>
                  Configure the <code>oidc</code> block in <code>solari-auth.json</code> (issuer, clientId, clientSecret, redirectUri) to enable SSO.
                </div>
              )}
              <div className="divider" />
              <div className="kpi__k" style={{ margin: "2px 0 6px", display: "flex", alignItems: "center", gap: 8 }}>
                Directory (AD / LDAP) <Chip on={dir.enabled} />
              </div>
              {dir.enabled ? (
                <>
                  {dir.type ? <Info k="Type" v={dir.type} /> : null}
                  {dir.realm ? <Info k="Realm" v={dir.realm} /> : null}
                  {dir.domain ? <Info k="Domain" v={dir.domain} /> : null}
                  {dir.host ? <Info k="Host" v={dir.host} /> : null}
                </>
              ) : (
                <div className="muted" style={{ fontSize: 11 }}>Directory bind not configured; local + SSO logins only.</div>
              )}
              {op && (
                <>
                  <div className="divider" />
                  <Info k="This session" v={(op.displayName || op.name) + " · " + (op.source || "local")} />
                </>
              )}
            </>
          )}
        </div>
      </div>
    );
  }

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
          <div><h1 className="page-title">Config & Rules</h1><div className="page-sub">effective global config · save & push as SCP control frames · CA / transport are server.conf (read-only)</div></div>
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
                <div className="kpi__k" style={{ margin: "2px 0 4px" }}>Internal CA <span className="tag" style={{ marginLeft: 6 }}>read-only</span></div>
                <div className="cfg-info"><span className="kpi__k">Issuer</span><span className="td-mono">{cfg.ca.issuer}</span></div>
                <div className="cfg-info"><span className="kpi__k">Cert TTL</span><span className="td-mono">{cfg.ca.certTtlDays} days</span></div>
                <div className="cfg-info"><span className="kpi__k">Enroll method</span><span className="td-mono">{cfg.ca.enroll}</span></div>
                <div className="muted" style={{ fontSize: 11, marginTop: 8 }}>
                  Set in <code>server.conf</code> — edit on the host + restart the server. Not editable here.
                </div>
              </div>
            </div>
            <div className="panel">
              <div className="panel__head"><Icon name="network" size={16} /><h3>Transport</h3><span className="tag">read-only</span><div className="right">{cfg.ingest.tls}</div></div>
              <div className="panel__body">
                <div className="cfg-info"><span className="kpi__k">Ingest</span><span className="td-mono">:{cfg.ingest.ports.ingest} · PULL</span></div>
                <div className="cfg-info"><span className="kpi__k">Survey</span><span className="td-mono">:{cfg.ingest.ports.survey} · SURVEYOR</span></div>
                <div className="cfg-info"><span className="kpi__k">Publish</span><span className="td-mono">:{cfg.ingest.ports.pub} · PUB</span></div>
                <div className="muted" style={{ fontSize: 11, marginTop: 8 }}>
                  Bind ports + TLS come from <code>server.conf</code> — edit on the host + restart the server. Not editable here.
                </div>
              </div>
            </div>
            <IdentityPanel />
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
    const [showRetired, setShowRetired] = useState(false);   // retired agents hidden by default
    const [modalId, setModalId] = useState(null);
    const retiredCount = S.nodes.filter((n) => n.state === "retired").length;
    const base = S.nodes.filter((n) => showRetired || n.state !== "retired");
    const pool = base.filter((n) => n.role === type);
    const list = pool.filter((n) => (!q || n.name.includes(q.toLowerCase()) || (n.ip || "").includes(q)) && (!driftOnly || !n.converged));
    const drift = pool.filter((n) => !n.converged).length;
    const modalNode = modalId ? S.nodes.find((n) => n.nodeId === modalId) : null;
    const TYPE_ICON = { server: "server", monitor: "monitor", client: "host" };
    const TYPE_LABEL = { server: "Server", monitor: "Network", client: "Application" };

    return (
      <div>
        <div className="filters">
          {["server", "monitor", "client"].map((t) => (
            <button key={t} className={"chip" + (type === t ? " on" : "")} onClick={() => setType(t)}><Icon name={TYPE_ICON[t]} size={14} />{TYPE_LABEL[t]}<span className="chip__n">{base.filter((n) => n.role === t).length}</span></button>
          ))}
          <div style={{ width: 1, height: 24, background: "var(--line)", margin: "0 4px" }} />
          <button className={"chip" + (driftOnly ? " on" : "")} onClick={() => setDriftOnly((d) => !d)}><span className="dot degraded" />Drift only<span className="chip__n">{drift}</span></button>
          {retiredCount > 0 && (
            <button className={"chip" + (showRetired ? " on" : "")} onClick={() => setShowRetired((v) => !v)}
              title={showRetired ? "Hide retired agents" : "Show retired agents"}>
              <span className="dot unknown" />Retired<span className="chip__n">{retiredCount}</span>
            </button>
          )}
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
    const [loaded, setLoaded] = useState(false);
    const TYPE_ICON = { server: "server", monitor: "monitor", client: "host" };
    // Load the node's ACTUAL persisted config so the editor reflects reality,
    // not synthetic defaults. Merge the stored configBlob over the default shape
    // (keeps slider keys present even if the blob omits them). Best-effort: if the
    // read fails or we're offline, the initDraft defaults stand.
    React.useEffect(() => {
      const a = api();
      if (!a || !a.getNodeConfig || node.nodeId == null) { setLoaded(true); return; }
      let live = true;
      a.getNodeConfig(node.nodeId).then(function (r) {
        if (!live) return;
        const blob = r && (r.configBlob || r.config || r.blob);
        if (blob && typeof blob === "object") setDraft((d) => ({ ...d, ...blob }));
        setLoaded(true);
      }).catch(function () { if (live) setLoaded(true); });
      return function () { live = false; };
    }, [node.nodeId]);
    React.useEffect(() => {
      function onKey(e) { if (e.key === "Escape") onClose(); }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, []);
    // destructive lifecycle — both flows confirm in a DangerModal AND carry
    // confirm:true on the wire (the PHP/bridge run the two-step token handshake).
    const [confirmKind, setConfirmKind] = useState(null);          // "retire" | "decom" | null
    const WIPE_SCOPES = [
      { id: "config", label: "Configuration", hint: "/etc/solarinet client config" },
      { id: "certs", label: "Certificates", hint: "client cert + key (CA-signed)" },
      { id: "spool", label: "Spool", hint: "queued, unsent samples" },
      { id: "logs", label: "Logs", hint: "agent log files" },
      { id: "unit", label: "Service unit", hint: "systemd unit — agent stops permanently" },
    ];
    const [wipe, setWipe] = useState(() => Object.fromEntries(WIPE_SCOPES.map((s) => [s.id, true])));
    // Decommission: securely wipe the selected scopes on the host, then retire.
    function doDecommission() {
      const a = api();
      if (!a || !a.decommission || node.nodeId == null) {
        toast("Decommission unavailable (offline)", "close"); setConfirmKind(null); return;
      }
      const scope = WIPE_SCOPES.map((s) => s.id).filter((id) => wipe[id]);
      return a.decommission(node.nodeId, scope).then(function () {
        toast(`Decommissioned → ${node.name} (wiped: ${scope.join(", ") || "nothing"} · retired)`, "close");
        setConfirmKind(null); refresh(); onClose();
      }).catch(function (e) { toast(`Decommission failed: ${e && e.message || "error"}`, "close"); });
    }
    // Retire: drop the node from the active roster WITHOUT touching the host —
    // the right call when the machine is already gone/offline.
    function doRetire() {
      const a = api();
      if (!a || !a.retireNode || node.nodeId == null) {
        toast("Retire unavailable (offline)", "close"); setConfirmKind(null); return;
      }
      return a.retireNode(node.nodeId).then(function () {
        toast(`Retired → ${node.name} (removed from active roster)`, "close");
        setConfirmKind(null); refresh(); onClose();
      }).catch(function (e) { toast(`Retire failed: ${e && e.message || "error"}`, "close"); });
    }
    function push() {
      const a = api();
      // setNodeConfig posts to /api/nodes/{id}/config; the server computes the new
      // epoch (max(nodeTarget, globalEpoch)+1) and PROVISIONs — no client-side epoch math.
      if (a && a.setNodeConfig && node.nodeId != null) {
        a.setNodeConfig(node.nodeId, draft).then(function () {
          toast("Config pushed → " + node.name + " (solariCtl · PROVISION)", "settings"); refresh(); onClose();
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
            <button className="btn-ghost" onClick={() => setConfirmKind("retire")} style={{ marginRight: "auto", color: "var(--crit)", borderColor: "var(--crit)" }}><Icon name="close" size={14} />Retire</button>
            <button className="btn-ghost" onClick={() => setConfirmKind("decom")} style={{ color: "var(--crit)", borderColor: "var(--crit)" }}><Icon name="close" size={14} />Decommission</button>
            <button className="btn-ghost" onClick={onClose}>Cancel</button>
            <button className="btn-primary" onClick={push}><Icon name="check" size={14} />Push to node</button>
          </div>
        </div>

        {confirmKind && <div onClick={(e) => e.stopPropagation()}>
        {confirmKind === "retire" && (
          <DangerModal title={`Retire ${node.name}?`} sub={`${node.hostFqdn || node.name} · ${node.ip || ""}`}
            verb="Retire node" busyVerb="Retiring…" onConfirm={doRetire} onClose={() => setConfirmKind(null)}>
            <p style={{ margin: 0 }}>
              The node is removed from the active roster: no more samples are expected, alerts stop
              firing for it, and it is hidden from the fleet views (behind the “Retired” filter).
            </p>
            <p style={{ margin: "10px 0 0" }} className="muted">
              Nothing on the host is touched — use <b>Decommission</b> instead to also wipe the agent
              from the machine. Use Retire when the machine is already offline or gone.
            </p>
          </DangerModal>
        )}
        {confirmKind === "decom" && (
          <DangerModal title={`Decommission ${node.name}?`} sub={`${node.hostFqdn || node.name} · ${node.ip || ""}`}
            verb="Decommission" busyVerb="Decommissioning…" confirmName={node.name}
            onConfirm={doDecommission} onClose={() => setConfirmKind(null)}>
            <p style={{ margin: 0 }}>
              Securely wipes the selected data from the host over the control plane, then retires the
              node. <b style={{ color: "var(--crit)" }}>This cannot be undone.</b>
            </p>
            <div className="agent-sect" style={{ marginTop: 12 }}>Wipe scope</div>
            {WIPE_SCOPES.map((s) => (
              <label key={s.id} style={{ display: "flex", alignItems: "center", gap: 9, padding: "6px 0", cursor: "pointer", fontSize: 13 }}>
                <input type="checkbox" checked={!!wipe[s.id]} onChange={(e) => setWipe((w) => ({ ...w, [s.id]: e.target.checked }))} />
                <span style={{ fontWeight: 600 }}>{s.label}</span>
                <span className="td-mono muted" style={{ fontSize: 11 }}>{s.hint}</span>
              </label>
            ))}
          </DangerModal>
        )}
        </div>}
      </div>
    );
  }

  /* ===================== POOL CARDS (top-level dashboard) ===================== */
  // Operator-defined functional pools with per-pool health rollups.
  function PoolCards({ onOpen }) {
    const pools = S.pools || [];
    const [del, setDel] = useState(null);   // pool pending delete confirmation
    if (!pools.length) return null;
    // Pool 1 ("Unassigned") is the server-side catch-all — never deletable.
    const deletable = (p) => Number(p.poolId) !== 1;
    function doDelete() {
      const a = api();
      if (!a || !a.deletePool) { toast("Pool deletion unavailable (offline)", "close"); setDel(null); return; }
      const p = del;
      return a.deletePool(p.poolId).then(function () {
        toast(`Pool deleted: ${p.name} — systems moved to Unassigned`, "close");
        setDel(null); refresh();
      }).catch(function (e) { toast(`Delete failed: ${e && e.message || "error"}`, "close"); });
    }
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
              {deletable(p) && (
                <button className="rowx kpi__x" title={`Delete pool ${p.name}`} aria-label={`Delete pool ${p.name}`}
                  onClick={(e) => { e.stopPropagation(); setDel(p); }}>
                  <Icon name="close" size={13} />
                </button>
              )}
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
        {del && (
          <DangerModal title={`Delete pool ${del.name}?`} sub={`${del.assetCount || 0} system(s) currently assigned`}
            verb="Delete pool" busyVerb="Deleting…" onConfirm={doDelete} onClose={() => setDel(null)}>
            <p style={{ margin: 0 }}>
              The pool is removed. Systems assigned to it are <b>not</b> deleted — they move to
              the <b>Unassigned</b> pool and keep their monitoring targets and history.
            </p>
          </DangerModal>
        )}
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

    // ---- removal (danger, typed-name confirm; confirm:true on the wire) ----
    const [confirmRemove, setConfirmRemove] = useState(false);
    const [rmTarget, setRmTarget] = useState(null);   // target row pending removal
    function removeSystem() {
      const a = api();
      if (!a || !a.removeAsset) { toast && toast("Removal unavailable (offline)", "close"); setConfirmRemove(false); return; }
      return a.removeAsset(assetId).then(() => {
        toast && toast(`Removed ${data.displayName} — monitoring stopped, history purged`, "close");
        setConfirmRemove(false);
        if (a.refresh) a.refresh().catch(() => {});
        onBack();
      }).catch((e) => { toast && toast("Remove failed: " + (e && e.message || "error"), "close"); });
    }
    function removeTargetGo() {
      const a = api();
      if (!a || !a.removeTarget) { toast && toast("Removal unavailable (offline)", "close"); setRmTarget(null); return; }
      const t = rmTarget;
      return a.removeTarget(assetId, t.targetId).then(() => {
        toast && toast(`Target removed: ${t.label || t.targetId}`, "close");
        setRmTarget(null); load();
        if (a.refresh) a.refresh().catch(() => {});
      }).catch((e) => { toast && toast("Remove failed: " + (e && e.message || "error"), "close"); });
    }

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
          <div className="page-head__right">
            <button className="btn-danger" onClick={() => setConfirmRemove(true)}><Icon name="close" size={14} />Remove system</button>
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
            <thead><tr><th>Target</th><th>Proto</th><th>Port</th><th>State</th><th>Vantages</th><th aria-label="remove" /></tr></thead>
            <tbody>
              {targets.map((t) => (
                <tr key={t.targetId} style={{ cursor: "pointer" }} onClick={() => onOpenService(t.targetId)}>
                  <td>{t.label || t.targetId}</td>
                  <td>{t.proto}</td>
                  <td className="td-mono">{t.port || "—"}</td>
                  <td><span style={dotStyle(t.state)} /> {t.state}</td>
                  <td className="td-mono">{t.vantages}</td>
                  <td style={{ width: 34, textAlign: "right" }}>
                    <button className="rowx" title={`Stop monitoring ${t.label || t.targetId}`} aria-label={`Remove target ${t.label || t.targetId}`}
                      onClick={(e) => { e.stopPropagation(); setRmTarget(t); }}>
                      <Icon name="close" size={13} />
                    </button>
                  </td>
                </tr>
              ))}
            </tbody>
          </table></div>
        )}
        <div className="muted" style={{ marginTop: 18, fontSize: 12 }}>
          {data.nodeId ? "Agent linked — host metrics available." : "No agent deployed on this system — reachability only. Deploy a SolariNet client to collect CPU / RAM / disk metrics."}
        </div>

        {confirmRemove && (
          <DangerModal title={`Remove ${data.displayName}?`} sub={`${data.ip}${data.host ? " · " + data.host : ""} · ${targets.length} target(s)`}
            verb="Remove system" busyVerb="Removing…" confirmName={data.displayName}
            onConfirm={removeSystem} onClose={() => setConfirmRemove(false)}>
            <p style={{ margin: 0 }}>
              Monitoring stops for this system: all {targets.length} probe target(s) are deleted and
              its reachability history is purged. <b style={{ color: "var(--crit)" }}>This cannot be undone.</b>
            </p>
            <p style={{ margin: "10px 0 0" }} className="muted">
              The host itself is untouched — it may reappear under Discovery on a future scan.
            </p>
          </DangerModal>
        )}
        {rmTarget && (
          <DangerModal title={`Remove target ${rmTarget.label || rmTarget.targetId}?`}
            sub={`${rmTarget.proto}${rmTarget.port ? ":" + rmTarget.port : ""} on ${data.displayName}`}
            verb="Remove target" busyVerb="Removing…"
            onConfirm={removeTargetGo} onClose={() => setRmTarget(null)}>
            <p style={{ margin: 0 }}>
              Probing stops for this target and its history is purged. The system itself stays
              monitored via its remaining target(s).
            </p>
          </DangerModal>
        )}
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

  /* ===================== MARKDOWN (self-contained, CSP-safe) ===================== */
  // A small, dependency-free Markdown → React renderer. Text is placed as React
  // children (auto-escaped), and links are scheme-filtered, so no raw HTML is ever
  // injected. Supports: headings, bold/italic/inline-code, code fences, ordered &
  // unordered lists, blockquotes, tables, horizontal rules, links, and paragraphs.
  function mdSplitRow(line) {
    let s = line.trim();
    if (s.charAt(0) === "|") s = s.slice(1);
    if (s.charAt(s.length - 1) === "|") s = s.slice(0, -1);
    return s.split("|").map((c) => c.trim());
  }
  function mdInline(text, kp) {
    const nodes = [];
    let rest = String(text == null ? "" : text);
    let i = 0;
    const re = /(`[^`]+`)|(\*\*[^*]+\*\*)|(\[[^\]]+\]\([^)]+\))|(\*[^*]+\*)|(_[^_]+_)/;
    while (rest) {
      const m = re.exec(rest);
      if (!m) { nodes.push(rest); break; }
      if (m.index > 0) nodes.push(rest.slice(0, m.index));
      const tok = m[0];
      const k = kp + "-i" + (i++);
      if (tok.charAt(0) === "`") nodes.push(<code key={k} className="md-code">{tok.slice(1, -1)}</code>);
      else if (tok.slice(0, 2) === "**") nodes.push(<strong key={k}>{tok.slice(2, -2)}</strong>);
      else if (tok.charAt(0) === "[") {
        const mm = /^\[([^\]]+)\]\(([^)]+)\)$/.exec(tok);
        const href = mm[2];
        const safe = /^(https?:|\/|#|mailto:)/i.test(href) ? href : "#";
        nodes.push(<a key={k} href={safe} target="_blank" rel="noopener noreferrer">{mm[1]}</a>);
      } else nodes.push(<em key={k}>{tok.slice(1, -1)}</em>);
      rest = rest.slice(m.index + tok.length);
    }
    return nodes;
  }
  function Markdown({ source }) {
    if (!source) return null;
    const lines = String(source).replace(/\r\n/g, "\n").split("\n");
    const blocks = [];
    let i = 0, key = 0;
    const K = () => "md" + (key++);
    const isBlockStart = (l) => /^```|^\s*[-*+]\s+|^\s*\d+[.)]\s+|^#{1,6}\s+|^\s*>\s?/.test(l) || /^\s*([-*_])(\s*\1){2,}\s*$/.test(l);
    while (i < lines.length) {
      const line = lines[i];
      if (/^```/.test(line.trim())) {
        const buf = [];
        i++;
        while (i < lines.length && !/^```/.test(lines[i].trim())) { buf.push(lines[i]); i++; }
        i++;
        blocks.push(<pre key={K()} className="md-pre"><code>{buf.join("\n")}</code></pre>);
        continue;
      }
      if (line.trim() === "") { i++; continue; }
      if (/^\s*([-*_])(\s*\1){2,}\s*$/.test(line)) { blocks.push(<hr key={K()} className="md-hr" />); i++; continue; }
      const h = /^(#{1,6})\s+(.*)$/.exec(line);
      if (h) {
        const lvl = Math.min(h[1].length, 4);
        const Tag = "h" + (lvl + 1);
        blocks.push(<Tag key={K()} className={"md-h md-h" + lvl}>{mdInline(h[2], K())}</Tag>);
        i++; continue;
      }
      if (line.indexOf("|") >= 0 && i + 1 < lines.length && lines[i + 1].indexOf("-") >= 0 && /^\s*\|?\s*:?-{2,}/.test(lines[i + 1])) {
        const header = mdSplitRow(line);
        i += 2;
        const rows = [];
        while (i < lines.length && lines[i].indexOf("|") >= 0 && lines[i].trim() !== "") { rows.push(mdSplitRow(lines[i])); i++; }
        blocks.push(
          <div key={K()} className="md-tablewrap"><table className="md-table">
            <thead><tr>{header.map((c, ci) => <th key={ci}>{mdInline(c, K() + "h" + ci)}</th>)}</tr></thead>
            <tbody>{rows.map((r, ri) => <tr key={ri}>{r.map((c, ci) => <td key={ci}>{mdInline(c, K() + "r" + ri + "c" + ci)}</td>)}</tr>)}</tbody>
          </table></div>
        );
        continue;
      }
      if (/^\s*>\s?/.test(line)) {
        const buf = [];
        while (i < lines.length && /^\s*>\s?/.test(lines[i])) { buf.push(lines[i].replace(/^\s*>\s?/, "")); i++; }
        blocks.push(<blockquote key={K()} className="md-quote">{mdInline(buf.join(" "), K())}</blockquote>);
        continue;
      }
      if (/^\s*[-*+]\s+/.test(line)) {
        const items = [];
        while (i < lines.length && /^\s*[-*+]\s+/.test(lines[i])) { items.push(lines[i].replace(/^\s*[-*+]\s+/, "")); i++; }
        blocks.push(<ul key={K()} className="md-ul">{items.map((it, ii) => <li key={ii}>{mdInline(it, K() + "l" + ii)}</li>)}</ul>);
        continue;
      }
      if (/^\s*\d+[.)]\s+/.test(line)) {
        const items = [];
        while (i < lines.length && /^\s*\d+[.)]\s+/.test(lines[i])) { items.push(lines[i].replace(/^\s*\d+[.)]\s+/, "")); i++; }
        blocks.push(<ol key={K()} className="md-ol">{items.map((it, ii) => <li key={ii}>{mdInline(it, K() + "o" + ii)}</li>)}</ol>);
        continue;
      }
      const buf = [line];
      i++;
      while (i < lines.length && lines[i].trim() !== "" && !isBlockStart(lines[i])) { buf.push(lines[i]); i++; }
      blocks.push(<p key={K()} className="md-p">{mdInline(buf.join(" "), K())}</p>);
    }
    return <div className="md">{blocks}</div>;
  }

  /* ===================== OPIE / ANALYSIS ===================== */
  const OPIE_STATUS = {
    investigating: { label: "Investigating", tone: "warn", icon: "pulse" },
    done:          { label: "Done", tone: "ok", icon: "check" },
    failed:        { label: "Failed", tone: "crit", icon: "close" },
  };
  const TRIGGER_LABEL = { crit: "Critical", "warn-storm": "Warn storm" };
  function opieAgo(r) {
    const m = r.startedMinAgo != null ? r.startedMinAgo : (r.startedAt ? Math.max(0, Math.round((Date.now() - Date.parse(r.startedAt)) / 60000)) : null);
    if (m == null || isNaN(m)) return "—";
    return (fmt && fmt.ago) ? fmt.ago(m) : (m + "m ago");
  }
  function opieDuration(sec) {
    if (sec == null) return "—";
    return sec >= 90 ? (sec / 60).toFixed(1) + "m" : sec + "s";
  }
  function OpieStatusPill({ status }) {
    const s = OPIE_STATUS[status] || OPIE_STATUS.investigating;
    return (
      <span className={"opie-stat " + s.tone}>
        <Icon name={s.icon} size={12} />{s.label}
      </span>
    );
  }

  function OpieScreen({ initialReportId, onOpenNode }) {
    const [list, setList] = useState(() => (S.opie || []).slice());
    const [selId, setSelId] = useState(initialReportId != null ? initialReportId : null);
    const [detail, setDetail] = useState(null);
    const [detErr, setDetErr] = useState(null);
    const [filter, setFilter] = useState("all");   // all | investigating | done

    // jump straight to a report when opened from an alert row
    useEffect(() => { if (initialReportId != null) setSelId(initialReportId); }, [initialReportId]);

    // (re)load the list from the API when live; fall back to the fixture.
    useEffect(() => {
      const a = api();
      if (a && a.opieList) {
        a.opieList("all").then((rows) => { if (Array.isArray(rows)) setList(rows); }).catch(() => {});
      }
    }, []);

    // load the selected report's full analysis (detail read carries the markdown)
    useEffect(() => {
      if (selId == null) { setDetail(null); setDetErr(null); return; }
      setDetail(null); setDetErr(null);
      const a = api();
      const local = (list.concat(S.opie || [])).find((r) => String(r.reportId) === String(selId));
      if (a && a.opieReport) {
        a.opieReport(selId)
          .then((r) => setDetail(r || local || null))
          .catch(() => { if (local) setDetail(local); else setDetErr("Couldn't load this report."); });
      } else {
        setDetail(local || null);
        if (!local) setDetErr("Report not found.");
      }
    }, [selId]);

    // ---------- detail view ----------
    if (selId != null) {
      const r = detail;
      const goBack = () => setSelId(null);
      if (detErr) return (<div className="page"><button className="btn-ghost" onClick={goBack} style={{ marginBottom: 12 }}><Icon name="chevronLeft" size={14} />Back to reports</button><div className="muted" style={{ marginTop: 16 }}>{detErr}</div></div>);
      if (!r) return (<div className="page"><button className="btn-ghost" onClick={goBack} style={{ marginBottom: 12 }}><Icon name="chevronLeft" size={14} />Back to reports</button><div className="muted" style={{ marginTop: 16 }}>Loading analysis…</div></div>);
      const host = r.hostFqdn || (r.nodeId != null ? "node " + r.nodeId : "Fleet");
      return (
        <div className="page">
          <button className="btn-ghost" onClick={goBack} style={{ marginBottom: 12 }}><Icon name="chevronLeft" size={14} />Back to reports</button>
          <div className="page-head">
            <div>
              <h1 className="page-title" style={{ display: "flex", alignItems: "center", gap: 10 }}>
                <Icon name="pulse" size={20} style={{ color: "var(--teal)" }} />
                Opie analysis
                <OpieStatusPill status={r.status} />
              </h1>
              <div className="page-sub">
                {(r.hostFqdn && r.nodeId != null)
                  ? <span className="opie-hostlink" onClick={() => onOpenNode && onOpenNode(r.nodeId)}>{host}</span>
                  : host}
                {" · "}{TRIGGER_LABEL[r.triggerKind] || r.triggerKind || "incident"} trigger
                {r.model ? " · " + r.model : ""}
              </div>
            </div>
            <div className="page-head__right">
              <span className={"alert-sev " + r.severity}>{r.severity}</span>
            </div>
          </div>

          <div className="kpis" style={{ gridTemplateColumns: "repeat(auto-fit,minmax(150px,1fr))", marginBottom: 16 }}>
            <div className="kpi"><div className="kpi__k">Status</div><div className="kpi__v" style={{ fontSize: 16, color: "var(--" + (OPIE_STATUS[r.status] || OPIE_STATUS.investigating).tone + ")" }}>{(OPIE_STATUS[r.status] || OPIE_STATUS.investigating).label}</div></div>
            <div className="kpi"><div className="kpi__k">Analysis time</div><div className="kpi__v" style={{ fontSize: 16 }}>{opieDuration(r.durationSec)}</div></div>
            <div className="kpi"><div className="kpi__k">Started</div><div className="kpi__v" style={{ fontSize: 16 }}>{opieAgo(r)}</div></div>
            <div className="kpi"><div className="kpi__k">Model</div><div className="kpi__v" style={{ fontSize: 16 }}>{r.model || "—"}</div></div>
          </div>

          {r.summary && (
            <div className="opie-summary">
              <Icon name="pulse" size={16} style={{ color: "var(--teal)", flex: "0 0 auto", marginTop: 1 }} />
              <div>{r.summary}</div>
            </div>
          )}

          <div className="panel" style={{ marginTop: 16 }}>
            <div className="panel__head"><Icon name="activity" size={16} /><h3>Analysis</h3></div>
            <div className="panel__body">
              {r.status === "investigating" && !r.analysis && (
                <div className="opie-note"><Icon name="pulse" size={15} style={{ color: "var(--warn)" }} />Opie is still investigating this incident — the write-up will appear here when it finishes.</div>
              )}
              {r.status === "failed" && !r.analysis && (
                <div className="opie-note"><Icon name="close" size={15} style={{ color: "var(--crit)" }} />Analysis failed to complete. {r.summary || "Re-run it from the incident when the model is reachable."}</div>
              )}
              {r.analysis
                ? <Markdown source={r.analysis} />
                : (r.status === "done" ? <div className="muted">No analysis text was recorded for this report.</div> : null)}
            </div>
          </div>
        </div>
      );
    }

    // ---------- list view ----------
    const shown = list.filter((r) => filter === "all" ? true : r.status === filter);
    const inv = list.filter((r) => r.status === "investigating").length;
    const done = list.filter((r) => r.status === "done").length;
    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title" style={{ display: "flex", alignItems: "center", gap: 10 }}><Icon name="pulse" size={22} style={{ color: "var(--teal)" }} />Opie · Analysis</h1>
            <div className="page-sub">{list.length} report{list.length === 1 ? "" : "s"} · {inv} investigating · {done} done</div>
          </div>
          <div className="page-head__right">
            <div className="seg">
              <button className={filter === "all" ? "on" : ""} onClick={() => setFilter("all")}>All</button>
              <button className={filter === "investigating" ? "on" : ""} onClick={() => setFilter("investigating")}>Investigating</button>
              <button className={filter === "done" ? "on" : ""} onClick={() => setFilter("done")}>Done</button>
            </div>
          </div>
        </div>

        {shown.length === 0 && <div className="empty">No analysis reports{filter !== "all" ? " in this state" : " yet — Opie writes one up when a critical alert fires or a warn storm forms."}.</div>}

        <div className="opie-list">
          {shown.map((r) => {
            const host = r.hostFqdn || (r.nodeId != null ? "node " + r.nodeId : "Fleet");
            return (
              <div key={r.reportId} className={"opie-row " + r.severity} onClick={() => setSelId(r.reportId)} role="button" tabIndex={0}
                onKeyDown={(e) => { if (e.key === "Enter" || e.key === " ") { e.preventDefault(); setSelId(r.reportId); } }}>
                <span className={"alert-sev " + r.severity}>{r.severity}</span>
                <div className="opie-row__main">
                  <div className="opie-row__top">
                    <span className="opie-row__host">{host}</span>
                    <span className="opie-row__trig">{TRIGGER_LABEL[r.triggerKind] || r.triggerKind || "incident"}</span>
                  </div>
                  <div className="opie-row__summary">{r.summary || "—"}</div>
                </div>
                <OpieStatusPill status={r.status} />
                <div className="opie-row__meta">
                  <div>{opieAgo(r)}</div>
                  <div className="muted">{opieDuration(r.durationSec)}</div>
                </div>
              </div>
            );
          })}
        </div>
      </div>
    );
  }

  Object.assign(window, { Discovery, Provisioning, ConfigScreen, PoolCards, Assets, AssetDetail, ServiceDetail, OpieScreen, Markdown });
})();
