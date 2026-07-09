/* ============================================================
   SolariNet — app root
   ============================================================ */
(function () {
  const { useState, useEffect, useRef, useCallback } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;
  const { Sidebar, TopBar, CommandPalette, Toasts, FleetOverview, NodeDetail, AlertsScreen, Reachability, Topology, Discovery, Provisioning, ConfigScreen, PoolCards, Assets, AssetDetail, ServiceDetail, OpieScreen, Inventory, MaintenanceScreen, GitScreen, CaScreen } = window;

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

    // live poll — re-fetch the model from the API on an interval so every metric
    // (CPU, RAM, disk, net, procs, probe state) reflects real data. Falls back to
    // a re-render when the adapter is offline (fixture mode).
    useEffect(() => {
      const iv = setInterval(() => {
        const api = S.api;
        if (api && api.refresh) api.refresh().then(() => force((x) => x + 1)).catch(() => {});
        else force((x) => x + 1);
      }, 10000);
      return () => clearInterval(iv);
    }, []);

    // SSE (§8.4) — near-real-time on top of the poll. Guarded: only when the
    // adapter is live and EventSource exists; on any failure the browser retries
    // and the 10s poll above keeps everything correct regardless.
    useEffect(() => {
      const api = S.api;
      if (S.source !== "live" || !api || !api.stream) return undefined;
      let es = null;
      const bump = () => { api.refresh().then(() => force((x) => x + 1)).catch(() => {}); };
      try {
        es = api.stream((name, data) => {
          if (name === "alertFired") {
            const sev = (data && data.severity) || "warn";
            const what = (data && (data.detail || data.ruleName)) || "Alert fired";
            const where = data && (data.node || data.hostFqdn) ? " — " + (data.node || String(data.hostFqdn).split(".")[0]) : "";
            toast(`${sev.toUpperCase()}: ${what}${where}`, "alerts");
            bump();
          } else if (name === "nodeStateChange") {
            bump();
          }
        });
      } catch (_) { es = null; }
      return () => { if (es && es.close) try { es.close(); } catch (_) {} };
    }, [toast]);

    const scrollTop = () => { const c = document.querySelector(".content"); if (c) c.scrollTop = 0; };
    const openNode = useCallback((nodeOrId) => {
      // Accept a node object OR a nodeId of any type (fixture ids are numeric,
      // wire ids may be strings) — resolve ids against the fleet.
      const node = (nodeOrId && typeof nodeOrId === "object")
        ? nodeOrId
        : ((S.fleet || S.nodes).find((n) => String(n.nodeId) === String(nodeOrId)));
      if (!node) return;
      // Adopted systems (no agent) get the asset detail page, not the metric-heavy
      // agent NodeDetail (which assumes host telemetry and would blank out).
      if (node.isAsset) setRoute({ name: "asset", assetId: node.assetId });
      else setRoute({ name: "node", node });
      scrollTop();
    }, []);
    const openService = useCallback((targetId, backRoute) => {
      setRoute({ name: "service", targetId, backRoute: backRoute || { name: "reachability" } });
      scrollTop();
    }, []);
    // Open Opie's analysis for a report (from the Analysis nav, an alert row, or
    // the command palette). Lands directly on the report's detail view.
    const openOpie = useCallback((reportId) => {
      // nonce forces a fresh mount so re-opening the same report always lands on
      // its detail view (even after an in-panel "back to reports").
      setRoute({ name: "opie", reportId: reportId != null ? reportId : null, nonce: Date.now() });
      scrollTop();
    }, []);

    // Universal entity click-through. Every cross-view reference (Topology glyph,
    // Alerts source, Reachability row, command palette) funnels through here so
    // the SAME entity always lands on the SAME detail view. Accepts either a
    // typed ref object {kind,id} or a bare ref: a node/asset object, an
    // "asset-<id>"/"node-<id>" id string, or a probe targetId ("icmp:…","tcp:…").
    const openEntity = useCallback((ref, opts) => {
      opts = opts || {};
      if (!ref && ref !== 0) return;
      // typed form
      if (typeof ref === "object" && ref.kind) {
        if (ref.kind === "service" || ref.kind === "target") return openService(ref.id, opts.backRoute);
        if (ref.kind === "pool") { setRoute({ name: "assets" }); scrollTop(); return; }
        return openNode(ref.id != null ? ref.id : ref);   // node | asset
      }
      // a probe targetId looks like "<proto>:<host>[:port]" — route to ServiceDetail
      if (typeof ref === "string" && /^(icmp|tcp|udp|https?):/.test(ref)) {
        return openService(ref, opts.backRoute);
      }
      // everything else (node/asset object, "asset-13", numeric id) → openNode,
      // which already forks adopted systems to AssetDetail and agents to NodeDetail.
      return openNode(ref);
    }, [openNode, openService]);
    // Expose globally so any screen (or the command palette) can link an entity
    // without threading a new prop through every layer.
    useEffect(() => { window.__solariOpenEntity = openEntity; }, [openEntity]);

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
        { id: "go-opie", group: "Navigate", label: "Opie · Analysis", icon: "pulse", action: () => go("opie"), sub: `${(S.opie || []).length} report${(S.opie || []).length === 1 ? "" : "s"}` },
        { id: "go-reach", group: "Navigate", label: "Reachability Matrix", icon: "reachability", action: () => go("reachability") },
        { id: "go-topo", group: "Navigate", label: "Topology Map", icon: "topology", action: () => go("topology") },
        { id: "go-systems", group: "Navigate", label: "Systems", icon: "host", action: () => go("assets"), sub: `${(S.assets || []).length} monitored` },
        { id: "go-inventory", group: "Navigate", label: "Inventory", icon: "inventory", action: () => go("inventory"), sub: `${((S.inventory && S.inventory.units) || []).length} components` },
        { id: "go-disc", group: "Navigate", label: "Discovery", icon: "discovery", action: () => go("discovery"), sub: `${S.discovered.length} new` },
        { id: "go-prov", group: "Navigate", label: "Provisioning", icon: "provision", action: () => go("provision"), sub: `${S.enrollments.length} pending` },
        { id: "go-maint", group: "Navigate", label: "Maintenance", icon: "maintenance", action: () => go("maintenance"), sub: `${((S.maintenance || []).filter((w) => w.live || w.status === "active")).length} live` },
        { id: "go-git", group: "Navigate", label: "Git", icon: "git", action: () => go("git"), sub: `${((S.git && S.git.repos) || []).length} repos` },
        { id: "go-ca", group: "Navigate", label: "Certificates", icon: "shield", action: () => go("certificates"), sub: `${((S.ca && S.ca.nodeCerts) || []).length} node certs` },
        { id: "go-cfg", group: "Navigate", label: "Config & Rules", icon: "settings", action: () => go("settings") },
        { id: "view-heat", group: "Actions", label: "Fleet: Heatmap view", icon: "grid", action: () => { setFleetView("heat"); go("fleet"); } },
        { id: "view-table", group: "Actions", label: "Fleet: Table view", icon: "table", action: () => { setFleetView("table"); go("fleet"); } },
        { id: "view-cards", group: "Actions", label: "Fleet: Cards view", icon: "cards", action: () => { setFleetView("cards"); go("fleet"); } },
        { id: "survey-all", group: "Actions", label: "Survey entire fleet now", icon: "survey", action: () => survey(null) },
        { id: "theme", group: "Actions", label: "Toggle dark / light theme", icon: theme === "dark" ? "sun" : "moon", action: () => setTheme((t) => t === "dark" ? "light" : "dark") },
        { id: "logout", group: "Actions", label: "Log out", icon: "enter", action: () => window.solariLogout() },
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
          onNav={go} collapsed={collapsed} onToggle={() => setCollapsed((c) => !c)} hidden={navHidden} summary={S.summary} activeCrit={S.activeCrit} activeWarn={S.activeWarn} />
        <div className="main">
          <TopBar onMenu={toggleNav} onOpenCmd={() => setCmdOpen(true)} theme={theme}
            onToggleTheme={() => setTheme((t) => t === "dark" ? "light" : "dark")}
            server={S.server} onSurvey={() => survey(null)} operator={S.operator} />
          <div className="content">
            {route.name === "fleet" && PoolCards && <PoolCards onOpen={() => go("assets")} />}
            {route.name === "fleet" && <FleetOverview onOpenNode={openNode} onNav={go} view={fleetView} setView={setFleetView} fleet={S.fleet || S.nodes} />}
            {route.name === "assets" && Assets && <Assets onOpenNode={openNode} />}
            {route.name === "inventory" && Inventory && <Inventory toast={toast} />}
            {route.name === "node" && <NodeDetail node={route.node} onBack={() => setRoute({ name: "fleet" })} onSurvey={survey} />}
            {route.name === "asset" && AssetDetail && <AssetDetail assetId={route.assetId} onBack={() => setRoute({ name: "fleet" })} onOpenService={(tid) => openService(tid, { name: "asset", assetId: route.assetId })} toast={toast} />}
            {route.name === "service" && ServiceDetail && <ServiceDetail targetId={route.targetId} onBack={() => setRoute(route.backRoute)} />}
            {route.name === "alerts" && <AlertsScreen onOpenNode={openNode} onOpenEntity={openEntity} onOpenOpie={openOpie} rules={rules} setRules={setRules} toast={toast} />}
            {route.name === "opie" && OpieScreen && <OpieScreen key={route.nonce || 0} initialReportId={route.reportId} onOpenNode={openNode} />}
            {route.name === "reachability" && <Reachability onOpenNode={openNode} onOpenEntity={openEntity} />}
            {route.name === "topology" && <Topology onOpenNode={openNode} onOpenEntity={openEntity} />}
            {route.name === "discovery" && <Discovery onOpenNode={openNode} />}
            {route.name === "provision" && <Provisioning onOpenNode={openNode} />}
            {route.name === "maintenance" && MaintenanceScreen && <MaintenanceScreen onOpenNode={openNode} toast={toast} />}
            {route.name === "git" && GitScreen && <GitScreen />}
            {route.name === "certificates" && CaScreen && <CaScreen />}
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
    // Surface an SSO failure the callback bounced back via ?sso_error=.
    const initialSsoErr = (() => {
      try { return new URLSearchParams(window.location.search).get("sso_error") || ""; }
      catch (_) { return ""; }
    })();
    const [err, setErr] = useState(initialSsoErr);
    // Which sign-in options the server offers. SSO is additive: the local form
    // is always shown; the button only appears when OIDC is enabled + configured.
    const [oidc, setOidc] = useState({ enabled: false, label: "Sign in with SSO", url: "/api/auth/oidc/login" });
    useEffect(() => {
      const api = window.SolariAPI;
      if (!api || !api.authConfig) return;
      api.authConfig()
        .then((cfg) => cfg && cfg.oidcEnabled && setOidc({
          enabled: true,
          label: cfg.oidcLabel || "Sign in with SSO",
          url: cfg.oidcLoginUrl || "/api/auth/oidc/login",
        }))
        .catch(() => {});
    }, []);
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
          {oidc.enabled ? (
            <>
              <div style={{ display: "flex", alignItems: "center", gap: 10, margin: "16px 0 12px", opacity: 0.5, fontSize: 11, textTransform: "uppercase", letterSpacing: 0.5 }}>
                <div style={{ flex: 1, height: 1, background: "rgba(255,255,255,0.12)" }} />
                or
                <div style={{ flex: 1, height: 1, background: "rgba(255,255,255,0.12)" }} />
              </div>
              <button type="button" onClick={() => { window.location.href = oidc.url; }}
                style={{ width: "100%", padding: "10px 12px", borderRadius: 8, cursor: "pointer", fontWeight: 600, fontSize: 14, background: "transparent", color: "inherit", border: "1px solid rgba(255,255,255,0.18)", display: "flex", alignItems: "center", justifyContent: "center", gap: 8 }}>
                <Icon name="shield" size={16} style={{ color: "var(--teal, #35e0d0)" }} />
                {oidc.label}
              </button>
            </>
          ) : null}
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
