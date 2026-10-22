# SolariNet and RackWire cross-lab review brief

**Prepared:** 2026-10-22
**Review type:** Independent, read-only release-candidate review
**Requested response:** Findings first, with severity, file/line, evidence, and a concrete fix. State explicitly when a claim is only inferred. Do not modify files, publish, merge, release, or deploy.

## Candidate refs

| Component | Candidate | Base / publication state |
|---|---|---|
| SolariNet | Last fully validated code candidate `c867bcb`; docs tip `05aa2be` | Forgejo and GitHub integration refs match at the docs tip; both `main` refs remain `5e8d688`. The status/CI readiness update was published to Forgejo first. Draft GitHub PR #37 remains open. |
| RackWire | `integrate/solarinet-20261021` at `a66bd21` | Draft GitHub PR #1 is open; its CI is green. Root candidate pins this gitlink. |
| scanner-ingest | `integrate/solarinet-20261021`, base `c2a3f7b` | Local repository only; no configured remote and no matching published GitHub/Forgejo repository. Latest test-fixture and manual/validation updates are committed locally at `9ac42d6`; independent review is pending. |

No main branch has been merged and no release tags exist. The scanner-ingest manual/validation edits remain local and have no configured remote.

## Current release-candidate recheck · 2026-10-22

- Forgejo is the primary remote; its integration ref and GitHub mirror currently match at `c867bcb`. GitHub run `39798443486` completed all 15 jobs successfully on that head. GitHub reports an active Actions incident delaying workflow/step result processing ([incident q29p76qtnkyk](https://www.githubstatus.com/api/v2/incidents/unresolved.json), checked 2026-10-22). The prior docs-only run `39738275605` passed all 15 jobs on `46045e9`; code and portable Release checks passed in `39737421770`.
- Forgejo is the primary Git remote, but the candidate has no `.forgejo/workflows` definition. A workflow prototype was removed before publication after review found it violated loopback-only integration fixture contracts and could not reach dynamic Docker-published ports. No Actions runner was found on Cesium, Benzene, Xenon, Chlorine, or this workspace. Steel is unreachable; other possible runner hosts remain unchecked. Forgejo-side CI is therefore not verified, and GitHub Actions is the only hosted validation path.
- Cesium development deployment now uses `dev-20261022-2cb099c-static`, source revision `2cb099c442d14eb8e57b140188941512440c8d3f`, with the RackWire gitlink pinned to `a66bd21`. The loopback PHP UI, native server, worker, and isolated MariaDB are running. Authenticated browser acceptance rejected `192.0.2.0/24`; a permitted `127.0.0.1/32` scan completed 1/1 and displayed its one candidate. The UI reported live data, all checked API reads returned 200, with no page errors or external requests.
- The first candidate package used sanitizer-linked binaries, and the first Release build used a dynamic NNG dependency unavailable on Cesium. Those package attempts failed to start and were replaced by the statically linked pinned NNG/mbedTLS Release build. The package manifest and Cesium runtime were checked. The repeatable CI Release profile and runtime dependency gate passed hosted verification: CI built and exercised Release server/worker binaries, then used those same artifacts in package activation/rollback tests.
- Authenticated startup initially lacked the development DB environment and the UI fell back offline. Configuring the PHP process with the isolated dev database restored live mode. This failure and recovery are recorded to prevent treating offline UI acceptance as runtime verification.

- GitHub PR #37 remains open and draft. The Forgejo integration ref is one docs-only follow-up ahead of the GitHub mirror; both root `main` refs remain `5e8d6889bdb3108cd65c594243395e60a7a68db7`.
- Hosted run `39737421770` passed all 15 jobs, including authenticated scan API, browser-to-native-worker candidate ingest, portable Release binary lifecycle tests, and release package activation/rollback. Earlier code run `39733247468` passed the intranet range-policy tests.
- RackWire PR #1 remains open and draft at `a66bd218acc89b668a8f1a89291f2c32bb38ac59`; its Forgejo and GitHub integration refs match and both `main` refs remain `5f29f590f601bc277390ef28c6569dde52ea432d`. Its hosted checks are green.
- `scanner-ingest` base is `c2a3f7b81c7569e408fc26746aa1358791d05b1b`; the current suite follow-up is committed locally at `9ac42d6`, with no configured Git remote or release tag. The full 63-test run passed against Cesium development MariaDB 12.3.2-log (one optional ZXing-C++ test skipped).
- **UNVERIFIED / pending:** independent cross-lab review is unavailable in this session; no main branch was merged, release tag created, or production deployment performed. The installed Cesium candidate remains a loopback-only development deployment. Scanner-ingest physical inventory import is snapshot-based; live device streaming/automatic physical asset ingestion is not claimed. SolariNet realtime network discovery is separately exercised: a direct-route 10.0.0.0/24 scan processed 254 addresses, surfaced candidates before completion, retried admission without duplicate work, and adopted a candidate into monitoring and SoR in disposable test schemas. Production scanning/adoption remains unverified.

## High-priority review surfaces

1. **Authentication, authorization, and request integrity:** dashboard session freshness, CSRF enforcement, PHP-to-`solariCtl` privilege boundary, enrollment identity, certificate revocation/replacement, and decommission challenges.
2. **Schema migration and recovery:** migration ledger, clean-install snapshots across MariaDB versions, transaction boundaries, upgrade/rollback behavior, and backup/restore assumptions.
3. **Inventory integration:** scanner-ingest preview/apply/replay semantics, SoR transaction/idempotency behavior, RackWire model/barcode/plan persistence, and duplicate location ambiguity.
4. **Realtime network discovery:** bounded scan admission, durable job/idempotency behavior, status recovery, live candidate refresh, adoption semantics, and RackWire handoff. Discovery candidates are reviewable network observations; host adoption does not itself assert a physical asset.
5. **Broker and DNS delivery:** RabbitMQ confirm/ack behavior and version upgrade evidence; DNS apply/reconcile and secondary consistency. Do not recommend an unreviewed DNS or UniFi write.
6. **Release packaging:** pinned RackWire gitlink, archive allowlist/provenance, integrity checks, atomic activation, and rollback.

## Evidence available

- Root GitHub PR #37: integration commit `23aee5b` passed hosted run `39720370015` across the full workflow matrix; status/docs head `4790dcd` passed run `39720900951`. Earlier runs `39718723877` and `39719780873` also passed. Coverage includes dashboard/API, scanner/RackWire browser acceptance, server/database tests, cross-platform builds, RabbitMQ versions, and release package activation/rollback.
- RackWire GitHub PR #1 at `a66bd21`: behavioral CSRF retry tests and combined browser acceptance are covered by the pinned root candidate; hosted RackWire run `39719734709` passed.
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


## 2026-10-22 candidate delta: durable scan recovery

The working candidate adds `GET /api/discovery/scan-jobs`, operator-authenticated and requester-scoped, returning only the ten newest jobs. Discovery fetches this list when sessionStorage has no active scan and restores a queued/running job (or the latest terminal result). Tests assert SQL requester scoping, API normalization, active-state restoration in the scanner/RackWire browser fixture, and terminal-state recovery against the real scratch monitoring DB after a native loopback scan.

Local verification passed: `php -l dashboard/api/routes/discovery_mut.php`, `php tests/dashboard/test_discovery_scan_input.php`, `node --check` for both changed browser tests, the scanner-import/RackWire Chromium E2E against Cesium loopback scratch SoR, and the RackWire-to-native-worker Chromium E2E against Cesium loopback scratch monitoring/SoR schemas. The worker test verified 1/1 loopback host, one candidate, retry idempotency, adoption, and terminal recovery after browser session state removal.

Review remains pending. This is an internal same-lab change and has not had a cross-lab reviewer. Wider subnet authorization, deployment behind PHP-FPM, durable worker restart/requeue policy, and scanner-device streaming/automatic ingest remain unverified or outside this patch. The scanner app integration remains a preview/apply import of a schema-v1 JSON snapshot; it does not claim live device streaming.


## Staged development deployment

The committed `4b50cca` candidate was built as a clean Release configuration (IO/SQLite/JSON enabled) and packaged as `dev-20261022-4b50cca`. Archive SHA-256: `b271c39fad19fa9ece8dbef96a07e94285f4a7ffd22dca9ad6b83754e49d0ab4`; 180 allowlisted files; clean source tree; pinned RackWire gitlink. The release installer suite passed 7/7 (two dirty-tree-only tests skipped). It is installed under Cesium `~/.local/share/solarinet-dev/release-root`. PHP CLI serves the package on `127.0.0.1:17701`; UI shell and public auth configuration return 200; unauthenticated scan history returns 401. It is configured for isolated scratch schemas on loopback port 13316. Chromium then signed in with a temporary operator credential, read requester-scoped scan history (200), logged out with CSRF (200), and received 401 from `whoami`; no page errors occurred. The temporary credential, auth file, rate directory, and session were removed, and local login was disabled again. This is not PHP-FPM/Apache and has no persistent scan worker.


### Authenticated deployed-browser follow-up

On 2026-10-22, Chromium accessed the installed loopback PHP CLI service through an SSH tunnel. A temporary random operator credential authenticated through the actual dashboard login form. The requester-scoped scan-history endpoint returned HTTP 200 with an empty list; CSRF-protected logout returned 200; subsequent `whoami` returned 401; no browser page errors occurred. The temporary auth file, password, rate directory, and browser session were removed, and the listener was restarted with local login disabled. PHP-FPM/Apache, OIDC, persistent worker startup, and production data remain unverified.


## Installed native scan and adoption follow-up

Cesium PHP CLI initially lacked `ctype`, causing Discovery input validation to fail before admission. Installing `php8-ctype` fixed the host runtime. The packaged `solariServer`, `solariScanWorker`, and PHP dashboard now run against separate loopback scratch schemas; the active TCP listeners on 17711, 17713, and 17714 bind only to 127.0.0.1, TLS is disabled for this loopback instance, and auto-discovery is off.

Chromium submitted `127.0.0.1/32` through the installed dashboard. Native worker and DB evidence show scan 9 completed with one host and one candidate. In a fresh browser session, the dashboard restored the job and candidate from durable history; operator adoption created a monitoring asset and SoR network entity with the IPv4-mapped loopback address, and the C bridge dispatched local probes. This is not a physical inventory unit or a lab-subnet scan. The operator credential remains in a mode-0600 file on Cesium; the PHP, server, and worker listeners are loopback-only and run as user processes, not systemd services. PHP-FPM/Apache, OIDC, restart/reboot recovery, larger ranges, physical scanner hardware, main-branch promotion, and cross-lab review remain open.


## PHP deployment prerequisite discovered

The live dev host's PHP CLI had neither `ctype` nor `openssl` loaded. Discovery failed at `ctype_digit()`; OIDC verification would also be unavailable without `openssl_verify()`. Installed the matching openSUSE packages `php8-ctype` and `php8-openssl`, restarted the development PHP server, and verified ctype, OpenSSL, and PDO MySQL functions in the active runtime. Added a reusable runtime extension check to dashboard CI and documented package requirements. `php tests/dashboard/test_php_runtime_extensions.php` passes locally; hosted CI for the new check is pending.


## 2026-10-22 candidate delta: realtime polling lifecycle and Cesium dev activation

The current root candidate is `c4b8975eef46f860aef90b11e50aaabbf4c3ed03`, pushed to both Forgejo and GitHub on `integrate/stability-20261020`. GitHub PR #37 remains draft. Hosted run `39730919455` passed all 17 checks. RackWire remains pinned at `a66bd218acc89b668a8f1a89291f2c32bb38ac59`, with draft PR #1 green. The scanner-ingest repository remains local-only at `c2a3f7b` without a configured remote.

The Discovery UI now forwards abort signals through API helpers, prevents overlapping status polls across mounts, cancels a pending request when leaving the screen, and ignores stale completion updates. Browser acceptance delays worker status responses, navigates away mid-poll, verifies cancellation, returns to Discovery, and asserts no more than one status request is active; the existing live scan, candidate refresh, durable recovery, and adoption assertions still pass. The scanner/RackWire acceptance continues to cover starting the same realtime workflow from RackWire and handing candidates back for review and ingest.

The clean `dev-20261022-c4b8975` package (180 allowlisted files, source tree clean, RackWire gitlink pinned) is installed under Cesium `~/.local/share/solarinet-dev/release-root`; archive SHA-256 is `aa2b9fe778cf5592323e592b6db54e12efaf62db9365a3eade6008e536165847`. The Release build completed, and `deploy/dashboard/test_release.py` passed 9 tests with 2 dirty-tree-only skips. Remote checks confirmed the active release points at `dev-20261022-c4b8975`, the loopback UI returns HTTP 200, unauthenticated scan history returns HTTP 401, and served assets contain both the API abort-signal forwarding and poll guard. Prior authenticated scan/adoption acceptance used isolated scratch schemas. The instance is development-only and loopback-bound.

**UNVERIFIED / pending:** PHP-FPM/Apache behavior, production database migration/rollback, wider subnet throughput, worker restart/reboot recovery, physical scanner streaming, and cross-lab review. Claude CLI has no authenticated session and no cross-lab reviewer is available in this environment. No production deployment, main merge, release tag, or scanner-ingest publication occurred.


## 2026-10-22 candidate delta: intranet-only Discovery scan scope

The current root code candidate is `c1ce64d315bf959fbd81fa7b44f198188e59d15f`, pushed to both Forgejo and GitHub on `integrate/stability-20261020`. GitHub PR #37 remains draft; hosted run `39733247468` passed all 15 jobs. The previous code head `c4b8975` passed all 17 checks in run `39730919455`; the docs-only follow-up at `2f850c3` passed after rerunning a core job that had stopped before compilation on a hosted Microsoft apt endpoint 403.

The dashboard API and native C worker now reject scan CIDRs outside RFC1918 private, loopback, and IPv4 link-local space. The whole requested CIDR must fit inside one allowed block, preventing a private starting address from masking a range that crosses into public space. The existing 4,096-host cap remains. Regression coverage accepts block-boundary private examples and rejects RFC 5737 documentation ranges, public IPv4 ranges, and cross-boundary ranges. Related browser fixtures now use intranet scan addresses. Local PHP helper/route tests and full native CTest (37/37) passed; the updated C range cases were rerun (scan/control 2/2). Hosted run `39733247468` passed the database-backed HTTP and browser-to-native-worker scan/adoption checks.

At the time of the original range-policy note, Cesium had not yet been updated. The follow-up deployment is recorded above. **UNVERIFIED:** larger-range throughput, PHP-FPM/Apache, and cross-lab review. The deployed acceptance used only the isolated loopback development database; production DNS and UniFi were untouched.
