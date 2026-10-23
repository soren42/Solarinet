# UniFi integration

`unifipolld.py` samples controller device statistics for the monitoring view.
`unifi_network_sor.py` reads network configuration into the SoR; it does not
write to the controller.

## Network import contract

The importer uses the controller's `/proxy/network/integration/v1` API through
the shared `UniFiClient`. It fetches every site and its paginated network
overview, then fetches each network's detail record before it opens a database
transaction. An incomplete site/network response aborts the whole snapshot.

It records VLANs, networks, IPv4 subnets, DHCP pools, and stable
`external_refs`, with `source_id=unifi` and machine assertion timestamps. It
rejects ambiguous name, VLAN, CIDR, or external-reference conflicts instead of
reassigning rows owned by another SoR source. Re-importing a complete site
snapshot is idempotent; source-owned rows missing from a site snapshot are
soft-deleted. A site that is absent from the API response is not pruned.

Imported networks default to `allocator_eligible=0`. Only an explicit operator
approval enables static allocation. Changes to network identity/configuration,
gateway/subnet, or DHCP pool revoke that approval; the allocator independently
rejects disabled, deleted, or unapproved networks and excludes DHCP ranges.

The dashboard exposes this review at **Provisioning → Static address
allocation**. Its API reads the SoR network, VLAN, subnet, gateway, and DHCP
facts; approval is operator-gated, requires exactly one live IPv4 subnet, and
records `allocator_approved_by` / `allocator_approved_at`. Approval rejects
overlapping live IPv4 prefixes. Imports revoke approvals when a changed prefix
overlaps another network, and the allocator rechecks approval under its subnet
lock. Revoking or a source-reported configuration change clears that audit
metadata. The route updates only the SoR; it never changes UniFi controller
configuration.

## Run

The normal mode is read-only dry-run. Configure the controller and database
environment from the existing protected files; never copy their values into
the command line, shell history, or project files. TLS verification requires an
trusted controller CA bundle (`UNIFI_CA_FILE`). The importer fails closed when
that file is missing and does not disable certificate checks. When the gateway
hostname resolves to a different service, set `UNIFI_CONNECT_IP` to the
controller's numeric address. The HTTPS URL hostname remains in place for SNI,
certificate hostname validation, and the HTTP Host header; only the socket
destination is pinned. The override requires a literal IPv4 or IPv6 address
and bypasses proxy environment settings for this connection.

For the current gateway, the publicly trusted certificate covers `akoria.net`
while local DNS for that name points at Xenon. A verified configuration can
therefore use:

```sh
UNIFI_GW=https://akoria.net
UNIFI_CONNECT_IP=10.0.0.1
UNIFI_CA_FILE=/etc/ssl/certs/ca-certificates.crt
```

Keep these values in the protected runtime environment file. This avoids
changing local DNS or weakening TLS verification.

```sh
set -a
. run/unifi.env
set +a
python3 deploy/unifi/unifi_network_sor.py
```

Review the output, then `--apply` writes the fetched snapshot to the SoR using
the `[sor]` connection in `deploy/sorsync/sorsync.conf`. It never changes
controller configuration. The writes are transactional and serialized with a
MariaDB advisory lock.

```sh
python3 deploy/unifi/unifi_network_sor.py --apply
```

Before application, deploy and validate SoR migrations `024`–`027` on the
writable primary, including the backup/restore and rollback gates in the
[Provisioning Plan §14](../../docs/SolariNet_Provisioning_Plan.html#s14).
Migrations `025` and `026` provide the network approval fields and CDC for
imported VLANs, DHCP ranges, and external references. Imported networks remain
ineligible for static allocation until an operator approves them.

## Persistent import service

The systemd oneshot imports a full snapshot every 15 minutes. Install it only
after the SoR schema and read-only dry-run have been accepted. The timer is not
enabled by these files.

Create the runtime directory and install the two configuration examples. Fill
the UniFi API key and scoped SoR database password in the protected copies;
keep those values out of git and command history:

```sh
sudo install -d -o root -g jason -m 0750 /etc/solarinet
sudo install -o root -g jason -m 0640 \
  deploy/unifi/unifi-network-sor.env.example \
  /etc/solarinet/unifi-network-sor.env
sudo install -o root -g jason -m 0640 \
  deploy/unifi/unifi-network-sor.conf.example \
  /etc/solarinet/unifi-network-sor.conf
sudoedit /etc/solarinet/unifi-network-sor.env
sudoedit /etc/solarinet/unifi-network-sor.conf
```

The SoR account `solarinet_unifi_import`@`10.0.0.20` must be provisioned with a
unique secret outside this repository. Apply
`deploy/unifi/sor-unifi-grants.sql` on the writable primary for its exact
per-table privileges: SELECT on source/entity lookup, and SELECT/INSERT/UPDATE
on imported facts and external references. It has no DELETE, DDL, trigger, or
global/schema-wide privilege. After the DBA has securely created that account,
apply its grants on Cesium with
`sudo mariadb < deploy/unifi/sor-unifi-grants.sql`. CI exercises an import and
replay with that grant set against disposable MariaDB.

The poller and importer share the pinned Python environment:

```sh
python3 -m venv /home/jason/Code/Solarinet/deploy/unifi/.venv
/home/jason/Code/Solarinet/deploy/unifi/.venv/bin/pip install \
  -r /home/jason/Code/Solarinet/deploy/unifi/requirements.txt
```

Install and parse-check the units, then run a manual dry-run before enabling
the writer:

```sh
sudo install -o root -g root -m 0644 deploy/unifi/unifi-network-sor.service \
  /etc/systemd/system/unifi-network-sor.service
sudo install -o root -g root -m 0644 deploy/unifi/unifi-network-sor.timer \
  /etc/systemd/system/unifi-network-sor.timer
sudo systemd-analyze verify /etc/systemd/system/unifi-network-sor.service \
  /etc/systemd/system/unifi-network-sor.timer
sudo systemctl daemon-reload
set -a
. /etc/solarinet/unifi-network-sor.env
set +a
SORSYNC_CONF=/etc/solarinet/unifi-network-sor.conf \
  /home/jason/Code/Solarinet/deploy/unifi/.venv/bin/python3 \
  /home/jason/Code/Solarinet/deploy/unifi/unifi_network_sor.py
```

The manual run defaults to dry-run. After reviewing the complete snapshot and
confirming the SoR backup, migration, and rollback gates, start one import with
`sudo systemctl start unifi-network-sor.service`. Check
`systemctl status unifi-network-sor.service` and its journal. Enable periodic
imports only after that apply and read-only SoR verification pass:

```sh
sudo systemctl enable --now unifi-network-sor.timer
systemctl list-timers unifi-network-sor.timer
```

The service runs as `jason` without Linux capabilities, with a read-only
filesystem/home view and private temporary storage. It contacts the UniFi
gateway and SoR only; it never changes controller configuration. Timer
activation is a separate deployment step and is not part of installing the
unit files.

## Verification

`test_unifi_network_sor.py` covers normalization, API pagination/detail paths,
invalid DHCP ranges, idempotency, source-conflict protection, stale-row
soft-deletion, and allocator approval invalidation. The database cases require
a disposable migrated MariaDB schema. `tests/netdb/test_unifi_network_cdc.sh`
checks transactional insert/update/delete publication for imported fact
tables.
