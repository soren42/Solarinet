# SolariNet — Overnight Build Plan (six goals)

*Autonomous multi-turn effort. Foundation first, then concrete fixes, then scaffold the hardware-dependent pieces. Everything documented, committed, pushed.*

## Architecture spine
The **System of Record (SoR, MariaDB)** is the hub; the **message queue (RabbitMQ)** is the nervous system. Every other goal plugs into these two:

```
   dashboard ⇄ SoR (MariaDB, cesium primary / benzene replica) ⇄ generators (DNS/AD/config)
                     ▲                          │
     monitoring/discovery ──detect──▶ MQ (RabbitMQ, benzene) ──act──▶ network + servers
                                          │
                        notifications (SMS via Tachyon) · NFC 2FA · Tab5
```
Rule (from the user): **data is never entered manually** — the SoR is seeded from today's live systems of record, and thereafter flows bidirectionally (dashboard edit → SoR → network change; detection → SoR update).

## The six goals & approach

**1 · System of Record (MariaDB).** Fully-normalized inventory/config/state DB on **cesium** (primary), replicated to **benzene** (MariaDB already there). Tables for entities (server/app/service/device supertype), hardware, OS, locations, interfaces, interconnects (physical+logical), networks/VLANs/IPAM, DNS, users, certificates, keys, tokens, monitoring-state, config, relationships, and full audit/history. Seeded from: `netdb` SoT, Samba AD, BIND zones, Forgejo, the SolariNet monitoring DB, UniFi, Pi-holes. The netdb generator's `load_source()` re-points here — DNS becomes a *view* of the SoR. Bidirectional sync via MQ triggers.

**2 · Message queue (RabbitMQ).** On **benzene** now, **portable** (containerized) so the primary can move to a dedicated box tomorrow with benzene as backup. The middleware decoupling logical architecture from physical infrastructure — eases migration + failover/DR. Topic exchanges: `sor.*` (record changes), `detect.*` (monitoring/discovery), `act.*` (apply), `notify.*`.

**3 · Notifications.** SMS/push, initially through **tachyon.akoria.net** (Particle Tachyon, cellular modem). Consumes `notify.*` from MQ. Fault-tolerant cellular context: `bunsen` 10.0.0.9 (UMR Ultra, AT&T, laboratory port 6) and USG Backup 10.3.199.1 (T-Mobile, family room).

**4 · Dashboard.** Full inventory of screens/views; **fix Reachability** (hang/blank — root-cause + resolve); **augment Discovery** with mDNS/avahi names + open-port service inspection; strengthen cross-view entity linkage; deepen config/settings to cover today's new services (AD, Keycloak, BIND/SoR, MQ). Taste-critical → opus/fable.

**5 · NFC 2FA.** Reader built into **xenon** + USB reader on **hydrogen**. A second factor bound to the dashboard/Keycloak auth.

**6 · Tab5 (M5Stack).** TOTP generator + password-manager client (assumes always-on home wifi) + optional SolariNet notification surface. Embedded firmware — design + scaffold; flashing needs the device.

## Sequencing (dependency-ordered)
1. MariaDB up (cesium) → schema → seed → replica (benzene). *[foundation]*
2. RabbitMQ up (benzene, portable). *[foundation]*
3. SoR⇄DNS/AD reconcilers move onto MQ; dashboard reads SoR.
4. Dashboard: Reachability fix + Discovery augmentation + new-infra views.
5. Notifications service (MQ consumer → Tachyon SMS).
6. NFC 2FA + Tab5 (scaffold/design; hardware-gated).

## Model allocation (per CLAUDE.md)
- **fable-5**: SoR schema design (foundational, best-practices).
- **opus-4.8**: dashboard UX/build, Reachability/Discovery (taste ≥7).
- **gpt-5.5 (codex)**: DDL implementation, seed/ETL scripts, MQ config, mechanical glue.
- **me**: live infra (DB/MQ stand-up), hardware investigation (Tachyon/NFC/Tab5), integration + review + commits.

## Progress log
- **Goal 2 (MQ): DONE** — RabbitMQ live on benzene (podman, boot-persistent, mgmt :15672, exchanges sor/detect/act/notify.events).
- **Goal 3 (notify): service LIVE** — `deploy/notify/` MQ-driven dispatcher, log sender smoke-tested end-to-end, running under systemd on xenon. SMS-via-Tachyon stubbed (Tachyon 10.6.6.10 unreachable tonight).
- **Goal 4 (dashboard): Reachability FIXED** — root-caused (missing `monitorName` → `undefined.localeCompare()` blanked the route), fixed additively + deployed. Full audit in `docs/SolariNet_Dashboard_Audit.md`; infra-insight build agent in progress.
- **Goal 1 (SoR): schema LIVE** — `netdb/sor/schema.sql` (30 tables + 3 views, provenance/audit, idempotent) applied to `sor` on cesium; population agent seeding from netdb/AD/Forgejo + wiring the generator's `load_source()` to render DNS FROM the SoR.
- In flight: SoR population, dashboard infra views, NFC 2FA design, Tab5 firmware scaffold. Pending: benzene SoR replica (after seed), Tachyon SMS (hardware).
