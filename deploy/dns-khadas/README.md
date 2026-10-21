# chlorine — authoritative BIND secondary

`chlorine` is the Khadas VIM4 at `10.7.0.10`. It runs the internal CA and an
authoritative-only BIND secondary. The installed configuration covers
`akoria.net`, generated reverse zones, SoR-managed single-label zones, and
`rpz.akoria`. The service binds only `10.7.0.10:53`, disables recursion,
permits queries from the private `10.0.0.0/8` network, and transfers zones from
primary `xenon` (`10.0.0.20`). It does not own or modify the CA files or service.

The secondary declarations merge Xenon's `named.conf.akoria`,
`named.conf.bare`, `named.conf.local`, and `named.conf.rpz`. Only `primary` and legacy `master`
zone declarations are copied; forwarding and secondary declarations are
ignored. Xenon watches all four fragments and the RPZ zone data file, then
pushes validated declarations to chlorine. Before updating the secondary, the
refresh compares its generated zone set with the include-expanded primary-zone
inventory from `named-checkconf -p`; it keeps the expanded output in memory and
does not log it. It then verifies SOA serial parity for every generated zone on
chlorine and also checks radium on the forward zone. Khadas applies the RPZ
through its local `response-policy` option.

Live state verified on 2026-10-21 at 07:35 UTC: `solari-bind9.service` and
`step-ca` are active; `named-checkconf` passes on chlorine, and TCP/UDP port 53
listen only on `10.7.0.10`. The primary passes `named-checkconf -z`. Chlorine
now serves 109 zones: `akoria.net`, 17 generated reverse zones, 90 single-label
zones, and `rpz.akoria`. Xenon's refresh service reports matching SOA serials
for all 109 zones plus Radium's `akoria.net`. Direct checks returned
authoritative answers for `dashboard` (`2026071001`) and `rpz.akoria`
(`2026102101`), and the `akoria.net` NS set includes chlorine. A query for
`example.com` returns `REFUSED`, as intended. The zone update changed BIND
declarations and transfer ACLs; the existing bare-zone files and serials were
left intact. Xenon's path watcher and five-minute retry timer are active. A
canonical `named-compilezone` comparison also found matching contents and SOAs
for all 109 primary/secondary zones. The verifier reports hashes and zone names
only; it does not emit DNS records. A
backup of the prior Xenon BIND fragments is at
`/var/backups/solarinet-dns/20261021T071429Z`.

The legacy live SoR applier remains active and still has its old direct
copy-to-Steel path; it has not been cut over. The release candidate now defaults
bare-zone AXFR/NOTIFY to chlorine (`10.7.0.10`) only. Its SoR applier takes
named secondary sections from `sorsync.conf`, refreshes the declarations on
each configured secondary, and requires SOA parity before acknowledging a
delivery. It has no direct copy-to-Steel path. A full event through this
candidate has not yet been run against production. The ZimaBlade remains
unreachable over the network; its tertiary service is not configured or
included in the NS set.
The offline host fixture reserves `iron` at `10.0.0.11` for an intentional
post-console handoff; live DNS remains `steel` until that handoff is deliberate.
SSH administration uses normal host-key verification.

## Install

The Khadas secondary and its primary refresh are installed. For a fresh
installation, configure transfer/notify access on Xenon for every zone before
starting the refresh. Keep `named.conf.local`'s RPZ declaration as one include
of `named.conf.rpz`, and generate `named.conf.bare` with current secondary
targets. Validate the full primary config before reconfiguring BIND:

```sh
sudo named-checkconf && sudo rndc reconfig && sudo rndc reload
```

Install BIND on chlorine only if it is absent. Install the isolated service,
the privileged atomic zone-list updater, and the refreshed primary watcher as
shown below:

```sh
python3 deploy/dns-khadas/gen-secondary.py /etc/bind/named.conf.akoria /tmp/zones.conf \
  --additional-config /etc/bind/named.conf.bare \
  --additional-config /etc/bind/named.conf.local \
  --additional-config /etc/bind/named.conf.rpz
ssh jason@chlorine 'sudo -n apt-get update && sudo -n apt-get install -y bind9'
scp deploy/dns-khadas/named.conf deploy/dns-khadas/options.conf /tmp/zones.conf \
    deploy/dns-khadas/solari-bind9.service deploy/dns-khadas/install.sh \
    deploy/dns-khadas/update-zones.py jason@chlorine:/tmp/
ssh jason@chlorine 'sudo -n sh /tmp/install.sh'

scp deploy/dns-khadas/gen-secondary.py deploy/dns-khadas/refresh-secondary-zones.py \
    deploy/dns-khadas/solari-dns-secondary-refresh.service \
    deploy/dns-khadas/solari-dns-secondary-refresh.path \
    deploy/dns-khadas/solari-dns-secondary-refresh.timer \
    deploy/dns-khadas/install-primary.sh jason@xenon:/tmp/
ssh jason@xenon 'sudo -n sh /tmp/install-primary.sh'
```

Run `named-checkconf` before starting and confirm the native package service is
not left enabled. The local `systemd-resolved` stub remains on loopback; BIND is
bound to the Khadas LAN address only. Xenon's path unit watches
`/etc/bind/named.conf.akoria`, `.bare`, `.local`, and `.rpz`; each primary zone-list change triggers a validated
atomic update on chlorine, followed by serial checks on all configured zones.

## Verify

Run the full canonical content comparison from a machine with passwordless SSH
and sudo access to both DNS hosts:

```sh
python3 deploy/dns-khadas/verify-zone-content.py --primary xenon --secondary chlorine --user jason
```

It compares the active primary zones with chlorine's generated secondary list,
canonicalizes primary text and secondary raw files with BIND, and compares
SHA-256 hashes. It prints no zone record data.

```sh
dig +time=2 +tries=1 @10.7.0.10 akoria.net SOA +noall +answer
dig +time=2 +tries=1 @10.7.0.10 akoria.net NS +noall +answer
dig +time=2 +tries=1 @10.7.0.10 -x 10.7.0.10 +noall +answer
ssh jason@chlorine 'sudo -n systemctl is-active solari-bind9 step-ca; sudo -n ss -lntup | grep ":53"'
```

Compare SOA serials for every generated zone with `@10.0.0.20`. In particular,
verify one bare zone and `rpz.akoria`, and confirm `chlorine.akoria.net` appears
in the NS set only after its secondary is serving the current serial.
Then check the CA health endpoint and certificate service again.

For a local runtime integration check, run the opt-in test on a Linux host with
BIND utilities and passwordless sudo:

```sh
SOLARI_DNS_AXFR_TEST=1 python3 -m unittest discover \
  -s deploy/dns-khadas -p 'test_axfr_runtime.py' -v
```

It starts isolated primary and secondary `named` processes on loopback ports,
checks initial AXFR convergence, changes the primary zone serial and address,
signals a primary reload, and checks that NOTIFY/refresh brings the secondary
to the new SOA and answer. This validates local BIND transfer behavior; it does
not verify the live Xenon-to-chlorine deployment or SoR event pipeline.
