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
                              sor_apply_dns ──▶ render zones FROM SoR ──▶ rndc reload
                                                 (debounced, only on real change)

  monitoring `discovered` ──▶ sor_reconcile_discovery ──▶ SoR entities/ips
                              (adopted+named, correlated by IP, provenance=monitor)
                              └─ writing the SoR re-enters the forward loop ─┘
```

## Components
| Daemon | Role |
|---|---|
| `sor_emitd.py` | Drains `sor_outbox` (CDC triggers) → publishes `sor.<table>.<action>` to `sor.events`; advances `sor_sync_state.emit_checkpoint`. At-least-once. |
| `sor_apply_dnsd.py` | Consumes DNS-relevant `sor.events`, debounces, re-renders zones (`gen-zones.py --source sor`), diffs vs live BIND, deploys + `rndc reload` only on change. Idempotent + startup reconcile. |
| `sor_apply_pihole.py` | Render target: SoR host set → `custom.list` on each Pi-hole (helium/mercury) over ssh; reload only on change. Runs on xenon. |
| `sor_apply_ad.py` | Render target: SoR host set → AD `akoria.org` via `samba-tool dns`. Runs **on radium** (the DC) as root; state-file deletion policy never touches AD infra. |
| `sor_reconcile_discovery.py` | Folds **adopted, named** `discovered` hosts into the SoR (correlated by IP, v4-mapped-aware), tagged `solarinet_monitor`. New hosts then flow forward to DNS automatically. |

## Data-capture
`sql/01-outbox.sql` — `sor_outbox` (transactional outbox) + `sor_sync_state`.
`sql/02-triggers.sql` — I/U/D triggers on `entities, ip_addresses, interfaces,
dns_records, dns_zones, networks, subnets` (generated; every table has PK `id`
after migration 001, so they're uniform). Triggers write in the change's own
transaction → no lost events.

## Message contract (`sor.events`, topic, durable)
Routing key `sor.<table>.<action>` (action ∈ insert|update|delete). Body:
`{"outbox_id":N,"table":"dns_records","row_id":N,"action":"update","ts":epoch}`.
Consumers re-render from **current** SoR state, so exact payload/ordering don't
matter — only "something relevant changed."

## Install
```
cp sorsync.conf.example sorsync.conf && $EDITOR sorsync.conf   # fill creds
chmod 600 sorsync.conf
python3 -m venv .venv && .venv/bin/pip install pika pymysql
mysql sor < sql/01-outbox.sql && mysql sor < sql/02-triggers.sql   # once, on cesium
sudo cp sor-*.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now sor-emit sor-apply-dns sor-reconcile
```
Live on xenon (reaches cesium SoR + benzene MQ). `sor_reconcile_discovery.py
--dry-run` previews the reverse path without writing.

## Follow-ups
- **Zone serial** — `gen-zones` uses `YYYYMMDDnn` (fixed nn); the primary serves
  fresh data on reload, but same-day multi-change secondary AXFR needs a monotonic
  serial. Bump it in the applier (or gen-zones) for correct secondary propagation.
- **More render targets** — AD (`sor_apply_ad`, on radium) and Pi-hole
  (`sor_apply_pihole`, on xenon) DONE. Config/other are also SoR views; add
  appliers binding the same `sor.events` as needed. The hourly AXFR-based
  `akoria-dns-reconcile` on radium is now redundant with `sor_apply_ad` (both keep
  akoria.org in sync from the SoR) — can be retired once the applier proves out.
- **Deploy note** — appliers run near their targets: emit/apply-dns/apply-pihole/
  reconcile on **xenon** (`deploy/sorsync/`), apply-ad on **radium**
  (`/opt/solari-sorsync/`, as root). Each host has its own gitignored `sorsync.conf`.
  Pi-hole CNAMEs (dnsmasq `cname=`) not yet rendered — A records only.
- **Dashboard write path — DONE.** `dashboard/api/lib/Sor.php` mirrors operator
  mutations into the SoR (fail-soft): ADOPT + ASSET_SET → `upsertHost`, ASSET_REMOVE
  → `removeHost` (soft-delete). Needs `SOR_DB_*` in the php-fpm pool
  (`/etc/php/8.4/fpm/pool.d/solarinet.conf`, alongside `SOLARI_DB_*`; **quote a
  password containing `!`** or php-fpm won't start). Operator edits now enter this
  loop directly (→ DNS), with `sor_reconcile_discovery` as the catch-all net.
- **Outbox pruning** — add a periodic `DELETE FROM sor_outbox WHERE id <= emit_checkpoint`.
