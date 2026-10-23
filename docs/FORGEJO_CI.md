# Forgejo CI

**Updated:** 2026-10-22  
**Primary repository:** Forgejo (`jason/Solarinet`)  
**Candidate branch:** `integrate/stability-20261020`

## Decision

Forgejo is the authoritative remote for this project. GitHub is a secondary mirror and can
lag or be unavailable during service incidents; work and verification proceed on Forgejo
first. The Forgejo workflow is maintained as a generated
adaptation of `.github/workflows/ci.yml`; the renderer and parity check are
`tools/ci/render_forgejo_workflow.py`. The GitHub workflow runs that check so changes to the
source CI cannot silently leave Forgejo CI stale.

## Execution contract

The workflow is in `.forgejo/workflows/ci.yml`. It uses a dedicated runner label,
`solarinet-ci-host`, and runs disposable MariaDB/RabbitMQ fixtures as Docker containers
published only on `127.0.0.1`. This preserves the existing integration tests' loopback
requirements and the RabbitMQ upgrade test's Docker access and dynamically published ports.
Tests and their live-SoR guards remain unchanged.

This contract requires a **dedicated, disposable Linux VM** with Docker, `sudo`, and the
Forgejo runner installed in host mode using label `solarinet-ci-host:host`. Configure runner
capacity to `1`; jobs share fixed loopback ports. Host mode does not isolate job code. Never
register a shared application, database, DNS, workstation, or production host. The VM must
hold no production credentials or writable production mounts and must be reverted or
recreated after every job, since job code can use `sudo` and persist changes across jobs.
The workflow cleans up its named service containers with `if: always()` steps, but that
cleanup does not replace a VM reset.

The host firewall must enforce the runner network boundary independently of the guest:
allow DHCP/DNS to the libvirt gateway, Forgejo on Cesium (`10.1.0.200:3000`), and required
public package/action downloads on TCP 80/443. Deny all other guest-to-host and
guest-to-private/reserved-network traffic, including IPv6 and link-local paths. Permit only
reply traffic for guest-initiated connections. Do not rely on guest firewall rules because
host-mode jobs have root privileges. Public HTTPS egress remains an exfiltration path for
anything exposed inside the VM, so runner jobs must not receive production or reusable
credentials; use one-job/ephemeral registration where supported and reset the VM after the
job.

The host network configuration is in `deploy/forgejo-runner/`. Its guard filters both the
dedicated bridge and pinned VM tap (`ci-tap0`). Libvirt reserves the `vnet`, `vif`, `macvtap`,
and `macvlan` prefixes and may ignore a manually requested target using one of them, so the
runner VM definition must use the nonreserved `ci-tap0` name. The CI regression test checks
the guard and deployment files for reserved tap targets. `install-host-network.sh` disables
libvirt network autostart and enables systemd units that start the network only after the
guard. Stopping the guard leaves its nftables rules installed and stops the dedicated
network. The systemd units also tie host `nftables.service` reloads to a guard reload and
stop/restart events to CI network teardown; that interaction
is configured but remains unexercised because `nftables.service` is disabled on Benzene.

Native macOS coverage remains GitHub-only. The Linux Forgejo workflow does not claim that
gate has run.

## Verification and rollout state

The workflow renderer's `--check` verifies generated-file parity. Local checks also parse the
YAML, verify checkout options and loopback fixture structure, and run `git diff --check`.
Forgejo Runner v13.2.0's own `validate --directory` command also accepted the workflow; the
downloaded binary matched Forgejo's published SHA-256 file. Forgejo documents this validator
as available from Runner v9 onward in its [Actions troubleshooting guide](https://forgejo.org/docs/v15.0/user/actions/troubleshooting/).
These checks validate schema and source shape; they do not prove that a compatible runner is
registered, that host mode executes successfully, or that the full CI matrix passes there.

**Host audit, 2026-10-22:** Forgejo 15.0.3 is active on Cesium. The installed Forgejo CLI
supports repository-scoped runner registration. No runner process/service was found on
Cesium, Benzene, Xenon, or Chlorine. Benzene has sufficient capacity for a separate VM; its
existing `argon` VM is a Rocky Linux workload and is left untouched. No runner VM or runner
has been provisioned. A dedicated disposable VM on Benzene remains the selected path. The
default libvirt NAT network remains untouched. A separate `solarinet-ci` NAT network is
active with libvirt autostart disabled; its nftables guard is loaded and enabled at boot.
Before applying the guard, the host had no active `firewalld`/`nftables` service and the
default libvirt NAT network allowed broad guest egress. A temporary network-namespace probe
verified the new policy: Forgejo TCP 3000, gateway DNS, and public TCP 443 connected; Cesium
MariaDB TCP 3306 and Benzene SSH TCP 22 did not. Stopping the guard service took down the
CI network while leaving the nftables rules installed; restarting the guard and network
restored both. The CI network currently has no attached guest. The runner VM must use this
network and a tested per-job reset mechanism before registration.

Unauthenticated requests to the repository Actions API and UI returned 404 because the
repository is private; this does not establish whether the repository Actions unit is
enabled. The server has no explicit `[actions]` override in `app.ini` (Forgejo v15 defaults
Actions to enabled), but repository unit state still needs authenticated verification.

Therefore Forgejo CI execution is **UNVERIFIED**. Do not describe this workflow as a green
hosted gate until a dedicated runner has run the complete matrix. GitHub remains a secondary
mirror and verification path when its service is healthy; its current Actions incident does
not change Forgejo's primary status.

## Forgejo references

- [Actions administrator guide](https://forgejo.org/docs/v15.0/admin/actions/) — runner
  installation, registration, and security model.
- [Runner configuration and labels](https://forgejo.org/docs/v15.0/admin/actions/configuration/)
  — host execution labels and runner capacity.
- [Docker access from Actions](https://forgejo.org/docs/v15.0/admin/actions/docker-access/)
  — Docker socket access grants substantial authority over the runner host.
- [Actions security guidance](https://forgejo.org/docs/v15.0/admin/actions/security/)
  — host execution is not job isolation.
- [Actions service reference](https://forgejo.org/docs/v15.0/user/actions/reference/)
  — service-container networking behavior.
- [Artifact action guidance](https://forgejo.org/docs/v15.0/user/actions/advanced-features/)
  — compatible artifact action versions.
