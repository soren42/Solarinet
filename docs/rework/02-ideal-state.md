# Ideal State — the fleet when it comes back online (2026-07-29 rework target)

*Everything in the current barebones state, plus the deployed-but-offline servers restored, plus the two new short-window provisioning jobs. Paired with the physical desk build and power topology.*

## Servers to restore (deployed, currently off)
| Host | Role | Why it matters |
|------|------|----------------|
| **steel** (10.0.0.11) | DNS secondary/resolver + RPZ ad-block | Restores DNS resilience (2 resolvers) and network ad-block. AXFRs akoria.net from xenon on boot |
| **benzene** (10.5.2.50) | eGPU host **+ RabbitMQ broker** | **Brings back the SoR→DNS CDC pipeline** (live auto-sync). Also the Oculink/eGPU compute host |

## New provisioning (short-window — need OS + enroll)
| Host | Role | Provisioning notes |
|------|------|--------------------|
| **Ryzen + Intel Arc eGPU box** | GPU/compute workstation | Fresh OS install; enroll solariClient; join network. Under GPU load draws >400W (power-plan it). Name it (next free element) |
| **tachyon** (~10.6.6.x, reservation exists) | **Notification + time server** | NTP (fleet time source) + the notify service (alerts → Apple/push bridge). Provision, set as internal NTP for the fleet, point notify at it |

## Network end-state (target topology)
- **Managed core does the work:** chemistry ═10G═ laboratory stays the backbone; VLANs + monitoring live here.
- **One switch per zone, uplinked straight to laboratory** — stop daisy-chaining dumb switches (each adds a 1G choke + a monitoring blind spot).
- **PoE placement:** laboratory has none → PoE devices on the UniFi PoE switches / the TEROW PoE (Pi cluster).
- **cyclotron/XGS → true 10G** once the 10G PoE++ injector replaces the interim 2.5G one (laboratory SFP+17 → UF-RJ45-10G transceiver → 10G injector → AP).
- **Servers home directly on laboratory** (xenon, cesium/nas-x on its 2.5G ports where the NIC allows).

## Physical desk build (IKEA MALM L-desk)
- **3U rack bolted under the pull-out:** `laboratory` + `cesium` (R410, 1U) + 1U PDU + light strip. *cesium moves off the floor — plan ventilation, it's loud/hot.*
- **Left stacked shelf (3 tiers):** SBCs (Pi) / SFF x86 / KVMs·KVM-switch·hubs.
- **Right two-level shelf:** network switches (lower) / NAS·eGPU·smart-home (upper).
- **Primary monitor → wall-mounted arm** (off the desk).
- **Pull-out surface:** EufyMake E1.
- **Power strips mounted to the desk back**, out of sight.
- 3D model: `docs/brand/../` → `akoria.net` (desk model artifact) / see the network-power plan.

## Power topology (decided)
```
EcoFlow DELTA Pro 3 (8 kWh)  ── feeds the ENTIRE desk (shore-power replacement)
  ├─ Vertiv 850W      → core server infrastructure (incl. cesium/R410, the dominant ~200–400W load)
  ├─ CyberPower 900W  → ALL network gear (gateway, laboratory, switches, APs, TEROW)
  ├─ APC 600W / River2 → workstations (Mac · MS-R1 · Ryzen+ARC)   ← confirm split
  └─ Wallecube DC     → NAS-X (dedicated)
```

## Open decisions to settle on the day
1. **Workstation UPS split** — Ryzen+ARC spikes >400W; likely APC for Mac/MS-R1, Ryzen straight off the EcoFlow.
2. **EcoFlow-as-source test** — the line-interactive UPSs (Vertiv/CyberPower) sit on the EcoFlow's inverter. Verify each passes through cleanly (doesn't "hunt"/chatter its AVR); if one does, feed it from the wall instead.
3. **cesium/R410 in the 3U rack** — noise + heat inches from the seat. Confirm airflow, or reconsider its placement.
4. **10G injector** for the XGS (if it's arrived) vs. staying 2.5G.
5. **Name the Ryzen box** — next free periodic-table element.

## Success criteria
Gateway + core switch + both DNS resolvers (xenon + steel) up · RabbitMQ back (SoR auto-sync live) · all core servers (xenon/cesium/radium/chlorine/nas-x) + steel + benzene online · SolariNet dashboard green · Ryzen+ARC and tachyon provisioned and enrolled · every UPS under ~70% load.
