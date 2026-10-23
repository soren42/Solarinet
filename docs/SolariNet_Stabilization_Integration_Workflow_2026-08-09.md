# SolariNet — Stabilization & Integration Workflow

*Program plan responding to the independent review · 2026-08-09 · owner: Jason C. Kay*
*Source findings: `docs/SolariNet_Findings_and_Recommendations_2026-08-09.md` (commit `586cac1`)*
*Dossier under review: `docs/SolariNet_Architecture_Review_2026-08-09.md`*

---

## 0. Mandate

**Feature development is frozen.** No new capability, page, device integration, or
protocol surface ships until this program's gates are met. Two goals only:

1. **Stability** — a successful response must mean *authorized, durably persisted,
   correctly delivered, actually applied, and currently fresh* — not merely that a
   socket accepted a message. This is the review's spine and the project's own
   first principle (honesty) applied to the control plane itself.
2. **Integration** — the pages, the components, and the services they represent
   must be tightly interlinked: one entity spine, one freshness/health contract
   surfaced everywhere, and consistent cross-navigation between a thing and
   everything known about it.

These are not separate programs. **The place they meet is honest state**: a page
that shows cached data as "live" (F-13), a UniFi panel that stays green on a failed
poll (F-25), a health record written twice (F-10) — each is simultaneously a
fragility bug *and* a broken integration seam. The integration track below is built
on the same honest-state primitives the stability track installs.

### What "frozen" explicitly means

Not production-authoritative, not to be extended, contained in Phase 0:
**Tab5 approvals · one-button PXE · arbitrary MCP SQL · Opie automation · remote CA
automation · LDAP/AD write paths beyond current use.** These stay lab-only until
their release blockers close (review §4, §7.9).

---

## 1. How to read this plan

- Every review finding **F-01…F-31** is dispositioned in §3 with a track, a phase,
  and — where I depart from the review's priority — a stated reason.
- **Priority reconciliation.** The review's P0/P1 rank severity *against the
  project's claims*. I keep those labels but add a **homelab blast-radius** column,
  because a fleet-of-one on a trusted LAN changes urgency (not correctness). Where
  I re-sequence, it is sequencing, never a downgrade of the underlying defect.
- Tags: **[Review]** = the reviewers observed it with a file:line cite; **[Judgment]**
  = my sequencing/scoping call; **[Verify]** = needs a live check before we rely on it.

---

## 2. The two tracks and how they interlock

```
   STABILITY TRACK                         INTEGRATION TRACK
   (review Phases 0–4)                      (woven through, my addition)

   P0 containment          ─────────────►  I0  capability-maturity labels in UI
   trust boundaries        ─────────────►  I1  ONE authorization matrix as the
   (authz, identity)                            PHP↔solariCtl↔SCP contract
   durable state           ─────────────►  I2  SoR entity spine + provenance
   (SoR CDC, migrations)                        surfaced on every page
   honest freshness        ═════════════   I3  freshness/health contract on every
   (F-10,F-13,F-25)         SHARED FIX          page + every integration source
   job decoupling          ─────────────►  I4  entity hub: node → alerts, config,
   (scans off the loop)                         topology, rackwire, panel, SoR
```

The arrows are dependencies: you cannot build I1 (one authorization matrix
surfaced in the UI) without doing F-02 (server-side authz) first. **The middle row
is one body of work counted under both goals** — do it once.

---

## 3. Finding disposition

Disposition legend: **Adopt** (do as recommended) · **Adapt** (do, scoped for a
homelab) · **Contain** (disable/gate now, real fix later) · **Fold** (satisfied by
another finding's fix).

### P0 — release blockers

| ID | Finding | Blast radius (homelab) | Disposition | Track / Phase |
|---|---|---|---|---|
| **F-02** | Dashboard authz incomplete; ≥13 write routes skip role check | **High — live now.** Dashboard is network-exposed; any viewer session can mutate inventory/monitoring. | **Adopt.** Default-deny non-GET in one policy layer; route×method×role metadata. | Stability + **I1** / Ph 1 |
| **F-03** | No CSRF / Origin defense on cookie-auth writes | **High — live now**, compounds F-02. | **Adopt.** Synchronizer token + Origin/Referer + require Content-Type. | Stability / Ph 1 |
| **F-05** | Viewer-triggerable scan starves the leadership lease | **Medium-High.** Single-thread block is real even fleet-of-one (ingest + control stall). | **Adopt, split:** gate scan behind operator *now* (rides F-02); real fix = job runner. | Stability / Ph 1 gate, Ph 3 fix |
| **F-11** | solariCtl wire unsafe both ends (4 KiB single read; PHP partial-write = success) | **Medium.** Truncation/desync on large config/control payloads. (This is dossier BUG-C18.) | **Adopt.** Length-prefixed framed I/O + looped I/O + deadlines + caps. | Stability / Ph 1 |
| **F-01** | Node cert not bound to frame `sourceNodeId`; gossip accepts unbound IDs | **Medium.** Requires an *already-enrolled* compromised client. Trusted LAN, ~9 hosts → lower than the exposed dashboard, but it is the identity foundation. | **Adopt, sequence after F-02/03.** CSR CN syntax matches the dotted-role parser and has a regression test. Approval compare-and-sets PENDING and atomically stores the node row, public certificate, and decision while clearing the CSR. Token-backed submission, digest-only token storage, expiry, repeatable signed-certificate pickup, and client acknowledgement/revocation are implemented. Fresh-schema MariaDB tests exercise the approval transaction and token lifecycle; installer tests cover successful/failed forced renewal with key/certificate preservation and `--resume` recovery after failed ACK without replacing a changed leaf. Verified manually on 2026-10-20 against a disposable MariaDB schema, the full IO-enabled `solariServer`, a temporary CA, and the PHP enrollment API: TLS-verified HTTPS→PHP→solariCtl→MariaDB token submission; over the same real bridge, pending→approved transaction, CA certificate pickup and OpenSSL chain validation, and token acknowledgement/cleanup. A CSR with a host-only CN was rejected; a role-prefixed CN with the enrolled DNS SAN was approved. The automated HTTPS fixture test also passes, and `tests/integration/test_enrollment_e2e.py` now repeats the full real-server exchange against a scratch schema; it passes locally and is wired into the full-server CI job. CI execution remains pending. The ingest/control gate matches authenticated CN to `sourceNodeId` on every frame, and HELLO requires role and FQDN to match that CN before node upsert; unit tests cover wrong node ID, CN, role, FQDN, and absent identity. The DB identity query now rejects `state='retired'`; approved enrollment resets a retired node to `unknown`. The MariaDB integration test verifies reject-after-retirement and reactivation after the enrollment upsert. Migration 024 stores the issued leaf DER SHA-256 fingerprint and serial atomically with approval. NNG 1.x exposes peer CN/SAN but no peer DER, so live leaf fingerprint comparison remains open. Gossip requires mutual TLS and binds each PEER_ALIVE source ID to authenticated `monitor.<fqdn>` CN; the loopback test verifies convergence, forged-ID rejection, and rejection of a CA-signed leaf revoked by an X.509 CRL bundled with the CA PEM. This verifies enforcement through pinned NNG 1.8.0 / mbedTLS 3.6.2, not operational TLS revocation: the candidate has an operator-only, two-step revocation ledger command, historical issuance inventory/backfill, and a CA-signed CRL generator. The command reports `tls_effective=0`; an HTTPS validator/atomic installer, per-service timer, and issuer-scoped static publisher now exist locally. No peer-trusted HTTPS vhost or peer is configured to use them. Missing-CRL rollout policy and central acknowledgements remain open. The node ID contract is resolved for this release: a read-only production query on 2026-10-21 found all nine deployed IDs match FNV-1a-64(`hostFqdn|roleNumber`), so retain `solariNodeIdFromFqdnRole()` and remove the nonce formula unless a future nonce lifecycle and migration are designed. The same query found all nine DB `certCn` values are host-only, while the candidate requires role-prefixed CNs; this is a certificate migration gate, not evidence independently validating leaf subjects or SANs. Do not cut over the stricter candidate until certificates are re-enrolled or a reviewed compatibility transition is ready. Remaining identity work: complete and test fleet CRL distribution/activation and freshness enforcement; select a supported peer-certificate API or verified handshake callback for fingerprint enforcement; and add a server-signed monitor-membership epoch. | Stability + **I1** / Ph 2 |
| **F-04** | PHP + server share the `jason` account; API served from writable checkout | **Medium-High.** “PHP holds no CA material” is convention, not a boundary; source-write = code-exec. | **Candidate implemented and disposable-boundary tested:** hardened service identities, immutable release paths, `SO_PEERCRED`, queued privileged requests, and scoped dashboard grants. Keep open until the full boundary is installed and verified on Xenon. | Stability / Ph 2; candidate complete, host cutover open |
| **F-06** | Tab5 approval request and response binding | Only if enabled. | **Contain now** (keep non-production); v2 broker/firmware contract is implemented in the local candidate, with broker and host canonical-vector tests passing. Complete disposable-MQTT, pinned-core firmware, trust rotation, and hardware gates in the [approval-binding contract](rework/F06-approval-binding-contract.md) before promotion. | Contain / Ph 0; implementation partial, verification open |
| **F-07** | PXE executes unauthenticated artifacts as root | Only when provisioning runs. | **Open P0:** five entry points fail closed by default. A CMS-signed release manifest/stager covers payload paths, sizes, modes, hashes, target, and validity; offline tamper/path/trust tests pass. Authenticated iPXE + Secure Boot, installer/firstboot verification, and isolated VM acceptance remain unimplemented. Keep PXE isolated and disabled until [F-07 gates](rework/F07-signed-provisioning-contract.md) pass. | Contain / Ph 0; candidate partial, integration Ph 3 |

### P1 — required for a trustworthy stabilization release

| ID | Finding | Disposition | Track / Phase |
|---|---|---|---|
| **F-08** | SoR CDC can lose events across broker/target failures | **Candidate implements** publisher confirms + mandatory routing before checkpoint, apply-before-ack consumers, and periodic reconciliation; 11 local fault-boundary tests pass. Disposable-broker/DNS-chain tests exist; deployed restart and multi-target recovery remain open. | Stability + **I2** / Ph 2; verification open |
| **F-10** | Health/convergence records can lie | Duplicate monitor report persistence is removed; client and monitor config controls stage, fsync, and persist before activation/ack (monitor fault suite 5/5). Migration 034 binds generic results to a persisted node/correlation/verb/epoch record and atomically consumes it with convergence; isolated migration/export and live DB tests pass. | Candidate implemented; full integration/release verification remains |
| **F-12** | Schema lifecycle not reproducible (no ledger; duplicate ordinal 016; `--init-db` applies a stale hybrid) | **Adopt.** Candidate has one migrator per DB, immutable full-file IDs + checksums + version ledger, and deterministic ordering for both 016 files. Monitoring and SoR snapshots are generated from clean migration runs and checked by CI. Legacy database adoption remains open. | Stability + **I2** / Ph 2 |
| **F-13** | Original review found cached authenticated GETs presented as live and SSE holding FPM workers | **Candidate implements core and per-source freshness:** API requests are network-only; core failure retains real data with a stale label, age, and mutation pause; optional endpoints retain their last successful payload and expose `fresh|stale|unknown` plus last success/attempt and a partial-data banner; retired SSE route returns 204. Two-session authenticated Chromium acceptance now verifies stale labeling, blocked writes, independent session state, and recovery; staged production verification remains open. | **SHARED: Stability + I3** / Ph 1 |
| **F-14** | OIDC accepts tokens w/o `exp`; break-glass local login always callable + unrate-limited; roles never revalidated | **Candidate implemented, runtime verification partial:** OIDC claim/state checks, opt-in local login with private-directory rate limiting, idle/absolute expiry, and local-account role revalidation have focused tests. HTTPS UserInfo now refreshes OIDC groups/roles every 60 seconds with subject binding and refresh-token rotation; provider errors downgrade the session to viewer. Verify deployed provider claims/configuration and hosted CI before closing. | Stability / Ph 1 |
| **F-09** | Server HA lease and activation semantics | Standby bridge is read-only for three scoped verbs; bind failure releases its exact token and backs off one TTL. Two-process test passed on a disposable MariaDB schema, including standby takeover, lease renewal during a slow scan, and orphaned-job recovery. Production HA remains unverified. | Candidate implementation verified locally; release verification remains open |
| **F-15** | Opie "read-only" not confidentiality-safe (alert text → prompt; generic Read/Grep tools; runs as `jason`) | **Contain now** (dedicated sandboxed account, drop generic file tools, inventory allowlist), then harden. | Contain / Ph 0, Ph 3 harden |
| **F-16** | Fleet provisioning leaves server-minted key material staged; firstboot never enrolls | **Adopt.** Node-generated keys + one-time CSR token; atomic cleanup; prove PXE→first-report in a VM. | Stability / Ph 3 |
| **F-17** | DNS/AD apply lacks safe deploy semantics (direct zone overwrite; no `named-checkzone`; creds in argv) | **Adopt.** Render+validate+atomic-switch+health-check+rollback; durable serial state; protected AD credential channel. | Stability + **I2** / Ph 2–3 |
| **F-18** | MCP database query policy | **Code containment implemented:** closed fixed-query registry, bound and validated values, result caps, adversarial boundary tests. Still verify dedicated read-only MariaDB grants and review server-side resource limits. | Contain done / deployment verification open |

### P2 — before scale/HA/maintainability claims (Phase 3–4)

`F-19` monitor probing sequential/capped · `F-20` HRW membership unproven (narrow the
"coordinator-free" claim) · `F-21` spool no quota/age, seq reuse · `F-22` SoR multi-step
writes untransacted · `F-23` durable decommission confirmation (implemented in migration
025; covered by live DB tests) · `F-24` Tab5
secret-at-rest unproven · `F-25` UniFi false-green (**Fold into I3** — same freshness
contract) · `F-27` backups nonempty ≠
restorable · `F-28` sparse referential integrity/orphans · `F-29` mTLS configurable not
invariant.

**Disposition:** `F-23`'s server-side confirmation gate is implemented in the current candidate: migration 025 persists a
SHA-256 challenge digest with a ten-minute expiry; confirmation binds node, scope, and
operator, consumes the challenge and retires the node in one transaction. Forced retirement
cancels pending challenges atomically. The first request only issues a token; the second
consumes it before publishing the wipe directive. Durable delivery retry and node
acknowledgement remain open because the PUB transport has no persistent outbox. Tests cover
bad digest/scope/operator, expiry, replay, and cancellation. Production rollout remains
unverified. `F-26` is fixed in the current candidate: authbroker now refuses
empty/example HTTP tokens and its bearer check fails closed. Local tests cover
startup validation and missing/wrong credentials; runtime deployment remains
unverified. Adopt the remaining items as a batch in Phases 3–4. `F-25` is pulled forward into the
Phase-1 freshness work (I3) because it is the same primitive. `F-20` is a
*documentation* fix too — narrow the "coordinator-free" language now; it costs
nothing and stops overclaiming.

### P3 — hardening/debt (ongoing)

`F-30` warning-clean CI + sanitizers + fuzzing + strict envelope validation ·
`F-31` CSP/HSTS/headers under the no-build constraint. **Adopt into the CI gate (§5)**
and close the remaining browser verification. The dashboard vhost now emits
HSTS, referrer, framing, and MIME-sniffing headers, with a same-origin CSP on
the main SPA document. Runtime Babel still requires `unsafe-eval`, and React
inline styles require `unsafe-inline`; RackWire's separate HTML shell retains
its inline scripts/styles and is not covered by this policy. A Clang/libFuzzer gate now exercises
frame, TLV, and message parsers; reserved-bit, flag, and TLV-count validation
is implemented symmetrically in the frame builder and parser. Protocol v1 rejects its reserved header
byte, unsupported flags (including compression without a decoder), malformed
TLV streams, and TLV-count mismatches while preserving unknown TLV types.
The deterministic fuzz smoke and strict envelope regression tests pass locally.
The Clang CI job compiles every test target with `-Werror`, then runs
the complete nine-test suite; it has not run remotely because the candidate is
not pushed. Cross-lab review remains open.

---

## 4. The phased workflow

Each phase has an **exit gate** that is an *executable test*, not a judgment call.
No phase is "done" because the code compiles.

### Phase 0 — Contain unsafe capabilities *(days, not weeks)*

The cheapest, highest-leverage work. Nothing here is a redesign; it is turning
things off and putting an operator gate in front of the rest.

1. Disable Tab5 approval authority and root PXE flows (F-06, F-07) — explicit,
   non-default, isolated-network only.
2. Replace arbitrary MCP SQL with fixed tools/views + read-only account (F-18);
   remove generic file tools from Opie and move it to a sandboxed account (F-15).
   **Candidate update:** the generic `solarinet_db_query` tool is removed; fixed
   database tools remain. Opie now has a candidate single MCP probe tool,
   disables built-in Claude tools, and has a dedicated-account,
   systemd-hardened service template. The probe uses an exact host allowlist
   and a forced-command remote wrapper; process arguments and full service unit
   contents are excluded from diagnostics. DB/API credentials are removed from
   the Claude child environment. `deploy/opie/grants.sql`
   specifies column-level DB rights for only its alert reads and report writes.
   Deployment remains gated on provisioning those DB/RabbitMQ identities and
   Claude credentials, installing pinned host keys, provisioning the restricted
   remote SSH account and narrowly scoped sudo rules, and verifying the
   installed runtime on Xenon.
3. Make **every human mutation operator/admin-only** and add CSRF + Origin checks
   (F-02, F-03) — this single change also neutralizes the *trigger* for F-05.
4. **F-13 containment and per-source freshness are implemented in the candidate:**
   service-worker API caching is disabled, `/api/stream` returns immediately,
   and optional endpoint failures are surfaced with last-success age. The
   authenticated multi-session browser acceptance now covers stale labeling,
   write blocking, session isolation, and recovery; production outage behavior
   remains a staged-deployment check.
5. Restrict dashboard exposure + rate-limit/disable break-glass local login (F-14
   partial), while the boundary is rebuilt.
6. **Integration I0:** stamp each capability with a maturity label
   (prototype / lab / production / authoritative) in one manifest, and surface it in
   the UI so a lab-only feature *reads* as lab-only.

   The dashboard manifest is `window.SOLARI.capabilityMaturity` in
   `dashboard/public/data.jsx`; `dashboard/public/app.jsx` renders its route label
   above each page. Frozen controls remain separate manifest entries, so a page can
   name a contained surface without making its entire parent feature look available.
   `tests/dashboard/test_capability_maturity.js` checks route coverage, recognized
   maturity values, and containment notes for the frozen surfaces. Maturity describes
   release readiness; it does not describe live operational health.

**Exit gate:** a generated route × method × role test passes (default-deny proven);
CSRF/Origin tests pass; unsafe prototype features cannot be enabled by accident;
maturity labels render in the UI.

### Phase 1 — Honest boundaries and honest state

The first release-quality phase. Trust at the edge, truth in the browser.

1. **F-02 → I1:** one server-side authorization policy layer; declare route metadata
   (required role + service principal). This metadata *is* the first slice of the
   integration authorization matrix.
2. **F-03:** synchronizer token + strict Origin/Referer + Content-Type enforcement.
3. **F-11:** length-prefixed framed solariCtl protocol (or looped I/O + newline
   completion + deadlines + hard caps), both ends; fragmentation/partial-write tests.
4. **F-05 (gate):** discovery/provision/deploy verbs require operator; the loop
   still runs them synchronously *for now* but cannot be triggered by a viewer.
5. **F-14:** OIDC claim/state hardening; break-glass independently disabled;
   idle/absolute expiry; role revalidation.
6. **F-13 → I3 (the hinge):** the candidate caches only the app shell, labels
   retained core data stale with its age, blocks mutations until recovery, and
   no longer holds SSE workers. Optional integrations expose
   `{lastSuccess, lastAttempt, state: fresh|stale|unknown}` and a partial-data
   banner. The two-session Chromium test now verifies core outage, stale-write
   prevention, unaffected-peer isolation, and recovery; staged production
   verification and broader authenticated auth/session flows remain open.

**Exit gate:** negative auth suite proves wrong-user/role/origin/stale-token cannot
act; framed-I/O fuzz passes at every byte boundary; no code path presents cached
data as live; the freshness badge renders on at least the alerts and nodes pages.

### Phase 2 — Durable state and reproducible schema

1. **F-12 → I2:** one migrator per DB (control-plane + SoR) with immutable full-file
   IDs, checksums, a version ledger, and honest failure reporting. The two 016
   migrations retain their historical ordinal but have distinct ledger IDs and a
   deterministic order. Generated snapshots are restored in clean-schema tests;
   verified adoption of the existing production databases remains a release gate.
2. **F-08 → I2:** publisher confirms + mandatory routing before checkpoint; consumers
   ack only after all targets apply; periodic authoritative full reconcile;
   broker-loss / partial-apply / duplicate-delivery fault tests.
3. **F-10 → I3:** persist monitor reports exactly once; save/fsync/atomically
   activate config *before* advancing the applied epoch; persist directives keyed by
   node+correlation+verb+epoch and consume one matching pending directive.
   **Observed transport gap (2026-10-20):** reports previously traveled over
   one-way NNG PUSH/PULL. The sender removed a spooled frame once the local PUSH
   socket accepts it; the server cannot confirm the report transaction over that
   channel. **Candidate implementation (2026-10-20):** CLIENT_REPORT,
   MONITOR_REPORT, and SURVEY_RESP now use the existing authenticated REQ/REP
   control endpoint with ACK_REQ; ACK echoes the report sequence and session and
   is built only after `serverIngestDispatch` succeeds. Report spools retain
   entries until that ACK validates, and duplicate durable receipts are ACKed
   idempotently. Loopback client/monitor tests pass, including rejection of a
   mismatched ACK while the report remains spooled. This remains a candidate
   until server-handler tests exercise commit failure and ACK loss against the
   real receipt path, and primary/failover, auth, protocol compatibility, crash
   recovery, and live fleet rollout gates pass. Do not count local socket
   acceptance as durable delivery.
4. **F-01 → I1:** canonical identity contract enforced on every ingress
   (cert SAN/CN ↔ node ID ↔ role ↔ FQDN); persist and enforce the leaf certificate
   fingerprint; add a server-signed monitor-membership epoch. HELLO and gossip
   identity checks are implemented with negative tests.
   **Observed dependency gap (2026-10-20):** the installed `libnng-dev` 1.11-3
   headers expose verified peer CN/SAN options, but no peer certificate object
   or DER API. The current transport wrapper therefore cannot compute a leaf
   certificate fingerprint. The NNG v1.12.0 public header also has no
   `nng_pipe_peer_cert` or `nng_tls_cert` declaration; the certificate-object
   APIs appear in the current NNG 2.0 draft reference, whose migration guide
   describes breaking API changes. Do not move production to an alpha API or
   substitute a CN/key hash for a certificate fingerprint. Keep fingerprint
   binding open until the project selects a supported NNG release or adds a
   verified handshake callback. Separately, pinned NNG 1.8.0 supports CRLs in
   the configured CA PEM bundle: the loopback gossip integration test confirms
   a revoked client leaf is rejected. The candidate includes historical issuance
   inventory/backfill, a CA-signed CRL generator, and a peer updater that
   verifies and activates one issuer CRL. No publisher or configured peer exists;
   missing-CRL rollout policy and fleet acknowledgements remain open. The
   transport wrapper now explicitly requires
   `NNG_OPT_TLS_VERIFIED == true` on the received pipe before returning its peer
   CN. The loopback mTLS convergence test passes for trusted peers and rejects
   the revoked leaf; a plaintext pipe is also rejected as lacking a verified
   peer identity. This adds an explicit runtime chain-verification guard; it
   does not add leaf fingerprint comparison. Evidence: [NNG 1.8.0 public
   header](https://github.com/nanomsg/nng/blob/v1.8.0/include/nng/nng.h), [NNG
   1.8.0 mbedTLS backend](https://github.com/nanomsg/nng/blob/v1.8.0/src/supplemental/tls/mbedtls/tls.c), [NNG TLS API
   reference](https://nng.nanomsg.org/ref/api/tls.html), and [NNG 1.x migration
   guide](https://nng.nanomsg.org/ref/migrate/nng1.html).
   The implementation contract for the missing ledger, generation, freshness,
   and fleet-acknowledged distribution path is
   [F-01 certificate revocation lifecycle](rework/F01-revocation-lifecycle-contract.md).
5. **F-04:** split least-privilege service accounts; deploy immutable artifacts
   outside the working tree; `SO_PEERCRED` on the socket; canonical hardened units.
6. **F-17 (render side) + F-22:** validated atomic DNS release with rollback; SoR
   multi-step writes inside transactions with idempotency keys.

**Exit gate:** crash/fault suite proves no silent event loss, no false convergence,
no partial domain mutation, no schema drift across clean-install / every upgrade
path / idempotent rerun. Identity negative suite proves cross-node/role/cert/CA/name
rejection.

### Phase 3 — Decouple operations and prove faults

1. **F-05 (real fix):** bounded background-job runner; the server loop enqueues,
   returns a job ID, stays deadline-driven; accepted Unix clients are nonblocking
   with read deadlines. Move discovery, provisioning, deploy, DNS render, SNMP,
   Opie off the request/lease loop.
2. **F-16 + F-07 (real fix):** node-generated enrollment keys; signed provisioning
   manifest verified before execution; atomic token/profile cleanup; PXE→first-report
   proven in a disposable VM.
3. **F-09:** make lease acquisition + epoch increment + endpoint activation one
   observable state machine; two-real-DB failover + bind-failure tests.
4. **F-17 (operational) + F-27 + F-26 + F-21:** DNS rollback drill; scheduled
   disposable restore drill; authbroker fail-closed +
   resource caps; spool quotas + persisted sequence state.

**Exit gate:** the control server renews its lease and ingests telemetry throughout a
worst-case scan, a broker loss, a consumer crash, and a two-server failover — proven,
not asserted.

### Phase 4 — Prove the intended architecture

1. **F-19, F-20:** ≥3 monitors in a lab topology; HRW convergence under join / loss /
   partition / stale + forged membership / uneven latency; bounded-concurrency
   probing with overrun telemetry.
2. Capacity test at the stated 300-host ceiling.
3. SLOs: sample freshness, alert latency, command convergence, outbox lag, DNS
   convergence, recovery time.

**Exit gate:** measured results support the resilience, honesty, frugality, and
portability claims. Only now does roadmap/feature work resume.

---

## 5. Integration track detail (the second goal, made concrete)

The stability phases install the primitives; this is what "tightly integrated and
interlinked" *is*, as a deliverable:

- **I0 · Capability maturity in the UI** (Ph 0). One manifest of feature → maturity;
  every page badges lab-only surfaces. Stops the blur the review flagged (§7.9).
- **I1 · One authorization matrix** (Ph 1–2). Route metadata (F-02) + solariCtl verb
  policy (F-04) + SCP identity contract (F-01) become *one* generated table:
  principal × action × enforcement-point. It is a security artifact and the
  canonical contract that binds the three layers together. CI generates tests from it.
- **I2 · SoR entity spine + provenance** (Ph 2). Durable, migration-managed SoR
  (F-08, F-12) is the single backbone every page hangs off. Scanner intake (I5)
  joins this spine only through a versioned, provenance-preserving import seam.
  Provenance (observed/adopted/authoritative) becomes visible, not just modeled.
- **I3 · One freshness/health contract** (Ph 1, extended Ph 2–3). The
  `{lastSuccess, ageSeconds, state}` envelope + shared badge, applied to **every**
  page and **every** integration source (UniFi F-25, DNS, AD, monitors, panel,
  broker). "Cached data must never masquerade as live" becomes a system invariant
  and the visible connective tissue between services.
- **I4 · Entity hub navigation** (Ph 2–3, no new backend). Each node/entity page is a
  hub that cross-links to everything known about it — its alerts, its config + applied
  epoch, its topology position, its RackWire connections, its panel state, its SoR
  provenance. This is the "pages need to be interlinked" ask, built on I2's spine and
  I3's freshness. Largely a frontend composition task once the spine and contract exist.

> **RackWire is now a dependency, not a subsystem.** The RackWire app lives at
> `dashboard/public/rackwire` as a **git submodule** (`RackWire.git` on Forgejo,
> pinned to a commit — it does not float). The integration seam stays in the SolariNet
> tree: the route `dashboard/api/routes/rackwire.php`, the screen glue
> `dashboard/public/screens-rackwire.jsx`, the SoR migration `netdb/sor/migrations/020_rackwire.sql`,
> and the boundary tests `tests/dashboard/test_rackwire_*`. Consequences for this plan:
> (a) I4 cross-links to RackWire across a **versioned dependency boundary** — treat it as
> an external surface with a pinned contract, not inlined UI; (b) the submodule pin is
> provenance — advancing it (`git submodule update --remote`) is a deliberate act, and a
> plain checkout/merge needs `git submodule update --init --recursive` to match the pin;
> (c) migration 020 remains SolariNet-side under the F-12 migrator (consistent with the
> review's §10 note that it belongs to the SoR tree).

### I5 — scanner inventory intake (authorized subproject)

Jason has added the standalone `~/Code/scanner-ingest` app to the integration
scope and authorized full integration. It stores catalog items and physical
assets in SQLite and has an insert-only SoR exporter (`export_sor.py`) with
preview/apply, provenance, idempotent external references, transaction
rollback, and target-collation collision checks. The dashboard already has a
separate SoR-backed Tags & Scan workflow. Integrate scanner workflows through
that authenticated SoR boundary; do not expose the unauthenticated standalone
LAN server or turn its SQLite database into a peer system of record. Retain the
standalone app as an offline/portable intake path only where it adds value.

**Live preview evidence (2026-10-20):** a read-only importer preview reached the
production SoR but stopped before planning writes because both active scanner
location labels lack exact live SoR matches. The SoR has two active location
rows with the same name, so neither can safely be selected by fuzzy matching.
No production rows changed. A second, diagnostic preview explicitly requested
the supported notes-only fallback and planned two models, two units, two
barcodes, four external references, and one source row (11 audit rows). It warned
that location IDs would remain unset and shared product barcodes would be kept
in notes. Do not treat this fallback preview as an approved import. Reconcile or
explicitly preserve those locations before writing.

**Dashboard intake candidate (2026-10-20):** the integration worktree now
implements an operator-only JSON upload, explicit location/model mapping,
preview and apply through the existing authenticated SoR API, insert-only
provenance, barcode records, serializable apply transaction, advisory lock,
idempotent replay, and rollback on collisions. The standalone scanner app still
captures locally and exports its own snapshot; no LAN listener or remote DB
credential was added. The upload is capped at 1 MiB on both the browser and API
body reader. JSX parsing, PHP syntax, and the route authorization audit pass.
A disposable-MariaDB test covering preview, apply, replay, conflict, and rollback
is in CI and passed locally against a temporary MariaDB 11.8 schema loaded with
SoR migrations 001/014/015/020; fixture rows were cleaned up. **UNVERIFIED:**
the staged operator browser flow, live location decisions, backup/restore
behavior, cross-lab review, release ownership, and production deployment. The app directory has
no root `.git`, so establish versioned provenance and synchronized release
ownership before claiming a subproject release. The local SoR import test gate
is now satisfied; deployment remains gated on the staged operator browser flow,
live location decisions by SoR row ID, cross-lab review, and release ownership.

**Exporter contract follow-up (2026-10-21):** source-side UUID validation now
requires canonical 8-4-4-4-12 text form, aligned with the dashboard
validator. The full standalone scanner suite ran twice against disposable local
MariaDB 11.8.6 instances, over both a private socket and loopback TCP: all 62
tests passed on each run, including the `zxing-cpp` decoder and all 15 database
cases. Added a standalone GitHub Actions workflow that checks out the current
SoR schema source and runs the same full suite against an isolated MariaDB
service. The YAML parses and its TCP path passes locally; remote Actions has
not run. A local Git root now exists, but no GitHub or Forgejo remote is
configured. See `~/Code/scanner-ingest/VALIDATION.md`. Live import, physical location
reconciliation, cross-lab review, release ownership, and deployment remain open.
Live import, physical location reconciliation, cross-lab review, release
ownership, and deployment remain open.

**Duplicate-location UI regression (2026-10-21):** the scanner import UI test
now models the live SoR shape with two `Painted Fern` rows (#9103, #9104). It
checks both IDs are visible in the selector, selects #9104, and verifies the
preview request and resolved-location summary both retain that exact ID. The
focused test passes. This confirms row selection and display; it does not
identify which physical destination corresponds to `Nook` or `Bin #5`.

**Sequencing note [Judgment]:** I4 is the most visibly "integration" deliverable and
the one you'll *feel*, but it is deliberately last — cross-linking pages before the
entity spine (I2) and freshness contract (I3) exist would just wire together data that
still lies. Integration built on dishonest state is worse than no integration.

---

## 6. CI and review gates to install alongside (review §9)

CI now runs the MCP, alertbridge, UniFi, authbroker, sorsync, notification
envelope, steel BIND include, and dashboard authorization suites, alongside
the existing C, dashboard, DNS, and panel checks. It is still
not a release gate. Add, roughly in Phase order:

- `SOLARI_TEST_DB=1` against the scratch MariaDB; **fail if the live-DB suite skips.**
- TLS loopback required; certificate-identity failure tests.
- `git submodule update --init --recursive` before dashboard tests (RackWire is now a
  submodule); run **every** SolariNet dashboard test, not just the bridge subset. RackWire's
  *own* UI tests live in the dependency and are its CI's job; SolariNet keeps the **boundary**
  tests (`test_rackwire_api.php` against the route).
- Locked Python deps. MCP, alertbridge, UniFi, and sorsync now run in CI. The
  authbroker approval round-trip runs against a disposable Mosquitto listener
  bound to loopback. Backup runner and failure-notification suites are in CI.
  Notifyd envelope validation now runs in CI; sender-specific transport checks
  remain open. Fleet provisioning still needs CI coverage when isolated tests
  are available.
- Generated route × method × role matrix (I1); CSRF/Origin tests.
- Strict SCP reserved-bit/flag/TLV-count validation (F-30), migration
  clean-install/upgrade/idempotent-rerun checks, and DNS validators. The full
  server ASAN/UBSAN and Clang/libFuzzer parser smoke gates now run in CI.
- Fault injection in CI: RabbitMQ loss, two-server lease race, partial Unix-socket
  I/O, concurrent config writes, backup restore, service-worker offline.
- Pin actions/runners/toolchains/deps enough to reproduce release artifacts.

**MCP CI enablement finding:** installing the declared optional test extra first
failed because setuptools could not infer the content type of `README.html`;
after declaring it as `text/html`, dependency resolution selected MCP SDK 2.x,
which removed the v1 `FastMCP` import used by this app. The project now caps the
SDK at `<2`; the MCP test suite passes with SDK 1.30.0. CI installs the package
and test extra to exercise this installation path on every push/PR.

**Cross-lab gate (AGENTS.md §4.4):** the review was single-lab by its own admission.
Before committing to the deep architectural choices (identity contract shape, HA
state machine), run one genuine cross-lab confirmation pass. Containment (Phase 0)
proceeds without waiting on it.

### 2026-10-21 continuation checkpoint

- **Identity derivation hardening:** centralized the existing
  `FNV-1a-64("fqdn|role-number")` node-ID derivation in
  `solariNodeIdFromFqdnRole()` and routed client, monitor, server, peer-CN, and
  provisioning paths through it. Fixed role vectors and invalid-input tests
  were added. `solariClient`, `solariServer`, and the four focused CTest targets
  (`test_crypto`, `test_client_report`, `test_peers`, and
  `test_server_provision`) build/pass. This does not change deployed IDs. A read-only production audit on 2026-10-21
  found all nine live IDs match this formula, so the current deployed contract is
  retained and the nonce proposal is retired absent a defined lifecycle and
  migration. Remaining work is fingerprint enforcement, the CRL lifecycle, and
  signed monitor membership.

- **F-05 candidate:** the dedicated scan worker now throttles unchanged enrichment
  progress while persisting the first completed-probe transition immediately.
  Regression coverage checks the 1-second heartbeat, result-count changes,
  malformed/empty PID markers, cancellation, and a real child timeout with output.
  The worker target builds; its focused suite passes 12/12. Full CTest passes
  34/34 with the live MariaDB case enabled against a temporary schema migrated
  through 028. Two-host takeover and SIGTERM through the installed unit remain
  unverified.
- **F-17 Khadas candidate:** the live Khadas VIM4 (`chlorine`) remains an active
  authoritative secondary for the forward zone and 17 reverse zones only. The
  refreshed candidate compares its generated zone set against Xenon's expanded
  active `named-checkconf -p` inventory before pushing; a read-only Xenon check
  observed 109 primary/master zones. Candidate suites pass: Khadas 11/11,
  SoR DNS applier 11/11, atomic DNS release 16/16. The candidate README now
  requires Xenon configuration validation and reload before initial refresh.
  Bare-zone/RPZ transfer is still not enabled on live hosts.
- **Review and rollout:** a same-lab adversarial review initially flagged missing
  independent zone-inventory verification and unclear primary-reload sequencing.
  Both are addressed in the candidate and covered locally, but those fixes have
  not yet received a second review. A Claude Fable 5 review was attempted through
  the installed CLI but could not authenticate because its OAuth session expired.
  Cross-lab review and rollback rehearsal remain required before changing live
  DNS. No DNS host, Git remote, or release was changed in this checkpoint.
- **Status-panel documentation:** added a dedicated developer reference and
  matching five-page US Letter PDF under `docs/panel/`. The source is one HTML
  file with inline CSS, JavaScript, and SVG. It covers the API/daemon/firmware
  boundaries, normative wire protocol, firmware modules, build/test commands,
  and release gates. Corrected `status-panel/daemon/README.md`: cJSON 1.7.19 is
  vendored and compiled directly in the current tree, rather than a system shim.
  The daemon's codec and TLS listener suites pass (listener: 16 cases); the
  firmware host suites pass, including 107 framebuffer parity assertions.
  The Pico toolchain build and physical panel behavior were not exercised.
- **Identity documentation:** the Architecture and Plan now retains the
  deployed FNV-1a-64 `hostFqdn|roleNumber` formula: a read-only production query
  found all nine live IDs match it. The proposed nonce has no defined lifecycle
  or migration. The same query found all nine DB `certCn` values are host-only,
  so the candidate role-prefixed CN contract requires certificate re-enrollment
  or a reviewed transition before cutover. The paired PDF was regenerated.
  Remaining F-01 work is CA revocation lifecycle, leaf fingerprint enforcement
  through a supported NNG API, and signed monitor membership.

---

## 7. Sequencing summary

| When | Do | Why first |
|---|---|---|
| **Now (Ph 0)** | Contain F-06/07/15/18; operator-gate + CSRF (F-02/03); candidate F-13 cache/SSE mitigation; maturity labels | Cheap, turns off live exposure, removes the F-05 trigger, no redesign |
| **Next (Ph 1)** | F-02/03/11/14 + remaining F-13 per-source freshness contract (I1 seed, I3 hinge) | Honest edge + honest browser; unblocks the integration contract |
| **Then (Ph 2)** | F-12/08/10/01/04/17 (I1/I2/I3 backbone) | Durable, reproducible, identity-bound state — the spine |
| **Then (Ph 3)** | Job runner F-05; F-16/09/17-ops + fault drills; I4 entity hub | Decouple + prove faults; build the interlinked UI on a true spine |
| **Last (Ph 4)** | F-19/20 multi-monitor + capacity + SLOs | Prove the claims before features resume |

---

## 8. What this program does *not* do

- It does not add features. Anything on the frozen list stays lab-only until its
  blockers close.
- It does not treat every P0 as equally urgent *in time* — it treats the
  network-exposed, trigger-now defects (F-02/03/05) ahead of the
  requires-a-compromised-client defects (F-01), while fixing both. Severity labels
  are unchanged; only sequencing reflects homelab blast radius.
- It does not build interlinked pages (I4) before the entity spine (I2) and freshness
  contract (I3) make the underlying state honest.

---

*Prepared as a working document for export. No code, live state, or deployment has
been changed by this document. Candidate code and documentation are maintained in
the local integration worktree; no commits, pushes, or deployments have been made
as of 2026-10-21. Recommend a cross-lab confirmation of the Phase-2 architectural
commitments before building on them.*

### 2026-10-21 MCP and DNS field checkpoint

- **MCP transport robustness:** `SOLARINET_MCP_PORT` now fails fast unless it is
  an integer in 1–65535. Streamable HTTP startup translates the project setting
  `streamable_http` to FastMCP 1.x's `streamable-http` name and applies the
  configured port. The package floor is `mcp>=1.9.4,<2`, covering that transport
  API. New tests cover defaults, bounds, malformed values, and both startup
  transports.
- **MCP documentation:** corrected the user guide's configuration table and
  duplicate evaluation command; added the monolithic HTML/CSS/JS/inline-SVG
  Developer Reference and regenerated both paired PDFs. The 86-test MCP suite
  passes, all 10 fixture-backed answers verify through tools, and `--check`
  lists all 26 tools without connecting to services. PDFs render as US Letter and
  were visually checked for readable diagrams. Live API/DB integration remains
  separate and unverified.
- **Khadas DNS observation:** read-only SSH inspection found `chlorine`
  (`10.7.0.10`) already running `solari-bind9.service` (BIND 9.18.39), with a
  passing `named-checkconf -z` and successful transfer records in the journal.
  Direct SOA queries to Xenon and chlorine both returned serial `2026102103`.
  No second instance was started and no DNS settings were changed.
- **Steel observation:** the operator reports the ZimaBlade is labeled Steel and
  attached to `laboratory` port 12. A read-only attempt from Xenon received no
  ARP reply and SSH returned `No route to host`; remote imaging was unavailable.
  The checked-in netdb still reserves `iron` at `10.0.0.11`, while live DNS
  answers `steel.akoria.net` at that address. The older offline guide labels
  port 12 `starship`. Preserve this as an unresolved source/field discrepancy;
  do not repoint DNS or make the Zima a tertiary authority until its canonical
  inventory name and the active port mapping are reconciled. The port observation
  is operator-supplied and was not independently queried from the switch.

### 2026-10-21 CI stabilization checkpoint

- **Pinned dependency build:** fixed the GCC version probe in
  `deploy/build-pinned-io-deps.sh`. Under `set -o pipefail`, its early-exit `awk`
  could SIGPIPE the compiler macro dump and terminate the script before setup.
  The parser now consumes the full stream. The pinned mbedTLS 3.6.2 and NNG 1.8.0
  build succeeds with GCC 15.2.
- **Enrollment/CA integration path:** ASan found `strncpy` source/destination overlap in
  `serverStrCpy()` while the server loaded defaulted config fields. The bounded
  helper now uses an explicit length scan plus `memmove`, preserving
  NUL-termination and supporting its same-field fallback callers. The full
  ASan/UBSan build succeeds, and the real HTTPS → PHP → solariCtl → MariaDB → CA
  enrollment/approval/pickup/ack test passes.
- **Repeatable DB suite:** rerunning CTest against the same disposable schema
  exposed fixed enrollment fixture rows and token hashes. The test now deletes
  only its deterministic fixture rows before setup. The full server CTest passes
  twice against the isolated schema (33/33 each run); the transport loopback
  passes 1/1. The test MariaDB used `/tmp/solarinet-mariadb-review-20261021` on
  loopback port 33106; the active MariaDB on port 3306 was not used or modified.
- **Other CI checks:** strict Clang protocol fuzz/build passes 9/9; core CTest
  passes 15/15; panel firmware, daemon, provisioning smoke, and page/firmware
  parity checks pass, including 59 parity assertions. Dashboard non-DB tests,
  PHP lint, JSX parsing, layouts, auth/CSRF routes, RackWire bridge/API, cache
  boundaries, and scanner UI pass. Alertbridge 5/5, UniFi 3/3, notifications
  3/3, backup 10/10 (under the workflow's root context), authbroker 6/6,
  DNS release 16/16, Khadas 11/11, Steel config fixture 1/1, netdb 5/5, schema
  export 7/7, and the SoR applier/unit suite reports 33 tests with 3 broker-only
  cases skipped and no failures. ShellCheck 0.11.0 passes across all repository
  shell scripts.
- **CI coverage repair:** a test-file/workflow audit found the dashboard
  capability-maturity test and Opie MCP/tool/service-isolation tests were not
  in CI. Added them to the dashboard and Python integration jobs. In a shared
  environment with MCP plus Opie dependencies installed, MCP passes 86/86 and
  Opie passes 9/9 with FastMCP 1.30.0 retained.
- **Remaining release gates:** three SoR real-broker tests were skipped because
  no isolated RabbitMQ broker was available locally. Cross-architecture/QEMU and
  macOS jobs were not run on this Linux host. Cross-lab review remains
  outstanding; the Claude Fable CLI session previously failed authentication.
  F-01 identity-contract reconciliation, live API/database integration, release
  review, and deployment remain open. No commit, push, release, or deployment
  occurred in this checkpoint.

### 2026-10-21 Khadas secondary expansion

- **Live DNS backup:** Xenon's primary config passed `named-checkconf -z` after
  adding chlorine transfer/notify access to all 90 single-label zones and the
  RPZ zone. The existing bare-zone files were left intact. Chlorine now serves
  109 zones (forward, 17 reverse, 90 bare-name, RPZ); the new refresh service
  verified current SOA serials for all 109 and Radium's forward zone. Direct
  queries confirmed authoritative answers for `dashboard` and `rpz.akoria`,
  chlorine in the `akoria.net` NS set, and `REFUSED` for recursion. Both
  `solari-bind9` and `step-ca` remain active on chlorine. Xenon's path watcher
  and five-minute retry timer are active. Prior Xenon config fragments are
  backed up under `/var/backups/solarinet-dns/20261021T071429Z`.
- **Full content parity:** added `deploy/dns-khadas/verify-zone-content.py`.
  It compares canonical BIND output hashes for active primary zones and the
  generated secondary list, printing names/digests but no records. Current live
  comparison reports matching zone content and SOAs for all 109 zones. The
  Khadas verifier unit tests pass 4/4; its zone-refresh/config suite passes
  11/11.
- **Protect future SoR updates:** the active legacy applier's bare-zone
  generator now retains transfer/notify access for chlorine and reserved Steel
  (`10.0.0.11`), and its per-zone serial helper preserves unchanged zones while
  advancing serials for changed data. The isolated regression tests pass 4/4.
  A live SoR delivery has not
  yet exercised the updated generator.
- **Steel/ZimaBlade:** Jason identifies Steel as a ZimaBlade on laboratory
  switch port 12; the port label and mapping are operator-supplied, not
  independently queried from the switch. Read-only checks resolve it to
  `10.0.0.11`, receive no ping reply, and return SSH `No route to host`. No
  remote imaging path is available while it is unreachable. Tertiary DNS is
  deferred until console redeployment; the host is not in the NS set.
  The offline host fixture intentionally reserves the future identity `iron`
  at `.11`; live DNS remains `steel` until a deliberate post-console handoff.
- **Inventory UI:** the scanner import warns when SoR locations share a name.
  Regression coverage verifies both duplicate row IDs, mapping submission, and
  preview destination. Focused UI test passes.
- **Release gates:** no commits, pushes, or releases were made. Cross-lab review,
  a live SoR generator invocation, Zima redeployment, and end-user resolver
  failover remain unverified.

### 2026-10-21 local full-build and dashboard regression checkpoint

- Rebuilt the existing full server configuration (`SOLARI_BUILD_SERVER`, IO,
  SQLite, JSON, and tests enabled); all targets completed successfully.
- Full server CTest passed 33/33, excluding only the separately designated
  transport-loopback test. The enrollment installer integration test ran as part
  of this suite. The live-MariaDB CTest target reports pass when its opt-in DB
  environment is absent, so this run does not replace the isolated MariaDB
  integration evidence above.
- Re-ran the dashboard's non-database CI acceptance scripts: RackWire UI/API,
  JSX and layout, scanner-import UI, capability maturity, service-worker cache,
  API freshness, SSE worker release, lifecycle actions, panel UI/mappings, PHP
  control bridge, scan-input bounds, local-login gate, OIDC/session, and authz
  policy/dispatch/enforcement/route audit. Every invoked script passed.
- Ran the standalone `~/Code/scanner-ingest` test suite: 47 passed, 15 skipped,
  and 31 subtests passed. The skipped cases are the real-MariaDB export tests;
  they require a disposable SoR schema and were not pointed at the active local
  MariaDB service. The directory still has no Git root or release remote, so
  synchronized subproject publication is not currently possible from that
  checkout. The dashboard's scanner-import UI path is tested, but the live SoR
  preview previously stopped on unmatched scanner location names; mapping and
  import remain pending.
- Ran the service integration CI suites with their declared dependencies in an
  isolated `/tmp` virtual environment: MCP 86/86, authbroker 6/6, Opie 9/9,
  and backup 10/10 under root. Notification passed 3/3; SoR sync passed 33/33
  after adding a loopback-only RabbitMQ fixture.
  The first non-root backup run failed its intended root-ownership assertions;
  rerunning under the workflow's root context passed all 10.
- Authenticated integrated F-13 browser acceptance remains open. A temporary
  Chromium/Playwright run now covers the standalone RackWire `file://` boundary
  (see the 2026-10-21 evidence below), but this does not exercise authenticated
  multi-session dashboard actions, offline-to-online recovery, or deployed
  session behavior. Adapter, route, freshness, and session tests also do not
  substitute for that integrated browser evidence.
- Closed the local SoR broker-test gap using the CI-pinned RabbitMQ 4 image in a
  disposable container bound only to `127.0.0.1:5679`. All 33 SoR sync tests
  passed, including mandatory-route failure, failed-apply redelivery, and
  heartbeat behavior (previously 3 skipped). The container was removed and the
  loopback listener is no longer present. This is local fixture evidence, not a
  remote CI run or production-broker acceptance.
- Release preflight found that the archive recorded `HEAD` without indicating
  the source tree was dirty. The packager now refuses dirty trees by default;
  explicitly allowed preflight bundles record `sourceTreeDirty: true`, and the
  installer/rollback path refuses them unless separately opted in. Added the
  release package/activation/rollback suite to CI, where it was previously
  absent. All eight local release-flow tests pass, including dirty-source
  rejection, manifest integrity, required embedded RackWire payload, path
  traversal, activation, and rollback. The packager now also requires the
  initialized RackWire checkout to match the parent gitlink and records its
  commit in `RELEASE.json.sourceSubmodules`, preserving subproject provenance
  in the deployable archive. The README includes the required initialization
  step.
- Both Forgejo and GitHub `main` still point to `5e8d6889`; the integration
  branch is 18 commits ahead and the worktree remains dirty. No release bundle
  was retained from the initial provenance test, and no remote branch changed.
- Remote CI remains a cross-repository gate. `.gitmodules` now uses the
  relative sibling URL `../RackWire.git`; after `git submodule sync`, Forgejo
  resolves it to `jason/RackWire`, and the dashboard GitHub Actions checkout now
  initializes submodules recursively. On GitHub the same relative URL resolves
  beside `soren42/Solarinet`. A read-only GitHub query confirms Solarinet is
  public but `soren42/RackWire` does not yet exist, so Actions cannot fetch the
  dependency today. Establish the RackWire GitHub mirror at its reviewed release
  commit before expecting a hosted CI run; keep the CI gate open until that run
  passes. The release manifest now records the submodule commit and the package
  builder verifies it matches the parent gitlink.
- Read-only migration audit of live Xenon/Cesium databases found 29 monitoring
  tables and 41 SoR base tables plus four views, with no migration ledger in
  either schema. The live SoR has the outbox/state tables and 24 triggers from
  the legacy bootstrap; its remaining drift from the current candidate is the
  four tables and two IP-address constraints from migration 021. No production
  schema or data was changed.
- Added ledgered SoR migrations 022–023 for the outbox/state tables and 21 CDC
  triggers; the three inventory-allocation triggers remain owned by 014. Fresh
  install instructions now use `tools/migrate.py`. On a disposable MariaDB 11
  container, all eight SoR migration entries applied and rechecked, the schema
  contained both outbox tables and all 24 triggers, and schema export refreshed
  the snapshot with the 022/023 checksums and trigger definitions. CI now expects
  eight SoR ledger rows and compares trigger names between migration and restored
  snapshot schemas. The scratch container was not the production database.
- `git diff --check` passed. These checks are local evidence only; they do not
  satisfy authenticated multi-session browser acceptance, production DB
  adoption, cross-lab review, release review, or deployment. No commits, pushes,
  releases, or deployments occurred.

### 2026-10-21 RackWire standalone and integration verification

- RackWire's standalone discovery suite passes **45/45**; all four pinned browser
  runtime checksums match. The parent dashboard RackWire API tests pass **57/57**,
  the integrated token/theme bridge passes **41/41**, and the dashboard JSX parser
  accepts all **16/16** screens.
- Added the standalone User Manual and Developer Manual as single-file HTML with
  inline CSS and SVG, plus matching Letter PDFs. Both PDFs have selectable text;
  visual inspection caught and fixed an orphaned Developer Manual section label
  at a page break. The RackWire docs are staged in its checkout but not committed.
- A temporary Chromium 156 / Playwright 1.64.1 setup ran the standalone browser
  acceptance test. It verified network-free `file://` boot, accessible library,
  search, scan, and help dialogs, focus trapping and restoration, Escape close
  from input fields, gear placement persisted across reload, Map/Data navigation,
  and the 390 px toolbar without horizontal overflow. No browser errors occurred.
- The test is checked in at `tests/browser/test_rackwire_standalone.cjs` and a
  dedicated CI job installs pinned Playwright plus Chromium. The hosted job awaits
  the public RackWire GitHub mirror. This local standalone run does not verify
  authenticated SolariNet iframe actions, live telemetry, or SoR commit/rollback.

### 2026-10-21 Scanner-to-SoR transaction verification

- Initialized a disposable MariaDB 11 container on loopback port `33106` using
  the same SoR schema and migration sequence as dashboard CI, then removed the
  container after testing. The live SoR database was not used or modified.
- Scanner import preview/apply/idempotent replay/no-overwrite tests passed;
  authenticated HTTP tests passed for auth, CSRF, preview fingerprint, apply,
  stale-plan rejection, and replay. Scanner UI tests passed for file selection,
  explicit location mapping, preview, and apply. The SoR mirror transaction
  suite passed rollback, concurrent retry, and idempotency checks. Migration and
  schema-export unit tests passed 8/8.
- This exercises the implemented changeover path against scratch data. It does
  not authorize or verify an import of the live scanner export: the two source
  labels still require Jason to map them to the physically correct duplicate
  SoR location rows (IDs `9103` and `9104`) or explicitly preserve them only as
  notes.
- Re-ran the standalone `~/Code/scanner-ingest` suite against a disposable
  MariaDB 11.8.6 container on loopback port `33107`, using the candidate SoR
  schema directory. All 14 MariaDB exporter cases ran and passed; overall, 61
  tests passed and the optional ZXing decoder test skipped because
  `zxing-cpp` was unavailable in that interpreter. The container was stopped
  after the suite. The app has paired user/developer manuals; its directory
  still has no `.git` metadata or publication remote, so its CI workflow and
  release ownership cannot be verified from a local repo or pushed with the
  current two-repo release process.
- A read-only production query confirms locations `9103` and `9104` are both
  active root `site` rows named `Painted Fern`, with no parent path to tell them
  apart. The scanner source-only export still has two items and two assets.
  This confirms the mapping is a physical-location decision; no destination was
  selected by inference and no live inventory rows were changed. The GitHub and
  Forgejo `scanner-ingest` repository lookups both returned not found.

### 2026-10-21 manual integration pass

- Rechecked the operator/developer manual inventory for SolariNet core, the
  status panel, RackWire, MCP, scanner import, and standalone scanner-ingest.
  Each app has paired HTML/PDF manuals. The core Status & Operations guide now
  has a dated current-state panel, an inline system map, a print control, and a
  clear marker that its Phase 7 implementation notes are historical. Its PDF
  remains 9 pages; the new current-state content is selectable and visually
  legible in the rendered PDF.
- Added inline print controls to the core Developer Reference and RackWire user
  and developer manuals. Regenerated their paired PDFs (37, 6, and 5 pages).
  Headless Chromium loaded the three manuals from `file://`, activated each print
  control through its button, and reported no page errors or external requests.
  PDF text extraction confirms the titles and manual content remain present;
  the print button is intentionally absent from the printed pages.
- The Operator Quickref remains a static reference, not a user/developer manual.
  Complete line-by-line HTML/PDF parity and final review of all printed pages
  remain release checks; current pairing, source self-containment, and targeted
  render checks do not establish full editorial parity.

### 2026-10-21 systemd retry-limit inspection

- Read-only inspection of Xenon found five active services whose deployed unit
  files put `StartLimitIntervalSec=60` and `StartLimitBurst=5` in `[Service]`:
  `sor-reconcile`, `sor-emit`, `sor-apply-pihole`, `sor-apply-dns`, and
  `notify`. `systemd-analyze verify` reports the keys as unknown in that section;
  `systemctl show` reports the effective interval as the 10-second default and
  burst as 5. The repository candidate units place these directives in `[Unit]`.
- Added `tests/test_systemd_unit_sections.py` to check all repository service
  units and wired it into CI. It passes across 43 service files. The live units
  were not reloaded or changed; install the reviewed candidate files during the
  eventual staged deployment and verify the effective limits afterward.

### 2026-10-21 Zima host clarification

- Jason confirms three physical devices: the Steel-labeled ZimaBlade on port 12
  of the `laboratory` switch, a ZimaBoard, and a ZimaBoard 2. Steel booted but
  exposed no useful service and remains unavailable over SSH; remote imaging is
  therefore not currently possible. Console access is the path to redeploy it.
- The Khadas VIM4 (`chlorine`, `10.7.0.10`) already runs the requested backup
  authoritative-only BIND instance and is verified against Xenon's 109 zones.
  The ZimaBlade remains reserved for a future tertiary role after redeployment.
  No confirmed hostnames/IPs exist yet for the ZimaBoard or ZimaBoard 2.
- No live network or device changes were made for this clarification.

### 2026-10-21 scanner browser acceptance

- Added an authenticated Chromium flow using temporary MariaDB SoR fixtures.
  It imports a scanner export with duplicate source location names, verifies
  row-ID mapping, proves Preview has no write side effect, applies using the
  preview fingerprint, and verifies the persisted asset/location in the SoR
  and refreshed Inventory view. The flow passed against local MariaDB 11.8.6 and is now
  wired into the dashboard CI job with pinned Playwright/Chromium.
- The flow revealed that the dashboard bootstrap called a documented combined
  `GET /api/inventory` endpoint that the PHP route did not implement. Added that
  read-only aggregate endpoint and normalized the camelCase API row fields for
  the Inventory screen. PHP lint, the route authorization audit (138 routes),
  JSX parse (16/16), scanner import UI test, and browser E2E pass.
- An independent source review caught that the pre-write assertions looked for
  snake_case `asset_tag` while the route emits `assetTag`; corrected the checks
  and reran the E2E. The combined read now returns all rows rather than silently
  truncating units at 5,000, and a successful import refreshes the Inventory
  component immediately without requiring a page reload.
- All test records were created and removed in the disposable local database.
  No production SoR or network system was used. Full CI execution remains
  unverified in this checkout.

### 2026-10-21 manual-pair and MCP verification

- Rechecked 14 operator/developer manual HTML/PDF pairs across SolariNet core,
  the status panel, RackWire, MCP, scanner import, and standalone scanner-ingest.
  Every paired PDF has extractable text and page metadata. The HTML files have
  no linked stylesheet, script, image, iframe, or source resources; visual
  review and line-by-line editorial parity remain separate release checks.
- Reinstalled MCP's declared requirements into an isolated temporary virtual
  environment. The full suite passes 86/86, the tool-backed evaluation passes
  10/10, and `python -m solarinet_mcp --check` lists all 26 tools without
  requiring configured API or database connections.
- Updated the baseline's paired HTML/PDF to reflect these current MCP results.
- Built the actual candidate runtime bundle from the current server and scan
  worker binaries, dashboard API/SPA, and the pinned RackWire submodule. The
  explicitly dirty preflight archive contains 179 hashed files; the temporary
  root installer validated it and activated it under `/tmp`, including the
  embedded RackWire entrypoint and scan worker. The manifest correctly records
  `sourceTreeDirty: true`. This proves package/install behavior for the real
  candidate contents; it is not a production release and was not deployed.

### 2026-10-21 concurrent lease election verification

- Extended `tests/integration/test_server_db_live.c` to run eight synchronized
  expiry races between independent MariaDB handles. Each race starts both
  claimants only after both connections are ready, then asserts exactly one
  winner, the persisted holder matches that winner, and the fencing epoch
  advances once.
- The target compiled cleanly, and the complete live-DB suite passed 14/14 on a
  disposable MariaDB 11.8.6 container bound to loopback port 33106. The scratch
  database and container were removed after the run. The production schema and
  live server lease were not used.
- Added `tests/integration/test_server_lease_processes.py`, which starts two real
  server binaries against one scratch schema and three shared loopback listeners.
  Server A held epoch 1 while B stayed standby; after A's SIGTERM shutdown, B
  acquired epoch 2 and rebound all listeners. The test passed locally and is
  wired into the full-server CI job with its own migrated disposable schema. It
  also passed against the ASan/UBSan `build-server` binary.
- SQL-level races and process-level takeover are now exercised. Deployed-pair
  failover under host/network timing, independent cross-lab review, and rollback
  rehearsal remain open release gates.

### 2026-10-21 scan-worker process verification

- Added `tests/integration/test_scan_worker_process.py` and enabled the optional
  discovery-tool code path in the full-server CI build. The test supplies a
  controlled fake nmap executable and a ready loopback listener, so it needs no
  external scanner or network target.
- Against a fresh disposable MariaDB schema, the ASan/UBSan scan worker passed
  SIGTERM cancellation, process-group cleanup, lease-loss cancellation, stale
  result rejection, standby refusal, successor orphan recovery, and a successful
  successor scan. The test cleared all scan, discovery, and lease rows; the
  database container was removed and port 33106 is no longer listening.
- CI now creates a separate migrated `solarinet_scan_worker_ci` database and
  runs this integration test. Actual systemd-managed stop timing, deployed
  enrichment binaries, lease timing between real hosts, and DB lock contention
  during a simultaneous result commit remain release gates.

### 2026-10-21 authenticated freshness browser acceptance

- Added `tests/browser/test_freshness_sessions_e2e.cjs`, which serves the actual
  dashboard SPA from a disposable loopback PHP server and establishes two
  independent operator sessions through the production `Auth` session code.
  Fixture API responses isolate the browser state machine from production data.
- In Chromium, one session loses the core nodes API and shows the stale banner,
  blocks a survey write before it reaches the server, then returns to live after
  recovery. The second session stays live and successfully sends its fixture
  survey request while the first is stale. The test also checks distinct PHP
  session IDs and absence of browser JavaScript errors.
- The browser test passes locally with Playwright 1.64.1 and cached Chromium and
  is wired into the existing dashboard CI job, reusing its pinned Playwright and
  Chromium setup. This closes the authenticated multi-session
  browser acceptance gate for F-13; production outage behavior and the remaining
  Phase-1 auth/session checks still require staged deployment verification.

### 2026-10-21 SoR-to-DNS delivery chain

- Added a guarded integration test that requires `SOLARI_TEST_SOR_DB=1`, a
  loopback `solarinet_*` database, and a loopback RabbitMQ URL. The CI SoR sync
  job now provisions both disposable services, applies the ledgered SoR schema,
  and runs the complete delivery suite against them.
- The test writes a zone row, records the outbox high-water mark, then inserts
  an entity and primary address so the real CDC triggers create DNS-relevant
  events. The real publisher confirms mandatory routing and persists its
  checkpoint. A RabbitMQ consumer runs the production SoR renderer and DNS
  applier state logic; the test verifies each delivery is acknowledged only
  after the generated zone contains the changed host and the applied target
  proof is durably recorded. The test runs the production release engine against
  an isolated loopback BIND process and queries the generated A record at the
  instant of each broker acknowledgment. The test invokes the actual release CLI
  through `sudo -n` with a root-owned temporary config/release tree; the private
  named process runs as the `bind` service account. Nameserver fixture records
  satisfy BIND's in-bailiwick glue checks.
- A restrictive-umask activation test found that `zones/` could remain mode
  `0700`, blocking the DNS service account from reading a root-owned generation.
  The helper now explicitly sets it to `0755`, independent of the caller's umask.
- The release helper now accepts NS data as a serving proof for zones with no
  A/PTR records and retains A/PTR preference when those records exist. Its
  isolated private-BIND suite passes 17/17, including an NS-only activation.
- The complete SoR sync suite passed 34/34 against isolated MariaDB 11 and
  RabbitMQ 4 containers. The integration test cleaned fixture entities,
  addresses, zones, sources, and outbox rows. A later cleanup check found the
  two exact test fixture containers still running on Xenon; they have now been
  stopped, and their mapped ports are no longer listening. The project RabbitMQ
  container on benzene was not changed. The integration test now closes its
  consumer connection while the real release helper is at the BIND reload gate,
  then verifies the DNS answer, no acknowledgement on the lost connection,
  RabbitMQ requeue with `redelivered=true`, and successful apply/serving proof
  before the next consumer's acknowledgement. **UNVERIFIED:** RabbitMQ service
  outage and restart, systemd-managed automatic process restart, multi-target
  recovery, and production cutover remain release gates.

- The chain test now starts a separate consumer process for the redelivery,
  waits until it has applied the real BIND generation and is blocked at the
  pre-ack boundary, then sends SIGKILL. RabbitMQ requeues both outstanding
  deliveries with the redelivered flag. A fresh consumer process reapplies the
  generation, verifies its served DNS answer and persisted target proof before
  each ack, and drains the queue. This proves the apply/ack crash window with a
  real process boundary; it does not validate systemd restart behavior.
- The full SoR sync suite passed 34/34 after this addition, using disposable
  loopback MariaDB 11 and RabbitMQ 4 containers. Both test containers were
  stopped afterward. **UNVERIFIED:** RabbitMQ service outage/restart,
  systemd-managed restart, multi-target recovery, cross-lab review, and
  production cutover remain open.

### 2026-10-21 live DNS and Steel reachability recheck

- Direct queries to chlorine (`10.7.0.10`) returned the current `akoria.net`
  SOA serial `2026102104`, the NS set (`xenon`, `chlorine`, `radium`), and the
  dashboard answer. SSH confirmed `solari-bind9` active, `named-checkconf`
  passing, and BIND listening on TCP/UDP port 53 at `10.7.0.10`. The Khadas
  already provides the requested backup authoritative DNS instance; no second
  service was installed.
- Steel (`10.0.0.11`) remains unreachable by SSH (`No route to host`). A
  read-only SNMPv2c query to `laboratory` (`10.0.0.3`) received no response, so
  port 12's link/MAC state remains **UNVERIFIED**; this does not contradict
  Jason's physical port identification. Network imaging is not available while
  Steel has no reachable address; console redeployment remains the practical
  recovery path. ZimaBoard and ZimaBoard 2 remain unmapped.

### 2026-10-21 isolated RabbitMQ application restart

- Added a guarded broker recovery test. It requires both an explicit loopback
  AMQP URL and an explicit Docker container ID, and verifies the selected image
  is RabbitMQ before control commands are allowed. The DNS consumer queue is
  configurable for the test; the production default remains
  `sor.apply.dns`.
- Against the disposable RabbitMQ 4 fixture, the test stopped and restarted
  RabbitMQ's application, then stopped and restarted the whole broker container
  while the consumer process remained alive. After each restart it verified the
  daemon stayed up, reconciled, and registered as a consumer. It then published
  a confirmed event, verified consumer processing/acknowledgment, and confirmed
  the fixture queue was empty. The test passed in isolation and in the full SoR
  sync suite. The full suite passed 35/35 against scratch MariaDB 11 and
  RabbitMQ 4; both local containers were stopped afterward. Benzene's production
  RabbitMQ container was not touched.
- Renderer failures now include a bounded diagnostic excerpt with the configured
  SoR password redacted; this exposed a missing local PyYAML installation during
  the first test attempt. The pinned SoR sync requirements were installed in
  the scratch test environment before the passing run.
- **Still unverified:** host reboot, systemd-managed consumer restart,
  multi-target recovery, cross-lab review, and production cutover. The container
  restart test does not claim those gates.

### 2026-10-21 F-01 transport API feasibility

- Checked the pinned NNG 1.8 API and the stable 1.12 API. Both expose peer
  verification and peer CN/SAN information, but not the raw peer certificate
  required to compare the leaf DER fingerprint stored by enrollment. The
  [NNG 1.12 public header](https://github.com/nanomsg/nng/blob/v1.12.0/include/nng/nng.h)
  still documents `NNG_OPT_TLS_PEER_CN`; `nng_pipe_peer_cert` is absent.
- NNG's peer-certificate object API is on the 2.0 development line. Upstream's
  [2.0 alpha 7 release](https://github.com/nanomsg/nng/releases/tag/v2.0.0-alpha.7)
  calls out the new TLS certificate API, while the
  [migration guide](https://github.com/nanomsg/nng/blob/main/docs/ref/migrate/nng1.md)
  lists breaking TLS-configuration and API changes. Upstream describes its main
  branch as development work not yet suitable for production.
- **Recommendation:** keep the stable 1.x transport for this release and do not
  claim live leaf-fingerprint enforcement. Keep F-01 open. Once NNG 2.0 is a
  stable upstream release, evaluate the certificate-object API in an isolated
  branch, port all supported targets and TLS tests, then require cross-lab
  security review before adoption. If fingerprint enforcement must precede that
  release, evaluate a supported TLS verification callback or a transport-boundary
  change separately; do not vendor an undocumented NNG internal or ship an alpha
  solely to close the checklist item.
- **UNVERIFIED:** no new transport dependency was built or adopted in this
  feasibility pass; Claude Fable cross-lab adjudication was unavailable in this
  session because its CLI session is unauthenticated.


### 2026-10-21 scanner category mapping in dashboard intake

- The dashboard importer already accepted scanner-category → SoR hardware-type
  mappings in its API contract, but the operator interface exposed only location
  and existing-model choices. The inventory import panel now provides a collapsed
  optional category section. It explains the case-insensitive exact-type default,
  the `other` fallback, and that overrides apply only to newly created models.
- The focused UI test verifies the automatic recognized-type and unknown-to-`other`
  labels, then selects a category override and confirms the preview request sends it
  together with the selected duplicate-name location ID. The authenticated HTTP test
  now also applies an explicit category override and checks the resulting SoR model type. The scanner import
  guide and standalone app README now describe the choice. `node
  tests/dashboard/test_scanner_import_ui.js`, PHP syntax checks, and the RackWire UI
  regression test passed. The MariaDB-backed PHP importer test was not run in this
  pass because its explicit disposable-database guard was not configured.
- **UNVERIFIED:** this candidate UI has not been deployed or exercised in an
  authenticated production browser; source-category choices have not been applied
  to the live SoR.

### 2026-10-21 scanner dashboard SoR integration verification

- Against a fresh loopback-only MariaDB 11 database with all current SoR
  migrations applied, `tests/dashboard/test_scanner_import.php` passed its
  preview/apply, idempotent replay, collision, and no-overwrite checks. The
  authenticated HTTP test also passed anonymous and viewer rejection, CSRF,
  required fingerprint, preview, category-to-model-type mapping, apply, stale-plan
  conflict, and replay checks. This closes the earlier candidate-only test gap for
  the dashboard integration seam; production behavior was not exercised.
- The scratch database container was stopped after the tests. **UNVERIFIED:**
  production SoR writes, authenticated production-browser operation, and physical
  source-to-destination location decisions remain open.

- Follow-up browser acceptance attempt: the local Playwright package was present,
  but its Chromium executable was absent. `playwright install chromium` could not
  resolve `cdn.playwright.dev` in this environment, so the browser-level
  scanner/RackWire flow did not run in this pass. This is an environment/setup
  blocker; the earlier browser acceptance result remains separate evidence and
  does not verify the latest category control in a browser.


### 2026-10-21 manual artifact parity check

- Checked the current named operator/developer manuals and operational guides in
  core docs, the panel app, MCP, RackWire, and scanner-ingest. Each has a paired
  HTML and PDF artifact. All checked PDFs are US Letter; the HTML files contain
  no linked stylesheet, script, or image assets.
- The scanner checkout has duplicate copies of the SolariNet architecture plan
  and interface guide without local PDFs; canonical copies and paired PDFs live
  under the SolariNet docs tree. Those copies are references, not scanner manuals.
- **UNVERIFIED:** documentation freshness across every deployable module and
  parity with deployed behavior; this packaging audit does not review every README
  or operator procedure.

### 2026-10-21 standalone scanner full-suite verification

- Re-ran the complete scanner-ingest Python suite with its pinned test virtualenv,
  optional ZXing dependency, and a disposable MariaDB 11 instance bound only to
  loopback. All 62 tests passed, including SoR preview/apply, read-only database
  account, rollback, idempotency, printer modules, product lookup, and Y486AP setup
  paths. The test database was stopped afterward and port 33320 was confirmed
  closed. `git diff --check` passed.
- **UNVERIFIED:** the suite does not establish scanner-ingest Git remote/release
  ownership, physical location mappings, production import behavior, or deployed
  printer access; the scanner checkout remains uncommitted and has no configured
  remote.

### 2026-10-21 Benzene RabbitMQ production health and upgrade gate

- Read-only checks on Benzene found the broker in rootful Podman as
  `rabbitmq:3-management`, with an `unless-stopped` restart policy and persistent
  `/var/lib/rabbitmq` volume. RabbitMQ 3.13.7 reports fully booted, no alarms,
  about 294 GB free disk, and six active client connections. The `solari` vhost
  has the four expected application queues; each is a durable, non-auto-delete
  classic queue with zero ready/unacknowledged messages and one consumer. No
  policies or queue arguments are configured. All four queues are currently
  healthy at the broker boundary. The first empty query used the default `/`
  vhost; the application queues live under `solari`.
- Host systemd has no RabbitMQ-specific unit. `podman-restart.service` is active
  as a oneshot and runs `podman start --all --filter
  should-start-on-boot=true`; this is the host boot integration for the container.
  A host reboot and subsequent consumer recovery were not tested.
- The current container tag is unpinned. RabbitMQ's [official release
  information](https://www.rabbitmq.com/release-information) says 3.13 community
  support ended 2024-09-30. Its 4.3 series is
  at 4.3.6; 3.13.x cannot be upgraded directly to 4.3.x and must first move to
  4.2.x, then 4.3.x, per the [4.3 upgrade guide](https://www.rabbitmq.com/docs/4.3/upgrade).
  The CI-pinned `rabbitmq:4` image reports 4.4.0 when run locally, and the
  [support timeline](https://www.rabbitmq.com/contact?tab=support-timelines)
  lists the 4.4 series beginning in October 2026. Its compatibility suite passes,
  but the exact supported in-place path from the live 3.13.7 broker through 4.4.0
  was not established here. The broker reports `khepri_db` disabled, avoiding the
  documented 3.13 Khepri in-place-upgrade incompatibility.
- **Recommendation:** treat an upgrade to a pinned, supported RabbitMQ release as
  a release gate. Prepare a versioned backup and rollback/recovery rehearsal,
  determine whether the selected target is 4.3.6 or 4.4.0, validate every
  required intermediate release and its enabled feature flags, then test the
  deployed consumers across the planned maintenance window. No production broker
  state was changed.
- **UNVERIFIED:** a fresh end-to-end production message through all configured
  targets; verified restore of the RabbitMQ data volume or definitions; exact
  plugin/config compatibility on 4.2 and 4.3; the operational recovery plan; and
  whether a RabbitMQ commercial-support entitlement covers the existing 3.13
  series.

### 2026-10-21 RabbitMQ consumer compatibility matrix

- `.github/workflows/ci.yml` now runs the SoR-sync job against four OCI-digest
  pinned RabbitMQ images: production's 3.13.7, the available 4.2.9 intermediate,
  4.3.6, and the existing 4.4.0 forward-compatibility fixture. Local image
  diagnostics confirmed each version before testing.
- The full 37-test SoR-sync suite passed against each image using the production
  `solari` vhost and the same fresh loopback MariaDB 11 schema. Coverage includes
  RabbitMQ application/container restart recovery, delivery confirmation and
  redelivery, and the SoR CDC → real DNS renderer → isolated BIND release chain.
  All scratch containers were removed and their ports confirmed closed.
- **UNVERIFIED:** the GitHub Actions matrix has not run remotely because the
  candidate is not pushed; the exact supported in-place route from 3.13.7 to
  RabbitMQ 4.4.0, production-volume restore, and deployed plugin/config behavior
  remain open. Passing application tests at each version do not simulate a
  version-to-version data-directory upgrade.

### 2026-10-21 isolated RabbitMQ persistent-volume upgrade rehearsal

- Added `deploy/sorsync/test_broker_upgrade_path.py` and wired it into the
  SoR-sync CI job. The test creates its own volumes, fixture credentials, and a
  durable classic queue, then exercises the persistent-volume path
  3.13.7 → 4.2.9 → 4.3.7 → 4.4.0. It verifies message survival and publishing at
  each stage, a 4.4.0 container restart, and restoration of the stopped pre-4.4
  volume snapshot under 4.3.7. All containers and volumes are removed.
- The local rehearsal passed 1/1 in 62.6 seconds. The missing Docker Hub 4.3.7
  tag is supplied by a test-only image built from the official generic-unix
  4.3.7 release (`SHA-256 83692adadc69b171af7c8188305d5c33c98bd17effb1ab993e405c0e9d1ebaef`)
  over the pinned official 4.3.6 image. The artifact hash was checked against
  GitHub release metadata; the image started and reported 4.3.7. CI builds the
  same image from that digest-verified artifact.
- `khepri_db` began disabled on 3.13.7 and was enabled by `enable_feature_flag
  all` after 4.2.9, as required before the 4.3 hop. This makes the metadata
  store transition part of the upgrade; the [official Khepri guide] describes
  the flag as the opt-in to the Khepri metadata store.
- **UNVERIFIED:** hosted CI execution, backup/restore of Benzene's actual volume,
  and deployed plugins/configuration. The local snapshot rehearsal used
  synthetic queue data and does not authorize or perform a production upgrade.

[official Khepri guide]: https://www.rabbitmq.com/docs/4.3/metadata-store/how-to-enable-khepri

### 2026-10-21 RabbitMQ 4.4 release gate refresh

- RabbitMQ published 4.4.0 on 2026-10-19 and 4.3.7 on 2026-10-20. The 4.4.0
  release notes identify a new classic-queue shared message-store file format;
  once those files exist, the node cannot be downgraded to 4.3 or earlier.
  They also recommend first moving through 4.3.7 or later when RabbitMQ and
  Erlang 28+ are upgraded together. See the [4.4.0 release notes].
- The 4.3.7 Docker Hub tag is still absent, so CI builds the official generic
  binary into the pinned 4.3.6 image for the intermediate stage. The full
  3.13.7 → 4.2.9 → 4.3.7 → 4.4.0 upgrade and pre-4.4 snapshot restore now pass
  locally. This provides a candidate target path for the current 4.4.0 release.
- **Recommendation:** keep Benzene on the healthy 3.13.7 broker until the
  actual off-host production volume backup and restore are verified, deployed
  plugin/config compatibility is audited, and a maintenance window is ready.
  Rollback from a live 4.4 data directory is not supported; restore the verified
  pre-4.4 snapshot instead. No production broker change was made.
- **UNVERIFIED:** hosted CI execution, restore of Benzene's actual data volume,
  and deployed plugin/config behavior.

[4.4.0 release notes]: https://github.com/rabbitmq/rabbitmq-server/releases/tag/v4.4.0

### 2026-10-21 consolidated local verification replay

- Rebuilt the current full-server candidate and passed all 33 configured CTest
  entries against a fresh migrated MariaDB 11 scratch database. Real HTTPS/PHP/
  `solariCtl`/MariaDB/CA enrollment passed through approval, certificate pickup,
  acknowledgement, and post-ack token cleanup. Separate migrated databases
  verified two-process lease transfer and scan-worker cancellation, lease
  fencing, standby refusal, orphan recovery, and successor completion.
- The CI best-effort `test_net_loopback` also passed separately. A fresh Clang
  `-Werror` build and all nine protocol fuzz/core CTest entries passed. ShellCheck
  0.11.0 passed every repository `.sh` and `.bash` file with sourced-file
  analysis enabled.
- Replayed the authenticated browser gates with Chromium 156 and Playwright
  1.64.1. Standalone RackWire, two-session dashboard freshness isolation, and
  scanner import plus embedded RackWire autosave all passed. The scanner flow
  verifies duplicate-name location choice by row ID, preview without writes,
  fingerprint-bound apply, SoR readback, CSRF on the iframe save, and no browser
  JavaScript errors or external requests. This replay found two test calls that
  supplied positional parameters using the wrong Playwright `evaluate` shape;
  they now pass one object argument. The test source was corrected and rerun.
- The complete scanner-ingest suite passed 62/62 with optional ZXing and all 15
  MariaDB exporter tests enabled against the disposable SoR schema. The allowed
  test-only SQL account host was set to `%` because container TCP connections
  arrive from the bridge address; the database and broker fixtures were removed
  and their loopback ports confirmed closed.
- Additional local results: MCP 86/86; alertbridge 5/5; UniFi 3/3; notifications
  3/3; authbroker 6/6 plus MQTT approve/deny round trips; Opie 9/9; backups
  10/10; systemd section validation across 43 service files; isolated DNS
  generation/release/secondary/bare-zone/Steel tests pass, including live
  loopback AXFR/NOTIFY. RackWire's own backend passes 45/45 and all four vendored
  runtime checksums verify; its parent API/UI contracts pass 57/57 and 41/41.
  Dashboard release package/install/rollback passes 8/8; migration/schema tools
  pass 17/17. Dashboard PHP syntax, authorization, scanner, freshness, lifecycle,
  and panel suites pass. `git diff --check` passes in the integration, primary,
  and scanner worktrees.
- All database, broker, HTTP, and browser fixtures used loopback or isolated
  containers; the temporary MariaDB and Mosquitto containers were stopped and
  removed. No production database, RabbitMQ, DNS, or deployed service was
  changed.
- **UNVERIFIED:** hosted GitHub CI; independent cross-lab review; production
  migration and rollback; live deployment; scanner remote/release ownership;
  physical mapping of `Bin #5` and `Nook`; and printer behavior on the actual
  handheld hardware.

### 2026-10-21 latest Cesium backup restore check

- A read-only SSH check confirmed Cesium's backup timer enabled/active and
  `sor-2026-10-21.sql.gz` present (31,297 compressed bytes, 216,122 uncompressed).
  `gzip -t` passed. The archive streamed over SSH directly into a loopback-only,
  disposable MariaDB 11 container; no disk copy of the dump was kept.
- Restore completed. `mariadb-check` passed all 41 base tables; the restored
  database contains four views and 24 triggers. The container was removed and
  its port closed. No live database or backup file was changed.
- A new read-only Xenon check confirmed the documented gap persists: the
  `solari-backup@xenon.timer`, config, runner, mounted `/mnt/solari-backups`
  target, and matching archive are absent. The candidate backup configuration
  is not installed. Xenon's monitoring-database backup and restore remain a
  production migration blocker; deploying the backup path requires a verified
  off-host target and a separate authorized operational change.

### 2026-10-21 Xenon backup target recheck

- Read-only inspection reconfirmed that Xenon's existing `//nas-x.local/Shared`
  mount is mounted at `/nas/Shared` with directory mode `0755`. The proposed
  `SolariNetBackups` subdirectory is absent, as is the root-readable
  `/etc/cifs-nas-x.cred` file referenced by the candidate mount unit.
- The Xenon backup timer is not installed, its runner is absent, and
  `/mnt/solari-backups` is not mounted. The candidate cannot be enabled until
  the NAS directory and credentials are provisioned and the restricted mount
  is verified. No live files, mounts, or services were changed.

### 2026-10-21 broker and scanner follow-up

- Benzene's RabbitMQ runs in Podman, not as a systemd service. Read-only
  diagnostics report RabbitMQ 3.13.7 fully booted with no local alarms. On the
  `solari` vhost, `sor.apply.ad`, `sor.apply.pihole`, `sor.apply.dns`, and
  `notify.dispatch` each had one active consumer and zero ready or
  unacknowledged messages. Xenon's `sor-emit`, all three apply workers,
  `sor-reconcile`, `notify`, and `alertbridge` were active. No messages were
  published during this check. Broker upgrade/recovery remains a gate because
  the installed release is old.
- The standalone scanner exporter now includes a MariaDB role definition with
  SELECT on its six planning tables and INSERT on its six insert tables. Its
  integration test executes the role file, confirms apply and replay behavior,
  and verifies UPDATE/DELETE are denied. All 20 exporter tests passed on an
  isolated MariaDB 11.8.6 server, including all 15 database cases.
- A current live scanner preview planned two catalog models and two units;
  the unmatched source locations were retained in notes. The configured live
  synchronization account has broader privileges than the importer requires,
  so no account role was provisioned and no SoR data was written. Live apply
  remains unverified pending a dedicated scoped account and physical mapping
  of the two source locations.
- Hardened the IF-MIB parser/merge path to reject malformed numeric OIDs,
  invalid/overflowing counters and statuses, invalid indices, and interface
  names that would previously be truncated. Added focused malformed-input and
  bounds tests. Both `SOLARI_WITH_SNMP=OFF` and `ON` server builds compile and
  pass the focused topology and SNMP suites. `test_server_topo_snmp` drives
  the actual LLDP collection path using a fake CLI, verifies normalized output,
  and checks nonzero command failure handling. No live SolariNet network gear
  was queried; device acceptance remains open.

### 2026-10-21 bounded SoR inventory reads

- Added deterministic `limit`/`offset` paging for models, components, receipts,
  projects, allocations, and flat locations. The nested locations endpoint is
  unchanged. Legacy collection response arrays remain available when paging is
  omitted; the combined `/api/inventory` response retains its object shape up
  to 500 rows per collection and returns HTTP 413 with `inventory_page_required`
  above that boundary.
- Changed the dashboard adapter to fetch each collection in 1000-row pages,
  validate totals and page boundaries, detect duplicate IDs/page-total changes,
  and keep a short shared snapshot for bootstrap and screen mount. Scanner
  import forces a fresh snapshot so it cannot reuse the pre-import cache.
  Collections above 100,000 rows fail visibly instead of being truncated.
  Components, receipts, and projects render 100 entries per page; the location
  tree expands deeper branches on demand. Inventory and barcode-reference
  screens report refresh errors instead of silently falling back to unbounded
  list calls.
- Added route integration checks for pagination metadata, page boundaries,
  flat locations, invalid limits, the 500-row aggregate error, and scanner
  import against an isolated MariaDB 11.8.6 container. The browser adapter
  test traverses a 1001-row two-page fixture and rejects over-limit and
  changing-total snapshots, verifies cache reuse/invalidation, and confirms
  post-import refresh. The UI test exercises 100-row paging for components,
  receipts, and projects. The authenticated browser E2E passed through scanner
  import and RackWire iframe autosave/readback.
- Verified `test_api_freshness.js`, `test_scanner_import_ui.js`, JSX parsing
  (16/16), `test_scanner_import_http.php`, browser E2E, PHP syntax, PDF text
  extraction, and `git diff --check`. Added the self-contained paired
  `SolariNet_Inventory_API.html` / `.pdf` manual. All database and browser
  fixtures were disposable and loopback-only; no production SoR or dashboard
  was changed. No commit, push, release, or deployment was performed.
- Remaining limitation: page reads are separate database requests, so same-
  count field updates may not be detected. The browser holds the complete
  loaded collections in memory for existing cross-reference workflows.

### 2026-10-21 F-01 durable revocation command, first slice

- Added migration 029 for an operator-attributed certificate revocation ledger
  and expiring one-time challenge. `REVOKE_CERT` binds the request to the
  current node ID, persisted issuing-CA DER fingerprint, leaf serial and DER
  fingerprint, reason, and operator. Migration 030 stores the issuing CA
  fingerprint alongside each new leaf, so a later configured-CA rotation does
  not relabel that certificate. Confirmation atomically consumes the
  challenge, records the revocation, and retires the node. The same consumed
  token is safe to retry. The operator CLI has a two-call confirmation flow.
- Confirming the ledger entry immediately causes the application identity gate
  to reject the retired node. The command reports `tls_effective=0`: no CRL is
  published or activated, and the TLS handshake still does not consult the
  ledger. This slice handles only a node's current leaf; issuance history,
  historical-certificate backfill, CRL generation, publication, freshness, and
  fleet activation remain open.
- Verified full server build; all 35 CTest cases; operator ACL and local socket
  CLI framing; and `test_server_db_live` against a disposable MariaDB 11
  database migrated through 030. The signer test confirms the issuing-CA
  fingerprint returned during signing matches the configured CA's DER
  fingerprint. The live DB test covers stored issuer matching and rejection of
  re-enrollment after revocation. The schema snapshot was regenerated from all
  31 migrations. Cross-lab review, hosted CI, production migration, and
  deployment remain **UNVERIFIED**; no production state was changed.

### 2026-10-21 scanner-ingest live-export changeover rehearsal

- Jason launched the local scanner-ingest app for testing. Read-only checks of
  its UI, state, printer, and JSON-export routes returned HTTP 200. Its state
  endpoint reports schema version 1 with two catalog items and two assets.
- Read the app's actual JSON export and ran it through the dashboard
  `ScannerImport` planner against a fresh disposable MariaDB 11.8.6 SoR fixture.
  An explicit mapping for the two source location labels was created only in
  that scratch database. Preview proposed two models, two units, four external
  references, and two barcodes while leaving inventory counts unchanged. A
  fingerprint-bound apply created those rows; a fresh preview and identical
  replay created no duplicates, and both chosen scratch location IDs read back.
  The scratch container was stopped and its port confirmed closed. The live SoR
  and scanner database were not written.
- A bounded same-lab review found no confirmed importer defect, but identified
  missing regression cases for anonymous, viewer, and missing-CSRF requests on
  the mutating apply route. Added those cases to
  `tests/dashboard/test_scanner_import_http.php`; the full authenticated HTTP
  suite passed against a separate disposable SoR, including bounded 5,000-asset
  preview, category mapping, stale fingerprint rejection, apply, and replay.
- Updated the scanner import and status manuals in monolithic HTML and paired
  PDF. Extracted text from both PDFs to confirm the new evidence is present.
  Also corrected stale status counts: the candidate lists 26 MCP tools; current
  structural checks cover 43 service files and 49 total systemd unit files.
  `systemd-analyze verify` still exits nonzero for 14 absent install-path
  executables; those are not claimed as passing deployment checks.
- **UNVERIFIED:** the correct live location mapping for `Bin #5` and `Nook`, any
  live SoR import, deployed dashboard/PHP-FPM acceptance, hosted GitHub CI,
  independent cross-lab review, and Forgejo/GitHub release synchronization.
  No commit, push, release, production migration, or deployment was performed.

### 2026-10-21 scanner subproject release ownership check

- The local `~/Code/scanner-ingest` worktree is on `main` with no commits and no
  configured remotes. `gh repo view soren42/scanner-ingest` reports no GitHub
  repository, the account repository list contains no scanner-ingest project,
  and `git ls-remote` for Forgejo `jason/scanner-ingest` reports no repository.
- **UNVERIFIED:** whether scanner-ingest should become a separate Forgejo/GitHub
  project or remain a standalone directory released from the SolariNet
  repository. No repository was created and no history was initialized.

### 2026-10-21 UniFi topology discovery and reconciliation gate

- Implemented `deploy/unifi/unifi_topology.py` as a GET-only topology discovery
  command. It follows pagination to `totalCount`, requires an explicit site when
  the controller has multiple sites, retrieves detail per network, and validates
  stable IDs, VLAN tags, IPv4 subnets, gateway addresses, and server DHCP ranges.
  Partial, malformed, empty, or internally inconsistent responses fail closed.
  It emits a versioned normalized snapshot with a UTC collection-completion
  timestamp; it has no SoR writer and makes no UniFi configuration calls. The
  multiple GET requests are not an atomic controller snapshot, so a future
  writer must bind preview and apply to current source state.
- Read the live controller using the existing local credential file without
  printing credentials. One site (`Default`) and one network (`Default`, VLAN 1)
  were returned. The detail endpoint reports gateway `10.0.0.1/8` and a DHCP
  server pool `10.1.0.26` through `10.7.161.117`. The canonical snapshot was
  emitted successfully. This does not establish consistency with the live SoR.
- The reported `/8` conflicts with the platform’s documented segmented network
  model and the SoR schema's explicit prefix/IPAM relationships. This is an
  observed source discrepancy; its cause is unknown. Do not reconcile or import
  it automatically. The next safe step is a read-only comparison against live
  SoR `networks` and `subnets`, followed by an operator decision on which source
  representation is authoritative for the default network.
- A same-lab read-only code review found gaps in pagination completeness, malformed
  site rows, DHCP gateway/broadcast validation, snapshot-internal overlap checks,
  TLS verification visibility, URL scheme validation, and UUID canonicalization.
  The collector now verifies pagination metadata and duplicate IDs, rejects bad
  site records, checks DHCP ranges against the gateway and reserved subnet
  addresses, reports overlaps within the fetched source, normalizes UUIDs,
  requires HTTPS, and requires a CA file or explicit `--allow-insecure-tls`.
  Cross-lab review remains unavailable and is still a release gate.
- DHCP relay configurations now fail closed with an explicit error because the
  source's relay-server fields are not validated or represented in the current
  snapshot contract.
- Added fixture tests for pagination, repeated/oversized/partial pages, malformed
  records, multi-site selection, URL/TLS policy, range validation, and overlap
  reporting. `python3 -m unittest -v deploy/unifi/test_unifi_topology.py
  deploy/unifi/test_unifipolld.py` passed 12 tests; Python compilation and scoped
  `git diff --check` passed. CI discovers all `deploy/unifi/test_*.py` tests.
- Re-ran the collector against the live controller with explicit insecure TLS
  opt-in; pagination metadata and current payload passed. The normalized
  snapshot still reports the single Default `/8` and no source-internal overlap.
- Queried live SoR `subnets` through the existing synchronization account in a
  read-only transaction. It returned 16 active `/24` prefixes across the named
  segments `core`, `production`, `IoT`, `peripherals`, `personal`, `workstations`,
  `netmgmt`, and `security`; every selected row has `vlan_id=NULL` and
  `gateway_ip=NULL`. Comparing the live source snapshot against those prefixes
  returns 16 overlaps and zero exact CIDR matches. No live SoR data was written.
- **INFERENCE:** the live SoR partitioning has no direct one-to-one CIDR match
  with UniFi's single `/8`; the UniFi DHCP pool spans several named SoR
  segments. The reason is unknown. The UniFi writer remains gated until the
  operator resolves source authority and whether the SoR VLAN/gateway NULLs are
  intentional planning data or incomplete imports.
- Updated the HTML status guide and regenerated its paired PDF. The automatic
  topology writer, live SoR comparison run, deployment, lease/reservation import,
  independent review, and release remain open. No live SoR write, UniFi
  configuration change, commit, push, or deployment was made.
- **UNVERIFIED:** whether the live `/8` is intentional, whether additional
  networks are exposed through another UniFi configuration surface, what
  authority the SoR's 16 subnets are meant to carry, and independent cross-lab
  review (no cross-lab reviewer is available in this harness).

### 2026-10-21 UniFi read-only SoR comparison

- Added optional `--compare-sor` to `deploy/unifi/unifi_topology.py`. It reads
  current VLAN, network, subnet, DHCP-range, and UniFi `external_refs` rows
  inside an explicit read-only transaction and attaches a structured
  `sor_comparison` report to the validated source snapshot. Network UUID
  external references are treated as exact identity; names and VLAN tags are
  candidates only. Prefix overlap, missing exact subnet, network/VLAN mismatch,
  gateway mismatch, DHCP overlap, and missing correlation are surfaced as
  review reasons. The command has no writer path.
- Rechecked the live UniFi API with GET only: it still returns one Default site
  network, VLAN 1, `10.0.0.0/8`, gateway `10.0.0.1`, and pool
  `10.1.0.26–10.7.161.117`, with no internal source-prefix overlap.
- Fifteen UniFi poller/topology tests pass, including a pure comparison fixture
  proving all 16 modeled `/24` prefixes and a DHCP range are reported as
  overlaps, repeated comparisons are deterministic, and a mocked DB reader
  issues only `SET TRANSACTION READ ONLY` and `SELECT` statements. The same
  reader also successfully queried all expected tables in a freshly migrated,
  disposable SoR MariaDB schema.
- Updated the single-file topology guide and paired Letter PDF.
- **UNVERIFIED:** a comparison against the live SoR has not been run in this
  step; no current SoR credentials are available in the candidate environment.
  The last recorded read-only SoR check found 16 `/24` prefixes with null VLAN
  and gateway references. No live SoR data or UniFi configuration was changed.
  The automatic writer, safe mapping, external-reference bootstrap, DHCP
  lease/reservation import, cross-lab review, and release remain open.

### 2026-10-21 scanner live-export and complete-suite recheck

- Rechecked Jason's launched scanner app at `127.0.0.1:8765`. The UI and JSON
  export routes responded with HTTP 200. The live export reports schema v1,
  two items, two assets, and location labels `Bin #5` and `Nook`.
- Ran all 63 scanner tests with a separately initialized socket-only MariaDB
  instance and the candidate SoR schema/migrations as read-only input. All 63
  passed, including all 15 database-backed exporter cases. The temporary server
  and datadir were removed after the run. An initial run without that disposable
  server skipped the DB cases and had a virtualenv path warning; the complete
  run used the correct local interpreter path and supersedes it.
- **UNVERIFIED:** browser-driven use of the live scanner, correct live SoR
  locations for the two source labels, live SoR apply, scanner repository
  ownership/remotes, independent cross-lab review, hosted CI, synchronized
  releases, and deployment. No live SoR or scanner data changed; no commit,
  push, or deployment was performed.

### 2026-10-21 native core rebuild and test pass

- Rebuilt the current candidate tree with `cmake --build build-full -j2`; all
  registered native targets completed successfully, including the scan worker
  and the live-database test binary.
- `ctest --test-dir build-full --output-on-failure` passed **37/37** tests in
  12.48 seconds. This covers core protocol/crypto, client, monitor, server,
  discovery, enrollment, certificates/CRL, scanning, SNMP and CLI paths.
- **UNVERIFIED:** hardware-specific fleet behavior, deployed service behavior,
  cross-lab review, hosted CI, synchronized releases, and deployment. Tests ran
  locally; no live services or network configuration changed.


## 2026-10-21 — RackWire and live discovery integration candidate

**Observed:** scanner-ingest export preview/apply writes physical assets to the SolariNet inventory SoR and barcode registry. RackWire resolves those barcodes against the shared model catalog; a model needs a RackWire device profile with port geometry before it can be placed from the library. The dashboard's authenticated discovery API queues bounded scans for operator review/adoption into monitoring. RackWire's standalone discovery route is a distinct contract and conflicts with the dashboard API when embedded.

**Candidate changes:** the embedded RackWire discovery action routes to the dashboard discovery screen; RackWire provides links back to inventory and discovery plus a host-level barcode scan action; scanner import completion offers a planning handoff. The embedded barcode action uses the frame's origin-checked message bridge, so the hidden standalone toolbar is not needed. RackWire manuals clarify that the product covers network, power, and interconnect planning, with rack layout optional. A Permissions-Policy now explicitly allows same-origin camera access for RackWire barcode capture while denying unrelated device features. Added browser UI and security-header regression checks.

**Verification:** RackWire UI (45/45), scanner import UI, JSX parse (16/16), and Python dashboard security-header tests (3/3) passed; the new user/developer PDFs were generated and their titles and opening text were checked with `pdftotext`. Native CTest (37/37) and scanner-ingest suite (63/63, including 15 MariaDB tests) passed in this candidate in the prior checkpoint. The browser E2E was attempted but could not start because the checkout has no installed `playwright` module; it did not reach the database fixture.

**Limits / UNVERIFIED:** the browser E2E uses a stub discovery admission endpoint and TEST-NET CIDR; it verifies queue submission, not a live lab subnet scan. Separately, the disposable worker-process integration test exercises a real loopback scan with a controlled scanner fixture. The discovery worker produces reviewable monitoring candidates; operator adoption creates/updates the SoR host entity and address mirror, not a physical `hardware_units` record. Physical inventory enters through scanner-ingest import or the inventory editor. An operator-facing SoR model-profile editor is implemented in Inventory → Components. The browser E2E exercises profile save, barcode lookup, asset placement, plan persistence, and the dashboard scan admission path with CSRF. Cross-lab review has not occurred. Apache full-site syntax validation is blocked by an unrelated host vhost referencing a missing Let's Encrypt certificate. No service deployment, release, commit, or push occurred.


## 2026-10-21 — SoR model profile editor

**Observed:** `POST /api/rackwire/library/{modelId}/groups` already validates operator authorization and CSRF, and refuses to overwrite a model-group row owned by another source. `GET /api/rackwire/library` returns SoR models and any existing RackWire group definitions. No dashboard UI previously exposed these contracts, so scanner-imported models could not be profiled from the operator interface.

**Candidate change:** Inventory → Components now has a RackWire device profile editor. It loads the shared model library, displays configured/unconfigured status, and saves one or more connector groups with connector type, count, speed, per-port watts, PoE standard and budget, plus device kind, label, and typical draw. The existing backend ownership check remains authoritative. The scanner-to-RackWire browser E2E now selects the just-imported model, saves an eight-port profile, checks the CSRF header and saved request shape, then continues into RackWire and discovery. Updated the scanner import guide and both RackWire manuals; regenerated paired PDFs.

**Design intent:** an inventory operator is reconciling physical gear and needs to model connectors before opening a plan. The editor stays beside components and uses the dashboard's established compact dark controls and cyan action accent, with borders and grouped fields rather than an extra navigation surface. It keeps connector and power details explicit and includes a verification reminder because a guessed device profile can make a plan misleading.

**Verification:** source edits are syntax-checked and will be exercised through the existing authenticated browser fixture. Profile save is designed to persist via SoR with operator authorization and CSRF. No profile has been saved into the live SoR.

**Limits / UNVERIFIED:** the browser E2E runs against a disposable SoR and stubs the scan queue response; it does not verify a lab subnet or deployed UI. The scan-worker process integration test verifies a loopback scan with a controlled scanner fixture, and the SoR host mirror transaction suite checks adoption-side idempotency/rollback. Discovery adoption creates a network host entity, not a physical inventory unit. Cross-lab review has not occurred. No service deployment, release, commit, or push occurred.


## 2026-10-21 — Barcode lookup CSRF and full workflow coverage

**Observed:** RackWire's SoR barcode lookup was a direct `POST /api/inventory/codes/lookup` without the dashboard's required `X-Solari-CSRF` header. The route writes an audit scan entry and rejects the request with 403, so the scanner-to-RackWire path failed even when the unit and model existed. RackWire's plan and profile mutations already had a session-token helper, but the barcode call bypassed it.

**Candidate change:** barcode lookup now obtains the authenticated session token from `/api/auth/whoami`, sends it with the same-origin session cookie, and refreshes/retries once when the API reports a stale CSRF token. The browser acceptance path now covers scanner export → SoR import → RackWire model profile creation → RackWire library merge → barcode scan resolution → plan save retaining the asset tag and serial → bounded dashboard scan admission. The HTML manual describes the profile editor, discovery/monitoring distinction, and the authenticated barcode path.

**Verification:** RackWire route tests pass (58/58); the RackWire UI contract test passes (49 checks), including CSRF refresh/retry and the host-level barcode action. Scanner import and RackWire profile API passed over HTTP against a throwaway MariaDB datadir, including viewer and missing-CSRF rejection. Scanner import UI, JSX parsing (16/16), dashboard security-header tests (3/3), and `git diff --check` pass.

**Network scan and ingest boundary:** the browser fixture covers asynchronous status transitions and navigation recovery with controlled API responses. A separate process/API/SoR test covers the native worker and authenticated candidate adoption on loopback. The real-browser worker acceptance below joins these boundaries: operator admission → live worker status polling → candidate listing in the same Discovery screen. Network scope remains bounded and operator initiated; discovered hosts remain reviewable candidates until explicit adoption. Adoption mirrors a SoR host entity/address, not a physical hardware unit. Lab-subnet coverage and deployed UI/telemetry remain unverified.


## 2026-10-21 — Reverified realtime discovery worker lifecycle

**Verification:** reran `tests/integration/test_scan_worker_process.py` against the built `solariScanWorker` and C adoption fixture using a fresh temporary MariaDB 11.8.6 instance bound only to loopback. The monitoring and SoR schemas applied all 34 and 8 migrations. The native C TCP connect-scan probed a real loopback listener; only its optional nmap enrichment executable was replaced by the deterministic fixture. The actual discovered candidate then passed through authenticated PHP status/list/adoption routes, the C `ADOPT` bridge, monitoring asset/service creation, and the separate SoR host/address mirror. All 13 HTTP/persistence checks passed. Worker SIGTERM cancellation and child cleanup, lease-loss fencing, standby refusal, orphan recovery, and successor scan completion also passed. Both temporary databases and test rows were removed; configured lab databases were not used.

**Cross-store acceptance added:** the process test continues its actual `via='portscan'` row through the real PHP front controller over HTTP. It covers anonymous-session rejection, viewer write rejection, CSRF enforcement, durable scan-request admission/polling, real worker status reads, authenticated candidate listing, and candidate adoption. The HTTP requests use the production `SolariCtl` socket client connected to a test-only C process built from the real `serverCtl` implementation and the actual monitoring database; no monitor transport is attached. The real C `ADOPT` handler creates the monitored asset and service target and changes candidate status. The PHP route then invokes the real `Sor::upsertHost()` against a separately migrated scratch SoR. The test verifies dashboard provenance and one primary address, then removes its marked fixtures. CI builds this C fixture and runs the acceptance flow with distinct monitoring and SoR scratch databases.

**Verification:** the expanded scan-worker process test passed locally end to end: a native TCP connect-scan against a loopback listener produced a candidate; authenticated HTTP scan admission created a pending request in the real C control queue; the status route read the completed worker job; the discovery API returned the candidate; and real C adoption wrote the monitoring asset, TCP service target, and adopted status before the SoR mirror created one host/address. All 13 HTTP and persistence checks passed alongside worker cancellation, lease fencing, standby, and recovery checks. A post-run query confirmed zero candidate rows, scan jobs, assets, probe targets, control requests, or active SoR loopback addresses remained. `git diff --check` passed.

**Limits / UNVERIFIED at this checkpoint:** discovery on a lab subnet and delivery of the new target to a live monitor remain unverified. The earlier browser suite simulated status progression, and the process/API/SoR acceptance did not drive the worker from a browser. The test queues a second scan admission but leaves it pending; it does not execute that queued request. The later browser-to-worker acceptance below closes the actual-worker browser gap for loopback only. No deployment, commit, or push occurred.


## 2026-10-21 — Browser to native realtime scan and candidate ingest

**Observed and fixed:** the Discovery screen kept its own candidate list. Completing a scan refreshed shared dashboard state but did not reload that list, so a newly ingested host remained invisible until navigation or reload. A completion-specific effect now fetches all candidates after the worker reports completion. This avoids an effect-cleanup race during the status transition.

**Acceptance:** `tests/browser/test_scan_worker_browser_e2e.cjs` starts in the authenticated RackWire screen and follows its **Network discovery** handoff into the Discovery UI, then exercises the actual PHP routes, a real C control queue, and native `solariScanWorker`. The only scan target is `127.0.0.1/32` with a temporary TCP listener; optional discovery tools are controlled fixtures. The browser observes pending/running/completed status, navigation away and return, and the candidate appearing in the same screen. Database assertions verify the completed job and one reviewable `via='portscan'` candidate. The CI server job runs this test against its disposable monitoring database and pinned Playwright/Chromium runtime.

**UNVERIFIED:** scanning an authorized lab subnet, deployed service behavior, live monitor delivery, scanner-ingest hardware import from a physical scanner, and independent cross-lab review. This acceptance does not adopt the candidate or create a physical inventory unit. No deployment, commit, or push occurred.

### 2026-10-21 local browser acceptance replay

- Rebuilt the `solariScanWorker` and `test_ctl_adopt_bridge` targets from the
  current `build-server` configuration, then ran
  `tests/browser/test_scan_worker_browser_e2e.cjs` against a fresh temporary
  MariaDB instance with all 34 monitoring migrations applied. The browser
  started from RackWire, followed its embedded Network discovery action, and
  exercised authenticated scan admission, the C control queue, the native
  loopback TCP probe, live status polling, navigation away and back, and the
  reviewable candidate appearing in Discovery. The test passed.
- The database, worker, bridge, PHP server, and temporary files were stopped or
  removed at test completion. The scan was restricted to `127.0.0.1/32` and a
  temporary listener. Playwright used the already-cached Chromium headless
  shell because this environment could not resolve the browser download host;
  the browser test itself ran successfully.
- **UNVERIFIED:** authorized lab-subnet scan behavior, deployed UI and worker
  behavior, delivery of adopted targets to a live monitor, physical scanner
  hardware intake, and independent cross-lab review. This test does not adopt a
  discovered host or create a physical inventory unit. No live database,
  network configuration, deployment, commit, or push was changed.


## 2026-10-21 — Token-free decommission queue acceptance

**Observed:** the dashboard's previous two-request confirmation flow put the
one-time decommission token in `ctlRequest.result` and `argsWire`. Candidate
migration 033 removes legacy values and cancels pending/claimed decommission
requests for a safe transition. The route now checks the bridge's
`decommissionInline=1` capability and submits one token-free intent; the C queue
consumer issues and confirms the challenge in memory.

**Verification:** a new HTTP integration test applies all 34 monitoring
migrations to a fresh scratch database, starts the real C control socket with
its privileged queue consumer and a local PHP server, then submits through an
operator session with CSRF. It verified `awaiting_node`, one completed request
with `inlineConfirm=1`, no `confirm=` value in stored args/result/error, and a
consumed 64-character challenge hash. The fixture has no PUB transport, so no
directive was sent to a node. C queue unit tests, PHP syntax, and the route
authorization audit also pass. CI now runs this integration test after the
worker/API/SoR acceptance in the same disposable schema.

**UNVERIFIED:** migration 033 remains unapplied to Xenon; cross-lab review,
coordinated server/dashboard rollout, remote node teardown, and physical wipe
remain open. No production deployment, commit, or push occurred.

### 2026-10-21 historical certificate replacement handshake

- Extended the pinned NNG/mbedTLS gossip integration fixture to issue two
  certificates with the same monitor CN and different serials. The fixture
  revokes the historic leaf and verifies that it fails TLS while the current
  replacement connects and its matching logical node ID is accepted by gossip.
- `cmake --build build-server --target test_gossip -j2` succeeded, and
  `ctest --test-dir build-server -R '^test_gossip$' --output-on-failure` passed.
  The script also retains expired, malformed, and wrong-issuer CRL cases.
- **UNVERIFIED:** deployed leaf migration, CRL distribution and activation on
  clients/monitors, fleet acknowledgement/freshness reporting, production
  revocation, and independent cross-lab review. No live certificate, service,
  or database was changed; no deployment, commit, or push occurred.

### 2026-10-21 RabbitMQ live health and ownership recheck

- Read-only SSH inspection of Benzene found RabbitMQ running in the rootful
  Podman container `rabbitmq`, using the floating `rabbitmq:3-management` tag
  and `unless-stopped` restart policy. The host `rabbitmq-server` systemd unit
  is inactive because this deployment is container-managed. The container is
  running RabbitMQ 3.13.7 and has been up since 2026-08-21.
- `rabbitmq-diagnostics ping`, `check_running`, and `check_local_alarms` passed;
  the broker reported no resource alarms. Its AMQP, management, Prometheus, and
  clustering listeners are active. All four SoR/notification queues in vhost
  `solari` were running as classic queues, each with one consumer and zero
  ready or unacknowledged messages.
- **Assessment:** RabbitMQ was healthy at the time of inspection. Its 3.13
  series is outside community support and the container tag floats. The
  isolated 3.13.7 → 4.2.9 → 4.3.7 → 4.4.0 persistent-volume rehearsal exists,
  but production backup/restore, deployed plugin/configuration compatibility,
  and a maintenance-window cutover remain open. No broker or host state was
  changed.

## 2026-10-21 — scanner, RackWire, and realtime discovery browser recheck

- `tests/browser/test_scanner_import_e2e.cjs` passed against a newly migrated,
  disposable SoR: a scanner-ingest JSON snapshot was previewed and applied,
  its model received a RackWire connection profile, barcode lookup resolved
  the imported unit, and RackWire persisted the asset tag and serial in its
  plan. Its scan-admission UI section uses controlled status responses.
- Extended `tests/browser/test_scan_worker_browser_e2e.cjs` to begin in RackWire,
  wait for the embedded planner to load, and use **Network discovery** before
  submitting a scan. The browser then exercised the real PHP routes, C control
  queue, native worker, and candidate list against a separate fresh monitoring
  database. A loopback listener produced the reviewable candidate; the browser
  observed progress and navigation recovery. Both scratch databases, temporary
  accounts, services, and rows were removed.
- RackWire UI/API checks passed (49 and 58 assertions), `git diff --check`
  passed, and the stabilization baseline PDF was regenerated and checked as
  10-page US Letter output.
- **UNVERIFIED:** physical barcode-reader use, authorized lab-subnet scanning,
  live monitor delivery, deployed RackWire/API/worker behavior, and cross-lab
  review. No live SoR or monitoring DB, network configuration, deployment,
  commit, or push was changed.

## 2026-10-21 — RabbitMQ backup-clone upgrade rehearsal

- Rechecked the Benzene backup and Xenon copy: both are root-owned mode 0600,
  287744 bytes, with identical SHA-256. Imported the Benzene archive into a new
  rootful Podman volume and ran the clone with `--network none` and the original
  node name. The live broker was not stopped or contacted.
- Replayed the data directory through RabbitMQ 3.13.7 → 4.2.9 → 4.3.7, using
  pinned official 3.13.7 and 4.2.9 images plus the checksum-verified official
  4.3.7 generic release. Each stage reported a running node, no local alarms,
  and all four durable classic `solari` queues with zero ready/unacknowledged
  messages. The restored data contained no queued messages, so this rehearsal
  proves queue-definition and empty-state continuity, not message continuity.
- After cleanup, the live 3.13.7 container still reported no alarms and all
  four application queues had one consumer each with no backlog. The temporary
  container, image, and volume were removed. No production state changed.
- **Recommendation:** 4.3.7 is now data-rehearsed from the actual volume
  backup, and it remains on the documented path from 3.13 through 4.2. The
  current official upgrade guide documents 3.13 → 4.2 → 4.3. RabbitMQ 4.4.0
  was released on 19 October 2026, but its release notes say new `.sqs` queue
  files prevent downgrade to 4.3 or earlier. Keep the live node on 3.13.7 until
  the 4.3.7 rollout is independently reviewed and startup is pinned; defer
  4.4 until its supported upgrade and rollback path is established.
- **UNVERIFIED:** cross-lab review, pinned live startup, host-reboot recovery,
  real message traversal after a version change, and 4.4 downgrade/restore
  behavior on the actual volume. No live upgrade occurred.

Sources: [RabbitMQ upgrade guide](https://www.rabbitmq.com/docs/upgrade),
[RabbitMQ 4.4.0 release notes](https://github.com/rabbitmq/rabbitmq-server/releases/tag/v4.4.0),
[RabbitMQ support timeline](https://www.rabbitmq.com/contact?tab=support-timelines).

## 2026-10-21 — direct-route native scan smoke

- The workstation route table places gateway `10.0.0.1` on the directly
  connected `enp1s0` route. With the existing authorized network-discovery
  scope, ran a native worker scan limited to `10.0.0.1/32` against a fresh,
  fully migrated monitoring database. Optional nmap, Avahi, and SNMP enrichment
  executables were failure-only test fixtures; the TCP connect scan itself used
  the real worker and network stack.
- The job completed with `hostsDone=1`, `hostsTotal=1`, `hostsFound=1`; SQL
  confirmed its sole `via='portscan'` candidate was `10.0.0.1`. The worker,
  scratch database, and temporary database accounts were removed. The scan did
  not write or alter live monitoring DB, SoR, DNS, or UniFi state.
- **UNVERIFIED:** full subnet-scale scan duration/capacity, other VLANs,
  deployed worker/service behavior, and monitor delivery remain open. This
  one-host smoke verifies a real non-loopback route and candidate persistence;
  it is not evidence for scanning the interface's broad `/8` route.

## 2026-10-21 — native and dashboard full-suite recheck

- Rebuilt the complete `build-server` tree successfully. CTest then passed
  **37/37** cases against a newly migrated, disposable MariaDB monitoring
  schema, including the live server DB, scan-worker planning, gossip/CRL,
  enrollment, and loopback transport cases.
- Against a separate newly migrated disposable SoR, all **31** dashboard,
  RackWire, scanner, browser, and release test commands passed. This included
  RackWire's 58 API checks and 49 UI checks, scanner-import validation over
  unit/API/browser paths, the scanner → SoR → RackWire browser flow, and the
  dashboard release activation/rollback contract.
- PHP syntax passed for all 50 dashboard API files. `git diff --check` passed.
  Both scratch databases and temporary database accounts were removed.
- **UNVERIFIED:** hosted GitHub/Forgejo CI, independent cross-lab review,
  deployed browser/API behavior, and the remaining live rollout gates. No
  commit, push, release, or deployment occurred.

## 2026-10-21 — isolated infrastructure test recheck

- Local Python suites passed: systemd unit sections **1/1**; DNS generation
  **5/5**; DNS release transaction **17/17**; Khadas secondary and zone
  refresh **15 passed, 1 opt-in AXFR test skipped**; bare DNS generator **4/4**;
  Steel DNS config **1/1**; alert bridge **5/5**; UniFi collector and topology
  **15/15**; notification parsing **3/3**; Opie boundary/isolation **5/5**;
  backup runner **10/10**; and auth broker **6/6** in a disposable venv with
  its pinned dependencies. Opie’s optional MCP test was skipped because its
  MCP requirements were not installed.
- Docker access works through the authorized local sudo path. A first container
  attempt exposed an Erlang-cookie ownership issue when RabbitMQ initialized
  its data volume as root; rerunning the disposable broker as UID 999 fixed
  this without changing host configuration. A fresh RabbitMQ 3.13.7 and
  MariaDB 11 pair ran the SoR migrations and the complete sync test discovery:
  **38 passed, 1 dedicated upgrade-path test skipped**. This included actual
  broker application/container restart recovery and a temporary BIND chain
  test that confirmed SoR-triggered delivery through to an authoritative DNS
  response before acknowledging the message. Both containers and their
  disposable database/volumes were removed; ports 15672 and 13306 are no longer
  listening.
- The Khadas AXFR runtime test remains opt-in and was not run. **UNVERIFIED:**
  hosted CI, that opt-in test, independent cross-lab review, and deployed
  behavior. No live services or data were changed.

## 2026-10-21 — F-01 replacement-certificate gate recheck

- A previously returned gate-audit finding said the gossip fixture rejected a
  revoked leaf without proving a valid replacement for the same node remained
  accepted. The current worktree already includes both leaves: the shell fixture
  issues the same-CN replacement with a distinct key/serial, revokes only the
  predecessor, and the C integration test verifies the replacement reaches the
  peer registry. Targeted `ctest --test-dir build-server -R gossip` passed
  **1/1**. No production certificate policy changed.
- Updated the F-01 contract's status paragraph to reflect the replacement
  assertion already present in the candidate. This is local regression evidence
  only; fleet CRL distribution, activation/freshness policy, live deployment,
  and cross-lab security review remain open.

## 2026-10-21 — live UniFi-to-SoR comparison

- Ran the candidate read-only collector against the configured UniFi API and
  compared its complete one-network snapshot with the primary SoR using the
  deployed sync service's database identity. The dashboard collector account
  was correctly denied access to the separate SoR schema; no data was returned
  on that attempt. The sync account query then ran in the collector's
  `SET TRANSACTION READ ONLY` transaction.
- Observed one UniFi `Default` network at `10.0.0.0/8`, no internal source
  overlap, and 16 overlapping active SoR subnets. The comparison returned
  `review_required` for missing exact subnet, missing UniFi external reference,
  missing VLAN mapping, and broad-prefix overlap. It found no overlap with
  existing SoR DHCP range rows. Neither UniFi nor the SoR was changed.
- **UNVERIFIED:** the configured UniFi API connection did not supply a trusted
  CA file, so TLS certificate authenticity was not checked in this read. The
  topology authority conflict and any automatic import remain unresolved.

## 2026-10-21 — directly connected `/24` scan

- Rebuilt the default release configuration with optional discovery enrichment
  disabled. On the directly connected `10.0.0.0/8` interface route, the native
  worker scanned the bounded `10.0.0.0/24` against the real network while all
  job and candidate persistence went to a newly migrated scratch monitoring
  schema. It reached terminal `completed`: **254/254** host addresses processed,
  **13** `portscan` candidates persisted, and **10.32 seconds** elapsed.
- Removed the scratch DB, temporary DB account, worker, config, and lock. No live
  monitoring DB, SoR, DNS, or UniFi state was changed. This verifies a real
  directly connected `/24`, not other VLANs or subnet adoption.
- The earlier test build had optional enrichment enabled. It completed the
  254-host TCP sweep, then was canceled after about two minutes while serial
  nmap enrichment had persisted nine candidates. That incomplete pre-mitigation
  run is superseded by the completed measurement below.
- Follow-up code inspection found Nmap was repeating its broad default port
  scan after the native TCP sweep. Optional enrichment now probes only the
  already discovered open ports, uses light service probes, and limits OS
  detection to hosts with usable port evidence. A focused unit test verifies
  these arguments; the tool-enabled worker rebuild and `test_server_scan` pass.
- Re-ran the optimized worker against the directly connected `10.0.0.0/24` with
  optional Nmap, SNMP, and Avahi tools installed. The worker completed **254/254**
  hosts, persisted **12** candidates, and reported terminal `completed` in
  **136.34 seconds**. Jobs and candidates were stored only in the current
  generated monitoring schema on a disposable loopback-only MariaDB; the worker,
  DB, account, config, and lock were removed. No live monitoring DB, SoR, DNS,
  or UniFi data was changed. **UNVERIFIED:** other VLANs, deployed worker
  behavior, and browser polling while enrichment is running.
- Clarified Discovery's live progress message: it reports hosts checked and
  candidates saved, then explicitly says when the host sweep has ended but
  candidate finalization is still running. The scanner-import browser acceptance
  drives the sweep-in-progress, post-sweep/no-candidate-yet, candidate-saved,
  and terminal states against disposable MariaDB and passes.

## 2026-10-21 — RackWire-to-worker browser `/24`

- Extended `tests/browser/test_scan_worker_browser_e2e.cjs` with a guarded LAN
  mode. The default remains loopback. LAN mode requires the explicit
  `SOLARI_TEST_NETWORK_SCAN=1`, fixed `10.0.0.0/24`, local address
  `10.0.0.20`, and `ip route get 10.0.0.254` to prove a direct route through
  that address's interface. It binds a temporary listener on the local address
  and verifies the candidate appears in Discovery.
- The real browser flow passed against a fresh migrated scratch DB using the
  release build with optional enrichment off: RackWire handoff → PHP scan API
  with CSRF → C control queue → native worker → live progress polling → browser
  navigation recovery → candidate display. The worker completed **254/254**
  hosts and persisted **14** candidates. The scratch DB/account and temporary
  listener, worker, bridge, PHP server, and browser session were cleaned up.
- The loopback default passed again with the same worker. A one-host scan can
  transition to `completed` before the browser observes `running`; the
  acceptance now supports that fast path and still checks the disabled
  in-progress control when `running` is observed. The Discovery sidebar test
  selects the label independently of its numeric candidate badge.
- No live monitoring/SoR data was written. **UNVERIFIED:** other VLANs,
  enrichment-enabled scan completion, deployed worker behavior, and live
  monitor delivery.

## 2026-10-21 — fresh database-backed CI recheck and migration snapshot reconciliation

- Started a disposable MariaDB 11.8.6 container published only on loopback. The
  monitoring migrations applied **34/34** and the SoR migrations **8/8**; both
  clean `--check` runs found no pending migrations. The generated monitoring
  schema differed from `db/schema.sql` only because the snapshot manifest
  omitted migration 033. Regenerated the snapshot, updated the CI ledger-count
  assertion to 34, and updated the migration runbook and paired PDF. Both
  generated snapshots now compare byte-for-byte, clean snapshot installs
  reproduce the migrated table/view sets, and SoR triggers match.
- Rebuilt the tool-enabled server and ran database-backed CTest against the
  scratch monitoring DB: **37/37 passed**, including the live MariaDB suite.
  The first run exposed timing sensitivity in the replacement-certificate
  gossip test: it expected a TLS frame to arrive after a fixed 30 ms sleep.
  The test now polls the listener with a bounded 500 ms deadline. The full
  gossip test then passed ten consecutive runs, and the complete CTest suite
  passed again. This changes test synchronization; it does not change the
  runtime gossip protocol.
- Against the same disposable DBs, scan-worker process acceptance passed for
  SIGTERM cancellation, lease-loss result fencing, standby refusal, orphan
  recovery, and successor scanning. The authenticated HTTP/control-queue test
  passed **13/13**. The RackWire-to-native-worker browser acceptance passed on
  loopback with live status polling, navigation recovery, and candidate ingest.
- On a fresh SoR schema, transaction rollback/idempotency, scanner preview and
  apply, authenticated HTTP import, and scanner-to-RackWire browser acceptance
  all passed. The browser flow created a model profile, resolved an imported
  asset by barcode, persisted its asset tag and serial to the RackWire plan,
  then exercised scan admission and progress persistence. The full HTTPS
  enrollment flow through PHP, `solariCtl`, the server, MariaDB, and its
  temporary CA also passed. The isolated loopback BIND AXFR/NOTIFY refresh test
  passed with two temporary named instances.
- All scratch containers, schemas, test accounts, and BIND fixtures were
  removed. No live SoR, monitoring database, DNS service, or network device was
  changed. **UNVERIFIED:** hosted CI, an independent cross-lab review, a current
  ShellCheck run (the executable is absent in this environment), and deployed
  behavior outside the previously recorded authorized `/24` acceptance.

## 2026-10-21 — cross-version broker and subproject verification

- Installed the declared test dependencies into a temporary virtual
  environment and verified MCP **86/86**, UniFi **15/15**, authbroker **6/6**,
  Opie **9/9**, alertbridge **5/5**, and notify **3/3**. The system Python was
  not modified.
- The SoR-sync suite passed **37 runnable tests** against both isolated
  RabbitMQ **3.13.7** and **4.3.6**. This includes confirmed delivery,
  redelivery after apply failure, container restart recovery, and the complete
  CDC-to-rendered-DNS-to-ack path using disposable MariaDB and loopback BIND.
  Its separate broker-upgrade test remains an opt-in step and was skipped in
  these suite runs; the recorded persistent-volume upgrade rehearsal is
  separate evidence.
- The 3.13.7 run exposed a synchronization race in the test's injected-loss
  scenario: it could release the gated BIND reload before the consumer thread
  had observed the simulated RabbitMQ loss. The test now waits for that
  observation before releasing the gate. The DNS-chain integration test and
  full 38-test suite then passed on 3.13.7.
- Status-panel firmware tests passed, its TLS listener suite passed **16
  cases**, provisioning/wipe and dropped-ack recovery smoke passed, and the
  page/firmware parity suite passed **59/59**. RackWire standalone browser
  acceptance and dashboard two-session freshness browser acceptance also
  passed.
- All broker and SoR fixtures were isolated from Benzene and Cesium. **UNVERIFIED:**
  hosted CI, cross-lab review, ShellCheck in this environment, and any live
  broker, SoR, DNS, or deployment changes.

## 2026-10-21 — scan failover, release archive, and production SoR recheck

- Expanded the two-server lease process test to start the real scan worker during
  controlled slow enrichment. The primary renewed its lease through multiple
  intervals; after primary shutdown, the standby acquired epoch 2, the old
  worker stopped on lease loss without publishing a stale candidate, and the
  successor worker failed the orphaned job and completed a new scan. The test
  verified exactly one successor candidate, passed against fresh MariaDB
  11.8.6, and is wired into GitHub Actions. The listener and all writes were
  confined to loopback and the disposable monitoring schema.
- Added a malicious symlink-entry archive case to the dashboard release suite.
  The full temporary-root suite passes **9/9**, including bundle tampering,
  traversal, symlink rejection, immutable install, and rollback. The release
  contract now records those checks.
- Completed scanner-ingest's missing Interface Guide PDF from its own standalone
  HTML with Playwright Chromium. The 18-page US Letter PDF now passes the guide's
  browser checks for page overflow, density, and its 12 px type floor; tightening
  the Audit and Handoff spacing fixed both clipped areas without reducing text
  size. The cover, Audit, and final page were visually checked. The subproject's
  `VALIDATION.md` records the result.
- Rechecked published refs: SolariNet GitHub and Forgejo `main` both resolve to
  `5e8d688`; RackWire Forgejo `main` and the parent gitlink both resolve to
  `5f29f59`. The RackWire working tree remains dirty with documentation and
  entrypoint edits. Scanner-ingest remains a local repository without a
  configured remote.
- A read-only SoR query from Xenon, using the existing sync service connection,
  confirmed Cesium MariaDB **12.3.2**, **41** SoR base tables, **24** triggers,
  and no `solari_migration_ledger`. No credentials or application rows were
  printed. This matches the migration runbook's existing production-adoption
  blocker; no live database state changed.
- Corrected the stabilization baseline's worktree count and RackWire cleanliness
  description, regenerated its PDF, and recorded the SoR recheck in the
  migration runbook and paired PDF. Python compilation, workflow YAML parsing,
  and `git diff --check` pass. **UNVERIFIED:** hosted CI, independent cross-lab
  review, production schema adoption, service deployment, and synchronized
  releases. No commit, push, release, or deployment occurred.
- Ran the CI ShellCheck command in the pinned **0.11.0** image. All **35**
  repository `.sh` and `.bash` files pass with sourced-file analysis and warning
  severity; the temporary image was removed after the run.

## 2026-10-21 — UniFi insecure-TLS runtime warning

- The topology reader already rejected an untrusted controller unless the
  operator explicitly supplied `--allow-insecure-tls`. It now prints a
  standard-error warning that certificate verification is disabled and the API
  key is sent over the unverified connection. Verified CA-file mode emits no
  warning. The usage guide and paired Chromium PDF document this behavior.
- UniFi topology and poller tests pass **18/18**; `git diff --check` passes.
  **UNVERIFIED:** no trusted controller CA is available here, so live verified
  TLS was not exercised. No controller or SoR state was changed.

## 2026-10-21 — authorization and scan risk status reconciliation

- Re-ran the dashboard authorization ladder: policy tests, router guard wiring,
  nine live enforcement scenarios, and route-table audit all pass. The route
  audit covers **139** registered routes; all writes require operator/admin
  except the documented login, logout, and enrollment bootstrap paths. JSON
  body parsing also enforces `application/json`. The C bridge has an optional
  `SO_PEERCRED` ACL enabled by the hardened config, but it defaults off for
  compatibility; queued privileged work still trusts the PHP-asserted actor
  name. That distinction is now explicit in F-02's candidate status.
- Updated findings F-02, F-03, and F-05 to distinguish current candidate
  evidence from the historical August observations. F-02's PHP gate and F-03's
  CSRF/Origin/Content-Type checks pass locally; C-side authorization remains
  open. F-05's asynchronous scan path and lease safety are verified, while
  other long-running control operations and deployed stop behavior remain open.
  No code was deployed or committed.
- During the route review, reproduced an exact-match bug in the JSON media-type
  guard: substring matching accepted `application/jsonp`. Replaced it with a
  normalized media-type comparison and added 5 accepted/8 rejected cases to CI.
  The focused PHP regression test and PHP HTTP-SAPI test pass; the latter sends
  a real request through `php://input` and confirms lookalikes return HTTP 415.
  This also strengthens the F-03 candidate evidence above.

## 2026-10-21 — local dashboard CI lane

- Ran the dashboard checks that do not require a MariaDB fixture: PHP syntax,
  scanner/bridge inputs, JSON media types, session/OIDC lifecycle, full route
  authorization, RackWire API/UI, stream retirement, panel mappings, JSX,
  lifecycle UI, scanner-import UI, capability maturity, service-worker privacy,
  API freshness, and Apache security headers. All passed.
- The authenticated two-session freshness browser acceptance passed, including
  stale warning, stale-write rejection, unaffected peer session, and recovery.
  The dashboard release package/install/rollback suite passed **9/9**.
  Focused UI counts: RackWire **51**, lifecycle **92**, panel AW **164**; all
  **16/16** dashboard JSX files parsed.
- These are local candidate results, not hosted CI. The MariaDB migration and
  SoR-backed scanner/RackWire cases were not rerun in this lane. No live DB,
  DNS, controller, or deployment state was changed.

## 2026-10-21 — migration runner clean-install verification

- On Xenon's MariaDB **11.8.6**, created two unique empty scratch schemas with
  the required `utf8mb4/utf8mb4_unicode_ci` defaults. `tools/migrate.py`
  applied the full monitoring and SoR streams, a second apply was idempotent,
  and `tools/audit_schema.py` passed on both schemas. Both scratch schemas were
  dropped in cleanup; a follow-up metadata query found **0** remaining.
- The migration, schema-audit, schema-export, and certificate-backfill tool
  suites passed **21/21**. Regenerated the migration runbook PDF from its
  monolithic HTML and verified the new evidence text is present in the PDF.
  Existing Xenon and Cesium schemas were not targeted or modified.
- Against a separate migrated loopback SoR scratch schema and narrowly granted
  temporary test account, the SoR mirror transaction suite, scanner import
  planner, authenticated import HTTP suite, and Chromium scanner→SoR→RackWire→
  discovery acceptance all passed. The browser flow created a device profile,
  resolved its barcode to the SoR asset, saved a RackWire plan, admitted an
  asynchronous network scan, preserved progress across navigation, and showed
  its completed candidate in Discovery. Both the schema and test account were
  removed and absence verified. Updated the scanner import manual and PDF to
  distinguish this scratch browser acceptance from the still-unverified staged
  deployment/operator flow.
- Rechecked publication state: Forgejo and GitHub SolariNet `main` both resolve
  to `5e8d6889`; RackWire Forgejo `main` and the parent gitlink resolve to
  `5f29f590`. Neither remote has a release tag. The local SolariNet candidate
  is on `integrate/stability-20261020` with 323 worktree changes. The separate
  `scanner-ingest` checkout has no remotes. Fresh catalog checks found no
  same-name repository on GitHub or Forgejo; Forgejo SSH authentication
  succeeds, but the guessed `jason/scanner-ingest` path does not exist. Its
  canonical publication destination is therefore unresolved.
- The standalone RackWire README, user guide, developer guide, and paired PDFs
  all describe network, power, and interconnect planning, with rack layouts as
  an optional view. No stale rack-only product framing was found in those
  artifacts.

## 2026-10-21 — native server and lease-failover verification

- Rebuilt the current full server tree and ran its **37/37** CTest cases. Then
  configured and built the same server/test matrix with AddressSanitizer and
  UndefinedBehaviorSanitizer; its **37/37** cases passed as well. The CTest
  `test_server_db_live` target intentionally self-skipped in both aggregate
  runs because the opt-in flag was absent; it is reported separately below.
- Applied the complete monitoring migration stream to a fresh loopback scratch
  schema and ran the actual live-DB C test binary in both normal and sanitized
  builds: **15/15** database cases passed each time. The two-server process test
  also passed against disposable MariaDB in both builds: the primary renewed
  during slow scan enrichment, standby takeover advanced the epoch, the old
  worker stopped without stale writes, and the successor recovered the orphan
  and completed a scan.
- All scratch schemas and narrowly scoped temporary accounts were removed; the
  cleanup queries returned zero remaining schemas/accounts. No existing
  monitoring/SoR schema or production service was changed.

## 2026-10-21 — standalone guide PDF pairing

- Added PDF companions for the monolithic Directory Services & SSO, Fleet
  Provisioning, Dual Topology Views, and Monitoring Overhaul HTML guides.
  Generated all four directly from their HTML with WeasyPrint; each PDF has
  the matching document title, selectable text, and 4–5 A4 pages. `pdftotext`
  checks confirmed the opening and closing sections are present, and a rendered
  topology page was visually inspected for readable dark-theme contrast and
  table layout.
- These source guides retain their original build dates and point-in-time
  status claims; treat them as historical design/implementation records until
  their operational claims are refreshed against the current services.
- A fresh read-only publication check confirmed that the Forgejo SSH key is
  accepted, but no `scanner-ingest` repository appears in the Forgejo search
  catalog or the `soren42` GitHub repository list. Exact guessed paths return
  repository-not-found; no remote or repository was created.

## 2026-10-21 — release-status refresh and independent-review attempt

- Requeried both SolariNet remotes: each `main` is still 18 commits behind the
  candidate and zero commits ahead of it; neither remote has a tag. The current
  worktree has 177 tracked paths changed and 150 untracked paths. RackWire has
  eight changed paths; scanner-ingest has three and no configured remote.
  Updated the current-status card and paired PDF with those counts and the
  unresolved scanner-ingest publication path.
- Regenerated the status PDF from its monolithic HTML. Visual inspection found
  the long status card was forced onto page two, leaving page one mostly blank;
  changed the print break rule so the current status starts on page one. The
  new PDF is 11 pages, US Letter, with selectable status text. `git diff
  --check` passes.
- Prepared a secret-pattern-scanned, read-only diff packet for Claude Fable 5
  covering the auth, migration, inventory, scan-worker, and lease changes. The
  local Claude Code CLI rejected the review because its OAuth session had
  expired; no review was received. Cross-lab review remains an open release
  gate. No repository, service, or network state was changed.
- Confirmed the root CI has recursive RackWire checkout steps, while the new
  relative `.gitmodules` URL resolves GitHub's copy to `soren42/RackWire`; that
  repository is absent. The RackWire checkout has eight local changes, but the
  SolariNet gitlink still targets published commit `5f29f590`. Updated the
  current-status guide with these verified integration gates. Publication
  destinations are awaiting operator direction; no repository was created.

## 2026-10-21 — RackWire integration candidate committed locally

- Created local branch `integrate/solarinet-20261021` in the RackWire submodule
  and committed its eight integration, accessibility, scope, and manual changes
  as `a0f456a`. The SolariNet worktree's gitlink now points to that exact
  candidate. Neither change has been pushed; GitHub's `soren42/RackWire` target
  is still absent.
- Corrected the RackWire handoff and contract: SoR plan persistence, hardware
  library merge, CSRF-protected barcode resolution, and embedded Discovery
  navigation are integrated in SolariNet; local file mode retains the offline
  planner; a RackWire WebSocket endpoint and deployed behavior remain
  unverified. Updated the developer/user manuals, regenerated both PDFs, and
  added current test evidence to the handoff.
- Re-ran RackWire verification: UI **51/51**, API **58/58**, and standalone
  Chromium acceptance passed (offline boot, dialog focus, persistence, map/table
  navigation, and mobile toolbar). Both manual PDFs are US Letter with no blank
  pages; their opening pages were visually inspected. `git diff --check` passed
  in both repositories.
- **UNVERIFIED:** independent cross-lab review remains unavailable because the
  Claude Code OAuth session expired; hosted CI, deployed SoR/API behavior, and
  publication of the RackWire commit remain open.
- Committed scanner-ingest's focused documentation update locally on
  `integrate/solarinet-20261021` as `ebb9a58`. The worktree is clean; its 18-page
  US Letter interface guide PDF pairs with the HTML, and the subproject's
  validation record contains the 63/63 suite result. The repository still has no
  remote, so this commit is not published.
- Re-ran `tests/browser/test_scanner_import_e2e.cjs` against a fresh local
  MariaDB 11.8.6 container after the parent gitlink moved to RackWire `a0f456a`.
  The browser imported a scanner snapshot into SoR, saved a RackWire model
  profile, resolved the asset barcode, persisted the plan, and followed the
  Discovery UI through pending/running/completed states across navigation. Its
  scan-admission/status API is a controlled fixture; it does not replace the
  separate browser-to-native-worker acceptance. The schema and container were
  removed after the test; the flow passed.
- Re-ran `tests/browser/test_scan_worker_browser_e2e.cjs` with the committed
  RackWire submodule against a fresh, fully migrated MariaDB 11.8.6 monitoring
  schema. Starting in RackWire, the browser followed the embedded Discovery
  handoff, submitted an authenticated scan, observed the native worker's live
  status, navigated away and back, and saw the resulting reviewable candidate.
  The actual TCP connect scan was limited to `127.0.0.1/32` with a temporary
  listener; optional nmap enrichment used the controlled fixture. The test
  passed with one host checked and one candidate ingested. The scratch schema
  and DB container were removed after completion.
- Rechecked GitHub and Forgejo catalogs for alternate scanner, inventory, asset,
  or RackWire repository names under the expected owners. None were found. Exact
  GitHub/Forgejo scanner-ingest paths remain absent, and GitHub RackWire remains
  absent; destination choice is still needed before publishing those projects.

## 2026-10-21 — SolariNet candidate recorded locally

- Committed the 327-path SolariNet integration candidate as
  `2721771 feat(stabilization): close platform integration seams`. This commit
  includes the RackWire gitlink to `a0f456a`, the migration/tooling and test
  additions, and the paired manuals/runbooks. It is local only; no branch or tag
  was pushed.
- A high-confidence scan across 356 candidate paths found no private-key,
  GitHub/AWS/OpenAI/Anthropic, or Slack token patterns. The scan suppressed all
  values; it is a pattern check, not a comprehensive secret audit. The staged
  whitespace check found trailing spaces in the F09 lease contract; those were
  removed before the commit and the final check passed.
- Refreshed the Status & Operations Guide and PDF after the commit. The status
  snapshot records a clean worktree, the two matching published `main` refs at
  `5e8d688`, no release tags, and the status refresh commit's **20-commit**
  lead. The RackWire gitlink is committed locally, while the GitHub mirror and
  scanner-ingest remote are still absent.

## 2026-10-21 — realtime Discovery candidate visibility

- Updated the Discovery page to fetch the durable candidate list immediately
  when a scan enters `running`, then every 2.5 seconds until it completes. Scan
  status continues polling each second. This exposes each host after the worker
  commits it, while the scan is still in progress.
- Extended `tests/browser/test_scanner_import_e2e.cjs` with a controlled scan
  fixture. It proves a newly ingested candidate becomes visible before the
  fixture reports terminal completion, and confirms the page has no scan
  completion state at that point.
- Re-ran that full Chromium browser acceptance against a fresh MariaDB 11.8.6
  container populated from the current SoR schema snapshot. Scanner import,
  RackWire profile creation, barcode lookup, plan persistence, scan admission,
  progress across navigation, and live candidate visibility passed. The
  browser's SoR rows and the disposable database/container were removed.
- Re-ran `node --check`, `git diff --check`, and generated the paired scanner
  import guide PDF from its single-file HTML source. The guide now documents
  the scan bounds, progress cadence, live candidate refresh, and the distinction
  between network host adoption and physical asset import. It records that the
  browser uses a controlled candidate fixture; separate actual-worker coverage
  is limited to a loopback listener, while full lab-subnet scanning remains
  **UNVERIFIED**.
- No deployment, push, or release occurred. Cross-lab review remains an open
  gate because the configured Claude Code OAuth session is expired.

## 2026-10-21 — direct lab `/24` scan acceptance

- Rechecked the opt-in LAN test preconditions: `10.0.0.20` is assigned to
  `enp1s0`, and `10.0.0.254` routes directly through that interface without a
  gateway hop. The test bound its temporary listeners to the local address and
  retained the configured `/24` route guard.
- Created a disposable MariaDB 11.8.6 instance and applied all 33 monitoring
  migrations with `tools/migrate.py`. The real C scan worker and C control
  bridge then accepted the authenticated dashboard scan for `10.0.0.0/24`.
  The worker processed **254/254** addresses and ingested **12** reviewable
  candidates. The browser saw the local listener candidate in Discovery while
  the scan job was still running, then verified completion and progress through
  navigation away and back. The database container was removed after the test.
- After adding an explicit pre-completion browser assertion, repeated the direct
  `/24` acceptance. It again completed **254/254** and found **14** candidates;
  the browser observed the local listener row while the persisted job status was
  still `running`. This verifies realtime candidate ingestion through the native
  worker and authenticated UI on the directly connected lab range.
- This is direct-route lab discovery traffic. Scan job state and candidate rows
  lived only in the disposable database; no live SolariNet or SoR database was
  used. The scan does not establish coverage for larger ranges, repeated
  production polling, or the deployed worker unit; those remain
  **UNVERIFIED**.
- Updated the operator guide and status page, regenerated both paired PDFs, and
  checked that the guide describes the `/24` result while preserving the
  larger-range and deployment limits.

## 2026-10-22 — Xenon local backup deployment and restore

- Initial inspection confirmed Xenon's backup timer, installed runner/config,
  and mounted target were absent. MariaDB 11.8.6 was active, the root NVMe had
  362 GiB free, and `/var/backups/solarinet` was mode 0750 with existing
  rollback content; it was left untouched. NAS-X `SolariNetBackups` is still
  unprovisioned; SSH to NAS-X timed out and no NAS state was changed.
- Attempted the configured MariaDB socket identity (`mysql`) and TCP root
  access. Both were denied. A root Unix-socket `mysqldump` succeeded instead;
  its gzip stream was piped directly over SSH into a disposable local MariaDB
  11.8.6 container without writing an archive to disk.
- The live source and initial scratch restore both reported 29 base tables and
  zero views, triggers, routines, and events. `mariadb-check` passed every
  restored table. No write was made to the source database; that scratch
  container was removed.
- Corrected the Xenon example to use root Unix-socket authentication and omit
  host/port overrides. The former `mysql` identity and TCP root path were
  rejected on the live host; the root socket path used for the successful drill
  matches the systemd service's root identity.
- Added `backup.conf.xenon-local.example` for the host-local NVMe target and
  installed the runner, service/timer units, and fail-soft notifier on Xenon.
  The root-owned config uses the tested root Unix socket, targets a new
  `/var/backups/solarinet-db` directory at mode 0700, and requires no password
  file. The pre-existing backup directory was not modified.
- Started the one-shot systemd service. It completed successfully in 22.4 s and
  wrote `solarinet-2026-10-22.sql.gz` (56,383,267 bytes), root:root mode 0600;
  `gzip -t` passed. Streamed this exact artifact into a disposable MariaDB
  11.8.6 container after creating the scratch DB. All 29 tables passed
  `mariadb-check`; object counts matched at 29 base tables and zero views,
  triggers, routines, or events. The scratch container was removed.
- Enabled `solari-backup@xenon.timer`; systemd reported it active with the next
  run at 2026-10-22 03:34 UTC. This provides a recurring local recovery copy,
  but Xenon/NVMe failure still loses both the DB and copy. NAS provisioning,
  effective CIFS modes, off-host artifact/restore, Forgejo recovery, and
  production recovery objectives remain **UNVERIFIED**.
- Updated the backup README, paired monolithic runbook HTML/PDF, and status
  guide HTML/PDF with the deployed state and remaining off-host gates.

## 2026-10-22 — Xenon discovery deployment acceptance inventory

- Read-only checks on Xenon found Apache, PHP 8.4 FPM, and `solarinet-server`
  active. The dashboard's unauthenticated `/api/discovery` request returned
  HTTP 401, consistent with its authentication boundary.
- `rackwire-discovery.service` is loaded and active on loopback port 8785. Its
  local health endpoint and the Apache RackWire proxy health endpoint both
  returned HTTP 200. This verifies service reachability only; the health response
  includes a CSRF token, which was not printed or retained.
- The candidate bundle's `dashboard/public/screens3.jsx` does not match the
  Xenon-served `/var/www/solarinet/screens3.jsx` hash. Xenon's RackWire service
  is therefore not evidence that the candidate's authenticated dashboard scan,
  live candidate polling, or scanner-ingest-to-RackWire flow is deployed there.
  The Xenon source checkout is on `stabilize/phase-0-containment` at `ed57e72`
  with pre-existing user changes; it was not modified.
- No database writes, scan jobs, service changes, or webroot changes were made.
  **UNVERIFIED:** authenticated PHP-FPM/browser flow on Xenon, live candidate
  scan behavior, and candidate release deployment/rollback. Rehearse against a
  disposable database and isolated release path after independent review.

## 2026-10-22 — realtime scan retry idempotency

- Wired optional request headers through the dashboard API helper and added a
  bounded per-request key cache for Discovery scan admission. A retry with the
  same CIDR/ports reuses its key after a lost response; a confirmed admission
  consumes the key so an intentional later scan is a new job.
- Extended the browser-to-native-worker test to let the first request reach
  PHP, the C control queue, and MariaDB, then abort the browser response. The
  retry used the same key, received the same durable admission, and the
  disposable schema contained exactly one scan job. A browser-level API check
  also confirms network failure retains the key across reload, successful admission consumes it,
  and a later intentional scan gets a fresh key. The native worker completed
  `127.0.0.1/32` and ingested one candidate.
- The test used a fresh MariaDB 11.8.6 container bound to loopback. Migration
  033 was applied, the browser test passed, and the container was removed. The
  PHP route test confirms the key is forwarded and invalid key syntax is rejected;
  `node --check` and `git diff --check` pass.
- **UNVERIFIED:** hosted CI for this uncommitted patch, independent review, and
  the authenticated scan UI against Xenon's deployed dashboard. A secret-free
  cross-lab review brief is available at
  `docs/SolariNet_Cross_Lab_Review_Brief_2026-10-22.md`; no reviewer has yet
  returned findings. No live SoR, scan, or service state was changed.

## 2026-10-22 — realtime network scan and SoR ingest acceptance

- Extended the authenticated browser acceptance through the operator's
  candidate adoption action. The browser follows RackWire's Network discovery
  handoff, scans `127.0.0.1/32` with the native worker, sees the candidate,
  handles a lost admission response by retrying the same idempotency key, and
  adopts the candidate through the Monitor dialog.
- The test confirms one durable scan job, one discovered row changed to
  `adopted`, a monitoring asset, and the mapped IPv4 address on a live SoR
  entity. The SoR row assertion queries the separate SoR database rather than
  the monitoring schema. Physical asset intake remains the scanner-ingest
  preview/apply flow; network discovery adoption creates a monitored host and
  SoR host/address, not a physical asset.
- The test passed using separate disposable monitoring and SoR schemas on
  MariaDB 11.8.9. The browser path used the locally installed Chromium and the
  real PHP API, C control bridge, native worker, and authenticated dashboard.
  The scratch database container used loopback only and was removed after the
  run. `php tests/dashboard/test_discovery_scan_input.php`, `node --check`,
  and `git diff --check` also passed.
- Repeated the combined acceptance with the guarded direct-route lab scan.
  The workstation owned `10.0.0.20` and routed directly to `10.0.0.254` through
  `enp1s0`; the test was restricted to `10.0.0.0/24`. It processed **254/254**
  addresses, found **12** candidates, and the browser observed the listener's
  candidate while the worker still reported `running`. It then retried the lost
  admission response, confirmed one durable scan job, adopted `10.0.0.20`, and
  verified the monitoring asset and SoR address in the separate scratch
  schemas. The MariaDB 11.8.9 container was removed after the run; no live
  inventory or SoR database was used.
- Updated the CI browser step to pass the isolated SoR connection and require
  `SOLARI_TEST_SOR_DB=1`; the end-to-end test now refuses to run without both
  dedicated scratch database configurations.
- **UNVERIFIED:** hosted CI for this uncommitted extension, cross-lab review,
  authenticated browser testing against Xenon's deployed dashboard, and scan
  throughput beyond the tested `/24`.

## 2026-10-22 — idempotency payload binding review fix

- Review identified that the queue previously replayed a matching operator/key
  without checking that the new verb and arguments matched the stored request.
  `serverDbCtlRequestSubmit` now compares both fields and rejects mismatched key
  reuse with `ERR_INVALID_ARG`; exact retries still return the existing request
  id. The live database integration test asserts the mismatch creates no row.
- `cmake --build build-server --parallel 4` passed. The full live database
  integration test passed against a fresh MariaDB 11.8 container after
  applying monitoring migrations through 033; the container was stopped and
  removed. Root PHP scan-route tests, JavaScript syntax checks, and
  `git diff --check` passed.
- **UNVERIFIED:** cross-lab review of this fix, hosted CI, and any behavior on
  the deployed dashboard. No live scan, SoR inventory, DNS, or UniFi state was
  changed.

## 2026-10-22 — realtime discovery and inventory candidate CI

- Committed the authenticated realtime scan retry, server-side payload-bound
  idempotency, scan-to-monitoring/SoR browser acceptance, scanner-ingest export
  to RackWire browser acceptance, scratch SoR CI wiring, accessibility labels,
  broker configuration guide, and updated status/review artifacts as
  `789b1d3`. The commit was pushed to matching GitHub and Forgejo
  `integrate/stability-20261020` refs. GitHub draft PR #37 run `39715155684`
  passed all 15 jobs, including dashboard/API, scanner/RackWire browser,
  server/database, cross-platform, and RabbitMQ-version coverage.
- Local verification also passed the full monitoring CTest suite (36/36) against
  disposable MariaDB migrated through 033, the separate loopback transport
  test, scanner-ingest pytest (47 passed, 16 skipped, 31 subtests), UniFi tests
  (18/18), and DNS release tests (17/17). RackWire PR #1 remains green at
  `99453f3`.
- **UNVERIFIED:** independent cross-lab review; authenticated browser behavior
  on Xenon's deployed dashboard; exact physical destination mapping for
  scanner locations “Bin #5” and “Nook”; scanner-ingest publication ownership
  and remote; and deployment/release integration. No main branch was merged,
  no version was tagged, and no live SoR, DNS, or UniFi state was changed.

## 2026-10-22 — mirrored CI and deployment-target recheck

- Root status and review documentation were committed as `04e80be` and pushed
  to matching GitHub and Forgejo integration refs. Hosted run `39715807073`
  passed all 15 jobs. The scanner-ingest manual/validation updates were
  committed locally as `c2a3f7b`; the repository still has no configured remote.
- Xenon was checked read-only before deployment. The active server runs from a
  separate checkout at `/home/jason/Code/Solarinet` on branch
  `stabilize/phase-0-containment`, with uncommitted changes. Apache serves
  `/var/www/solarinet`; PHP 8.4-FPM is active, `/opt/solarinet/current` is
  absent, and `solarinet-scan-worker.service` is inactive. This differs from
  the candidate's atomic release contract. No host files or services were
  changed; an isolated candidate target or reviewed host-layout migration is
  required before deployment.
- **UNVERIFIED:** candidate runtime behavior on Xenon; cross-lab review; and
  whether the user prefers creating a private hosted repository for the local
  scanner-ingest app. The production-like DNS and UniFi systems were untouched.

## 2026-10-22 — deployment boundary and final status verification

- The status-documentation update records the Xenon deployment mismatch and
  was committed as `b81e2cc`, then pushed to the matching GitHub and Forgejo
  integration refs. Hosted PR run `39716727270` passed all 15 jobs. Earlier
  code-bearing commit `789b1d3` passed run `39715155684` (15/15), including the
  browser scan-to-SoR and scanner/RackWire paths.
- **UNVERIFIED:** no candidate has been activated on Xenon or Cesium. Xenon
  remains on its separate dirty checkout, with no `/opt/solarinet/current` and
  no active scan worker; the documented atomic release contract is not wired
  to that host. No main branch merge, release tag, or deployment was performed.

## 2026-10-22 — authoritative scanner-location hierarchy check

- Corrected the earlier location-state check: `sor_probe` was on the local test
  environment, not Cesium. A read-only transaction using Xenon's existing
  SoR-sync service configuration queried the live SoR. Active location IDs
  9103 and 9104 are duplicate root sites named “Painted Fern”; both have blank
  notes, no child locations, and no attached hardware units or entities. No
  exact SoR locations named “Bin #5” or “Nook” were present.
- The duplicate-site mapping is therefore still ambiguous. No location, unit,
  entity, or scanner data was written. Keep those source locations explicitly
  mapped by an operator before applying the live scanner import.

## 2026-10-22 — RabbitMQ live health and upgrade gate recheck

- Read-only Benzene inspection found the rootful Podman container `rabbitmq`
  running RabbitMQ 3.13.7 with zero restarts and `unless-stopped` policy.
  `podman-restart.service` is active. The image remains the floating
  `rabbitmq:3-management` tag.
- Diagnostics reported the node fully booted with no local or cluster alarms.
  In vhost `solari`, all four application queues are durable classic queues;
  each has one consumer and zero ready or unacknowledged messages. Seven client
  connections were present.
- The data-volume rehearsal and restore remain as previously recorded. The
  project gate still requires independent review and pinned startup before
  upgrading the live broker; the cross-lab reviewer is not authenticated. No
  broker, volume, or service state was changed during this check.
- **UNVERIFIED:** host-reboot recovery on a pinned image, a real application
  publish/consume during the upgrade window, and rollback from the upgraded
  volume. Defer live cutover until the review and recovery plan are closed.

## 2026-10-22 — certificate replacement and UniFi comparison gate audit

- Rechecked the current tree after an audit reported a missing certificate
  replacement assertion. `tests/integration/test_gossip.sh` already creates
  old and replacement leaf certificates with the same subject and distinct
  serials, revokes the old leaf, and passes both to the pinned-stack test.
  `tests/integration/test_gossip.c` verifies the old leaf is rejected and the
  replacement is accepted and admitted as the same logical gossip identity.
  `ctest --test-dir build-server --output-on-failure -R '^test_gossip$'` passed
  1/1. This is loopback CRL-handshake evidence, not proof of fleet distribution
  or activation.
- Rechecked the proposed UniFi/SoR planner gap against the current code. The
  GET-only collector, `load_sor_state()` read-only transaction, and
  `compare_sor()` report already implement the needed conservative comparison;
  a second planner would duplicate that behavior. The comparison labels the
  source `/8` versus overlapping SoR `/24`s `review_required`, reports subnet
  and DHCP conflicts, and has no write path. All 18 UniFi poller/topology tests
  passed. This test run used fixtures and did not re-read the live controller or
  SoR.
- These checks close the specific replacement-leaf test gap and show the
  proposed separate planner is redundant. They do not close the broader F-01
  fleet-update/revocation release gate or resolve the live UniFi source-of-truth
  discrepancy. Cross-lab security review remains **UNVERIFIED**.

## 2026-10-22 — launched scanner export through SoR and RackWire

- Read the running scanner-ingest app's schema-v1 JSON export through its
  loopback endpoint. It contained two catalog items and two assets. The browser
  E2E uploaded that exact export, previewed without writes, mapped both source
  locations, applied to a disposable SoR, and read both assets back. RackWire
  then resolved one imported asset's barcode and persisted its tag and serial on
  a plan. The CI-default synthetic export fixture passed in a second run.
- The export's two location selectors initially had accessible names that
  included their option lists, causing an ambiguous test selection. Added
  explicit accessible names to the destination selectors and unmatched-location
  policy in `dashboard/public/screens4.jsx`; both the actual-export and synthetic
  browser paths passed afterward.
- The test deliberately created duplicate scratch destinations for each source
  location and chose one by row ID. This verifies operator mapping and the
  integration contract; it does not establish that either real source location
  maps to the duplicate live “Painted Fern” sites. The recorded ambiguity
  remains unresolved.
- A post-run check found zero imported models, units, locations, RackWire plans,
  external references, or scanner-ingest source rows in the disposable schema.
  The export file was removed; the scanner app remained unchanged and loopback
  bound. No live SoR write occurred.
- **UNVERIFIED:** production SoR import and exact location ownership, hosted CI
  for the updated browser harness, cross-lab review, and deployed dashboard
  behavior. The standalone scanner-ingest repository still has no release
  remote.

## 2026-10-22 — hosted candidate pass and RackWire CSRF retry coverage

- GitHub PR #37 run `39718723877` for commit `5160202` passed all 15 workflow
  jobs, including live Discovery worker/API and candidate adoption, authenticated
  scanner import and RackWire browser acceptance, dashboard UI checks, clean
  database installs, and release package activation/rollback tests. This is
  hosted CI evidence, not a deployed-host check.
- A read-only SSH preflight of Cesium confirmed the `jason` account, MariaDB,
  and ample disk capacity. PHP CLI and Podman/Docker are absent. No test app,
  runtime, or database was deployed or created.
- Independent internal RackWire review found that stale-CSRF retry was only
  covered by source matching. Added behavioral tests that expire the first token
  for plan writes and barcode lookup, verify the refreshed token and unchanged
  request payload, and prove a second rejection stops after exactly one retry.
  `node tests/dashboard/test_rackwire_ui.js` now passes 60/60 locally.
- Extended the combined scanner/RackWire browser E2E to inject an
  expiry-shaped 403 for the first barcode request, refresh identity through the
  real PHP endpoint, and let the retry reach the real guarded inventory/SoR
  handler. The browser flow passed against a freshly migrated loopback scratch
  SoR. The test driver dropped the scratch schema and temporary account after
  the run.
- Hosted CI run `39720370015` for commit `23aee5b` passed the full workflow matrix,
  including the scanner/RackWire browser path and dashboard API tests.
- **UNVERIFIED:** cross-lab review, actual elapsed-token expiry handling by the PHP
  guard, deployed candidate behavior, exact scanner source-location mapping, and
  scanner-ingest repository ownership/remotes.

## 2026-10-22 — durable realtime Discovery recovery

- Added the operator-authenticated `GET /api/discovery/scan-jobs` endpoint. It
  returns at most the ten newest jobs for the authenticated requester, with
  bounded progress fields and no requester identity in the response.
- Discovery now queries that endpoint when browser session state is missing. It
  resumes an active queued/running job first, or shows the newest terminal job;
  recovered active jobs continue the existing status polling and keep duplicate
  scan submission disabled. The scan still runs in the durable worker/database
  path when a tab closes.
- Added route coverage for requester scoping and API row normalization, plus
  browser coverage for active recovery and lost-session recovery after a native
  scan completes.
- Verification on 2026-10-22: PHP syntax and scan route tests passed; the
  scanner-import/RackWire Chromium E2E passed against the isolated Cesium SoR
  schema on loopback port 13316; the RackWire-to-native-worker E2E passed with a
  real loopback scan, native worker and C bridge, disposable MariaDB schemas,
  SoR candidate adoption, idempotent admission retry, and terminal job recovery
  after clearing sessionStorage. These tests establish scan plumbing and local
  host discovery, not authorization to scan a broader subnet or deployed service
  behavior.
- Cesium has only a per-user loopback MariaDB scratch instance and temporary
  PHP CLI packages for these checks. No dashboard service, public listener,
  persistent worker, or live SoR change was deployed. Package installation also
  caused repository-resolved glibc package updates; `zypper ps -s` reported no
  process using deleted files or updated core libraries/services since boot.
- `git diff --check`, PHP syntax, route tests, and Node syntax checks passed.
  Hosted CI, cross-lab review, a staged app deployment, mirrored push, merge,
  and release have not yet been performed for this working diff.

## 2026-10-22 — isolated Cesium development deployment

- Built a clean `Release` configuration with the IO, SQLite, JSON, server, and
  scan-worker components enabled. The compiler emitted existing truncation and
  maybe-uninitialized warnings in client/SNMP paths; both server and scan-worker
  binaries built successfully without ASAN/UBSAN runtime dependencies.
- Packaged commit `4b50cca7d8d78bbd644b86e1d86a70990a0f7051` as
  `dev-20261022-4b50cca`. The archive contains 180 allowlisted files, records
  the pinned RackWire gitlink, reports `sourceTreeDirty=false`, and has SHA-256
  `b271c39fad19fa9ece8dbef96a07e94285f4a7ffd22dca9ad6b83754e49d0ab4`.
  Dashboard release tests passed 7/7; two dirty-tree-only cases were skipped.
- Installed and activated the release under Cesium's separate
  `~/.local/share/solarinet-dev/release-root`. The release integrity installer
  validated the payload. A PHP CLI development server is listening on
  `127.0.0.1:17701` only, using the separate disposable SoR/monitoring databases
  on `127.0.0.1:13316`. The UI shell and public auth configuration return 200;
  unauthenticated scan-job history returns 401. No test identity bypass was
  installed. This is a loopback development deployment, not PHP-FPM, Apache, a
  persistent scan worker, or the production dashboard.
- The currently running dev package is the code candidate at `4b50cca`; this
  workflow note records the subsequent deployment verification. Its app code
  has not changed since the release was packaged. The user-owned PHP CLI package
  install on Cesium also resolved glibc updates as recorded above.
- **UNVERIFIED:** authenticated browser operation against the installed PHP
  service, persistent worker startup/restart, broader subnet scan policy, live
  SoR migration/adoption, cross-lab review, and promotion to a main branch/tag.

### Staged dashboard authentication acceptance

- On 2026-10-22, exercised the installed candidate through Chromium via an SSH
  loopback tunnel. A random temporary operator credential was stored only in a
  mode-0600 scratch file and used to sign in through the actual dashboard UI.
  The browser then fetched authenticated scan history (HTTP 200, empty result),
  obtained the session CSRF token, logged out through the protected route
  (HTTP 200), and verified `whoami` returned 401 afterward. No browser page
  errors occurred.
- The temporary auth file, password, rate-limit directory, and browser session
  were removed. The PHP listener was restarted without local-login settings;
  `auth/config` reports local login disabled, the UI still returns 200, and an
  unauthenticated scan-history request returns 401. The listener remains bound
  to `127.0.0.1:17701` and uses only the disposable Cesium databases.
- This verifies the installed PHP CLI runtime and actual login/session/CSRF
  flow. It does not verify PHP-FPM, Apache, OIDC, a persistent scan worker,
  production data, or a cross-lab review.

## 2026-10-22 — installed scan worker and operator adoption acceptance

- The first installed scan attempt exposed a host dependency absent from Cesium's
  PHP CLI: `ctype_digit()` was undefined. Installed `php8-ctype` 8.5.12-4.1,
  restarted the loopback PHP server, and repeated the flow successfully. The
  server's scan input validation, not the scan worker, had failed before this
  dependency was present.
- Started the packaged `solariServer` and `solariScanWorker` against the separate
  `solarinet_scan_worker_dev_20261022` schema. The server bridge socket is in the
  user-owned dev runtime directory. All TCP listeners (17711–17714) bind only
  to 127.0.0.1; TLS is disabled only for this loopback instance; auto-discovery
  is disabled. The PHP API points at that bridge and the isolated monitoring
  and SoR schemas on loopback port 13316.
- Through Chromium, signed into the installed dashboard, submitted an explicit
  `127.0.0.1/32` scan, and observed completion. Native worker log and monitoring
  DB both show scan 9 completed: one host processed and one candidate saved.
  The scan found the temporary loopback HTTP listener and other local services;
  it did not scan a lab subnet.
- In a new browser session with empty sessionStorage, the dashboard recovered
  the completed job and candidate from the server. The operator adopted that
  candidate as `cesium-dev-scan-1792645974579`. The C bridge logged the adoption
  and dispatched local TCP/HTTP/ICMP probes. Read-only DB checks found the
  monitoring asset at `127.0.0.1`, the SoR network entity, and its IPv4-mapped
  address (`00000000000000000000FFFF7F000001`). This adoption created a network
  host entity, not a physical hardware unit.
- The development operator account is retained for Jason's loopback testing;
  the password is in the mode-0600 file
  `~/.local/share/solarinet-dev/test-password`. The account is rate-limited,
  local login is enabled only on the loopback PHP CLI instance, and there is no
  listener exposed on the LAN. Use an SSH tunnel to reach port 17701. The app,
  server, and worker are running as user processes, not reboot-persistent
  systemd services.
- Test target `127.0.0.1:8080` remains an explicit local HTTP endpoint returning
  200 so the adopted development host's HTTP check does not report a false
  failure. No production SoR, DNS, UniFi, or external network state was changed.
- **UNVERIFIED:** PHP-FPM/Apache behavior, OIDC, service restart/reboot recovery,
  broad-range throughput/policy, physical scanner hardware, promotion to main,
  release tagging, and cross-lab review.


### PHP runtime dependency contract

The Cesium acceptance found that the separated PHP CLI image lacked both `ctype` and `openssl`: the first prevented scan CIDR validation; the second would prevent OIDC signature verification. Installed `php8-ctype` and `php8-openssl` 8.5.12-4.1. The active PHP process was restarted and verified with `ctype_digit()`, `openssl_verify()`, and `pdo_mysql`. Added `tests/dashboard/test_php_runtime_extensions.php` and a CI step covering ctype, OpenSSL, PDO/MySQL, sessions, and JSON; documented the required extensions and openSUSE package names in `deploy/dashboard/README.md`. Local runtime contract test and PHP syntax check pass.


### 2026-10-22 Discovery polling overlap and cancellation hardening

- A slow-response browser regression reproduced overlapping scan-status requests
  when Discovery was navigated away from and mounted again. The previous
  interval could outlive its screen component, and each component instance
  tracked requests independently.
- The dashboard API now forwards an optional `AbortSignal` to `fetch` for scan
  admission/status reads. Discovery uses one module-scoped poll lock across
  component remounts, aborts its active read on unmount, and bounds each read to
  eight seconds so a stalled request cannot suspend scan progress indefinitely.
- `tests/browser/test_scanner_import_e2e.cjs` now holds status responses for
  1.5 seconds (longer than the one-second polling interval), verifies repeated
  requests do not overlap, navigates away to assert cancellation, and returns to
  confirm the durable scan resumes. It passed against a fresh, fully migrated
  local SoR scratch database; the database, user, and temporary credential file
  were removed. Dashboard JSX parsing (16/16), layout, scanner-import UI, API
  freshness, Node syntax, and whitespace checks pass.
- **UNVERIFIED:** hosted CI for this polling patch and independent cross-lab
  review. No production SoR, DNS, UniFi, or deployment state changed.


### 2026-10-22 Intranet-only scan range enforcement

- The Discovery admission endpoint and native worker previously accepted any
  IPv4 CIDR up to the 4,096-host cap. That allowed a valid operator request to
  target public address space, despite the scanner being intended for the lab
  intranet.
- Both the PHP API and native C parser now require the complete CIDR to fit
  within one RFC1918 private block, loopback (`127.0.0.0/8`), or IPv4
  link-local (`169.254.0.0/16`) block. A CIDR that crosses a prefix boundary is
  rejected even if its starting address is private. The host-count limit and
  operator/CSRF checks remain in force.
- PHP tests cover accepted private, loopback, and link-local ranges plus
  public documentation ranges and private/public boundary-crossing prefixes.
  Native parser and scan-plan tests cover the same public-range rejection and
  private-range boundary behavior. Local validation passed: PHP input/route
  tests, full CTest (37/37), the updated scan/control C tests (2/2), and
  Node/Python syntax checks. Hosted run `39733247468` passed all 15 jobs,
  including the database-backed scan/adoption API test and browser-to-native
  worker candidate-ingest acceptance.
- The scanner/import HTML manual now describes the allowed ranges and realtime
  polling cancellation behavior, and its matching Letter PDF was regenerated.
- **UNVERIFIED at this checkpoint:** deployment of the updated worker/API to
  the Cesium loopback development instance. The follow-up below records that
  deployment and the runtime packaging correction.

## 2026-10-22 — loopback deployment of intranet scan policy

- Deployed clean package `dev-20261022-2cb099c-static` from source revision
  `2cb099c442d14eb8e57b140188941512440c8d3f`; the embedded RackWire pin is
  `a66bd218acc89b668a8f1a89291f2c32bb38ac59`. The archive SHA-256 is
  `17233165ace3b100e211bc06546612ecfffc1da4e903361c787a2d50db19ec49`. The
  package contains 180 allowlisted files and its RELEASE manifest was verified.
- Two package experiments exposed a portability gap. Sanitizer binaries
  required Cesium libraries that were absent. A normal Release build linked
  NNG dynamically from a temporary build path and then resolved an incompatible
  system NNG. The active package uses a Release build with the pinned NNG and
  mbedTLS archives statically linked; `ldd` and `readelf` showed no NNG,
  mbedTLS, ASan/UBSan, or temporary RUNPATH dependencies. Server, worker, and
  PHP processes listen only on loopback; the worker logged ready and the
  server acquired its development DB lease.
- The first PHP launch omitted the development DB variables and correctly
  rendered offline mode. After restarting the PHP process with the isolated
  `solarinet_scan_worker_dev_20261022` database configuration, the authenticated
  dashboard returned live mode. The API rejected `192.0.2.0/24`; the browser
  then submitted only `127.0.0.1/32`. Scan job 9 completed with 1/1 hosts and
  one candidate; authenticated Discovery displayed it. Summary, jobs, and
  candidate reads returned HTTP 200, with no browser page errors or
  cross-origin requests.
- Hosted docs-only run `39734027278` passed all 15 jobs; code run
  `39733247468` passed all 15 jobs. This was a development-only loopback scan;
  no lab subnet or production monitoring, inventory, DNS, or UniFi data was
  changed.
- **UNVERIFIED:** reproducible CI packaging of a portable Release binary set,
  PHP-FPM/Apache behavior, larger-range throughput, service reboot recovery,
  and independent cross-lab review. The current CI release package test still
  uses sanitizer-built binaries and needs a runtime dependency gate.

## 2026-10-22 — Release binary portability gate added

- Added `deploy/build-release-binaries.sh` to build the Linux Release server and
  scan worker against the existing pinned static NNG/mbedTLS archives.
- Added a runtime checker that rejects ASan/UBSan and dynamic NNG/mbedTLS
  dependencies, any embedded RPATH/RUNPATH, and unresolved `ldd` libraries.
  The local Cesium-compatible Release binaries pass; the existing sanitizer
  binary pair is rejected as expected.
- CI now keeps the instrumented build for sanitizer coverage, builds the
  portable Release pair separately, exercises the Release server and worker
  against disposable MariaDB schemas, uploads those binaries, and uses them
  for release archive activation/rollback tests. The packaging test accepts
  an explicit binary directory so it exercises the actual Release artifact.
- Local static Release build and the 9 release archive/rollback tests pass.
  Shell syntax, Python compilation, CI YAML parsing, and `git diff --check`
  pass. ShellCheck is not installed in this workspace.
- GitHub run `39737421770` passed all 15 jobs on commit `8f5af081`. It built
  the portable Release binaries, exercised the Release server failover and
  scan worker lifecycle against disposable MariaDB schemas, uploaded those
  artifacts, and passed dashboard archive activation/rollback using them.
- **UNVERIFIED:** independent cross-lab review. This CI-profile patch changed
  no installed binary or production service.


## 2026-10-23 — F-01 status reconciliation

- Reconciled the current F-01 findings-table summary with the candidate contract:
  issuance inventory/backfill and CA-signed CRL generation exist locally; peer
  distribution, activation/freshness enforcement, fleet acknowledgements, and
  rollout remain open. Historical dated implementation notes are unchanged.
- Re-ran the OpenSSL-backed CRL generator suite (5/5) and pinned-stack gossip
  CRL handshake integration (`ctest -R gossip`, 1/1); both passed.
- **UNVERIFIED:** end-to-end fleet delivery/activation, stale-CRL policy,
  acknowledgement tracking, deployed migration/backfill, and independent
  cross-lab security review. This documentation reconciliation made no live
  network or service changes.

## 2026-10-23 — F-01 scheduled generation candidate

- Added a hardened systemd oneshot and six-hour timer for the CRL generator.
  It runs as `solari`, reads the CA key within that service boundary, uses a
  dedicated MariaDB defaults file and grants, and writes only to a private
  state directory. Added operator setup and verification notes.
- `sudo systemd-analyze verify` passed for both units; the calendar expression
  resolves to a six-hour cadence. The OpenSSL-backed generator tests passed
  5/5. Added the verifier to both GitHub and Forgejo CI workflows; the local
  workflow parity check passed. Hosted CI has not run for these edits. No host
  files or services were changed.
- **UNVERIFIED:** deployment-specific DB grants/credential, installed CA paths
  and permissions, timer execution, publication, peer activation, freshness
  enforcement, and acknowledgement reporting. The generated CRL remains local
  and does not make TLS revocation effective on peers.


## 2026-10-23 — F-01 peer activation candidate

- Added `activate_crl.py`: it requires HTTPS, validates the issuing CA is in the
  immutable base bundle, verifies CRL signature/issuer/number/freshness, rejects
  rollback and conflicts, atomically installs the bundle, restarts an
  allowlisted SolariNet service, and restores the prior bundle if the restart or
  active-state check fails. A per-service activation marker makes interrupted
  installs reconcile on the next run. A templated hourly systemd timer supports
  separate per-service configuration.
- Added seven tests covering verified local HTTPS, valid install, signature and
  expiry rejection, rollback rejection, idempotence, interrupted-activation
  recovery, service allowlisting, and restart rollback. Both CRL test suites,
  systemd unit verification, unit-section checks, Forgejo workflow parity, and
  whitespace validation pass locally. CI jobs now include both cryptographic
  suites and both timer/service unit checks.
- **UNVERIFIED:** publisher endpoint, deployment configuration, multiple issuer
  bundle merging, central acknowledgement reporting, hosted CI, and fleet
  behavior. No host or live service was changed.

## 2026-10-23 — F-14 authentication candidate reconciliation

- Re-ran `test_auth_session.php`, `test_oidc_claims.php`, and the new
  HTTPS-backed `test_oidc_userinfo_session.php`; all pass with
  `TMPDIR=/dev/shm`. PHP syntax checks pass for `Auth.php` and `Oidc.php`.
- The candidate requires signed OIDC issuer/audience/subject/expiry/issued-at/
  nonce claims and a bounded one-shot state transaction. Local password login
  is disabled unless explicitly enabled and a private rate-limit directory is
  configured; attempts are capped per account/source. Sessions have idle and
  absolute limits, local sessions re-read role changes, and disabling local
  login revokes existing local sessions.
- OIDC sessions now refresh group-derived roles over HTTPS UserInfo every 60
  seconds, require the stable OIDC subject to match, rotate expiring access
  tokens through the refresh grant, and downgrade to viewer on provider
  transport/service errors. Discovery issuer identity is checked and every
  authorization, token, JWKS, and UserInfo endpoint must use HTTPS on the
  issuer origin or an explicitly configured `allowedEndpointOrigins` entry.
  OIDC HTTP response bodies are capped at 1 MiB. Subject mismatch revokes the
  session; transport, service, and oversized-response failures demote it to
  viewer.
- **OPEN:** deployed provider UserInfo group claims and refresh-token behavior,
  production configuration, hosted CI, and independent cross-lab review remain
  **UNVERIFIED**. No authentication service or live configuration was changed.

## 2026-10-23 — reconciliation regression sweep

- Built the current server profile and ran `TMPDIR=/dev/shm ctest
  --test-dir build-server --output-on-failure -E test_net_loopback`: all 38
  configured tests passed. `test_net_loopback` is excluded by the repository's
  normal CI command and was not run here. The first attempt without `TMPDIR`
  failed multiple temporary-file tests with `Disk quota exceeded`; all 11
  affected core tests passed on retry with `/dev/shm`.
- Authbroker passed 20 tests, MCP passed 48, CRL generation/activation passed
  17, fleet manifest passed 17, the F-07 provisioning gate passed, and the
  Tab5 canonical approval host-vector compiled with warnings as errors and
  passed. Dashboard PHP syntax checks, auth/session/OIDC, authorization, route,
  security-header, TLS, JSON, RackWire API/UI, and local scanner UI suites passed.
- The standalone scanner-ingest suite completed 63 tests: 48 passed and 15
  MariaDB-backed tests were skipped because no disposable DB credentials were
  configured. The running scanner app answered a read-only loopback export
  request with schema v1, two catalog items, and two assets; its socket is
  bound only to `127.0.0.1:8765`. No inventory content was logged or changed.
- A read-only SSH check reached Cesium, but its MariaDB root account rejected
  socket access without a password. No database or account was created. SoR
  importer integration is therefore **UNVERIFIED in this sweep**; earlier
  disposable-database evidence remains recorded above.
- Rechecked PHP after the Foundation's 2026-10-23 session-progress notice.
  Cesium's sole active PHP listener is fixed PHP 8.5.12 on loopback; effective
  `session.serialize_handler` is `php`, and deployed PHP source does not change
  it at runtime. Xenon's FPM reports upload progress `Off`; Nitrogen has fixed
  PHP 8.4.27 CLI and no PHP service. No restart was needed: the PHP notice says
  the additional unusual trigger requires a runtime serializer switch, which
  this code does not perform. See [PHP security disposition](PHP_Critical_Update_2026-10-23.md).
- Added a 1 MiB response cap to OIDC HTTP reads and tested oversized UserInfo
  downgrade behavior. Discovery identity and HTTPS origin checks cover the
  issuer and authorization, token, JWKS, and UserInfo endpoints. The
  Directory Services HTML/PDF guide now describes this candidate behavior; the
  regenerated PDF has four A4 pages and was visually checked on pages 3–4.
- **OPEN:** independent cross-lab review remains unavailable in this harness:
  the operator-required `model-routing` skill/roster is absent. Hosted CI,
  disposable SoR integration in this sweep, browser acceptance against the
  live provider, publication, and deployment remain **UNVERIFIED**. No code was
  pushed and no service, production database, DNS, UniFi, or network state was
  changed.

### 2026-10-23 — scanner-to-SoR and RackWire database integration

- Ran `tests/dashboard/test_scanner_import_http.php` against a fresh
  `solarinet_scanner_it` MariaDB database initialized from
  `netdb/sor/schema.snapshot.sql`. The test passed its authenticated scanner
  preview/apply, provenance, SoR persistence, explicit entity association,
  RackWire model profile save/readback, pagination, stale-fingerprint, and
  idempotent replay checks.
- The isolated fixture included the `manual`, `dashboard`, and `rackwire`
  source rows required by those API paths. The schema snapshot contains the
  tables and triggers but does not seed those rows; without them the API
  correctly rejected audit/profile writes through foreign-key/source checks.
- The scratch MariaDB used a dedicated datadir on Cesium, bound only to
  `127.0.0.1:13317`, and was reached through a temporary SSH tunnel. The
  scratch database, MariaDB process, tunnel, and datadir were removed after
  the run. Cesium's existing MariaDB services, shared SoR, and scanner app
  inventory were not used or changed.
- This verifies the real dashboard API/database integration path. Physical
  scanner/iPhone use and a live shared-SoR import remain unverified. Hosted
  CI, publication, deployment, and independent cross-lab review remain open.
- By Jason's explicit request, the standalone scanner-ingest development app
  was rebound from loopback to `0.0.0.0:8765` for iPhone barcode-scanner use.
  Its current host address is `10.0.0.20`; the API responds through that
  interface with two items and two assets. This remains a foreground,
  unauthenticated HTTP process with no TLS, not a durable service deployment;
  network reachability from the iPhone itself is still unverified.
- Checked the standalone repository's expected publish destinations. Neither
  `jason/scanner-ingest` on Forgejo nor `soren42/scanner-ingest` on GitHub
  exists, and its local Git config has no remotes. The Forgejo HTTPS API was
  unavailable during this check. No repository was created and nothing was
  pushed; a confirmed destination is still required before publishing this
  separately rooted subproject.

### 2026-10-23 — OIDC hardening re-review and TLS fixture rerun

- An independent review confirmed that issuer identity and HTTPS origin
  enforcement now cover authorization, token, JWKS, refresh, and UserInfo
  requests through the shared transport; redirects are disabled, explicit
  split-host origins are exact-origin matched, and responses are capped at
  1 MiB. No code edits were requested.
- Re-ran `test_oidc_claims.php` and `test_oidc_userinfo_session.php` with
  `TMPDIR=/dev/shm`; both passed. The TLS fixture exercised token rotation,
  role downgrade on service/oversized UserInfo responses, and session revocation
  on subject mismatch. This resolves the earlier quota-related test gap.
- **OPEN:** production OIDC configuration and a real identity provider remain
  unverified. In particular, the explicit development-only
  `insecureSkipVerify` option is false by default, but deployed values were not
  inspected. Independent cross-lab review remains open.

### 2026-10-23 — CRL peer activation regression check

- Re-ran `python3 -m unittest deploy.crl.test_activate_crl -v`; all 12
  activation tests passed, including verified HTTPS, issuer/signature and
  freshness rejection, rollback protection, atomic install, service-restart
  recovery, and idempotent reconciliation after interruption.
- The activation code remains a local candidate. **UNVERIFIED:** real CRL
  publication endpoint, missing/expired-CRL fleet policy, central peer
  acknowledgements, deployment configuration, and independent cross-lab review.
- Rechecked the scanner-ingest process after the LAN-use request: the host
  listener is `0.0.0.0:8765`, and `http://10.0.0.20:8765/` returns HTTP 200.
  The earlier loopback-only note records the prior state; this later check is
  authoritative. Authentication, TLS, and iPhone reachability remain
  **UNVERIFIED**.

### 2026-10-23 — F-01 static CRL publisher candidate

- Added `deploy/crl/publish_crl.py`: it verifies an exact single PEM CRL, its
  issuer, signature, number, and bounded freshness; publishes under a
  fingerprint-derived filename; prevents number rollback; and uses an atomic,
  fsynced replacement. The separate unprivileged system account has read access to the
  generated CRL and public CA certificate but no CA-key or DB credential
  access. A one-minute timer checks for generator updates, and a sample Apache
  HTTPS-vhost include serves the static files without directory listing or
  dotfile access.
- Added six publisher tests for normal publication, rollback prevention,
  expired-output recovery, peer-aligned time bounds, and explicit key/credential
  masking. Wired them, the publisher sysuser
  declaration, and all CRL systemd units,
  into both GitHub and Forgejo CI definitions. Local publisher tests passed 6/6;
  `sudo systemd-analyze verify` passed for the generator, publisher, and
  activation service/timer definitions.
- **UNVERIFIED:** no TLS vhost or endpoint was installed, no remote peer fetched
  this URL, no peer activation was deployed, and cross-lab review remains open.
  F-01 remains a release blocker because missing-CRL policy and fleet
  acknowledgement/convergence are not complete.

### 2026-10-23 — live SoR grant and replica audit

- Read-only connection through Xenon's deployed SoR-sync configuration
  confirmed Cesium MariaDB 12.3.2 is writable and currently has 45 base tables
  and four views. The service identity `solari` has no global privileges, but
  its `sor` schema grant includes `ALTER`, `CREATE`, `DROP`, `CREATE ROUTINE`,
  `CREATE VIEW`, and `TRIGGER` in addition to data privileges. The deployment
  uses this account for SoR synchronization; these DDL privileges exceed the
  observed runtime role and violate the planned least-privilege boundary.
- The Benzene `sor-replica` MariaDB 11.8.8 container is read-only. A direct
  `SHOW SLAVE STATUS` check reports both replication threads running, zero lag,
  and zero I/O/SQL errors. A recent container log has one binlog-read retry;
  the current replication status is healthy.
- No application rows were dumped and no grants, schema, replica, or service
  state were changed. **OPEN:** split migration/administration privileges from
  runtime identities; map each SoR worker's actual SQL needs; validate the
  exact grants against disposable MariaDB 11.8 and 12.3 schemas; then plan a
  reviewed live grant transition with service-account rollback. Until that
  work closes, treat SoR runtime credential compromise as DDL-capable and keep
  production SoR schema adoption blocked.

### 2026-10-23 — cross-lab PKI and CRL hardening

- A fresh read-only Claude review found no High findings and two Medium
  findings: generated CRLs lacked the RFC 5280 Authority Key Identifier, and
  the trust publisher did not require an independently verified expected root
  fingerprint. Lower findings covered unspecified reason codes, CRL mode under
  restrictive umask, stale migration-runbook counts, missing rollback/mode and
  CRL monotonic-guard tests, plus an exact-artifact transfer caveat.
- The CRL generator now emits AKI from the CA's subject key identifier and
  rejects a candidate if that identifier is absent or mismatched; an
  `unspecified` revocation omits the reason-code extension; generated public
  CRLs are mode 0644. New tests cover AKI, restrictive umask, unspecified
  reason handling, number rollback, ledger drift during generation, and a
  failed state commit after file replacement. The generator suite passes
  9/9 and all CRL tests pass 33/33.
- The trust publisher now requires `--expect-sha256`, validates it before any
  filesystem change, and compares it with the generated root's fingerprint. A
  separate `--activate-release` path verifies a retained release by exact file
  set, ownership, modes, checksums, release-name fingerprint, and independent
  root pin without rebuilding artifacts from possibly changed templates.
  The README pins the currently recorded Akoria root and explains transfer of
  only the five expected artifacts. Tests cover mismatch/malformed pins,
  colon-delimited fingerprints, rollback across builder-template changes,
  retained-release integrity and modes, and bad directory modes. PKI tests pass
  35/35; CRL tests pass 33/33 with an emitted-CRL check for omitted reasonCode.
- The migration runbook now distinguishes historical SoR counts from the
  current candidate (12 source files through 028; 46 tables, four views, 47
  triggers), and lists the current monitoring set as 36 migration files through
  035 while preserving the historical 34-file scratch result through 033. Its
  paired Letter PDF is six pages with the final-page footer checked visually. The browser fixture's SQL string encoder now uses UTF-8
  hex literals instead of quote-only escaping.
- **UNVERIFIED:** a cross-lab review covered the prior PKI/CRL fixes but did not execute tests. The retained-release activation addition and emitted-CRL reason assertion have not yet had a follow-up review. Hosted CI, portal HTTPS publication and device/browser trust remain release
  gates. The public CA fingerprint and SKI were rechecked over verified SSH;
  no CA key or live trust configuration was changed. Nothing was committed, pushed,
  deployed, or written to a live database.

### 2026-10-23 — SoR runtime privilege split

- Static SQL review enumerated the SoR operations in the DNS renderer/appliers,
  outbox emitter, adopted-discovery reconciler, and UniFi snapshot importer.
  Added four separate runtime sections: read-only DNS (`[sor]`), outbox checkpoint
  (`[sor_emit]`), adopted discovery inserts (`[sor_discovery]`), and UniFi-owned
  network facts (`[sor_unifi]`). Missing sections retain the old `[sor]` fallback
  for staged config rollout; the candidate grants use newly provisioned accounts
  and leave the existing `solari` account untouched.
- `deploy/sorsync/sor-runtime-grants.sql` scopes grants to exact tables and views.
  It gives the emitter SELECT plus checkpoint INSERT/UPDATE; discovery SELECT on
  source/IP ownership and INSERT on entities/IPs; the UniFi importer SELECT and
  owned table INSERT/UPDATE; and DNS rendering only the zone/view reads. None of
  the four identities receives DDL, DELETE, GRANT OPTION, or global rights.
- Added a disposable MariaDB integration test that creates ephemeral worker
  identities, applies the grants to a sentinel-marked scratch schema, exercises
  permitted queries and writes, verifies excess DML/DDL denial, then invokes the
  actual UniFi importer twice using `[sor_unifi]` and rolls back/removes all
  fixtures. `netdb/sor/test_allocator_db.sh` passed end-to-end: the role test,
  provisioning CDC, UniFi import (10/10), allocator API (12/12), RackWire DB
  commit (15/15), and all allocator concurrency scenarios. Its EXIT cleanup
  dropped the scratch schema and temporary users.
- SoR sync unit suite: 51 tests passed, 6 integration cases skipped because no
  isolated broker/database was configured for that invocation. Forgejo workflow
  parity, Python compilation, and `git diff --check` pass.
- **OPEN:** the live `solari@10.0.0.0/255.0.0.0` account remains unchanged and
  DDL-capable. Provision the four accounts with unique secrets, stage the
  per-host configs, rehearse worker cutover/rollback, then remove legacy grants
  only after inventorying every deployed consumer. Independent cross-lab review
  and hosted CI remain release gates; this candidate has not been committed,
  pushed, or deployed.

### 2026-10-23 — SoR privilege split gate and repository status

- Re-ran the exact scratch SoR integration after the four-identity config became
  section-atomic and the grant test began checking `SHOW GRANTS`: all runtime
  reads/writes passed, global rights and extra DML/DDL remained denied, and the
  actual UniFi import with DHCP ran twice. The full scratch harness passed
  (UniFi 10/10, allocator API 12/12, RackWire DB 15/15, all concurrency
  proofs), then dropped `sor_test` and all generated users; a metadata recheck
  found zero remaining schema/users.
- Final local SoR sync suite: 51 passed, 6 skipped for separately gated RabbitMQ
  and live-schema cases. Python compilation, Forgejo workflow parity, and
  `git diff --check` pass.
- The authenticated GitHub PR for
  `integrate/phase0-reconcile-20261022` is #38, open with no reviews. Its 17
  reported checks passed for pushed commit `d93b40e`; that run predates the
  current dirty local tree and does not exercise the new privilege split. Both
  remote candidate refs still point to `d93b40e` and both `main` refs to
  `5e8d688`; 143 paths are locally modified/untracked. No commit or push was
  made.
- `claude auth status` currently reports Claude Code authenticated. The project
  model-routing skill still marks non-interactive Fable invocation and harness
  reachability as pending; no command or route is documented, so no reviewer
  invocation was guessed. **OPEN:** independent Fable review of these changes,
  current-candidate hosted CI, live account/config cutover and rollback, the
  remaining platform release gates, and deployment.
