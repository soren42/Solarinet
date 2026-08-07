# RackWire — Handoff to Claude Code

A browser-based planner for cabling servers, network gear, power and peripherals.
Two views: a logical connection map and a data table. Everything runs client-side today.

**Nothing here needs a rewrite to become real.** Every place that wants live data already
has a defined seam. This document lists the seams, in priority order.

---

## 1. Files

| File | What it is |
|---|---|
| `Connection Planner.dc.html` | The whole app — template + logic class. Opens directly in a browser. |
| `device-library.js` | `window.RW` — port glyph geometry, cable standards, seeded device definitions, the layout engine. |
| `live-adapter.js` | `window.RWLive` — telemetry transport layer. Contract documented at the top of the file. |
| `support.js` | Runtime shim for the component format. Do not edit. |

No build step, no dependencies, no network calls. Open the HTML file and it works.

---

## 2. What is real and what is stubbed

**Real and complete**

- Port-to-port cabling with curve routing around obstacles
- Capacity analysis: link speed negotiation, connector fit, per-standard length limits,
  PoE per-port class and per-switch budget, AC circuit loading at 80% derate,
  adapter pass-through with conversion loss, uplink oversubscription, double-landed ports
- Undo/redo (80 steps), snapshots, CSV and JSON import/export, printable punch list
- Touch canvas: pinch zoom, pan, grid snap, marquee select, group move, racks, notes
- Live-activity rendering: utilization-weighted cable widths, flow animation, carrier dots,
  path-to-internet and on-battery glows, live faults in the punch list
- Host API and postMessage bridge for embedding

**Stubbed — needs you**

| Seam | Where | What it needs |
|---|---|---|
| Telemetry source | `live-adapter.js` | Point `solarinet-ws` / `solarinet-rest` at real endpoints |
| Barcode lookup | `handleCode()` in the logic class | A UPC/model lookup service |
| Device library | `window.RW.DEVICES` in `device-library.js` | Serve from inventory instead of a static array |
| Persistence | `load()` / `persist()` | Swap localStorage for a backend if plans must be shared |

---

## 3. Telemetry — the main integration

`live-adapter.js` holds the full contract in a comment block at the top. Summary:

A **source** is a factory `function(ctx) -> { start(), stop() }` where `ctx` gives you
`getPlan()`, `emit(frame)`, `setStatus(state, msg)` and `config`.

Three sources are registered: `simulator` (working demo), `solarinet-ws` and
`solarinet-rest`. The latter two are written end to end — auth frame, asset subscribe,
exponential-backoff reconnect, bearer-token polling — and only need a URL that exists.

### Frame schema

Every field is optional. Anything you omit is simply not overlaid.

```js
{
  ts: 1730000000000,
  devices: {
    "<deviceKey>": {
      reachable: true,     // false → device flagged unreachable, glows drop
      loadW: 143.2,        // measured; replaces the planned figure on the badge
      amps: 1.19,
      poeUsedW: 61,        // checked against the device's poeBudgetW
      tempC: 44,
      cpuPct: 12,
      uptimeS: 918273
    }
  },
  ports: {
    "<deviceKey>|<portId>": {
      link: true,             // false on a planned cable → red dashed, punch-list error
      negotiatedGbps: 2.5,    // below the port's rated speed → warning
      rxGbps: 0.42,
      txGbps: 1.1,            // >85% of capacity → saturation warning
      poeDeliveredW: 13.4,    // past the port's class → error
      errors: 0,              // non-zero → warning
      sfpTempC: 39,
      opticalDbm: -4.2
    }
  }
}
```

### Key resolution

`deviceKey` resolves in order: `device.liveKey` → `device.assetId` → `device.name`,
case-insensitive. **Live key** and **asset ID** are editable per device in the Devices
table and the inspector, so telemetry keyed by your system of record lines up without
renaming anything on the canvas.

`portId` is the app's internal ID (`ge-3`, `sfp-1`, `ac-1`). Get the list for a device
with `RackWire.getPlan()` and running each device's groups through `RW.layout()`, or read
them off the search index via `RackWire.find(deviceName)`.

### Three ways to feed it

```js
// 1. Register your own transport
RWLive.register('solarinet-grpc', (ctx) => ({ start(){…}, stop(){…} }));

// 2. Use a built-in with a URL
RackWire.setLive(true, 'solarinet-ws', { url: 'wss://…', token: '…' });

// 3. Push frames from a transport that lives elsewhere
RackWire.pushLive(frame);
```

If your backend's shape differs from the schema, pass `config.transform` — it is applied
to every inbound message before the UI sees it.

---

## 4. Host API

`window.RackWire`, 23 methods. Same surface works over postMessage for an iframe embed:
`{ source:'rackwire', cmd:'<method>', args:[…] }` in, and `ready` / `change` / `select` /
`reply` events back out.

```
getPlan()                 → { devices, cables, racks, notes, codeMap }
setPlan(plan)             replace everything
patch({devices, cables})  merge by uid
subscribe(fn)             fn(plan) on every change; returns unsubscribe
pushLive(frame)           inject telemetry
setLive(on, source, cfg)  start/stop a registered source
select(uids) / focus(uid)
setView('map'|'table')
setChrome(false)          hide the app header for embedding
theme({accent, ok, …})    retint via --sn-* tokens
undo() / redo()
find(q)                   → [{dev, port, label, sub}]
saveVersion(name) / versions() / restoreVersion(id)
printReport()             printable punch list
issues()                  → [{sev, msg, cable?}]
library() / cableStandards() / tokens()
```

Query flags for a no-JS embed: `?embed=1` (no header), `?view=table`, `?live=simulator`.

**Standalone must keep working.** None of the above runs unless a host calls it. Please
preserve that — the file is used directly from disk on an iPad.

---

## 5. Theming

Everything reads SolariNet Rev 2 tokens off `:root`. Template inline styles use
`var(--sn-x, #fallback)`; SVG (which cannot read `var()`) goes through a resolver in
`tok()` that re-reads on `theme()`.

```
--sn-bg --sn-surface --sn-raised
--sn-ink --sn-dim --sn-faint --sn-quiet
--sn-divider --sn-divider-strong
--sn-accent --sn-accent-700 --sn-on-field
--sn-ok --sn-warn --sn-crit --sn-maint --sn-neutral
--sn-crit-ink --sn-crit-surface --sn-crit-border
--sn-signal-other --sn-signal-open
```

Two extension tokens beyond the SolariNet set, both deliberate:

- `--sn-signal-other` (#E8833A) — the four cable domains are network / power / USB / other
  and the palette has no orange. Retire it to `--sn-neutral` from the host if you prefer.
- `--sn-signal-open` (#FFE04D) — unconnected cable ends. Held clear of the amber
  `--sn-warn` band so the two never blur at 3 px stroke width.

Type is IBM Plex Sans throughout, IBM Plex Mono for all numerics, port IDs and status.

---

## 6. Device library

`window.RW.DEVICES` is a plain array. Each entry:

```js
{
  id, vendor, name, model, kind, ru, drawW, note,
  budgetW,          // output rating for a power source; overrides circuit math
  circuitA, circuitV,
  poeBudgetW,
  eff,              // adapter conversion efficiency, default 0.9
  isInternet,       // termination point — seeds the green path glow
  isBattery,        // UPS — seeds the amber battery glow
  groups: [{
    k,              // stable group key; migrations match on this
    label, glyph, count, rows,
    role,           // lan wan uplink poe-in power-in power-out data video audio module passive gpio
    gbps, watts, amps, volts,
    poe, poeW,      // what this port delivers
    poeDrawW,       // what this device draws when powered over ethernet
    batt,           // outlet is on battery
    domain,         // net power usb other
    conn            // rj45 sfp usba usbc hdmi iec barrel audio any
  }]
}
```

`RW.layout({groups, name, model})` turns that into positioned ports and a device box size.
Glyphs are simplified line art in `RW.GLYPHS`; add new ones as arrays of rect/circle/line/path.

Currently seeded: Ubiquiti UDM-Pro-Max, USW-Pro-Max-16-PoE, USW-Ultra, U7 Pro, U7 Pro XGS;
Raspberry Pi 5; Mac mini M4 Pro; Minisforum MS-R1; Google Fiber Jack GOXP330C; four power
units (Sabrent VOLTIK, SuperDanny strip, CyberPower CP1500AVRLCD, APC BE1050G3); three
SFP+ to RJ45 transceivers; seven power adapters; and generic PDU, outlet, UPS, patch panel,
display, NAS, injector, blank.

### Migration matters

Devices are **snapshots** of a library definition taken when placed. `migrate()` runs on
load, JSON import and `setPlan()`, backfilling any field an instance is missing from its
definition — device-level flags and per-group keys matched by `k`. User-edited values are
never overwritten, only absent ones are filled. **If you add a field to the library, add it
to `DEV_FIELDS` or `GROUP_FIELDS` in `migrate()`** or existing plans will not see it.

---

## 7. Barcode lookup

`handleCode(code)` currently: checks the user's saved code→model bindings, then matches
normalised model and name against the local library, then offers a bind dialog.

A USB or Bluetooth reader already works anywhere in the app — a keystroke buffer catches
the scan and fires on Enter. Camera scanning uses `BarcodeDetector` where the browser has
it; **iOS Safari does not ship one**, so on an iPad the hardware reader is the path.

To make lookup real, replace the middle of `handleCode()` with a call to your service and
have it return a library-shaped device definition. The bind UI and the code map behind it
can stay as the fallback for unrecognised codes.

---

## 8. Known limits

- Plans live in one localStorage key (`rackwire.project.v2`), snapshots in
  `rackwire.project.v2.versions`, capped at 25. No multi-user, no conflict resolution.
- No cable slack or physical distance modelling. Deliberate — the map is logical, not spatial.
- No rack elevation view. Racks exist as containers with U slots but are not drawn as a front face.
- Curve routing is a heuristic over eight candidate control-point pairs, not a real router.
  Dense plans will still cross.
- The simulator invents plausible numbers. It is for demoing the live view, not for tests.
