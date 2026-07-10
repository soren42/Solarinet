/* ============================================================
   SolariNet — screens7: DNS (query/trace · parity health · SoR browser)

   One operator screen, three stacked sections:
     • DnsQueryTool   — HERO: fan a query out to steel/xenon/radium plus an
       external control resolver and show per-server answers side-by-side,
       flagging NXDOMAIN, RPZ blocks, and internal-server disagreement.
       Backed by GET /api/dns/query (routes/dns.php → lib/Dig.php).
     • DnsHealthStrip — zone-parity table (SOA serial per expected server)
       + RPZ tile. Backed by GET /api/dns/health; 60s poll while mounted.
     • DnsRecordBrowser — read-only SoR zone/record browser + "next render
       preview". Backed by /api/dns/zones, /zones/{id}/records, /hosts.

   All fetches are lazy (screen-mounted) — DNS data is never in loadLive().
   Same window-global IIFE idiom as screens6.jsx; loaded after it.
   Exports: DnsScreen.
   ============================================================ */
(function () {
  const { useState, useEffect, useRef } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;

  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function isOffline() { return !window.SOLARI || window.SOLARI.source === "offline"; }

  // ---- server registry mirror (ids only — the PHP side owns the IPs; these
  // labels/ips are display hints and stay correct as long as routes/dns.php's
  // DNS_SERVERS const matches; the wire always carries the real values). ----
  const SERVER_ORDER = ["steel", "xenon", "radium", "external"];
  const SERVER_META = {
    steel:    { label: "steel",   hint: "10.0.0.11", kind: "internal" },
    xenon:    { label: "xenon",   hint: "10.0.0.20", kind: "internal" },
    radium:   { label: "radium",  hint: "10.1.0.10", kind: "internal" },
    external: { label: "control", hint: "1.1.1.1",   kind: "external" },
  };
  const QUERY_TYPES = ["A", "AAAA", "CNAME", "PTR", "TXT", "MX", "SRV", "NS", "SOA", "ANY"];
  const HISTORY_KEY = "solari.dns.history";
  const CANARIES = [
    { name: "xenon.akoria.net", type: "A" },
    { name: "radium.akoria.org", type: "A" },
    { name: "doubleclick.net", type: "TXT" },
  ];

  const FLD = {
    boxSizing: "border-box", padding: "8px 10px", borderRadius: 8,
    border: "1px solid var(--line-glow, rgba(255,255,255,0.14))",
    background: "rgba(255,255,255,0.04)", color: "inherit",
    fontFamily: "inherit", fontSize: 13,
  };

  function agoLabel(iso) {
    if (!iso) return "—";
    const t = Date.parse(iso);
    if (isNaN(t)) return "—";
    const min = Math.max(0, Math.round((Date.now() - t) / 60000));
    if (min < 1) return "just now";
    if (min < 60) return min + "m ago";
    if (min < 1440) return Math.floor(min / 60) + "h ago";
    return Math.floor(min / 1440) + "d ago";
  }

  function Badge({ tone, children, title }) {
    return <span className="inv-badge" title={title} style={{ color: tone, borderColor: tone }}>{children}</span>;
  }

  // status → theme tones (NOERROR calm, NXDOMAIN cautionary, hard failures red)
  function statusTone(status) {
    if (status === "NOERROR") return { fg: "var(--ok)", bg: "var(--ok-bg)" };
    if (status === "NXDOMAIN") return { fg: "var(--warn)", bg: "var(--warn-bg)" };
    return { fg: "var(--crit)", bg: "var(--crit-bg)" };
  }

  function StatusPill({ status, rpzBlocked }) {
    if (rpzBlocked) {
      return (
        <span className="inv-badge" style={{ color: "var(--crit)", borderColor: "var(--crit)", background: "var(--crit-bg)" }}
          title="answer rewritten by the RPZ policy zone (rpz.akoria)">
          <Icon name="shield" size={11} style={{ marginRight: 4, verticalAlign: -1 }} />RPZ BLOCKED
        </span>
      );
    }
    const t = statusTone(status);
    return <span className="inv-badge" style={{ color: t.fg, borderColor: t.fg, background: t.bg }}>{status || "—"}</span>;
  }

  // ---- localStorage query history (last 10 {name,type}) ----
  function loadHistory() {
    try {
      const raw = JSON.parse(localStorage.getItem(HISTORY_KEY) || "[]");
      return Array.isArray(raw) ? raw.filter(function (h) { return h && h.name; }).slice(0, 10) : [];
    } catch (e) { return []; }
  }
  function saveHistory(list) {
    try { localStorage.setItem(HISTORY_KEY, JSON.stringify(list.slice(0, 10))); } catch (e) { /* quota/off */ }
  }

  // Normalise one server result's answers to a canonical multiset key so
  // internal servers can be compared: sorted "TYPE|rdata" lines.
  function answerKey(r) {
    return (r.answers || [])
      .map(function (a) { return (a.type || "") + "|" + (a.data || ""); })
      .sort()
      .join("\n");
  }

  /* ===================================================================
     HERO — per-server query tool
     =================================================================== */
  function DnsServerCard({ r, differs }) {
    const srv = r.server || {};
    const answers = r.answers || [];
    const authority = r.authority || [];
    const isNx = r.status === "NXDOMAIN";
    return (
      <div className="panel" style={{
        minWidth: 0,
        borderLeft: differs ? "2px solid var(--warn)" : undefined,
      }}>
        <div className="panel__head" style={{ gap: 8 }}>
          <h3 style={{ display: "flex", alignItems: "baseline", gap: 7, minWidth: 0, flex: 1 }}>
            <span>{srv.label || srv.id || "?"}</span>
            <span className="td-mono" style={{ color: "var(--ink-faint)", fontSize: 10.5, fontWeight: 400 }}>{srv.ip || ""}</span>
            {differs && <Badge tone="var(--warn)" title="answer set differs from the other internal resolvers">differs</Badge>}
          </h3>
          <StatusPill status={r.status} rpzBlocked={r.rpzBlocked} />
        </div>
        <div className="panel__body" style={{ padding: "8px 12px", fontFamily: "var(--mono)", fontSize: 11.5 }}>
          {answers.length > 0 && answers.map(function (a, i) {
            return (
              <div key={i} style={{ display: "flex", gap: 10, padding: "3px 0", minWidth: 0 }}>
                <span style={{ color: "var(--teal)", flex: "0 0 44px" }}>{a.type}</span>
                <span style={{ color: "var(--ink-faint)", flex: "0 0 44px", textAlign: "right" }}>{a.ttl}</span>
                <span style={{ flex: 1, minWidth: 0, overflowX: "auto", whiteSpace: "nowrap" }} title={a.name}>{a.data}</span>
              </div>
            );
          })}
          {answers.length === 0 && !isNx && !r.error && (
            <div className="muted" style={{ padding: "3px 0" }}>no answer records</div>
          )}
          {/* NXDOMAIN tell: which zone said no */}
          {answers.length === 0 && isNx && authority.length > 0 && authority.map(function (a, i) {
            return (
              <div key={"au" + i} style={{ color: "var(--ink-dim)", padding: "3px 0", overflowX: "auto", whiteSpace: "nowrap" }}
                title="authority section — the zone that answered NXDOMAIN">
                {a.name} {a.type} {a.data}
              </div>
            );
          })}
          {r.error && <div style={{ color: "var(--crit)", padding: "3px 0" }}>{r.error}</div>}
          <div style={{ textAlign: "right", color: "var(--ink-faint)", fontSize: 10, marginTop: 6 }}>
            {r.queryTimeMs != null ? r.queryTimeMs + " ms" : "—"}
          </div>
        </div>
      </div>
    );
  }

  function DnsQueryTool({ onFocusHealth }) {
    const [name, setName] = useState("");
    const [type, setType] = useState("A");
    const [servers, setServers] = useState(function () { return new Set(SERVER_ORDER); });
    const [running, setRunning] = useState(false);
    const [result, setResult] = useState(null);
    const [error, setError] = useState(null);
    const [history, setHistory] = useState(loadHistory);
    const offline = isOffline();

    function toggleServer(id) {
      setServers(function (prev) {
        const next = new Set(prev);
        if (next.has(id)) { if (next.size > 1) next.delete(id); } else next.add(id);
        return next;
      });
    }

    function run(qName, qType) {
      const a = api();
      const n = (qName != null ? qName : name).trim();
      const t = qType != null ? qType : type;
      if (!n || !a || !a.dnsQuery || offline || running) return;
      if (qName != null) { setName(qName); setType(t); }
      setRunning(true);
      setError(null);
      const order = SERVER_ORDER.filter(function (id) { return servers.has(id); });
      a.dnsQuery(n, t, order)
        .then(function (d) {
          setResult(d || null);
          setRunning(false);
          if (d) {
            const next = [{ name: n, type: t }].concat(
              history.filter(function (h) { return !(h.name === n && h.type === t); })).slice(0, 10);
            setHistory(next); saveHistory(next);
          }
        })
        .catch(function () {
          setResult(null); setRunning(false);
          setError("Query failed — the DNS API is unreachable or errored (session expired, or php-fpm/dig).");
        });
    }

    // ---- client-side disagreement: modal answer set among internal servers ----
    const results = (result && result.results) || [];
    const internal = results.filter(function (r) { return r.server && r.server.kind !== "external"; });
    const diffIds = {};
    let steelXenonDisagree = false;
    if (internal.length > 1) {
      const keys = internal.map(answerKey);
      const counts = {};
      keys.forEach(function (k) { counts[k] = (counts[k] || 0) + 1; });
      let modal = keys[0];
      Object.keys(counts).forEach(function (k) { if (counts[k] > counts[modal]) modal = k; });
      internal.forEach(function (r, i) { if (keys[i] !== modal) diffIds[r.server.id] = true; });
      const st = internal.find(function (r) { return r.server.id === "steel"; });
      const xe = internal.find(function (r) { return r.server.id === "xenon"; });
      if (st && xe && answerKey(st) !== answerKey(xe)) steelXenonDisagree = true;
    }

    return (
      <div className="panel" style={{ marginBottom: 18 }}>
        <div className="panel__head"><Icon name="dns" size={16} /><h3>Query</h3>
          <span className="muted" style={{ fontSize: 11, fontWeight: 400 }}>ask every resolver at once</span>
        </div>
        <div className="panel__body" style={{ padding: "12px 14px" }}>
          <div style={{ display: "flex", gap: 8, flexWrap: "wrap", alignItems: "center" }}>
            <input
              autoFocus
              style={Object.assign({}, FLD, { flex: "1 1 260px", minWidth: 180, fontFamily: "var(--mono)" })}
              value={name}
              onChange={function (e) { setName(e.target.value); }}
              onKeyDown={function (e) { if (e.key === "Enter") run(); }}
              placeholder='name — try a bare name like "nas-x" vs "nas-x.akoria.net"'
            />
            <select style={Object.assign({}, FLD, { flex: "0 0 auto" })} value={type}
              onChange={function (e) { setType(e.target.value); }}>
              {QUERY_TYPES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}
            </select>
            {SERVER_ORDER.map(function (id) {
              const m = SERVER_META[id];
              const on = servers.has(id);
              return (
                <button key={id} className={"chip" + (on ? " on" : "")} onClick={function () { toggleServer(id); }}
                  title={m.hint + (m.kind === "external" ? " · external control, never counted in parity" : "")}>
                  <span className="dot" style={{ background: on ? (m.kind === "external" ? "var(--violet)" : "var(--teal)") : "var(--line)" }} />
                  {m.label}
                </button>
              );
            })}
            <button className="chip on" style={{ color: "var(--teal)", fontWeight: 600 }}
              disabled={offline || running || !name.trim()}
              onClick={function () { run(); }}
              title={offline ? "live resolvers unavailable in offline mode" : "run the query (Enter)"}>
              {running ? "querying…" : "Query"}
            </button>
          </div>

          {/* history + canary chips */}
          <div style={{ display: "flex", gap: 6, flexWrap: "wrap", marginTop: 10 }}>
            {CANARIES.map(function (c, i) {
              return (
                <button key={"c" + i} className="chip" style={{ fontSize: 11, borderStyle: "dashed" }}
                  disabled={offline || running}
                  onClick={function () { run(c.name, c.type); }}
                  title="canary query">
                  {c.name} <span className="td-mono" style={{ color: "var(--ink-faint)", marginLeft: 4 }}>{c.type}</span>
                </button>
              );
            })}
            {history.length > 0 && <span style={{ width: 1, height: 18, background: "var(--line)", margin: "0 2px", alignSelf: "center" }} />}
            {history.map(function (h, i) {
              return (
                <button key={"h" + i} className="chip" style={{ fontSize: 11 }}
                  disabled={offline || running}
                  onClick={function () { run(h.name, h.type); }}>
                  {h.name} <span className="td-mono" style={{ color: "var(--ink-faint)", marginLeft: 4 }}>{h.type}</span>
                </button>
              );
            })}
          </div>

          {offline && (
            <div className="muted" style={{ fontSize: 12, marginTop: 10 }}>
              live resolvers unavailable in offline mode
            </div>
          )}

          {steelXenonDisagree && (
            <div style={{
              marginTop: 12, padding: "8px 12px", borderRadius: 8, fontSize: 12.5,
              color: "var(--warn)", background: "var(--warn-bg)", border: "1px solid var(--warn)",
              cursor: onFocusHealth ? "pointer" : "default",
            }}
              onClick={onFocusHealth}
              title="jump to zone parity">
              steel and xenon disagree — check AXFR / zone push ↓
            </div>
          )}

          {error && (
            <div style={{ color: "var(--crit)", marginTop: 12, fontSize: 13 }}>{error}</div>
          )}

          {result && (
            <div style={{
              display: "grid", gap: 12, marginTop: 14,
              gridTemplateColumns: "repeat(auto-fit, minmax(230px, 1fr))",
            }}>
              {results.map(function (r, i) {
                return <DnsServerCard key={(r.server && r.server.id) || i} r={r}
                  differs={!!diffIds[(r.server && r.server.id) || ""]} />;
              })}
            </div>
          )}
          {result && (
            <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 8 }}>
              {result.name} {result.type} · {agoLabel(result.ts)}
            </div>
          )}
        </div>
      </div>
    );
  }

  /* ===================================================================
     Health strip — zone parity + RPZ tile
     =================================================================== */
  function RpzTile({ rpz }) {
    if (!rpz) return null;
    const serials = rpz.serials || {};
    const parityTone = rpz.parity ? "var(--ok)" : "var(--crit)";
    return (
      <div className="panel" style={{ flex: "0 1 300px", minWidth: 240 }}>
        <div className="panel__head"><Icon name="shield" size={15} /><h3>RPZ · {rpz.zone || "rpz.akoria"}</h3></div>
        <div className="panel__body" style={{ padding: "12px 14px" }}>
          <div style={{ fontSize: 30, fontWeight: 700, lineHeight: 1 }}>
            {rpz.blockedCount != null ? rpz.blockedCount.toLocaleString() : "—"}
          </div>
          <div className="muted" style={{ fontSize: 11.5, marginTop: 3 }}>blocked domains</div>
          <div className="td-mono" style={{ fontSize: 11.5, marginTop: 10, display: "flex", flexDirection: "column", gap: 4 }}>
            {["xenon", "steel"].map(function (id) {
              const s = serials[id] || {};
              const ok = s.status === "NOERROR";
              return (
                <div key={id} style={{ display: "flex", gap: 8, alignItems: "center" }}>
                  <span className="dot" style={{ width: 7, height: 7, borderRadius: "50%", background: ok ? "var(--ok)" : "var(--crit)" }} />
                  <span style={{ flex: "0 0 48px" }}>{id}</span>
                  <span style={{ color: ok ? "var(--ink)" : "var(--crit)" }}>{s.serial != null ? s.serial : (s.status || "unreachable")}</span>
                </div>
              );
            })}
          </div>
          <div style={{ marginTop: 10, display: "flex", alignItems: "center", gap: 8 }}>
            <Badge tone={parityTone}>{rpz.parity ? "in sync" : "serials differ"}</Badge>
            <span className="muted" style={{ fontSize: 11 }}>refreshed {agoLabel(rpz.lastRefresh)}</span>
          </div>
          {rpz.sourceUrl && (
            <div className="td-mono" style={{ color: "var(--ink-faint)", fontSize: 10, marginTop: 8, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}
              title={rpz.sourceUrl}>{rpz.sourceUrl}</div>
          )}
        </div>
      </div>
    );
  }

  function DnsHealthStrip() {
    const [health, setHealth] = useState(function () { return (S.dns && S.dns.health) || null; });
    const [loading, setLoading] = useState(true);

    function fetchHealth() {
      const a = api();
      if (!a || !a.dnsHealth || isOffline()) { setLoading(false); return; }
      a.dnsHealth()
        .then(function (d) { setHealth(d || null); setLoading(false); })
        .catch(function () { setLoading(false); });
    }

    useEffect(function () {
      fetchHealth();
      const t = setInterval(fetchHealth, 60000); // while mounted only
      return function () { clearInterval(t); };
    }, []);

    const zones = (health && health.zones) || [];
    const rpz = health && health.rpz;

    return (
      <div id="dns-health" style={{ display: "flex", gap: 18, flexWrap: "wrap", alignItems: "stretch", marginBottom: 18 }}>
        <div className="panel" style={{ flex: "1 1 420px", minWidth: 0 }}>
          <div className="panel__head">
            <Icon name="pulse" size={16} /><h3>Zone parity</h3>
            {health && health.sorAvailable === false &&
              <Badge tone="var(--warn)" title="SoR (cesium) unreachable — showing the fallback zone list">SoR offline</Badge>}
            <span style={{ flex: 1 }} />
            <button className="iconbtn" onClick={fetchHealth} title="refresh now"><Icon name="refresh" size={14} /></button>
          </div>
          <div className="panel__body" style={{ padding: 0 }}>
            {loading && !health && <div className="empty">Checking resolvers…</div>}
            {!loading && zones.length === 0 && (
              <div className="empty">{isOffline() ? "live resolvers unavailable in offline mode" : "No zone health available."}</div>
            )}
            {zones.length > 0 && (
              <div style={{ overflowX: "auto" }}>
                <table className="grid">
                  <thead><tr>
                    <th>Zone</th><th>Kind</th><th>Authority</th><th>SOA serials</th><th>Parity</th>
                  </tr></thead>
                  <tbody>
                    {zones.map(function (z, i) {
                      const soa = z.soa || {};
                      const exp = z.expectedServers || [];
                      return (
                        <tr key={i}>
                          <td className="td-mono" style={{ fontWeight: 600 }}>{z.name}</td>
                          <td><Badge tone="var(--ink-faint)">{z.kind || "—"}</Badge></td>
                          <td><Badge tone={z.authority === "sor_rendered" ? "var(--teal)" : "var(--violet)"}>{z.authority || "—"}</Badge></td>
                          <td>
                            <div style={{ display: "flex", gap: 12, flexWrap: "wrap" }}>
                              {exp.map(function (sid) {
                                const s = soa[sid] || {};
                                const ok = s.status === "NOERROR";
                                return (
                                  <span key={sid} className="td-mono" style={{ fontSize: 11.5, display: "inline-flex", alignItems: "center", gap: 5 }}>
                                    <span style={{ width: 7, height: 7, borderRadius: "50%", background: ok ? "var(--ok)" : "var(--crit)", display: "inline-block" }} />
                                    <span className="muted">{sid}</span>
                                    {ok
                                      ? <span>{s.serial != null ? s.serial : "—"}</span>
                                      : <span style={{ color: "var(--crit)" }}>unreachable</span>}
                                  </span>
                                );
                              })}
                            </div>
                          </td>
                          <td>
                            {z.parity
                              ? <Badge tone="var(--ok)">in sync</Badge>
                              : <Badge tone="var(--crit)">serials differ</Badge>}
                          </td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            )}
          </div>
        </div>
        <RpzTile rpz={rpz} />
      </div>
    );
  }

  /* ===================================================================
     Record browser — SoR zones + records + next-render preview
     =================================================================== */
  function DnsRecordBrowser() {
    const [zones, setZones] = useState(null);
    const [activeZoneId, setActiveZoneId] = useState(null);
    const [recData, setRecData] = useState(null);     // {zone, records, total, truncated}
    const [recLoading, setRecLoading] = useState(false);
    const [recQuery, setRecQuery] = useState("");
    const [recType, setRecType] = useState("");
    const [genFilter, setGenFilter] = useState("all"); // all | generated | explicit
    const [tab, setTab] = useState("records");
    const [hosts, setHosts] = useState(null);
    const debounceRef = useRef(null);

    useEffect(function () {
      const a = api();
      if (a && a.dnsZones && !isOffline()) {
        a.dnsZones().then(function (zs) { setZones(Array.isArray(zs) ? zs : []); })
          .catch(function () { setZones([]); });
      } else {
        setZones([]);
      }
    }, []);

    function fetchRecords(zoneId, q, t, gen) {
      const a = api();
      if (!a || !a.dnsZoneRecords || isOffline() || !zoneId) return;
      let qs = "?limit=500";
      if (q) qs += "&q=" + encodeURIComponent(q);
      if (t) qs += "&type=" + encodeURIComponent(t);
      if (gen === "generated") qs += "&generated=1";
      if (gen === "explicit") qs += "&generated=0";
      setRecLoading(true);
      a.dnsZoneRecords(zoneId, qs)
        .then(function (d) { setRecData(d || null); setRecLoading(false); })
        .catch(function () { setRecData(null); setRecLoading(false); });
    }

    function openZone(id) {
      setActiveZoneId(id);
      setTab("records");
      fetchRecords(id, recQuery, recType, genFilter);
    }

    // debounced re-fetch on filter change
    useEffect(function () {
      if (!activeZoneId) return;
      if (debounceRef.current) clearTimeout(debounceRef.current);
      debounceRef.current = setTimeout(function () {
        fetchRecords(activeZoneId, recQuery, recType, genFilter);
      }, 300);
      return function () { if (debounceRef.current) clearTimeout(debounceRef.current); };
    }, [recQuery, recType, genFilter]);

    function openPreview() {
      setTab("preview");
      if (hosts) return;
      const a = api();
      if (a && a.dnsHosts && !isOffline()) {
        a.dnsHosts().then(function (d) { setHosts(d || { hosts: [], ifaces: [], cnames: [] }); })
          .catch(function () { setHosts({ hosts: [], ifaces: [], cnames: [] }); });
      } else {
        setHosts({ hosts: [], ifaces: [], cnames: [] });
      }
    }

    const records = (recData && recData.records) || [];

    return (
      <div className="panel">
        <div className="panel__head">
          <Icon name="table" size={16} /><h3>SoR records</h3>
          <span className="muted" style={{ fontSize: 11, fontWeight: 400 }}>read-only · cesium system of record</span>
          <span style={{ flex: 1 }} />
          <div className="seg">
            <button className={tab === "records" ? "on" : ""} onClick={function () { setTab("records"); }}>Records</button>
            <button className={tab === "preview" ? "on" : ""} onClick={openPreview}>Render preview</button>
          </div>
        </div>
        <div className="panel__body" style={{ padding: 0 }}>
          <div style={{ display: "flex", alignItems: "stretch", minHeight: 220 }}>
            {/* left rail: zones */}
            <div style={{ flex: "0 0 250px", borderRight: "1px solid var(--line)", padding: "8px 6px", minWidth: 0 }}>
              {zones === null && <div className="empty" style={{ padding: 20 }}>Loading zones…</div>}
              {zones !== null && zones.length === 0 && (
                <div className="empty" style={{ padding: 20 }}>{isOffline() ? "SoR unavailable offline" : "No zones in SoR."}</div>
              )}
              {(zones || []).map(function (z) {
                const on = z.id === activeZoneId;
                return (
                  <div key={z.id}
                    onClick={function () { openZone(z.id); }}
                    style={{
                      padding: "9px 10px", borderRadius: 8, cursor: "pointer", marginBottom: 4,
                      background: on ? "var(--panel-hi)" : "transparent",
                      boxShadow: on ? "inset 0 0 0 1px var(--line-glow)" : "none",
                    }}>
                    <div className="td-mono" style={{ fontWeight: 600, fontSize: 12.5 }}>{z.name}</div>
                    <div style={{ display: "flex", gap: 6, marginTop: 5, flexWrap: "wrap", alignItems: "center" }}>
                      <Badge tone="var(--ink-faint)">{z.kind}</Badge>
                      <Badge tone={z.authority === "sor_rendered" ? "var(--teal)" : "var(--violet)"}>{z.authority}</Badge>
                      <span className="muted" style={{ fontSize: 10.5 }}>{z.recordCount != null ? z.recordCount + " rec" : ""}</span>
                    </div>
                  </div>
                );
              })}
            </div>

            {/* right: records table OR preview */}
            <div style={{ flex: 1, minWidth: 0, padding: "10px 12px" }}>
              {tab === "records" && !activeZoneId && (
                <div className="empty">Select a zone to browse its records.</div>
              )}

              {tab === "records" && activeZoneId && (
                <>
                  <div style={{ display: "flex", gap: 8, flexWrap: "wrap", marginBottom: 10 }}>
                    <input style={Object.assign({}, FLD, { flex: "1 1 180px", minWidth: 140 })}
                      value={recQuery} onChange={function (e) { setRecQuery(e.target.value); }}
                      placeholder="filter name / rdata" />
                    <select style={FLD} value={recType} onChange={function (e) { setRecType(e.target.value); }}>
                      <option value="">all types</option>
                      {QUERY_TYPES.filter(function (t) { return t !== "ANY"; }).map(function (t) {
                        return <option key={t} value={t}>{t}</option>;
                      })}
                    </select>
                    <div className="seg">
                      {["all", "generated", "explicit"].map(function (g) {
                        return <button key={g} className={genFilter === g ? "on" : ""}
                          onClick={function () { setGenFilter(g); }}>{g}</button>;
                      })}
                    </div>
                  </div>

                  {recLoading && <div className="empty">Loading records…</div>}
                  {!recLoading && records.length === 0 && <div className="empty">No records match.</div>}
                  {!recLoading && records.length > 0 && (
                    <div style={{ overflowX: "auto" }}>
                      <table className="grid">
                        <thead><tr><th>Name</th><th>Type</th><th>TTL</th><th>Rdata</th><th>Origin</th></tr></thead>
                        <tbody>
                          {records.map(function (r) {
                            return (
                              <tr key={r.id}>
                                <td className="td-mono" style={{ fontWeight: 600 }}>{r.name}</td>
                                <td><Badge tone="var(--ink-dim)">{r.rrtype}</Badge></td>
                                <td className="td-mono" style={{ color: r.ttl == null ? "var(--ink-faint)" : undefined }}>
                                  {r.ttl == null ? "—" : r.ttl}
                                </td>
                                <td className="td-mono" style={{ maxWidth: 340 }}>
                                  <div style={{ overflowX: "auto", whiteSpace: "nowrap" }}>{r.rdata}</div>
                                </td>
                                <td>
                                  {r.isGenerated
                                    ? <Badge tone="var(--teal)" title="derived from IPAM — edit the entity, not the record">generated</Badge>
                                    : <Badge tone="var(--ink-faint)">explicit</Badge>}
                                </td>
                              </tr>
                            );
                          })}
                        </tbody>
                      </table>
                    </div>
                  )}
                  {!recLoading && recData && recData.truncated && (
                    <div className="muted" style={{ fontSize: 11.5, marginTop: 8 }}>
                      showing first 500 of {recData.total} — refine the filter
                    </div>
                  )}
                </>
              )}

              {tab === "preview" && (
                <>
                  <div className="muted" style={{ fontSize: 12, marginBottom: 10 }}>
                    next render preview — exactly what gen-zones will emit
                  </div>
                  {!hosts && <div className="empty">Loading preview…</div>}
                  {hosts && (
                    <div style={{ display: "flex", gap: 18, flexWrap: "wrap", alignItems: "flex-start" }}>
                      <div style={{ flex: "2 1 260px", minWidth: 0, overflowX: "auto" }}>
                        <table className="grid">
                          <thead><tr><th>Host</th><th>IP</th><th>Role</th><th>Lifecycle</th></tr></thead>
                          <tbody>
                            {(hosts.hosts || []).map(function (h, i) {
                              return (
                                <tr key={i}>
                                  <td className="td-mono" style={{ fontWeight: 600 }}>{h.host}</td>
                                  <td className="td-mono muted">{h.ip}</td>
                                  <td>{h.role ? <Badge tone="var(--violet)">{h.role}</Badge> : <span className="muted">—</span>}</td>
                                  <td className="td-mono muted">{h.lifecycle || "—"}</td>
                                </tr>
                              );
                            })}
                            {(!hosts.hosts || !hosts.hosts.length) && <tr><td colSpan={4} className="muted">none</td></tr>}
                          </tbody>
                        </table>
                      </div>
                      <div style={{ flex: "1 1 180px", minWidth: 0, overflowX: "auto" }}>
                        <table className="grid">
                          <thead><tr><th>Interface label</th><th>IP</th></tr></thead>
                          <tbody>
                            {(hosts.ifaces || []).map(function (f, i) {
                              return <tr key={i}><td className="td-mono">{f.label}</td><td className="td-mono muted">{f.ip}</td></tr>;
                            })}
                            {(!hosts.ifaces || !hosts.ifaces.length) && <tr><td colSpan={2} className="muted">none</td></tr>}
                          </tbody>
                        </table>
                      </div>
                      <div style={{ flex: "1 1 180px", minWidth: 0, overflowX: "auto" }}>
                        <table className="grid">
                          <thead><tr><th>Alias</th><th>Target host</th></tr></thead>
                          <tbody>
                            {(hosts.cnames || []).map(function (c, i) {
                              return <tr key={i}><td className="td-mono">{c.alias}</td><td className="td-mono muted">{c.targetHost}</td></tr>;
                            })}
                            {(!hosts.cnames || !hosts.cnames.length) && <tr><td colSpan={2} className="muted">none</td></tr>}
                          </tbody>
                        </table>
                      </div>
                    </div>
                  )}
                </>
              )}
            </div>
          </div>
        </div>
      </div>
    );
  }

  /* ===================================================================
     Page shell
     =================================================================== */
  function DnsScreen() {
    function focusHealth() {
      const el = document.getElementById("dns-health");
      if (el && el.scrollIntoView) el.scrollIntoView({ behavior: "smooth", block: "start" });
    }
    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">DNS</h1>
            <div className="page-sub">per-server query · zone parity · SoR record browser</div>
          </div>
        </div>
        <DnsQueryTool onFocusHealth={focusHealth} />
        <DnsHealthStrip />
        <DnsRecordBrowser />
      </div>
    );
  }

  Object.assign(window, { DnsScreen });
})();
