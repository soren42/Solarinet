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
| **F-01** | Node cert not bound to frame `sourceNodeId`; gossip accepts unbound IDs | **Medium.** Requires an *already-enrolled* compromised client. Trusted LAN, ~9 hosts → lower than the exposed dashboard, but it is the identity foundation. | **Adopt, sequence after F-02/03.** Canonical identity contract; persist cert fingerprint; reject HELLO/gossip mismatch; negative tests. | Stability + **I1** / Ph 2 |
| **F-04** | PHP + server share the `jason` account; API served from writable checkout | **Medium-High.** "PHP holds no CA material" is convention, not a boundary; source-write = code-exec. | **Adopt.** Split least-priv accounts; immutable artifacts outside the tree; `SO_PEERCRED`; canonical hardened units. | Stability / Ph 2 |
| **F-06** | Tab5 approval display not cryptographically bound to the signed decision | Only if enabled. | **Contain now** (keep non-production), fix if/when promoted. | Contain / Ph 0 |
| **F-07** | PXE executes unauthenticated artifacts as root | Only when provisioning runs. | **Contain now** (gate behind an explicit, isolated run), signed-manifest fix later. | Contain / Ph 0, Ph 3 fix |

### P1 — required for a trustworthy stabilization release

| ID | Finding | Disposition | Track / Phase |
|---|---|---|---|
| **F-08** | SoR CDC not actually at-least-once (no publisher confirms; consumers ack before apply; no reconcile) | **Adopt.** Confirms+mandatory before checkpoint; ack after all targets; periodic full reconcile; fault-injection tests. | Stability + **I2** / Ph 2 |
| **F-10** | Health/convergence records can lie (double-write; epoch advanced before durable; missing error = success) | **Adopt.** Persist reports once; save/fsync/activate before advancing epoch; correlate directives. | Stability + **I3** / Ph 2 |
| **F-12** | Schema lifecycle not reproducible (no ledger; duplicate ordinal 016; `--init-db` applies a stale hybrid) | **Adopt.** One migrator per DB, immutable IDs + checksums + version ledger; generate bootstrap from applied set. | Stability + **I2** / Ph 2 |
| **F-13** | Service worker serves cached authenticated GETs; `loadLive()` labels them "live"; SSE holds FPM workers | **Adopt — this is the hinge.** Cache shell only; partition offline snapshots by identity, clear at logout, show age, fail-visibly-stale; move SSE off FPM. | **SHARED: Stability + I3** / Ph 1 |
| **F-14** | OIDC accepts tokens w/o `exp`; break-glass local login always callable + unrate-limited; roles never revalidated | **Adopt.** Require standard claims + bounded state age; independently disable break-glass; rate-limit; idle/absolute expiry; revalidate role. | Stability / Ph 1 |
| **F-09** | Server HA internally inconsistent (standby writes; lease epoch churn; bind-fail keeps renewing) | **Adapt.** Real defect, but **production is fleet-of-one** — no second server exists to conflict. Fix the state machine as designed; **prove it in Phase 3/4, don't let it block Ph 1–2.** | Stability / Ph 3 |
| **F-15** | Opie "read-only" not confidentiality-safe (alert text → prompt; generic Read/Grep tools; runs as `jason`) | **Contain now** (dedicated sandboxed account, drop generic file tools, inventory allowlist), then harden. | Contain / Ph 0, Ph 3 harden |
| **F-16** | Fleet provisioning leaves server-minted key material staged; firstboot never enrolls | **Adopt.** Node-generated keys + one-time CSR token; atomic cleanup; prove PXE→first-report in a VM. | Stability / Ph 3 |
| **F-17** | DNS/AD apply lacks safe deploy semantics (direct zone overwrite; no `named-checkzone`; creds in argv) | **Adopt.** Render+validate+atomic-switch+health-check+rollback; durable serial state; protected AD credential channel. | Stability + **I2** / Ph 2–3 |
| **F-18** | Arbitrary MCP SQL guarded only by a token regex | **Contain now:** replace arbitrary SQL with fixed tools/views + dedicated read-only account. | Contain / Ph 0 |

### P2 — before scale/HA/maintainability claims (Phase 3–4)

`F-19` monitor probing sequential/capped · `F-20` HRW membership unproven (narrow the
"coordinator-free" claim) · `F-21` spool no quota/age, seq reuse · `F-22` SoR multi-step
writes untransacted · `F-23` decommission sends before durable confirm · `F-24` Tab5
secret-at-rest unproven · `F-25` UniFi false-green (**Fold into I3** — same freshness
contract) · `F-26` authbroker fails open on empty token · `F-27` backups nonempty ≠
restorable · `F-28` sparse referential integrity/orphans · `F-29` mTLS configurable not
invariant.

**Disposition:** Adopt as a batch in Phases 3–4. `F-25` is pulled forward into the
Phase-1 freshness work (I3) because it is the same primitive. `F-20` is a
*documentation* fix too — narrow the "coordinator-free" language now; it costs
nothing and stops overclaiming.

### P3 — hardening/debt (ongoing)

`F-30` warning-clean CI + sanitizers + fuzzing + strict envelope validation ·
`F-31` CSP/HSTS/headers under the no-build constraint. **Adopt into the CI gate (§5)**
and address opportunistically; not gating.

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
3. Make **every human mutation operator/admin-only** and add CSRF + Origin checks
   (F-02, F-03) — this single change also neutralizes the *trigger* for F-05.
4. Stop API service-worker caching; move or disable SSE if worker starvation is
   observable (F-13, first half).
5. Restrict dashboard exposure + rate-limit/disable break-glass local login (F-14
   partial), while the boundary is rebuilt.
6. **Integration I0:** stamp each capability with a maturity label
   (prototype / lab / production / authoritative) in one manifest, and surface it in
   the UI so a lab-only feature *reads* as lab-only.

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
6. **F-13 → I3 (the hinge):** cache only the app shell; partition any offline
   snapshot by identity + clear at logout + show age + fail-visibly-stale;
   `loadLive()` never labels cached data "live." Ship the **freshness contract
   primitive** here: `{lastSuccess, ageSeconds, state: fresh|stale|unknown}` as a
   reusable envelope field and a shared UI badge component.

**Exit gate:** negative auth suite proves wrong-user/role/origin/stale-token cannot
act; framed-I/O fuzz passes at every byte boundary; no code path presents cached
data as live; the freshness badge renders on at least the alerts and nodes pages.

### Phase 2 — Durable state and reproducible schema

1. **F-12 → I2:** one migrator per DB (control-plane + SoR) with immutable IDs,
   checksums, a version ledger, and honest failure reporting; resolve the duplicate
   ordinal 016; generate the bootstrap snapshot from the applied set.
2. **F-08 → I2:** publisher confirms + mandatory routing before checkpoint; consumers
   ack only after all targets apply; periodic authoritative full reconcile;
   broker-loss / partial-apply / duplicate-delivery fault tests.
3. **F-10 → I3:** persist monitor reports exactly once; save/fsync/atomically
   activate config *before* advancing the applied epoch; persist directives keyed by
   node+correlation+verb+epoch and consume one matching pending directive.
4. **F-01 → I1:** canonical identity contract enforced on every ingress
   (cert SAN/CN ↔ node ID ↔ role ↔ FQDN); persist cert fingerprint; reject
   HELLO/gossip mismatch; server-signed monitor-membership epoch; negative tests.
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
4. **F-17 (operational) + F-27 + F-23 + F-26 + F-21:** DNS rollback drill; scheduled
   disposable restore drill; durable decommission intent; authbroker fail-closed +
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
  (F-08, F-12) is the single backbone every page hangs off. Provenance
  (observed/adopted/authoritative) becomes visible, not just modeled.
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

**Sequencing note [Judgment]:** I4 is the most visibly "integration" deliverable and
the one you'll *feel*, but it is deliberately last — cross-linking pages before the
entity spine (I2) and freshness contract (I3) exist would just wire together data that
still lies. Integration built on dishonest state is worse than no integration.

---

## 6. CI and review gates to install alongside (review §9)

Current CI is a compile gate, not a release gate. Add, roughly in Phase order:

- `SOLARI_TEST_DB=1` against the scratch MariaDB; **fail if the live-DB suite skips.**
- TLS loopback required; certificate-identity failure tests.
- `git submodule update --init --recursive` before dashboard tests (RackWire is now a
  submodule); run **every** SolariNet dashboard test, not just the bridge subset. RackWire's
  *own* UI tests live in the dependency and are its CI's job; SolariNet keeps the **boundary**
  tests (`test_rackwire_api.php` against the route).
- Locked Python deps; run MCP, alertbridge, UniFi, authbroker, sorsync, backup,
  provisioning, notification tests (they exist — the defect is CI not running them,
  per review §10).
- Generated route × method × role matrix (I1); CSRF/Origin tests.
- shellcheck, C sanitizers (ASAN/UBSAN) + fuzzers (F-30), migration
  clean-install/upgrade/idempotent-rerun checks, DNS validators.
- Fault injection in CI: RabbitMQ loss, two-server lease race, partial Unix-socket
  I/O, concurrent config writes, backup restore, service-worker offline.
- Pin actions/runners/toolchains/deps enough to reproduce release artifacts.

**Cross-lab gate (AGENTS.md §4.4):** the review was single-lab by its own admission.
Before committing to the deep architectural choices (identity contract shape, HA
state machine), run one genuine cross-lab confirmation pass. Containment (Phase 0)
proceeds without waiting on it.

---

## 7. Sequencing summary

| When | Do | Why first |
|---|---|---|
| **Now (Ph 0)** | Contain F-06/07/15/18; operator-gate + CSRF (F-02/03); stop SW caching; maturity labels | Cheap, turns off live exposure, removes the F-05 trigger, no redesign |
| **Next (Ph 1)** | F-02/03/11/14 + F-13 freshness primitive (I1 seed, I3 hinge) | Honest edge + honest browser; unblocks the integration contract |
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
been changed. Recommend a cross-lab confirmation of the Phase-2 architectural
commitments before building on them.*
