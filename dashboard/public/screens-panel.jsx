/*
 * screens-panel.jsx — Panel control page (window.PanelScreen).
 *
 * CONTRACT-CP.md v1.1 §7 (dashboard page) + §10 (dispositions, which win).
 * Renders all 4 themes x 3 screens of the physical Galactic Unicorn status
 * panel simultaneously as 53x11 virtual LED matrices, plus the §7 software
 * control set (theme / screen / brightness+AUTO / ACK / dwell / sleep-wake).
 *
 * SOURCES, and the rule for reconciling them (§10: "every ported renderer
 * cites its mockup function AND its firmware file:line"):
 *   - status-panel/project/"Galactic Unicorn Themes.dc.html" — the canonical
 *     mockup. Function citations below are line numbers in that file.
 *   - status-panel/firmware/panelScreens{A,B,C,D}.c, panelFont.c, panelFb.c,
 *     panelHist.c, panelInlay.c, main.c — the firmware ports. Where the
 *     firmware DEVIATES from the mockup (hardware passes, real telemetry
 *     substitutions, layout fixes) the FIRMWARE is reproduced here, because
 *     this page exists to show what the panel is actually painting.
 *
 * SPA idiom: Babel-in-browser, no build step, no imports, window globals only
 * (matches screens.jsx / screens6.jsx). Everything below lives in one IIFE and
 * only PanelScreen escapes onto window.
 *
 * PARITY MODE (§10): ?fixture=1 renders status-panel/fixtures/panel-snapshot
 * .json deterministically with no API traffic and no animation clock, so two
 * reviewers looking at the same build see identical pixels.
 */
(function () {
  const { useState, useEffect, useRef, useMemo, useCallback } = React;
  const Icon = window.Icon;

  // =====================================================================
  // 1. Panel geometry and palette
  //    mockup: `const W = 53, H = 11` + the C{} palette (Themes.dc.html top
  //    of the simulator block) · firmware: panelHw.h PANEL_W/PANEL_H,
  //    panelFb.c:11-18.
  //    NOTE (governance): the Interface Guide Rev 2 palette governs the page
  //    chrome only. These eight colours reproduce the physical panel and are
  //    explicitly exempt (CONTRACT-CP §7).
  // =====================================================================
  const PW = 53;
  const PH = 11;
  const HIST_LEN = PW;                 // panelHist.h:26
  const LOAD_BUCKETS = 35;             // panelHist.h:27
  const LATTICE_SLOTS = PW * PH;       // panelHist.h:28  (583)
  const MAX_POOLS = 8;                 // protocol.h PANEL_MAX_POOLS
  const MAX_SYSTEMS = 64;              // protocol.h PANEL_MAX_SYSTEMS

  const cOk      = [61, 214, 140];     // panelFb.c:11
  const cWarn    = [245, 166, 35];     // panelFb.c:12
  const cCrit    = [255, 77, 94];      // panelFb.c:13
  const cMaint   = [155, 135, 255];    // panelFb.c:14
  const cAzure   = [34, 184, 240];     // panelFb.c:15
  const cQuiet   = [78, 107, 126];     // panelFb.c:16
  const cInk     = [223, 232, 242];    // panelFb.c:17
  const cUnknown = [124, 138, 160];    // panelFb.c:18 — the mockup has no
                                       // UNKNOWN colour; DESIGN-BRIEF
                                       // Ambiguity #2 reserves #7C8AA0.

  // protocol.h PanelState enum
  const ST_OK = 0, ST_DEGRADED = 1, ST_DOWN = 2, ST_UNKNOWN = 3, ST_MAINT = 4;

  /* Network gear — CONTRACT-AW §3.1, the v2 SNAPSHOT gear section. Two enums
     arrive on the wire and they are NOT the two above: role is its own
     namespace, and gear STATE is 0=down 1=up 2=degraded, which is neither the
     PanelState order nor its membership. stateEnum() must never be pointed at
     a gear row — hence the separate gearState() below. */
  const GR_ROUTER = 0, GR_SWITCH = 1, GR_HUB = 2, GR_AP = 3, GR_WANBK = 4;
  const GS_DOWN = 0, GS_UP = 1, GS_DEGRADED = 2;
  const MAX_GEAR = 12;                 // §3.1 gearCount 0..12
  const MAX_LEVEL = 7;                 // §3.1 rx/txLevel 0..7

  const PANEL_SEP = "   ";             // panelHist.h:33
  const PANEL_TAIL = "       ";        // panelHist.h:34

  // =====================================================================
  // 2. Framebuffer — panelFb.c
  //    Float RGB, clamped only at flush, exactly as the firmware. FB is a
  //    module-level current-target so the renderers below read as a
  //    line-for-line port of the C rather than a re-architecture of it.
  // =====================================================================
  let FB = null;
  function fbUse(buf) { FB = buf; }
  function fbNew() { return new Float32Array(PW * PH * 3); }

  function fbClear() { FB.fill(0); }                                  // panelFb.c:20
  function fbSet(x, y, c, b) {                                        // panelFb.c:24
    if (x < 0 || x >= PW || y < 0 || y >= PH) return;
    const i = (y * PW + x) * 3;
    FB[i] = c[0] * b; FB[i + 1] = c[1] * b; FB[i + 2] = c[2] * b;
  }
  function fbSetRgb(x, y, r, g, bl, b) {                              // panelFb.c:30
    if (x < 0 || x >= PW || y < 0 || y >= PH) return;
    const i = (y * PW + x) * 3;
    FB[i] = r * b; FB[i + 1] = g * b; FB[i + 2] = bl * b;
  }
  function fbAdd(x, y, c, b) {                                        // panelFb.c:36
    if (x < 0 || x >= PW || y < 0 || y >= PH) return;
    const i = (y * PW + x) * 3;
    FB[i] += c[0] * b; FB[i + 1] += c[1] * b; FB[i + 2] += c[2] * b;
  }
  function fbDim(x, y, k) {                                           // panelFb.c:42
    if (x < 0 || x >= PW || y < 0 || y >= PH) return;
    const i = (y * PW + x) * 3;
    FB[i] *= k; FB[i + 1] *= k; FB[i + 2] *= k;
  }
  function fbDimAll(k) { for (let i = 0; i < FB.length; i++) FB[i] *= k; } // panelFb.c:48

  // =====================================================================
  // 3. Fonts — panelFont.c
  //    The packed tables are lifted verbatim from panelFont.c:17-39 (which
  //    were themselves generated mechanically from the mockup's F and BIG
  //    string tables). Small: 5 rows x 3 bits in a u16, row 0 in the high
  //    bits. Big: 7 rows x 4 bits in a u32.
  // =====================================================================
  const FONT_SMALL = {
    "0": 0x7B6F, "1": 0x2C97, "2": 0x73E7, "3": 0x73CF, "4": 0x5BC9,
    "5": 0x79CF, "6": 0x79EF, "7": 0x7292, "8": 0x7BEF, "9": 0x7BCF,
    "A": 0x7BED, "B": 0x6BAE, "C": 0x7927, "D": 0x6B6E, "E": 0x79E7,
    "F": 0x79E4, "G": 0x796F, "H": 0x5BED, "I": 0x7497, "J": 0x126F,
    "K": 0x5BAD, "L": 0x4927, "M": 0x5FED, "N": 0x6B6D, "O": 0x7B6F,
    "P": 0x7BE4, "Q": 0x7B79, "R": 0x7BF5, "S": 0x79CF, "T": 0x7492,
    "U": 0x5B6F, "V": 0x5B6A, "W": 0x5BFD, "X": 0x5AAD, "Y": 0x5A92,
    "Z": 0x72A7, "/": 0x12A4, ":": 0x0410, ".": 0x0002, "-": 0x01C0,
    "%": 0x52A5, "+": 0x05D0, ">": 0x4454, "!": 0x2482, " ": 0x0000,
  };
  const FONT_BIG = {
    "0": 0x6999996, "1": 0x2622227, "2": 0x691248F, "3": 0xE11611E,
    "4": 0x26AAF22, "5": 0xF88E196, "6": 0x688E996, "7": 0xF122444,
    "8": 0x6996996, "9": 0x6997116, ".": 0x0000006, "%": 0x9922499,
    "G": 0x788B996, "M": 0x9FF9999, "S": 0x788611E, " ": 0x0000000,
  };
  function glyphSmall(ch) { const g = FONT_SMALL[ch.toUpperCase()]; return g === undefined ? 0 : g; }
  function glyphBig(ch) { const g = FONT_BIG[ch.toUpperCase()]; return g === undefined ? 0 : g; }

  function textW(s) { return String(s).length * 4 - 1; }              // panelFont.c:61
  function bigW(s) { return String(s).length * 5 - 1; }               // panelFont.c:62

  /* panelTextInk — panelFont.c:112-116. The 2026-08-04 reflection-floor pass:
     small-font text is mapped into the narrow high band 0.85 + 0.15*b so it
     clears the unlit packages' cream reflection. Applied to small text ONLY —
     never panelBig, never bars/ramps/particles, and it never moves a hue. */
  function textInk(b) {
    if (b <= 0) return 0;
    if (b >= 1) return 1;
    return 0.85 + 0.15 * b;
  }
  function drawSmall(cx, y, ch, c, b) {                               // panelFont.c:121
    const g = glyphSmall(ch);
    const ink = textInk(b);
    for (let ry = 0; ry < 5; ry++)
      for (let rx = 0; rx < 3; rx++)
        if (g & (1 << (14 - (ry * 3 + rx)))) fbSet(cx + rx, y + ry, c, ink);
  }
  function panelText(x, y, s, c, b) {                                 // panelFont.c:129
    s = String(s); let cx = x;
    for (let i = 0; i < s.length; i++) { drawSmall(cx, y, s.charAt(i), c, b); cx += 4; }
    return cx;
  }
  function panelTextOver(x, y, s, c, b) {                             // panelFont.c:135
    s = String(s); let cx = x;
    for (let i = 0; i < s.length; i++) {
      const g = glyphSmall(s.charAt(i));
      for (let ry = 0; ry < 5; ry++) {
        for (let rx = 0; rx < 3; rx++) {
          if (!(g & (1 << (14 - (ry * 3 + rx))))) continue;
          for (let dy = -1; dy <= 1; dy++)
            for (let dx = -1; dx <= 1; dx++) fbDim(cx + rx + dx, y + ry + dy, 0.12);
        }
      }
      cx += 4;
    }
    return panelText(x, y, s, c, b);
  }
  function panelBig(x, y, s, c, b) {                                  // panelFont.c:152
    s = String(s); let cx = x;
    for (let i = 0; i < s.length; i++) {
      const g = glyphBig(s.charAt(i));
      for (let ry = 0; ry < 7; ry++)
        for (let rx = 0; rx < 4; rx++)
          if (g & (1 << (27 - (ry * 4 + rx)))) fbSet(cx + rx, y + ry, c, b);
      cx += 5;
    }
    return cx;
  }
  function panelScroll(t, y, s, c, b, speed) {                        // panelFont.c:164
    s = String(s);
    const total = textW(s) + PW;
    if (total <= 0) return;
    let off = (t * speed) % total;
    if (off < 0) off += total;
    off = Math.floor(off);
    let cx = PW - off;
    for (let i = 0; i < s.length; i++) {
      if (cx > -4 && cx < PW) drawSmall(cx, y, s.charAt(i), c, b);
      cx += 4;
    }
  }

  // =====================================================================
  // 4. Small numeric helpers that mirror C semantics exactly
  // =====================================================================
  function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
  function ri(v) { return Math.floor(v + 0.5); }        // (int)(v + 0.5f), v >= 0
  function trunc(v) { return v < 0 ? Math.ceil(v) : Math.floor(v); } // (int) cast
  function num(v) { const n = Number(v); return isFinite(n) ? n : 0; }
  function f1(v) { return v.toFixed(1); }               // "%.1f"

  /* PanelRng — the mockup's rng(seed) LCG (Themes.dc.html) and panelScreensA.c
     :12-16. Seeded per screen so the animated screens are reproducible. */
  function rngNew(seed) { return { s: seed >>> 0 }; }
  function rngNext(r) {
    r.s = (Math.imul(r.s, 1664525) + 1013904223) >>> 0;
    return r.s / 4294967296;
  }

  /* stateRank — panelHist.c:19-27. UNKNOWN sits between degraded and maint. */
  function stateRank(st) {
    if (st === ST_DOWN) return 4;
    if (st === ST_DEGRADED) return 3;
    if (st === ST_UNKNOWN) return 2;
    if (st === ST_MAINT) return 1;
    return 0;
  }
  const STATE_ENUM = { ok: ST_OK, up: ST_OK, degraded: ST_DEGRADED, down: ST_DOWN, unknown: ST_UNKNOWN, maint: ST_MAINT };
  function stateEnum(s) {
    if (typeof s === "number") return (s >= 0 && s <= 4) ? s : ST_UNKNOWN;
    const v = STATE_ENUM[String(s || "").toLowerCase()];
    return v === undefined ? ST_UNKNOWN : v;
  }

  /* gearRole / gearState / gearLevel — CONTRACT-AW §3.1 arrives as plain u8s,
     so the numeric path is the real one. The string branches exist because the
     server maps these from `networkGear.kind` text (§2) and a mapper that
     forgets to numify would otherwise render every device as a router at
     index 0 — silently, and plausibly. Anything unrecognised is REJECTED
     (null / down / 0), never coerced to a flattering default. */
  const GEAR_ROLE_ENUM = {
    router: GR_ROUTER, gateway: GR_ROUTER, udm: GR_ROUTER,
    switch: GR_SWITCH, usw: GR_SWITCH,
    hub: GR_HUB,
    ap: GR_AP, accesspoint: GR_AP,
    wanbackup: GR_WANBK, backup: GR_WANBK, wan: GR_WANBK,
  };
  const GEAR_STATE_ENUM = { down: GS_DOWN, up: GS_UP, ok: GS_UP, degraded: GS_DEGRADED };
  function gearRole(v) {
    if (typeof v === "number") return (v >= 0 && v <= GR_WANBK) ? v : null;
    const r = GEAR_ROLE_ENUM[String(v || "").toLowerCase().replace(/[^a-z]/g, "")];
    return r === undefined ? null : r;
  }
  function gearState(v) {
    if (typeof v === "number") return (v >= 0 && v <= GS_DEGRADED) ? v : GS_DOWN;
    const s = GEAR_STATE_ENUM[String(v || "").toLowerCase()];
    return s === undefined ? GS_DOWN : s;
  }
  function gearLevel(v) {
    let n = Math.round(num(v));
    if (!(n > 0)) return 0;
    return n > MAX_LEVEL ? MAX_LEVEL : n;
  }

  /* panelFormatRate — panelHist.c:65-74. */
  function formatRate(kbps) {
    const gbps = kbps / 1000000;
    if (gbps >= 10) return ri(gbps) + "G";
    if (gbps >= 1) return f1(gbps) + "G";
    const mbps = kbps / 1000;
    if (mbps >= 10) return ri(mbps) + "M";
    return f1(mbps) + "M";
  }

  // =====================================================================
  // 5. buildEnv — the page's panelEnvApply (panelHist.c:124-273)
  //
  //    The firmware accumulates its history rings one sample per snapshot
  //    arrival. The page accumulates the same rings from the polls it has
  //    seen, but CONTRACT-CP §10 requires the resample be done on the SERVER
  //    timestamp and be gap-aware — a browser tab that was backgrounded for
  //    four minutes must not paint four minutes of missing history as if the
  //    fleet had been flat, and must not compress it either.
  //
  //    Slot i covers tNewest - (52 - i) * cadence. Each slot takes the newest
  //    sample at or before it (within half a cadence of tolerance); a slot
  //    whose backing sample is more than 2 cadences old is flagged a GAP and
  //    holds the previous value, which is what the ring physically does. Slots
  //    before the first sample are unfilled, and histAt() clamps reads to the
  //    first real slot exactly as panelHistAt (panelHist.c:50-58) does.
  // =====================================================================
  const CADENCE_MS_DEFAULT = 5000;      // §7 poll cadence
  const PEAK_FLOOR_KBPS = 1000;         // panelHist.c:37
  const PEAK_DECAY = 0.997;             // panelHist.c:38

  function sampleMs(d) {
    const ts = num(d.ts);
    return ts > 1e11 ? ts : ts * 1000;   // tolerate ms or s epochs
  }

  function resampleRings(samples, cadenceMs) {
    const N = HIST_LEN;
    const thru = new Float32Array(N), egress = new Float32Array(N);
    const cpuHist = new Float32Array(N), rttHist = new Float32Array(N);
    const newest = sampleMs(samples[samples.length - 1]);
    const half = cadenceMs / 2;
    const pick = new Array(N);
    const gapAt = new Array(N);
    let j = 0;
    for (let i = 0; i < N; i++) {
      const slotT = newest - (N - 1 - i) * cadenceMs;
      while (j + 1 < samples.length && sampleMs(samples[j + 1]) <= slotT + half) j++;
      const s = samples[j];
      if (sampleMs(s) > slotT + half) { pick[i] = null; gapAt[i] = false; continue; }
      pick[i] = s;
      gapAt[i] = (slotT - sampleMs(s)) > 2 * cadenceMs;
    }
    let firstReal = 0;
    while (firstReal < N && !pick[firstReal]) firstReal++;
    if (firstReal >= N) firstReal = N - 1;

    // Adaptive peak, replayed chronologically so old slots normalise against
    // the then-current peak exactly as the incremental firmware does.
    let peak = PEAK_FLOOR_KBPS;
    let gaps = 0;
    for (let i = firstReal; i < N; i++) {
      const s = pick[i] || samples[samples.length - 1];
      if (gapAt[i]) gaps++;
      const rx = num(s.rxKbps), tx = num(s.txKbps);
      peak *= PEAK_DECAY;
      if (rx + tx > peak) peak = rx + tx;
      if (peak < PEAK_FLOOR_KBPS) peak = PEAK_FLOOR_KBPS;
      thru[i] = clamp01(rx / peak);
      egress[i] = clamp01(tx / peak);
      cpuHist[i] = clamp01(num(s.meanLoadPct) / 100);
      rttHist[i] = clamp01(num(s.rttTenthMs) / 10 / 100);   // panelHist.c:161
    }
    for (let i = 0; i < firstReal; i++) {
      thru[i] = thru[firstReal]; egress[i] = egress[firstReal];
      cpuHist[i] = cpuHist[firstReal]; rttHist[i] = rttHist[firstReal];
    }
    return { thru: thru, egress: egress, cpuHist: cpuHist, rttHist: rttHist,
             histFill: N - firstReal, gaps: gaps, peakKbps: peak };
  }

  const EMPTY_POOL = { name: "", tier: 3, ok: 0, degraded: 0, down: 0, unknown: 0, maint: 0, total: 1, load: 0 };

  function buildEnv(samples, cadenceMs) {
    const d = samples[samples.length - 1];
    const env = {};
    const roll = d.stateRoll || {};

    env.haveData = true;
    env.alarmActive = !!d.alarmActive;
    env.dataStale = !!d.dataStale;
    env.score = num(d.score);
    env.episodeId = num(d.episodeId);

    // panelHist.c:133-138 — stateRoll counts AGENT NODES (CONTRACT §9b).
    env.up = num(roll.up != null ? roll.up : roll.ok);
    env.deg = num(roll.degraded);
    env.down = num(roll.down);
    env.unknown = num(roll.unknown);
    env.maint = num(roll.maint);
    env.total = env.up + env.deg + env.down + env.unknown + env.maint;

    env.meanLoad = clamp01(num(d.meanLoadPct) / 100);
    env.rtt = num(d.rttTenthMs) / 10;
    env.loss = num(d.lossPermille) / 10;

    const rxK = num(d.rxKbps), txK = num(d.txKbps);
    env.rxGbps = rxK / 1000000;
    env.txGbps = txK / 1000000;
    env.gbps = (rxK + txK) / 1000000;
    env.gLabel = formatRate(rxK + txK);                    // panelHist.c:153

    const rings = resampleRings(samples, cadenceMs);
    env.thru = rings.thru; env.egress = rings.egress;
    env.cpuHist = rings.cpuHist; env.rttHist = rings.rttHist;
    env.histFill = rings.histFill;
    env.gaps = rings.gaps;
    env.peakKbps = rings.peakKbps;

    // panelHist.c:164-193 — pools in wire order, plus the worst-first index.
    const rawPools = Array.isArray(d.pools) ? d.pools.slice(0, MAX_POOLS) : [];
    env.pools = rawPools.map(function (p) {
      return {
        name: String(p.name || ""), tier: num(p.tier),
        ok: num(p.up != null ? p.up : p.ok), degraded: num(p.degraded),
        down: num(p.down), unknown: num(p.unknown), maint: num(p.maint),
        total: num(p.total) || 1, load: clamp01(num(p.loadPct) / 100),
      };
    });
    env.poolCount = env.pools.length;
    env.poolWorstFirst = env.pools.map(function (_, i) { return i; });
    // (down*9 + degraded) desc, then load desc — panelHist.c:179-193.
    for (let i = 1; i < env.poolCount; i++) {
      const key = env.poolWorstFirst[i];
      let j2 = i - 1;
      while (j2 >= 0) {
        const a = env.pools[env.poolWorstFirst[j2]], b = env.pools[key];
        const wa = a.down * 9 + a.degraded, wb = b.down * 9 + b.degraded;
        const worse = (wb !== wa) ? (wb > wa) : (b.load > a.load);
        if (!worse) break;
        env.poolWorstFirst[j2 + 1] = env.poolWorstFirst[j2];
        j2--;
      }
      env.poolWorstFirst[j2 + 1] = key;
    }

    // systems — capped at 64, already ordered (tier, name) by the API.
    const rawSys = Array.isArray(d.systems) ? d.systems.slice(0, MAX_SYSTEMS) : [];
    env.systems = rawSys.map(function (s) {
      return { name: String(s.name || ""), pool: num(s.pool), tier: num(s.tier),
               state: stateEnum(s.state), loadPct: num(s.loadPct) };
    });
    const sysN = env.systems.length;

    // C0 load histogram — panelHist.c:198-215. down/unknown excluded, then
    // normalised by the tallest bucket.
    const hist = new Array(LOAD_BUCKETS).fill(0);
    for (let i = 0; i < sysN; i++) {
      const st = env.systems[i].state;
      if (st === ST_DOWN || st === ST_UNKNOWN) continue;
      let b = trunc((env.systems[i].loadPct / 100) * LOAD_BUCKETS);
      if (b < 0) b = 0;
      if (b >= LOAD_BUCKETS) b = LOAD_BUCKETS - 1;
      hist[b]++;
    }
    let hMax = 1;
    for (let b = 0; b < LOAD_BUCKETS; b++) if (hist[b] > hMax) hMax = hist[b];
    env.loadHist = hist.map(function (v) { return v / hMax; });

    // A0 lattice — panelHist.c:217-245. Systems bucketed evenly across the
    // 583 slots so the picture holds at any fleet size.
    const order = env.systems.map(function (_, i) { return i; });
    sortLatticeOrder(env.systems, order);
    env.latticeOwner = new Uint8Array(LATTICE_SLOTS).fill(0xFF);
    env.lattice = new Array(LATTICE_SLOTS);
    let per = Math.ceil(sysN / LATTICE_SLOTS);
    if (per < 1) per = 1;
    for (let slot = 0; slot < LATTICE_SLOTS; slot++) {
      const a = slot * per;
      if (a >= sysN) break;
      let worst = ST_OK, tier = 3, n = 0, load = 0;
      for (let k = a; k < sysN && k < a + per; k++) {
        const s = env.systems[order[k]];
        if (stateRank(s.state) > stateRank(worst)) { worst = s.state; tier = s.tier; }
        if (s.tier < tier && worst === ST_OK) tier = s.tier;
        load += s.loadPct / 100;
        n++;
      }
      env.latticeOwner[slot] = 1;
      env.lattice[slot] = { state: worst, tier: tier > 3 ? 3 : tier, load: n ? load / n : 0 };
    }

    // D0 waterfall rows — panelHist.c:247-259, the 11 worst-first systems.
    const worstOrder = env.systems.map(function (_, i) { return i; });
    sortWorstFirst(env.systems, worstOrder);
    env.probeRows = sysN < PH ? sysN : PH;
    env.probeRow = new Array(PH).fill(0xFF);
    for (let i = 0; i < env.probeRows; i++) env.probeRow[i] = worstOrder[i];

    // Alert text — panelHist.c:261-272.
    const ta = d.topAlert;
    env.hasAlert = !!ta;
    env.subject = ta ? String(ta.subject || "") : "ALL NOMINAL";
    env.detail = ta ? String(ta.detail || "") : "NO FINDINGS";
    env.ticker = env.subject + PANEL_SEP + env.detail + PANEL_TAIL;

    /* Gear — CONTRACT-AW §3.1. A row whose role does not decode is DROPPED
       rather than defaulted, because a mis-roled device does not just look
       wrong, it moves gates into the wrong band and changes what the picture
       claims about the network. Order is preserved exactly as the server sent
       it (§3.1: router, switch(es), hubs, aps, wanBackup; stable by gearId
       within a role) — the layout re-groups by role but keeps wire order
       inside each band, so gate identity is stable poll to poll. */
    const rawGear = Array.isArray(d.gear) ? d.gear.slice(0, MAX_GEAR) : [];
    env.gear = [];
    for (let i = 0; i < rawGear.length; i++) {
      const g = rawGear[i] || {};
      const role = gearRole(g.role);
      if (role === null) continue;
      env.gear.push({
        role: role,
        state: gearState(g.state),
        rx: gearLevel(g.rxLevel),
        tx: gearLevel(g.txLevel),
      });
    }
    env.gearCount = env.gear.length;

    env.poolLane = function (wanted) {                   // panelScreensA.c:20-25
      if (env.poolCount <= 0) return EMPTY_POOL;
      if (wanted >= env.poolCount) wanted = env.poolCount - 1;
      if (wanted < 0) wanted = 0;
      return env.pools[wanted];
    };
    return env;
  }

  /* sortWorstFirst — panelHist.c:86-103: rank desc, tier asc, load desc.
     Insertion sort, kept because the comparator is not a total order on ties
     and the firmware's stability is observable in the D0 row identities. */
  function sortWorstFirst(sys, idx) {
    for (let i = 1; i < idx.length; i++) {
      const key = idx[i];
      let j = i - 1;
      while (j >= 0) {
        const a = sys[idx[j]], b = sys[key];
        const ra = stateRank(a.state), rb = stateRank(b.state);
        const worse = (rb !== ra) ? (rb > ra)
          : (b.tier !== a.tier) ? (b.tier < a.tier)
            : (b.loadPct > a.loadPct);
        if (!worse) break;
        idx[j + 1] = idx[j];
        j--;
      }
      idx[j + 1] = key;
    }
  }
  /* sortLatticeOrder — panelHist.c:107-122: tier asc, then rank desc. */
  function sortLatticeOrder(sys, idx) {
    for (let i = 1; i < idx.length; i++) {
      const key = idx[i];
      let j = i - 1;
      while (j >= 0) {
        const a = sys[idx[j]], b = sys[key];
        const before = (b.tier !== a.tier) ? (b.tier < a.tier)
          : (stateRank(b.state) > stateRank(a.state));
        if (!before) break;
        idx[j + 1] = idx[j];
        j--;
      }
      idx[j + 1] = key;
    }
  }

  /* histAt — panelHist.c:50-58. */
  function histAt(env, ring, i) {
    if (i < 0) i = 0;
    if (i >= HIST_LEN) i = HIST_LEN - 1;
    let firstReal = HIST_LEN - env.histFill;
    if (firstReal < 0) firstReal = 0;
    if (i < firstReal) i = firstReal;
    if (i >= HIST_LEN) i = HIST_LEN - 1;
    return ring[i];
  }

  // =====================================================================
  // 6. THEME A · ABSTRACT — panelScreensA.c
  // =====================================================================

  /* A0 · State lattice.
     mockup: sA0() Themes.dc.html:408 · firmware: panelScreensA.c:36-75.
     One cell per system bucket over the full 53x11 grid; heat by tier;
     tier-0 downs pulse at 3 rad/s. The UNKNOWN branch (A.c:59-67) is a
     firmware DEVIATION — the mockup never emits UNKNOWN. */
  const HEAT_BY_TIER = [1.0, 0.62, 0.4, 0.26];                    // A.c:37
  function sA0(env, t) {
    for (let i = 0; i < LATTICE_SLOTS; i++) {
      if (env.latticeOwner[i] === 0xFF) continue;
      const s = env.lattice[i];
      const x = i % PW, y = Math.floor(i / PW);
      const heat = HEAT_BY_TIER[s.tier & 3];
      const phase = (i * 0.41) % 6.283;
      if (s.state === ST_OK) {
        fbSet(x, y, cQuiet, (0.1 + s.load * 0.14) * (0.6 + heat * 0.5)
          + Math.sin(t * 0.6 + phase) * 0.015);
      } else if (s.state === ST_DEGRADED) {
        fbSet(x, y, cWarn, (0.3 + 0.5 * heat) * (0.92 + 0.08 * Math.sin(t * 2.0 + phase)));
      } else if (s.state === ST_DOWN) {
        fbSet(x, y, cCrit, 0.3 + 0.68 * heat
          * (s.tier === 0 ? 0.7 + 0.3 * Math.abs(Math.sin(t * 3.0)) : 1.0));
      } else if (s.state === ST_UNKNOWN) {
        fbSet(x, y, cUnknown, 0.18 + 0.14 * heat
          + (Math.floor(t * 1.4 + phase) & 1) * 0.08);
      } else {  // ST_MAINT
        fbSet(x, y, cMaint, 0.2 + 0.16 * heat
          + (Math.floor(t * 1.4 + phase) & 1) * 0.1);
      }
    }
  }

  /* A1 · Flow gates — CONTRACT-AW §1 + §4 (task #11).
     This REPLACES the pool-lane A1 that shipped under CONTRACT-CP. That screen
     drew four synthetic pool lanes through one bar at x=34; A1 is now a picture
     of the actual Ubiquiti path, one vertical gate per device, traffic flowing
     left→right along the direction packets really travel:

         APs  →  hubs  →  switch  →  router  →  internet (+ wanBackup)

     §1's two placement bullets cannot both be read literally (the first puts
     every hub on the left, the second puts a hub on the right, and "the router
     line" is never placed). §4 IS unambiguous — "Internet: rightmost column
     treatment right of the router gate" — so §4 governs and §1's "switch + hub"
     is read as "switch + router". Flagged to Lead 2026-08-07; if that
     adjudication comes back differently, only the band table below moves.

     §1/§4 fix the HEIGHTS (router 11, switch 8, hubs 5-6 staggered, APs 3) and
     the stagger, but not the columns. The band table is therefore this lane's
     proposal, sent to Lead for ratification so the firmware (AW2) and this
     renderer agree on the same fixture bytes — acceptance A1 is a PARITY test,
     so a private choice here is a divergence there.

     DEVIATION (page-only, inherited from the old A1): the firmware holds its
     particle array in a file static because only one screen is ever live;
     twelve are live here, so the state is per-tile in `st`. */

  // Band table — [lo, hi] columns on the 53-wide grid. n gates spread evenly
  // inside the band; a single gate sits at the band's centre.
  const A1_BAND = {};
  A1_BAND[GR_AP] = [2, 16];
  A1_BAND[GR_HUB] = [20, 34];
  A1_BAND[GR_SWITCH] = [38, 44];
  A1_BAND[GR_ROUTER] = [46, 48];
  const A1_INET_X = 51;                 // §4 "rightmost column treatment"
  const A1_WAN_X = 52;                  // §4 wanBackup, "dimmer beside it"
  // Left-to-right band order. wanBackup is NOT a band: §1 draws it with the
  // internet connection, not as a gate in the chain.
  const A1_BANDS = [GR_AP, GR_HUB, GR_SWITCH, GR_ROUTER];

  function a1Spread(n, lo, hi) {
    if (n <= 0) return [];
    if (n === 1) return [ri((lo + hi) / 2)];
    const out = new Array(n);
    const step = (hi - lo) / (n - 1);
    for (let i = 0; i < n; i++) out[i] = ri(lo + i * step);
    return out;
  }

  /* a1GateGeom — §1 heights and §4 stagger, by role and index-within-role. */
  function a1GateGeom(role, i, n) {
    if (role === GR_ROUTER) return { h: PH, y0: 0 };                    // 100%
    if (role === GR_SWITCH) {
      const h = 8;                                                      // 75%
      return { h: h, y0: Math.floor((PH - h) / 2) };
    }
    if (role === GR_HUB) {
      // 50% of 11 is 5.5, so §1's "5-6 rows" alternates with the stagger:
      // even gates are 6 rows aligned TOP, odd are 5 rows aligned BOTTOM.
      const h = (i % 2 === 0) ? 6 : 5;
      return { h: h, y0: (i % 2 === 0) ? 0 : (PH - h) };
    }
    /* APs, §9 A-1 verbatim: h=3, y0 = round(i*(11-3)/(n-1)), centred at 4 when
       n==1. The band is DISTRIBUTED down the height, not parked on shared
       rows. (AW2's firmware briefly fixed these at y0=4 and this lane matched
       it; A-1 then ratified the distribution and pointed AW2 here. The
       contract is the parity oracle, not either implementation.) */
    if (n <= 1) return { h: 3, y0: 4 };
    return { h: 3, y0: ri(i * (PH - 3) / (n - 1)) };
  }

  /* a1Layout — pure: gear rows in, gates + legs out. Exported to the harness
     so the geometry can be asserted without a canvas. */
  function a1Layout(gear) {
    const bands = [];
    for (let b = 0; b < A1_BANDS.length; b++) {
      const role = A1_BANDS[b];
      const members = [];
      for (let i = 0; i < gear.length; i++) if (gear[i].role === role) members.push(gear[i]);
      if (!members.length) { bands.push([]); continue; }
      const xs = a1Spread(members.length, A1_BAND[role][0], A1_BAND[role][1]);
      const row = [];
      for (let i = 0; i < members.length; i++) {
        const g = a1GateGeom(role, i, members.length);
        row.push({
          role: role, state: members[i].state, rx: members[i].rx, tx: members[i].tx,
          x: xs[i], y0: g.y0, h: g.h, cy: g.y0 + Math.floor(g.h / 2),
        });
      }
      bands.push(row);
    }
    const gates = [];
    for (let b = 0; b < bands.length; b++)
      for (let i = 0; i < bands[b].length; i++) gates.push(bands[b][i]);

    /* Legs. Each gate feeds the gate at (i mod n) in the next NON-EMPTY band,
       so a fleet missing a whole tier still draws a connected path instead of
       a floating column. The last band's gates run on to the internet. */
    const legs = [];
    const nonEmpty = [];
    for (let b = 0; b < bands.length; b++) if (bands[b].length) nonEmpty.push(bands[b]);
    for (let b = 0; b + 1 < nonEmpty.length; b++) {
      const src = nonEmpty[b], dst = nonEmpty[b + 1];
      for (let i = 0; i < src.length; i++) {
        const d = dst[i % dst.length];
        legs.push({ x0: src[i].x, y0: src[i].cy, x1: d.x, y1: d.cy, src: src[i] });
      }
    }
    if (nonEmpty.length) {
      const last = nonEmpty[nonEmpty.length - 1];
      for (let i = 0; i < last.length; i++)
        legs.push({ x0: last[i].x, y0: last[i].cy, x1: A1_INET_X, y1: 5, src: last[i] });
    }
    let wan = null;
    for (let i = 0; i < gear.length; i++) if (gear[i].role === GR_WANBK) { wan = gear[i]; break; }
    let router = null;
    for (let i = 0; i < gear.length; i++) if (gear[i].role === GR_ROUTER) { router = gear[i]; break; }
    return { bands: bands, gates: gates, legs: legs, wan: wan, router: router };
  }

  function a1Col(state) {
    return state === GS_DOWN ? cCrit : state === GS_DEGRADED ? cWarn : cAzure;
  }

  /* One vertical flow gate. A line-for-line port of
     firmware/panelScreensA.c:95 a1Gate() — same signature, same broken-when-
     down rule (every other row dropped, so a dead link reads as severed and
     not merely as a red one from across the room), same lack of an endcap
     boost. Both sides must paint identical pixels from identical gear bytes;
     keep this function and that one in step. */
  function a1Gate(x, y0, h, state, brightness) {
    const col = a1Col(state);
    for (let k = 0; k < h; k++) {
      if (state === GS_DOWN && (k % 2) === 1) continue;
      fbSet(x, y0 + k, col, brightness);
    }
  }

  const A1_PARTICLES = 72;
  function a1Init(nLegs) {
    const r = rngNew(77);              // same seed as the old A1 (A.c:97)
    const out = new Array(A1_PARTICLES);
    for (let i = 0; i < A1_PARTICLES; i++) {
      out[i] = {
        leg: nLegs > 0 ? (i % nLegs) : 0,
        u: rngNext(r),                 // progress along the leg, 0..1
        v: 0.55 + rngNext(r) * 0.6,    // per-particle speed jitter
      };
    }
    return out;
  }

  function sA1(env, t, dt, st) {
    /* §4: "No gear data (gearCount 0 or v1 server) → A1's existing no-data
       treatment." A v1 server, an empty inventory and a poller that has been
       down past the 60 s freshness window (§E7) all land here, and all three
       mean the same thing to a viewer: this screen has nothing to say. */
    if (!env.gearCount) { screenNoData(t); return; }

    const L = a1Layout(env.gear);
    if (!st.a1 || st.a1.n !== L.legs.length) {
      st.a1 = { n: L.legs.length, p: a1Init(L.legs.length) };
    }
    const parts = st.a1.p;

    /* Legs first, dimmest: the wire is lit by the SOURCE's rxLevel, so a link
       carrying nothing still shows its shape rather than vanishing. */
    for (let i = 0; i < L.legs.length; i++) {
      const lg = L.legs[i];
      const b = 0.035 + (lg.src.rx / MAX_LEVEL) * 0.05;
      const span = lg.x1 - lg.x0;
      for (let x = lg.x0 + 1; x < lg.x1; x++) {
        const k = span > 0 ? (x - lg.x0) / span : 0;
        fbAdd(x, ri(lg.y0 + (lg.y1 - lg.y0) * k), cQuiet, b);
      }
    }

    /* Gates. §4: colour by state, and a DOWN gate is "rendered broken" — every
       other pixel dropped, so it reads as a severed line at a glance and not
       merely as a red one, which matters on a panel seen from across a room. */
    for (let i = 0; i < L.gates.length; i++) {
      const g = L.gates[i];
      const lvl = (g.rx > g.tx ? g.rx : g.tx) / MAX_LEVEL;
      const base = g.state === GS_DOWN ? 0.8
        : g.state === GS_DEGRADED ? 0.5 * (0.8 + 0.2 * Math.abs(Math.sin(t * 2.2)))
          : 0.20 + lvl * 0.30;
      a1Gate(g.x, g.y0, g.h, g.state, base);
    }

    /* Particles. §4: spawn rate and brightness scale by rx/txLevel — here the
       UPSTREAM direction, so a leg is driven by its source's txLevel. Level 0
       is idle by §3.1 and draws nothing at all: a creeping particle would be a
       claim of traffic the panel was told there is none of. */
    for (let i = 0; i < A1_PARTICLES; i++) {
      const pt = parts[i];
      const lg = L.legs[pt.leg];
      if (!lg) continue;
      const lvl = lg.src.tx / MAX_LEVEL;
      const gate = lg.src.state;
      const rate = gate === GS_DOWN ? 0 : gate === GS_DEGRADED ? 0.45 : 1.0;
      pt.u += pt.v * (0.25 + lvl * 1.15) * rate * dt;
      while (pt.u >= 1) pt.u -= 1;
      if (lg.src.tx <= 0 || rate === 0) continue;
      const px = ri(lg.x0 + (lg.x1 - lg.x0) * pt.u);
      const py = ri(lg.y0 + (lg.y1 - lg.y0) * pt.u);
      const col = a1Col(gate);
      const b = 0.22 + lvl * 0.38;
      fbAdd(px, py, col, b);
      fbAdd(px - 1, py, col, b * 0.3);
    }

    /* Internet, §4: the rightmost column, right of the router gate, with the
       backup WAN dimmer beside it.

       §10 A-3, the ruling on this lane's flag: the internet is only meaningful
       THROUGH the router, so the marker column renders in the ROUTER's state
       treatment — warn when the router is degraded, crit and broken when it is
       down. Never a healthy full-height marker over a dead router. A missing
       router row is treated as down for the same reason: nothing is reaching
       the internet through a gateway we have no word on. */
    a1Gate(A1_INET_X, 0, PH, L.router ? L.router.state : GS_DOWN, 0.50);

    /* wanBackup keeps its OWN device state (§10 A-3), which is the point: when
       the router goes down this column stays lit beside a broken internet
       marker and reads as the only live path left. Rows 3..7, dimmer —
       firmware/panelScreensA.c:154, a1Gate(52, 3, 5, state, 0.28f). */
    if (L.wan) a1Gate(A1_WAN_X, 3, 5, L.wan.state, 0.28);
  }

  /* A2 · MQ and SNMP.
     mockup: sA2() Themes.dc.html:474 · firmware: panelScreensA.c:173-207.
     DEVIATION (A.c:158-172): the mockup's lane backlogs are scenario
     constants; protocol.h carries no MQ/SNMP telemetry, so the four lanes are
     driven by loss / dataStale / unknown-share / degraded-share. Geometry,
     rates, colours and thresholds unchanged. */
  const A2_LANE_Y = [1, 3, 6, 8];                                 // A.c:174
  const A2_LANE_RATES = [0.7, 0.45, 0.9, 0.3];                    // A.c:175
  function sA2(env, t) {
    const denom = env.total > 0 ? env.total : 1;
    const backlog = [
      env.loss / 100 * 2,
      env.dataStale ? 0.8 : 0.05,
      env.unknown / denom,
      env.deg / denom,
    ];
    for (let i = 0; i < 4; i++) {
      const y = A2_LANE_Y[i];
      for (let x = 4; x < PW; x++) fbSet(x, y, cQuiet, 0.04);
      const bl = backlog[i] > 1 ? 1 : backlog[i];
      const q = ri(bl * 3.0);
      for (let k = 0; k < 3; k++)
        fbSet(k, y, k < q ? (bl > 0.5 ? cCrit : cWarn) : cQuiet, k < q ? 0.7 : 0.06);
      const base = (i < 2) ? cAzure : cOk;
      for (let j = 0; j < 5; j++) {
        const x = 5.0 + ((t * A2_LANE_RATES[i] * 22.0 + j * 10.0 + i * 4.0) % (PW - 6));
        const col = bl > 0.5 ? cWarn : base;
        const px = ri(x);
        fbAdd(px, y, col, 0.45);
        fbAdd(px - 1, y, col, 0.15);
      }
    }
    for (let x = 0; x < PW; x++) fbSet(x, 5, cQuiet, (x % 4) === 0 ? 0.07 : 0.02);
    fbSet(0, 10, cAzure, 0.1);
    fbSet(1, 10, cAzure, 0.1);
  }

  // =====================================================================
  // 7. THEME B · TEXT — panelScreensB.c
  // =====================================================================

  /* B0 · Condition counts.
     mockup: sB0() Themes.dc.html:498 · firmware: panelScreensB.c:16-56.
     DEVIATION (B.c:29-48, the 2026-08-04 hardware pass): text hierarchy is
     carried by HUE, not brightness — cQuiet is never used for small text any
     more, so a zero count reads azure and a real one reads its condition
     colour. The tier flag is computed from POOL aggregates (full fleet) rather
     than the 64-capped systems array. */
  function sB0(env) {
    const up = String(env.up), dn = String(env.down);
    const dg = String(env.deg), mn = String(env.maint);

    panelBig(1, 2, up, cQuiet, 0.3);
    panelTextOver(1, 0, "UP", cOk, 0.5);

    let x = 2 + bigW(up) + 3;
    if (x < 22) x = 22;

    let cx = panelTextOver(x, 0, dn, env.down ? cCrit : cAzure, env.down ? 0.9 : 0.16);
    cx = panelTextOver(cx + 1, 0, "DOWN", cAzure, 0.3);

    let tierZeroDown = 0;
    for (let i = 0; i < env.poolCount; i++)
      if (env.pools[i].tier === 0 && env.pools[i].down > 0) tierZeroDown = 1;
    const tierMsg = env.down ? (tierZeroDown ? "TIER 0" : "TIER 3") : "OK";
    panelTextOver(cx + 3, 0, tierMsg, tierZeroDown ? cCrit : cAzure, 0.4);

    cx = panelTextOver(x, 6, dg, env.deg ? cWarn : cAzure, env.deg ? 0.65 : 0.16);
    cx = panelTextOver(cx + 1, 6, "DEG", cAzure, 0.3);
    cx = panelTextOver(cx + 3, 6, mn, env.maint ? cMaint : cAzure, env.maint ? 0.5 : 0.16);
    panelTextOver(cx + 1, 6, "MNT", cAzure, 0.3);
  }

  /* B1 · Throughput.
     mockup: sB1() Themes.dc.html:519 · firmware: panelScreensB.c:63-98.
     DEVIATION (B.c:88-97): the mockup synthesised OUT as 0.6x and PEAK as
     1.4x of one aggregate figure; the wire carries the real split, so IN/OUT
     are the real directions and the bar is scaled against the adaptive peak. */
  function sB1(env, t) {
    panelBig(1, 2, env.gLabel, cQuiet, 0.3);
    panelTextOver(1, 0, "AGGREGATE", cAzure, 0.5);

    let x0 = bigW(env.gLabel) + 4;
    if (x0 < 26) x0 = 26;

    let util = histAt(env, env.thru, HIST_LEN - 1) + histAt(env, env.egress, HIST_LEN - 1);
    if (util > 1) util = 1;

    for (let k = 0; k < 24; k++) {
      const frac = k / 24;
      const on = frac < util;
      const col = frac > 0.85 ? cCrit : frac > 0.7 ? cWarn : cAzure;
      fbSet(x0 + k, 1, on ? col : cQuiet, on ? 0.5 : 0.05);
      fbSet(x0 + k, 2, on ? col : cQuiet, on ? 0.18 : 0.03);
    }

    panelTextOver(x0, 4, ri(util * 100) + "% LINK", util > 0.85 ? cWarn : cInk, 0.5);

    const inLabel = formatRate(env.rxGbps * 1000000);
    const outLabel = formatRate(env.txGbps * 1000000);
    const ticker = "IN " + inLabel + "/S" + PANEL_SEP + "OUT " + outLabel + "/S"
      + PANEL_SEP + "LINK " + ri(util * 100) + "%" + PANEL_TAIL;
    panelScroll(t, 6, ticker, cInk, 0.45, 11.0);
  }

  /* B2 · Load and latency.
     mockup: sB2() Themes.dc.html:535 · firmware: panelScreensB.c:108-180.
     Two firmware DEVIATIONS reproduced verbatim because they are the shipped
     truth and the mockup's layout does not fit 53 px:
       #14 (B.c:113-150) row 0 — label contracts to "LOAD"; the P95 and LOSS
           readings ALTERNATE on a 5 s slot, right-aligned to the panel edge
           and floored clear of the label at rMinX = textW("LOAD") + 3.
       #13 (B.c:160-179) row 6 — "%d/%d>85": the denominator makes CONTRACT
           §9b's second population self-evident, right-aligned and floored at
           minX = bigW(pct) + 3. */
  function sB2(env, t) {
    const pct = ri(env.meanLoad * 100) + "%";
    panelBig(1, 2, pct, env.meanLoad > 0.75 ? cWarn : cQuiet, env.meanLoad > 0.75 ? 0.22 : 0.3);
    panelTextOver(1, 0, "LOAD", cInk, 0.45);

    let reading, rcol;
    if ((trunc(t / 5.0) & 1) === 0) {
      let ms = ri(env.rtt);
      if (ms > 999) ms = 999;
      reading = "P95 " + ms + "MS";
      rcol = env.rtt > 35.0 ? cWarn : cInk;
    } else {
      reading = env.loss >= 10.0 ? ("LOSS " + ri(env.loss) + "%")
        : ("LOSS " + f1(env.loss) + "%");
      rcol = env.loss > 1.0 ? cCrit : cInk;
    }
    let rx = PW - textW(reading);
    const rMinX = textW("LOAD") + 3;
    if (rx < rMinX) rx = rMinX;
    panelTextOver(rx, 0, reading, rcol, 0.5);

    let hot = 0;
    const n = env.systems.length;
    for (let i = 0; i < n; i++) if (env.systems[i].loadPct > 85) hot++;

    const over = hot + "/" + n + ">85";
    let tx = PW - textW(over);
    const minX = bigW(pct) + 3;
    if (tx < minX) tx = minX;
    panelTextOver(tx, 6, over, hot ? cWarn : cAzure, 0.45);
  }

  // =====================================================================
  // 8. THEME C · CHARTS — panelScreensC.c
  //    DEVIATION common to C0 and C1 (panelScreensC.c:6-12): the mockup
  //    scrolls a STATIC waveform with a synthetic offset because its data
  //    never changed. The rings genuinely advance here, so the traces are
  //    indexed chronologically and the offset is dropped.
  // =====================================================================

  /* C0 · Load distribution.
     mockup: sC0() Themes.dc.html:547 · firmware: panelScreensC.c:25-54. */
  function sC0(env) {
    for (let b = 0; b < LOAD_BUCKETS; b++) {
      const share = env.loadHist[b];
      const pct = (b + 0.5) / LOAD_BUCKETS;
      let h = share <= 0 ? 0 : ri(Math.pow(share, 0.62) * 7.0);
      if (share > 0 && h < 1) h = 1;
      const col = pct > 0.9 ? cCrit : pct > 0.75 ? cWarn : cQuiet;
      const base = pct > 0.9 ? 0.8 : pct > 0.75 ? 0.5 : 0.12 + pct * 0.15;
      for (let k = 0; k < h; k++)
        fbSet(b, 7 - k, col, base * (k === h - 1 ? 1.5 : 1.0));
    }
    for (let x = 0; x < LOAD_BUCKETS; x++)
      fbSet(x, 8, cQuiet, (x % 7) === 0 ? 0.12 : 0.03);

    for (let k = 0; k < 15; k++) {
      const idx = HIST_LEN - 15 + k;
      const v = histAt(env, env.thru, idx);
      const o = histAt(env, env.egress, idx);
      const x = 38 + k;
      let hi = ri(v * 4.0); if (hi < 1) hi = 1;
      let ho = ri(o * 4.0); if (ho < 1) ho = 1;
      for (let j = 0; j < hi; j++)
        fbSet(x, 4 - j, v > 0.85 ? cWarn : cAzure, j === hi - 1 ? 0.55 : 0.14);
      for (let j = 0; j < ho; j++)
        fbSet(x, 6 + j, cAzure, j === ho - 1 ? 0.38 : 0.11);
    }
    for (let y = 0; y < PH; y++)
      fbSet(36, y, cQuiet, (y % 2) ? 0.05 : 0.1);
  }

  /* C1 · Live traces.
     mockup: sC1() Themes.dc.html:567 · firmware: panelScreensC.c:61-82. */
  function sC1(env) {
    const traces = [
      { data: env.thru, y0: 0, col: cAzure, hot: 0.85 },
      { data: env.cpuHist, y0: 4, col: cOk, hot: 0.80 },
      { data: env.rttHist, y0: 8, col: cAzure, hot: 0.70 },
    ];
    for (let ti = 0; ti < 3; ti++) {
      for (let x = 0; x < PW; x++) {
        const v = histAt(env, traces[ti].data, x);
        let h = ri(v * 3.0); if (h < 1) h = 1;
        const col = v > traces[ti].hot ? cWarn : traces[ti].col;
        for (let k = 0; k < h; k++)
          fbSet(x, traces[ti].y0 + 3 - 1 - k, col, k === h - 1 ? 0.5 : 0.12);
      }
    }
    for (let x = 0; x < PW; x += 3) {
      fbAdd(x, 3, cQuiet, 0.05);
      fbAdd(x, 7, cQuiet, 0.05);
    }
  }

  /* C2 · Pool load.
     mockup: sC2() Themes.dc.html:588 · firmware: panelScreensC.c:90-116.
     DEVIATION #8 (C.c:98-101): severity order is down > degraded > unknown >
     maint — a pool nobody can reach outranks one down for planned work. D1
     uses the same order; the two pool screens must not disagree. */
  function sC2(env) {
    const n = env.poolCount > 7 ? 7 : env.poolCount;
    for (let i = 0; i < n; i++) {
      const p = env.pools[env.poolWorstFirst[i]];
      const y = i + 2;
      if (y > 8) break;
      const key = p.down ? cCrit : p.degraded ? cWarn
        : p.unknown ? cUnknown : p.maint ? cMaint : cQuiet;
      const kb = p.down ? 0.9 : p.degraded ? 0.6 : p.unknown ? 0.4
        : p.maint ? 0.3 : 0.12;
      fbSet(0, y, key, kb);
      const len = ri(p.load * 46.0);
      for (let k = 0; k < 46; k++) {
        const on = k < len;
        const frac = k / 46;
        const col = frac > 0.85 ? cCrit : frac > 0.7 ? cWarn : cAzure;
        fbSet(2 + k, y, on ? col : cQuiet, on ? (k === len - 1 ? 0.6 : 0.22) : 0.03);
      }
      if (p.tier === 0) fbSet(49, y, cCrit, 0.18);
    }
    for (let x = 0; x < PW; x++)
      fbSet(x, 0, cQuiet, (x % 4) === 0 ? 0.08 : 0.02);
    panelText(2, 9, "BY POOL", cAzure, 0.28);
  }

  // =====================================================================
  // 9. THEME D · INSTRUMENTS — panelScreensD.c
  // =====================================================================

  /* D0 · Reachability.
     mockup: sD0() Themes.dc.html:609 + sample() :626 · firmware:
     panelScreensD.c:38-87. 11-row waterfall, one new column every 0.42 s.
     DEVIATION (D.c:18-24): the mockup's rows are 11 named probe targets with
     synthetic per-row loss; protocol.h carries only fleet-aggregate
     rtt/loss, so each row tracks one of the 11 worst-first systems.
     DEVIATION (page-only): gWf/gWfRng/gWfStep are firmware file statics —
     per-tile here, for the same reason as A1. */
  const D0_LOSS = 0, D0_SLOW = 1, D0_OK = 2;                      // D.c:25-27
  function d0Sample(env, i, rng) {                                // D.c:38-62
    const si = (i < env.probeRows) ? env.probeRow[i] : 0xFF;
    let st = ST_UNKNOWN;
    if (si !== 0xFF && si < env.systems.length) st = env.systems[si].state;

    const lossRate = env.loss / 100;
    const r = rngNext(rng);
    if (st === ST_DOWN) {
      if (r > 0.25) return { st: D0_LOSS, rtt: 0.15 };
      return { st: D0_SLOW, rtt: 0.15 };
    }
    if (st === ST_DEGRADED || st === ST_UNKNOWN) {
      if (r > 0.5) return { st: D0_SLOW, rtt: 0.15 };
    } else if (lossRate > 0 && r < lossRate) {
      return { st: D0_LOSS, rtt: 0.15 };
    }
    let base = env.rtt / 100;
    if (base < 0.05) base = 0.05;
    if (base > 1.0) base = 1.0;
    let rtt = base * (0.8 + rngNext(rng) * 0.4);
    if (rtt > 1.0) rtt = 1.0;
    return { st: D0_OK, rtt: rtt };
  }
  function sD0(env, t, dt, st) {
    if (!st.d0) {
      const rng = rngNew(4211);                                   // D.c:66
      const grid = new Array(PH);
      for (let y = 0; y < PH; y++) {
        grid[y] = new Array(PW);
        for (let x = 0; x < PW; x++) grid[y][x] = d0Sample(env, y, rng);
      }
      st.d0 = { rng: rng, grid: grid, step: t };
    }
    const w = st.d0;
    if (t - w.step > 0.42) {                                      // D.c:72
      w.step = t;
      for (let y = 0; y < PH; y++) {
        w.grid[y].shift();
        w.grid[y].push(d0Sample(env, y, w.rng));
      }
    }
    for (let y = 0; y < PH; y++) {
      for (let x = 0; x < PW; x++) {
        const v = w.grid[y][x];
        if (v.st === D0_LOSS) fbSet(x, y, cCrit, 0.85);
        else if (v.st === D0_SLOW) fbSet(x, y, cWarn, 0.5);
        else fbSet(x, y, cAzure, 0.06 + v.rtt * 0.26);
      }
    }
  }

  /* D1 · Pool bars + ticker.
     mockup: sD1() Themes.dc.html:634 · firmware: panelScreensD.c:95-143.
     Two firmware DEVIATIONS the mockup lacks:
       - worst-first row ordering (D.c:101-103): with only 6 of up to 8 rows
         visible, wire order can push the one down pool off the bottom;
       - the UNKNOWN band `nu` (D.c:121/126/134): unreachable hosts previously
         rendered as idle shimmer. */
  function sD1(env, t) {
    let big = 1;
    for (let i = 0; i < env.poolCount; i++)
      if (env.pools[i].total > big) big = env.pools[i].total;

    for (let i = 0; i < env.poolCount && i <= 5; i++) {
      const p = env.pools[env.poolWorstFirst[i]];
      const y = i;
      const worst = p.down ? cCrit : p.degraded ? cWarn
        : p.unknown ? cUnknown : p.maint ? cMaint : cQuiet;
      const kb = p.down ? 0.9 : p.degraded ? 0.6 : p.unknown ? 0.4
        : p.maint ? 0.3 : 0.14;
      fbSet(0, y, worst, kb);
      fbSet(1, y, worst, kb * 0.5);

      let len = ri(50.0 * p.total / big);
      if (len < 6) len = 6;
      const tot = p.total ? p.total : 1;
      let nd = ri(len * p.down / tot);
      let ng = ri(len * p.degraded / tot);
      let nu = ri(len * p.unknown / tot);
      let nm = ri(len * p.maint / tot);
      if (p.down && nd < 1) nd = 1;
      if (p.degraded && ng < 1) ng = 1;
      if (p.unknown && nu < 1) nu = 1;
      if (p.maint && nm < 1) nm = 1;
      for (let k = 0; k < len; k++) {
        const x = 3 + k;
        if (k >= len - nd) fbSet(x, y, cCrit, 0.9);
        else if (k >= len - nd - ng) fbSet(x, y, cWarn, 0.58);
        else if (k >= len - nd - ng - nu) fbSet(x, y, cUnknown, 0.42);
        else if (k >= len - nd - ng - nu - nm) fbSet(x, y, cMaint, 0.36);
        else fbSet(x, y, cQuiet, 0.09 + 0.08 * (0.5 + 0.5 * Math.sin(t * 0.9 + k * 0.35 + i)));
      }
    }
    for (let x = 0; x < PW; x += 2) fbSet(x, 6, cQuiet, 0.05);
    panelScroll(t, 6, env.ticker, cInk, 0.48, 13.0);
  }

  /* D2 · Ambient field.
     mockup: sD2() Themes.dc.html:657 · firmware: panelScreensD.c:152-207.
     Procedural 3-sine field, warmth from mean load, up to 9 crit sparks whose
     glow is set by the tier of the first down system in WIRE order (D.c:178),
     and a 5 s rotating centred caption over a dimmed knockout box. */
  const D2_TIER_GLOW = [1.0, 0.7, 0.45, 0.3];                     // D.c:185
  function sD2(env, t) {
    const totalF = env.total < 10 ? 10 : env.total;
    const gran = 0.6 + Math.log(totalF) / Math.LN10 * 1.5;
    let warmth = (env.meanLoad - 0.35) / 0.5;
    if (warmth < 0) warmth = 0;
    if (warmth > 1) warmth = 1;

    for (let y = 0; y < PH; y++) {
      for (let x = 0; x < PW; x++) {
        const u = x / PW * gran;
        const v = y / PH * gran * 0.4;
        let n = Math.sin(u * 2.1 + t * 0.5) * 0.5
          + Math.sin(v * 3.3 - t * 0.31) * 0.3
          + Math.sin((u + v) * 1.7 + t * 0.77) * 0.4;
        n = (n + 1.2) / 2.4;
        if (n < 0) n = 0;
        const b = 0.03 + Math.pow(n, 3.1) * 0.6;
        fbSetRgb(x, y,
          cAzure[0] + (cWarn[0] - cAzure[0]) * warmth * n,
          cAzure[1] + (cWarn[1] - cAzure[1]) * warmth * n,
          cAzure[2] + (cWarn[2] - cAzure[2]) * warmth * n,
          b);
      }
    }

    const sparks = env.down > 9 ? 9 : env.down;
    let downTier = 3;
    for (let i = 0; i < env.systems.length; i++)
      if (env.systems[i].state === ST_DOWN) { downTier = env.systems[i].tier & 3; break; }
    for (let i = 0; i < sparks; i++) {
      const x = 4 + ((i * 11 + 3) % (PW - 8));
      const y = (i * 4 + 2) % PH;
      const puls = (0.35 + 0.65 * D2_TIER_GLOW[downTier]) * (0.7 + 0.3 * Math.sin(t * 3.0 + i));
      fbSet(x, y, cCrit, puls);
      fbAdd(x - 1, y, cCrit, puls * 0.25);
      fbAdd(x + 1, y, cCrit, puls * 0.25);
    }

    const slot = trunc(t / 5.0) % 3;
    let msg;
    if (slot === 0) msg = env.total + " SYS";
    else if (slot === 1) msg = ri(env.meanLoad * 100) + "% LOAD";
    else msg = env.gLabel + "/S";
    const col = (slot === 1 && warmth > 0.6) ? cWarn : cInk;
    const tw = textW(msg), tx = Math.floor((PW - tw) / 2);
    for (let y = 2; y <= 8; y++)
      for (let x = tx - 2; x <= tx + tw + 1; x++) fbDim(x, y, 0.1);
    panelText(tx, 3, msg, col, 0.6);
  }

  // =====================================================================
  // 10. Universal treatments — panelInlay.c
  // =====================================================================

  /* Alert inlay. mockup: inlay() Themes.dc.html:687 · firmware:
     panelInlay.c:20-43. dimAll 0.1, pulsing crit rails on x=0 and x=52,
     subject truncated to 12 chars centred on row 1, detail scrolling row 6. */
  function inlay(env, t) {
    fbDimAll(0.1);
    const pulse = 0.55 + 0.45 * Math.abs(Math.sin(t * 2.2));
    for (let y = 0; y < PH; y++) {
      fbSet(0, y, cCrit, pulse);
      fbSet(PW - 1, y, cCrit, pulse);
    }
    const subj = env.subject.slice(0, 12);
    let x = Math.floor((PW - textW(subj)) / 2);
    if (x < 2) x = 2;
    panelText(x, 1, subj, cCrit, 0.95);
    panelScroll(t, 6, env.detail + PANEL_TAIL, cInk, 0.55, 11.0);
  }

  /* Unacknowledged-episode beacon. firmware: panelInlay.c:63-67 (operator
     amendment 2026-08-04; the mockup and the DESIGN-BRIEF have no beacon).
     Four pixels top-right, square 1 Hz 50% duty, b=1.0, painted LAST. */
  function beacon(t) {
    if ((t % 1.0) >= 0.5) return;
    for (let y = 0; y <= 1; y++)
      for (let x = PW - 2; x <= PW - 1; x++) fbSet(x, y, cCrit, 1.0);
  }

  /* LINK LOST — panelInlay.c:76-85. Deliberately warn amber, not the alarm's
     crit rails: a dead cable is a panel fault, not a fleet fault. */
  function screenLinkLost(t) {
    fbDimAll(0.15);
    const msg = "LINK LOST";
    const tw = textW(msg), tx = Math.floor((PW - tw) / 2);
    for (let y = 2; y <= 8; y++)
      for (let x = tx - 2; x <= tx + tw + 1; x++) fbDim(x, y, 0.1);
    panelText(tx, 3, msg, cWarn, 0.55 + 0.35 * Math.abs(Math.sin(t * 1.6)));
    fbSet(trunc(t * 12.0) % PW, 9, cQuiet, 0.35);
  }

  /* NO DATA — panelInlay.c:90-95. CONTRACT §9: a zero-node fleet is valid
     data, not an error. */
  function screenNoData(t) {
    const msg = "NO DATA";
    const tw = textW(msg), tx = Math.floor((PW - tw) / 2);
    panelText(tx, 3, msg, cAzure, 0.28 + 0.14 * (0.5 + 0.5 * Math.sin(t * 0.8)));
    for (let x = 0; x < PW; x += 4) fbSet(x, 9, cQuiet, 0.05);
  }

  // =====================================================================
  // 11. Screen table + the paint order from main.c
  // =====================================================================
  const THEMES = [
    { key: "A", name: "ABSTRACT" },
    { key: "B", name: "TEXT" },
    { key: "C", name: "CHARTS" },
    { key: "D", name: "INSTRUMENTS" },
  ];
  const SCREENS = [
    { id: "A0", theme: 0, screen: 0, title: "State lattice", render: sA0 },
    { id: "A1", theme: 0, screen: 1, title: "Flow gates", render: sA1 },
    { id: "A2", theme: 0, screen: 2, title: "MQ and SNMP", render: sA2 },
    { id: "B0", theme: 1, screen: 0, title: "Condition counts", render: sB0 },
    { id: "B1", theme: 1, screen: 1, title: "Throughput", render: sB1 },
    { id: "B2", theme: 1, screen: 2, title: "Load and latency", render: sB2 },
    { id: "C0", theme: 2, screen: 0, title: "Load distribution", render: sC0 },
    { id: "C1", theme: 2, screen: 1, title: "Live traces", render: sC1 },
    { id: "C2", theme: 2, screen: 2, title: "Pool load", render: sC2 },
    { id: "D0", theme: 3, screen: 0, title: "Reachability", render: sD0 },
    { id: "D1", theme: 3, screen: 1, title: "Pool bars", render: sD1 },
    { id: "D2", theme: 3, screen: 2, title: "Ambient field", render: sD2 },
  ];

  /* paintScreen — the firmware's per-frame paint order (main.c): no-data
     treatment when the fleet is empty, else the active screen, then the inlay
     when the alarm is armed, then the beacon LAST so it overrides the inlay's
     right-hand rail. `armed` mirrors main.c's gAlarmArmed: alarmActive and not
     acknowledged for the current episode. */
  function paintScreen(fb, idx, env, t, dt, st, armed) {
    fbUse(fb);
    fbClear();
    if (!env || !env.haveData || env.total === 0) {
      screenNoData(t);
      if (armed) beacon(t);
      return;
    }
    SCREENS[idx].render(env, t, dt, st);
    if (armed) { inlay(env, t); beacon(t); }
  }

  // =====================================================================
  // 12. Canvas painter
  //
  //    583 LEDs x 12 screens x 25 fps is ~175k arc() calls a second, which no
  //    browser will hold. Instead: write the framebuffer as a 53x11 ImageData,
  //    nearest-neighbour upscale it, mask it to round dots with a cached
  //    destination-in stencil, then composite it with 'lighter' over a cached
  //    unlit-package layer. ~4 drawImage calls per tile per frame, and the
  //    cost is independent of how many LEDs are lit.
  //
  //    The composite reproduces the mockup's paint(): unlit package #0E1014,
  //    lit cell = rgb(14+R, 16+G, 20+B).
  // =====================================================================
  const CELL = 8;                       // device px per LED pitch
  const DOT_R = 3.1;
  const TILE_W = PW * CELL;             // 424
  const TILE_H = PH * CELL;             // 88
  const BEZEL = "#05070b";
  const UNLIT = "#0e1014";

  let LAYERS = null;
  function getLayers() {
    if (LAYERS) return LAYERS;
    function mk(w, h) { const c = document.createElement("canvas"); c.width = w; c.height = h; return c; }
    const tiny = mk(PW, PH);
    const mask = mk(TILE_W, TILE_H);
    const unlit = mk(TILE_W, TILE_H);
    const scratch = mk(TILE_W, TILE_H);
    const mctx = mask.getContext("2d"), uctx = unlit.getContext("2d");
    mctx.fillStyle = "#fff";
    uctx.fillStyle = UNLIT;
    for (let y = 0; y < PH; y++) {
      for (let x = 0; x < PW; x++) {
        const cx = x * CELL + CELL / 2, cy = y * CELL + CELL / 2;
        mctx.beginPath(); mctx.arc(cx, cy, DOT_R, 0, Math.PI * 2); mctx.fill();
        uctx.beginPath(); uctx.arc(cx, cy, DOT_R, 0, Math.PI * 2); uctx.fill();
      }
    }
    const tctx = tiny.getContext("2d");
    LAYERS = {
      tiny: tiny, tctx: tctx, img: tctx.createImageData(PW, PH),
      mask: mask, unlit: unlit, scratch: scratch, sctx: scratch.getContext("2d"),
    };
    return LAYERS;
  }

  /* gain — the LED driver's global brightness. panelHw.cpp:70 floors it at
     0.25 and ceilings at 1.00, so a virtual screen can be dimmed down but
     never out, exactly like the hardware. */
  function driverGain(brightnessPct) {
    let g = (brightnessPct == null ? 100 : brightnessPct) / 100;
    if (g < 0.25) g = 0.25;
    if (g > 1) g = 1;
    return g;
  }

  function paintCanvas(canvas, fb, gain) {
    if (!canvas) return;
    const ctx = canvas.getContext("2d");
    const L = getLayers();
    const px = L.img.data;
    for (let n = 0, i = 0; n < PW * PH; n++, i += 3) {
      let r = fb[i] * gain, g = fb[i + 1] * gain, b = fb[i + 2] * gain;
      px[n * 4] = r < 0 ? 0 : (r > 255 ? 255 : r);
      px[n * 4 + 1] = g < 0 ? 0 : (g > 255 ? 255 : g);
      px[n * 4 + 2] = b < 0 ? 0 : (b > 255 ? 255 : b);
      px[n * 4 + 3] = 255;
    }
    L.tctx.putImageData(L.img, 0, 0);

    const sc = L.sctx;
    sc.globalCompositeOperation = "source-over";
    sc.clearRect(0, 0, TILE_W, TILE_H);
    sc.imageSmoothingEnabled = false;
    sc.drawImage(L.tiny, 0, 0, PW, PH, 0, 0, TILE_W, TILE_H);
    sc.globalCompositeOperation = "destination-in";
    sc.drawImage(L.mask, 0, 0);
    sc.globalCompositeOperation = "source-over";

    ctx.globalCompositeOperation = "source-over";
    ctx.globalAlpha = 1;
    ctx.fillStyle = BEZEL;
    ctx.fillRect(0, 0, TILE_W, TILE_H);
    ctx.drawImage(L.unlit, 0, 0);
    ctx.globalCompositeOperation = "lighter";
    ctx.drawImage(L.scratch, 0, 0);
    // Cheap bloom: the same lit layer, blurred by a one-cell upscale.
    ctx.imageSmoothingEnabled = true;
    ctx.globalAlpha = 0.22;
    ctx.drawImage(L.scratch, -CELL, -CELL, TILE_W + CELL * 2, TILE_H + CELL * 2);
    ctx.globalAlpha = 1;
    ctx.globalCompositeOperation = "source-over";
  }

  // =====================================================================
  // 13. Data plumbing — live poll, fixture load, role gate
  // =====================================================================
  const POLL_MS = 5000;
  const CMD_KIND = {
    theme: 1, screen: 2, brightness: 3, autoBright: 4, ack: 5, dwell: 6, sleep: 7,
    screenEn: 8, screenWt: 9,            // CONTRACT-AW §3.2
  };
  const CMD_GROUP = { 1: "theme", 2: "screen", 3: "brightness", 4: "brightness", 5: "ack", 6: "dwell", 7: "sleep" };
  const CMD_LABEL = { 1: "theme", 2: "screen", 3: "brightness", 4: "auto-brightness", 5: "acknowledge", 6: "dwell", 7: "sleep/wake" };
  const CMD_EXPIRY_MS = 120000;          // CONTRACT-CP §10
  const CMD_GIVEUP_MS = 130000;          // expiry + one poll of slack

  // =====================================================================
  // 13a. Per-screen enable + dwell weight — CONTRACT-AW §3.2/§3.3 (task #12)
  // =====================================================================
  const N_SCREENS = 12;                  // §3.2 screenIdx 0..11
  /* §3.2 weightCode 0..5. The multiplier column is the operator's mental
     model (30 s base × 5 = 150 s, §1) and is shown as such — the code is a
     wire detail and never appears in the UI. */
  const WEIGHTS = [
    { code: 0, label: "¼×", mul: 0.25 },
    { code: 1, label: "½×", mul: 0.5 },
    { code: 2, label: "1×", mul: 1 },
    { code: 3, label: "2×", mul: 2 },
    { code: 4, label: "5×", mul: 5 },
    { code: 5, label: "10×", mul: 10 },
  ];
  function weightLabel(code) {
    for (let i = 0; i < WEIGHTS.length; i++) if (WEIGHTS[i].code === code) return WEIGHTS[i].label;
    return null;
  }

  /* Argument packing — §3.2, exactly:
       kind 8  arg = (screenIdx << 1) | enabled
       kind 9  arg = (screenIdx << 3) | weightCode
     Note the two use DIFFERENT shifts, which is easy to get quietly wrong;
     the harness asserts both against hand-computed values. */
  function packScreenEn(idx, enabled) { return (idx << 1) | (enabled ? 1 : 0); }
  function packScreenWt(idx, code) { return (idx << 3) | (code & 7); }

  /* cmdGroup — the pend map is keyed per CONTROL, and kinds 8/9 address one of
     twelve screens each, so they cannot share a key the way the global
     controls do: toggling A1 must not paint B2 as pending. */
  function cmdGroup(kind, arg) {
    if (kind === CMD_KIND.screenEn) return "screenEn:" + (arg >> 1);
    if (kind === CMD_KIND.screenWt) return "screenWt:" + (arg >> 3);
    return CMD_GROUP[kind];
  }
  function cmdLabel(kind, arg) {
    if (kind === CMD_KIND.screenEn) {
      const s = SCREENS[arg >> 1];
      return (s ? s.id : "screen " + (arg >> 1)) + " enable";
    }
    if (kind === CMD_KIND.screenWt) {
      const s = SCREENS[arg >> 3];
      return (s ? s.id : "screen " + (arg >> 3)) + " weight";
    }
    return CMD_LABEL[kind] || "command";
  }

  /* readScreenCfg — decode whatever GET /api/panel puts in panelScreenConfig
     into 12 entries of { enabled, weightCode } or null.
     STATE AUTHORITY (CONTRACT-AW §3.3): this is the PANEL's report of its own
     persisted config, relayed by the daemon. A null entry means the panel has
     not told us, and the UI must render that as unknown — NEVER as the §3.4
     factory default (enabled, 1×), because "we have not heard" and "the panel
     says all-on at 1×" are different facts and only one of them is ours to
     assert. Returning null for the whole thing is a valid outcome.

     AW1's exact packet shape is not landed yet, so the shapes below are all
     accepted and a mismatch is reported to Lead rather than guessed at:
       - [12] u8 bytes            bit0 enabled, bits1..3 weightCode  (§3.3)
       - [12] objects             { enabled, weightCode | weight }
       - { screenCfg: <above> }   the §3.3 field name, wrapped
       - 24-char hex string       the 12 bytes, as the daemon might relay them
     Anything else decodes to null: unknown, not assumed. */
  /* §10 D3: screenCfg bits 4..7 are RESERVED, transmitted zero, and receivers
     must IGNORE them — so they are masked off rather than treated as a
     malformed byte. weightCode values 6 and 7 are also reserved, but those are
     a different case: the field is in range yet carries no meaning we can
     render, so the entry reads as unknown rather than being clamped to a real
     weight the panel never claimed. */
  function decodeScreenByte(b) {
    const v = Number(b);
    if (!isFinite(v) || v < 0 || v > 255 || Math.floor(v) !== v) return null;
    const code = (v >> 1) & 7;
    if (code > 5) return null;                // reserved, not a weight
    return { enabled: (v & 1) === 1, weightCode: code };
  }
  function decodeScreenEntry(e) {
    if (e == null) return null;
    if (typeof e === "number") return decodeScreenByte(e);
    if (typeof e === "object") {
      const raw = (e.weightCode != null) ? e.weightCode : e.weight;
      const code = Number(raw);
      if (!isFinite(code) || code < 0 || code > 5 || Math.floor(code) !== code) return null;
      if (typeof e.enabled !== "boolean" && typeof e.enabled !== "number") return null;
      return { enabled: !!e.enabled, weightCode: code };
    }
    return null;
  }
  function readScreenCfg(raw) {
    let src = raw;
    if (src && !Array.isArray(src) && typeof src === "object") src = src.screenCfg;
    if (typeof src === "string") {
      const hex = src.trim();
      if (!/^[0-9a-fA-F]{24}$/.test(hex)) return null;
      const out = new Array(N_SCREENS);
      for (let i = 0; i < N_SCREENS; i++) out[i] = decodeScreenByte(parseInt(hex.substr(i * 2, 2), 16));
      return out;
    }
    if (!Array.isArray(src) || src.length !== N_SCREENS) return null;
    const out = new Array(N_SCREENS);
    for (let i = 0; i < N_SCREENS; i++) out[i] = decodeScreenEntry(src[i]);
    return out;
  }

  /* §10 D3: CONFIG flags bit0 is `cfgPersisted`, set only after the firmware
     has successfully written the config to flash. Reporting a setting and
     having survived a power cycle are different claims, and the page should not
     conflate them.

     Returns true only on a positive report. Everything else — no flags field,
     bit clear, unparseable — returns false, which the UI renders as SILENCE
     rather than as "not saved": D4 allows persistence to ship deferred and
     session-volatile, and in that world a standing "unsaved" warning would be
     noise about a feature that was never coming. All other flag bits are
     reserved; they are masked off and ignored. */
  function readCfgPersisted(raw) {
    if (!raw || Array.isArray(raw) || typeof raw !== "object") return false;
    const f = Number(raw.flags);
    if (!isFinite(f) || f < 0 || f > 255) return false;
    return (f & 1) === 1;
  }

  // Where the parity fixture may live once deployed. The repo path is
  // status-panel/fixtures/panel-snapshot.json; deployment copies it under the
  // docroot. Tried in order, first hit wins.
  const FIXTURE_PATHS = [
    "fixtures/panel-snapshot.json",
    "panel-snapshot.json",
    "../status-panel/fixtures/panel-snapshot.json",
  ];

  function api() { return window.SolariAPI; }

  /* readRole — FAIL CLOSED (CONTRACT-CP §10). Controls are presentation only;
     authority is server-side. Anything that is not a confirmed operator or
     admin — a viewer, a missing profile, a failed or still-pending whoami, or
     fixture mode, which by definition has no session — renders read-only. */
  function roleFromProfile(op) {
    if (!op || typeof op.role !== "string") return null;
    const r = op.role.toLowerCase();
    return (r === "operator" || r === "admin") ? r : "viewer";
  }

  /* parseReportedAt — the API hands back the raw DB datetime
     ("YYYY-MM-DD HH:MM:SS", UTC). Date.parse of that form is
     implementation-defined, so normalise it first. Returns ms or null. */
  function parseReportedAt(s) {
    if (!s) return null;
    if (typeof s === "number") return s > 1e11 ? s : s * 1000;
    const m = String(s).match(/^(\d{4})-(\d{2})-(\d{2})[ T](\d{2}):(\d{2}):(\d{2})/);
    if (m) return Date.UTC(+m[1], +m[2] - 1, +m[3], +m[4], +m[5], +m[6]);
    const v = Date.parse(s);
    return isNaN(v) ? null : v;
  }

  function fmtAge(ms) {
    const s = Math.round(ms / 1000);
    if (s < 90) return s + "s";
    if (s < 5400) return Math.round(s / 60) + "m";
    return Math.round(s / 3600) + "h";
  }

  // =====================================================================
  // 14. Virtual panel grid
  // =====================================================================
  const FIXTURE_T = 3.0;      // deterministic clock for ?fixture=1
  const FIXTURE_DT = 0.04;    // 25 Hz, the firmware's frame budget
  const FRAME_MS = 40;

  /* ScreenTileCfg — the §3.2 per-screen controls, rendered inside the tile of
     the screen they govern so "this one, off" needs no cross-reference.

     Every selected state here means THE PANEL SAID SO (§3.3). Three distinct
     situations must stay distinguishable, and the failure mode to avoid is
     collapsing them into a cheerful default:
       - the panel has not reported a config at all → nothing selected, said so
         in words. This is the §3.4 factory default's shape, but it is NOT the
         factory default and must not be drawn as one.
       - a command is in flight → nothing selected, spinner. The OLD value is
         withheld rather than left standing, so it cannot be misread as the new
         one having landed.
       - the panel reported → that one chip, and only that one, is pressed. */
  function ScreenTileCfg({ idx, cfg, pend, send, disabled }) {
    const enP = pend["screenEn:" + idx];
    const wtP = pend["screenWt:" + idx];
    const busy = function (p) { return !!p && (p.status === "sending" || p.status === "pending"); };
    const enBusy = busy(enP), wtBusy = busy(wtP);
    const known = !!cfg;
    const off = !!disabled;

    return (
      <div className="vp-cfg">
        <div className="vp-cfg__row">
          <span className="vp-cfg__label">Rotation</span>
          {[["On", true], ["Off", false]].map(function (o) {
            const on = known && !enBusy && cfg.enabled === o[1];
            return (
              <button key={o[0]} type="button"
                className={"chip chip--mini" + (on ? " on" : "")}
                aria-pressed={on}
                aria-label={SCREENS[idx].id + " rotation " + o[0]}
                disabled={off || enBusy}
                onClick={function () { send(CMD_KIND.screenEn, packScreenEn(idx, o[1])); }}>
                {o[0]}
              </button>
            );
          })}
          <CmdState p={enP} />
        </div>
        <div className="vp-cfg__row">
          <span className="vp-cfg__label">Dwell</span>
          {WEIGHTS.map(function (w) {
            const on = known && !wtBusy && cfg.weightCode === w.code;
            return (
              <button key={w.code} type="button"
                className={"chip chip--mini" + (on ? " on" : "")}
                aria-pressed={on}
                aria-label={SCREENS[idx].id + " dwell weight " + w.label + " of base"}
                disabled={off || wtBusy}
                onClick={function () { send(CMD_KIND.screenWt, packScreenWt(idx, w.code)); }}>
                {w.label}
              </button>
            );
          })}
          <CmdState p={wtP} />
        </div>
        {!known && (
          <div className="vp-cfg__note">
            The panel has not reported its screen configuration, so nothing is shown as in force.
          </div>
        )}
      </div>
    );
  }

  function VirtualPanel({ env, armed, gain, currentIdx, sleeping, still, screenCfg, pend, send, ctlDisabled }) {
    const canvasRefs = useRef([]);
    const stRef = useRef(null);
    const fbRef = useRef(null);
    const clockRef = useRef(0);
    /* Perf: visRef tracks which tiles are actually on screen (offscreen tiles
       skip both sim and paint — on a phone that is most of the grid), prevRef
       holds each tile's last-painted framebuffer so a static screen skips the
       putImageData + composite entirely. Both are invisible when everything is
       in view and animating; they only shed work that cannot be seen. */
    const visRef = useRef(SCREENS.map(function () { return true; }));
    const prevRef = useRef(null);
    const paintedRef = useRef(null);

    if (!fbRef.current) fbRef.current = fbNew();
    if (!stRef.current) stRef.current = SCREENS.map(function () { return {}; });
    if (!prevRef.current) prevRef.current = SCREENS.map(function () { return new Float32Array(PW * PH * 3); });

    /* Observe tile visibility once; entries map back through canvasRefs. */
    useEffect(function () {
      if (typeof IntersectionObserver === "undefined") return undefined;
      const obs = new IntersectionObserver(function (entries) {
        for (let k = 0; k < entries.length; k++) {
          const idx = canvasRefs.current.indexOf(entries[k].target);
          if (idx >= 0) visRef.current[idx] = entries[k].isIntersecting;
        }
      }, { rootMargin: "80px" });
      for (let i = 0; i < canvasRefs.current.length; i++)
        if (canvasRefs.current[i]) obs.observe(canvasRefs.current[i]);
      return function () { obs.disconnect(); };
    }, []);

    useEffect(function () {
      const fb = fbRef.current;
      const sts = stRef.current;
      /* env/armed/gain changed: every tile must repaint at least once. */
      paintedRef.current = SCREENS.map(function () { return false; });

      function drawAll(t, dt, force) {
        for (let i = 0; i < SCREENS.length; i++) {
          if (!force && !visRef.current[i]) continue;
          paintScreen(fb, i, env, t, dt, sts[i], armed);
          const prev = prevRef.current[i];
          let dirty = !paintedRef.current[i];
          if (!dirty) {
            for (let n = 0; n < prev.length; n++) {
              if (fb[n] !== prev[n]) { dirty = true; break; }
            }
          }
          if (!dirty) continue;
          prev.set(fb);
          paintedRef.current[i] = true;
          paintCanvas(canvasRefs.current[i], fb, gain);
        }
      }

      if (still) {
        // Parity mode: reset every stateful screen and advance a FIXED clock
        // in fixed steps, so the render is reproducible frame-for-frame.
        for (let i = 0; i < sts.length; i++) { delete sts[i].a1; delete sts[i].d0; }
        clockRef.current = 0;
        const steps = Math.round(FIXTURE_T / FIXTURE_DT);
        for (let k = 1; k <= steps; k++) {
          const t = k * FIXTURE_DT;
          for (let i = 0; i < SCREENS.length; i++) {
            paintScreen(fb, i, env, t, FIXTURE_DT, sts[i], armed);
          }
        }
        drawAll(FIXTURE_T, FIXTURE_DT, true);
        return undefined;
      }

      let raf = 0, last = 0, alive = true;
      function frame(now) {
        if (!alive) return;
        raf = window.requestAnimationFrame(frame);
        if (!last) last = now;
        const elapsed = now - last;
        if (elapsed < FRAME_MS) return;
        last = now;
        const dt = Math.min(elapsed, 200) / 1000;
        clockRef.current += dt;
        drawAll(clockRef.current, dt);
      }
      raf = window.requestAnimationFrame(frame);
      return function () { alive = false; window.cancelAnimationFrame(raf); };
    }, [env, armed, gain, still]);

    return (
      <div className="vp-grid">
        {SCREENS.map(function (s, i) {
          const isCurrent = i === currentIdx;
          const cfg = screenCfg ? screenCfg[i] : null;
          /* Only a REPORTED disable greys the tile. An unknown config leaves it
             looking normal, because the panel may well be showing it. */
          const skipped = !!cfg && !cfg.enabled;
          const wl = cfg ? weightLabel(cfg.weightCode) : null;
          return (
            <figure key={s.id} className={"vp-tile" + (isCurrent ? " vp-tile--live" : "")
              + (skipped ? " vp-tile--skipped" : "")}>
              <figcaption className="vp-tile__head">
                <span className="vp-tile__id">{s.id}</span>
                <span className="vp-tile__title">{s.title}</span>
                <span className="vp-tile__theme">{THEMES[s.theme].name}</span>
                {cfg && cfg.enabled && wl && wl !== "1×" && (
                  <span className="vp-tile__badge vp-tile__badge--wt" title="Dwell weight the panel reports">{wl}</span>
                )}
                {skipped && <span className="vp-tile__badge vp-tile__badge--off">SKIPPED</span>}
                {isCurrent && <span className="vp-tile__badge">ON PANEL</span>}
              </figcaption>
              <div className="vp-tile__screen">
                <canvas
                  ref={function (el) { canvasRefs.current[i] = el; }}
                  width={TILE_W} height={TILE_H}
                  className="vp-canvas"
                  role="img"
                  aria-label={s.id + " — " + s.title + ", simulated 53 by 11 LED matrix"}
                />
                {isCurrent && sleeping && (
                  <div className="vp-tile__sleep"><span>ASLEEP</span></div>
                )}
              </div>
              {send && (
                <ScreenTileCfg idx={i} cfg={cfg} pend={pend || {}} send={send} disabled={ctlDisabled} />
              )}
            </figure>
          );
        })}
      </div>
    );
  }

  /* Reference treatments. LINK LOST is a panel-side condition (CONTRACT §4:
     more than 15 s without a valid frame) which the dashboard cannot observe
     — the server only knows when the panel last POSTed STATE, which is not the
     same fact. Rather than infer it, both firmware treatments are rendered
     here as static reference so every paint path in panelInlay.c is visible
     and reviewable on this page. */
  function TreatmentStrip() {
    const refs = useRef([]);
    useEffect(function () {
      const fb = fbNew();
      fbUse(fb); fbClear(); screenLinkLost(FIXTURE_T);
      paintCanvas(refs.current[0], fb, 1);
      fbUse(fb); fbClear(); screenNoData(FIXTURE_T);
      paintCanvas(refs.current[1], fb, 1);
    }, []);
    const items = [
      { id: "LINK LOST", note: "panelInlay.c:76 — no valid frame for 15 s" },
      { id: "NO DATA", note: "panelInlay.c:90 — zero-node fleet, or pre-first-snapshot" },
    ];
    return (
      <div className="vp-strip">
        {items.map(function (it, i) {
          return (
            <figure key={it.id} className="vp-tile vp-tile--ref">
              <figcaption className="vp-tile__head">
                <span className="vp-tile__id">{it.id}</span>
                <span className="vp-tile__title">{it.note}</span>
              </figcaption>
              <div className="vp-tile__screen">
                <canvas ref={function (el) { refs.current[i] = el; }}
                  width={TILE_W} height={TILE_H} className="vp-canvas"
                  role="img" aria-label={it.id + " treatment, simulated LED matrix"} />
              </div>
            </figure>
          );
        })}
      </div>
    );
  }

  // =====================================================================
  // 15. Controls (CONTRACT-CP §7)
  // =====================================================================
  function CmdState({ p }) {
    if (!p) return null;
    if (p.status === "sending" || p.status === "pending") {
      return <span className="vp-cmd vp-cmd--pending"><span className="vp-spin" />pending</span>;
    }
    if (p.status === "applied") {
      return <span className="vp-cmd vp-cmd--ok">applied</span>;
    }
    return <span className="vp-cmd vp-cmd--fail" title={p.reason || ""}>failed</span>;
  }

  function PanelControls({ state, pend, send, disabled, disabledNote }) {
    const [bright, setBright] = useState(state && state.brightnessPct != null ? state.brightnessPct : 70);
    const brightPend = pend.brightness && (pend.brightness.status === "sending" || pend.brightness.status === "pending");
    useEffect(function () {
      if (!brightPend && state && state.brightnessPct != null) setBright(state.brightnessPct);
    }, [state && state.brightnessPct, brightPend]);

    function busyKey(k) { const p = pend[k]; return !!p && (p.status === "sending" || p.status === "pending"); }
    const off = !!disabled;
    const dwellPend = busyKey("dwell");
    const dwellSec = num(state && state.dwellSec);          // 0 = never reported
    const dwellOther = dwellSec > 0 && [3, 6, 30].indexOf(dwellSec) < 0;
    const alarmArmed = !!(state && state.alarmArmed) && !(state && state.alarmAcked);

    return (
      <div className="panel vp-controls">
        <div className="panel__head">
          <Icon name="settings" size={16} className="ico" />
          <h3>Controls</h3>
          <span className="right">
            {off ? disabledNote : "Queued as CONTROL commands; the panel confirms by reporting a higher lastCmdId."}
          </span>
        </div>
        <div className="panel__body vp-controls__body">

          <div className="vp-ctl">
            <div className="vp-ctl__label">Theme <CmdState p={pend.theme} /></div>
            <div className="vp-ctl__row">
              {THEMES.map(function (th, i) {
                const on = state && state.theme === i;
                return (
                  <button key={th.key} type="button"
                    className={"chip" + (on ? " on" : "")}
                    disabled={off || busyKey("theme")}
                    onClick={function () { send(CMD_KIND.theme, i); }}>
                    {th.key} · {th.name}
                  </button>
                );
              })}
            </div>
          </div>

          <div className="vp-ctl">
            <div className="vp-ctl__label">Screen <CmdState p={pend.screen} /></div>
            <div className="vp-ctl__row">
              {[0, 1, 2].map(function (n) {
                const on = state && state.screen === n;
                const title = state ? (SCREENS[(state.theme || 0) * 3 + n] || {}).title : "";
                return (
                  <button key={n} type="button"
                    className={"chip" + (on ? " on" : "")}
                    disabled={off || busyKey("screen")}
                    onClick={function () { send(CMD_KIND.screen, n); }}>
                    {n} · {title || ("Screen " + n)}
                  </button>
                );
              })}
            </div>
          </div>

          <div className="vp-ctl">
            <div className="vp-ctl__label">
              Brightness <CmdState p={pend.brightness} />
              <span className="vp-ctl__val">{bright}%{state && state.autoBright ? " · AUTO" : " · MANUAL"}</span>
            </div>
            <div className="vp-ctl__row">
              <input type="range" min="0" max="100" step="1" value={bright}
                className="vp-slider"
                disabled={off || busyKey("brightness")}
                onChange={function (e) { setBright(Number(e.target.value)); }}
                onMouseUp={function (e) { send(CMD_KIND.brightness, Number(e.target.value)); }}
                onTouchEnd={function (e) { send(CMD_KIND.brightness, Number(e.target.value)); }}
                onKeyUp={function (e) { if (e.key === "Enter") send(CMD_KIND.brightness, Number(e.target.value)); }} />
              <button type="button"
                className={"chip" + (state && state.autoBright ? " on" : "")}
                disabled={off || busyKey("brightness")}
                onClick={function () { send(CMD_KIND.autoBright, 0); }}>AUTO</button>
            </div>
            <div className="vp-ctl__note">
              A manual level latches exactly like the physical LUX± buttons; AUTO re-enables the sensor.
              The driver floors at 25% (panelHw.cpp:70), so the panel dims down but never out.
            </div>
          </div>

          <div className="vp-ctl">
            <div className="vp-ctl__label">Alarm <CmdState p={pend.ack} /></div>
            <div className="vp-ctl__row">
              <button type="button" className="btn-danger"
                disabled={off || !alarmArmed || busyKey("ack")}
                onClick={function () { send(CMD_KIND.ack, 0); }}>
                <Icon name="bell" size={14} /> Acknowledge
              </button>
              <span className="vp-ctl__val">
                {!state ? "panel has not reported"
                  : alarmArmed ? "armed — unacknowledged episode"
                    : state.alarmArmed ? "acknowledged (episode " + (state.ackedEpisodeId || 0) + ")"
                      : "not armed"}
              </span>
            </div>
          </div>

          {/* Dwell confirmation reads panelState.dwellSec (u8 seconds; 0 =
              unreported). A selection means the PANEL said so, never that we
              sent it: while a dwell command is in flight nothing is marked
              selected, so the old value cannot be mistaken for the new one
              landing, and 0 stays neutral rather than pretending to a default. */}
          <div className="vp-ctl">
            <div className="vp-ctl__label">Dwell <CmdState p={pend.dwell} /></div>
            <div className="vp-ctl__row">
              {[3, 6, 30].map(function (n) {
                const on = !dwellPend && dwellSec === n;
                return (
                  <button key={n} type="button" className={"chip" + (on ? " on" : "")}
                    disabled={off || busyKey("dwell")}
                    onClick={function () { send(CMD_KIND.dwell, n); }}>{n}s</button>
                );
              })}
            </div>
            <div className="vp-ctl__note">
              {dwellPend
                ? "Waiting for the panel to report the new dwell."
                : dwellSec === 0
                  ? "The panel has not reported a dwell yet, so no interval is shown as in force."
                  : dwellOther
                    ? "The panel reports " + dwellSec + " s, which is not one of the presets — none is marked selected."
                    : "The panel reports " + dwellSec + " s in force."}
            </div>
          </div>

          <div className="vp-ctl">
            <div className="vp-ctl__label">Power <CmdState p={pend.sleep} /></div>
            <div className="vp-ctl__row">
              <button type="button" className={"chip" + (state && !state.sleeping ? " on" : "")}
                disabled={off || busyKey("sleep")}
                onClick={function () { send(CMD_KIND.sleep, 0); }}>Wake</button>
              <button type="button" className={"chip" + (state && state.sleeping ? " on" : "")}
                disabled={off || busyKey("sleep")}
                onClick={function () { send(CMD_KIND.sleep, 1); }}>Sleep</button>
              <span className="vp-ctl__val">
                {state ? (state.sleeping ? "asleep" : "awake") : "unknown"}
              </span>
            </div>
          </div>

          {Object.keys(pend).map(function (k) {
            const p = pend[k];
            if (!p || p.status !== "failed") return null;
            return (
              <div key={k} className="vp-fail">
                <Icon name="alerts" size={14} />
                <span><strong>{cmdLabel(p.kind, p.arg)}</strong> did not apply — {p.reason}</span>
              </div>
            );
          })}
        </div>
      </div>
    );
  }

  // =====================================================================
  // 16. The page
  // =====================================================================
  function PanelScreen() {
    const search = typeof window.location !== "undefined" ? window.location.search : "";
    const params = new URLSearchParams(search);
    const fixtureMode = params.get("fixture") === "1";

    const [samples, setSamples] = useState([]);        // chronological payloads
    const [err, setErr] = useState(null);
    const [role, setRole] = useState(undefined);       // undefined = not resolved yet
    const [pend, setPend] = useState({});
    const [fixDoc, setFixDoc] = useState(null);
    const [fixScenario, setFixScenario] = useState(null);
    const [cadenceMs, setCadenceMs] = useState(CADENCE_MS_DEFAULT);
    const [now, setNow] = useState(Date.now());
    const pendRef = useRef({});
    pendRef.current = pend;
    /* The live poll's tick, exposed so a successful command POST can nudge an
       early reconcile instead of waiting out the 5 s interval. */
    const tickRef = useRef(null);

    // ---- fixture mode: load the committed snapshot, no API traffic --------
    useEffect(function () {
      if (!fixtureMode) return undefined;
      let alive = true;
      let i = 0;
      function attempt() {
        if (!alive) return;
        if (i >= FIXTURE_PATHS.length) {
          setErr("Parity fixture not found. Deploy status-panel/fixtures/panel-snapshot.json to "
            + FIXTURE_PATHS[0] + " under the dashboard docroot.");
          return;
        }
        const path = FIXTURE_PATHS[i++];
        window.fetch(path, { credentials: "same-origin" })
          .then(function (r) { if (!r.ok) throw new Error("HTTP " + r.status); return r.json(); })
          .then(function (doc) {
            if (!alive) return;
            setFixDoc(doc);
            setCadenceMs((Number(doc.cadenceSec) || 5) * 1000);
            setFixScenario(doc.defaultScenario || (doc.scenarios && doc.scenarios[0] && doc.scenarios[0].id));
          })
          .catch(function () { attempt(); });
      }
      attempt();
      return function () { alive = false; };
    }, [fixtureMode]);

    /* ---- live mode: whoami (fail closed) --------------------------------
       The ONLY path that can set a controlling role is a resolved
       /api/auth/whoami response. There is deliberately no seed, hint or
       pre-resolution promotion from window.SOLARI or any other already-loaded
       profile: CONTRACT-CP §10 requires read-only under EVERY whoami outcome
       including "not answered yet", and a seed would open a window in which
       the controls are live before the server has confirmed anything. */
    useEffect(function () {
      if (fixtureMode) { setRole(null); return undefined; }
      let alive = true;
      const A = api();
      setRole(undefined);                    // unresolved ⇒ read-only
      if (!A || !A.get) { setRole(null); return undefined; }
      A.get("/api/auth/whoami")
        .then(function (w) { if (alive) setRole(roleFromProfile({ role: w && w.role })); })
        .catch(function () { if (alive) setRole(null); });
      return function () { alive = false; };
    }, [fixtureMode]);

    /* reconcile — CONTRACT-CP §10: a control shows pending until the queue row
       says applied or expired; an expired row (or no confirmation at all
       within the 120 s expiry plus a poll of slack) shows a failed state with
       its reason. Nothing here re-enables a control on a timer — only a
       terminal status does. */
    const reconcile = useCallback(function (recent) {
      const rows = Array.isArray(recent) ? recent : null;
      const prev = pendRef.current;
      const keys = Object.keys(prev);
      if (!keys.length) return;
      let changed = false;
      const next = {};
      const tNow = Date.now();
      keys.forEach(function (k) {
        const p = prev[k];
        /* A POST whose fetch never settled (Safari suspending the tab
           mid-request) sits at "sending" forever — the chip renders it the
           same as pending, so without this it is an eternal spinner. 15 s is
           far beyond any honest POST round-trip. */
        if (p.status === "sending") {
          if (tNow - p.at > 15000) {
            next[k] = Object.assign({}, p, {
              status: "failed",
              reason: "the request never completed (connection dropped mid-request) — try again",
            });
            changed = true;
          } else next[k] = p;
          return;
        }
        if (p.status !== "pending") { next[k] = p; return; }
        let row = null;
        if (rows && p.cmdId != null) {
          for (let i = 0; i < rows.length; i++) if (Number(rows[i].id) === p.cmdId) { row = rows[i]; break; }
        }
        if (row && row.status === "applied") {
          next[k] = Object.assign({}, p, { status: "applied" }); changed = true; return;
        }
        if (row && row.status === "expired") {
          next[k] = Object.assign({}, p, {
            status: "failed",
            reason: row.failureReason || "expired after " + (CMD_EXPIRY_MS / 1000)
              + " s without the panel collecting it (panel offline, or the daemon is not polling)",
          });
          changed = true; return;
        }
        if (tNow - p.at > CMD_GIVEUP_MS) {
          next[k] = Object.assign({}, p, {
            status: "failed",
            reason: "no confirmation from the panel within " + Math.round(CMD_GIVEUP_MS / 1000) + " s",
          });
          changed = true; return;
        }
        next[k] = p;
      });
      if (changed) setPend(next);
    }, []);

    // ---- live mode: poll /api/panel --------------------------------------
    useEffect(function () {
      if (fixtureMode) return undefined;
      let alive = true;
      let timer = 0;
      /* One request at a time — but with a staleness cap, not a boolean.
         api() has no fetch timeout, and mobile Safari suspending a tab
         mid-fetch can leave a promise that NEVER settles; a plain boolean
         guard then wedges the poll permanently (observed live: commands
         applied server-side in seconds while the page showed pending
         forever). After 15 s an open request is presumed dead and a new one
         starts; reqSeq makes sure a zombie that settles late is discarded
         rather than clobbering newer data. */
      let inFlightAt = 0;        // epoch ms of the open request, 0 = none
      let reqSeq = 0;
      let lastMs = -Infinity;    // newest server ts APPLIED to the rings
      function tick() {
        const A = api();
        if (!A || !A.get) { setErr("The dashboard API client is unavailable."); return; }
        const tStart = Date.now();
        if (inFlightAt && tStart - inFlightAt < 15000) return;  // open and young
        inFlightAt = tStart;
        const my = ++reqSeq;
        A.get("/api/panel").then(function (d) {
          if (my !== reqSeq) return;         // superseded: a zombie, drop it
          inFlightAt = 0;
          if (!alive || !d) return;
          setErr(null);
          /* Monotonic on the SERVER ts, not on arrival order. A response that
             completes late (network reordering, or a slow request finishing
             after a newer one) carries an older or equal ts and is dropped
             outright — appending it would push stale data into the history
             rings and visibly regress the fleet state. */
          const ms = sampleMs(d);
          if (ms < lastMs) return;              // reordered: nothing here is current
          if (ms > lastMs) {                    // strictly newer: this one extends the rings
            lastMs = ms;
            setSamples(function (prev) {
              const next = prev.concat([d]);
              return next.length > 240 ? next.slice(next.length - 240) : next;
            });
          }
          /* An equal ts (the collector has not produced a new snapshot yet)
             adds no history, but its recentCommands are still current, so the
             command lifecycle keeps reconciling. */
          reconcile(d.recentCommands);
        }).catch(function (e) {
          if (my !== reqSeq) return;
          inFlightAt = 0;
          if (alive) setErr((e && e.message) || "Could not reach /api/panel.");
        });
      }
      tickRef.current = tick;
      tick();
      timer = window.setInterval(tick, POLL_MS);
      /* Coming back to a suspended tab: poll immediately instead of waiting
         out the interval (and, via the staleness cap, instead of never). */
      function onVis() { if (!document.hidden) tick(); }
      document.addEventListener("visibilitychange", onVis);
      return function () {
        alive = false; tickRef.current = null;
        window.clearInterval(timer);
        document.removeEventListener("visibilitychange", onVis);
      };
    }, [fixtureMode]);

    // ---- a slow clock so staleness ages visibly --------------------------
    useEffect(function () {
      if (fixtureMode) return undefined;
      const id = window.setInterval(function () { setNow(Date.now()); }, 1000);
      return function () { window.clearInterval(id); };
    }, [fixtureMode]);

    const send = useCallback(function (kind, arg) {
      const key = cmdGroup(kind, arg);
      const A = api();
      /* `stamp` identifies THIS attempt: the timeout and both settle paths
         only touch the pend entry if it is still this attempt's (a newer
         click replaces the entry and takes over the group). */
      const stamp = Date.now();
      setPend(function (p) {
        const n = Object.assign({}, p);
        n[key] = { kind: kind, arg: arg, status: "sending", at: stamp };
        return n;
      });
      if (!A || !A.post) {
        setPend(function (p) {
          const n = Object.assign({}, p);
          n[key] = { kind: kind, arg: arg, status: "failed", at: stamp, reason: "the dashboard API client is unavailable" };
          return n;
        });
        return;
      }
      /* api()'s fetch has no timeout; a request Safari abandons on tab
         suspend would otherwise leave "sending" forever. If the response
         does arrive after this fires, the settle paths below still run and
         self-correct the state (the server DID accept the command). */
      const giveUp = window.setTimeout(function () {
        setPend(function (p) {
          const cur = p[key];
          if (!cur || cur.status !== "sending" || cur.at !== stamp) return p;
          const n = Object.assign({}, p);
          n[key] = Object.assign({}, cur, {
            status: "failed",
            reason: "no response after 12 s (connection may have dropped) — try again",
          });
          return n;
        });
      }, 12000);
      A.post("/api/panel/command", { kind: kind, arg: arg }).then(function (d) {
        window.clearTimeout(giveUp);
        setPend(function (p) {
          const cur = p[key];
          if (cur && cur.at !== stamp) return p;      // superseded by a newer click
          const n = Object.assign({}, p);
          n[key] = { kind: kind, arg: arg, status: "pending", at: stamp, cmdId: Number(d && d.cmdId) };
          return n;
        });
        /* The panel typically applies within a second or two; poll early so
           the confirmation shows promptly instead of on the next 5 s slot. */
        window.setTimeout(function () { if (tickRef.current) tickRef.current(); }, 1500);
      }).catch(function (e) {
        window.clearTimeout(giveUp);
        setPend(function (p) {
          const cur = p[key];
          if (cur && cur.at !== stamp) return p;      // superseded by a newer click
          const n = Object.assign({}, p);
          n[key] = { kind: kind, arg: arg, status: "failed", at: stamp, reason: (e && e.message) || "the request was rejected" };
          return n;
        });
      });
    }, []);

    // ---- resolve the working sample set ----------------------------------
    const scenario = useMemo(function () {
      if (!fixDoc || !Array.isArray(fixDoc.scenarios)) return null;
      for (let i = 0; i < fixDoc.scenarios.length; i++)
        if (fixDoc.scenarios[i].id === fixScenario) return fixDoc.scenarios[i];
      return fixDoc.scenarios[0] || null;
    }, [fixDoc, fixScenario]);

    const working = fixtureMode ? (scenario ? scenario.history : []) : samples;
    const head = working.length ? working[working.length - 1] : null;

    const env = useMemo(function () {
      if (!working.length) return null;
      return buildEnv(working, cadenceMs);
    }, [working, cadenceMs]);

    const pstate = head ? head.panelState : null;
    /* §3.3 serves panelScreenConfig read-only at the top of the GET payload.
       panelState is also checked because AW1's packet is not landed yet and a
       singleton is as likely to be nested there; both resolve through the same
       decoder, and anything unrecognised stays null — unknown, not defaulted. */
    const screenCfg = useMemo(function () {
      if (!head) return null;
      return readScreenCfg(head.panelScreenConfig)
        || readScreenCfg(pstate && pstate.panelScreenConfig)
        || readScreenCfg(pstate && pstate.screenCfg);
    }, [head, pstate]);
    /* §10 D3 cfgPersisted — reported and survives-a-power-cycle are separate
       claims. Shown only on a positive report; see readCfgPersisted. */
    const cfgSaved = useMemo(function () {
      if (!head) return false;
      return readCfgPersisted(head.panelScreenConfig)
        || readCfgPersisted(pstate && pstate.panelScreenConfig);
    }, [head, pstate]);
    const armed = !!(env && env.alarmActive && !(pstate && pstate.alarmAcked
      && Number(pstate.ackedEpisodeId) === Number(env.episodeId)));
    const currentIdx = pstate ? (num(pstate.theme) * 3 + num(pstate.screen)) : -1;
    const gain = driverGain(pstate ? pstate.brightnessPct : 100);

    const reportedAt = pstate ? parseReportedAt(pstate.reportedAt) : null;
    const refAge = fixtureMode && head ? (sampleMs(head) - (reportedAt || 0)) : (reportedAt ? now - reportedAt : null);
    const stale = refAge != null && refAge > 90000;

    /* A one-line read of the rotation the panel reports: how many screens it is
       skipping and how far the dwell weights spread the cycle. `unknown` is
       counted separately and never folded into "on". */
    const cfgSummary = useMemo(function () {
      if (!screenCfg) return null;
      let on = 0, offN = 0, unknown = 0, weighted = 0;
      for (let i = 0; i < N_SCREENS; i++) {
        const c = screenCfg[i];
        if (!c) { unknown++; continue; }
        if (c.enabled) { on++; if (c.weightCode !== 2) weighted++; } else offN++;
      }
      const bits = [on + " of " + N_SCREENS + " screens in rotation"];
      if (offN) bits.push(offN + " skipped");
      if (weighted) bits.push(weighted + " weighted");
      if (unknown) bits.push(unknown + " unreported");
      return bits.join(" · ");
    }, [screenCfg]);

    const canControl = !fixtureMode && (role === "operator" || role === "admin");
    const disabledNote = fixtureMode
      ? "Parity mode renders the committed fixture with no session and no API traffic — controls are inert here by design."
      : role === undefined ? "Confirming your role…"
        : role === null ? "Your role could not be confirmed, so this page is read-only."
          : "Your role is " + role + ". Panel control requires operator or admin.";

    // ---- render ----------------------------------------------------------
    return (
      <div className="page vp-page">
        <div className="page-head">
          <div>
            <h2 className="page-title">Panel</h2>
            <div className="page-sub">
              All twelve status-panel screens, simulated from the same telemetry the physical panel receives.
            </div>
          </div>
          <div className="page-head__right">
            {fixtureMode && <span className="vp-chip vp-chip--warn">Parity fixture</span>}
            {!fixtureMode && !canControl && <span className="vp-chip vp-chip--muted">Read-only</span>}
          </div>
        </div>

        {err && (
          <div className="panel vp-err">
            <div className="panel__body"><Icon name="alerts" size={16} /> {err}</div>
          </div>
        )}

        {fixtureMode && fixDoc && (
          <div className="panel vp-scenarios">
            <div className="panel__body">
              <div className="filters">
                {fixDoc.scenarios.map(function (s) {
                  return (
                    <button key={s.id} type="button"
                      className={"chip" + (scenario && scenario.id === s.id ? " on" : "")}
                      onClick={function () { setFixScenario(s.id); }}>{s.label || s.id}</button>
                  );
                })}
              </div>
              {scenario && <div className="vp-scenarios__note">{scenario.note}</div>}
            </div>
          </div>
        )}

        <div className="vp-chips">
          {!pstate && <span className="vp-chip vp-chip--muted">Panel has never reported</span>}
          {pstate && (
            <React.Fragment>
              <span className="vp-chip">
                {THEMES[num(pstate.theme)] ? THEMES[num(pstate.theme)].key : "?"}
                {num(pstate.screen)} · {(SCREENS[currentIdx] || {}).title || "unknown screen"}
              </span>
              <span className="vp-chip">{num(pstate.brightnessPct)}% · {pstate.autoBright ? "AUTO" : "MANUAL"}</span>
              <span className={"vp-chip" + (pstate.sleeping ? " vp-chip--muted" : "")}>
                {pstate.sleeping ? "Asleep" : "Awake"}
              </span>
              {armed && <span className="vp-chip vp-chip--crit">Alarm armed · episode {env ? env.episodeId : 0}</span>}
              {!armed && pstate.alarmArmed && <span className="vp-chip vp-chip--warn">Alarm acknowledged</span>}
              {stale && <span className="vp-chip vp-chip--warn">Panel state {fmtAge(refAge)} old</span>}
            </React.Fragment>
          )}
          {env && env.dataStale && <span className="vp-chip vp-chip--warn">Upstream data stale</span>}
          {env && env.gaps > 0 && <span className="vp-chip vp-chip--muted">{env.gaps} history gap{env.gaps === 1 ? "" : "s"}</span>}
          {env && <span className="vp-chip vp-chip--muted">{env.histFill}/{HIST_LEN} history slots</span>}
          {env && (
            <span className={"vp-chip" + (env.gearCount ? " vp-chip--muted" : " vp-chip--warn")}>
              {env.gearCount
                ? env.gearCount + " network device" + (env.gearCount === 1 ? "" : "s") + " on A1"
                : "No gear data — A1 shows NO DATA"}
            </span>
          )}
          {pstate && !screenCfg && (
            <span className="vp-chip vp-chip--muted">Screen config not reported</span>
          )}
          {screenCfg && cfgSummary && <span className="vp-chip vp-chip--muted">{cfgSummary}</span>}
          {cfgSaved && <span className="vp-chip vp-chip--muted">Saved to panel flash</span>}
        </div>

        {!env && !err && <div className="empty">Waiting for the first /api/panel sample…</div>}

        {env && (
          <VirtualPanel
            env={env}
            armed={armed}
            gain={gain}
            currentIdx={currentIdx}
            sleeping={!!(pstate && pstate.sleeping)}
            still={fixtureMode}
            screenCfg={screenCfg}
            pend={pend}
            send={send}
            ctlDisabled={!canControl}
          />
        )}

        {env && <TreatmentStrip />}

        <p className="vp-footnote">
          The simulation is a <strong>~5&nbsp;s cadence approximation</strong>: the panel is driven at the
          firmware's own snapshot rate, while this page resamples the 5&nbsp;s dashboard poll onto the same
          53-sample rings (gap-aware, on the server timestamp). Ribbons, histograms and traces will therefore
          differ in detail from the physical panel even when the fleet data is identical. The sleep state is
          shown as an overlay on the on-panel tile rather than blanking every screen, so the other eleven stay
          previewable. Colours reproduce the panel and are exempt from the Interface Guide palette.
        </p>

        <p className="vp-footnote">
          <strong>Rotation and dwell</strong> are set per screen on the tile itself. A weight multiplies the
          theme&rsquo;s base dwell, so at a 30&nbsp;s base ½× is 15&nbsp;s and 5× is 150&nbsp;s. The panel stores
          this in its own flash and keeps it across resets, which is why a control here shows a setting as in
          force <em>only</em> once the panel has reported it back &mdash; before that, and while a change is in
          flight, no option is marked. Turning every screen of a theme off is not a way to blank the panel: the
          firmware treats an all-disabled theme as all-enabled rather than showing a black wall.
          <strong> A1</strong> draws the live Ubiquiti path &mdash; access points and hubs on the left feeding
          the switch and router, the internet in the rightmost column &mdash; from the gear section of the
          snapshot; with no gear data it shows NO DATA rather than an invented topology.
        </p>

        <PanelControls
          state={pstate}
          pend={pend}
          send={send}
          disabled={!canControl}
          disabledNote={disabledNote}
        />
      </div>
    );
  }

  Object.assign(window, { PanelScreen: PanelScreen });
})();
