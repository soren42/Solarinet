/* RackWire live telemetry layer.
 *
 * ─────────────────────────────────────────────────────────────────────────────
 * INTEGRATION CONTRACT (for Claude Code / backend work)
 *
 * A source is a factory:  function(ctx) -> { start(), stop() }
 *   ctx.getPlan()   -> { devices, cables }  current plan snapshot (read-only)
 *   ctx.emit(frame) -> push a normalized telemetry frame into the UI
 *   ctx.setStatus(s, msg) -> 'connecting' | 'live' | 'error' | 'idle'
 *   ctx.config      -> whatever was passed to RWLive.start()
 *
 * FRAME SCHEMA (all fields optional; omit what the backend does not report)
 * {
 *   ts: 1730000000000,
 *   devices: {
 *     "<deviceKey>": {
 *       reachable: true,
 *       loadW: 143.2,          // measured, not planned
 *       amps: 1.19,
 *       poeUsedW: 61,
 *       tempC: 44,
 *       uptimeS: 918273,
 *       cpuPct: 12
 *     }
 *   },
 *   ports: {
 *     "<deviceKey>|<portId>": {
 *       link: true,            // carrier
 *       negotiatedGbps: 2.5,   // actual autoneg result
 *       rxGbps: 0.42, txGbps: 1.1,
 *       poeDeliveredW: 13.4,
 *       errors: 0,             // CRC / discard counter delta
 *       sfpTempC: 39, opticalDbm: -4.2
 *     }
 *   }
 * }
 *
 * deviceKey resolution order: device.liveKey → device.assetId → device.name
 * (case-insensitive). Set liveKey per device to bind to your system of record.
 * ─────────────────────────────────────────────────────────────────────────────
 */
(function () {
  var sources = {};
  var active = null;

  function register(name, factory) { sources[name] = factory; }

  function keyOf(d) { return String(d.liveKey || d.assetId || d.name || d.uid).toLowerCase(); }

  // ---- built-in: simulator -------------------------------------------------
  register('simulator', function (ctx) {
    var timer = null, t = 0, seeds = {};
    function rnd(k, lo, hi) {
      if (seeds[k] == null) seeds[k] = Math.random();
      var s = Math.sin(t / 9 + seeds[k] * 31) * 0.5 + 0.5;
      return lo + (hi - lo) * (s * 0.75 + Math.random() * 0.25);
    }
    function tick() {
      var plan = ctx.getPlan();
      var frame = { ts: Date.now(), devices: {}, ports: {} };
      var linked = {};
      plan.cables.forEach(function (c) {
        [c.a, c.b].forEach(function (e) { if (e && e.dev) linked[e.dev + '|' + e.port] = c; });
      });
      plan.devices.forEach(function (d) {
        var dk = keyOf(d);
        var down = rnd('dev' + d.uid, 0, 1) > 0.985;
        var load = d.drawW ? rnd('load' + d.uid, d.drawW * 0.32, d.drawW * 0.86) : 0;
        var poe = 0;
        (d._livePorts || []).forEach(function () {});
        frame.devices[dk] = {
          reachable: !down,
          loadW: Math.round(load * 10) / 10,
          amps: Math.round((load / 120) * 100) / 100,
          tempC: Math.round(rnd('t' + d.uid, 31, 58)),
          cpuPct: Math.round(rnd('c' + d.uid, 3, 46)),
          uptimeS: 86400 * 6 + t * 5
        };
        (d.ports || []).forEach(function (p) {
          var pk = dk + '|' + p.id;
          var cable = linked[d.uid + '|' + p.id];
          if (!cable) { frame.ports[pk] = { link: false }; return; }
          var flap = rnd('f' + pk, 0, 1) > 0.975;
          var neg = p.gbps || 0;
          // occasionally negotiate down a tier, the way real copper does
          if (rnd('n' + pk, 0, 1) > 0.93 && neg >= 2.5) neg = neg >= 10 ? 2.5 : 1;
          var cap = neg || 1;
          var util = rnd('u' + pk, 0.02, p.role === 'uplink' || p.role === 'wan' ? 0.94 : 0.55);
          var deliver = 0;
          if (p.poe) {
            var other = cable.a && cable.a.dev === d.uid ? cable.b : cable.a;
            var draw = other && other._poeDrawW ? other._poeDrawW : 0;
            deliver = draw ? Math.round(rnd('p' + pk, draw * 0.6, draw * 1.08) * 10) / 10 : 0;
            poe += deliver;
          }
          frame.ports[pk] = {
            link: !flap,
            negotiatedGbps: neg,
            rxGbps: Math.round(cap * util * 100) / 100,
            txGbps: Math.round(cap * util * rnd('x' + pk, 0.2, 0.9) * 100) / 100,
            poeDeliveredW: deliver || undefined,
            errors: rnd('e' + pk, 0, 1) > 0.97 ? Math.round(rnd('ec' + pk, 1, 240)) : 0
          };
        });
        if (poe) frame.devices[dk].poeUsedW = Math.round(poe * 10) / 10;
      });
      t++;
      ctx.emit(frame);
    }
    return {
      start: function () {
        ctx.setStatus('connecting', 'Starting simulated feed');
        setTimeout(function () {
          ctx.setStatus('live', 'Simulated feed · ' + ((ctx.config.intervalMs || 2000) / 1000) + 's poll');
          tick();
          timer = setInterval(tick, ctx.config.intervalMs || 2000);
        }, 450);
      },
      stop: function () { clearInterval(timer); ctx.setStatus('idle', ''); }
    };
  });

  // ---- Solarinet dashboard: websocket ------------------------------------
  // Expects frames already in the schema above, or set config.transform to map.
  register('solarinet-ws', function (ctx) {
    var ws = null, retry = null, tries = 0;
    function open() {
      var url = ctx.config.url;
      if (!url) { ctx.setStatus('error', 'No Solarinet websocket URL configured'); return; }
      ctx.setStatus('connecting', 'Connecting to ' + url);
      try { ws = new WebSocket(url); } catch (e) { ctx.setStatus('error', e.message); return; }
      ws.onopen = function () {
        tries = 0;
        ctx.setStatus('live', 'Solarinet · ' + url);
        if (ctx.config.token) ws.send(JSON.stringify({ type: 'auth', token: ctx.config.token }));
        // tell the backend which assets this plan cares about
        ws.send(JSON.stringify({ type: 'subscribe', assets: ctx.getPlan().devices.map(keyOf) }));
      };
      ws.onmessage = function (ev) {
        var msg;
        try { msg = JSON.parse(ev.data); } catch (e) { return; }
        ctx.emit(ctx.config.transform ? ctx.config.transform(msg) : msg);
      };
      ws.onerror = function () { ctx.setStatus('error', 'Socket error'); };
      ws.onclose = function () {
        if (!ws) return;
        var wait = Math.min(15000, 1000 * Math.pow(2, tries++));
        ctx.setStatus('error', 'Disconnected · retrying in ' + Math.round(wait / 1000) + 's');
        retry = setTimeout(open, wait);
      };
    }
    return {
      start: open,
      stop: function () { clearTimeout(retry); var s = ws; ws = null; if (s) s.close(); ctx.setStatus('idle', ''); }
    };
  });

  // ---- Solarinet dashboard: REST poll -------------------------------------
  register('solarinet-rest', function (ctx) {
    var timer = null, stopped = false;
    function halt(msg) {
      stopped = true;
      clearInterval(timer);
      timer = null;
      ctx.setStatus('error', msg);
    }
    async function poll() {
      if (stopped) return;
      try {
        var r = await fetch(ctx.config.url, { headers: ctx.config.token ? { Authorization: 'Bearer ' + ctx.config.token } : {} });
        if (stopped) return;
        // A logged-out session will never recover on its own, so retrying every
        // 5s only hammers the API and pins a wrong "error" status. Terminal until
        // the operator signs in and starts the feed again.
        if (r.status === 401 || r.status === 403) {
          return halt('Sign-in required — open the dashboard and log in');
        }
        if (!r.ok) throw new Error('HTTP ' + r.status);
        var j = await r.json();
        if (stopped) return;
        ctx.setStatus('live', 'Solarinet REST · ' + ctx.config.url);
        ctx.emit(ctx.config.transform ? ctx.config.transform(j) : j);
      } catch (e) {
        if (stopped) return;
        ctx.setStatus('error', e.message);
      }
    }
    return {
      start: function () { stopped = false; ctx.setStatus('connecting', 'Polling ' + ctx.config.url); poll(); timer = setInterval(poll, ctx.config.intervalMs || 5000); },
      stop: function () { stopped = true; clearInterval(timer); ctx.setStatus('idle', ''); }
    };
  });

  window.RWLive = {
    sources: sources,
    register: register,
    keyOf: keyOf,
    list: function () { return Object.keys(sources); },
    start: function (opts) {
      this.stop();
      var factory = sources[opts.source];
      if (!factory) { opts.onStatus && opts.onStatus('error', 'Unknown source: ' + opts.source); return null; }
      var ctx = {
        config: opts.config || {},
        getPlan: opts.getPlan,
        emit: function (f) { opts.onFrame && opts.onFrame(f); },
        setStatus: function (s, m) { opts.onStatus && opts.onStatus(s, m); }
      };
      active = factory(ctx);
      active.start();
      return active;
    },
    stop: function () { if (active) { try { active.stop(); } catch (e) {} active = null; } },
    /* Manual push, for a backend that already has its own transport:
         RWLive.push({ ts: Date.now(), ports: { 'core switch|sfp-1': { link: true, rxGbps: 3.2 } } }) */
    push: function (frame) { if (this._sink) this._sink(frame); }
  };
})();
