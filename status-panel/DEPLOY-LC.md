# DEPLOY-LC — D6 deploy sequence (lifecycle + criticality)

`Lead-executed, in order, each step gated on the previous. CONTRACT-LC §7 D6.`
`Pre-validated: migration 018 on a live-schema clone — columns, old-binary
INSERT tolerance, idempotent re-apply (2026-08-06).`

## Gates before step 1
- [ ] REVIEW-LC12 verdicts: SHIP or all MUST-FIXes fixed + re-verified
- [ ] LC3 codex review verdict: same
- [ ] Working tree committed on feat/lifecycle-criticality (Lead commits, per lane)

## Sequence

1. **Migration 018** (additive, live DB):
   `sudo mysql solarinet < db/migrations/018_lifecycle_criticality.sql`
   Verify: information_schema shows all 8 columns + probeTargetTombstone;
   /api/panel still 200 (guarded code flips to real tiers); /api/assets and
   /api/nodes now EMIT criticality/lifecycle.
2. **alertbridge** (benzene-independent restart of the consumer side is NOT
   needed — bridge runs where?): confirm alertbridge host, deploy new
   alertbridge.py + test_dispositions.py, restart its unit. NULL-disposition
   rows = legacy behavior, so order vs C server is safe either way, but
   bridge FIRST per D6. If benzene (MQ) is still down: bridge will retry
   publishes from its checkpoint — deploy anyway, nothing lost.
3. **C server**: rebuild from the committed tree on the server host
   (cmake …), install, restart solariServer. Verify: journal clean start,
   a fresh alertEvent row (any) carries eventKind/disposition non-NULL.
4. **PHP routes**: live on save (already live for guarded reads); verify the
   new mutation routes answer (403 for viewer, confirm_required 409 shape).
5. **UI**: cp changed dashboard/public files to /var/www/solarinet/ (+ bump
   ?v= tags in index.html), verify parse + page loads.
6. **Acceptance battery** A1–A14 (status-panel/CONTRACT-LC.md §5 + §7):
   - A1 decommission/restore round-trip on a scratch asset (create one)
   - A2 delete + discovery-notice
   - A3 purge authz + typed-name
   - A4 tier 0/1/4 behaviors (tier4 leg needs a down entity — use scratch)
   - A5 authz matrix (viewer/operator/admin/panel-svc)
   - A6 topAlert advance on decommission
   - A7 gates (lint/build/harness) — already green pre-deploy
   - A8 service TARGET_REMOVE no-resurrect
   - A10 same-second race probe
   - A11/A12/A13 bridge dispositions — MQ legs DEFERRED until benzene up
   - A14 id helper unit — in C test suite
7. **Cleanup**: pihole assets 8/9 get PROPER lifecycle=decommissioned via the
   new verb (replacing the interim monitorHost=0), proving A1 on the real
   motivating case.

## Rollback
Application rollback only; schema stays (additive, old-binary tolerant —
proven). Bridge/PHP/UI: git revert + redeploy file(s). C server: previous
binary retained at /usr/local/bin/solariServer.prev (make the copy in step 3
BEFORE install).
