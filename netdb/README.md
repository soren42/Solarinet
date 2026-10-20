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
