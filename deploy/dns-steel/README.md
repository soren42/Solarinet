# steel DNS secondary

Steel (`10.0.0.11`) was reimaged in P6 to openSUSE Tumbleweed. The former
Docker BIND configuration is retired. Apply the native Ansible `dns-secondary`
role through the host's SoR stack using Apply-stacks-only; this role does not
manage recursion, RPZ, DHCP, or the nameserver set. Xenon remains primary at
`10.0.0.20`; chlorine remains a secondary at `10.7.0.10`.

## Deployment sequence

1. On Xenon, add `10.0.0.11` to `transfer` (forward and reverse zones) and
   `bareTransfer` (bare zones) in the root-owned `/etc/solarinet/dns-release.json`
   (back it up first). `solarinet-dns-release` writes those lists into each
   staged generation's `allow-transfer`/`also-notify`, so they take effect with
   the next generation (restart `sor-apply-dns` to stage one). The
   `gen-zones`/`gen-bare` transfer defaults do not control the live primary.
   Zones outside the release (`named.conf.rpz`) carry their own lists. Do not
   add steel to any NS record.
2. Authorize the Xenon DNS applier's existing SSH public key in
   `solari-admin`'s `authorized_keys` on steel. Do not copy private key
   material. Confirm host-key verification from Xenon.
3. Prepare the `[dns.secondary.steel]` section in Xenon's `sorsync.conf` and
   keep `sor-apply-dns` stopped until the role has been applied:

   ```ini
   [dns.secondary.steel]
   address = 10.0.0.11
   ssh_host = solari-admin@steel.akoria.net
   update_helper = /usr/local/sbin/solari-bind9-update-zones
   ```

4. Apply the target SoR stack to steel through `prov-finish` / `STACK_APPLY`
   (Apply-stacks-only; it logs in as the admin user of steel's last reimage). The defaults fit steel: `dns_secondary_address` is the
   default-route IPv4 (`10.0.0.11`), and the provisioning baseline already gives
   `solari-admin` NOPASSWD sudo. Set `dns_secondary_admin_user: solari-admin`
   and `dns_secondary_manage_sudoers: true` only where the baseline grant is
   removed. Then enable the prepared sorsync section and resume `sor-apply-dns`.

## Validate

Steel serves no transfers of its own (`allow-transfer { none; }`), so an AXFR
query *to* steel is refused by design. Verify that the primary allows steel to
transfer (run from steel), that steel loaded the zones, and that SOA serials match:

```sh
ssh solari-admin@steel.akoria.net 'dig +tcp @10.0.0.20 akoria.net AXFR | tail -1'
ssh solari-admin@steel.akoria.net 'sudo -n /usr/bin/named-checkconf /etc/named.conf; sudo -n systemctl is-active named.service; sudo -n ls /var/lib/named/solari-secondary'
dig @10.0.0.11 akoria.net SOA +noall +answer
dig @10.0.0.20 akoria.net SOA +noall +answer
```

The AXFR from the primary must complete (its last line reports the XFR size),
steel must hold a db file per zone, and the SOA serials must match. Then send a DNS change
through SoR and confirm the applier records steel's secondary refresh with SOA
parity before acknowledging the event: `secondary:steel` in
`/var/lib/solarinet-dns/state/targets.json` reaches `phase: applied` at the
primary's `desiredSource`. Steel must not be added to NS until
a separate change explicitly authorizes it.
