# steel — akoria.net authoritative secondary (Docker BIND9)

**steel** (`10.0.0.11`) is the inventory-labeled ZimaBlade. Jason booted it on
2026-10-21 and observed no useful running service; it is on port 12 of the
`laboratory` switch. Network discovery and SSH checks still found `.11`
unreachable. The separately verified Khadas VIM4 is **chlorine** (`10.7.0.10`),
which runs the internal CA; its BIND deployment is documented in
[`../dns-khadas/README.md`](../dns-khadas/README.md). Do not install the Khadas
configuration on steel. The ZimaBlade will be considered for a future tertiary
DNS role after console access and redeployment. The operator confirms exactly
three distinct Zima devices: one ZimaBlade, one ZimaBoard, and one ZimaBoard 2;
the mapping of the other two units to inventory records remains pending.

The Docker BIND files below are an unverified deployment candidate for steel,
not a description of its current runtime. Recheck the physical device, address
handoff, container, mounts, and listeners before using them. Steel is not
currently advertised as an authoritative nameserver.

## Files (config, no secrets)

- `named.conf` — top-level include of the two files below.
- `named.conf.options` — authoritative-only (`recursion no`), listen on all,
  `allow-query { any; }`. The resolver/recursion role is added at cutover.
- `named.conf.local` — includes `named.conf.bare` for the legacy SoR-managed
  bare zones, followed by one `type secondary` stanza per zone (forward +
  16 reverses), `primaries { 10.0.0.20; }`. The release helper removes the
  legacy bare include as it replaces this parent with the managed generation.
  Regenerate the secondary zone list from the live primary's `named.conf.akoria`
  if zones are added/removed.

## Deploy

Config lives on the host at `/mnt/data/bind/config` (this dir); BIND's writable
zone store is `/mnt/data/bind/cache`.

```sh
docker run -d --name bind9 --restart unless-stopped \
  -p 53:53/tcp -p 53:53/udp \
  -v /mnt/data/bind/config:/etc/bind:ro \
  -v /mnt/data/bind/cache:/var/cache/bind \
  internetsystemsconsortium/bind9:9.20 \
  -g -c /etc/bind/named.conf
```

Note the trailing `-g -c /etc/bind/named.conf`: it **overrides the image's
default CMD**, which otherwise logs to a file (`-L /var/log/bind/default.log`)
that doesn't exist in the container — named then exits 1 with empty
`docker logs`. `-g` logs to stderr so `docker logs bind9` works.

## Transfer authorization (on the primary)

xenon authorizes steel via the zone generator: `netdb/gen-zones.py` lists steel
in `SECONDARIES`, so every zone renders `allow-transfer { 10.1.0.10; 10.0.0.11; }`
and `also-notify { … }` — NOTIFY-driven AXFR, no polling lag. Never hand-edit
`named.conf.akoria`; edit the SoR / generator and let `sor-apply-dns` re-render.

## Validate

```sh
dig +short @10.0.0.11 steel.akoria.net          # -> 10.0.0.11
dig +short @10.0.0.11 akoria.net SOA | awk '{print $3}'   # serial == xenon's
dig +short @10.0.0.11 -x 10.0.0.20              # -> xenon.akoria.net.
```
