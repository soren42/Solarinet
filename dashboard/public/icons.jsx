/* ============================================================
   SolariNet — icon set (solid geometric glyphs, currentColor)
   <Icon name="server" size={18} /> ; exported to window.
   ============================================================ */
(function () {
  const P = {
    // nav / structure
    overview: <><rect x="3" y="3" width="8" height="8" rx="1.5"/><rect x="13" y="3" width="8" height="5" rx="1.5"/><rect x="13" y="10" width="8" height="11" rx="1.5"/><rect x="3" y="13" width="8" height="8" rx="1.5"/></>,
    server: <><rect x="3" y="3" width="18" height="7" rx="2"/><rect x="3" y="14" width="18" height="7" rx="2"/><circle cx="7" cy="6.5" r="1.2" fill="#05080e"/><circle cx="7" cy="17.5" r="1.2" fill="#05080e"/></>,
    host: <><rect x="3" y="4" width="18" height="12" rx="2"/><rect x="8" y="18" width="8" height="2.4" rx="1.2"/><rect x="6" y="19.6" width="12" height="1.8" rx="0.9"/></>,
    monitor: <><circle cx="12" cy="12" r="2.6"/><path d="M12 2a10 10 0 0 1 10 10h-3a7 7 0 0 0-7-7z" opacity="0.85"/><path d="M12 22A10 10 0 0 1 2 12h3a7 7 0 0 0 7 7z" opacity="0.55"/></>,
    reachability: <><circle cx="5" cy="6" r="2.4"/><circle cx="5" cy="18" r="2.4"/><circle cx="19" cy="12" r="2.4"/><path d="M7 7l10 4M7 17l10-4" stroke="currentColor" strokeWidth="1.7" fill="none"/></>,
    topology: <><circle cx="12" cy="5" r="2.4"/><circle cx="5" cy="18" r="2.4"/><circle cx="19" cy="18" r="2.4"/><path d="M12 7v4m0 0l-6 6m6-6l6 6" stroke="currentColor" strokeWidth="1.7" fill="none"/></>,
    alerts: <><path d="M12 3l9 16H3z"/><rect x="11" y="9" width="2" height="5" rx="1" fill="#05080e"/><circle cx="12" cy="16.5" r="1.1" fill="#05080e"/></>,
    discovery: <><circle cx="11" cy="11" r="6.5" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M16 16l5 5" stroke="currentColor" strokeWidth="2.4" strokeLinecap="round"/><circle cx="11" cy="11" r="2"/></>,
    provision: <><rect x="4" y="4" width="16" height="16" rx="3" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M12 8v8M8 12h8" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round"/></>,
    settings: <><circle cx="12" cy="12" r="3"/><path d="M12 2l1.5 3 3.3-.6L17 7.7 20 9l-1.3 3L20 15l-2.2 2.3.2 3.3L15 21l-3 1-3-1-2.9.6.2-3.3L4 15l1.3-3L4 9l3-1.3-.2-3.3L10 5z" opacity="0.35"/></>,
    // metrics
    cpu: <><rect x="6" y="6" width="12" height="12" rx="2"/><rect x="9" y="9" width="6" height="6" rx="1" fill="#05080e"/><path d="M9 2v3M15 2v3M9 19v3M15 19v3M2 9h3M2 15h3M19 9h3M19 15h3" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"/></>,
    ram: <><rect x="2" y="7" width="20" height="10" rx="2"/><rect x="5" y="10" width="2.5" height="7" fill="#05080e"/><rect x="9" y="10" width="2.5" height="7" fill="#05080e"/><rect x="13" y="10" width="2.5" height="7" fill="#05080e"/><rect x="17" y="10" width="2.5" height="7" fill="#05080e"/></>,
    disk: <><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="2.4" fill="#05080e"/><path d="M12 3v4" stroke="#05080e" strokeWidth="1.6"/></>,
    network: <><rect x="9" y="2" width="6" height="5" rx="1.4"/><rect x="2" y="17" width="6" height="5" rx="1.4"/><rect x="16" y="17" width="6" height="5" rx="1.4"/><path d="M12 7v4m0 0H5v6m7-6h7v6" stroke="currentColor" strokeWidth="1.7" fill="none"/></>,
    bandwidth: <><path d="M3 17a9 9 0 0 1 18 0" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M12 17l5-4" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round"/><circle cx="12" cy="17" r="1.8"/></>,
    activity: <><path d="M2 12h4l3 8 4-16 3 8h6" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round"/></>,
    process: <><rect x="3" y="3" width="18" height="18" rx="3" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M7 9l3 3-3 3M12 15h5" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/></>,
    // ui
    search: <><circle cx="11" cy="11" r="6.5" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M16 16l5 5" stroke="currentColor" strokeWidth="2.4" strokeLinecap="round"/></>,
    command: <><path d="M9 3a3 3 0 1 1-3 3v12a3 3 0 1 1 3-3h6a3 3 0 1 1-3 3V6a3 3 0 1 1 3 3H9" fill="none" stroke="currentColor" strokeWidth="2"/></>,
    grid: <><rect x="3" y="3" width="7" height="7" rx="1.5"/><rect x="14" y="3" width="7" height="7" rx="1.5"/><rect x="3" y="14" width="7" height="7" rx="1.5"/><rect x="14" y="14" width="7" height="7" rx="1.5"/></>,
    table: <><rect x="3" y="4" width="18" height="16" rx="2" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M3 9h18M3 14h18M9 9v11" stroke="currentColor" strokeWidth="1.7"/></>,
    cards: <><rect x="3" y="4" width="18" height="7" rx="2"/><rect x="3" y="13" width="18" height="7" rx="2" opacity="0.5"/></>,
    bell: <><path d="M6 16V11a6 6 0 0 1 12 0v5l2 2H4z"/><path d="M10 20a2 2 0 0 0 4 0" fill="none" stroke="currentColor" strokeWidth="1.8"/></>,
    sun: <><circle cx="12" cy="12" r="4.5"/><path d="M12 1v3M12 20v3M1 12h3M20 12h3M4.2 4.2l2.1 2.1M17.7 17.7l2.1 2.1M19.8 4.2l-2.1 2.1M6.3 17.7l-2.1 2.1" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/></>,
    moon: <><path d="M20 14.5A8 8 0 1 1 9.5 4a6.5 6.5 0 0 0 10.5 10.5z"/></>,
    menu: <><path d="M4 6h16M4 12h16M4 18h16" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round"/></>,
    close: <><path d="M6 6l12 12M18 6L6 18" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round"/></>,
    chevronLeft: <><path d="M15 5l-7 7 7 7" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round"/></>,
    chevronRight: <><path d="M9 5l7 7-7 7" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round"/></>,
    check: <><path d="M4 12l5 5L20 6" fill="none" stroke="currentColor" strokeWidth="2.4" strokeLinecap="round" strokeLinejoin="round"/></>,
    refresh: <><path d="M20 11a8 8 0 1 0-1 5" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round"/><path d="M20 4v6h-6" fill="none" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round" strokeLinejoin="round"/></>,
    survey: <><circle cx="12" cy="12" r="2.2"/><path d="M12 12L4 6M12 12l8-6" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round"/><path d="M5 19a10 10 0 0 1 14 0" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/></>,
    filter: <><path d="M3 5h18l-7 8v6l-4 2v-8z"/></>,
    shield: <><path d="M12 2l8 3v6c0 5-3.5 8.5-8 11-4.5-2.5-8-6-8-11V5z"/><path d="M8.5 12l2.5 2.5 4.5-5" fill="none" stroke="#05080e" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"/></>,
    pulse: <><path d="M2 12h5l2-5 4 12 2-7h7" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/></>,
    clock: <><circle cx="12" cy="12" r="9" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M12 7v5l3.5 2" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round" strokeLinejoin="round"/></>,
    // wrench — Maintenance (scheduled downtime / suppressed alerting)
    maintenance: <><path d="M20.6 6.1a4.3 4.3 0 0 1-5.6 5.4L6.6 19.9a2 2 0 0 1-2.8-2.8l8.4-8.4A4.3 4.3 0 0 1 17.6 3l-2.9 2.9.7 2.3 2.3.7z" fill="currentColor"/></>,
    calendar: <><rect x="3" y="4.5" width="18" height="16.5" rx="2.5" fill="none" stroke="currentColor" strokeWidth="2"/><path d="M3 9.5h18M8 2.5v4M16 2.5v4" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/></>,
    arch: <><rect x="4" y="3" width="16" height="4" rx="1.4"/><rect x="4" y="10" width="16" height="4" rx="1.4" opacity="0.7"/><rect x="4" y="17" width="16" height="4" rx="1.4" opacity="0.45"/></>,
    chip: <><rect x="6" y="6" width="12" height="12" rx="2"/></>,
    link: <><path d="M9 15l6-6" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/><path d="M8 12l-2 2a3 3 0 0 0 4 4l2-2M16 12l2-2a3 3 0 0 0-4-4l-2 2" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/></>,
    plus: <><path d="M12 5v14M5 12h14" stroke="currentColor" strokeWidth="2.4" strokeLinecap="round"/></>,
    enter: <><path d="M9 10l-4 4 4 4" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"/><path d="M5 14h10a4 4 0 0 0 4-4V6" fill="none" stroke="currentColor" strokeWidth="1.8" strokeLinecap="round" strokeLinejoin="round"/></>,
    gateway: <><rect x="3" y="9" width="18" height="9" rx="2"/><circle cx="7" cy="13.5" r="1.3" fill="#05080e"/><rect x="11" y="12.5" width="7" height="2" rx="1" fill="#05080e"/><path d="M8 9V6a4 4 0 0 1 8 0v3" fill="none" stroke="currentColor" strokeWidth="1.8"/></>,
    netswitch: <><rect x="2" y="8" width="20" height="8" rx="1.6"/><rect x="5" y="11" width="2" height="2.5" fill="#05080e"/><rect x="8.5" y="11" width="2" height="2.5" fill="#05080e"/><rect x="12" y="11" width="2" height="2.5" fill="#05080e"/><rect x="15.5" y="11" width="2" height="2.5" fill="#05080e"/></>,
    wifi: <><path d="M5 12.5a10 10 0 0 1 14 0" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/><path d="M8 15.5a6 6 0 0 1 8 0" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round"/><circle cx="12" cy="18.5" r="1.6"/></>,
    close2: <><path d="M6 6l12 12M18 6L6 18" stroke="currentColor" strokeWidth="2.2" strokeLinecap="round"/></>,
  };

  function Icon({ name, size = 18, className, style }) {
    const body = P[name];
    if (!body) return null;
    return (
      <svg className={className} style={style} width={size} height={size} viewBox="0 0 24 24"
        fill="currentColor" aria-hidden="true" focusable="false">
        {body}
      </svg>
    );
  }

  window.Icon = Icon;
})();
