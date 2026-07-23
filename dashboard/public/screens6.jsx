/* ============================================================
   SolariNet — screens6: Git (Forgejo) + Certificates (CA / PKI)

   Two READ-ONLY operator screens:
     • GitScreen — the Akoria Forgejo instance: repositories, recent
       commits across repos, and open pull requests. Backed by
       /api/forgejo (lib/Forgejo.php), configured from FORGEJO_URL /
       FORGEJO_TOKEN. Fail-soft: an unavailable payload renders an
       explanatory empty state, never a crash.
     • CaScreen — the Akoria PKI: step-ca health, root + mTLS CA
       certificate expiry (green/amber/red pill), the provisioner set,
       and the per-node mTLS cert inventory. Backed by /api/ca (lib/Ca.php).

   Same window-global IIFE idiom as screens4/5; loaded after screens5.jsx.
   Exports: GitScreen, CaScreen.
   ============================================================ */
(function () {
  const { useState, useEffect } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;

  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function isOffline() { return !window.SOLARI || window.SOLARI.source === "offline"; }

  // ---- ISO-8601 (UTC "…Z") → relative label -------------------------
  function agoLabel(iso) {
    if (!iso) return "—";
    const t = Date.parse(iso);
    if (isNaN(t)) return "—";
    const min = Math.max(0, Math.round((Date.now() - t) / 60000));
    if (min < 1) return "just now";
    if (min < 60) return min + "m ago";
    if (min < 1440) return Math.floor(min / 60) + "h ago";
    if (min < 43200) return Math.floor(min / 1440) + "d ago";
    return Math.floor(min / 43200) + "mo ago";
  }
  function dateLabel(iso) {
    if (!iso) return "—";
    const d = new Date(iso);
    if (isNaN(d.getTime())) return "—";
    return d.toLocaleDateString(undefined, { year: "numeric", month: "short", day: "numeric" });
  }

  function Badge({ tone, children, title }) {
    return <span className="inv-badge" title={title} style={{ color: tone, borderColor: tone }}>{children}</span>;
  }

  /* ===================================================================
     GIT (Forgejo)
     =================================================================== */
  function GitScreen() {
    const [git, setGit] = useState(function () { return S.git || null; });
    const [loading, setLoading] = useState(!S.git);

    useEffect(function () {
      const a = api();
      if (a && a.forgejoInfo && !isOffline()) {
        a.forgejoInfo()
          .then(function (d) { setGit(d || null); setLoading(false); })
          .catch(function () { setLoading(false); });
      } else {
        setLoading(false);
      }
    }, []);

    const available = !!(git && git.available);
    const repos = (git && git.repos) || [];
    const commits = (git && git.commits) || [];
    const prs = (git && git.prs) || [];
    const counts = (git && git.counts) || { repos: repos.length, openIssues: 0, openPrs: prs.length };

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Git</h1>
            <div className="page-sub">
              {available
                ? <>{git.baseUrl}{git.version && git.version !== "—" ? " · Forgejo " + git.version : ""}</>
                : "Forgejo · source control"}
            </div>
          </div>
        </div>

        {loading && <div className="empty">Loading repositories…</div>}

        {!loading && !available && (
          <div className="panel"><div className="panel__body">
            <div className="empty" style={{ padding: "26px 10px" }}>
              <Icon name="git" size={40} style={{ color: "var(--ink-faint)", marginBottom: 10 }} />
              <div style={{ fontWeight: 600, marginBottom: 4 }}>Forgejo unavailable</div>
              <div className="muted" style={{ fontSize: 12.5 }}>
                {(git && git.reason) || "The Git integration is not configured."}
                {git && git.baseUrl ? <div className="td-mono" style={{ marginTop: 6, fontSize: 11 }}>{git.baseUrl}</div> : null}
              </div>
            </div>
          </div></div>
        )}

        {!loading && available && (
          <>
            <div className="kpis">
              <div className="kpi teal"><div className="kpi__k">Repositories</div><div className="kpi__v">{counts.repos}</div><div className="kpi__sub">tracked</div><div className="kpi__bar" /></div>
              <div className="kpi violet"><div className="kpi__k">Open PRs</div><div className="kpi__v">{counts.openPrs}</div><div className="kpi__sub">awaiting review</div><div className="kpi__bar" /></div>
              <div className="kpi ok"><div className="kpi__k">Open issues</div><div className="kpi__v">{counts.openIssues}</div><div className="kpi__sub">across repos</div><div className="kpi__bar" /></div>
              <div className="kpi"><div className="kpi__k">Server</div><div className="kpi__v" style={{ fontSize: 20 }}>{git.version || "—"}</div><div className="kpi__sub">Forgejo</div><div className="kpi__bar" /></div>
            </div>

            <div className="filters">
              <div className="page-sub" style={{ margin: 0 }}>Repositories</div>
            </div>
            {repos.length === 0 && <div className="empty">No repositories.</div>}
            <div className="cards">
              {repos.map(function (r) {
                return (
                  <a key={r.fullName || r.name} className="inv-receipt" style={{ textDecoration: "none", color: "inherit", cursor: r.htmlUrl ? "pointer" : "default" }}
                    href={r.htmlUrl || undefined} target={r.htmlUrl ? "_blank" : undefined} rel="noopener noreferrer">
                    <div className="inv-receipt__body" style={{ padding: 14 }}>
                      <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                        <Icon name="git" size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                        <div style={{ fontWeight: 700, fontSize: 14, flex: 1, minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={r.fullName}>{r.name}</div>
                        {r.private && <Badge tone="var(--warn)">private</Badge>}
                        {r.fork && <Badge tone="var(--ink-faint)">fork</Badge>}
                      </div>
                      <div className="muted" style={{ fontSize: 12, marginTop: 6, minHeight: 17, overflow: "hidden", textOverflow: "ellipsis", display: "-webkit-box", WebkitLineClamp: 2, WebkitBoxOrient: "vertical" }}>
                        {r.description || "No description"}
                      </div>
                      <div className="td-mono muted" style={{ fontSize: 11, marginTop: 10, display: "flex", gap: 12, flexWrap: "wrap" }}>
                        <span title="stars">★ {r.stars || 0}</span>
                        <span title="open issues" style={r.openIssues > 0 ? { color: "var(--ok)" } : undefined}>◦ {r.openIssues || 0} issues</span>
                        {r.defaultBranch && <span title="default branch">⑂ {r.defaultBranch}</span>}
                        <span style={{ flex: 1 }} />
                        <span title={r.updatedAt || ""}>{agoLabel(r.updatedAt)}</span>
                      </div>
                    </div>
                  </a>
                );
              })}
            </div>

            <div style={{ display: "flex", gap: 18, marginTop: 22, flexWrap: "wrap" }}>
              <div className="panel" style={{ flex: "1 1 380px", minWidth: 0 }}>
                <div className="panel__head"><Icon name="git" size={16} /><h3>Recent commits</h3><span className="chip__n">{commits.length}</span></div>
                <div className="panel__body" style={{ padding: "6px 6px" }}>
                  {commits.length === 0 && <div className="empty">No recent commits.</div>}
                  {commits.map(function (c, i) {
                    return (
                      <div key={i} className="inv-tree-row" style={{ alignItems: "flex-start", paddingLeft: 10 }}>
                        <span className="td-mono" style={{ color: "var(--teal)", flex: "0 0 auto", fontSize: 11, marginTop: 2 }}>{c.sha || "—"}</span>
                        <div style={{ minWidth: 0, flex: 1 }}>
                          <div style={{ fontWeight: 600, fontSize: 12.5, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={c.message}>{c.message || "(no message)"}</div>
                          <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 2 }}>{c.repo} · {c.author} · {agoLabel(c.date)}</div>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>

              <div className="panel" style={{ flex: "1 1 380px", minWidth: 0 }}>
                <div className="panel__head"><Icon name="pulse" size={16} /><h3>Open pull requests</h3><span className="chip__n">{prs.length}</span></div>
                <div className="panel__body" style={{ padding: "6px 6px" }}>
                  {prs.length === 0 && <div className="empty">No open pull requests.</div>}
                  {prs.map(function (p, i) {
                    return (
                      <div key={i} className="inv-tree-row" style={{ alignItems: "flex-start", paddingLeft: 10 }}>
                        <Badge tone="var(--violet)">#{p.number}</Badge>
                        <div style={{ minWidth: 0, flex: 1 }}>
                          <div style={{ fontWeight: 600, fontSize: 12.5, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={p.title}>{p.title}</div>
                          <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 2 }}>{p.repo} · {p.author} · {agoLabel(p.createdAt)}</div>
                        </div>
                      </div>
                    );
                  })}
                </div>
              </div>
            </div>
          </>
        )}
      </div>
    );
  }

  /* ===================================================================
     CERTIFICATES (CA / PKI)
     =================================================================== */
  const CERT_TONE = { ok: "var(--ok)", expiring: "var(--warn)", expired: "var(--crit)", unknown: "var(--ink-faint)" };
  const CERT_LABEL = { ok: "valid", expiring: "expiring", expired: "expired", unknown: "unknown" };
  const CA_SOURCE_LABEL = { "step-ca": "step-ca", "solarinet-mtls": "SolariNet mTLS" };
  const ROLE_TONE = { server: "var(--violet)", monitor: "var(--teal)", client: "var(--ok)" };

  function ExpiryPill({ severity, daysLeft }) {
    const tone = CERT_TONE[severity] || CERT_TONE.unknown;
    let text;
    if (severity === "expired") text = "expired";
    else if (daysLeft == null) text = "unknown";
    else text = daysLeft + "d left";
    return (
      <span className="inv-badge" style={{ color: tone, borderColor: tone }} title={CERT_LABEL[severity] || "unknown"}>
        <span className="dot" style={{ background: tone, width: 7, height: 7, marginRight: 5 }} />{text}
      </span>
    );
  }

  function CaScreen() {
    const [ca, setCa] = useState(function () { return S.ca || null; });
    const [loading, setLoading] = useState(!S.ca);

    useEffect(function () {
      const a = api();
      if (a && a.caInfo && !isOffline()) {
        a.caInfo()
          .then(function (d) { setCa(d || null); setLoading(false); })
          .catch(function () { setLoading(false); });
      } else {
        setLoading(false);
      }
    }, []);

    const stepCa = (ca && ca.stepCa) || null;
    const roots = (ca && ca.roots) || [];
    const provisioners = (ca && ca.provisioners) || [];
    const nodeCerts = (ca && ca.nodeCerts) || [];
    const caUp = !!(stepCa && stepCa.available && (stepCa.status === "ok" || stepCa.status === "unknown"));
    const flagged = roots.filter(function (r) { return r.severity === "expiring" || r.severity === "expired"; }).length;

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Certificates</h1>
            <div className="page-sub">
              Akoria PKI{stepCa && stepCa.url ? " · " + stepCa.url : ""}
              {flagged > 0 ? <span style={{ color: "var(--warn)" }}> · {flagged} expiring soon</span> : null}
            </div>
          </div>
        </div>

        {loading && <div className="empty">Loading PKI…</div>}

        {!loading && (
          <>
            <div className="kpis">
              <div className={"kpi " + (caUp ? "ok" : "")}>
                <div className="kpi__k">step-ca</div>
                <div className="kpi__v" style={{ fontSize: 20, color: caUp ? "var(--ok)" : "var(--crit)" }}>{caUp ? "healthy" : "unreachable"}</div>
                <div className="kpi__sub">{stepCa && stepCa.status ? stepCa.status : (stepCa && stepCa.reason) || "—"}</div><div className="kpi__bar" />
              </div>
              <div className="kpi teal"><div className="kpi__k">CA certificates</div><div className="kpi__v">{roots.length}</div><div className="kpi__sub">roots + mTLS</div><div className="kpi__bar" /></div>
              <div className="kpi violet"><div className="kpi__k">Provisioners</div><div className="kpi__v">{provisioners.length}</div><div className="kpi__sub">step-ca</div><div className="kpi__bar" /></div>
              <div className="kpi"><div className="kpi__k">Node certs</div><div className="kpi__v">{nodeCerts.length}</div><div className="kpi__sub">fleet mTLS</div><div className="kpi__bar" /></div>
            </div>

            {/* CA / root certificates */}
            <div className="filters"><div className="page-sub" style={{ margin: 0 }}>CA certificates</div></div>
            {roots.length === 0 && <div className="empty">No CA certificates could be read.</div>}
            <div className="cards">
              {roots.map(function (r, i) {
                return (
                  <div key={i} className="inv-receipt" style={{ cursor: "default" }}>
                    <div className="inv-receipt__body" style={{ padding: 14 }}>
                      <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
                        <Icon name="shield" size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                        <div style={{ fontWeight: 700, fontSize: 13.5, flex: 1, minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={r.subject || r.name}>{r.subject || r.name}</div>
                        <ExpiryPill severity={r.severity} daysLeft={r.daysLeft} />
                      </div>
                      <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 8, display: "flex", gap: 8, flexWrap: "wrap" }}>
                        <span className="tag">{CA_SOURCE_LABEL[r.source] || r.source}</span>
                        <span className="tag">{r.kind}</span>
                        {r.selfSigned && <span className="tag">self-signed</span>}
                      </div>
                      {r.issuer && r.issuer !== r.subject && <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 6 }}>issuer: {r.issuer}</div>}
                      <div className="td-mono muted" style={{ fontSize: 10.5, marginTop: 6 }}>expires {dateLabel(r.notAfter)}</div>
                    </div>
                  </div>
                );
              })}
            </div>

            {/* provisioners */}
            <div className="filters" style={{ marginTop: 22 }}><div className="page-sub" style={{ margin: 0 }}>Provisioners</div></div>
            <div className="filters" style={{ gap: 8 }}>
              {provisioners.length === 0 && <div className="muted" style={{ fontSize: 12 }}>None.</div>}
              {provisioners.map(function (p, i) {
                return (
                  <span key={i} className="chip" style={{ cursor: "default" }}>
                    <Icon name="shield" size={13} style={{ color: "var(--teal)", marginRight: 5 }} />
                    {p.name}<span className="tag" style={{ marginLeft: 6 }}>{p.type}</span>
                  </span>
                );
              })}
            </div>

            {/* per-node mTLS cert inventory */}
            <div className="filters" style={{ marginTop: 22 }}><div className="page-sub" style={{ margin: 0 }}>Fleet mTLS certificates</div></div>
            {nodeCerts.length === 0 && <div className="empty">No node certificates on record.</div>}
            {nodeCerts.length > 0 && (
              <div className="tablewrap"><table className="grid">
                <thead><tr>
                  <th>Host</th><th>Cert CN</th><th>Role</th><th>Arch</th><th>Enrolled</th><th>Last seen</th>
                </tr></thead>
                <tbody>
                  {nodeCerts.map(function (n, i) {
                    return (
                      <tr key={i}>
                        <td style={{ fontWeight: 600 }}>{n.hostFqdn}</td>
                        <td className="td-mono muted">{n.certCn || "—"}</td>
                        <td>{n.role ? <Badge tone={ROLE_TONE[n.role] || "var(--ink-faint)"}>{n.role}</Badge> : <span className="muted">—</span>}</td>
                        <td className="td-mono muted">{n.arch || "—"}</td>
                        <td className="td-mono muted" title={n.enrolledAt || ""}>{dateLabel(n.enrolledAt)}</td>
                        <td className="td-mono muted" title={n.lastSeenAt || ""}>{agoLabel(n.lastSeenAt)}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table></div>
            )}
          </>
        )}
      </div>
    );
  }

  Object.assign(window, { GitScreen, CaScreen });
})();
