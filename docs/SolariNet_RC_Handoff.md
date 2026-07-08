# SolariNet — First Release Candidate (handoff)

*2026-07-06. Standard-C control plane + PHP dashboard, per `CLAUDE.md`. Repo: github.com/soren42/Solarinet. This is a snapshot for handing the workstream off (agents moving to QuakeKit); it records what's live, what's committed, and what's next.*

## One-line status
A coherent first RC: a normalized **System of Record** (mirrored, DNS renders from it), a **message bus**, **notifications**, and a **dashboard/monitor** that genuinely probes the whole homelab — including app-layer health of the new infra.

## Live subsystems
| Subsystem | State | Where |
|---|---|---|
| **System of Record** (MariaDB `sor`) | Live — 30 tables, surrogate PKs, populated from netdb/AD/Forgejo | cesium `10.1.0.200:3306` |
| **SoR replica** | Live — real-time, write-tested, 48/48 parity | benzene container `:3307` |
| **DNS from SoR** | Live — `gen-zones.py --source sor`, byte-identical to YAML | xenon BIND |
| **Message queue** | Live — RabbitMQ (`:5672`/`:15672`) + Mosquitto (`:1883`/`:9001`) | benzene |
| **Notifications** | Live — `notifyd` → {log, MQTT}; SMS gated on Tachyon | xenon (`deploy/notify/`) |
| **Dashboard** | Live — Apache `:9443`, local + Keycloak OIDC login | xenon `/var/www/solarinet` + repo API |
| **Monitor fleet** | Live — 49 targets, ~45 ok; xenon + benzene outposts | `solarinet-{server,monitor,client}` |
| **App-layer health** | Live — LDAP/MySQL/AMQP checks on AD/SoR/RabbitMQ | monitor `probeType` |
| **SoR bidirectional sync** | Live — CDC outbox → `sor.events` → DNS; discovery → SoR | `deploy/sorsync/` (3 units on xenon) |
| **Tab5 Authenticator** | Software-complete (flashing pending) | `firmware/tab5/` + `deploy/authbroker/` |
| **Host-health monitoring** | Live — `solariHostHealth` (fs-readonly/blockdev/SMART/failed-units/dmesg-crit) collected every report cycle, ingested + alert-ruled | contract `docs/design/HOST_HEALTH_CONTRACT.md` §1–§4; client PAL + `serverIngest.c`/`serverAlert.c` |
| **HTTP status-aware probe** | Live — `appCheckHttp()` takes `path\|status` (exact code or `Nxx` class); 80/443/3000/8080/9000 default to `http` | `src/monitor/probeNet.c`, contract §5 |
| **Alert → MQ → Apple bridge** | Live — polls `alertEvent`, publishes `notify.events`, checkpointed/idempotent | `deploy/alertbridge/` (systemd, xenon), contract §6 |
| **Dead-man's-switch** | Live — silent node (>3× sampleInterval) emits a synthetic `crit` itself; "monitor the monitor" | `deploy/alertbridge/`, contract §6 |
| **Nightly backups** | Live — `solari-backup.sh` + systemd timer; cesium dumps `sor` (+ Forgejo once relocated) to a separate spindle (`sdb1`); xenon dumps `solarinet`; 14-day retention | `deploy/backups/`, contract §7 |

## This session's commits (RC push)
- `2bbaa22` SoR migration 001 — surrogate PK on every table, name-uniqueness relaxed to indexes (collisions are the dashboard's job).
- `eef3a25` App-layer health probes in the C monitor (DNS/LDAP/MySQL/AMQP/HTTP) + `mdnsName` on discovered; migrations 008/009.
- `355368c` + `12cd34c` **The activation fix**: asset-adopted targets now pick an app-layer `probeType` by port *and* are broadcast to the fleet (`serverAssetsAdopt` previously wrote the DB row but never dispatched `CTRL_ADOPT_TARGET`, so cross-subnet infra was silently never probed).
- `9c3abdd` Tab5 → dedicated SolariNet Authenticator (TOTP · push-approval · vault client · password generator · BLE-HID autotype) + working ECDSA-P256 approval broker.
- `4e46c7b` docs.

## Known gaps / follow-ups (honest, prioritized)
1. **Bidirectional SoR sync — LIVE + closed** (`deploy/sorsync/` + `dashboard/api/lib/Sor.php`): operator create/alter/remove in the dashboard → **writes the SoR** → CDC outbox → `sor.events` → DNS auto-rendered/reloaded; and detected+adopted host → SoR → onward to DNS. Verified end-to-end both directions (insert/update/delete). Render targets: BIND (xenon), **AD `akoria.org`** (`sor_apply_ad` on radium, safe state-file deletes), **Pi-hole** (`sor_apply_pihole` on xenon → helium/mercury) — all live + verified. The hourly `akoria-dns-reconcile` on radium is **retired** (disabled; `sor_apply_ad` is now sole keeper of `akoria.org`). Remaining extensions: config as a render target; Pi-hole CNAMEs; monotonic zone serial for secondary AXFR; outbox pruning. (`sor_apply_ad` now has a periodic self-reconcile tick — hourly by default.)
2. **Monitor fleet registry** — no `monitor`-role rows in the `node` table (all `client`); HRW owner-select is a documented **CONTRACT GAP** (fleet-of-one = server nodeId). Works today via broadcast; close it (fleet enumerator → true k-of-n) before the fleet grows. Note: adopted targets are runtime state — a from-scratch rebuild needs re-adopt.
3. **Keycloak app-layer check — CLOSED.** `probeTypeForPort()` now maps `9000→http` with default `checkArg` `/health/ready|200`, and `appCheckHttp()` enforces it (exact code or `Nxx` class); a bare-`tcp` "port open" no longer reads as healthy. (Same fix reclassifies `3000→http` — see the cesium incident below, the motivating case.)
4. **Hardware-gated** — Tachyon SMS (`10.6.6.10` unreachable), Tab5 flashing + C6 BLE firmware, NFC (ST25DV04K tag confirmed on xenon `i2c-4`, deprioritized).
5. Housekeeping — revoke the `sor-seed-*` Forgejo token; re-tighten radium `named` AppArmor.

## Incident: cesium Forgejo outage (2026-07-06/07)

A USB-attached UAS disk backing Forgejo's data volume on cesium dropped off the bus;
btrfs forced an emergency read-only shutdown and every Forgejo request 500'd for **~26h**
while the monitor stayed green — the `:3000` check was bare `tcp` (socket open ≠ healthy)
and there was no host-local health signal or alert path for a fault of this class. SoR was
never at risk (separate disk/controller); net data loss was effectively zero. This incident
is the direct motivation for the host-health monitoring, HTTP status-aware probe, alert→MQ→Apple
bridge, dead-man's-switch, and nightly-backups work above (`docs/design/HOST_HEALTH_CONTRACT.md`).
Full write-up: `docs/SolariNet_Incident_2026-07-07_cesium.md`.

## Operational reference
- **Endpoints**: SoR cesium `:3306` / replica benzene `:3307`; RabbitMQ benzene `:5672`/`:15672`; MQTT benzene `:1883`/`:9001`; dashboard xenon `:9443`.
- **Credentials**: in `/root/.*-pw` on each host + private agent memory — **never** in this repo.
- **Deeper docs**: `docs/SolariNet_Overnight_Build.md` (plan+progress), `docs/SolariNet_Dashboard_Audit.md`, per-component READMEs (`deploy/notify/`, `deploy/authbroker/`, `netdb/sor/`, `firmware/tab5/`).
- **Activate a new monitored service** (the working pattern): `ADOPT disc=<discId> services=<port[,port]> heartbeat=1` over `run/solariCtl.sock`; ports 389/3306/5672 auto-upgrade to LDAP/MySQL/AMQP app-checks (`serverAssets.c probeTypeForPort`).
