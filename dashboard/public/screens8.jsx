/* ============================================================
   SolariNet — screens8: Tags & Scan (barcode inventory layer)
   CodesScreen  (SoR migration 015)

   The physical-code layer over the inventory SoR: every unit,
   bin, and project can carry scannable codes (QR + Code128).
   The scanner is a USB/BT HID keyboard wedge — it types the code
   and sends Enter — so capture is a focused input reading a fast
   keystroke burst terminated by Enter; no driver, no build step.

   Scan-session state machine (context is client-side; every
   server call is a stateless verb — see routes/inv_codes.php):
     unknown code            → ReceiveModal (intake → new unit + AKO tag)
     location scanned        → arm location mode / bin→project associate
     project scanned         → arm project mode
     unit + location armed   → move (bulk bin-filling, stays armed)
     unit + project armed    → commit allocation (409 = double-commit guard)
     unit, idle              → UnitCard

   Live-only: the SoR has no offline fixture — when the adapter is
   offline this screen renders the standard .empty panel.
   ============================================================ */
(function () {
  const { useState, useEffect, useRef } = React;
  const Icon = window.Icon;
  const S = window.SOLARI;
  const fmt = S.fmt;

  function toast(msg, icon) { if (window.__solariToast) window.__solariToast(msg, icon); }
  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function isOffline() { return !window.SOLARI || window.SOLARI.source === "offline"; }
  function is409(e) {
    if (!e) return false;
    const c = String(e.code || "");
    const m = String(e.message || "").toLowerCase();
    return c === "HTTP_409" || c === "conflict" || c === "already_committed" || m.indexOf("already committed") >= 0 || m.indexOf("another project") >= 0;
  }
  function minsSince(iso) {
    if (!iso) return null;
    const t = Date.parse(iso);
    if (isNaN(t)) return null;
    return Math.max(0, Math.round((Date.now() - t) / 60000));
  }
  function agoIso(iso) { const m = minsSince(iso); return m == null ? "—" : fmt.ago(m); }

  // ---- enum → tone maps (match screens4's vocabulary) ----
  const STOCK_TONE = { in_stock: "var(--teal)", allocated: "var(--violet)", deployed: "var(--ok)", consumed: "var(--ink-faint)", retired: "var(--ink-faint)" };
  const STOCK_LABEL = { in_stock: "in stock", allocated: "allocated", deployed: "deployed", consumed: "consumed", retired: "retired" };
  const ALLOC_TONE = { needed: "var(--warn)", committed: "var(--teal)", installed: "var(--ok)", returned: "var(--ink-faint)" };
  const PROJ_TONE = { planning: "var(--ink-dim)", active: "var(--ok)", staged: "var(--violet)", on_hold: "var(--warn)", complete: "var(--teal)", archived: "var(--ink-faint)" };
  const ACTION_TONE = { lookup: "var(--ink-dim)", create: "var(--teal)", receive: "var(--ok)", move: "var(--violet)", associate: "var(--violet)", allocate: "var(--teal)", print: "var(--warn)", delete: "var(--crit)" };
  const SUBJ_LABEL = { hardware_units: "unit", locations: "loc", projects: "project", interfaces: "port" };
  const TAG_LOC_TYPES = ["bin", "drawer", "shelf", "tray", "container", "cabinet"];
  const LOC_ICON = { room: "location", rack: "server", shelf: "arch", cabinet: "box", drawer: "box", tray: "inventory", bin: "box", container: "box" };
  const PROFILES = [["default", "50×25 EP53"], ["makeid_small", "40×12 E1"], ["nemonic", "80×40 Nemonic"]];
  const RECEIVE_TYPES = ["serial", "upc", "custom"];
  const BURST_MS = 300;   // scanner inter-key gap is < ~35 ms; > this is a stale partial

  function Badge({ tone, children, title }) {
    return <span className="inv-badge" title={title} style={{ color: tone, borderColor: tone }}>{children}</span>;
  }

  // shared form styles (mirror screens4)
  const FLD = { width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 4, borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };
  const LBL = { fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, marginTop: 14, display: "block" };

  const PULSE_CSS =
    "@keyframes codesPulse{0%{box-shadow:0 0 0 0 rgba(53,224,208,.30)}70%{box-shadow:0 0 0 8px rgba(53,224,208,0)}100%{box-shadow:0 0 0 0 rgba(53,224,208,0)}}";

  /* ===================== generic modal shell ===================== */
  function Modal({ title, icon, sub, onClose, children, footer, wide }) {
    useEffect(function () {
      function onKey(e) { if (e.key === "Escape") { e.stopPropagation(); onClose(); } }
      window.addEventListener("keydown", onKey);
      return function () { window.removeEventListener("keydown", onKey); };
    }, [onClose]);
    return (
      <div onClick={onClose} style={{ position: "fixed", inset: 0, zIndex: 1000, background: "rgba(0,0,0,0.55)", display: "flex", alignItems: "center", justifyContent: "center", padding: 20 }}>
        <div onClick={function (e) { e.stopPropagation(); }} style={{ width: wide ? 640 : 480, maxWidth: "94vw", maxHeight: "88vh", overflowY: "auto", background: "var(--panel, #0c1118)", border: "1px solid var(--line-glow, rgba(255,255,255,0.12))", borderRadius: 14, padding: 22, boxShadow: "0 16px 56px rgba(0,0,0,0.5)" }}>
          <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: sub ? 2 : 12 }}>
            <Icon name={icon || "tag2"} size={22} style={{ color: "var(--teal)" }} />
            <div style={{ fontWeight: 700, fontSize: 16, flex: 1 }}>{title}</div>
            <button className="iconbtn" style={{ width: 34, height: 34 }} onClick={onClose} aria-label="Close"><Icon name="close" size={15} /></button>
          </div>
          {sub && <div className="muted" style={{ fontSize: 12, marginBottom: 6 }}>{sub}</div>}
          {children}
          {footer && <div style={{ display: "flex", justifyContent: "flex-end", gap: 10, marginTop: 22 }}>{footer}</div>}
        </div>
      </div>
    );
  }

  /* ===================== reference data (models / locations / receipts) =====
     Prefer the combined /api/inventory read; fall back to the per-list
     endpoints. Both wire dialects (snake_case fixture-era, camelCase rows)
     are normalised into one canonical shape so the modals never care. */
  function normLoc(l) {
    return {
      id: l.id,
      parentId: l.parentId !== undefined ? l.parentId : (l.parent_id !== undefined ? l.parent_id : null),
      name: l.name, code: l.code != null ? l.code : null,
      locType: l.locType || l.loc_type || "bin",
    };
  }
  function normModel(m) {
    return { id: m.id, make: m.make, model: m.model, hwType: m.hwType || m.hw_type || "other" };
  }
  function normReceipt(r) {
    return { id: r.id, vendor: r.vendor || null, purchasedOn: r.purchasedOn || r.purchased_on || null };
  }
  function flattenLocTree(nodes, out) {
    out = out || [];
    (nodes || []).forEach(function (n) {
      out.push(normLoc(n));
      if (n.children && n.children.length) flattenLocTree(n.children, out);
    });
    return out;
  }
  function loadInvRefs() {
    const a = api();
    function fromLists() {
      return Promise.all([
        a.get("/api/inventory/models").catch(function () { return []; }),
        a.get("/api/inventory/locations").catch(function () { return []; }),
        a.get("/api/inventory/receipts").catch(function () { return []; }),
      ]).then(function (res) {
        return {
          models: (Array.isArray(res[0]) ? res[0] : []).map(normModel),
          locations: flattenLocTree(Array.isArray(res[1]) ? res[1] : []),
          receipts: (Array.isArray(res[2]) ? res[2] : []).map(normReceipt),
        };
      });
    }
    return a.inventory().then(function (d) {
      if (!d || !Array.isArray(d.models)) return fromLists();
      return {
        models: d.models.map(normModel),
        // combined read is flat; a tree (children[]) flattens the same way
        locations: flattenLocTree(d.locations),
        receipts: (d.receipts || []).map(normReceipt),
      };
    }).catch(fromLists);
  }
  function locPathOf(locs, id) {
    const byId = {}; locs.forEach(function (l) { byId[l.id] = l; });
    const parts = []; let l = byId[id], g = 0;
    while (l && g++ < 16) { parts.unshift(l.name + (l.code ? " " + l.code : "")); l = l.parentId != null ? byId[l.parentId] : null; }
    return parts.join(" › ");
  }

  /* ===================== ScanCapture ===================== */
  // The HID keyboard-wedge reader. A visually prominent monospace input that
  // stays focused; a keystroke burst terminated by Enter is a scan. A keydown
  // arriving > BURST_MS after the previous one on a scanner-fed (never
  // manually edited) buffer clears the stale partial first. A window-level
  // keydown fallback catches scans that land while focus wandered.
  function ScanCapture({ armed, onScan, onClear, modalOpen }) {
    const [val, setVal] = useState("");
    const inputRef = useRef(null);
    const valRef = useRef("");
    const lastTs = useRef(0);
    const manual = useRef(false);        // true once the buffer was hand-edited
    const cb = useRef({});
    valRef.current = val;
    cb.current = { onScan: onScan, onClear: onClear, modalOpen: modalOpen };

    function submit(raw) {
      const code = String(raw).trim();
      setVal(""); valRef.current = ""; manual.current = false;
      if (code.length >= 2) cb.current.onScan(code);
    }
    function gapNow() {
      const now = Date.now();
      const gap = now - (lastTs.current || 0);
      lastTs.current = now;
      return gap;
    }
    function onChange(e) {
      const next = e.target.value;
      const prev = valRef.current;
      const gap = gapNow();
      // exactly one char appended = keystroke; anything else = hand edit
      if (next.length === prev.length + 1 && next.indexOf(prev) === 0) {
        if (gap > BURST_MS && !manual.current && prev.length > 0) {
          // A slow keystroke into the focused input is a human hunt-and-pecking,
          // not a scanner (scanner inter-key gap is < ~35 ms). Flag it as manual
          // so the rest of the typed code appends instead of resetting the buffer
          // on every keystroke. The stale-partial burst-reset lives in the
          // window-level fallback path, where there is no input to type into.
          manual.current = true;
        }
        setVal(next);
      } else {
        manual.current = true;               // paste / deletion / mid-edit
        setVal(next);
      }
    }
    function onKeyDown(e) {
      if (e.key === "Enter") { e.preventDefault(); submit(valRef.current); }
      else if (e.key === "Escape") {
        setVal(""); valRef.current = ""; manual.current = false;
        cb.current.onClear();
      }
    }
    function onBlur() {
      // never let a wandering click eat a scan — unless a modal owns focus or
      // the operator deliberately moved into another field.
      setTimeout(function () {
        if (cb.current.modalOpen) return;
        const el = inputRef.current;
        if (!el) return;
        const ae = document.activeElement;
        if (ae && ae !== el && (ae.tagName === "INPUT" || ae.tagName === "TEXTAREA" || ae.tagName === "SELECT")) return;
        el.focus();
      }, 50);
    }
    // global fallback (mirrors app.jsx's ⌘K listener pattern): buffer printable
    // chars when focus is not on an input, same burst window.
    useEffect(function () {
      function onKey(e) {
        if (cb.current.modalOpen) return;
        if (document.querySelector(".cmdk-overlay")) return;   // command palette owns the keyboard
        const ae = document.activeElement;
        if (ae && (ae.tagName === "INPUT" || ae.tagName === "TEXTAREA" || ae.tagName === "SELECT" || ae.isContentEditable)) return;
        if (e.metaKey || e.ctrlKey || e.altKey) return;
        if (e.key === "Enter") {
          if (valRef.current.trim().length >= 2) { e.preventDefault(); submit(valRef.current); }
        } else if (e.key.length === 1) {
          e.preventDefault();
          const gap = gapNow();
          const prev = valRef.current;
          const next = (gap > BURST_MS && !manual.current && prev.length > 0) ? e.key : prev + e.key;
          valRef.current = next;
          setVal(next);
          if (inputRef.current) inputRef.current.focus();
        }
      }
      window.addEventListener("keydown", onKey);
      return function () { window.removeEventListener("keydown", onKey); };
    }, []);
    // when a modal closes, re-arm the bar
    useEffect(function () {
      if (!modalOpen && inputRef.current) inputRef.current.focus();
    }, [modalOpen]);

    return (
      <div style={{ position: "relative", flex: 1, minWidth: 220 }}>
        <Icon name="tag2" size={17} style={{ position: "absolute", left: 13, top: "50%", transform: "translateY(-50%)", color: "var(--teal)", pointerEvents: "none" }} />
        <input ref={inputRef} autoFocus spellCheck={false} autoComplete="off"
          value={val} onChange={onChange} onKeyDown={onKeyDown} onBlur={onBlur}
          placeholder="Scan a code — or type it and press Enter"
          aria-label="Scan a barcode"
          style={{
            width: "100%", boxSizing: "border-box", padding: "12px 14px 12px 38px",
            borderRadius: 10, fontFamily: "var(--mono)", fontSize: 15, letterSpacing: "0.03em",
            color: "inherit", background: "rgba(53,224,208,0.05)",
            border: "1px solid " + (armed ? "var(--teal)" : "var(--line-glow, rgba(255,255,255,0.18))"),
            animation: armed && !modalOpen ? "codesPulse 1.8s ease-out infinite" : "none",
          }} />
      </div>
    );
  }

  /* ===================== cards ===================== */
  function CardShell({ icon, tone, title, chips, onClose, children, actions }) {
    return (
      <div className="panel">
        <div className="panel__head">
          <Icon name={icon} size={16} style={{ color: tone || "var(--teal)" }} />
          <h3 style={{ minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{title}</h3>
          {chips}
          <div className="right">
            <button className="iconbtn" style={{ width: 30, height: 30 }} onClick={onClose} aria-label="Dismiss card"><Icon name="close" size={13} /></button>
          </div>
        </div>
        <div className="panel__body">
          {children}
          {actions && <div style={{ display: "flex", gap: 8, marginTop: 14, flexWrap: "wrap" }}>{actions}</div>}
        </div>
      </div>
    );
  }

  function UnitCard({ unit, locs, onClose, onPrint, onMove }) {
    const codes = unit.codes || [];
    const assetCode = codes.find(function (c) { return c.codeType === "asset_tag"; }) || codes[0] || null;
    const path = unit.locationId ? locPathOf(locs, unit.locationId) : "";
    return (
      <CardShell icon="box" title={(unit.make || "") + " " + (unit.model || "")} onClose={onClose}
        chips={<>
          {unit.hwType && <span className="tag">{unit.hwType}</span>}
          <Badge tone={STOCK_TONE[unit.stockStatus]}>{STOCK_LABEL[unit.stockStatus] || unit.stockStatus}</Badge>
        </>}
        actions={<>
          <button className="btn-primary" disabled={!assetCode} onClick={function () { assetCode && onPrint(assetCode); }}><Icon name="tag2" size={13} />Print label</button>
          <button className="btn-ghost" onClick={function () { onMove(unit); }}><Icon name="location" size={13} style={{ marginRight: 4 }} />Move…</button>
        </>}>
        <div style={{ display: "flex", gap: 6, flexWrap: "wrap", marginBottom: 10 }}>
          {codes.length === 0 && <span className="muted" style={{ fontSize: 12 }}>no codes on file</span>}
          {codes.map(function (c) {
            return <span key={c.id} className="inv-code" title={c.codeType} style={c.codeType === "asset_tag" ? { color: "var(--teal)", fontWeight: 700 } : undefined}>{c.code}</span>;
          })}
        </div>
        <div className="td-mono muted" style={{ fontSize: 11.5, marginBottom: 4 }}>
          {unit.serial ? "SN " + unit.serial : "no serial"} · qty {unit.quantity || 1} · {fmt.usd(unit.unitCostCents)}
        </div>
        <div style={{ fontSize: 12.5 }}>
          <Icon name="location" size={13} style={{ verticalAlign: "-2px", color: "var(--teal)", marginRight: 5 }} />
          {path || unit.locationName || <span className="muted">unassigned</span>}
        </div>
        {unit.committedProject && (
          <div style={{ fontSize: 12.5, marginTop: 6 }}>
            <Icon name="project" size={13} style={{ verticalAlign: "-2px", color: "var(--violet)", marginRight: 5 }} />
            committed to <b>{unit.committedProject.name}</b>
          </div>
        )}
      </CardShell>
    );
  }

  function LocationCard({ data, barcode, armed, onArm, onClose, onPrint }) {
    const loc = data.location;
    return (
      <CardShell icon={LOC_ICON[loc.locType] || "box"} title={loc.name} onClose={onClose}
        chips={<>
          {loc.code != null && <span className="inv-code">{loc.code}</span>}
          <span className="tag">{loc.locType}</span>
        </>}
        actions={<>
          <button className={armed ? "btn-ghost" : "btn-primary"} disabled={armed} onClick={onArm}>
            <Icon name="check" size={13} />{armed ? "Armed — scans move stock here" : "Arm as target"}
          </button>
          <button className="btn-ghost" disabled={!barcode} title={barcode ? "" : "This location has no tag yet — mint one on the Generate tags tab"}
            onClick={function () { barcode && onPrint(barcode); }}><Icon name="tag2" size={13} style={{ marginRight: 4 }} />Print label</button>
        </>}>
        {data.path && <div className="td-mono muted" style={{ fontSize: 11.5, marginBottom: 10 }}>{data.path}</div>}
        {(data.units || []).length === 0
          ? <div className="muted" style={{ fontSize: 12.5 }}>Nothing stored here (or below) yet.</div>
          : <div className="tablewrap"><table className="grid">
              <thead><tr><th>Model</th><th>Serial</th><th style={{ textAlign: "right" }}>Qty</th><th>Stock</th><th style={{ textAlign: "right" }}>Value</th></tr></thead>
              <tbody>
                {data.units.map(function (u) {
                  return (
                    <tr key={u.id}>
                      <td><div style={{ fontWeight: 600 }}>{(u.make || "") + " " + (u.model || "")}</div></td>
                      <td className="td-mono">{u.serial || "—"}</td>
                      <td className="td-mono" style={{ textAlign: "right" }}>{u.quantity || 1}</td>
                      <td><Badge tone={STOCK_TONE[u.stockStatus]}>{STOCK_LABEL[u.stockStatus] || u.stockStatus}</Badge></td>
                      <td className="td-mono" style={{ textAlign: "right" }}>{fmt.usd((u.unitCostCents || 0) * (u.quantity || 1))}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table></div>}
        <div style={{ display: "flex", gap: 16, marginTop: 12, fontSize: 12.5 }}>
          <span><b className="td-mono">{data.unitCount || 0}</b> <span className="muted">unit{data.unitCount === 1 ? "" : "s"} in subtree</span></span>
          <span><b className="td-mono" style={{ color: "var(--teal)" }}>{fmt.usd(data.valueCents)}</b> <span className="muted">total value</span></span>
        </div>
      </CardShell>
    );
  }

  function ProjectCard({ data, armed, locs, onArm, onClose, onPrintTray }) {
    const p = data.project;
    const allocs = data.allocations || [];
    const counts = { needed: 0, committed: 0, installed: 0 };
    allocs.forEach(function (a) { if (counts[a.allocation] != null) counts[a.allocation] += 1; });
    return (
      <CardShell icon="project" tone="var(--violet)" title={p.name} onClose={onClose}
        chips={<>
          {p.code && <span className="inv-code">{p.code}</span>}
          <Badge tone={PROJ_TONE[p.status]}>{String(p.status || "").replace("_", " ")}</Badge>
        </>}
        actions={<>
          <button className={armed ? "btn-ghost" : "btn-primary"} disabled={armed} onClick={onArm}>
            <Icon name="check" size={13} />{armed ? "Armed — scans commit here" : "Arm project"}
          </button>
          <button className="btn-ghost" disabled={!data.tray} title={data.tray ? "" : "No tray location set"}
            onClick={onPrintTray}><Icon name="tag2" size={13} style={{ marginRight: 4 }} />Print tray label</button>
        </>}>
        <div style={{ fontSize: 12.5, marginBottom: 10 }}>
          <Icon name="inventory" size={13} style={{ verticalAlign: "-2px", color: "var(--teal)", marginRight: 5 }} />
          {data.tray
            ? <span title={locPathOf(locs, data.tray.id)}>tray: <b>{data.tray.name}</b>{data.tray.code != null ? <span className="inv-code" style={{ marginLeft: 6 }}>{data.tray.code}</span> : null}</span>
            : <span className="muted">no tray — scan a bin while armed to make it the tray</span>}
        </div>
        <div style={{ display: "flex", gap: 6, marginBottom: 12 }}>
          {["needed", "committed", "installed"].map(function (k) {
            return <Badge key={k} tone={ALLOC_TONE[k]}>{k} {counts[k]}</Badge>;
          })}
        </div>
        <div className="page-sub" style={{ margin: "0 0 6px" }}>On the tray ({(data.trayContents || []).length})</div>
        {(data.trayContents || []).length === 0
          ? <div className="muted" style={{ fontSize: 12.5 }}>Tray is empty.</div>
          : <div className="tablewrap"><table className="grid">
              <thead><tr><th>Model</th><th>Serial</th><th>Stock</th></tr></thead>
              <tbody>
                {data.trayContents.map(function (u) {
                  return (
                    <tr key={u.id}>
                      <td style={{ fontWeight: 600 }}>{(u.make || "") + " " + (u.model || "")}</td>
                      <td className="td-mono">{u.serial || "—"}</td>
                      <td><Badge tone={STOCK_TONE[u.stockStatus]}>{STOCK_LABEL[u.stockStatus] || u.stockStatus}</Badge></td>
                    </tr>
                  );
                })}
              </tbody>
            </table></div>}
      </CardShell>
    );
  }

  function PortCard({ port, onClose }) {
    return (
      <CardShell icon="netswitch" title={port.name || ("port #" + port.id)} onClose={onClose}
        chips={port.ifKind ? <span className="tag">{port.ifKind}</span> : null}>
        <div className="td-mono muted" style={{ fontSize: 12 }}>
          entity #{port.entityId}{port.mac ? " · " + port.mac : ""}
        </div>
        <div className="muted" style={{ fontSize: 12, marginTop: 8 }}>Physical-port tagging lands in a later iteration.</div>
      </CardShell>
    );
  }

  /* ===================== modals ===================== */
  function ModelModal({ onClose, onCreated }) {
    const HW_TYPES = ["server", "workstation", "sbc", "nas", "router", "switch", "ap", "printer", "scanner", "iot", "appliance", "console", "media", "vehicle", "tablet", "phone", "kvm", "sensor", "camera", "ups", "moca", "other"];
    const [make, setMake] = useState("");
    const [model, setModel] = useState("");
    const [hwType, setHwType] = useState("other");
    const [busy, setBusy] = useState(false);
    function submit() {
      if (!make.trim() || !model.trim()) { toast("Make and model are required", "close"); return; }
      setBusy(true);
      api().createModel({ make: make.trim(), model: model.trim(), hw_type: hwType })
        .then(function (row) {
          toast("Model added: " + make.trim() + " " + model.trim(), "check");
          onCreated(normModel(row || { id: null, make: make.trim(), model: model.trim(), hw_type: hwType }));
        })
        .catch(function (e) { setBusy(false); toast("Model failed: " + (e && e.message || "error"), "close"); });
    }
    return (
      <Modal title="New model" icon="tag2" sub="Add a make/model to the catalog" onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />Add model</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Make</label><input autoFocus style={FLD} value={make} onChange={function (e) { setMake(e.target.value); }} placeholder="Dell, Ubiquiti…" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Model</label><input style={FLD} value={model} onChange={function (e) { setModel(e.target.value); }} placeholder="R410, U7 Pro…" /></div>
        </div>
        <label style={LBL}>Type</label>
        <select style={FLD} value={hwType} onChange={function (e) { setHwType(e.target.value); }}>{HW_TYPES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}</select>
      </Modal>
    );
  }

  // The intake loop — the flow that must feel fastest. Scanned unknown code →
  // pick/create a model → codeReceive mints the unit + AKO tag → PrintModal.
  function ReceiveModal({ code, guessedType, refs, armedLocId, onAddModel, onClose, onDone }) {
    const initType = RECEIVE_TYPES.indexOf(guessedType) >= 0 ? guessedType : "custom";
    const [codeType, setCodeType] = useState(initType);
    const [modelId, setModelId] = useState(refs.models[0] ? refs.models[0].id : "");
    const [serial, setSerial] = useState(initType === "serial" ? code : "");
    const [locId, setLocId] = useState(armedLocId != null ? String(armedLocId) : "");
    const [qty, setQty] = useState(1);
    const [cost, setCost] = useState("");
    const [receiptId, setReceiptId] = useState("");
    const [addModel, setAddModel] = useState(false);
    const [busy, setBusy] = useState(false);
    function pickType(t) {
      setCodeType(t);
      if (t === "serial" && !serial) setSerial(code);
    }
    function submit() {
      if (!modelId) { toast("Pick a model", "close"); return; }
      setBusy(true);
      const body = {
        code: code,
        code_type: codeType,
        model_id: Number(modelId),
        quantity: Number(qty) || 1,
      };
      if (serial.trim()) body.serial = serial.trim();
      if (locId) body.location_id = Number(locId);
      if (receiptId) body.receipt_id = Number(receiptId);
      if (cost !== "") body.unit_cost_cents = Math.round(parseFloat(cost) * 100);
      api().codeReceive(body)
        .then(function (resp) {
          const u = resp && resp.unit;
          const tag = resp && resp.assetTag;
          toast("Received · " + (u ? (u.make + " " + u.model) : "unit") + " tagged " + (tag ? tag.code : "—"), "check");
          onDone(resp);
        })
        .catch(function (e) {
          setBusy(false);
          if (is409(e)) toast("409 · that code is already registered", "close");
          else toast("Receive failed: " + (e && e.message || "error"), "close");
        });
    }
    return (
      <Modal title="Receive new stock" icon="cart" wide
        sub={<span>Unknown code <span className="inv-code" style={{ color: "var(--teal)" }}>{code}</span> — intake it as a new component</span>}
        onClose={onClose}
        footer={<>
          <button className="btn-ghost" onClick={onClose}>Cancel</button>
          <button className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />{busy ? "Receiving…" : "Receive + tag"}</button>
        </>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}>
            <label style={LBL}>Model</label>
            <div style={{ display: "flex", gap: 8 }}>
              <select style={FLD} value={String(modelId)} onChange={function (e) { setModelId(e.target.value); }}>
                {refs.models.length === 0 && <option value="">— no models yet —</option>}
                {refs.models.map(function (m) { return <option key={m.id} value={String(m.id)}>{m.make + " " + m.model} · {m.hwType}</option>; })}
              </select>
              <button className="btn-ghost" style={{ marginTop: 4, flex: "0 0 auto" }} onClick={function () { setAddModel(true); }} title="Add a new model"><Icon name="plus" size={13} /></button>
            </div>
          </div>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Code type</label>
            <select style={FLD} value={codeType} onChange={function (e) { pickType(e.target.value); }}>
              {RECEIVE_TYPES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}
            </select>
          </div>
        </div>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Serial</label><input style={FLD} value={serial} onChange={function (e) { setSerial(e.target.value); }} placeholder="manufacturer serial" /></div>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Location{armedLocId != null ? " (armed bin)" : ""}</label>
            <select style={FLD} value={locId} onChange={function (e) { setLocId(e.target.value); }}>
              <option value="">— unassigned —</option>
              {refs.locations.map(function (l) { return <option key={l.id} value={String(l.id)}>{locPathOf(refs.locations, l.id)}</option>; })}
            </select>
          </div>
        </div>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Quantity</label><input type="number" min="1" style={FLD} value={qty} onChange={function (e) { setQty(e.target.value); }} /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Unit cost (USD)</label><input type="number" min="0" step="0.01" style={FLD} value={cost} onChange={function (e) { setCost(e.target.value); }} placeholder="0.00" /></div>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Receipt</label>
            <select style={FLD} value={receiptId} onChange={function (e) { setReceiptId(e.target.value); }}>
              <option value="">— none —</option>
              {refs.receipts.map(function (r) { return <option key={r.id} value={String(r.id)}>{(r.vendor || ("#" + r.id)) + (r.purchasedOn ? " · " + r.purchasedOn : "")}</option>; })}
            </select>
          </div>
        </div>
        {addModel && <ModelModal onClose={function () { setAddModel(false); }}
          onCreated={function (m) { setAddModel(false); onAddModel(m); if (m.id != null) setModelId(m.id); }} />}
      </Modal>
    );
  }

  function MoveModal({ unit, refs, onClose, onMoved }) {
    const [locId, setLocId] = useState(unit.locationId != null ? String(unit.locationId) : "");
    const [busy, setBusy] = useState(false);
    function submit() {
      if (!locId) { toast("Pick a location", "close"); return; }
      setBusy(true);
      api().codeMove({ hardware_unit_id: unit.id, location_id: Number(locId) })
        .then(function (row) {
          toast((row.make + " " + row.model) + " → " + (row.locationName || "location #" + row.locationId), "check");
          onMoved(row);
        })
        .catch(function (e) { setBusy(false); toast("Move failed: " + (e && e.message || "error"), "close"); });
    }
    return (
      <Modal title={"Move " + (unit.make || "") + " " + (unit.model || "")} icon="location"
        sub={unit.serial ? "SN " + unit.serial : (unit.assetTag || "")} onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />Move</button></>}>
        <label style={LBL}>To location</label>
        <select autoFocus style={FLD} value={locId} onChange={function (e) { setLocId(e.target.value); }}>
          <option value="">— pick a bin / drawer / shelf —</option>
          {refs.locations.map(function (l) { return <option key={l.id} value={String(l.id)}>{locPathOf(refs.locations, l.id)}</option>; })}
        </select>
        <div className="muted" style={{ fontSize: 11.5, marginTop: 12 }}>
          Tip: scan the destination bin first to arm it, then scan units — no modal needed.
        </div>
      </Modal>
    );
  }

  // Label preview + download + mark-printed. The GET is side-effect-free;
  // "Mark printed" is the separate POST that stamps label_printed_at.
  function PrintModal({ code, onClose, onPrinted }) {
    const [profile, setProfile] = useState("default");
    const [format, setFormat] = useState("png");
    const [imgErr, setImgErr] = useState(false);
    const [printedAt, setPrintedAt] = useState(code.labelPrintedAt || null);
    const [busy, setBusy] = useState(false);
    const a = api();
    const previewUrl = a.codeLabelUrl(code.id, "?profile=" + encodeURIComponent(profile));
    const downloadUrl = a.codeLabelUrl(code.id, "?profile=" + encodeURIComponent(profile) + "&format=" + encodeURIComponent(format));
    function markPrinted() {
      setBusy(true);
      a.codeMarkPrinted(code.id)
        .then(function (row) {
          setBusy(false);
          setPrintedAt((row && row.labelPrintedAt) || new Date().toISOString());
          toast("Label marked printed · " + code.code, "check");
          onPrinted && onPrinted(row);
        })
        .catch(function (e) { setBusy(false); toast("Mark failed: " + (e && e.message || "error"), "close"); });
    }
    const checker = {
      display: "flex", alignItems: "center", justifyContent: "center", padding: 18, borderRadius: 10,
      border: "1px solid var(--line-glow, rgba(255,255,255,0.12))",
      backgroundImage: "linear-gradient(45deg, rgba(255,255,255,0.07) 25%, transparent 25%, transparent 75%, rgba(255,255,255,0.07) 75%), linear-gradient(45deg, rgba(255,255,255,0.07) 25%, transparent 25%, transparent 75%, rgba(255,255,255,0.07) 75%)",
      backgroundSize: "16px 16px", backgroundPosition: "0 0, 8px 8px",
    };
    return (
      <Modal title="Print label" icon="tag2" wide
        sub={<span><span className="inv-code" style={{ color: "var(--teal)" }}>{code.code}</span> · {code.codeType} · {code.symbology}</span>}
        onClose={onClose}
        footer={<>
          <button className="btn-ghost" onClick={onClose}>Close</button>
          <a className="btn-ghost" style={{ textDecoration: "none", display: "inline-flex", alignItems: "center", gap: 6 }}
            href={downloadUrl} download={"label-" + code.code + "." + format}><Icon name="enter" size={13} />Download {format.toUpperCase()}</a>
          <button className="btn-primary" disabled={busy} onClick={markPrinted}><Icon name="check" size={14} />Mark printed</button>
        </>}>
        <div style={checker}>
          {imgErr
            ? <div style={{ textAlign: "center", padding: "18px 8px" }}>
                <Icon name="alerts" size={26} style={{ color: "var(--warn)" }} />
                <div style={{ fontSize: 12.5, marginTop: 8, maxWidth: 380 }}>
                  Label renderer unavailable — run <span className="td-mono">sudo apt install python3-qrcode python3-barcode</span> on the API host.
                </div>
              </div>
            : <img key={profile} src={previewUrl} alt={"label " + code.code}
                onError={function () { setImgErr(true); }} onLoad={function () { setImgErr(false); }}
                style={{ maxWidth: "100%", imageRendering: "pixelated", background: "#fff", borderRadius: 4 }} />}
        </div>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}>
            <label style={LBL}>Label profile</label>
            <select style={FLD} value={profile} onChange={function (e) { setProfile(e.target.value); setImgErr(false); }}>
              {PROFILES.map(function (p) { return <option key={p[0]} value={p[0]}>{p[1]}</option>; })}
            </select>
          </div>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Format</label>
            <div className="seg" style={{ marginTop: 4 }}>
              {["png", "pdf"].map(function (f) {
                return <button key={f} className={format === f ? "on" : ""} onClick={function () { setFormat(f); }}>{f.toUpperCase()}</button>;
              })}
            </div>
          </div>
        </div>
        <div className="td-mono muted" style={{ fontSize: 11, marginTop: 14 }}>
          {printedAt ? "last printed " + agoIso(printedAt) : "never printed"} · transport to EP53 / E1 / Nemonic is download-only in this iteration
        </div>
      </Modal>
    );
  }

  /* ===================== recent scans panel ===================== */
  function RecentScans({ rows, onRefresh }) {
    return (
      <div className="panel">
        <div className="panel__head">
          <Icon name="clock" size={15} />
          <h3>Recent scans</h3>
          <div className="right">
            <button className="iconbtn" style={{ width: 30, height: 30 }} onClick={onRefresh} title="Refresh" aria-label="Refresh history"><Icon name="refresh" size={13} /></button>
          </div>
        </div>
        <div className="panel__body" style={{ padding: "6px 10px" }}>
          {rows.length === 0 && <div className="muted" style={{ fontSize: 12, padding: "8px 4px" }}>No scans yet — the audit log fills as you work.</div>}
          {rows.map(function (r) {
            return (
              <div key={r.id} className="inv-tree-row" style={{ paddingLeft: 4 }}>
                <Badge tone={ACTION_TONE[r.action] || "var(--ink-dim)"}>{r.action}</Badge>
                <span className="inv-code" style={{ maxWidth: 130, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }} title={r.code}>{r.code}</span>
                <span className="td-mono muted" style={{ fontSize: 10.5 }}>
                  {r.subjectTable ? (SUBJ_LABEL[r.subjectTable] || r.subjectTable) + " #" + r.subjectId : "—"}
                </span>
                <span style={{ flex: 1 }} />
                <span className="td-mono muted" style={{ fontSize: 10.5, whiteSpace: "nowrap" }}>{agoIso(r.createdAt)}</span>
              </div>
            );
          })}
        </div>
      </div>
    );
  }

  /* ===================== Generate tags tab ===================== */
  function GenerateTab({ refs, onPrint }) {
    const [codesByLoc, setCodesByLoc] = useState(null);   // locId -> codeRow
    const [filter, setFilter] = useState("all");
    const [busyAll, setBusyAll] = useState(false);
    const [busyIds, setBusyIds] = useState({});

    function loadCodes() {
      api().codes("?subject_table=locations&limit=2000")
        .then(function (rows) {
          const map = {};
          (Array.isArray(rows) ? rows : []).forEach(function (c) {
            if (map[c.subjectId] == null) map[c.subjectId] = c;   // id DESC — keep newest
          });
          setCodesByLoc(map);
        })
        .catch(function (e) { toast("Codes failed: " + (e && e.message || "error"), "close"); setCodesByLoc({}); });
    }
    useEffect(loadCodes, []);

    const all = refs.locations.filter(function (l) { return TAG_LOC_TYPES.indexOf(l.locType) >= 0; })
      .slice().sort(function (a, b) { return locPathOf(refs.locations, a.id).localeCompare(locPathOf(refs.locations, b.id)); });
    const rows = all.filter(function (l) { return filter === "all" || l.locType === filter; });
    const untagged = codesByLoc ? all.filter(function (l) { return !codesByLoc[l.id]; }) : [];

    function tagOne(l) {
      setBusyIds(function (b) { const n = Object.assign({}, b); n[l.id] = true; return n; });
      return api().codeCreate({ code_type: "location", subject_table: "locations", subject_id: l.id })
        .then(function (row) {
          setCodesByLoc(function (m) { const n = Object.assign({}, m); n[l.id] = row; return n; });
          return row;
        })
        .finally(function () {
          setBusyIds(function (b) { const n = Object.assign({}, b); delete n[l.id]; return n; });
        });
    }
    function tagAll() {
      if (untagged.length === 0) return;
      setBusyAll(true);
      let done = 0;
      // sequential so inv_next_code never races itself
      untagged.reduce(function (p, l) {
        return p.then(function () { return tagOne(l).then(function () { done += 1; }).catch(function () {}); });
      }, Promise.resolve()).then(function () {
        setBusyAll(false);
        toast("Tagged " + done + " of " + untagged.length + " location" + (untagged.length === 1 ? "" : "s"), "check");
      });
    }

    return (
      <div>
        <div className="filters">
          {["all"].concat(TAG_LOC_TYPES).map(function (t) {
            const n = t === "all" ? all.length : all.filter(function (l) { return l.locType === t; }).length;
            if (t !== "all" && n === 0) return null;
            return <button key={t} className={"chip" + (filter === t ? " on" : "")} onClick={function () { setFilter(t); }}>
              {t === "all" ? "All" : t}<span className="chip__n">{n}</span>
            </button>;
          })}
          <button className="btn-primary" style={{ marginLeft: "auto" }} disabled={busyAll || !codesByLoc || untagged.length === 0} onClick={tagAll}>
            <Icon name="tag2" size={14} />{busyAll ? "Tagging…" : "Tag all untagged (" + untagged.length + ")"}
          </button>
        </div>

        {rows.length === 0 && <div className="empty">No bins, drawers, shelves, trays, containers, or cabinets yet — add storage locations on the Inventory screen first.</div>}
        {rows.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr><th>Location</th><th>Path</th><th>Type</th><th>Tag</th><th style={{ textAlign: "right" }}>Actions</th></tr></thead>
            <tbody>
              {rows.map(function (l) {
                const c = codesByLoc ? codesByLoc[l.id] : null;
                return (
                  <tr key={l.id}>
                    <td>
                      <Icon name={LOC_ICON[l.locType] || "box"} size={14} style={{ color: "var(--teal)", verticalAlign: "-2px", marginRight: 6 }} />
                      <span style={{ fontWeight: 600 }}>{l.name}</span>
                      {l.code != null && <span className="inv-code" style={{ marginLeft: 6 }}>{l.code}</span>}
                    </td>
                    <td className="td-mono muted" style={{ fontSize: 11 }}>{locPathOf(refs.locations, l.id)}</td>
                    <td><span className="tag">{l.locType}</span></td>
                    <td>{c
                      ? <span className="inv-code" style={{ color: "var(--teal)", fontWeight: 700 }} title={c.labelPrintedAt ? "printed " + agoIso(c.labelPrintedAt) : "never printed"}>{c.code}</span>
                      : <span className="muted">—</span>}</td>
                    <td style={{ textAlign: "right", whiteSpace: "nowrap" }}>
                      {!c && <button className="inv-mini" disabled={!codesByLoc || !!busyIds[l.id]} onClick={function () {
                        tagOne(l).then(function (row) { toast("Tagged " + l.name + " · " + row.code, "check"); })
                          .catch(function (e) { toast("Tag failed: " + (e && e.message || "error"), "close"); });
                      }}>{busyIds[l.id] ? "Tagging…" : "Tag"}</button>}
                      {c && <button className="inv-mini" onClick={function () { onPrint(c); }}>Print</button>}
                    </td>
                  </tr>
                );
              })}
            </tbody>
          </table></div>
        )}
      </div>
    );
  }

  /* ===================== root screen ===================== */
  function CodesScreen() {
    const [tab, setTab] = useState("scan");
    // scan-session context — client-side only; every server call is stateless
    const [ctx, setCtx] = useState({ mode: "idle" });
    const [card, setCard] = useState(null);
    const [hist, setHist] = useState([]);
    const [refs, setRefs] = useState({ models: [], locations: [], receipts: [] });
    const [receive, setReceive] = useState(null);   // { code, guessedType }
    const [printFor, setPrintFor] = useState(null); // codeRow
    const [moveFor, setMoveFor] = useState(null);   // unit componentRow(+codes)
    const ctxRef = useRef(ctx); ctxRef.current = ctx;
    const cardRef = useRef(card); cardRef.current = card;
    const modalOpen = !!(receive || printFor || moveFor);

    const offline = isOffline();
    useEffect(function () {
      if (offline) return;
      loadInvRefs().then(setRefs).catch(function () {});
      loadHistory();
    }, []);

    function loadHistory() {
      const a = api();
      if (!a || isOffline()) return;
      a.codeHistory("?limit=25").then(function (rows) { setHist(Array.isArray(rows) ? rows : []); }).catch(function () {});
    }

    function openLocationCard(locId, barcode) {
      api().locationContents(locId)
        .then(function (d) { setCard({ kind: "location", data: d, barcode: barcode || null }); })
        .catch(function (e) { toast("Contents failed: " + (e && e.message || "error"), "close"); });
    }
    function openProjectCard(projId, barcode) {
      api().projectRollup(projId)
        .then(function (d) { setCard({ kind: "project", data: d, barcode: barcode || null }); })
        .catch(function (e) { toast("Rollup failed: " + (e && e.message || "error"), "close"); });
    }
    function refreshCard() {
      const c = cardRef.current;
      if (!c) return;
      if (c.kind === "location") openLocationCard(c.data.location.id, c.barcode);
      else if (c.kind === "project") openProjectCard(c.data.project.id, c.barcode);
    }
    function clearContext() { setCtx({ mode: "idle" }); }

    // ---- the state machine ----
    function onScan(code) {
      const a = api();
      a.codeLookup(code).then(function (r) {
        loadHistory();
        if (!r.found) { setReceive({ code: r.code || code, guessedType: r.guessedType || "custom" }); return; }
        const st = r.subjectTable || (r.barcode && r.barcode.subjectTable);
        const subj = r.subject;
        const mode = ctxRef.current.mode;

        // The code resolved, but its subject row has since been soft-deleted
        // (lookup returns found:true, subject:null). Bail before dereferencing.
        if (!subj) { toast("Code points at a deleted " + (st || "record") + " row", "close"); return; }

        if (st === "locations") {
          if (mode === "project") {
            // bin → project: becomes the tray, or nests under it; STAY armed
            const proj = ctxRef.current.proj;
            a.codeAssociate({ project_id: proj.id, location_id: subj.id })
              .then(function (resp) {
                toast(resp.mode === "tray"
                  ? "Bin " + subj.name + " is now " + proj.name + "'s tray"
                  : "Bin " + subj.name + " nested under " + proj.name + "'s tray", "check");
                loadHistory(); refreshCard();
              })
              .catch(function (e) {
                if (is409(e)) toast("409 · " + (e.message || "would create a location cycle"), "close");
                else toast("Associate failed: " + (e && e.message || "error"), "close");
              });
          } else {
            setCtx({ mode: "location", loc: subj });
            openLocationCard(subj.id, r.barcode);
          }
        } else if (st === "projects") {
          setCtx({ mode: "project", proj: subj });
          openProjectCard(subj.id, r.barcode);
        } else if (st === "hardware_units") {
          if (mode === "location") {
            // bulk bin-filling: move, stay armed, refresh the bin card
            const loc = ctxRef.current.loc;
            a.codeMove({ hardware_unit_id: subj.id, location_id: loc.id })
              .then(function (row) {
                toast((row.make + " " + row.model) + " → " + loc.name, "check");
                loadHistory(); refreshCard();
              })
              .catch(function (e) { toast("Move failed: " + (e && e.message || "error"), "close"); });
          } else if (mode === "project") {
            // commit to the armed project; 409 = double-commit guard, stay armed
            const proj = ctxRef.current.proj;
            a.codeAssociate({ project_id: proj.id, hardware_unit_id: subj.id })
              .then(function () {
                toast((subj.make + " " + subj.model) + " committed to " + proj.name, "check");
                loadHistory(); refreshCard();
              })
              .catch(function (e) {
                if (is409(e)) toast("409 · unit already committed to another project", "close");
                else toast("Commit failed: " + (e && e.message || "error"), "close");
              });
          } else {
            setCard({ kind: "unit", unit: subj, barcode: r.barcode });
          }
        } else {
          setCard({ kind: "port", port: subj, barcode: r.barcode });
        }
      }).catch(function (e) {
        toast("Scan failed: " + (e && e.message || "error"), "close");
      });
    }

    function onReceived(resp) {
      setReceive(null);
      const unit = Object.assign({}, resp.unit, {
        codes: [resp.assetTag, resp.scannedCode].filter(Boolean),
        committedProject: null,
      });
      setCard({ kind: "unit", unit: unit, barcode: resp.assetTag });
      loadHistory();
      loadInvRefs().then(setRefs).catch(function () {});   // a new unit may have landed in a bin
      // one-keystroke intake→label loop: the fresh AKO tag goes straight to print
      if (resp.assetTag) setPrintFor(resp.assetTag);
    }
    function onMoved(row) {
      setMoveFor(null);
      const c = cardRef.current;
      if (c && c.kind === "unit" && c.unit.id === row.id) {
        setCard({ kind: "unit", unit: Object.assign({}, c.unit, row), barcode: c.barcode });
      } else refreshCard();
      loadHistory();
    }
    function printTray(data) {
      if (!data.tray) return;
      api().codes("?subject_table=locations&subject_id=" + data.tray.id)
        .then(function (rows) {
          const c = (Array.isArray(rows) ? rows : [])[0];
          if (c) setPrintFor(c);
          else toast("Tray has no tag yet — mint one on the Generate tags tab", "close");
        })
        .catch(function (e) { toast("Codes failed: " + (e && e.message || "error"), "close"); });
    }
    function onPrinted(row) {
      if (row) {
        setPrintFor(row);
        // reflect the stamp on a unit card's chips
        const c = cardRef.current;
        if (c && c.kind === "unit" && c.unit.codes) {
          const codes = c.unit.codes.map(function (x) { return x.id === row.id ? row : x; });
          setCard({ kind: "unit", unit: Object.assign({}, c.unit, { codes: codes }), barcode: c.barcode });
        }
      }
      loadHistory();
    }

    const TABS = [["scan", "Scan", "tag2"], ["generate", "Generate tags", "grid"]];
    const armed = ctx.mode !== "idle";

    return (
      <div className="page">
        <style>{PULSE_CSS}</style>
        <div className="page-head">
          <div>
            <h1 className="page-title">Tags &amp; Scan</h1>
            <div className="page-sub">scan a code · arm a bin or project · receive new stock</div>
          </div>
          <div className="page-head__right">
            <div className="seg">
              {TABS.map(function (t) {
                return <button key={t[0]} className={tab === t[0] ? "on" : ""} onClick={function () { setTab(t[0]); }}><Icon name={t[2]} size={14} />{t[1]}</button>;
              })}
            </div>
          </div>
        </div>

        {offline && (
          <div className="empty">
            Barcode tags need the live API — the SoR (codes, scan audit, label renderer) has no offline fixture.
          </div>
        )}

        {!offline && tab === "scan" && (
          <div>
            <div style={{ display: "flex", gap: 10, alignItems: "center", marginBottom: 16, flexWrap: "wrap" }}>
              <ScanCapture armed={armed} onScan={onScan} onClear={clearContext} modalOpen={modalOpen} />
              {ctx.mode === "location" && (
                <span className="inv-badge" style={{ color: "var(--teal)", borderColor: "var(--teal)", display: "inline-flex", alignItems: "center", gap: 6, padding: "7px 10px", fontSize: 12 }}>
                  ◉ {ctx.loc.name}{ctx.loc.code != null ? " " + ctx.loc.code : ""} — scans move stock here
                  <button className="rowx" onClick={clearContext} title="Disarm (Esc)" aria-label="Disarm"><Icon name="close" size={11} /></button>
                </span>
              )}
              {ctx.mode === "project" && (
                <span className="inv-badge" style={{ color: "var(--violet)", borderColor: "var(--violet)", display: "inline-flex", alignItems: "center", gap: 6, padding: "7px 10px", fontSize: 12 }}>
                  ◉ {ctx.proj.name} — scans commit to this project
                  <button className="rowx" onClick={clearContext} title="Disarm (Esc)" aria-label="Disarm"><Icon name="close" size={11} /></button>
                </span>
              )}
            </div>

            <div style={{ display: "grid", gridTemplateColumns: "minmax(0, 1fr) minmax(240px, 340px)", gap: 16, alignItems: "start" }}>
              <div>
                {!card && (
                  <div className="empty">
                    <div style={{ fontWeight: 600, marginBottom: 6 }}>Ready to scan.</div>
                    <div className="muted" style={{ fontSize: 12.5, lineHeight: 1.6 }}>
                      Scan a <b>unit</b> to see its card · a <b>bin</b> to arm it (then every unit scanned moves there) ·
                      a <b>project</b> to arm commits · an <b>unknown code</b> to receive new stock. Esc disarms.
                    </div>
                  </div>
                )}
                {card && card.kind === "unit" && (
                  <UnitCard unit={card.unit} locs={refs.locations}
                    onClose={function () { setCard(null); }}
                    onPrint={setPrintFor}
                    onMove={setMoveFor} />
                )}
                {card && card.kind === "location" && (
                  <LocationCard data={card.data} barcode={card.barcode}
                    armed={ctx.mode === "location" && ctx.loc.id === card.data.location.id}
                    onArm={function () { setCtx({ mode: "location", loc: card.data.location }); }}
                    onClose={function () { setCard(null); }}
                    onPrint={setPrintFor} />
                )}
                {card && card.kind === "project" && (
                  <ProjectCard data={card.data} locs={refs.locations}
                    armed={ctx.mode === "project" && ctx.proj.id === card.data.project.id}
                    onArm={function () { setCtx({ mode: "project", proj: card.data.project }); }}
                    onClose={function () { setCard(null); }}
                    onPrintTray={function () { printTray(card.data); }} />
                )}
                {card && card.kind === "port" && (
                  <PortCard port={card.port} onClose={function () { setCard(null); }} />
                )}
              </div>
              <RecentScans rows={hist} onRefresh={loadHistory} />
            </div>
          </div>
        )}

        {!offline && tab === "generate" && <GenerateTab refs={refs} onPrint={setPrintFor} />}

        {receive && (
          <ReceiveModal code={receive.code} guessedType={receive.guessedType} refs={refs}
            armedLocId={ctx.mode === "location" ? ctx.loc.id : null}
            onAddModel={function (m) { setRefs(function (r) { return Object.assign({}, r, { models: r.models.concat([m]) }); }); }}
            onClose={function () { setReceive(null); }}
            onDone={onReceived} />
        )}
        {moveFor && (
          <MoveModal unit={moveFor} refs={refs}
            onClose={function () { setMoveFor(null); }}
            onMoved={onMoved} />
        )}
        {printFor && (
          <PrintModal code={printFor}
            onClose={function () { setPrintFor(null); }}
            onPrinted={onPrinted} />
        )}
      </div>
    );
  }

  Object.assign(window, { CodesScreen });
})();
