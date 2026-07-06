# netdb — Akoria network source-of-truth + DNS generator

Single source of truth for host identity + DNS, rendered to BIND.

- **`akoria-hosts.yml`** — the SoT: `hosts` (element/compound A records), `ifaces`
  (extra A), `cnames` (function → host). Schema mirrors the eventual DB tables.
- **`gen-zones.py`** — renders `zones/` (BIND `akoria.net` forward + per-/24 reverse
  zones + `named.conf.akoria`). `load_source()` is the only seam that changes when
  the central **MySQL** DB replaces this file; the **SolariNet dashboard** becomes
  its CRUD front-end. Never hand-edit zone files — edit the SoT and regenerate.
- **`sync-net-to-org.py`** — hourly reconciler on radium (the AD DC): AXFRs
  `akoria.net` from xenon and mirrors each host A record into AD-managed
  `akoria.org`, so every host resolves under both domains.

Topology: **xenon** = BIND primary for `akoria.net` (+ reverse); **radium** =
BIND9_DLZ master for `akoria.org` (AD) + `akoria.net` AXFR secondary; Pi-holes
(helium/mercury) conditional-forward `akoria.*` → xenon. Technitium retired.
