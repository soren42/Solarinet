# avahi_import — Avahi/mDNS discovery importer

Augments the SolariNet Discovery asset table (`discovered`) with mDNS host
names and advertised-service summaries pulled from an **Avahi Browser**
JSON export (the mobile/desktop app's "export" feature — top-level
`{"hosts": [...]}`, one entry per mDNS host with its `addresses` and
`services`). Ops/infra glue — Python, not part of the C `solariServer`/
`solariMonitor` core (accepted per `CLAUDE.md`, same convention as
`deploy/sorsync/` and `deploy/alertbridge/`).

## What it does

For each host in the export with at least one IPv4 address:

- **Existing `discovered` row for that IP** (any discovery method — ARP,
  portscan, LLDP, previous mDNS import) → `UPDATE`s `mdnsName` +
  `mdnsServices`, bumps `seenCount`, refreshes `lastSeenAt`. The row's `via`
  (how it was *first* discovered) and everything else is left alone.
- **No row for that IP** → `INSERT`s a fresh `discovered` row
  (`kind='host'`, `via='mdns'`, `status='new'`) so the host shows up in
  Discovery for an operator to triage/adopt.

`mdnsServices` is a compact, comma-joined, de-duplicated list of advertised
DNS-SD service types with the trailing dot stripped, e.g.
`_spotify-connect._tcp,_airplay._tcp` (see `db/migrations/013_mdns_services.sql`).

A host with multiple IPv4 addresses (dual-homed boxes) produces one
`discovered` row per address, all sharing the same `mdnsName`/`mdnsServices` —
same one-row-per-`(ip, kind)` convention ARP/portscan discovery already uses.
IPv6-only entries (e.g. bare Matter/Thread devices with no IPv4) are skipped:
`ip` is the correlation key and there's nothing to correlate on.

Idempotent — re-running the same export is a no-op beyond `seenCount`/
`lastSeenAt`. Fail-soft — a malformed host entry or a per-row DB error is
logged and skipped; the run keeps going and exits non-zero only if any row
errored.

## Usage

```
python3 -m venv .venv && .venv/bin/pip install pymysql
cp avahi_import.conf.example avahi_import.conf && $EDITOR avahi_import.conf
chmod 600 avahi_import.conf

# Preview only, no writes:
source ../../run/db.env   # exports SOLARI_DB_PASS
.venv/bin/python3 avahi_import.py --file /path/to/export.json --dry-run

# Apply:
.venv/bin/python3 avahi_import.py --file /path/to/export.json
```

`--file` defaults to `flame-export.json` next to the script (or
`$AVAHI_IMPORT_FILE`) if omitted. `--conf` defaults to `avahi_import.conf`
next to the script (or `$AVAHI_IMPORT_CONF`).

This is a one-shot importer, not a daemon — run it by hand after pulling a
fresh export, or wire it into a cron/systemd-timer the same way
`deploy/backups/` or `deploy/opie/` schedule their one-shot jobs, sourcing
`run/db.env` for `SOLARI_DB_PASS` first (same convention as
`deploy/alertbridge/alertbridge.service`).

## Schema

`db/migrations/013_mdns_services.sql` adds `discovered.mdnsServices
VARCHAR(512) NULL`, additive/idempotent, alongside the existing `mdnsName`
(`009_mdns_name.sql`). `db/schema.sql` is the baseline-only (migration 001)
canonical schema and doesn't define `discovered` at all — that table (and
every other migration since 002) already isn't mirrored there, so this
migration follows the same, established precedent.

## Export shape

```json
{"hosts": [
  {"name": "hydrogen (617)",
   "addresses": ["10.0.1.50", "10.6.172.229", "hydrogen.local.",
                 "fd1f:8000:3b42:91a4::1"],
   "services": [
     {"type": "_airplay._tcp.", "port": 7000, "domain": null,
      "name": "hydrogen", "data": {"deviceid": "...", "model": "..."}}
   ]}
]}
```

Only `addresses` (IPv4 literals) and `services[].type` are used; `.local.`
hostnames, IPv6 literals, and per-service `data`/`port`/`domain` are ignored
for now (the `discovered.mdnsServices` column is a type summary, not a full
service dump — `discovered.services` already exists as a generic JSON bag for
other `via` methods and is intentionally left untouched here).
