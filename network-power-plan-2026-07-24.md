# SolariNet — Network & Power Plan

*Generated 2026-07-24 from the live UniFi controller. **Filled** rows are observed fact; **`??`** and blank cells are for you to capture on your walkthrough. Craft-room cleanup + family-room track.*

## How to use this
- §1–§4 are the **current state** (auto-populated from the network) plus recommendations.
- §5–§7 are **templates to populate** after you capture brand/model + power infrastructure.
- Send me the Excalidraw **SVG** and I'll overlay port/power assignments onto the desk zones (§6).

---

## 1. Device inventory — *confirm product name + physical location*

| Name | UniFi model code | Likely product | Role | PoE source? | Location (fill) |
|------|------------------|----------------|------|-------------|-----------------|
| chemistry | UDMA67A | Dream Router 7 (UDR7) | Gateway — **Wi-Fi retired** | provides PoE on LAN | Craft room |
| laboratory | USPM16 | USW-Pro-Max-16 | Core switch — **NO PoE** | — | ?? |
| Test Tube | USM8P | USW 8-port PoE *(confirm)* | Edge switch (family-room side?) | provides PoE | ?? |
| slide | USM8P | USW 8-port PoE *(confirm)* | Edge switch | provides PoE | ?? |
| pipette | USM8P | USW 8-port PoE *(confirm)* | Edge switch | provides PoE | ?? |
| cyclotron | UAPA6A4 | **U7 Pro XGS** | AP (craft room) — 10G-capable | needs PoE++ | Craft room |
| flask | U7PRO | U7 Pro | AP | needs PoE++ | ?? |
| beaker | U7PRO | U7 Pro | AP | needs PoE++ | ?? |
| covalent | UMBBE633 | UniFi building/mobile bridge *(confirm)* | Wireless bridge (family-room link?) | ?? | ?? |

---

## 2. Craft-room port maps (current, live)

**Legend:** speed shown is *negotiated now*; media cap in brackets. `FREE` = available for planning. `??` = a device is attached but unnamed — identify it.

### chemistry (UDR7) — gateway, 5 ports
| Port | Media | Now | Attached |
|-----:|-------|----:|----------|
| 1 | 2.5G | down | FREE |
| 2 | 2.5G | 1G | Raspberry Pi (`toymaker`) |
| 3 | 2.5G | 2.5G | → laboratory |
| 4 | 2.5G | 2.5G | **??** identify |
| 5 | SFP+ | 10G | **??** identify (10G device — Ugreen NAS?) |

### laboratory (USW-Pro-Max-16) — core, 18 ports · **NO PoE**
| Port | Media | Now | Attached |
|-----:|-------|----:|----------|
| 1 | 1G | 1G | `nas-x` |
| 2–10 | 1G | down | **FREE** (9 ports) |
| 11 | 1G | 1G | **??** identify |
| 12 | 1G | 1G | `starship` |
| 13 | 2.5G | 1G | **??** identify |
| 14 | 2.5G | 1G | Home Assistant |
| 15 | 2.5G | 1G | `xenon` |
| 16 | 2.5G | 1G | `radium` |
| 17 | SFP+ | 10G | `cyclotron` (U7 Pro XGS) — 2.5G-capped by injector |
| 18 | SFP+ | 10G | → chemistry (10G backbone) |

### Test Tube (USW 8-PoE)
| Port | Media | Now | PoE | Attached |
|-----:|-------|----:|-----|----------|
| 1 | 1G | 100M | ✓ | `platinum` |
| 2 | 1G | down | ✓ | FREE |
| 3 | 1G | 1G | ✓ | Family Room Google TV |
| 4 | 1G | 1G | ✓ | covalent (bridge) |
| 5 | 1G | 1G | ✓ | `NanoPi-R1` |
| 6 | 1G | 1G | ✓ | flask (AP) |
| 7 | 1G | 100M | ✓ | Samsung *(device?)* |
| 8 | 1G | 1G | — | uplink → (daisy chain) |

### slide (USW 8-PoE)
| Port | Media | Now | PoE | Attached |
|-----:|-------|----:|-----|----------|
| 1 | 1G | 100M | ✓ | **??** identify |
| 2 | 1G | 1G | ✓ | `glkvm` / `silver` |
| 3 | 1G | 1G | ✓ | `cesium` |
| 4 | 1G | down | ✓ | FREE |
| 5 | 1G | 1G | ✓ | Google device *(Chromecast?)* |
| 6 | 1G | down | ✓ | FREE |
| 7 | 1G | 1G | ✓ | `transporter` |
| 8 | 1G | 1G | — | → pipette |

### pipette (USW 8-PoE)
| Port | Media | Now | PoE | Attached |
|-----:|-------|----:|-----|----------|
| 1 | 1G | 1G | ✓ | beaker (AP) |
| 2 | 1G | 1G | ✓ | ASIX USB-Ethernet *(device?)* |
| 3–7 | 1G | down | ✓ | **FREE** (5 ports) |
| 8 | 1G | 1G | — | → Test Tube |

---

## 3. Backbone / topology
```
chemistry (UDR7 gateway)
  ├─ SFP+ p5  ═10G═  ?? (unidentified 10G device)
  ├─ 2.5G p3  ─2.5G─ laboratory (core)
  │                    ├─ SFP+ p17 ═10G═ cyclotron / U7 Pro XGS  (injector caps AP at 2.5G)
  │                    ├─ SFP+ p18 ═10G═ back to chemistry
  │                    └─ 2.5G/1G ports → xenon, radium, nas-x, starship, Home Assistant, cesium*
  └─ (edge daisy-chain, all 1G):  slide ─ pipette ─ Test Tube  → APs + family-room gear
```
\* cesium is actually on **slide p3**, reached via the 1G edge chain.

## 4. Recommendations / constraints (from the data)
1. **laboratory is non-PoE.** Keep it for servers/10G only. All **PoE devices (APs, cameras, PoE Pis) must stay on Test Tube / slide / pipette.**
2. **xenon, radium, Home Assistant sit on 2.5G-capable ports but negotiate 1G** — likely 1G NICs or cabling. If you want 2.5G to the servers, that's a NIC/cable check, not a switch change.
3. **The whole edge chain (slide→pipette→Test Tube) is 1G, daisy-chained.** Everything on it — cesium, the APs, family-room gear — shares a 1G path back to core. If any of those need more, home them directly on laboratory instead.
4. **U7 Pro XGS** is on the right port (SFP+17) but the **2.5G injector caps it at 2.5G**; swap to the 10G injector during teardown to unlock full 10G.
5. **chemistry radios stay OFF** (freeze fix — permanent).
6. **Identify the `??` ports** — especially **chemistry SFP+ p5 (a 10G device)** and **chemistry p4 (2.5G)**; those are your fastest links and worth knowing.
7. **Spare capacity for planning:** laboratory ~9× 1G free; pipette 5× 1G-PoE free; Test Tube + slide 1 each. Plenty of room.

---

## 5. Power / circuit / UPS map — *TEMPLATE, populate*

Device list pre-filled; add power draw, which outlet/strip, UPS, and circuit after you capture the infrastructure.

| Device | Typ. draw (W) | Outlet / strip | On UPS? | UPS unit | Circuit / breaker | Notes |
|--------|--------------|----------------|---------|----------|-------------------|-------|
| chemistry (UDR7) | | | | | | must stay up (gateway) |
| laboratory (core sw) | | | | | | must stay up |
| cyclotron (XGS) | | | | | | PoE from ?? |
| flask / beaker (APs) | | | | | | PoE |
| xenon | | | | | | server |
| radium | | | | | | AD/DNS — keep up |
| cesium | | | | | | server (R410 — heavy) |
| nas-x | | | | | | storage |
| starship | | | | | | |
| Home Assistant | | | | | | |
| transporter | | | | | | |
| *(add rows)* | | | | | | |

**UPS inventory (capture):**

| UPS unit | Model | VA / W rating | Outlets | Battery age | Feeds circuit |
|----------|-------|---------------|---------|-------------|---------------|
| | | | | | |

**Circuits (capture):**

| Breaker # | Amps | Room/zone | What's on it |
|-----------|------|-----------|--------------|
| | | | |

---

## 6. Desk real-estate — *await Excalidraw SVG*

Send the SVG and I'll place: which desk zone each device sits in, its network port assignment (from §2), and its power/outlet assignment (from §5) — so the diagram, the ports, and the power all reconcile in one view.

---

## 7. Family room — *smaller-scale TEMPLATE*

Known so far: **Family Room Google TV** (Test Tube p3), **covalent** wireless bridge (Test Tube p4) likely carries this zone. Fewer devices/ports.

| Device | Type | Network (switch/port or Wi-Fi) | Power / outlet | On UPS? | Notes |
|--------|------|-------------------------------|----------------|---------|-------|
| Google TV | media | Test Tube p3 | | | new TV mount |
| covalent bridge | net | Test Tube p4 | | | craft↔family link |
| *(cabinet gear)* | | | | | behind fireplace built-in |
| *(case fans)* | cooling | — | | | new conduit + fan holes |

**Family-room notes:** conduit hole into cabinet top; case-fan holes for airflow in the restricted built-in; clear empty boxes first.

---

# IDEAL END-STATE DESIGN
*Added 2026-07-24 with the full gear inventory + desk diagram. Treats the current patchwork as disposable; this is the target to re-cable toward.*

## 8. Desk zones (from Desk-Layout.svg)
| Tier | Zone | Houses | Network need | Power need |
|------|------|--------|--------------|-----------|
| **Top shelf** | NAS / eGPU | nas-x, eGPUs, BC-503 | NAS wants 2.5G+; eGPUs = compute (no net) | NAS-X on its DC UPS; eGPUs heavy draw |
| **Middle** | Network Switches | core (laboratory), edge switches, chemistry | the wiring hub — 10G core lives here | Tier-1 clean/AVR |
| **Middle** | KVM/HDMI | glkvm/silver, HDMI matrix | 1× 1G | low |
| **Middle** | Primary Workstations | main x86 box(es) | 2.5G+ | high (workstation + monitors) |
| **Middle** | Bench | soldering, oscilloscopes, bench PSU | none/low | switched, non-critical |
| **Lower** | x86 Systems | xenon, cesium (R410), starship, transporter, platinum | 2.5G where NIC allows | **cesium R410 = the big hog** |
| **Lower** | Pi Systems | radium (CM5), toymaker, NanoPi-R1, Pi cluster | 1G, several want PoE | low each, but many |
| **Lower** | Displays | secondary/tertiary monitors | none | medium |
| **Lower** | Hubs / I/O / Sound | USB hubs, audio, ASIX adapter | 1G, low | low |

## 9. Added gear inventory (invisible to the controller)

**Unmanaged switches** — all 1G, no VLAN/monitoring (they're the "?? multi-client" fan-outs the controller saw):

| Switch | Ports | PoE | Best end-state role |
|--------|-------|-----|---------------------|
| TEROW 10-port PoE | 8×1G PoE (af/at, 120W) + 2 uplink | ✓ | **Pi Systems zone** — powers + connects the Pi cluster from one box |
| TP-Link SG116 | 16×1G | — | High-fan-out zone (Hubs/I/O, or displays) — *but a monitoring blind spot* |
| TP-Link SG108 | 8×1G | — | Small zone fan-out (Bench, Sound) |
| NETGEAR GS208 | 8×1G | — | Small zone fan-out |
| NETGEAR GS305 ×2 | 5×1G | — | KVM cluster / Displays / spot fan-out |

## 10. Ideal end-state network

**Principles:**
1. **Managed core carries anything that matters.** chemistry ═10G═ laboratory stays the backbone; VLANs + monitoring live here.
2. **One switch per zone, uplinked to laboratory** — not daisy-chained switch→switch→switch. Every dumb switch you chain adds a 1G choke *and* a blind spot; hang each directly off a laboratory port instead.
3. **Managed for value, dumb for volume.** Servers, NAS, APs, and anything you want to *see* in SolariNet → UniFi ports (laboratory / the PoE USW trio). Low-value, low-bandwidth peripherals → the unmanaged switches, accepting they're invisible.
4. **PoE placement:** laboratory has none. Pis needing PoE → **TEROW**; APs/cameras → the UniFi PoE switches.

**Proposed zone → switch → uplink:**
| Zone | Switch | Uplink to | Speed |
|------|--------|-----------|-------|
| Network core | laboratory (USW-Pro-Max-16) | chemistry SFP+ | 10G |
| Pi Systems | **TEROW PoE** | laboratory 1G port | 1G |
| x86 Systems | **direct to laboratory** (xenon/cesium/starship on 2.5G ports 13–16) | — | 2.5G* |
| NAS / eGPU (top) | nas-x direct to laboratory; a GS305 if more ports needed | laboratory | 1–2.5G |
| KVM/HDMI | GS305 | laboratory 1G | 1G |
| Displays | SG116 or GS208 | laboratory 1G | 1G |
| Hubs / I/O / Sound | SG108 / GS208 | laboratory 1G | 1G |
| APs | flask/beaker → UniFi PoE switches; cyclotron/XGS → laboratory SFP+17 | — | 10G* |

\* gated by NIC/injector — see §4. laboratory has ~9 free 1G + spare 2.5G, so one-hop-per-zone fits without daisy-chaining.

**Open question:** do you want VLAN segmentation (mgmt / servers / IoT / guest)? If yes, keep more devices on UniFi ports (dumb switches can't tag), which shifts the managed-vs-dumb line. Flag it and I'll redraw.

## 11. Ideal end-state power

**Capacity on hand:**
| Unit | Rating | Type | Instant failover? |
|------|--------|------|-------------------|
| EcoFlow DELTA Pro 3 + extra batt | **8192 Wh**, ~4000W | LFP station | ~10–30 ms (EPS) |
| CyberPower CP1500AVRLCD | 1500VA/**900W**, 12 outlet | Line-interactive + AVR | yes |
| Vertiv Liebert PSA6E-1500LVT | 1440VA/**850W**, 8 outlet | Line-interactive + AVR | yes |
| APC Back-UPS 1050VA/**600W** | AVR/surge | Line-interactive | yes |
| EcoFlow River 2 | 256 Wh, 600W | LFP station | ~10–30 ms |
| Wallecube NAS UPS | 150W **DC** | DC UPS | — (NAS-X dedicated) |

**DECIDED topology (2026-07-24) — EcoFlow as the whole-desk source, AVR UPSs downstream per branch:**
```
EcoFlow DELTA Pro 3 (8 kWh)  ── feeds the ENTIRE desk (shore-power replacement)
  ├─ Vertiv 850W      → core server infrastructure
  ├─ CyberPower 900W  → ALL network gear (USW-16-Pro, gateway, switches, APs, TEROW)
  ├─ [UPS TBD]        → workstations: Mac · MS-R1 · new Ryzen 5 (+ Intel ARC eGPU)
  └─ Wallecube DC     → NAS-X (dedicated)
```
- **Open — workstation UPS:** the sentence trailed off; workstations need an assignment. Free units are the **APC 600W** and **River 2 (256 Wh)**. The Ryzen+ARC box under GPU load can spike >400W, so size it — likely APC for the Mac/MS-R1 and the Ryzen either on APC or straight off the EcoFlow.
- **⚠ Heads-up on EcoFlow-as-source:** the downstream **line-interactive** UPSs (Vertiv/CyberPower) sit on the EcoFlow's inverter output, not the wall. Some line-interactive units *hunt* (flip to their own battery, or chatter their AVR relay) on inverter/EPS power if the waveform/frequency isn't clean enough. Verify each passes through cleanly on EcoFlow power before trusting it — if one hunts, feed it from the wall instead. (Not a blocker; just test it.)
- **Load math (needs §5 watts):** keep each UPS under ~70% of its W rating. **cesium/R410 is the dominant single load (~200–400W)** and now sits on the Vertiv (core servers) — measure it first to confirm the Vertiv's 850W has headroom for the rest of core infra.

**Physical placement:**
- **3U under-desk rack, mounted to the underside of the pull-out surface** — holds the core switch **`laboratory` (USW-Pro-Max-16, non-PoE)**, **cesium (R410, 1U)**, a **1U power strip**, and a **lighting strip**. *(cesium therefore moves OFF the floor into this rack.)*
- **Power strips mounted to the back of the desk**, out of sight, to preserve surface/real-estate.
- **Resolved (2026-07-25):** there is **no new switch** — "USW-16-Pro" was a misremembered name. Core = the existing **`laboratory` USW-Pro-Max-16 (non-PoE)**, so §10's PoE constraint stands: PoE devices stay on the UniFi PoE switches / TEROW, not the core.

## 12. Reconciliation checklist (desk × ports × power)
Once §5 wattages + the zone port-counts are in, each device should have: **zone → switch/port → UPS/outlet**, with no zone daisy-chained more than one hop and no UPS over ~70% loaded. I'll render the final one-view version (and the 3D model) from that.
