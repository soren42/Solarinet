# SolariNet Dashboard Audit

_Audit date: 2026-07-06 · Scope: `dashboard/` (PHP read/mutation API + JSX SPA). The C
control plane was not touched. Live target: `https://xenon:9443` (Apache + php-fpm serving
the repo working tree; SPA copied to `/var/www/solarinet`)._

---

## 1. Architecture & data flow

```
Browser SPA (React via in-browser Babel, no build step)
  index.html loads, in order:
    data.jsx      offline fixture + fmt helpers (window.SOLARI seed)
    api.jsx       live adapter: EP map, mapXxx() wire->UI, loadLive(), SSE, auth gate
    icons.jsx / components.jsx   shared UI (Sidebar, TopBar, gauges, RTTBars, DangerModal…)
    screens.jsx   Fleet, NodeDetail, Alerts, AttentionPanel
    layout.jsx    topology graph engine (force/MST layout)
    screens2.jsx  Reachability, Topology, GearThroughput
    screens3.jsx  Discovery, Provisioning, Config&Rules, Pools, Assets, AssetDetail, ServiceDetail
    app.jsx       router (route.name switch), command palette, login gate, boot

SPA  --fetch /api/*-->  php-fpm (dashboard/api/index.php front controller)
  index.php: bootstrap -> auth routes -> read routes -> mutation routes -> AUTH GATE -> dispatch
    Reads  (GET) : SELECT-only over MariaDB `solarinet` (lib/Db.php, PDO)
    Writes (POST): marshalled to the C server over AF_UNIX solariCtl (lib/SolariCtl.php)
  Auth: session cookie `solari_sess`; local users in solari-auth.json (bcrypt) OR Keycloak OIDC
```

Hard rules observed in the code: PHP is a **read layer**; every mutation goes through the
`solariCtl` bridge verbs (DISCOVER/ADOPT/IGNORE/APPROVE/PROVISION/DECOMMISSION/SURVEY/config).
PHP holds no CA material and writes no telemetry tables (the one exception is `alertRule`
CRUD via `Db::exec`, documented in Db.php).

The SPA **always** boots the offline fixture first, then `loadLive()` overwrites
`window.SOLARI` from the API. If any core fetch rejects, it keeps the fixture and shows the
amber "DEMO DATA" banner. If the API is reachable but returns 401, it shows the login gate.

---

## 2. Screen / view inventory

| Nav / route | Component (file) | Entities shown | API reads | Mutations |
|---|---|---|---|---|
| Fleet Overview `fleet` | `PoolCards` + `FleetOverview` (screens.jsx) — heat/table/cards | nodes (agent hosts) + adopted assets as reachability-only rows; pools | `/api/summary`, `/api/nodes`, `/api/pools`, `/api/assets` (via loadLive) | survey |
| — Node detail `node` | `NodeDetail` (screens.jsx) | one agent host: CPU/RAM/disk/net/procs, history, alerts | `/api/nodes/{id}`, `/api/nodes/{id}/history` | survey, re-converge |
| Systems `assets` | `Assets` + `AssetDetail` (screens3.jsx) | adopted (agent-less) monitored systems, their probe targets | `/api/assets`, `/api/assets/{id}` | remove asset, add/remove target |
| — Service detail `service` | `ServiceDetail` (screens3.jsx) | one probe target across vantages | `/api/probes` (filtered client-side) | — |
| **Reachability** `reachability` | `Reachability` (screens2.jsx) | probe targets × monitor vantages matrix (RTT/loss/outcome) | `/api/probes` | survey |
| Topology `topology` | `Topology` + engine (layout.jsx) + `GearThroughput` | nodes, gear, edges (monitoring & network views) | `/api/topology?view=`, `/api/gear`, `/api/gear/{id}/interfaces` | — |
| Alerts `alerts` | `AlertsScreen` (screens.jsx) | alert events + rules (tolerances) | `/api/alerts`, `/api/rules` | ack alert, rule CRUD |
| Discovery `discovery` | `Discovery` + `AdoptModal` (screens3.jsx) | discovered candidates (mDNS/ARP/portscan/LLDP), enrichment | `/api/discovery?status=` | scan, adopt, ignore, toggle auto |
| Provisioning `provision` | `Provisioning` + `AgentDirectory`/`AgentModal` (screens3.jsx) | enrollments, build artifacts, remote deploy, fleet imaging | `/api/enrollments`, `/api/builds`, `/api/control/fleet-catalog` | approve/reject, provision, deploy, decommission, image |
| Config & Rules `settings` | `ConfigScreen` (screens3.jsx) | global config (probe/gossip/discovery), alert rules | `/api/config`, `/api/rules` | save config, rule CRUD |
| (global) | `CommandPalette`, `TopBar`, `Sidebar`, `Toasts`, login | operator, summary counts, SSE ticks | `/api/auth/*`, `/api/stream` (SSE) | logout |

**Endpoint registration** (`dashboard/api/`): read routes in `routes.php`
(summary, nodes, probes, alerts, topology, gear, provisioning — note `GET /api/discovery`
lives in `routes/provisioning.php`); mutations + SSE in `routes_mutations.php`
(discovery_mut, enrollments_mut, control, config, pools, assets, stream). Auth routes in
`routes/auth.php` are the only ones reachable without a session.

**Same entity across views** (linkage map):
- A **node** appears in Fleet (row), NodeDetail, Topology (glyph), Alerts (source), Command
  palette (jump), and as a probe **vantage** (monitor) in Reachability.
- An **asset** (adopted system) appears in Fleet (reachability-only row), Systems list,
  AssetDetail, and as a probe **target host** in Reachability/ServiceDetail.
- A **probe target** appears in Reachability (row), ServiceDetail, and AssetDetail (target list).

---

## 3. Reachability screen — root cause & fix  ✅ FIXED + VERIFIED

### Symptom
The Reachability view hangs / stays blank on the **live** dashboard (works with the offline
fixture / DEMO DATA).

### Root cause
`GET /api/probes` returns, per vantage, a `monitorNode` id but **no `monitorName`**. The
column builder in `screens2.jsx` did:

```js
S.probes.forEach(p => p.vantages.forEach(v => map.set(v.monitorNode, v.monitorName))); // name = undefined
[...map.entries()].map(([id,name]) => ({id,name}))
  .sort((a,b) => a.name.localeCompare(b.name));   // undefined.localeCompare -> THROWS
```

With live data every `name` is `undefined`, so the sort throws
`TypeError: Cannot read properties of undefined (reading 'localeCompare')`. The exception
propagates out of the component render; with no error boundary the whole route paints blank
(the "hang"). The offline fixture ships `monitorName` on every vantage, which is why the bug
only manifests against the live API.

Why `monitorName` was missing: the SQL in `routes/probes.php` selected `monitorNode` only and
never resolved a name. Worse, on this deployment the two reporting monitors
(`9042290951195531063`, `11306471821965834356`) are **not** rows in the `node` table (they are
monitor outposts, never enrolled), so nothing in the DB carried a name for them — a naive JOIN
alone would still yield NULL.

### Fix (additive, three defensive layers)
1. **`dashboard/api/routes/probes.php`** — `LEFT JOIN node` to pick up the monitor's
   `hostFqdn` when it *is* an enrolled node, plus a new `ProbeRollup::monitorLabel()` that
   returns the short hostname, or a stable synthesized `mon-<last4 of id>` fallback when the
   monitor is not in the roster. Each vantage now always carries a non-empty `monitorName`.
2. **`dashboard/public/api.jsx`** (`mapProbe`) — `monitorName` defaults to
   `mon-<last4>`/`"monitor"` if a payload ever omits it (guards older/partial payloads).
3. **`dashboard/public/screens2.jsx`** (`monCols`) — synthesizes a label if one is missing and
   sorts with `String(a.name).localeCompare(String(b.name))`, so the render can never throw
   again even on unexpected data.

### Verification
- `php -l routes/probes.php` → clean.
- Live `GET /api/probes` (authenticated) now emits `"monitorName":"mon-1063"` / `"mon-4356"`
  on every vantage; all boot endpoints return 200 in <200 ms (no server-side hang).
- Reproduced the exact failure and the fix against the **live** payload in Node:
  old `monCols` → `Cannot read properties of undefined (reading 'localeCompare')`;
  new `monCols` → `[{id:…,name:"mon-1063"},{id:…,name:"mon-4356"}]`, no throw.
- Deployed `api.jsx` + `screens2.jsx` to `/var/www/solarinet` (docroot is www-data; SW is
  network-first for app code so a reload picks them up — no cache bump needed).

### Secondary follow-up (not blocking, noted)
`/api/probes` also omits `hostNode`, so Reachability's per-target "Open host …" button calls
`onOpenNode(undefined)` and is a silent no-op with live data. Low priority; fix by having the
probes route resolve the target's `assetId`/`nodeId` and the SPA route to AssetDetail.

---

## 4. Discovery augmentation plan — mDNS names + port-scan service fingerprinting

Good news: the schema and enrichment pipeline already exist (PR #9, gated by
`SOLARI_WITH_DISCOVERY_TOOLS`). `discovered` has `via ENUM('mdns','arp','scp_advert','portscan','lldp')`,
`services` (JSON like `["ssh:22","http:80"]`), and enrichment columns `mac,vendor,osName,
deviceRole,sysDescr,enrichedAt`. The SPA already renders `via`, `services`, `vendor`, `osName`,
`deviceRole`, `mac`. So this is mostly **filling** existing fields plus a modest surface.

### 4a. mDNS / zeroconf / avahi names
- **C backend (delegated):** in the discovery/enrichment worker, run an avahi/mDNS resolve
  (`avahi-resolve -a`, or browse `_services._dns-sd._udp`) per host; write the `.local`
  hostname into `discovered.host` and, when the mDNS record advertises services
  (`_http._tcp`, `_ssh._tcp`, `_ipp._tcp`…), merge those into `services` and set `via='mdns'`.
  Store the friendly instance name (e.g. "Brother HL-2270DW") — a new `mdnsName VARCHAR`
  column is the clean home for it (keeps `host` a resolvable name).
- **PHP:** if `mdnsName` is added, surface it in `routes/provisioning.php` (the discovery GET
  map) and `mapDiscovered` in api.jsx. Otherwise zero PHP change — `host`/`services`/`via`
  already flow through.
- **JSX (`Discovery`, screens3.jsx):** show the mDNS instance name as the row's friendly title
  (fall back to `host`/`ip`), and render a "mDNS" origin chip alongside the existing `via` tag.
  ~15 lines, additive.

### 4b. Light port-scan / service fingerprint per host
- **C backend (delegated):** the active scan verb (`DISCOVER`, TCP connect) already yields
  `name:port`. Add an optional light fingerprint per open port: banner grab / `nmap -sV`
  (already available under `SOLARI_WITH_DISCOVERY_TOOLS`) capped to a small port set and short
  timeout to stay lightweight. Extend each `services` entry to carry product/version, e.g.
  `{"port":22,"name":"ssh","product":"OpenSSH","version":"9.6"}` (make the JSON tolerate both
  the legacy `"ssh:22"` string and the new object form). Keep the scan cap (~/20) documented.
- **PHP:** `mapDiscovered` passes `services` through untouched today; when the shape becomes an
  object, no server change is required — just ensure `AdoptModal`'s port parser handles both
  forms.
- **JSX:** in `Discovery`, render each service chip with its product/version (tooltip or
  sub-label); in `AdoptModal`, the `svcList` parser (currently `String(s).split(":")`) must
  accept the object form `{port,name,product}`. ~25 lines, additive, with a back-compat branch.

**Owner split:** the actual avahi/nmap probing and DB writes are **C** (delegate). The dashboard
work is **JSX** (chips/labels + object-form parsing) plus a one-line **PHP** passthrough if new
columns are introduced.

---

## 5. Cross-view linkage + new-infra insight plan (prioritized)

New infrastructure to surface (per memory / directory-services docs): **AD on radium**,
**Keycloak SSO** (already wired for login), **BIND DNS**, a new **MariaDB System-of-Record**,
and a **coming RabbitMQ**. These are services on hosts, so the cleanest model is to adopt each
as a monitored **asset** with per-service probe targets, then add light service-specific
insight panels. Priorities below are ordered by value/effort.

| # | Improvement | What / where | Owner | Effort |
|---|---|---|---|---|
| P1 | **Reachability → AssetDetail click-through** | Fix the `hostNode`/`assetId` gap so a probe row/target opens the owning system (§3 secondary). Have `/api/probes` return `assetId`; SPA routes to AssetDetail. | PHP + JSX | S |
| P2 | **Universal entity click-through** | Make every node/asset/target reference a link: Topology glyph → NodeDetail/AssetDetail; Alerts source → detail; AssetDetail target → ServiceDetail (already partial). Standardize an `openEntity()` in app.jsx. | JSX | M |
| P3 | **Infra "Services" pool + adopt AD/Keycloak/BIND/MariaDB/RabbitMQ** | Adopt each as an asset in a "Core Infrastructure/Services" pool with the right probe targets (LDAP/LDAPS 389/636, Keycloak 8443/health, DNS 53 udp+tcp, MySQL 3306, AMQP 5672 + mgmt 15672). Mostly data/config via existing adopt flow. | JSX/config (probes = C) | M |
| P4 | **Service-aware health checks (beyond TCP)** | Extend probe target types with app-layer checks: DNS query resolves, LDAP bind (anon/whoami), HTTP 200 on Keycloak `/health/ready`, MySQL ping, AMQP handshake. Backend verb + a `probeType` column; SPA shows the richer outcome. | C-backend + JSX | L |
| P5 | **SSO/auth panel in Config** | Surface OIDC state (issuer, client, enabled) read-only from `/api/auth/config` + a "test SSO" affordance; today OIDC is configured only via `solari-auth.json`. Show whether AD/directory auth is enabled (the `directory` block stub). | PHP + JSX | S |
| P6 | **DNS insight tile (BIND)** | On the BIND asset's AssetDetail, show a small panel: zone reachability, query latency (from the DNS probe), and NXDOMAIN/SERVFAIL rate if the backend exposes it. Reuses RTTBars/TimeSeries. | JSX (+ C metrics) | M |
| P7 | **System-of-Record (MariaDB) status card** | AssetDetail panel for the MariaDB SoR: up/replication/connection latency; clearly distinguish it from the dashboard's *own* `solarinet` DB. Metric source is a probe/agent check. | C-backend + JSX | M |
| P8 | **RabbitMQ readiness (pre-stand-up)** | Add the AMQP/management probe targets and a queue-depth/consumer tile to AssetDetail so it lights up the moment the broker is deployed. Ship the SPA panel now, gated on data presence (renders nothing until metrics arrive, like GearThroughput). | JSX (+ C metrics) | M |

**Cross-cutting note:** the dashboard already has the right seams for all of this — assets +
pools + probe targets + `AssetDetail` panels that render nothing until data exists
(`GearThroughput` is the template). The heavy lifting (new probe/check types, app-layer
metrics) is C-backend; the dashboard side is additive panels and click-through wiring.

---

## 6. Files changed in this audit

- `dashboard/api/routes/probes.php` — LEFT JOIN node + `ProbeRollup::monitorLabel()`; each
  vantage now carries `monitorName`.
- `dashboard/public/api.jsx` — `mapProbe` defaults `monitorName`.
- `dashboard/public/screens2.jsx` — `monCols` synthesizes label + null-safe sort.
- Deployed `api.jsx` + `screens2.jsx` to `/var/www/solarinet`.
- `docs/SolariNet_Dashboard_Audit.md` — this document.

No git commits made (per instructions). A temporary local test user added to
`solari-auth.json` for authenticated verification was removed; the file is byte-identical to
its pre-audit state.
