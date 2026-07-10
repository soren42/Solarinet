# Bare-name (single-label) resolution — dnsmasq parity

Dumb SMB / legacy clients (e.g. the Epson scanner using `\\NAS-X\`) query the
**bare hostname** `NAS-X` without the `akoria.net` suffix and don't honor the
DHCP search domain. Pi-hole's dnsmasq resolved these transparently; authoritative
BIND does not. `gen-bare.py` restores it by emitting a single-label **master**
zone per host + CNAME alias (apex `A` = the resolved IP).

## Regenerate + deploy (run when the SoR host/alias set changes)
```sh
# on xenon (reads the rendered forward zone):
sudo python3 gen-bare.py /etc/bind/zones/db.akoria.net /tmp/bare && \
  sudo cp /tmp/bare/db.bare-* /etc/bind/zones/ && \
  sudo cp /tmp/bare/named.conf.bare /etc/bind/ && sudo rndc reload
# steel serves the same set as master zones under /mnt/data/bind/config/zones/
# (mounts at /etc/bind/zones), included via /etc/bind/named.conf.bare; restart bind9.
```
`named.conf.local` on each resolver must `include "/etc/bind/named.conf.bare";`.

## TODO
Wire `gen-bare.py` into `sor-apply-dns` so bare zones regenerate + redeploy to
**both** resolvers on every SoR change (currently a manual re-run; host IPs are
stable so drift is slow). The applier runs on xenon — the steel half needs a
remote file push + container reload.
