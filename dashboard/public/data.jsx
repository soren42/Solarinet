/* ============================================================
   SolariNet — mock data layer (deterministic, seeded)
   Exposes window.SOLARI = { nodes, segments, alerts, rules, server, ... }
   ============================================================ */
(function () {
  // ---- seeded RNG (mulberry32) for stable data across reloads ----
  function mulberry32(a) {
    return function () {
      a |= 0; a = (a + 0x6D2B79F5) | 0;
      let t = Math.imul(a ^ (a >>> 15), 1 | a);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }
  const rng = mulberry32(20260606);
  const rint = (lo, hi) => Math.floor(rng() * (hi - lo + 1)) + lo;
  const pick = (arr) => arr[Math.floor(rng() * arr.length)];
  const chance = (p) => rng() < p;

  // ---- periodic table (symbol + name) ----
  const ELEMENTS = [
    ["H", "hydrogen"], ["He", "helium"], ["Li", "lithium"], ["Be", "beryllium"], ["B", "boron"],
    ["C", "carbon"], ["N", "nitrogen"], ["O", "oxygen"], ["F", "fluorine"], ["Ne", "neon"],
    ["Na", "sodium"], ["Mg", "magnesium"], ["Al", "aluminium"], ["Si", "silicon"], ["P", "phosphorus"],
    ["S", "sulfur"], ["Cl", "chlorine"], ["Ar", "argon"], ["K", "potassium"], ["Ca", "calcium"],
    ["Sc", "scandium"], ["Ti", "titanium"], ["V", "vanadium"], ["Cr", "chromium"], ["Mn", "manganese"],
    ["Fe", "iron"], ["Co", "cobalt"], ["Ni", "nickel"], ["Cu", "copper"], ["Zn", "zinc"],
    ["Ga", "gallium"], ["Ge", "germanium"], ["As", "arsenic"], ["Se", "selenium"], ["Br", "bromine"],
    ["Kr", "krypton"], ["Rb", "rubidium"], ["Sr", "strontium"], ["Y", "yttrium"], ["Zr", "zirconium"],
    ["Nb", "niobium"], ["Mo", "molybdenum"], ["Tc", "technetium"], ["Ru", "ruthenium"], ["Rh", "rhodium"],
    ["Pd", "palladium"], ["Ag", "silver"], ["Cd", "cadmium"], ["In", "indium"], ["Sn", "tin"],
    ["Sb", "antimony"], ["Te", "tellurium"], ["I", "iodine"], ["Xe", "xenon"], ["Cs", "caesium"],
    ["Ba", "barium"], ["La", "lanthanum"], ["Ce", "cerium"], ["Pr", "praseodymium"], ["Nd", "neodymium"],
    ["Pm", "promethium"], ["Sm", "samarium"], ["Eu", "europium"], ["Gd", "gadolinium"], ["Tb", "terbium"],
    ["Dy", "dysprosium"], ["Ho", "holmium"], ["Er", "erbium"], ["Tm", "thulium"], ["Yb", "ytterbium"],
    ["Lu", "lutetium"], ["Hf", "hafnium"], ["Ta", "tantalum"], ["W", "tungsten"], ["Re", "rhenium"],
    ["Os", "osmium"], ["Ir", "iridium"], ["Pt", "platinum"], ["Au", "gold"], ["Hg", "mercury"],
    ["Tl", "thallium"], ["Pb", "lead"], ["Bi", "bismuth"], ["Po", "polonium"], ["At", "astatine"],
    ["Rn", "radon"], ["Fr", "francium"], ["Ra", "radium"], ["Ac", "actinium"], ["Th", "thorium"],
    ["Pa", "protactinium"], ["U", "uranium"], ["Np", "neptunium"], ["Pu", "plutonium"], ["Am", "americium"],
    ["Cm", "curium"], ["Bk", "berkelium"], ["Cf", "californium"], ["Es", "einsteinium"], ["Fm", "fermium"],
    ["Md", "mendelevium"], ["No", "nobelium"], ["Lr", "lawrencium"], ["Rf", "rutherfordium"], ["Db", "dubnium"],
    ["Sg", "seaborgium"], ["Bh", "bohrium"], ["Hs", "hassium"], ["Mt", "meitnerium"], ["Ds", "darmstadtium"],
    ["Rg", "roentgenium"], ["Cn", "copernicium"], ["Nh", "nihonium"], ["Fl", "flerovium"], ["Mc", "moscovium"],
    ["Lv", "livermorium"], ["Ts", "tennessine"], ["Og", "oganesson"]
  ];

  // ---- network segments ----
  const SEGMENTS = [
    { id: "core", name: "Core", cidr: "10.42.0.0/24", desc: "Routers, gateways, infra services" },
    { id: "compute", name: "Compute", cidr: "10.42.10.0/23", desc: "Hypervisors & app hosts" },
    { id: "storage", name: "Storage", cidr: "10.42.20.0/24", desc: "NAS, object, backup" },
    { id: "dmz", name: "DMZ", cidr: "10.42.30.0/24", desc: "Edge / reverse proxies" },
    { id: "lab", name: "Lab", cidr: "10.42.40.0/23", desc: "ARM SBCs, experiments" },
    { id: "iot", name: "IoT / OT", cidr: "10.42.50.0/24", desc: "Sensors, controllers" },
  ];

  const OSES = [
    { os: "Debian 12", archs: ["x86_64", "arm64"] },
    { os: "Ubuntu 24.04", archs: ["x86_64", "arm64"] },
    { os: "Alpine 3.20", archs: ["x86_64", "armv7", "arm64"] },
    { os: "Raspberry Pi OS", archs: ["arm64", "armv7"] },
    { os: "FreeBSD 14", archs: ["x86_64"] },
    { os: "Windows Server 2022", archs: ["x86_64"] },
    { os: "macOS 14", archs: ["arm64"] },
  ];

  const SERVICES = ["apache2", "mariadbd", "nginx", "sshd", "redis-server", "node_exporter", "haproxy", "postgres", "dockerd", "coredns", "vault", "step-ca"];
  const DISK_MOUNTS = ["/", "/var", "/data", "/srv", "/backup"];
  const IFACES = ["eth0", "eth1", "wlan0", "bond0", "usb0"];

  function fnv(str) {
    let h = 0xcbf29ce484222325n;
    for (let i = 0; i < str.length; i++) { h ^= BigInt(str.charCodeAt(i)); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; }
    return h.toString(16).padStart(16, "0");
  }

  function spark(n, base, vol, trend) {
    const out = []; let v = base;
    for (let i = 0; i < n; i++) {
      v += (rng() - 0.5) * vol + trend;
      v = Math.max(0, Math.min(100, v));
      out.push(Math.round(v * 10) / 10);
    }
    return out;
  }

  // ---- build fleet ----
  const nodes = [];
  let idx = 0;

  function makeNode(role, segId, forceState) {
    const [sym, baseName] = ELEMENTS[idx % ELEMENTS.length];
    const wrap = Math.floor(idx / ELEMENTS.length);
    const name = wrap > 0 ? baseName + (wrap + 1) : baseName; // e.g. iron2 on wrap
    idx++;
    const seg = SEGMENTS.find((s) => s.id === segId);
    const osPick = pick(OSES);
    const arch = pick(osPick.archs);
    // state distribution
    let state = forceState;
    if (!state) {
      const r = rng();
      state = r < 0.80 ? "up" : r < 0.90 ? "degraded" : r < 0.965 ? "down" : "unknown";
    }
    const isDown = state === "down";
    const isUnknown = state === "unknown";
    const deg = state === "degraded";

    const cpuBase = isDown ? 0 : deg ? rint(72, 95) : rint(6, 55);
    const ramTotal = pick([4, 8, 16, 32, 64, 128]) * 1024 * 1024; // KB
    const ramPct = isDown ? 0 : deg && chance(0.5) ? rint(82, 97) : rint(20, 75);
    const swapTotal = ramTotal / 2;
    const swapPct = deg && chance(0.4) ? rint(30, 80) : rint(0, 15);

    const ncores = pick([2, 4, 4, 8, 8, 16, 32]);
    const cores = Array.from({ length: ncores }, () =>
      isDown ? 0 : Math.max(0, Math.min(100, Math.round(cpuBase + (rng() - 0.5) * 40)))
    );

    const ndisks = rint(1, role === "server" ? 4 : 2);
    const disks = Array.from({ length: ndisks }, (_, i) => {
      const totalGb = pick([64, 128, 256, 512, 1024, 2048, 4096]);
      const usedPct = deg && chance(0.5) ? rint(85, 98) : rint(18, 80);
      return { mount: DISK_MOUNTS[i] || `/mnt/d${i}`, totalGb, usedPct, fs: pick(["ext4", "xfs", "zfs", "btrfs"]) };
    });

    const nif = rint(1, role === "monitor" ? 3 : 2);
    const ifaces = Array.from({ length: nif }, (_, i) => {
      const cap = pick([100, 1000, 1000, 2500, 10000]); // Mbps
      const rxMbps = isDown ? 0 : Math.round(rng() * cap * (deg ? 0.85 : 0.4));
      const txMbps = isDown ? 0 : Math.round(rng() * cap * (deg ? 0.7 : 0.3));
      return { name: IFACES[i] || `eth${i}`, capMbps: cap, rxMbps, txMbps, errs: deg && chance(0.5) ? rint(20, 400) : rint(0, 4) };
    });

    const nproc = role === "client" ? rint(2, 5) : rint(2, 4);
    const usedServices = [];
    const procs = Array.from({ length: nproc }, () => {
      let s = pick(SERVICES); let guard = 0;
      while (usedServices.includes(s) && guard++ < 6) s = pick(SERVICES);
      usedServices.push(s);
      const runState = isDown ? "Z" : deg && chance(0.3) ? "D" : "R";
      return {
        name: s, pid: rint(200, 39000), runState,
        nFiles: rint(8, 520), nSockets: rint(1, 180),
        rssKb: rint(8000, ramTotal * (ramPct / 100) / 2 | 0 || 200000),
      };
    });

    const lastSeenMin = isDown ? rint(3, 90) : isUnknown ? rint(120, 1440) : rint(0, 1);
    const alertsCount = isDown ? rint(1, 3) : deg ? rint(0, 2) : 0;

    const node = {
      nodeId: fnv(`${name}.akoria.net`),
      sym, name,
      hostFqdn: `${name}.akoria.net`,
      ip: `${seg.cidr.split(".").slice(0, 3).join(".")}.${rint(2, 250)}`,
      role, segId, segName: seg.name, cidr: seg.cidr,
      osName: osPick.os, arch,
      state,
      lastSeenMin,
      enrolledDaysAgo: rint(20, 700),
      configEpoch: rint(3, 240),
      converged: chance(0.9),
      // metrics
      cpuPct: isDown ? 0 : Math.round(cores.reduce((a, b) => a + b, 0) / cores.length),
      cores,
      ramPct, ramUsedKb: Math.round(ramTotal * ramPct / 100), ramTotalKb: ramTotal,
      swapPct, swapUsedKb: Math.round(swapTotal * swapPct / 100), swapTotalKb: swapTotal,
      disks, ifaces, procs,
      diskMaxPct: disks.reduce((m, d) => Math.max(m, d.usedPct), 0),
      netTotalMbps: ifaces.reduce((a, b) => a + b.rxMbps + b.txMbps, 0),
      netCapMbps: ifaces.reduce((a, b) => a + b.capMbps, 0),
      alertsCount,
      // history sparks (60 pts)
      hist: {
        cpu: spark(60, cpuBase || 5, 14, isDown ? -0.3 : deg ? 0.25 : 0),
        ram: spark(60, ramPct || 5, 7, 0),
        net: spark(60, isDown ? 0 : 30, 22, 0),
        disk: spark(60, disks[0] ? disks[0].usedPct : 30, 1.2, 0.06),
      },
      uptimeDays: isDown ? 0 : rint(1, 410),
    };
    nodes.push(node);
    return node;
  }

  // servers (2) in core
  const primary = makeNode("server", "core", "up");
  const failover = makeNode("server", "core", "up");
  primary.label = "Primary"; failover.label = "Failover";

  // monitors (~12) spread across segments
  for (let i = 0; i < 12; i++) makeNode("monitor", pick(SEGMENTS).id);

  // clients — fill out a 100+ fleet across segments
  const segWeights = { core: 6, compute: 32, storage: 14, dmz: 12, lab: 22, iot: 16 };
  Object.entries(segWeights).forEach(([segId, count]) => {
    for (let i = 0; i < count; i++) makeNode("client", segId);
  });

  // ---- probe targets (per-vantage) ----
  const monitors = nodes.filter((n) => n.role === "monitor");
  const probeServices = [
    ["tcp", 443, "web"], ["tcp", 5432, "postgres"], ["tcp", 3306, "mariadb"],
    ["icmp", 0, "gateway"], ["udp", 53, "dns"], ["tcp", 6379, "redis"],
    ["tcp", 22, "ssh"], ["tcp", 8200, "vault"], ["tcp", 9100, "node-exp"],
  ];
  const probes = [];
  const probeHosts = nodes.filter((n) => n.role !== "monitor").slice(0, 26);
  probeHosts.forEach((h) => {
    const [proto, port, label] = pick(probeServices);
    const replFactor = 2;
    const vantages = [];
    const pickedMons = [...monitors].sort(() => rng() - 0.5).slice(0, replFactor + (chance(0.3) ? 1 : 0));
    const hostBad = h.state === "down";
    const partial = chance(0.12);
    pickedMons.forEach((m, vi) => {
      let outcome = "ok";
      if (hostBad) outcome = pick(["timeout", "unreachable", "refused"]);
      else if (partial && vi === 0) outcome = pick(["timeout", "tls_fail"]);
      else if (chance(0.06)) outcome = pick(["timeout", "proto_err"]);
      const ok = outcome === "ok";
      vantages.push({
        monitorNode: m.nodeId, monitorName: m.name,
        outcome,
        rttMicros: ok ? rint(180, proto === "icmp" ? 4000 : 28000) : 0,
        jitterMicros: ok ? rint(40, 2200) : 0,
        lossPermille: ok ? (chance(0.7) ? 0 : rint(1, 45)) : 1000,
        throughputKbps: ok ? rint(2000, 940000) : 0,
        sampledMin: rint(0, 2),
      });
    });
    const anyBad = vantages.some((v) => v.outcome !== "ok");
    const allBad = vantages.every((v) => v.outcome !== "ok");
    probes.push({
      targetId: `${proto}:${h.name}:${port || ""}`.replace(/:$/, ""),
      host: h.hostFqdn, hostNode: h.nodeId, port, proto, label, replFactor,
      vantages,
      state: allBad ? "down" : anyBad ? "degraded" : "up",
    });
  });

  // ---- alert rules ----
  const rules = [
    { ruleId: 1, name: "CPU saturation", scope: "host", metric: "cpuAvgMilli", op: "gt", threshold: 90, unit: "%", forSeconds: 120, severity: "warn", enabled: true },
    { ruleId: 2, name: "Memory pressure", scope: "host", metric: "ramUsedPct", op: "gt", threshold: 92, unit: "%", forSeconds: 180, severity: "warn", enabled: true },
    { ruleId: 3, name: "Disk almost full", scope: "host", metric: "diskUsedPct", op: "gt", threshold: 95, unit: "%", forSeconds: 0, severity: "crit", enabled: true },
    { ruleId: 4, name: "Host unreachable", scope: "host", metric: "lastSeenSec", op: "gt", threshold: 60, unit: "s", forSeconds: 0, severity: "crit", enabled: true },
    { ruleId: 5, name: "Swap thrash", scope: "host", metric: "swapUsedPct", op: "gt", threshold: 60, unit: "%", forSeconds: 300, severity: "info", enabled: true },
    { ruleId: 6, name: "Probe packet loss", scope: "probe", metric: "lossPermille", op: "gt", threshold: 50, unit: "‰", forSeconds: 60, severity: "warn", enabled: true },
    { ruleId: 7, name: "Probe latency high", scope: "probe", metric: "rttMicros", op: "gt", threshold: 25000, unit: "µs", forSeconds: 90, severity: "warn", enabled: true },
    { ruleId: 8, name: "Service down (any vantage)", scope: "probe", metric: "outcome", op: "transition", threshold: 0, unit: "", forSeconds: 0, severity: "crit", enabled: true },
    { ruleId: 9, name: "NIC error burst", scope: "host", metric: "ifErrs", op: "gt", threshold: 100, unit: "/s", forSeconds: 60, severity: "info", enabled: false },
    { ruleId: 10, name: "Watched proc missing", scope: "host", metric: "procRunState", op: "transition", threshold: 0, unit: "", forSeconds: 0, severity: "crit", enabled: true },
  ];

  // ---- alert events derived from fleet ----
  const alerts = [];
  let eid = 9000;
  // extra: { clearedMinAgo, ackedMinAgo, ackedBy, opieReportId }
  function fire(node, rule, detail, mins, cleared, extra) {
    extra = extra || {};
    const clearedMinAgo = cleared ? (extra.clearedMinAgo != null ? extra.clearedMinAgo : Math.min(mins, rint(2, 55))) : null;
    const acked = extra.ackedMinAgo != null;
    const a = {
      eventId: eid++, ruleId: rule.ruleId, ruleName: rule.name,
      node: node ? node.name : null, nodeId: node ? node.nodeId : null,
      segName: node ? node.segName : null,
      severity: rule.severity, detail, firedMinAgo: mins,
      cleared: !!cleared,
      clearedMinAgo,
      ackedAt: acked ? true : null,
      ackedBy: acked ? (extra.ackedBy || "operator") : null,
      ackedMinAgo: acked ? extra.ackedMinAgo : null,
      status: acked ? "acked" : (cleared ? "cleared" : "active"),
      opieReportId: extra.opieReportId != null ? extra.opieReportId : null,
    };
    alerts.push(a);
    return a;
  }
  nodes.forEach((n) => {
    if (n.state === "down") {
      fire(n, rules[3], `No report received for ${n.lastSeenMin}m — last seen ${n.lastSeenMin}m ago`, n.lastSeenMin, false);
      const dead = n.procs.find((p) => p.runState === "Z");
      if (dead) fire(n, rules[9], `Watched process '${dead.name}' not running (state Z)`, n.lastSeenMin - 1, false);
    } else if (n.state === "degraded") {
      if (n.cpuPct > 88) fire(n, rules[0], `CPU avg ${n.cpuPct}% sustained over threshold (90%)`, rint(2, 40), false);
      if (n.ramPct > 90) fire(n, rules[1], `RAM ${n.ramPct}% of ${(n.ramTotalKb / 1048576).toFixed(0)}GB`, rint(2, 50), false);
      if (n.diskMaxPct > 94) fire(n, rules[2], `Mount at ${n.diskMaxPct}% — ${(n.disks[0].totalGb * (100 - n.diskMaxPct) / 100).toFixed(0)}GB free`, rint(1, 20), false);
      if (n.swapPct > 60) fire(n, rules[4], `Swap ${n.swapPct}% — possible memory thrash`, rint(5, 120), false);
    }
  });
  probes.forEach((p) => {
    if (p.state === "down") fire({ name: p.host.split(".")[0], nodeId: p.hostNode, segName: "—" }, rules[7], `${p.targetId} unreachable from all ${p.vantages.length} vantages`, rint(1, 30), false);
    else if (p.state === "degraded") {
      const v = p.vantages.find((x) => x.outcome !== "ok");
      fire({ name: p.host.split(".")[0], nodeId: p.hostNode, segName: "—" }, rules[5], `${p.targetId}: ${v.outcome} from monitor '${v.monitorName}' (split vantage)`, rint(1, 25), false);
    }
  });
  // a few cleared (historical, retired >60m ago — land in History)
  for (let i = 0; i < 6; i++) {
    const n = pick(nodes.filter((x) => x.state === "up"));
    fire(n, pick([rules[0], rules[1], rules[5]]), `Auto-cleared after recovery`, rint(120, 1200), true, { clearedMinAgo: rint(70, 900) });
  }
  // a recently-cleared one (resolved <60m ago) — stays on the live board, styled resolved
  {
    const n = pick(nodes.filter((x) => x.state === "up"));
    fire(n, rules[1], `RAM pressure eased — back under tolerance`, rint(30, 55), true, { clearedMinAgo: rint(4, 40) });
  }
  // an already-acknowledged one — shows the Acked badge + owner in History
  {
    const n = pick(nodes.filter((x) => x.state === "up"));
    fire(n, rules[0], `CPU spike during nightly backup — acknowledged, expected`, rint(80, 400), false, { ackedMinAgo: rint(10, 120), ackedBy: "jason" });
  }
  alerts.sort((a, b) => {
    const sev = { crit: 0, warn: 1, info: 2 };
    if (a.cleared !== b.cleared) return a.cleared ? 1 : -1;
    if (sev[a.severity] !== sev[b.severity]) return sev[a.severity] - sev[b.severity];
    return a.firedMinAgo - b.firedMinAgo;
  });

  // ---- Opie / Analysis reports (LLM incident write-ups) ----
  // Attach a finished report to the two most severe active crit alerts, plus a
  // couple of standalone reports (one still investigating, one failed) so the
  // panel demonstrates every status. firstEventId links back to the alert.
  const opie = [];
  let orid = 4200;
  function opieFor(alert, o) {
    const reportId = orid++;
    if (alert) alert.opieReportId = reportId;
    opie.push(Object.assign({
      reportId,
      incidentKey: (alert ? (alert.node || "fleet") : "fleet") + "/" + reportId,
      triggerKind: o.triggerKind || (alert && alert.severity === "crit" ? "crit" : "warn-storm"),
      firstEventId: alert ? alert.eventId : null,
      nodeId: alert ? alert.nodeId : null,
      hostFqdn: (alert && alert.node ? alert.node + ".akoria.net" : null),
      severity: alert ? alert.severity : "warn",
      status: "done",
      summary: "",
      analysis: "",
      model: "fable-5",
      durationSec: rint(9, 48),
      startedMinAgo: rint(1, 40),
      finishedMinAgo: rint(0, 3),
    }, o));
  }
  const critActive = alerts.filter((a) => a.severity === "crit" && !a.cleared && !a.ackedAt);
  if (critActive[0]) {
    const a = critActive[0];
    opieFor(a, {
      status: "done", triggerKind: "crit", durationSec: 31,
      summary: `${a.node || "host"} stopped reporting — Opie correlated the silence to a kernel OOM-kill of the agent`,
      analysis: [
        `## Incident summary`,
        ``,
        `**${a.node || "host"}** went silent at the last report window. Opie cross-checked the`,
        `final telemetry burst, the segment's reachability matrix, and peer nodes on the same`,
        `switch before the agent dropped off.`,
        ``,
        `### What happened`,
        ``,
        `1. RAM crossed **96%** for ~4 minutes while a nightly \`rsync\` fanned out.`,
        `2. The kernel OOM-killer reaped the largest RSS process — which was \`solariAgent\`.`,
        `3. With no agent, reporting stopped; ICMP from two vantages still succeeds, so the`,
        `   **host is up, the agent is down** (not a hardware fault).`,
        ``,
        `### Evidence`,
        ``,
        `| Signal | Last value | Threshold |`,
        `| --- | --- | --- |`,
        `| RAM used | 96.4% | 90% |`,
        `| Swap used | 71% | 60% |`,
        `| Agent heartbeat | none for ${a.firedMinAgo}m | 2× interval |`,
        ``,
        `> ICMP to the host still answers from \`neon\` and \`argon\` — this is an **agent** outage,`,
        `> not a node outage.`,
        ``,
        `### Recommended action`,
        ``,
        `- \`systemctl restart solariAgent\` on the host to restore reporting.`,
        `- Add a \`MemoryHigh=\` cgroup guard to the agent unit so it is never the OOM victim.`,
        `- Consider staggering the nightly \`rsync\` fan-out to flatten the memory spike.`,
        ``,
        `_No data loss expected; historical samples resume on agent restart._`,
      ].join("\n"),
    });
  }
  if (critActive[1]) {
    const a = critActive[1];
    opieFor(a, {
      status: "done", triggerKind: "crit", durationSec: 22,
      summary: `Watched process crashed on ${a.node || "host"} — restart loop detected, root cause is a config typo`,
      analysis: [
        `## Root cause: crash-loop on a bad config key`,
        ``,
        `The watched process on **${a.node || "host"}** is in a restart loop. Opie pulled the last`,
        `three exit records and the process would come up, fail readiness, and exit within seconds.`,
        ``,
        `### Timeline`,
        ``,
        `- \`T-6m\` process exits \`code=78\` (\`EX_CONFIG\`)`,
        `- \`T-6m\` supervisor restarts it`,
        `- \`T-5m … now\` the loop repeats every ~40s`,
        ``,
        `\`\`\``,
        `fatal: unknown directive "lisen_addr" at /etc/svc/site.conf:14`,
        `\`\`\``,
        ``,
        `The directive **\`lisen_addr\`** is a typo for \`listen_addr\`. The parser rejects the file`,
        `and the service never binds.`,
        ``,
        `### Fix`,
        ``,
        `1. Correct \`lisen_addr\` → \`listen_addr\` in \`/etc/svc/site.conf\`.`,
        `2. Validate with \`svc --check-config\` before reload.`,
        `3. The alert auto-clears once the readiness probe passes.`,
      ].join("\n"),
    });
  }
  // one still investigating (no analysis yet) — a warn storm across a segment
  opie.push({
    reportId: orid++, incidentKey: "iot/warn-storm", triggerKind: "warn-storm",
    firstEventId: null, nodeId: null, hostFqdn: "seg-iot.akoria.net",
    severity: "warn", status: "investigating",
    summary: "Warn storm on the IoT segment — 11 probes flapping; Opie is correlating vantages",
    analysis: null, model: "fable-5", durationSec: null,
    startedMinAgo: 1, finishedMinAgo: null,
  });
  // one failed (model/tooling error) — surfaced so the operator knows to retry
  opie.push({
    reportId: orid++, incidentKey: "cesium/disk", triggerKind: "crit",
    firstEventId: null, nodeId: null, hostFqdn: "cesium.akoria.net",
    severity: "crit", status: "failed",
    summary: "Analysis failed — model call timed out after 60s; retry from the report",
    analysis: null, model: "fable-5", durationSec: 60,
    startedMinAgo: 18, finishedMinAgo: 17,
  });

  // ---- discovered (not-yet-monitored) ----
  const discovered = [
    { host: "tungsten.akoria.net", ip: "10.42.11.84", via: "mDNS", kind: "host", services: ["ssh:22", "http:80"], seen: 4, seg: "compute", arch: "x86_64", status: "new", mac: "b8:27:eb:41:9a:c2", vendor: "Dell Inc.", osName: "Debian 12", deviceRole: "server", mdnsName: "tungsten._workstation._tcp", mdnsServices: ["_ssh._tcp", "_workstation._tcp", "_http._tcp"], neighbor: { gearName: "sw-core-01", peerPort: "Gi1/0/12", localIf: "eth0", linkType: "wired", speedMbps: 1000, viaLldp: true } },
    { host: "10.42.50.119", ip: "10.42.50.119", via: "ARP sweep", kind: "host", services: ["mqtt:1883"], seen: 11, seg: "iot", arch: "armv7", status: "new", mac: "d8:3a:dd:0f:2b:71", vendor: "Espressif Inc.", deviceRole: "iot", mdnsName: "sensor-hub", mdnsServices: ["_mqtt._tcp", "_arduino._tcp"] },
    { host: "osmium.akoria.net", ip: "10.42.21.40", via: "SCP advert", kind: "service", services: ["minio:9000", "minio:9001"], seen: 2, seg: "storage", arch: "arm64", status: "new", mac: "dc:a6:32:8e:44:10", vendor: "Raspberry Pi Trading Ltd", osName: "Raspberry Pi OS", deviceRole: "nas", sysDescr: "MinIO object storage, RELEASE.2024-01-16" },
    { host: "10.42.30.66", ip: "10.42.30.66", via: "port scan", kind: "service", services: ["https:443"], seen: 19, seg: "dmz", arch: "x86_64", status: "new", deviceRole: "server" },
    { host: "rhenium.akoria.net", ip: "10.42.12.7", via: "mDNS", kind: "host", services: ["ssh:22", "postgres:5432", "node-exp:9100"], seen: 1, seg: "compute", arch: "arm64", status: "new", mac: "dc:a6:32:11:aa:5f", vendor: "Raspberry Pi Trading Ltd", osName: "Ubuntu 24.04", deviceRole: "server", mdnsName: "rhenium", mdnsServices: ["_ssh._tcp", "_postgresql._tcp"], neighbor: { gearName: "sw-edge-02", peerPort: "Gi0/5", localIf: "eth0", linkType: "wired", speedMbps: 1000, viaLldp: true } },
    { host: "10.42.40.158", ip: "10.42.40.158", via: "ARP sweep", kind: "host", services: ["ssh:22"], seen: 33, seg: "lab", arch: "armv7", status: "new" },
    { host: "iridium.akoria.net", ip: "10.42.20.91", via: "SCP advert", kind: "service", services: ["nfs:2049", "smb:445"], seen: 6, seg: "storage", arch: "x86_64", status: "new", mac: "00:11:32:5c:9d:e8", vendor: "Synology Incorporated", osName: "DSM 7.2", deviceRole: "nas", sysDescr: "Synology DS1520+ NAS", mdnsName: "iridium", mdnsServices: ["_smb._tcp", "_nfs._tcp", "_afpovertcp._tcp", "_device-info._tcp"] },
    { host: "10.42.50.204", ip: "10.42.50.204", via: "port scan", kind: "host", services: ["coap:5683"], seen: 47, seg: "iot", arch: "armv7", status: "new", mac: "b0:4e:26:77:31:a2", vendor: "TP-Link Technologies", deviceRole: "iot", mdnsName: "livingroom-bulb", mdnsServices: ["_spotify-connect._tcp", "_airplay._tcp", "_hap._tcp"] },
  ];

  // ---- binary builds / deploy convergence ----
  const archCount = {};
  nodes.forEach((n) => { archCount[n.arch] = (archCount[n.arch] || 0) + 1; });
  const builds = [
    { arch: "x86_64", os: "Linux", version: "1.0.3", channel: "stable", nodes: archCount["x86_64"] || 0, status: "current" },
    { arch: "arm64", os: "Linux", version: "1.0.3", channel: "stable", nodes: archCount["arm64"] || 0, status: "current" },
    { arch: "armv7", os: "Linux", version: "1.0.2", channel: "stable", nodes: archCount["armv7"] || 0, status: "update" },
    { arch: "x86_64", os: "Windows", version: "1.0.3", channel: "stable", nodes: archCount["x86_64"] ? 3 : 0, status: "current" },
    { arch: "arm64", os: "macOS", version: "1.0.1", channel: "beta", nodes: 2, status: "update" },
  ];

  // ---- pending enrollments (CSRs awaiting operator approval) ----
  const enrollments = [
    { host: "rhenium.akoria.net", ip: "10.42.12.7", role: "client", fp: "9F:2C:A1:88:E4:0B", requestedMin: 3, status: "pending" },
    { host: "iridium.akoria.net", ip: "10.42.20.91", role: "monitor", fp: "47:DE:90:1A:3C:F5", requestedMin: 12, status: "pending" },
    { host: "tungsten.akoria.net", ip: "10.42.11.84", role: "client", fp: "B0:11:7E:62:9A:D3", requestedMin: 26, status: "token" },
  ];

  // ---- global configuration defaults ----
  const config = {
    schedule: { sampleIntervalSec: 15, watchdogIntervalSec: 5 },
    probe: { roundIntervalSec: 30, probesPerRound: 5, replFactor: 2, gossipIntervalSec: 20 },
    retention: { historyDays: 90, partitionByMonth: true },
    lease: { renewSec: 5, ttlSec: 15 },
    ingest: { ports: { ingest: 7701, survey: 7702, pub: 7703 }, tls: "TLS 1.3 · mbedTLS" },
    ca: { issuer: "akoria internal CA (step-ca)", enroll: "token + CSR", certTtlDays: 365 },
    autoDiscover: true, autoEnroll: false,
  };

  // ---- network gear & LLDP-derived uplinks (for LAN-hierarchy topology) ----
  const NETGEAR = [
    { id: "gw", name: "gw-core", kind: "gateway", model: "pfSense+ · 10G", seg: "core", ports: 8, uplink: null },
    { id: "sw-core", name: "sw-core-01", kind: "switch", model: "Arista 7050X · 48p", seg: "core", ports: 48, uplink: "gw" },
    { id: "sw-compute", name: "sw-comp-01", kind: "switch", model: "Arista 7050X · 48p", seg: "compute", ports: 48, uplink: "sw-core" },
    { id: "sw-storage", name: "sw-stor-01", kind: "switch", model: "MikroTik CRS · 24p", seg: "storage", ports: 24, uplink: "sw-core" },
    { id: "sw-dmz", name: "sw-dmz-01", kind: "switch", model: "MikroTik CRS · 16p", seg: "dmz", ports: 16, uplink: "gw" },
    { id: "ap-lab", name: "ap-lab-01", kind: "ap", model: "UniFi U6-Pro · wifi6", seg: "lab", ports: 0, uplink: "sw-core", wireless: true },
    { id: "ap-iot", name: "ap-iot-01", kind: "ap", model: "UniFi U6-LR · wifi6", seg: "iot", ports: 0, uplink: "sw-core", wireless: true },
  ];
  const SEG_GEAR = { core: "sw-core", compute: "sw-compute", storage: "sw-storage", dmz: "sw-dmz", lab: "ap-lab", iot: "ap-iot" };
  const WIRELESS_SEGS = { lab: true, iot: true };
  const portCtr = {};
  nodes.forEach((n) => {
    const gid = SEG_GEAR[n.segId] || "sw-core";
    n.uplink = gid;
    n.linkType = WIRELESS_SEGS[n.segId] ? "wireless" : "wired";
    n.linkSpeedMbps = n.ifaces[0] ? n.ifaces[0].capMbps : 1000;
    if (n.linkType === "wired") { portCtr[gid] = (portCtr[gid] || 0) + 1; n.uplinkPort = "Gi1/0/" + portCtr[gid]; n.lldp = true; }
    else { n.uplinkPort = "wlan0"; n.rssi = -(42 + rint(0, 44)); n.lldp = false; }
  });
  NETGEAR.forEach((g) => { g.attached = nodes.filter((n) => n.uplink === g.id).length; });


  // ---- rollups ----
  function rollup(list) {
    const r = { total: list.length, up: 0, degraded: 0, down: 0, unknown: 0 };
    list.forEach((n) => r[n.state]++);
    return r;
  }
  const fleetRoll = rollup(nodes);
  const segRollups = {};
  SEGMENTS.forEach((s) => { segRollups[s.id] = rollup(nodes.filter((n) => n.segId === s.id)); });

  // ---- monitoring-wide summary (controller status) ----
  const appInstances = nodes.reduce((a, n) => a + n.procs.length, 0);
  const summary = {
    systems: nodes.length,                 // enrolled hosts (clients + monitors + servers)
    hosts: nodes.filter((n) => n.role === "client").length,
    servers: nodes.filter((n) => n.role === "server").length,
    monitors: nodes.filter((n) => n.role === "monitor").length,
    applications: appInstances,            // watched service/process instances across the fleet
    probes: probes.length,                 // network service endpoints under probe
    version: "1.0.3",
    uptimeStr: "134d 06h",                 // controller (active server) uptime
  };

  window.SOLARI = {
    nodes, segments: SEGMENTS, segRollups, fleetRoll, summary,
    probes, rules, alerts, opie, discovered, builds, enrollments, config, netgear: NETGEAR,
    monitors,
    server: {
      primary: primary.name, primaryId: primary.nodeId,
      failover: failover.name, failoverId: failover.nodeId,
      leaseEpoch: 4471, leaseHealthy: true, leaseTtlSec: 15, leaseAgeSec: 2,
      dbHost: "127.0.0.1", historyDays: 90,
    },
    activeCrit: alerts.filter((a) => !a.ackedAt && !a.cleared && a.severity === "crit").length,
    activeWarn: alerts.filter((a) => !a.ackedAt && !a.cleared && a.severity === "warn").length,
    fmt: {
      kb(kb) { if (kb >= 1048576) return (kb / 1048576).toFixed(1) + " GB"; if (kb >= 1024) return (kb / 1024).toFixed(0) + " MB"; return kb + " KB"; },
      mbps(m) { if (m >= 1000) return (m / 1000).toFixed(1) + " Gb/s"; return m + " Mb/s"; },
      ago(min) { if (min < 1) return "just now"; if (min < 60) return min + "m ago"; if (min < 1440) return Math.floor(min / 60) + "h ago"; return Math.floor(min / 1440) + "d ago"; },
      rtt(us) { if (!us) return "—"; if (us >= 1000) return (us / 1000).toFixed(1) + " ms"; return us + " µs"; },
    },
  };
})();
