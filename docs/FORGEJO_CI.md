# Forgejo CI

**Updated:** 2026-10-23
**Primary repository:** Forgejo (`jason/Solarinet`)
**Candidate branches:** Forgejo `integrate/phase0-reconcile-20261022`; GitHub mirror `integrate/phase0-reconcile-20261022` after publication

## Decision

Forgejo is the authoritative remote for this project. GitHub is a secondary mirror and can
lag or be unavailable during service incidents; work and verification proceed on Forgejo
first. The Forgejo workflow is maintained as a generated
adaptation of `.github/workflows/ci.yml`; the renderer and parity check are
`tools/ci/render_forgejo_workflow.py`. The GitHub workflow runs that check so changes to the
source CI cannot silently leave Forgejo CI stale.

RackWire is a separate repository. Forgejo keeps it private, so the RackWire
browser and dashboard jobs use the repository secret `RACKWIRE_READ_TOKEN`,
which must have read-only access to `jason/RackWire`. The current GitHub
RackWire repository is anonymously readable, so GitHub jobs can use the
default workflow token when that optional secret is absent. Jobs that do not
need RackWire use a non-recursive checkout.

## Execution contract

The workflow is in `.forgejo/workflows/ci.yml`. It uses a dedicated runner label,
`solarinet-ci-host`, and runs disposable MariaDB/RabbitMQ fixtures as Docker containers
published only on `127.0.0.1`. This preserves the existing integration tests' loopback
requirements and the RabbitMQ upgrade test's Docker access and dynamically published ports.
Tests and their live-SoR guards remain unchanged.

This contract requires a **dedicated, disposable Linux VM** with Docker, `sudo`, and the
Forgejo runner installed in host mode using label `solarinet-ci-host`. Configure runner
capacity to `1`; jobs share fixed loopback ports. Host mode does not isolate job code. Never
register a shared application, database, DNS, workstation, or production host. The VM must
hold no production credentials or writable production mounts and must be reverted or
recreated after every job, since job code can use `sudo` and persist changes across jobs.
The workflow cleans up its named service containers with `if: always()` steps, but that
cleanup does not replace a VM reset.

The host firewall must enforce the runner network boundary independently of the guest:
allow DHCP/DNS to the libvirt gateway, Forgejo on Cesium (`10.1.0.200:3000`), and required
public package/action downloads on TCP 80/443. Deny all other guest-to-host and
guest-to-private/reserved-network traffic, including IPv6 and link-local paths. For
forwarded traffic entering the guest network, permit only established or related replies;
drop other forwarded traffic toward the runner. Do not rely on guest firewall rules because
host-mode jobs have root privileges. Public HTTPS egress remains an exfiltration path for
anything exposed inside the VM, so runner jobs must not receive production or reusable
credentials; use one-job/ephemeral registration where supported and reset the VM after the
job.

The dedicated libvirt network forwards `akoria.net` queries to Xenon's authoritative BIND
at `10.0.0.20` and all other guest DNS queries to the home router at `10.0.0.1`. Benzene's
host resolver points at Xenon's BIND, which does not recurse for public zones; inheriting
that resolver caused Forgejo run 70 to fail before browser tests because `cdn.playwright.dev`
returned NXDOMAIN. The server-job preflight now checks both the Playwright CDN and
`cesium.akoria.net` before browser installation, covering public recursion and internal
Forgejo resolution. Do not change Xenon's production DNS behavior to accommodate CI; the
split resolver remains inside the isolated runner network.

The host network configuration is in `deploy/forgejo-runner/`. Inet input and forwarding
rules enforce the boundary on the dedicated bridge; the fixed VM tap `ci-tap0` is checked
against the live domain XML and included in nftables rules as an additional match. Libvirt
reserves the `vnet`, `vif`, `macvtap`, and `macvlan` prefixes and may ignore a manually
requested target using one of them, so the runner VM definition uses the nonreserved
`ci-tap0` name. The CI regression test checks the guard and deployment files for reserved tap
targets. `install-host-network.sh` disables
libvirt network autostart and enables systemd units that start the network only after the
guard. Stopping the guard leaves its nftables rules installed and stops the dedicated
network. The guard unit does not pull in or start the host `nftables.service`. An installed
drop-in reapplies the CI guard after the host ruleset starts and requests the CI network;
that network dependency starts the guard after nftables and before the network. The network
in turn starts the supervisor. The guard also reloads with nftables reloads and
stops/restarts with the service. Stopping the guard by itself leaves the rules installed;
stopping host `nftables.service` tears down the runner VM and CI network first. The drop-in
checks that the host's configured rules file exists before the service can run its
`flush ruleset` command. These interactions are not fully exercised because
`nftables.service` is disabled on Benzene and `/etc/nftables/rules/main.nft` is absent.

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
Both workflow files run `test_guard_namespace.sh` in the Python integrations job using
disposable network namespaces. It checks HTTPS egress and its reply path, rejects unsolicited
forwarding into the runner network, and confirms that disallowed private HTTPS and SSH egress
hit the guest drop rules. The test passed locally and on Benzene; it will run in that job on
the next push.

**Host audit, 2026-10-22:** Forgejo 15.0.3 is active on Cesium. The installed Forgejo CLI
supports repository-scoped runner registration. No runner process/service was found on
Cesium, Benzene, Xenon, or Chlorine. Benzene has sufficient capacity for a separate VM; its
existing `argon` VM is a Rocky Linux workload and is left untouched. No runner VM or runner
has been provisioned. A dedicated disposable VM on Benzene remains the selected path. The
default libvirt NAT network remains untouched. A recheck on 2026-10-23 found no defined
`solarinet-ci` network (`virsh net-list --all` was empty), despite a stale active oneshot
systemd state. The installer now defines the dedicated network when absent, restarts its
unit, and checks that the bridge is `virbr-ci0` before starting it. The current deployment
state appears below.
Before applying the guard, the host had no active `firewalld`/`nftables` service and the
default libvirt NAT network allowed broad guest egress. A temporary network-namespace probe
verified the new policy: Forgejo TCP 3000, gateway DNS, and public TCP 443 connected; Cesium
MariaDB TCP 3306 and Benzene SSH TCP 22 did not. Stopping the guard service took down the
CI network while leaving the nftables rules installed; restarting the guard and network
restored both. That earlier check had no guest attached. The later 2026-10-23 recheck found
the network definition missing; the current deployment state appears below.

**Tap-name correction, 2026-10-23:** libvirt reserves interface-name prefixes `vnet`, `vif`,
`macvtap`, and `macvlan` and may ignore a requested target using one of them. The runner tap
contract is now `ci-tap0`; the deployment regression test checks the firewall and guest
configuration files for reserved targets. The candidate nft file passed `nft -c` locally and
on Benzene. The `inet` forwarding boundary is enforced on `virbr-ci0`; `ci-tap0` is an
additional match and a checked domain-definition invariant. The forward chain now permits
established/related return packets into the guest and drops other forwarded packets aimed at
the guest network. With no CI guest attached, this guard was installed on Benzene and the
guard service reloaded. Its SHA-256 is
`d5cd5fdcd250ca30ab7b9a4b707d4e28f36c8122137ec2be501457ea850b50e5`. This is the deployed
revision before removal of an unreachable hairpin-drop rule. The candidate hash is
`2040ee96893d2405387a79d0dcde590bb3104aa9c78c9ec69c2d2790e29ca084`; the supervisor
installer replaces the deployed file and reloads the guard. The new isolated
namespace packet test allows HTTPS egress and established replies, and asserts the inbound
forwarding drop and disallowed private HTTPS and SSH egress increment their exact nftables
drop-rule counters. It passed locally and on Benzene. At this checkpoint, a live disposable
guest was attached to the verified `ci-tap0` on `virbr-ci0`; its DHCP and DNS traffic passed
the guard, while public package download and Forgejo connectivity were still unverified. The
later replacement-guest checks below supersede that initial status.

**Disposable runner implementation, 2026-10-23:** the candidate adds a root supervisor on
Benzene, Ubuntu 24.04 cloud-init guests, a 24-hour idle-job wait and a fresh 24-hour job
execution window once Forgejo Runner reports that it fetched a task, a pinned base-image
checksum, a per-cycle token in a temporary cloud-init ISO, a SHA-256-pinned Node.js 24.10.0
runtime for JavaScript actions, and Forgejo v15's `one-job`
ephemeral mode. A fixed runner identifier lets the server-side CLI rotate credentials
without accumulating idle runner records. Registration is repository-scoped to
`jason/Solarinet`; Benzene's SSH key is accepted on Cesium only from Benzene's observed
source address `10.5.2.50`, through a forced command that validates the secret and invokes
that registration command. The live Forgejo `ROOT_URL` is
`http://cesium.akoria.net:3000/`, resolves to `10.1.0.200` from Benzene, and returned
version `15.0.3` there. The SoR inventory and developer reference still call the service
`git.akoria.net`, which currently has no DNS answer; Actions checkout is expected to use the
live `ROOT_URL`, whose endpoint is allowed by the guard. The guest uses `ci-tap0`,
and the supervisor checks the libvirt XML before waiting for a job. Benzene's existing base
image matches the pinned SHA-256. The VM exposes a serial console for first-boot logs; guest
login remains locked and the ttyS0 serial getty is masked to keep its login prompt out of
the status stream. A bounded host-side serial parser retains at most 4 MiB of log output
and extracts CRLF-safe job-start and exit markers separately. Fixed secret-free stage markers
show guest bootstrap progress. The guest reports job start
and one-job exit status over its serial console using the runner account's unrestricted
passwordless sudo, which host-mode workflow jobs already require. The supervisor treats
missing or nonzero status as a failed cycle and backs off before
registering another runner. Stopping the supervisor/network tears down the disposable VM
and cancels any active job. Local tests cover cloud-init rendering, credential shape, shell
syntax, forced-command installation, tap selection, and bounded serial capture under output
flood. At this implementation checkpoint, first-boot completion, serial-result capture,
complete Forgejo job execution, and teardown had not yet been exercised; the deployment
results below supersede this earlier status. The source-restricted forced key is installed on
Cesium; its installer checked Jason's noninteractive Forgejo CLI access before changing
`authorized_keys`.
The first independent review found serial-log growth and CRLF-marker issues; both are fixed
and covered by a serial flood/marker test. A second cross-lab review found no deployment
blockers in network ordering, registration, firewall, or bounded serial parsing.

**Deployment, 2026-10-23:** Benzene now has the candidate firewall hash
`2040ee96893d2405387a79d0dcde590bb3104aa9c78c9ec69c2d2790e29ca084`, an active isolated
`solarinet-ci` network with bridge `virbr-ci0` and autostart disabled, and an active supervisor.
The live disposable VM is named `solarinet-ci-runner`, has the verified `ci-tap0` attachment,
and receives a DHCP lease on `192.168.123.0/24`. The first boot stopped after the cloud-init
`bootcmd` marker and before DNS or TCP traffic. The next `bootcmd` action was `systemd-run`
creating an in-guest deadline timer; removing that call allowed cloud-init to reach the guest
bootstrap. The supervisor now enforces a 45-minute boot deadline and resets it to 24 hours
when the runner fetches a task. Package update and install run in the bootstrap script with
separate serial markers and bounded apt retries/timeouts.

The replacement guest reached package update/install, verified package installation, fetched
the SHA-256-pinned Forgejo runner and Node.js runtime, started Docker, connected to Forgejo,
and fetched jobs. Host firewall counters recorded HTTPS egress and Forgejo traffic on TCP
3000; the guest remains isolated from other private/reserved destinations. Run `57`'s RabbitMQ
3.13.7 and 4.2.9 SoR tasks completed successfully, including checkout, disposable
database/broker setup, migrations, rollback/recovery, DNS-chain checks, and snapshot recovery.
Runs `63` and `65` then exercised later commits on the candidate branch.

**Latest observed run, 2026-10-23:** Forgejo run `65` used commit
`f0508d93bcbe5b64ad8347977084710be8c63546`. The ephemeral runner registered under the
workflow's `solarinet-ci-host` label and fetched the job. The Python integration job failed at
the Opie MCP subprocess step because the child interpreter could not load the dynamic Python
runtime from the Actions tool cache. The runner's constrained subprocess environment omits
`LD_LIBRARY_PATH`; the candidate fix now passes that path alongside the MCP package path.
The exact Opie suite passes 9 tests under Python 3.12 in a disposable container. The SoR
RabbitMQ 3.13.7, 4.2.9, 4.3.6, and 4.4.0 jobs passed, including the 4.3.7 upgrade intermediate
and pre-4.4 snapshot restore. The run was superseded before the protocol-fuzz, core, server,
RackWire browser, dashboard, panel, and cross-build jobs started; those jobs were cancelled.
The dashboard gate remains unverified, so this was not a complete matrix.

The candidate now adds `pkg-config` and OpenSSL development headers to the panel workflow
dependencies. The panel PTY test double now waits for five consecutive empty-queue checks
before closing its master descriptor, preventing the intermittent final-ACK `EIO` observed
under repetition. The panel smoke passed 10 consecutive local runs after that change, and the
daemon listener/codec suite passed. The firmware host suite passed 107 tests and the
page/firmware parity suite passed 59 checks locally. The firmware build used `TMPDIR=/var/tmp`
because `/tmp` returned `EDQUOT` during ASAN compilation despite available tmpfs space; the exact per-user limit remains unknown.

**Follow-up runs, 2026-10-23:** Forgejo's stored numeric statuses distinguish success (`1`),
failure (`2`), cancelled (`3`), skipped (`4`), waiting (`5`), running (`6`), and blocked (`7`)
([Forgejo 15.0.3 status enum](https://codeberg.org/forgejo/forgejo/raw/tag/v15.0.3/models/actions/status.go)).
Run `66` used commit `9dd7d9b9f0b6b15f41f63ee1f2ca1b27843e1988`; only Python completed successfully
before the next push cancelled the run. Runs `67` and `68` were cancelled before their jobs
started when later commits were pushed. Their dashboard jobs were cancelled, not skipped.
Run `69`, commit `65cbb4a244a4ae3cfdfeba444feb4148921a90bc`, terminated with failure (status `2`).
The independent SoR, Python, protocol-fuzz, core, panel, and cross jobs passed. The `server` and
`rackwire-browser` jobs failed during recursive checkout because the per-repository workflow
token received HTTP 403 for private Forgejo repository `jason/RackWire`; no server or browser
tests ran. The dashboard remained blocked behind `needs: server`. The subsequent candidate
removed the recursive submodule checkout from the server job on the assumption that it did not
need RackWire, while passing the existing Forgejo secret `RACKWIRE_READ_TOKEN` to the RackWire
and dashboard checkouts. Run 75 later disproved that assumption: the server's Discovery browser
acceptance loads the planner at `/rackwire/index.html`. The server job now needs the same
token-scoped recursive submodule checkout. The secret name is configured; its value was not read.
GitHub's RackWire repository is anonymously readable, so GitHub's default workflow token can read
the same relative submodule URL there. Forgejo's dependency condition remains `needs: server`; no
diagnostic `if: true` override is present. Forgejo documents that `needs` prerequisites must
complete successfully before a dependent job runs
([Actions reference](https://forgejo.org/docs/v15.0/user/actions/reference/)).

Run `70` exercised commit `69997a2af27b` after the checkout fix. Seven jobs passed,
including the SoR delivery and RabbitMQ upgrade matrix. The server job reached the
Playwright browser download but failed because the guest resolver returned NXDOMAIN for
`cdn.playwright.dev`; the dashboard dependent was skipped and the remaining queued jobs did
not run. Read-only DNS checks on Benzene confirmed that its configured resolver
`10.0.0.20` (Xenon's authoritative BIND) returns authoritative NXDOMAIN for this public
name, while router resolver `10.0.0.1` returns valid CDN records. The fix adds split DNS only
to the isolated `solarinet-ci` libvirt network: `akoria.net` goes to Xenon and all other
queries go to the router. It retains the network UUID so `virsh net-define` updates the
existing persistent network, and checks both public CDN and internal Forgejo resolution
before browser installation. The XML validates against Benzene's libvirt schema and 13
local runner-supervisor tests pass. After confirming no Forgejo task was active, the network
was restarted; live queries through `192.168.123.1` resolved both `cdn.playwright.dev` and
`cesium.akoria.net`. The isolated runner network is active again. Fix commit `11cf15d69c9a`
is published on the integration branch. Follow-on commit `b4c0fa71faed` adds the expanded
scanner/RackWire port-map browser test and its updated HTML/PDF evidence. The current branch
head is confirmed on Forgejo. Run `71` was active for `11cf15d69c9a` at 2026-10-23 09:34 UTC:
its first five jobs had passed and the SCP fuzz job was running; the server job containing
the new DNS preflight had not started. **Hosted CI status for `b4c0fa7` and the later
completion status of run 71 have not been verified.**

Unauthenticated requests to the repository Actions API and UI returned 404 because the
repository is private. Forgejo run `65` confirms that Actions are enabled and dispatch jobs for
this repository. The server has no explicit `[actions]` override in `app.ini` (Forgejo v15
defaults Actions to enabled).

**Run 74, 2026-10-23:** commit `60e4245effc02d9e0b6734447657e2edcf91edfe` reached the full
server job. Python integrations, RabbitMQ 3.13.7/4.2.9/4.3.6/4.4.0, SCP fuzz, core, RackWire
browser, and panel jobs passed. The server job failed at `ShellCheck shell scripts including
sourced helpers`: SC2034 in `deploy/forgejo-runner/test_guard_namespace.sh`, SC2154 in
`deploy/forgejo-runner/guest-runner.sh`, and SC2068 in three invocations in
`tests/netdb/test_sor_migration_rollback.sh`. The dashboard job was skipped because it depends
on the server job; the cross jobs remained waiting after the run failed. A reviewed local fix
addresses those findings and passes the tree-wide ShellCheck command. **UNVERIFIED:** hosted CI
for that fix and all downstream server, dashboard, and cross jobs on a passing run.

Run 74 exposed why green results from isolated earlier runs are insufficient for promotion:
the corrected DNS path allowed the server job to advance to ShellCheck, where it found issues.
The full Forgejo matrix remains a release gate. GitHub run `39898481633` completed successfully
for older commit `853b0bc6007f274bfc281d62ad4d0a531e12f74c` on 2026-10-23; GitHub remains a
secondary mirror and verification path when available.

**Run 75, 2026-10-23:** commit `ff81283c309743d15467e91f0a1cedb35324a452` cleared the server
ShellCheck gate, then failed in the Discovery browser acceptance because its RackWire iframe
returned 404 for `/rackwire/index.html`. The run's Python integrations, all four RabbitMQ
compatibility jobs, protocol fuzz, core, panel, and dedicated RackWire browser jobs passed. The
server job had also passed ShellCheck, the ASAN/UBSAN build, DNS generation and rollback checks,
inventory/configuration tests, schema checks, ctest, HTTPS enrollment, two-process failover, and
scan worker/API/SoR adoption before the browser failure. The dashboard job was skipped behind
the failed server dependency. The `cross linux-arm64` job later completed successfully; the arm32
and Windows cross jobs remained waiting after the run failed. The run's overall status was failure.
The server workflow checkout had not initialized `dashboard/public/rackwire`, although the
browser job using that planner did. The candidate fix adds recursive submodule checkout and the
existing read-scoped token to the server job in both Forgejo and GitHub workflows. Local workflow
render/parity validation passes, and Claude cross-lab review passed the workflow fix. **UNVERIFIED:**
hosted CI has not yet exercised the restored checkout, and downstream dashboard/cross completion
on a passing server run remains unverified.

**Run 76, 2026-10-23:** commit `63fd675cccc2150815c26d54fd3244fa759afca2` completed successfully
(status `1`). All 14 jobs passed, including the server Discovery browser with recursive RackWire
checkout, dashboard, RackWire browser, all four RabbitMQ compatibility jobs, protocol fuzz, core,
panel, Python integrations, and Linux ARM64, Linux ARM32, and Windows cross builds. The dashboard
job also passed scanner import through authenticated HTTP routes, scanner/RackWire browser
integration, packaged release-tree browser acceptance, and deployment/rollback contracts. This
verifies the CI checkout fix on Forgejo. **UNVERIFIED:** the GitHub workflow run for this commit;
main promotion, synchronized release publication, and service deployment remain behind the wider
RC review.

## Forgejo references

- [Actions administrator guide](https://forgejo.org/docs/v15.0/admin/actions/) — runner
  installation, registration, and security model.
- [Runner configuration and labels](https://forgejo.org/docs/v15.0/admin/actions/configuration/)
  — host execution labels and runner capacity.
- [Docker access from Actions](https://forgejo.org/docs/v15.0/admin/actions/docker-access/)
  — Docker socket access grants substantial authority over the runner host.
- [Actions security guidance](https://forgejo.org/docs/v15.0/admin/actions/security/)
  — host execution is not job isolation.
- [Forgejo v15.0 release](https://forgejo.org/2026-04-release-v15-0/) — ephemeral runner
  creation and the `one-job --token-url file://...` flow.
- [Actions service reference](https://forgejo.org/docs/v15.0/user/actions/reference/)
  — service-container networking behavior.
- [Artifact action guidance](https://forgejo.org/docs/v15.0/user/actions/advanced-features/)
  — compatible artifact action versions.
