# steel — akoria.net authoritative secondary (Docker BIND9)

**steel** (ZimaBlade @ `10.0.0.11`) runs ISC BIND9 in Docker as an authoritative
**secondary** for `akoria.net` and its reverse zones, transferring from the
primary **xenon** (`10.0.0.20`). This offloads DNS redundancy from xenon (the
concentration-risk host) ahead of a staged cutover that promotes steel to
primary and retires the Pi-holes.

## Files (config, no secrets)

- `named.conf` — top-level include of the two files below.
- `named.conf.options` — authoritative-only (`recursion no`), listen on all,
  `allow-query { any; }`. The resolver/recursion role is added at cutover.
- `named.conf.local` — one `type secondary` stanza per zone (forward +
  16 reverses), `primaries { 10.0.0.20; }`. Regenerate the zone list from the
  live primary's `named.conf.akoria` if zones are added/removed.

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
