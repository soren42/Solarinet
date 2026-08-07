<?php
declare(strict_types=1);

/**
 * RackWire integration API (CONTRACT-RW.md §2-§4): plan CRUD + versions,
 * device library, telemetry frames, commit-to-SoR, and a read-only SoR
 * overlay for the connection-planner front end.
 *
 * STORAGE / PROVENANCE: like routes/inventory.php and routes/inv_codes.php,
 * this module is a direct SoR writer (Sor::db(), authoritative — 503 when
 * unreachable), not a solariCtl-mediated mutation. Every insert stamps
 * source_id = rw_source_id() (the 'rackwire' sources row, migration 020 §1.6),
 * asserted_kind='human', asserted_at=NOW(). Every read filters deleted_at IS
 * NULL; every delete SETs deleted_at rather than removing the row. Telemetry
 * (§3) is the one exception: it reads the LOCAL monitoring DB (Db::) — SoR
 * plays no part there.
 *
 * TABLE SHAPES — as landed in netdb/sor/migrations/020_rackwire.sql:
 *
 *   rw_plans           id, name, slug, plan_json (LONGTEXT, JSON_VALID),
 *                       thumbnail (MEDIUMTEXT NULL), device_count/cable_count
 *                       (denormalized ints, kept in sync on every write),
 *                       updated_by (VARCHAR, the operator username), notes,
 *                       + provenance quintet + created_at/updated_at/deleted_at.
 *                       UNIQUE(name, live_flag) and UNIQUE(slug, live_flag).
 *   rw_plan_versions    id, plan_id FK, version_name (VARCHAR NULL — NOT
 *                       `name`), plan_json, created_by, + source_id/
 *                       asserted_kind/asserted_at + created_at only
 *                       (append-only, no updated_at/deleted_at — §1.2).
 *   rw_model_groups     id, hardware_model_id FK (UNIQUE(hardware_model_id,
 *                       live_flag) — one live row per model), library_id
 *                       (VARCHAR NULL, UNIQUE(library_id, live_flag) — the
 *                       RW.DEVICES seed id, not written by this file today),
 *                       groups_json (LONGTEXT JSON_VALID — HANDOFF §6
 *                       `groups` array verbatim), device_json (LONGTEXT NULL
 *                       JSON_VALID — NOT `extras_json` — device-level
 *                       extras: name, kind, ru, note, drawW, budgetW,
 *                       circuitA, circuitV, poeBudgetW, eff, isInternet,
 *                       isBattery), notes, + provenance quintet +
 *                       created_at/updated_at/deleted_at.
 *   sources             a 'rackwire' row, resolved by `slug` (rw_source_id()),
 *                       mirroring Sor::sourceId()'s 'dashboard' lookup.
 *
 * power_circuits, interfaces.circuit_id/watts_rated/poe_class, and
 * interfaces.if_kind += power_inlet/power_outlet also landed in 020 but are
 * OUT OF SCOPE for this file — CONTRACT-RW.md §2's route table has no power-
 * circuit endpoint, and §4's commit semantics only mention locations and
 * interconnects. Left for a follow-up work package.
 *
 * READ/MUTATION SPLIT — DEVIATION, documented for review: CONTRACT-RW.md §2
 * and the WP-2 assignment both say "reads registered in routes.php, mutations
 * in routes_mutations.php". This file does not follow that split. It is a
 * single Sor::db()-writing route file, matching the routes/inventory.php +
 * routes/inv_codes.php precedent exactly: both mix GET and POST handlers in
 * files loaded ONLY from routes.php's $groups loop, never from
 * routes_mutations.php. routes_mutations.php's own header documents its
 * split as being FOR the solariCtl-bridged domains specifically ("EVERY
 * mutation is marshalled to the C server over solariCtl") — that is not true
 * of rackwire.php, which writes Sor::db() directly, so filing its mutations
 * there would be actively misleading. This file returns one
 * fn(Router):void, registered once from routes.php's $groups array
 * (see routes.php), registering both its GET and POST routes on the same
 * $router in one pass — same as inventory.php.
 *
 * COMMIT-TO-SOR CABLE SHAPE: a plan cable is {uid, a:{dev,port}, b:{dev,port},
 * standard, category, label, lengthM, jacket, notes, status} and carries no
 * explicit domain of its own. link_kind is resolved from the ENDPOINT PORT:
 * each device in the plan snapshots its library `groups` array (HANDOFF §6),
 * every group carrying `domain` (net|power|usb|other) and `conn`, and
 * RW.layout() names ports `<group.k>-<n>` — so rw_port_group() walks a device's
 * groups back from the cable's port id and rw_link_kind_for_port() maps
 * domain power -> 'power', domain net -> 'fiber'/'copper' by connector and
 * cable standard, and domain usb/other -> skipped (no SoR link_kind).
 * rw_cable_link_kind()'s standard/category/jacket text heuristic remains only
 * as the fallback for a port that cannot be resolved (device not in the plan,
 * unknown port id, group with no domain); both paths label themselves in the
 * skipped[]/committed detail so the operator can see which one decided.
 */

require_once __DIR__ . '/../lib/Sor.php';
require_once __DIR__ . '/../lib/Operator.php';
require_once __DIR__ . '/../lib/Coerce.php';

/**
 * Telemetry ?keys= ceiling (see rw_telemetry_keys()). The keys become IN(...)
 * placeholders — three times over in the gear query — so an unbounded key list
 * is an unbounded query; a plan with more devices than this is well past
 * anything the planner draws. Keys past the cap are dropped and their devices
 * simply omitted from the frame, which §3's "fields with no backing data are
 * omitted" already tolerates.
 *
 * Declared HERE, above the file-scope `return` below: that return ends
 * execution of this file on require, and unlike function declarations a const
 * is not hoisted — anything after it would never be defined.
 */
const RW_TELEMETRY_MAX_KEYS = 256;

return static function (Router $router): void {
    // ==================================================================
    // Reads (GET)
    // ==================================================================

        // GET /api/rackwire/plans — list (sans plan_json blob).
        $router->get('/api/rackwire/plans', static function (): void {
            $rows = rw_rows(
                'SELECT id, name, slug, updated_at, created_at
                   FROM rw_plans
                  WHERE deleted_at IS NULL
                  ORDER BY updated_at DESC'
            );
            Response::ok(array_map('rw_plan_summary_row', $rows));
        });

        // GET /api/rackwire/plans/{id} — full plan_json.
        $router->get('/api/rackwire/plans/{id}', static function (array $p): void {
            $id  = rw_id_param($p['id']);
            $row = rw_row(
                'SELECT id, name, slug, plan_json, thumbnail, updated_at, created_at
                   FROM rw_plans WHERE id = :id AND deleted_at IS NULL',
                [':id' => $id]
            );
            if ($row === null) {
                Response::error('not_found', "No plan $id", 404);
            }
            Response::ok(rw_plan_full_row($row));
        });

        // GET /api/rackwire/plans/{id}/versions — snapshot list (sans blob).
        $router->get('/api/rackwire/plans/{id}/versions', static function (array $p): void {
            $id = rw_id_param($p['id']);
            if (rw_row('SELECT id FROM rw_plans WHERE id = :id AND deleted_at IS NULL', [':id' => $id]) === null) {
                Response::error('not_found', "No plan $id", 404);
            }
            $rows = rw_rows(
                'SELECT id, plan_id, version_name, created_by, created_at
                   FROM rw_plan_versions
                  WHERE plan_id = :pid
                  ORDER BY created_at DESC, id DESC',
                [':pid' => $id]
            );
            Response::ok(array_map('rw_version_row', $rows));
        });

        // GET /api/rackwire/library — hardware_models ⋈ rw_model_groups,
        // shaped per HANDOFF §6, plus a per-model live unit count.
        $router->get('/api/rackwire/library', static function (): void {
            $rows = rw_rows(
                'SELECT hm.id, hm.make, hm.model, hm.hw_type, hm.notes,
                        g.groups_json, g.device_json,
                        (SELECT COUNT(*) FROM hardware_units hu
                          WHERE hu.model_id = hm.id AND hu.deleted_at IS NULL) AS unitCount
                   FROM hardware_models hm
                   LEFT JOIN rw_model_groups g
                     ON g.hardware_model_id = hm.id AND g.deleted_at IS NULL
                  WHERE hm.deleted_at IS NULL
                  ORDER BY hm.make, hm.model'
            );
            Response::ok(array_map('rw_library_row', $rows));
        });

        // GET /api/rackwire/telemetry?keys=k1,k2,... — frame endpoint (§3).
        $router->get('/api/rackwire/telemetry', static function (): void {
            // Multi-tab safety (stream.php precedent): release the session
            // file lock before querying so this poll never serialises behind
            // (or blocks) any other request from the same browser session.
            session_write_close();

            $keys = rw_telemetry_keys((string) ($_GET['keys'] ?? ''));
            $frame = ['ts' => (int) round(microtime(true) * 1000)];
            if ($keys === []) {
                $frame['devices'] = new stdClass();
                $frame['ports']   = new stdClass();
                Response::ok($frame);
            }

            $devices = [];
            $ports   = [];

            // The key set drives the SQL, so both queries below are bounded by
            // the caller's (deduped, capped) key list rather than loading every
            // node/gear row and filtering in PHP. Matching mirrors
            // rw_match_key(): full value OR its leading dot-label, folded case
            // on both sides (LOWER() here, strtolower() in rw_telemetry_keys()).
            $ph = implode(',', array_fill(0, count($keys), '?'));

            // ---- node / hostCurrent: reachable + cpuPct --------------------
            // No uptime/load-watt/temperature column exists anywhere in the
            // monitoring schema (db/schema.sql) — those frame fields are
            // omitted for every device, per §3's "fields with no backing
            // data are omitted".
            foreach (Db::rows(
                "SELECT n.hostFqdn, n.state, hc.cpuLoadMilli
                   FROM node n
                   LEFT JOIN hostCurrent hc ON hc.nodeId = n.nodeId
                  WHERE LOWER(n.hostFqdn) IN ($ph)
                     OR LOWER(SUBSTRING_INDEX(n.hostFqdn, '.', 1)) IN ($ph)",
                array_merge($keys, $keys)
            ) as $n) {
                $key = rw_match_key($keys, (string) ($n['hostFqdn'] ?? ''));
                if ($key === null) {
                    continue;
                }
                $entry = ['reachable' => $n['state'] === 'up'];
                $cpuPct = rw_cpu_pct($n['cpuLoadMilli'] ?? null);
                if ($cpuPct !== null) {
                    $entry['cpuPct'] = $cpuPct;
                }
                $devices[$key] = ($devices[$key] ?? []) + $entry;
            }

            // ---- networkGear: reachable by recent lastSeenAt ---------------
            $gearKeyByGearId = [];
            foreach (Db::rows(
                "SELECT gearId, name, lastSeenAt
                   FROM networkGear
                  WHERE LOWER(gearId) IN ($ph)
                     OR LOWER(SUBSTRING_INDEX(gearId, '.', 1)) IN ($ph)
                     OR LOWER(name) IN ($ph)",
                array_merge($keys, $keys, $keys)
            ) as $g) {
                $key = rw_match_key($keys, (string) $g['gearId'], (string) ($g['name'] ?? ''));
                if ($key === null) {
                    continue;
                }
                $gearKeyByGearId[(string) $g['gearId']] = $key;
                $reachable = $g['lastSeenAt'] !== null
                    && strtotime((string) $g['lastSeenAt']) >= (time() - 300);
                $devices[$key] = ($devices[$key] ?? []) + ['reachable' => $reachable];
            }

            // ---- gearInterfaceCurrent: per-port link/rx/tx -----------------
            // gearInterfaceCurrent (db/migrations/006_snmp_gear.sql) has no
            // speed or error-counter column, so negotiatedGbps/errors are
            // omitted for every port (same "no backing data" rule).
            if ($gearKeyByGearId !== []) {
                $gearIds = array_keys($gearKeyByGearId);
                $gearPh = implode(',', array_fill(0, count($gearIds), '?'));
                $ifRows = Db::rows(
                    "SELECT gearId, ifName, operStatus, inRateKbps, outRateKbps
                       FROM gearInterfaceCurrent
                      WHERE gearId IN ($gearPh)",
                    $gearIds
                );
                foreach ($ifRows as $if) {
                    $devKey = $gearKeyByGearId[(string) $if['gearId']] ?? null;
                    $ifName = (string) ($if['ifName'] ?? '');
                    if ($devKey === null || $ifName === '') {
                        continue;
                    }
                    $portEntry = ['link' => ((int) ($if['operStatus'] ?? 0)) === 1];
                    if ($if['inRateKbps'] !== null) {
                        $portEntry['rxGbps'] = round(((int) $if['inRateKbps']) / 1000000, 4);
                    }
                    if ($if['outRateKbps'] !== null) {
                        $portEntry['txGbps'] = round(((int) $if['outRateKbps']) / 1000000, 4);
                    }
                    $ports["$devKey|$ifName"] = $portEntry;
                }
            }

            $frame['devices'] = $devices === [] ? new stdClass() : $devices;
            $frame['ports']   = $ports === []   ? new stdClass() : $ports;
            Response::ok($frame);
        });

        // GET /api/rackwire/sor/interconnects — read-only overlay: live
        // cables + LLDP-discovered adjacency, labeled by asserted_kind.
        $router->get('/api/rackwire/sor/interconnects', static function (): void {
            $rows = rw_rows(
                'SELECT ic.id, ic.link_kind, ic.label, ic.length_m, ic.notes,
                        ic.asserted_kind, ic.asserted_at, s.slug AS sourceSlug,
                        COALESCE(ea.name, eaif.name) AS aName,
                        COALESCE(eb.name, ebif.name) AS bName,
                        ifa.name AS aIfName, ifb.name AS bIfName
                   FROM interconnects ic
                   JOIN sources s ON s.id = ic.source_id AND s.deleted_at IS NULL
                   LEFT JOIN entities  ea   ON ea.id   = ic.a_entity_id    AND ea.deleted_at   IS NULL
                   LEFT JOIN entities  eb   ON eb.id   = ic.b_entity_id    AND eb.deleted_at   IS NULL
                   LEFT JOIN interfaces ifa ON ifa.id  = ic.a_interface_id AND ifa.deleted_at  IS NULL
                   LEFT JOIN interfaces ifb ON ifb.id  = ic.b_interface_id AND ifb.deleted_at  IS NULL
                   LEFT JOIN entities  eaif ON eaif.id = ifa.entity_id     AND eaif.deleted_at IS NULL
                   LEFT JOIN entities  ebif ON ebif.id = ifb.entity_id     AND ebif.deleted_at IS NULL
                  WHERE ic.deleted_at IS NULL
                  ORDER BY ic.id DESC
                  LIMIT 5000'
            );
            Response::ok(array_map('rw_interconnect_row', $rows));
        });

    // ==================================================================
    // Mutations (POST) — all Operator::requireOperator()-gated.
    // ==================================================================

        // POST /api/rackwire/plans — create/update (upsert by id).
        $router->post('/api/rackwire/plans', static function (): void {
            $operator = Operator::requireOperator();
            $b = solari_json_body();

            $name      = rw_str_required($b, 'name', 128);
            $planJson  = rw_plan_json_required($b);
            $thumbnail = isset($b['thumbnail']) && is_string($b['thumbnail']) && $b['thumbnail'] !== ''
                ? $b['thumbnail'] : null;
            $notes = isset($b['notes']) && is_string($b['notes']) && $b['notes'] !== '' ? $b['notes'] : null;
            [$deviceCount, $cableCount] = rw_plan_counts($b['plan']);
            $srcId = rw_source_id();

            $id = isset($b['id']) && $b['id'] !== null && $b['id'] !== ''
                ? rw_id_param((string) $b['id']) : null;

            if ($id !== null) {
                rw_plan_for_write($id);
                rw_write(
                    'UPDATE rw_plans
                        SET name = :name, slug = :slug, plan_json = :pj, thumbnail = :th,
                            device_count = :dc, cable_count = :cc, updated_by = :ub, notes = :notes,
                            asserted_kind = \'human\', asserted_at = NOW()
                      WHERE id = :id AND deleted_at IS NULL AND source_id = :src',
                    [':name' => $name, ':slug' => rw_slug($name), ':pj' => $planJson,
                     ':th' => $thumbnail, ':dc' => $deviceCount, ':cc' => $cableCount,
                     ':ub' => $operator, ':notes' => $notes, ':src' => $srcId, ':id' => $id]
                );
            } else {
                rw_write(
                    'INSERT INTO rw_plans (name, slug, plan_json, thumbnail, device_count, cable_count,
                                            updated_by, notes, source_id, asserted_kind, asserted_at)
                     VALUES (:name, :slug, :pj, :th, :dc, :cc, :ub, :notes, :src, \'human\', NOW())',
                    [':name' => $name, ':slug' => rw_slug($name), ':pj' => $planJson,
                     ':th' => $thumbnail, ':dc' => $deviceCount, ':cc' => $cableCount,
                     ':ub' => $operator, ':notes' => $notes, ':src' => $srcId]
                );
                $id = (int) Sor::db()->lastInsertId();
            }

            $row = rw_row(
                'SELECT id, name, slug, plan_json, thumbnail, updated_at, created_at
                   FROM rw_plans WHERE id = :id',
                [':id' => $id]
            );
            Response::ok(rw_plan_full_row($row), 201);
        });

        // POST /api/rackwire/plans/{id}/delete — soft delete.
        $router->post('/api/rackwire/plans/{id}/delete', static function (array $p): void {
            Operator::requireOperator();
            $id = rw_id_param($p['id']);
            rw_plan_for_write($id);
            $n = rw_write(
                'UPDATE rw_plans SET deleted_at = NOW()
                  WHERE id = :id AND deleted_at IS NULL AND source_id = :src',
                [':id' => $id, ':src' => rw_source_id()]
            );
            if ($n === 0) {
                Response::error('not_found', "No plan $id", 404);
            }
            Response::ok(['id' => $id, 'deleted' => true]);
        });

        // POST /api/rackwire/plans/{id}/versions — save snapshot, cap 25
        // (prune oldest), operator. Snapshots the plan's CURRENT persisted
        // plan_json unless the body supplies its own {plan} to snapshot.
        $router->post('/api/rackwire/plans/{id}/versions', static function (array $p): void {
            $operator = Operator::requireOperator();
            $id = rw_id_param($p['id']);
            $b  = solari_json_body();

            $versionName = isset($b['name']) && is_string($b['name']) && trim($b['name']) !== ''
                ? trim($b['name']) : null;
            // Validate the optional inline plan BEFORE opening the transaction:
            // rw_plan_json_required() 400s by exit()ing, which would otherwise
            // abandon an open transaction rather than roll it back.
            $bodyPlanJson = isset($b['plan']) ? rw_plan_json_required($b) : null;
            $srcId = rw_source_id();

            // Insert + count + prune are one transaction, and the parent plan
            // row is locked FOR UPDATE before the count: two operators saving
            // a snapshot of the same plan at once would otherwise both read a
            // pre-insert count of 25, both conclude no prune was due, and leave
            // 27 rows behind the cap. Serialising on the plan row also means
            // the snapshot always pairs with the plan_json the lock holder saw.
            $db = Sor::db();
            $db->beginTransaction();
            try {
                $plan = rw_row(
                    'SELECT id, source_id, plan_json FROM rw_plans
                      WHERE id = :id AND deleted_at IS NULL FOR UPDATE',
                    [':id' => $id]
                );
                if ($plan === null) {
                    $db->rollBack();
                    Response::error('not_found', "No plan $id", 404);
                }
                if ((int) $plan['source_id'] !== $srcId) {
                    $db->rollBack();
                    Response::error('conflict', "Plan $id is owned by another source; RackWire will not snapshot it.", 409);
                }
                $planJson = $bodyPlanJson ?? (string) $plan['plan_json'];

                rw_exec(
                    'INSERT INTO rw_plan_versions
                        (plan_id, version_name, plan_json, created_by, source_id, asserted_kind, asserted_at)
                     VALUES (:pid, :vname, :pj, :ub, :src, \'human\', NOW())',
                    [':pid' => $id, ':vname' => $versionName, ':pj' => $planJson,
                     ':ub' => $operator, ':src' => $srcId]
                );
                $verId = (int) $db->lastInsertId();

                // Cap 25 (§2, HANDOFF §8): prune the oldest beyond the cap for this plan.
                $total = (int) (rw_scalar('SELECT COUNT(*) FROM rw_plan_versions WHERE plan_id = :pid', [':pid' => $id]) ?? 0);
                if ($total > 25) {
                    $excess = $total - 25;
                    rw_exec(
                        'DELETE FROM rw_plan_versions WHERE plan_id = :pid
                          ORDER BY created_at ASC, id ASC LIMIT ' . $excess,
                        [':pid' => $id]
                    );
                }

                $db->commit();
            } catch (\PDOException $e) {
                if ($db->inTransaction()) {
                    $db->rollBack();
                }
                if ((string) $e->getCode() === '23000') {
                    Response::error('conflict', 'Duplicate or constraint violation.', 409);
                }
                throw $e;
            } catch (\Throwable $e) {
                if ($db->inTransaction()) {
                    $db->rollBack();
                }
                throw $e;
            }

            Response::ok(rw_version_row(rw_row(
                'SELECT id, plan_id, version_name, created_by, created_at FROM rw_plan_versions WHERE id = :id',
                [':id' => $verId]
            )), 201);
        });

        // POST /api/rackwire/plans/{id}/restore — restore a snapshot onto
        // the live plan. Body: {versionId}.
        $router->post('/api/rackwire/plans/{id}/restore', static function (array $p): void {
            Operator::requireOperator();
            $id = rw_id_param($p['id']);
            $b  = solari_json_body();

            if (!isset($b['versionId']) || $b['versionId'] === null || $b['versionId'] === '') {
                Response::error('bad_request', 'versionId is required.', 400);
            }
            $versionId = rw_id_param((string) $b['versionId'], 'versionId');
            $srcId = rw_source_id();
            rw_plan_for_write($id);

            $ver = rw_row(
                'SELECT id, plan_json FROM rw_plan_versions WHERE id = :vid AND plan_id = :pid',
                [':vid' => $versionId, ':pid' => $id]
            );
            if ($ver === null) {
                Response::error('not_found', "No version $versionId for plan $id", 404);
            }

            rw_write(
                'UPDATE rw_plans
                    SET plan_json = :pj, asserted_kind = \'human\', asserted_at = NOW()
                  WHERE id = :id AND deleted_at IS NULL AND source_id = :src',
                [':pj' => $ver['plan_json'], ':src' => $srcId, ':id' => $id]
            );

            $row = rw_row(
                'SELECT id, name, slug, plan_json, thumbnail, updated_at, created_at
                   FROM rw_plans WHERE id = :id AND deleted_at IS NULL',
                [':id' => $id]
            );
            if ($row === null) {
                Response::error('not_found', "No plan $id", 404);
            }
            Response::ok(rw_plan_full_row($row));
        });

        // POST /api/rackwire/library/{modelId}/groups — upsert rw_model_groups
        // from a library-shaped device definition (HANDOFF §6).
        $router->post('/api/rackwire/library/{modelId}/groups', static function (array $p): void {
            Operator::requireOperator();
            $modelId = rw_id_param($p['modelId'], 'modelId');
            $b = solari_json_body();

            if (rw_row('SELECT id FROM hardware_models WHERE id = :id AND deleted_at IS NULL', [':id' => $modelId]) === null) {
                Response::error('not_found', "No hardware model $modelId", 404);
            }

            $groups = $b['groups'] ?? [];
            if (!is_array($groups)) {
                Response::error('bad_request', 'groups must be an array.', 400);
            }
            $groupsJson = json_encode(array_values($groups), JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
            if ($groupsJson === false) {
                Response::error('bad_request', 'groups could not be serialised to JSON.', 400);
            }

            $extras = [];
            foreach (['name', 'kind', 'ru', 'note', 'drawW', 'budgetW', 'circuitA', 'circuitV',
                      'poeBudgetW', 'eff', 'isInternet', 'isBattery'] as $key) {
                if (array_key_exists($key, $b)) {
                    $extras[$key] = $b[$key];
                }
            }
            $deviceJson = json_encode($extras, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);

            $srcId    = rw_source_id();
            $existing = rw_row(
                'SELECT id, source_id FROM rw_model_groups WHERE hardware_model_id = :mid AND deleted_at IS NULL',
                [':mid' => $modelId]
            );
            if ($existing !== null) {
                // Port geometry another source asserted is that source's record
                // (§4's ownership rule); refuse rather than silently relabel it
                // as RackWire's. 409 mirrors rw_plan_for_write()'s refusal.
                if ((int) $existing['source_id'] !== $srcId) {
                    Response::error(
                        'conflict',
                        "Port geometry for hardware model $modelId is owned by another source; RackWire will not overwrite it.",
                        409
                    );
                }
                rw_write(
                    'UPDATE rw_model_groups
                        SET groups_json = :g, device_json = :dj,
                            asserted_kind = \'human\', asserted_at = NOW()
                      WHERE id = :id AND deleted_at IS NULL AND source_id = :src',
                    [':g' => $groupsJson, ':dj' => $deviceJson, ':src' => $srcId, ':id' => $existing['id']]
                );
            } else {
                rw_write(
                    'INSERT INTO rw_model_groups (hardware_model_id, groups_json, device_json, source_id, asserted_kind, asserted_at)
                     VALUES (:mid, :g, :dj, :src, \'human\', NOW())',
                    [':mid' => $modelId, ':g' => $groupsJson, ':dj' => $deviceJson, ':src' => $srcId]
                );
            }

            $row = rw_row(
                'SELECT hm.id, hm.make, hm.model, hm.hw_type, hm.notes, g.groups_json, g.device_json,
                        (SELECT COUNT(*) FROM hardware_units hu
                          WHERE hu.model_id = hm.id AND hu.deleted_at IS NULL) AS unitCount
                   FROM hardware_models hm
                   JOIN rw_model_groups g ON g.hardware_model_id = hm.id AND g.deleted_at IS NULL
                  WHERE hm.id = :id',
                [':id' => $modelId]
            );
            Response::ok(rw_library_row($row), 201);
        });

        // POST /api/rackwire/commit — plan → SoR interconnects/locations (§4).
        // Body: {planId} or {plan: <inline plan>}, plus {dryRun}. One
        // transaction; dryRun rolls back after computing the same shape.
        $router->post('/api/rackwire/commit', static function (): void {
            Operator::requireOperator();
            $b = solari_json_body();
            $dryRun = !empty($b['dryRun']);

            $plan = null;
            if (isset($b['plan']) && is_array($b['plan'])) {
                $plan = $b['plan'];
            } elseif (isset($b['planId']) && $b['planId'] !== null && $b['planId'] !== '') {
                $planId = rw_id_param((string) $b['planId'], 'planId');
                $row = rw_row('SELECT plan_json FROM rw_plans WHERE id = :id AND deleted_at IS NULL', [':id' => $planId]);
                if ($row === null) {
                    Response::error('not_found', "No plan $planId", 404);
                }
                $decoded = Coerce::json($row['plan_json']);
                $plan = is_array($decoded) ? $decoded : null;
            }
            if (!is_array($plan)) {
                Response::error('bad_request', 'planId or plan is required.', 400);
            }

            $devices = is_array($plan['devices'] ?? null) ? $plan['devices'] : [];
            $cables  = is_array($plan['cables']  ?? null) ? $plan['cables']  : [];
            $racks   = is_array($plan['racks']   ?? null) ? $plan['racks']   : [];

            $db    = Sor::db();
            $srcId = rw_source_id();
            $created = 0;
            $updated = 0;
            $skipped = [];
            // One line per committed cable naming the link_kind and the rule
            // that chose it, so the operator can see at a glance which cables
            // fell back to the text heuristic. Additive to §4's response shape.
            $committed = [];

            $db->beginTransaction();
            try {
                // ---- racks -> locations (loc_type='rack') -------------------
                foreach ($racks as $r) {
                    if (!is_array($r) || !isset($r['name']) || trim((string) $r['name']) === '') {
                        $skipped[] = 'rack: missing name';
                        continue;
                    }
                    $name  = trim((string) $r['name']);
                    $units = isset($r['units']) && is_numeric($r['units']) ? (int) $r['units'] : null;

                    $existing = rw_row(
                        'SELECT id, source_id FROM locations
                          WHERE parent_id IS NULL AND name = :n AND loc_type = \'rack\' AND deleted_at IS NULL',
                        [':n' => $name]
                    );
                    if ($existing !== null) {
                        if ((int) $existing['source_id'] !== $srcId) {
                            $skipped[] = "rack '$name': owned by another source, not touched";
                            continue;
                        }
                        if (!$dryRun) {
                            rw_exec(
                                'UPDATE locations
                                    SET rack_units = :ru, source_id = :src, asserted_kind = \'human\', asserted_at = NOW()
                                  WHERE id = :id',
                                [':ru' => $units, ':src' => $srcId, ':id' => $existing['id']]
                            );
                        }
                        $updated++;
                    } else {
                        if (!$dryRun) {
                            rw_exec(
                                'INSERT INTO locations (parent_id, name, loc_type, rack_units, source_id, asserted_kind, asserted_at)
                                 VALUES (NULL, :n, \'rack\', :ru, :src, \'human\', NOW())',
                                [':n' => $name, ':ru' => $units, ':src' => $srcId]
                            );
                        }
                        $created++;
                    }
                }

                // ---- devices: resolve plan uid -> SoR entity id -------------
                // $deviceByUid keeps the whole device (its `groups` snapshot in
                // particular) because the cable loop below reads each endpoint
                // port's group to decide link_kind.
                $entityByUid = [];
                $deviceByUid = [];
                foreach ($devices as $d) {
                    if (!is_array($d) || !isset($d['uid'])) {
                        continue;
                    }
                    $deviceByUid[(string) $d['uid']] = $d;
                    $eid = rw_resolve_entity($d);
                    if ($eid !== null) {
                        $entityByUid[(string) $d['uid']] = $eid;
                    }
                }

                // ---- cables -> interconnects ---------------------------------
                foreach ($cables as $c) {
                    if (!is_array($c)) {
                        $skipped[] = 'cable: malformed';
                        continue;
                    }
                    $cid  = isset($c['uid']) ? (string) $c['uid'] : '?';
                    $aEnd = is_array($c['a'] ?? null) ? $c['a'] : null;
                    $bEnd = is_array($c['b'] ?? null) ? $c['b'] : null;
                    $aDev = $aEnd['dev'] ?? null;
                    $bDev = $bEnd['dev'] ?? null;

                    if ($aDev === null || $bDev === null) {
                        $skipped[] = "cable $cid: unterminated end (only fully-landed cables commit)";
                        continue;
                    }
                    $aEnt = $entityByUid[(string) $aDev] ?? null;
                    $bEnt = $entityByUid[(string) $bDev] ?? null;
                    if ($aEnt === null || $bEnt === null) {
                        $skipped[] = "cable $cid: endpoint device did not resolve to a SoR entity";
                        continue;
                    }

                    [$linkKind, $kindDetail] = rw_cable_link_kind_resolved($c, $deviceByUid);
                    if ($linkKind === null) {
                        $skipped[] = "cable $cid: $kindDetail";
                        continue;
                    }
                    $label = isset($c['label']) && is_string($c['label']) && $c['label'] !== '' ? $c['label'] : null;

                    $existing = rw_row(
                        'SELECT id, source_id FROM interconnects
                          WHERE deleted_at IS NULL AND link_kind = :lk
                            AND ((a_entity_id = :a1 AND b_entity_id = :b1)
                              OR (a_entity_id = :b2 AND b_entity_id = :a2))
                          LIMIT 1',
                        [':lk' => $linkKind, ':a1' => $aEnt, ':b1' => $bEnt, ':b2' => $aEnt, ':a2' => $bEnt]
                    );
                    if ($existing !== null) {
                        if ((int) $existing['source_id'] !== $srcId) {
                            $skipped[] = "cable $cid: existing link owned by another source, not touched";
                            continue;
                        }
                        if (!$dryRun) {
                            rw_exec(
                                'UPDATE interconnects
                                    SET label = :lbl, source_id = :src, asserted_kind = \'human\', asserted_at = NOW()
                                  WHERE id = :id',
                                [':lbl' => $label, ':src' => $srcId, ':id' => $existing['id']]
                            );
                        }
                        $updated++;
                    } else {
                        if (!$dryRun) {
                            rw_exec(
                                'INSERT INTO interconnects (a_entity_id, b_entity_id, link_kind, label, source_id, asserted_kind, asserted_at)
                                 VALUES (:a, :b, :lk, :lbl, :src, \'human\', NOW())',
                                [':a' => $aEnt, ':b' => $bEnt, ':lk' => $linkKind, ':lbl' => $label, ':src' => $srcId]
                            );
                        }
                        $created++;
                    }
                    $committed[] = "cable $cid: $linkKind — $kindDetail";
                }

                // dryRun: roll back every write above, keep the tallies.
                if ($dryRun) {
                    $db->rollBack();
                } else {
                    $db->commit();
                }
            } catch (\Throwable $e) {
                if ($db->inTransaction()) {
                    $db->rollBack();
                }
                throw $e;
            }

            Response::ok([
                'created'   => $created,
                'updated'   => $updated,
                'skipped'   => $skipped,
                'committed' => $committed,
                'dryRun'    => $dryRun,
            ]);
        });
};

/* ========================================================================
 * SoR access helpers (Sor::db(); mirrors routes/inventory.php's Inv class).
 * ==================================================================== */

/** @param array<string,mixed> $params @return array<int,array<string,mixed>> */
function rw_rows(string $sql, array $params = []): array
{
    $st = Sor::db()->prepare($sql);
    $st->execute($params);
    return $st->fetchAll();
}

/** @param array<string,mixed> $params @return array<string,mixed>|null */
function rw_row(string $sql, array $params = []): ?array
{
    $st = Sor::db()->prepare($sql);
    $st->execute($params);
    $r = $st->fetch();
    return $r === false ? null : $r;
}

/** @param array<string,mixed> $params */
function rw_scalar(string $sql, array $params = [])
{
    $st = Sor::db()->prepare($sql);
    $st->execute($params);
    $v = $st->fetchColumn();
    return $v === false ? null : $v;
}

/** Plain parameterised write; exceptions propagate (used inside rw_commit's own transaction). */
function rw_exec(string $sql, array $params = []): int
{
    $st = Sor::db()->prepare($sql);
    $st->execute($params);
    return $st->rowCount();
}

/**
 * Parameterised write for the non-transactional mutation handlers: SQLSTATE
 * 23000 (unique/FK violation — e.g. a duplicate live plan name) becomes a
 * clean 409, mirroring Inv::exec().
 */
function rw_write(string $sql, array $params = []): int
{
    try {
        return rw_exec($sql, $params);
    } catch (\PDOException $e) {
        if ((string) $e->getCode() === '23000') {
            Response::error('conflict', 'Duplicate or constraint violation.', 409);
        }
        throw $e;
    }
}

/** The fixed 'rackwire' sources.id (migration 020 §1.6), resolved by slug. */
function rw_source_id(): int
{
    static $id = null;
    if ($id !== null) {
        return $id;
    }
    $found = rw_scalar("SELECT id FROM sources WHERE slug = 'rackwire' AND deleted_at IS NULL");
    if ($found === null) {
        Response::error('service_unavailable', 'RackWire is not registered as a SoR source (migration 020 not applied).', 503);
    }
    $id = (int) $found;
    return $id;
}

/**
 * Fetch a live plan that this request is about to write, or end the request.
 *
 * 404 when the plan does not exist (or is already soft-deleted); 409
 * 'conflict' when it exists but another source asserted it. §4's "never touch
 * rows whose source_id ≠ rackwire's" is stated for the SoR tables the commit
 * path writes, but it holds just as well for RackWire's own tables: a plan row
 * some other writer owns is not RackWire's to overwrite. 409 is the house code
 * for "the row is there, the write is refused" (routes/inventory.php's
 * Inv::exec() reports its trigger and constraint refusals the same way); a 404
 * would be a lie, since the plan is listed by the read endpoints.
 *
 * Callers still scope their UPDATE by `source_id` as well — this check and the
 * write are two statements, and the WHERE clause is what actually makes the
 * write safe.
 *
 * @param array<int,string> $cols extra columns to select beside id/source_id
 * @return array<string,mixed>
 */
function rw_plan_for_write(int $id, array $cols = []): array
{
    $select = implode(', ', array_merge(['id', 'source_id'], $cols));
    $row = rw_row("SELECT $select FROM rw_plans WHERE id = :id AND deleted_at IS NULL", [':id' => $id]);
    if ($row === null) {
        Response::error('not_found', "No plan $id", 404);
    }
    if ((int) $row['source_id'] !== rw_source_id()) {
        Response::error('conflict', "Plan $id is owned by another source; RackWire will not overwrite it.", 409);
    }
    return $row;
}

/** Validate a {id}-style path/body segment is a positive integer (Inv::idParam mirror). */
function rw_id_param(string $raw, string $label = 'id'): int
{
    if (preg_match('/^\d+$/', $raw) !== 1 || $raw === '0') {
        Response::error('bad_request', "$label must be a positive integer.", 400);
    }
    return (int) $raw;
}

/* ========================================================================
 * body-field validation helpers
 * ==================================================================== */

/** @param array<string,mixed> $b */
function rw_str_required(array $b, string $key, int $maxLen): string
{
    $v = $b[$key] ?? null;
    if (!is_string($v) || trim($v) === '') {
        Response::error('bad_request', "$key is required.", 400);
    }
    $v = trim($v);
    if (strlen($v) > $maxLen) {
        Response::error('bad_request', "$key must be at most $maxLen characters.", 400);
    }
    return $v;
}

/** @param array<string,mixed> $b */
function rw_plan_json_required(array $b): string
{
    $plan = $b['plan'] ?? null;
    if (!is_array($plan)) {
        Response::error('bad_request', 'plan is required and must be an object.', 400);
    }
    $json = json_encode($plan, JSON_UNESCAPED_SLASHES | JSON_UNESCAPED_UNICODE);
    if ($json === false) {
        Response::error('bad_request', 'plan could not be serialised to JSON.', 400);
    }
    return $json;
}

function rw_slug(string $name): string
{
    $slug = strtolower(trim($name));
    $slug = preg_replace('/[^a-z0-9]+/', '-', $slug) ?? '';
    $slug = trim($slug, '-');
    return $slug === '' ? 'plan' : substr($slug, 0, 96);
}

/**
 * rw_plans.device_count / cable_count are denormalized from plan_json so the
 * plan-list endpoint never reads the blob (§2 comment in migration 020).
 * Caller has already validated $plan is an array via rw_plan_json_required().
 *
 * @param mixed $plan
 * @return array{0:int,1:int}
 */
function rw_plan_counts($plan): array
{
    $devices = is_array($plan) && is_array($plan['devices'] ?? null) ? $plan['devices'] : [];
    $cables  = is_array($plan) && is_array($plan['cables']  ?? null) ? $plan['cables']  : [];
    return [count($devices), count($cables)];
}

/* ========================================================================
 * row shapers
 * ==================================================================== */

/** @param array<string,mixed> $r */
function rw_plan_summary_row(array $r): array
{
    return [
        'id'        => (int) $r['id'],
        'name'      => $r['name'],
        'slug'      => $r['slug'],
        'updatedAt' => Coerce::iso($r['updated_at'] ?? null),
        'createdAt' => Coerce::iso($r['created_at'] ?? null),
    ];
}

/** @param array<string,mixed> $r */
function rw_plan_full_row(array $r): array
{
    return [
        'id'        => (int) $r['id'],
        'name'      => $r['name'],
        'slug'      => $r['slug'],
        'plan'      => Coerce::json($r['plan_json'] ?? null),
        'thumbnail' => $r['thumbnail'] ?? null,
        'updatedAt' => Coerce::iso($r['updated_at'] ?? null),
        'createdAt' => Coerce::iso($r['created_at'] ?? null),
    ];
}

/** @param array<string,mixed> $r */
function rw_version_row(array $r): array
{
    return [
        'id'        => (int) $r['id'],
        'planId'    => (int) $r['plan_id'],
        'name'      => $r['version_name'] ?? null,
        'createdBy' => $r['created_by'] ?? null,
        'createdAt' => Coerce::iso($r['created_at'] ?? null),
    ];
}

/**
 * hardware_models ⋈ rw_model_groups -> HANDOFF §6 device-definition shape.
 * device_json fields override the hardware_models-derived defaults where
 * present (name/kind/note); the rest (ru, drawW, budgetW, circuitA,
 * circuitV, poeBudgetW, eff, isInternet, isBattery) have no hardware_models
 * analogue and are simply absent (null) until a groups POST sets them.
 *
 * @param array<string,mixed> $r
 */
function rw_library_row(array $r): array
{
    $groups = Coerce::json($r['groups_json'] ?? null);
    if (!is_array($groups)) {
        $groups = [];
    }
    $extras = Coerce::json($r['device_json'] ?? null);
    if (!is_array($extras)) {
        $extras = [];
    }

    $str = static function (array $extras, string $key, $fallback) {
        return isset($extras[$key]) && is_string($extras[$key]) && $extras[$key] !== '' ? $extras[$key] : $fallback;
    };
    $num = static function (array $extras, string $key) {
        return isset($extras[$key]) && is_numeric($extras[$key]) ? (float) $extras[$key] : null;
    };
    $bool = static function (array $extras, string $key) {
        return array_key_exists($key, $extras) ? (bool) $extras[$key] : null;
    };

    return [
        'id'         => (string) $r['id'],
        'vendor'     => $r['make'],
        'name'       => $str($extras, 'name', $r['model']),
        'model'      => $r['model'],
        'kind'       => $str($extras, 'kind', $r['hw_type']),
        'ru'         => isset($extras['ru']) && is_numeric($extras['ru']) ? (int) $extras['ru'] : null,
        'drawW'      => $num($extras, 'drawW'),
        'note'       => $str($extras, 'note', $r['notes']),
        'budgetW'    => $num($extras, 'budgetW'),
        'circuitA'   => $num($extras, 'circuitA'),
        'circuitV'   => $num($extras, 'circuitV'),
        'poeBudgetW' => $num($extras, 'poeBudgetW'),
        'eff'        => $num($extras, 'eff'),
        'isInternet' => $bool($extras, 'isInternet'),
        'isBattery'  => $bool($extras, 'isBattery'),
        'groups'     => $groups,
        'unitCount'  => (int) ($r['unitCount'] ?? 0),
    ];
}

/** @param array<string,mixed> $r */
function rw_interconnect_row(array $r): array
{
    return [
        'id'           => (int) $r['id'],
        'linkKind'     => $r['link_kind'],
        'label'        => $r['label'],
        'lengthM'      => Coerce::float($r['length_m'] ?? null),
        'notes'        => $r['notes'],
        'assertedKind' => $r['asserted_kind'],
        'assertedAt'   => Coerce::iso($r['asserted_at'] ?? null),
        'source'       => $r['sourceSlug'],
        'a'            => ['entity' => $r['aName'] ?? null, 'ifName' => $r['aIfName'] ?? null],
        'b'            => ['entity' => $r['bName'] ?? null, 'ifName' => $r['bIfName'] ?? null],
    ];
}

/* ========================================================================
 * telemetry helpers (§3)
 * ==================================================================== */

/** @return array<int,string> lowercased, deduped, non-empty keys, at most the cap */
function rw_telemetry_keys(string $raw): array
{
    $keys = array_map(
        static fn (string $k): string => strtolower(trim($k)),
        explode(',', $raw)
    );
    $keys = array_values(array_unique(array_filter($keys, static fn (string $k): bool => $k !== '')));
    return count($keys) > RW_TELEMETRY_MAX_KEYS ? array_slice($keys, 0, RW_TELEMETRY_MAX_KEYS) : $keys;
}

/**
 * Resolve $primary (and optionally $secondary) against the caller's key set:
 * full value first, then $primary's leading dot-label (so "xenon.akoria.net"
 * matches a plan key of "xenon") — mirrors discovery.php's node/neighbor
 * matching. Returns the matched key (as it appears in $keys) or null.
 *
 * @param array<int,string> $keys
 */
function rw_match_key(array $keys, string $primary, string $secondary = ''): ?string
{
    $primary = strtolower(trim($primary));
    if ($primary !== '') {
        if (in_array($primary, $keys, true)) {
            return $primary;
        }
        $label = strtok($primary, '.');
        if ($label !== false && $label !== '' && in_array($label, $keys, true)) {
            return $label;
        }
    }
    $secondary = strtolower(trim($secondary));
    if ($secondary !== '' && in_array($secondary, $keys, true)) {
        return $secondary;
    }
    return null;
}

/**
 * hostCurrent.cpuLoadMilli (per-core thousandths-of-a-percent array) ->
 * 0..100 integer, or null when there is no sample (device omitted from the
 * frame's cpuPct rather than reported as 0). Same formula as
 * routes/panel.php's panelLoadPct(), which returns int 0 on empty input
 * because the panel always shows a number; here empty means "omit".
 *
 * @param mixed $cpuLoadMilli
 */
function rw_cpu_pct($cpuLoadMilli): ?int
{
    $loads = is_string($cpuLoadMilli) ? json_decode($cpuLoadMilli, true) : $cpuLoadMilli;
    if (!is_array($loads) || $loads === []) {
        return null;
    }
    $sum = 0.0;
    $count = 0;
    foreach ($loads as $load) {
        if (is_numeric($load)) {
            $sum += (float) $load;
            $count++;
        }
    }
    return $count === 0 ? null : max(0, min(100, (int) round(($sum / $count) / 10.0)));
}

/* ========================================================================
 * commit-to-SoR helpers (§4)
 * ==================================================================== */

/**
 * Resolve a plan device to a SoR entity id: prefer device.assetId matched
 * against a live hardware_units.asset_tag (then that unit's owning entity),
 * else a case-insensitive entities.name match on assetId/liveKey/name in
 * that order. Returns null (the cable's endpoint stays unresolved, and the
 * caller records a skip reason) when nothing matches.
 *
 * @param array<string,mixed> $d
 */
function rw_resolve_entity(array $d): ?int
{
    $assetId = isset($d['assetId']) && is_string($d['assetId']) && trim($d['assetId']) !== ''
        ? trim($d['assetId']) : null;
    if ($assetId !== null) {
        $row = rw_row(
            'SELECT e.id FROM entities e
               JOIN hardware_units hu ON hu.id = e.hardware_unit_id
              WHERE hu.asset_tag = :tag AND hu.deleted_at IS NULL AND e.deleted_at IS NULL
              LIMIT 1',
            [':tag' => $assetId]
        );
        if ($row !== null) {
            return (int) $row['id'];
        }
    }

    foreach (['assetId', 'liveKey', 'name'] as $field) {
        $v = isset($d[$field]) && is_string($d[$field]) ? trim($d[$field]) : '';
        if ($v === '') {
            continue;
        }
        $row = rw_row('SELECT id FROM entities WHERE LOWER(name) = LOWER(:n) AND deleted_at IS NULL LIMIT 1', [':n' => $v]);
        if ($row !== null) {
            return (int) $row['id'];
        }
    }
    return null;
}

/**
 * The cable's own free text: `standard` is a RW.CABLES id ("om3", "dac10"),
 * `category` that entry's display name ("OM3 duplex LC + SFP+ SR"), `jacket`
 * whatever the operator typed. Lowercased and joined for pattern matching.
 *
 * @param array<string,mixed> $c
 */
function rw_cable_text(array $c): string
{
    return strtolower(trim(implode(' ', array_filter([
        (string) ($c['standard'] ?? ''),
        (string) ($c['category'] ?? ''),
        (string) ($c['jacket']   ?? ''),
    ]))));
}

/**
 * The group definition a plan port id belongs to. RW.layout()
 * (RackWire/device-library.js) names every port `<group.k>-<n>`, so the port's
 * group is the one whose `k` the id was built from. Matched with an anchored
 * `-\d+` tail rather than a plain prefix, because group keys contain hyphens
 * themselves ("poe-out-1" belongs to `poe-out`, not to `poe`).
 *
 * @param array<string,mixed> $device a plan device (carries a snapshot of its
 *     library `groups`, per HANDOFF §6's migrate() contract)
 * @return array<string,mixed>|null
 */
function rw_port_group(array $device, string $portId): ?array
{
    $portId = strtolower(trim($portId));
    if ($portId === '') {
        return null;
    }
    $groups = is_array($device['groups'] ?? null) ? $device['groups'] : [];
    foreach ($groups as $g) {
        if (!is_array($g) || !isset($g['k']) || !is_string($g['k']) || $g['k'] === '') {
            continue;
        }
        $k = strtolower($g['k']);
        if ($portId === $k || preg_match('/^' . preg_quote($k, '/') . '-\d+$/', $portId) === 1) {
            return $g;
        }
    }
    return null;
}

/**
 * Is this a fiber run? Decided from the endpoint connectors (`groups[].conn`,
 * where "sfp" is the optical-module cage) plus the cable's own text. Twinax
 * DAC is checked first and wins: it seats in an SFP cage but is copper, so
 * connector alone would misclassify it.
 *
 * @param array<int,string> $conns
 */
function rw_is_fiber(array $conns, string $cableText): bool
{
    $text = strtolower(trim($cableText . ' ' . implode(' ', $conns)));
    if (preg_match('/\bdac\b|dac\d|twinax/', $text) === 1) {
        return false;
    }
    return preg_match('/\bfibre?\b|optic|\baoc\b|aoc\d|\bom[1-5]\b|\bos[12]\b|\blc\b|\bmpo\b|\bsfp\b|\bqsfp\b/', $text) === 1;
}

/**
 * Resolve interconnects.link_kind for one plan cable from its ENDPOINT PORTS
 * (HANDOFF §6 `groups[].domain`/`conn`), which is what §4 actually specifies —
 * "link_kind from cable domain: net→copper/fiber by connector, power→'power'".
 * Falls back to rw_cable_link_kind()'s text heuristic only when neither end's
 * port resolves to a group with a domain (device missing from the plan,
 * hand-edited port id, a group predating the `domain` field).
 *
 * Never returns 'lldp_adjacency' (§4: that link_kind is machine-discovered
 * context, never planned-cable output).
 *
 * @param array<string,mixed> $c
 * @param array<string,array<string,mixed>> $deviceByUid plan devices keyed by uid
 * @return array{0:?string,1:string} [link_kind, detail] — link_kind null means
 *     the cable must be skipped, with `detail` as the reason; otherwise
 *     `detail` names which rule decided, for the committed/skipped report.
 */
function rw_cable_link_kind_resolved(array $c, array $deviceByUid): array
{
    $cableText = rw_cable_text($c);

    $domains = [];
    $conns   = [];
    foreach (['a', 'b'] as $end) {
        $e = is_array($c[$end] ?? null) ? $c[$end] : null;
        if ($e === null || !isset($e['dev'], $e['port'])) {
            continue;
        }
        $dev = $deviceByUid[(string) $e['dev']] ?? null;
        if ($dev === null) {
            continue;
        }
        $group = rw_port_group($dev, (string) $e['port']);
        if ($group === null) {
            continue;
        }
        if (isset($group['domain']) && is_string($group['domain']) && $group['domain'] !== '') {
            $domains[] = strtolower($group['domain']);
        }
        if (isset($group['conn']) && is_string($group['conn']) && $group['conn'] !== '') {
            $conns[] = strtolower($group['conn']);
        }
    }

    $domains = array_values(array_unique($domains));
    if ($domains === []) {
        return [rw_cable_link_kind($c), 'link_kind from cable text (no endpoint port resolved to a group domain)'];
    }
    if (count($domains) > 1) {
        return [null, 'endpoint port domains disagree (' . implode(' vs ', $domains) . ')'];
    }

    $domain = $domains[0];
    if ($domain === 'power') {
        return ['power', 'link_kind from port domain power'];
    }
    if ($domain === 'net') {
        return rw_is_fiber($conns, $cableText)
            ? ['fiber',  'link_kind from port domain net, optical connector/standard']
            : ['copper', 'link_kind from port domain net, electrical connector/standard'];
    }
    return [null, "port domain '$domain' has no SoR link_kind (only net and power commit)"];
}

/**
 * Fallback only: infer link_kind from the cable's standard/category/jacket
 * text when its endpoint ports cannot be resolved. See
 * rw_cable_link_kind_resolved(), which is what the commit path calls.
 *
 * @param array<string,mixed> $c
 */
function rw_cable_link_kind(array $c): string
{
    $text = rw_cable_text($c);
    if ($text === '') {
        return 'copper';
    }
    if (preg_match('/\b(iec|nema|barrel|power|ac|dc)\b/', $text) === 1) {
        return 'power';
    }
    if (preg_match('/\b(fiber|sfp|optic)\b/', $text) === 1) {
        return 'fiber';
    }
    return 'copper';
}
