/* ============================================================
   SolariNet — Maintenance windows screen

   Schedule planned outages (a host, a probe-only host typed by hand, or the
   whole fleet), and see the active & upcoming windows with a LIVE badge and a
   Cancel action. A host under an ACTIVE window is expected-down: the alert
   bridge suppresses its notifications + dead-man's-switch, so the rest of the
   dashboard surfaces a "Maintenance" badge instead of reading it as "down".

   Consumes the maint-api unit:
     GET  /api/maintenance?status=active|scheduled|all
     POST /api/maintenance            {host|all, reason, hours|from+to}
     POST /api/maintenance/{id}/cancel

   Same idiom as the other screen modules: plain JSX hung off window globals,
   loaded by index.html; no build step.
   ============================================================ */
(function () {
  const { useState, useEffect, useCallback } = React;
  const Icon = window.Icon;
  const MaintenanceBadge = window.MaintenanceBadge;
  const S = window.SOLARI;
  const fmt = S.fmt;

  const api = () => (window.SOLARI && window.SOLARI.api) || null;

  // ---- time helpers (wire is ISO-8601 UTC; render local + relative) --------
  function fmtWhen(iso) {
    const t = Date.parse(iso);
    if (isNaN(t)) return "—";
    return new Date(t).toLocaleString(undefined, { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
  }
  function relWhen(iso) {
    const t = Date.parse(iso);
    if (isNaN(t)) return "";
    const min = Math.round((t - Date.now()) / 60000);
    if (min <= 0) return fmt.ago(-min);                        // already elapsed
    if (min < 60) return "in " + min + "m";
    if (min < 1440) return "in " + Math.floor(min / 60) + "h";
    return "in " + Math.floor(min / 1440) + "d";
  }
  // live | upcoming | past — mirrors the bridge's "in effect" test.
  function classify(w) {
    if (w.status === "cancelled" || w.status === "completed") return "past";
    const now = Date.now();
    const s = Date.parse(w.startsAt), e = Date.parse(w.endsAt);
    if (!isNaN(e) && e < now) return "past";
    if (w.live || (!isNaN(s) && !isNaN(e) && now >= s && now <= e)) return "live";
    return "upcoming";
  }
  const targetLabel = (w) => w.scope === "all" ? "Whole fleet" : (w.hostFqdn || "—");

  const fld = { width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 4, borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };
  const lbl = { fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, marginTop: 14, display: "block" };

  /* ===================== SCHEDULE FORM ===================== */
  function ScheduleForm({ nodes, onSubmit }) {
    const [whole, setWhole] = useState(false);
    const [host, setHost] = useState("");
    const [reason, setReason] = useState("");
    const [mode, setMode] = useState("hours");   // "hours" | "window"
    const [hours, setHours] = useState(4);
    const [from, setFrom] = useState("");
    const [to, setTo] = useState("");
    const [busy, setBusy] = useState(false);

    const valid = (whole || host.trim().length > 0) &&
      (mode === "hours" ? Number(hours) > 0 : (from && to && Date.parse(to.replace("T", " ")) > Date.parse(from.replace("T", " "))));

    function submit() {
      if (!valid || busy) return;
      const body = {};
      if (whole) body.all = true; else body.host = host.trim();
      if (reason.trim()) body.reason = reason.trim();
      if (mode === "hours") body.hours = Number(hours);
      else { body.from = from.replace("T", " "); body.to = to.replace("T", " "); }
      setBusy(true);
      Promise.resolve(onSubmit(body)).then((ok) => {
        setBusy(false);
        if (ok) { setHost(""); setReason(""); }   // keep duration for a quick repeat
      });
    }

    return (
      <div className="panel">
        <div className="panel__head">
          <Icon name="wrench" size={16} /><h3>Schedule a window</h3>
        </div>
        <div className="panel__body">
          <div style={{ display: "flex", alignItems: "center", justifyContent: "space-between", gap: 12, padding: "8px 10px", borderRadius: 8, background: "rgba(139,108,255,.06)", border: "1px solid rgba(139,108,255,.2)" }}>
            <div>
              <div style={{ fontWeight: 600, fontSize: 13 }}>Whole fleet</div>
              <div className="td-mono muted" style={{ fontSize: 11 }}>suppress every host at once</div>
            </div>
            <button className={"switch" + (whole ? " on" : "")} onClick={() => setWhole((v) => !v)} aria-label="whole fleet toggle"><i /></button>
          </div>

          <label style={lbl}>Host</label>
          <input list="maint-host-list" style={{ ...fld, opacity: whole ? 0.45 : 1 }} value={host} disabled={whole}
            onChange={(e) => setHost(e.target.value)} placeholder={whole ? "— whole fleet —" : "e.g. cesium, or benzene (probe-only)"}
            autoComplete="off" spellCheck={false} />
          <datalist id="maint-host-list">
            {(nodes || []).map((n) => <option key={n.nodeId} value={n.name}>{n.hostFqdn || n.name}</option>)}
          </datalist>
          {!whole && <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 4 }}>Pick an enrolled node or type any hostname — a probe-only host works too.</div>}

          <label style={lbl}>Reason</label>
          <input style={fld} value={reason} onChange={(e) => setReason(e.target.value)} placeholder="e.g. NVMe swap + firmware flash" />

          <label style={lbl}>Duration</label>
          <div className="seg" style={{ marginTop: 6 }}>
            <button className={mode === "hours" ? "on" : ""} onClick={() => setMode("hours")}><Icon name="clock" size={14} />For N hours</button>
            <button className={mode === "window" ? "on" : ""} onClick={() => setMode("window")}><Icon name="table" size={14} />From / to</button>
          </div>

          {mode === "hours" ? (
            <div style={{ marginTop: 10 }}>
              <label style={lbl}>Hours from now</label>
              <input type="number" min="0.25" step="0.25" style={fld} value={hours} onChange={(e) => setHours(e.target.value)} />
            </div>
          ) : (
            <div style={{ display: "flex", gap: 12, marginTop: 10 }}>
              <div style={{ flex: 1 }}>
                <label style={lbl}>Starts</label>
                <input type="datetime-local" style={fld} value={from} onChange={(e) => setFrom(e.target.value)} />
              </div>
              <div style={{ flex: 1 }}>
                <label style={lbl}>Ends</label>
                <input type="datetime-local" style={fld} value={to} onChange={(e) => setTo(e.target.value)} />
              </div>
            </div>
          )}

          <button className={"btn-primary" + (valid ? "" : " disabled")} onClick={submit} disabled={busy}
            style={{ width: "100%", justifyContent: "center", marginTop: 18 }}>
            <Icon name="wrench" size={14} />{busy ? "Scheduling…" : "Schedule maintenance"}
          </button>
        </div>
      </div>
    );
  }

  /* ===================== SCREEN ===================== */
  function MaintenanceScreen({ toast, onOpenNode }) {
    const [windows, setWindows] = useState(() => S.maintenance || []);
    const [filter, setFilter] = useState("all");      // all | live | upcoming | past
    const [confirmId, setConfirmId] = useState(null); // window pending cancel-confirm
    const [busyId, setBusyId] = useState(null);
    const [, force] = useState(0);

    // Pull the full history (active + scheduled + past) from the API; offline we
    // render the seeded fixture. Re-runs on the app's 10s poll via `force`.
    const load = useCallback(() => {
      const a = api();
      if (a && a.maintenanceList) {
        a.maintenanceList("all").then((ws) => setWindows(ws)).catch(() => { setWindows(S.maintenance || []); });
      } else {
        setWindows(S.maintenance || []);
      }
    }, []);
    useEffect(() => { load(); }, [load]);

    const toastFn = toast || window.__solariToast || (() => {});

    function schedule(body) {
      const a = api();
      if (!a || !a.scheduleMaintenance) {
        // offline: optimistic local window so the list + cross-surface badges move.
        const now = Date.now();
        const start = body.from ? Date.parse(body.from) : now;
        const end = body.to ? Date.parse(body.to) : now + (Number(body.hours) || 1) * 3600000;
        const match = !body.all ? (S.nodes || []).find((n) => n.name === body.host || (n.hostFqdn || "").split(".")[0] === body.host) : null;
        const w = {
          windowId: "local-" + now, scope: body.all ? "all" : "node",
          nodeId: match ? match.nodeId : null,
          hostFqdn: body.all ? null : (match ? match.hostFqdn : (body.host.indexOf(".") > -1 ? body.host : body.host + ".akoria.net")),
          ipAddr: match ? match.ip : null, reason: body.reason || null,
          startsAt: new Date(start).toISOString(), endsAt: new Date(end).toISOString(),
          status: start <= now ? "active" : "scheduled", live: start <= now && end >= now, createdBy: "you",
        };
        setWindows((prev) => [w, ...prev]);
        S.maintenance = [w, ...(S.maintenance || [])];
        toastFn("Scheduled (offline — not persisted)", "wrench");
        force((x) => x + 1);
        return Promise.resolve(true);
      }
      return a.scheduleMaintenance(body).then(() => {
        toastFn(body.all ? "Fleet maintenance scheduled" : "Maintenance scheduled — " + body.host, "wrench");
        if (a.refresh) a.refresh().catch(() => {});
        load();
        return true;
      }).catch((e) => { toastFn("Schedule failed: " + (e && e.message || "error"), "close"); return false; });
    }

    function cancel(w) {
      const a = api();
      const finish = () => { setConfirmId(null); setBusyId(null); toastFn("Window cancelled — #" + w.windowId, "check"); };
      if (!a || !a.cancelMaintenance) {
        setWindows((prev) => prev.map((x) => x.windowId === w.windowId ? { ...x, status: "cancelled", live: false } : x));
        S.maintenance = (S.maintenance || []).filter((x) => x.windowId !== w.windowId);
        force((x) => x + 1);
        finish();
        return;
      }
      setBusyId(w.windowId);
      a.cancelMaintenance(w.windowId).then(() => {
        if (a.refresh) a.refresh().catch(() => {});
        load();
        finish();
      }).catch((e) => { setBusyId(null); toastFn("Cancel failed: " + (e && e.message || "error"), "close"); });
    }

    // counts + filtered view
    const buckets = { live: 0, upcoming: 0, past: 0 };
    windows.forEach((w) => { buckets[classify(w)]++; });
    const sorted = [...windows].sort((a, b) => (Date.parse(b.startsAt) || 0) - (Date.parse(a.startsAt) || 0));
    const shown = sorted.filter((w) => filter === "all" || classify(w) === filter);
    const openNode = onOpenNode || window.__solariOpenEntity || (() => {});

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Maintenance</h1>
            <div className="page-sub">{buckets.live} live · {buckets.upcoming} upcoming · {windows.length} total</div>
          </div>
        </div>

        <div className="kpis" style={{ gridTemplateColumns: "repeat(auto-fit,minmax(150px,1fr))" }}>
          <div className="kpi violet"><div className="kpi__k">Live now</div><div className="kpi__v">{buckets.live}</div><div className="kpi__sub">alerts suppressed</div><div className="kpi__bar" /></div>
          <div className="kpi teal"><div className="kpi__k">Upcoming</div><div className="kpi__v">{buckets.upcoming}</div><div className="kpi__sub">scheduled ahead</div><div className="kpi__bar" /></div>
          <div className="kpi"><div className="kpi__k">Past</div><div className="kpi__v">{buckets.past}</div><div className="kpi__sub">completed / cancelled</div><div className="kpi__bar" /></div>
        </div>

        <div className="two-col" style={{ alignItems: "start" }}>
          <div>
            <div className="filters">
              {[["all", "All", windows.length], ["live", "Live", buckets.live], ["upcoming", "Upcoming", buckets.upcoming], ["past", "Past", buckets.past]].map(([k, label, n]) => (
                <button key={k} className={"chip" + (filter === k ? " on" : "")} onClick={() => setFilter(k)}>
                  {label}<span className="chip__n">{n}</span>
                </button>
              ))}
            </div>

            {shown.length === 0 && <div className="empty">{filter === "all" ? "No maintenance windows scheduled." : "No " + filter + " windows."}</div>}

            {shown.length > 0 && (
              <div className="tablewrap"><table className="grid">
                <thead><tr>
                  <th>Target</th><th>Window</th><th>Status</th><th>Reason</th><th style={{ textAlign: "right" }}>Action</th>
                </tr></thead>
                <tbody>
                  {shown.map((w) => {
                    const c = classify(w);
                    const cancellable = c === "live" || c === "upcoming";
                    return (
                      <tr key={w.windowId}>
                        <td>
                          <div className="td-host" style={{ gap: 8 }}>
                            <Icon name={w.scope === "all" ? "topology" : "host"} size={15} className="ico" />
                            {w.scope === "all"
                              ? <b>Whole fleet</b>
                              : w.nodeId != null
                                ? <span style={{ cursor: "pointer" }} onClick={() => openNode(w.nodeId)} title="Open node">{targetLabel(w)}</span>
                                : <span>{targetLabel(w)}</span>}
                            {w.scope !== "all" && w.nodeId == null && <span className="tag" title="No client agent — matched by IP">probe-only</span>}
                          </div>
                        </td>
                        <td className="td-mono" style={{ fontSize: 11.5 }}>
                          <div>{fmtWhen(w.startsAt)} <span style={{ color: "var(--ink-faint)" }}>→</span> {fmtWhen(w.endsAt)}</div>
                          <div style={{ color: "var(--ink-faint)", fontSize: 10.5 }}>
                            {c === "upcoming" ? "starts " + relWhen(w.startsAt) : c === "live" ? "ends " + relWhen(w.endsAt) : "ended " + relWhen(w.endsAt)}
                          </div>
                        </td>
                        <td>
                          {c === "live"
                            ? <span className="maint-live"><span className="pulse-dot" />Live</span>
                            : c === "upcoming"
                              ? <span className="tag" style={{ color: "var(--teal)", borderColor: "var(--line-glow)" }}>scheduled</span>
                              : <span className="tag">{w.status || "ended"}</span>}
                        </td>
                        <td style={{ fontSize: 12.5, color: "var(--ink-dim)" }}>{w.reason || <span className="muted">—</span>}</td>
                        <td style={{ textAlign: "right" }}>
                          {cancellable ? (
                            confirmId === w.windowId ? (
                              <span style={{ display: "inline-flex", gap: 6, justifyContent: "flex-end" }}>
                                <button className="btn-danger" disabled={busyId === w.windowId} onClick={() => cancel(w)}>
                                  <Icon name="close" size={13} />{busyId === w.windowId ? "…" : "Confirm"}
                                </button>
                                <button className="btn-ghost" onClick={() => setConfirmId(null)}>Keep</button>
                              </span>
                            ) : (
                              <button className="btn-ghost" onClick={() => setConfirmId(w.windowId)} title="Cancel this window">
                                <Icon name="close" size={13} />Cancel
                              </button>
                            )
                          ) : <span className="td-mono" style={{ color: "var(--ink-faint)" }}>—</span>}
                        </td>
                      </tr>
                    );
                  })}
                </tbody>
              </table></div>
            )}
          </div>

          <ScheduleForm nodes={(S.nodes || [])} onSubmit={schedule} />
        </div>
      </div>
    );
  }

  Object.assign(window, { MaintenanceScreen, Maintenance: MaintenanceScreen });
})();
