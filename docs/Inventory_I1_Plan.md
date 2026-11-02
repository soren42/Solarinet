# Inventory I1 Implementation Plan

**Status:** reviewed draft (Codex author; Claude cross-lab review 2026-11-02) for approved I1 remit. **Scope:** catalog foundation, location hierarchy and safe merge/move, reusable project containers. **Out of scope:** web product lookup, scanner dispatch and record edit/delete (I2+), unified search (I4), printer transport/templates (I5), catalog content imported from mockups, and merge/delete execution before operator review of Appendix A.

## Evidence convention

- **Observed** means directly visible in repository state or read by SELECT from the configured live SoR.
- **Inferred** means design recommendation or implication, pending implementation/operator decision.

## Current state

| Area | Observed | I1 consequence |
|---|---|---|
| Persistence | `dashboard/api/lib/Sor.php` connects inventory to MariaDB SoR using `SOR_DB_*`; inventory is authoritative there. | Extend SoR with numbered migrations; never use the monitoring DB. |
| Catalog | `hardware_models` has required make/model and hardware enum; `hardware_units.model_id` is required. Migration 014 adds inventory attributes; 015 adds polymorphic `barcodes`. | Introduce general `product_types`; preserve model and unit IDs/relationships through backfill. |
| Locations | `locations` has `parent_id`, `name`, enum `loc_type`, nullable `deleted_at`; migration 001 dropped `uq_locations_parent_name` for a plain `idx_locations_parent_name(parent_id,name)` (live `SHOW CREATE TABLE` confirms: no sibling uniqueness at all); 014 widened the enum. | Migrate levels and enforce sibling uniqueness across roots and children among live rows. |
| API | `dashboard/api/routes/inventory.php` uses Router closures, `Inv` validation helpers, prepared PDO statements and standard `Response::error` envelopes. Reads require an authenticated session; writes are subject to operator policy and CSRF. | Add endpoints following Router and Policy conventions, not direct ad-hoc handlers. |
| UI | `dashboard/public/screens4.jsx` is the inventory screen; `screens8.jsx` implements Tags & Scan. UI is vanilla JSX in existing dashboard runtime. | Extend Inventory screen for hierarchy and batch actions; no build or bundler. |
| Tests | `tests/dashboard/test_inventory_http.php` refuses anything except loopback scratch SoR DBs matching `solarinet_*`; migration guard exists under `tests/netdb`. | Add unit and HTTP coverage on isolated scratch DB only. |
| Locations conflict | Live SoR `sor` on `cesium` has live root rows 9103 and 9104, both named Painted Fern and `loc_type=site`. Appendix A (independently re-verified 2026-11-02) found zero references of any kind; they are the only two location rows in the SoR. | Merge executed 2026-11-02 (D6); the merge endpoint is still built for future duplicates. |

## Numbered schema migrations

**Inferred proposal:** reserve the next unused migration numbers after checking branch/main at implementation time; use existing zero-padded numbered style. Suggested `032_inventory_catalog.sql`, `033_inventory_locations_containers.sql`. Migrations are additive/forward-only operationally; rollback scripts are compensating steps and must refuse destructive reversal when new references/data exist.

### 032 — broadened product catalog

1. Create `product_types` with `id INT UNSIGNED AUTO_INCREMENT PK`; `kind ENUM('hardware','peripheral','component','software','accessory','consumable','other') NOT NULL`; `name VARCHAR(160) NOT NULL`; nullable `brand VARCHAR(128)`, `model VARCHAR(128)`, `category VARCHAR(128)`, `specs JSON`, `notes TEXT`, `image_url VARCHAR(1024)`, `lookup_source VARCHAR(64)`, `lookup_ref VARCHAR(255)`; standard SoR provenance (`source_id`, `asserted_kind`, `asserted_at`), timestamps, `deleted_at`.
2. Add indexes `ix_product_types_kind(kind,deleted_at)`, `ix_product_types_name(name)`, and nullable-row dedupe index on `(brand,model,deleted_at)` only if data audit shows it is safe. Do not require brand/model and do not invent catalog rows from design mockups.
3. Add `hardware_models.product_type_id INT UNSIGNED NULL`, index and FK `ON DELETE RESTRICT`. One-to-one uniqueness for non-null links is enforced by unique index on `product_type_id` (NULLs allowed). Keep legacy `make/model/hw_type/arch` while existing API and provisioning consumers migrate.
4. Add `hardware_units.product_type_id INT UNSIGNED NULL`, indexed FK `ON DELETE RESTRICT`. During compatibility window populate it from model link; after staged client rollout, require non-null in a later migration, not I1's first migration.
5. Create `product_barcodes`: `id BIGINT UNSIGNED PK`; `product_type_id INT UNSIGNED NOT NULL FK RESTRICT`; `code VARCHAR(128) NOT NULL`; `code_type ENUM('gtin','sku','other') NOT NULL DEFAULT 'other'`; `source_id`, assertion fields, timestamps, `deleted_at`; generated live marker `live_flag TINYINT AS (IF(deleted_at IS NULL,1,NULL)) PERSISTENT`; unique `(code,live_flag)`; indexes `(product_type_id,deleted_at)` and `code`. No lookup cache/provider work in I1.
6. Extend `barcodes.subject_table` validation (`InvCodes::SUBJECTS` and any enum) to `product_types`, per remit §4.1, so a product type can carry a SolariNet system code. `product_barcodes` remains the separate namespace for commercial GTIN/UPC/EAN/SKU codes.
7. Software follows the hardware pattern (decision D1): one `product_types` row (`kind='software'`) per product, one `hardware_units` row per installed instance. Add nullable `licence_seats INT UNSIGNED`, `licence_expires_on DATE`, `licence_reference VARCHAR(1024)` and `installed_on_unit_id INT UNSIGNED` (FK `hardware_units(id)` `ON DELETE SET NULL`, indexed; must point at a non-software unit; no self-reference) to `hardware_units`, plus index `(licence_expires_on,deleted_at)`. Assignment is the SQL relation installation → hardware running it (D2), not a person. Reference only; never store key text, token, or secret.

### Backfill and rollback for 032

- Backfill one `product_types` row per existing `hardware_models` row with `product_types.id = hardware_models.id` (remit §4.1: "preserving IDs and links"; decided, not open). `product_types` is created empty in the same migration, so there is no collision; set `AUTO_INCREMENT` above `MAX(hardware_models.id)` afterwards. Observed live: 41 models, IDs 1–9105. No mapping table.
- Map kind from `hw_type` with a documented deterministic translation; default hardware. Map name from existing `model`, brand from `make`, model from `model`, notes copied. Preserve arch and source/provenance on hardware_models; product type provenance should identify migration/dashboard source and mark machine-derived only if schema supports it, otherwise human-asserted migration with note.
- Set hardware_units.product_type_id by joining model map; do not rewrite `model_id`. Preserve serials, asset tags, receipts, allocations and existing barcode rows unchanged.
- Do not auto-convert existing generic `barcodes` rows into `product_barcodes` (D3): barcode-derived data only ever **prefills an editable form**, and is committed when the operator saves. Backfill creates no barcode associations; live data has none to convert (3 system codes only).
- Validate counts: models mapped 1:1, units mapped to corresponding product type, no dangling references, barcode unique-live conflicts enumerated. Write counts into migration log/release record.
- Rollback: first stop application writes; verify no product types/barcodes/licence fields were created/edited after migration. Delete only migration-created product rows and mapping rows, then drop FKs/indexes/columns/tables in reverse order. If post-migration references exist, rollback exits/refuses and requires forward repair. Never cascade-delete user-created data.

### 033 — location levels, live sibling uniqueness, storage container metadata

1. Replace `loc_type` enum with `ENUM('property','floor','room','section','storage_unit') NOT NULL` (widen enum → UPDATE → narrow); map legacy `site`/`building`→property; `floor`→floor; `room`→room; all other current types (`rack`,`shelf`,`position`,`zone`,`cabinet`,`drawer`,`tray`,`bin`,`container`)→storage_unit. Preserve ID, parent, code, notes, provenance, dates. Audit invalid parent/child combinations before enabling enforcement. Observed live: the only rows are the two `site` roots, so the mapping table only matters for fixtures and other installs.
2. Add `location_name_key VARCHAR(128) GENERATED ALWAYS AS (IF(deleted_at IS NULL,name,NULL)) PERSISTENT`; replace old unique key with `uq_locations_live_sibling(parent_id,location_name_key)`. MariaDB permits multiple NULLs in unique keys, so root uniqueness needs generated normalized parent sentinel; use generated `parent_key BIGINT UNSIGNED AS (IFNULL(parent_id,0)) PERSISTENT` plus unique `(parent_key,location_name_key)`. Confirm 0 cannot be a real location ID.
3. Add `container_type_id INT UNSIGNED NULL` on `locations`. Create editable `container_types(id INT UNSIGNED PK,name VARCHAR(64) NOT NULL, source/provenance, timestamps, deleted_at)` with live unique name generated-key pattern. Rename and soft-delete are both allowed, **including in-use types** (D5): existing containers keep their reference and show the type as retired; it leaves pick lists. Nothing (types, locations, product types) is ever auto-deleted when empty. Seed only generic types already present in existing rows if explicit types are inferable; avoid mockup-derived catalog population.
4. Add `assigned_project_id INT UNSIGNED NULL` FK to projects `ON DELETE SET NULL`, index `(assigned_project_id,deleted_at)`. Existing `projects.tray_location_id` remains compatible; assignment is explicit container ownership. Empty/release clears project assignment while retaining location ID/code and physical identity.
5. Add DB triggers or equivalent enforced stored procedure constraints for valid parent-child levels and acyclic parent chain only if the MariaDB trigger/privilege model supports safe recursive checks; otherwise enforce all writes through transactional API and add DB constraints/triggers to prevent bypass where feasible. Record this residual in implementation review.
6. Add indexes for `parent_id`, `(loc_type,deleted_at)`, `container_type_id`; do not remove `code` or barcodes.

**Migration preflight:** SELECT duplicates by normalized parent/name (including roots), invalid type transitions, cycles/orphans, and duplicate existing container codes. Stop before DDL that enforces uniqueness if conflicts exist; migration should fail with a concise conflict report rather than silently rename or merge locations.

## API plan

All routes use the existing `/api/inventory/...` prefix and JSON envelopes. **Observed convention:** authenticated session for reads; operator/admin role for writes; mutations require `X-Solari-CSRF` matching session token through global policy. Error response shape is `error.code`, message, HTTP status. Specific route policy declarations must be registered centrally in `dashboard/api/lib/Policy.php` consistent with nearby route entries.

| Method / path | Role / CSRF | Validation and behavior | Errors |
|---|---|---|---|
| `GET /api/inventory/product-types` | authenticated / n/a | `q`, `kind`, `limit`, `offset`; exclude soft-deleted by default; paged response | `bad_request` 400 |
| `GET /api/inventory/product-types/{id}` | authenticated / n/a | positive ID, live row | `bad_request` 400, `not_found` 404 |
| `POST /api/inventory/product-types` | operator / required | required trimmed name, allowed kind; optional strings and JSON object; reject licence secret-like field names | `bad_request` 400, `duplicate_barcode` 409 (if supplied), `service_unavailable` 503 |
| `PUT /api/inventory/product-types/{id}` | operator / required | partial allowlist; optimistic conflict policy; name/kind and lengths checked | `bad_request` 400, `not_found` 404, `conflict` 409 |
| `GET /api/inventory/product-barcodes?code=` | authenticated / n/a | Exact normalized code only; returns product type or no match; no provider/web lookup and no item creation | `bad_request` 400 |
| `POST /api/inventory/product-barcodes` | operator / required | attach validated code/type; unique live code; `gtin` syntax/check digit optional validation only when declared GTIN | `bad_request` 400, `not_found` 404, `duplicate_barcode` 409 |
| `DELETE /api/inventory/product-barcodes/{id}` | operator / required | soft-delete and audit; no physical record delete | `not_found` 404 |
| `GET /api/inventory/locations` | authenticated / n/a | retain existing tree/flat and pagination shapes; include level and container metadata | existing errors |
| `POST /api/inventory/locations` | operator / required | validate parent transition: root property only; property→floor; floor→room; room→section; storage unit under **any** non-storage level (property, floor, room, section) so containers can sit off-site/"In transit" (remit §4.2: "moved anywhere"); storage unit may contain storage units (D4: bag inside a crate), so the move cycle guard applies to container nesting too. Trim name, enforce live sibling uniqueness | `bad_request` 400, `duplicate_location` 409, `not_found` 404 |
| `PUT /api/inventory/locations/{id}` | operator / required | patch name/type/code/notes/container type/project; validate proposed transition and descendants atomically | `bad_request` 400, `not_found` 404, `duplicate_location` 409, `location_cycle` 409 |
| `POST /api/inventory/locations/{id}/move` | operator / required | body `parent_id` nullable; reject self/descendant, invalid level, sibling collision; transaction and audit old/new parent | `bad_request` 400, `not_found` 404, `location_cycle` 409, `duplicate_location` 409 |
| `POST /api/inventory/locations/{id}/merge` | operator / required | body `target_id`, preview required confirmation token/counts; merge source into target only when same level and compatible names/types; never hard delete | `bad_request` 400, `not_found` 404, `merge_conflict` 409 |
| `POST /api/inventory/locations/bulk-storage-units` | operator / required | parent, count 1–500, prefix/start or explicit names, container_type_id, optional project_id; prevalidate all names and generated codes; single transaction all-or-none | `bad_request` 400, `duplicate_location` 409, `not_found` 404 |
| `PUT /api/inventory/locations/{id}/project` | operator / required | set/clear project assignment; require storage_unit; release requires empty container (no live child locations or live items assigned there) | `bad_request` 400, `not_found` 404, `container_not_empty` 409 |

**Audit:** use existing `audit_log` semantics for each changed location and `barcode_scans` only for scan actions (not relevant to I1). Return IDs and changed fields, never secret values. Existing handler error conventions include `bad_request`, `not_found`, `forbidden`, `service_unavailable`, and 409 conflict codes; add named codes only with tests and API docs.

## Move, merge, and assignment algorithms

### Cycle-safe move (inferred)

1. Begin transaction; lock the moving location, its proposed parent, and relevant descendant chain/target sibling key using `SELECT ... FOR UPDATE`.
2. Reject moving node to itself. Walk ancestors from proposed parent with visited-ID set and depth cap; reject if source is encountered, repeated node is found, or cap exceeded. Lock rows as traversed to serialize competing moves.
3. Validate `loc_type` transition for source under new parent and every child against the source's unchanged type; validate live sibling uniqueness after move.
4. Update parent and audit old/new values; commit. On any check or SQL uniqueness failure, rollback and map deterministic conflict code. Tests include two concurrent opposing moves and corrupted pre-existing cycles.

### Merge (inferred)

1. Preview endpoint shows source/target IDs, names/types, subtree counts, direct item counts, barcodes, project assignments and FK/soft-reference counts. Require same location level and explicit operator confirmation of source/target direction. The two Painted Fern rows are not merged automatically by deploy or migration.
2. Start transaction and lock source, target, descendants, item rows, project references, barcodes and all known FK references. Recompute preview fingerprint/counts; stale confirmation returns `merge_conflict`.
3. Refuse merge if target/source level differs; resulting child names conflict; moving children would violate level rules; a cycle would result; a non-movable external hard FK exists; or project/container assignments disagree and cannot be resolved without operator choice. No implicit child rename.
4. Move source's child locations to target; update direct `hardware_units.location_id`, `entities.location_id`, `power_circuits.location_id`, `projects.tray_location_id`, `locations.assigned_project_id` references, and any other discovered FK/soft reference according to reference semantics. For polymorphic references (`barcodes`, `barcode_scans`, `external_refs`), preserve history and identity; do not rewrite append-only scan snapshots. Retarget live barcode registry rows only if code remains unique and explicit API contract allows it; otherwise retain source as soft-deleted alias or refuse.
5. Preserve source barcode/reference/history. Mark source `deleted_at` and record merge target in audit note/new values; do not hard delete. Commit atomically. Return moved counts. Implement idempotency key or deterministic retry semantics.
6. For Painted Fern, operator gets report before merge; this plan reports zero references as of observed query, not authorization to merge. Proposed direction: keep **9103** (older, carries `code='1'`), retire 9104.

**Open design issue:** existing barcodes and other polymorphic references have no FK and may contain arbitrary table strings. Implementation must inventory DB metadata and known route writers before declaring merge completeness.

## UI plan

- **iPad Safari primary:** inventory hierarchy browser with expandable Property → Floor → Room → optional Section → Storage unit; full path; large touch targets; sticky actions; safe confirmation summary before merge; bulk-create wizard showing generated names/codes and all-or-none result; project assignment and release action; editable container type list.
- **iPhone second:** responsive single-column list, breadcrumb/back navigation, compact inline create/edit; bulk count generation remains usable in a narrow modal; avoid hover-only controls and dense tables.
- Product type forms support all kind values, optional brand/model, notes/specs/image URL; barcode form is manual exact association only. Display “No match” without triggering external lookup.
- Keep existing API adapters and JSX runtime, use existing components/styles, and preserve legacy model screen compatibility during backfill window. No build step, bundler, or server-side JavaScript.
- Catalogue screen starts empty apart from backfilled hardware model rows. Do not seed showcase/mockup objects.

## Test plan (scratch databases only)

No test may point at `sor`, `solarinet`, `solarinet_stage`, or `sor_probe`. Existing HTTP harness explicitly requires loopback and a `solarinet_*` scratch database; retain its guard and add a second explicit denylist.

- Unit tests for product-type validation, GTIN check-digit helper if included, location transition matrix, name normalization/uniqueness key, path renderer, merge preview fingerprint, and refusal conditions.
- Migration tests: clean install from schema snapshot through 033; fixture backfill for ID mapping, NULL brand/model, units, collisions, rollback refuses after writes, rollback succeeds on untouched fixture; no mockup seeds.
- PHP API tests on disposable MariaDB scratch DB: auth/session read, operator role, missing/bad CSRF, required fields, lengths, enum values, exact barcode lookup only, uniqueness race (409), create/update/soft-delete, hierarchy transitions, cycle-safe move, sibling duplicate at root and child, bulk 1/500/501 and atomic failure, release empty/nonempty, merge preview stale fingerprint and every refusal path, successful merge preserving audit/history.
- UI tests under existing frontend test harness for iPad/iPhone responsive paths, keyboard wedge-compatible text entry, bulk generated names, and confirmation preview; no browser or production DB needed for API logic.
- Cross-lab review gate per remit before live rollout; separate from implementation tests.

## Work packages (separate implementation/review units)

| WP | Size | Output | Gate |
|---|---:|---|---|
| WP0 Schema/data audit | S | SELECT-only inventories, conflicts, mappings, numbered migration allocation | Operator reviews duplicate and collision reports |
| WP1 Product schema + migration/backfill | M | 032 migration, rollback, scratch migration tests | Independent schema review |
| WP2 Catalog API | M | product types/barcodes endpoints, policy and PHP API tests | API contract review |
| WP3 Location schema + invariants | L | 033 migration, preflight, constraints/triggers strategy, rollback | DB concurrency/constraint review |
| WP4 Move/merge/container API | L | transactional endpoints, bulk/release, audit, scratch tests | Independent algorithm review; operator sees Painted Fern report before merge execution |
| WP5 Inventory UI | L | responsive hierarchy/catalog/bulk/project flows in existing JSX | iPad Safari and iPhone manual acceptance |
| WP6 Data rollout | S | staged migration, count verification, explicit operator-approved Painted Fern merge execution | No merge until specific authorization after report |

## Risks

1. Current location uniqueness semantics and NULL-root behavior may contain additional conflicts; preflight must halt rather than auto-repair.
2. Existing enum consumers may break when legacy `loc_type` values are remapped; search all route/UI/tests and provisioning readers before migration.
3. Product-type mapping of `hw_type` to broad kind is lossy; preserve old fields and make mapping explicit.
4. Existing `barcodes` is polymorphic and soft-deleted codes may duplicate; catalog barcode uniqueness needs a clear namespace/policy.
5. MariaDB generated columns, unique constraints, migration locking, and trigger privileges vary by deployment version; verify on scratch clone matching server version.
6. Merge has many FK and non-FK references; report zero in known tables does not prove there are no unmodeled references.
7. A location can represent both a physical place and reusable container; storage-unit project assignment/release semantics need operator confirmation.
8. Live DB credentials may be able to write; operational procedure must enforce SELECT-only report query and separate tested scratch credentials for implementation.
9. Licence assignment and expiry data may be sensitive; reference-only policy prohibits storing keys or licence text.

## Operator decisions (Jason, 2026-11-02)

| # | Decision |
|---|---|
| D1 | Software mirrors hardware: one product type per product, one item per instance. |
| D2 | A software item is assigned to the system it is installed on, as an FK installation → hardware unit; not to a user. |
| D3 | Product barcode data only prefills editable fields; nothing barcode-derived is committed without operator save. |
| D4 | Containers nest: storage unit → storage unit allowed. |
| D5 | Container types support rename and delete; nothing auto-deletes when empty (items are often transient). |
| D6 | Painted Fern duplicate is an integration artifact. **Executed 2026-11-02:** 9104 soft-deleted into 9103 (`audit_log` id 17); zero references moved. WP6 no longer carries a merge. |

## UNVERIFIED

- No migration or application change was implemented or exercised; this is a plan.
- Proposed migration numbers must be reconciled with the current branch before implementation.
- MariaDB version-specific generated-column/DDL/trigger behavior has not been verified on a scratch clone.
- Painted Fern: an `information_schema` FK scan (5 hard FKs) and a column-name scan were run 2026-11-02. References outside the SoR (backups, printed labels carrying `code=1`) cannot be checked from here.
- “Labels” in current schema are represented by barcode rows and label print timestamps; no separate label table was found by repository search. External backups/logs may still refer to these IDs.
- Container physical-code retention and barcode collision behavior during merge require explicit API design.

## Appendix A — Painted Fern dependency report (READ ONLY)

**Observed query target:** database `sor`, server `cesium`, identified by `Sor.php` (`SOR_DB_*`, default database `sor`) and `/etc/solarinet/prov-sor.cnf` (read locally; credentials omitted). The first invocation using that file failed because its group is `[sor]`; the corrected client-format invocation succeeded. All successful SQL statements were `SELECT`/metadata reads. No DB writes, merge, or delete were issued.

| Reference class | Query | Count for 9103/9104 | Example rows |
|---|---|---:|---|
| Locations directly parented to either | `locations.parent_id IN (9103,9104)` | 0 | none |
| Items/assets directly assigned | `hardware_units.location_id IN (...)` | 0 | none |
| Entity location overrides | `entities.location_id IN (...)` | 0 | none |
| Project tray links | `projects.tray_location_id IN (...)` | 0 | none |
| Barcode/label records | `barcodes.subject_table='locations' AND subject_id IN (...)` | 0 | none |
| Scan/history snapshots | `barcode_scans.subject_table='locations' AND subject_id IN (...)` | 0 | none |
| External soft references | `external_refs.subject_table='locations' AND subject_id IN (...)` | 0 | none |
| Power circuit location refs | `power_circuits.location_id IN (...)` | 0 | none |
| Audit history rows | `audit_log.table_name='locations' AND row_id IN (...)` | 0 | none |

**Independent re-verification (Claude, 2026-11-02, read-only, MariaDB 12.3.2):** confirmed zero across all classes above, plus `sor_outbox.row_id`. `information_schema` lists exactly 5 hard FKs to `locations` (entities, locations.parent_id, hardware_units, projects.tray_location_id, power_circuits), and all are 0. `audit_log` has no `locations` rows at all, so the two creates were not audited. `barcodes` holds only 3 `hardware_units` rows. These two rows are the entire `locations` table.

**Observed records:** `9103 | NULL | Painted Fern | site | code=1 | created 2026-07-23 17:51 | live`; `9104 | NULL | Painted Fern | site | code=NULL | created 2026-08-07 04:00:01 | live`. The second create's machine-like timestamp suggests a re-run seed (inferred). No direct dependent rows existed in the queried classes, therefore there are no example dependent rows to list. `barcodes` are the current label/code table; scan rows snapshot subject IDs. Existing schema also has a hard FK `locations.parent_id → locations.id`, `hardware_units.location_id → locations.id`, `entities.location_id → locations.id`, `projects.tray_location_id → locations.id`; these are represented in the direct counts above. No separate label table was found in the searched schema/migrations.

**Scope limitation / inference:** this is not proof that every soft reference in every application dataset is absent. The database query enumerated known schema columns (`parent_id`, `location_id`, `tray_location_id`, `subject_id`/`subject_table`) plus audit `table_name/row_id`, and queried known location-dependent tables. Future merge implementation must dynamically inspect all FKs and re-run dependency discovery, including broader history or external systems, immediately before any merge. No merge/delete is authorized by this report.
