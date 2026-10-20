# netdb — Akoria DNS renderers and historical seed

The MariaDB System of Record on cesium is authoritative for host identity and
DNS. `netdb` contains the original YAML seed, the SoR reader, and the BIND
renderer. Edit authoritative records through a SoR write path; never hand-edit
rendered zone files.

- **`akoria-hosts.yml`** — historical seed and offline input: `hosts`
  (element/compound A records), `ifaces` (extra A), and `cnames` (function to
  host). It is no longer the live authority after the SoR cutover.
- **`gen-zones.py`** — renders `zones/` (BIND `akoria.net` forward + per-/24 reverse
  zones + `named.conf.akoria`). Production uses `--source sor`, which reads the
  renderer views in `netdb/sor/source_sor.py`. The dashboard is one SoR CRUD
  front-end; the `sor-apply-dns` service renders and reloads BIND.
- **`sync-net-to-org.py`** — retired historical reconciler. The live AD target
  is `deploy/sorsync/sor_apply_ad.py`; do not schedule both writers on radium.

Topology: **xenon** renders BIND primary zones for `akoria.net` and reverse DNS;
**radium** owns AD-managed `akoria.org` through the SoR apply service. The
Pi-hole apply service renders local records for helium and mercury. See
`deploy/sorsync/README.md` for the current sync chain and failure behavior.

## Isolated generation candidate

Both renderers accept a new output directory and a separate **last applied**
zone directory. Unchanged zone content keeps its serial; changed content
advances it under RFC 1982 ordering. Bare zones use the same allocator. Drafts
never update the previous generation.

```sh
python3 netdb/gen-zones.py --source sor --output /path/to/new-generation \
  --previous /path/to/applied-zones --zone-dir /etc/bind/solarinet-current/zones
python3 deploy/dns-bare/gen-bare.py /path/to/new-generation/db.akoria.net \
  /path/to/new-generation --previous /path/to/applied-zones \
  --zone-dir /etc/bind/solarinet-current/zones
```

Use `--source yaml` for the offline fixture. A fresh output directory contains
only the desired zone set, including removal of retired files. Five tests cover
serial reuse, changes, wrap, corrupt previous state, isolated output, and CNAME
cycles; generated zones and the combined config pass BIND's validation tools.
The live applier still needs atomic release switching, served-data verification,
rollback, and independent steel retry before F-17/F-08 can close.
