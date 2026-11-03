/* ============================================================
   SolariNet — screens4: Inventory management
   Components · Catalog · Locations · Receipts · Projects  (SoR migration 014)

   Reads/writes the inventory subsystem: hardware_units (components),
   hardware_models (legacy models), product_types + product_barcodes (catalog,
   I1 WP2 API; items are created on a product type, migration 037), locations (property → floor → room → section,
   storage units anywhere — I1 WP4 API: move/merge/bulk/project/container types),
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
  // hardware_models.arch (031) — the CPU arch provisioning matches images against.
  const HW_ARCHES = ["x86_64", "arm64", "armhf", "riscv64", "i386"];
  const HW_TYPES = ["server", "workstation", "sbc", "nas", "router", "switch", "ap", "printer", "scanner", "iot", "appliance", "console", "media", "vehicle", "tablet", "phone", "kvm", "sensor", "camera", "ups", "moca", "other"];
  const LOC_TYPES = ["property", "floor", "room", "section", "storage_unit"];
  const LOC_ICON = { property: "location", floor: "location", room: "location", section: "location", storage_unit: "box" };
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

  // Mirrors inv_catalog_display_name() (migration 035) so legacy make+model
  // labels never read "Brand Brand model"; bare model strings ("400") pass through.
  function catalogName(make, model) {
    const b = String(make || "").trim(), m = String(model || "").trim();
    if (!b) return m;
    if (!m) return b;
    if (m.indexOf(b) === 0) return m;
    if (b.length >= m.length && b.slice(b.length - m.length) === m) return b;
    return b + " " + m;
  }
  function nextId(list) { return list.reduce(function (m, x) { return Math.max(m, x.id || 0); }, 0) + 1; }
  // The locations API returns camelCase (parentId/locType) and, on the list/combined
  // reads, a nested tree. This screen renders flat by snake_case (parent_id/loc_type),
  // so normalize on ingestion — flatten any tree and map field names. Idempotent for
  // already-flat snake_case data (offline fixture, optimistic drafts).
  // Pre-032 drafts and the offline fixture still carry legacy levels; show
  // them on the five-level model the API now enforces (migration 032 mapping).
  const LEGACY_LOC_TYPE = { site: "property", building: "property", rack: "storage_unit", shelf: "storage_unit", position: "storage_unit", zone: "storage_unit", cabinet: "storage_unit", drawer: "storage_unit", tray: "storage_unit", bin: "storage_unit", container: "storage_unit" };
  function locType(t) { return LOC_TYPES.indexOf(t) >= 0 ? t : (LEGACY_LOC_TYPE[t] || "storage_unit"); }
  function pick(n, camel, snake) { return n[camel] != null ? n[camel] : (n[snake] != null ? n[snake] : null); }
  function normLocations(list) {
    const out = [];
    (function walk(nodes) {
      (nodes || []).forEach(function (n) {
        if (!n) return;
        out.push({
          id: n.id,
          parent_id: pick(n, "parentId", "parent_id"),
          name: n.name,
          code: n.code != null ? n.code : null,
          loc_type: locType(n.locType || n.loc_type),
          container_type_id: pick(n, "containerTypeId", "container_type_id"),
          assigned_project_id: pick(n, "assignedProjectId", "assigned_project_id"),
          notes: n.notes != null ? n.notes : null,
          updated_at: pick(n, "updatedAt", "updated_at"),
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
    function modelLabel(id) { const m = modelById[id]; return m ? catalogName(m.make, m.model) : "—"; }
    // Items are identified by product type (037); model_id is legacy and may be null.
    function unitLabel(u) {
      if (!u) return "—";
      const name = String(u.product_type_name || "").trim();
      return name || modelLabel(u.model_id);
    }
    function unitKind(u) {
      if (!u) return "";
      if (u.product_type_kind) return CAT_KIND_LABEL[u.product_type_kind] || u.product_type_kind;
      const m = modelById[u.model_id];
      return m ? m.hw_type : "";
    }
    function unitCommitment(unitId) { return inv.allocations.find(function (a) { return a.hardware_unit_id === unitId && (a.allocation === "committed" || a.allocation === "installed") && !a.deleted; }); }

    // ----- mutations (optimistic local + best-effort live persist) -----
    function persist(kind, editing, id, body, okMsg) {
      const a = api();
      if (isOffline() || !a) { toast(okMsg + " (offline)", "check"); return; }
      const fn = {
        unit: editing ? function () { return a.updateUnit(id, body); } : function () { return a.createUnit(body); },
        model: function () { return a.createModel(body); },
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
      persist("model", false, null, draft, "Model added: " + catalogName(draft.make, draft.model));
      return created ? created.id : null;
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

    const helpers = { locById, modelById, receiptById, projectById, locLabel, locPath, modelLabel, unitLabel, unitKind, unitCommitment };
    const actions = { saveUnit, saveModel, saveReceipt, saveProject, commitUnit, addNeeded, setAllocation, removeAllocation };

    const TABS = [
      ["components", "Components", "box"],
      ["catalog", "Catalog", "tag2"],
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
        {tab === "catalog" && <CatalogTab inv={inv} h={helpers} onChanged={function () { return loadInventory(true); }} />}
        {tab === "locations" && <LocationsTab inv={inv} h={helpers} onChanged={function () { return loadInventory(true); }} />}
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
      const hay = [h.unitLabel(u), u.serial, u.asset_tag, h.locLabel(u.location_id), u.licence_reference].join(" ").toLowerCase();
      return hay.indexOf(ql) >= 0;
    });
    function keyOf(u) {
      switch (sort.key) {
        case "model": return h.unitLabel(u).toLowerCase();
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
          <button className="btn-primary" onClick={function () { setEdit({ unit: null }); }}><Icon name="plus" size={14} />New item</button>
          <button className="btn-ghost" aria-expanded={!!showRackWireProfiles} onClick={onToggleRackWireProfiles}>RackWire device profiles</button>
        </div>

        {showRackWireProfiles && <RackWireProfileEditor models={inv.models} onNavigate={onNavigate} />}

        {rows.length === 0 && <div className="empty">No components match.</div>}
        {rows.length > 0 && (
          <div className="tablewrap"><table className="grid">
            <thead><tr>
              {th("model", "Item")}{th("serial", "Serial")}{th("asset", "Asset tag")}{th("location", "Location")}{th("qty", "Qty", true)}{th("stock", "Stock")}{th("cost", "Cost", true)}<th>SoR entity</th><th>Receipt</th>
            </tr></thead>
            <tbody>
              {visibleRows.map(function (u) {
                const kindLabel = h.unitKind(u);
                const licence = licenceSummary(u);
                const r = u.receipt_id ? h.receiptById[u.receipt_id] : null;
                return (
                  <tr key={u.id} style={{ cursor: "pointer" }} onClick={function () { setEdit({ unit: u }); }}>
                    <td><div style={{ fontWeight: 600 }}>{h.unitLabel(u)}</div>{kindLabel && <div className="td-mono muted" style={{ fontSize: 10.5 }}>{kindLabel}</div>}{licence && <div className="muted inv-unit__licence">{licence}</div>}</td>
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

        {edit && <UnitModal unit={edit.unit} inv={inv} h={h} act={act} onChanged={onInventoryChanged} onClose={function () { setEdit(null); }} />}
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
          <strong>{h.unitLabel(unit)}</strong>
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

  /* ---- items on product types (migration 037) ---- */
  const ITEM_BATCH_MAX = 50;   // mirrors INV_COMPONENT_BATCH_MAX
  const PICK_CAP = 200;
  // Mirrors the obvious cases of inv_licence_reference_secret_like(); the API
  // stays the authority. "1Password" names a vault, not a secret.
  const PRODUCT_KEY = /(?:^|[^A-Za-z0-9])[A-Za-z0-9]{4,6}(?:-[A-Za-z0-9]{4,6}){3,}(?![A-Za-z0-9])/;
  function referenceLooksSecret(ref) {
    const r = String(ref || "").replace(/1Password/gi, "");
    return SECRET_KEY.test(r) || PRODUCT_KEY.test(r);
  }
  function itemErrorText(e, fallback) {
    const msg = String((e && e.message) || "");
    return msg || fallback || "The request failed.";
  }
  function byLabel(a, b) { return a.label.localeCompare(b.label, undefined, { numeric: true, sensitivity: "base" }); }
  function locationOptions(inv, h) {
    return inv.locations.map(function (l) { return { id: Number(l.id), label: h.locPath(l.id), tag: LOC_LABEL[l.loc_type] || "" }; }).sort(byLabel);
  }
  // Software is installed on another live item that is not itself software.
  function installedOnOptions(inv, h, selfId) {
    return inv.units.filter(function (u) {
      return Number(u.id) !== Number(selfId) && (u.product_type_kind ? u.product_type_kind !== "software" : true);
    }).map(function (u) {
      return { id: Number(u.id), label: h.unitLabel(u), sub: [u.asset_tag || u.serial, u.location_id != null ? h.locLabel(u.location_id) : ""].filter(Boolean).join(" · "), tag: h.unitKind(u) };
    }).sort(byLabel);
  }
  function licenceState(u) {
    return {
      seats: u && u.licence_seats != null ? String(u.licence_seats) : "",
      expires: (u && u.licence_expires_on) || "",
      reference: (u && u.licence_reference) || "",
      installedOn: u && u.installed_on_unit_id != null ? Number(u.installed_on_unit_id) : null,
    };
  }
  // API licence fields: values for software, nulls otherwise (the API refuses
  // them on other kinds, so a retype away from software clears them).
  function licenceBody(v, software) {
    if (!software) return { body: { licence_seats: null, licence_expires_on: null, licence_reference: null, installed_on_unit_id: null } };
    const seats = String(v.seats || "").trim();
    if (seats && !/^[1-9]\d{0,8}$/.test(seats)) return { error: "Seats must be a whole number of 1 or more." };
    const expires = String(v.expires || "").trim();
    if (expires && !/^\d{4}-\d{2}-\d{2}$/.test(expires)) return { error: "The expiry must be a date." };
    const ref = String(v.reference || "").trim();
    if (ref.length > 1024) return { error: "The licence reference must be 1024 characters or fewer." };
    if (ref && referenceLooksSecret(ref)) return { error: "The licence reference should say where the licence is kept — a vault entry or an order number — never the key, password or token itself." };
    return { body: { licence_seats: seats ? Number(seats) : null, licence_expires_on: expires || null, licence_reference: ref || null, installed_on_unit_id: v.installedOn != null ? Number(v.installedOn) : null } };
  }
  function licenceSummary(u) {
    if (!u || u.product_type_kind !== "software") return "";
    const parts = [];
    if (u.licence_seats != null) parts.push(u.licence_seats + (Number(u.licence_seats) === 1 ? " seat" : " seats"));
    if (u.licence_expires_on) parts.push("expires " + u.licence_expires_on);
    if (u.installed_on_unit_id != null) parts.push("on " + (u.installed_on_name || ("item #" + u.installed_on_unit_id)));
    if (u.licence_reference) parts.push(u.licence_reference);
    return parts.join(" · ");
  }
  // Every live (non-retired) product type, a page at a time.
  function loadLiveProductTypes(a) {
    const out = [];
    function next(offset) {
      return a.productTypes({ limit: 500, offset: offset }).then(function (res) {
        const items = res && Array.isArray(res.items) ? res.items : [];
        items.forEach(function (t) { out.push(t); });
        const page = (res && res.page) || {};
        if (page.hasMore && page.nextOffset != null && items.length && out.length < 10000) return next(Number(page.nextOffset));
        return out;
      });
    }
    return next(0);
  }

  // Searchable single choice — a tap list instead of a long <select>, which is
  // unusable on iPad with hundreds of locations or product types.
  function PickList({ label, options, value, onChange, noneLabel, placeholder, emptyText }) {
    const [q, setQ] = useState("");
    const needle = q.trim().toLowerCase();
    const matches = needle ? options.filter(function (o) { return (o.label + " " + (o.sub || "") + " " + (o.tag || "")).toLowerCase().indexOf(needle) >= 0; }) : options;
    const shown = matches.slice(0, PICK_CAP);
    const chosen = value == null ? null : options.filter(function (o) { return String(o.id) === String(value); })[0];
    return <div className="inv-pickfield">
      <span className="inv-pickfield__label">{label}</span>
      <div className="inv-pickfield__chosen" aria-live="polite">{chosen ? chosen.label + (chosen.sub ? " · " + chosen.sub : "") : (noneLabel || "Nothing chosen")}</div>
      {options.length > 6 && <div className="search inv-loc-search inv-sheet__search"><Icon name="search" size={15} /><input type="search" aria-label={"Filter " + label.toLowerCase()} value={q} onChange={function (e) { setQ(e.target.value); }} onKeyDown={holdEnter} placeholder={placeholder || "Filter…"} /></div>}
      <div className="inv-pick-list" role="radiogroup" aria-label={label}>
        {noneLabel && !needle && <button type="button" role="radio" aria-checked={value == null} className={"inv-pick" + (value == null ? " on" : "")} onClick={function () { onChange(null); }}><span className="inv-pick__text">{noneLabel}</span></button>}
        {shown.length === 0 && <div className="inv-loc__none">{emptyText || "Nothing matches."}</div>}
        {shown.map(function (o) {
          const on = String(o.id) === String(value);
          return <button type="button" key={o.id} role="radio" aria-checked={on} className={"inv-pick" + (on ? " on" : "")} onClick={function () { onChange(o.id); }}>
            <span className="inv-pick__text">{o.label}{o.sub ? <span className="muted"> · {o.sub}</span> : null}</span>
            {o.tag ? <span className="tag">{o.tag}</span> : null}
          </button>;
        })}
        {matches.length > shown.length && <div className="inv-loc__none">Showing {shown.length} of {matches.length} — filter to narrow.</div>}
      </div>
    </div>;
  }

  function LicenceFields({ value, onChange, options }) {
    function set(patch) { onChange(Object.assign({}, value, patch)); }
    return <fieldset className="inv-cat-specs inv-licence">
      <legend>Licence</legend>
      <div className="inv-form__row">
        <label className="inv-field"><span>Seats</span><input type="number" inputMode="numeric" min="1" step="1" value={value.seats} onChange={function (e) { set({ seats: e.target.value }); }} placeholder="—" /></label>
        <label className="inv-field"><span>Expires on</span><input type="date" value={value.expires} onChange={function (e) { set({ expires: e.target.value }); }} /></label>
      </div>
      <label className="inv-field"><span>Licence reference</span><input value={value.reference} maxLength={1024} autoComplete="off" spellCheck={false} onKeyDown={holdEnter} onChange={function (e) { set({ reference: e.target.value }); }} placeholder="Vault entry or order number" /></label>
      <div className="muted inv-cat__hint">Say where the licence is kept. Keys, passwords and tokens are refused.</div>
      <PickList label="Installed on" options={options} value={value.installedOn} onChange={function (id) { set({ installedOn: id }); }} noneLabel="Not installed" placeholder="Filter items…" emptyText="No hardware item matches." />
    </fieldset>;
  }

  // Create or edit one item. Live, the item is identified by its product type
  // (retired types are not offered); offline, the fixture still uses models.
  function UnitModal({ unit, inv, h, act, onChanged, onClose }) {
    const editing = !!unit;
    const a = api();
    const live = !isOffline() && !!a && !!a.productTypes && !!a.createUnit;
    const [types, setTypes] = useState(null);   // live product types, or { error }
    const [typeId, setTypeId] = useState(unit && unit.product_type_id != null ? Number(unit.product_type_id) : null);
    const [modelId, setModelId] = useState(unit ? unit.model_id : (inv.models[0] ? inv.models[0].id : 0));
    const [serial, setSerial] = useState(unit ? (unit.serial || "") : "");
    const [assetTag, setAssetTag] = useState(unit ? (unit.asset_tag || "") : "");
    const [locId, setLocId] = useState(unit && unit.location_id != null ? Number(unit.location_id) : null);
    const [acquired, setAcquired] = useState(unit ? (unit.acquired_on || "") : "");
    const [qty, setQty] = useState(unit ? String(unit.quantity || 1) : "1");
    const [cost, setCost] = useState(unit && unit.unit_cost_cents != null ? (unit.unit_cost_cents / 100).toFixed(2) : "");
    const [stock, setStock] = useState(unit ? unit.stock_status : "in_stock");
    const [receiptId, setReceiptId] = useState(unit ? (unit.receipt_id || "") : "");
    const [notes, setNotes] = useState(unit ? (unit.notes || "") : "");
    const [lic, setLic] = useState(function () { return licenceState(unit); });
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const [addModel, setAddModel] = useState(false);

    useEffect(function () {
      if (!live) return;
      loadLiveProductTypes(a).then(function (list) { setTypes(list); })
        .catch(function (e) { setTypes({ error: itemErrorText(e, "Product types could not be loaded.") }); });
    }, []);

    const typeList = Array.isArray(types) ? types : [];
    const current = unit && unit.product_type_id != null
      ? { id: Number(unit.product_type_id), name: unit.product_type_name, kind: unit.product_type_kind, retired: !!unit.product_type_retired }
      : null;
    const typeOptions = typeList.map(function (t) { return { id: Number(t.id), label: typeTitle(t), sub: typeByline(t), tag: CAT_KIND_LABEL[t.kind] || t.kind }; });
    if (current && !typeOptions.some(function (o) { return o.id === current.id; })) {
      typeOptions.unshift({ id: current.id, label: current.name || ("Product type #" + current.id), sub: "", tag: current.retired ? "retired" : (CAT_KIND_LABEL[current.kind] || "") });
    }
    const listed = typeId == null ? null : typeList.filter(function (t) { return Number(t.id) === Number(typeId); })[0];
    const chosenType = listed || (current && current.id === Number(typeId) ? current : null);
    const software = live ? !!chosenType && chosenType.kind === "software" : false;
    const keepsRetired = !!current && current.retired && Number(typeId) === current.id;

    function submit(e) {
      if (e && e.preventDefault) e.preventDefault();
      const qtyText = String(qty).trim();
      if (!/^\d+$/.test(qtyText) || Number(qtyText) < 1) { setErr("Quantity must be a whole number of 1 or more."); return; }
      const costText = String(cost).trim();
      if (costText !== "" && !(parseFloat(costText) >= 0)) { setErr("The unit cost must be a number."); return; }
      const fields = {
        serial: serial.trim() || null, asset_tag: assetTag.trim() || null,
        location_id: locId != null ? Number(locId) : null, acquired_on: acquired || null,
        quantity: Number(qtyText), unit_cost_cents: costText === "" ? null : Math.round(parseFloat(costText) * 100),
        stock_status: stock, receipt_id: receiptId ? Number(receiptId) : null, notes: notes.trim() || null,
      };
      if (!live) {
        if (!modelId) { setErr("Pick a model."); return; }
        act.saveUnit(Object.assign({ id: unit ? unit.id : undefined, model_id: Number(modelId) }, fields, { notes: fields.notes || "" }), editing);
        onClose();
        return;
      }
      if (!chosenType) { setErr(types === null ? "Product types are still loading." : "Choose what this item is — pick a product type."); return; }
      const lb = licenceBody(lic, software);
      if (lb.error) { setErr(lb.error); return; }
      const body = Object.assign({}, fields, lb.body);
      if (!editing || Number(typeId) !== Number(unit.product_type_id)) body.product_type_id = Number(typeId);
      setBusy(true); setErr("");
      (editing ? a.updateUnit(unit.id, body) : a.createUnit(body)).then(function () {
        toast(editing ? "Item updated" : "Item added", "check");
        if (onChanged) Promise.resolve(onChanged()).catch(function () {});
        onClose();
      }).catch(function (error) { setBusy(false); setErr(itemErrorText(error, "Save failed.")); });
    }

    return (
      <Sheet title={editing ? "Edit item" : "New item"} icon="box" wide
        sub={editing ? h.unitLabel(unit) + (unit.serial ? " · " + unit.serial : "") : "Record one item of a catalog product type"}
        onClose={onClose}
        footer={<><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button><button type="button" className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />{busy ? "Saving…" : (editing ? "Save changes" : "Add item")}</button></>}>
        <form className="inv-form" onSubmit={submit}>
          <SheetError text={err} />
          {live
            ? (types === null
                ? <div className="inv-loc__none" role="status">Loading product types…</div>
                : types.error
                  ? <div className="inv-loc-note inv-loc-note--err" role="alert">{types.error}</div>
                  : <PickList label="Product type" options={typeOptions} value={typeId} onChange={setTypeId} placeholder="Filter product types…" emptyText="No live product type matches. Add one in Catalog." />)
            : <div className="inv-form__row">
                <label className="inv-field inv-field--grow"><span>Model</span>
                  <select value={String(modelId)} onChange={function (e) { setModelId(e.target.value); }}>
                    {inv.models.map(function (m) { return <option key={m.id} value={String(m.id)}>{catalogName(m.make, m.model)} · {m.hw_type}</option>; })}
                  </select></label>
                <button type="button" className="btn-ghost inv-unit__addmodel" onClick={function () { setAddModel(true); }} aria-label="Add a new model"><Icon name="plus" size={13} /></button>
              </div>}
          {keepsRetired && <div className="inv-loc-note inv-loc-note--warn" role="note">{(current.name || "This product type") + " is retired. The item keeps it unless you choose a live type."}</div>}
          <div className="inv-form__row">
            <label className="inv-field"><span>Stock status</span>
              <select value={stock} onChange={function (e) { setStock(e.target.value); }}>
                {STOCK_STATES.map(function (st) { return <option key={st} value={st}>{STOCK_LABEL[st]}</option>; })}
              </select></label>
            <label className="inv-field"><span>Quantity</span><input type="number" inputMode="numeric" min="1" step="1" value={qty} onChange={function (e) { setQty(e.target.value); }} /></label>
          </div>
          <div className="inv-form__row">
            <label className="inv-field"><span>Serial</span><input value={serial} maxLength={128} autoComplete="off" spellCheck={false} onKeyDown={holdEnter} onChange={function (e) { setSerial(e.target.value); }} placeholder="Manufacturer serial" /></label>
            <label className="inv-field"><span>Asset tag</span><input value={assetTag} maxLength={64} autoComplete="off" spellCheck={false} onKeyDown={holdEnter} onChange={function (e) { setAssetTag(e.target.value); }} placeholder="Internal tag" /></label>
          </div>
          <PickList label="Location" options={locationOptions(inv, h)} value={locId} onChange={setLocId} noneLabel="Unassigned" placeholder="Filter locations…" />
          {software && <LicenceFields value={lic} onChange={setLic} options={installedOnOptions(inv, h, unit ? unit.id : null)} />}
          <div className="inv-form__row">
            <label className="inv-field"><span>Acquired on</span><input type="date" value={acquired || ""} onChange={function (e) { setAcquired(e.target.value); }} /></label>
            <label className="inv-field"><span>Unit cost (USD)</span><input type="number" inputMode="decimal" min="0" step="0.01" value={cost} onChange={function (e) { setCost(e.target.value); }} placeholder="0.00" /></label>
            <label className="inv-field"><span>Receipt</span>
              <select value={String(receiptId)} onChange={function (e) { setReceiptId(e.target.value); }}>
                <option value="">— none —</option>
                {inv.receipts.map(function (r) { return <option key={r.id} value={String(r.id)}>{(r.vendor || ("#" + r.id)) + (r.purchased_on ? " · " + r.purchased_on : "")}</option>; })}
              </select></label>
          </div>
          <label className="inv-field"><span>Notes</span><input value={notes} onChange={function (e) { setNotes(e.target.value); }} placeholder="Warranty, condition…" /></label>
        </form>
        {addModel && <ModelModal act={act} onClose={function () { setAddModel(false); }} onCreated={function (id) { setModelId(id); setAddModel(false); }} />}
      </Sheet>
    );
  }

  function ModelModal({ act, onClose, onCreated }) {
    const [make, setMake] = useState("");
    const [model, setModel] = useState("");
    const [hwType, setHwType] = useState("other");
    const [arch, setArch] = useState("");
    const [notes, setNotes] = useState("");
    function submit() {
      if (!make.trim() || !model.trim()) { toast("Make and model are required", "close"); return; }
      const id = act.saveModel({ make: make.trim(), model: model.trim(), hw_type: hwType, arch: arch || null, notes: notes.trim() || "" });
      onCreated && onCreated(id);
    }
    return (
      <Modal title="New model" icon="tag2" sub="Add a make/model to the catalog" onClose={onClose}
        footer={<><button className="btn-ghost" onClick={onClose}>Cancel</button><button className="btn-primary" onClick={submit}><Icon name="check" size={14} />Add model</button></>}>
        <div style={{ display: "flex", gap: 12 }}>
          <div style={{ flex: 1 }}><label style={LBL}>Make</label><input autoFocus className="touch-field" style={FLD} value={make} onChange={function (e) { setMake(e.target.value); }} placeholder="Dell, Ubiquiti…" /></div>
          <div style={{ flex: 1 }}><label style={LBL}>Model</label><input className="touch-field" style={FLD} value={model} onChange={function (e) { setModel(e.target.value); }} placeholder="R410, U7 Pro…" /></div>
        </div>
        <label style={LBL}>Type</label>
        <select className="touch-field" style={FLD} value={hwType} onChange={function (e) { setHwType(e.target.value); }}>{HW_TYPES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}</select>
        <label style={LBL} htmlFor="inv-model-arch">CPU architecture</label>
        <select id="inv-model-arch" className="touch-field" style={FLD} value={arch} onChange={function (e) { setArch(e.target.value); }} aria-describedby="inv-model-arch-hint">
          <option value="">unknown</option>{HW_ARCHES.map(function (t) { return <option key={t} value={t}>{t}</option>; })}
        </select>
        <div id="inv-model-arch-hint" className="muted" style={{ fontSize: 11.5, marginTop: 4 }}>Provisioning only reimages a machine whose CPU architecture is known.</div>
        <label style={LBL}>Notes</label>
        <input className="touch-field" style={FLD} value={notes} onChange={function (e) { setNotes(e.target.value); }} placeholder="spec links, quirks…" />
      </Modal>
    );
  }

  /* ===================== LOCATIONS TAB ===================== */
  /* ===================== LOCATIONS TAB (Inventory I1 WP5) =====================
     Hierarchy browser over the WP4 location API. Levels mirror location_admin.php
     LOC_CHILD_LEVEL: property → floor → room → optional section; storage units
     (D4) sit under any location and nest. Writes go straight to the live API —
     the server stays authoritative (level, cycle, duplicate, lock checks) and
     the screen re-reads after every change; offline browsing is read-only. */
  const LOC_CHILD_LEVEL = { property: "floor", floor: "room", room: "section" };
  const LOC_LABEL = { property: "Property", floor: "Floor", room: "Room", section: "Section", storage_unit: "Storage unit" };
  const LOC_ORDER = { property: 0, floor: 1, room: 2, section: 3, storage_unit: 4 };
  // Merge-preview reference classes → operator wording. Unknown keys fall back
  // to the raw "table.column" so a new FK is still visible, never hidden.
  const MERGE_REF_LABEL = {
    "locations.parent_id": "Child locations (move to the target)",
    "hardware_units.location_id": "Components (move to the target)",
    "entities.location_id": "Devices (move to the target)",
    "power_circuits.location_id": "Power circuits (move to the target)",
    "projects.tray_location_id": "Project trays (move to the target)",
    "barcodes": "Live barcode labels (retargeted)",
    "barcode_scans": "Scan history (stays with the source)",
    "external_refs": "External references (stay with the source)",
  };

  function childTypesFor(parentType) {
    if (!parentType) return ["property"];
    return LOC_CHILD_LEVEL[parentType] ? [LOC_CHILD_LEVEL[parentType], "storage_unit"] : ["storage_unit"];
  }
  function legalUnder(type, parentType) {
    if (type === "property") return !parentType;
    if (!parentType) return false;
    return type === "storage_unit" || LOC_CHILD_LEVEL[parentType] === type;
  }
  function byLocOrder(a, b) {
    return (LOC_ORDER[a.loc_type] - LOC_ORDER[b.loc_type])
      || String(a.name || "").localeCompare(String(b.name || ""), undefined, { numeric: true, sensitivity: "base" })
      || Number(a.id) - Number(b.id);
  }
  function locErrorText(e, fallback) {
    const code = String((e && e.code) || "");
    const msg = String((e && e.message) || "");
    switch (code) {
      case "conflict": return "This was changed elsewhere since you opened it. The latest version has been loaded — check it and save again.";
      case "location_cycle": return "A location can’t be placed inside itself or anything it contains.";
      case "duplicate_location": return "That name or code is already in use here. " + msg;
      case "merge_conflict": return "Merge refused: " + msg + " Preview the merge again before confirming.";
      case "container_not_empty": return "This container still holds components or child locations. Move or remove its contents before releasing it from the project.";
      case "not_found": return "It no longer exists — it may have been merged or removed elsewhere. " + msg;
      case "bad_request": return msg || "The server rejected this request.";
      case "DATA_STALE": return msg;
      default: return msg || fallback || "The request failed.";
    }
  }
  function intField(value, min, max) {
    const s = String(value == null ? "" : value).trim();
    if (!/^\d+$/.test(s)) return null;
    const n = Number(s);
    return n >= min && n <= max ? n : null;
  }
  function freshLocation(row) { return normLocations([row])[0]; }

  // Shared bottom-sheet/dialog shell (styles.css .modal family). Body scrolls;
  // the action footer never scrolls away, which keeps phone forms usable.
  function Sheet({ title, icon, sub, onClose, children, footer, wide }) {
    useEffect(function () {
      function onKey(e) { if (e.key === "Escape") onClose(); }
      window.addEventListener("keydown", onKey);
      return function () { window.removeEventListener("keydown", onKey); };
    }, [onClose]);
    return (
      <div className="modal-overlay inv-sheet-overlay" onClick={onClose}>
        <div className={"modal inv-sheet" + (wide ? " inv-sheet--wide" : "")} role="dialog" aria-modal="true" aria-label={title} onClick={function (e) { e.stopPropagation(); }}>
          <div className="modal__head">
            <Icon name={icon || "location"} size={20} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
            <div className="inv-sheet__title"><h3>{title}</h3>{sub && <div className="muted">{sub}</div>}</div>
            <button type="button" className="iconbtn" onClick={onClose} aria-label="Close"><Icon name="close" size={15} /></button>
          </div>
          <div className="modal__body">{children}</div>
          {footer && <div className="modal__foot">{footer}</div>}
        </div>
      </div>
    );
  }
  function SheetError({ text }) { return text ? <div className="inv-loc-note inv-loc-note--err" role="alert">{text}</div> : null; }

  function LocationsTab({ inv, h, onChanged }) {
    const [section, setSection] = useState("tree");
    const [selected, setSelected] = useState(null);
    const [expanded, setExpanded] = useState({});
    const [q, setQ] = useState("");
    const [sheet, setSheet] = useState(null);
    const [notice, setNotice] = useState("");
    const [types, setTypes] = useState([]);
    const [typesError, setTypesError] = useState("");
    const offline = isOffline() || !api() || !api().updateLocation;

    function loadTypes() {
      const a = api();
      if (offline || !a.containerTypes) return Promise.resolve([]);
      return a.containerTypes(true).then(function (rows) {
        setTypes(Array.isArray(rows) ? rows : []); setTypesError(""); return rows;
      }).catch(function (e) { setTypesError(locErrorText(e, "Container types could not be loaded.")); });
    }
    useEffect(function () { loadTypes(); }, []);

    const byId = h.locById;
    const childrenOf = {};
    inv.locations.forEach(function (l) { const k = l.parent_id != null && byId[l.parent_id] ? l.parent_id : 0; (childrenOf[k] = childrenOf[k] || []).push(l); });
    Object.keys(childrenOf).forEach(function (k) { childrenOf[k].sort(byLocOrder); });
    const unitsAt = {};
    inv.units.forEach(function (u) { if (u.location_id != null) unitsAt[u.location_id] = (unitsAt[u.location_id] || 0) + 1; });
    const typeById = {}; types.forEach(function (t) { typeById[t.id] = t; });
    const activeTypes = types.filter(function (t) { return !t.deleted_at; });
    function subtree(id) {
      const seen = {}; const stack = [id];
      while (stack.length) { const cur = stack.pop(); if (seen[cur]) continue; seen[cur] = true; (childrenOf[cur] || []).forEach(function (c) { stack.push(c.id); }); }
      return seen;
    }
    function ancestors(id) {
      const out = []; let l = byId[id] && byId[id].parent_id != null ? byId[byId[id].parent_id] : null; let g = 0;
      while (l && g++ < 64) { out.unshift(l); l = l.parent_id != null ? byId[l.parent_id] : null; }
      return out;
    }
    const tree = { byId, childrenOf, unitsAt, typeById, activeTypes, subtree, ancestors, path: h.locPath, projects: inv.projects, projectById: h.projectById, locations: inv.locations };

    function select(id) {
      setSelected(id);
      if (id != null) {
        const open = {};
        ancestors(id).forEach(function (a) { open[a.id] = true; });
        setExpanded(function (prev) { return Object.assign({}, prev, open); });
      }
    }
    function done(message, selectId) {
      setSheet(null);
      setNotice(message);
      toast(message, "check");
      return Promise.resolve(onChanged && onChanged()).catch(function () {}).then(function () {
        refresh();
        if (selectId !== undefined) select(selectId);
      });
    }
    const sel = selected != null ? byId[selected] : null;
    function closeSheet() { setSheet(null); }

    function treeRows(nodes, depth) {
      const out = [];
      (nodes || []).forEach(function (loc) {
        const kids = childrenOf[loc.id] || [];
        const open = expanded[loc.id] === undefined ? depth === 0 : expanded[loc.id];
        const n = unitsAt[loc.id] || 0;
        const on = selected === loc.id;
        out.push(
          <div key={loc.id} className={"inv-loc-row" + (on ? " on" : "")} style={{ paddingLeft: 4 + Math.min(depth, 8) * 12 }}>
            {kids.length > 0
              ? <button type="button" className="inv-loc-twisty" aria-label={(open ? "Collapse " : "Expand ") + loc.name} aria-expanded={open} onClick={function () { setExpanded(function (prev) { return Object.assign({}, prev, { [loc.id]: !open }); }); }}>{open ? "▾" : "▸"}</button>
              : <span className="inv-loc-twisty" aria-hidden="true" />}
            <button type="button" className="inv-loc-pick" aria-current={on ? "true" : undefined} onClick={function () { select(loc.id); }}>
              <Icon name={LOC_ICON[loc.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
              <span className="inv-loc-pick__name">{loc.name}</span>
              {loc.code != null && loc.code !== "" && <span className="inv-code">{loc.code}</span>}
              {loc.loc_type !== "storage_unit" && <span className="tag">{LOC_LABEL[loc.loc_type] || loc.loc_type}</span>}
              {n > 0 && <span className="inv-loc-pick__count">{n}</span>}
            </button>
          </div>
        );
        if (open && kids.length) out.push.apply(out, treeRows(kids, depth + 1));
      });
      return out;
    }

    const needle = q.trim().toLowerCase();
    const matches = needle ? inv.locations.filter(function (l) {
      return String(l.name || "").toLowerCase().indexOf(needle) >= 0 || String(l.code || "").toLowerCase().indexOf(needle) >= 0;
    }).sort(byLocOrder).slice(0, 200) : [];

    function renderDetail() {
      if (!sel) {
        const roots = childrenOf[0] || [];
        return <div className="inv-loc__empty">
          <Icon name="location" size={28} style={{ color: "var(--teal)" }} />
          <h3>{roots.length ? "Choose a location" : "No locations yet"}</h3>
          <p className="muted">{roots.length
            ? "Select a property, floor, room, section or storage unit to see its path, contents and actions."
            : "Start with a property (a house, office or site). Floors, rooms, sections and storage units go inside it."}</p>
          {!offline && <button type="button" className="btn-primary" onClick={function () { setSheet({ kind: "create", parent: null }); }}><Icon name="plus" size={14} />New property</button>}
        </div>;
      }
      const kids = childrenOf[sel.id] || [];
      const units = inv.units.filter(function (u) { return u.location_id === sel.id; });
      const isUnit = sel.loc_type === "storage_unit";
      const ctype = sel.container_type_id != null ? typeById[sel.container_type_id] : null;
      const project = sel.assigned_project_id != null ? h.projectById[sel.assigned_project_id] : null;
      const parentLoc = sel.parent_id != null ? byId[sel.parent_id] : null;
      const sameLevel = inv.locations.some(function (l) { return l.id !== sel.id && l.loc_type === sel.loc_type; });
      return <section className="inv-loc__card" aria-label={"Location " + sel.name}>
        <nav className="inv-crumbs" aria-label="Location path">
          <button type="button" className="inv-crumb inv-crumb--back" onClick={function () { select(parentLoc ? parentLoc.id : null); }} aria-label={"Back to " + (parentLoc ? parentLoc.name : "all locations")}>‹ {parentLoc ? parentLoc.name : "All"}</button>
          <ol>
            <li><button type="button" className="inv-crumb" onClick={function () { select(null); }}>All locations</button></li>
            {ancestors(sel.id).map(function (a) { return <li key={a.id}><button type="button" className="inv-crumb" onClick={function () { select(a.id); }}>{a.name}</button></li>; })}
            <li><span className="inv-crumb inv-crumb--here" aria-current="location">{sel.name}</span></li>
          </ol>
        </nav>
        <div className="inv-loc__head">
          <Icon name={LOC_ICON[sel.loc_type] || "box"} size={24} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
          <div style={{ minWidth: 0 }}>
            <h2>{sel.name}</h2>
            <div className="inv-loc__meta">
              <span className="tag">{LOC_LABEL[sel.loc_type] || sel.loc_type}</span>
              {sel.code != null && sel.code !== "" && <span className="inv-code">{sel.code}</span>}
              {ctype && <span className="tag">{ctype.name}{ctype.deleted_at ? " (retired)" : ""}</span>}
              {project && <Badge tone={PROJ_TONE[project.status] || "var(--violet)"}>{project.name}</Badge>}
            </div>
          </div>
        </div>
        {sel.notes && <p className="inv-loc__notes">{sel.notes}</p>}
        <dl className="inv-loc__facts">
          <div><dt>Inside</dt><dd>{kids.length} location{kids.length === 1 ? "" : "s"}</dd></div>
          <div><dt>Components</dt><dd>{units.length}</dd></div>
          {isUnit && <div><dt>Container type</dt><dd>{ctype ? ctype.name : "—"}</dd></div>}
          {isUnit && <div><dt>Project</dt><dd>{project ? project.name : "Unassigned"}</dd></div>}
        </dl>
        <h3 className="inv-loc__sub">Inside {sel.name}</h3>
        {kids.length === 0 && <div className="inv-loc__none">Nothing inside yet.</div>}
        {kids.length > 0 && <div className="inv-loc__kids">
          {kids.map(function (c) {
            const n = unitsAt[c.id] || 0;
            return <button type="button" key={c.id} className="inv-loc-pick inv-loc-kid" onClick={function () { select(c.id); }}>
              <Icon name={LOC_ICON[c.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
              <span className="inv-loc-pick__name">{c.name}</span>
              {c.code != null && c.code !== "" && <span className="inv-code">{c.code}</span>}
              <span className="tag">{LOC_LABEL[c.loc_type] || c.loc_type}</span>
              {n > 0 && <span className="inv-loc-pick__count">{n}</span>}
              <span aria-hidden="true" className="muted">›</span>
            </button>;
          })}
        </div>}
        {units.length > 0 && <>
          <h3 className="inv-loc__sub">Components here</h3>
          <ul className="inv-loc__units">
            {units.slice(0, 25).map(function (u) { return <li key={u.id}><span>{h.unitLabel(u)}</span>{(u.asset_tag || u.serial) && <span className="td-mono muted">{u.asset_tag || u.serial}</span>}</li>; })}
            {units.length > 25 && <li className="muted">…and {units.length - 25} more (see Components).</li>}
          </ul>
        </>}
        {offline
          ? <div className="inv-loc-note" role="note">Changes need a live connection. This view is read-only until the dashboard reconnects.</div>
          : <div className="inv-loc__actions" role="toolbar" aria-label={"Actions for " + sel.name}>
              <button type="button" className="btn-primary" onClick={function () { setSheet({ kind: "create", parent: sel }); }}><Icon name="plus" size={14} />Add inside</button>
              <button type="button" className="btn-ghost" onClick={function () { setSheet({ kind: "bulk", parent: sel }); }}><Icon name="box" size={14} />Add storage units</button>
              <button type="button" className="btn-ghost" onClick={function () { setSheet({ kind: "edit", loc: sel }); }}>Edit</button>
              {sel.loc_type !== "property" && <button type="button" className="btn-ghost" onClick={function () { setSheet({ kind: "move", loc: sel }); }}>Move</button>}
              {sameLevel && <button type="button" className="btn-ghost" onClick={function () { setSheet({ kind: "merge", loc: sel }); }}>Merge…</button>}
              {isUnit && <button type="button" className="btn-ghost" onClick={function () { setSheet({ kind: "project", loc: sel }); }}><Icon name="project" size={14} />{project ? "Project" : "Assign project"}</button>}
            </div>}
      </section>;
    }

    return (
      <div className="inv-locs">
        <div className="filters inv-loc-bar">
          <div className="seg inv-loc-views" role="group" aria-label="Location views">
            <button type="button" className={section === "tree" ? "on" : ""} aria-pressed={section === "tree"} onClick={function () { setSection("tree"); }}><Icon name="location" size={14} />Hierarchy</button>
            <button type="button" className={section === "types" ? "on" : ""} aria-pressed={section === "types"} onClick={function () { setSection("types"); }}><Icon name="box" size={14} />Container types</button>
          </div>
          {section === "tree" && <div className="search inv-loc-search"><Icon name="search" size={15} /><input type="search" aria-label="Find a location by name or code" value={q} onChange={function (e) { setQ(e.target.value); }} placeholder="Find name or code…" /></div>}
          {section === "tree" && !offline && <button type="button" className="btn-primary inv-loc-new" onClick={function () { setSheet({ kind: "create", parent: null }); }}><Icon name="plus" size={14} />New property</button>}
        </div>
        {notice && <div className="inv-loc-note inv-loc-note--ok" role="status">{notice}</div>}
        {typesError && <div className="inv-loc-note inv-loc-note--err" role="alert">{typesError}</div>}

        {section === "types"
          ? <ContainerTypesAdmin types={types} inv={inv} offline={offline} reload={loadTypes} />
          : <div className="inv-loc" data-detail={sel && !q.trim() ? "1" : "0"}>
              <div className="inv-loc__tree" aria-label="Location hierarchy">
                {needle
                  ? <>
                      <div className="inv-loc__treehead">{matches.length} match{matches.length === 1 ? "" : "es"}</div>
                      {matches.length === 0 && <div className="inv-loc__none">No location matches “{q.trim()}”.</div>}
                      {matches.map(function (l) {
                        return <button type="button" key={l.id} className={"inv-loc-pick inv-loc-hit" + (selected === l.id ? " on" : "")} onClick={function () { select(l.id); setQ(""); }}>
                          <Icon name={LOC_ICON[l.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                          <span className="inv-loc-hit__text"><span className="inv-loc-pick__name">{l.name}{l.code ? " · " + l.code : ""}</span><span className="muted">{h.locPath(l.id)}</span></span>
                        </button>;
                      })}
                    </>
                  : <>
                      <div className="inv-loc__treehead">{inv.locations.length} location{inv.locations.length === 1 ? "" : "s"}</div>
                      {(childrenOf[0] || []).length === 0 && <div className="inv-loc__none">No locations yet.</div>}
                      {treeRows(childrenOf[0], 0)}
                    </>}
              </div>
              <div className="inv-loc__detail">{renderDetail()}</div>
            </div>}

        {sheet && sheet.kind === "create" && <LocationFormSheet tree={tree} onClose={closeSheet} onDone={done} onChanged={onChanged} parent={sheet.parent} />}
        {sheet && sheet.kind === "edit" && <LocationFormSheet tree={tree} onClose={closeSheet} onDone={done} onChanged={onChanged} loc={sheet.loc} />}
        {sheet && sheet.kind === "move" && <MoveLocationSheet tree={tree} onClose={closeSheet} onDone={done} loc={sheet.loc} />}
        {sheet && sheet.kind === "merge" && <MergeLocationSheet tree={tree} onClose={closeSheet} onDone={done} loc={sheet.loc} />}
        {sheet && sheet.kind === "bulk" && <BulkStorageSheet tree={tree} onClose={closeSheet} onDone={done} parent={sheet.parent} />}
        {sheet && sheet.kind === "project" && <ContainerProjectSheet tree={tree} onClose={closeSheet} onDone={done} loc={sheet.loc} />}
      </div>
    );
  }

  function ProjectOptions({ projects, current }) {
    return (projects || []).filter(function (p) { return p.status !== "archived" || p.id === current; })
      .map(function (p) { return <option key={p.id} value={String(p.id)}>{p.name}{p.status && p.status !== "active" ? " · " + String(p.status).replace("_", " ") : ""}</option>; });
  }
  function TypeOptions({ tree, current }) {
    const list = tree.activeTypes.slice();
    const cur = current != null ? tree.typeById[current] : null;
    if (cur && cur.deleted_at) list.unshift(cur);   // keep a retired type visible on its own container only
    return list.map(function (t) { return <option key={t.id} value={String(t.id)}>{t.name}{t.deleted_at ? " (retired)" : ""}</option>; });
  }

  // Create (parent given / root) or edit (loc given). Only legal levels are offered.
  function LocationFormSheet({ tree, loc, parent, onClose, onDone, onChanged }) {
    const editing = !!loc;
    const parentLoc = editing ? (loc.parent_id != null ? tree.byId[loc.parent_id] : null) : parent;
    const parentType = parentLoc ? parentLoc.loc_type : null;
    const kids = editing ? (tree.childrenOf[loc.id] || []) : [];
    const options = editing
      ? ["property", "floor", "room", "section", "storage_unit"].filter(function (t) {
          return t === loc.loc_type || (legalUnder(t, parentType) && kids.every(function (c) { return legalUnder(c.loc_type, t); }));
        })
      : childTypesFor(parentType);
    const [base, setBase] = useState(loc || null);
    const [name, setName] = useState(editing ? loc.name : "");
    const [code, setCode] = useState(editing ? (loc.code || "") : "");
    const [locType, setLocType] = useState(editing ? loc.loc_type : options[0]);
    const [ctype, setCtype] = useState(editing && loc.container_type_id != null ? String(loc.container_type_id) : "");
    const [project, setProject] = useState("");
    const [notes, setNotes] = useState(editing ? (loc.notes || "") : "");
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const isUnit = locType === "storage_unit";

    function adopt(row) {
      setBase(row); setName(row.name || ""); setCode(row.code || ""); setLocType(row.loc_type);
      setCtype(row.container_type_id != null ? String(row.container_type_id) : ""); setNotes(row.notes || "");
    }
    function submit(e) {
      if (e && e.preventDefault) e.preventDefault();
      const trimmed = name.trim();
      if (!trimmed) { setErr("Name is required."); return; }
      if (trimmed.length > 128) { setErr("Name must be 128 characters or fewer."); return; }
      if (code.trim().length > 32) { setErr("Code must be 32 characters or fewer."); return; }
      const a = api();
      const body = { name: trimmed, loc_type: locType, code: code.trim() || null, notes: notes.trim() || null };
      // Re-sending an unchanged (possibly retired) type would fail validation; send changes only.
      const baseType = base && base.container_type_id != null ? String(base.container_type_id) : "";
      if (isUnit && (!editing || ctype !== baseType)) body.container_type_id = ctype ? Number(ctype) : null;
      setBusy(true); setErr("");
      let call;
      if (editing) {
        body.updated_at = base.updated_at || "";
        if (!isUnit && loc.loc_type === "storage_unit") {
          body.container_type_id = null;
          if (loc.assigned_project_id != null) body.assigned_project_id = null; // release only what is assigned
        }
        call = a.updateLocation(loc.id, body);
      } else {
        body.parent_id = parentLoc ? parentLoc.id : null;
        if (isUnit && project) body.assigned_project_id = Number(project);
        call = a.createLocation(body);
      }
      call.then(function (row) {
        onDone((editing ? "Saved " : "Added ") + trimmed, row && row.id != null ? Number(row.id) : (editing ? loc.id : undefined));
      }).catch(function (error) {
        setBusy(false);
        setErr(locErrorText(error, "Save failed."));
        if (editing && error && error.code === "conflict" && a.getLocation) {
          a.getLocation(loc.id).then(function (row) { adopt(freshLocation(row)); }).catch(function () {});
          if (onChanged) Promise.resolve(onChanged()).catch(function () {});
        }
      });
    }
    const where = parentLoc ? tree.path(parentLoc.id) : "Top level";
    return (
      <Sheet title={editing ? "Edit " + loc.name : (parentLoc ? "Add inside " + parentLoc.name : "New property")} icon="location"
        sub={editing ? "In " + where : where} onClose={onClose}
        footer={<><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button><button type="button" className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />{busy ? "Saving…" : (editing ? "Save changes" : "Add location")}</button></>}>
        <form className="inv-form" onSubmit={submit}>
          <SheetError text={err} />
          <div className="inv-form__row">
            <label className="inv-field inv-field--grow"><span>Name</span><input value={name} maxLength={128} onChange={function (e) { setName(e.target.value); }} placeholder={isUnit ? "Drawer 3" : "Name"} /></label>
            <label className="inv-field"><span>Code (optional)</span><input value={code} maxLength={32} autoCapitalize="characters" onChange={function (e) { setCode(e.target.value); }} placeholder="D3" /></label>
          </div>
          {options.length > 1
            ? <label className="inv-field"><span>Level</span>
                <select value={locType} onChange={function (e) { setLocType(e.target.value); }}>
                  {options.map(function (t) { return <option key={t} value={t}>{LOC_LABEL[t]}</option>; })}
                </select></label>
            : <div className="inv-field"><span>Level</span><div className="inv-field__static">{LOC_LABEL[locType]}{!parentLoc ? " — top-level locations are always properties" : ""}</div></div>}
          {isUnit && <label className="inv-field"><span>Container type (optional)</span>
            <select value={ctype} onChange={function (e) { setCtype(e.target.value); }}>
              <option value="">— none —</option>
              <TypeOptions tree={tree} current={editing ? loc.container_type_id : null} />
            </select></label>}
          {isUnit && !editing && <label className="inv-field"><span>Project (optional)</span>
            <select value={project} onChange={function (e) { setProject(e.target.value); }}>
              <option value="">— unassigned —</option>
              <ProjectOptions projects={tree.projects} />
            </select></label>}
          {editing && loc.loc_type === "storage_unit" && !isUnit && loc.assigned_project_id != null &&
            <div className="inv-loc-note" role="note">Changing the level releases this container from its project; that needs it to be empty.</div>}
          <label className="inv-field"><span>Notes (optional)</span><textarea rows={3} value={notes} onChange={function (e) { setNotes(e.target.value); }} /></label>
        </form>
      </Sheet>
    );
  }

  // Move: the picker hides the node's own subtree and offers only parents that
  // are legal for its level (the server still enforces both).
  function MoveLocationSheet({ tree, loc, onClose, onDone }) {
    const [target, setTarget] = useState(null);
    const [q, setQ] = useState("");
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const inside = tree.subtree(loc.id);
    const needle = q.trim().toLowerCase();
    const candidates = tree.locations.filter(function (l) {
      return !inside[l.id] && legalUnder(loc.loc_type, l.loc_type);
    }).map(function (l) { return { loc: l, path: tree.path(l.id) }; })
      .filter(function (c) { return !needle || c.path.toLowerCase().indexOf(needle) >= 0; })
      .sort(function (a, b) { return a.path.localeCompare(b.path, undefined, { numeric: true, sensitivity: "base" }); });
    const shown = candidates.slice(0, 300);
    const chosen = target != null ? tree.byId[target] : null;
    function submit() {
      if (!chosen) return;
      setBusy(true); setErr("");
      api().moveLocation(loc.id, chosen.id).then(function () {
        onDone("Moved " + loc.name + " into " + chosen.name, loc.id);
      }).catch(function (e) { setBusy(false); setErr(locErrorText(e, "Move failed.")); });
    }
    const rule = loc.loc_type === "storage_unit" ? "Storage units can go inside any location, including another storage unit."
      : LOC_LABEL[loc.loc_type] + "s can only sit inside a " + LOC_LABEL[Object.keys(LOC_CHILD_LEVEL).filter(function (k) { return LOC_CHILD_LEVEL[k] === loc.loc_type; })[0]].toLowerCase() + ".";
    return (
      <Sheet title={"Move " + loc.name} icon="location" sub={"Now in " + (loc.parent_id != null ? tree.path(loc.parent_id) : "top level")} onClose={onClose} wide
        footer={<><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button><button type="button" className="btn-primary" disabled={!chosen || busy} onClick={submit}>{busy ? "Moving…" : (chosen ? "Move into " + chosen.name : "Choose a destination")}</button></>}>
        <SheetError text={err} />
        <p className="muted inv-sheet__hint">{rule} {loc.name} and everything inside it are hidden from this list.</p>
        <div className="search inv-loc-search inv-sheet__search"><Icon name="search" size={15} /><input type="search" aria-label="Filter destinations" value={q} onChange={function (e) { setQ(e.target.value); }} placeholder="Filter destinations…" /></div>
        <div className="inv-pick-list" role="radiogroup" aria-label="Destination">
          {shown.length === 0 && <div className="inv-loc__none">No legal destination{needle ? " matches" : ""}.</div>}
          {shown.map(function (c) {
            const current = c.loc.id === loc.parent_id;
            return <button type="button" key={c.loc.id} role="radio" aria-checked={target === c.loc.id} disabled={current}
              className={"inv-pick" + (target === c.loc.id ? " on" : "")} onClick={function () { setTarget(c.loc.id); }}>
              <Icon name={LOC_ICON[c.loc.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
              <span className="inv-pick__text">{c.path}</span>
              {current ? <span className="tag">current</span> : <span className="tag">{LOC_LABEL[c.loc.loc_type]}</span>}
            </button>;
          })}
          {candidates.length > shown.length && <div className="inv-loc__none">Showing {shown.length} of {candidates.length} — filter to narrow.</div>}
        </div>
      </Sheet>
    );
  }

  // Merge: pick a same-level target outside the source subtree, preview the
  // reference counts, then confirm with the preview fingerprint.
  function MergeLocationSheet({ tree, loc, onClose, onDone }) {
    const [target, setTarget] = useState(null);
    const [q, setQ] = useState("");
    const [preview, setPreview] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const inside = tree.subtree(loc.id);
    const needle = q.trim().toLowerCase();
    const candidates = tree.locations.filter(function (l) { return l.loc_type === loc.loc_type && !inside[l.id]; })
      .map(function (l) {
        const clash = loc.assigned_project_id != null && l.assigned_project_id != null && l.assigned_project_id !== loc.assigned_project_id;
        return { loc: l, path: tree.path(l.id), clash: clash };
      })
      .filter(function (c) { return !needle || c.path.toLowerCase().indexOf(needle) >= 0; })
      .sort(function (a, b) { return a.path.localeCompare(b.path, undefined, { numeric: true, sensitivity: "base" }); })
      .slice(0, 300);
    const chosen = target != null ? tree.byId[target] : null;

    function loadPreview() {
      if (!chosen) return;
      setBusy(true); setErr(""); setPreview(null);
      api().locationMergePreview(loc.id, chosen.id).then(function (p) { setBusy(false); setPreview(p); })
        .catch(function (e) { setBusy(false); setErr(locErrorText(e, "Preview failed.")); });
    }
    function confirm() {
      if (!preview || !chosen) return;
      setBusy(true); setErr("");
      api().mergeLocation(loc.id, chosen.id, preview.fingerprint).then(function (r) {
        onDone("Merged " + loc.name + " into " + chosen.name + (r && r.idempotent ? " (already merged)" : ""), chosen.id);
      }).catch(function (e) { setBusy(false); setPreview(null); setErr(locErrorText(e, "Merge failed.")); });
    }

    const counts = preview && preview.counts ? preview.counts : {};
    const countKeys = Object.keys(counts).sort(function (a, b) { return (MERGE_REF_LABEL[a] ? 0 : 1) - (MERGE_REF_LABEL[b] ? 0 : 1) || a.localeCompare(b); });
    const nonZero = countKeys.filter(function (k) { return Number(counts[k]) > 0; });
    const inheritsProject = preview && preview.source && preview.target && preview.source.assigned_project_id != null && preview.target.assigned_project_id == null;
    const footer = preview
      ? <><button type="button" className="btn-ghost" onClick={function () { setPreview(null); }} disabled={busy}>Back</button>
          <button type="button" className="btn-primary inv-btn-danger" onClick={confirm} disabled={busy}>{busy ? "Merging…" : "Merge into " + chosen.name}</button></>
      : <><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button>
          <button type="button" className="btn-primary" onClick={loadPreview} disabled={!chosen || busy}>{busy ? "Loading preview…" : "Preview merge"}</button></>;
    return (
      <Sheet title={"Merge " + loc.name} icon="location" sub={"Combine a duplicate " + (LOC_LABEL[loc.loc_type] || "").toLowerCase() + " into another one"} onClose={onClose} wide footer={footer}>
        <SheetError text={err} />
        {!preview && <>
          <p className="muted inv-sheet__hint">Choose the location to keep. Only other {(LOC_LABEL[loc.loc_type] || "").toLowerCase()}s outside {loc.name} can be merge targets.</p>
          <div className="search inv-loc-search inv-sheet__search"><Icon name="search" size={15} /><input type="search" aria-label="Filter merge targets" value={q} onChange={function (e) { setQ(e.target.value); }} placeholder="Filter targets…" /></div>
          <div className="inv-pick-list" role="radiogroup" aria-label="Merge target">
            {candidates.length === 0 && <div className="inv-loc__none">No eligible target{needle ? " matches" : ""}.</div>}
            {candidates.map(function (c) {
              return <button type="button" key={c.loc.id} role="radio" aria-checked={target === c.loc.id} disabled={c.clash}
                className={"inv-pick" + (target === c.loc.id ? " on" : "")} onClick={function () { setTarget(c.loc.id); }}>
                <Icon name={LOC_ICON[c.loc.loc_type] || "box"} size={16} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
                <span className="inv-pick__text">{c.path}</span>
                {c.clash && <span className="tag">different project</span>}
              </button>;
            })}
          </div>
        </>}
        {preview && <div className="inv-merge" aria-live="polite">
          <div className="inv-merge__dir">
            <div className="inv-merge__end"><span className="inv-merge__eyebrow">Merged away</span><strong>{loc.name}</strong><span className="muted">{tree.path(loc.id)}</span></div>
            <div className="inv-merge__arrow" aria-hidden="true">→</div>
            <div className="inv-merge__end inv-merge__end--keep"><span className="inv-merge__eyebrow">Kept</span><strong>{chosen.name}</strong><span className="muted">{tree.path(chosen.id)}</span></div>
          </div>
          <h3 className="inv-loc__sub">What moves</h3>
          {nonZero.length === 0 && <div className="inv-loc__none">Nothing references {loc.name}; only the duplicate record is retired.</div>}
          {nonZero.length > 0 && <dl className="inv-merge__counts">
            {nonZero.map(function (k) { return <div key={k}><dt>{MERGE_REF_LABEL[k] || k}</dt><dd>{counts[k]}</dd></div>; })}
          </dl>}
          <ul className="inv-merge__facts">
            <li><strong>{loc.name}</strong> is soft-deleted, not erased: it stays in the audit trail and its scan history and external references remain attached to it.</li>
            {inheritsProject && <li>{chosen.name} takes over the project assignment from {loc.name}.</li>}
            <li>This can’t be undone from the dashboard.</li>
          </ul>
        </div>}
      </Sheet>
    );
  }

  // Bulk storage units: numbered (prefix + start) or pasted names, optional
  // generated codes. Server is all-or-none; the preview mirrors its rules
  // (trimmed prefixes, ≤32-char codes, no collisions within the batch).
  function bulkPlan(s, tree, parent) {
    const errors = [];
    let names = [];
    if (s.mode === "names") {
      names = s.pasted.split(/\r?\n/).map(function (n) { return n.trim(); }).filter(Boolean);
      if (!names.length) errors.push("Paste at least one name, one per line.");
      if (names.length > 500) errors.push("At most 500 storage units per batch (" + names.length + " pasted).");
    } else {
      const count = intField(s.count, 1, 500);
      const start = intField(s.start, 0, 999999999);
      const prefix = s.prefix.trim();
      if (count == null) errors.push("Count must be a whole number from 1 to 500.");
      if (!prefix) errors.push("Enter a name prefix, e.g. “Bin-”.");
      else if (prefix.length > 100) errors.push("Name prefix must be 100 characters or fewer.");
      if (start == null) errors.push("Start number must be a whole number, 0 or more.");
      if (count != null && prefix && start != null) for (let i = 0; i < count; i++) names.push(prefix + (start + i));
    }
    if (names.some(function (n) { return n.length > 128; })) errors.push("Names must be 128 characters or fewer.");
    const seen = {};
    const dupe = names.find(function (n) { const k = n.toLowerCase(); if (seen[k]) return true; seen[k] = true; return false; });
    if (dupe) errors.push("“" + dupe + "” appears twice in this batch.");
    const siblings = {};
    (tree.childrenOf[parent.id] || []).forEach(function (c) { siblings[String(c.name).toLowerCase()] = true; });
    const taken = names.find(function (n) { return siblings[n.toLowerCase()]; });
    if (taken) errors.push("“" + taken + "” already exists in " + parent.name + ".");
    let codes = names.map(function () { return null; });
    const codePrefix = s.codePrefix.trim();
    if (codePrefix) {
      const codeStart = intField(s.codeStart, 0, 999999999);
      if (codePrefix.length > 20) errors.push("Code prefix must be 20 characters or fewer.");
      if (codeStart == null) errors.push("Code start must be a whole number, 0 or more.");
      else {
        codes = names.map(function (_, i) { return codePrefix + (codeStart + i); });
        if (codes.some(function (c) { return c.length > 32; })) errors.push("Generated codes must be 32 characters or fewer.");
        const used = {};
        tree.locations.forEach(function (l) { if (l.code) used[String(l.code).toLowerCase()] = true; });
        const clash = codes.find(function (c) { return used[c.toLowerCase()]; });
        if (clash) errors.push("Code " + clash + " is already used by another location.");
      }
    }
    return { names: names, codes: codes, errors: errors };
  }

  function BulkStorageSheet({ tree, parent, onClose, onDone }) {
    const [s, setS] = useState({ mode: "numbered", count: "10", prefix: "", start: "1", pasted: "", codePrefix: "", codeStart: "1", ctype: "", project: "" });
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    function set(key, value) { setS(function (prev) { return Object.assign({}, prev, { [key]: value }); }); }
    const plan = bulkPlan(s, tree, parent);
    const n = plan.names.length;
    const ready = n > 0 && plan.errors.length === 0;
    function submit() {
      if (!ready) return;
      const body = { parent_id: parent.id, count: n };
      if (s.mode === "names") body.names = plan.names;
      else { body.prefix = s.prefix.trim(); body.start = intField(s.start, 0, 999999999); }
      if (s.codePrefix.trim()) { body.code_prefix = s.codePrefix.trim(); body.code_start = intField(s.codeStart, 0, 999999999); }
      if (s.ctype) body.container_type_id = Number(s.ctype);
      if (s.project) body.project_id = Number(s.project);
      setBusy(true); setErr("");
      api().bulkStorageUnits(body).then(function (r) {
        const made = r && r.count != null ? r.count : n;
        onDone("Created all " + made + " storage unit" + (made === 1 ? "" : "s") + " in " + parent.name + ".", parent.id);
      }).catch(function (e) { setBusy(false); setErr("Nothing was created. " + locErrorText(e, "The batch failed.")); });
    }
    const rows = n <= 12 ? plan.names.map(function (name, i) { return i; })
      : [0, 1, 2, 3, 4, 5, 6, 7, -1, n - 2, n - 1];
    return (
      <Sheet title={"Add storage units to " + parent.name} icon="box" sub={tree.path(parent.id)} onClose={onClose} wide
        footer={<><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button><button type="button" className="btn-primary" disabled={!ready || busy} onClick={submit}><Icon name="plus" size={14} />{busy ? "Creating…" : "Create " + n + " storage unit" + (n === 1 ? "" : "s")}</button></>}>
        <div className="inv-form">
          <SheetError text={err} />
          <div className="seg inv-form__seg" role="group" aria-label="Naming">
            <button type="button" className={s.mode === "numbered" ? "on" : ""} aria-pressed={s.mode === "numbered"} onClick={function () { set("mode", "numbered"); }}>Numbered</button>
            <button type="button" className={s.mode === "names" ? "on" : ""} aria-pressed={s.mode === "names"} onClick={function () { set("mode", "names"); }}>Paste names</button>
          </div>
          {s.mode === "numbered"
            ? <div className="inv-form__row">
                <label className="inv-field"><span>How many</span><input type="number" inputMode="numeric" min={1} max={500} value={s.count} onChange={function (e) { set("count", e.target.value); }} /></label>
                <label className="inv-field inv-field--grow"><span>Name prefix</span><input value={s.prefix} maxLength={100} onChange={function (e) { set("prefix", e.target.value); }} placeholder="Bin-" /></label>
                <label className="inv-field"><span>Start at</span><input type="number" inputMode="numeric" min={0} value={s.start} onChange={function (e) { set("start", e.target.value); }} /></label>
              </div>
            : <label className="inv-field"><span>Names, one per line (up to 500)</span><textarea rows={6} value={s.pasted} onChange={function (e) { set("pasted", e.target.value); }} placeholder={"Red drawer\nBlue drawer"} /></label>}
          {s.mode === "numbered" && <p className="muted inv-sheet__hint">Spaces at the ends of the prefix are trimmed, so “Bin-” gives Bin-1, Bin-2…</p>}
          <div className="inv-form__row">
            <label className="inv-field inv-field--grow"><span>Code prefix (optional)</span><input value={s.codePrefix} maxLength={20} autoCapitalize="characters" onChange={function (e) { set("codePrefix", e.target.value); }} placeholder="B" /></label>
            <label className="inv-field"><span>Code start</span><input type="number" inputMode="numeric" min={0} value={s.codeStart} disabled={!s.codePrefix.trim()} onChange={function (e) { set("codeStart", e.target.value); }} /></label>
          </div>
          <div className="inv-form__row">
            <label className="inv-field inv-field--grow"><span>Container type (optional)</span>
              <select value={s.ctype} onChange={function (e) { set("ctype", e.target.value); }}><option value="">— none —</option><TypeOptions tree={tree} /></select></label>
            <label className="inv-field inv-field--grow"><span>Project (optional)</span>
              <select value={s.project} onChange={function (e) { set("project", e.target.value); }}><option value="">— unassigned —</option><ProjectOptions projects={tree.projects} /></select></label>
          </div>
          <h3 className="inv-loc__sub">Preview · {n} storage unit{n === 1 ? "" : "s"}</h3>
          {plan.errors.length > 0 && <ul className="inv-loc-note inv-loc-note--warn inv-bulk-errors" aria-live="polite">{plan.errors.map(function (m) { return <li key={m}>{m}</li>; })}</ul>}
          {n > 0 && <ol className="inv-bulk-preview" aria-label="Generated storage units">
            {rows.map(function (i, k) {
              if (i < 0) return <li key={"gap" + k} className="inv-bulk-preview__gap">… {n - 10} more …</li>;
              return <li key={i}><span>{plan.names[i]}</span>{plan.codes[i] && <span className="inv-code">{plan.codes[i]}</span>}</li>;
            })}
          </ol>}
          <p className="muted inv-sheet__hint">All-or-none: if any name or code collides, nothing is created.</p>
        </div>
      </Sheet>
    );
  }

  function ContainerProjectSheet({ tree, loc, onClose, onDone }) {
    const current = loc.assigned_project_id;
    const [choice, setChoice] = useState(current != null ? String(current) : "");
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const currentProject = current != null ? tree.projectById[current] : null;
    function save(projectId) {
      setBusy(true); setErr("");
      api().setLocationProject(loc.id, projectId).then(function () {
        const p = projectId != null ? tree.projectById[projectId] : null;
        onDone(projectId == null ? "Released " + loc.name + " from its project" : "Assigned " + loc.name + " to " + (p ? p.name : "project"), loc.id);
      }).catch(function (e) { setBusy(false); setErr(locErrorText(e, "Project change failed.")); });
    }
    return (
      <Sheet title={"Project for " + loc.name} icon="project" sub={tree.path(loc.id)} onClose={onClose}
        footer={<>
          {current != null && <button type="button" className="btn-ghost inv-btn-left" disabled={busy} onClick={function () { save(null); }}>Release</button>}
          <button type="button" className="btn-ghost" onClick={onClose}>Cancel</button>
          <button type="button" className="btn-primary" disabled={busy || !choice || choice === String(current)} onClick={function () { save(Number(choice)); }}>{busy ? "Saving…" : "Assign"}</button>
        </>}>
        <div className="inv-form">
          <SheetError text={err} />
          <p className="muted inv-sheet__hint">{currentProject ? "Currently reserved for " + currentProject.name + ". Releasing requires the container to be empty." : "Reserve this container for a project's parts."}</p>
          <label className="inv-field"><span>Project</span>
            <select value={choice} onChange={function (e) { setChoice(e.target.value); }}>
              <option value="">— choose a project —</option>
              <ProjectOptions projects={tree.projects} current={current} />
            </select></label>
        </div>
      </Sheet>
    );
  }

  // Container types (D5): rename and retire while in use; retired types keep
  // their references but leave every pick list.
  function ContainerTypesAdmin({ types, inv, offline, reload }) {
    const [showRetired, setShowRetired] = useState(false);
    const [newName, setNewName] = useState("");
    const [editing, setEditing] = useState(null);
    const [retiring, setRetiring] = useState(null);
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const [msg, setMsg] = useState("");
    const used = {};
    inv.locations.forEach(function (l) { if (l.container_type_id != null) used[l.container_type_id] = (used[l.container_type_id] || 0) + 1; });
    const retiredCount = types.filter(function (t) { return t.deleted_at; }).length;
    const list = types.filter(function (t) { return showRetired || !t.deleted_at; });
    function run(promise, okMsg) {
      setBusy(true); setErr(""); setMsg("");
      return promise.then(function () {
        setBusy(false); setMsg(okMsg); setEditing(null); setRetiring(null); toast(okMsg, "check"); return reload();
      }).catch(function (e) {
        setBusy(false); setErr(locErrorText(e, "Container type change failed."));
        if (e && (e.code === "conflict" || e.code === "not_found")) { setEditing(null); reload(); }
      });
    }
    function create(e) {
      if (e && e.preventDefault) e.preventDefault();
      const name = newName.trim();
      if (!name) { setErr("Enter a name for the new container type."); return; }
      if (name.length > 64) { setErr("Container type names must be 64 characters or fewer."); return; }
      run(api().createContainerType(name), "Added container type " + name).then(function () { setNewName(""); });
    }
    function rename(t) {
      const name = editing.name.trim();
      if (!name) { setErr("Name cannot be empty."); return; }
      if (name.length > 64) { setErr("Container type names must be 64 characters or fewer."); return; }
      run(api().renameContainerType(t.id, name, t.updated_at || ""), "Renamed to " + name);
    }
    return (
      <div className="inv-ctypes">
        <p className="muted inv-sheet__hint">Kinds of storage unit — drawer, bin, tray, shelf. Retiring a type keeps it on existing containers but removes it from pick lists.</p>
        {offline && <div className="inv-loc-note" role="note">Container types need a live connection.</div>}
        {err && <div className="inv-loc-note inv-loc-note--err" role="alert">{err}</div>}
        {msg && <div className="inv-loc-note inv-loc-note--ok" role="status">{msg}</div>}
        {!offline && <form className="inv-ctypes__add" onSubmit={create}>
          <label className="inv-field inv-field--grow"><span>New container type</span><input value={newName} maxLength={64} onChange={function (e) { setNewName(e.target.value); }} placeholder="Drawer" /></label>
          <button type="submit" className="btn-primary" disabled={busy}><Icon name="plus" size={14} />Add type</button>
        </form>}
        <div className="filters inv-ctypes__filters">
          <button type="button" className={"chip" + (showRetired ? " on" : "")} aria-pressed={showRetired} onClick={function () { setShowRetired(!showRetired); }}>Include retired ({retiredCount})</button>
        </div>
        <ul className="inv-ctypes__list" aria-label="Container types">
          {list.length === 0 && <li className="inv-loc__none">{offline ? "Unavailable offline." : "No container types yet."}</li>}
          {list.map(function (t) {
            const n = used[t.id] || 0;
            const isEditing = editing && editing.id === t.id;
            return <li key={t.id} className={"inv-ctype" + (t.deleted_at ? " retired" : "")}>
              {isEditing
                ? <form className="inv-ctype__edit" onSubmit={function (e) { e.preventDefault(); rename(t); }}>
                    <label className="inv-field inv-field--grow"><span>Rename {t.name}</span><input value={editing.name} maxLength={64} onChange={function (e) { setEditing({ id: t.id, name: e.target.value }); }} /></label>
                    <button type="submit" className="btn-primary" disabled={busy}>Save</button>
                    <button type="button" className="btn-ghost" onClick={function () { setEditing(null); }}>Cancel</button>
                  </form>
                : <>
                    <div className="inv-ctype__text"><strong>{t.name}</strong><span className="muted">{n ? "On " + n + " container" + (n === 1 ? "" : "s") : "Not in use"}</span></div>
                    {t.deleted_at && <Badge tone="var(--ink-faint)">retired</Badge>}
                    {!t.deleted_at && !offline && (retiring === t.id
                      ? <div className="inv-ctype__confirm" role="group" aria-label={"Confirm retiring " + t.name}>
                          <span>{n ? n + " container" + (n === 1 ? " keeps" : "s keep") + " this type. " : ""}Retire it?</span>
                          <button type="button" className="btn-ghost inv-btn-danger" disabled={busy} onClick={function () { run(api().retireContainerType(t.id), "Retired " + t.name); }}>Retire</button>
                          <button type="button" className="btn-ghost" onClick={function () { setRetiring(null); }}>Keep</button>
                        </div>
                      : <div className="inv-ctype__acts">
                          <button type="button" className="btn-ghost" onClick={function () { setRetiring(null); setEditing({ id: t.id, name: t.name }); }}>Rename</button>
                          <button type="button" className="btn-ghost" onClick={function () { setEditing(null); setRetiring(t.id); }}>Retire</button>
                        </div>)}
                  </>}
            </li>;
          })}
        </ul>
      </div>
    );
  }

  /* ===================== CATALOG TAB (I1 WP5) ===================== */
  // Product types (what a thing is) and their barcodes, over the WP2 catalog API.
  // Lookups only ever prefill (D3); retiring is a soft delete allowed while in use (D5).
  const CAT_KINDS = ["hardware", "peripheral", "component", "software", "accessory", "consumable", "other"];
  const CAT_KIND_LABEL = { hardware: "Hardware", peripheral: "Peripheral", component: "Component", software: "Software", accessory: "Accessory", consumable: "Consumable", other: "Other" };
  const CODE_TYPES = ["gtin", "sku", "other"];
  const CODE_TYPE_LABEL = { gtin: "GTIN / UPC / EAN", sku: "SKU", other: "Other" };
  const CAT_PAGE = 50;
  // Mirrors catalog_has_secret_key(): licence data is a reference on the item, never a key.
  const SECRET_KEY = /(?:secret|password|passwd|token|api.?key|license.?key|licence.?key|credential|private.?key)/i;

  function catErrorText(e, fallback) {
    const code = String((e && e.code) || "");
    const msg = String((e && e.message) || "");
    switch (code) {
      case "conflict": return "This product type was changed elsewhere since you opened it. The latest version has been loaded — check it and save again.";
      case "duplicate_barcode": return "That code is already attached to another product type.";
      case "not_found": return "It no longer exists — it may have been retired elsewhere.";
      case "bad_request": return msg || "The server rejected this request.";
      case "DATA_STALE": return msg;
      default: return msg || fallback || "The request failed.";
    }
  }
  // Server-side normalization (catalog_normalize_code): whitespace removed, upper-cased.
  function normCode(code) { return String(code || "").replace(/\s+/g, "").toUpperCase(); }
  function gtinValid(code) {
    if (!/^(?:\d{8}|\d{12}|\d{13}|\d{14})$/.test(code)) return false;
    const digits = code.split("").map(Number); const check = digits.pop();
    let sum = 0, weight = 3;
    for (let i = digits.length - 1; i >= 0; i--) { sum += digits[i] * weight; weight = weight === 3 ? 1 : 3; }
    return (10 - sum % 10) % 10 === check;
  }
  function guessCodeType(code) { return gtinValid(code) ? "gtin" : "other"; }
  function codeError(code, codeType) {
    if (!code) return "Enter or scan a code.";
    if (code.length > 128) return "Codes must be 128 characters or fewer.";
    if (codeType === "gtin" && !gtinValid(code)) return "That isn’t a valid GTIN — it needs 8, 12, 13 or 14 digits with a correct check digit. Choose SKU or Other for shop codes.";
    return "";
  }
  // Display name (migration 035 already stores it). A type saved with a bare model
  // ("400") still reads with its brand.
  function typeTitle(t) {
    const name = String((t && t.name) || "").trim(), model = String((t && t.model) || "").trim();
    if (!name || name === model) return catalogName(t && t.brand, model) || name || "Untitled";
    return name;
  }
  function typeByline(t) { return [t.brand, t.model, t.category].filter(function (x) { return x != null && String(x).trim() !== ""; }).join(" · "); }
  function isLegacyLinked(t) { const s = String((t && t.lookup_source) || ""); return s === "migration:033" || s.indexOf("legacy:") === 0; }
  function hasSecretKey(value) {
    if (!value || typeof value !== "object") return false;
    return Object.keys(value).some(function (k) { return SECRET_KEY.test(k) || hasSecretKey(value[k]); });
  }
  function specObject(specs) {
    if (!specs || typeof specs !== "object") return {};
    if (Array.isArray(specs)) { const o = {}; specs.forEach(function (v, i) { o[String(i)] = v; }); return o; }   // PHP decodes "{}" as []
    return specs;
  }
  function specText(v) { return v === null ? "null" : typeof v === "object" ? JSON.stringify(v) : String(v); }
  function stable(v) {
    if (Array.isArray(v)) return "[" + v.map(stable).join(",") + "]";
    if (v && typeof v === "object") return "{" + Object.keys(v).sort().map(function (k) { return JSON.stringify(k) + ":" + stable(v[k]); }).join(",") + "}";
    return JSON.stringify(v === undefined ? null : v);
  }
  // Spec rows remember the value they started from so an untouched number,
  // boolean or nested value round-trips with its JSON type intact.
  function specRows(specs) {
    const o = specObject(specs);
    return Object.keys(o).map(function (k, i) {
      const v = o[k];
      return { rid: i + 1, key: k, text: specText(v), orig: v, origText: specText(v), has: true, nested: v !== null && typeof v === "object" };
    });
  }
  function specsFromRows(rows) {
    const out = {}; const seen = {};
    for (let i = 0; i < rows.length; i++) {
      const r = rows[i]; const key = r.key.trim(); const text = r.text;
      if (!key && !text.trim()) continue;
      if (!key) return { error: "Spec " + (i + 1) + " needs a name." };
      if (seen[key]) return { error: "“" + key + "” is listed twice. Each spec name can appear once." };
      seen[key] = true;
      let v;
      if (r.has && text === r.origText) v = r.orig;
      else if (r.nested) {
        try { v = JSON.parse(text); } catch (e) { return { error: "“" + key + "” holds structured data; keep it valid JSON (or replace it with plain text)." }; }
      } else if (r.has && typeof r.orig === "number" && /^-?\d+(?:\.\d+)?$/.test(text.trim())) v = Number(text.trim());
      else if (r.has && typeof r.orig === "boolean" && /^(?:true|false)$/i.test(text.trim())) v = text.trim().toLowerCase() === "true";
      else v = text.trim();
      out[key] = v;
    }
    if (hasSecretKey(out)) return { error: "Spec names like “licence key”, “password” or “token” look like secrets, and secrets are never stored. Record a licence reference on the item instead." };
    return { specs: Object.keys(out).length ? out : null };
  }
  // A wedge scanner ends with Enter; in a field that feeds a write, that must not submit (D3).
  function holdEnter(e) { if (e && e.key === "Enter" && e.preventDefault) e.preventDefault(); }
  function specDisplay(v) { return v === null || v === "" ? "—" : typeof v === "object" ? JSON.stringify(v) : String(v); }

  function CatalogTab({ inv, h, onChanged }) {
    const [q, setQ] = useState("");
    const [kind, setKind] = useState("");
    const [list, setList] = useState({ items: [], total: 0, hasMore: false, nextOffset: 0, loaded: false, q: "", kind: "" });
    const [loading, setLoading] = useState(false);
    const [listErr, setListErr] = useState("");
    const [selected, setSelected] = useState(null);
    const [detail, setDetail] = useState(null);
    const [detailErr, setDetailErr] = useState("");
    const [sheet, setSheet] = useState(null);
    const [notice, setNotice] = useState("");
    const [scan, setScan] = useState("");
    const [lookup, setLookup] = useState(null);      // { code, found, name? } | { error }
    const [retiring, setRetiring] = useState(false);
    const [removing, setRemoving] = useState(null);  // barcode id awaiting confirmation
    const [attach, setAttach] = useState({ code: "", code_type: "other" });
    const [busy, setBusy] = useState(false);
    const [actErr, setActErr] = useState("");
    const [ctl] = useState(function () { return { seq: 0, dseq: 0, timer: null }; });
    const a = api();
    const offline = isOffline() || !a || !a.productTypes;

    function load(next, append) {
      if (offline) return Promise.resolve();
      const qq = (next && next.q != null ? next.q : q).trim();
      const kk = next && next.kind != null ? next.kind : kind;
      const offset = append ? list.nextOffset : 0;
      const seq = ++ctl.seq;
      setLoading(true);
      return a.productTypes({ q: qq, kind: kk, limit: CAT_PAGE, offset: offset }).then(function (res) {
        if (seq !== ctl.seq) return;
        const items = (res && Array.isArray(res.items)) ? res.items : [];
        const page = (res && res.page) || {};
        setList(function (prev) {
          return {
            items: append ? prev.items.concat(items) : items,
            total: page.total != null ? Number(page.total) : offset + items.length,
            hasMore: !!page.hasMore,
            nextOffset: page.nextOffset != null ? Number(page.nextOffset) : offset + items.length,
            loaded: true, q: qq, kind: kk,
          };
        });
        setLoading(false); setListErr("");
      }).catch(function (e) {
        if (seq !== ctl.seq) return;
        setLoading(false); setListErr(catErrorText(e, "The catalog could not be loaded."));
      });
    }
    useEffect(function () {
      load();
      return function () { if (ctl.timer) window.clearTimeout(ctl.timer); };
    }, []);

    function searchNow(e) {
      if (e && e.preventDefault) e.preventDefault();
      if (ctl.timer) { window.clearTimeout(ctl.timer); ctl.timer = null; }
      load({ q: q });
    }
    function onSearch(value) {
      setQ(value);
      if (ctl.timer) window.clearTimeout(ctl.timer);
      ctl.timer = window.setTimeout(function () { ctl.timer = null; load({ q: value }); }, 250);
    }
    function pickKind(k) { setKind(k); load({ kind: k }); }

    function openType(id, prefillCode) {
      setSelected(id); setRetiring(false); setRemoving(null); setActErr("");
      setAttach(prefillCode ? { code: prefillCode, code_type: guessCodeType(prefillCode) } : { code: "", code_type: "other" });
      const seq = ++ctl.dseq;
      if (id == null) { setDetail(null); setDetailErr(""); return Promise.resolve(); }
      setDetailErr("");
      setDetail(function (prev) { return prev && prev.id === id ? prev : null; });
      return a.getProductType(id).then(function (row) {
        if (seq === ctl.dseq) setDetail(row || null);
      }).catch(function (e) {
        if (seq === ctl.dseq) { setDetail(null); setDetailErr(catErrorText(e, "This product type could not be loaded.")); }
      });
    }
    function done(message, selectId, inventoryChanged) {
      setSheet(null); setNotice(message); toast(message, "check");
      if (inventoryChanged && onChanged) Promise.resolve(onChanged()).catch(function () {});
      load();
      if (selectId !== undefined) return openType(selectId);
      return Promise.resolve();
    }

    // Keyboard-wedge scanners type the code and press Enter, which submits this form.
    function runLookup(e) {
      if (e && e.preventDefault) e.preventDefault();
      const code = normCode(scan);
      if (!code) return;
      setLookup(null); setNotice("");
      a.lookupProductBarcode(code).then(function (res) {
        if (res && res.found && res.productType) {
          setScan("");
          setLookup({ code: code, found: true, name: typeTitle(res.productType) });
          openType(Number(res.productType.id));
        } else {
          setLookup({ code: code, found: false });
        }
      }).catch(function (err) { setLookup({ code: code, error: catErrorText(err, "Lookup failed.") }); });
    }

    function retire() {
      const t = detail; if (!t) return;
      setBusy(true); setActErr("");
      a.retireProductType(t.id).then(function () {
        setBusy(false); setRetiring(false);
        return done("Retired " + typeTitle(t), null, true);
      }).catch(function (e) {
        setBusy(false); setActErr(catErrorText(e, "Retire failed."));
        if (e && e.code === "not_found") { openType(null); load(); }
      });
    }
    function attachCode(e) {
      if (e && e.preventDefault) e.preventDefault();
      const t = detail; if (!t) return;
      const code = normCode(attach.code);
      const bad = codeError(code, attach.code_type);
      if (bad) { setActErr(bad); return; }
      setBusy(true); setActErr("");
      a.addProductBarcode(t.id, code, attach.code_type).then(function () {
        setBusy(false); setNotice("Attached " + code + " to " + typeTitle(t)); toast("Code attached", "check");
        if (lookup && lookup.code === code) setLookup(null);
        return openType(t.id);
      }).catch(function (err) {
        setBusy(false); setActErr(catErrorText(err, "Attach failed."));
        if (err && err.code === "duplicate_barcode" && a.lookupProductBarcode) {
          a.lookupProductBarcode(code).then(function (res) {
            if (res && res.found && res.productType) setActErr(code + " is already attached to “" + typeTitle(res.productType) + "”. Remove it there first if it belongs here.");
          }).catch(function () {});
        }
      });
    }
    function removeCode(b) {
      const t = detail; if (!t) return;
      setBusy(true); setActErr("");
      a.removeProductBarcode(b.id).then(function () {
        setBusy(false); setRemoving(null); setNotice("Removed " + b.code + " from " + typeTitle(t)); toast("Code removed", "check");
        return openType(t.id);
      }).catch(function (err) {
        setBusy(false); setActErr(catErrorText(err, "Remove failed."));
        if (err && err.code === "not_found") openType(t.id);
      });
    }

    const filtered = !!(list.q || list.kind);
    const sel = selected != null ? detail : null;

    function renderLookup() {
      if (!lookup) return null;
      if (lookup.error) return <div className="inv-loc-note inv-loc-note--err" role="alert">{lookup.error}</div>;
      if (lookup.found) return <div className="inv-loc-note inv-loc-note--ok" role="status"><span className="inv-code">{lookup.code}</span> is {lookup.name}.</div>;
      return <div className="inv-loc-note inv-loc-note--warn inv-cat-miss" role="status">
        <span>No product type has the code <span className="inv-code">{lookup.code}</span>. Nothing has been saved.</span>
        {!offline && <div className="inv-cat-miss__acts">
          <button type="button" className="btn-primary" onClick={function () { setSheet({ kind: "create", code: lookup.code }); }}><Icon name="plus" size={14} />New product type with this code</button>
          {sel && <button type="button" className="btn-ghost" onClick={function () { setAttach({ code: lookup.code, code_type: guessCodeType(lookup.code) }); setActErr(""); }}>Use for {typeTitle(sel)}</button>}
          <button type="button" className="btn-ghost" onClick={function () { setLookup(null); }}>Dismiss</button>
        </div>}
      </div>;
    }

    function renderList() {
      if (offline) return <div className="inv-loc__none">The catalog needs a live connection.</div>;
      if (!list.loaded && !listErr) return <div className="inv-loc__none" role="status">Loading the catalog…</div>;
      if (list.loaded && list.items.length === 0) {
        return <div className="inv-loc__none">{filtered
          ? "No product types match" + (list.q ? " “" + list.q + "”" : "") + (list.kind ? " in " + CAT_KIND_LABEL[list.kind] : "") + "."
          : "No product types yet."}</div>;
      }
      return <>
        {list.items.map(function (t) {
          const on = selected === t.id;
          const by = typeByline(t);
          return <button type="button" key={t.id} className={"inv-loc-pick inv-cat-row" + (on ? " on" : "")} aria-current={on ? "true" : undefined} onClick={function () { openType(t.id); }}>
            <span className="inv-loc-hit__text">
              <span className="inv-loc-pick__name">{typeTitle(t)}</span>
              {by && <span className="muted">{by}</span>}
            </span>
            <span className="tag">{CAT_KIND_LABEL[t.kind] || t.kind}</span>
          </button>;
        })}
        {list.hasMore && <button type="button" className="btn-ghost inv-cat-more" disabled={loading} onClick={function () { load(null, true); }}>{loading ? "Loading…" : "Show more (" + list.items.length + " of " + list.total + ")"}</button>}
      </>;
    }

    function renderDetail() {
      if (selected == null) {
        const empty = list.loaded && !filtered && list.total === 0;
        return <div className="inv-loc__empty">
          <Icon name="tag2" size={28} style={{ color: "var(--teal)" }} />
          <h3>{offline ? "Catalog unavailable" : empty ? "No product types yet" : "Choose a product type"}</h3>
          <p className="muted">{offline
            ? "Product types and barcodes load from the live system of record. This view returns when the dashboard reconnects."
            : empty
              ? "A product type says what something is — a make and model, a software title, a consumable. Add one, or scan a barcode to start from its code."
              : "Select a product type to see its specifications and barcodes, or scan a code to jump straight to it."}</p>
          {!offline && <button type="button" className="btn-primary" onClick={function () { setSheet({ kind: "create" }); }}><Icon name="plus" size={14} />New product type</button>}
        </div>;
      }
      const back = <nav className="inv-crumbs" aria-label="Catalog"><button type="button" className="inv-crumb inv-crumb--back" onClick={function () { openType(null); }}>‹ All product types</button></nav>;
      if (!sel) {
        return <section className="inv-loc__card inv-cat__card" aria-label="Product type">
          {back}
          {detailErr ? <div className="inv-loc-note inv-loc-note--err" role="alert">{detailErr}</div> : <div className="inv-loc__none" role="status">Loading…</div>}
        </section>;
      }
      const title = typeTitle(sel);
      const by = [sel.brand, sel.model].filter(function (x) { return x != null && String(x).trim() !== ""; }).join(" · ");
      const specs = specObject(sel.specs);
      const specKeys = Object.keys(specs);
      const codes = Array.isArray(sel.barcodes) ? sel.barcodes : [];
      return <section className="inv-loc__card inv-cat__card" aria-label={"Product type " + title}>
        {back}
        <div className="inv-loc__head">
          <Icon name="tag2" size={24} style={{ color: "var(--teal)", flex: "0 0 auto" }} />
          <div style={{ minWidth: 0 }}>
            <h2>{title}</h2>
            {by && <div className="inv-cat__byline">{by}</div>}
            <div className="inv-loc__meta">
              <span className="tag">{CAT_KIND_LABEL[sel.kind] || sel.kind}</span>
              {sel.category && <span className="tag">{sel.category}</span>}
            </div>
          </div>
        </div>
        {sel.notes && <p className="inv-loc__notes">{sel.notes}</p>}
        <dl className="inv-loc__facts">
          <div><dt>Brand</dt><dd>{sel.brand || "—"}</dd></div>
          <div><dt>Model</dt><dd>{sel.model || "—"}</dd></div>
          <div><dt>Category</dt><dd>{sel.category || "—"}</dd></div>
          <div><dt>Codes</dt><dd>{codes.length}</dd></div>
        </dl>
        {sel.image_url && <a className="inv-cat__link" href={sel.image_url} target="_blank" rel="noopener noreferrer">Open product image ↗</a>}
        {isLegacyLinked(sel) && <div className="inv-loc-note inv-cat__note" role="note">Linked to a component model. Brand, model, notes and kind changes also update that model in Components, provisioning and RackWire.</div>}

        <h3 className="inv-loc__sub">Specifications</h3>
        {specKeys.length === 0
          ? <div className="inv-loc__none">No specifications recorded.</div>
          : <dl className="inv-cat__specs">
              {specKeys.map(function (k) { return <div key={k}><dt>{k}</dt><dd>{specDisplay(specs[k])}</dd></div>; })}
            </dl>}
        {sel.kind === "software" && <div className="inv-loc-note inv-cat__note" role="note">Licence seats, expiry and the licence reference are recorded on each item. Licence keys are never stored here.</div>}
        {inv && <TypeItems inv={inv} h={h} typeId={sel.id} />}

        <h3 className="inv-loc__sub">Barcodes</h3>
        {codes.length === 0 && <div className="inv-loc__none">No codes attached. Scanning one of this product’s barcodes will then find it.</div>}
        {codes.length > 0 && <ul className="inv-cat-codes" aria-label={"Codes for " + title}>
          {codes.map(function (b) {
            return <li key={b.id}>
              <span className="inv-cat-codes__code"><span className="inv-code">{b.code}</span><span className="tag">{CODE_TYPE_LABEL[b.code_type] || b.code_type}</span></span>
              {!offline && (removing === b.id
                ? <span className="inv-ctype__confirm" role="group" aria-label={"Confirm removing " + b.code}>
                    <span>Remove this code?</span>
                    <button type="button" className="btn-ghost inv-btn-danger" disabled={busy} onClick={function () { removeCode(b); }}>Remove</button>
                    <button type="button" className="btn-ghost" onClick={function () { setRemoving(null); }}>Keep</button>
                  </span>
                : <button type="button" className="btn-ghost" aria-label={"Remove code " + b.code} onClick={function () { setRemoving(b.id); setRetiring(false); }}>Remove</button>)}
            </li>;
          })}
        </ul>}
        {actErr && <div className="inv-loc-note inv-loc-note--err inv-cat__err" role="alert">{actErr}</div>}
        {!offline && <form className="inv-cat-attach" onSubmit={attachCode}>
          <label className="inv-field inv-field--grow"><span>Attach a code</span><input value={attach.code} maxLength={128} autoComplete="off" autoCapitalize="characters" spellCheck={false} enterKeyHint="done" onKeyDown={holdEnter} onChange={function (e) { setAttach({ code: e.target.value, code_type: attach.code_type }); }} placeholder="Scan or type" /></label>
          <label className="inv-field"><span>Code type</span>
            <select value={attach.code_type} onChange={function (e) { setAttach({ code: attach.code, code_type: e.target.value }); }}>
              {CODE_TYPES.map(function (c) { return <option key={c} value={c}>{CODE_TYPE_LABEL[c]}</option>; })}
            </select></label>
          <button type="submit" className="btn-ghost" disabled={busy}><Icon name="plus" size={14} />Attach</button>
        </form>}

        {offline
          ? <div className="inv-loc-note" role="note">Changes need a live connection. This view is read-only until the dashboard reconnects.</div>
          : <div className="inv-loc__actions" role="toolbar" aria-label={"Actions for " + title}>
              {retiring
                ? <div className="inv-cat-retire" role="group" aria-label={"Confirm retiring " + title}>
                    <p>Retire {title}? Items already recorded keep this type; it leaves pick lists{codes.length ? " and its " + codes.length + " code" + (codes.length === 1 ? " is" : "s are") + " released for reuse" : ""}.</p>
                    <div className="inv-cat-retire__acts">
                      <button type="button" className="btn-ghost inv-btn-danger" disabled={busy} onClick={retire}>{busy ? "Retiring…" : "Retire product type"}</button>
                      <button type="button" className="btn-ghost" onClick={function () { setRetiring(false); }}>Keep it</button>
                    </div>
                  </div>
                : <>
                    <button type="button" className="btn-primary" onClick={function () { setSheet({ kind: "items", type: sel }); }}><Icon name="plus" size={14} />Add item(s)</button>
                    <button type="button" className="btn-ghost" onClick={function () { setSheet({ kind: "edit", type: sel }); }}><Icon name="edit" size={14} />Edit</button>
                    <button type="button" className="btn-ghost inv-btn-danger" onClick={function () { setRetiring(true); setRemoving(null); }}><Icon name="trash" size={14} />Retire…</button>
                  </>}
            </div>}
      </section>;
    }

    return (
      <div className="inv-locs inv-cat">
        <div className="filters inv-loc-bar inv-cat-bar">
          <form className="search inv-loc-search inv-cat-search" role="search" onSubmit={searchNow}>
            <Icon name="search" size={15} />
            <input type="search" aria-label="Find a product type by name, brand, model or category" value={q} onChange={function (e) { onSearch(e.target.value); }} placeholder="Find name, brand, model…" disabled={offline} />
          </form>
          <form className="inv-cat-scan" role="search" aria-label="Barcode lookup" onSubmit={runLookup}>
            <div className="search">
              <Icon name="tag2" size={15} />
              <input aria-label="Scan or type a barcode" value={scan} autoComplete="off" autoCapitalize="characters" spellCheck={false} enterKeyHint="search" onChange={function (e) { setScan(e.target.value); }} placeholder="Scan or type a barcode" disabled={offline} />
            </div>
            <button type="submit" className="btn-ghost" disabled={offline || !scan.trim()}>Look up</button>
          </form>
          {!offline && <button type="button" className="btn-primary inv-loc-new" onClick={function () { setSheet({ kind: "create" }); }}><Icon name="plus" size={14} />New product type</button>}
        </div>
        <div className="filters inv-cat-kinds" role="group" aria-label="Filter by kind">
          {[""].concat(CAT_KINDS).map(function (k) {
            return <button type="button" key={k || "all"} className={"chip" + (kind === k ? " on" : "")} aria-pressed={kind === k} disabled={offline} onClick={function () { pickKind(k); }}>{k ? CAT_KIND_LABEL[k] : "All kinds"}</button>;
          })}
        </div>
        {renderLookup()}
        {notice && <div className="inv-loc-note inv-loc-note--ok" role="status">{notice}</div>}
        {listErr && <div className="inv-loc-note inv-loc-note--err" role="alert">{listErr}</div>}

        <div className="inv-loc" data-detail={selected != null ? "1" : "0"}>
          <div className="inv-loc__tree inv-cat__list" aria-label="Product types">
            <div className="inv-loc__treehead" aria-live="polite">{offline ? "Unavailable offline" : loading && !list.loaded ? "Loading…" : list.total + " product type" + (list.total === 1 ? "" : "s") + (filtered ? " found" : "")}</div>
            {renderList()}
          </div>
          <div className="inv-loc__detail">{renderDetail()}</div>
        </div>

        {sheet && sheet.kind === "create" && <ProductTypeSheet prefillCode={sheet.code} onClose={function () { setSheet(null); }} onDone={done} />}
        {sheet && sheet.kind === "edit" && <ProductTypeSheet type={sheet.type} onClose={function () { setSheet(null); }} onDone={done} />}
        {sheet && sheet.kind === "items" && inv && <AddItemsSheet type={sheet.type} inv={inv} h={h} onClose={function () { setSheet(null); }} onDone={done} />}
      </div>
    );
  }

  // Items already recorded on a product type (from the inventory snapshot).
  function TypeItems({ inv, h, typeId }) {
    const items = inv.units.filter(function (u) { return Number(u.product_type_id) === Number(typeId); });
    return <>
      <h3 className="inv-loc__sub">Items ({items.length})</h3>
      {items.length === 0
        ? <div className="inv-loc__none">No items recorded yet. Use “Add item(s)” to record what you have.</div>
        : <ul className="inv-loc__units" aria-label="Items of this product type">
            {items.slice(0, 25).map(function (u) {
              const lic = licenceSummary(u);
              return <li key={u.id}><span>{u.asset_tag || u.serial || ("Item #" + u.id)}{(u.quantity || 1) > 1 ? " × " + u.quantity : ""}{lic ? " · " + lic : ""}</span><span className="td-mono muted">{h.locLabel(u.location_id)}</span></li>;
            })}
            {items.length > 25 && <li className="muted">…and {items.length - 25} more in Components</li>}
          </ul>}
    </>;
  }

  // Record items of one product type (037): N identical items in one call, or
  // for consumables one item that carries the quantity. Serial and asset tag
  // belong to a single item, so they are offered only when adding one.
  function AddItemsSheet({ type, inv, h, onClose, onDone }) {
    const consumable = type.kind === "consumable";
    const software = type.kind === "software";
    const [qty, setQty] = useState("1");
    const [locId, setLocId] = useState(null);
    const [serial, setSerial] = useState("");
    const [assetTag, setAssetTag] = useState("");
    const [notes, setNotes] = useState("");
    const [lic, setLic] = useState(function () { return licenceState(null); });
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const title = typeTitle(type);
    const qtyText = String(qty).trim();
    const validN = /^\d+$/.test(qtyText) && Number(qtyText) >= 1;
    const n = validN ? Number(qtyText) : 0;
    const single = consumable || n <= 1;

    function submit(e) {
      if (e && e.preventDefault) e.preventDefault();
      if (!validN) { setErr("Quantity must be a whole number of 1 or more."); return; }
      if (!consumable && n > ITEM_BATCH_MAX) { setErr("Add up to " + ITEM_BATCH_MAX + " items at a time."); return; }
      const lb = licenceBody(lic, software);
      if (lb.error) { setErr(lb.error); return; }
      const body = Object.assign({ product_type_id: Number(type.id), location_id: locId != null ? Number(locId) : null, notes: notes.trim() || null }, software ? lb.body : {});
      if (consumable) { body.count = 1; body.quantity = n; } else body.count = n;
      if (single) { body.serial = serial.trim() || null; body.asset_tag = assetTag.trim() || null; }
      setBusy(true); setErr("");
      api().createUnit(body).then(function (res) {
        const made = res && res.count != null ? Number(res.count) : 1;
        onDone("Added " + (consumable ? n + " × " + title : made === 1 ? "1 item of " + title : made + " items of " + title), type.id, true);
      }).catch(function (error) { setBusy(false); setErr(itemErrorText(error, "Nothing was added.")); });
    }

    const addLabel = !consumable && n > 1 ? "Add " + n + " items" : "Add item";
    return (
      <Sheet title={"Add " + title} icon="box" wide sub={(CAT_KIND_LABEL[type.kind] || type.kind) + " · items appear in Components"} onClose={onClose}
        footer={<><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button><button type="button" className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />{busy ? "Adding…" : addLabel}</button></>}>
        <form className="inv-form" onSubmit={submit}>
          <SheetError text={err} />
          <label className="inv-field"><span>{consumable ? "Quantity" : "How many items"}</span><input type="number" inputMode="numeric" min="1" max={consumable ? undefined : ITEM_BATCH_MAX} step="1" value={qty} onChange={function (e) { setQty(e.target.value); }} /></label>
          {consumable
            ? <div className="muted inv-cat__hint">Consumables are counted on one item.</div>
            : n > 1 && <div className="muted inv-cat__hint">Adds {n} separate items. Serial numbers and asset tags are recorded one item at a time — edit each one in Components.</div>}
          {single && <div className="inv-form__row">
            <label className="inv-field"><span>Serial (optional)</span><input value={serial} maxLength={128} autoComplete="off" spellCheck={false} onKeyDown={holdEnter} onChange={function (e) { setSerial(e.target.value); }} placeholder="Manufacturer serial" /></label>
            <label className="inv-field"><span>Asset tag (optional)</span><input value={assetTag} maxLength={64} autoComplete="off" spellCheck={false} onKeyDown={holdEnter} onChange={function (e) { setAssetTag(e.target.value); }} placeholder="Internal tag" /></label>
          </div>}
          <PickList label="Location" options={locationOptions(inv, h)} value={locId} onChange={setLocId} noneLabel="Unassigned" placeholder="Filter locations…" />
          {software && <LicenceFields value={lic} onChange={setLic} options={installedOnOptions(inv, h, null)} />}
          <label className="inv-field"><span>Notes (optional)</span><textarea rows={2} value={notes} onChange={function (e) { setNotes(e.target.value); }} /></label>
        </form>
      </Sheet>
    );
  }

  // Create (optionally with a prefilled, uncommitted barcode) or edit a product
  // type. Edits send only changed fields plus updated_at (409 → reload latest).
  function ProductTypeSheet({ type, prefillCode, onClose, onDone }) {
    const editing = !!type;
    const [base, setBase] = useState(type || null);
    const [kind, setKind] = useState(editing ? type.kind : "hardware");
    const [name, setName] = useState(editing ? (type.name || "") : "");
    const [brand, setBrand] = useState(editing ? (type.brand || "") : "");
    const [model, setModel] = useState(editing ? (type.model || "") : "");
    const [category, setCategory] = useState(editing ? (type.category || "") : "");
    const [imageUrl, setImageUrl] = useState(editing ? (type.image_url || "") : "");
    const [notes, setNotes] = useState(editing ? (type.notes || "") : "");
    const [rows, setRows] = useState(function () { return editing ? specRows(type.specs) : []; });
    const [rseq, setRseq] = useState(function () { return 1000; });
    const [code, setCode] = useState(prefillCode || "");
    const [codeType, setCodeType] = useState(prefillCode ? guessCodeType(normCode(prefillCode)) : "other");
    const [busy, setBusy] = useState(false);
    const [err, setErr] = useState("");
    const autoName = catalogName(brand, model);

    function adopt(row) {
      setBase(row); setKind(row.kind); setName(row.name || ""); setBrand(row.brand || ""); setModel(row.model || "");
      setCategory(row.category || ""); setImageUrl(row.image_url || ""); setNotes(row.notes || ""); setRows(specRows(row.specs));
    }
    function setRow(rid, patch) { setRows(rows.map(function (r) { return r.rid === rid ? Object.assign({}, r, patch) : r; })); }
    function addRow() { setRows(rows.concat([{ rid: rseq + 1, key: "", text: "", orig: null, origText: "", has: false, nested: false }])); setRseq(rseq + 1); }
    function dropRow(rid) { setRows(rows.filter(function (r) { return r.rid !== rid; })); }

    function submit(e) {
      if (e && e.preventDefault) e.preventDefault();
      const finalName = name.trim() || autoName.trim();
      if (!finalName) { setErr("Give it a name, or a brand and model to name it from."); return; }
      if (finalName.length > 160) { setErr("Names must be 160 characters or fewer."); return; }
      if (brand.trim().length > 128 || model.trim().length > 128 || category.trim().length > 128) { setErr("Brand, model and category must each be 128 characters or fewer."); return; }
      const url = imageUrl.trim();
      if (url && (!/^https?:\/\/\S+$/i.test(url) || url.length > 1024)) { setErr("The image link must be an http:// or https:// address."); return; }
      const spec = specsFromRows(rows);
      if (spec.error) { setErr(spec.error); return; }
      const fields = { kind: kind, name: finalName, brand: brand.trim() || null, model: model.trim() || null, category: category.trim() || null, image_url: url || null, notes: notes.trim() || null };
      const a = api();
      let call;
      if (editing) {
        const body = {};
        Object.keys(fields).forEach(function (k) {
          const was = base[k] == null || String(base[k]).trim() === "" ? null : String(base[k]).trim();
          if (fields[k] !== was) body[k] = fields[k];
        });
        const baseSpecs = Object.keys(specObject(base.specs)).length ? specObject(base.specs) : null;
        if (stable(spec.specs) !== stable(baseSpecs)) body.specs = spec.specs;
        if (!Object.keys(body).length) { onClose(); toast("No changes to save", "check"); return; }
        body.updated_at = base.updated_at || "";
        call = a.updateProductType(base.id, body);
      } else {
        const body = Object.assign({}, fields);
        if (spec.specs) body.specs = spec.specs;
        const c = normCode(code);
        if (c) {
          const bad = codeError(c, codeType);
          if (bad) { setErr(bad); return; }
          body.barcode = { code: c, code_type: codeType };
        }
        call = a.createProductType(body);
      }
      setBusy(true); setErr("");
      call.then(function (row) {
        const id = row && row.id != null ? Number(row.id) : (editing ? base.id : undefined);
        onDone((editing ? "Saved " : "Added ") + finalName, id, editing);
      }).catch(function (error) {
        setBusy(false);
        setErr(error && error.code === "duplicate_barcode"
          ? "The code " + normCode(code) + " is already attached to another product type, so nothing was saved. Remove the code here, or look it up to find that type."
          : catErrorText(error, "Save failed."));
        if (editing && error && error.code === "conflict" && a.getProductType) {
          a.getProductType(base.id).then(function (row) { if (row) adopt(row); }).catch(function () {});
        }
      });
    }

    const title = editing ? "Edit " + typeTitle(base) : "New product type";
    return (
      <Sheet title={title} icon="tag2" wide sub={editing ? (CAT_KIND_LABEL[base.kind] || base.kind) + (base.brand ? " · " + base.brand : "") : "Describe what this thing is. Individual items are recorded in Components."} onClose={onClose}
        footer={<><button type="button" className="btn-ghost" onClick={onClose}>Cancel</button><button type="button" className="btn-primary" disabled={busy} onClick={submit}><Icon name="check" size={14} />{busy ? "Saving…" : (editing ? "Save changes" : "Add product type")}</button></>}>
        <form className="inv-form" onSubmit={submit}>
          <SheetError text={err} />
          {editing && isLegacyLinked(base) && <div className="inv-loc-note" role="note">Linked to a component model — brand, model, notes and kind changes update it too.</div>}
          <div className="inv-form__row">
            <label className="inv-field"><span>Brand</span><input value={brand} maxLength={128} onChange={function (e) { setBrand(e.target.value); }} placeholder="Raspberry Pi" /></label>
            <label className="inv-field"><span>Model</span><input value={model} maxLength={128} onChange={function (e) { setModel(e.target.value); }} placeholder="400" /></label>
          </div>
          <label className="inv-field"><span>Display name</span><input value={name} maxLength={160} onChange={function (e) { setName(e.target.value); }} placeholder={autoName || "Name shown everywhere"} /></label>
          {!name.trim() && autoName && <div className="muted inv-cat__hint">Will be saved as “{autoName}”.</div>}
          {name.trim() && brand.trim() && name.trim() === model.trim() && autoName !== name.trim() && (
            <div className="muted inv-cat__hint inv-cat__hint--act">Lists show this as “{autoName}”.
              <button type="button" className="btn-ghost" onClick={function () { setName(autoName); }}>Use “{autoName}”</button>
            </div>
          )}
          <div className="inv-form__row">
            <label className="inv-field"><span>Kind</span>
              <select value={kind} onChange={function (e) { setKind(e.target.value); }}>
                {CAT_KINDS.map(function (k) { return <option key={k} value={k}>{CAT_KIND_LABEL[k]}</option>; })}
              </select></label>
            <label className="inv-field inv-field--grow"><span>Category (optional)</span><input value={category} maxLength={128} onChange={function (e) { setCategory(e.target.value); }} placeholder="Single-board computer" /></label>
          </div>
          {kind === "software" && <div className="inv-loc-note inv-cat__note" role="note">Licence seats, expiry and the licence reference belong on each item. Don’t put licence keys here — they are refused.</div>}

          <fieldset className="inv-cat-specs">
            <legend>Specifications</legend>
            {rows.length === 0 && <div className="muted inv-cat__hint">None yet — add things like RAM, ports or wattage.</div>}
            {rows.map(function (r, i) {
              return <div key={r.rid} className="inv-cat-spec">
                <input aria-label={"Spec " + (i + 1) + " name"} value={r.key} maxLength={64} autoCapitalize="none" spellCheck={false} onChange={function (e) { setRow(r.rid, { key: e.target.value }); }} placeholder="ram" />
                <input aria-label={"Spec " + (i + 1) + " value"} className={r.nested ? "inv-cat-spec__json" : ""} value={r.text} onChange={function (e) { setRow(r.rid, { text: e.target.value }); }} placeholder="4 GB" />
                <button type="button" className="iconbtn inv-cat-spec__drop" aria-label={"Remove spec " + (r.key.trim() || i + 1)} onClick={function () { dropRow(r.rid); }}><Icon name="close" size={14} /></button>
                {r.nested && <div className="muted inv-cat-spec__note">Structured value — edit as JSON.</div>}
              </div>;
            })}
            <button type="button" className="btn-ghost" onClick={addRow}><Icon name="plus" size={14} />Add spec</button>
          </fieldset>

          <label className="inv-field"><span>Image link (optional)</span><input type="url" inputMode="url" autoCapitalize="none" spellCheck={false} value={imageUrl} maxLength={1024} onChange={function (e) { setImageUrl(e.target.value); }} placeholder="https://…" /></label>
          <label className="inv-field"><span>Notes (optional)</span><textarea rows={3} value={notes} onChange={function (e) { setNotes(e.target.value); }} /></label>
          {!editing && <div className="inv-form__row">
            <label className="inv-field inv-field--grow"><span>Barcode (optional)</span><input value={code} maxLength={128} autoComplete="off" autoCapitalize="characters" spellCheck={false} onKeyDown={holdEnter} onChange={function (e) { setCode(e.target.value); }} placeholder="Scan or type" /></label>
            <label className="inv-field"><span>Code type</span>
              <select value={codeType} onChange={function (e) { setCodeType(e.target.value); }}>
                {CODE_TYPES.map(function (c) { return <option key={c} value={c}>{CODE_TYPE_LABEL[c]}</option>; })}
              </select></label>
          </div>}
          {!editing && prefillCode && <div className="muted inv-cat__hint">Prefilled from your scan. Nothing is saved until you add the product type.</div>}
        </form>
      </Sheet>
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
            <Icon name="box" size={14} style={{ color: "var(--teal)" }} /><span style={{ fontWeight: 600 }}>{h.unitLabel(u)}</span>
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
      const label = unit ? h.unitLabel(unit) : (a.hardware_model_id ? h.modelLabel(a.hardware_model_id) : "—");
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
              {h.unitLabel(o.u)}{o.u.serial ? " · " + o.u.serial : ""}{other ? "  (committed: " + other.name + ")" : ""}
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
              {inv.models.map(function (m) { return <option key={m.id} value={String(m.id)}>{catalogName(m.make, m.model)} · {m.hw_type}</option>; })}
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
    const trays = inv.locations.filter(function (l) { return l.loc_type === "storage_unit"; });
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
