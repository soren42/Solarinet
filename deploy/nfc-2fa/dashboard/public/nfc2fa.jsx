/*
 * nfc2fa.jsx — SolariNet NFC 2FA front-end (SCAFFOLD).
 *
 * Two components exposed on window.SolariNfc:
 *   <TapPrompt challenge expiresIn onDone />  — the login second step.
 *   <EnrollCard onDone />                     — admin binds a card to a user.
 *
 * Both talk to the local reader daemon (deploy/nfc-2fa/reader/nfc-reader.py) at
 * http://127.0.0.1:8770 and to the dashboard API. Config-gated server-side: when
 * nfc2fa is off these are never rendered (login never returns stage:"nfc_required").
 *
 * No build step (matches app.jsx: React/ReactDOM as globals, Babel in-browser).
 * This is a scaffold — styling is minimal and intentionally mirrors LoginScreen.
 */
(function () {
  "use strict";
  const { useState, useEffect, useCallback } = React;

  const READER_BASE = "http://127.0.0.1:8770";

  // Read one card from the local daemon. Resolves {uid,type,...} or throws.
  function readCard(params) {
    const qs = new URLSearchParams(Object.assign({ timeout: "20" }, params || {}));
    return fetch(READER_BASE + "/read?" + qs.toString(), { method: "GET" })
      .then((r) => {
        if (r.status === 204) throw new Error("No card detected — tap and hold it on the reader.");
        if (r.status === 503) throw new Error("No NFC reader found on this machine.");
        return r.json();
      })
      .then((j) => {
        if (!j || !j.ok || !j.uid) throw new Error("Card read failed.");
        return j;
      })
      .catch((e) => {
        // A network error here usually means the reader daemon isn't running.
        if (e instanceof TypeError) throw new Error("Reader daemon unreachable on 127.0.0.1:8770.");
        throw e;
      });
  }

  const card = {
    width: 340, padding: 28, borderRadius: 14,
    background: "rgba(255,255,255,0.04)", border: "1px solid rgba(255,255,255,0.08)",
    boxShadow: "0 12px 48px rgba(0,0,0,0.45)",
  };
  const btn = (disabled) => ({
    width: "100%", padding: "10px 12px", borderRadius: 8, border: "none",
    cursor: disabled ? "default" : "pointer", fontWeight: 600, fontSize: 14,
    background: "var(--teal, #35e0d0)", color: "#05080e", opacity: disabled ? 0.6 : 1,
  });
  const field = {
    width: "100%", boxSizing: "border-box", padding: "10px 12px", marginBottom: 10,
    borderRadius: 8, border: "1px solid rgba(255,255,255,0.12)",
    background: "rgba(255,255,255,0.04)", color: "inherit", fontSize: 14, fontFamily: "inherit",
  };

  // ------------------------------------------------------------- TapPrompt
  // Shown after primary auth succeeds and the server returned stage:"nfc_required".
  function TapPrompt(props) {
    const challenge = props.challenge;
    const [status, setStatus] = useState("idle"); // idle | reading | verifying | done
    const [err, setErr] = useState("");

    const go = useCallback(() => {
      setErr(""); setStatus("reading");
      readCard({ challenge: challenge })
        .then((c) => {
          setStatus("verifying");
          return fetch("/api/auth/nfc/verify", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            credentials: "same-origin",
            body: JSON.stringify({ challenge: challenge, uid: c.uid, crypto: c.crypto || null }),
          }).then((r) => r.json());
        })
        .then((j) => {
          if (j && j.ok && j.data && j.data.stage === "authenticated") {
            setStatus("done");
            if (props.onDone) props.onDone(j.data); else window.location.reload();
          } else {
            setStatus("idle");
            setErr((j && j.error && j.error.message) || "Card not recognised.");
          }
        })
        .catch((e) => { setStatus("idle"); setErr(e.message || "Card read failed."); });
    }, [challenge]);

    // Auto-start one read attempt when mounted.
    useEffect(() => { go(); }, [go]);

    const busy = status === "reading" || status === "verifying";
    return (
      <div style={{ minHeight: "100vh", display: "flex", alignItems: "center", justifyContent: "center", padding: 20 }}>
        <div style={card}>
          <div style={{ textAlign: "center", marginBottom: 18 }}>
            <div style={{ fontSize: 40 }}>{status === "verifying" ? "🔐" : "💳"}</div>
            <div style={{ fontWeight: 700, fontSize: 18, marginTop: 6 }}>Tap your card</div>
            <div style={{ fontSize: 12, opacity: 0.6, marginTop: 2 }}>
              {status === "reading" ? "Waiting for card…"
                : status === "verifying" ? "Verifying…"
                : "Second factor required"}
            </div>
          </div>
          {err ? <div style={{ color: "var(--red, #ff3d72)", fontSize: 12, margin: "2px 0 12px", textAlign: "center" }}>{err}</div> : null}
          <button type="button" disabled={busy} style={btn(busy)} onClick={go}>
            {busy ? "Reading…" : "Tap again / retry"}
          </button>
          <div style={{ fontSize: 11, opacity: 0.5, marginTop: 12, textAlign: "center" }}>
            Reader daemon must be running on this machine (127.0.0.1:8770).
          </div>
        </div>
      </div>
    );
  }

  // ------------------------------------------------------------ EnrollCard
  // Admin UI: pick a user, tap the NEW card, bind it. Operator-role gated server-side.
  function EnrollCard(props) {
    const [username, setUsername] = useState(props.username || "");
    const [label, setLabel] = useState("");
    const [status, setStatus] = useState("idle"); // idle | tapping | saving | done
    const [err, setErr] = useState("");
    const [result, setResult] = useState(null);

    const api = (path, body) =>
      fetch(path, {
        method: "POST", headers: { "Content-Type": "application/json" },
        credentials: "same-origin", body: JSON.stringify(body || {}),
      }).then((r) => r.json());

    const enroll = () => {
      if (!username) { setErr("Enter the username to bind the card to."); return; }
      setErr(""); setResult(null); setStatus("tapping");
      api("/api/auth/nfc/enroll/begin", { username: username })
        .then((j) => {
          if (!j || !j.ok) throw new Error((j && j.error && j.error.message) || "Could not begin enrolment.");
          const ticket = j.data.ticket;
          return readCard({ ticket: ticket }).then((c) => ({ ticket: ticket, card: c }));
        })
        .then(({ ticket, card: c }) => {
          setStatus("saving");
          return api("/api/auth/nfc/enroll/complete", {
            ticket: ticket, uid: c.uid, type: c.type, label: label,
          });
        })
        .then((j) => {
          if (!j || !j.ok) throw new Error((j && j.error && j.error.message) || "Enrolment failed.");
          setStatus("done"); setResult(j.data.card);
          if (props.onDone) props.onDone(j.data);
        })
        .catch((e) => { setStatus("idle"); setErr(e.message || "Enrolment failed."); });
    };

    const busy = status === "tapping" || status === "saving";
    return (
      <div style={card}>
        <div style={{ fontWeight: 700, fontSize: 16, marginBottom: 14 }}>Enroll NFC card</div>
        <input value={username} onChange={(e) => setUsername(e.target.value)} placeholder="Username" style={field} />
        <input value={label} onChange={(e) => setLabel(e.target.value)} placeholder="Card label (e.g. Blue keyfob)" style={field} />
        {err ? <div style={{ color: "var(--red, #ff3d72)", fontSize: 12, margin: "2px 0 10px" }}>{err}</div> : null}
        {result ? <div style={{ color: "var(--teal, #35e0d0)", fontSize: 12, margin: "2px 0 10px" }}>
          Enrolled {result.cardType} ({result.mode}) — {result.label}
        </div> : null}
        <button type="button" disabled={busy} style={btn(busy)} onClick={enroll}>
          {status === "tapping" ? "Tap the new card…" : status === "saving" ? "Saving…" : "Begin enrolment"}
        </button>
      </div>
    );
  }

  window.SolariNfc = { TapPrompt: TapPrompt, EnrollCard: EnrollCard, readCard: readCard };
})();
