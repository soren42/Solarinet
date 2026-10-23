# sorsync — SoR bidirectional sync

Makes the MariaDB **System of Record** the live hub: changes flow *out* to the
network (DNS today) and detections flow *in*, both over the RabbitMQ bus. Python
ops glue (the accepted non-C exception); the SoR + MQ do the heavy lifting.

```
  dashboard / seed / reconciler ─▶ SoR (cesium)
                                    │  CDC triggers → sor_outbox (same txn)
                                    ▼
                              sor_emitd ──▶ sor.events (RabbitMQ topic)
                                                 │
              sor.{entities,ip_addresses,dns_records,dns_zones,…}.*
                                                 ▼
                              sor_apply_dns ──▶ isolated SoR generation
                                                 ├─▶ verified xenon release
                                                 └─▶ generated zones AXFR/NOTIFY + SOA verification on each configured secondary

  monitoring `discovered` ──▶ sor_reconcile_discovery ──▶ SoR entities/ips
                              (adopted+named, correlated by IP, provenance=monitor)
                              └─ writing the SoR re-enters the forward loop ─┘
```

## Components
| Daemon | Role |
|---|---|
| `sor_emitd.py` | Drains `sor_outbox` (CDC triggers) → publishes `sor.<table>.<action>` to `sor.events`; advances `sor_sync_state.emit_checkpoint` only after publisher confirms and mandatory routing. The emitter retries confirmed prefixes after a failure. |
| `sor_apply_dnsd.py` | Renders a private complete generation, verifies the root-owned release on xenon, then refreshes and verifies SOA parity on each named secondary. It records per-target desired input / verified generation state. Every configured secondary is retried even when another target fails or the primary is unchanged. Startup and hourly reconcile. |
| `sor_apply_pihole.py` | SoR host set → verified atomic `custom.list` replacement on helium/mercury. Reload is retried even when files match, to recover a prior reload failure. |
| `sor_apply_ad.py` | Render target: SoR host set → AD `akoria.org` via `samba-tool dns`. Runs **on radium** (the DC) as root; state-file deletion policy never touches AD infra. |
| `sor_reconcile_discovery.py` | Folds **adopted, named** `discovered` hosts into the SoR (correlated by IP, v4-mapped-aware), tagged `solarinet_monitor`. New hosts then flow forward to DNS automatically. |

## Data-capture
`netdb/sor/migrations/022_transactional_outbox.sql` creates `sor_outbox`
(transactional outbox) and `sor_sync_state`. Migration
`023_sor_cdc_triggers.sql` creates I/U/D triggers on `entities, ip_addresses, interfaces,
dns_records, dns_zones, networks, subnets` (generated; every table has PK `id`
after migration 001, so they're uniform). Triggers write in the change's own
transaction → no lost events. These objects are now part of the ledgered SoR
migration stream; the copies under `sql/` are retained for reference to existing
installations and are not the clean-install path.

## Message contract (`sor.events`, topic, durable)
Routing key `sor.<table>.<action>` (action ∈ insert|update|delete). Body:
`{"outbox_id":N,"table":"dns_records","row_id":N,"action":"update","ts":epoch}`.
Consumers re-render from **current** SoR state, so exact payload/ordering don't
matter — only "something relevant changed."

**Delivery status:** publisher confirms protect the outbox-to-broker boundary.
The DNS, Pi-hole, and AD appliers acknowledge after all configured targets apply
and periodically reconcile from the SoR. DNS secondaries are configured as
`[dns.secondary.<name>]` sections with an address, SSH destination, and remote
atomic zone-list updater. After the verified Xenon release, the applier runs
the installed `solari-dns-secondary-refresh` helper for each configured target;
that helper updates the declaration list and returns success only after every
zone's SOA matches Xenon (and Radium matches the forward zone). Each secondary
is retried on every reconciliation after primary success, even if another
secondary already applied. The generated bare-zone transfer ACL contains
exactly the configured secondary addresses.
Steel is intentionally absent while the ZimaBlade is offline; add its named
section only after redeployment and independent verification. The
`sor-apply-dns` event no longer waits for a direct file copy to Steel.
Target work runs in a worker while the
owning thread services RabbitMQ heartbeats; a lost broker connection waits for
the in-flight apply to finish before retry, leaving deliveries unacknowledged.
Thirty-seven tests pass with real Pika and isolated MariaDB/RabbitMQ services.
Alongside heartbeat, redelivery, and mandatory-route fault cases, the chain test
uses SoR CDC triggers to publish entity/IP changes, invokes the real SoR DNS zone
generator, runs the real release engine against a private loopback BIND process,
and checks the DNS answer plus persisted target proof before each RabbitMQ
acknowledgment. It invokes the actual release CLI through `sudo -n` with a
root-owned temporary config and release tree. A private BIND process running as
the `bind` account serves the resulting generation. The release helper accepts
an NS record as its served-data check when a zone has no A/PTR records, while
preferring A/PTR checks when present; generated zone directories are explicitly
traversable even under a restrictive applier umask. PyYAML is pinned in
`requirements.txt` because `netdb/gen-zones.py` imports it during SoR rendering.
The chain test also drops the consumer connection during a real target release
at BIND reload. It verifies that the completed generation is served, deliveries
remain unacknowledged and are requeued with RabbitMQ's `redelivered` flag, and a
fresh consumer reapplies/verifies the target before acknowledging. This covers
client-connection loss and retry against a real broker and BIND process.
It also SIGKILLs a separate consumer process after the serving proof is recorded
but before its first broker ack, then verifies requeue and successful
apply/verification by the restarted consumer. This covers a process crash across
the apply/ack boundary; it does not exercise systemd's restart policy.
**F-08 remains open** until daemon restart and multi-target recovery drills prove
the deployed chain. The isolated suite stops and starts RabbitMQ's application,
then stops and restarts the entire disposable broker container while the DNS
consumer process stays alive. It verifies startup reconciliation after each
recovery, consumer re-registration, event apply, and acknowledgement. Host
reboot, systemd-managed consumer restart, multi-target recovery, and production
cutover remain unverified. Do not infer target freshness from an advanced
emitter checkpoint alone.

## Install

For a fresh SoR installation only, create the empty database on Cesium with
`sudo mariadb -e 'CREATE DATABASE sor CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;'`.
Existing databases must follow the migration audit and backup gate in the
standalone migration runbook.

```
cp sorsync.conf.example sorsync.conf && $EDITOR sorsync.conf   # fill creds
chmod 600 sorsync.conf
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
python3 ../../tools/migrate.py --database sor --db-name sor \
  --defaults-file /protected/path/sor-client.cnf --host cesium.akoria.net \
  --user solari
sudo cp sor-*.service /etc/systemd/system/
sudo systemctl daemon-reload
# Complete DNS helper installation/bootstrap and the new config before starting
# sor-apply-dns. See ../dns-release/README.md; do not mix it with the legacy copier.
```
Live on xenon (reaches cesium SoR + benzene MQ). `sor_reconcile_discovery.py
--dry-run` previews the reverse path without writing.
The command above is for a clean SoR only. The current production schema is
nonempty and has no migration ledger; do not run it there until the production
schema audit/adoption procedure is complete. See
`docs/SolariNet_Migration_Runbook_2026-10-20.html`.

## Follow-ups
- **DNS cutover** — stop the legacy writer, install immutable root helpers and
  protected configs on Xenon and each configured secondary, bootstrap their managed includes, and create
  private applier-owned incoming/state directories. The new `[dns]` keys replace
  direct destination/reload settings. `[dns.secondary.<name>]` sections define
  the address, SSH host, and atomic remote zone-list updater for each verified
  secondary. `targets.json` records desiredSource, appliedSource,
  appliedGeneration, lastAttempt, lastSuccess, and failure phase for `primary`
  and each `secondary:<name>`. The offline Steel/ZimaBlade is deliberately not
  configured; add it after console redeployment and independent verification.
  These source hashes identify rendered inputs; target serial rebasing means the
  root helper's appliedGeneration is the serving artifact identity.
- **Zone serial** — isolated renders retain unchanged serials. Release journals
  retain high-water marks across rollback and zone removal/reintroduction.
  Secondary AXFR rollback and restore drills remain required before deployment.
- **More render targets** — AD (`sor_apply_ad`, on radium) and Pi-hole
  (`sor_apply_pihole`, on xenon) DONE. Config/other are also SoR views; add
  appliers binding the same `sor.events` as needed. The old
  `akoria-dns-reconcile` writer is retired; do not run competing AD writers.
- **Deploy note** — appliers run near their targets: emit/apply-dns/apply-pihole/
  reconcile on **xenon** (`deploy/sorsync/`), apply-ad on **radium**
  (`/opt/solari-sorsync/`, as root). Each host has its own gitignored `sorsync.conf`.
  Pi-hole CNAMEs (dnsmasq `cname=`) not yet rendered — A records only.
- **Legacy dashboard mirror.** `dashboard/api/lib/Sor.php` mirrors operator
  mutations into the SoR (fail-soft): ADOPT + ASSET_SET → `upsertHost`, ASSET_REMOVE
  → `removeHost` (soft-delete). Each host mirror operation now runs as one SoR
  transaction under a per-IP advisory lock, making concurrent/retried desired
  state writes atomic. It remains a fail-soft mirror after the primary control
  mutation; F-22 stays open for the remaining cross-store transaction/idempotency
  boundaries. Needs `SOR_DB_*` in the php-fpm pool
  (`/etc/php/8.4/fpm/pool.d/solarinet.conf`, alongside `SOLARI_DB_*`; **quote a
  password containing `!`** or php-fpm won't start). Operator edits now enter this
  loop directly (→ DNS), with `sor_reconcile_discovery` as the catch-all net.
- **Outbox pruning** — define a retention/restore policy after queue durability
  drills; an emitter checkpoint alone does not prove downstream freshness.
