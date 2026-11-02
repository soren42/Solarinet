# Inventory I1 WP0 audit

Scope: SELECT-only audit of the live SoR on cesium, plus the named repository
consumers and migration/test conventions. No live data was changed. Credentials
were read from `/etc/solarinet/prov-sor.cnf` into a mode-0600 temporary option
file under `/dev/shm`, with its group rewritten to `[client]`; the file was
shredded after each connection. No credential values are recorded here.

## Live SoR: observed

- Server/database: cesium, MariaDB 12.3.2, `sor`.
- `hardware_models`: 41 rows, IDs 1–9105. Existing enum `hw_type` includes
  19 populated values. One row has non-NULL `arch`; 3 models have notes.
- `hardware_units`: 43 rows; every unit joins to a model through required
  `model_id`. Existing inventory columns include quantity, cost, stock status,
  and receipt reference. No product-type or licence-reference columns exist.
- `barcodes`: 3 rows, all `subject_table='hardware_units'` and
  `code_type='asset_tag'`. No product-type subjects or retail barcode rows.
- The `dashboard` provenance source exists. Hardware-model source counts are
  dashboard 3, netdb 35, scanner-ingest 3.
- The hardware-model/unit/barcode FKs are restrictive by default, with the
  existing location/receipt references using `SET NULL`. The new installed-on
  reference can follow that convention.

## Repository consumers: observed

- `dashboard/api/routes/inventory.php` reads/writes `hardware_models` and
  `hardware_units`; model filters, validation and compatibility output depend
  on `hw_type`, and units depend on `model_id`.
- `dashboard/api/routes/rackwire.php` and its static data/UI adapters use
  `hardware_models`/`hw_type`; `dashboard/api/routes/provisioning.php` and
  `deploy/fleet/prov-stage.py` join units to models and read `arch`.
- `dashboard/api/routes/inv_codes.php` centralizes allowed polymorphic barcode
  subjects in `InvCodes::SUBJECTS`; `dashboard/public/screens8.jsx` has a
  display-label map for the current subjects. WP1 changes the validator list
  only. The route's subject-card and label builders still cover the original
  four subjects; full product-type card behavior remains with later API/UI work.
- `netdb/sor/seed.py` upserts models by make/model and units by external
  reference. It does not yet populate product-type IDs. Existing clients remain
  compatible because WP1 retains their legacy columns and relationships.
- No matching `hardware_models` or `hw_type` consumer was found under `src/`.

## Migration and test conventions: observed

- Migration IDs are zero-padded SQL filenames, ordered numerically by
  `tools/migrate.py`; the runner records checksums in its ledger and refuses
  unmanaged live schemas. The repository currently contains migrations through
  031. 032 is reserved by the parallel task; this work uses 033.
- Migrations use MariaDB DDL, `IF NOT EXISTS`/information-schema guards where
  supported, and SoR FK/index/provenance naming conventions. Manual down scripts
  live under `netdb/sor/rollback/`; 031 demonstrates a preflight procedure that
  signals before destructive DDL.
- `tests/netdb/test_prov_arch_refresh_031.sh` creates uniquely named scratch
  databases and refuses to run if they already exist. The shared
  `require_scratch_sor_db.sh` is fail-closed to explicit loopback scratch DBs
  plus a sentinel row. The new migration test follows the create/verify/drop
  scratch-database pattern and uses no live SoR name.
- `netdb/sor/SCHEMA.md` describes the generated snapshot convention. The
  snapshot is exported from the full schema plus ordered migrations; it is not
  the migration ledger.

## Mapping decision: inferred and documented in migration

The `hw_type` enum has no exact equivalent in the broader `kind` enum. WP1
uses a stable coarse mapping: `printer` and `scanner` become `peripheral`,
`other` remains `other`, and every other legacy class becomes `hardware`.
Legacy `hw_type` remains authoritative for existing dashboard/provisioning
consumers during the compatibility period. The optional `(brand, model,
deleted_at)` uniqueness index is omitted: nullable `deleted_at` does not enforce
live-row uniqueness in MariaDB's ordinary unique-key semantics, so adding it
would imply a guarantee it cannot provide.
