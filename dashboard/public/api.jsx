/* ============================================================
   SolariNet — live API adapter (Handoff §8.2 wiring pattern)

   Fetches the §11.2 + §6 REST endpoints and populates window.SOLARI
   with the EXACT same shape data.jsx produces, so every screen keeps
   working unchanged. The wire is canonical (KB / milli / ‰ / ISO-8601
   UTC, raw units — Architecture §11.2); ALL unit + time conversion to
   what the UI wants (% and "min ago") lives here.

   data.jsx remains the labelled offline / demo fixture. If there is no
   live API (global flag, <meta>, or a failed/garbage fetch) we fall
   back to that fixture and tag window.SOLARI.source = "offline".

   No build step: plain JS hung off window globals, same idiom as
   data.jsx. Loaded by index.html AFTER data.jsx (so SOLARI/fmt exist
   as the fallback) and BEFORE app.jsx (so app boots against live data).
   ============================================================ */
(function () {
  "use strict";

  // ---- where the REST API lives (same-origin by default) -------------
  function apiBase() {
    var m = document.querySelector('meta[name="solari-api-base"]');
    if (m && m.content) return m.content.replace(/\/+$/, "");
    if (window.SOLARI_API_BASE) return String(window.SOLARI_API_BASE).replace(/\/+$/, "");
    return "/api";
  }
  const BASE = apiBase();

  // ---- "is there a live API?" switch ---------------------------------
  // Force offline:  window.SOLARI_OFFLINE = true  OR  <meta name="solari-mode" content="offline">
  // Force live:     window.SOLARI_OFFLINE = false OR  <meta name="solari-mode" content="live">
  // Default: probe /api/summary; fall back to the fixture if it fails.
  function modeHint() {
    var m = document.querySelector('meta[name="solari-mode"]');
    if (m && m.content) return m.content.trim().toLowerCase();
    if (window.SOLARI_OFFLINE === true) return "offline";
    if (window.SOLARI_OFFLINE === false) return "live";
    return "auto";
  }

  // ---- the offline fixture (whatever data.jsx already built) ----------
  // data.jsx is guaranteed to have run first; keep a frozen reference so
  // a fall-back never re-runs the seeded generator.
  const FIXTURE = window.SOLARI || {};
  // The fmt helpers are defined by data.jsx; reuse them verbatim so the
  // wire->display conversion matches the fixture exactly.
  const fmt = (FIXTURE.fmt) || {
    kb(kb) { if (kb >= 1048576) return (kb / 1048576).toFixed(1) + " GB"; if (kb >= 1024) return (kb / 1024).toFixed(0) + " MB"; return kb + " KB"; },
    mbps(m) { if (m >= 1000) return (m / 1000).toFixed(1) + " Gb/s"; return m + " Mb/s"; },
    ago(min) { if (min < 1) return "just now"; if (min < 60) return min + "m ago"; if (min < 1440) return Math.floor(min / 60) + "h ago"; return Math.floor(min / 1440) + "d ago"; },
    rtt(us) { if (!us) return "—"; if (us >= 1000) return (us / 1000).toFixed(1) + " ms"; return us + " µs"; },
  };

  // =====================================================================
  // wire -> UI primitives (raw units & ISO-8601 in; UI scalars out)
  // =====================================================================
  function minsSince(iso) {
    if (!iso) return null;
    var t = Date.parse(iso);
    if (isNaN(t)) return null;
    return Math.max(0, Math.round((Date.now() - t) / 60000));
  }
  function daysSince(iso) {
    var m = minsSince(iso);
    return m == null ? null : Math.floor(m / 1440);
  }
  function secsSince(iso) {
    if (!iso) return null;
    var t = Date.parse(iso);
    if (isNaN(t)) return null;
    return Math.max(0, Math.round((Date.now() - t) / 1000));
  }
  function pct(used, total) {
    if (!total) return 0;
    return Math.round((used / total) * 100);
  }
  // cpuLoadMilli[] (per-core, 1000 = one full core-second) -> 0..100 %
  function milliToPct(milli) {
    if (milli == null) return 0;
    return Math.max(0, Math.min(100, Math.round(milli / 10)));
  }

  // =====================================================================
  // fetch helpers — unwrap the §11.2 envelope {ok,data,ts}/{ok:false,error}
  // =====================================================================
  function ApiError(code, message) {
    var e = new Error(message || "api error");
    e.code = code || "ERR";
    e.isApiError = true;
    return e;
  }

  function unwrap(json) {
    // accept the stable envelope; also tolerate a bare payload defensively.
    if (json && typeof json === "object" && "ok" in json) {
      if (json.ok === false) {
        var err = json.error || {};
        throw ApiError(err.code, err.message);
      }
      return json.data;
    }
    return json;
  }

  function getJSON(path, opts) {
    opts = opts || {};
    var url = path.charAt(0) === "/" && path.indexOf("/api") === 0 ? path : (BASE + path);
    return fetch(url, {
      method: opts.method || "GET",
      headers: Object.assign(
        { "Accept": "application/json" },
        opts.body ? { "Content-Type": "application/json" } : {}
      ),
      credentials: "same-origin",
      body: opts.body ? JSON.stringify(opts.body) : undefined,
    }).then(function (r) {
      return r.json().catch(function () {
        throw ApiError("BAD_JSON", "HTTP " + r.status + " — non-JSON response");
      }).then(function (j) {
        if (!r.ok && !(j && "ok" in j)) throw ApiError("HTTP_" + r.status, "HTTP " + r.status);
        return unwrap(j);
      });
    });
  }
  // expose raw helpers for screens that want ad-hoc calls
  function post(path, body) { return getJSON(path, { method: "POST", body: body || {} }); }
  function del(path, body) { return getJSON(path, { method: "DELETE", body: body || {} }); }

  // =====================================================================
  // ENDPOINT PATHS (the PHP agent must match these exactly)
  // =====================================================================
  const EP = {
    summary:      "/api/summary",
    nodes:        "/api/nodes",
    node:         function (id) { return "/api/nodes/" + encodeURIComponent(id); },
    nodeHistory:  function (id, metric) { return "/api/nodes/" + encodeURIComponent(id) + "/history?metric=" + encodeURIComponent(metric); },
    nodeConfig:   function (id) { return "/api/nodes/" + encodeURIComponent(id) + "/config"; },
    alerts:       "/api/alerts",            // ?status=active|history|all
    alertAck:     "/api/alerts/ack",         // POST {eventId}
    alertAckAll:  "/api/alerts/ack-all",     // POST {} -> { count }
    pushSubscribe: "/api/push/subscribe",
    pushVapid:     "/api/push/vapid",
    opie:         "/api/opie",               // ?status=all|done|investigating
    opieReport:   function (id) { return "/api/opie/" + encodeURIComponent(id); },
    rules:        "/api/rules",
    rule:         function (id) { return "/api/rules/" + encodeURIComponent(id); },
    probes:       "/api/probes",
    segments:     "/api/segments",
    topology:     function (view) { return "/api/topology?view=" + encodeURIComponent(view || "monitoring"); },
    netgear:      "/api/netgear",
    gear:         "/api/gear",
    gearInterfaces: function (id) { return "/api/gear/" + encodeURIComponent(id) + "/interfaces"; },
    gearHistory:  function (id, ifIndex, metric) {
      return "/api/gear/" + encodeURIComponent(id) + "/history?ifIndex=" + encodeURIComponent(ifIndex) +
             "&metric=" + encodeURIComponent(metric || "inRateKbps");
    },
    discovery:    "/api/discovery",         // ?status=new|all
    discScan:     "/api/discovery/scan",     // POST {cidr, ports?}
    discAdopt:    function (id) { return "/api/discovery/" + encodeURIComponent(id) + "/adopt"; },
    discIgnore:   function (id) { return "/api/discovery/" + encodeURIComponent(id) + "/ignore"; },
    enrollments:  "/api/enrollments",       // ?status=
    enrApprove:   function (id) { return "/api/enrollments/" + encodeURIComponent(id) + "/approve"; },
    enrReject:    function (id) { return "/api/enrollments/" + encodeURIComponent(id) + "/reject"; },
    builds:       "/api/builds",
    config:       "/api/config",
    pools:        "/api/pools",
    pool:         function (id) { return "/api/pools/" + encodeURIComponent(id); },
    assets:       "/api/assets",
    asset:        function (id) { return "/api/assets/" + encodeURIComponent(id); },
    provision:    "/api/control/provision",
    decommission: "/api/control/decommission",
    survey:       "/api/control/survey",
    deploy:       "/api/control/deploy",
    fleetCatalog: "/api/control/fleet-catalog",
    fleetProvision: "/api/control/fleet-provision",
    image:        "/api/control/fleet-image",
    stream:       "/api/stream",
    login:        "/api/auth/login",
    logout:       "/api/auth/logout",
    whoami:       "/api/auth/whoami",
    authConfig:   "/api/auth/config",   // public: which sign-in options to show
    // maintenance windows (scheduled downtime; suppresses alerting)
    maintenance:  "/api/maintenance",   // ?status=active|upcoming|past|all
    maintCancel:  function (id) { return "/api/maintenance/" + encodeURIComponent(id) + "/cancel"; },

    // read-only integrations
    forgejo:      "/api/forgejo",        // Forgejo repos/commits/PRs summary
    ca:           "/api/ca",             // CA/PKI health/roots/provisioners/node-certs
    identity:     "/api/identity",       // Keycloak realm users/factors/groups

    // ---- inventory (SoR migration 014) ----
    inventory:    "/api/inventory",          // combined read: {models,units,locations,receipts,projects,allocations}
    invUnits:     "/api/inventory/units",    // GET list / POST create
    invUnit:      function (id) { return "/api/inventory/units/" + encodeURIComponent(id); },   // POST update
    invModels:    "/api/inventory/models",   // GET list / POST create
    invLocations: "/api/inventory/locations",// GET tree / POST create
    invReceipts:  "/api/inventory/receipts", // GET list / POST create (multipart scan upload)
    invProjects:  "/api/inventory/projects", // GET list / POST create
    invProject:   function (id) { return "/api/inventory/projects/" + encodeURIComponent(id); },
    invAllocations: "/api/inventory/allocations",   // POST create (commit owned unit OR add needed model)
    invAllocation:  function (id) { return "/api/inventory/allocations/" + encodeURIComponent(id); },
    invShopping:  function (pid) { return "/api/inventory/projects/" + encodeURIComponent(pid) + "/shopping"; },
  };

  // =====================================================================
  // mappers: wire row -> the SOLARI shape data.jsx exposes
  // =====================================================================

  // node summary (/api/nodes) — list rows are light; detail (/api/nodes/{id})
  // carries cores/disks/ifaces/procs/hist. mapNode handles either.
  function mapNode(w, segIndex) {
    var seg = segIndex[w.segId] || {};
    var ramTotalKb = w.ramTotalKb || 0;
    var ramUsedKb = w.ramUsedKb || 0;
    var swapTotalKb = w.swapTotalKb || 0;
    var swapUsedKb = w.swapUsedKb || 0;
    var lastSeenMin = w.lastSeenMin != null ? w.lastSeenMin : minsSince(w.lastSeenAt);
    if (lastSeenMin == null) lastSeenMin = 0;

    var cores = (w.cpuLoadMilli || w.cores || []).map(function (c) {
      // detail gives cpuLoadMilli (raw); a re-served fixture may already be %.
      return (w.cpuLoadMilli) ? milliToPct(c) : c;
    });
    var cpuPct = w.cpuPct != null ? w.cpuPct
      : cores.length ? Math.round(cores.reduce(function (a, b) { return a + b; }, 0) / cores.length)
      : milliToPct(w.cpuAvgMilli);

    var disks = (w.disks || []).map(function (d) {
      return {
        mount: d.mount,
        totalGb: d.totalGb != null ? d.totalGb : (d.totalKb ? Math.round(d.totalKb / 1048576) : 0),
        usedPct: d.usedPct != null ? d.usedPct : pct(d.usedKb, d.totalKb),
        fs: d.fs,
      };
    });
    var ifaces = (w.ifaces || []).map(function (f) {
      // wire reports throughput in Kbps -> Mbps for the UI
      var rxMbps = f.rxMbps != null ? f.rxMbps : Math.round((f.rxKbps || 0) / 1000);
      var txMbps = f.txMbps != null ? f.txMbps : Math.round((f.txKbps || 0) / 1000);
      var capMbps = f.capMbps != null ? f.capMbps : (f.speedMbps || 0);
      return { name: f.name, capMbps: capMbps, rxMbps: rxMbps, txMbps: txMbps, errs: f.errs || 0 };
    });
    var procs = (w.procs || []).map(function (p) {
      return {
        name: p.name, pid: p.pid, runState: p.runState,
        nFiles: p.nFiles, nSockets: p.nSockets,
        rssKb: p.rssKb != null ? p.rssKb : 0,
      };
    });

    var ramPct = w.ramPct != null ? w.ramPct : pct(ramUsedKb, ramTotalKb);
    var swapPct = w.swapPct != null ? w.swapPct : pct(swapUsedKb, swapTotalKb);
    var diskMaxPct = disks.reduce(function (m, d) { return Math.max(m, d.usedPct); }, 0);

    var node = {
      nodeId: w.nodeId,
      sym: w.sym || "",
      name: w.name || (w.hostFqdn ? w.hostFqdn.split(".")[0] : String(w.nodeId)),
      hostFqdn: w.hostFqdn,
      ip: w.ip,
      role: w.role,
      segId: w.segId, segName: seg.name || w.segName || "", cidr: seg.cidr || w.cidr || "",
      osName: w.osName, arch: w.arch,
      state: w.state,
      lastSeenMin: lastSeenMin,
      enrolledDaysAgo: w.enrolledDaysAgo != null ? w.enrolledDaysAgo : daysSince(w.enrolledAt),
      configEpoch: w.configEpoch != null ? w.configEpoch : (w.appliedEpoch || 0),
      converged: w.converged != null ? w.converged : true,
      cpuPct: cpuPct, cores: cores,
      ramPct: ramPct, ramUsedKb: ramUsedKb, ramTotalKb: ramTotalKb,
      swapPct: swapPct, swapUsedKb: swapUsedKb, swapTotalKb: swapTotalKb,
      disks: disks, ifaces: ifaces, procs: procs,
      diskMaxPct: diskMaxPct,
      netTotalMbps: ifaces.reduce(function (a, b) { return a + b.rxMbps + b.txMbps; }, 0),
      netCapMbps: ifaces.reduce(function (a, b) { return a + b.capMbps; }, 0),
      alertsCount: w.alertsCount != null ? w.alertsCount : 0,
      hist: w.hist || { cpu: [], ram: [], net: [], disk: [] },
      uptimeDays: w.uptimeDays != null ? w.uptimeDays : daysSince(w.bootAt),
      // network-hierarchy fields (topology view=network)
      uplink: w.uplinkGearId || w.uplink || null,
      uplinkPort: w.uplinkPort || w.localIf || null,
      linkType: w.linkType || null,
      linkSpeedMbps: w.linkSpeedMbps != null ? w.linkSpeedMbps : (w.speedMbps || (ifaces[0] && ifaces[0].capMbps) || 1000),
      lldp: w.lldp != null ? w.lldp : (w.viaLldp || false),
      rssi: w.rssi,
      label: w.label,
    };
    return node;
  }

  function mapProbe(w) {
    var vantages = (w.vantages || []).map(function (v) {
      return {
        monitorNode: v.monitorNode,
        // Always a non-empty string: the API supplies monitorName, but guard
        // against older/partial payloads so the Reachability matrix (which sorts
        // and renders column headers by name) can never hit undefined.
        monitorName: v.monitorName
          || (v.monitorNode != null ? ("mon-" + String(v.monitorNode).slice(-4)) : "monitor"),
        outcome: v.outcome,
        rttMicros: v.rttMicros || 0,
        jitterMicros: v.jitterMicros || 0,
        lossPermille: v.lossPermille != null ? v.lossPermille : 0,
        throughputKbps: v.throughputKbps || 0,
        // wire carries sampledAt (ISO) -> UI wants "min ago" scalar
        sampledMin: v.sampledMin != null ? v.sampledMin : (minsSince(v.sampledAt) || 0),
      };
    });
    var anyBad = vantages.some(function (v) { return v.outcome !== "ok"; });
    var allBad = vantages.length > 0 && vantages.every(function (v) { return v.outcome !== "ok"; });
    return {
      targetId: w.targetId, host: w.host,
      // Cross-view click-through: hostNode is an "asset-<id>" ref (or null) the
      // app's fleet resolver maps to AssetDetail; hostName is a friendly label.
      hostNode: w.hostNode, hostName: w.hostName || null, assetId: w.assetId,
      port: w.port, proto: w.proto, label: w.label,
      replFactor: w.replFactor != null ? w.replFactor : vantages.length,
      vantages: vantages,
      state: w.state || (allBad ? "down" : anyBad ? "degraded" : "up"),
    };
  }

  function mapAlert(w, nodeIndex) {
    var n = w.nodeId != null ? nodeIndex[w.nodeId] : null;
    var cleared = w.cleared != null ? w.cleared : !!w.clearedAt;
    var clearedMinAgo = w.clearedMinAgo != null ? w.clearedMinAgo : minsSince(w.clearedAt);
    // Derived lifecycle status (mirrors the server rule): acked wins, then
    // cleared, else active. Tolerate a server-supplied status when present.
    var status = w.status || (w.ackedAt ? "acked" : (cleared ? "cleared" : "active"));
    return {
      eventId: w.eventId, ruleId: w.ruleId, ruleName: w.ruleName,
      node: w.node || (n ? n.name : null),
      nodeId: w.nodeId != null ? w.nodeId : null,
      segName: w.segName || (n ? n.segName : null),
      severity: w.severity, detail: w.detail,
      firedMinAgo: w.firedMinAgo != null ? w.firedMinAgo : (minsSince(w.firedAt) || 0),
      cleared: cleared,
      clearedMinAgo: clearedMinAgo,
      ackedAt: w.ackedAt || null,
      ackedBy: w.ackedBy || null,
      ackedMinAgo: w.ackedMinAgo != null ? w.ackedMinAgo : minsSince(w.ackedAt),
      status: status,
      opieReportId: w.opieReportId != null ? w.opieReportId : null,
    };
  }

  // opieReport (§ analysis) — the LLM incident write-up attached to an alert.
  function mapOpie(w) {
    return {
      reportId: w.reportId,
      incidentKey: w.incidentKey || null,
      triggerKind: w.triggerKind || null,       // 'crit' | 'warn-storm'
      firstEventId: w.firstEventId != null ? w.firstEventId : null,
      nodeId: w.nodeId != null ? w.nodeId : null,
      hostFqdn: w.hostFqdn || null,
      severity: w.severity || "warn",
      status: w.status || "investigating",       // 'investigating' | 'done' | 'failed'
      summary: w.summary || "",
      analysis: w.analysis != null ? w.analysis : null,   // markdown; only on detail read
      model: w.model || null,
      durationSec: w.durationSec != null ? w.durationSec : null,
      startedAt: w.startedAt || null,
      finishedAt: w.finishedAt || null,
      startedMinAgo: minsSince(w.startedAt),
      finishedMinAgo: minsSince(w.finishedAt),
    };
  }

  function mapDiscovered(w) {
    // mdnsServices arrives as an array from /api/discovery; tolerate the legacy
    // comma-joined string (and null) so an old server never breaks the chips.
    var mdnsSvcs = Array.isArray(w.mdnsServices)
      ? w.mdnsServices
      : (typeof w.mdnsServices === "string"
        ? w.mdnsServices.split(",").map(function (s) { return s.trim(); }).filter(Boolean)
        : []);
    // openPorts arrives as an array from /api/discovery (comma-joined "port/svc"
    // tokens split server-side); tolerate a raw comma-joined string too.
    var openPorts = Array.isArray(w.openPorts)
      ? w.openPorts
      : (typeof w.openPorts === "string"
        ? w.openPorts.split(",").map(function (s) { return s.trim(); }).filter(Boolean)
        : []);
    return {
      discId: w.discId,
      host: w.host || w.ip,
      ip: w.ip,
      via: w.via,
      kind: w.kind,
      services: w.services || [],
      seen: w.seen != null ? w.seen : (minsSince(w.lastSeenAt) || 0),
      seenCount: w.seenCount,
      seg: w.segId || w.seg,
      arch: w.arch,
      status: w.status,
      lastSeenAt: w.lastSeenAt || null,
      firstSeenAt: w.firstSeenAt || null,
      // enrichment (nmap/SNMP/mDNS/OUI) — any may be null on an un-enriched record
      mac: w.mac || null,
      vendor: w.vendor || null,
      osName: w.osName || null,
      deviceRole: w.deviceRole || null,
      sysDescr: w.sysDescr || null,
      enrichedAt: w.enrichedAt || null,
      mdnsName: w.mdnsName || null,
      mdnsServices: mdnsSvcs,
      // active-recon (nmap) enrichment — any may be null on a not-yet-scanned host
      osGuess: w.osGuess || null,
      openPorts: openPorts,
      banners: w.banners || null,
      nmapEnrichedAt: w.nmapEnrichedAt || null,
      // best-effort LLDP/topology neighbor {gearName,peerPort,localIf,linkType,speedMbps,rssi,viaLldp} or null
      neighbor: w.neighbor || null,
    };
  }

  // maintenance window — the API stores/returns startsAt/endsAt as UTC DATETIME
  // strings; we keep them RAW here so the screen can convert to the browser's
  // local zone at render time (see screens5.jsx). 'live' is a server-derived bool.
  function mapMaintenance(w) {
    return {
      windowId: w.windowId,
      scope: w.scope || (w.nodeId == null && !w.hostFqdn ? "all" : "node"),
      nodeId: w.nodeId != null ? w.nodeId : null,
      hostFqdn: w.hostFqdn || null,
      ipAddr: w.ipAddr || w.ip || null,
      reason: w.reason || "",
      startsAt: w.startsAt || null,
      endsAt: w.endsAt || null,
      status: w.status || "scheduled",
      createdBy: w.createdBy || null,
      live: w.live != null ? !!w.live : (w.status === "active"),
    };
  }

  // Forgejo (Git) summary — the server already shapes this; normalise the lists
  // and the available/reason fail-soft envelope so the screen never sees null.
  function mapForgejo(w) {
    w = w || {};
    var A = function (v) { return Array.isArray(v) ? v : []; };
    return {
      available: !!w.available,
      reason: w.reason || null,
      baseUrl: w.baseUrl || null,
      version: w.version || "—",
      counts: w.counts || { repos: A(w.repos).length, openIssues: 0, openPrs: A(w.prs).length },
      repos: A(w.repos),
      commits: A(w.commits),
      prs: A(w.prs),
    };
  }

  // CA / PKI summary — server-shaped; normalise the lists + step-ca descriptor.
  function mapCa(w) {
    w = w || {};
    var A = function (v) { return Array.isArray(v) ? v : []; };
    return {
      stepCa: w.stepCa || { available: false, status: null, reason: "unavailable", url: null },
      roots: A(w.roots),
      provisioners: A(w.provisioners),
      nodeCerts: A(w.nodeCerts),
      warnDays: w.warnDays != null ? w.warnDays : 60,
    };
  }

  // Identity (Keycloak users) summary — server-shaped; normalise the user list
  // and the available/reason fail-soft envelope so the screen never sees null.
  function mapIdentity(w) {
    w = w || {};
    var A = function (v) { return Array.isArray(v) ? v : []; };
    return {
      available: !!w.available,
      reason: w.reason || null,
      realmUrl: w.realmUrl || null,
      users: A(w.users).map(function (u) {
        u = u || {};
        var f = u.factors || {};
        return {
          id: u.id,
          username: u.username || "",
          email: u.email || null,
          enabled: !!u.enabled,
          emailVerified: !!u.emailVerified,
          createdAt: u.createdAt || null,
          groups: A(u.groups),
          factors: {
            password: !!f.password,
            otp: !!f.otp,
            webauthn: !!f.webauthn,
            webauthnPasswordless: !!f.webauthnPasswordless,
            types: A(f.types),
          },
        };
      }),
    };
  }

  function mapEnrollment(w) {
    return {
      enrId: w.enrId,
      host: w.host, ip: w.ip, role: w.role,
      fp: w.certFp || w.fp,
      requestedMin: w.requestedMin != null ? w.requestedMin : (minsSince(w.requestedAt) || 0),
      status: w.status,
    };
  }

  function mapBuild(w) {
    return {
      buildId: w.buildId,
      arch: w.arch, os: w.os, version: w.version, channel: w.channel,
      nodes: w.nodes != null ? w.nodes : (w.nodeCount || 0),
      status: w.status || (w.updateAvailable ? "update" : "current"),
    };
  }

  function mapGear(w) {
    return {
      id: w.gearId || w.id,
      name: w.name, kind: w.kind, model: w.model,
      seg: w.segId || w.seg, ports: w.ports,
      uplink: w.uplinkGearId || w.uplink || null,
      wireless: !!w.wireless,
      attached: w.attached != null ? w.attached : (w.attachedCount || 0),
    };
  }

  // rollups, identical to data.jsx
  function rollup(list) {
    var r = { total: list.length, up: 0, degraded: 0, down: 0, unknown: 0, retired: 0 };
    list.forEach(function (n) { if (r[n.state] != null) r[n.state]++; });
    return r;
  }

  // Synthesize a node-shaped row for a monitored asset (no agent), so adopted
  // systems appear in the Fleet Overview alongside enrolled nodes. Host metrics
  // are blank (reachability-only); state comes from its probe-target rollup.
  function mapAssetNode(a) {
    return {
      nodeId: "asset-" + a.assetId, assetId: a.assetId, isAsset: true,
      sym: "", name: a.displayName || a.ip, hostFqdn: a.host || a.ip, ip: a.ip,
      role: "system", segId: a.poolId, segName: a.poolName || "—", cidr: "",
      osName: a.class, arch: "", state: a.state || "unknown",
      // Reachability-only rows: staleness comes from the probe payload when the
      // API provides it; otherwise null (the UI renders "—", never "just now").
      lastSeenMin: a.lastSeenAt != null ? minsSince(a.lastSeenAt)
        : (a.lastSeenMin != null ? a.lastSeenMin : null),
      enrolledDaysAgo: 0, configEpoch: 0, converged: true,
      cpuPct: 0, cores: [], ramPct: 0, ramUsedKb: 0, ramTotalKb: 0,
      swapPct: 0, swapUsedKb: 0, swapTotalKb: 0,
      disks: [], ifaces: [], procs: [], diskMaxPct: 0,
      netTotalMbps: 0, netCapMbps: 0, alertsCount: 0,
      hist: { cpu: [], ram: [], net: [], disk: [] }, uptimeDays: 0,
      uplink: null, uplinkPort: null, linkType: null, linkSpeedMbps: 0, lldp: false,
      targetCount: a.targetCount, tags: a.tags || [], pool: a.poolName,
    };
  }

  // =====================================================================
  // assemble window.SOLARI live (parallel fetch of the §6/§11.2 reads)
  // =====================================================================
  function loadLive() {
    return Promise.all([
      getJSON(EP.summary),
      getJSON(EP.nodes),
      getJSON(EP.segments),
      getJSON(EP.probes),
      getJSON(EP.alerts + "?status=all"),
      getJSON(EP.rules),
      getJSON(EP.discovery + "?status=new"),
      getJSON(EP.enrollments),
      getJSON(EP.builds),
      getJSON(EP.config),
      getJSON(EP.netgear),
      // new operator-config reads; tolerate failure so they never force the
      // whole dashboard into the offline fixture.
      getJSON(EP.pools).catch(function () { return []; }),
      getJSON(EP.assets).catch(function () { return []; }),
      // whoami — identity chip in the TopBar. Never let it break the boot:
      // an auth-gated deployment already 401s on the reads above.
      getJSON(EP.whoami).catch(function () { return null; }),
      // Opie / Analysis reports — never let a missing panel break the boot.
      getJSON(EP.opie + "?status=all").catch(function () { return []; }),
      // Inventory (SoR migration 014) — tolerate absence so it never forces the
      // whole dashboard offline; the Inventory screen also re-reads on mount.
      getJSON(EP.inventory).catch(function () { return null; }),
      // Maintenance windows — the whole set (active/upcoming/past) so the screen
      // AND the in-maintenance badges (which cross-reference active windows) have
      // the data client-side. Tolerate absence so it never forces the fixture.
      getJSON(EP.maintenance + "?status=all").catch(function () { return []; }),
      // Git (Forgejo) + CA/PKI read-only integrations — tolerate absence so a
      // missing token / unreachable CA never forces the whole dashboard offline.
      getJSON(EP.forgejo).catch(function () { return null; }),
      getJSON(EP.ca).catch(function () { return null; }),
      // Identity (Keycloak) — tolerate absence so a missing/unreachable
      // directory never forces the whole dashboard offline.
      getJSON(EP.identity).catch(function () { return null; }),
    ]).then(function (res) {
      // Defensive unpack: a list endpoint that unexpectedly returns an object
      // must degrade that one section to empty, never throw — a throw here would
      // drop the WHOLE dashboard to the offline fixture.
      var A = function (v) { return Array.isArray(v) ? v : []; };
      var summaryW = res[0] || {};
      var nodesW = A(res[1]);
      var segsW = A(res[2]);
      var probesW = A(res[3]);
      var alertsW = A(res[4]);
      var rulesW = A(res[5]);
      var discW = A(res[6]);
      var enrW = A(res[7]);
      // /api/builds returns { builds:[], nodeVersionDist:[] } — take the array.
      var buildsW = (res[8] && Array.isArray(res[8].builds)) ? res[8].builds : A(res[8]);
      var configW = res[9] || {};
      var gearW = A(res[10]);
      var poolsW = A(res[11]);
      var assetsW = A(res[12]);
      var opieW = A(res[14]);
      var maintW = A(res[16]);
      // Git (Forgejo) + CA/PKI — mapped (null-tolerant) integration payloads.
      var git = mapForgejo(res[17]);
      var ca = mapCa(res[18]);
      var identity = mapIdentity(res[19]);
      // inventory — combined read; normalise each list, tolerate a null/partial payload.
      var invW = res[15] && typeof res[15] === "object" ? res[15] : {};
      var inventory = {
        models: A(invW.models), units: A(invW.units), locations: A(invW.locations),
        receipts: A(invW.receipts), projects: A(invW.projects), allocations: A(invW.allocations),
        seq: (FIXTURE.inventory && FIXTURE.inventory.seq) || { unit: 100, location: 100, receipt: 100, project: 100, allocation: 100, model: 100 },
      };
      // whoami -> S.operator {name, role, displayName, source, directoryEnabled}
      var whoW = res[13];
      var operator = (whoW && whoW.operator) ? {
        name: whoW.operator,
        role: whoW.role || "viewer",
        displayName: whoW.displayName || whoW.operator,
        source: whoW.source || "local",
        directoryEnabled: !!whoW.directoryEnabled,
      } : null;
      // /api/summary carries the lease/failover state (§6) — no separate call.
      var leaseW = (summaryW && summaryW.lease) || {};
      var countsW = (summaryW && summaryW.counts) || {};

      // segments (with adapter-side rollups so the UI shape is identical)
      var segIndex = {};
      var segments = segsW.map(function (s) {
        var seg = { id: s.segId, name: s.label, cidr: s.cidr, desc: s.notes || "", wireless: !!s.wireless };
        segIndex[s.segId] = seg;
        return seg;
      });

      var nodes = nodesW.map(function (w) { return mapNode(w, segIndex); });
      var nodeIndex = {};
      nodes.forEach(function (n) { nodeIndex[n.nodeId] = n; });

      var probes = probesW.map(mapProbe);
      var alerts = alertsW.map(function (a) { return mapAlert(a, nodeIndex); });
      // same ordering discipline as data.jsx
      alerts.sort(function (a, b) {
        var sev = { crit: 0, warn: 1, info: 2 };
        if (a.cleared !== b.cleared) return a.cleared ? 1 : -1;
        if (sev[a.severity] !== sev[b.severity]) return sev[a.severity] - sev[b.severity];
        return a.firedMinAgo - b.firedMinAgo;
      });

      var opie = opieW.map(mapOpie).sort(function (a, b) {
        return (Date.parse(b.startedAt) || 0) - (Date.parse(a.startedAt) || 0);   // newest first
      });
      var maintenance = maintW.map(mapMaintenance).sort(function (a, b) {
        return (Date.parse((a.startsAt || "").replace(" ", "T") + "Z") || 0) -
               (Date.parse((b.startsAt || "").replace(" ", "T") + "Z") || 0);
      });
      var rules = rulesW.map(function (r) { return Object.assign({}, r); });
      var discovered = discW.map(mapDiscovered);
      var enrollments = enrW.map(mapEnrollment);
      var builds = buildsW.map(mapBuild);
      var netgear = gearW.map(mapGear);

      var monitors = nodes.filter(function (n) { return n.role === "monitor"; });

      // adopted systems (assets) appear in the fleet view as reachability-only rows
      var systemNodes = assetsW.map(mapAssetNode);
      var allFleet = nodes.concat(systemNodes);

      // rollups (over nodes + systems so the overview tiles count everything)
      var fleetRoll = rollup(allFleet);
      var segRollups = {};
      segments.forEach(function (s) {
        segRollups[s.id] = rollup(nodes.filter(function (n) { return n.segId === s.id; }));
      });

      // summary — the cheap /api/summary call returns counts.* (§6); fall back
      // to fixture-style derivation from the node list if a count is absent.
      var summary = {
        systems: countsW.nodes != null ? countsW.nodes : nodes.length,
        hosts: countsW.clients != null ? countsW.clients : nodes.filter(function (n) { return n.role === "client"; }).length,
        servers: countsW.servers != null ? countsW.servers : nodes.filter(function (n) { return n.role === "server"; }).length,
        monitors: countsW.monitors != null ? countsW.monitors : monitors.length,
        applications: countsW.applications != null ? countsW.applications : nodes.reduce(function (a, n) { return a + n.procs.length; }, 0),
        probes: countsW.probes != null ? countsW.probes : probes.length,
        version: summaryW.version || "—",
        uptimeStr: summaryW.uptimeStr || (summaryW.uptimeSec ? fmtUptime(summaryW.uptimeSec) : "—"),
      };

      // server lease banner — derived from summary.lease {leaseHolder, leaseEpoch, expiresAt}.
      var primary = nodeIndex[leaseW.leaseHolder];
      var ttlSec = configW.lease ? configW.lease.ttlSec : 15;
      var msToExpiry = leaseW.expiresAt ? (Date.parse(leaseW.expiresAt) - Date.now()) : null;
      var ageSec = msToExpiry != null ? Math.max(0, ttlSec - (msToExpiry / 1000)) : 0;
      var server = {
        primary: primary ? primary.name : (leaseW.leaseHolder || "—"),
        primaryId: leaseW.leaseHolder,
        failover: null,
        failoverId: null,
        leaseEpoch: leaseW.leaseEpoch,
        leaseHealthy: leaseW.expiresAt ? Date.parse(leaseW.expiresAt) > Date.now() : false,
        leaseTtlSec: ttlSec,
        leaseAgeSec: Math.round(ageSec) || 0,
        dbHost: configW.dbHost || "—",
        historyDays: (configW.retention && configW.retention.historyDays) || 90,
      };

      var live = {
        nodes: nodes, segments: segments, segRollups: segRollups, fleetRoll: fleetRoll, summary: summary,
        probes: probes, rules: rules, alerts: alerts, opie: opie, discovered: discovered,
        maintenance: maintenance,
        builds: builds, enrollments: enrollments, config: configW, netgear: netgear,
        pools: poolsW, assets: assetsW,
        inventory: inventory,
        git: git, ca: ca, identity: identity,
        systemNodes: systemNodes, fleet: allFleet,
        monitors: monitors,
        server: server,
        operator: operator,
        activeCrit: alerts.filter(function (a) { return !a.ackedAt && !a.cleared && a.severity === "crit"; }).length,
        activeWarn: alerts.filter(function (a) { return !a.ackedAt && !a.cleared && a.severity === "warn"; }).length,
        fmt: fmt,
        source: "live",
        api: API,
      };
      return live;
    });
  }

  function fmtUptime(sec) {
    var d = Math.floor(sec / 86400);
    var h = Math.floor((sec % 86400) / 3600);
    return d + "d " + String(h).padStart(2, "0") + "h";
  }

  // =====================================================================
  // on-demand loaders (detail view + history; keep the same shapes)
  // =====================================================================
  function loadNode(id) {
    var segIndex = {};
    (window.SOLARI.segments || []).forEach(function (s) { segIndex[s.id] = s; });
    return getJSON(EP.node(id)).then(function (w) {
      // /api/nodes/{id} nests {node, host, procs, alerts}; mapNode wants a flat
      // record, so merge node identity + host metrics (+ procs) before mapping.
      var flat = Object.assign({}, w.node || {}, w.host || {});
      if (w.procs) flat.procs = w.procs;
      return mapNode(flat, segIndex);
    });
  }
  function historyMetric(metric) {
    var map = {
      cpu: "cpuAvgMilli",
      ram: "ramUsedKb",
      swap: "swapUsedKb",
      disk: "diskMinFreePct",
      net: "netKbps",
    };
    return map[metric] || metric || "cpuAvgMilli";
  }

  function historyValue(raw, metric, opts) {
    var v = Number(raw || 0);
    opts = opts || {};
    if (metric === "cpuAvgMilli") return milliToPct(v);
    if (metric === "ramUsedKb") return pct(v, opts.totalKb || opts.ramTotalKb || 0);
    if (metric === "swapUsedKb") return pct(v, opts.totalKb || opts.swapTotalKb || 0);
    if (metric === "diskMinFreePct") return Math.max(0, Math.min(100, 100 - v));
    if (metric === "netKbps") return Math.round(v / 1000 * 10) / 10;  // Kbps -> Mbps
    return v;
  }

  function loadNodeHistory(id, metric, opts) {
    // server returns {series:[{ts,value},...]} today; tolerate {points:[..]} and
    // bare arrays so older fixtures still render. Return the UI spark array.
    var wireMetric = historyMetric(metric);
    return getJSON(EP.nodeHistory(id, wireMetric)).then(function (w) {
      var series = Array.isArray(w) ? w : ((w && (w.series || w.points)) || []);
      return series.map(function (p) {
        var raw = (p && typeof p === "object" && "value" in p) ? p.value : p;
        return historyValue(raw, wireMetric, opts);
      });
    });
  }
  function loadTopology(view) { return getJSON(EP.topology(view)); }

  // =====================================================================
  // mutation helpers — route through the POST endpoints; surface {ok:false}
  // =====================================================================
  const API = {
    base: BASE,
    endpoints: EP,
    get: getJSON,
    post: post,
    delete: del,

    // detail / history / topology reads
    node: loadNode,
    nodeHistory: loadNodeHistory,
    topology: loadTopology,

    // SNMP-managed gear (read-only tier) — list, per-gear interfaces, and an
    // interface time series. gearHistory returns the raw {series:[{ts,value}]}
    // Kbps points (Sparkline auto-scales, so no unit conversion needed here).
    gearList: function () { return getJSON(EP.gear); },
    gearInterfaces: function (id) {
      return getJSON(EP.gearInterfaces(id)).then(function (w) {
        return Array.isArray(w) ? w : ((w && w.interfaces) || []);
      });
    },
    gearHistory: function (id, ifIndex, metric) {
      return getJSON(EP.gearHistory(id, ifIndex, metric)).then(function (w) {
        var series = Array.isArray(w) ? w : ((w && w.series) || []);
        return series.map(function (p) {
          return (p && typeof p === "object" && "value" in p) ? Number(p.value || 0) : Number(p || 0);
        });
      });
    },

    // discovery
    // Full candidate list for the Discovery screen (status: new|ignored|adopting|
    // adopted|all). Returns mapped rows so the screen renders identical shapes to
    // the boot fixture; defaults to every status so the screen can filter locally.
    listDiscovered: function (status) {
      return getJSON(EP.discovery + "?status=" + encodeURIComponent(status || "all"))
        .then(function (w) { return (Array.isArray(w) ? w : []).map(mapDiscovered); });
    },
    discoverScan: function (cidr, ports) { return post(EP.discScan, { cidr: cidr, ports: ports || "" }); },
    adoptDiscovered: function (discId, body) { return post(EP.discAdopt(discId), body || {}); },
    ignoreDiscovered: function (discId) { return post(EP.discIgnore(discId), {}); },
    // pools + assets (operator config; writes go to the C bridge server-side)
    pools: function () { return getJSON(EP.pools); },
    createPool: function (body) { return post(EP.pools, body || {}); },
    updatePool: function (id, body) { return post(EP.pool(id), body || {}); },
    assets: function () { return getJSON(EP.assets); },
    asset: function (id) { return getJSON(EP.asset(id)); },
    updateAsset: function (id, body) { return post(EP.asset(id), body || {}); },
    // removal / lifecycle (PR-C endpoints) — all destructive, all double-confirmed:
    // the UI gates on a real confirm dialog AND the body carries confirm:true.
    removeAsset: function (id) { return post(EP.asset(id) + "/remove", { confirm: true }); },
    removeTarget: function (assetId, targetId) {
      return post(EP.asset(assetId) + "/targets/" + encodeURIComponent(targetId) + "/remove", { confirm: true });
    },
    retireNode: function (id) { return post(EP.node(id) + "/retire", { confirm: true }); },
    deletePool: function (id) { return post(EP.pool(id) + "/delete", { confirm: true }); },
    deleteRule: function (id) { return post(EP.rule(id) + "/delete", { confirm: true }); },

    // enrollment — approve is destructive (signs a cert) so it carries the
    // explicit double-confirm the PHP layer + solariCtl demand.
    approveEnrollment: function (enrId) { return post(EP.enrApprove(enrId), { confirm: true }); },
    rejectEnrollment: function (enrId) { return post(EP.enrReject(enrId), {}); },

    // lifecycle
    provision: function (body) { return post(EP.provision, body || {}); }, // {nodeId|enrId, configBlob, buildId}
    decommission: function (nodeId, wipeScope) {
      return post(EP.decommission, { nodeId: nodeId, wipeScope: wipeScope || ["config", "certs", "spool", "logs", "unit"], confirm: true });
    },

    // config & rules
    saveConfig: function (config) { return post(EP.config, config); },
    getNodeConfig: function (id) { return getJSON(EP.nodeConfig(id)); },
    setNodeConfig: function (id, cfg) { return post(EP.nodeConfig(id), { config: cfg }); },
    // remote client deployment (server-side, via the bridge; poll the log)
    deployClient: function (body) { return post(EP.deploy, body || {}); },
    deployLog: function (host) { return getJSON(EP.deploy + "?host=" + encodeURIComponent(host)); },
    // fleet provisioning (bare-metal OS install + Pi imaging), server-side via bridge
    fleetCatalog: function () { return getJSON(EP.fleetCatalog); },
    fleetProvision: function (body) { return post(EP.fleetProvision, body || {}); },
    buildImage: function (body) { return post(EP.image, body || {}); },
    provisionLog: function (hostname, isImage) {
      return getJSON(EP.fleetProvision + "?hostname=" + encodeURIComponent(hostname) + (isImage ? "&image=1" : ""));
    },
    saveRule: function (ruleId, patch) { return post(EP.rule(ruleId), patch); },
    toggleRule: function (ruleId, enabled) { return post(EP.rule(ruleId), { enabled: enabled }); },

    // survey (already in base §11.2)
    survey: function (scope) { return post(EP.survey, { scope: scope || "all" }); },

    // ---- inventory (SoR migration 014) ----
    // Combined read → {models,units,locations,receipts,projects,allocations}.
    inventory:        function () { return getJSON(EP.inventory); },
    // components (hardware_units)
    createUnit:       function (body) { return post(EP.invUnits, body || {}); },
    updateUnit:       function (id, body) { return post(EP.invUnit(id), body || {}); },
    // models catalog (hardware_models)
    createModel:      function (body) { return post(EP.invModels, body || {}); },
    // locations (drawer/tray/bin hierarchy)
    createLocation:   function (body) { return post(EP.invLocations, body || {}); },
    // receipts — scan upload is multipart (FormData); pass a FormData for the file.
    createReceipt:    function (body) { return post(EP.invReceipts, body || {}); },
    uploadReceipt:    function (form) {
      // form is a FormData (vendor, purchased_on, subtotal_cents, order_ref, scan file).
      var url = EP.invReceipts.charAt(0) === "/" ? EP.invReceipts : (BASE + EP.invReceipts);
      return fetch(url, { method: "POST", credentials: "same-origin", body: form })
        .then(function (r) { return r.json().then(unwrap); });
    },
    // projects + allocations
    createProject:    function (body) { return post(EP.invProjects, body || {}); },
    updateProject:    function (id, body) { return post(EP.invProject(id), body || {}); },
    // Commit an owned unit (allocation:'committed'|'installed') OR add a needed
    // model (allocation:'needed', hardware_model_id). The DB double-commit guard
    // SIGNALs SQLSTATE 45000 → the server maps it to HTTP 409; callers catch it.
    createAllocation: function (body) { return post(EP.invAllocations, body || {}); },
    updateAllocation: function (id, body) { return post(EP.invAllocation(id), body || {}); },
    // per-project shopping list (v_project_shopping): needed items with no owned unit.
    shopping:         function (projectId) { return getJSON(EP.invShopping(projectId)); },

    // refresh the whole model in place (re-runs loadLive, keeps fmt/api). Mutates
    // window.SOLARI in place so captured `const S = window.SOLARI` refs stay live.
    refresh: function () {
      return loadLive().then(function (live) {
        Object.keys(live).forEach(function (k) { window.SOLARI[k] = live[k]; });
        window.SOLARI.source = "live";
        window.SOLARI.api = API;
        return window.SOLARI;
      });
    },

    // ---- read-only integrations (Git / CA) ----
    // Forgejo summary (repos/commits/PRs); server envelope carries available/reason.
    forgejoInfo: function () { return getJSON(EP.forgejo).then(mapForgejo); },
    // CA/PKI summary (step-ca health/roots/provisioners/node-certs).
    caInfo: function () { return getJSON(EP.ca).then(mapCa); },
    // Identity summary (Keycloak realm users/factors/groups).
    identityInfo: function () { return getJSON(EP.identity).then(mapIdentity); },

    // ---- alert lifecycle ----
    ackAlert: function (eventId) { return post(EP.alertAck, { eventId: eventId }); },
    // Ack every currently-ACTIVE alert; resolves to { count }.
    ackAllAlerts: function () { return post(EP.alertAckAll, {}); },

    // ---- Opie / Analysis reports ----
    opieList: function (status) { return getJSON(EP.opie + "?status=" + encodeURIComponent(status || "all")); },
    opieReport: function (reportId) { return getJSON(EP.opieReport(reportId)); },

    // ---- maintenance windows ----
    // list windows by status ('active'|'upcoming'|'past'|'all'); rows carry
    // startsAt/endsAt as UTC DATETIME strings + a derived 'live' bool.
    maintenanceList: function (status) {
      return getJSON(EP.maintenance + "?status=" + encodeURIComponent(status || "all"))
        .then(function (w) { return (Array.isArray(w) ? w : []).map(mapMaintenance); });
    },
    // schedule a window. body: { host?|all:true, reason?, hours?:number
    //                            | from?+to?:"YYYY-MM-DD HH:MM" }
    scheduleMaintenance: function (body) { return post(EP.maintenance, body || {}); },
    cancelMaintenance: function (windowId) { return post(EP.maintCancel(windowId), {}); },

    // ---- authentication (§11.1) ----
    whoami: function () { return getJSON(EP.whoami); },
    // Public auth config — readable while logged out so the login screen knows
    // whether to offer the SSO button. Never carries secrets.
    authConfig: function () { return getJSON(EP.authConfig); },
    login:  function (username, password) { return post(EP.login, { username: username, password: password }); },
    logout: function () { return post(EP.logout, {}); },

    // ---- browser Web Push ----
    pushVapid: function () { return getJSON(EP.pushVapid); },
    subscribePush: function (subscription) { return post(EP.pushSubscribe, subscription); },
    unsubscribePush: function (endpoint) { return del(EP.pushSubscribe, { endpoint: endpoint }); },

    // live SSE stream (§8.4 / §11.1) — optional; screens may subscribe.
    stream: function (onEvent) {
      if (!window.EventSource) return null;
      var es = new EventSource(BASE === "/api" ? EP.stream : (BASE + "/stream"));
      ["nodeStateChange", "alertFired", "probeUpdate"].forEach(function (name) {
        es.addEventListener(name, function (e) {
          var data; try { data = JSON.parse(e.data); } catch (_) { data = e.data; }
          onEvent && onEvent(name, data);
        });
      });
      return es;
    },
  };

  // =====================================================================
  // boot: decide live vs. offline, then resolve window.solariReady
  // =====================================================================
  function useFixture(reason) {
    // keep the seeded fixture as-is, just label it.
    window.SOLARI = FIXTURE;
    window.SOLARI.source = "offline";
    window.SOLARI.api = API;            // mutations still callable (will 404 offline, handled by caller)
    window.SOLARI.offlineReason = reason || null;
    if (window.SOLARI.operator === undefined) window.SOLARI.operator = null;  // no session offline — the profile chip hides
    return window.SOLARI;
  }

  // Apply a freshly-loaded live model onto the EXISTING window.SOLARI object
  // in place (rather than replacing the reference). data.jsx, app.jsx, and every
  // screen capture `const S = window.SOLARI` at module-load — mutating in place
  // keeps all of those references live without touching any screen file.
  function applyLive(live) {
    var target = window.SOLARI || (window.SOLARI = {});
    Object.keys(live).forEach(function (k) { target[k] = live[k]; });
    target.source = "live";
    target.api = API;
    return target;
  }

  // An authenticated-API error (401 / login failure) is NOT the same as the API
  // being unreachable: we must show a login screen, not silently serve the demo
  // fixture. app.jsx checks window.SOLARI_NEEDS_AUTH to gate on a login form.
  function isAuthErr(err) {
    if (!err) return false;
    return err.code === "unauthorized" ||
           err.code === "invalid_credentials" ||
           err.code === "HTTP_401";
  }

  var hint = modeHint();
  var bootP;
  if (hint === "offline") {
    bootP = Promise.resolve(useFixture("forced offline"));
  } else {
    bootP = loadLive().then(function (live) {
      window.SOLARI_NEEDS_AUTH = false;
      return applyLive(live);
    }).catch(function (err) {
      if (isAuthErr(err)) {
        // Live API is up but we are not logged in: gate on the login screen.
        window.SOLARI_NEEDS_AUTH = true;
        return useFixture("authentication required");
      }
      if (hint === "live") {
        // explicitly live: surface the failure but still keep the app bootable
        console.error("[SolariNet] live API required but failed:", err);
      } else {
        console.warn("[SolariNet] live API unavailable, using offline fixture:", err && err.message);
      }
      return useFixture(err && err.message);
    });
  }

  // app.jsx can `await window.solariReady` before first render; if it
  // doesn't, window.SOLARI is still populated synchronously enough for
  // the existing synchronous boot (fixture) and re-rendered on refresh.
  window.solariReady = bootP;
  window.SolariAPI = API;

  // ---------------------------------------------------------------------
  // service worker registration (§8.4). sw.js lives next to index.html.
  // index.html is owned by another agent; if it registers the SW there,
  // this is a harmless no-op (re-register resolves to the same worker).
  // ---------------------------------------------------------------------
  if ("serviceWorker" in navigator) {
    window.addEventListener("load", function () {
      navigator.serviceWorker.register("/sw.js").catch(function (e) {
        console.warn("[SolariNet] service worker registration failed:", e && e.message);
      });
    });
  }
})();
