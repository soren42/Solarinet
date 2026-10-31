/* SolariNet — Provisioning · Images & Stacks (P4).
   Implements docs/mockups/provisioning-images-stacks.html against the P3
   /api/provisioning routes: a target band (SoR machine → MAC + arch), the
   image tree (distro → release → arch), the software stacks, ONE sticky
   Deploy (reimage → PROV_STAGE, apply-only → STACK_APPLY with a dry-run
   first), recent runs + helper log tails, and the software-pool view.
   Every write is operator-gated server-side and queued through the ctl
   request queue; nothing here reaches the host directly. Specs are edited as
   structured fields — the SoR is the source of truth and the backend renders
   Ansible vars; no YAML is ever edited in the browser. */
(function () {
  const { useState, useEffect, useMemo, useRef, useCallback } = React;
  const Icon = window.Icon;

  function api() { return (window.SOLARI && window.SOLARI.api) || null; }
  function isLive() { return !!(window.SOLARI && window.SOLARI.source === "live"); }
  function canOp() { return !!(window.solariCanOperate && window.solariCanOperate()); }
  const OP_ONLY = "Operator role required";

  // ------------------------------------------------------------ vocabulary
  const FAMS = ["suse", "debian", "fedora"];
  const FAM_LABEL = { suse: "SUSE", debian: "Debian / Ubuntu", fedora: "Fedora" };
  const ARCHES = ["x86_64", "arm64", "armhf", "riscv64", "i386"];
  const ART_ARCHES = ["any"].concat(ARCHES);
  const ITEM_TYPES = ["pkg", "repo", "artifact", "script", "build", "role"];
  const TYPE_RANK = { repo: 1, pkg: 2, artifact: 3, build: 4, script: 5, role: 6 };
  const TYPE_HELP = {
    pkg: "Distro package — a real name per family; blank = unsupported there.",
    repo: "Third-party repository + signing key, added before packages.",
    artifact: "Upstream release binary / tarball, per arch, sha256-pinned.",
    script: "Install script: inline body, or a remote URL with a mandatory sha256. Needs an idempotency guard.",
    build: "Source build from a pinned git commit or checksummed tarball; runs unprivileged into a DESTDIR stage.",
    role: "Existing Ansible role.",
  };
  const BUILD_SYSTEMS = ["cmake", "make", "autotools", "meson", "custom"];
  const BOOT = { ipxe: ["iPXE", "teal"], pi_netboot: ["Pi netboot", ""], media: ["Write to media", ""] };
  const BOOT_LONG = { ipxe: "iPXE (kernel + initrd)", pi_netboot: "Pi netboot", media: "Write to media" };
  const REFRESH = { manual: "Manual", weekly: "Weekly (rolling)", point_release: "On point release" };
  // Install templates shipped in deploy/fleet/netboot/configs/ (free text
  // is still accepted — the server validates the file-name shape).
  const TEMPLATES = ["opensuse.autoyast.xml.tmpl", "debian.preseed.tmpl", "ubuntu.autoinstall.tmpl"];
  const DISTRO_LABEL = {
    opensuse: "openSUSE", debian: "Debian", ubuntu: "Ubuntu", fedora: "Fedora",
    raspios: "Raspberry Pi OS", rocky: "Rocky Linux", alma: "AlmaLinux", centos: "CentOS Stream",
  };
  const DISTRO_FAMILY = { opensuse: "suse", debian: "debian", ubuntu: "debian", raspios: "debian", fedora: "fedora", rocky: "fedora", alma: "fedora", centos: "fedora" };
  function distroLabel(d) { return DISTRO_LABEL[d] || (d ? d.charAt(0).toUpperCase() + d.slice(1) : "—"); }
  function imageLabel(img) {
    if (!img) return "";
    return distroLabel(img.distro) + " " + (img.release_label || img.release_name) + " " + img.arch;
  }

  // Stale rule (client-side, shown in the tag tooltip): rolling images go
  // stale after 8 days, point-release images after 45; manual images are
  // only ever "not fetched". Tunable — see the P4 return packet.
  const STALE_DAYS = { weekly: 8, point_release: 45 };
  function imageFreshness(img) {
    if (!img.fetched) return { state: "none", label: "not fetched", why: "No verified artifact yet — refresh to fetch it." };
    const days = STALE_DAYS[img.refresh_policy];
    const at = Date.parse(img.fetched_at);
    if (days && Number.isFinite(at)) {
      const age = (Date.now() - at) / 86400000;
      if (age > days) {
        return { state: "stale", label: "stale", why: "Fetched " + Math.floor(age) + " days ago; " + REFRESH[img.refresh_policy].toLowerCase() + " images go stale after " + days + " days." };
      }
    }
    return { state: "ok", label: "sha256 ✓", why: "Checksum verified at fetch: " + img.sha256 };
  }

  function fmtDate(iso) { return iso ? String(iso).slice(0, 10) : "—"; }
  function fmtWhen(iso) {
    if (!iso) return "—";
    const d = new Date(iso);
    if (!Number.isFinite(d.getTime())) return String(iso);
    const p = (n) => String(n).padStart(2, "0");
    return d.getFullYear() + "-" + p(d.getMonth() + 1) + "-" + p(d.getDate()) + " " + p(d.getHours()) + ":" + p(d.getMinutes());
  }
  function errText(e) { return (e && e.message) || "Request failed"; }
  function shortHost(n) { return String(n || "").split(".")[0].toLowerCase(); }

  // Stack closure + order — mirrors deploy/fleet/stackgen.py resolve_order():
  // requested + every locked stack + transitive depends_on; topological, every
  // unlocked stack implicitly after every locked one; ties by (locked first,
  // sort_order, slug). The generator re-resolves server-side; this is the
  // preview the operator confirms.
  function resolveOrder(stacks, selected) {
    const by = {};
    stacks.forEach((s) => { by[s.slug] = s; });
    const locked = stacks.filter((s) => s.locked).map((s) => s.slug).sort();
    const closure = new Set();
    const todo = selected.concat(locked);
    while (todo.length) {
      const slug = todo.pop();
      if (closure.has(slug) || !by[slug]) continue;
      closure.add(slug);
      (by[slug].depends_on || []).forEach((d) => todo.push(d));
    }
    const deps = {};
    closure.forEach((s) => {
      const d = new Set((by[s].depends_on || []).filter((x) => closure.has(x)));
      if (!by[s].locked) locked.forEach((l) => { if (closure.has(l)) d.add(l); });
      deps[s] = d;
    });
    const key = (s) => [by[s].locked ? 0 : 1, by[s].sort_order || 0, s];
    const cmp = (a, b) => { const x = key(a), y = key(b); return x[0] - y[0] || x[1] - y[1] || (x[2] < y[2] ? -1 : x[2] > y[2] ? 1 : 0); };
    const order = [];
    const done = new Set();
    while (order.length < closure.size) {
      const ready = Array.from(closure).filter((s) => !done.has(s) && Array.from(deps[s]).every((d) => done.has(d)));
      if (!ready.length) return { order: order, cycle: Array.from(closure).filter((s) => !done.has(s)) };
      ready.sort(cmp);
      order.push(ready[0]);
      done.add(ready[0]);
    }
    return { order: order, cycle: null };
  }

  // Per-family readiness of one stack from its members + the pool rows,
  // mirroring stackgen's rules: an entry whose applies_families excludes a
  // family is skipped there; otherwise a null family name is a hard
  // "unmapped" error at render time.
  function stackCompat(members, swById, arch) {
    const out = {};
    FAMS.forEach((fam) => {
      const unmapped = [];
      let applicable = 0;
      (members || []).forEach((m) => {
        const sw = swById[m.software_id];
        if (!sw) return;
        if (sw.applies_families && sw.applies_families.indexOf(fam) < 0) return;
        if (arch && sw.applies_arches && sw.applies_arches.indexOf(arch) < 0) return;
        applicable++;
        if (sw["name_" + fam] == null) unmapped.push(sw.logical_name);
      });
      out[fam] = { applicable: applicable, unmapped: unmapped };
    });
    let archSkipped = [];
    if (arch) {
      (members || []).forEach((m) => {
        const sw = swById[m.software_id];
        if (sw && sw.applies_arches && sw.applies_arches.indexOf(arch) < 0) archSkipped.push(sw.logical_name);
      });
    }
    return { fams: out, archSkipped: archSkipped };
  }

  // A fetch with loading / error / last-good data. reload() returns a promise.
  function useResource(loader) {
    const [st, setSt] = useState({ data: null, error: null, loading: true });
    const seq = useRef(0);
    const ref = useRef(loader);
    ref.current = loader;
    const reload = useCallback(() => {
      const n = ++seq.current;
      setSt((s) => ({ data: s.data, error: null, loading: true }));
      return Promise.resolve().then(() => ref.current()).then(
        (d) => { if (n === seq.current) setSt({ data: d, error: null, loading: false }); return d; },
        (e) => { if (n === seq.current) setSt((s) => ({ data: s.data, error: e, loading: false })); }
      );
    }, []);
    useEffect(() => { reload(); }, [reload]);
    return [st, reload];
  }

  // ------------------------------------------------------------ primitives
  function ErrorBox({ error, onRetry }) {
    if (!error) return null;
    return (
      <div className="prov-err" role="alert">
        {error.code && <b>{String(error.code).replace(/_/g, " ")}</b>}
        {errText(error)}
        {onRetry && <> · <button type="button" className="linkbtn" onClick={onRetry}>Retry</button></>}
      </div>
    );
  }

  function IconBtn({ icon, label, onClick, danger, disabled, title, busy }) {
    return (
      <button type="button" className={"prov-ib" + (danger ? " danger" : "") + (busy ? " spin" : "")}
        onClick={onClick} disabled={disabled} title={title || label} aria-label={label}>
        <Icon name={icon} size={16} />
      </button>
    );
  }

  function Skeleton({ rows }) {
    return <div aria-busy="true" aria-label="Loading">{Array.from({ length: rows || 3 }).map((_, i) => <div key={i} className="prov-skel" />)}</div>;
  }

  // Focus is moved into a dialog on open and restored to the opener on close.
  function useDialogFocus(onClose) {
    const box = useRef(null);
    useEffect(() => {
      const opener = document.activeElement;
      const el = box.current;
      if (el) {
        const first = el.querySelector("[data-autofocus]") || el.querySelector("input:not([disabled]),select:not([disabled]),textarea:not([disabled]),button:not([disabled])");
        if (first) first.focus({ preventScroll: true });
      }
      function onKey(e) {
        if (e.key === "Escape") { e.stopPropagation(); onClose(); return; }
        if (e.key !== "Tab" || !box.current) return;
        const f = Array.from(box.current.querySelectorAll("a[href],button:not([disabled]),input:not([disabled]),select:not([disabled]),textarea:not([disabled]),summary,[tabindex]:not([tabindex='-1'])"));
        if (!f.length) return;
        const a = f[0], z = f[f.length - 1];
        if (e.shiftKey && document.activeElement === a) { e.preventDefault(); z.focus(); }
        else if (!e.shiftKey && document.activeElement === z) { e.preventDefault(); a.focus(); }
      }
      window.addEventListener("keydown", onKey, true);
      return () => {
        window.removeEventListener("keydown", onKey, true);
        if (opener && opener.focus && document.contains(opener)) opener.focus({ preventScroll: true });
      };
    }, []);
    return box;
  }

  // Right-anchored editor sheet (the mockup's drawer).
  let dialogSeq = 0;
  function Drawer({ title, sub, icon, onClose, children, footer }) {
    const box = useDialogFocus(onClose);
    const [id] = useState(() => "prov-dlg-" + (++dialogSeq));
    return (
      <div className="modal-overlay prov-drawer-overlay" onMouseDown={(e) => { if (e.target === e.currentTarget) onClose(); }}>
        <div className="modal prov-drawer" role="dialog" aria-modal="true" aria-labelledby={id} ref={box}>
          <div className="modal__head">
            {icon && <Icon name={icon} size={20} className="prov-ico" />}
            <div style={{ flex: 1, minWidth: 0 }}>
              <h2 id={id}>{title}</h2>
              {sub && <div className="td-mono muted" style={{ fontSize: 11, marginTop: 2 }}>{sub}</div>}
            </div>
            <button type="button" className="prov-ib" onClick={onClose} aria-label="Close"><Icon name="close" size={16} /></button>
          </div>
          <div className="modal__body">{children}</div>
          {footer && <div className="modal__foot">{footer}</div>}
        </div>
      </div>
    );
  }

  // Confirm dialog. confirmWord set → type it to arm (destructive grade).
  // onConfirm returns a promise; a rejection is shown inline and the dialog
  // stays open so the operator can retry (same Idempotency-Key upstream).
  function Confirm({ title, sub, danger, children, verb, busyVerb, confirmWord, disabled, onConfirm, onClose }) {
    const box = useDialogFocus(onClose);
    const [id] = useState(() => "prov-cfm-" + (++dialogSeq));
    const [typed, setTyped] = useState("");
    const [busy, setBusy] = useState(false);
    const [error, setError] = useState(null);
    const armed = !disabled && !busy && (!confirmWord || typed === confirmWord);
    function go(e) {
      if (e) e.preventDefault();
      if (!armed) return;
      setBusy(true);
      setError(null);
      Promise.resolve().then(onConfirm).then(() => setBusy(false), (err) => { setBusy(false); setError(err); });
    }
    return (
      <div className="modal-overlay" style={{ zIndex: 1200 }} onMouseDown={(e) => { if (e.target === e.currentTarget && !busy) onClose(); }}>
        <form className="modal" role="alertdialog" aria-modal="true" aria-labelledby={id} ref={box} onSubmit={go}
          style={{ width: "min(520px, 94vw)", borderColor: danger ? "var(--crit)" : undefined }}>
          <div className="modal__head">
            <Icon name={danger ? "trash" : "deploy"} size={20} style={{ color: danger ? "var(--crit)" : "var(--teal)" }} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div id={id} style={{ fontWeight: 700, fontSize: 15 }}>{title}</div>
              {sub && <div className="td-mono muted" style={{ fontSize: 11 }}>{sub}</div>}
            </div>
            <button type="button" className="prov-ib" onClick={onClose} aria-label="Close" disabled={busy}><Icon name="close" size={16} /></button>
          </div>
          <div className="modal__body" style={{ fontSize: 13, lineHeight: 1.55 }}>
            {children}
            {confirmWord && (
              <div className="prov-field" style={{ marginTop: 14 }}>
                <label className="prov-kicker" htmlFor={id + "-w"}>Type <span className="mono" style={{ color: "var(--ink)", letterSpacing: 0, textTransform: "none" }}>{confirmWord}</span> to confirm</label>
                <input id={id + "-w"} className="prov-input mono" autoComplete="off" spellCheck={false} value={typed}
                  data-autofocus onChange={(e) => setTyped(e.target.value)} />
              </div>
            )}
            <ErrorBox error={error} />
          </div>
          <div className="modal__foot">
            <button type="button" className="btn-ghost" onClick={onClose} disabled={busy}>Cancel</button>
            <button type="submit" className={danger ? "btn-danger" : "btn-primary"} disabled={!armed}>{busy ? (busyVerb || "Working…") : verb}</button>
          </div>
        </form>
      </div>
    );
  }

  function Field({ label, hint, htmlFor, children, style }) {
    return (
      <div className="prov-field" style={style}>
        <label className="prov-kicker" htmlFor={htmlFor}>{label}</label>
        {children}
        {hint && <div className="prov-note" style={{ marginTop: 4 }}>{hint}</div>}
      </div>
    );
  }

  // Toggle chips for a SET column (null = all). Unticking every chip = all.
  function SetChips({ label, options, value, onChange, disabled }) {
    const on = value || [];
    return (
      <div className="prov-field">
        <span className="prov-kicker">{label}</span>
        <div className="prov-checks" role="group" aria-label={label}>
          <button type="button" className={"chip" + (!value ? " on" : "")} aria-pressed={!value} disabled={disabled}
            onClick={() => onChange(null)}>all</button>
          {options.map((o) => (
            <button key={o} type="button" className={"chip" + (on.indexOf(o) >= 0 ? " on" : "")} aria-pressed={on.indexOf(o) >= 0}
              disabled={disabled}
              onClick={() => {
                const next = on.indexOf(o) >= 0 ? on.filter((x) => x !== o) : on.concat([o]);
                onChange(next.length ? options.filter((x) => next.indexOf(x) >= 0) : null);
              }}>{o}</button>
          ))}
        </div>
      </div>
    );
  }

  // Field-level error marking: the server names the offending field first
  // ("version_pin is required…", "spec.sha256 must…").
  function errKey(error) {
    const m = error && /^([a-z_]+(?:\.[a-z_]+)*)\b/.exec(errText(error));
    return m ? m[1] : null;
  }
  function invalid(error, key) {
    const k = errKey(error);
    return k ? (k === key || k.indexOf(key + ".") === 0) : false;
  }

  // ======================================================================
  // PART 2 — main screen
  // ======================================================================
  function noApi() { const e = new Error("Live API unavailable."); e.code = "OFFLINE"; return e; }
  function needApi(fn) { return () => { const a = api(); return a && a.provImages ? fn(a) : Promise.reject(noApi()); }; }

  // Stack list + each stack's members (for compat tags and the item count).
  function loadStacks(a) {
    return a.provStacks().then((list) => Promise.all(list.map((s) =>
      a.provStackGet(s.id).then((d) => Object.assign({}, s, { members: d.members || [] }),
        () => Object.assign({}, s, { members: null })))));
  }

  function nodeFor(target) {
    if (!target) return null;
    const want = shortHost(target.name);
    const list = (window.SOLARI && (window.SOLARI.nodes || window.SOLARI.fleet)) || [];
    return list.find((n) => shortHost(n.name || n.hostFqdn) === want) || null;
  }
  function primaryIface(t) {
    const ifs = (t && t.interfaces) || [];
    return ifs.find((i) => i.management) || ifs[0] || null;
  }
  function modelName(t) { return [t.hw_make, t.hw_model].filter(Boolean).join(" "); }
  // Why a reimage cannot run on a target whose CPU arch is unknown, and the fix.
  function archUnknownReason(t) {
    return "Target CPU architecture unknown — " + (t.hw_model_id
      ? "set the CPU architecture on hardware model " + modelName(t) + " (Inventory) or let the agent report it."
      : "give " + t.name + " a hardware model with a CPU architecture (Inventory) or let the agent report it.");
  }
  const RUN_TAG = { succeeded: "ok", failed: "crit", running: "teal", queued: "", cancelled: "" };
  const RUN_DONE = { succeeded: true, failed: true, cancelled: true };
  const RUN_LOG_KIND = { reimage: "prov-stage", apply: "stack-apply", refresh: "image-refresh" };
  // Log id for a run: refresh logs are per image, the others per entity.
  function runLogId(r) { return r.mode === "refresh" ? r.image_id : r.entity_id; }
  function runLogTitle(r) {
    if (r.mode === "refresh") return "Refresh log · " + (r.image_label || "image " + r.image_id);
    return (r.mode === "reimage" ? "Stage" : "Apply") + " log · " + (r.entity_name || r.entity_id);
  }
  function maxRunId(runs) { return (runs || []).reduce((n, r) => Math.max(n, Number(r.id) || 0), 0); }

  // Set hardware_models.arch (PUT /api/inventory/models/{id}) — the first
  // arch source provisioning trusts. "" clears it back to unknown.
  function ModelArchDialog({ target, say, onClose, onSaved }) {
    const [arch, setArch] = useState(target.arch_source === "hardware_model" ? target.arch : "");
    const model = modelName(target) || "model " + target.hw_model_id;
    return (
      <Confirm title={"CPU architecture of " + model} sub={"hardware model #" + target.hw_model_id + " · Inventory"}
        verb="Save" busyVerb="Saving…"
        onConfirm={() => api().updateModel(target.hw_model_id, { arch: arch }).then(() => {
          say("Hardware model " + model + ": arch " + (arch || "unknown"), "check");
          onSaved();
          onClose();
        })} onClose={onClose}>
        <Field label="CPU architecture" htmlFor="prov-model-arch"
          hint={"Applies to every machine of this model. " + (target.agent_arch ? "The agent on " + target.name + " reports " + target.agent_arch + "." : "No agent report for " + target.name + ".")}>
          <select id="prov-model-arch" className="prov-input" value={arch} data-autofocus onChange={(e) => setArch(e.target.value)}>
            <option value="">unknown</option>
            {ARCHES.map((x) => <option key={x} value={x}>{x}</option>)}
          </select>
        </Field>
      </Confirm>
    );
  }

  function ProvImagesScreen({ toast, onNavigate }) {
    const say = useCallback((m, i) => {
      if (toast) toast(m, i); else if (window.__solariToast) window.__solariToast(m, i);
    }, [toast]);
    const [tab, setTab] = useState("deploy");
    const [imagesR, reloadImages] = useResource(needApi((a) => a.provImages()));
    const [stacksR, reloadStacks] = useResource(needApi(loadStacks));
    const [softR, reloadSoft] = useResource(needApi((a) => a.provSoftware()));
    const [targetsR, reloadTargets] = useResource(needApi((a) => a.provTargets()));
    const [runsR, reloadRuns] = useResource(needApi((a) => a.provRuns("?limit=25")));

    const [targetId, setTargetId] = useState("");
    const [mode, setMode] = useState("reimage");
    const [imageId, setImageId] = useState(null);
    const [picked, setPicked] = useState(() => new Set());
    const [onlyCompat, setOnlyCompat] = useState(false);
    const [runsMine, setRunsMine] = useState(false);
    const [collapsed, setCollapsed] = useState(() => new Set());
    const [modal, setModal] = useState(null);
    const [refreshing, setRefreshing] = useState({});   // imageId -> { at, fetched_at, afterRun }
    const [ifaceMac, setIfaceMac] = useState("");
    const [pollUntil, setPollUntil] = useState(0);
    const op = canOp();
    const live = isLive();

    const images = imagesR.data || [];
    const stacks = stacksR.data || [];
    const software = softR.data || [];
    const targets = targetsR.data || [];
    const runs = runsR.data || [];
    const swById = useMemo(() => { const m = {}; software.forEach((s) => { m[s.id] = s; }); return m; }, [software]);
    const target = targets.find((t) => t.id === targetId) || null;
    const tArch = target ? target.arch : null;
    const node = nodeFor(target);
    // Netboot interface: the operator's pick, else the management one.
    const iface = (target && target.interfaces.find((i) => i.mac === ifaceMac)) || primaryIface(target);

    function reloadAll() { reloadImages(); reloadStacks(); reloadSoft(); reloadTargets(); reloadRuns(); }

    // A different machine invalidates the picked image (the mockup does the same).
    useEffect(() => { setImageId(null); setIfaceMac(""); }, [targetId]);

    // Poll runs while any is in flight, or for a minute after an enqueue
    // (the helper inserts its prov_runs row when it picks the request up).
    const active = runs.some((r) => r.status === "queued" || r.status === "running");
    useEffect(() => {
      if (!active && !pollUntil) return undefined;
      const t = setInterval(() => {
        if (!active && Date.now() > pollUntil) { setPollUntil(0); return; }
        reloadRuns();
      }, 5000);
      return () => clearInterval(t);
    }, [active, pollUntil, reloadRuns]);

    // Poll images while a refresh is outstanding; a changed fetched_at ends it.
    const pendingRefresh = Object.keys(refreshing).length > 0;
    useEffect(() => {
      if (!pendingRefresh) return undefined;
      const t = setInterval(() => { reloadImages(); }, 8000);
      return () => clearInterval(t);
    }, [pendingRefresh, reloadImages]);
    useEffect(() => {
      if (!pendingRefresh || !imagesR.data) return;
      const next = {};
      let changed = false;
      Object.keys(refreshing).forEach((id) => {
        const r = refreshing[id];
        const img = imagesR.data.find((x) => String(x.id) === id);
        if (!img || img.fetched_at !== r.fetched_at || Date.now() - r.at > 20 * 60000) { changed = true; return; }
        next[id] = r;
      });
      if (changed) setRefreshing(next);
    }, [imagesR.data]);
    // A refresh records a prov_runs row (031); its terminal status ends the
    // wait and is reported, so the operator is not left with "started".
    useEffect(() => {
      if (!pendingRefresh || !runsR.data) return;
      const next = Object.assign({}, refreshing);
      let changed = false;
      Object.keys(refreshing).forEach((id) => {
        const w = refreshing[id];
        const run = runsR.data.find((r) => r.mode === "refresh" && String(r.image_id) === id && Number(r.id) > w.afterRun);
        if (!run) return;
        if (RUN_DONE[run.status]) {
          delete next[id];
          say("Refresh of " + (run.image_label || "image " + id) + " " + run.status + (run.detail ? ": " + run.detail : ""),
            run.status === "succeeded" ? "check" : "close");
          if (run.status === "succeeded") reloadImages();
        } else if (w.run !== run.id || w.status !== run.status) {
          next[id] = Object.assign({}, w, { run: run.id, status: run.status });
        } else return;
        changed = true;
      });
      if (changed) setRefreshing(next);
    }, [runsR.data]);

    // ---- selection model -------------------------------------------------
    const slugs = useMemo(() => new Set(stacks.map((s) => s.slug)), [stacks]);
    const pickedLive = Array.from(picked).filter((s) => slugs.has(s));
    const plan = useMemo(() => resolveOrder(stacks, pickedLive), [stacks, pickedLive.join(",")]);
    const bySlug = useMemo(() => { const m = {}; stacks.forEach((s) => { m[s.slug] = s; }); return m; }, [stacks]);
    const itemCount = plan.order.reduce((n, s) => n + ((bySlug[s] && bySlug[s].member_count) || 0), 0);

    function offReason(img) {
      if (tArch && img.arch !== tArch) return "image is " + img.arch + ", target is " + tArch;
      if (mode === "apply") return "not used in apply-only mode";
      return "";
    }
    function pickReason(img) {
      const off = offReason(img);
      if (off) return off;
      if (!img.fetched) return "not fetched — refresh it first";
      if (img.boot_method !== "ipxe") return "only iPXE images can be staged for netboot (" + (BOOT[img.boot_method] || [img.boot_method])[0] + ")";
      return "";
    }
    const selImage = images.find((i) => i.id === imageId) || null;
    const selOk = selImage && !pickReason(selImage);

    // Family readiness of the run on the picked image's family.
    const fam = mode === "reimage" && selImage ? selImage.family : null;
    const famBlockers = useMemo(() => {
      if (!fam) return [];
      const out = [];
      plan.order.forEach((slug) => {
        const s = bySlug[slug];
        if (!s || !s.members) return;
        const c = stackCompat(s.members, swById, tArch).fams[fam];
        if (c && c.unmapped.length) out.push(s.name + ": " + c.unmapped.join(", "));
      });
      return out;
    }, [fam, plan.order.join(","), bySlug, swById, tArch]);

    // Pool conflicts across the whole run: the generator refuses these, so
    // block before confirm and name the pair (and the stacks pulling them in).
    const conflicts = useMemo(() => {
      const from = {};
      plan.order.forEach((slug) => {
        const s = bySlug[slug];
        if (!s || !s.members) return;
        s.members.forEach((m) => { (from[m.software_id] = from[m.software_id] || []).push(s.name); });
      });
      const out = [];
      Object.keys(from).forEach((id) => {
        const sw = swById[id];
        (sw && sw.conflicts_with || []).forEach((other) => {
          if (other > sw.id && from[other] && swById[other]) {
            out.push(sw.logical_name + " (" + from[id].join(", ") + ") ✕ " + swById[other].logical_name + " (" + from[other].join(", ") + ")");
          }
        });
      });
      return out;
    }, [plan.order.join(","), bySlug, swById]);

    let blocked = "";
    if (!live) blocked = "Live API unavailable — actions are paused.";
    else if (!op) blocked = OP_ONLY + " to deploy.";
    else if (!target) blocked = "Choose a target machine.";
    else if (plan.cycle) blocked = "Stack dependency cycle: " + plan.cycle.join(" → ") + ".";
    else if (!plan.order.length) blocked = "No stacks to run.";
    else if (conflicts.length) blocked = "Conflicting entries selected: " + conflicts.join("; ") + ".";
    else if (mode === "reimage" && !tArch) blocked = archUnknownReason(target);
    else if (mode === "reimage" && !iface) blocked = "Target has no interface with a MAC address.";
    else if (mode === "reimage" && !selOk) blocked = selImage ? "Image not deployable: " + pickReason(selImage) + "." : "Select an image.";

    let warnline = "";
    if (target && mode === "reimage" && selOk) {
      warnline = "Wipes the install disk on " + target.name + "'s next network boot from " + iface.name + " (" + iface.mac + "). Staged per-MAC; unstaged machines fall back to local disk.";
    } else if (target && mode === "apply") {
      const st = node && node.state;
      if (!node) warnline = "No monitoring agent matches this target; apply needs SSH + sudo on the target.";
      else if (st && st !== "up") warnline = "Target agent is not healthy (" + st + "); apply needs SSH + sudo on the target.";
    }

    function toggleStack(slug, on) {
      setPicked((prev) => { const n = new Set(prev); if (on) n.add(slug); else n.delete(slug); return n; });
    }

    // ---- actions ---------------------------------------------------------
    function refreshImage(img) {
      const a = api();
      const key = a.provIdemKey();
      const afterRun = maxRunId(runsR.data);
      return a.provImageRefresh(img.id, key).then((res) => {
        setRefreshing((r) => Object.assign({}, r, { [img.id]: { at: Date.now(), fetched_at: img.fetched_at, afterRun: afterRun } }));
        setPollUntil(Date.now() + 60000);
        reloadRuns();
        say("Refresh of " + imageLabel(img) + (res.status === "queued" ? " queued (request " + res.request + ")" : " started")
          + " — follow it in Recent runs", "refresh");
      }, (e) => say("Refresh failed: " + errText(e), "close"));
    }

    function submitDeploy(dry) {
      const a = api();
      const m = modal;
      if (mode === "reimage") {
        return a.provStage({ entity: target.id, image: selImage.id, stacks: plan.order, mac: iface.mac }, m.keys.stage).then((res) => {
          setModal(null);
          setPollUntil(Date.now() + 60000);
          reloadRuns();
          say("Reinstall of " + target.name + " staged on " + iface.mac + (res.status === "queued" ? " — request " + res.request + " queued" : ""), "provision");
        });
      }
      return a.provApply({ entity: target.id, stacks: plan.order, dry_run: dry }, dry ? m.keys.dry : m.keys.apply).then((res) => {
        setModal(null);
        setPollUntil(Date.now() + 60000);
        reloadRuns();
        say((dry ? "Dry-run" : "Apply") + " on " + target.name + (res.status === "queued" ? " queued (request " + res.request + ")" : " started"), "deploy");
      });
    }

    function openDeploy() {
      const a = api();
      setModal({ kind: "deploy", dry: true, keys: { stage: a.provIdemKey(), dry: a.provIdemKey(), apply: a.provIdemKey() } });
    }

    function deleteImage(img) {
      return api().provImageDelete(img.id).then(() => {
        setModal(null);
        if (imageId === img.id) setImageId(null);
        say("Deleted image " + imageLabel(img), "trash");
        reloadImages();
      });
    }
    function deleteStack(s) {
      return api().provStackDelete(s.id).then(() => {
        setModal(null);
        toggleStack(s.slug, false);
        say("Deleted stack " + s.name, "trash");
        reloadStacks();
      });
    }

    // ---- images grouped distro → release ---------------------------------
    const groups = useMemo(() => {
      const out = [];
      const idx = {};
      images.forEach((img) => {
        if (!idx[img.distro]) { idx[img.distro] = { distro: img.distro, releases: [], rIdx: {} }; out.push(idx[img.distro]); }
        const g = idx[img.distro];
        if (!g.rIdx[img.release_name]) { g.rIdx[img.release_name] = { name: img.release_name, label: img.release_label || img.release_name, rows: [] }; g.releases.push(g.rIdx[img.release_name]); }
        g.rIdx[img.release_name].rows.push(img);
      });
      return out;
    }, [images]);

    const runsShown = runsMine && target ? runs.filter((r) => r.entity_id === target.id) : runs;

    // ---- render ----------------------------------------------------------
    return (
      <div className="page prov-page">
        <div className="page-head">
          <div>
            <h1 className="page-title">Images &amp; Stacks</h1>
            <div className="page-sub">Provisioning · OS images · software stacks · one deploy</div>
          </div>
          <div className="page-head__right">
            <div className="seg" role="group" aria-label="View">
              <button type="button" className={tab === "deploy" ? "on" : ""} aria-pressed={tab === "deploy"} onClick={() => setTab("deploy")}>Deploy</button>
              <button type="button" className={tab === "pool" ? "on" : ""} aria-pressed={tab === "pool"} onClick={() => setTab("pool")}>Software pool</button>
            </div>
            <button type="button" className="btn-ghost" onClick={reloadAll} title="Reload images, stacks, pool, targets and runs">
              <Icon name="refresh" size={14} /> Reload
            </button>
          </div>
        </div>

        {!live && <div className="prov-warnbox" role="status" style={{ marginTop: 0, marginBottom: 16 }}>Live API unavailable — this screen shows no sample data and every action is paused.</div>}

        {tab === "pool" ? (
          <SoftwarePool software={softR} stacks={stacks} reloadSoft={reloadSoft} reloadStacks={reloadStacks} say={say} op={op && live} />
        ) : (
          <>
            {/* 1 · target band */}
            <section className="panel" aria-labelledby="prov-h-target">
              <div className="panel__head">
                <Icon name="target" size={16} className="ico" />
                <h3 id="prov-h-target">Target</h3>
                <div className="right prov-mode" role="radiogroup" aria-label="Deploy mode">
                  {[["reimage", "Reimage (wipe + install)"], ["apply", "Apply stacks only"]].map(([m, l]) => (
                    <button key={m} type="button" role="radio" aria-checked={mode === m} className={"chip" + (mode === m ? " on" : "")}
                      tabIndex={mode === m ? 0 : -1}
                      onClick={() => setMode(m)}
                      onKeyDown={(e) => { if (e.key === "ArrowRight" || e.key === "ArrowLeft" || e.key === "ArrowUp" || e.key === "ArrowDown") { e.preventDefault(); const n = mode === "reimage" ? "apply" : "reimage"; setMode(n); const sib = e.currentTarget.parentNode.querySelector("[aria-checked='false']"); if (sib) sib.focus(); } }}>{l}</button>
                  ))}
                </div>
              </div>
              <div className="panel__body prov-target">
                <Field label="Machine (from inventory)" htmlFor="prov-target">
                  <select id="prov-target" className="prov-input" value={targetId} onChange={(e) => setTargetId(e.target.value)}
                    disabled={targetsR.loading && !targetsR.data}>
                    <option value="">{targetsR.loading && !targetsR.data ? "Loading inventory…" : targets.length ? "Choose a machine…" : "No machines with a MAC in the SoR"}</option>
                    {targets.map((t) => (
                      <option key={t.id} value={t.id}>{t.name}{t.hw_model ? " — " + [t.hw_make, t.hw_model].filter(Boolean).join(" ") : ""}{t.arch ? " · " + t.arch : ""}</option>
                    ))}
                  </select>
                  <ErrorBox error={targetsR.error} onRetry={reloadTargets} />
                </Field>
                <div className="prov-facts">
                  {target ? (
                    <>
                      <div className="prov-fact"><div className="prov-kicker">Arch</div><div className="v">{tArch
                        ? <span className="tag teal" title={target.arch_source === "hardware_model" ? "From hardware model " + modelName(target) + " (Inventory)" : "Reported by the SolariNet agent"}>{tArch}</span>
                        : <span className="tag warn" title="No CPU arch on the hardware model and no agent report; reimage is blocked">unknown</span>}
                        {target.arch_source === "hardware_model" && target.agent_arch && target.agent_arch !== tArch &&
                          <span className="tag warn" title={"The hardware model says " + tArch + " but the agent reports " + target.agent_arch + "; the hardware model wins"}>agent: {target.agent_arch}</span>}
                        {target.hw_model_id && op && live &&
                          <button type="button" className="chip" style={{ marginLeft: 4 }} title={"Set the CPU architecture on hardware model " + modelName(target)}
                            onClick={() => setModal({ kind: "modelArch" })}>Set</button>}</div></div>
                      <div className="prov-fact"><div className="prov-kicker"><label htmlFor="prov-iface">Netboot interface</label></div><div className="v">
                        <select id="prov-iface" className="prov-input mono" style={{ padding: "2px 6px", fontSize: 12 }} value={iface ? iface.mac : ""}
                          disabled={target.interfaces.length < 2} title="The interface whose MAC is staged for netboot"
                          onChange={(e) => setIfaceMac(e.target.value)}>
                          {target.interfaces.map((i) => <option key={i.mac} value={i.mac}>{i.name} · {i.mac}{i.management ? " (mgmt)" : ""}</option>)}
                        </select></div></div>
                      <div className="prov-fact"><div className="prov-kicker">Model</div><div className="v">{[target.hw_make, target.hw_model].filter(Boolean).join(" ") || "—"}</div></div>
                      <div className="prov-fact"><div className="prov-kicker">Current OS</div><div className="v">{(node && node.osName) || "—"}</div></div>
                      <div className="prov-fact"><div className="prov-kicker">Lifecycle</div><div className="v">{target.lifecycle || "—"}</div></div>
                    </>
                  ) : <span className="prov-faint" style={{ fontSize: 12.5 }}>Pick the machine to deploy to. Only SoR entities with a MAC address are listed — netboot is staged per-MAC.</span>}
                </div>
                <div>
                  <div className="prov-kicker">Agent</div>
                  <div style={{ marginTop: 4 }}>{!target ? <span className="tag">—</span>
                    : !node ? <span className="tag">not enrolled</span>
                      : <span className={"tag " + (node.state === "up" ? "ok" : node.state === "down" ? "crit" : "warn")}>{node.state || "unknown"}</span>}</div>
                </div>
              </div>
            </section>

            <div className="prov-cols">
              {/* 2 · images */}
              <section className="panel" aria-labelledby="prov-h-img">
                <div className="panel__head">
                  <Icon name="boot" size={16} className="ico" />
                  <h3 id="prov-h-img">Images</h3>
                  <div className="right" style={{ display: "flex", gap: 6, alignItems: "center" }}>
                    <button type="button" className={"chip" + (onlyCompat ? " on" : "")} aria-pressed={onlyCompat}
                      disabled={!tArch} title={tArch ? "Hide rows for other architectures" : "Target arch unknown"}
                      onClick={() => setOnlyCompat(!onlyCompat)}>Compatible only</button>
                    <button type="button" className="btn-ghost" disabled={!op || !live} title={op ? "Add an image record" : OP_ONLY}
                      onClick={() => setModal({ kind: "image", img: null })}><Icon name="plus" size={14} /> Add image</button>
                  </div>
                </div>
                {imagesR.loading && !imagesR.data ? <Skeleton rows={4} />
                  : imagesR.error && !imagesR.data ? <div className="panel__body"><ErrorBox error={imagesR.error} onRetry={reloadImages} /></div>
                    : !images.length ? (
                      <div className="prov-empty">No images in the catalog yet.<br />
                        <button type="button" className="btn-ghost" disabled={!op || !live} onClick={() => setModal({ kind: "image", img: null })}><Icon name="plus" size={14} /> Add the first image</button>
                      </div>
                    ) : groups.map((g) => {
                      const nFor = tArch ? g.releases.reduce((n, r) => n + r.rows.filter((i) => i.arch === tArch).length, 0) : null;
                      return (
                        <details key={g.distro} className="prov-distro" open={!collapsed.has(g.distro)}
                          onToggle={(e) => { const open = e.currentTarget.open; setCollapsed((c) => { const n = new Set(c); if (open) n.delete(g.distro); else n.add(g.distro); return n; }); }}>
                          <summary>
                            <Icon name="chev" size={14} className="chev" />
                            <span className="name">{distroLabel(g.distro)}</span>
                            <span className="right">
                              <span className="tag">{g.releases.length} release{g.releases.length > 1 ? "s" : ""}</span>
                              {tArch && <span className={"tag" + (nFor ? " teal" : "")}>{nFor} for {tArch}</span>}
                            </span>
                          </summary>
                          {(() => {
                            const rels = g.releases.map((r) => {
                              const rows = r.rows.filter((img) => !(onlyCompat && tArch && img.arch !== tArch));
                              if (!rows.length) return null;
                              return (
                                <div key={r.name} className="prov-release" role="radiogroup" aria-label={distroLabel(g.distro) + " " + r.label}>
                                  <h4>{r.label}{r.label !== r.name && <span className="tag">{r.name}</span>}</h4>
                                  {rows.map((img) => {
                                    const off = offReason(img);
                                    const why = pickReason(img);
                                    const fr = imageFreshness(img);
                                    const sel = imageId === img.id;
                                    const busy = !!refreshing[img.id];
                                    const [bl, bc] = BOOT[img.boot_method] || [img.boot_method, ""];
                                    return (
                                      <div key={img.id} className={"prov-irow" + (sel ? " sel" : "") + (off ? " off" : "")} title={why || undefined}>
                                        <input className="prov-pick" type="radio" name="prov-image" checked={sel} disabled={!!why}
                                          aria-label={"Select " + imageLabel(img) + (why ? " (" + why + ")" : "")}
                                          onChange={() => setImageId(img.id)} />
                                        <span className="arch">{img.arch}</span>
                                        <span className="boot"><span className={"tag " + bc}>{bl}</span></span>
                                        <span className="meta">
                                          <span>v {img.version || "—"}</span><span>· fetched {fmtDate(img.fetched_at)}</span>
                                          <span className={"tag " + (fr.state === "ok" ? "ok" : fr.state === "stale" ? "warn" : "")} title={fr.why}>{fr.label}</span>
                                          {img.boot_method === "ipxe" && !img.netboot_verified && <span className="tag warn" title="No real bare-metal netboot install from this image has been confirmed yet">netboot unverified</span>}
                                          {busy && <span className="tag teal" title={refreshing[img.id].run ? "Run " + refreshing[img.id].run + " in Recent runs" : "Waiting for the refresh run to appear in Recent runs"}>{refreshing[img.id].status ? "refresh " + refreshing[img.id].status + "…" : "refreshing…"}</span>}
                                        </span>
                                        <span className="prov-acts">
                                          <IconBtn icon="refresh" label={"Refresh " + imageLabel(img) + " from upstream"} title={op ? "Refresh from upstream (IMAGE_REFRESH)" : OP_ONLY}
                                            disabled={!op || !live || busy} busy={busy} onClick={() => refreshImage(img)} />
                                          <IconBtn icon="edit" label={"Edit " + imageLabel(img)} title="Edit source / install template"
                                            onClick={() => setModal({ kind: "image", img: img })} />
                                          <IconBtn icon="trash" danger label={"Delete " + imageLabel(img)} title={op ? "Delete" : OP_ONLY}
                                            disabled={!op || !live} onClick={() => setModal({ kind: "delImage", img: img })} />
                                        </span>
                                      </div>
                                    );
                                  })}
                                </div>
                              );
                            }).filter(Boolean);
                            return rels.length ? rels : <div className="prov-release prov-faint" style={{ fontSize: 12, paddingBottom: 12 }}>No images for this architecture.</div>;
                          })()}
                        </details>
                      );
                    })}
                {imagesR.error && imagesR.data && <div className="panel__body"><ErrorBox error={imagesR.error} onRetry={reloadImages} /></div>}
              </section>

              {/* 3 · stacks */}
              <section className="panel" aria-labelledby="prov-h-st">
                <div className="panel__head">
                  <Icon name="layers" size={16} className="ico" />
                  <h3 id="prov-h-st">Software stacks</h3>
                  <div className="right" style={{ display: "flex", gap: 6, alignItems: "center" }}>
                    <span className="tag teal">{plan.order.length} selected</span>
                    <button type="button" className="chip" onClick={() => setPicked(new Set(stacks.filter((s) => !s.locked).map((s) => s.slug)))} disabled={!stacks.length}>Select all</button>
                    <button type="button" className="chip" onClick={() => setPicked(new Set())} disabled={!picked.size}>Clear</button>
                    <button type="button" className="btn-ghost" disabled={!op || !live} title={op ? "New stack" : OP_ONLY}
                      onClick={() => setModal({ kind: "stack", id: null })}><Icon name="plus" size={14} /> New</button>
                  </div>
                </div>
                {stacksR.loading && !stacksR.data ? <Skeleton rows={3} />
                  : stacksR.error && !stacksR.data ? <div className="panel__body"><ErrorBox error={stacksR.error} onRetry={reloadStacks} /></div>
                    : !stacks.length ? <div className="prov-empty">No stacks defined.</div>
                      : stacks.map((s) => {
                        const auto = !s.locked && !picked.has(s.slug) && plan.order.indexOf(s.slug) >= 0;
                        const on = s.locked || auto || picked.has(s.slug);
                        const c = s.members ? stackCompat(s.members, swById, tArch) : null;
                        return (
                          <div key={s.id} className="prov-stack">
                            <input className="prov-cb" type="checkbox" id={"prov-st-" + s.id} checked={on} disabled={s.locked || auto}
                              title={s.locked ? "Always included" : auto ? "Required by another selected stack" : undefined}
                              onChange={(e) => toggleStack(s.slug, e.target.checked)} />
                            <div>
                              <div className="t">
                                <label htmlFor={"prov-st-" + s.id}>{s.name}</label>
                                {s.locked && <span className="tag" title="Always included, always first"><Icon name="lock" size={10} className="prov-lockico" />always</span>}
                                {s.depends_on && s.depends_on.length > 0 && <span className="tag">needs {s.depends_on.join(", ")}</span>}
                                {auto && <span className="tag teal">auto · required by selection</span>}
                              </div>
                              {s.description && <div className="d">{s.description}</div>}
                              <div className="compat">
                                {c ? FAMS.map((f) => {
                                  const x = c.fams[f];
                                  const cls = !x.applicable ? "" : x.unmapped.length ? "crit" : "ok";
                                  const tip = !x.applicable ? FAM_LABEL[f] + ": nothing in this stack applies"
                                    : x.unmapped.length ? FAM_LABEL[f] + ": unmapped " + x.unmapped.join(", ") + " — a run on this family is refused"
                                      : FAM_LABEL[f] + ": all items mapped";
                                  return <span key={f} className={"tag " + cls} title={tip} style={fam && fam !== f ? { opacity: .55 } : undefined}>{f}</span>;
                                }) : <span className="tag warn" title="Stack detail failed to load">compat unknown</span>}
                                <span className="tag">{s.member_count} item{s.member_count === 1 ? "" : "s"}</span>
                                {c && c.archSkipped.length > 0 && <span className="tag warn" title={"Skipped on " + tArch + ": " + c.archSkipped.join(", ")}>{c.archSkipped.length} skipped on {tArch}</span>}
                              </div>
                            </div>
                            <div className="prov-acts">
                              <IconBtn icon="edit" label={"Edit stack " + s.name} onClick={() => setModal({ kind: "stack", id: s.id })} />
                              <IconBtn icon="trash" danger label={"Delete stack " + s.name}
                                title={s.locked ? "Locked stacks cannot be deleted" : op ? "Delete" : OP_ONLY}
                                disabled={s.locked || !op || !live} onClick={() => setModal({ kind: "delStack", s: s })} />
                            </div>
                          </div>
                        );
                      })}
                {stacksR.error && stacksR.data && <div className="panel__body"><ErrorBox error={stacksR.error} onRetry={reloadStacks} /></div>}
              </section>
            </div>

            {/* 4 · recent runs */}
            <section className="panel" style={{ marginTop: 16 }} aria-labelledby="prov-h-runs">
              <div className="panel__head">
                <Icon name="clock" size={16} className="ico" />
                <h3 id="prov-h-runs">Recent runs</h3>
                <div className="right" style={{ display: "flex", gap: 8, alignItems: "center" }}>
                  <span>audit-logged · operator-gated</span>
                  <button type="button" className={"chip" + (runsMine ? " on" : "")} aria-pressed={runsMine} disabled={!target}
                    onClick={() => setRunsMine(!runsMine)}>This target</button>
                  <IconBtn icon="refresh" label="Reload runs" busy={runsR.loading} onClick={reloadRuns} />
                </div>
              </div>
              <div className="panel__body prov-log" aria-live="polite">
                {runsR.loading && !runsR.data ? <span className="prov-faint">Loading…</span>
                  : runsR.error && !runsR.data ? <ErrorBox error={runsR.error} onRetry={reloadRuns} />
                    : !runsShown.length ? <span className="prov-faint">No runs yet{runsMine ? " for this target" : ""}.</span>
                      : runsShown.map((r) => (
                        <div key={r.id} className="row">
                          <span className="when prov-faint" title={"requested " + fmtWhen(r.requested_at) + (r.finished_at ? " · finished " + fmtWhen(r.finished_at) : "")}>{fmtWhen(r.requested_at)}</span>
                          <span>
                            {r.mode === "refresh" ? "refresh  " + (r.image_label || "image " + r.image_id) : <>
                              {r.mode === "reimage" ? "stage  " + (r.image_label || "image " + r.image_id) + " + " : "apply  "}
                              {(r.stack_slugs || []).join("+")} → {r.entity_name || "entity " + r.entity_id}
                              {r.dry_run ? " (dry-run)" : ""}
                            </>}
                            <span className="prov-faint"> · {r.requested_by}</span>
                          </span>
                          <span className={"tag " + (RUN_TAG[r.status] || "")}>{r.status}</span>
                          {RUN_LOG_KIND[r.mode] && runLogId(r) != null ? <IconBtn icon="logs" label={"Helper log for run " + r.id}
                            onClick={() => setModal({ kind: "log", logKind: RUN_LOG_KIND[r.mode], logId: runLogId(r), title: runLogTitle(r) })} /> : <span />}
                          {r.detail && <span className="detail">{r.detail}</span>}
                        </div>
                      ))}
                {runsR.error && runsR.data && <ErrorBox error={runsR.error} onRetry={reloadRuns} />}
              </div>
            </section>

            {/* deploy bar */}
            <div className="prov-dbar" role="region" aria-label="Deploy">
              <div className="in">
                <div style={{ flex: 1, minWidth: 280 }}>
                  <div className="sum">
                    <b>{target ? target.name : <span className="prov-faint">choose a target</span>}</b>
                    {" ← "}
                    {mode === "reimage"
                      ? <>{selImage ? <b>{imageLabel(selImage)}</b> : <span className="prov-faint">select an image</span>}{" + "}</>
                      : "apply "}
                    {plan.order.map((slug, i) => <span key={slug} className="tag teal">{i + 1}·{(bySlug[slug] && bySlug[slug].name) || slug}</span>)}
                    <span className="prov-faint">({plan.order.length} stack{plan.order.length === 1 ? "" : "s"} · {itemCount} items · one run{mode === "apply" ? " · no reimage, Ansible over SSH" : ""})</span>
                  </div>
                  {warnline && <div className="warnline">{warnline}</div>}
                  {famBlockers.length > 0 && <div className="warnline" style={{ color: "var(--crit)" }}>Unmapped on {fam}: {famBlockers.join("; ")} — the generator will refuse this run.</div>}
                  {blocked && <div className="warnline" style={{ color: "var(--ink-faint)" }}>{blocked}</div>}
                </div>
                <button type="button" className="btn-primary" disabled={!!blocked} onClick={openDeploy} title={blocked || undefined}>
                  <Icon name="deploy" size={16} /> Deploy
                </button>
              </div>
            </div>
          </>
        )}

        {modal && modal.kind === "deploy" && target && (
          mode === "reimage" ? (
            <Confirm title={"Stage reinstall of " + target.name + "?"} sub={"PROV_STAGE · " + iface.name + " " + iface.mac + " · " + tArch}
              danger verb="Stage reinstall" busyVerb="Staging…" confirmWord={target.name}
              onConfirm={() => submitDeploy(false)} onClose={() => setModal(null)}>
              <p style={{ marginTop: 0 }}>Stacks applied at first boot, in order: <b>{plan.order.map((s) => (bySlug[s] && bySlug[s].name) || s).join(" → ")}</b>.</p>
              <p><b style={{ color: "var(--warn)" }}>Destructive.</b> {imageLabel(selImage)} will wipe the install disk on {target.name}'s next network boot from <span className="mono">{iface.name} ({iface.mac})</span>. Its node certificate is re-minted at first boot.</p>
              {famBlockers.length > 0 && <div className="prov-err">Unmapped on {fam}: {famBlockers.join("; ")}. The first-boot apply will fail until these are mapped.</div>}
            </Confirm>
          ) : (
            <Confirm title={(modal.dry ? "Dry-run stacks on " : "Apply stacks to ") + target.name + "?"} sub={"STACK_APPLY · " + (modal.dry ? "check mode" : "live change")}
              danger={!modal.dry} verb={modal.dry ? "Run dry-run" : "Apply now"} busyVerb="Queuing…"
              confirmWord={modal.dry ? null : target.name}
              onConfirm={() => submitDeploy(modal.dry)} onClose={() => setModal(null)}>
              <p style={{ marginTop: 0 }}>One Ansible run over SSH, in order: <b>{plan.order.map((s) => (bySlug[s] && bySlug[s].name) || s).join(" → ")}</b>.</p>
              <label style={{ display: "flex", gap: 10, alignItems: "flex-start", cursor: "pointer" }}>
                <input type="checkbox" className="prov-cb" checked={modal.dry} onChange={(e) => setModal(Object.assign({}, modal, { dry: e.target.checked }))} />
                <span><b>Dry-run first</b> <span className="muted">— Ansible check mode with a diff; nothing changes on the target. Review the log in Recent runs, then come back and untick this to apply.</span></span>
              </label>
              {warnline && <div className="prov-warnbox">{warnline}</div>}
            </Confirm>
          )
        )}
        {modal && modal.kind === "delImage" && (
          <Confirm title={"Delete " + imageLabel(modal.img) + "?"} danger verb="Delete" busyVerb="Deleting…" confirmWord="delete"
            onConfirm={() => deleteImage(modal.img)} onClose={() => setModal(null)}>
            Removes the image record from the catalog. Hosts already installed from it are unaffected.
          </Confirm>
        )}
        {modal && modal.kind === "delStack" && (
          <Confirm title={"Delete stack " + modal.s.name + "?"} danger verb="Delete" busyVerb="Deleting…" confirmWord="delete"
            onConfirm={() => deleteStack(modal.s)} onClose={() => setModal(null)}>
            Removes the stack and its member list. Pool entries stay in the software pool, and hosts already configured from it are unaffected.
          </Confirm>
        )}
        {modal && modal.kind === "image" && (
          <ImageEditor img={modal.img} op={op && live} say={say} onClose={() => setModal(null)}
            onSaved={() => { reloadImages(); }}
            onLog={(img) => setModal({ kind: "log", logKind: "image-refresh", logId: img.id, title: "Refresh log · " + imageLabel(img) })} />
        )}
        {modal && modal.kind === "stack" && (
          <StackEditor id={modal.id} stacks={stacks} software={software} op={op && live} say={say}
            onClose={() => setModal(null)} onChanged={reloadStacks} />
        )}
        {modal && modal.kind === "modelArch" && target && target.hw_model_id && (
          <ModelArchDialog target={target} say={say} onClose={() => setModal(null)} onSaved={reloadTargets} />
        )}
        {modal && modal.kind === "log" && (
          <LogViewer kind={modal.logKind} id={modal.logId} title={modal.title} onClose={() => setModal(null)} />
        )}
      </div>
    );
  }

  // Helper log tail (GET /api/provisioning/log — read-only, 64 KiB tail).
  function LogViewer({ kind, id, title, onClose }) {
    const [follow, setFollow] = useState(true);
    const [st, reload] = useResource(needApi((a) => a.provLog(kind, id)));
    const pre = useRef(null);
    useEffect(() => {
      if (!follow) return undefined;
      const t = setInterval(() => { reload(); }, 3000);
      return () => clearInterval(t);
    }, [follow, reload]);
    useEffect(() => { if (follow && pre.current) pre.current.scrollTop = pre.current.scrollHeight; }, [st.data, follow]);
    const d = st.data;
    return (
      <Drawer title={title} sub={kind + "-" + id + ".log"} icon="logs" onClose={onClose}
        footer={<>
          <label style={{ display: "flex", gap: 8, alignItems: "center", marginRight: "auto", fontSize: 12.5 }} className="muted">
            <input type="checkbox" checked={follow} onChange={(e) => setFollow(e.target.checked)} /> Follow (3 s)
          </label>
          <button type="button" className="btn-ghost" onClick={reload}>Reload</button>
          <button type="button" className="btn-primary" onClick={onClose}>Close</button>
        </>}>
        {st.loading && !d ? <Skeleton rows={2} />
          : st.error && !d ? <ErrorBox error={st.error} onRetry={reload} />
            : d && !d.exists ? <div className="prov-empty">No log yet — the helper writes one when it picks up the request.</div>
              : d ? (
                <>
                  <div className="prov-note" style={{ marginTop: 0, marginBottom: 8 }}>
                    {d.truncated ? "Last 64 KiB of " + Math.round(d.size / 1024) + " KiB" : Math.max(1, Math.round(d.size / 1024)) + " KiB"} · updated {fmtWhen(d.updated_at)}
                  </div>
                  <pre ref={pre} className="prov-tail" tabIndex={0} aria-label="Log output">{d.log || "(empty)"}</pre>
                </>
              ) : null}
        {st.error && d && <ErrorBox error={st.error} />}
      </Drawer>
    );
  }

  // ======================================================================
  // PART 3 — image + stack editors
  // ======================================================================
  function nn(v) { if (typeof v !== "string") return v == null ? null : v; const t = v.trim(); return t === "" ? null : t; }
  // Changed keys only (PUT sends a diff; an empty diff is "nothing to save").
  function canon(v) {
    if (Array.isArray(v)) return "[" + v.map(canon).join(",") + "]";
    if (v && typeof v === "object") return "{" + Object.keys(v).sort().map((k) => JSON.stringify(k) + ":" + canon(v[k])).join(",") + "}";
    return JSON.stringify(v === undefined ? null : v);
  }
  function diffOf(next, cur) {
    const d = {};
    Object.keys(next).forEach((k) => {
      const a = next[k], b = cur[k] == null ? null : cur[k];
      if (canon(a) !== canon(b)) d[k] = a;
    });
    return d;
  }
  function onField(setF, k) {
    return (e) => { const t = e.target; const v = t.type === "checkbox" ? t.checked : t.value; setF((p) => Object.assign({}, p, { [k]: v })); };
  }

  // ---- image record (catalog row; the fetch fields belong to IMAGE_REFRESH) --
  const IMAGE_SOURCE = ["distro", "release_name", "arch", "boot_method", "source_url", "checksum_url"];
  function ImageEditor({ img, op, say, onClose, onSaved, onLog }) {
    const editing = !!img;
    const [f, setF] = useState(() => ({
      distro: img ? img.distro : "", family: img ? img.family : "",
      release_name: img ? img.release_name : "", release_label: img ? img.release_label || "" : "",
      arch: img ? img.arch : "x86_64", boot_method: img ? img.boot_method : "ipxe",
      source_url: img ? img.source_url || "" : "", checksum_url: img ? img.checksum_url || "" : "",
      install_template: img ? img.install_template || "" : "", refresh_policy: img ? img.refresh_policy : "manual",
      netboot_verified: img ? !!img.netboot_verified : false, notes: img ? img.notes || "" : "",
    }));
    const [error, setError] = useState(null);
    const [busy, setBusy] = useState(false);
    const ro = !op;

    function setDistro(e) {
      const v = e.target.value;
      setF((p) => {
        const n = Object.assign({}, p, { distro: v });
        // Suggest the family for a known distro unless the operator chose one.
        if (DISTRO_FAMILY[v] && (!p.family || p.family === DISTRO_FAMILY[p.distro])) n.family = DISTRO_FAMILY[v];
        return n;
      });
    }
    const body = {
      distro: f.distro.trim(), family: f.family || null, release_name: f.release_name.trim(),
      release_label: nn(f.release_label), arch: f.arch, boot_method: f.boot_method,
      source_url: nn(f.source_url), checksum_url: nn(f.checksum_url), install_template: nn(f.install_template),
      refresh_policy: f.refresh_policy, netboot_verified: !!f.netboot_verified, notes: nn(f.notes),
    };
    const diff = editing ? diffOf(body, img) : body;
    const clears = editing && img.fetched && IMAGE_SOURCE.some((k) => k in diff);
    const piBad = f.boot_method === "pi_netboot" && f.arch !== "arm64" && f.arch !== "armhf";

    function save(e) {
      e.preventDefault();
      if (ro || busy) return;
      if (editing && !Object.keys(diff).length) { onClose(); return; }
      setBusy(true);
      setError(null);
      const a = api();
      const p = editing ? a.provImageUpdate(img.id, diff) : a.provImageCreate(body);
      p.then((row) => {
        setBusy(false);
        say((editing ? "Saved " : "Added ") + imageLabel(row || Object.assign({}, img, body)), "check");
        onSaved();
        onClose();
      }, (err) => { setBusy(false); setError(err); });
    }

    const fid = "prov-img-form";
    const inv = (k) => invalid(error, k) || undefined;
    return (
      <Drawer title={editing ? "Edit image · " + imageLabel(img) : "Add image"}
        sub={editing ? "prov_images #" + img.id : "catalog row · the artifact is fetched by IMAGE_REFRESH"} icon="boot" onClose={onClose}
        footer={<>
          {ro && <span className="muted" style={{ marginRight: "auto", fontSize: 12 }}>Read-only — {OP_ONLY.toLowerCase()}.</span>}
          <button type="button" className="btn-ghost" onClick={onClose}>{ro ? "Close" : "Cancel"}</button>
          {!ro && <button type="submit" form={fid} className="btn-primary" disabled={busy || piBad}>{busy ? "Saving…" : editing ? "Save" : "Add image"}</button>}
        </>}>
        <form id={fid} onSubmit={save} noValidate>
          <fieldset disabled={ro || busy} style={{ border: 0, padding: 0, margin: 0 }}>
            <div className="prov-section">
              <div className="prov-section__head"><h3>Identity</h3></div>
              <div className="prov-grid3">
                <Field label="Distro" htmlFor="pie-distro" hint="lowercase slug">
                  <input id="pie-distro" className="prov-input mono" list="pie-distros" value={f.distro} onChange={setDistro}
                    aria-invalid={inv("distro")} autoComplete="off" spellCheck={false} data-autofocus />
                  <datalist id="pie-distros">{Object.keys(DISTRO_LABEL).map((d) => <option key={d} value={d}>{DISTRO_LABEL[d]}</option>)}</datalist>
                </Field>
                <Field label="Family" htmlFor="pie-family" hint="selects the package-name column">
                  <select id="pie-family" className="prov-input" value={f.family} onChange={onField(setF, "family")} aria-invalid={inv("family")}>
                    <option value="">Choose…</option>
                    {FAMS.map((x) => <option key={x} value={x}>{FAM_LABEL[x]}</option>)}
                  </select>
                </Field>
                <Field label="Arch" htmlFor="pie-arch">
                  <select id="pie-arch" className="prov-input mono" value={f.arch} onChange={onField(setF, "arch")} aria-invalid={inv("arch") || piBad || undefined}>
                    {ARCHES.map((x) => <option key={x} value={x}>{x}</option>)}
                  </select>
                </Field>
                <Field label="Release" htmlFor="pie-rel" hint="token, e.g. 16.0 or bookworm">
                  <input id="pie-rel" className="prov-input mono" value={f.release_name} onChange={onField(setF, "release_name")}
                    aria-invalid={inv("release_name")} autoComplete="off" spellCheck={false} />
                </Field>
                <Field label="Release label" htmlFor="pie-rlab" hint="shown in the tree (optional)">
                  <input id="pie-rlab" className="prov-input" value={f.release_label} onChange={onField(setF, "release_label")}
                    aria-invalid={inv("release_label")} placeholder="Leap 16.0" />
                </Field>
                <Field label="Boot method" htmlFor="pie-boot">
                  <select id="pie-boot" className="prov-input" value={f.boot_method} onChange={onField(setF, "boot_method")} aria-invalid={inv("boot_method") || piBad || undefined}>
                    {Object.keys(BOOT_LONG).map((x) => <option key={x} value={x}>{BOOT_LONG[x]}</option>)}
                  </select>
                </Field>
              </div>
              {piBad && <div className="prov-err" role="alert">Pi netboot needs arch arm64 or armhf.</div>}
            </div>

            <div className="prov-section">
              <div className="prov-section__head"><h3>Source</h3><span className="prov-faint" style={{ fontSize: 11.5 }}>Only iPXE images can be staged per-MAC today.</span></div>
              <Field label="Source URL" htmlFor="pie-src" hint="where the kernel/initrd (or media image) comes from">
                <input id="pie-src" className="prov-input mono" type="url" value={f.source_url} onChange={onField(setF, "source_url")}
                  aria-invalid={inv("source_url")} placeholder="https://…" spellCheck={false} />
              </Field>
              <Field label="Checksum URL" htmlFor="pie-sum" hint="SHA256SUMS the refresh verifies against">
                <input id="pie-sum" className="prov-input mono" type="url" value={f.checksum_url} onChange={onField(setF, "checksum_url")}
                  aria-invalid={inv("checksum_url")} placeholder="https://…/SHA256SUMS" spellCheck={false} />
              </Field>
              <div className="prov-grid2">
                <Field label="Install template" htmlFor="pie-tpl" hint="deploy/fleet/netboot/configs/">
                  <input id="pie-tpl" className="prov-input mono" list="pie-tpls" value={f.install_template} onChange={onField(setF, "install_template")}
                    aria-invalid={inv("install_template")} autoComplete="off" spellCheck={false} />
                  <datalist id="pie-tpls">{TEMPLATES.map((t) => <option key={t} value={t} />)}</datalist>
                </Field>
                <Field label="Refresh policy" htmlFor="pie-pol" hint="drives the stale flag">
                  <select id="pie-pol" className="prov-input" value={f.refresh_policy} onChange={onField(setF, "refresh_policy")} aria-invalid={inv("refresh_policy")}>
                    {Object.keys(REFRESH).map((x) => <option key={x} value={x}>{REFRESH[x]}</option>)}
                  </select>
                </Field>
              </div>
              <label style={{ display: "flex", gap: 10, alignItems: "center", fontSize: 12.5, marginTop: 4, cursor: "pointer" }}>
                <input type="checkbox" className="prov-cb" checked={f.netboot_verified} onChange={onField(setF, "netboot_verified")} />
                <span>Netboot verified <span className="muted">— a real bare-metal install from this image has been confirmed</span></span>
              </label>
              <Field label="Notes" htmlFor="pie-notes" style={{ marginTop: 12 }}>
                <input id="pie-notes" className="prov-input" value={f.notes} onChange={onField(setF, "notes")} aria-invalid={inv("notes")} maxLength={255} />
              </Field>
              {clears && <div className="prov-warnbox" role="status">Changing the distro, release, arch, boot method or a source URL clears the fetched artifact — refresh the image again before staging it.</div>}
            </div>
          </fieldset>
          {editing && (
            <div className="prov-section">
              <div className="prov-section__head"><h3>Fetched artifact</h3>
                <span className="prov-faint" style={{ fontSize: 11.5 }}>managed by IMAGE_REFRESH</span>
                <button type="button" className="linkbtn" style={{ marginLeft: "auto" }} onClick={() => onLog(img)}>Refresh log</button>
              </div>
              <div className="prov-grid3" style={{ fontSize: 12.5 }}>
                <div><div className="prov-kicker">Version</div><div className="mono">{img.version || "—"}</div></div>
                <div><div className="prov-kicker">Fetched</div><div>{fmtWhen(img.fetched_at)}</div></div>
                <div><div className="prov-kicker">Status</div><div><span className={"tag " + ({ ok: "ok", stale: "warn" }[imageFreshness(img).state] || "")}>{imageFreshness(img).label}</span></div></div>
              </div>
              {img.sha256 && <div className="mono prov-faint" style={{ fontSize: 11, marginTop: 8, wordBreak: "break-all" }}>sha256 {img.sha256}</div>}
            </div>
          )}
          <ErrorBox error={error} />
        </form>
      </Drawer>
    );
  }

  // ---- stack: meta, ordered members (pool references), stack deps ---------
  function StackEditor({ id, stacks, software, op, say, onClose, onChanged }) {
    const [curId, setCurId] = useState(id);
    const [detR, reloadDet] = useResource(() => (curId ? api().provStackGet(curId) : Promise.resolve(null)));
    useEffect(() => { if (curId && curId !== id) reloadDet(); }, [curId]);
    const det = detR.data;
    const [f, setF] = useState(null);
    useEffect(() => {
      if (det) setF({ name: det.name, description: det.description || "", sort_order: String(det.sort_order), locked: det.locked });
    }, [det && det.updated_at, det && det.id]);
    const [nf, setNf] = useState({ slug: "", name: "", description: "", sort_order: "100", locked: false });
    const [metaErr, setMetaErr] = useState(null);
    const [memErr, setMemErr] = useState(null);
    const [depErr, setDepErr] = useState(null);
    const [busy, setBusy] = useState(false);
    const [addSw, setAddSw] = useState("");
    const [addDep, setAddDep] = useState("");
    const [moved, setMoved] = useState(null);   // software_id to keep focus on after a move
    const ro = !op;

    function run(p, setErr, okMsg) {
      setBusy(true);
      setErr(null);
      return p.then((r) => {
        setBusy(false);
        if (okMsg) say(okMsg, "check");
        onChanged();
        return reloadDet().then(() => r);
      }, (e) => { setBusy(false); setErr(e); throw e; });
    }

    function create(e) {
      e.preventDefault();
      if (ro || busy) return;
      const so = parseInt(nf.sort_order, 10);
      const body = { slug: nf.slug.trim(), name: nf.name.trim(), description: nn(nf.description), sort_order: Number.isFinite(so) ? so : nf.sort_order, locked: !!nf.locked };
      setBusy(true);
      setMetaErr(null);
      api().provStackCreate(body).then((d) => {
        setBusy(false);
        say("Created stack " + d.name + " — now add its members", "check");
        onChanged();
        setCurId(d.id);
      }, (err) => { setBusy(false); setMetaErr(err); });
    }

    function saveMeta(e) {
      e.preventDefault();
      if (ro || busy || !f) return;
      const so = parseInt(f.sort_order, 10);
      const next = { name: f.name.trim(), description: nn(f.description), sort_order: Number.isFinite(so) ? so : f.sort_order, locked: !!f.locked };
      const d = diffOf(next, det);
      if (!Object.keys(d).length) { say("No changes to save", "check"); return; }
      run(api().provStackUpdate(det.id, d), setMetaErr, "Saved stack " + next.name).catch(() => {});
    }

    const members = (det && det.members) || [];
    function move(i, dir) {
      const j = i + dir;
      if (j < 0 || j >= members.length) return;
      const list = members.slice();
      const t = list[i]; list[i] = list[j]; list[j] = t;
      // Pin the whole list to explicit orders 10, 20, … — one PUT per member
      // whose stored order differs.
      const a = api();
      const puts = list.map((m, k) => ((k + 1) * 10 === m.item_order ? null : () => a.provMemberUpdate(det.id, m.software_id, { item_order: (k + 1) * 10 }))).filter(Boolean);
      setMoved(t.software_id);
      run(puts.reduce((p, fn) => p.then(fn), Promise.resolve()), setMemErr).catch(() => {});
    }
    function resetOrder() {
      const a = api();
      const puts = members.filter((m) => m.item_order !== null).map((m) => () => a.provMemberUpdate(det.id, m.software_id, { item_order: null }));
      run(puts.reduce((p, fn) => p.then(fn), Promise.resolve()), setMemErr, "Members back in type-default order").catch(() => {});
    }
    function addMember() {
      if (!addSw) return;
      const sw = software.find((s) => String(s.id) === addSw);
      run(api().provMemberAdd(det.id, { software_id: Number(addSw) }), setMemErr, "Added " + (sw ? sw.logical_name : "member")).then(() => setAddSw(""), () => {});
    }
    function removeMember(m) {
      run(api().provMemberDelete(det.id, m.software_id), setMemErr, "Removed " + m.logical_name).catch(() => {});
    }
    function addDependency() {
      if (!addDep) return;
      run(api().provDepAdd(det.id, Number(addDep)), setDepErr).then(() => setAddDep(""), () => {});
    }
    function removeDependency(d) {
      run(api().provDepDelete(det.id, d.stack_id), setDepErr).catch(() => {});
    }
    useEffect(() => {
      if (moved == null || busy) return;
      const el = document.querySelector("[data-prov-member='" + moved + "'] .prov-ib:not([disabled])");
      if (el) el.focus({ preventScroll: true });
      setMoved(null);
    }, [busy, det]);

    // ---- create form ----
    if (!curId) {
      return (
        <Drawer title="New stack" sub="prov_stacks · members are added after it is created" icon="layers" onClose={onClose}
          footer={<>
            <button type="button" className="btn-ghost" onClick={onClose}>Cancel</button>
            <button type="submit" form="prov-st-new" className="btn-primary" disabled={ro || busy}>{busy ? "Creating…" : "Create stack"}</button>
          </>}>
          <form id="prov-st-new" onSubmit={create} noValidate>
            <fieldset disabled={ro || busy} style={{ border: 0, padding: 0, margin: 0 }}>
              <div className="prov-grid2">
                <Field label="Slug" htmlFor="pse-slug" hint="lowercase, used in run requests; cannot change later">
                  <input id="pse-slug" className="prov-input mono" value={nf.slug} onChange={onField(setNf, "slug")} aria-invalid={invalid(metaErr, "slug") || undefined}
                    autoComplete="off" spellCheck={false} data-autofocus placeholder="media-tools" />
                </Field>
                <Field label="Name" htmlFor="pse-name">
                  <input id="pse-name" className="prov-input" value={nf.name} onChange={onField(setNf, "name")} aria-invalid={invalid(metaErr, "name") || undefined} />
                </Field>
              </div>
              <Field label="Description" htmlFor="pse-desc">
                <input id="pse-desc" className="prov-input" value={nf.description} onChange={onField(setNf, "description")} maxLength={255} aria-invalid={invalid(metaErr, "description") || undefined} />
              </Field>
              <div className="prov-grid2">
                <Field label="Sort order" htmlFor="pse-so" hint="tie-break between independent stacks (lower first)">
                  <input id="pse-so" className="prov-input mono" inputMode="numeric" value={nf.sort_order} onChange={onField(setNf, "sort_order")} aria-invalid={invalid(metaErr, "sort_order") || undefined} />
                </Field>
                <label style={{ display: "flex", gap: 10, alignItems: "center", fontSize: 12.5, cursor: "pointer", alignSelf: "center" }}>
                  <input type="checkbox" className="prov-cb" checked={nf.locked} onChange={onField(setNf, "locked")} />
                  <span>Locked <span className="muted">— always included, runs before unlocked stacks</span></span>
                </label>
              </div>
            </fieldset>
            <ErrorBox error={metaErr} />
          </form>
        </Drawer>
      );
    }

    // ---- edit ----
    const memberIds = new Set(members.map((m) => m.software_id));
    const free = software.filter((s) => !memberIds.has(s.id));
    const depIds = new Set(((det && det.depends_on) || []).map((d) => d.stack_id));
    const depChoices = stacks.filter((s) => s.id !== curId && !depIds.has(s.id));
    const explicit = members.some((m) => m.item_order !== null);
    return (
      <Drawer title={det ? "Edit stack · " + det.name : "Edit stack"} sub={det ? det.slug + " · prov_stacks #" + det.id : ""} icon="layers" onClose={onClose}
        footer={<>
          {ro && <span className="muted" style={{ marginRight: "auto", fontSize: 12 }}>Read-only — {OP_ONLY.toLowerCase()}.</span>}
          <button type="button" className="btn-primary" onClick={onClose}>Done</button>
        </>}>
        {detR.loading && !det ? <Skeleton rows={4} />
          : detR.error && !det ? <ErrorBox error={detR.error} onRetry={reloadDet} />
            : det && f ? (
              <>
                <form className="prov-section" onSubmit={saveMeta} noValidate>
                  <div className="prov-section__head"><h3>Stack</h3>
                    {det.locked && <span className="tag"><Icon name="lock" size={10} className="prov-lockico" />always</span>}
                  </div>
                  <fieldset disabled={ro || busy} style={{ border: 0, padding: 0, margin: 0 }}>
                    <div className="prov-grid2">
                      <Field label="Slug" htmlFor="pse-slug-ro" hint="fixed — run requests and dependencies refer to it">
                        <input id="pse-slug-ro" className="prov-input mono" value={det.slug} readOnly disabled />
                      </Field>
                      <Field label="Name" htmlFor="pse-name">
                        <input id="pse-name" className="prov-input" value={f.name} onChange={onField(setF, "name")} aria-invalid={invalid(metaErr, "name") || undefined} data-autofocus />
                      </Field>
                    </div>
                    <Field label="Description" htmlFor="pse-desc">
                      <input id="pse-desc" className="prov-input" value={f.description} onChange={onField(setF, "description")} maxLength={255} aria-invalid={invalid(metaErr, "description") || undefined} />
                    </Field>
                    <div className="prov-grid2" style={{ alignItems: "end" }}>
                      <Field label="Sort order" htmlFor="pse-so">
                        <input id="pse-so" className="prov-input mono" inputMode="numeric" value={f.sort_order} onChange={onField(setF, "sort_order")} aria-invalid={invalid(metaErr, "sort_order") || undefined} />
                      </Field>
                      <label style={{ display: "flex", gap: 10, alignItems: "center", fontSize: 12.5, cursor: "pointer", paddingBottom: 12 }}>
                        <input type="checkbox" className="prov-cb" checked={f.locked} onChange={onField(setF, "locked")} />
                        <span>Locked <span className="muted">— always included</span></span>
                      </label>
                    </div>
                    {!ro && <div style={{ display: "flex", justifyContent: "flex-end" }}><button type="submit" className="btn-ghost">Save details</button></div>}
                  </fieldset>
                  <ErrorBox error={metaErr} />
                </form>

                <div className="prov-section">
                  <div className="prov-section__head"><h3>Members · run order</h3>
                    <span className="prov-faint" style={{ fontSize: 11.5 }}>{members.length} item{members.length === 1 ? "" : "s"}</span>
                    {explicit && !ro && <button type="button" className="linkbtn" style={{ marginLeft: "auto" }} disabled={busy} onClick={resetOrder}>Reset to default order</button>}
                  </div>
                  {!members.length ? <div className="prov-empty" style={{ padding: "18px 0" }}>No members yet — add pool entries below.</div> : (
                    <table className="prov-items">
                      <thead><tr><th scope="col">#</th><th scope="col">Entry</th><th scope="col">Type</th><th scope="col">Order</th><th scope="col"><span className="sr-only">Actions</span></th></tr></thead>
                      <tbody>
                        {members.map((m, i) => (
                          <tr key={m.software_id} data-prov-member={m.software_id}>
                            <td className="mono prov-faint">{i + 1}</td>
                            <td>{m.logical_name}{m.notes && <div className="prov-faint" style={{ fontSize: 11 }}>{m.notes}</div>}</td>
                            <td><span className="tag">{m.item_type}</span></td>
                            <td className="mono" title={m.item_order === null ? "Type default (" + m.item_type + " rank × 100)" : "Explicit order"}>{m.effective_order}{m.item_order === null && <span className="prov-faint"> · default</span>}</td>
                            <td className="prov-acts" style={{ whiteSpace: "nowrap" }}>
                              <IconBtn icon="arrowup" label={"Move " + m.logical_name + " earlier"} disabled={ro || busy || i === 0} onClick={() => move(i, -1)} />
                              <IconBtn icon="arrowdown" label={"Move " + m.logical_name + " later"} disabled={ro || busy || i === members.length - 1} onClick={() => move(i, 1)} />
                              <IconBtn icon="close" danger label={"Remove " + m.logical_name + " from this stack"} disabled={ro || busy} onClick={() => removeMember(m)} />
                            </td>
                          </tr>
                        ))}
                      </tbody>
                    </table>
                  )}
                  {!ro && (
                    <div className="prov-addrow">
                      <label className="sr-only" htmlFor="pse-add">Pool entry to add</label>
                      <select id="pse-add" className="prov-input" value={addSw} onChange={(e) => setAddSw(e.target.value)} disabled={busy || !free.length}>
                        <option value="">{free.length ? "Add a pool entry…" : software.length ? "Every pool entry is already a member" : "The software pool is empty"}</option>
                        {ITEM_TYPES.map((t) => {
                          const opts = free.filter((s) => s.item_type === t);
                          return opts.length ? <optgroup key={t} label={t}>{opts.map((s) => <option key={s.id} value={s.id}>{s.logical_name}{s.version_pin ? " · " + s.version_pin : ""}</option>)}</optgroup> : null;
                        })}
                      </select>
                      <button type="button" className="btn-ghost" disabled={busy || !addSw} onClick={addMember}><Icon name="plus" size={14} /> Add</button>
                    </div>
                  )}
                  <div className="prov-note">Default order is by type (repo → pkg → artifact → build → script → role). Moving an item pins explicit orders for the whole list.</div>
                  <ErrorBox error={memErr} />
                </div>

                <div className="prov-section">
                  <div className="prov-section__head"><h3>Depends on</h3><span className="prov-faint" style={{ fontSize: 11.5 }}>pulled in automatically and run first</span></div>
                  <div className="prov-checks" style={{ marginBottom: 8 }}>
                    {!det.depends_on.length && <span className="prov-faint" style={{ fontSize: 12 }}>No stack dependencies{det.locked ? "" : " (locked stacks always run first anyway)"}.</span>}
                    {det.depends_on.map((d) => (
                      <span key={d.stack_id} className="tag teal">{d.slug}
                        {!ro && <button type="button" className="prov-ib" style={{ width: 18, height: 18, marginLeft: 2 }} disabled={busy}
                          aria-label={"Remove dependency on " + d.slug} onClick={() => removeDependency(d)}><Icon name="close" size={10} /></button>}
                      </span>
                    ))}
                  </div>
                  {!ro && (
                    <div className="prov-addrow narrow">
                      <label className="sr-only" htmlFor="pse-dep">Stack to depend on</label>
                      <select id="pse-dep" className="prov-input" value={addDep} onChange={(e) => setAddDep(e.target.value)} disabled={busy || !depChoices.length}>
                        <option value="">{depChoices.length ? "Add a dependency…" : "No other stacks"}</option>
                        {depChoices.map((s) => <option key={s.id} value={s.id}>{s.name} ({s.slug})</option>)}
                      </select>
                      <button type="button" className="btn-ghost" disabled={busy || !addDep} onClick={addDependency}><Icon name="plus" size={14} /> Add</button>
                    </div>
                  )}
                  {det.depended_on_by.length > 0 && <div className="prov-note">Required by: {det.depended_on_by.map((d) => d.slug).join(", ")}.</div>}
                  <ErrorBox error={depErr} />
                </div>
              </>
            ) : null}
      </Drawer>
    );
  }

  // ======================================================================
  // PART 4 — software pool
  // ======================================================================
  function SoftwarePool({ software, stacks, reloadSoft, reloadStacks, say, op }) {
    const [q, setQ] = useState("");
    const [type, setType] = useState("all");
    const [modal, setModal] = useState(null);
    const rows = software.data || [];
    const usedIn = useMemo(() => {
      const m = {};
      stacks.forEach((st) => (st.members || []).forEach((x) => { (m[x.software_id] = m[x.software_id] || []).push(st.slug); }));
      return m;
    }, [stacks]);
    const counts = useMemo(() => { const c = { all: rows.length }; rows.forEach((r) => { c[r.item_type] = (c[r.item_type] || 0) + 1; }); return c; }, [rows]);
    const needle = q.trim().toLowerCase();
    const shown = rows.filter((r) => (type === "all" || r.item_type === type) && (!needle || [r.logical_name, r.name_suse, r.name_debian, r.name_fedora, r.ansible_role, r.version_pin, r.notes]
      .some((v) => v && String(v).toLowerCase().indexOf(needle) >= 0)));

    function famCell(r, fam) {
      const n = r["name_" + fam];
      const skipped = r.applies_families && r.applies_families.indexOf(fam) < 0;
      if (skipped) return <td key={fam} className="fam none" title={"Not applied on " + FAM_LABEL[fam]}>skipped</td>;
      if (n == null) return <td key={fam} className="fam none" title={"Unmapped on " + FAM_LABEL[fam] + " — a run there is refused"}><span className="tag crit">unmapped</span></td>;
      if (r.item_type !== "pkg") return <td key={fam} className="fam" title={"Supported on " + FAM_LABEL[fam]}><Icon name="check" size={13} style={{ color: "var(--ok)" }} /></td>;
      return <td key={fam} className="fam mono">{n}</td>;
    }
    function del(r) {
      return api().provSoftwareDelete(r.id).then(() => {
        setModal(null);
        say("Deleted pool entry " + r.logical_name, "trash");
        reloadSoft();
      });
    }

    return (
      <section className="panel" aria-labelledby="prov-h-pool">
        <div className="panel__head">
          <Icon name="box" size={16} className="ico" />
          <h3 id="prov-h-pool">Software pool</h3>
          <div className="right">
            <button type="button" className="btn-ghost" disabled={!op} title={op ? "New pool entry" : OP_ONLY} onClick={() => setModal({ kind: "sw", id: null })}>
              <Icon name="plus" size={14} /> New entry
            </button>
          </div>
        </div>
        <div className="panel__body">
          <div className="prov-note" style={{ marginTop: 0 }}>
            <b>pkg</b> = distro package (a real name per family); <b>repo</b> = third-party repository + key; <b>artifact</b> = pinned upstream binary per arch;
            {" "}<b>build</b> = pinned source build; <b>script</b> = guarded install script; <b>role</b> = existing Ansible role.
            {" "}Stored in the SoR; rendered to Ansible vars at run time. No YAML is edited here.
          </div>
          <div className="prov-pool__filters">
            <div className="search" style={{ minWidth: 220, flex: "0 1 320px" }}>
              <Icon name="search" size={14} />
              <label className="sr-only" htmlFor="prov-pool-q">Search the pool</label>
              <input id="prov-pool-q" className="prov-input" placeholder="Search names, pins, roles…" value={q} onChange={(e) => setQ(e.target.value)} />
            </div>
            <div className="prov-checks" role="group" aria-label="Filter by type">
              {["all"].concat(ITEM_TYPES).map((t) => (
                <button key={t} type="button" className={"chip" + (type === t ? " on" : "")} aria-pressed={type === t} onClick={() => setType(t)}>
                  {t} <span className="prov-faint">{counts[t] || 0}</span>
                </button>
              ))}
            </div>
          </div>
        </div>
        {software.loading && !software.data ? <Skeleton rows={5} />
          : software.error && !software.data ? <div className="panel__body"><ErrorBox error={software.error} onRetry={reloadSoft} /></div>
            : !rows.length ? (
              <div className="prov-empty">The pool is empty. Add the packages, repos, artifacts and scripts your stacks are built from.<br />
                <button type="button" className="btn-ghost" disabled={!op} onClick={() => setModal({ kind: "sw", id: null })}><Icon name="plus" size={14} /> New entry</button>
              </div>
            ) : !shown.length ? <div className="prov-empty">Nothing matches{needle ? " “" + q.trim() + "”" : ""}{type !== "all" ? " in " + type : ""}. <button type="button" className="linkbtn" onClick={() => { setQ(""); setType("all"); }}>Clear filters</button></div>
              : (
                <div className="prov-pool__wrap">
                  <table className="prov-pool">
                    <thead><tr>
                      <th scope="col">Logical name</th><th scope="col">Type</th>
                      {FAMS.map((f) => <th key={f} scope="col">{FAM_LABEL[f]}</th>)}
                      <th scope="col">Pin</th><th scope="col">Applies</th><th scope="col">Used in</th><th scope="col"><span className="sr-only">Actions</span></th>
                    </tr></thead>
                    <tbody>
                      {shown.map((r) => {
                        const used = usedIn[r.id] || [];
                        const applies = [r.applies_families ? r.applies_families.join(", ") : null, r.applies_arches ? r.applies_arches.join(", ") : null].filter(Boolean).join(" · ");
                        return (
                          <tr key={r.id}>
                            <td><button type="button" className="linkbtn" onClick={() => setModal({ kind: "sw", id: r.id })}>{r.logical_name}</button>
                              {r.item_type === "role" && r.ansible_role && <div className="mono prov-faint" style={{ fontSize: 11 }}>{r.ansible_role}</div>}
                              {r.notes && <div className="prov-faint" style={{ fontSize: 11 }}>{r.notes}</div>}</td>
                            <td><span className="tag">{r.item_type}</span></td>
                            {FAMS.map((f) => famCell(r, f))}
                            <td className="mono">{r.version_pin || <span className="prov-faint">—</span>}</td>
                            <td className="mono" style={{ fontSize: 11.5 }}>{applies || <span className="prov-faint">all</span>}</td>
                            <td style={{ fontSize: 12 }}>{used.length ? used.join(", ") : <span className="prov-faint">unused</span>}</td>
                            <td className="prov-acts" style={{ whiteSpace: "nowrap" }}>
                              <IconBtn icon="edit" label={"Edit " + r.logical_name} onClick={() => setModal({ kind: "sw", id: r.id })} />
                              <IconBtn icon="trash" danger label={"Delete " + r.logical_name}
                                title={!op ? OP_ONLY : used.length ? "Used in " + used.join(", ") + " — remove it from those stacks first" : "Delete"}
                                disabled={!op || used.length > 0} onClick={() => setModal({ kind: "del", sw: r })} />
                            </td>
                          </tr>
                        );
                      })}
                    </tbody>
                  </table>
                </div>
              )}
        {software.error && software.data && <div className="panel__body"><ErrorBox error={software.error} onRetry={reloadSoft} /></div>}

        {modal && modal.kind === "sw" && (
          <SoftwareEditor id={modal.id} software={rows} op={op} say={say} onClose={() => setModal(null)}
            onChanged={() => { reloadSoft(); reloadStacks(); }} />
        )}
        {modal && modal.kind === "del" && (
          <Confirm title={"Delete " + modal.sw.logical_name + "?"} danger verb="Delete" busyVerb="Deleting…" confirmWord="delete"
            onConfirm={() => del(modal.sw)} onClose={() => setModal(null)}>
            Removes the pool entry with its artifacts and requires/conflicts edges. Hosts already configured with it are unaffected.
          </Confirm>
        )}
      </section>
    );
  }

  // ---- spec <-> structured form (per item type; the server re-validates) ----
  function linesOf(t) { return String(t || "").split("\n").map((x) => x.trim()).filter(Boolean); }
  function emptyRepo() { return { on: false, url: "", key_url: "", fingerprint: "", suite: "", components: "" }; }
  function specState(type, spec) {
    const s = spec || {};
    const src = s.source || {};
    const repo = {};
    FAMS.forEach((f) => {
      const r = s[f];
      repo[f] = r ? { on: true, url: r.url || "", key_url: r.key_url || "", fingerprint: r.fingerprint || "", suite: r.suite || "", components: (r.components || []).join(" ") } : emptyRepo();
    });
    return {
      extract: !!s.extract, creates: s.creates || "",
      src: s.url ? "url" : "inline", body: s.body || "", url: s.url || "", sha256: s.sha256 || "",
      interpreter: s.interpreter || "", run_as: s.run_as || "root",
      env: Object.keys(s.env || {}).map((k) => ({ k: k, v: String(s.env[k]) })),
      check: (s.check || []).join("\n"), version_check: s.version_check || "",
      bsrc: src.tarball ? "tarball" : "git", git: src.git || "", tag: src.tag || "", tarball: src.tarball || "", tsha: src.sha256 || "",
      system: s.system || "cmake", configure_args: (s.configure_args || []).join("\n"), prefix: s.prefix || "",
      build: s.build || "", install: s.install || "",
      repo: repo,
    };
  }
  function specJson(type, st) {
    const put = (o, k, v) => { const x = nn(v); if (x != null) o[k] = x; };
    const env = () => { const e = {}; st.env.forEach((r) => { if (r.k.trim()) e[r.k.trim()] = r.v; }); return Object.keys(e).length ? e : null; };
    if (type === "pkg" || type === "role") return null;
    const o = {};
    if (type === "artifact") {
      if (st.extract) o.extract = true;
      put(o, "creates", st.creates);
      return o;
    }
    if (type === "script") {
      if (st.src === "url") { put(o, "url", st.url); put(o, "sha256", st.sha256); } else if (st.body.trim()) o.body = st.body;
      put(o, "interpreter", st.interpreter);
      o.run_as = st.run_as;
    }
    if (type === "build") {
      o.source = st.bsrc === "git"
        ? Object.assign({ git: st.git.trim() }, nn(st.tag) ? { tag: st.tag.trim() } : {})
        : { tarball: st.tarball.trim(), sha256: st.tsha.trim() };
      o.system = st.system;
      const args = linesOf(st.configure_args);
      if (args.length) o.configure_args = args;
      put(o, "prefix", st.prefix);
      if (st.system === "custom") { put(o, "build", st.build); put(o, "install", st.install); }
    }
    if (type === "script" || type === "build") {
      const e = env();
      if (e) o.env = e;
      put(o, "creates", st.creates);
      const chk = linesOf(st.check);
      if (chk.length) o.check = chk;
      put(o, "version_check", st.version_check);
      return o;
    }
    if (type === "repo") {
      FAMS.forEach((f) => {
        const r = st.repo[f];
        if (!r.on) return;
        const x = {};
        put(x, "url", r.url); put(x, "key_url", r.key_url); put(x, "fingerprint", r.fingerprint);
        if (f === "debian" || r.suite.trim()) put(x, "suite", r.suite);
        const comps = r.components.split(/\s+/).filter(Boolean);
        if (comps.length) x.components = comps;
        o[f] = x;
      });
      return o;
    }
    return o;
  }

  function SpecForm({ type, st, setSt, error }) {
    const upd = (k) => (e) => { const t = e.target; const v = t.type === "checkbox" ? t.checked : t.value; setSt((p) => Object.assign({}, p, { [k]: v })); };
    const inv = (k) => invalid(error, "spec." + k) || undefined;
    const guard = (
      <>
        <div className="prov-grid2">
          <Field label="Creates" htmlFor="pss-creates" hint="absolute path; present = already installed (guard)">
            <input id="pss-creates" className="prov-input mono" value={st.creates} onChange={upd("creates")} aria-invalid={inv("creates")} placeholder="/usr/local/bin/tool" spellCheck={false} />
          </Field>
          <Field label="Version check" htmlFor="pss-vc" hint="regex with one group, matched against the check output; needs a pin">
            <input id="pss-vc" className="prov-input mono" value={st.version_check} onChange={upd("version_check")} aria-invalid={inv("version_check")} placeholder="v([0-9.]+)" spellCheck={false} />
          </Field>
        </div>
        <Field label="Check command" htmlFor="pss-check" hint="argv, one argument per line; exit 0 = installed (guard)">
          <textarea id="pss-check" className="prov-input mono" rows={2} value={st.check} onChange={upd("check")} aria-invalid={inv("check")} placeholder={"/usr/local/bin/tool\n--version"} spellCheck={false} />
        </Field>
      </>
    );
    const envRows = (
      <div className="prov-field">
        <span className="prov-kicker">Environment</span>
        {st.env.map((r, i) => (
          <div key={i} className="prov-addrow" style={{ marginTop: 0, marginBottom: 6 }}>
            <input className="prov-input mono" aria-label={"Variable " + (i + 1) + " name"} value={r.k} placeholder="NAME" spellCheck={false}
              onChange={(e) => setSt((p) => { const env = p.env.slice(); env[i] = { k: e.target.value, v: r.v }; return Object.assign({}, p, { env: env }); })} />
            <input className="prov-input mono" aria-label={"Variable " + (i + 1) + " value"} value={r.v} spellCheck={false}
              onChange={(e) => setSt((p) => { const env = p.env.slice(); env[i] = { k: r.k, v: e.target.value }; return Object.assign({}, p, { env: env }); })} />
            <IconBtn icon="close" danger label={"Remove variable " + (r.k || i + 1)} onClick={() => setSt((p) => Object.assign({}, p, { env: p.env.filter((_, j) => j !== i) }))} />
          </div>
        ))}
        <div><button type="button" className="linkbtn" onClick={() => setSt((p) => Object.assign({}, p, { env: p.env.concat([{ k: "", v: "" }]) }))}>+ Add variable</button>
          <span className="prov-faint" style={{ fontSize: 11.5 }}> · plain values only — secret-looking names are refused</span></div>
      </div>
    );

    if (type === "pkg") return <div className="prov-note">Packages need no spec — the per-family names above are installed by the family's package manager.</div>;
    if (type === "role") return <div className="prov-note">Roles need no spec — the role runs with the stack's rendered vars.</div>;
    if (type === "artifact") {
      return (
        <>
          <label style={{ display: "flex", gap: 10, alignItems: "center", fontSize: 12.5, marginBottom: 10, cursor: "pointer" }}>
            <input type="checkbox" className="prov-cb" checked={st.extract} onChange={upd("extract")} />
            <span>Extract <span className="muted">— the artifact is an archive to unpack</span></span>
          </label>
          <Field label="Creates" htmlFor="pss-creates" hint="absolute path the install produces (idempotency guard)">
            <input id="pss-creates" className="prov-input mono" value={st.creates} onChange={upd("creates")} aria-invalid={inv("creates")} spellCheck={false} />
          </Field>
          <div className="prov-note">Per-arch download URLs and sha256s are managed under <b>Artifacts</b> below once the entry is saved.</div>
        </>
      );
    }
    if (type === "script") {
      return (
        <>
          <div className="prov-mode" role="radiogroup" aria-label="Script source" style={{ marginBottom: 10 }}>
            {[["inline", "Inline body"], ["url", "Remote URL + sha256"]].map(([k, l]) => (
              <button key={k} type="button" role="radio" aria-checked={st.src === k} className={"chip" + (st.src === k ? " on" : "")}
                onClick={() => setSt((p) => Object.assign({}, p, { src: k }))}>{l}</button>
            ))}
          </div>
          {st.src === "inline" ? (
            <Field label="Body" htmlFor="pss-body">
              <textarea id="pss-body" className="prov-input mono" rows={7} value={st.body} onChange={upd("body")} aria-invalid={inv("body")} spellCheck={false} placeholder={"set -eu\n…"} />
            </Field>
          ) : (
            <div className="prov-grid2">
              <Field label="URL" htmlFor="pss-url"><input id="pss-url" className="prov-input mono" value={st.url} onChange={upd("url")} aria-invalid={inv("url")} spellCheck={false} placeholder="https://…/install.sh" /></Field>
              <Field label="sha256" htmlFor="pss-sha" hint="mandatory — an unpinned remote script is refused"><input id="pss-sha" className="prov-input mono" value={st.sha256} onChange={upd("sha256")} aria-invalid={inv("sha256")} spellCheck={false} /></Field>
            </div>
          )}
          <div className="prov-grid2">
            <Field label="Interpreter" htmlFor="pss-int" hint="absolute path (default /bin/sh)">
              <input id="pss-int" className="prov-input mono" value={st.interpreter} onChange={upd("interpreter")} aria-invalid={inv("interpreter")} placeholder="/bin/sh" spellCheck={false} />
            </Field>
            <Field label="Run as" htmlFor="pss-runas">
              <select id="pss-runas" className="prov-input" value={st.run_as} onChange={upd("run_as")} aria-invalid={inv("run_as")}>
                <option value="root">root</option><option value="admin">admin (fleet user)</option>
              </select>
            </Field>
          </div>
          {guard}
          {envRows}
          <div className="prov-note">A script needs an idempotency guard: <b>creates</b> and/or a <b>check</b> command.</div>
        </>
      );
    }
    if (type === "build") {
      return (
        <>
          <div className="prov-mode" role="radiogroup" aria-label="Build source" style={{ marginBottom: 10 }}>
            {[["git", "Git (pinned commit)"], ["tarball", "Tarball + sha256"]].map(([k, l]) => (
              <button key={k} type="button" role="radio" aria-checked={st.bsrc === k} className={"chip" + (st.bsrc === k ? " on" : "")}
                onClick={() => setSt((p) => Object.assign({}, p, { bsrc: k }))}>{l}</button>
            ))}
          </div>
          {st.bsrc === "git" ? (
            <div className="prov-grid2">
              <Field label="Git URL" htmlFor="pss-git"><input id="pss-git" className="prov-input mono" value={st.git} onChange={upd("git")} aria-invalid={inv("source")} spellCheck={false} /></Field>
              <Field label="Tag (informational)" htmlFor="pss-tag" hint="the version pin must be the full commit hash"><input id="pss-tag" className="prov-input mono" value={st.tag} onChange={upd("tag")} spellCheck={false} /></Field>
            </div>
          ) : (
            <div className="prov-grid2">
              <Field label="Tarball URL" htmlFor="pss-tb"><input id="pss-tb" className="prov-input mono" value={st.tarball} onChange={upd("tarball")} aria-invalid={inv("source")} spellCheck={false} /></Field>
              <Field label="sha256" htmlFor="pss-tsha"><input id="pss-tsha" className="prov-input mono" value={st.tsha} onChange={upd("tsha")} aria-invalid={inv("source")} spellCheck={false} /></Field>
            </div>
          )}
          <div className="prov-grid2">
            <Field label="Build system" htmlFor="pss-sys">
              <select id="pss-sys" className="prov-input" value={st.system} onChange={upd("system")} aria-invalid={inv("system")}>
                {BUILD_SYSTEMS.map((b) => <option key={b} value={b}>{b}</option>)}
              </select>
            </Field>
            <Field label="Prefix" htmlFor="pss-prefix" hint="default /usr/local">
              <input id="pss-prefix" className="prov-input mono" value={st.prefix} onChange={upd("prefix")} aria-invalid={inv("prefix")} placeholder="/usr/local" spellCheck={false} />
            </Field>
          </div>
          <Field label="Configure arguments" htmlFor="pss-cfg" hint="one per line">
            <textarea id="pss-cfg" className="prov-input mono" rows={2} value={st.configure_args} onChange={upd("configure_args")} aria-invalid={inv("configure_args")} spellCheck={false} />
          </Field>
          {st.system === "custom" && (
            <div className="prov-grid2">
              <Field label="Build command" htmlFor="pss-b"><input id="pss-b" className="prov-input mono" value={st.build} onChange={upd("build")} aria-invalid={inv("build")} spellCheck={false} /></Field>
              <Field label="Install command" htmlFor="pss-i" hint="installs into $DESTDIR"><input id="pss-i" className="prov-input mono" value={st.install} onChange={upd("install")} aria-invalid={inv("install")} spellCheck={false} /></Field>
            </div>
          )}
          {guard}
          {envRows}
        </>
      );
    }
    // repo
    return (
      <>
        {FAMS.map((f) => {
          const r = st.repo[f];
          const set = (k) => (e) => { const t = e.target; const v = t.type === "checkbox" ? t.checked : t.value; setSt((p) => { const repo = Object.assign({}, p.repo); repo[f] = Object.assign({}, repo[f], { [k]: v }); return Object.assign({}, p, { repo: repo }); }); };
          return (
            <fieldset key={f} className="prov-section" style={{ border: 0, padding: 0, margin: "0 0 12px" }}>
              <legend style={{ padding: 0 }}>
                <label style={{ display: "flex", gap: 10, alignItems: "center", fontSize: 12.5, fontWeight: 600, cursor: "pointer" }}>
                  <input type="checkbox" className="prov-cb" checked={r.on} onChange={set("on")} /> {FAM_LABEL[f]}
                </label>
              </legend>
              {r.on && (
                <div style={{ marginTop: 8 }}>
                  <div className="prov-grid2">
                    <Field label="Repository URL" htmlFor={"pss-r-" + f + "-url"}><input id={"pss-r-" + f + "-url"} className="prov-input mono" value={r.url} onChange={set("url")} aria-invalid={inv(f)} spellCheck={false} /></Field>
                    <Field label="Key URL" htmlFor={"pss-r-" + f + "-key"}><input id={"pss-r-" + f + "-key"} className="prov-input mono" value={r.key_url} onChange={set("key_url")} aria-invalid={inv(f)} spellCheck={false} /></Field>
                  </div>
                  <Field label="Key fingerprint" htmlFor={"pss-r-" + f + "-fp"} hint="full 40- or 64-hex fingerprint; the key is verified before it is trusted">
                    <input id={"pss-r-" + f + "-fp"} className="prov-input mono" value={r.fingerprint} onChange={set("fingerprint")} aria-invalid={inv(f)} spellCheck={false} />
                  </Field>
                  {f === "debian" && (
                    <div className="prov-grid2">
                      <Field label="Suite" htmlFor="pss-r-suite"><input id="pss-r-suite" className="prov-input mono" value={r.suite} onChange={set("suite")} aria-invalid={inv(f)} placeholder="bookworm" spellCheck={false} /></Field>
                      <Field label="Components" htmlFor="pss-r-comp" hint="space-separated"><input id="pss-r-comp" className="prov-input mono" value={r.components} onChange={set("components")} aria-invalid={inv(f)} placeholder="main" spellCheck={false} /></Field>
                    </div>
                  )}
                </div>
              )}
            </fieldset>
          );
        })}
        <div className="prov-note">Tick at least one family. Debian / Ubuntu repositories need a suite and components.</div>
      </>
    );
  }

  function swForm(sw) {
    const r = sw || {};
    const type = r.item_type || "pkg";
    const names = {}, support = {};
    FAMS.forEach((f) => { const n = r["name_" + f]; names[f] = type === "pkg" ? (n || "") : ""; support[f] = sw ? n != null : true; });
    return {
      logical_name: r.logical_name || "", item_type: type, names: names, support: support,
      applies_families: r.applies_families || null, applies_arches: r.applies_arches || null,
      ansible_role: r.ansible_role || "", source_url: r.source_url || "", version_pin: r.version_pin || "", notes: r.notes || "",
    };
  }
  function swBody(f, spec) {
    const b = { logical_name: f.logical_name.trim(), item_type: f.item_type };
    FAMS.forEach((x) => { b["name_" + x] = f.item_type === "pkg" ? nn(f.names[x]) : (f.support[x] ? "=" : null); });
    b.applies_families = f.applies_families;
    b.applies_arches = f.applies_arches;
    b.ansible_role = f.item_type === "role" ? nn(f.ansible_role) : null;
    b.source_url = nn(f.source_url);
    b.version_pin = nn(f.version_pin);
    b.spec = spec;
    b.notes = nn(f.notes);
    return b;
  }

  function SoftwareEditor({ id, software, op, say, onClose, onChanged }) {
    const [curId, setCurId] = useState(id);
    const [det, setDet] = useState(null);
    const [loadErr, setLoadErr] = useState(null);
    const [f, setF] = useState(() => swForm(null));
    const [st, setSt] = useState(() => specState("pkg", null));
    const [error, setError] = useState(null);
    const [relErr, setRelErr] = useState(null);
    const [busy, setBusy] = useState(false);
    const [art, setArt] = useState({ arch: "any", url: "", sha256: "" });
    const [reqPick, setReqPick] = useState("");
    const [cfPick, setCfPick] = useState("");
    const ro = !op;

    function adopt(d) { setDet(d); setF(swForm(d)); setSt(specState(d.item_type, d.spec)); }
    function load(sid) {
      setLoadErr(null);
      return api().provSoftwareGet(sid).then(adopt, setLoadErr);
    }
    useEffect(() => { if (curId) load(curId); }, [curId]);

    const spec = specJson(f.item_type, st);
    const body = swBody(f, spec);
    const editing = !!curId;
    const diff = det ? diffOf(body, det) : body;

    function save(e) {
      e.preventDefault();
      if (ro || busy) return;
      if (det && !Object.keys(diff).length) { say("No changes to save", "check"); return; }
      setBusy(true);
      setError(null);
      const a = api();
      (det ? a.provSoftwareUpdate(det.id, diff) : a.provSoftwareCreate(body)).then((d) => {
        setBusy(false);
        say((det ? "Saved " : "Added ") + d.logical_name, "check");
        onChanged();
        if (!det) { setCurId(d.id); }
        adopt(d);
      }, (err) => { setBusy(false); setError(err); });
    }
    function rel(p, after) {
      setBusy(true);
      setRelErr(null);
      return p.then((d) => {
        setBusy(false);
        if (d && d.logical_name) adopt(d); else load(curId);
        onChanged();
        if (after) after();
      }, (e) => { setBusy(false); setRelErr(e); });
    }
    const a = api();
    const others = software.filter((s) => s.id !== curId);
    const reqIds = new Set(((det && det.requires) || []).map((r) => r.software_id));
    const cfIds = new Set(((det && det.conflicts) || []).map((r) => r.software_id));
    const setType = (t) => setF((p) => Object.assign({}, p, { item_type: t }));
    const inv = (k) => invalid(error, k) || undefined;
    const typeChanged = det && det.item_type !== f.item_type;

    if (editing && !det) {
      return (
        <Drawer title="Edit pool entry" icon="box" onClose={onClose}
          footer={<button type="button" className="btn-primary" onClick={onClose}>Close</button>}>
          {loadErr ? <ErrorBox error={loadErr} onRetry={() => load(curId)} /> : <Skeleton rows={5} />}
        </Drawer>
      );
    }
    return (
      <Drawer title={det ? "Edit · " + det.logical_name : "New pool entry"} sub={det ? det.item_type + " · prov_software #" + det.id : TYPE_HELP[f.item_type]} icon="box" onClose={onClose}
        footer={<>
          {ro && <span className="muted" style={{ marginRight: "auto", fontSize: 12 }}>Read-only — {OP_ONLY.toLowerCase()}.</span>}
          <button type="button" className="btn-ghost" onClick={onClose}>{ro || det ? "Close" : "Cancel"}</button>
          {!ro && <button type="submit" form="prov-sw-form" className="btn-primary" disabled={busy}>{busy ? "Saving…" : det ? "Save" : "Add entry"}</button>}
        </>}>
        <form id="prov-sw-form" onSubmit={save} noValidate>
          <fieldset disabled={ro || busy} style={{ border: 0, padding: 0, margin: 0 }}>
            <div className="prov-section">
              <div className="prov-section__head"><h3>Entry</h3></div>
              <div className="prov-grid2">
                <Field label="Logical name" htmlFor="psw-name" hint="what stacks and the operator call it">
                  <input id="psw-name" className="prov-input" value={f.logical_name} onChange={onField(setF, "logical_name")} aria-invalid={inv("logical_name")} maxLength={96} data-autofocus />
                </Field>
                <Field label="Type" htmlFor="psw-type" hint={TYPE_HELP[f.item_type]}>
                  <select id="psw-type" className="prov-input" value={f.item_type} onChange={(e) => setType(e.target.value)} aria-invalid={inv("item_type")}>
                    {ITEM_TYPES.map((t) => <option key={t} value={t}>{t}</option>)}
                  </select>
                </Field>
              </div>
              {typeChanged && <div className="prov-warnbox" role="status">Changing the type re-validates the names, pin and spec against the new type{det.artifacts && det.artifacts.length ? "; remove its artifact rows first" : ""}.</div>}

              {f.item_type === "pkg" ? (
                <div className="prov-grid3">
                  {FAMS.map((x) => (
                    <Field key={x} label={FAM_LABEL[x] + " package"} htmlFor={"psw-n-" + x}>
                      <input id={"psw-n-" + x} className="prov-input mono" value={f.names[x]} placeholder="blank = unmapped" spellCheck={false} maxLength={128}
                        aria-invalid={inv("name_" + x)}
                        onChange={(e) => { const v = e.target.value; setF((p) => Object.assign({}, p, { names: Object.assign({}, p.names, { [x]: v }) })); }} />
                    </Field>
                  ))}
                </div>
              ) : (
                <div className="prov-field">
                  <span className="prov-kicker">Supported on</span>
                  <div className="prov-checks" role="group" aria-label="Supported families">
                    {FAMS.map((x) => (
                      <label key={x} style={{ display: "inline-flex", gap: 8, alignItems: "center", fontSize: 12.5, cursor: "pointer", marginRight: 14 }}>
                        <input type="checkbox" className="prov-cb" checked={f.support[x]} aria-invalid={inv("name_" + x)}
                          onChange={(e) => { const v = e.target.checked; setF((p) => Object.assign({}, p, { support: Object.assign({}, p.support, { [x]: v }) })); }} />
                        {FAM_LABEL[x]}
                      </label>
                    ))}
                  </div>
                  <div className="prov-note" style={{ marginTop: 4 }}>Unticked = unmapped there: a run on that family is refused unless “Applies to” skips it.</div>
                </div>
              )}

              <div className="prov-grid2">
                <SetChips label="Applies to families" options={FAMS} value={f.applies_families} onChange={(v) => setF((p) => Object.assign({}, p, { applies_families: v }))} />
                <SetChips label="Applies to arches" options={ARCHES} value={f.applies_arches} onChange={(v) => setF((p) => Object.assign({}, p, { applies_arches: v }))} />
              </div>
              <div className="prov-grid2">
                <Field label={"Version pin" + (f.item_type === "artifact" || f.item_type === "build" ? " (required)" : "")} htmlFor="psw-pin"
                  hint={f.item_type === "build" && st.bsrc === "git" ? "full 40/64-hex commit hash" : "one safe token, e.g. 1.4.2"}>
                  <input id="psw-pin" className="prov-input mono" value={f.version_pin} onChange={onField(setF, "version_pin")} aria-invalid={inv("version_pin")} spellCheck={false} maxLength={64} />
                </Field>
                {f.item_type === "role" ? (
                  <Field label="Ansible role" htmlFor="psw-role">
                    <input id="psw-role" className="prov-input mono" value={f.ansible_role} onChange={onField(setF, "ansible_role")} aria-invalid={inv("ansible_role")} spellCheck={false} />
                  </Field>
                ) : (
                  <Field label="Source URL" htmlFor="psw-src" hint="upstream project page (informational)">
                    <input id="psw-src" className="prov-input mono" value={f.source_url} onChange={onField(setF, "source_url")} aria-invalid={inv("source_url")} spellCheck={false} placeholder="https://…" />
                  </Field>
                )}
              </div>
              <Field label="Notes" htmlFor="psw-notes">
                <input id="psw-notes" className="prov-input" value={f.notes} onChange={onField(setF, "notes")} aria-invalid={inv("notes")} maxLength={255} />
              </Field>
            </div>

            <div className="prov-section">
              <div className="prov-section__head"><h3>Spec · {f.item_type}</h3></div>
              <SpecForm type={f.item_type} st={st} setSt={setSt} error={error} />
            </div>
          </fieldset>
          <ErrorBox error={error} />
        </form>

        {det && (
          <fieldset disabled={ro || busy} style={{ border: 0, padding: 0, margin: 0 }}>
            {det.item_type === "artifact" && (
              <div className="prov-section">
                <div className="prov-section__head"><h3>Artifacts</h3><span className="prov-faint" style={{ fontSize: 11.5 }}>one download per arch (“any” = arch-independent)</span></div>
                {det.artifacts.length ? (
                  <table className="prov-items">
                    <thead><tr><th scope="col">Arch</th><th scope="col">URL</th><th scope="col">sha256</th><th scope="col"><span className="sr-only">Actions</span></th></tr></thead>
                    <tbody>{det.artifacts.map((x) => (
                      <tr key={x.id}>
                        <td className="mono">{x.arch}</td>
                        <td className="mono" style={{ wordBreak: "break-all", fontSize: 11.5 }}>{x.url}</td>
                        <td className="mono prov-faint" title={x.sha256} style={{ fontSize: 11.5 }}>{x.sha256.slice(0, 12)}…</td>
                        <td className="prov-acts"><IconBtn icon="trash" danger label={"Remove the " + x.arch + " artifact"} onClick={() => rel(a.provArtifactDelete(det.id, x.id))} /></td>
                      </tr>
                    ))}</tbody>
                  </table>
                ) : <div className="prov-faint" style={{ fontSize: 12, marginBottom: 8 }}>No artifacts yet — a run on any arch fails until one is added.</div>}
                {!ro && (
                  <div className="prov-addrow" style={{ gridTemplateColumns: "110px 1fr 1fr auto" }}>
                    <select className="prov-input mono" aria-label="Artifact arch" value={art.arch} onChange={(e) => setArt(Object.assign({}, art, { arch: e.target.value }))}>
                      {ART_ARCHES.map((x) => <option key={x} value={x}>{x}</option>)}
                    </select>
                    <input className="prov-input mono" aria-label="Artifact URL" placeholder="https://…" value={art.url} spellCheck={false} onChange={(e) => setArt(Object.assign({}, art, { url: e.target.value }))} />
                    <input className="prov-input mono" aria-label="Artifact sha256" placeholder="sha256" value={art.sha256} spellCheck={false} onChange={(e) => setArt(Object.assign({}, art, { sha256: e.target.value }))} />
                    <button type="button" className="btn-ghost" disabled={!art.url.trim() || !art.sha256.trim()}
                      onClick={() => rel(a.provArtifactAdd(det.id, { arch: art.arch, url: art.url.trim(), sha256: art.sha256.trim().toLowerCase() }), () => setArt({ arch: "any", url: "", sha256: "" }))}>
                      <Icon name="plus" size={14} /> Add</button>
                  </div>
                )}
              </div>
            )}

            {[["requires", "Requires", "installed earlier in the same run", reqIds, reqPick, setReqPick,
              (o) => a.provRequireAdd(det.id, o, null), (x) => a.provRequireDelete(det.id, x.software_id)],
              ["conflicts", "Conflicts with", "never selected together in one run", cfIds, cfPick, setCfPick,
              (o) => a.provConflictAdd(det.id, o, null), (x) => a.provConflictDelete(det.id, x.software_id)]].map(([key, title, why, ids, pick, setPick, add, remove]) => {
              const choices = others.filter((s) => !ids.has(s.id));
              return (
                <div className="prov-section" key={key}>
                  <div className="prov-section__head"><h3>{title}</h3><span className="prov-faint" style={{ fontSize: 11.5 }}>{why}</span></div>
                  <div className="prov-checks" style={{ marginBottom: 8 }}>
                    {!det[key].length && <span className="prov-faint" style={{ fontSize: 12 }}>None.</span>}
                    {det[key].map((x) => (
                      <span key={x.software_id} className={"tag" + (key === "conflicts" ? " warn" : " teal")}>{x.logical_name}
                        {!ro && <button type="button" className="prov-ib" style={{ width: 18, height: 18, marginLeft: 2 }}
                          aria-label={"Remove " + title.toLowerCase() + " " + x.logical_name} onClick={() => rel(remove(x))}><Icon name="close" size={10} /></button>}
                      </span>
                    ))}
                  </div>
                  {!ro && (
                    <div className="prov-addrow narrow">
                      <select className="prov-input" aria-label={title + ": entry to add"} value={pick} onChange={(e) => setPick(e.target.value)} disabled={!choices.length}>
                        <option value="">{choices.length ? "Add…" : "No other entries"}</option>
                        {ITEM_TYPES.map((t) => {
                          const opts = choices.filter((s) => s.item_type === t);
                          return opts.length ? <optgroup key={t} label={t}>{opts.map((s) => <option key={s.id} value={s.id}>{s.logical_name}</option>)}</optgroup> : null;
                        })}
                      </select>
                      <button type="button" className="btn-ghost" disabled={!pick} onClick={() => rel(add(Number(pick)), () => setPick(""))}><Icon name="plus" size={14} /> Add</button>
                    </div>
                  )}
                  {key === "requires" && det.required_by.length > 0 && <div className="prov-note">Required by: {det.required_by.map((x) => x.logical_name).join(", ")}.</div>}
                </div>
              );
            })}
            <div className="prov-section">
              <div className="prov-section__head"><h3>Used in stacks</h3></div>
              <div style={{ fontSize: 12.5 }}>{det.stacks.length ? det.stacks.map((x) => x.slug).join(", ") : <span className="prov-faint">Not a member of any stack.</span>}</div>
            </div>
            <ErrorBox error={relErr} />
          </fieldset>
        )}
      </Drawer>
    );
  }

  Object.assign(window, { ProvImagesScreen });
})();
