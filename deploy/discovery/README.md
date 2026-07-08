# mDNS/zeroconf discovery — live inspector + file importer

Two tools, one shared upsert core, both augmenting the SolariNet Discovery
asset table (`discovered`) with mDNS host names and advertised-service
summaries:

- **`mdns_inspect.py`** (primary, LIVE) — actively browses mDNS/zeroconf/
  Bonjour on the wire for a bounded window and upserts whatever it resolves.
  This is what a discovery probe runs.
- **`avahi_import.py`** (secondary, file-based) — the original one-shot
  importer: reads a previously-saved **Avahi Browser** app JSON export
  (top-level `{"hosts": [...]}`) and upserts it. Still useful for replaying
  a capture from a phone/tablet that can reach a segment the probe host
  can't. `mdns_inspect.py` imports this module and calls its upsert logic
  directly rather than reimplementing it — the two tools share one
  correlate/UPDATE/INSERT code path.

Ops/infra glue — Python, not part of the C `solariServer`/`solariMonitor`
core (accepted per `CLAUDE.md`, same convention as `deploy/sorsync/` and
`deploy/alertbridge/`).

## What it does (shared upsert logic)

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

## mdns_inspect.py — LIVE inspector

Actively browses mDNS/DNS-SD for a bounded window (`--timeout`, default 8s)
instead of reading a saved export, then upserts exactly like `avahi_import.py`
would for the equivalent hosts/services (it imports `avahi_import` as a module
and calls its `load_cfg()`/`db_connect()`/`import_export()` directly — the
correlate-by-IPv4 / UPDATE-`mdnsName`+`mdnsServices` / INSERT-`via='mdns'`
logic lives in exactly one place).

Two browse backends, tried in order:

1. **python-zeroconf** (`pip install zeroconf`), if importable — pure-Python,
   no external binary needed.
2. **`avahi-browse -aprtk`** (all types, parsable, resolve, terminate,
   raw/no-db-lookup type names), shelled out to if the binary is on `PATH`.
   Same tool `serverScan.c`'s `scanEnrichMdns()` already shells out to
   per-host during a portscan sweep — this is the standalone, DB-writing,
   whole-LAN equivalent.

If **neither** is available, it logs one line and exits **0** — a probe host
without mDNS tooling installed must never fail a discovery run over it.

```
python3 -m venv .venv && .venv/bin/pip install pymysql
# optional but preferred: .venv/bin/pip install zeroconf
cp mdns_inspect.conf.example mdns_inspect.conf && $EDITOR mdns_inspect.conf
chmod 600 mdns_inspect.conf

# Preview only, no writes, single pass:
source ../../run/db.env   # exports SOLARI_DB_PASS
.venv/bin/python3 mdns_inspect.py --once --dry-run

# Apply, single pass (what the systemd oneshot unit runs):
.venv/bin/python3 mdns_inspect.py --once

# Standalone continuous mode (no --once): browses, upserts, sleeps
# --interval seconds (default 60), repeats until SIGTERM/SIGINT.
.venv/bin/python3 mdns_inspect.py
```

Flags: `--once` (single pass then exit — what the oneshot unit uses),
`--timeout SECONDS` (per-pass active browse duration, default 8),
`--interval SECONDS` (sleep between passes in the non-`--once` loop, default
60), `--dry-run`, `--conf PATH` (default `mdns_inspect.conf` next to the
script, or `$MDNS_INSPECT_CONF`). DB creds come from `mdns_inspect.conf` (same
`[db]` shape as `avahi_import.conf`) or `$SOLARI_DB_PASS` — source
`run/db.env` first, same convention as `deploy/alertbridge/alertbridge.py`.

### Wiring into a discovery probe

`mdns_inspect.py`/`solari-mdns-inspect.service` are standalone — the C
control plane (`solariServer`'s `serverScan.c` / `solariMonitor`) is **not**
modified to call them. Two ways to make "runs with each discovery probe" real
without touching the C core:

1. **Scan hook (recommended).** `serverScanRun()` (`serverScan.h`) is the
   entry point an operator-driven discovery scan already funnels through
   (`solariCtl` -> `serverScan.c`). Add a thin wrapper around whatever
   invokes a scan (an operator script, a cron/systemd-timer, or a future
   `solariCtl scan` post-hook) that does, after the scan:
   `systemctl start solari-mdns-inspect.service` (or, run from the repo
   directly, `deploy/discovery/.venv/bin/python3
   deploy/discovery/mdns_inspect.py --once`). This keeps the mDNS browse
   fully out-of-process from `solariServer` — same arm's-length relationship
   `avahi_import.py` already had to the core.
2. **Its own cadence.** Since `mdns_inspect.py --once` is cheap
   (`--timeout`-bounded, a handful of seconds) and idempotent, it's also fine
   to just enable a `systemd` timer (`solari-mdns-inspect.timer`, not
   included — copy the pattern from `deploy/backups/`) firing every few
   minutes independent of when a portscan-style discovery sweep runs; either
   way it lands in the same `discovered` table a scan does, correlated by IP.

## avahi_import.py — file importer

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
