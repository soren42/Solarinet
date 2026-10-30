/*
 * screens-rackwire.jsx — RackWire network, power, and interconnect planning page (window.RackWireScreen).
 *
 * CONTRACT-RW.md §5.7 (SPA embedding). RackWire is NOT a React screen: it is a
 * self-contained, build-step-free app under public/rackwire/ that also runs from
 * disk on an iPad. This page embeds it in a same-origin iframe and speaks the
 * documented postMessage bridge (HANDOFF §4) — nothing here reaches inside the
 * frame, so the standalone file keeps working untouched.
 *
 *   host  → frame  { source:'rackwire', cmd:'<method>', args:[...] }
 *   frame → host   { source:'rackwire', event:'ready'|'change'|'select'|'reply', payload }
 *
 * Three jobs:
 *   1. Theme push. The frame does not load styles.css, so on `ready` and on every
 *      <html data-theme> flip we send `setTheme(name, vars)` with this document's
 *      RESOLVED dashboard tokens. The frame's bridge stylesheet then resolves
 *      var(--accent) &c. against the real palette instead of its copied fallbacks.
 *   2. Open standalone. A plain link out to the same file without ?embed=1, so the
 *      planner can be driven full-window or saved to an iPad home screen.
 *   3. Select / focus relay. Selection made inside the frame surfaces here as chips
 *      (frame → host `select`); clicking one sends `focus` back and Clear sends
 *      `select([])`, so the page chrome and the canvas never disagree.
 *
 * SPA idiom: Babel-in-browser, no build step, no imports, window globals only.
 */
(function () {
  const { useState, useEffect, useRef, useCallback } = React;
  const Icon = window.Icon;

  // The dashboard tokens the frame's bridge stylesheet actually reads. Keep in
  // step with the :root block in rackwire/index.html — anything not listed falls
  // back to the bridge's own copy of the styles.css value, which is correct but
  // frozen at the time that file was written.
  const PUSH_TOKENS = [
    "--bg", "--bg-deep", "--panel", "--panel-3",
    "--ink", "--ink-dim", "--ink-faint",
    "--line", "--line-2", "--accent",
    "--ok", "--warn", "--crit", "--crit-bg", "--violet",
  ];

  function readTokens() {
    const cs = getComputedStyle(document.documentElement);
    const out = {};
    PUSH_TOKENS.forEach((k) => {
      const v = cs.getPropertyValue(k);
      if (v && v.trim()) out[k] = v.trim();
    });
    return out;
  }

  // Release tag, in step with the ?v= on the script tags in public/index.html.
  // The frame is a separate document, so the SPA's own cache-bust does not reach
  // it — without this an operator keeps a stale planner after a deploy.
  const RW_V = "20261030a";

  function currentTheme() {
    return document.documentElement.getAttribute("data-theme") === "light" ? "light" : "dark";
  }

  function namesFromDevices(devices) {
    if (!Array.isArray(devices)) return null;
    return devices.reduce((names, device) => {
      if (!device || typeof device.uid !== "string") return names;
      const label = typeof device.name === "string" && device.name.trim()
        ? device.name.trim()
        : typeof device.model === "string" && device.model.trim()
          ? device.model.trim()
          : device.uid;
      names[device.uid] = label;
      return names;
    }, {});
  }

  function RackWireScreen({ onNavigate }) {
    const frame = useRef(null);
    const ready = useRef(false);
    const [live, setLive] = useState(false);
    const [sel, setSel] = useState([]);
    const [counts, setCounts] = useState(null);
    const [devNames, setDevNames] = useState({});
    const [view, setView] = useState("map");

    // host → frame
    const send = useCallback((cmd, args) => {
      const el = frame.current;
      if (!el || !el.contentWindow) return;
      try {
        el.contentWindow.postMessage({ source: "rackwire", cmd: cmd, args: args || [] }, window.location.origin);
      } catch (e) { /* frame not ready yet; the ready handshake re-pushes */ }
    }, []);

    const pushTheme = useCallback(() => {
      send("setTheme", [currentTheme(), readTokens()]);
    }, [send]);

    // frame → host
    useEffect(() => {
      function onMsg(e) {
        if (e.origin !== window.location.origin) return;
        // Same origin is not enough: any other frame or opener on this origin could
        // forge a rackwire event. Only the planner frame we mounted is the peer.
        const el = frame.current;
        if (!el || !el.contentWindow || e.source !== el.contentWindow) return;
        const m = e.data;
        if (!m || m.source !== "rackwire" || !m.event) return;
        if (m.event === "navigate") {
          const route = m.payload && m.payload.route;
          if ((route === "discovery" || route === "inventory") && typeof onNavigate === "function") onNavigate(route);
          return;
        }
        if (m.event === "ready") {
          ready.current = true;
          setLive(true);
          pushTheme();
          send("getPlan");
          return;
        }
        if (m.event === "select") { setSel([].concat(m.payload || [])); return; }
        if (m.event === "reply" && m.payload && m.payload.cmd === "getPlan") {
          const names = namesFromDevices(m.payload.value && m.payload.value.devices);
          if (names) setDevNames(names);
          return;
        }
        if (m.event === "change") {
          const p = m.payload || {};
          setCounts({ devices: (p.devices || []).length, cables: (p.cables || []).length });
          const names = namesFromDevices(p.devices);
          if (names) setDevNames(names);
        }
      }
      window.addEventListener("message", onMsg);
      return () => window.removeEventListener("message", onMsg);
    }, [pushTheme, onNavigate]);

    // re-push on theme flip (TopBar / command palette stamp data-theme on <html>)
    useEffect(() => {
      if (typeof MutationObserver !== "function") return undefined;
      const obs = new MutationObserver(() => { if (ready.current) pushTheme(); });
      obs.observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
      return () => obs.disconnect();
    }, [pushTheme]);

    function setFrameView(v) {
      setView(v);
      send("setView", [v]);
    }

    const devName = (uid) => devNames[uid] || uid;

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">RackWire</h1>
            <div className="page-sub">
              Network, power & interconnect planning · ports, circuits, PoE and live telemetry
              {counts ? ` · ${counts.devices} devices · ${counts.cables} cables` : ""}
            </div>
          </div>
          <div className="page-head__right">
            <button type="button" className="btn-ghost" onClick={() => onNavigate && onNavigate("discovery")}>Network discovery</button>
            <button type="button" className="btn-ghost" onClick={() => onNavigate && onNavigate("inventory")}>Inventory</button>
            <button type="button" className="btn-ghost" onClick={() => send("openScan")}>Scan inventory barcode</button>
            <div className="seg" role="group" aria-label="Planner view">
              <button type="button" className={view === "map" ? "on" : ""} aria-pressed={view === "map"} onClick={() => setFrameView("map")}>Map</button>
              <button type="button" className={view === "table" ? "on" : ""} aria-pressed={view === "table"} onClick={() => setFrameView("table")}>Table</button>
            </div>
            <a className="btn-ghost" href={`rackwire/index.html?v=${RW_V}`} target="_blank" rel="noopener"
               style={{ display: "inline-flex", alignItems: "center", gap: 7, lineHeight: "38px", textDecoration: "none" }}>
              <Icon name="link" size={14} /> Open standalone
            </a>
          </div>
        </div>

        {sel.length > 0 && (
          <div className="panel" style={{ marginBottom: 12 }}>
            <div className="panel__body" style={{ display: "flex", alignItems: "center", gap: 8, flexWrap: "wrap" }}>
              <span className="page-sub" style={{ marginTop: 0 }}>Selected</span>
              {sel.slice(0, 12).map((uid) => (
                <button key={uid} className="chip" onClick={() => send("focus", [uid])} title={`Device ID: ${uid}`} aria-label={`Centre the map on ${devName(uid)}`}>
                  {devName(uid)}
                </button>
              ))}
              <button className="btn-ghost" style={{ marginLeft: "auto" }} onClick={() => { send("select", [[]]); setSel([]); }}>Clear</button>
            </div>
          </div>
        )}

        <div className="panel" style={{ overflow: "hidden" }}>
          <iframe
            ref={frame}
            title="RackWire network, power, and interconnect planner"
            src={`rackwire/index.html?embed=1&solarinet=1&integrated=1&v=${RW_V}`}
            onLoad={() => { if (ready.current) pushTheme(); }}
            style={{
              display: "block",
              width: "100%",
              height: "calc(100vh - 210px)",
              minHeight: 520,
              border: 0,
              background: "var(--bg)",
            }}
          />
        </div>

        <div className="page-sub" role="status" aria-live="polite" aria-atomic="true" style={{ marginTop: 10 }}>
          {live ? "Planner ready." : "Waiting for the planner to hand back its ready frame…"}
        </div>
      </div>
    );
  }

  Object.assign(window, { RackWireScreen: RackWireScreen });
})();
