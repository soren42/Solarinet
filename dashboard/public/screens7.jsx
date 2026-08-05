/* ============================================================
   SolariNet — screens7: Identity (Keycloak users)

   One READ-ONLY operator screen:
     • IdentityScreen — a friendly view over the `akoria` realm's users:
       enabled/disabled state, email verification, enrolled credential
       factors (Password / Passkey / OTP — Passkey is the passwordless
       goal, so it gets a distinct green, prominent chip), and group
       memberships. Backed by /api/identity (lib/Identity.php).
       Searchable (username/email/group) + filter (enabled/disabled,
       has-passkey). No write/CRUD actions — pure read.

   Same window-global IIFE idiom as screens6/screens5, loaded after
   screens6.jsx. Exports: IdentityScreen.
   ============================================================ */
(function () {
  const { useState, useEffect, useMemo } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;

  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function isOffline() { return !window.SOLARI || window.SOLARI.source === "offline"; }

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
     IDENTITY (Keycloak)
     =================================================================== */
  const STATUS_CHIPS = [
    { k: "all", label: "All" },
    { k: "enabled", label: "Enabled" },
    { k: "disabled", label: "Disabled" },
    { k: "passkey", label: "Passkey-ready" },
  ];

  function FactorChips({ factors }) {
    const f = factors || {};
    return (
      <div style={{ display: "flex", gap: 5, flexWrap: "wrap" }}>
        {f.password && (
          <span className="svc-chip" title="Password credential enrolled">Password</span>
        )}
        {f.otp && (
          <span className="svc-chip" style={{ borderColor: "var(--violet)", color: "var(--violet)" }} title="One-time-password (TOTP) enrolled">OTP</span>
        )}
        {f.webauthnPasswordless && (
          <span className="svc-chip" style={{ borderColor: "var(--ok)", color: "var(--ok)", background: "rgba(46,214,126,0.12)", fontWeight: 700 }} title="Passwordless passkey (WebAuthn) enrolled">
            <Icon name="shield" size={11} style={{ marginRight: 3, verticalAlign: "-1.5px" }} />Passkey
          </span>
        )}
        {f.webauthn && !f.webauthnPasswordless && (
          <span className="svc-chip" style={{ borderColor: "var(--teal)", color: "var(--teal)" }} title="Second-factor WebAuthn security key enrolled">WebAuthn</span>
        )}
        {!f.password && !f.otp && !f.webauthn && !f.webauthnPasswordless && (
          <span className="muted" style={{ fontSize: 11.5 }}>no credentials</span>
        )}
      </div>
    );
  }

  function IdentityScreen() {
    const [identity, setIdentity] = useState(function () { return S.identity || null; });
    const [loading, setLoading] = useState(!S.identity);
    const [q, setQ] = useState("");
    const [statusFilter, setStatusFilter] = useState("all");

    useEffect(function () {
      const a = api();
      if (a && a.identityInfo && !isOffline()) {
        a.identityInfo()
          .then(function (d) { setIdentity(d || null); setLoading(false); })
          .catch(function () { setLoading(false); });
      } else {
        setLoading(false);
      }
    }, []);

    const available = !!(identity && identity.available);
    const users = (identity && identity.users) || [];

    const passwordlessCount = users.filter(function (u) { return u.factors && u.factors.webauthnPasswordless; }).length;
    const disabledCount = users.filter(function (u) { return !u.enabled; }).length;

    const statusCounts = useMemo(function () {
      return {
        all: users.length,
        enabled: users.length - disabledCount,
        disabled: disabledCount,
        passkey: passwordlessCount,
      };
    }, [users, disabledCount, passwordlessCount]);

    const view = useMemo(function () {
      const ql = q.trim().toLowerCase();
      return users.filter(function (u) {
        if (statusFilter === "enabled" && !u.enabled) return false;
        if (statusFilter === "disabled" && u.enabled) return false;
        if (statusFilter === "passkey" && !(u.factors && u.factors.webauthnPasswordless)) return false;
        if (!ql) return true;
        const hay = [u.username, u.email, (u.groups || []).join(" ")].filter(Boolean).join(" ").toLowerCase();
        return hay.indexOf(ql) !== -1;
      });
    }, [users, q, statusFilter]);

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Identity</h1>
            <div className="page-sub">
              {available
                ? <>Keycloak · akoria realm{identity.realmUrl ? " · " + identity.realmUrl : ""}</>
                : "Keycloak · directory users"}
            </div>
          </div>
        </div>

        {loading && <div className="empty">Loading users…</div>}

        {!loading && !available && (
          <div className="panel"><div className="panel__body">
            <div className="empty" style={{ padding: "26px 10px" }}>
              <Icon name="users" size={40} style={{ color: "var(--ink-faint)", marginBottom: 10 }} />
              <div style={{ fontWeight: 600, marginBottom: 4 }}>Identity directory unavailable</div>
              <div className="muted" style={{ fontSize: 12.5 }}>
                {(identity && identity.reason) || "The Keycloak integration is not configured."}
              </div>
            </div>
          </div></div>
        )}

        {!loading && available && (
          <>
            <div className="kpis">
              <div className="kpi teal"><div className="kpi__k">Users</div><div className="kpi__v">{users.length}</div><div className="kpi__sub">akoria realm</div><div className="kpi__bar" /></div>
              <div className="kpi ok"><div className="kpi__k">Passwordless-ready</div><div className="kpi__v">{passwordlessCount}</div><div className="kpi__sub">passkey enrolled</div><div className="kpi__bar" /></div>
              <div className="kpi"><div className="kpi__k">Disabled</div><div className="kpi__v" style={{ color: disabledCount > 0 ? "var(--warn)" : undefined }}>{disabledCount}</div><div className="kpi__sub">accounts</div><div className="kpi__bar" /></div>
            </div>

            <div className="filters">
              {STATUS_CHIPS.map(function (c) {
                return (
                  <button key={c.k} className={"chip" + (statusFilter === c.k ? " on" : "")} onClick={function () { setStatusFilter(c.k); }}>
                    {c.label}<span className="chip__n">{statusCounts[c.k]}</span>
                  </button>
                );
              })}
              <div className="search" style={{ marginLeft: "auto", maxWidth: 280, height: 36 }}>
                <Icon name="search" size={15} />
                <input value={q} onChange={function (e) { setQ(e.target.value); }} placeholder="username · email · group" />
              </div>
            </div>

            {view.length === 0 && <div className="empty">No users match these filters.</div>}
            {view.length > 0 && (
              <div className="tablewrap"><table className="grid">
                <thead><tr>
                  <th>Username</th><th>Email</th><th>Status</th><th>Credentials</th><th>Groups</th><th>Created</th>
                </tr></thead>
                <tbody>
                  {view.map(function (u) {
                    return (
                      <tr key={u.id}>
                        <td style={{ fontWeight: 600 }}>
                          <div style={{ display: "flex", alignItems: "center", gap: 7 }}>
                            <Icon name="users" size={14} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                            {u.username}
                          </div>
                        </td>
                        <td className="td-mono muted" style={{ fontSize: 12 }}>
                          {u.email || "—"}
                          {u.email && !u.emailVerified && (
                            <span className="tag" style={{ marginLeft: 6, borderColor: "var(--warn)", color: "var(--warn)" }} title="Email not verified">unverified</span>
                          )}
                        </td>
                        <td>
                          {u.enabled
                            ? <Badge tone="var(--ok)">enabled</Badge>
                            : <Badge tone="var(--crit)">disabled</Badge>}
                        </td>
                        <td><FactorChips factors={u.factors} /></td>
                        <td>
                          {(u.groups || []).length === 0 && <span className="muted">—</span>}
                          <div style={{ display: "flex", gap: 5, flexWrap: "wrap" }}>
                            {(u.groups || []).map(function (g, i) {
                              return <span key={i} className="tag">{g}</span>;
                            })}
                          </div>
                        </td>
                        <td className="td-mono muted" style={{ fontSize: 11 }} title={u.createdAt || ""}>{dateLabel(u.createdAt)}</td>
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

  Object.assign(window, { IdentityScreen });
})();
