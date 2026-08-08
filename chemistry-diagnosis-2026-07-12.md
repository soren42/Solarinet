# Chemistry (UDR7) Recurring Freeze — Investigation

*Diagnosed 2026-07-12 from xenon (root SSH into chemistry via jason's key).*

## TL;DR

The router (**chemistry**, UDR7 / UDMA67A) is **not losing its internet connection — it is freezing completely**: a total control-plane lockup where the web UI and SSH are both dead and only a power-cable pull recovers it. Cellular failover can't help, because failover is a job the router itself performs — a frozen router has nothing left alive to switch over. **Fixing the freezes is the real fix.**

**Root cause (corrected — see section below):** this is **not** an overloaded/undersized router. The controller manages only **9 devices** — a light load. The leading cause is a **firmware/driver-level fault or a hardware defect on the unit itself** (silent hard-hangs preceded by Qualcomm Wi-Fi driver churn). The fix path is firmware-update-then-RMA, **not** buy-bigger or offload-Mongo. Neither freeze left a usable crash log, so I stopped a log-eviction source and set up off-box capture to catch the next one red-handed and confirm firmware-vs-hardware.

## Timeline of the failures

- **Last night ~3:30–4:30 AM** — outage, no failover.
- **Today ~16:11** — same; UniFi resets unresponsive, direct web UI dead, hard power-pull was the only option.
- `last -x` shows **only one clean shutdown in two weeks** (Jul 5). Nearly every boot — Jul 5, Jul 9, Jul 11 20:47, **Jul 12 10:38** (an event you hadn't flagged), Jul 12 16:12 — records as **unclean**. This is happening ~daily and accelerating.

## Why nothing failed over

- **Failover is executed by the router.** It rescues you only when the Google Fiber uplink dies while the UDR7 stays alive. When the box itself freezes, there is no running system left to perform the switchover. That's why neither cellular endpoint took over.
- **Your two cellular backups are not independent.** In mcad's WAN table, both **WAN4 (UniFi 5G)** and **WAN_LTE_FAILOVER** share the *same* interface `gre1` and the *same* IP `100.127.125.129` — a single tunnel through the **U5G Backup** device (10.3.199.1). During the outage that device was in a **DHCP-request storm**, hammering the router several times per second. So even a live router would have had a sick backup path.

## Why there's no smoking-gun crash log

- **Today's hang:** `ramoops` (the persistent-RAM panic logger) came up with `uncorrectable error in header` — what you get when RAM loses power *before* a panic is written. The power-pull erased it.
- **Last night's hang:** aged out entirely. The persistent journal is capped at ~80 MB / ~2 boots, and it was being flooded (see crash-loop below), so it now only reaches back to 10:40 this morning.
- No OOM-killer, no MCE/EDAC hardware error, and no soft-lockup was logged on the surviving boot.

## Contributing stressors found

- **Wi-Fi band-steering thrash** right before today's freeze: a weak client (`cc:88:26:26:f3:db`, RSSI −79 to −85) roaming/reassociating every ~30 s, "force to roam," "BTM candidates reach max." The **last thing the kernel logged** before the box went silent at 16:11:17 — a stream of Qualcomm radio-driver (`wlan`/`qca`) activity. This is the strongest lead (see corrected root cause below).
- **A crash-looping agent — and it's yours.** `solarinet-client.service` (`/usr/local/bin/solariClient`) had restarted **6,192 times**, failing every ~3 s because the binary was built against **GLIBC 2.33/2.34** but the UDR7's libc is older (Debian 11). Lightweight per-restart, but never stops — and its log spam is *why last night's evidence is gone*.
- **Memory:** `Committed_AS` ≈ 5.6 GB vs 2.9 GB RAM, but ~1.2 GB truly available and the **OOM killer never fired** — largely a Java/Mongo virtual-address artifact. A risk indicator, **not** a proven cause. (Revised down from the initial read.)
- **Temps** ~63–67 °C at idle — not conclusive, can't rule out thermal at the moment of hang under load.
- **laboratory switch (10.0.0.3):** couldn't inspect — its SSH host key changed from your redeploys and my xenon key isn't authorized there (it uses the UniFi device SSH password). It's forwarding all traffic cleanly, and a leaf switch can't hard-hang a gateway — **not the culprit**.

## Corrected root cause (after counting the fleet)

Initial writeup leaned on "controller + MongoDB overloading an undersized gateway." **New data undercuts that** and I'm revising:

- **The network is small.** The controller manages **9 UniFi devices** (`ace.device = 9`). A UDR7 is rated for far more. IDS/IPS and DPI are both **off**. So this is a *light* load — **the network is NOT too large for the hardware, and this is not a "bought too little router" problem.**
- **Removed apps are cleanly gone.** app-protect / app-talk / app-connect / app-access slices are all **inactive**, no processes, no meaningful RAM. Only Network + InnerSpace run — both of which Jason uses. Nothing left to reclaim by removing apps. (Two cosmetic dangling units: `access.service`, `unifi-user-assets.service` in `bad` state — harmless leftover symlinks, not running.)
- **Leading hypothesis is now firmware/driver or a hardware defect on the unit** — not the deployment. Every freeze is a silent hard-hang with no saved panic, and each is preceded by Qualcomm `wlan` radio-driver churn. Silent SoC lockups after `wlan`/`qca` activity are a known failure class on IPQ-based UniFi consoles. Can't prove firmware-vs-hardware without the next freeze's logs (hence `chemistry-watch`), but the "is it me or the gear?" answer is **the gear**.
- **MongoDB cannot be migrated off a UDR7** as a standalone piece — it's baked into the Network app bundle (port 27117, dbpath `/data/unifi/data/db`, WiredTiger cache already capped 128 MB). Only way to offload is running the whole Network controller on separate hardware and demoting the gateway — awkward/semi-supported on UDR/UDM, and it **wouldn't fix a driver freeze anyway**. Don't lead with this.
- **VLANs are safe to enable.** They're a dataplane feature (hardware-offloaded), add negligible controller memory, and are unrelated to the freeze mechanism. Only heavy IDS/IPS or deep inter-VLAN inspection would be expensive — and both are off.

## What I changed today

- **Stopped the crash-loop.** `systemctl disable --now solarinet-client.service` on chemistry (was `activating`, now `inactive`/`disabled`). This halts the churn *and* stops it evicting your logs. *To restore chemistry metrics later, rebuild solariClient against the UDR7's older glibc (or link it static).*
- **Set up off-box log capture on xenon** so the next freeze is recorded where it's safe:
  - Service: `chemistry-watch.service` (runs as `jason`, whose key roots into chemistry).
  - Binary: `/usr/local/bin/chemistry-watch` — source in `~/Code/Solarinet/chemistry-watch.sh`.
  - Output in `~/chemistry-watch/`:
    - `journal.log` — live `journalctl -f` follow, auto-reconnects across reboots/hangs.
    - `stats.log` — load, MemAvailable, Committed_AS, temps, and top CPU **every 30 s**; writes a timestamped `POLL-FAILED` line the moment chemistry goes unreachable, **bracketing the exact freeze onset**.
  - Verified live and writing valid data.

## Recommendations (priority order — revised)

1. **Update UDR7 firmware** (currently UniFi OS 5.1.19 / kernel 5.4.213). Silent hard-freezes preceded by `wlan` driver activity are frequently firmware bugs — this is the cheapest, likeliest fix and the first thing to try.
2. **Let `chemistry-watch` catch the next freeze** to confirm driver-vs-hardware (freeze-onset minute + resource trend). Root cause can't be nailed without it.
3. **If it survives a firmware update, open a Ubiquiti RMA** using the capture as evidence — treat it as a hardware fault on the unit.
4. **Rebuild/relink solariClient** against the UDR7's glibc so chemistry reports metrics again — without the crash-loop.
5. **Investigate the U5G Backup DHCP storm** and the fact that both cellular paths converge on one `gre1` tunnel — the "redundant" backup is a single point of failure.
6. **Reset the failover expectation:** it cannot help against router lockups. The freezes are the problem to solve.

**Explicitly NOT recommended (ruled out):** moving MongoDB off the gateway (not possible standalone on a UDR7, and wouldn't fix a driver freeze); removing more UniFi apps (Protect/Talk/Connect/Access already cleanly off); buying a bigger router (9 devices is well within UDR7 spec). **VLANs are safe to enable** and won't move the resource numbers that matter.

## Next step

When the next freeze hits, tell me and I'll read `~/chemistry-watch/journal.log` and `stats.log` — that's where we get the smoking gun (freeze-onset minute + the resource trend leading into it).

#SolariNet/chemistry #SolariNet/diagnostics
