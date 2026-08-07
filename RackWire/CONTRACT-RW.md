# CONTRACT-RW v1.0 — RackWire backend integration

Authoritative design contract for integrating RackWire (browser-based connection
planner, see `RackWire/HANDOFF.md`) into SolariNet. All work packages build against
this document. Deviations require Lead approval.

Worktree: `.claude/worktrees/rackwire`, branch `feat/rackwire` (off main).
**Never edit files in `/home/jason/Code/Solarinet` proper — that tree serves the
live PHP API.**

---

## 0. Architecture decisions (locked)

| # | Decision | Rationale |
|---|---|---|
| D1 | RackWire ships as a self-contained app under `dashboard/public/rackwire/` (`index.html` + `device-library.js` + `live-adapter.js` + `support.js`), served same-origin from `/var/www/solarinet/rackwire/`. | No new Apache config; session cookie rides along; standalone URL works; iPad file:// offline use preserved. |
| D2 | Dashboard embed = same-origin iframe screen (`screens-rackwire.jsx`) driving the existing postMessage bridge (`{source:'rackwire', cmd, args}`). First iframe in the SPA; `X-Frame-Options SAMEORIGIN` already permits it. | RackWire is not a Babel-JSX module; porting = rewrite; HANDOFF §4 requires standalone to keep working. |
| D3 | Telemetry transport = `solarinet-rest` polling `GET /api/rackwire/telemetry` with session-cookie auth (no token; fetch default `credentials: same-origin` suffices). No WebSocket — none exists and persistent PHP workers are forbidden by architecture. | Dashboard brief §7. `solarinet-ws` stays registered but unused. |
| D4 | Plan persistence = new SoR tables (`rw_plans`, `rw_plan_versions`) on cesium via `Sor::db()` (authoritative, 503 when unreachable), with client-side localStorage fallback for file:// / offline. | Plans are human-curated infra records; SoR replicates to benzene; commit-to-SoR then shares one DB/txn. |
| D5 | Power modeling extends the existing SoR shape rather than a parallel domain: power inlets/outlets are `interfaces` rows (new `if_kind` values), power cords are `interconnects` (new `link_kind 'power'`), plus a new `power_circuits` table hanging off `locations`. | SoR brief §2/§8: reuse existing structures, no new edge tables. |
| D6 | Committing a plan to SoR writes `interconnects` (asserted_kind='human', a dedicated `sources` row for RackWire) and `locations` racks/positions. Machine `lldp_adjacency` rows are read-only context, never treated as planned cables — filter on `link_kind` + `asserted_kind`. | SoR brief §2, §4. |
| D7 | Barcode lookup = existing `POST /api/inventory/codes/lookup` (operator-gated; writes an audit scan row). No new barcode service. | Migration 015 anticipated this. |
| D8 | Device library served from SoR `hardware_models` (+ optional per-unit rows), merged client-side over the static `RW.DEVICES` seed. Port-geometry (`groups`) not in SoR yet → new `rw_model_groups` JSON column/table in migration 020 keyed to `hardware_models`. | Library must round-trip the `groups` schema in HANDOFF §6. |
| D9 | Theming: token bridge stylesheet mapping `--sn-*` → dashboard tokens (`--sn-accent: var(--accent)` etc.), scoped in the rackwire app; SVG `tok()` re-resolves on a `MutationObserver` of `documentElement[data-theme]` (iframe inherits by copying parent's data-theme via bridge or reading `localStorage["solari-theme"]`). `--sn-signal-other`/`--sn-signal-open` stay literal. Standalone-from-disk keeps the existing `var(--sn-x, #fallback)` fallbacks. | Dashboard brief §6: `--sn-*` does not exist dashboard-side. |
| D10 | No CSRF tokens (house style — session cookie SameSite=Lax + JSON body); all mutations `Operator::requireOperator()`. Flagged to operator as a known repo-wide gap. | Dashboard brief §3. |

## 1. Migration 020 — `netdb/sor/migrations/020_rackwire.sql`

Single global numbering across `db/migrations/` and `netdb/sor/migrations/`; highest
is 019, so RackWire takes 020. Target DB: `sor` on cesium. Idempotent DDL only,
copying the guard patterns of `014_inventory.sql:86-99` (`CREATE TABLE IF NOT
EXISTS`, `ADD COLUMN IF NOT EXISTS`, information_schema-guarded FK/ENUM changes).
Every new table carries the provenance quintet (`source_id`, `asserted_kind`,
`asserted_at`, `created_at`/`updated_at`/`deleted_at`) and soft-delete-aware
uniques. Contents:

1. `rw_plans` — id, name (unique among live), slug, plan_json (LONGTEXT, JSON_VALID
   check), thumbnail?, updated tracking, provenance quintet.
2. `rw_plan_versions` — plan_id FK, version name, plan_json snapshot, created_at.
   Append-only (no updated_at/deleted_at), cap enforcement in API not schema.
3. `power_circuits` — name, location_id FK, volts, amps, phase, breaker label,
   parent_circuit_id (tree), notes, provenance quintet.
4. ENUM extensions: `interconnects.link_kind` + `'power'`; `interfaces.if_kind` +
   `'power_inlet'`, `'power_outlet'`; interfaces gains nullable `circuit_id` FK →
   power_circuits and nullable `watts_rated` / `poe_class` if not representable
   already. Guard every ENUM alter idempotently.
5. `rw_model_groups` — hardware_model_id FK, groups_json (the HANDOFF §6 `groups`
   array verbatim), plus device-level extras (drawW, budgetW, poeBudgetW, eff,
   isInternet, isBattery) as JSON. One live row per model.
6. A `sources` row for RackWire (INSERT … WHERE NOT EXISTS pattern), name
   `rackwire`, so API code can `SELECT id FROM sources WHERE name='rackwire'`.

No CDC triggers on new tables (sorsync bypass is intended). Writes to `interfaces`
DO fire CDC — acceptable, appliers diff before acting.

### 1.1 Binding column names (addendum, ratified by Lead — M1 and M2 MUST match)

- `rw_plans`: `id`, `name`, `slug`, `plan_json` (LONGTEXT, JSON_VALID), `thumbnail`
  (LONGTEXT NULL), `source_id`, `asserted_kind`, `asserted_at`, `created_at`,
  `updated_at`, `deleted_at`; generated `live_flag` column per `015_barcodes.sql`;
  UNIQUE (`name`, `live_flag`).
- `rw_plan_versions`: `id`, `plan_id` FK→rw_plans (ON DELETE CASCADE),
  `version_name` (as built — supersedes the earlier `name`), `plan_json`,
  `created_by`, `source_id`/`asserted_kind`/`asserted_at`, `created_at` only —
  append-only, no updated_at/deleted_at.
- `rw_model_groups`: `id`, `hardware_model_id` FK→hardware_models (CASCADE),
  `library_id` (RW.DEVICES id for client merge), `groups_json` (LONGTEXT
  JSON_VALID — HANDOFF §6 groups array), `device_json` (as built — supersedes the
  earlier `extras_json`; drawW/budgetW/poeBudgetW/eff/isInternet/isBattery),
  provenance quintet, `live_flag`; UNIQUE (`hardware_model_id`, `live_flag`) and
  UNIQUE (`library_id`, `live_flag`).
- `rw_plans` additionally carries denormalized `device_count`/`cable_count` (list
  endpoint never reads the blob), `updated_by`, `notes`, and UNIQUE (`slug`,
  `live_flag`).
- (v1.1 note: names above reflect the landed migration 020, ratified as-built;
  M2 API code was verified against the real DDL.)
- Sources row: `INSERT IGNORE INTO sources (slug, display_name, kind, endpoint)
  VALUES ('rackwire', 'RackWire connection planner', 'human',
  'https://dashboard.akoria.net/rackwire/')`. API resolves via
  `SELECT id FROM sources WHERE slug = 'rackwire'` (mirrors `Sor::sourceId()`).

## 2. PHP API — `dashboard/api/routes/rackwire.php`

One route file returning `fn(Router): void`, registered wholly in `routes.php`
(as built — ratified deviation matching the `inventory.php`/`inv_codes.php`
precedent: SoR-backed CRUD modules live in `routes.php` regardless of verb;
`routes_mutations.php` is reserved for solariCtl-bridged domains). House rules: `Response::ok/fail` envelope,
`solari_json_body()` for POST bodies, `Coerce` for wire units (raw canonical units,
no display formatting server-side), inline SQL via `Sor::db()` (SoR) and `Db::`
(monitoring), `Operator::requireOperator()` on every mutation, generic 500 via
bootstrap. Endpoints:

| Verb | Path | Notes |
|---|---|---|
| GET | `/api/rackwire/plans` | list (id, name, updated, sans blob) |
| GET | `/api/rackwire/plans/{id}` | full plan_json |
| POST | `/api/rackwire/plans` | create/update (upsert by id), operator |
| POST | `/api/rackwire/plans/{id}/delete` | soft delete, operator |
| GET | `/api/rackwire/plans/{id}/versions` | list snapshots |
| POST | `/api/rackwire/plans/{id}/versions` | save snapshot, cap 25 (prune oldest), operator |
| POST | `/api/rackwire/plans/{id}/restore` | restore snapshot, operator |
| GET | `/api/rackwire/library` | hardware_models ⋈ rw_model_groups → HANDOFF §6 device-definition shape; include per-model unit count |
| POST | `/api/rackwire/library/{modelId}/groups` | upsert rw_model_groups from a library-shaped definition, operator |
| GET | `/api/rackwire/telemetry` | frame endpoint, see §3 |
| POST | `/api/rackwire/commit` | plan → SoR interconnects/locations, operator, see §4 |
| GET | `/api/rackwire/sor/interconnects` | read-only: live cables + lldp_adjacency for overlay/diff, labeled by asserted_kind |

## 3. Telemetry endpoint

`GET /api/rackwire/telemetry?keys=k1,k2,…` (keys = the plan devices'
`liveKey`/`assetId`/`name` set, lowercased by client). Returns the HANDOFF §3 frame
verbatim: `{ts, devices:{key:{reachable,loadW,tempC,cpuPct,uptimeS,…}},
ports:{"key|<ifName>":{link,negotiatedGbps,rxGbps,txGbps,errors}}}`.

Sources (monitoring DB, local): `node`/`hostCurrent` for reachability/cpu/uptime;
`networkGear` + `gearInterfaceCurrent` for per-port `operStatus`,
`speedMbps`→negotiatedGbps, `inRateKbps`/`outRateKbps`→rx/txGbps. Key matching:
case-insensitive against node name and gearId. Port keys use the monitoring
`ifName`; the client maps ifName→internal portId via a per-device `ifMap` (see §5).
Fields with no backing data (PoE watts, optical dBm, SFP temp) are omitted — frame
schema tolerates absence. No loops/sleeps — single bounded query set per poll; call
`session_write_close()` before querying (multi-tab safety).

## 4. Commit-to-SoR semantics

Input: `{planId}` or inline plan + `{dryRun}`. For each planned cable whose both
endpoints resolve to SoR entities (via device `assetId` → `hardware_units.asset_tag`
or entity match): upsert an `interconnects` row (link_kind from cable domain:
net→copper/fiber by connector, power→'power'; label = cable label; source =
rackwire; asserted_kind='human'). Racks → `locations` rows (loc_type='rack',
rack_units). Never touch rows whose `source_id` ≠ rackwire's; never write
`lldp_adjacency`. Response: `{created, updated, skipped:[reason]}`. `dryRun:true`
returns the same shape without writing — the UI's preflight. All writes in one
transaction.

## 5. Frontend work (in `dashboard/public/rackwire/` + SPA touches)

1. Move app files to `dashboard/public/rackwire/` (rename `Connection
   Planner.dc.html` → `index.html`); keep `?embed=1`, `?view=`, `?live=` flags.
2. Token bridge `<style>` (D9) + `tok()` re-resolve on data-theme changes; theme
   handshake from parent via existing bridge `theme()` or localStorage read.
3. Persistence adapter: `load()`/`persist()` try `/api/rackwire/plans` first
   (same-origin fetch), fall back to localStorage on network/401/file:// —
   standalone offline must keep working byte-for-byte.
4. `handleCode()` middle swapped for `POST /api/inventory/codes/lookup`; bind
   dialog stays as fallback for unknown codes.
5. Live preset: settings default source `solarinet-rest`, url
   `/api/rackwire/telemetry?keys=…` (keys recomputed from plan on start), no token.
   `config.transform` maps `key|ifName` → `key|portId` using per-device `ifMap`
   (editable in inspector next to liveKey/assetId; persisted in plan).
6. Library merge: fetch `/api/rackwire/library`, merge into `RW.DEVICES` by id
   (server wins), before first render; offline → seed array as today. Respect
   `migrate()` — any new device/group field added to `DEV_FIELDS`/`GROUP_FIELDS`.
7. SPA integration (four-touch pattern): `screens-rackwire.jsx` (iframe +
   postMessage bridge: theme push, focus/select relay, "open standalone" link),
   `index.html` script tag + rackwire iframe src with `?v=` bump, NAV entry
   (Manage group), `app.jsx` destructure + route + command palette. Icon: reuse or
   register one glyph in `icons.jsx`.

## 6. Tests (must be runnable in CI `dashboard` job)

- `php -l` clean over all touched PHP (already in CI).
- `tests/dashboard/test_rackwire_api.php` — instantiate Router, register rackwire
  routes, assert route table + envelope behavior with a stubbed DB (pattern:
  `test_solari_ctl.php`, plain executable, exit code).
- `tests/dashboard/test_rackwire_ui.js` — parse `screens-rackwire.jsx` (Babel
  parse, pattern `test_jsx_parse.js`) + sanity-check the token bridge maps every
  `--sn-*` token used by the app.
- Migration: syntax-validate 020 against a scratch MariaDB/`solarinet_stage`-style
  DB before any live apply; **never** point tests at live `sor`.
- Wire the two new tests into `.github/workflows/ci.yml` `dashboard` job.

## 7. Deploy (Lead-executed, after review + tests)

1. Stage-validate 020 on a scratch DB; then `sudo mariadb sor <
   netdb/sor/migrations/020_rackwire.sql` on cesium (replicates to benzene).
2. Merge `feat/rackwire` → main via PR (github + forgejo remotes).
3. In live tree: pull; `sudo cp -r dashboard/public/. /var/www/solarinet/` +
   chown; asset `?v=` bump already in commit. PHP live from tree — pull is deploy.
4. Smoke: login → RackWire screen renders; plans save/load; telemetry frames flow;
   barcode lookup round-trips; standalone URL + file:// both work.

## 8. Out of scope (v1)

Known accepted limits (Lead-ratified at review):
- `rackwire/index.html` loads its three sub-scripts without `?v=` — deliberate
  (query strings are untested on the iPad file:// path); staleness is covered by
  the SW's network-first strategy for non-vendor assets.
- `support.js` (vendored, do-not-edit per HANDOFF) has a second wildcard
  message listener limited to `__dc_theme`/`__dc_design_mode` — same class as
  the fixed bridge issue but theme-toggle reach only; revisit if support.js is
  ever re-vendored.


Multi-user conflict resolution (last-write-wins + versions only), WebSocket/SSE
telemetry, rack elevation view, CORS/token auth for off-origin standalone, PoE
telemetry (no data source), camera scanning on iOS.
