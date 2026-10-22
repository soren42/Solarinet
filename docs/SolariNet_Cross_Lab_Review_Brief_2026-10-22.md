# SolariNet and RackWire cross-lab review brief

**Prepared:** 2026-10-22
**Review type:** Independent, read-only release-candidate review
**Requested response:** Findings first, with severity, file/line, evidence, and a concrete fix. State explicitly when a claim is only inferred. Do not modify files, publish, merge, release, or deploy.

## Candidate refs

| Component | Candidate | Base / publication state |
|---|---|---|
| SolariNet | `integrate/stability-20261020` at `3a0246d` | Both Forgejo and GitHub `main` are `5e8d688`; draft GitHub PR #37 is open. The local worktree also has uncommitted scan retry/idempotency code, scan-to-SoR browser acceptance, CI environment changes, and documentation. |
| RackWire | `integrate/solarinet-20261021` at `99453f3` | Draft GitHub PR #1 is open; its CI is green. Root candidate pins this gitlink. |
| scanner-ingest | `integrate/solarinet-20261021` at `ebb9a58` | Local repository only; no configured remote and no matching published GitHub/Forgejo repository. Manual and validation documentation is locally modified. |

No main branch has been merged and no release tags exist. Do not treat the newest local diff as part of PR #37: it is not yet committed or hosted.

## High-priority review surfaces

1. **Authentication, authorization, and request integrity:** dashboard session freshness, CSRF enforcement, PHP-to-`solariCtl` privilege boundary, enrollment identity, certificate revocation/replacement, and decommission challenges.
2. **Schema migration and recovery:** migration ledger, clean-install snapshots across MariaDB versions, transaction boundaries, upgrade/rollback behavior, and backup/restore assumptions.
3. **Inventory integration:** scanner-ingest preview/apply/replay semantics, SoR transaction/idempotency behavior, RackWire model/barcode/plan persistence, and duplicate location ambiguity.
4. **Realtime network discovery:** bounded scan admission, durable job/idempotency behavior, status recovery, live candidate refresh, adoption semantics, and RackWire handoff. Discovery candidates are reviewable network observations; host adoption does not itself assert a physical asset.
5. **Broker and DNS delivery:** RabbitMQ confirm/ack behavior and version upgrade evidence; DNS apply/reconcile and secondary consistency. Do not recommend an unreviewed DNS or UniFi write.
6. **Release packaging:** pinned RackWire gitlink, archive allowlist/provenance, integrity checks, atomic activation, and rollback.

## Evidence available

- Root GitHub PR #37 at `3a0246d`: hosted CI run `39706191166`, 15/15 jobs successful.
- RackWire GitHub PR #1 at `99453f3`: hosted run `39704732584`, backend/runtime checks successful.
- Root candidate browser/integration suite previously exercised scanner import, RackWire plan persistence, barcode resolution, authenticated scan admission, native worker progress, and candidate visibility. A direct, bounded `10.0.0.0/24` acceptance processed 254 addresses and produced 14 reviewable candidates in a disposable database.
- New local scan retry and ingest patch: API retains the idempotency key after an uncertain network failure, clears it after successful admission, and uses a fresh key for a later intentional scan. The server binds idempotency replay to the original verb and argument payload; same-key/different-payload requests fail closed. The live DB integration test covers mismatch rejection and no extra row. In the browser test, the first scan reached PHP, the C queue, and MariaDB before its response was dropped; after reload, the retry returned the original admission. The browser then adopted the candidate through the Monitor dialog and verified one monitoring asset plus the mapped IPv4 address on a separate SoR entity. The combined direct-route run passed on disposable MariaDB 11.8.9 monitoring and SoR schemas: 254/254 addresses processed, 12 candidates found, the listener candidate visible while the scan remained running, and `10.0.0.20` adopted into monitoring and SoR. A loopback `/32` combined run also passed. Both used the native worker, C bridge, PHP API, authenticated dashboard, and local Chromium. The CI browser step now receives both scratch database configurations, and the harness rejects missing or non-scratch SoR settings. The PHP route test, Node syntax check, YAML parse, and whitespace check pass. A local reviewer found and cleared the payload-binding fix; the requested cross-lab review and hosted CI remain pending.
- The standalone scanner app's current two-item/two-asset schema-v1 export was also passed as an input to `tests/browser/test_scanner_import_e2e.cjs`. In a fresh scratch SoR, the authenticated dashboard previewed without writes, applied and read back both assets, configured a RackWire profile, resolved an imported asset barcode, and persisted its tag/serial in a RackWire plan. The synthetic CI fixture path passed too. This exposed ambiguous accessible names on multiple location selects; `dashboard/public/screens4.jsx` now gives each source-location selector and the unmatched-location policy an explicit accessible name. The test used duplicate scratch rows to verify operator mapping; it does not resolve the real `Bin #5`/`Nook` mapping to duplicate “Painted Fern” SoR sites. The scratch DB was empty after cleanup, and no scanner app or live SoR data was changed. Hosted CI and independent review are pending.
- RackWire deployment health on Xenon: standalone discovery service and Apache health proxy return HTTP 200. The dashboard's unauthenticated discovery route returns 401. The Xenon-served dashboard bundle differs from the candidate; these checks do not prove candidate PHP-FPM/browser behavior.
- Current committed F-01 coverage includes same-CN renewal: `test_gossip.sh` creates two distinct leaf serials for the same identity, revokes the old leaf, and `test_gossip.c` verifies rejection of the revoked leaf plus acceptance and gossip admission of the replacement. `ctest --test-dir build-server --output-on-failure -R '^test_gossip$'` passed 1/1 locally. This proves the pinned loopback TLS fixture behavior after CRL activation, not fleet distribution, freshness acknowledgement, or production rollout.
- UniFi already has a read-only SoR comparison in `deploy/unifi/unifi_topology.py`: its source collector rejects incomplete pages, `load_sor_state()` uses `SET TRANSACTION READ ONLY` and `SELECT` statements, and `compare_sor()` emits `review_required` conflicts without a writer. `python3 -m unittest discover -s deploy/unifi -p 'test_*.py' -v` passed 18/18. The test suite verifies a source `/8` overlapping 16 SoR `/24`s and a DHCP overlap. This turn used fixtures only; the live UniFi/SoR snapshot was not re-read.
- Benzene RabbitMQ is currently healthy on the floating `rabbitmq:3-management` tag with four empty application queues and seven connections. Its live upgrade remains deferred pending independent review and pinned startup. DNS state, source ambiguity, off-host backup limits, and remaining release gates are recorded in the linked Status & Operations guide and stabilization workflow.
- Candidate validation after the latest local changes: `cmake --build build-server --parallel 4` passed; full monitoring CTest passed 36/36 against a disposable MariaDB migrated through 033, and `test_net_loopback` passed separately. scanner-ingest pytest passed 47 tests with 16 skips and 31 subtests. UniFi tests passed 18/18 and DNS release tests passed 17/17. Hosted CI has not yet run on these changes.

## Open release questions

- Cross-lab review has not run: Claude CLI currently reports no authenticated session.
- Candidate authenticated browser behavior on deployed PHP-FPM remains unverified.
- Live migration adoption and rollback against the current SoR remain unverified.
- UniFi reports a broad `10.0.0.0/8` while the SoR has 16 named `/24` segments; source authority and safe reconciliation remain unresolved. Keep comparison read-only.
- The scanner-ingest publication destination and ownership are unresolved.
- Mosquitto application-client authentication has not been verified; Authbroker is absent on Xenon.

## Review questions

- Identify correctness, security, data-loss, concurrency, and rollout issues that block a release.
- Check whether tests establish the claims above or leave gaps, especially on retry/idempotency, migration recovery, and rollback.
- Separate must-fix findings from follow-up improvements. Give each finding a reproducible path and the narrowest correction that preserves the stated architecture.
- State whether the code is suitable for a development deployment, and list the exact conditions still required before merging or tagging.

**Secret handling:** This brief contains no credentials, tokens, private keys, or secret values. Refer to secret files by path only; do not copy their contents into review notes.
