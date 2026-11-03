# Inventory I1 — WP6 data rollout and count verification

**Date:** 2026-11-03 · **Author:** Lead (Opus 5.5) · **Mode:** read-only SELECTs against live `sor` (cesium, MariaDB 12.3.2) and the `sor-replica` container on benzene.

## Staged migrations (already applied per WP)

| Migration | WP | Applied | Backup |
|---|---|---|---|
| 033 inventory catalog | WP0/WP1 | 2026-11-02 | `sor-pre-m033-20261102T232911Z.sql.gz` |
| 034 prov_images OS FK | (OS identity) | 2026-11-03 | — |
| 035 catalog display names | WP2 | 2026-11-03 | `sor-pre-m035-20261103T025528Z.sql.gz` |
| 036 inventory locations | WP3 | 2026-11-03 | `sor-pre-m036-20261103T030347Z.sql.gz` |

WP4 and WP5 are application-only (no DDL). D6 (Painted Fern 9104 → 9103) was executed 2026-11-02 (`audit_log` id 17); WP6 carries no merge.

## Count verification

| Check | Primary | Replica | Expected |
|---|---:|---:|---|
| `hardware_models` live | 43 | 43 | = product types |
| `product_types` live | 43 | 43 | one per model (IDs preserved) |
| models with no product type of the same id | 0 | — | 0 |
| `hardware_units` live | 45 | — | unchanged |
| units with NULL `product_type_id` | 0 | — | 0 |
| units with `product_type_id <> model_id` | 0 | 45 match | 0 |
| `product_barcodes` live | 0 | — | 0 (none seeded; D3, no mockups) |
| `locations` total / live | 2 / 1 | 2 | 9103 live, 9104 soft-deleted |
| live `loc_type` distribution | property=1 | property=1 | Painted Fern only |
| live roots that are not `property` | 0 | — | 0 |
| live children of a deleted parent | 0 | — | 0 |
| `container_types` | 0 | 0 | 0 (operator-created) |
| `solari_migration_ledger` rows | 21 | 21 | 21; `migrate.py --verify-live` MATCH |

**Result: PASS.** The backfill is complete and the replica is in sync. The hierarchy satisfies the 036 rules.

## UNVERIFIED

- References outside the SoR, such as printed labels or external backups, can't be checked from here.
- Data the operator enters after this date isn't covered. Re-run these SELECTs after the first real inventory session.
