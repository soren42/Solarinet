/* ============================================================
   SolariNet — Maintenance windows (scheduled downtime)

   A window suppresses alerting for a host (or the whole fleet) over a
   time range, so an expected-down system reads "maintenance", not "down".

   TIMEZONE: the API stores & returns startsAt/endsAt as UTC DATETIME
   strings ("YYYY-MM-DD HH:MM:SS"). We DISPLAY everything in the browser's
   LOCAL zone (parseUtc → toLocaleString). When the operator picks an
   explicit from/to we send the LOCAL wall-clock ("YYYY-MM-DD HH:MM");
   durations go through the `hours` field (unambiguous — preferred).

   No build step: window-global IIFE idiom, loaded after screens3.jsx.
   Exports: MaintenanceScreen (route), MaintenanceBadge + maintenanceForHost
   (the reusable "In Maintenance" badge surfaced on Fleet + Alerts).
   ============================================================ */
(function () {
  const { useState, useEffect, useMemo, useCallback } = React;
  const Icon = window.Icon;

  // ---- UTC-string → local Date helpers -------------------------------
  // The wire carries UTC without a zone marker; append 'Z' so the browser
  // parses it as UTC and toLocaleString renders it in the viewer's zone.
  function parseUtc(s) {
    if (!s) return null;
    const str = String(s).trim();
    const hasZone = /[zZ]$|[+\-]\d\d:?\d\d$/.test(str);
    const d = hasZone ? new Date(str) : new Date(str.replace(" ", "T") + "Z");
    return isNaN(d.getTime()) ? null : d;
  }
  function fmtLocal(s) {
    const d = parseUtc(s);
    if (!d) return "—";
    return d.toLocaleString(undefined, { month: "short", day: "numeric", hour: "2-digit", minute: "2-digit" });
  }
  function fmtLocalTime(s) {
    const d = parseUtc(s);
    if (!d) return "—";
    return d.toLocaleTimeString(undefined, { hour: "2-digit", minute: "2-digit" });
  }
  // Same calendar day (local) → collapse the second endpoint to just its time.
  function fmtRange(startsAt, endsAt) {
    const a = parseUtc(startsAt), b = parseUtc(endsAt);
    if (!a) return "—";
    if (!b) return fmtLocal(startsAt);
    const sameDay = a.toDateString() === b.toDateString();
    return fmtLocal(startsAt) + " → " + (sameDay ? fmtLocalTime(endsAt) : fmtLocal(endsAt));
  }

  // Bucket a window into the API's status filter vocabulary.
  function classify(w) {
    if (w.live || w.status === "active") return "active";
    if (w.status === "completed" || w.status === "cancelled") return "past";
    const end = parseUtc(w.endsAt);
    if (end && end.getTime() < Date.now()) return "past";
    return "upcoming";
  }
  const isLive = (w) => !!(w && (w.live || w.status === "active"));

  // ---- cross-reference: is a host under an ACTIVE window right now? ----
  // Matches by nodeId, then hostFqdn (case-insensitive), then IP — so both
  // enrolled nodes and probe-only hosts resolve. scope:'all' matches anything.
  function maintenanceForHost(ref) {
    ref = ref || {};
    const nodeId = ref.nodeId != null ? ref.nodeId : (ref.node && ref.node.nodeId);
    const hostFqdn = ref.hostFqdn || (ref.node && ref.node.hostFqdn) || null;
    const ip = ref.ip || ref.ipAddr || (ref.node && ref.node.ip) || null;
    const list = (window.SOLARI && window.SOLARI.maintenance) || [];
    const fq = hostFqdn ? String(hostFqdn).toLowerCase() : null;
    return list.filter(isLive).find(function (w) {
      if (w.scope === "all") return true;
      if (nodeId != null && w.nodeId != null && String(w.nodeId) === String(nodeId)) return true;
      if (fq && w.hostFqdn && String(w.hostFqdn).toLowerCase() === fq) return true;
      if (ip && w.ipAddr && w.ipAddr === ip) return true;
      return false;
    }) || null;
  }

  // Reusable badge. Self-hides when the host is not under an active window, so
  // callers can drop it in unconditionally.
  function MaintenanceBadge(props) {
    const w = maintenanceForHost(props);
    if (!w) return null;
    const title = w.scope === "all"
      ? "Fleet-wide maintenance window" + (w.reason ? " — " + w.reason : "")
      : "In maintenance" + (w.reason ? " — " + w.reason : "") +
        (w.endsAt ? " · until " + fmtLocal(w.endsAt) : "");
    return (
      <span className="maint-badge" title={title} style={props.style}>
        <Icon name="maintenance" size={11} />Maint
      </span>
    );
  }

  // ---- schedule form -------------------------------------------------
  function ScheduleForm({ onScheduled, toast }) {
    const [scopeAll, setScopeAll] = useState(false);
    const [host, setHost] = useState("");
    const [reason, setReason] = useState("");
    const [mode, setMode] = useState("duration");   // "duration" | "window"
    const [hours, setHours] = useState(2);
    const [from, setFrom] = useState("");
    const [to, setTo] = useState("");
    const [busy, setBusy] = useState(false);

    // known hosts for the datalist — enrolled nodes + adopted systems. The input
    // is free text too, so a probe-only host ("benzene") can just be typed.
    const hosts = useMemo(function () {
      const seen = {}; const out = [];
      ((window.SOLARI.fleet) || (window.SOLARI.nodes) || []).forEach(function (n) {
        const h = n.hostFqdn || n.name;
        if (h && !seen[h]) { seen[h] = 1; out.push(h); }
      });
      return out.sort();
    }, []);

    const valid = (scopeAll || host.trim().length > 0) &&
      (mode === "duration" ? Number(hours) > 0 : (from && to));

    function submit() {
      if (!valid || busy) return;
      const api = window.SOLARI.api;
      if (!api || !api.scheduleMaintenance || window.SOLARI.source !== "live") {
        toast && toast("Scheduling unavailable (offline demo)", "close");
        return;
      }
      const body = {};
      if (scopeAll) body.all = true; else body.host = host.trim();
      if (reason.trim()) body.reason = reason.trim();
      if (mode === "duration") {
        body.hours = Number(hours) || 0;
      } else {
        // datetime-local gives "YYYY-MM-DDTHH:MM" in LOCAL wall-clock; the API
        // expects "YYYY-MM-DD HH:MM" and interprets it in the server's zone.
        body.from = from.replace("T", " ");
        body.to = to.replace("T", " ");
      }
      setBusy(true);
      api.scheduleMaintenance(body).then(function () {
        toast && toast(scopeAll ? "Fleet-wide maintenance scheduled" : "Maintenance scheduled — " + host.trim(), "maintenance");
        setHost(""); setReason(""); setFrom(""); setTo("");
        if (api.refresh) api.refresh().catch(function () {});
        onScheduled && onScheduled();
        setBusy(false);
      }).catch(function (e) {
        toast && toast("Schedule failed: " + ((e && e.message) || "error"), "close");
        setBusy(false);
      });
    }

    return (
      <div className="panel" style={{ marginBottom: 20 }}>
        <div className="panel__head">
          <Icon name="calendar" size={16} /><h3>Schedule a window</h3>
        </div>
        <div className="panel__body">
          <div className="maint-form">
            <div className="full" style={{ display: "flex", alignItems: "center", gap: 10 }}>
              <button className={"switch" + (scopeAll ? " on" : "")} onClick={() => setScopeAll((v) => !v)} aria-label="toggle whole-fleet scope"><i /></button>
              <span style={{ fontSize: 13 }}>Whole fleet{scopeAll ? "" : " — or a single host:"}</span>
            </div>

            {!scopeAll && (
              <div className="full">
                <label className="maint-lbl">Host</label>
                <input className="maint-input" list="maint-known-hosts" value={host}
                  onChange={(e) => setHost(e.target.value)} placeholder="pick a node, or type any host (e.g. benzene)" spellCheck={false} autoComplete="off" />
                <datalist id="maint-known-hosts">
                  {hosts.map((h) => <option key={h} value={h} />)}
                </datalist>
              </div>
            )}

            <div className="full">
              <label className="maint-lbl">Reason <span style={{ opacity: .5 }}>(optional)</span></label>
              <input className="maint-input" value={reason} onChange={(e) => setReason(e.target.value)} placeholder="e.g. kernel upgrade + reboot" />
            </div>

            <div className="full">
              <label className="maint-lbl">When</label>
              <div className="seg" style={{ marginBottom: 10 }}>
                <button className={mode === "duration" ? "on" : ""} onClick={() => setMode("duration")}><Icon name="clock" size={14} />Duration</button>
                <button className={mode === "window" ? "on" : ""} onClick={() => setMode("window")}><Icon name="calendar" size={14} />From / to</button>
              </div>
            </div>

            {mode === "duration" ? (
              <div>
                <label className="maint-lbl">Duration (hours from now)</label>
                <input className="maint-input" type="number" min="0.5" step="0.5" value={hours} onChange={(e) => setHours(e.target.value)} />
              </div>
            ) : (
              <React.Fragment>
                <div>
                  <label className="maint-lbl">Starts (your local time)</label>
                  <input className="maint-input" type="datetime-local" value={from} onChange={(e) => setFrom(e.target.value)} />
                </div>
                <div>
                  <label className="maint-lbl">Ends (your local time)</label>
                  <input className="maint-input" type="datetime-local" value={to} onChange={(e) => setTo(e.target.value)} />
                </div>
              </React.Fragment>
            )}

            <div className="full" style={{ display: "flex", justifyContent: "flex-end", marginTop: 4 }}>
              <button className={"btn-primary" + (valid ? "" : " disabled")} disabled={busy} onClick={submit}>
                <Icon name="maintenance" size={14} />{busy ? "Scheduling…" : "Schedule maintenance"}
              </button>
            </div>
          </div>
        </div>
      </div>
    );
  }

  // ---- a single window row (inline-confirm cancel) -------------------
  function WindowRow({ w, onCancelled, toast }) {
    const [confirming, setConfirming] = useState(false);
    const [busy, setBusy] = useState(false);
    const kind = classify(w);
    const live = isLive(w);
    const label = w.scope === "all" ? "Whole fleet" : (w.hostFqdn ? w.hostFqdn.split(".")[0] : (w.nodeId != null ? "node " + w.nodeId : "host"));
    const fqdn = w.scope === "all" ? null : (w.hostFqdn && w.hostFqdn.indexOf(".") > -1 ? w.hostFqdn.slice(w.hostFqdn.indexOf(".")) : (w.ipAddr || null));

    function cancel() {
      const api = window.SOLARI.api;
      if (!api || !api.cancelMaintenance || window.SOLARI.source !== "live") {
        toast && toast("Cancel unavailable (offline demo)", "close");
        setConfirming(false);
        return;
      }
      setBusy(true);
      api.cancelMaintenance(w.windowId).then(function () {
        toast && toast("Maintenance cancelled — " + label, "check");
        if (api.refresh) api.refresh().catch(function () {});
        onCancelled && onCancelled();
      }).catch(function (e) {
        toast && toast("Cancel failed: " + ((e && e.message) || "error"), "close");
        setBusy(false); setConfirming(false);
      });
    }

    return (
      <div className={"maint-row" + (live ? " is-live" : "") + (kind === "past" ? " is-past" : "")}>
        <Icon name={w.scope === "all" ? "overview" : "maintenance"} size={18} style={{ color: live ? "var(--ok)" : "var(--violet)", flex: "0 0 auto" }} />
        <div className="maint-row__main">
          <div className="maint-row__host">
            {label}{fqdn && <span className="fqdn">{fqdn}</span>}
            {live && <span className="maint-live-pill">Live</span>}
            {kind === "upcoming" && <span className="tag">upcoming</span>}
            {w.status === "cancelled" && <span className="tag">cancelled</span>}
            {w.status === "completed" && <span className="tag">completed</span>}
          </div>
          <div className="maint-row__when"><Icon name="clock" size={11} style={{ verticalAlign: -1, marginRight: 4 }} />{fmtRange(w.startsAt, w.endsAt)}{w.createdBy ? " · by " + w.createdBy : ""}</div>
          {w.reason && <div className="maint-row__reason">{w.reason}</div>}
        </div>
        {kind !== "past" && (
          confirming ? (
            <div style={{ display: "inline-flex", gap: 6, alignItems: "center", flex: "0 0 auto" }}>
              <span className="td-mono" style={{ fontSize: 11, color: "var(--ink-faint)" }}>Cancel?</span>
              <button className="btn-primary" disabled={busy} onClick={cancel}><Icon name="check" size={13} />{busy ? "…" : "Yes"}</button>
              <button className="btn-ghost" disabled={busy} onClick={() => setConfirming(false)}>No</button>
            </div>
          ) : (
            <button className="btn-ghost" style={{ flex: "0 0 auto" }} onClick={() => setConfirming(true)} title="Cancel this window">
              <Icon name="close" size={13} />Cancel
            </button>
          )
        )}
      </div>
    );
  }

  // ---- the screen ----------------------------------------------------
  function MaintenanceScreen({ toast }) {
    const [filter, setFilter] = useState("active");   // active | upcoming | past | all
    const [windows, setWindows] = useState(null);
    const [nonce, setNonce] = useState(0);
    const bump = useCallback(() => setNonce((n) => n + 1), []);

    // Load the filtered list. Live: hit the API (server-side status filter).
    // Offline / no adapter: filter the in-memory fixture client-side.
    useEffect(function () {
      let alive = true;
      const api = window.SOLARI.api;
      function localFiltered() {
        const all = (window.SOLARI.maintenance) || [];
        return filter === "all" ? all.slice() : all.filter((w) => classify(w) === filter);
      }
      if (api && api.maintenanceList && window.SOLARI.source === "live") {
        api.maintenanceList(filter)
          .then(function (list) { if (alive) setWindows(list); })
          .catch(function () { if (alive) setWindows(localFiltered()); });
      } else {
        setWindows(localFiltered());
      }
      return function () { alive = false; };
    }, [filter, nonce]);

    const all = (window.SOLARI.maintenance) || [];
    const liveCount = all.filter(isLive).length;
    const upcomingCount = all.filter((w) => classify(w) === "upcoming").length;
    const rows = windows || [];

    const FILTERS = [
      ["active", "Active", liveCount],
      ["upcoming", "Upcoming", upcomingCount],
      ["past", "Past", null],
      ["all", "All", all.length],
    ];

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Maintenance</h1>
            <div className="page-sub">{liveCount} live · {upcomingCount} upcoming · alerting suppressed while a window is open</div>
          </div>
        </div>

        <ScheduleForm onScheduled={bump} toast={toast} />

        <div className="filters" style={{ marginBottom: 14 }}>
          {FILTERS.map(([k, lbl, n]) => (
            <button key={k} className={"chip" + (filter === k ? " on" : "")} onClick={() => setFilter(k)}>
              {lbl}{n != null && <span className="chip__n">{n}</span>}
            </button>
          ))}
        </div>

        <div className="maint-list">
          {windows === null && <div className="empty">Loading…</div>}
          {windows !== null && rows.length === 0 && (
            <div className="empty">
              {filter === "active" ? "No active maintenance — the fleet is fully monitored."
                : filter === "upcoming" ? "No upcoming windows scheduled."
                : filter === "past" ? "No past windows."
                : "No maintenance windows."}
            </div>
          )}
          {rows.map((w) => <WindowRow key={w.windowId} w={w} onCancelled={bump} toast={toast} />)}
        </div>
      </div>
    );
  }

  Object.assign(window, { MaintenanceScreen, MaintenanceBadge, maintenanceForHost });
})();
