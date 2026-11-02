# steel DNS secondary

Steel (`10.0.0.11`) was reimaged in P6 to openSUSE Tumbleweed. The former
Docker BIND configuration is retired. Apply the native Ansible `dns-secondary`
role through the host's SoR stack using Apply-stacks-only; this role does not
manage recursion, RPZ, DHCP, or the nameserver set. Xenon remains primary at
`10.0.0.20`; chlorine remains a secondary at `10.7.0.10`.

## Deployment sequence

1. On Xenon, update forward and reverse transfer/NOTIFY declarations and bare
   zone defaults to include `10.0.0.11`; validate the complete configuration,
   then apply it. Do not add steel to any NS record.
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
   (Apply-stacks-only), with `dns_secondary_address: 10.0.0.11`,
   `dns_secondary_admin_user: solari-admin`, and
   `dns_secondary_manage_sudoers: true`. Then enable the prepared sorsync
   section and resume `sor-apply-dns`.

## Validate

On Xenon, verify transfer over TCP and compare current SOA serials:

```sh
dig +tcp @10.0.0.11 akoria.net AXFR
dig @10.0.0.11 akoria.net SOA +noall +answer
dig @10.0.0.20 akoria.net SOA +noall +answer
ssh solari-admin@steel.akoria.net 'sudo -n /usr/bin/named-checkconf /etc/named.conf; sudo -n systemctl is-active named.service'
```

The AXFR must complete, and the SOA serials must match. Then send a DNS change
through SoR and confirm the applier logs steel's secondary refresh and records
SOA parity before acknowledging the event. Steel must not be added to NS until
a separate change explicitly authorizes it.
