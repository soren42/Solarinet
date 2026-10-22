# SolariNet and RackWire cross-lab review brief

**Prepared:** 2026-10-22
**Review type:** Independent, read-only release-candidate review
**Requested response:** Findings first, with severity, file/line, evidence, and a concrete fix. State explicitly when a claim is only inferred. Do not modify files, publish, merge, release, or deploy.

## Candidate refs

| Component | Candidate | Base / publication state |
|---|---|---|
| SolariNet | code candidate `789b1d3`; docs follow-ups `04e80be`, `b81e2cc`, `fa1ba87` | Both Forgejo and GitHub `main` are `5e8d688`; both integration refs match. Draft GitHub PR #37 is open. |
| RackWire | `integrate/solarinet-20261021` at `99453f3` | Draft GitHub PR #1 is open; its CI is green. Root candidate pins this gitlink. |
| scanner-ingest | `integrate/solarinet-20261021` at `c2a3f7b` | Local repository only; no configured remote and no matching published GitHub/Forgejo repository. Updated manuals and validation are committed locally. |

No main branch has been merged and no release tags exist. The scanner-ingest manual/validation edits remain local and have no configured remote.

## High-priority review surfaces

1. **Authentication, authorization, and request integrity:** dashboard session freshness, CSRF enforcement, PHP-to-`solariCtl` privilege boundary, enrollment identity, certificate revocation/replacement, and decommission challenges.
2. **Schema migration and recovery:** migration ledger, clean-install snapshots across MariaDB versions, transaction boundaries, upgrade/rollback behavior, and backup/restore assumptions.
3. **Inventory integration:** scanner-ingest preview/apply/replay semantics, SoR transaction/idempotency behavior, RackWire model/barcode/plan persistence, and duplicate location ambiguity.
4. **Realtime network discovery:** bounded scan admission, durable job/idempotency behavior, status recovery, live candidate refresh, adoption semantics, and RackWire handoff. Discovery candidates are reviewable network observations; host adoption does not itself assert a physical asset.
5. **Broker and DNS delivery:** RabbitMQ confirm/ack behavior and version upgrade evidence; DNS apply/reconcile and secondary consistency. Do not recommend an unreviewed DNS or UniFi write.
6. **Release packaging:** pinned RackWire gitlink, archive allowlist/provenance, integrity checks, atomic activation, and rollback.

## Evidence available

- Root GitHub PR #37: code-bearing commit `789b1d3` passed hosted run `39715155684` (15/15); documentation commits `04e80be`, `b81e2cc`, and `fa1ba87` passed runs `39715807073`, `39716727270`, and `39718050283` (15/15 each). Coverage includes dashboard/API, scanner/RackWire browser acceptance, server/database tests, cross-platform builds, and the RabbitMQ version matrix.
- RackWire GitHub PR #1 at `99453f3`: hosted run `39704732584`, backend/runtime checks successful.
- Root candidate browser/integration suite previously exercised scanner import, RackWire plan persistence, barcode resolution, authenticated scan admission, native worker progress, and candidate visibility. A direct, bounded `10.0.0.0/24` acceptance processed 254 addresses and produced 14 reviewable candidates in a disposable database.
- Pushed scan retry and ingest patch: API retains the idempotency key after an uncertain network failure, clears it after successful admission, and uses a fresh key for a later intentional scan. The server binds idempotency replay to the original verb and argument payload; same-key/different-payload requests fail closed. The live DB integration test covers mismatch rejection and no extra row. In the browser test, the first scan reached PHP, the C queue, and MariaDB before its response was dropped; after reload, the retry returned the original admission. The browser then adopted the candidate through the Monitor dialog and verified one monitoring asset plus the mapped IPv4 address on a separate SoR entity. The combined direct-route run passed on disposable MariaDB 11.8.9 monitoring and SoR schemas: 254/254 addresses processed, 12 candidates found, the listener candidate visible while the scan remained running, and `10.0.0.20` adopted into monitoring and SoR. A loopback `/32` combined run also passed. Both used the native worker, C bridge, PHP API, authenticated dashboard, and local Chromium. The CI browser step now receives both scratch database configurations, and the harness rejects missing or non-scratch SoR settings. The PHP route test, Node syntax check, YAML parse, and whitespace check pass. The local reviewer found and cleared the payload-binding fix; hosted CI passed, while cross-lab review remains unavailable.
- The standalone scanner app's current two-item/two-asset schema-v1 export was also passed as an input to `tests/browser/test_scanner_import_e2e.cjs`. In a fresh scratch SoR, the authenticated dashboard previewed without writes, applied and read back both assets, configured a RackWire profile, resolved an imported asset barcode, and persisted its tag/serial in a RackWire plan. The synthetic CI fixture path passed too. This exposed ambiguous accessible names on multiple location selects; `dashboard/public/screens4.jsx` now gives each source-location selector and the unmatched-location policy an explicit accessible name. The test used duplicate scratch rows to verify operator mapping; it does not resolve the real `Bin #5`/`Nook` mapping to duplicate “Painted Fern” SoR sites. A later read-only transaction against the live SoR confirmed IDs 9103 and 9104 are active duplicate root sites with blank notes, no children, and no located hardware units or entities; there are no exact `Bin #5` or `Nook` SoR locations. The scratch DB was empty after cleanup, and no scanner app or live SoR data was changed. Hosted CI passed; cross-lab review remains unavailable.
- RackWire deployment health on Xenon: standalone discovery service and Apache health proxy return HTTP 200. The dashboard's unauthenticated discovery route returns 401. The Xenon-served dashboard bundle differs from the candidate; these checks do not prove candidate PHP-FPM/browser behavior.
- Xenon was rechecked read-only for deployment planning. Its active `solarinet-server.service` runs from the separate checkout `/home/jason/Code/Solarinet`; that checkout is on `stabilize/phase-0-containment` with uncommitted changes. Apache serves `/var/www/solarinet`, PHP 8.4-FPM is active, `/opt/solarinet/current` does not exist, and `solarinet-scan-worker.service` is inactive. No files or services were changed. The current host does not match the candidate's atomic release contract, so production-style deployment needs an isolated target or a reviewed layout migration.
- Current committed F-01 coverage includes same-CN renewal: `test_gossip.sh` creates two distinct leaf serials for the same identity, revokes the old leaf, and `test_gossip.c` verifies rejection of the revoked leaf plus acceptance and gossip admission of the replacement. `ctest --test-dir build-server --output-on-failure -R '^test_gossip$'` passed 1/1 locally. This proves the pinned loopback TLS fixture behavior after CRL activation, not fleet distribution, freshness acknowledgement, or production rollout.
- UniFi already has a read-only SoR comparison in `deploy/unifi/unifi_topology.py`: its source collector rejects incomplete pages, `load_sor_state()` uses `SET TRANSACTION READ ONLY` and `SELECT` statements, and `compare_sor()` emits `review_required` conflicts without a writer. `python3 -m unittest discover -s deploy/unifi -p 'test_*.py' -v` passed 18/18. The test suite verifies a source `/8` overlapping 16 SoR `/24`s and a DHCP overlap. This turn used fixtures only; the live UniFi/SoR snapshot was not re-read.
- Benzene RabbitMQ is currently healthy on the floating `rabbitmq:3-management` tag with four empty application queues and seven connections. Its live upgrade remains deferred pending independent review and pinned startup. DNS state, source ambiguity, off-host backup limits, and remaining release gates are recorded in the linked Status & Operations guide and stabilization workflow.
- Candidate validation after the latest code changes: `cmake --build build-server --parallel 4` passed; full monitoring CTest passed 36/36 against a disposable MariaDB migrated through 033, and `test_net_loopback` passed separately. scanner-ingest pytest passed 47 tests with 16 skips and 31 subtests. UniFi tests passed 18/18 and DNS release tests passed 17/17. The hosted run listed above covers the pushed code candidate.
- GitHub PR #37 run `39718723877` for docs/candidate commit `5160202` completed with all 15 workflow jobs successful. It covered the live Discovery worker/API and candidate adoption, scanner import and RackWire browser flow, dashboard lifecycle/UI checks, clean database installs, and release package activation/rollback. A read-only Cesium preflight confirmed ample disk and MariaDB but no PHP CLI or container runtime; no instance or test database was created.
- Independent internal RackWire review identified that stale-CSRF recovery had only source-text coverage. Added behavioral tests for plan writes and barcode lookup that simulate an expired token, verify a single refresh/retry with the same request body, and bound a second rejection. The reviewer rechecked the follow-up and reported no findings. Local checks: UI 60/60, RackWire API 58/58, standalone browser acceptance passed with Playwright 1.64.1 and cached Chromium. The combined scanner/RackWire browser E2E now injects one expiry-shaped 403, refreshes identity through the real PHP endpoint, and succeeds on the real barcode/SoR route; this passed against a freshly migrated loopback scratch schema, which was dropped afterward. This is same-lab internal review and does not satisfy the cross-lab review gate.

## Open release questions

- Cross-lab review has not run: Claude CLI currently reports no authenticated session.
- RackWire stale-CSRF retry behavior passed local client-harness and browser-to-real-PHP/SoR tests after an injected expiry-shaped 403. Actual elapsed-token expiry/denial by the live PHP session guard remains unverified.
- Candidate authenticated browser behavior on deployed PHP-FPM remains unverified.
- scanner-ingest publication ownership and remote remain unresolved; the app is not published to either GitHub or Forgejo.
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
