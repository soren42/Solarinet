/* ============================================================
   SolariNet — app root
   ============================================================ */
(function () {
  const { useState, useEffect, useRef, useCallback } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;
  const { Sidebar, TopBar, CommandPalette, Toasts, FleetOverview, NodeDetail, AlertsScreen, Reachability, Topology, Discovery, Provisioning, ConfigScreen, PoolCards, Assets } = window;

  const PLANNED_LABEL = {
    reachability: ["Reachability Matrix", "Probe targets × monitor vantages — RTT, loss, and split-vantage divergence rendered as a live matrix."],
    topology: ["Topology Map", "Live C2 map of nodes and monitor→target assignment edges (HRW result) across every segment."],
    discovery: ["Discovery", "Auto-found hosts & services (mDNS, ARP, SCP advert) staged for one-tap enrolment into monitoring."],
    provision: ["Provisioning", "Issue enrolment tokens, sign CSRs, and push binaries to new nodes."],
    settings: ["Config & Rules", "Global tolerances, retention, and per-node config overlays — managed centrally, pushed as SCP control frames."],
  };

  function PlannedPage({ id }) {
    const [title, desc] = PLANNED_LABEL[id] || ["Planned", ""];
    const preview = id === "discovery";
    return (
      <div className="page">
        <div className="placeholder-page">
          <div className="box">
            <Icon name={id === "discovery" ? "discovery" : id === "topology" ? "topology" : id === "reachability" ? "reachability" : id === "provision" ? "provision" : "settings"} size={52} className="ico" />
            <h2>{title}</h2>
            <p>{desc}</p>
            <div className="tag" style={{ display: "inline-block", marginTop: 8 }}>next iteration</div>
            {preview && (
              <div style={{ marginTop: 26, textAlign: "left" }}>
                <div className="page-sub" style={{ marginBottom: 10 }}>Discovered · not yet monitored</div>
                {S.discovered.map((d, i) => (
                  <div key={i} className="alert-row info" style={{ cursor: "default" }}>
                    <Icon name={d.kind === "service" ? "link" : "host"} size={18} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                    <div className="alert-main"><div className="t">{d.host}</div><div className="d">{d.services.join(" · ")} — via {d.via}</div></div>
                    <div className="alert-meta">{d.seen}m ago<div><span className="tag" style={{ borderColor: "var(--line-glow)", color: "var(--teal)" }}>+ monitor</span></div></div>
                  </div>
                ))}
              </div>
            )}
          </div>
        </div>
      </div>
    );
  }

  function App() {
    const [theme, setTheme] = useState(() => localStorage.getItem("solari-theme") || "dark");
    const [route, setRoute] = useState({ name: "fleet" });
    const [fleetView, setFleetView] = useState("heat");
    const [collapsed, setCollapsed] = useState(false);
    const [navHidden, setNavHidden] = useState(false);
    const [cmdOpen, setCmdOpen] = useState(false);
    const [toasts, setToasts] = useState([]);
    const [rules, setRules] = useState(S.rules);
    const [, force] = useState(0);

    useEffect(() => { document.documentElement.setAttribute("data-theme", theme); localStorage.setItem("solari-theme", theme); }, [theme]);

    const toast = useCallback((msg, icon) => {
      const id = Math.random().toString(36).slice(2);
      setToasts((t) => [...t, { id, msg, icon }]);
      setTimeout(() => setToasts((t) => t.filter((x) => x.id !== id)), 2600);
    }, []);

    useEffect(() => { window.__solariToast = toast; }, [toast]);

    // keyboard: ⌘K / "/" open palette
    useEffect(() => {
      function onKey(e) {
        if ((e.metaKey || e.ctrlKey) && e.key.toLowerCase() === "k") { e.preventDefault(); setCmdOpen(true); }
        else if (e.key === "/" && document.activeElement.tagName !== "INPUT") { e.preventDefault(); setCmdOpen(true); }
      }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, []);

    // live tick — gentle moving sparklines + jittered load
    useEffect(() => {
      const iv = setInterval(() => {
        S.nodes.forEach((n) => {
          if (n.state === "down") return;
          const base = n.cpuPct;
          let nv = base + (Math.random() - 0.5) * 9;
          nv = Math.max(0, Math.min(100, nv));
          n.hist.cpu = [...n.hist.cpu.slice(1), Math.round(nv)];
          n.hist.net = [...n.hist.net.slice(1), Math.max(0, Math.min(100, n.hist.net[n.hist.net.length - 1] + (Math.random() - 0.5) * 16))];
          n.cpuPct = Math.round(nv);
        });
        force((x) => x + 1);
      }, 5000);
      return () => clearInterval(iv);
    }, []);

    const openNode = useCallback((nodeOrId) => {
      const node = typeof nodeOrId === "string" ? S.nodes.find((n) => n.nodeId === nodeOrId) : nodeOrId;
      if (node) { setRoute({ name: "node", node }); document.querySelector(".content") && (document.querySelector(".content").scrollTop = 0); }
    }, []);

    const go = useCallback((id) => {
      setRoute({ name: id });
      if (window.innerWidth <= 980) setNavHidden(true);
      const c = document.querySelector(".content"); if (c) c.scrollTop = 0;
    }, []);

    const survey = useCallback((node, kind) => {
      const api = S.api;
      const label = kind === "config"
        ? `Re-converge requested${node ? " — " + node.name : ""}`
        : (node ? `Survey requested — ${node.name}` : "Fleet survey dispatched");
      const icon = kind === "config" ? "settings" : "survey";
      // SURVEY is a fleet-wide demand (SCP_MSG_SURVEY) published to the fleet.
      if (api && api.survey) {
        api.survey("all")
          .then(() => toast(label, icon))
          .catch((e) => toast("Survey failed: " + (e && e.message), "alerts"));
      } else {
        toast(label + " (offline)", icon);
      }
    }, [toast]);

    function toggleNav() {
      if (window.innerWidth <= 980) setNavHidden((h) => !h);
      else setCollapsed((c) => !c);
    }

    // command palette commands
    const commands = (() => {
      const cmds = [
        { id: "go-fleet", group: "Navigate", label: "Fleet Overview", icon: "overview", action: () => go("fleet") },
        { id: "go-alerts", group: "Navigate", label: "Alerts & Tolerances", icon: "alerts", action: () => go("alerts"), sub: `${S.activeCrit + S.activeWarn} active` },
        { id: "go-reach", group: "Navigate", label: "Reachability Matrix", icon: "reachability", action: () => go("reachability") },
        { id: "go-topo", group: "Navigate", label: "Topology Map", icon: "topology", action: () => go("topology") },
        { id: "go-systems", group: "Navigate", label: "Systems", icon: "host", action: () => go("assets"), sub: `${(S.assets || []).length} monitored` },
        { id: "go-disc", group: "Navigate", label: "Discovery", icon: "discovery", action: () => go("discovery"), sub: `${S.discovered.length} new` },
        { id: "go-prov", group: "Navigate", label: "Provisioning", icon: "provision", action: () => go("provision"), sub: `${S.enrollments.length} pending` },
        { id: "go-cfg", group: "Navigate", label: "Config & Rules", icon: "settings", action: () => go("settings") },
        { id: "view-heat", group: "Actions", label: "Fleet: Heatmap view", icon: "grid", action: () => { setFleetView("heat"); go("fleet"); } },
        { id: "view-table", group: "Actions", label: "Fleet: Table view", icon: "table", action: () => { setFleetView("table"); go("fleet"); } },
        { id: "view-cards", group: "Actions", label: "Fleet: Cards view", icon: "cards", action: () => { setFleetView("cards"); go("fleet"); } },
        { id: "survey-all", group: "Actions", label: "Survey entire fleet now", icon: "survey", action: () => survey(null) },
        { id: "theme", group: "Actions", label: "Toggle dark / light theme", icon: theme === "dark" ? "sun" : "moon", action: () => setTheme((t) => t === "dark" ? "light" : "dark") },
        { id: "logout", group: "Actions", label: "Log out", icon: "enter", action: () => {
            const api = window.SolariAPI;
            const done = () => window.location.reload();
            (api && api.logout ? api.logout() : Promise.resolve()).then(done, done);
        } },
      ];
      // node jump targets — prioritise problem nodes, bound the list
      const problem = S.nodes.filter((n) => n.state === "down" || n.state === "degraded");
      const healthy = S.nodes.filter((n) => n.state === "up").slice(0, 40);
      [...problem, ...healthy].forEach((n) => {
        cmds.push({ id: "node-" + n.nodeId, group: "Jump to node", label: `${n.name}.akoria.net`, dot: n.state, sub: n.segName, keywords: n.role + " " + n.ip + " " + n.segName, action: () => openNode(n) });
      });
      return cmds;
    })();

    return (
      <div className="app">
        {S.source === "offline" && (
          <div style={{ position: "fixed", top: 0, left: 0, right: 0, zIndex: 9999, background: "var(--amber, #ffb23d)", color: "#05080e", textAlign: "center", fontSize: 12, fontWeight: 600, padding: "5px 10px", letterSpacing: "0.02em" }}>
            ⚠ DEMO DATA — live API unreachable{S.offlineReason ? " (" + S.offlineReason + ")" : ""}; showing the offline fixture, not your fleet.
          </div>
        )}
        {!navHidden && window.innerWidth <= 980 && <div className="scrim" onClick={() => setNavHidden(true)} />}
        <Sidebar active={route.name === "node" ? "fleet" : route.name}
          onNav={go} collapsed={collapsed} onToggle={() => setCollapsed((c) => !c)} hidden={navHidden} summary={S.summary} activeCrit={S.activeCrit} />
        <div className="main">
          <TopBar onMenu={toggleNav} onOpenCmd={() => setCmdOpen(true)} theme={theme}
            onToggleTheme={() => setTheme((t) => t === "dark" ? "light" : "dark")}
            server={S.server} onSurvey={() => survey(null)} />
          <div className="content">
            {route.name === "fleet" && PoolCards && <PoolCards onOpen={() => go("assets")} />}
            {route.name === "fleet" && <FleetOverview onOpenNode={openNode} view={fleetView} setView={setFleetView} fleet={S.fleet || S.nodes} />}
            {route.name === "assets" && Assets && <Assets onOpenNode={openNode} />}
            {route.name === "node" && <NodeDetail node={route.node} onBack={() => setRoute({ name: "fleet" })} onSurvey={survey} />}
            {route.name === "alerts" && <AlertsScreen onOpenNode={openNode} rules={rules} setRules={setRules} toast={toast} />}
            {route.name === "reachability" && <Reachability onOpenNode={openNode} />}
            {route.name === "topology" && <Topology onOpenNode={openNode} />}
            {route.name === "discovery" && <Discovery onOpenNode={openNode} />}
            {route.name === "provision" && <Provisioning onOpenNode={openNode} />}
            {route.name === "settings" && <ConfigScreen onNav={go} />}
          </div>
        </div>
        <CommandPalette open={cmdOpen} onClose={() => setCmdOpen(false)} commands={commands} />
        <Toasts items={toasts} />
      </div>
    );
  }

  // Login gate. When the live API is reachable but we have no session, api.jsx
  // sets window.SOLARI_NEEDS_AUTH; we show this instead of the app. On success we
  // reload so the adapter re-boots against the now-authenticated API.
  function LoginScreen() {
    const [u, setU] = useState("");
    const [p, setP] = useState("");
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const submit = (e) => {
      e.preventDefault();
      if (!u || !p) return;
      setBusy(true); setErr("");
      const api = window.SolariAPI;
      (api && api.login ? api.login(u, p) : Promise.reject(new Error("API unavailable")))
        .then(() => window.location.reload())
        .catch((ex) => { setErr((ex && ex.message) || "Login failed"); setBusy(false); });
    };
    const field = { width: "100%", boxSizing: "border-box", padding: "10px 12px", marginBottom: 10, borderRadius: 8, border: "1px solid rgba(255,255,255,0.12)", background: "rgba(255,255,255,0.04)", color: "inherit", fontSize: 14, fontFamily: "inherit" };
    return (
      <div style={{ minHeight: "100vh", display: "flex", alignItems: "center", justifyContent: "center", padding: 20 }}>
        <form onSubmit={submit} style={{ width: 320, padding: 28, borderRadius: 14, background: "rgba(255,255,255,0.04)", border: "1px solid rgba(255,255,255,0.08)", boxShadow: "0 12px 48px rgba(0,0,0,0.45)" }}>
          <div style={{ textAlign: "center", marginBottom: 20 }}>
            <Icon name="shield" size={34} style={{ color: "var(--teal, #35e0d0)" }} />
            <div style={{ fontWeight: 700, fontSize: 18, marginTop: 6 }}>SolariNet</div>
            <div style={{ fontSize: 12, opacity: 0.6, marginTop: 2 }}>Monitoring · sign in</div>
          </div>
          <input autoFocus value={u} onChange={(e) => setU(e.target.value)} placeholder="Username" autoComplete="username" style={field} />
          <input type="password" value={p} onChange={(e) => setP(e.target.value)} placeholder="Password" autoComplete="current-password" style={field} />
          {err ? <div style={{ color: "var(--red, #ff3d72)", fontSize: 12, margin: "2px 0 10px" }}>{err}</div> : null}
          <button type="submit" disabled={busy || !u || !p}
            style={{ width: "100%", padding: "10px 12px", borderRadius: 8, border: "none", cursor: busy ? "default" : "pointer", fontWeight: 600, fontSize: 14, background: "var(--teal, #35e0d0)", color: "#05080e", opacity: (busy || !u || !p) ? 0.6 : 1 }}>
            {busy ? "Signing in…" : "Sign in"}
          </button>
        </form>
      </div>
    );
  }

  // Boot against the adapter: wait for window.solariReady (api.jsx) so the first
  // paint reflects the live API when reachable, else the offline fixture. The
  // adapter mutates window.SOLARI in place, so the `S` captured above is live by
  // the time this resolves. If api.jsx is absent (or older), render immediately.
  function mount() {
    const root = ReactDOM.createRoot(document.getElementById("root"));
    if (window.SOLARI_NEEDS_AUTH) { root.render(<LoginScreen />); return; }
    root.render(<App />);
  }
  if (window.solariReady && typeof window.solariReady.then === "function") {
    window.solariReady.then(mount, mount);
  } else {
    mount();
  }
})();
