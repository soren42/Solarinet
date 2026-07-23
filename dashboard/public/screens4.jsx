/* ============================================================
   SolariNet — screens4: Inventory management
   Components · Locations · Receipts · Projects  (SoR migration 014)

   Reads/writes the inventory subsystem: hardware_units (components),
   hardware_models (catalog), locations (drawer/tray/bin hierarchy),
   receipts (purchase record + scan), projects + project_allocations
   (reserve owned units — no double-commit — and list needed-to-buy items).

   Offline-first: mutates a local working copy so the whole screen is
   interactive against the fixture; when the live API is present it also
   persists through the adapter and re-reads. The double-commit guard
   (DB trigger → SQLSTATE 45000 → HTTP 409) is enforced client-side too
   so the 409 UX is demonstrable offline.
   ============================================================ */
(function () {
  const { useState, useEffect } = React;
  const Icon = window.Icon;
  const { DangerModal } = window;
  const S = window.SOLARI;
  const fmt = S.fmt;

  function toast(msg, icon) { if (window.__solariToast) window.__solariToast(msg, icon); }
  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function refresh() { const a = api(); if (a && a.refresh) a.refresh().catch(function () {}); }
  function isOffline() { return !window.SOLARI || window.SOLARI.source === "offline"; }
  function is409(e) {
    if (!e) return false;
    const c = String(e.code || "");
    const m = String(e.message || "").toLowerCase();
    return c === "HTTP_409" || c === "already_committed" || m.indexOf("already committed") >= 0 || m.indexOf("another project") >= 0;
  }

  // ---- enum → tone maps (badges) ----
  const STOCK_TONE = { in_stock: "var(--teal)", allocated: "var(--violet)", deployed: "var(--ok)", consumed: "var(--ink-faint)", retired: "var(--ink-faint)" };
  const STOCK_LABEL = { in_stock: "in stock", allocated: "allocated", deployed: "deployed", consumed: "consumed", retired: "retired" };
  const ALLOC_TONE = { needed: "var(--warn)", committed: "var(--teal)", installed: "var(--ok)", returned: "var(--ink-faint)" };
  const PROJ_TONE = { planning: "var(--ink-dim)", active: "var(--ok)", staged: "var(--violet)", on_hold: "var(--warn)", complete: "var(--teal)", archived: "var(--ink-faint)" };
  const HW_TYPES = ["server", "workstation", "sbc", "nas", "router", "switch", "ap", "printer", "scanner", "iot", "appliance", "console", "media", "vehicle", "tablet", "phone", "kvm", "sensor", "camera", "ups", "moca", "other"];
  const LOC_TYPES = ["site", "building", "floor", "room", "rack", "shelf", "position", "zone", "cabinet", "drawer", "tray", "bin", "container"];
  const LOC_ICON = { room: "location", rack: "server", shelf: "arch", cabinet: "box", drawer: "box", tray: "inventory", bin: "box", container: "box" };
  const PROJ_STATUS = ["planning", "active", "staged", "on_hold", "complete", "archived"];
  const STOCK_STATES = ["in_stock", "allocated", "deployed", "consumed", "retired"];

  function Badge({ tone, children, title }) {
    return <span className="inv-badge" title={title} style={{ color: tone, borderColor: tone }}>{children}</span>;
  }

  // shared form styles (mirror AdoptModal)
  const FLD = { width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 4, borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };
  const LBL = { fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, marginTop: 14, display: "block" };

  function nextId(list) { return list.reduce(function (m, x) { return Math.max(m, x.id || 0); }, 0) + 1; }
  function cloneInv(src) {
    src = src || {};
    return {
      models: (src.models || []).slice(),
      units: (src.units || []).slice(),
      locations: (src.locations || []).slice(),
      receipts: (src.receipts || []).slice(),
      projects: (src.projects || []).slice(),
      allocations: (src.allocations || []).slice(),
      seq: Object.assign({}, src.seq),
    };
  }

  /* ===================== generic modal shell ===================== */
  function Modal({ title, icon, sub, onClose, children, footer, wide }) {
    useEffect(function () {
      function onKey(e) { if (e.key === "Escape") onClose(); }
      window.addEventListener("keydown", onKey);
      return function () { window.removeEventListener("keydown", onKey); };
    }, [onClose]);
    return (
      <div onClick={onClose} style={{ position: "fixed", inset: 0, zIndex: 1000, background: "rgba(0,0,0,0.55)", display: "flex", alignItems: "center", justifyContent: "center", padding: 20 }}>
        <div onClick={function (e) { e.stopPropagation(); }} style={{ width: wide ? 640 : 480, maxWidth: "94vw", maxHeight: "88vh", overflowY: "auto", background: "var(--panel, #0c1118)", border: "1px solid var(--line-glow, rgba(255,255,255,0.12))", borderRadius: 14, padding: 22, boxShadow: "0 16px 56px rgba(0,0,0,0.5)" }}>
          <div style={{ display: "flex", alignItems: "center", gap: 10, marginBottom: sub ? 2 : 12 }}>
            <Icon name={icon || "box"} size={22} style={{ color: "var(--teal)" }} />
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

  /* ===================== root screen ===================== */
  function Inventory() {
    const [tab, setTab] = useState("components");
    const [inv, setInv] = useState(function () { return cloneInv(S.inventory); });
    const [viewer, setViewer] = useState(null);   // receipt being viewed (shared across tabs)

    // Live re-read on mount (offline keeps the fixture working copy).
    useEffect(function () {
      const a = api();
      if (a && a.inventory && !isOffline()) {
        a.inventory().then(function (d) {
          setInv({
            models: d.models || [], units: d.units || [], locations: d.locations || [],
            receipts: d.receipts || [], projects: d.projects || [], allocations: d.allocations || [],
            seq: (S.inventory && S.inventory.seq) || {},
          });
        }).catch(function () {});
      }
    }, []);

    // ----- lookups (recomputed each render; datasets are small) -----
    const locById = {}; inv.locations.forEach(function (l) { locById[l.id] = l; });
    const modelById = {}; inv.models.forEach(function (m) { modelById[m.id] = m; });
    const receiptById = {}; inv.receipts.forEach(function (r) { receiptById[r.id] = r; });
    const projectById = {}; inv.projects.forEach(function (p) { projectById[p.id] = p; });
    function locLabel(id) { const l = locById[id]; return l ? (l.name + (l.code ? " (" + l.code + ")" : "")) : "—"; }
    function locPath(id) { const parts = []; let l = locById[id], g = 0; while (l && g++ < 16) { parts.unshift(l.name + (l.code ? " " + l.code : "")); l = l.parent_id ? locById[l.parent_id] : null; } return parts.join(" › "); }
    function modelLabel(id) { const m = modelById[id]; return m ? (m.make + " " + m.model) : "—"; }
    function unitCommitment(unitId) { return inv.allocations.find(function (a) { return a.hardware_unit_id === unitId && (a.allocation === "committed" || a.allocation === "installed") && !a.deleted; }); }

    // ----- mutations (optimistic local + best-effort live persist) -----
    function persist(kind, editing, id, body, okMsg) {
      const a = api();
      if (isOffline() || !a) { toast(okMsg + " (offline)", "check"); return; }
      const fn = {
        unit: editing ? function () { return a.updateUnit(id, body); } : function () { return a.createUnit(body); },
        model: function () { return a.createModel(body); },
        location: function () { return a.createLocation(body); },
        project: editing ? function () { return a.updateProject(id, body); } : function () { return a.createProject(body); },
        receipt: function () { return a.createReceipt(body); },
      }[kind];
      if (!fn) { toast(okMsg + " (offline)", "check"); return; }
      fn().then(function () { toast(okMsg, "check"); refresh(); })
        .catch(function (e) { toast("Save failed: " + (e && e.message || "error"), "close"); });
    }

    function saveUnit(draft, editing) {
      setInv(function (prev) {
        const units = editing
          ? prev.units.map(function (u) { return u.id === draft.id ? Object.assign({}, u, draft) : u; })
          : prev.units.concat([Object.assign({ id: nextId(prev.units) }, draft)]);
        return Object.assign({}, prev, { units: units });
      });
      persist("unit", editing, draft.id, draft, editing ? "Component updated" : "Component added");
    }
    function saveModel(draft) {
      let created;
      setInv(function (prev) {
        created = Object.assign({ id: nextId(prev.models) }, draft);
        return Object.assign({}, prev, { models: prev.models.concat([created]) });
      });
      persist("model", false, null, draft, "Model added: " + draft.make + " " + draft.model);
      return created ? created.id : null;
    }
    function saveLocation(draft) {
      setInv(function (prev) { return Object.assign({}, prev, { locations: prev.locations.concat([Object.assign({ id: nextId(prev.locations) }, draft)]) }); });
      persist("location", false, null, draft, "Location added: " + draft.name);
    }
    function saveReceipt(draft) {
      setInv(function (prev) { return Object.assign({}, prev, { receipts: prev.receipts.concat([Object.assign({ id: nextId(prev.receipts) }, draft)]) }); });
      const a = api();
      if (!isOffline() && a && a.uploadReceipt && draft._file) {
        const form = new FormData();
        form.append("vendor", draft.vendor || "");
        form.append("purchased_on", draft.purchased_on || "");
        form.append("subtotal_cents", String(draft.subtotal_cents || 0));
        form.append("currency", draft.currency || "USD");
        form.append("order_ref", draft.order_ref || "");
        form.append("notes", draft.notes || "");
        form.append("scan", draft._file);
        a.uploadReceipt(form).then(function () { toast("Receipt uploaded", "check"); refresh(); })
          .catch(function (e) { toast("Upload failed: " + (e && e.message || "error"), "close"); });
      } else {
        toast("Receipt saved" + (isOffline() ? " (offline)" : ""), "check");
      }
    }
    function saveProject(draft, editing) {
      setInv(function (prev) {
        const projects = editing
          ? prev.projects.map(function (p) { return p.id === draft.id ? Object.assign({}, p, draft) : p; })
          : prev.projects.concat([Object.assign({ id: nextId(prev.projects) }, draft)]);
        return Object.assign({}, prev, { projects: projects });
      });
      persist("project", editing, draft.id, draft, editing ? "Project updated" : "Project created");
    }

    // Commit (or install) an OWNED unit to a project. Guards double-commit → 409.
    function commitUnit(project, unitId, mode) {
      const existing = unitCommitment(unitId);
      if (existing && existing.project_id !== project.id) {
        const other = projectById[existing.project_id];
        toast("409 · unit already committed to " + (other ? other.name : "another project"), "close");
        return Promise.reject(new Error("already committed to another project"));
      }
      setInv(function (prev) {
        const alloc = { id: nextId(prev.allocations), project_id: project.id, hardware_unit_id: unitId, hardware_model_id: null, allocation: mode, quantity: 1, notes: "" };
        const units = prev.units.map(function (u) {
          return u.id === unitId ? Object.assign({}, u, { location_id: project.tray_location_id || u.location_id, stock_status: mode === "installed" ? "deployed" : "allocated" }) : u;
        });
        return Object.assign({}, prev, { allocations: prev.allocations.concat([alloc]), units: units });
      });
      const a = api();
      if (!isOffline() && a && a.createAllocation) {
        return a.createAllocation({ projectId: project.id, hardwareUnitId: unitId, allocation: mode })
          .then(function () { toast((mode === "installed" ? "Installed" : "Committed") + " to " + project.name, "check"); refresh(); })
          .catch(function (e) {
            if (is409(e)) toast("409 · unit already committed to another project", "close");
            else toast("Commit failed: " + (e && e.message || "error"), "close");
            if (a.inventory) a.inventory().then(setInv).catch(function () {});
          });
      }
      toast((mode === "installed" ? "Installed" : "Committed") + " to " + project.name + " (offline)", "check");
      return Promise.resolve();
    }

    // Add a NEEDED model (feeds the shopping list).
    function addNeeded(project, modelId, qty) {
      setInv(function (prev) {
        const alloc = { id: nextId(prev.allocations), project_id: project.id, hardware_unit_id: null, hardware_model_id: modelId, allocation: "needed", quantity: qty || 1, notes: "" };
        return Object.assign({}, prev, { allocations: prev.allocations.concat([alloc]) });
      });
      const a = api();
      if (!isOffline() && a && a.createAllocation) {
        a.createAllocation({ projectId: project.id, hardwareModelId: modelId, allocation: "needed", quantity: qty || 1 })
          .then(function () { toast("Added to needs: " + modelLabel(modelId), "check"); refresh(); })
          .catch(function (e) { toast("Add failed: " + (e && e.message || "error"), "close"); });
      } else {
        toast("Added to needs: " + modelLabel(modelId) + " (offline)", "check");
      }
    }

    // Advance / return an allocation.
    function setAllocation(allocId, mode) {
      setInv(function (prev) {
        const al = prev.allocations.find(function (a) { return a.id === allocId; });
        const allocations = prev.allocations.map(function (a) { return a.id === allocId ? Object.assign({}, a, { allocation: mode }) : a; });
        let units = prev.units;
        if (al && al.hardware_unit_id) {
          const st = mode === "installed" ? "deployed" : mode === "committed" ? "allocated" : mode === "returned" ? "in_stock" : null;
          if (st) units = prev.units.map(function (u) { return u.id === al.hardware_unit_id ? Object.assign({}, u, { stock_status: st }) : u; });
        }
        return Object.assign({}, prev, { allocations: allocations, units: units });
      });
      const a = api();
      if (!isOffline() && a && a.updateAllocation) {
        a.updateAllocation(allocId, { allocation: mode }).then(function () { refresh(); })
          .catch(function (e) { toast("Update failed: " + (e && e.message || "error"), "close"); });
      }
    }
    function removeAllocation(allocId) {
      setInv(function (prev) { return Object.assign({}, prev, { allocations: prev.allocations.filter(function (a) { return a.id !== allocId; }) }); });
      const a = api();
      if (!isOffline() && a && a.updateAllocation) a.updateAllocation(allocId, { deleted: true }).then(refresh).catch(function () {});
    }

    const helpers = { locById, modelById, receiptById, projectById, locLabel, locPath, modelLabel, unitCommitment };
    const actions = { saveUnit, saveModel, saveLocation, saveReceipt, saveProject, commitUnit, addNeeded, setAllocation, removeAllocation };

    const TABS = [
      ["components", "Components", "box"],
      ["locations", "Locations", "location"],
      ["receipts", "Receipts", "receipt"],
      ["projects", "Projects", "project"],
    ];

    return (
      <div className="page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Inventory</h1>
            <div className="page-sub">{inv.units.length} components · {inv.locations.length} locations · {inv.receipts.length} receipts · {inv.projects.length} projects</div>
          </div>
          <div className="page-head__right">
            <div className="seg">
              {TABS.map(function (t) {
                return <button key={t[0]} className={tab === t[0] ? "on" : ""} onClick={function () { setTab(t[0]); }}><Icon name={t[2]} size={14} />{t[1]}</button>;
              })}
            </div>
          </div>
        </div>

        {tab === "components" && <ComponentsTab inv={inv} h={helpers} act={actions} onViewReceipt={setViewer} />}
        {tab === "locations" && <LocationsTab inv={inv} h={helpers} act={actions} />}
        {tab === "receipts" && <ReceiptsTab inv={inv} h={helpers} act={actions} onView={setViewer} />}
        {tab === "projects" && <ProjectsTab inv={inv} h={helpers} act={actions} />}

        {viewer && <ReceiptViewer receipt={viewer} inv={inv} h={helpers} onClose={function () { setViewer(null); }} />}
      </div>
    );
  }

  /* ===================== COMPONENTS TAB ===================== */
  function ComponentsTab({ inv, h, act, onViewReceipt }) {
    const [q, setQ] = useState("");
    const [stockFilter, setStockFilter] = useState("all");
    const [sort, setSort] = useState({ key: "model", dir: 1 });
    const [edit, setEdit] = useState(null);   // { unit } editing, or { unit:null } new

    const value = inv.units.reduce(function (a, u) { return a + (u.unit_cost_cents || 0) * (u.quantity || 1); }, 0);
    const inStock = inv.units.filter(function (u) { return u.stock_status === "in_stock"; }).length;
    const committed = inv.units.filter(function (u) { return u.stock_status === "allocated" || u.stock_status === "deployed"; }).length;

    const ql = q.trim().toLowerCase();
    let rows = inv.units.filter(function (u) {
      if (stockFilter !== "all" && u.stock_status !== stockFilter) return false;
      if (!ql) return true;
      const hay = [h.modelLabel(u.model_id), u.serial, u.asset_tag, h.locLabel(u.location_id)].join(" ").toLowerCase();
      return hay.indexOf(ql) >= 0;
    });
    function keyOf(u) {
      switch (sort.key) {
        case "model": return h.modelLabel(u.model_id).toLowerCase();
        case "serial": return (u.serial || "").toLowerCase();
        case "asset": return (u.asset_tag || "").toLowerCase();
        case "location": return h.locLabel(u.location_id).toLowerCase();
        case "qty": return u.quantity || 0;
        case "stock": return u.stock_status;
        case "cost": return u.unit_cost_cents || 0;
        default: return 0;
      }
    }
    rows = rows.slice().sort(function (a, b) { const ka = keyOf(a), kb = keyOf(b); if (ka < kb) return -sort.dir; if (ka > kb) return sort.dir; return 0; });
    function th(key, label, right) {
      const on = sort.key === key;
      return <th style={{ cursor: "pointer", textAlign: right ? "right" : undefined }} onClick={function () { setSort(function (s) { return { key: key, dir: s.key === key ? -s.dir : 1 }; }); }}>
        {label}{on ? <span style={{ color: "var(--teal)", marginLeft: 4 }}>{sort.dir > 0 ? "▲" : "▼"}</span> : null}
      </th>;
    }

    return (
      <div>
        <div className="kpis">
          <div className="kpi teal"><div className="kpi__k">Components</div><div className="kpi__v">{inv.units.length}</div><div className="kpi__sub">tracked units</div><div className="kpi__bar" /></div>
          <div className="kpi ok"><div className="kpi__k">In stock</div><div className="kpi__v">{inStock}</div><div className="kpi__sub">available</div><div className="kpi__bar" /></div>
          <div className="kpi violet"><div className="kpi__k">In use</div><div className="kpi__v">{committed}</div><div className="kpi__sub">allocated / deployed</div><div className="kpi__bar" /></div>
          <div className="kpi"><div className="kpi__k">Inventory value</div><div className="kpi__v" style={{ fontSize: 22 }}>{fmt.usd(value)}</div><div className="kpi__sub">at unit cost</div><div className="kpi__bar" /></div>
        </div>

        <div className="filters">
          {["all"].concat(STOCK_STATES).map(function (s) {
            const n = s === "all" ? inv.units.length : inv.units.filter(function (u) { return u.stock_status === s; }).length;
            return <button key={s} className={"chip" + (stockFilter === s ? " on" : "")} onClick={function () { setStockFilter(s); }}>
              {s !== "all" && <span className="dot" style={{ background: STOCK_TONE[s] }} />}{s === "all" ? "All" : STOCK_LABEL[s]}<span className="chip__n">{n}</span>
            </button>;
          })}
          <div className="search" style={{ marginLeft: "auto", maxWidth: 240, height: 36 }}><Icon name="search" size={15} /><input value={q} onChange={function (e) { setQ(e.target.value); }} placeholder="Find component…" /></div>
          <button className="btn-primary" onClick={function () { setEdit({ unit: null }); }}><Icon name="plus" size={14} />New component</button>
        </div>

        {rows.length === 0 && <div className="empty">No components match.</div>}
        {rows.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr>
              {th("model", "Model")}{th("serial", "Serial")}{th("asset", "Asset tag")}{th("location", "Location")}{th("qty", "Qty", true)}{th("stock", "Stock")}{th("cost", "Cost", true)}<th>Receipt</th>
            </tr></thead>
            <tbody>
              {rows.map(function (u) {
                const m = h.modelById[u.model_id];
                const r = u.receipt_id ? h.receiptById[u.receipt_id] : null;
                return (
                  <tr key={u.id} style={{ cursor: "pointer" }} onClick={function () { setEdit({ unit: u }); }}>
                    <td><div style={{ fontWeight: 600 }}>{h.modelLabel(u.model_id)}</div>{m && <div className="td-mono muted" style={{ fontSize: 10.5 }}>{m.hw_type}</div>}</td>
                    <td className="td-mono">{u.serial || "—"}</td>
                    <td className="td-mono muted">{u.asset_tag || "—"}</td>
                    <td title={h.locPath(u.location_id)}>{h.locLabel(u.location_id)}</td>
                    <td className="td-mono" style={{ textAlign: "right" }}>{u.quantity || 1}</td>
                    <td><Badge tone={STOCK_TONE[u.stock_status]}>{STOCK_LABEL[u.stock_status]}</Badge></td>
                    <td className="td-mono" style={{ textAlign: "right" }}>{fmt.usd(u.unit_cost_cents)}</td>
                    <td>{r
                      ? <button className="inv-link" onClick={function (e) { e.stopPropagation(); onViewReceipt(r); }}><Icon name="receipt" size={12} style={{ verticalAlign: "-2px", marginRight: 3 }} />{r.vendor || ("#" + r.id)}</button>
                      : <span className="muted">—</span>}</td>
                  </tr>
                );
              })}
            </tbody>
          </table></div>
        )}

        {edit && <UnitModal unit={edit.unit} inv={inv} h={h} act={act} onClose={function () { setEdit(null); }} />}
      </div>
    );
  }

  function UnitModal({ unit, inv, h, act, onClose }) {
    const editing = !!unit;
    const [modelId, setModelId] = useState(unit ? unit.model_id : (inv.models[0] ? inv.models[0].id : 0));
    const [serial, setSerial] = useState(unit ? (unit.serial || "") : "");
    const [assetTag, setAssetTag] = useState(unit ? (unit.asset_tag || "") : "");
    const [locId, setLocId] = useState(unit ? (unit.location_id || "") : "");
    const [acquired, setAcquired] = useState(unit ? (unit.acquired_on || "") : "");
    const [qty, setQty] = useState(unit ? (unit.quantity || 1) : 1);
    const [cost, setCost] = useState(unit && unit.unit_cost_cents != null ? (unit.unit_cost_cents / 100).toFixed(2) : "");
    const [stock, setStock] = useState(unit ? unit.stock_status : "in_stock");
    const [receiptId, setReceiptId] = useState(unit ? (unit.receipt_id || "") : "");
    const [notes, setNotes] = useState(unit ? (unit.notes || "") : "");
    const [addModel, setAddModel] = useState(false);

    function submit() {
      if (!modelId) { toast("Pick a model", "close"); return; }
      const draft = {
        id: unit ? unit.id : undefined,
        model_id: Number(modelId), serial: serial.trim() || null, asset_tag: assetTag.trim() || null,
        location_id: locId ? Number(locId) : null, acquired_on: acquired || null,
        quantity: Number(qty) || 1, unit_cost_cents: cost === "" ? null : Math.round(parseFloat(cost) * 100),
        stock_status: stock, receipt_id: receiptId ? Number(receiptId) : null, notes: notes.trim() || "",
      };
      act.saveUnit(draft, editing);
      onClose();
    }

    return (
      <Modal title={editing ? "Edit component" : "New component"} icon="box" wide
        sub={editing ? h.modelLabel(unit.model_id) + (unit.serial ? " · " + unit.serial : "") : "Add a hardware unit to inventory"}
        onClose={onClose}
        footer={<>
          <button className="btn-ghost" onClick={onClose}>Cancel</button>
          <button className="btn-primary" onClick={submit}><Icon name="check" size={14} />{editing ? "Save changes" : "Add component"}</button>
        </>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}>
            <label style={LBL}>Model</label>
            <div style={{ display: "flex", gap: 8 }}>
              <select style={FLD} value={String(modelId)} onChange={function (e) { setModelId(e.target.value); }}>
                {inv.models.map(function (m) { return <option key={m.id} value={String(m.id)}>{m.make + " " + m.model} · {m.hw_type}</option>; })}
              </select>
              <button className="btn-ghost" style={{ marginTop: 4, flex: "0 0 auto" }} onClick={function () { setAddModel(true); }} title="Add a new model"><Icon name="plus" size={13} /></button>
            </div>
          </div>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Stock status</label>
            <select style={FLD} value={stock} onChange={function (e) { setStock(e.target.value); }}>
              {STOCK_STATES.map(function (s) { return <option key={s} value={s}>{STOCK_LABEL[s]}</option>; })}
            </select>
          </div>
        </div>

        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Serial</label><input style={FLD} value={serial} onChange={function (e) { setSerial(e.target.value); }} placeholder="manufacturer serial" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Asset tag</label><input style={FLD} value={assetTag} onChange={function (e) { setAssetTag(e.target.value); }} placeholder="internal tag" /></div>
        </div>

        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}>
            <label style={LBL}>Location</label>
            <select style={FLD} value={String(locId)} onChange={function (e) { setLocId(e.target.value); }}>
              <option value="">— unassigned —</option>
              {inv.locations.map(function (l) { return <option key={l.id} value={String(l.id)}>{h.locPath(l.id)}</option>; })}
            </select>
          </div>
          <div style={{ flex: 1 }}><label style={LBL}>Quantity</label><input type="number" min="1" style={FLD} value={qty} onChange={function (e) { setQty(e.target.value); }} /></div>
        </div>

        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Acquired on</label><input type="date" style={FLD} value={acquired || ""} onChange={function (e) { setAcquired(e.target.value); }} /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Unit cost (USD)</label><input type="number" min="0" step="0.01" style={FLD} value={cost} onChange={function (e) { setCost(e.target.value); }} placeholder="0.00" /></div>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Receipt</label>
            <select style={FLD} value={String(receiptId)} onChange={function (e) { setReceiptId(e.target.value); }}>
              <option value="">— none —</option>
              {inv.receipts.map(function (r) { return <option key={r.id} value={String(r.id)}>{(r.vendor || ("#" + r.id)) + (r.purchased_on ? " · " + r.purchased_on : "")}</option>; })}
            </select>
          </div>
        </div>

        <label style={LBL}>Notes</label>
        <input style={FLD} value={notes} onChange={function (e) { setNotes(e.target.value); }} placeholder="warranty, condition…" />

        {addModel && <ModelModal act={act} onClose={function () { setAddModel(false); }} onCreated={function (id) { setModelId(id); setAddModel(false); }} />}
      </Modal>
    );
  }

  function ModelModal({ act, onClose, onCreated }) {
    const [make, setMake] = useState("");
    const [model, setModel] = useState("");
    const [hwType, setHwType] = useState("other");
    const [notes, setNotes] = useState("");
    function submit() {
      if (!make.trim() || !model.trim()) { toast("Make and model are required", "close"); return; }
      const id = act.saveModel({ make: make.trim(), model: model.trim(), hw_type: hwType, notes: notes.trim() || "" });
      onCreated && onCreated(id);
    }
    return (
      <Modal title="New model" icon="tag2" sub="Add a make/model to the catalog" onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Add model</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Make</label><input autoFocus style={FLD} value={make} onChange={function (e) { setMake(e.target.value); }} placeholder="Dell, Ubiquiti…" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Model</label><input style={FLD} value={model} onChange={function (e) { setModel(e.target.value); }} placeholder="R410, U7 Pro…" /></div>
        </div>
        <label style={LBL}>Type</label>
        <select style={FLD} value={hwType} onChange={function (e) { setHwType(e.target.value); }}>{HW_TYPES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}</select>
        <label style={LBL}>Notes</label>
        <input style={FLD} value={notes} onChange={function (e) { setNotes(e.target.value); }} placeholder="spec links, quirks…" />
      </Modal>
    );
  }

  /* ===================== LOCATIONS TAB ===================== */
  function LocationsTab({ inv, h, act }) {
    const [add, setAdd] = useState(false);
    const childrenOf = {};
    inv.locations.forEach(function (l) { const k = l.parent_id || 0; (childrenOf[k] = childrenOf[k] || []).push(l); });
    function unitCount(id) { return inv.units.filter(function (u) { return u.location_id === id; }).length; }

    function Node({ loc, depth }) {
      const kids = childrenOf[loc.id] || [];
      const n = unitCount(loc.id);
      return (
        <>
          <div className="inv-tree-row" style={{ paddingLeft: 10 + depth * 22 }}>
            <Icon name={LOC_ICON[loc.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
            <span style={{ fontWeight: 600 }}>{loc.name}</span>
            {loc.code != null && <span className="inv-code">{loc.code}</span>}
            <span className="tag" style={{ marginLeft: 2 }}>{loc.loc_type}</span>
            <span style={{ flex: 1 }} />
            {n > 0 && <span className="td-mono muted" style={{ fontSize: 11 }}>{n} unit{n === 1 ? "" : "s"}</span>}
          </div>
          {kids.map(function (c) { return <Node key={c.id} loc={c} depth={depth + 1} />; })}
        </>
      );
    }
    const roots = childrenOf[0] || [];

    return (
      <div>
        <div className="filters">
          <div className="page-sub" style={{ margin: 0 }}>Storage hierarchy — rooms · racks · cabinets · drawers · trays · bins</div>
          <button className="btn-primary" style={{ marginLeft: "auto" }} onClick={function () { setAdd(true); }}><Icon name="plus" size={14} />New location</button>
        </div>
        <div className="panel"><div className="panel__body" style={{ padding: "8px 6px" }}>
          {roots.length === 0 && <div className="empty">No locations yet.</div>}
          {roots.map(function (r) { return <Node key={r.id} loc={r} depth={0} />; })}
        </div></div>
        {add && <LocationModal inv={inv} h={h} act={act} onClose={function () { setAdd(false); }} />}
      </div>
    );
  }

  function LocationModal({ inv, h, act, onClose }) {
    const [name, setName] = useState("");
    const [code, setCode] = useState("");
    const [locType, setLocType] = useState("drawer");
    const [parent, setParent] = useState("");
    function submit() {
      if (!name.trim()) { toast("Name is required", "close"); return; }
      act.saveLocation({ name: name.trim(), code: code.trim() || null, loc_type: locType, parent_id: parent ? Number(parent) : null });
      onClose();
    }
    return (
      <Modal title="New location" icon="location" sub="Add a room, rack, drawer, tray, or bin" onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Add location</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}><label style={LBL}>Name</label><input autoFocus style={FLD} value={name} onChange={function (e) { setName(e.target.value); }} placeholder="Tray 3" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Code / number</label><input style={FLD} value={code} onChange={function (e) { setCode(e.target.value); }} placeholder="3" /></div>
        </div>
        <label style={LBL}>Type</label>
        <select style={FLD} value={locType} onChange={function (e) { setLocType(e.target.value); }}>{LOC_TYPES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}</select>
        <label style={LBL}>Inside (parent)</label>
        <select style={FLD} value={String(parent)} onChange={function (e) { setParent(e.target.value); }}>
          <option value="">— top level —</option>
          {inv.locations.map(function (l) { return <option key={l.id} value={String(l.id)}>{h.locPath(l.id)}</option>; })}
        </select>
      </Modal>
    );
  }

  /* ===================== RECEIPTS TAB ===================== */
  function ReceiptsTab({ inv, h, act, onView }) {
    const [add, setAdd] = useState(false);
    const total = inv.receipts.reduce(function (a, r) { return a + (r.subtotal_cents || 0); }, 0);
    function itemCount(id) { return inv.units.filter(function (u) { return u.receipt_id === id; }).length; }
    const sorted = inv.receipts.slice().sort(function (a, b) { return String(b.purchased_on || "").localeCompare(String(a.purchased_on || "")); });

    return (
      <div>
        <div className="kpis">
          <div className="kpi teal"><div className="kpi__k">Receipts</div><div className="kpi__v">{inv.receipts.length}</div><div className="kpi__sub">purchase records</div><div className="kpi__bar" /></div>
          <div className="kpi"><div className="kpi__k">Total spend</div><div className="kpi__v" style={{ fontSize: 22 }}>{fmt.usd(total)}</div><div className="kpi__sub">across receipts</div><div className="kpi__bar" /></div>
        </div>
        <div className="filters">
          <div className="page-sub" style={{ margin: 0 }}>Purchase receipts & scans</div>
          <button className="btn-primary" style={{ marginLeft: "auto" }} onClick={function () { setAdd(true); }}><Icon name="plus" size={14} />Upload receipt</button>
        </div>
        {sorted.length === 0 && <div className="empty">No receipts yet.</div>}
        <div className="cards">
          {sorted.map(function (r) {
            const isImg = r.file_mime && r.file_mime.indexOf("image/") === 0 && r.scan;
            const items = itemCount(r.id);
            return (
              <div key={r.id} className="inv-receipt" onClick={function () { onView(r); }}>
                <div className="inv-receipt__thumb">
                  {isImg
                    ? <img src={r.scan} alt={r.vendor || "receipt"} />
                    : <div className="inv-receipt__pdf"><Icon name="receipt" size={30} /><span>{(r.file_mime || "file").split("/")[1] || "file"}</span></div>}
                </div>
                <div className="inv-receipt__body">
                  <div style={{ display: "flex", alignItems: "baseline", gap: 8 }}>
                    <div style={{ fontWeight: 700, fontSize: 14, flex: 1, minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{r.vendor || ("Receipt #" + r.id)}</div>
                    <div className="td-mono" style={{ color: "var(--teal)", fontWeight: 600 }}>{fmt.usd(r.subtotal_cents)}</div>
                  </div>
                  <div className="td-mono muted" style={{ fontSize: 11, marginTop: 3 }}>{r.purchased_on || "—"}{r.order_ref ? " · " + r.order_ref : ""}</div>
                  <div className="td-mono muted" style={{ fontSize: 11, marginTop: 3 }}>{items} linked component{items === 1 ? "" : "s"}</div>
                </div>
              </div>
            );
          })}
        </div>
        {add && <ReceiptModal act={act} onClose={function () { setAdd(false); }} />}
      </div>
    );
  }

  function ReceiptModal({ act, onClose }) {
    const [vendor, setVendor] = useState("");
    const [date, setDate] = useState("");
    const [subtotal, setSubtotal] = useState("");
    const [orderRef, setOrderRef] = useState("");
    const [notes, setNotes] = useState("");
    const [file, setFile] = useState(null);       // { name, mime, dataUrl, raw }
    function onFile(e) {
      const f = e.target.files && e.target.files[0];
      if (!f) { setFile(null); return; }
      const reader = new FileReader();
      reader.onload = function () { setFile({ name: f.name, mime: f.type || "application/octet-stream", dataUrl: reader.result, raw: f }); };
      reader.readAsDataURL(f);
    }
    function submit() {
      const draft = {
        vendor: vendor.trim() || null, purchased_on: date || null,
        subtotal_cents: subtotal === "" ? null : Math.round(parseFloat(subtotal) * 100),
        currency: "USD", order_ref: orderRef.trim() || null,
        file_path: file ? ("/srv/solari/receipts/" + file.name) : null,
        file_mime: file ? file.mime : null,
        scan: file && file.mime.indexOf("image/") === 0 ? file.dataUrl : null,
        notes: notes.trim() || "", _file: file ? file.raw : null,
      };
      act.saveReceipt(draft);
      onClose();
    }
    return (
      <Modal title="Upload receipt" icon="receipt" sub="Record a purchase and attach its scan" onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Save receipt</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}><label style={LBL}>Vendor</label><input autoFocus style={FLD} value={vendor} onChange={function (e) { setVendor(e.target.value); }} placeholder="Newegg, B&H…" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Purchased on</label><input type="date" style={FLD} value={date} onChange={function (e) { setDate(e.target.value); }} /></div>
        </div>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Subtotal (USD)</label><input type="number" min="0" step="0.01" style={FLD} value={subtotal} onChange={function (e) { setSubtotal(e.target.value); }} placeholder="0.00" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Order ref</label><input style={FLD} value={orderRef} onChange={function (e) { setOrderRef(e.target.value); }} placeholder="order #" /></div>
        </div>
        <label style={LBL}>Scan (image or PDF)</label>
        <input type="file" accept="image/*,application/pdf" onChange={onFile} style={Object.assign({}, FLD, { padding: "7px 10px" })} />
        {file && <div className="td-mono muted" style={{ fontSize: 11, marginTop: 6 }}>{file.name} · {file.mime}</div>}
        {file && file.mime.indexOf("image/") === 0 && <img src={file.dataUrl} alt="preview" style={{ maxWidth: "100%", marginTop: 10, borderRadius: 8, border: "1px solid var(--line)" }} />}
        <label style={LBL}>Notes</label>
        <input style={FLD} value={notes} onChange={function (e) { setNotes(e.target.value); }} placeholder="what was on this order…" />
      </Modal>
    );
  }

  function ReceiptViewer({ receipt, inv, h, onClose }) {
    const isImg = receipt.file_mime && receipt.file_mime.indexOf("image/") === 0 && receipt.scan;
    const items = inv.units.filter(function (u) { return u.receipt_id === receipt.id; });
    return (
      <Modal title={receipt.vendor || ("Receipt #" + receipt.id)} icon="receipt" wide
        sub={(receipt.purchased_on || "—") + (receipt.order_ref ? " · " + receipt.order_ref : "") + " · " + fmt.usd(receipt.subtotal_cents)}
        onClose={onClose}
        footer={<button className="btn-ghost" onClick={onClose}>Close</button>}>
        <div style={{ display: "flex", justifyContent: "center", marginTop: 10 }}>
          {isImg
            ? <img src={receipt.scan} alt="receipt scan" style={{ maxWidth: "100%", maxHeight: "52vh", borderRadius: 8, border: "1px solid var(--line)" }} />
            : <div className="inv-receipt__pdf" style={{ width: 200, height: 240, borderRadius: 10 }}><Icon name="receipt" size={44} /><span>{receipt.file_mime || "no scan"}</span></div>}
        </div>
        {receipt.file_path && <div className="td-mono muted" style={{ fontSize: 11, marginTop: 10, textAlign: "center", wordBreak: "break-all" }}>{receipt.file_path}</div>}
        {receipt.notes && <div style={{ fontSize: 13, marginTop: 10 }}>{receipt.notes}</div>}
        <div className="page-sub" style={{ margin: "16px 0 8px" }}>Linked components ({items.length})</div>
        {items.length === 0 && <div className="muted" style={{ fontSize: 12 }}>No components reference this receipt.</div>}
        {items.map(function (u) {
          return <div key={u.id} className="inv-tree-row" style={{ paddingLeft: 10 }}>
            <Icon name="box" size={14} style={{ color: "var(--teal)" }} /><span style={{ fontWeight: 600 }}>{h.modelLabel(u.model_id)}</span>
            <span className="td-mono muted" style={{ fontSize: 11 }}>{u.serial || ""}</span><span style={{ flex: 1 }} />
            <span className="td-mono">{fmt.usd(u.unit_cost_cents)}</span>
          </div>;
        })}
      </Modal>
    );
  }

  /* ===================== PROJECTS TAB ===================== */
  function ProjectsTab({ inv, h, act }) {
    const [addProj, setAddProj] = useState(false);
    const [commitFor, setCommitFor] = useState(null);   // project to commit an owned unit to
    const [needFor, setNeedFor] = useState(null);       // project to add a needed model to

    if (inv.projects.length === 0) {
      return (
        <div>
          <div className="filters"><div className="page-sub" style={{ margin: 0 }}>Projects reserve inventory and stage a physical tray</div>
            <button className="btn-primary" style={{ marginLeft: "auto" }} onClick={function () { setAddProj(true); }}><Icon name="plus" size={14} />New project</button></div>
          <div className="empty">No projects yet. Create one to start reserving components.</div>
          {addProj && <ProjectModal inv={inv} h={h} act={act} onClose={function () { setAddProj(false); }} />}
        </div>
      );
    }

    return (
      <div>
        <div className="filters">
          <div className="page-sub" style={{ margin: 0 }}>Projects reserve inventory · commit owned units · list what to buy</div>
          <button className="btn-primary" style={{ marginLeft: "auto" }} onClick={function () { setAddProj(true); }}><Icon name="plus" size={14} />New project</button>
        </div>
        <div style={{ display: "flex", flexDirection: "column", gap: 18 }}>
          {inv.projects.map(function (p) {
            return <ProjectBoard key={p.id} project={p} inv={inv} h={h} act={act}
              onCommit={function () { setCommitFor(p); }} onNeed={function () { setNeedFor(p); }} />;
          })}
        </div>
        {addProj && <ProjectModal inv={inv} h={h} act={act} onClose={function () { setAddProj(false); }} />}
        {commitFor && <CommitModal project={commitFor} inv={inv} h={h} act={act} onClose={function () { setCommitFor(null); }} />}
        {needFor && <NeededModal project={needFor} inv={inv} act={act} onClose={function () { setNeedFor(null); }} />}
      </div>
    );
  }

  function ProjectBoard({ project, inv, h, act, onCommit, onNeed }) {
    const allocs = inv.allocations.filter(function (a) { return a.project_id === project.id; });
    const cols = {
      needed: allocs.filter(function (a) { return a.allocation === "needed"; }),
      committed: allocs.filter(function (a) { return a.allocation === "committed"; }),
      installed: allocs.filter(function (a) { return a.allocation === "installed"; }),
    };
    // shopping list = needed allocations with NO owned unit, grouped by model
    const shopMap = {};
    allocs.forEach(function (a) {
      if (a.allocation === "needed" && !a.hardware_unit_id && a.hardware_model_id) {
        const k = a.hardware_model_id;
        shopMap[k] = (shopMap[k] || 0) + (a.quantity || 1);
      }
    });
    const shopping = Object.keys(shopMap).map(function (k) { return { modelId: Number(k), qty: shopMap[k] }; });

    function AllocCard({ a }) {
      const unit = a.hardware_unit_id ? inv.units.find(function (u) { return u.id === a.hardware_unit_id; }) : null;
      const label = unit ? h.modelLabel(unit.model_id) : (a.hardware_model_id ? h.modelLabel(a.hardware_model_id) : "—");
      return (
        <div className="inv-alloc">
          <div style={{ display: "flex", alignItems: "center", gap: 8 }}>
            <Badge tone={ALLOC_TONE[a.allocation]}>{a.allocation}</Badge>
            <span style={{ fontWeight: 600, flex: 1, minWidth: 0, overflow: "hidden", textOverflow: "ellipsis", whiteSpace: "nowrap" }}>{label}</span>
            {a.allocation === "needed" && <button className="rowx" title="Remove" onClick={function () { act.removeAllocation(a.id); }}><Icon name="close" size={12} /></button>}
          </div>
          <div className="td-mono muted" style={{ fontSize: 11, marginTop: 3 }}>
            {unit ? (unit.serial || "no serial") : ("need ×" + (a.quantity || 1))}
            {a.notes ? " · " + a.notes : ""}
          </div>
          <div style={{ display: "flex", gap: 6, marginTop: 8 }}>
            {a.allocation === "committed" && <button className="inv-mini" onClick={function () { act.setAllocation(a.id, "installed"); }}>Mark installed</button>}
            {a.allocation === "installed" && <button className="inv-mini" onClick={function () { act.setAllocation(a.id, "committed"); }}>Un-install</button>}
            {(a.allocation === "committed" || a.allocation === "installed") && <button className="inv-mini" onClick={function () { act.setAllocation(a.id, "returned"); }}>Return</button>}
            {a.allocation === "returned" && <button className="inv-mini" onClick={function () { act.removeAllocation(a.id); }}>Clear</button>}
          </div>
        </div>
      );
    }

    return (
      <div className="panel">
        <div className="panel__head">
          <Icon name="project" size={16} />
          <h3>{project.name}</h3>
          {project.code && <span className="inv-code">{project.code}</span>}
          <Badge tone={PROJ_TONE[project.status]} >{project.status.replace("_", " ")}</Badge>
          <div className="right">
            {project.tray_location_id
              ? <span title={h.locPath(project.tray_location_id)}><Icon name="inventory" size={13} style={{ verticalAlign: "-2px", color: "var(--teal)", marginRight: 4 }} />{h.locLabel(project.tray_location_id)}</span>
              : <span className="muted">no tray</span>}
          </div>
        </div>
        <div className="panel__body">
          {project.description && <div className="muted" style={{ fontSize: 12.5, marginBottom: 12 }}>{project.description}</div>}
          <div style={{ display: "flex", gap: 8, marginBottom: 14, flexWrap: "wrap" }}>
            <button className="btn-primary" onClick={onCommit}><Icon name="plus" size={13} />Commit component</button>
            <button className="btn-ghost" onClick={onNeed}><Icon name="cart" size={13} style={{ marginRight: 4 }} />Add needed model</button>
            {project.target_on && <span className="td-mono muted" style={{ alignSelf: "center", fontSize: 11 }}>target {project.target_on}</span>}
            {project.owner && <span className="td-mono muted" style={{ alignSelf: "center", fontSize: 11 }}>· owner {project.owner}</span>}
          </div>

          <div className="inv-cols">
            {[["needed", "Needed"], ["committed", "Committed"], ["installed", "Installed"]].map(function (c) {
              return (
                <div key={c[0]} className="inv-col">
                  <div className="inv-col__head"><span className="dot" style={{ background: ALLOC_TONE[c[0]] }} />{c[1]}<span className="chip__n">{cols[c[0]].length}</span></div>
                  {cols[c[0]].length === 0 && <div className="muted" style={{ fontSize: 11.5, padding: "8px 2px" }}>—</div>}
                  {cols[c[0]].map(function (a) { return <AllocCard key={a.id} a={a} />; })}
                </div>
              );
            })}
          </div>

          <div className="inv-shop">
            <div className="inv-col__head" style={{ marginBottom: 8 }}><Icon name="cart" size={14} style={{ color: "var(--warn)" }} />Shopping list<span className="chip__n">{shopping.length}</span></div>
            {shopping.length === 0 && <div className="muted" style={{ fontSize: 12 }}>Nothing to buy — every needed item is owned or committed.</div>}
            {shopping.map(function (s) {
              const m = h.modelById[s.modelId];
              return (
                <div key={s.modelId} className="inv-shop__row">
                  <Icon name="box" size={14} style={{ color: "var(--warn)" }} />
                  <span style={{ fontWeight: 600 }}>{h.modelLabel(s.modelId)}</span>
                  {m && <span className="tag">{m.hw_type}</span>}
                  <span style={{ flex: 1 }} />
                  <span className="td-mono" style={{ color: "var(--warn)" }}>× {s.qty}</span>
                </div>
              );
            })}
          </div>
        </div>
      </div>
    );
  }

  function CommitModal({ project, inv, h, act, onClose }) {
    // Every owned unit; annotate ones already committed/installed elsewhere so the
    // operator can still try (and see the guarded 409), matching the DB trigger.
    const options = inv.units.map(function (u) {
      const commit = inv.allocations.find(function (a) { return a.hardware_unit_id === u.id && (a.allocation === "committed" || a.allocation === "installed"); });
      return { u: u, committedTo: commit ? commit.project_id : null };
    });
    const first = options.find(function (o) { return !o.committedTo; });
    const [unitId, setUnitId] = useState(first ? first.u.id : (options[0] ? options[0].u.id : ""));
    const [mode, setMode] = useState("committed");
    function submit() {
      if (!unitId) { toast("Pick a component", "close"); return; }
      Promise.resolve(act.commitUnit(project, Number(unitId), mode)).then(function () { onClose(); }, function () { /* keep open on 409 */ });
    }
    return (
      <Modal title={"Commit to " + project.name} icon="project"
        sub={"Reserve an owned unit → moves it to " + (project.tray_location_id ? h.locLabel(project.tray_location_id) : "this project")}
        onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Commit</button></>}>
        <label style={LBL}>Component</label>
        <select style={FLD} value={String(unitId)} onChange={function (e) { setUnitId(e.target.value); }}>
          {options.map(function (o) {
            const other = o.committedTo && o.committedTo !== project.id ? h.projectById[o.committedTo] : null;
            return <option key={o.u.id} value={String(o.u.id)}>
              {h.modelLabel(o.u.model_id)}{o.u.serial ? " · " + o.u.serial : ""}{other ? "  (committed: " + other.name + ")" : ""}
            </option>;
          })}
        </select>
        <label style={LBL}>Allocation</label>
        <select style={FLD} value={mode} onChange={function (e) { setMode(e.target.value); }}>
          <option value="committed">committed (reserved on the tray)</option>
          <option value="installed">installed (deployed into the build)</option>
        </select>
        <div className="muted" style={{ fontSize: 11.5, marginTop: 12 }}>
          A unit already committed to another project is rejected by the database (SQLSTATE 45000 → HTTP 409); the guard is surfaced here.
        </div>
      </Modal>
    );
  }

  function NeededModal({ project, inv, act, onClose }) {
    const [modelId, setModelId] = useState(inv.models[0] ? inv.models[0].id : "");
    const [qty, setQty] = useState(1);
    function submit() {
      if (!modelId) { toast("Pick a model", "close"); return; }
      act.addNeeded(project, Number(modelId), Number(qty) || 1);
      onClose();
    }
    return (
      <Modal title={"Add a needed model to " + project.name} icon="cart"
        sub="A model with no owned unit lands on the shopping list" onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Add to needs</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}>
            <label style={LBL}>Model</label>
            <select style={FLD} value={String(modelId)} onChange={function (e) { setModelId(e.target.value); }}>
              {inv.models.map(function (m) { return <option key={m.id} value={String(m.id)}>{m.make + " " + m.model} · {m.hw_type}</option>; })}
            </select>
          </div>
          <div style={{ flex: 1 }}><label style={LBL}>Quantity</label><input type="number" min="1" style={FLD} value={qty} onChange={function (e) { setQty(e.target.value); }} /></div>
        </div>
      </Modal>
    );
  }

  function ProjectModal({ inv, h, act, onClose }) {
    const [name, setName] = useState("");
    const [code, setCode] = useState("");
    const [status, setStatus] = useState("planning");
    const [tray, setTray] = useState("");
    const [owner, setOwner] = useState("");
    const [target, setTarget] = useState("");
    const [desc, setDesc] = useState("");
    const trays = inv.locations.filter(function (l) { return l.loc_type === "tray"; });
    function submit() {
      if (!name.trim()) { toast("Project name is required", "close"); return; }
      act.saveProject({
        name: name.trim(), code: code.trim() || null, status: status,
        tray_location_id: tray ? Number(tray) : null, owner: owner.trim() || null,
        target_on: target || null, started_on: null, completed_on: null, description: desc.trim() || "",
      }, false);
      onClose();
    }
    return (
      <Modal title="New project" icon="project" sub="Reserve inventory and stage a tray" onClose={onClose} wide
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Create project</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 2 }}><label style={LBL}>Name</label><input autoFocus style={FLD} value={name} onChange={function (e) { setName(e.target.value); }} placeholder="DNS HA cluster" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Code</label><input style={FLD} value={code} onChange={function (e) { setCode(e.target.value); }} placeholder="DNS-HA" /></div>
        </div>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}>
            <label style={LBL}>Status</label>
            <select style={FLD} value={status} onChange={function (e) { setStatus(e.target.value); }}>{PROJ_STATUS.map(function (s) { return <option key={s} value={s}>{s.replace("_", " ")}</option>; })}</select>
          </div>
          <div style={{ flex: 2 }}>
            <label style={LBL}>Tray</label>
            <select style={FLD} value={String(tray)} onChange={function (e) { setTray(e.target.value); }}>
              <option value="">— none —</option>
              {trays.map(function (l) { return <option key={l.id} value={String(l.id)}>{h.locPath(l.id)}</option>; })}
            </select>
          </div>
        </div>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Owner</label><input style={FLD} value={owner} onChange={function (e) { setOwner(e.target.value); }} placeholder="jason" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Target date</label><input type="date" style={FLD} value={target} onChange={function (e) { setTarget(e.target.value); }} /></div>
        </div>
        <label style={LBL}>Description</label>
        <input style={FLD} value={desc} onChange={function (e) { setDesc(e.target.value); }} placeholder="what this project builds…" />
      </Modal>
    );
  }

  Object.assign(window, { Inventory });
})();
