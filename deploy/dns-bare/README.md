# Bare-name (single-label) resolution — dnsmasq parity

Dumb SMB / legacy clients (e.g. the Epson scanner using `\\NAS-X\`) query the
**bare hostname** `NAS-X` without the `akoria.net` suffix and don't honor the
DHCP search domain. Pi-hole's dnsmasq resolved these transparently; authoritative
BIND does not. `gen-bare.py` restores it by emitting a single-label **primary**
zone per host + CNAME alias (apex `A` = the resolved IP). AXFR is denied unless
transfer targets are configured; authorized targets also receive NOTIFY. With
no `--transfer` arguments, the generator authorizes the active chlorine Khadas
secondary (`10.7.0.10`) only. Explicit `--transfer` arguments replace that
target set; add a future Zima address only after it is provisioned as a
secondary and independently verified.

## Regenerate + deploy (run when the SoR host/alias set changes)
```sh
# on xenon (reads the rendered forward zone):
sudo python3 gen-bare.py /etc/bind/zones/db.akoria.net /tmp/bare \
  --transfer 10.7.0.10 && \
  sudo cp /tmp/bare/db.bare-* /etc/bind/zones/ && \
  sudo cp /tmp/bare/named.conf.bare /etc/bind/ && sudo rndc reload
```
`named.conf.local` on Xenon must `include "/etc/bind/named.conf.bare";`.

## Sync
`sor-apply-dns` renders these zones from the SoR and authorizes exactly the
configured secondary targets for AXFR/NOTIFY. It refreshes their generated
secondary-zone declarations and waits for serial parity before acknowledging
the broker delivery. The active config contains chlorine (`10.7.0.10`). Steel's
ZimaBlade (`10.0.0.11`) is offline and is not a transfer target; add it only
after console redeployment, secondary installation, and verification. There is
no direct copy/release path to Steel.

The refresh helper also watches the RPZ zone file because its contents change
without changing the primary zone declaration. See
[`../dns-khadas/README.md`](../dns-khadas/README.md) for secondary verification.
