# SolariNet Fleet Provisioning — Ansible

This tree provisions **fresh** remote machines into the SolariNet fleet. An
Ansible control node runs on host `benzene` (openSUSE); the whole repo Ansible
tree is rsync'd to `/srv/solari-provision/ansible/` there and executed in place.

Managed targets span **Debian, Ubuntu, and openSUSE** across **x86_64 / arm64 /
arm32** (including Raspberry Pi OS). The design goal, verbatim from the project
owner:

> *the software default stack and core package list will be the same across
> distributions and architectures.*

## The shared-stack philosophy

There is exactly **one** logical package set and **one** base configuration,
applied identically to every host. Distributions differ only in a handful of
package *names*, so we keep a small translation map instead of per-distro lists:

- `group_vars/all.yml` → `solari_core_packages` — the logical list, the same
  everywhere.
- `group_vars/all.yml` → `solari_pkg_map` — logical name → real name, keyed by
  `ansible_os_family` (`Debian` covers Debian + Ubuntu; `Suse` covers
  openSUSE/SLES). Anything not in the map installs under its own name.

Architecture never changes package names, so it needs no mapping — the same
playbook runs unmodified on x86_64, arm64, and arm32.

## Layout

```
deploy/fleet/ansible/
├── ansible.cfg                 # inventory path, no host-key checking, yaml output, pipelining
├── inventory/hosts.yml         # example inventory: family groups + role groups + dashboard example
├── group_vars/all.yml          # THE shared contract: core packages, pkg map, base config, enroll vars
├── playbooks/site.yml          # base-os -> common-stack -> solarinet-client
├── roles/
│   ├── base-os/                # hostname, tz/locale, admin user+keys, sshd hardening, journald, chrony
│   ├── common-stack/           # installs solari_core_packages mapped via solari_pkg_map
│   └── solarinet-client/       # installs + enrolls + supervises the monitoring agent
└── README.md
```

## Running manually

From `/srv/solari-provision/ansible/` on `benzene`:

```bash
# Whole site against one host:
ansible-playbook -i inventory/hosts.yml playbooks/site.yml --limit pi-porch

# Just re-run one layer with tags:
ansible-playbook -i inventory/hosts.yml playbooks/site.yml --limit pi-porch --tags stack

# Syntax check only (no target contact):
ansible-playbook --syntax-check -i inventory/hosts.yml playbooks/site.yml
```

Only `ansible.builtin` modules are used. (`ansible.posix` is **not** required;
SSH authorized keys are managed with a plain template so no extra collection is
needed.)

## Vars the dashboard sets per host

The dashboard writes these into `inventory/host_vars/<host>.yml` (or an inline
host entry) when it provisions a box. Everything else is inherited from
`group_vars/all.yml`, preserving the "same stack everywhere" contract.

| Var | Purpose |
|-----|---------|
| `ansible_host` | IP reached on the provisioning link |
| `ansible_user` | image's default bootstrap user (e.g. `pi`) |
| `solari_hostname` | final hostname to set |
| `solari_server_url` | `[server] primaryUrl`, e.g. `tls+tcp://solari.example.intranet:7701` |
| `solari_op` | operator id recorded with the enrollment |
| `solari_ca_hint` | optional CA root SPKI fingerprint hint |
| `solari_server_name` / `solari_server_ip` | pinned in `/etc/hosts` (older mbedTLS verifies a DNS-SAN name, not a raw IP) |
| `solari_client_bin_src` | control-node path to the per-arch `solariClient` binary |
| `solari_ca_src` / `solari_cert_src` / `solari_key_src` | control-node paths to enrollment material (`ca.pem` / `node.pem` / `node.key`) |

### Graceful partial provisioning

`solarinet-client` always installs `client.conf` and the systemd unit. If the
agent binary or the three enrollment files are not yet provided, the role prints
a clear debug notice, **enables** the unit for boot, but does **not** start it —
so a half-provisioned host is left clean and resumable. Re-running the play once
the material is in place arms and starts the agent.
