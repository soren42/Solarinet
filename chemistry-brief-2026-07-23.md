# Chemistry (UDR7) Freeze — Brief & Tonight's Runbook

*2026-07-23. Follow-up to [chemistry-diagnosis-2026-07-12.md]. Prepared for after-hours execution (Lisa WFH today — no disruptive changes until this evening).*

## TL;DR

The UDR7 (**chemistry**, UDMA67A, Qualcomm **IPQ5322**) is suffering **silent SoC hard-hangs in its Wi-Fi driver**. Three today. Not overload, not memory, not CPU-thermal — the radio silicon locks the whole SoC so hard it can't even log a panic. Plan: **offload all Wi-Fi to dedicated APs and cut the gateway's built-in radios** (the crashing subsystem), then watch 24 h. If stable → the gateway's Wi-Fi role is retired for good. **Firmware is confirmed current (UniFi OS 5.1.19, latest per the console) — no update exists, so that step is dropped; the radio cut is the sole intervention.**

## Evidence (today)

- **3 hard resets**, reboots at **07:57 / 08:58 / 13:24 EDT**, each ~1 min after a silent freeze `chemistry-watch` bracketed (07:56 / 08:52 / 13:21). Exact correlation.
- **No panic, ever** — zero `oops`/`BUG`/`watchdog`/`soft-lockup`/`call trace` in the full day's kernel log. Silent hard-lock.
- **Resources flat into every freeze** — load ~3.1, MemAvailable ~1.2 GB (no leak, OOM never fires), CPU zones 62–67 °C. Then an instant drop to unreachable, no precursor.
- **Always in Wi-Fi-driver territory** — kernel log saturated with Qualcomm `wlan`/`ol_ath` activity; the **Wi-Fi chip is thermally throttling** (`Thermal level 5 → 4`) while CPU temps look fine. The radio is the hot, busy, unstable part.
- **Firmware unchanged** since the 7/12 workup: UniFi OS **5.1.19**, Network **10.4.57**, kernel `5.4.213-ui-ipq5322`.

## Root cause

A **firmware/driver-level fault (or hardware defect) in the IPQ5322 Wi-Fi stack** — a known failure class on IPQ-based UniFi consoles. The gateway is currently serving ~5 Wi-Fi clients on `solarian`/`solarian-2.4` (wifi0=4, wifi1=1), so its radios are doing real, crash-triggering work that the dedicated APs can absorb.

## Coverage math (why the offload is safe)

Wi-Fi is already carried mainly by two **U7 Pro** APs — **flask (29 clients)** and **beaker (7)**. The gateway's own radios carry ~5 and are redundant. Moving one more AP into the craft room (where chemistry sits) closes the only gap before its radios go dark.

## Two-night plan

### Night 1 (tonight) — cut the gateway radios

**Pre (you):** Move the master-bedroom AP into the craft room (temporary coverage); confirm it adopts and shows online. Tell me when it's up.

**Then (me, via SSH + UniFi API — `chemistry-fix-tonight.sh`):**
1. **`precheck`** — confirm flask, beaker, and the relocated AP are online; snapshot client distribution.
2. **`radios-off`** — snapshot radio_table, disable chemistry's wifi0/1/2. *(Firmware step dropped — already current.)*
3. **`verify`** — the ~5 gateway clients re-home to flask/beaker/craft-AP, WAN stays up, no SSID gap, `chemistry-watch` still capturing.

**Watch (both): ~24 h.** Success = zero freezes with radios off → the gateway Wi-Fi driver was the trigger; retire it permanently. Still freezing → **Ubiquiti RMA** (captures = evidence).

### Night 2 (tomorrow eve) — permanent craft-room AP

New **U7 Pro XGS** arrives. Install in the craft room, return the borrowed AP to the master bedroom.

**Wired path (true 10G):**
`laboratory SFP+ port 17 → UF-RJ45-10G transceiver → RJ45 → 10G PoE++ injector → U7 Pro XGS`

**Parts:**
- **laboratory = USW-Pro-Max-16 (NON-PoE)** — has the free 10G SFP+ port 17, but **zero PoE**, so the AP must be powered externally.
- **U7 Pro XGS** — 10GbE **RJ45** uplink (10/5/2.5/1G), **PoE++ 29 W / 802.3bt**.
- **UF-RJ45-10G** (10GBASE-T SFP+ transceiver) — bridges the AP's RJ45 to laboratory's SFP+ 17. *(Confirm on hand / ordered.)*
- **10G PoE++ injector** — Jason has one. Must be multi-gig (a 1G injector would cap the link to 1G).
- Power outlet at the AP for the injector.
- *Fallback if no transceiver:* laboratory 2.5G RJ45 port (13–16) → injector → AP, capped at 2.5G.

**Steps:** mount AP → transceiver into SFP+17 → patch to injector → injector to AP → adopt in UniFi → confirm link negotiates 10G (`stat/device` port speed) and radios come up on the same SSIDs → move borrowed AP back to bedroom.

## Success criteria & decision tree

- **Stable 24 h with radios off** → the gateway Wi-Fi driver was the trigger. **Retire the UDR7's Wi-Fi permanently**; order a replacement **U7** for the master bedroom. Done.
- **Still hangs with radios off (+ firmware current)** → software and load are eliminated; a silent SoC lock with clean thermals = **defective unit → Ubiquiti RMA**, with the `chemistry-watch` captures as evidence.

## Rollback

Every step is reversible: re-enable the radios (API/app toggle) or roll back firmware via the console. Internet routing is unaffected by the radio cut.

## Not the cause (ruled out)

Overload (9-device network, IDS/IPS/DPI off), memory/Mongo, VLANs, the leaf switches. Don't chase these.

#SolariNet/chemistry #SolariNet/diagnostics
