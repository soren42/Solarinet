# CONTRACT-LC — Entity Lifecycle (delete/rework) + Criticality Tiers

`v1.0 · 2026-08-05 · Lead: fable-5 · tasks #9 #10 · gates panel go-live`
`recon: recon-lc brief 2026-08-05 (file:line refs therein)`

Two features, one contract: shared entity surfaces, shared detail-view UI,
shared downstream consumers (alert engine, notifier, panel feed).

## 1. Problem

**Lifecycle (#9).** A reprovisioned/retired machine cannot be removed from
monitoring. Live evidence (pihole case, 2026-08-05):
- L1: deleting probeTarget/probeCurrent rows is futile — `assetSyncHeartbeat()`
  (serverAssets.c:125) re-upserts from the asset table when `monitorHost=1`.
- L2: removal orphans open alertEvent rows (90 found) — the C engine's
  fired-state is in-memory only (gAlertState, serverAlert.c:56); "cleared" is
  a SECOND row, and `clearedAt` is only ever set by PHP ack or
  serverDbAckAlertEvent(); nothing auto-clears when the target vanishes.
- L3: discovery adoption (serverProvisionAdoptTarget, serverDiscovery.c:351)
  can re-create entities seen on the network — hard delete of a live device
  silently resurrects. Only a tombstone is durable.

**Criticality (#10).** All entities alert equally. alertRule.severity is
per-rule; UNIQUE(scope,metric) on alertRule blocks per-tier rule variants —
so weighting must live on the ENTITY, composed with the rule at fire time.
The alertbridge publishes EVERY alertEvent row past its checkpoint — no
severity filter, no cleared filter (alertbridge.py:246); the only suppression
is maintenanceWindow. Notification has no notion of "this box matters."

## 2. Existing substrate (reuse, don't duplicate)

- `node.state` already has terminal `'retired'` (002:79) + route
  POST /api/nodes/{nodeId}/retire (control.php:97).
- ASSET_REMOVE / TARGET_REMOVE solariCtl verbs exist (solariCtl.c:683,700);
  destructive verbs enumerate in the confirm gate at solariCtl.c:331-334.
- AssetDetail already has a "Remove system" danger flow (screens3.jsx:1428,
  1437, 1461); AlertsScreen has a reusable DangerModal (screens.jsx:1063).
- FleetOverview already has a "Retired" filter chip (screens.jsx:286).
- Mutation idiom (assets.php:113-135): validate → 404 check → json body →
  Operator::requireOperator() → confirm!==true → 409 confirm_required →
  SolariCtl::call(VERB) → Response::ok. PHP NEVER writes the DB.
- Blunt kill switch that works today: asset.monitorHost=0.

## 3. Architecture

### 3.1 Lifecycle: tombstone column on asset (node reuses state='retired')

Migration **018_lifecycle_criticality.sql**:
```sql
ALTER TABLE asset
  ADD lifecycle ENUM('active','decommissioned','deleted') NOT NULL DEFAULT 'active' AFTER monitorHost,
  ADD criticality TINYINT NOT NULL DEFAULT 2 AFTER lifecycle;
ALTER TABLE node
  ADD criticality TINYINT NOT NULL DEFAULT 2 AFTER state;
CREATE INDEX ix_asset_lifecycle ON asset (lifecycle);
```
(IF-NOT-EXISTS-guarded per house migration style; 018 is the next free slot —
015 is the uncommitted barcode migration, keep clear of it.)

Semantics:
- **active** — normal.
- **decommissioned** — tombstone, reversible. Visible greyed in lists
  (default filter shows it, "hide retired" respects it); excluded from probe
  enumeration, alert evaluation, panel score/alarm, notification.
- **deleted** — hidden from default lists; row retained so discovery
  reappearance is DETECTED and surfaced as a notice, never silently
  re-adopted. Restore possible.
- **purge** — true row delete, admin-only, typed-name confirm; cascades
  probeTarget/probeCurrent/probeHistory rows; open alertEvents are CLEARED
  (never deleted — audit history keeps all rows).

Transitions (all via solariCtl, single DB transaction in C):
`LIFECYCLE_SET {assetId, to}` — active↔decommissioned, deleted→active
(restore), decommissioned→deleted. On every transition AWAY from active:
1. serverDbDeleteProbeTarget for every target of the asset (icmp + services).
2. UPDATE alertEvent SET clearedAt=COALESCE(clearedAt,UTC_TIMESTAMP())
   for open rows matching the asset (see §3.4 id-space note).
3. Drop the asset's gAlertState in-memory breach slots.
`ASSET_PURGE {assetId, confirmName}` — new destructive verb, added to the
solariCtl confirm-gate list, admin-only at the PHP layer.

`assetSyncHeartbeat()` and `assetAddService()` gain `lifecycle='active'` as a
precondition (alongside monitorHost). Discovery adopt path checks the
tombstone by ip: match on decommissioned/deleted → write a `discovered`
row with status 'ignored' + notice detail instead of adopting.

**Rework** = existing edit surfaces + lifecycle; only missing edit
affordances (NodeDetail, ServiceDetail) are added. No new mechanism.

### 3.2 Criticality tiers

`criticality TINYINT 0..4` on asset and node, default 2:

| tier | name | offline behavior |
|---|---|---|
| 0 | ignore | monitored/graphed; alert engine suppresses insert entirely |
| 1 | low | severity capped at warn; bridge never publishes |
| 2 | normal | today's behavior exactly |
| 3 | high | crit publishes push; panel alarm participates (status quo crit path, explicit) |
| 4 | vital | fired severity floored to crit; bridge publishes immediately; panel forces alarmActive while down |

Consumption points:
- **Engine (C)**: serverAlertEval* composes fired severity =
  f(rule.severity, entity.criticality): tier0 → skip insert; tier1 → min(sev,
  warn); tier4 → max(sev, crit). Entity resolution: host scope → node.criticality;
  probe scope → asset.criticality via probeTarget.assetId. Rules stay untouched
  (UNIQUE(scope,metric) never fought).
- **Bridge (python)**: alertbridge gains two filters at the natural
  _suppressed() insertion point: (a) skip rows whose entity tier ≤1,
  (b) skip CLEARED transition rows unless tier ≥3 (recovery notices only for
  things that paged). Severity itself was already composed by the engine.
- **Panel feed (php, read-only)**: panel.php weights score by tier (tier 0/1
  excluded), and any tier-4 asset/node down forces alarmActive; topAlert
  ordering prefers tier desc then severity desc then recency.
- **UI**: tier selector (5 labeled tiers with behavior text) on
  AssetDetail/NodeDetail/ServiceDetail; tier chip in FleetOverview/Assets
  lists; deleted hidden by default filter, decommissioned greyed.

### 3.3 API (thin PHP, house idiom, all writes via SolariCtl)

- `POST /api/assets/{id}/lifecycle` {action: decommission|delete|restore, confirm:true} → LIFECYCLE_SET
- `POST /api/assets/{id}/purge` {confirmName} → ASSET_PURGE (admin-only: new Operator::requireAdmin or role check)
- `POST /api/assets/{id}/criticality` {tier:0..4} → CRIT_SET (operator)
- `POST /api/nodes/{nodeId}/criticality` {tier:0..4} → CRIT_SET (operator)
- Node retire: existing route stands; add clearing cascade to its C verb so
  retiring a node also clears its open alerts (same L2 fix).
- Service-level: services are probeTarget rows under an asset — service
  "delete" = existing TARGET_REMOVE + heartbeat no-resurrect (already keyed
  on servicesJson; verify and cover with acceptance A8).

### 3.4 The two id-space hazard (load-bearing)

alertEvent.targetId uses NUMERIC proto ("2:10.0.0.254:53",
alertProbeTargetId serverAlert.c:196); probeTarget.targetId uses the string
form ("tcp:10.0.0.254:53", dbProbeTargetId serverDb.c:172). The cascade
clear and the bridge tier lookup MUST map between them (proto 1=icmp,2=tcp,
3=udp assumed — VERIFY in code before relying). LC1 must add ONE shared
helper for this mapping (C) and LC2 must not hand-roll a second (python
parses ip from the numeric form directly). Mutation test this mapping.

## 4. Edge cases

- E1: decommission with OPEN crit alert → alerts cleared in the same tx,
  panel score drops next poll, no ghost. (pihole case, automated.)
- E2: purge cascades probe history; closed alertEvent rows stay (no FK).
- E3: tier change with open alert → next engine pass re-evaluates; no
  retroactive severity rewrite; bridge dedupe unaffected (eventId checkpoint).
- E4: all pool entities decommissioned → pool 0/0; panel total=0 path
  already handled (verify in acceptance).
- E5: same-IP reprovision of a tombstoned asset → discovery notice, operator
  chooses restore or purge+readopt. Documented, surfaced, never guessed.
- E6: transitions are single-tx in C; concurrent LIFECYCLE_SET last-wins.
- E7: tier 4 + decommissioned → lifecycle wins; tombstone silences all.
- E8: server restart mid-anything → gAlertState is memory-only and already
  rebuilds; lifecycle/criticality read fresh per report (rules already are).
- E9: alertbridge checkpoint must NOT skip-then-advance past tier-gated rows
  in a way that loses later ungated rows (filter ≠ checkpoint semantics).

## 5. Acceptance

- A1: decommission live-probed asset → no new probeCurrent within one cycle;
  open alerts cleared; panel counts drop; UI greyed. Restore → probes resume.
- A2: delete → hidden by default; discovery reappearance → 'ignored'
  discovered-row notice, no live entity.
- A3: purge — admin+typed-name only; operator 403; wrong name 400/409;
  probe rows gone, closed alert history intact.
- A4: tier 0 down → zero new alertEvents. tier 1 → warn rows in DB, nothing
  published to MQ. tier 4 down → crit row + MQ publish + panel alarmActive=1
  within one panel poll.
- A5: authz matrix — viewer 403 all mutations; operator all except purge;
  admin all; panel service principal 403 all.
- A6: panel E2E — decommission the entity behind current topAlert → topAlert
  advances within one poll.
- A7: php -l clean; migration 018 applied BEFORE referencing code goes live
  (CP1 rule); C server builds -Werror clean; alertbridge/notifyd unit tests
  or dry-run pass; UI harness passes.
- A8: TARGET_REMOVE on a service target does not resurrect via heartbeat
  within two cycles.
- A9: id-space mapping helper round-trips all three protos both directions
  (unit test, mutation-verified).

## 6. Build lanes

- **LC1 — C server + migration** (codex/gpt-5.6): 018 migration; LIFECYCLE_SET
  / ASSET_PURGE / CRIT_SET verbs + confirm-gate entries; cascade tx; heartbeat
  + discovery tombstone checks; engine tier composition; id-space helper +
  unit test. IN-SCOPE: src/server/*, db/migrations/018*. OUT: PHP, python, UI.
- **LC2 — bridge + PHP** (codex/gpt-5.6): alertbridge tier+cleared filters
  (E9-safe); PHP routes (§3.3) in house idiom; panel.php tier weighting +
  topAlert ordering. IN-SCOPE: deploy/alertbridge/, dashboard/api/routes/
  {assets,control,panel}.php + lib if needed. OUT: C, UI. Depends on LC1's
  verb names + migration (interface freeze below).
- **LC3 — UI** (opus taste lane): NodeDetail/ServiceDetail lifecycle actions
  (reuse DangerModal), criticality selector + chips, list filters
  (deleted hidden, decommissioned greyed), discovery notice row. IN-SCOPE:
  dashboard/public/screens.jsx, screens3.jsx (+styles). OUT: api, C.
- **Interface freeze (Lead-owned)**: verb names + payloads (§3.3), tier
  table (§3.2), migration 018 DDL (§3.1), id-space helper signature. Lanes
  code against this file; changes route through Lead only.
- **Review**: cross-lab — LC1/LC2 reviewed by opus/fable; LC3 reviewed by
  codex. Mutation tests: cascade tx (A1), id mapping (A9), bridge checkpoint
  (E9).
- **Deploy order**: 018 migration → C server rebuild+restart → PHP (live on
  save) → alertbridge/notifyd restart → UI copy to docroot → acceptance.

## 7. Design-review dispositions (v1.1, binding — override earlier sections)

Cross-lab review (gpt-5.6, 2026-08-05) found six flaws; all accepted:

**D1 — Event model.** FIRED/CLEARED are insert-only rows; `clearedAt` is the
ACK field, not incident state. Overloading it was wrong. Migration 018 adds
to alertEvent: `eventKind ENUM('fired','cleared','audit') NOT NULL DEFAULT
'fired'`, `openedEventId BIGINT UNSIGNED NULL` (a cleared row points at its
fired row), `assetId BIGINT UNSIGNED NULL`, `effectiveTier TINYINT NULL`,
`disposition ENUM('publish','suppress') NULL`. Engine writes all five on
insert (audit rows: eventKind='audit'). Legacy rows keep NULLs.

**D2 — Lifecycle is a write-path predicate, not just a cascade.** The
cascade tx cannot serialize against concurrent evaluation. Therefore:
serverDbUpsertProbeTarget, assetAddService, assetSyncHeartbeat, and
serverAlertEval* each check entity lifecycle at write/eval time (fresh read,
same as rules are loaded). The cascade remains (cleanup), but correctness
rests on the predicate. Acceptance gains A10: fire a report for a
just-decommissioned entity in the same second — no new alertEvent, no new
probeTarget.

**D3 — Service tombstone.** TARGET_REMOVE alone is not durable (servicesJson
reconcile re-upserts). Migration 018 adds `probeTargetTombstone (targetId
VARCHAR(128) PK, removedAt DATETIME, removedBy VARCHAR(64))`; every service
reconcile path skips tombstoned ids; TARGET_REMOVE writes it; restore of the
parent asset clears its tombstones.

**D4 — Disposition at write time (kills two flaws).** The engine snapshots
effectiveTier + disposition onto each alertEvent row at insert; cleared rows
INHERIT the fired row's disposition via openedEventId. The bridge does NO
tier lookup and NO id parsing: it processes every row in eventId order to a
durable terminal outcome (published|suppressed, decided purely from
disposition/eventKind columns; NULL disposition = legacy = today's
behavior), then advances the checkpoint. E9 resolved: filter ≠ skip;
checkpoint advances after EITHER outcome; crash-between test required.
alertEvent.assetId (D1) also kills the cross-language id-space parsing — the
C helper (§3.4) remains C-internal for the cascade only.

**D5 — Direct C publisher.** alertPublish() (SCP PUB) also carries alarm
data to opied (remediation) — it must keep seeing everything. Disposition:
SCP PUB stays unfiltered (machine consumers decide themselves), but the
frame gains the effectiveTier byte so opied MAY filter. Human notification
is exclusively the MQ path, which is gated. Documented in the manual.

**D6 — Deploy order (revised, binding).** (1) migration 018 (additive,
NULL-tolerant — verify old C binary tolerates via staging INSERT test);
(2) alertbridge NEW version (understands disposition, NULL=legacy) +
restart; (3) C server rebuild + restart (starts writing dispositions);
(4) PHP routes (live on save, after C verbs exist); (5) UI to docroot;
(6) acceptance battery. Rollback = application rollback, schema stays.

Acceptance additions: A10 (D2 race), A11 (bridge processes [tier1, tier4]
in order, suppresses first, publishes second, checkpoint correct after
crash-between-disposition-and-checkpoint), A12 (recovery published exactly
once and only for a previously published incident), A13 (audit rows never
reach MQ), A14 (id helper: IPv6, icmp port-omission, malformed — C unit).

## 8. Interface amendments (Lead, during build)

**A-1 (LC3 scope grant).** dashboard/public/api.jsx: LC3 may make a surgical
passthrough edit ONLY — mapNode() and mapAssetNode() pass `criticality` and
`lifecycle` through verbatim; mapDiscovered() passes `tombstone` through.
Nothing else in api.jsx changes.

**A-2 (discovery notice, frozen).** No new column. The discovery READ route
(dashboard/api/routes/discovery.php — LC2 scope addition) computes, per
discovered row, `tombstone: {assetId, lifecycle, displayName} | null` by
joining asset ON ip WHERE lifecycle <> 'active'. LC1's C side writes only
what it already planned (status='ignored' on the reappearance row); the
JOIN, not a stored field, is the notice. UI renders the notice only when
`tombstone` is non-null; absent field = no notice (graceful pre-deploy).

**A-3 (missing-field semantics, binding on LC3).** UI must treat absent
criticality/lifecycle as "unknown" (no chip, no grey), NEVER as tier 0 /
non-active — a wrong default would mislabel every host. (LC3 already
building this way; now contractual.)

## 9. Review-LC12 adjudications (Lead, binding)

**J1 (M1 — verb keys FROZEN, an interface-freeze failure I own).** §3.3 named
routes but not verb argument keys. Frozen now: CRIT_SET takes `asset=<id>` OR
`node=<id>` plus `tier=<0..4>` (C side as implemented); PHP adapts.
LIFECYCLE_SET takes `asset=<id>`, `to=<state>`. ASSET_PURGE takes
`asset=<id>`, `confirm=<name>`.

**J2 (M3 — "open" stays clearedAt IS NULL).** The cascade sets clearedAt on
exactly the fired rows it closes (targeted, not the historical mass-sweep D1
rejected), AND writes the eventKind='cleared' incident row — which is BORN
with clearedAt=NOW, since a recovery notice is not an open alert. All four
existing "open" readers stay untouched. eventKind remains the incident
model; clearedAt remains the open/ack flag. Both, always, together.

**J3 (M2 — cascade must catch legacy + host rows).** Asset cascade clears
open rows matching f.assetId = ? OR (f.assetId IS NULL AND targetId
suffix-matches one of the asset's captured targets via the id helper).
Node retire cascade clears by nodeId. New host-scope inserts carry nodeId
(as today) — assetId stays NULL for them by design; the node cascade owns
them.

**J4 (M4/M5 — purge confirm).** Server-side comparison in C is mandatory:
confirmName must equal displayName OR host OR ip (ip is NOT NULL UNIQUE, so
every asset — including nameless — is purgeable). PHP passes confirmName
through verbatim; UI already derives displayName||host||ip.

**J5 (M6).** Disposition selects publish CANDIDATES; maintenanceWindow
suppression still applies after. Order: eventKind/audit gate → disposition
gate → maintenance gate → publish.

**J6 (M7).** Engine inherits cleared-row disposition+effectiveTier from the
fired row via openedEventId (one SELECT), never recomputes. D4 verbatim.

**J7 (M9).** alertPublish (SCP PUB) fires for every evaluated breach
INDEPENDENT of tier gating — tier 0 suppresses the DB insert and MQ, never
the PUB. opied keeps full visibility per D5.

**J8 (M8).** serverDbLifecycleTransition gains a live-DB test case in
test_server_db_live (runs under SOLARI_TEST_DB; the Lead runs it against
the solarinet_stage clone as a deploy gate) — the failure-path rollback
mutation must fail the test.
