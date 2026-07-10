/* ============================================================
   SolariNet — shared components & charts
   ============================================================ */
(function () {
  const { useState, useEffect, useRef } = React;
  const Icon = window.Icon;

  // ---------- status helpers ----------
  const STATE_LABEL = { up: "Up", degraded: "Degraded", down: "Down", unknown: "Unknown", retired: "Retired" };
  function StatusDot({ state, glow = true, size = 9, style }) {
    return <span className={"dot " + (glow ? "glow " : "") + state} style={{ width: size, height: size, ...style }} />;
  }
  function metricColor(pct) {
    if (pct >= 90) return "var(--crit)";
    if (pct >= 75) return "var(--warn)";
    return "var(--teal)";
  }

  // ---------- sparkline ----------
  function Sparkline({ data, color = "var(--teal)", w = 120, h = 28, fill = true, strokeW = 1.6 }) {
    if (!data || !data.length) return null;
    const min = Math.min(...data), max = Math.max(...data);
    const span = max - min || 1;
    const stepX = w / (data.length - 1);
    const pts = data.map((v, i) => [i * stepX, h - 3 - ((v - min) / span) * (h - 6)]);
    const line = pts.map((p, i) => (i ? "L" : "M") + p[0].toFixed(1) + " " + p[1].toFixed(1)).join(" ");
    const area = line + ` L ${w} ${h} L 0 ${h} Z`;
    const gid = "sg" + Math.random().toString(36).slice(2, 8);
    return (
      <svg width="100%" height={h} viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" style={{ display: "block" }}>
        {fill && (
          <>
            <defs>
              <linearGradient id={gid} x1="0" y1="0" x2="0" y2="1">
                <stop offset="0" stopColor={color} stopOpacity="0.35" />
                <stop offset="1" stopColor={color} stopOpacity="0" />
              </linearGradient>
            </defs>
            <path d={area} fill={`url(#${gid})`} />
          </>
        )}
        <path d={line} fill="none" stroke={color} strokeWidth={strokeW} strokeLinejoin="round" strokeLinecap="round" vectorEffect="non-scaling-stroke" />
      </svg>
    );
  }

  // ---------- time-series (detail charts) ----------
  function TimeSeries({ data, color = "var(--teal)", h = 150, unit = "%", max: forceMax }) {
    const w = 600;
    const max = forceMax != null ? forceMax : Math.max(10, Math.ceil(Math.max(...data) / 10) * 10);
    const stepX = w / (data.length - 1);
    const y = (v) => h - 22 - (v / max) * (h - 34);
    const pts = data.map((v, i) => [i * stepX, y(v)]);
    const line = pts.map((p, i) => (i ? "L" : "M") + p[0].toFixed(1) + " " + p[1].toFixed(1)).join(" ");
    const area = line + ` L ${w} ${h - 22} L 0 ${h - 22} Z`;
    const gid = "ts" + Math.random().toString(36).slice(2, 8);
    const grid = [0, 0.5, 1];
    const last = data[data.length - 1];
    return (
      <svg width="100%" height={h} viewBox={`0 0 ${w} ${h}`} preserveAspectRatio="none" style={{ display: "block" }}>
        <defs>
          <linearGradient id={gid} x1="0" y1="0" x2="0" y2="1">
            <stop offset="0" stopColor={color} stopOpacity="0.30" />
            <stop offset="1" stopColor={color} stopOpacity="0" />
          </linearGradient>
        </defs>
        {grid.map((g, i) => (
          <line key={i} x1="0" x2={w} y1={(h - 22) - g * (h - 34)} y2={(h - 22) - g * (h - 34)}
            stroke="var(--line)" strokeWidth="1" vectorEffect="non-scaling-stroke" />
        ))}
        <path d={area} fill={`url(#${gid})`} />
        <path d={line} fill="none" stroke={color} strokeWidth="1.8" strokeLinejoin="round" strokeLinecap="round" vectorEffect="non-scaling-stroke" />
        <circle cx={w} cy={y(last)} r="3" fill={color} />
      </svg>
    );
  }

  // ---------- horizontal bar gauge (bandwidth vs capacity) ----------
  function BandwidthGauge({ label, used, cap, unit, color = "var(--teal)" }) {
    const pct = cap ? Math.min(100, (used / cap) * 100) : 0;
    const c = pct >= 85 ? "var(--crit)" : pct >= 65 ? "var(--warn)" : color;
    return (
      <div>
        <div style={{ display: "flex", justifyContent: "space-between", fontFamily: "var(--mono)", fontSize: 11, marginBottom: 6 }}>
          <span className="muted" style={{ letterSpacing: ".08em", textTransform: "uppercase", fontSize: 10 }}>{label}</span>
          <span style={{ color: c, fontWeight: 600 }}>{unit(used)} <span className="muted">/ {unit(cap)}</span></span>
        </div>
        <div className="metricbar" style={{ width: "100%", height: 9 }}>
          <i style={{ width: pct + "%", background: c, boxShadow: `0 0 calc(8px*var(--glow)) ${c}` }} />
        </div>
      </div>
    );
  }

  // ---------- radial gauge ----------
  function RadialGauge({ value, max = 100, label, sub, size = 116, color }) {
    const pct = Math.min(1, value / max);
    const r = size / 2 - 9;
    const circ = 2 * Math.PI * r;
    const c = color || (pct >= 0.9 ? "var(--crit)" : pct >= 0.75 ? "var(--warn)" : "var(--teal)");
    return (
      <div className="gauge" style={{ width: size }}>
        <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`}>
          <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke="var(--line)" strokeWidth="8" />
          <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke={c} strokeWidth="8" strokeLinecap="round"
            strokeDasharray={circ} strokeDashoffset={circ * (1 - pct)}
            transform={`rotate(-90 ${size / 2} ${size / 2})`}
            style={{ filter: "drop-shadow(0 0 calc(5px*var(--glow)) " + c + ")", transition: "stroke-dashoffset .5s ease" }} />
          <text x="50%" y="48%" textAnchor="middle" dominantBaseline="middle"
            style={{ fontFamily: "var(--mono)", fontSize: 22, fontWeight: 600, fill: "var(--ink)" }}>{Math.round(value)}</text>
          <text x="50%" y="63%" textAnchor="middle" style={{ fontFamily: "var(--mono)", fontSize: 9, fill: "var(--ink-faint)" }}>{sub}</text>
        </svg>
        <div className="lab">{label}</div>
      </div>
    );
  }

  // ---------- donut for segment rollup ----------
  function HealthDonut({ roll, size = 76 }) {
    const r = size / 2 - 7, circ = 2 * Math.PI * r;
    const order = [["up", "var(--ok)"], ["degraded", "var(--warn)"], ["down", "var(--crit)"], ["unknown", "var(--unknown)"]];
    let offset = 0;
    const total = roll.total || 1;
    return (
      <div style={{ position: "relative", width: size, height: size, flex: "0 0 " + size + "px" }}>
        <svg width={size} height={size} viewBox={`0 0 ${size} ${size}`} style={{ transform: "rotate(-90deg)" }}>
          <circle cx={size / 2} cy={size / 2} r={r} fill="none" stroke="var(--line)" strokeWidth="7" />
          {order.map(([k, c]) => {
            const frac = roll[k] / total;
            if (!frac) return null;
            const seg = <circle key={k} cx={size / 2} cy={size / 2} r={r} fill="none" stroke={c} strokeWidth="7"
              strokeDasharray={`${frac * circ} ${circ}`} strokeDashoffset={-offset * circ} />;
            offset += frac;
            return seg;
          })}
        </svg>
        <div style={{ position: "absolute", inset: 0, display: "grid", placeItems: "center" }}>
          <div style={{ textAlign: "center" }}>
            <div style={{ fontFamily: "var(--mono)", fontSize: 18, fontWeight: 700, lineHeight: 1 }}>{roll.total}</div>
            <div style={{ fontFamily: "var(--mono)", fontSize: 8, color: "var(--ink-faint)", letterSpacing: ".1em" }}>NODES</div>
          </div>
        </div>
      </div>
    );
  }

  // ---------- RTT distribution (per vantage mini histogram) ----------
  function RTTBars({ vantages, fmt }) {
    const max = Math.max(1, ...vantages.map((v) => v.rttMicros || 0));
    return (
      <div style={{ display: "flex", alignItems: "flex-end", gap: 6, height: 56 }}>
        {vantages.map((v, i) => {
          const ok = v.outcome === "ok";
          const hpx = ok ? Math.max(4, (v.rttMicros / max) * 48) : 48;
          const c = !ok ? "var(--crit)" : v.lossPermille > 0 ? "var(--warn)" : "var(--teal)";
          return (
            <div key={i} style={{ flex: 1, textAlign: "center" }}>
              <div style={{ height: 48, display: "flex", alignItems: "flex-end" }}>
                <div style={{ width: "100%", height: hpx, background: ok ? c : "var(--crit-bg)", border: ok ? "none" : "1px solid var(--crit)", borderRadius: 3, boxShadow: ok ? `0 0 calc(6px*var(--glow)) ${c}` : "none" }} />
              </div>
              <div style={{ fontFamily: "var(--mono)", fontSize: 8.5, color: "var(--ink-faint)", marginTop: 4, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{v.monitorName}</div>
            </div>
          );
        })}
      </div>
    );
  }

  // ---------- sidebar ----------
  const NAV = [
    { group: "Monitor" },
    { id: "fleet", label: "Fleet Overview", icon: "overview" },
    { id: "reachability", label: "Reachability", icon: "reachability" },
    { id: "topology", label: "Topology Map", icon: "topology" },
    { id: "alerts", label: "Alerts", icon: "alerts", badge: "alerts" },
    { id: "opie", label: "Analysis", icon: "pulse", badge: "opie" },
    { group: "Manage" },
    { id: "assets", label: "Systems", icon: "host" },
    { id: "inventory", label: "Inventory", icon: "inventory" },
    { id: "discovery", label: "Discovery", icon: "discovery", badge: "discovery" },
    { id: "provision", label: "Provisioning", icon: "provision" },
    { id: "maintenance", label: "Maintenance", icon: "maintenance" },
    { id: "git", label: "Git", icon: "git" },
    { id: "certificates", label: "Certificates", icon: "shield" },
    { id: "identity", label: "Identity", icon: "users" },
    { id: "settings", label: "Config & Rules", icon: "settings" },
  ];

  // ---------- shared logout (used by the TopBar profile menu AND the command
  // palette "Log out" action — one code path so they can't drift) ----------
  function solariLogout() {
    const api = window.SolariAPI;
    const done = () => window.location.reload();
    return (api && api.logout ? api.logout() : Promise.resolve()).then(done, done);
  }

  // ---------- operator profile chip (TopBar) ----------
  const ROLE_BADGE = { admin: "violet", operator: "teal", viewer: "muted" };
  function ProfileChip({ operator }) {
    const [open, setOpen] = useState(false);
    useEffect(() => {
      if (!open) return undefined;
      function onKey(e) { if (e.key === "Escape") setOpen(false); }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, [open]);
    if (!operator) return null;
    const dn = operator.displayName || operator.name || "operator";
    const initial = (dn.trim().charAt(0) || "?").toUpperCase();
    const role = operator.role || "viewer";
    const tone = ROLE_BADGE[role] || "muted";
    return (
      <div className="profile">
        <button className={"profile-chip" + (open ? " open" : "")} onClick={() => setOpen((o) => !o)}
          aria-haspopup="menu" aria-expanded={open} title={`Signed in as ${dn}`}>
          <span className={"avatar " + tone}>{initial}</span>
          <span className="profile-chip__name">{dn}</span>
          <span className={"role-badge " + tone}>{role}</span>
        </button>
        {open && (
          <>
            <div className="menu-scrim" onClick={() => setOpen(false)} />
            <div className="profile-menu" role="menu">
              <div className="profile-menu__id">
                <span className={"avatar lg " + tone}>{initial}</span>
                <div style={{ minWidth: 0 }}>
                  <div className="profile-menu__name">{dn}</div>
                  <div className="profile-menu__sub">
                    {operator.name && operator.name !== dn ? <span className="td-mono">{operator.name} · </span> : null}
                    <span className={"role-badge " + tone}>{role}</span>
                  </div>
                </div>
              </div>
              <div className="profile-menu__row">
                <Icon name="shield" size={14} />
                <span>{operator.source === "directory" ? "Directory account" : "Local account"}</span>
              </div>
              <div className="profile-menu__sep" />
              <button className="profile-menu__item" role="menuitem"
                onClick={() => { setOpen(false); solariLogout(); }}>
                <Icon name="enter" size={15} />Log out
              </button>
            </div>
          </>
        )}
      </div>
    );
  }

  function Sidebar({ active, onNav, collapsed, onToggle, hidden, summary, activeCrit, activeWarn }) {
    return (
      <aside className={"sidebar" + (collapsed ? " collapsed" : "") + (hidden ? " hidden" : "")}>
        <div className="brand">
          <div className="brand__mark"><Icon name="topology" size={18} style={{ color: "#05080e" }} /></div>
          <div className="brand__name">solari<b>Net</b></div>
        </div>
        <nav className="nav">
          {NAV.map((item, i) => {
            if (item.group) return <div key={i} className="nav-section">{item.group}</div>;
            const isActive = active === item.id;
            return (
              <div key={item.id} className={"nav-item" + (isActive ? " active" : "") + (item.planned ? " planned" : "")}
                onClick={() => !item.planned && onNav(item.id)}>
                <Icon name={item.icon} size={19} className="ico" />
                <span className="navlabel">{item.label}</span>
                {item.planned && <span className="nav-item__pill">soon</span>}
                {item.badge === "alerts" && (activeCrit + (activeWarn || 0)) > 0 && (
                  <span className="nav-item__count" style={activeCrit > 0 ? undefined : { background: "var(--warn-bg)", color: "var(--warn)" }}>
                    {activeCrit + (activeWarn || 0)}
                  </span>
                )}
                {item.badge === "discovery" && window.SOLARI.discovered.length > 0 && <span className="nav-item__count" style={{ background: "var(--ok-bg)", color: "var(--ok)" }}>{window.SOLARI.discovered.length}</span>}
                {item.badge === "opie" && (() => { const n = (window.SOLARI.opie || []).filter((r) => r.status === "investigating").length; return n > 0 ? <span className="nav-item__count" style={{ background: "rgba(53,224,208,.13)", color: "var(--teal)" }}>{n}</span> : null; })()}
              </div>
            );
          })}
        </nav>
        <div className="sysstat">
          <div className="sysstat__grid">
            <div className="sysstat__cell"><div className="sysstat__v">{summary.systems}</div><div className="sysstat__k">Systems</div></div>
            <div className="sysstat__cell"><div className="sysstat__v">{summary.applications}</div><div className="sysstat__k">Applications</div></div>
            <div className="sysstat__cell"><div className="sysstat__v">{summary.uptimeStr}</div><div className="sysstat__k">Uptime</div></div>
            <div className="sysstat__cell"><div className="sysstat__v">v{summary.version}</div><div className="sysstat__k">Version</div></div>
          </div>
        </div>
        <button className="collapse-btn" onClick={onToggle} aria-label={collapsed ? "Expand sidebar" : "Collapse sidebar"} title="Collapse / expand">
          <Icon name={collapsed ? "chevronRight" : "chevronLeft"} size={18} />
          <span className="navlabel">Collapse</span>
        </button>
      </aside>
    );
  }

  // ---------- top bar ----------
  function TopBar({ onMenu, onOpenCmd, theme, onToggleTheme, server, lastTick, onSurvey, operator }) {
    return (
      <header className="topbar">
        <button className="iconbtn" onClick={onMenu} aria-label="Toggle navigation"><Icon name="menu" size={20} /></button>
        <div className="search" onClick={onOpenCmd}>
          <Icon name="search" size={17} />
          <input placeholder="Search nodes, probes, alerts…  press / or ⌘K" readOnly />
          <span className="kbd">⌘K</span>
        </div>
        <div className="topbar__spacer" />
        <div className="statuschip" title="Active control server holding the failover lease">
          <span className="dot up glow" style={{ width: 8, height: 8 }} />
          <span style={{ fontSize: 9.5, letterSpacing: ".14em", textTransform: "uppercase", color: "var(--ink-faint)" }}>Active&nbsp;C2</span>
          <b>{server.primary}</b>
          <span style={{ color: "var(--ink-faint)" }}>·</span>
          <span style={{ color: "var(--ink-faint)" }}>failover&nbsp;{server.failover}</span>
        </div>
        <button className="iconbtn" onClick={onSurvey} aria-label="Survey now" title="Survey fleet now"><Icon name="survey" size={20} /></button>
        <button className="iconbtn" onClick={onToggleTheme} aria-label="Toggle theme">
          <Icon name={theme === "dark" ? "sun" : "moon"} size={19} />
        </button>
        <ProfileChip operator={operator} />
      </header>
    );
  }

  // ---------- command palette ----------
  function CommandPalette({ open, onClose, commands }) {
    const [q, setQ] = useState("");
    const [sel, setSel] = useState(0);
    const inputRef = useRef(null);
    useEffect(() => { if (open) { setQ(""); setSel(0); setTimeout(() => inputRef.current && inputRef.current.focus(), 30); } }, [open]);
    if (!open) return null;
    const ql = q.toLowerCase();
    const filtered = commands.filter((c) => !ql || (c.label + " " + (c.group || "") + " " + (c.keywords || "")).toLowerCase().includes(ql));
    const groups = {};
    filtered.forEach((c) => { (groups[c.group] = groups[c.group] || []).push(c); });
    const flat = filtered;
    function run(i) { const c = flat[i]; if (c) { c.action(); onClose(); } }
    function onKey(e) {
      if (e.key === "ArrowDown") { e.preventDefault(); setSel((s) => Math.min(flat.length - 1, s + 1)); }
      else if (e.key === "ArrowUp") { e.preventDefault(); setSel((s) => Math.max(0, s - 1)); }
      else if (e.key === "Enter") { e.preventDefault(); run(sel); }
      else if (e.key === "Escape") { onClose(); }
    }
    let runningIndex = -1;
    return (
      <div className="cmdk-overlay" onClick={onClose}>
        <div className="cmdk" onClick={(e) => e.stopPropagation()}>
          <div className="cmdk__input">
            <Icon name="search" size={20} style={{ color: "var(--teal)" }} />
            <input ref={inputRef} value={q} onChange={(e) => { setQ(e.target.value); setSel(0); }} onKeyDown={onKey}
              placeholder="Jump to a node, run a command…" />
            <span className="kbd">esc</span>
          </div>
          <div className="cmdk__list">
            {flat.length === 0 && <div className="empty">No matches</div>}
            {Object.entries(groups).map(([g, items]) => (
              <div key={g}>
                <div className="cmdk__group">{g}</div>
                {items.map((c) => {
                  runningIndex++;
                  const i = runningIndex;
                  return (
                    <div key={c.id} className={"cmdk__item" + (i === sel ? " sel" : "")}
                      onMouseEnter={() => setSel(i)} onClick={() => run(i)}>
                      {c.dot ? <span className={"dot " + c.dot} /> : <Icon name={c.icon || "enter"} size={18} className="ico" />}
                      <span>{c.label}</span>
                      <span className="sub">{c.sub || (i === sel ? "↵" : "")}</span>
                    </div>
                  );
                })}
              </div>
            ))}
          </div>
        </div>
      </div>
    );
  }

  // ---------- danger confirm modal ----------
  // Shared confirm for destructive actions (remove / retire / decommission /
  // delete). Matches the AgentModal .modal-overlay/.modal design. Two grades:
  //   confirmName set  → the operator must TYPE the exact name to arm the button
  //   confirmName null → a plain OK-style confirm (for lesser deletions)
  // onConfirm may return a Promise; the button shows busyVerb until it settles.
  // The CALLER closes the modal on success (and toasts either way).
  function DangerModal({ icon = "close", title, sub, children, verb = "Remove", busyVerb = "Removing…", confirmName, confirmHint, disabled, onConfirm, onClose }) {
    const [typed, setTyped] = useState("");
    const [busy, setBusy] = useState(false);
    useEffect(() => {
      function onKey(e) { if (e.key === "Escape") onClose(); }
      window.addEventListener("keydown", onKey);
      return () => window.removeEventListener("keydown", onKey);
    }, [onClose]);
    const armed = !disabled && !busy && (!confirmName || typed === confirmName);
    function go() {
      if (!armed) return;
      setBusy(true);
      Promise.resolve(onConfirm()).catch(() => {}).then(() => setBusy(false));
    }
    return (
      <div className="modal-overlay" style={{ zIndex: 1200 }} onClick={onClose}>
        <div className="modal" style={{ width: "min(460px, 94vw)", borderColor: "var(--crit)" }} onClick={(e) => e.stopPropagation()}>
          <div className="modal__head">
            <Icon name={icon} size={20} style={{ color: "var(--crit)" }} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ fontWeight: 700, fontSize: 15 }}>{title}</div>
              {sub && <div className="td-mono muted" style={{ fontSize: 11 }}>{sub}</div>}
            </div>
            <button className="iconbtn" style={{ width: 36, height: 36, flex: "0 0 36px" }} onClick={onClose} aria-label="Close"><Icon name="close" size={16} /></button>
          </div>
          <div className="modal__body" style={{ fontSize: 13, lineHeight: 1.55 }}>
            {children}
            {confirmName && (
              <div style={{ marginTop: 14 }}>
                <label style={{ fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, display: "block" }}>
                  {confirmHint || <span>Type <b className="td-mono" style={{ color: "var(--crit)" }}>{confirmName}</b> to confirm</span>}
                </label>
                <input autoFocus value={typed} onChange={(e) => setTyped(e.target.value)}
                  onKeyDown={(e) => { if (e.key === "Enter") go(); }}
                  placeholder={confirmName} spellCheck={false} autoComplete="off"
                  style={{ width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 6, borderRadius: 8, border: "1px solid " + (typed === confirmName ? "var(--crit)" : "var(--line-glow, rgba(255,255,255,0.14))"), background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "var(--mono)", fontSize: 13 }} />
              </div>
            )}
          </div>
          <div className="modal__foot">
            <button className="btn-ghost" onClick={onClose} disabled={busy}>Cancel</button>
            <button className="btn-danger" disabled={!armed} onClick={go}>
              <Icon name="close" size={14} />{busy ? busyVerb : verb}
            </button>
          </div>
        </div>
      </div>
    );
  }

  // ---------- toasts ----------
  function Toasts({ items }) {
    return (
      <div className="toasts">
        {items.map((t) => (
          <div key={t.id} className="toast"><Icon name={t.icon || "check"} size={18} /><span>{t.msg}</span></div>
        ))}
      </div>
    );
  }

  Object.assign(window, {
    StatusDot, Sparkline, TimeSeries, BandwidthGauge, RadialGauge, HealthDonut, RTTBars,
    Sidebar, TopBar, CommandPalette, Toasts, NAV, STATE_LABEL, metricColor,
    ProfileChip, solariLogout, DangerModal,
  });
})();
