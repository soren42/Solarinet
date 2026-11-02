# dns-secondary Ansible role

Installs an authoritative-only BIND secondary on openSUSE or Debian. SUSE uses
`bind`, `named.service`, `/etc/named.conf`, `/var/lib/named`, and the `named`
account. Debian uses `bind9`, `bind9.service`, `/etc/bind`, and the `bind`
account. Recursion is disabled, queries are restricted to `10.0.0.0/8`, and
the listener defaults to `ansible_default_ipv4.address`. Xenon (`10.0.0.20`) is
the only transfer source.

The privileged updater accepts the existing Xenon refresh helper's zone-list
format on stdin. It validates declarations, writes the zone file atomically,
runs `named-checkconf`, and calls `rndc reconfig`. The input's Khadas data path
is translated to the target's native data directory. When firewalld is active,
the role enables its DNS service.

## Stack invocation

Create a `prov_software` row with `logical_name = 'DNS secondary'`,
`item_type = 'role'`, `name_suse = '='`, `name_debian = '='`,
`ansible_role = 'dns-secondary'`, and null names for unsupported families.
Add that software entry as a member of the target stack. A corresponding
stackgen JSON item has this exact shape:

```json
{
  "logical_name": "DNS secondary",
  "item_type": "role",
  "names": {"suse": "=", "debian": "="},
  "ansible_role": "dns-secondary"
}
```

Set target-specific variables on the host or in its stack vars:

```yaml
dns_secondary_address: 10.0.0.11
dns_secondary_admin_user: solari-admin
dns_secondary_manage_sudoers: true
```

The sudoers entry permits only that account to run the configured updater as
root, without arguments. Xenon's applier SSH public key must be authorized for
`solari-admin` on the secondary. Keep private key material on Xenon.

## Check

Run the fleet playbook through Apply-stacks-only and confirm the second run is
idempotent. Validate configuration and service state with:

```sh
sudo /usr/bin/named-checkconf /etc/named.conf
sudo systemctl is-active named.service
dig +tcp @10.0.0.11 akoria.net AXFR
dig @10.0.0.11 akoria.net SOA +noall +answer
```

On Debian, use `/usr/sbin/named-checkconf /etc/bind/named.conf` and
`bind9.service` in the checks.
Compare the returned SOA serial with Xenon's. Enable the matching named section
in `sorsync.conf` only after SSH and AXFR validation succeed.
