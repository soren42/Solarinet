# SolariNet Incident Post-Mortem — cesium Forgejo outage, 2026-07-06/07

*Written 2026-07-07. Severity: **high** (26h silent outage of the git service) but blast
radius: **low** (no SoR impact, no data loss). This document is the motivating case for
`docs/design/HOST_HEALTH_CONTRACT.md` (branch `feat/host-health-monitoring`) — read that
alongside this one; the contract is the fix, this is why it exists.*

## Summary

cesium (Dell R410) runs two independent services on two independent disks: the MariaDB
System of Record on internal PERC RAID (`/dev/sda2`), and Forgejo, whose data volume was
a bare SATA SSD in a USB (UAS) enclosure — no drive caddy was ever fitted for it. At
approximately **2026-07-06 02:45**, the USB enclosure dropped off the bus. The kernel
gave up on it (`uas_eh_host_reset_handler FAILED err -19`), `/dev/sde` "has gone missing,"
and the `cesium-git` btrfs filesystem forced an **emergency read-only shutdown** rather
than risk corruption. Every subsequent Forgejo request hit `EIO` (`database disk image is
malformed`, `disk I/O error`) and the Forgejo web UI returned **HTTP 500 on every request**
for roughly the next 26 hours.

SolariNet's own monitor did not notice. The `:3000` check on cesium was a bare `tcp`
probe: it opens a socket, sees Forgejo's listener still accepting connections (Forgejo
itself hadn't crashed, it was just returning 500 to everything), and reports OK. There was
no host-local telemetry at all — no filesystem-readonly flag, no missing-block-device
check, no SMART, no failed-unit scan, no dmesg watch — and no alerting path wired for a
host-level fault even if one had been observed. The outage was found by hand, by a human
noticing Forgejo was down, not by the monitoring system that exists specifically to catch
this.

The SoR was never at risk: it lives on a completely different, healthy disk on the same
box. No repository data was lost — the only repo in Forgejo at the time was an empty
`QuakeKit` scaffold, and the migration off Forgejo/USB onto durable storage had not
happened yet. This incident is the reason it needs to happen before anything real lives
there.

## Timeline (UTC, approximate)

| When | Event |
|---|---|
| 2026-07-06 ~02:45 | USB (UAS) enclosure holding the Forgejo data disk drops off `scsi host7`. Kernel logs `uas_eh_host_reset_handler FAILED err -19`; `/dev/sde` reported missing. |
| ~02:45 | `cesium-git` (btrfs, label `cesium-git`) forces an emergency read-only remount to avoid writing to a filesystem it can no longer trust. |
| ~02:45 onward | Every Forgejo request returns HTTP 500. Errors in the Forgejo/sqlite path read `database disk image is malformed` and `disk I/O error` — consistent with the underlying block device vanishing mid-write, not with actual on-disk corruption (btrfs device stats stayed at **0**, no checksum errors — see Root Cause). |
| ~02:45–~04:45 (26h) | Monitor's `cesium:3000` check stays **green** throughout. It is a bare `tcp` probe: it only confirms Forgejo's listen socket accepts a TCP connection, which it does — Forgejo is alive, just erroring on every request. No alert fires because there is nothing watching for this class of failure. |
| 2026-07-07 (~04:45, ~26h after onset) | Operator finds the outage manually (opens Forgejo, gets a 500) and diagnoses via host inspection: `dmesg`, `btrfs dev stats`, `lsblk` — none of which SolariNet was collecting or would have surfaced automatically. |
| 2026-07-07 | Root cause confirmed as the USB/UAS disk failure, not the MariaDB SoR (verified untouched). Post-mortem + remediation work (this document, `HOST_HEALTH_CONTRACT.md`) begun same day. |

## Root cause (hardware)

The proximate hardware fault: a SATA SSD in a **USB-attached UAS enclosure**, with no
drive caddy, dropped off the SCSI/USB bus. This is a known-bad pattern for "infra that
matters" — USB enclosures are consumer-grade, UAS resets are common under any sustained
I/O load or marginal cabling/power, and unlike a backplane-attached disk there is no
hot-swap caddy, no dedicated power rail, and no controller-level fault isolation. When
the bus reset failed (`err -19`, ENODEV), the kernel had no recovery path and the device
simply vanished from `/sys/block`.

btrfs did exactly the right thing here: it detected it could no longer reach its device
and forced itself read-only rather than risk writing anything further or reporting false
success. Notably, **btrfs device stats stayed at 0 and there were no checksum errors** —
this was not silent bitrot or a write-hole; it was a clean "I lost the device" halt. The
`EIO`/"database disk image is malformed" messages Forgejo produced are a downstream
symptom of the filesystem beneath its sqlite-backed data going read-only mid-operation on
NOCOW files (Forgejo's sqlite db is typically NOCOW, so it wasn't protected by btrfs
checksums the way a normal file would be) — not evidence that data was actually corrupted
on disk. This distinction matters for the "what was lost" section below.

**Root cause, one line:** infrastructure-critical storage was placed on a USB/UAS
enclosure with no caddy, a class of hardware that is not appropriate for anything that
isn't disposable/scratch, and it failed in exactly the way that class of hardware fails.

## Detection failure (the actual point of this post-mortem)

The hardware fault is boring and, frankly, expected of USB-attached storage. What is not
acceptable is that **SolariNet's monitoring — the system built for exactly this job — ran
for 26 hours without noticing.** That failure decomposes into three independent, stacked
gaps, each of which alone would have been enough to catch this if the other two hadn't
also been missing:

1. **The check measured the wrong thing.** `cesium:3000` was configured as a bare `tcp`
   probe. A TCP probe only proves a process is listening on a port; it says nothing about
   whether that process can actually do its job. Forgejo's listener stayed up the entire
   time — it was accepting connections and answering with 500 on every one of them — so
   the one signal SolariNet was collecting was, technically, true and useless. **Port-open
   is not the same thing as healthy.**

2. **There was zero host-local health telemetry.** Even if the app-layer check had also
   been green (or absent), the underlying condition — a filesystem forced read-only, a
   block device that vanished, degrading SMART, failed systemd units, critical kernel
   log lines — was in principle observable *from the host itself*, independent of
   whatever application happens to be running there. SolariNet collected none of it.
   There was no `fsReadonlyCount`, no missing-block-device baseline, no SMART poll, no
   failed-unit scan, no dmesg watch. The host was screaming (`uas_eh_host_reset_handler
   FAILED`, "has gone missing," emergency read-only remount, EIO on every syscall) and
   nothing was listening.

3. **There was no alert path for a host fault, even a hypothetical one.** Suppose (2) had
   existed and detected the fs-readonly condition — there was still no wiring from "a
   metric crossed a threshold" to "a human gets notified." No alert rule, no bridge to
   the message bus, no notification channel for this class of event. Detection without an
   alert path is just a dashboard nobody is looking at 26 hours a day.

Any one of these three would have turned a 26-hour silent outage into a page within
minutes of onset. All three were simultaneously absent, which is why a human had to find
this by hand.

## Blast radius / what was safe

- **MariaDB System of Record** (`sor` DB, `/dev/sda2`, internal PERC RAID) — **completely
  unaffected**. Different disk, different controller, different filesystem. Verified
  healthy throughout and after the incident.
- **SoR replica** (benzene), **DNS rendering from SoR**, **message queue**, **notify
  service**, **dashboard**, **monitor fleet itself** — all on other hosts, all
  unaffected. This was a single-host, single-service (well, single-filesystem) incident.
- **Every other cesium service/filesystem not on the affected USB disk** — unaffected.

## What was NOT lost

- The only repository present in Forgejo at incident time was an **empty `QuakeKit`
  scaffold** — no commits of consequence, nothing that hadn't already been pushed
  elsewhere (this repo included).
- **No other repositories existed on this Forgejo instance yet** — the planned migration
  of real repos onto Forgejo had **not yet happened**. This incident occurred, in effect,
  in the best possible window: before there was anything on it worth losing.
- btrfs device stats and checksum state indicate this was a clean "lost the device" halt,
  not silent write corruption — so even the data that *was* there was not scrambled, it
  was simply inaccessible for the duration.
- Net data loss from this incident: **effectively zero.** The 26-hour outage was a
  detection and availability failure, not a data-integrity failure.

## Remediation

This incident is the direct motivation for `docs/design/HOST_HEALTH_CONTRACT.md`
(branch `feat/host-health-monitoring`), which specifies the fix as five parallel,
non-overlapping work units:

- **Host-local health telemetry** (contract §1–§2): a new `solariHostHealth` wire struct
  (fs-readonly count + list, missing-block-device count, SMART fail count + list, failed
  systemd unit count + list, dmesg-critical count + latest sample) collected by the
  client's platform-abstraction layer every report cycle and shipped to the server —
  independent of whatever application happens to be running on the host. This directly
  targets gap (2): the fs-readonly and missing-block-device collectors are written
  specifically to catch *this exact failure mode* (a normally-rw mount going read-only, a
  baselined block device disappearing).
- **HTTP status-aware probing** (contract §5): `appCheckHttp()` now accepts an expected
  status/class (`path|status`, e.g. `/|2xx` or `/health/ready|200`) and fails the probe on
  mismatch. Forgejo's `:3000` check is being reclassified from bare `tcp` to `http` with a
  status expectation, so a 500-on-every-request future incident **fails the probe
  immediately** instead of reading as healthy. This is the direct fix for gap (1).
- **Alert rules on the new metrics** (contract §4): `health.fsReadonly`,
  `health.blockDevMissing`, and `health.smartFail` are seeded as **crit**, sustained
  immediately (no debounce window) — the class of failure this incident represents is not
  one to wait out. `health.failedUnits` and `health.dmesgCrit` are seeded **warn**.
- **Alert → MQ → Apple notification bridge, plus a dead-man's-switch** (contract §6): a
  new `deploy/alertbridge/` service publishes alert events onto the existing RabbitMQ
  `notify.events` path (already wired to the Apple/iMessage channel per
  `feat(notify): native Apple notification channel`), closing gap (3). The dead-man's-switch
  is the deliberate answer to "what if the monitoring pipeline itself goes quiet": if a
  node that was reporting stops reporting for more than 3× its sample interval, the bridge
  emits a synthetic crit itself — so cesium's *silence* would have been the alert, even if
  every other layer had somehow also failed to fire.
- **Backups** (contract §7): a nightly `solari-backup.sh` + systemd timer, on cesium
  dumping both the `sor` DB and (once Forgejo is relocated) a `forgejo dump`, to a
  **different spindle** (`/data/backups` on `sdb1`, not the sda2 datadir) — so a future
  incident, even a real one with actual data loss, has a recovery path that doesn't depend
  on the disk that just failed.

## Lessons

- **Port-open is not healthy.** A TCP-accept check on an app port validates only that a
  process is listening, not that it is functioning. Any service capable of returning an
  error status on a healthy-looking connection needs an app-layer probe with an expected
  result, not a bare reachability check. This generalizes beyond Forgejo — every `:3000`,
  `:9000`, `:8080` style adopted service in the fleet should be status-aware, which is why
  the contract widens the port→probeType defaults (80/443/3000/8080/9000 → `http`) rather
  than special-casing cesium alone.
- **Monitor the monitor.** Detection coverage and alert-path coverage are two separate
  failure modes, and a system can have neither, one, or both — this incident had neither
  for the specific condition, and would have had no delivery path even if detection had
  existed. The dead-man's-switch exists because "the thing that watches went quiet" is
  itself a signal, and the previous design had no way to express that.
- **Don't run infrastructure that matters on USB.** A SATA SSD in a caddy-less USB/UAS
  enclosure is fine for scratch space or a throwaway scaffold; it is not an acceptable
  home for a service with any real data. The remediation plan explicitly calls for
  relocating Forgejo's data off USB onto durable, properly-attached storage (and backing
  it up to a *different* spindle) before any real repository migration happens — this
  incident is the reason that ordering is now a hard requirement, not a nice-to-have.
- **Backups and `/var` are not covered by any snapshot regime today.** This incident
  happened to hit an empty scaffold repo, which is luck, not process. There is currently
  no backup for Forgejo (there wasn't anything to protect yet) and no snapshotting of
  `/var` state generally on these hosts. Unit E's nightly backup service is the first step
  of closing that gap; it should not be the last — application state under `/var`
  (notify, alertbridge, dashboard session/config state) is next.
