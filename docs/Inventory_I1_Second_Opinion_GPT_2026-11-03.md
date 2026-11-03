> **Provenance:** independent read-only review by GPT-5.5 (Codex CLI, reasoning effort high) of Forgejo main `48bd836`, run 2026-11-03 at Jason's request as the final I1 step. The report below is verbatim except that `/tmp/merge-wp5/` link prefixes were made repo-relative. Lead adjudication follows the report.

# Inventory I1 — Independent Second-Opinion Review (GPT)

## Executive assessment

**Verdict: Conditional readiness, 3/5.** I1 delivers a substantial catalog and location foundation: operators can manage product types and commercial codes, and the location UI supports hierarchy editing, moves, merges, bulk storage units, container types, and project assignment. The main remit gap is that the new catalog does not yet provide a complete path from a manually created type to an item: item creation still requires a legacy hardware model, and the software licence fields exist only in the schema. Location codes also lack the uniqueness guarantee implied by the migration preflight and bulk flow. Transactions and merge safeguards are generally well designed; timestamp-based optimistic locking can miss rapid same-second updates, and deadlocks may surface as 500s. API authorization is consistent with the manifest. The permitted UI and PHP checks passed; I could not run the real-DB HTTP suite or independently verify live SoR state.

## Scorecard

| Dimension | Rating 1–5 | One-line rationale |
|---|---:|---|
| Functional | 3 | Catalog and location workflows work; general item creation and licence management do not reach the UI/API. |
| Stability | 3 | Mutations are mostly transactional and merge is guarded; concurrency edge cases remain. |
| Integration | 4 | Route registration, API adapters, authorization manifest, and full schema snapshot align; inventory has no SoR outbox CDC. |
| Completeness | 3 | Most location capabilities are present; manual software/item requirements are only partially delivered. |
| Tests | 3 | Mocked UI tests and scratch-DB suites are substantial, but I1 migration rehearsals are not wired into this CI workflow. |
| Docs | 4 | The API reference is detailed; its general pagination limit conflicts with the product API’s actual cap. |

## Capability trace

| Remit capability | UI | API | Schema | Status | Evidence |
|---|---|---|---|---|---|
| Create/edit product types across the catalog kinds; attach and look up a known commercial barcode | Catalog supports create/edit, barcode attach/remove, and exact lookup. A miss offers a manual type form with the code prefilled. | Product type CRUD and barcode attach/exact lookup routes are registered. Lookup does not call an external provider. | `product_types` supports the remit kinds; `product_barcodes` has a live-code uniqueness key. | **Complete for I1 manual catalog and known-code lookup.** | [screens4.jsx](../dashboard/public/screens4.jsx:1766), [screens4.jsx](../dashboard/public/screens4.jsx:1829), [product_catalog.php](../dashboard/api/routes/product_catalog.php:123), [033_inventory_catalog.sql](../netdb/sor/migrations/033_inventory_catalog.sql:20) |
| Unknown retail barcode → free web lookup → confirm candidate → create N items | A miss only offers a manually prefilled type form. | Exact local lookup only; no provider adapter or batch-item endpoint. | No I1 lookup cache/provider tables. | **Partial; provider lookup and scan-to-N-items are deferred to I3.** | [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:70), [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:87), [product_catalog.php](../dashboard/api/routes/product_catalog.php:123), [screens4.jsx](../dashboard/public/screens4.jsx:1833) |
| Manually create physical or software items, including licence seats, expiry, installation target, and licence reference | The catalog form creates product types and explicitly says individual items are recorded in Components; it has no item licence fields. | Component creation requires `model_id`; it does not accept `product_type_id` or licence fields. | Licence fields and `installed_on_unit_id` exist on `hardware_units`, but legacy `model_id` remains non-null. | **Partial; this is an in-scope R2 gap.** | [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:20), [screens4.jsx](../dashboard/public/screens4.jsx:2086), [inventory.php](../dashboard/api/routes/inventory.php:331), [033_inventory_catalog.sql](../netdb/sor/migrations/033_inventory_catalog.sql:77) |
| Property → Floor → Room → optional Section → Storage unit, with full path and sibling uniqueness | Hierarchy UI supports create/edit, breadcrumbs, and location paths; storage units can nest. | Create, edit, move, and database triggers enforce level transitions and live sibling uniqueness. | Migration 036 adds normalized levels, generated sibling keys, and transition/cycle triggers. | **Complete for the hierarchy and moves.** | [screens4.jsx](../dashboard/public/screens4.jsx:1011), [screens4.jsx](../dashboard/public/screens4.jsx:1062), [location_admin.php](../dashboard/api/routes/location_admin.php:33), [036_inventory_locations.sql](../netdb/sor/migrations/036_inventory_locations.sql:137) |
| Hundreds of reusable project containers; bulk create, type list, assign, move, release | Bulk wizard previews generated names/codes; project and type controls are present. | Bulk API accepts 1–500 per transaction; project assignment and empty-only release are implemented. | Locations store `container_type_id` and `assigned_project_id`; container types are soft-deletable. | **Complete for the stated reusable-container workflows, except label creation in the same step.** | [screens4.jsx](../dashboard/public/screens4.jsx:1358), [screens4.jsx](../dashboard/public/screens4.jsx:1469), [location_admin.php](../dashboard/api/routes/location_admin.php:211), [location_admin.php](../dashboard/api/routes/location_admin.php:313) |
| Place units in locations | Locations show components assigned there; existing Components flow can create/update location for a legacy model. | Component APIs accept `location_id`, but still require legacy `model_id`. | `hardware_units.location_id` remains available alongside the new product link. | **Partial for new general catalog types.** | [screens4.jsx](../dashboard/public/screens4.jsx:1053), [inventory.php](../dashboard/api/routes/inventory.php:335), [inventory.php](../dashboard/api/routes/inventory.php:391) |
| Labels rendered as image-only for chosen printer transport | No I1 label-image workflow is present in these tabs. | No I1 label-generation endpoint. | No separate label-image model added by I1. | **Deferred to I5; not counted as an I1 gap.** | [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:76), [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:87) |
| Scan SolariNet codes to open/edit/delete records | Catalog scan is a commercial product-barcode lookup, not general system-code dispatch. | I1 adds product barcode routes; item/location/project dispatch is not delivered here. | Existing barcode registry remains separate from commercial product barcodes. | **Deferred to I2.** | [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:64), [Inventory_Remit_2026-11-01.md](../docs/Inventory_Remit_2026-11-01.md:87), [033_inventory_catalog.sql](../netdb/sor/migrations/033_inventory_catalog.sql:46) |

## Findings

1. **High — General manual items and software licences are not operable through UI → API.** The remit accepts manual product and item entry, including software licence fields. I1 stores the licence columns, but Components creation requires `model_id`, accepts neither `product_type_id` nor licence fields, and the catalog form only creates product types. An operator can create a software type but cannot create its installed instance, record its licence reference or seats, or assign its installation target through the dashboard. **Confidence: Observed.** [Remit R2](../docs/Inventory_Remit_2026-11-01.md:20), [UI item guidance](../dashboard/public/screens4.jsx:2086), [component API](../dashboard/api/routes/inventory.php:331), [schema fields](../netdb/sor/migrations/033_inventory_catalog.sql:77)

2. **Medium — Location codes can be duplicated after migration.** Migration 036 preflights existing duplicate location codes, and the bulk API checks generated codes, but the added location unique key covers sibling names only. Single-location create passes `code` directly into the insert without checking it. Thus two operators—or one operator using ordinary create/edit—can assign the same code, making code-based identification ambiguous. **Confidence: Observed.** [036 preflight](../netdb/sor/migrations/036_inventory_locations.sql:78), [036 indexes](../netdb/sor/migrations/036_inventory_locations.sql:137), [single create](../dashboard/api/routes/inventory.php:571), [bulk code check](../dashboard/api/routes/location_admin.php:273)

3. **Medium — Timestamp optimistic locks can accept stale writes made within the same second.** Location, product type, and container type updates compare `updated_at`, but the relevant columns are `DATETIME` without fractional-second precision. Two updates in the same second can leave the same timestamp; a stale client can then pass the equality check and overwrite the newer value despite the row lock. Product type updates also permit omitting `updated_at`, disabling the check altogether. **Confidence: Inferred from schema precision and route logic.** [location updated_at schema](../netdb/sor/schema.snapshot.sql:1467), [product updated_at schema](../netdb/sor/schema.snapshot.sql:1777), [product update lock](../dashboard/api/routes/product_catalog.php:77), [location update lock](../dashboard/api/routes/inventory.php:624), [container type lock](../dashboard/api/routes/location_admin.php:369)

4. **Medium — Concurrent opposing location moves can surface as 500s.** A move locks the source and then walks/locks ancestors. Two opposing moves can contend on those rows and deadlock. `loc_transaction()` maps selected database errors, but `loc_map_exception()` rethrows other SQL states, including the expected deadlock state; absent a specialized outer mapping, the operator receives a server error and must retry. **Confidence: Inferred.** [move locking](../dashboard/api/routes/location_admin.php:42), [transaction wrapper](../dashboard/api/routes/location_admin.php:423), [exception mapping](../dashboard/api/routes/location_admin.php:433)

5. **Medium — I1 migration rehearsal scripts are not run by the supplied Forgejo CI workflow.** Scratch migration tests exist for 033, 035, and 036, but the workflow explicitly invokes earlier migration rehearsals and the inventory HTTP/UI suites without invoking those I1 scripts. A regression in I1 preflight, rollback, or MariaDB DDL behavior can therefore escape this CI gate. **Confidence: Observed in the named workflow.** [033 test](../tests/netdb/test_inventory_catalog_033.sh:1), [035 test](../tests/netdb/test_catalog_display_names_035.sh:1), [036 test](../tests/netdb/test_inventory_locations_036.sh:1), [CI migration tests](../.forgejo/workflows/ci.yml:228), [CI dashboard tests](../.forgejo/workflows/ci.yml:843)

6. **Low — API reference overstates the product catalog page-size limit.** The reference says page sizes are 1–5000, while the product-types handler caps its own limit at 500. Current UI requests 50, so this is a contract/documentation error rather than a demonstrated UI failure. **Confidence: Observed.** [API reference](../docs/SolariNet_Inventory_API.html:44), [product API limit](../dashboard/api/routes/product_catalog.php:22)

## Gap analysis

### In-scope gaps

- **R2/manual item entry:** general product types cannot be turned into items through the UI/API; software licence seats, expiry, reference, and installed-on fields are schema-only. This is the principal remit shortfall.
- **Location code uniqueness:** the migration detects existing duplicates but only sibling names are uniquely constrained; ordinary create/edit can introduce duplicate codes.
- **Concurrency behavior:** timestamp locks can miss same-second writes; move deadlocks are not mapped to a deliberate retryable response.
- **CI coverage:** I1 migration rehearsal scripts exist but are not invoked by the supplied Forgejo workflow.

### Explicitly deferred; not counted as I1 gaps

- **I2:** system-code scan dispatch, record views with edit/delete for items, locations, and projects.
- **I3:** free external product lookup and scan → confirm → add N items.
- **I4:** unified search and editable browse views.
- **I5:** label templates, verified printer profiles, and image/share-to-vendor-app printing.

These phase boundaries are stated in the remit and plan. [Remit phasing](../docs/Inventory_Remit_2026-11-01.md:83), [plan scope](../docs/Inventory_I1_Plan.md:3)

## Recommendations

1. **Close the item workflow (Finding 1):** add an item form/API that uses `product_type_id`, supports `location_id`, and exposes the approved software licence fields while retaining the secret-reference-only rule. Keep legacy model projection compatibility explicit.
2. **Enforce location code uniqueness (Finding 2):** decide whether codes are globally unique among live locations, then enforce that in the schema and all write paths, including edit and single create.
3. **Make optimistic locking robust (Finding 3):** use a monotonic version column or fractional-second timestamp, and require the expected version on updates. Add same-second stale-write coverage.
4. **Map transient transaction conflicts (Finding 4):** return a documented retryable conflict for deadlocks/serialization failures, and test concurrent opposing moves.
5. **Wire I1 migration tests into CI (Finding 5):** invoke 033/035/036 scratch rehearsals in the migration job against disposable MariaDB matching the supported runtime.
6. **Correct the API page-size documentation (Finding 6):** document the product catalog’s 500-row cap or raise the route cap to match the published contract.

## Method and limits

- Read the remit, plan, WP0 audit, WP6 rollout record, API reference, migrations, full schema snapshot, API/UI adapters, authorization manifest, tests, CI workflow, and the supplied first-parent history.
- Ran `node tests/dashboard/test_inventory_locations_ui.js` and `node tests/dashboard/test_inventory_catalog_ui.js`; both passed.
- Ran `php -l` on the three I1 route files, `php tests/dashboard/test_pdo_placeholders.php`, and `php tests/dashboard/test_authz_manifest.php`; all passed. The authz test reported the manifest matched 194 registered routes.
- Did not run `tests/dashboard/test_inventory_http.php` or the migration shell suites: they require a disposable MariaDB scratch database, which was unavailable. No database access was used, so live-state counts and the WP6 rollout result are treated as the Lead’s documented verification, not independently rechecked.
- Did not exercise iPad Safari, concurrent transactions, or printer vendor apps. The full schema snapshot does include the I1 catalog, licence columns, location constraints, and triggers. [SCHEMA.md](../netdb/sor/SCHEMA.md:3), [schema snapshot](../netdb/sor/schema.snapshot.sql:1734)

---

## Lead adjudication (Opus 5.5, 2026-11-03)

| # | Verdict | Action |
|---|---|---|
| 1 Item/licence entry not operable | **Valid remit gap, not an I1 plan miss.** The I1 plan scoped licence fields as schema only (plan §032 step 7), and its API table has no item endpoint. Item records with edit/delete belong to I2. | Carry into I2 as a first-class requirement: item create/edit keyed on `product_type_id`, with `location_id`, licence seats/expiry/reference and `installed_on_unit_id`, reference-only. |
| 2 Location codes not unique | **Valid.** Bulk create checks codes. Single create/edit doesn't, and 036 adds no unique key. | Backlog: needs a decision on global vs per-property scope, then a migration (live-flag unique key) and API mapping to `duplicate_location`. |
| 3 Same-second optimistic lock | **Valid, known** (WP5b follow-up). Optional `updated_at` on product-type PUT is the documented contract, and the UI always sends it. | Backlog: `DATETIME(6)` or a version column; add a same-second stale-write test. |
| 4 Opposing-move deadlock → 500 | **Plausible** (inferred; plan §86 called for this test and it wasn't written). The impact for a single operator is low. | Backlog: map 1213/1205 to a retryable 409 `conflict`, plus the concurrent test. |
| 5 I1 migration rehearsals not in CI | **Valid. Fixed** in this commit: scripts 033–036 gain the 031-style CI mode (TCP + `MYSQL_PWD`) and run as CI steps in both workflows. Verified locally in both sudo and CI mode (32/13/18/23 assertions). | Done. |
| 6 Page-size doc drift | **Valid. Fixed** in this commit (the API reference now states the 500 cap for product types). | Done. |

**Lead's overall view:** the 3/5 "conditional readiness" rating is fair if measured against the full remit. Measured against the I1 plan, every work package WP0–WP6 shipped, plus a gap the plan didn't cover (finding 1). There are no critical or high findings against delivered I1 functionality. Finding 1 is the most important input to I2 planning.
