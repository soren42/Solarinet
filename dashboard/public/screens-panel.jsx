/*
 * screens-panel.jsx — Panel control page (window.PanelScreen).
 *
 * CONTRACT-V2: sixteen firmware screens, universal treatments and controls.
 * Renders all 4 themes x 4 screens of the physical Galactic Unicorn status
 * panel simultaneously as 53x11 virtual LED matrices, plus the panel software
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
 * PARITY MODE (§10): ?fixture=1 renders status-panel/fixtures/parity-fixture.json
 * deterministically with no API traffic and no animation clock, so two
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
  // 4. Small numeric helpers that mirror C semantics exactly
  // =====================================================================
  function clamp01(v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
  function ri(v) { return Math.floor(v + 0.5); }        // (int)(v + 0.5f), v >= 0
  function trunc(v) { return v < 0 ? Math.ceil(v) : Math.floor(v); } // (int) cast
  function num(v) { const n = Number(v); return isFinite(n) ? n : 0; }
  function f1(v) { return v.toFixed(1); }               // "%.1f"

  const STATE_ENUM = { ok: ST_OK, up: ST_OK, degraded: ST_DEGRADED, down: ST_DOWN, unknown: ST_UNKNOWN, maint: ST_MAINT };
  function stateEnum(s) {
    if (typeof s === "number") return (s >= 0 && s <= 4) ? s : ST_UNKNOWN;
    const v = STATE_ENUM[String(s || "").toLowerCase()];
    return v === undefined ? ST_UNKNOWN : v;
  }

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

  function buildEnv(samples, cadenceMs) {
    const d = samples[samples.length - 1];
    const env = {};
    const roll = d.stateRoll || {};

    env.haveData = true;
    env.raw = d;
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

    const rings = resampleRings(samples, cadenceMs);
    env.histFill = rings.histFill;
    env.gaps = rings.gaps;
    // Firmware-local rings replay snapshots in arrival order, oldest first.
    env.wanDown = new Float32Array(HIST_LEN).fill(-1); env.wanUp = new Float32Array(HIST_LEN).fill(-1);
    env.lanDown = new Float32Array(HIST_LEN).fill(-1); env.lanUp = new Float32Array(HIST_LEN).fill(-1);
    env.probeHist = Array.from({length: 11}, () => new Float32Array(HIST_LEN).fill(-1));
    env.cpuLocal = new Float32Array(HIST_LEN).fill(-1); env.rttLocal = new Float32Array(HIST_LEN).fill(-1);
    for (let si=0; si<samples.length; si++) {
      const q=samples[si], totals=q.totals;
      [env.wanDown,env.wanUp,env.lanDown,env.lanUp].forEach(a=>{a.copyWithin(0,1);a[HIST_LEN-1]=-1;});
  if (totals) {
    env.wanDown[HIST_LEN - 1] = totals.wanDownKbps == null ? -1 : num(totals.wanDownKbps);
    env.wanUp[HIST_LEN - 1] = totals.wanUpKbps == null ? -1 : num(totals.wanUpKbps);
    env.lanDown[HIST_LEN - 1] = totals.lanDownKbps == null ? -1 : num(totals.lanDownKbps);
    env.lanUp[HIST_LEN - 1] = totals.lanUpKbps == null ? -1 : num(totals.lanUpKbps);
  }
      env.cpuLocal.copyWithin(0,1); env.cpuLocal[HIST_LEN-1]=num(q.meanLoadPct)/100;
      env.rttLocal.copyWithin(0,1); env.rttLocal[HIST_LEN-1]=num(q.lossPermille)>=1000?-1:num(q.rttTenthMs)/10;
  for (let pi = 0; pi < 11; pi++) {
    const a = env.probeHist[pi];
    a.copyWithin(0, 1);
    a[HIST_LEN - 1] = -1;
    const probe = q.probes && q.probes[pi];
    if (probe) {
      a[HIST_LEN - 1] = num(probe.lossPermille) >= 500 ? 65535 : num(probe.rttTenthMs);
    }
  }
    }

    // Alert text — panelHist.c:261-272.
    const ta = d.topAlert;
    env.hasAlert = !!ta;
    env.subject = ta ? String(ta.subject || "") : "ALL NOMINAL";
    env.detail = ta ? String(ta.detail || "") : "NO FINDINGS";
    return env;
  }

  // V2 renderers, aligned with panelScreensA-D.c.
  function panelRateLevel(v){return v<=1?0:Math.min(1,Math.log10(v)/6);}
  function textFit(x,y,s,n,c,b){s=String(s).slice(0,n);while(s.endsWith(" "))s=s.slice(0,-1);panelText(x,y,s,c,b);}
  function textRight(x,y,s,c,b){panelText(x-textW(String(s))+1,y,String(s),c,b);}
  function textCentre(y,s,c,b){s=String(s).slice(0,13);panelText(Math.floor((PW-textW(s))/2),y,s,c,b);}
  const v2PageClock=new Float32Array(16),v2LanePhase=[];
  function pageFor(i,dt,p){v2PageClock[i]=(v2PageClock[i]+dt)%(3*256);return Math.floor(v2PageClock[i]/3)%Math.max(1,p);}
  function pageBar(p,n){if(n<=1)return;for(let x=Math.floor(p*PW/n);x<Math.floor((p+1)*PW/n);x++)fbSet(x,5,cAzure,.45);}
  function stateColor(s){return [cOk,cWarn,cCrit,cUnknown,cMaint][stateEnum(s)]||cUnknown;}
  function pageCount(n,k){return Math.max(1,Math.ceil(n/k));}
  function sA0(env) {
    const a = (env.raw.systems || []).map(q => ({
      state: stateEnum(q.state),
      load: num(q.loadPct) / 100
    }));
    for (const b of env.raw.popTail || []) a.push({
      state: num(b) >> 5 & 7,
      load: (num(b) & 31) / 31
    });
    let n = a.length;
    if (!n) return;
    let cols = 1,
      rows = 1,
      gx = 0,
      gy = 0,
      ba = -1,
      bg = -1;
    for (let r = 1; r <= PH; r++) {
      let c = Math.ceil(n / r);
      if (c > PW) continue;
      for (let g = 0; g < 4; g++) {
        let ux = g & 1,
          uy = g >> 1,
          tw = Math.floor((PW - (c - 1) * ux) / c),
          th = Math.floor((PH - (r - 1) * uy) / r);
        if (tw < 1 || th < 1) continue;
        let ar = tw * th,
          gu = ux + uy;
        if (ar > ba || ar === ba && gu > bg) {
          ba = ar;
          bg = gu;
          cols = c;
          rows = r;
          gx = ux;
          gy = uy;
        }
      }
    }
    for (let i = 0; i < n; i++) {
      let q = a[i],
        col = i % cols,
        row = Math.floor(i / cols),
        x0 = Math.floor(col * (PW + gx) / cols),
        x1 = Math.floor((col + 1) * (PW + gx) / cols) - gx,
        y0 = Math.floor(row * (PH + gy) / rows),
        y1 = Math.floor((row + 1) * (PH + gy) / rows) - gy,
        h = y1 - y0,
        H = stateColor(q.state),
        flat = q.state === ST_DOWN || q.state === ST_UNKNOWN || q.state === ST_MAINT;
      if (flat) {
        let b = q.state === ST_DOWN ? 1 : .35;
        for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) fbSet(x, y, H, b);
      } else if (h >= 3) {
        let lit = Math.max(1, ri(q.load * h));
        for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) fbSet(x, y, H, y >= y1 - lit ? 1 : .28);
      } else for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) fbSet(x, y, H, .2 + .8 * q.load);
    }
  }
  function sA1(env, t, dt) {
    let a = env.raw.systems || [],
      p = pageFor(1, dt, pageCount(a.length, 2));
    for (let j = 0; j < 2; j++) {
      let q = a[p * 2 + j];
      if (!q) continue;
      let y = j ? 6 : 0,
        h = stateColor(q.state),
        st = stateEnum(q.state);
      for (let r = 0; r < 5; r++) {
        fbSet(0, y + r, h, 1);
        fbSet(1, y + r, h, 1);
      }
      textFit(3, y, q.name, 8, cInk, 1);
      textRight(52, y, st === ST_DOWN ? "DOWN" : st === ST_MAINT ? "MNT" : st === ST_UNKNOWN ? "?" : num(q.loadPct) + "%", st === ST_OK ? cAzure : h, 1);
    }
    pageBar(p, pageCount(a.length, 2));
  }
  function sA2(env, t, dt) {
    let a = env.raw.pools || [],
      p = pageFor(2, dt, pageCount(a.length, 2));
    for (let j = 0; j < 2; j++) {
      let q = a[p * 2 + j];
      if (!q) continue;
      let y = j ? 6 : 0;
      textFit(0, y, q.name, 8, cInk, 1);
      if (num(q.total) === 0) continue;
      let order = [ST_DOWN, ST_DEGRADED, ST_UNKNOWN, ST_MAINT, ST_OK],
        counts = [q.down, q.degraded, q.unknown, q.maint, q.up],
        total = num(q.total) || 1,
        cells = Math.min(20, total),
        gap = total <= 10 ? 1 : 0,
        run = 0,
        k = 0;
      for (let x = 0; x < cells; x++) {
        while (k < 4 && run >= num(counts[k])) {
          k++;
          run = 0;
        }
        let x0 = 33 + Math.floor(x * (20 + gap) / cells),
          x1 = 33 + Math.floor((x + 1) * (20 + gap) / cells) - gap,
          used = Math.floor((x + 1) * total / cells) - Math.floor(x * total / cells);
        for (let r = 0; r < 3; r++) for (let xx = x0; xx < x1; xx++) fbSet(xx, y + r, stateColor(order[k]), 1);
        run += used;
      }
      let lit = Math.max(num(q.loadPct) > 0 ? 1 : 0, Math.floor((num(q.loadPct) * 20 + 50) / 100));
      for (let x = 0; x < 20; x++) fbSet(33 + x, y + 4, cAzure, x < lit ? 1 : .1);
    }
    pageBar(p, pageCount(a.length, 2));
  }
  function sA3(env) {
    let r = env.raw.stateRoll || {},
      up = num(r.up),
      d = num(r.down),
      g = num(r.degraded),
      u = num(r.unknown),
      m = num(r.maint),
      h = d ? cCrit : g ? cWarn : u ? cUnknown : m ? cMaint : cOk,
      line = d ? d + " DOWN" : g ? g + " DEGRADED" : u ? u + " UNKNOWN" : m ? m + " MAINT" : "ALL OK",
      x = panelBig(0, 2, String(up), h, 1) + 1;
    textFit(x, 0, "OF " + env.total, Math.floor((PW - x + 1) / 4), cInk, 1);
    textFit(x, 6, line, Math.floor((PW - x + 1) / 4), h, 1);
  }
  function linkKind(k){if(typeof k==="string")return ({wan:0,trunk:1,wifi:2,host:3})[k.toLowerCase()]||0;return num(k);}
  function sB2(env, t, dt) {
    let a = (env.raw.systems || []).map((q, i) => ({
        q,
        i,
        r: (q.rxKbps == null ? 0 : num(q.rxKbps)) + (q.txKbps == null ? 0 : num(q.txKbps))
      })).filter(x => x.q.rxKbps != null || x.q.txKbps != null).sort((x, y) => y.r - x.r).slice(0, 8),
      p = pageFor(6, dt, pageCount(a.length, 2));
    for (let j = 0; j < 2; j++) {
      let x = a[p * 2 + j];
      if (!x) continue;
      let y = j ? 6 : 0;
      textFit(0, y, x.q.name, 8, cInk, 1);
      textRight(52, y, rateLabel(Math.min(x.r, RATE_MAX)), cAzure, 1);
    }
    pageBar(p, pageCount(a.length, 2));
  }
  function sC0(env, t, dt) {
    let a = env.raw.alertList;
    if (!a) {
      textCentre(3, "NO ALERT DATA", cAzure, 1);
      return;
    }
    let items = a.items || [];
    if (!items.length) {
      textCentre(3, "ALL CLEAR", cOk, 1);
      return;
    }
    let p = pageFor(8, dt, pageCount(items.length + (a.total > items.length ? 1 : 0), 1));
    if (p >= items.length) {
      textCentre(0, "+" + (a.total - items.length) + " MORE", cInk, 1);
      textCentre(6, "SEE DASHBOARD", cAzure, 1);
    } else {
      let q = items[p],
        age = num(q.ageMin) < 60 ? q.ageMin + "M" : num(q.ageMin) < 1440 ? Math.floor(q.ageMin / 60) + "H" : Math.floor(q.ageMin / 1440) + "D",
        h = num(q.severity) === 2 ? cCrit : num(q.severity) === 1 ? cWarn : cAzure;
      textFit(0, 0, q.host, 9, cInk, 1);
      textRight(52, 0, age, cAzure, 1);
      textCentre(6, q.msg, h, 1);
    }
    pageBar(p, items.length + (a.total > items.length ? 1 : 0));
  }
  function sC1(env, t, dt) {
    let a = env.raw.alertList;
    if (!a) {
      textCentre(3, "NO ALERT DATA", cAzure, 1);
      return;
    }
    let f = a.faultCounts || [],
      p = pageFor(9, dt, f[0] ? 2 : 1);
    if (p) {
      textCentre(0, "OTHER " + num(f[0]), cWarn, 1);
      textCentre(6, "SEE DASHBOARD", cAzure, 1);
    } else {
      [[0, 0, "NET", f[1], cCrit], [28, 0, "SVC", f[2], cWarn], [0, 6, "DSK", num(f[3]) + num(f[4]) + num(f[6]), cWarn], [28, 6, "KRN", f[5], cWarn]].forEach(([x, y, l, n, h]) => panelText(x, y, l + " " + Math.min(99, num(n)), num(n) ? h : cOk, 1));
    }
    pageBar(p, f[0] ? 2 : 1);
  }
  function sC2(env) {
    let a = env.raw.probes || [];
    if (!a.length) {
      textCentre(3, "NO PROBE DATA", cAzure, 1);
      return;
    }
    let n = Math.min(11, a.length),
      gap = n <= 6 ? 1 : 0;
    for (let p = 0; p < n; p++) {
      let y0 = Math.floor(p * (PH + gap) / n),
        y1 = Math.floor((p + 1) * (PH + gap) / n) - gap,
        row = env.probeHist[p],
        base = 65535;
      for (let x = 0; x < HIST_LEN; x++) if (row[x] > 0 && row[x] < base) base = row[x];
      for (let x = 0; x < HIST_LEN; x++) {
        let cell = row[x];
        if (cell < 0) continue;
        let h, b;
        if (stateEnum(a[p].state) === ST_UNKNOWN) {
          h = cUnknown;
          b = .25;
        } else if (cell >= 65535) {
          h = cCrit;
          b = 1;
        } else if (base !== 65535 && cell >= base * 2 + 5) {
          h = cWarn;
          b = 1;
        } else {
          h = cOk;
          b = .4;
        }
        for (let y = y0; y < y1; y++) fbSet(x, y, h, b);
      }
    }
  }
  function sC0oldUnused(){}

  const RATE_MAX = 4294967294; // One below the wire's "no rate" value, as firmware pins it.
  function rateLabel(k) {
    if (k == null) return "--";
    k = num(k);
    if (k < 1000) return Math.floor(k) + "K";
    const m = k / 1000;
    if (m < 9.95) return m.toFixed(1) + "M";
    if (m < 999.5) return Math.floor(m + .5) + "M";
    if (m < 9950) return (m / 1000).toFixed(1) + "G";
    return Math.floor(m / 1000 + .5) + "G";
  }
  function sB1(env) {
    const q = env.raw.totals;
    if (!q) {
      textCentre(3, "NO RATE DATA", cAzure, 1);
      return;
    }
    function row(y, l, d, u, dr, ur, h) {
      let total = d == null ? u == null ? null : num(u) : u == null ? num(d) : Math.min(num(d) + num(u), RATE_MAX),
        x = panelText(0, y, l + " ", cInk, 1);
      x = panelText(x, y, rateLabel(total), h, 1);
      let x0 = Math.max(33, x),
        cols = PW - x0,
        peak = 1;
      for (let a = 0; a < cols; a++) {
        let dn = dr[HIST_LEN - 1 - a],
          up = ur[HIST_LEN - 1 - a],
          v = (dn > 0 ? dn : 0) + (up > 0 ? up : 0);
        if (v > peak) peak = v;
      }
      for (let a = 0; a < cols; a++) {
        let dn = dr[HIST_LEN - 1 - a],
          up = ur[HIST_LEN - 1 - a];
        if (dn < 0 && up < 0) continue;
        if (dn < 0) dn = 0;
        if (up < 0) up = 0;
        let xx = PW - 1 - a,
          hd = ri(dn / peak * 5),
          hu = ri((dn + up) / peak * 5) - hd;
        if (hd < 1 && dn > 0) hd = 1;
        if (hu < 0) hu = 0;
        for (let r = 0; r < hd && r < 5; r++) fbSet(xx, y + 4 - r, h, 1);
        for (let r = hd; r < hd + hu && r < 5; r++) fbSet(xx, y + 4 - r, cMaint, 1);
      }
    }
    row(0, "WAN", q.wanDownKbps, q.wanUpKbps, env.wanDown, env.wanUp, cAzure);
    row(6, "LAN", q.lanDownKbps, q.lanUpKbps, env.lanDown, env.lanUp, cOk);
  }

  // Detailed firmware ports for the topology and resource charts.
  function sB3(env, t, dt) {
    const a = env.raw.links || [],
      n = a.length;
    if (!n) {
      textCentre(3, "NO LINK DATA", cAzure, 1);
      return;
    }
    function parentOf(i) {
      let p = a[i].parent;
      if (p == null || num(p) >= n) return -1;
      let at = i,
        steps = 0;
      while (a[at].parent != null && num(a[at].parent) < n) {
        at = num(a[at].parent);
        if (++steps > n) return -1;
      }
      return at === i ? -1 : num(p);
    }
    function depth(i) {
      let d = 0,
        at = i;
      while (a[at].parent != null && num(a[at].parent) < n) {
        at = num(a[at].parent);
        if (++d > n) return 0;
      }
      return d;
    }
    let par = a.map((_, i) => parentOf(i)),
      dep = a.map((_, i) => par[i] < 0 ? 0 : depth(i)),
      md = Math.max.apply(null, dep),
      order = [],
      stack = [];
    for (let i = n - 1; i >= 0; i--) if (par[i] < 0) stack.push(i);
    while (stack.length && order.length < n) {
      let at = stack.pop();
      order.push(at);
      for (let c = n - 1; c >= 0; c--) if (par[c] === at) stack.push(c);
    }
    let leaves = order.filter(i => !par.some(x => x === i)).length || 1,
      leaf = 0,
      nx = [],
      ny = [];
    for (const i of order) {
      let child = par.some(x => x === i);
      nx[i] = 5 + Math.floor(dep[i] * 45 / (md || 1));
      ny[i] = 1 + Math.floor((2 * leaf + 1) * 9 / (2 * leaves));
      if (!child) leaf++;
    }
    function point(px, py, cx, cy, d) {
      let dy = Math.abs(cy - py);
      return d <= dy ? [px, py + (cy > py ? d : -d)] : [px + d - dy, cy];
    }
    for (const i of order) {
      let px = par[i] < 0 ? 0 : nx[par[i]],
        py = par[i] < 0 ? ny[i] : ny[par[i]],
        len = Math.abs(ny[i] - py) + nx[i] - px,
        q = a[i],
        h = stateEnum(q.state) === ST_DOWN ? cCrit : [cInk, cAzure, cMaint, cOk][linkKind(q.kind)] || cOk;
      for (let d = 0; d <= len; d++) {
        let [x, y] = point(px, py, nx[i], ny[i], d);
        fbSet(x, y, h, stateEnum(q.state) === ST_DOWN ? .6 : .22);
      }
      for (let dir = 0; dir < 2; dir++) {
        let rate = q[dir ? "upKbps" : "downKbps"];
        if (rate == null || num(rate) === 0 || stateEnum(q.state) === ST_DOWN || len <= 0) continue;
        let spacing = 14 - 11 * panelRateLevel(rate),
          speed = 2 + 22 * panelRateLevel(rate),
          key = i * 2 + dir;
        v2LanePhase[16 + key] = (v2LanePhase[16 + key] || 0) + speed * dt;
        if (spacing > len) spacing = len + 1;
        for (let z = v2LanePhase[16 + key] % spacing; z <= len; z += spacing) {
          let d = Math.floor(z);
          if (dir) d = len - d;
          let [x, y] = point(px, py, nx[i], ny[i], d);
          fbSet(x, y, h, 1);
        }
      }
    }
    for (const i of order) for (let yy = 0; yy < 2; yy++) for (let xx = 0; xx < 2; xx++) fbSet(nx[i] + xx, ny[i] + yy, stateColor(a[i].state), 1);
  }
  function sC3(env) {
    const heat = env.raw.alertHeat;
    if (!heat) {
      textCentre(3, "NO HISTORY", cAzure, 1);
      return;
    }
    const w = heat.warn || [],
      c = heat.crit || [];
    let peak = 3;
    for (let i = 0; i < 52; i++) peak = Math.max(peak, num(w[i]) + num(c[i]));
    const scale = PH / Math.log2(peak + 1);
    for (let i = 0; i < 52; i++) {
      const nw = num(w[i]),
        nc = num(c[i]),
        h = Math.min(PH, Math.ceil(Math.log2(nw + nc + 1) * scale)),
        hc = nc ? Math.ceil(Math.log2(nc + 1) * scale) : 0;
      for (let r = 0; r < h; r++) fbSet(i, PH - 1 - r, r >= h - hc ? cCrit : cWarn, 1);
      if (!h && (51 - i) % 12 === 0) fbSet(i, 10, cAzure, .3);
    }
    for (let y = 0; y < PH; y++) fbSet(52, y, cAzure, .45);
  }
  function sD0(env, t, dt) {
    const a = env.raw.systems || [];
    if (!a.length) return;
    const p = pageFor(12, dt, a.length),
      q = a[p],
      st = stateEnum(q.state),
      h = stateColor(st),
      word = ["OK", "DEG", "DOWN", "?", "MNT"][st];
    textFit(0, 0, q.name, 8, cInk, 1);
    textRight(52, 0, word, h, 1);
    if (st === ST_DOWN || st === ST_UNKNOWN) textCentre(6, "NO READINGS", cUnknown, 1);else {
      [[0, "C", q.loadPct, cAzure], [19, "M", q.memPct, cMaint], [38, "D", q.diskPct, cOk]].forEach(([x, l, v, col]) => {
        panelText(x, 6, l, cInk, 1);
        const valid = v != null && num(v) <= 100,
          pct = num(v);
        panelText(x + 4, 6, valid ? String(pct) : "--", valid ? pct >= 90 ? cCrit : pct >= 75 ? cWarn : col : cUnknown, 1);
      });
    }
    pageBar(p, a.length);
  }
  function sD1(env, t, dt) {
    const a = env.raw.systems || [],
      n = Math.min(PW, a.length);
    if (!n) return;
    const gap = PW >= n * 2 ? 1 : 0;
    for (let i = 0; i < n; i++) {
      let q = a[i],
        x0 = Math.floor(i * (PW + gap) / n),
        x1 = Math.floor((i + 1) * (PW + gap) / n) - gap,
        st = stateEnum(q.state),
        h = stateColor(st),
        mem = q.memPct == null ? 255 : num(q.memPct),
        measured = st !== ST_DOWN && st !== ST_UNKNOWN,
        up = measured ? Math.floor((num(q.loadPct) * 5 + 50) / 100) : 0,
        dn = measured && mem <= 100 ? Math.floor((mem * 5 + 50) / 100) : 0;
      if (measured && up < 1 && num(q.loadPct) > 0) up = 1;
      if (dn < 1 && measured && mem <= 100 && mem > 0) dn = 1;
      for (let x = x0; x < x1; x++) {
        fbSet(x, 5, h, 1);
        for (let r = 1; r <= 5; r++) {
          if (r <= up) fbSet(x, 5 - r, num(q.loadPct) >= 90 ? cCrit : num(q.loadPct) >= 75 ? cWarn : cAzure, 1);
          if (r <= dn) fbSet(x, 5 + r, mem >= 90 ? cCrit : mem >= 75 ? cWarn : cMaint, 1);
        }
      }
    }
  }
  function sD2(env) {
    const rings = [env.wanDown, env.cpuLocal, env.rttLocal],
      rings2 = [env.wanUp, null, null],
      peaks = [1, .4, 10],
      values = Array.from({
        length: 3
      }, () => new Float32Array(PW));
    for (let a = 0; a < PW; a++) for (let k = 0; k < 3; k++) {
      let v = rings[k][HIST_LEN - 1 - a];
      if (rings2[k]) {
        let v2 = rings2[k][HIST_LEN - 1 - a];
        if (v2 > 0) v = (v > 0 ? v : 0) + v2;
      }
      values[k][a] = v;
      if (v > peaks[k]) peaks[k] = v;
    }
    for (let a = 0; a < PW; a++) for (let k = 0; k < 3; k++) {
      let v = values[k][a];
      if (v < 0) continue;
      let rows = Math.min(3, v / peaks[k] * 3),
        x = PW - 1 - a,
        y = [0, 4, 8][k],
        h = [cInk, cAzure, cMaint][k];
      for (let r = 0; r < 3; r++) {
        let cover = rows - r;
        if (cover <= 0) break;
        fbSet(x, y + 2 - r, h, cover >= 1 ? 1 : .25 + .75 * cover);
      }
    }
  }
  function sD3(env, t, dt) {
    const hosts = env.raw.systems || [],
      tiles = hosts.map(q => ({
        state: stateEnum(q.state),
        load: clamp01(num(q.loadPct) / 100)
      }));
    for (const b of env.raw.popTail || []) tiles.push({
      state: num(b) >> 5 & 7,
      load: (num(b) & 31) / 31
    });
    let n = tiles.length;
    if (!n) return;
    let total = 0,
      q = env.raw.totals || {};
    for (const k of ["wanDownKbps", "wanUpKbps", "lanDownKbps", "lanUpKbps"]) if (q[k] != null) total += num(q[k]);
    if (total > 0) env.aurora = (env.aurora || 0) + dt * (.15 + 2.2 * panelRateLevel(total));
    for (let x = 0; x < PW; x++) {
      let pos = (x + .5) * n / PW - .5,
        a = Math.floor(pos),
        b = a + 1,
        mix = pos - a;
      if (a < 0) a = 0;
      if (b >= n) b = n - 1;
      let ta = tiles[a],
        tb = tiles[b],
        own = mix < .5 ? ta : tb;
      if (own.state === ST_DOWN) {
        for (let y = 0; y < PH; y++) fbSet(x, y, cCrit, 1);
        continue;
      }
      let load = ta.load + (tb.load - ta.load) * mix,
        h = own.state === ST_OK ? cAzure : stateColor(own.state),
        top = own.state === ST_OK ? cOk : h,
        height = 4 + load * 7,
        wave = Math.sin(env.aurora + x * .28) * 1.3 + Math.sin(env.aurora * .61 + x * .11) * .9,
        crest = PH - height + wave,
        ray = .62 + .38 * Math.sin(env.aurora * 2.3 + x * 1.35);
      for (let y = 0; y < PH; y++) {
        let d = y - crest;
        if (d < -.5) continue;
        fbSet(x, y, d < 1 ? top : h, d < 1 ? 1 : ray / (1 + d * .28));
      }
    }
  }

  function sB0(env, t, dt) {
    const a = env.raw.links || [];
    if (!a.length) {
      textCentre(3, "NO LINK DATA", cAzure, 1);
      return;
    }
    const p = pageFor(4, dt, pageCount(a.length, 2));
    function lane(x0, x1, y, rate, key, dir, h, step) {
      if (rate == null) {
        for (let x = x0; x <= x1; x += 4) fbSet(x, y, cUnknown, .35);
        return;
      }
      rate = num(rate);
      if (rate === 0) {
        for (let x = x0; x <= x1; x++) fbSet(x, y, h, .06);
        return;
      }
      let spacing = 14 - 11 * panelRateLevel(rate),
        speed = 2 + 22 * panelRateLevel(rate);
      v2LanePhase[key] = (v2LanePhase[key] || 0) + speed * step;
      v2LanePhase[key] %= spacing * 64;
      for (let x = x0; x <= x1; x++) fbSet(x, y, h, .08);
      for (let z = v2LanePhase[key] % spacing - spacing; z <= x1 - x0; z += spacing) {
        let d = Math.floor(z);
        if (d < 0 || d > x1 - x0) continue;
        let head = dir > 0 ? x0 + d : x1 - d;
        fbSet(head, y, h, 1);
        let tail = head - dir;
        if (tail >= x0 && tail <= x1) fbSet(tail, y, h, .4);
      }
    }
    for (let slot = 0; slot < 2; slot++) {
      let i = p * 2 + slot,
        q = a[i];
      if (!q) break;
      let y = slot ? 6 : 0,
        st = stateEnum(q.state),
        kind = linkKind(q.kind),
        h = [cInk, cAzure, cMaint, cOk][kind] || cOk,
        label = kind === 0 ? String(q.name).indexOf("BACKUP") === 0 ? "WAN2" : "WAN" : q.name;
      textFit(0, y, label, 5, st === ST_DOWN ? cCrit : cInk, 1);
      if (st === ST_DOWN) {
        for (let x = 21; x < PW; x++) {
          fbSet(x, y, cCrit, .6);
          fbSet(x, y + 4, cCrit, .6);
        }
        continue;
      }
      lane(21, 52, y, q.downKbps, i * 2, -1, h, dt);
      lane(21, 52, y + 1, q.downKbps, i * 2, -1, h, 0);
      lane(21, 52, y + 3, q.upKbps, i * 2 + 1, 1, h, dt);
      lane(21, 52, y + 4, q.upKbps, i * 2 + 1, 1, h, 0);
    }
    pageBar(p, pageCount(a.length, 2));
  }

  // 11. Screen table + the paint order from main.c
  // =====================================================================
  const THEMES = [
    { key: "A", name: "FLEET" }, { key: "B", name: "TRAFFIC" },
    { key: "C", name: "TROUBLE" }, { key: "D", name: "RESOURCES" },
  ];
  const SCREENS = [
    { id: "A0", theme: 0, screen: 0, title: "Host Tiles", render: sA0 },
    { id: "A1", theme: 0, screen: 1, title: "Roster", render: sA1 },
    { id: "A2", theme: 0, screen: 2, title: "Pools", render: sA2 },
    { id: "A3", theme: 0, screen: 3, title: "Head Count", render: sA3 },
    { id: "B0", theme: 1, screen: 0, title: "Swim Lanes", render: sB0 },
    { id: "B1", theme: 1, screen: 1, title: "WAN and LAN", render: sB1 },
    { id: "B2", theme: 1, screen: 2, title: "Top Talkers", render: sB2 },
    { id: "B3", theme: 1, screen: 3, title: "Flow Map", render: sB3 },
    { id: "C0", theme: 2, screen: 0, title: "Alerts", render: sC0 },
    { id: "C1", theme: 2, screen: 1, title: "Fault Board", render: sC1 },
    { id: "C2", theme: 2, screen: 2, title: "Reachability", render: sC2 },
    { id: "C3", theme: 2, screen: 3, title: "Alert History", render: sC3 },
    { id: "D0", theme: 3, screen: 0, title: "Host Detail", render: sD0 },
    { id: "D1", theme: 3, screen: 1, title: "CPU and Memory", render: sD1 },
    { id: "D2", theme: 3, screen: 2, title: "Trends", render: sD2 },
    { id: "D3", theme: 3, screen: 3, title: "Aurora", render: sD3 },
  ];

  /* SCREEN_BLURBS — one short operator description per screen, keyed by id.
     Same source of truth as the screen renderers above and the panel manual
     (docs/panel/SolariNet_Panel_Manual.html): what the regions mean, and any
     deviation worth stating plainly. Rendered for the screen the wall is
     showing right now, and as the hover text on every tile. */
  const SCREEN_BLURBS = {
    A0: "One state-coloured tile per host; hue shows state and fill shows load. Down and unmeasured hosts use a flat tile.",
    A1: "A two-host roster page, worst first. Each row shows the host name, state block and load or state word.",
    A2: "Two pools per page. State cells show each pool's host mix; the lower bar shows mean load.",
    A3: "The up-host count in large type, total fleet size, and the worst current state beneath it.",
    B0: "Two network links per page, with measured upload and download rates shown as moving streams. Link kind sets the colour.",
    B1: "WAN and LAN rate labels with separate 53-sample, newest-at-right throughput sparklines.",
    B2: "The eight busiest measured hosts, two per page, ranked by combined receive and transmit rate.",
    B3: "A parent tree of network links. Uplinks show topology and moving dots indicate measured traffic.",
    C0: "One open alert per page with host, age and condition text; an extra page points to remaining alerts.",
    C1: "Current fault counts grouped as network, services, disks and kernel, with a separate other page when needed.",
    C2: "Probe histories across the width, newest at the right. Red marks loss, amber slow response and green normal response.",
    C3: "Warning and critical alert firings in 30-minute buckets across 26 hours; the right edge is now.",
    D0: "One worst-first host per page, showing state, CPU, memory and disk readings.",
    D1: "A host-by-host butterfly chart: CPU rises above the center line and memory falls below it.",
    D2: "Three local history strips for WAN traffic, mean CPU and round-trip time. Unfilled history stays dark.",
    D3: "A measurement-driven aurora: host bands carry load and state; down hosts form red pillars.",
  };

  /* paintScreen — the firmware's per-frame paint order (main.c): no-data
     treatment when the fleet is empty, else the active screen, then the inlay
     when the alarm is armed, then the beacon LAST so it overrides the inlay's
     right-hand rail. `armed` mirrors main.c's gAlarmArmed: alarmActive and not
     acknowledged for the current episode. */
  const SCREEN_LABELS = ["HOST TILES", "ROSTER", "POOLS", "HEAD COUNT", "SWIM LANES", "WAN AND LAN", "TOP TALKERS", "FLOW MAP", "ALERTS", "FAULT BOARD", "REACHABILITY", "ALERT HISTORY", "HOST DETAIL", "CPU AND MEM", "TRENDS", "AURORA"];
  const HELP_TEXTS=[
    ["TILE IS HOST","FILL IS LOAD"],["HOST LIST","WORST FIRST"],["EACH POOL","HOSTS LOAD"],["HOSTS UP","AND WORST NOW"],
    ["LINK TRAFFIC","DOTS ARE RATE"],["INTERNET AND","LOCAL TRAFFIC"],["BUSIEST HOSTS","BY TRAFFIC"],["NETWORK TREE","DOTS ARE RATE"],
    ["OPEN ALERTS","WORST FIRST"],["OPEN FAULTS","BY KIND"],["PING HISTORY","RED IS LOSS"],["ALERTS 26 HRS","NOW AT RIGHT"],
    ["ONE HOST","CPU MEM DISK"],["UP IS CPU","DOWN IS MEM"],["TRAFFIC CPU","AND PING"],["AMBIENT FLEET","RED IS FAULT"]
  ];
  function helpOverlay(index){if(index<0||index>15)return;const h=HELP_TEXTS[index];textCentre(0,h[0],cInk,1);textCentre(6,h[1],cAzure,1);}
  function titleCard(index,age){if(age<0||age>=1)return;fbDimAll(.08);textCentre(3,SCREEN_LABELS[index],cInk,1);}
  function inlay(env, t) {
    fbDimAll(.1);
    const pulse = .55 + .45 * Math.abs(Math.sin(t * 2.2));
    for (let y = 0; y < PH; y++) {
      fbSet(0, y, cCrit, pulse);
      fbSet(52, y, cCrit, pulse);
    }
    for (let y = 0; y < PH; y++) for (let x = 1; x < 52; x++) fbDim(x, y, .2);
    textCentre(0, String(env.subject || "ALERT").slice(0, 12), cCrit, 1);
    textCentre(6, env.detail || "", cInk, 1);
  }
  function beacon(t){if(t%1>=.5)return;for(let y=0;y<=1;y++)for(let x=51;x<=52;x++)fbSet(x,y,cCrit,1);}
  function screenNoData(t,haveData){textCentre(3,haveData?"NO HOSTS":"AWAITING DATA",cAzure,.6+.4*(.5+.5*Math.sin(t*.8)));}
  function screenLinkLost(t){fbDimAll(.06);textCentre(0,"NO DATA FEED",cWarn,1);textCentre(6,"SCREEN IS OLD",cInk,1);fbSet(Math.floor(t*12)%PW,5,cWarn,.6);}

  function paintScreen(fb, idx, env, t, dt, st, armed, currentIdx, titleAge) {
    fbUse(fb);
    fbClear();
    if (!env || !env.haveData || env.total === 0) {
      screenNoData(t, !!(env && env.haveData));
      if (armed) beacon(t);
      return;
    }
    SCREENS[idx].render(env, t, dt, st);
    if (env.dataStale) { fbSet(0, 10, cWarn, 1); fbSet(1, 10, cWarn, 1); }
    if (idx === currentIdx) titleCard(idx, titleAge == null ? 99 : titleAge);
    if (armed) { inlay(env, t); beacon(t); }
  }

  // =====================================================================
  // 12. Canvas painter
  //
  //    583 LEDs x 16 screens x 25 fps is ~175k arc() calls a second, which no
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
  const N_SCREENS = 16;                  // CONTRACT-V2 §1, theme*4+slot
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
     sixteen screens each, so they cannot share a key the way the global
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
     into 16 entries of { enabled, weightCode } or null.
     STATE AUTHORITY (CONTRACT-V2 §1): this is the PANEL's report of its own
     persisted config, relayed by the daemon. A null entry means the panel has
     not told us, and the UI must render that as unknown — NEVER as the §3.4
     factory default (enabled, 1×), because "we have not heard" and "the panel
     says all-on at 1×" are different facts and only one of them is ours to
     assert. Returning null for the whole thing is a valid outcome.

     The config API accepts sixteen entries and preserves twelve-entry legacy
     reports by applying the firmware defaults to the four new slots:
       - [12] u8 bytes            bit0 enabled, bits1..3 weightCode  (§3.3)
       - [12] objects             { enabled, weightCode | weight }
       - { screenCfg: <above> }   the §3.3 field name, wrapped
       - 32-char hex string       the 16 bytes, as the daemon might relay them
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
      if (!/^(?:[0-9a-fA-F]{24}|[0-9a-fA-F]{32})$/.test(hex)) return null;
      const old = hex.length === 24, out = new Array(N_SCREENS);
      for (let i = 0; i < 12; i++) out[i] = decodeScreenByte(parseInt(hex.substr(i * 2, 2), 16));
      for (let i = 12; i < N_SCREENS; i++) out[i] = old ? { enabled: true, weightCode: 2 } : decodeScreenByte(parseInt(hex.substr(i * 2, 2), 16));
      return out;
    }
    if (!Array.isArray(src) || (src.length !== N_SCREENS && src.length !== 12)) return null;
    const out = new Array(N_SCREENS);
    for (let i = 0; i < N_SCREENS; i++) out[i] = i < src.length ? decodeScreenEntry(src[i]) : { enabled: true, weightCode: 2 };
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

  // Fixture copy paths, tried in order for local and deployed docroots.
  const FIXTURE_PATHS = ["fixtures/parity-fixture.json", "parity-fixture.json", "../status-panel/fixtures/parity-fixture.json"];

  function api() { return window.SolariAPI; }

  /* readRole — FAIL CLOSED. Controls are presentation only;
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

  function VirtualPanel({ env, armed, gain, currentIdx, titleAge, sleeping, still, screenCfg, pend, send, ctlDisabled }) {
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
          paintScreen(fb, i, env, t, i === currentIdx ? dt : 0, sts[i], armed, currentIdx, titleAge);
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
            paintScreen(fb, i, env, t, i === currentIdx ? FIXTURE_DT : 0, sts[i], armed, currentIdx, titleAge);
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
    }, [env, armed, gain, still, currentIdx, titleAge]);

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
                <span className="vp-tile__title" title={SCREEN_BLURBS[s.id] || ""}>{s.title}</span>
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

  /* Firmware-owned treatments are shown as static reference frames. */
  function TreatmentStrip({ env }) {
    const refs = useRef([]);
    useEffect(function () {
      const fb = fbNew();
      fbUse(fb); fbClear(); screenLinkLost(FIXTURE_T); paintCanvas(refs.current[0], fb, 1);
      fbUse(fb); fbClear(); screenNoData(FIXTURE_T, false); paintCanvas(refs.current[1], fb, 1);
      fbUse(fb); fbClear(); helpOverlay(0); paintCanvas(refs.current[2], fb, 1);
      fbUse(fb); fbClear(); SCREENS[0].render(env, 3, 0, {}); titleCard(11, .5); paintCanvas(refs.current[3], fb, 1);
      fbUse(fb); fbClear(); SCREENS[0].render(env, 3, 0, {}); inlay(env, 3); paintCanvas(refs.current[4], fb, 1);
    }, [env]);
    const items = [
      { id: "LINK LOST", note: "No valid frame for more than 15 seconds" },
      { id: "NO DATA", note: "Awaiting the first snapshot" },
      { id: "HELP", note: "Two still instruction rows" },
      { id: "TITLE CARD", note: "Screen name over a dimmed frame" },
      { id: "ALERT INLAY", note: "Alert subject and detail over the active screen" },
    ];
    return <div className="vp-strip">{items.map(function (it, i) {
      return <figure key={it.id} className="vp-tile vp-tile--ref">
        <figcaption className="vp-tile__head"><span className="vp-tile__id">{it.id}</span><span className="vp-tile__title">{it.note}</span></figcaption>
        <div className="vp-tile__screen"><canvas ref={function (el) { refs.current[i] = el; }} width={TILE_W} height={TILE_H} className="vp-canvas" role="img" aria-label={it.id + " reference treatment on the 53 by 11 matrix"} /></div>
      </figure>;
    })}</div>;
  }

  // =====================================================================
  // 15. Controls (panel command API)
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
            <div className="vp-ctl__note">
              Identical to pressing that theme&rsquo;s button on the unit: the panel jumps to the
              theme&rsquo;s first screen that is still in rotation, and carries on from there.
            </div>
          </div>

          <div className="vp-ctl">
            <div className="vp-ctl__label">Screen <CmdState p={pend.screen} /></div>
            <div className="vp-ctl__row">
              {[0, 1, 2, 3].map(function (n) {
                const on = state && state.screen === n;
                const title = state ? (SCREENS[(state.theme || 0) * 4 + n] || {}).title : "";
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
            <div className="vp-ctl__note">
              A slot within the current theme. A screen taken out of rotation is still accepted here —
              operator intent wins — and rotation resumes per the configuration at the next advance.
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
            <div className="vp-ctl__note">
              Acknowledging silences the tone and the beacon; it does not clear the alert, which stays
              active in the pipeline. It is scoped to the episode the server composed, so a new episode
              re-arms tone, inlay and beacon even if the previous one was acknowledged seconds earlier.
              Any physical button on the unit does the same thing, by the same code path.
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
            <div className="vp-ctl__note">
              Sleep blanks the matrix but not the firmware: snapshots keep arriving, history keeps
              accumulating, and the alarm beacon is still painted. An arriving alarm wakes the panel once,
              on the rising edge, and does not hold it awake. Other commands deliberately do not wake it.
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
          setErr("Parity fixture not found. Deploy status-panel/fixtures/parity-fixture.json to "
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
       profile: The command API requires read-only under EVERY whoami outcome
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
    const working = fixtureMode ? (fixDoc && Array.isArray(fixDoc.history) ? fixDoc.history : []) : samples;
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
    const currentIdx = pstate ? (num(pstate.theme) * 4 + num(pstate.screen)) : -1;
    const gain = driverGain(pstate ? pstate.brightnessPct : 100);

    const reportedAt = pstate ? parseReportedAt(pstate.reportedAt) : null;
    const refAge = fixtureMode && head ? (sampleMs(head) - (reportedAt || 0)) : (reportedAt ? now - reportedAt : null);
    const stale = refAge != null && refAge > 90000;
    const titleAge = refAge == null ? 99 : Math.max(0, refAge / 1000);

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
              All sixteen status-panel screens, simulated from the same telemetry the physical panel receives.
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

        {fixtureMode && fixDoc && <p className="vp-footnote">Rendering the committed parity fixture ({working.length} chronological samples).</p>}

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
          {pstate && !screenCfg && (
            <span className="vp-chip vp-chip--muted">Screen config not reported</span>
          )}
          {screenCfg && cfgSummary && <span className="vp-chip vp-chip--muted">{cfgSummary}</span>}
          {cfgSaved && <span className="vp-chip vp-chip--muted">Saved to panel flash</span>}
        </div>

        {!env && !err && <div className="empty">Waiting for the first /api/panel sample…</div>}

        {currentIdx >= 0 && SCREENS[currentIdx] && SCREEN_BLURBS[SCREENS[currentIdx].id] && (
          <p className="vp-footnote">
            <strong>On the wall now &mdash; {SCREENS[currentIdx].id} {SCREENS[currentIdx].title}.</strong>{" "}
            {SCREEN_BLURBS[SCREENS[currentIdx].id]}
          </p>
        )}

        {env && (
          <VirtualPanel
            env={env}
            armed={armed}
            gain={gain}
            currentIdx={currentIdx}
            titleAge={titleAge}
            sleeping={!!(pstate && pstate.sleeping)}
            still={fixtureMode}
            screenCfg={screenCfg}
            pend={pend}
            send={send}
            ctlDisabled={!canControl}
          />
        )}

        {env && <TreatmentStrip env={env} />}

        <p className="vp-footnote">
          The sixteen screen previews use the same framebuffer geometry, state, layout, page timing and history rules as the firmware. The selected screen preview also shows the title card during its reported first second. Sleep blanks the physical panel; the previews remain visible for comparison.
        </p>

        <p className="vp-footnote">
          <strong>Rotation and dwell</strong> are set per screen on the tile itself. A weight multiplies the
          theme&rsquo;s base dwell, so at a 30&nbsp;s base ½× is 15&nbsp;s and 5× is 150&nbsp;s. The panel stores
          this in its own flash and keeps it across resets, which is why a control here shows a setting as in
          force <em>only</em> once the panel has reported it back &mdash; before that, and while a change is in
          flight, no option is marked. Turning every screen of a theme off is not a way to blank the panel: the
          firmware treats an all-disabled theme as all-enabled rather than showing a black wall.

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
