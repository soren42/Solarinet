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

  function InventoryPager({ currentPage, pageSize, count, label, onPage }) {
    const pageCount = Math.max(1, Math.ceil(count / pageSize));
    const page = Math.min(currentPage, pageCount - 1);
    if (count <= pageSize) return null;
    return <div className="filters" aria-label={label + " pages"} style={{ justifyContent: "flex-end", alignItems: "center" }}>
      <span className="muted" aria-live="polite">Showing {page * pageSize + 1}–{Math.min((page + 1) * pageSize, count)} of {count} {label}</span>
      <button className="btn-ghost" disabled={page === 0} onClick={function () { onPage(page - 1); }}>Previous</button>
      <span aria-label={"Page " + (page + 1) + " of " + pageCount}>Page {page + 1} of {pageCount}</span>
      <button className="btn-ghost" disabled={page + 1 >= pageCount} onClick={function () { onPage(page + 1); }}>Next</button>
    </div>;
  }

  // shared form styles (mirror AdoptModal)
  const FLD = { width: "100%", boxSizing: "border-box", padding: "8px 10px", marginTop: 4, borderRadius: 8, border: "1px solid var(--line-glow, rgba(255,255,255,0.14))", background: "rgba(255,255,255,0.04)", color: "inherit", fontFamily: "inherit", fontSize: 13 };
  const LBL = { fontSize: 11, textTransform: "uppercase", letterSpacing: "0.05em", opacity: 0.6, marginTop: 14, display: "block" };

  function nextId(list) { return list.reduce(function (m, x) { return Math.max(m, x.id || 0); }, 0) + 1; }
  // The locations API returns camelCase (parentId/locType) and, on the list/combined
  // reads, a nested tree. This screen renders flat by snake_case (parent_id/loc_type),
  // so normalize on ingestion — flatten any tree and map field names. Idempotent for
  // already-flat snake_case data (offline fixture, optimistic drafts).
  function normLocations(list) {
    const out = [];
    (function walk(nodes) {
      (nodes || []).forEach(function (n) {
        if (!n) return;
        out.push({
          id: n.id,
          parent_id: n.parentId != null ? n.parentId : (n.parent_id != null ? n.parent_id : null),
          name: n.name,
          code: n.code != null ? n.code : null,
          loc_type: n.locType || n.loc_type || "bin",
        });
        if (n.children && n.children.length) walk(n.children);
      });
    })(list);
    return out;
  }
  function normRows(list) {
    return (list || []).map(function (row) {
      const out = {};
      Object.keys(row || {}).forEach(function (key) {
        out[key.replace(/[A-Z]/g, function (letter) { return "_" + letter.toLowerCase(); })] = row[key];
      });
      return out;
    });
  }
  function cloneInv(src) {
    src = src || {};
    return {
      models: normRows(src.models),
      units: normRows(src.units),
      locations: normLocations(src.locations),
      receipts: normRows(src.receipts),
      projects: normRows(src.projects),
      allocations: normRows(src.allocations),
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
  function Inventory({ onNavigate }) {
    const [tab, setTab] = useState("components");
    const [showRackWireProfiles, setShowRackWireProfiles] = useState(false);
    const [inv, setInv] = useState(function () { return cloneInv(S.inventory); });
    const [viewer, setViewer] = useState(null);   // receipt being viewed (shared across tabs)
    const [loadError, setLoadError] = useState("");

    function loadInventory(force) {
      const a = api();
      if (!a || !a.inventory || isOffline()) return Promise.resolve(null);
      setLoadError("");
      return a.inventory(!!force).then(function (d) {
        setInv({
          models: normRows(d.models), units: normRows(d.units), locations: normLocations(d.locations),
          receipts: normRows(d.receipts), projects: normRows(d.projects), allocations: normRows(d.allocations),
          seq: (S.inventory && S.inventory.seq) || {},
        });
        setLoadError("");
        return d;
      }).catch(function (error) {
        setLoadError((error && error.message) || "The inventory data could not be refreshed.");
        throw error;
      });
    }

    function openRackWireProfiles() {
      setTab("components");
      setShowRackWireProfiles(true);
    }

    // Live re-read on mount (offline keeps the fixture working copy).
    useEffect(function () {
      loadInventory().catch(function () {});
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
          <div className="page-head__right inv-head-right">
            <div className="seg inv-tabs" role="group" aria-label="Inventory sections">
              {TABS.map(function (t) {
                return <button key={t[0]} className={tab === t[0] ? "on" : ""} aria-pressed={tab === t[0]} onClick={function () { setTab(t[0]); }}><Icon name={t[2]} size={14} />{t[1]}</button>;
              })}
            </div>
          </div>
        </div>

        {loadError && <div role="alert" className="empty" style={{ marginBottom: 16 }}>Inventory refresh failed: {loadError} Current screen contents are retained and may be stale.</div>}

        {tab === "components" && <ComponentsTab inv={inv} h={helpers} act={actions} onViewReceipt={setViewer} onInventoryChanged={function () { return loadInventory(true); }} showRackWireProfiles={showRackWireProfiles} onToggleRackWireProfiles={function () { setShowRackWireProfiles(function (value) { return !value; }); }} onNavigate={onNavigate} />}
        {tab === "locations" && <LocationsTab inv={inv} h={helpers} act={actions} />}
        {tab === "receipts" && <ReceiptsTab inv={inv} h={helpers} act={actions} onView={setViewer} />}
        {tab === "projects" && <ProjectsTab inv={inv} h={helpers} act={actions} />}

        {viewer && <ReceiptViewer receipt={viewer} inv={inv} h={helpers} onClose={function () { setViewer(null); }} />}
      </div>
    );
  }

  /* ===================== COMPONENTS TAB ===================== */
  function ComponentsTab({ inv, h, act, onViewReceipt, onInventoryChanged, showRackWireProfiles, onToggleRackWireProfiles, onNavigate }) {
    const [q, setQ] = useState("");
    const [stockFilter, setStockFilter] = useState("all");
    const [sort, setSort] = useState({ key: "model", dir: 1 });
    const [page, setPage] = useState(0);
    const [edit, setEdit] = useState(null);   // { unit } editing, or { unit:null } new
    const [entityLinkUnit, setEntityLinkUnit] = useState(null);
    const pageSize = 100;

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
    rows = rows.slice().sort(function (a, b) { const ka = keyOf(a), kb = keyOf(b); if (ka < kb) return -sort.dir; if (ka > kb) return sort.dir; return Number(a.id) - Number(b.id); });
    const pageCount = Math.max(1, Math.ceil(rows.length / pageSize));
    const activePage = Math.min(page, pageCount - 1);
    const visibleRows = rows.slice(activePage * pageSize, (activePage + 1) * pageSize);
    function th(key, label, right) {
      const on = sort.key === key;
      return <th style={{ cursor: "pointer", textAlign: right ? "right" : undefined }} onClick={function () { setSort(function (s) { return { key: key, dir: s.key === key ? -s.dir : 1 }; }); setPage(0); }}>
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
            return <button key={s} className={"chip" + (stockFilter === s ? " on" : "")} onClick={function () { setStockFilter(s); setPage(0); }}>
              {s !== "all" && <span className="dot" style={{ background: STOCK_TONE[s] }} />}{s === "all" ? "All" : STOCK_LABEL[s]}<span className="chip__n">{n}</span>
            </button>;
          })}
          <div className="search" style={{ marginLeft: "auto", maxWidth: 240, height: 36 }}><Icon name="search" size={15} /><input value={q} onChange={function (e) { setQ(e.target.value); setPage(0); }} placeholder="Find component…" /></div>
          <button className="btn-primary" onClick={function () { setEdit({ unit: null }); }}><Icon name="plus" size={14} />New component</button>
          <button className="btn-ghost" aria-expanded={!!showRackWireProfiles} onClick={onToggleRackWireProfiles}>RackWire device profiles</button>
        </div>

        {showRackWireProfiles && <RackWireProfileEditor models={inv.models} onNavigate={onNavigate} />}

        {rows.length === 0 && <div className="empty">No components match.</div>}
        {rows.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr>
              {th("model", "Model")}{th("serial", "Serial")}{th("asset", "Asset tag")}{th("location", "Location")}{th("qty", "Qty", true)}{th("stock", "Stock")}{th("cost", "Cost", true)}<th>SoR entity</th><th>Receipt</th>
            </tr></thead>
            <tbody>
              {visibleRows.map(function (u) {
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
                    <td>
                      {u.entity_name
                        ? <button type="button" className="inv-entity-link__current" onClick={function (e) { e.stopPropagation(); setEntityLinkUnit(u); }}>
                            <span aria-hidden="true">↔</span>{u.entity_name}
                          </button>
                        : <button type="button" className="inv-entity-link__action" onClick={function (e) { e.stopPropagation(); setEntityLinkUnit(u); }}>Link entity</button>}
                    </td>
                    <td>{r
                      ? <button className="inv-link" onClick={function (e) { e.stopPropagation(); onViewReceipt(r); }}><Icon name="receipt" size={12} style={{ verticalAlign: "-2px", marginRight: 3 }} />{r.vendor || ("#" + r.id)}</button>
                      : <span className="muted">—</span>}</td>
                  </tr>
                );
              })}
            </tbody>
          </table></div>
        )}

        {entityLinkUnit && <EntityLinkEditor unit={entityLinkUnit} h={h} onChanged={onInventoryChanged}
          onClose={function () { setEntityLinkUnit(null); }}
          />}

        <InventoryPager currentPage={activePage} pageSize={pageSize} count={rows.length} label="components" onPage={setPage} />

        {edit && <UnitModal unit={edit.unit} inv={inv} h={h} act={act} onClose={function () { setEdit(null); }} />}
      </div>
    );
  }

  function EntityLinkEditor({ unit, h, onClose, onChanged }) {
    const [data, setData] = useState(null);
    const [query, setQuery] = useState("");
    const [selected, setSelected] = useState("");
    const [loading, setLoading] = useState(true);
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState("");
    const [message, setMessage] = useState("");
    const a = api();

    function load(search) {
      if (!a || !a.inventoryEntityLink) { setError("Entity linking needs a live dashboard session."); setLoading(false); return; }
      setLoading(true); setError("");
      a.inventoryEntityLink(unit.id, search || "").then(function (result) {
        setData(result); setSelected("");
      }).catch(function (e) { setError((e && e.message) || "Could not load SoR entities."); })
        .finally(function () { setLoading(false); });
    }
    useEffect(function () { load(""); }, [unit.id]);

    function link(event) {
      event.preventDefault();
      if (!selected || !a || !a.linkInventoryEntity) return;
      setBusy(true); setError(""); setMessage("");
      a.linkInventoryEntity(unit.id, selected).then(function () {
        return onChanged().then(function () { setMessage("Entity association saved."); load(query.trim()); });
      }).catch(function (e) { setError((e && e.message) || "Could not link this entity."); })
        .finally(function () { setBusy(false); });
    }
    function unlink() {
      if (!a || !a.unlinkInventoryEntity) return;
      setBusy(true); setError(""); setMessage("");
      a.unlinkInventoryEntity(unit.id).then(function () {
        return onChanged().then(function () { setMessage("Entity association removed."); load(query.trim()); });
      }).catch(function (e) { setError((e && e.message) || "Could not remove this association."); })
        .finally(function () { setBusy(false); });
    }

    return <section className="panel inv-entity-link" aria-labelledby="inv-entity-link-title">
      <div className="panel__head"><Icon name="link" size={16} /><h3 id="inv-entity-link-title">Physical identity ↔ SoR entity</h3>
        <span className="right">explicit operator link</span>
        <button type="button" className="btn-ghost" onClick={onClose}>Close</button>
      </div>
      <div className="inv-entity-link__bridge" aria-label="Physical inventory unit links to a canonical SoR entity">
        <div className="inv-entity-link__node"><span className="inv-entity-link__eyebrow">PHYSICAL UNIT</span>
          <strong>{h.modelLabel(unit.model_id)}</strong>
          <span className="td-mono">{unit.asset_tag || "No asset tag"}</span>
          {unit.serial && <span className="td-mono muted">Serial {unit.serial}</span>}
        </div>
        <svg className="inv-entity-link__glyph" viewBox="0 0 64 24" role="img" aria-label="links to">
          <rect x="1" y="5" width="14" height="14" rx="2" fill="none" stroke="currentColor" strokeWidth="1.5" />
          <path d="M16 12h32m-7-7 7 7-7 7" fill="none" stroke="currentColor" strokeWidth="1.5" strokeLinecap="round" strokeLinejoin="round" />
          <rect x="49" y="5" width="14" height="14" rx="2" fill="none" stroke="currentColor" strokeWidth="1.5" />
        </svg>
        <div className="inv-entity-link__node"><span className="inv-entity-link__eyebrow">CANONICAL ENTITY</span>
          {data && data.entity
            ? <><strong>{data.entity.name}</strong><span className="muted">{data.entity.entityType} · {data.entity.lifecycle}</span></>
            : <strong className="muted">No entity linked</strong>}
        </div>
      </div>
      <p className="muted inv-entity-link__help">Choose the network or service record that represents this physical unit. RackWire uses this association when it resolves the unit’s asset tag for cable planning. Names and labels are shown as candidates; they are never matched automatically.</p>
      {data && data.entity && <button type="button" className="btn-ghost" onClick={unlink} disabled={busy || loading}>Remove association</button>}
      <form className="inv-entity-link__form" onSubmit={link}>
        <label>Find a SoR entity
          <div className="inv-entity-link__search"><input value={query} onChange={function (e) { setQuery(e.target.value); }} placeholder="Search hostname or role" />
            <button type="button" className="btn-ghost" onClick={function () { load(query.trim()); }} disabled={loading}>Search</button></div>
        </label>
        <label>Entity
          <select value={selected} onChange={function (e) { setSelected(e.target.value); }} disabled={loading || busy}>
            <option value="">Choose an entity…</option>
            {(data && data.candidates || []).map(function (entity) {
              return <option key={entity.id} value={entity.id}>#{entity.id} · {entity.name} · {entity.entityType} · {entity.lifecycle}</option>;
            })}
          </select>
        </label>
        <button type="submit" className="btn-primary" disabled={!selected || busy || loading}>
          {busy ? "Saving…" : data && data.entity ? "Change association" : "Link entity"}
        </button>
      </form>
      {loading && <p className="muted" role="status" aria-live="polite">Loading eligible SoR entities…</p>}
      {message && <p className="inv-entity-link__message" role="status">{message}</p>}
      {error && <p className="empty" role="alert">{error}</p>}
      {data && data.candidates.length === 0 && !loading && <p className="muted" role="status">No eligible entities match this search. Clear the search or create the canonical record first.</p>}
    </section>;
  }

  // A model profile turns an imported/catalog model into placeable RackWire
  // gear. Keep the editor beside inventory so the person reviewing physical
  // stock can define the connector geometry before moving to a plan.
  function RackWireProfileEditor({ models, onNavigate }) {
    const [library, setLibrary] = useState([]);
    const [modelId, setModelId] = useState("");
    const [name, setName] = useState("");
    const [kind, setKind] = useState("device");
    const [drawW, setDrawW] = useState("");
    const [poeBudgetW, setPoeBudgetW] = useState("");
    const [groups, setGroups] = useState([]);
    const [busy, setBusy] = useState(false);
    const [loadingProfiles, setLoadingProfiles] = useState(false);
    const [loadFailed, setLoadFailed] = useState(false);
    const [message, setMessage] = useState("");
    const [error, setError] = useState("");
    const selected = models.find(function (model) { return String(model.id) === String(modelId); }) || null;
    const a = api();
    const connectors = {
      rj45: { glyph: "rj45", domain: "net", conn: "rj45", role: "lan" },
      sfp: { glyph: "sfp", domain: "net", conn: "sfp", role: "uplink" },
      usba: { glyph: "usba", domain: "usb", conn: "usba", role: "data" },
      usbc: { glyph: "usbc", domain: "usb", conn: "usbc", role: "data" },
      iec_in: { glyph: "iec_in", domain: "power", conn: "iec", role: "power-in" },
      iec_out: { glyph: "iec_out", domain: "power", conn: "iec", role: "power-out" },
      nema: { glyph: "nema", domain: "power", conn: "iec", role: "power-out" },
      barrel: { glyph: "barrel", domain: "power", conn: "barrel", role: "power-in" },
      hdmi: { glyph: "hdmi", domain: "other", conn: "hdmi", role: "video" },
    };
    function blankGroup() { return { label: "Ethernet", connector: "rj45", count: 1, gbps: "1", watts: "", poe: "", poeW: "" }; }
    function chooseModel(value, profileRows) {
      setModelId(value); setError(""); setMessage("");
      const model = models.find(function (row) { return String(row.id) === String(value); });
      const available = profileRows || library;
      const profile = available.find(function (row) { return String(row.id) === String(value); });
      setName(profile ? (profile.name || profile.model) : (model ? (model.make + " " + model.model).trim() : ""));
      setKind(profile ? (profile.kind || model.hw_type || "device") : (model ? (model.hw_type || "device") : "device"));
      setDrawW(profile && profile.drawW != null ? String(profile.drawW) : "");
      setPoeBudgetW(profile && profile.poeBudgetW != null ? String(profile.poeBudgetW) : "");
      setGroups(profile && profile.groups && profile.groups.length ? profile.groups.map(function (group) {
        const connector = Object.keys(connectors).find(function (key) { return connectors[key].glyph === group.glyph; }) || "rj45";
        return { label: String(group.label || group.k || "Ports").replace(/\s+×\s*\d+$/, ""), connector: connector, count: group.count || 1, gbps: group.gbps == null ? "" : String(group.gbps), watts: group.watts == null ? "" : String(group.watts), poe: group.poe || "", poeW: group.poeW == null ? "" : String(group.poeW) };
      }) : [blankGroup()]);
    }
    function loadProfiles() {
      if (!a || !a.rackwireLibrary) return;
      setLoadingProfiles(true); setLoadFailed(false); setError("");
      a.rackwireLibrary().then(function (rows) {
        setLibrary(Array.isArray(rows) ? rows : []);
        const first = models.find(function (model) { return Array.isArray(rows) && rows.some(function (row) { return String(row.id) === String(model.id); }); }) || models[0];
        if (first) chooseModel(String(first.id), Array.isArray(rows) ? rows : []);
      }).catch(function (e) { setLoadFailed(true); setError((e && e.message) || "RackWire model profiles could not be loaded."); })
        .finally(function () { setLoadingProfiles(false); });
    }
    useEffect(function () {
      if (models.length) loadProfiles();
    }, []);
    function updateGroup(index, field, value) {
      setGroups(function (old) { return old.map(function (group, i) { return i === index ? Object.assign({}, group, { [field]: value }) : group; }); });
    }
    function save() {
      if (!selected || !a || !a.saveRackwireProfile) { setError("RackWire profiles require a live, authenticated dashboard session."); return; }
      const usedKeys = Object.create(null);
      const output = groups.map(function (group, index) {
        const spec = connectors[group.connector];
        const label = String(group.label || "").trim();
        const count = Number(group.count);
        const gbps = group.gbps === "" ? null : Number(group.gbps);
        const watts = group.watts === "" ? null : Number(group.watts);
        const poeW = group.poeW === "" ? null : Number(group.poeW);
        if (!label || !Number.isInteger(count) || count < 1 || count > 48 || (gbps != null && (!Number.isFinite(gbps) || gbps < 0 || gbps > 1000)) || (watts != null && (!Number.isFinite(watts) || watts < 0 || watts > 100000)) || (poeW != null && (!Number.isFinite(poeW) || poeW < 0 || poeW > 1000)) || (group.poe && !poeW) || (!group.poe && poeW)) throw new Error("Each port group needs a label, 1–48 ports, and valid speed or power values. Set both a PoE standard and a positive per-port watt value.");
        const baseKey = label.toLowerCase().replace(/[^a-z0-9]+/g, "-").replace(/^-|-$/g, "") || "ports";
        const keyCount = (usedKeys[baseKey] || 0) + 1;
        usedKeys[baseKey] = keyCount;
        const profileGroup = { k: keyCount === 1 ? baseKey : baseKey + "-" + keyCount, label: label + " ×" + count, glyph: spec.glyph, count: count, rows: Math.max(1, Math.ceil(count / 8)), role: spec.role, domain: spec.domain, conn: spec.conn };
        if (gbps != null && gbps > 0) profileGroup.gbps = gbps;
        if (watts != null && watts > 0) profileGroup.watts = watts;
        if (group.poe) { profileGroup.poe = group.poe; profileGroup.poeW = poeW; }
        return profileGroup;
      });
      if (!output.length) { setError("Add at least one port group before saving."); return; }
      const watts = drawW === "" ? null : Number(drawW);
      if (watts != null && (!Number.isFinite(watts) || watts < 0 || watts > 100000)) { setError("Device draw must be a number from 0 to 100,000 watts."); return; }
      const poeBudget = poeBudgetW === "" ? null : Number(poeBudgetW);
      if (poeBudget != null && (!Number.isFinite(poeBudget) || poeBudget < 0 || poeBudget > 100000)) { setError("PoE budget must be a number from 0 to 100,000 watts."); return; }
      setBusy(true); setError(""); setMessage("");
      a.saveRackwireProfile(selected.id, { name: name.trim() || (selected.make + " " + selected.model).trim(), kind: kind.trim() || selected.hw_type || "device", drawW: watts, poeBudgetW: poeBudget, groups: output })
        .then(function (saved) {
          setLibrary(function (old) { return old.map(function (row) { return String(row.id) === String(saved.id) ? saved : row; }); });
          setMessage("Profile saved. This model is now available in the RackWire gear library.");
          toast("RackWire device profile saved", "check");
        }).catch(function (e) { setError((e && e.message) || "Profile could not be saved."); })
        .finally(function () { setBusy(false); });
    }
    if (!models.length) return <div className="empty">Add or import a hardware model before configuring a RackWire profile.</div>;
    return <section className="panel" aria-labelledby="rackwire-profile-title" style={{ marginBottom: 18 }}>
      <div className="panel__head"><Icon name="rackwire" size={16} /><h3 id="rackwire-profile-title">RackWire device profile</h3><span className="right">connector and power planning</span>{typeof onNavigate === "function" && <button type="button" className="btn-ghost" onClick={function () { onNavigate("rackwire"); }}>Open RackWire planner</button>}</div>
      <p className="muted">Choose a SoR hardware model and describe its physical connections. Saved profiles appear in RackWire’s gear library and let scanned inventory be placed with usable port geometry.</p>
      {!a && <div role="alert" className="empty">The dashboard API is unavailable. Profile editing needs a live session.</div>}
      {loadingProfiles && <div role="status" aria-live="polite" className="muted">Loading SoR hardware models and RackWire profiles…</div>}
      <label style={Object.assign({}, LBL, { textTransform: "none", letterSpacing: 0 })}>SoR hardware model<select aria-label="SoR hardware model" value={modelId} onChange={function (e) { chooseModel(e.target.value); }} style={Object.assign({}, FLD, { maxWidth: 640 })}>
        <option value="">Choose a model…</option>{models.map(function (model) { const profile = library.find(function (row) { return String(row.id) === String(model.id); }); return <option key={model.id} value={String(model.id)}>{profile && profile.groups.length ? "✓ " : "○ "}#{model.id} · {model.make} {model.model}</option>; })}
      </select></label>
      {selected && <>
        <div style={{ display: "grid", gridTemplateColumns: "repeat(auto-fit,minmax(190px,1fr))", gap: 12, marginTop: 12 }}>
          <label style={Object.assign({}, LBL, { textTransform: "none", letterSpacing: 0, marginTop: 0 })}>Planner label<input aria-label="Planner label" value={name} onChange={function (e) { setName(e.target.value); }} style={FLD} /></label>
          <label style={Object.assign({}, LBL, { textTransform: "none", letterSpacing: 0, marginTop: 0 })}>Device kind<input aria-label="Device kind" value={kind} onChange={function (e) { setKind(e.target.value); }} style={FLD} /></label>
          <label style={Object.assign({}, LBL, { textTransform: "none", letterSpacing: 0, marginTop: 0 })}>Typical draw (W)<input aria-label="Typical draw watts" type="number" min="0" max="100000" value={drawW} onChange={function (e) { setDrawW(e.target.value); }} placeholder="Optional" style={FLD} /></label>
          <label style={Object.assign({}, LBL, { textTransform: "none", letterSpacing: 0, marginTop: 0 })}>PoE output budget (W)<input aria-label="PoE output budget watts" type="number" min="0" max="100000" value={poeBudgetW} onChange={function (e) { setPoeBudgetW(e.target.value); }} placeholder="Optional" style={FLD} /></label>
        </div>
        <div style={{ marginTop: 18, fontWeight: 600 }}>Connection groups</div>
        {groups.map(function (group, index) { return <div key={index} style={{ display: "grid", gridTemplateColumns: "repeat(auto-fit,minmax(105px,1fr))", gap: 8, alignItems: "end", marginTop: 8 }}>
          <label style={{ fontSize: 12 }}>Label<input aria-label={"Port group " + (index + 1) + " label"} value={group.label} onChange={function (e) { updateGroup(index, "label", e.target.value); }} style={FLD} /></label>
          <label style={{ fontSize: 12 }}>Connector<select aria-label={"Port group " + (index + 1) + " connector"} value={group.connector} onChange={function (e) { updateGroup(index, "connector", e.target.value); }} style={FLD}>{Object.keys(connectors).map(function (key) { return <option key={key} value={key}>{key.toUpperCase().replace("_", " ")}</option>; })}</select></label>
          <label style={{ fontSize: 12 }}>Count<input aria-label={"Port group " + (index + 1) + " count"} type="number" min="1" max="48" value={group.count} onChange={function (e) { updateGroup(index, "count", e.target.value); }} style={FLD} /></label>
          <label style={{ fontSize: 12 }}>Gbps<input aria-label={"Port group " + (index + 1) + " speed"} type="number" min="0" max="1000" step="0.1" value={group.gbps} onChange={function (e) { updateGroup(index, "gbps", e.target.value); }} placeholder="—" style={FLD} /></label>
          <label style={{ fontSize: 12 }}>Watts<input aria-label={"Port group " + (index + 1) + " watts"} type="number" min="0" max="100000" value={group.watts || ""} onChange={function (e) { updateGroup(index, "watts", e.target.value); }} placeholder="—" style={FLD} /></label>
          <label style={{ fontSize: 12 }}>PoE standard<select aria-label={"Port group " + (index + 1) + " PoE standard"} value={group.poe || ""} onChange={function (e) { updateGroup(index, "poe", e.target.value); }} style={FLD}><option value="">None</option>{["poe", "poe+", "poe++", "poe+++"].map(function (mode) { return <option key={mode} value={mode}>{mode}</option>; })}</select></label>
          <label style={{ fontSize: 12 }}>PoE W / port<input aria-label={"Port group " + (index + 1) + " PoE watts"} type="number" min="0" max="1000" value={group.poeW || ""} onChange={function (e) { updateGroup(index, "poeW", e.target.value); }} placeholder="—" style={FLD} /></label>
          <button type="button" className="btn-ghost" aria-label={"Remove port group " + (index + 1)} onClick={function () { setGroups(function (old) { return old.filter(function (_, i) { return i !== index; }); }); }}>×</button>
        </div>; })}
        <div style={{ display: "flex", gap: 10, flexWrap: "wrap", marginTop: 16 }}><button type="button" className="btn-ghost" onClick={function () { setGroups(function (old) { return old.concat([blankGroup()]); }); }}>Add connection group</button><button type="button" className="btn-primary" disabled={busy || !modelId} onClick={save}>{busy ? "Saving…" : "Save RackWire profile"}</button></div>
        <p className="muted" style={{ marginBottom: 0 }}>Connector details are used for compatibility checks. Verify port counts, speeds, and power values against the device specifications before relying on a plan.</p>
      </>}
      {message && <div role="status" style={{ marginTop: 12, color: "var(--ok)" }}>{message}</div>}
      {error && <div role="alert" style={{ marginTop: 12, color: "var(--crit)" }}>{error}</div>}
      {loadFailed && !loadingProfiles && <button type="button" className="btn-ghost" onClick={loadProfiles}>Retry profile load</button>}
    </section>;
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
    const [expanded, setExpanded] = useState({});
    const childrenOf = {};
    inv.locations.forEach(function (l) { const k = l.parent_id || 0; (childrenOf[k] = childrenOf[k] || []).push(l); });
    function unitCount(id) { return inv.units.filter(function (u) { return u.location_id === id; }).length; }

    function Node({ loc, depth }) {
      const kids = childrenOf[loc.id] || [];
      const n = unitCount(loc.id);
      const open = expanded[loc.id] === undefined ? depth === 0 : expanded[loc.id];
      return (
        <>
          <div className="inv-tree-row" style={{ paddingLeft: 10 + depth * 22 }}>
            {kids.length > 0
              ? <button className="rowx" aria-label={(open ? "Collapse " : "Expand ") + loc.name} aria-expanded={open} onClick={function () { setExpanded(function (prev) { return Object.assign({}, prev, { [loc.id]: !open }); }); }}>{open ? "▾" : "▸"}</button>
              : <span style={{ width: 24, flex: "0 0 auto" }} />}
            <Icon name={LOC_ICON[loc.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
            <span style={{ fontWeight: 600 }}>{loc.name}</span>
            {loc.code != null && <span className="inv-code">{loc.code}</span>}
            <span className="tag" style={{ marginLeft: 2 }}>{loc.loc_type}</span>
            <span style={{ flex: 1 }} />
            {n > 0 && <span className="td-mono muted" style={{ fontSize: 11 }}>{n} unit{n === 1 ? "" : "s"}</span>}
          </div>
          {open && kids.map(function (c) { return <Node key={c.id} loc={c} depth={depth + 1} />; })}
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
    const [page, setPage] = useState(0);
    const pageSize = 100;
    const total = inv.receipts.reduce(function (a, r) { return a + (r.subtotal_cents || 0); }, 0);
    function itemCount(id) { return inv.units.filter(function (u) { return u.receipt_id === id; }).length; }
    const sorted = inv.receipts.slice().sort(function (a, b) { return String(b.purchased_on || "").localeCompare(String(a.purchased_on || "")) || Number(b.id) - Number(a.id); });
    const pageCount = Math.max(1, Math.ceil(sorted.length / pageSize));
    const activePage = Math.min(page, pageCount - 1);
    const visibleReceipts = sorted.slice(activePage * pageSize, (activePage + 1) * pageSize);

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
          {visibleReceipts.map(function (r) {
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
        <InventoryPager currentPage={activePage} pageSize={pageSize} count={sorted.length} label="receipts" onPage={setPage} />
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
    const [page, setPage] = useState(0);
    const pageSize = 100;
    const sortedProjects = inv.projects.slice().sort(function (a, b) { return Number(b.id) - Number(a.id); });
    const pageCount = Math.max(1, Math.ceil(sortedProjects.length / pageSize));
    const activePage = Math.min(page, pageCount - 1);
    const visibleProjects = sortedProjects.slice(activePage * pageSize, (activePage + 1) * pageSize);

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
          {visibleProjects.map(function (p) {
            return <ProjectBoard key={p.id} project={p} inv={inv} h={h} act={act}
              onCommit={function () { setCommitFor(p); }} onNeed={function () { setNeedFor(p); }} />;
          })}
        </div>
        <InventoryPager currentPage={activePage} pageSize={pageSize} count={sortedProjects.length} label="projects" onPage={setPage} />
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
