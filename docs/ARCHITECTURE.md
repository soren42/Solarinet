# SolariNet — Architecture &amp; Design Rationale

*A rationale document: **why** SolariNet is built the way it is. It records the reasoning behind the load-bearing
decisions — not a history of rejected alternatives. For current operational state see `SolariNet_RC_Handoff.md`; for
the health-monitoring contract see `docs/design/HOST_HEALTH_CONTRACT.md`.*

Standard C control plane + PHP/JSX dashboard, per `CLAUDE.md`. Last updated 2026-07-09.

---

## What SolariNet is

A self-hosted monitoring and control plane for a heterogeneous home lab (~25–300 element-named hosts, from Dell
servers to Raspberry Pis to SBCs and appliances). It is a **three-tier, coordinator-free** system: lightweight
**clients** report host health, **monitors** probe targets (including app-layer health), and a **server** ingests,
records to a normalized System of Record, alerts, and drives the dashboard. Everything downstream — DNS, alerts,
notifications — is rendered from the same source of truth.

The design goals, in priority order, are: **honesty** (a green light must mean actually-healthy, not
socket-open), **resilience** (no single coordinator whose loss stops the fleet), **frugality** (runs on SBCs, sips
resources), and **portability** (one small C client across every CPU/libc in the lab).

---

## The control protocol (SCP) — length-prefixed binary TLV over nng

**Decision:** the SolariNet Control Protocol is a **length-prefixed, binary, type-length-value (TLV)** wire format
carried over **nng** (nanomsg-next-gen) sockets. Ports: **7701** ingest (client→server reports), **7702** survey
(request/reply control), **7703** fleet PUB (server→fleet broadcast of config/adopt).

**Why length-prefixed binary TLV, not JSON/HTTP:**
- **Framing is unambiguous.** A length prefix means the receiver always knows exactly how many bytes a message is,
  so partial reads and message boundaries are trivial — no delimiter escaping, no "read until newline" ambiguity on
  binary payloads.
- **TLV is forward/backward compatible.** New fields are new type tags; an old peer skips a tag it doesn't know by
  its length. The fleet is deliberately heterogeneous and upgraded piecemeal, so wire-format evolution without
  lockstep upgrades is a requirement, not a nicety.
- **It's cheap on an SBC.** No JSON parser, no reflection, no allocation storms — a client on a NanoPi or a Pi Zero
  encodes a report in a fixed buffer. Frugality is a first-class goal.
- **nng gives us the messaging patterns for free.** Survey/respond, pub/sub, and req/rep are native nng patterns
  that map directly onto the three tiers (broadcast config to the fleet, survey targets, ingest reports) without
  hand-rolling a socket layer.

**Why over TLS (mbedTLS):** every node carries a per-node X.509 client certificate (CN `client.<fqdn>`), issued by
signing a CSR against the internal CA — **the CA private key never leaves the server/CA process.** mTLS gives us
mutual authentication and encryption on the control plane with a library small enough to link statically into the
SBC client. (Note: older mbedTLS does not match IP SANs, so nodes dial the server by a **name** pinned in their
hosts file — this is why the deploy tooling pins `xenon → <ip>`.)

---

## Coordinator-free architecture — HRW (rendezvous) hashing

**Decision:** which monitor owns which target is decided by **Highest-Random-Weight (rendezvous) hashing**, computed
independently and identically by every node. There is **no elected coordinator, no consensus protocol, no leader.**

**Why:**
- **The failure mode of a coordinator is the whole system.** An elected-leader design (Raft/Paxos/a "primary
  monitor") means leader loss triggers an election, and election bugs are the classic distributed-systems footgun.
  For a home lab that must keep watching itself *especially* when things are breaking, the coordinator is exactly the
  wrong single point to introduce.
- **HRW makes ownership a pure function.** Each node hashes `(target, monitorId)` for every candidate monitor and the
  highest score wins. Every node computes the same owner from the same membership list — no coordination messages, no
  shared mutable state to keep consistent.
- **Rebalancing is minimal and local.** When a monitor joins or leaves, HRW only reassigns the targets that node
  owned (or would own) — every other assignment is stable. Compare consistent hashing's ring: HRW needs no virtual
  nodes to balance well and reassigns strictly less on membership change.
- **It degrades gracefully.** A monitor dropping doesn't stop the fleet; its targets simply re-hash to the next-best
  owner on the next cycle. "Monitor redundancy" is k-of-n by construction: pick the top-k HRW scorers per target.

**Current honest state:** the fleet runs today as a *fleet-of-one* server (all `node` rows are `client`-role; there
are no `monitor`-role rows yet), so HRW owner-select resolves trivially and targets are dispatched by broadcast. This
is a documented **contract gap** — the fleet enumerator that turns HRW into true k-of-n redundancy is the next piece
to close before the monitor tier grows past one.

---

## Three-tier topology &amp; monitor redundancy

**Decision:** three roles — **client** (host agent: reports metrics + host-health every cycle), **monitor** (probes
targets it owns, including app-layer checks), **server** (ingest, SoR, alerting, dashboard/API, CA). A node can hold
more than one role.

**Why three tiers, not two:** separating *reporting* (client) from *probing* (monitor) lets us (a) run a
featherweight client everywhere — even on boxes that can't or shouldn't probe others — and (b) place probing power
where it has network vantage. A monitor on a far segment sees that segment's targets directly; the client on a Pi
Zero just reports itself. The server stays the one place that holds truth and issues certificates.

**Monitor redundancy:** targets are owned by the top-k HRW monitors, so a monitor loss re-homes its targets without
gaps or double-alerting. The **dead-man's-switch** is the backstop even for the server's own liveness: a node silent
for >3× its sample interval has a synthetic `crit` emitted *on its behalf* — the system monitors the monitor, so a
dead watcher is itself an alert rather than a silent blind spot.

---

## Technology choices &amp; rationale

| Choice | Why |
|---|---|
| **C for the control plane** | Runs on every CPU/libc in the lab with a tiny footprint; links mbedTLS/nng/sqlite statically for dependency-free deploys; forces an explicit, auditable wire format. (Per `CLAUDE.md`, the control plane is strictly C.) |
| **nng** | Native survey/pub/rep patterns matching the tiers; small, portable, no broker required for the control plane. |
| **mbedTLS** | Small enough to static-link into an SBC client; gives mTLS + a workable embedded CA/CSR flow. |
| **SQLite on nodes** | Local, zero-config, crash-safe buffering of samples on the client with no server round-trip per metric. |
| **MariaDB for the SoR** | The System of Record is relational truth (entities, IPs, DNS, hardware, inventory) that many consumers read; a real RDBMS with replication is the right tool. Replica on benzene gives a promotable copy. |
| **RabbitMQ (+ Mosquitto)** | The change-data-capture bus: SoR writes emit to an outbox → `sor.events` → fan-out to DNS/AD/Pi-hole appliers and notifications. AMQP for services, MQTT for lightweight/IoT. |
| **PHP-on-Apache dashboard** | Deliberately boring and ubiquitous; a thin `Router/Db/Response` API over the two databases. The SPA is React/JSX transpiled in-browser (Babel) — **no build step**, so the dashboard is editable and deployable by copying files, which suits a solo operator. |
| **Keycloak (OIDC) + local login** | Standards-based SSO federated with AD; the dashboard is an OIDC client, so family/operator identity lives in one place. |

---

## The System of Record and the data flow

The **SoR** (MariaDB `sor` on cesium) is the normalized truth: entities, IP addresses, DNS records/zones, hardware
units/models, locations, users, inventory, projects. **Everything downstream renders from it.** DNS zones
(`akoria.net` on xenon BIND, `akoria.org` on radium AD, Pi-hole forwarders) are *generated* from the SoR, byte-identical
to the legacy YAML source it replaced.

**Why CDC-over-a-bus, not triggers or polling:** a write to the SoR appends to a **change-data-capture outbox**; a
dispatcher publishes it to `sor.events`; independent appliers (BIND, AD, Pi-hole) consume and reconcile. This
decouples the writer from every consumer, survives a consumer being down (the event waits), and makes adding a new
render target (config files, a future cache) a matter of adding a subscriber — not editing the write path. The sync is
**bidirectional**: operator edits in the dashboard write the SoR → render out; discovered-and-adopted hosts flow into
the SoR → render out. Verified end-to-end both directions.

---

## Health philosophy — why "socket-open" is a lie

The design's sharpest lesson has a date: **2026-07-06/07**, when a USB disk backing Forgejo on cesium dropped off the
bus, btrfs went emergency-read-only, and **every Forgejo request 500'd for ~26 hours while the monitor stayed green** —
because the `:3000` check was a bare TCP connect, and socket-open is not the same as healthy.

That incident is the direct rationale for four capabilities, and for a principle: **a health signal must reflect the
thing users actually experience.**
- **Host-health monitoring** — the client reports fs-readonly, block-device-missing, SMART-fail, failed systemd units,
  and dmesg-crit every cycle. A host in trouble says so directly, even if its services still accept connections.
- **HTTP status-aware probes** — an HTTP target is checked for an *expected status* (exact code or `Nxx` class), not a
  bare connect. `80/443/3000/8080/9000` default to real HTTP checks; Keycloak's `9000→/health/ready|200`.
- **Alert → MQ → Apple bridge** — `alertEvent` → `notify.events` → iMessage, checkpointed and idempotent, so a real
  fault reaches the operator's phone.
- **Dead-man's-switch** — a silent node emits its own `crit`; the watcher is watched.
- **Nightly backups** — the SoR (and Forgejo) dump to a separate spindle, 14-day retention, because "it was never at
  risk" should be a fact you can prove, not a hope.

---

## Phase structure &amp; current completion state

SolariNet reached a coherent **first release candidate**: a normalized SoR (mirrored, DNS rendered from it), a message
bus, notifications, and a dashboard/monitor that genuinely probes the whole lab including app-layer health. Live
today: SoR + real-time replica; DNS-from-SoR; RabbitMQ + Mosquitto; `notifyd`; the Apache/OIDC dashboard; the
`server/monitor/client` fleet (~49 targets); app-layer LDAP/MySQL/AMQP/HTTP checks; bidirectional SoR sync (3 units);
host-health + HTTP-status probes + alert-bridge + dead-man's-switch + nightly backups; a Tab5 authenticator
(software-complete). Recent work (this workstream): fleet-wide client onboarding with a hardened adaptive deploy,
inventory + maintenance-window + discovery-enrichment + Git + Certificates dashboard views, and an HA/DR + DNS
consolidation plan.

---

## Known issues &amp; pending capabilities

Honest and prioritized:
1. **Monitor-role registry / true k-of-n HRW.** No `monitor`-role rows yet (fleet-of-one); HRW owner-select is a
   documented contract gap. Build the fleet enumerator before the monitor tier grows. Adopted targets are runtime
   state — a from-scratch rebuild needs re-adopt (argues for persisting adoptions).
2. **Decommission / lifecycle support.** Clean node retirement (cert revocation, SoR soft-delete, target reassignment)
   exists partially (`solari-decommission.sh`) but isn't a first-class, dashboard-driven lifecycle yet.
3. **Remote provisioning.** OS install/enroll (PXE on benzene, `deploy/fleet/`) is staged but not a one-button flow;
   the adaptive client deploy is solid but the *bare-metal → enrolled* path still has manual steps.
4. **Dual-hierarchy topology visualization.** The dashboard shows network + monitoring topology, but the
   *network* hierarchy (physical/L2 via LLDP) and the *monitoring* hierarchy (who-probes-whom) are not yet a single
   reconciled dual-view.
5. **Network segments as a first-class concept.** Segmentation is inferred; making segments explicit in the SoR would
   sharpen discovery scoping, probe placement, and topology.
6. **C2-based config.** Node configuration is pushed via `CTRL_*` broadcasts today; a durable, versioned
   config-as-a-render-target (config epochs reconciled like DNS) would make configuration auditable and replayable.
7. **Portability tail.** Immutable/appliance hosts (ZimaOS) need a static-musl client (no runtime lib deps); the
   `cross-build` matrix covers x86_64/arm64/arm32 but the static tier is unfinished.
8. **Housekeeping.** Revoke transient seed tokens; re-tighten `named` AppArmor on radium; monotonic zone serials for
   secondary AXFR; outbox pruning.
