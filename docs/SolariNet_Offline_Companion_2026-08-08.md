# SolariNet — Offline Companion & Rewiring Field Guide

`For: the Claude phone app while the whole network is torn down · Author: Claude (Fable 5, Lead) · 2026-08-08`
`Operator: Jason C. Kay (he/him) · Homelab: SolariNet / Akoria · Naming: periodic table`

> **What this is.** A self-contained brief so a fresh Claude instance on your phone — online via cellular, blind to the LAN — can help you troubleshoot, sequence, and sanity-check while everything is unplugged, inventoried, and re-cabled. It contains **no secrets** (it lives in a cloud-synced app). Passwords, API keys, and the OIDC client secret are on the workstation, referenced by path only.
>
> **How to use it.** Paste or attach this file to the phone app before you start pulling cables. Then ask it plainly: "cesium won't come back up, here's what I see" / "what order do I power things on?" / "is it safe to move X off the daisy chain?" It has the topology, the port maps, the power plan, the boot order, and the failure modes below.

---

## 0. First principles (read once)

- **Two Tier-0 services, everything else is recoverable.** (1) the **SoR** — MariaDB on **cesium**; (2) **authoritative DNS** — BIND on **xenon** (`.net`) + **radium** (`.org`). If those two survive, the network can be rebuilt around them. Nothing else is irreplaceable.
- **Gateway and core are the spine.** **chemistry** (UDR7 gateway) → **laboratory** (core switch). If either is down, nothing routes. These two + xenon are being kept online during shutdown for exactly this reason.
- **UniFi gear has no software shutdown.** APs, switches, the gateway — you power them by pulling the cable / cutting PoE. There is nothing to `shutdown -h` on them. Don't wait for a graceful stop that can't happen.
- **The edge is a 1G daisy chain** (slide → pipette → Test Tube). Anything on it shares one 1G path to core. Good to know when something feels slow — it may just be three hops deep on a shared gigabit.
- **Power comes from one EcoFlow now.** The DELTA Pro 3 (8 kWh) feeds the whole desk; AVR UPSs sit downstream per branch. Pulling the EcoFlow drops everything not on a separately-fed UPS.

---

## 1. Naming scheme (so a name tells you what a box is)

- **A-record = physical identity** — the element/compound the box is: `chemistry`, `laboratory`, `xenon`, `radium`, `cesium`, `benzene`, `steel`, `chlorine`, `nitrogen`, `tungsten`, `valence`, `lithium`, `helium`, `starship`, `pipette`, `slide`, `beaker`, `flask`, `cyclotron`, `covalent`, `platinum`, `transporter`, `toymaker`.
- **CNAME = function** — `ad`, `sso`, `git`, `dns`, `nas`, etc. Second instance → `-alt` / `-secondary` / `-backup`.
- **Domains:** `akoria.net` (primary, BIND on xenon) and `akoria.org` (AD-integrated, BIND9_DLZ on radium). Every host resolves under both (hourly reconciler mirrors `.net` A-records into AD). Public-facing services use descriptive `*.akoria.net` subdomains (CNAME → host + SNI :443 vhost), **not** arbitrary ports.

---

## 2. Full device inventory

### 2.1 Servers & compute (software-shutdownable)

| Host | IP | What it is | Role | Notes |
|---|---|---|---|---|
| **xenon** | 10.0.0.20 | x86 server | **Authoritative BIND `.net`** + monitoring stack + notify + dashboard (Apache/php-fpm/MariaDB read view) | **On UPS — kept up.** Concentration risk: 4 critical svcs. This is where you'll be chatting from. |
| **cesium** | 10.1.0.200 | Dell R410 (1U) | **SoR: MariaDB** + **Forgejo git** (internal storage) | Tier-0. Big power hog (~200–400W). Moving into the 3U under-desk rack. |
| **radium** | 10.1.0.10 | Pi CM5 (NVMe) | **Samba AD DC + Keycloak SSO + BIND9_DLZ `.org`** | Tier-0 for AD/SSO. Debian 13. |
| **chlorine** | 10.7.0.10 | VIM4-provisioned | **Internal CA** (step-ca: X.509 + S/MIME + ACME + SSH CA) | Issues the Akoria certs. |
| **nitrogen** | 10.5.2.20 | Raspberry Pi (`dev-family-pi01`) | dev / family services | Leaf. |
| **benzene** | 10.5.2.50 | x86 server | RabbitMQ (MQ broker) + fleet provisioning (PXE) | **Unreachable since 2026-08-06 storm** — no route from xenon. Needs console/switch-port attention. |
| **steel** | 10.0.0.11 | ZimaBlade | DNS secondary (Docker BIND9, AXFR from xenon) + Docker host | **Down since storm.** MAC 00:e0:4c:40:59:df. |
| **tungsten** | 10.5.0.5 | x86 (dev) | dev churn box | **Excluded from DR** (untraced ~48h hard-resets). SSH timed out during sweep. |
| **valence** | 10.6.0.20 | monitoring outpost | solariMonitor outpost | Permission denied during sweep (no current creds). |
| **lithium** | 10.6.4.33 (wired) | Arduino UNO Q | **Status panel host** (Galactic Unicorn LED panel) | Panel firmware d86f11e3, asleep. Also a stray DHCP lease 10.6.102.248 may linger. |
| **helium** | 10.0.0.5 | HackBoard 2 / openSUSE | (was mis-assumed a Pi-hole) | **Down since storm.** |
| **nas-x / NAS** | 10.0.0.10 | NAS appliance (`NAS-X`) | **Primary storage** | Shared beyond monitoring — treat as production storage. On dedicated Wallecube DC UPS. |

### 2.2 Network gear (UniFi — no software shutdown; power = unplug/cut PoE)

| Name | IP | Model | Role | PoE |
|---|---|---|---|---|
| **chemistry** | 10.0.0.1 (WAN 136.57.196.188) | UDR7 (Dream Router 7) | **Gateway** — Wi-Fi radios **retired/OFF** (freeze fix, permanent) | provides PoE on LAN |
| **laboratory** | 10.0.0.3 | USW-Pro-Max-16 | **Core switch — NO PoE** | none |
| **Test Tube** | — | USW 8-PoE | Edge switch (family-room side) | provides |
| **slide** | 10.0.0.32 | USW Ultra | Edge switch | provides |
| **pipette** | 10.0.0.31 | USW Ultra | Edge switch | provides |
| **cyclotron** | 10.0.0.6 | U7 Pro XGS | AP (10G-capable, **capped 2.5G by injector**) | needs PoE++ |
| **beaker** | 10.0.0.7 | U7 Pro | AP | needs PoE++ |
| **flask** | 10.0.0.8 | U7 Pro | AP | needs PoE++ |
| **covalent** | 10.0.0.160 | U5G Backup / bridge | Wireless bridge (craft↔family link) | — |

### 2.3 Unmanaged switches (invisible to controller — all 1G)

TEROW 10-port PoE (8×1G PoE, 120W) · TP-Link SG116 (16×1G) · TP-Link SG108 (8×1G) · NETGEAR GS208 (8×1G) · NETGEAR GS305 ×2 (5×1G). *End-state role: TEROW powers the Pi cluster; the dumb switches fan out low-value peripherals per zone.*

### 2.4 Endpoints / peripherals seen on ports (no shutdown needed — they follow their switch)

toymaker (Pi, chemistry p2) · Home Assistant (laboratory p14) · starship (laboratory p12) · platinum (Test Tube p1) · Family-room Google TV (Test Tube p3) · NanoPi-R1 (Test Tube p5) · Samsung device (Test Tube p7) · glkvm/silver (slide p2) · transporter (slide p7) · Google/Chromecast (slide p5) · ASIX USB-Ethernet adapter (pipette p2) · Pi-hole 1 (10.0.0.254, decommissioned) · Pi-hole 2 (10.0.0.253, decommissioned).

---

## 3. Port maps (current wiring — your teardown/rebuild reference)

**Legend:** speed = negotiated now (media cap differs). `FREE` = open for planning. `??` = attached-but-unidentified — **capture these on the walkthrough**, especially the fast links.

### chemistry (UDR7 gateway) — 5 ports
| Port | Media | Now | Attached |
|--:|--|--:|--|
| 1 | 2.5G | down | FREE |
| 2 | 2.5G | 1G | Raspberry Pi (`toymaker`) |
| 3 | 2.5G | 2.5G | → **laboratory** |
| 4 | 2.5G | 2.5G | **?? identify** |
| 5 | SFP+ | 10G | **?? identify** (10G device — Ugreen NAS?) |

### laboratory (USW-Pro-Max-16) — core, 18 ports · NO PoE
| Port | Media | Now | Attached |
|--:|--|--:|--|
| 1 | 1G | 1G | `nas-x` |
| 2–10 | 1G | down | **FREE (9 ports)** |
| 11 | 1G | 1G | **?? identify** |
| 12 | 1G | 1G | `starship` |
| 13 | 2.5G | 1G | **?? identify** |
| 14 | 2.5G | 1G | Home Assistant |
| 15 | 2.5G | 1G | `xenon` |
| 16 | 2.5G | 1G | `radium` |
| 17 | SFP+ | 10G | `cyclotron` (capped 2.5G by injector) |
| 18 | SFP+ | 10G | → **chemistry** (10G backbone) |

### Test Tube (USW 8-PoE)
| Port | Now | PoE | Attached |
|--:|--:|:--:|--|
| 1 | 100M | ✓ | `platinum` |
| 2 | down | ✓ | FREE |
| 3 | 1G | ✓ | Family Room Google TV |
| 4 | 1G | ✓ | covalent (bridge) |
| 5 | 1G | ✓ | `NanoPi-R1` |
| 6 | 1G | ✓ | flask (AP) |
| 7 | 100M | ✓ | Samsung |
| 8 | 1G | — | uplink → (daisy chain) |

### slide (USW Ultra)
| Port | Now | PoE | Attached |
|--:|--:|:--:|--|
| 1 | 100M | ✓ | **?? identify** |
| 2 | 1G | ✓ | `glkvm` / `silver` |
| 3 | 1G | ✓ | `cesium` |
| 4 | down | ✓ | FREE |
| 5 | 1G | ✓ | Google device (Chromecast?) |
| 6 | down | ✓ | FREE |
| 7 | 1G | ✓ | `transporter` |
| 8 | 1G | — | → **pipette** |

### pipette (USW Ultra)
| Port | Now | PoE | Attached |
|--:|--:|:--:|--|
| 1 | 1G | ✓ | beaker (AP) |
| 2 | 1G | ✓ | ASIX USB-Ethernet |
| 3–7 | down | ✓ | **FREE (5 ports)** |
| 8 | 1G | — | → **Test Tube** |

### Backbone
```
chemistry (UDR7 gateway)
  ├─ SFP+ p5  ═10G═  ?? (unidentified 10G device)
  ├─ 2.5G p3  ─2.5G─ laboratory (core)
  │                    ├─ SFP+ p17 ═10G═ cyclotron / U7 Pro XGS  (injector caps at 2.5G)
  │                    ├─ SFP+ p18 ═10G═ back to chemistry
  │                    └─ 2.5G/1G  → xenon, radium, nas-x, starship, Home Assistant
  └─ edge daisy-chain, all 1G:  slide ─ pipette ─ Test Tube  → APs + family-room gear
     (cesium hangs off slide p3, so it too rides the 1G edge chain)
```

---

## 4. Planned end-state (what you're re-cabling *toward*)

Treat the current patchwork as disposable. Targets:

1. **Managed core carries anything that matters.** chemistry ═10G═ laboratory stays the backbone; VLANs + monitoring live here.
2. **One switch per zone, uplinked to laboratory** — **stop daisy-chaining** switch→switch→switch. Every dumb hop adds a 1G choke *and* a monitoring blind spot. Hang each zone switch directly off a laboratory port. (laboratory has ~9 free 1G + spare 2.5G — one hop per zone fits.)
3. **Managed for value, dumb for volume.** Servers, NAS, APs, anything you want to *see* in SolariNet → UniFi ports. Low-value peripherals → the unmanaged switches (accept they're invisible).
4. **PoE placement:** laboratory has none. PoE Pis → **TEROW**; APs/cameras → the UniFi PoE switches.
5. **Swap cyclotron's 2.5G injector for the 10G injector** during teardown to unlock full 10G on SFP+17.
6. **Move cesium off the floor** into the 3U under-desk rack (with laboratory + a 1U power strip).

**DNS end-state:** collapse toward a clean 2-tier authoritative setup — xenon primary `.net` with **steel** + radium as secondaries (AXFR), radium primary `.org`; **retire both Pi-holes** (UniFi does resolve/filter, or steel takes the resolver role); repoint UniFi DHCP `dns_1/2` off the Pi-holes → steel + a second resolver. **Serial bug is fixed** (monotonic) so secondaries AXFR correctly. NAS-X name-resolution fix (DHCP `domain_name=akoria.net`) is already applied.

**HA/DR end-state:** two idle Pi 5s become DR peers — DR-1 (DNS secondary + MariaDB arbiter), DR-2 (monitoring/dashboard warm-standby + MQ mirror). SoR replication: cesium → benzene replica → Pi5 arbiter. Capacity is abundant; this is a reallocation effort, **zero purchasing**. Power is not the failure mode — DR targets host/software/disk failure.

**Open decision:** VLAN segmentation (mgmt / servers / IoT / guest)? If yes, keep more devices on UniFi ports (dumb switches can't tag), which shifts the managed-vs-dumb line.

---

## 5. Power plan (decided 2026-07-24)

**EcoFlow DELTA Pro 3 (8 kWh, ~4000W) is the whole-desk source; AVR UPSs sit downstream per branch:**
```
EcoFlow DELTA Pro 3 (8 kWh)  ── feeds the ENTIRE desk (shore-power replacement)
  ├─ Vertiv 850W      → core server infrastructure (incl. cesium/R410 — the dominant load)
  ├─ CyberPower 900W  → ALL network gear (laboratory, chemistry, edge switches, APs, TEROW)
  ├─ [UPS TBD]        → workstations: Mac · MS-R1 · new Ryzen 5 (+ Intel ARC eGPU)
  └─ Wallecube DC     → NAS-X (dedicated)
```

| Unit | Rating | Type |
|---|---|---|
| EcoFlow DELTA Pro 3 + extra batt | 8192 Wh / ~4000W | LFP station (EPS ~10–30ms) |
| CyberPower CP1500AVRLCD | 1500VA / 900W, 12-outlet | line-interactive + AVR |
| Vertiv Liebert PSA6E-1500LVT | 1440VA / 850W, 8-outlet | line-interactive + AVR |
| APC Back-UPS 1050VA / 600W | — | line-interactive |
| EcoFlow River 2 | 256 Wh / 600W | LFP station |
| Wallecube NAS UPS | 150W DC | DC UPS (NAS-X only) |

**Watch-outs:**
- **⚠ Line-interactive UPSs on inverter power can "hunt."** The Vertiv/CyberPower sit on the EcoFlow's output, not the wall. Some line-interactive units flip to their own battery or chatter their AVR relay if the inverter waveform/frequency isn't clean. **Test each one on EcoFlow power before trusting it** — if one hunts, feed it from the wall instead. Not a blocker, just verify.
- **Keep each UPS under ~70% of its W rating.** cesium/R410 (~200–400W) is the single biggest load and now sits on the Vertiv — measure it first to confirm 850W has headroom for the rest of core.
- **Workstation UPS is unassigned** — free units are the APC 600W and River 2. The Ryzen+ARC box can spike >400W under GPU load; likely APC for Mac/MS-R1, Ryzen on APC or straight off the EcoFlow.

---

## 6. Service → host map (what breaks when a box is down)

| Service | Host | If it's down… |
|---|---|---|
| Internet / routing | chemistry | nothing routes; family internet drops |
| LAN switching (core) | laboratory | everything downstream of core is dark |
| Authoritative DNS `.net` | xenon | new lookups fail (unless a secondary answers); cached names keep working a while |
| Authoritative DNS `.org` + AD + SSO | radium | Keycloak logins + AD-joined auth fail |
| SoR (MariaDB) + Forgejo git | cesium | dashboard writes fail; git unreachable |
| Internal CA (step-ca) | chlorine | cert issuance/renewal stops; existing certs valid until expiry |
| MQ broker (RabbitMQ) | benzene | alert/notify pipeline can't publish |
| DNS secondary | steel | redundancy only — not user-visible if xenon is up |
| Status panel | lithium | LED panel goes dark; nothing else affected |
| Primary storage | nas-x | shared files unavailable |

---

## 7. Boot / restore order (power-on after rewiring)

Bring the spine up first, then Tier-0, then leaves. Wait for each tier to settle before the next.

1. **Power source:** EcoFlow up → confirm each downstream UPS is passing clean power (§5 hunt check).
2. **Gateway + core:** chemistry (gateway), then laboratory (core switch). Confirm WAN is up and LAN links negotiate.
3. **Edge switches / APs:** pipette → Test Tube (or hang them directly off laboratory per §4), then APs (cyclotron/beaker/flask), covalent bridge.
4. **DNS + directory (Tier-0):** xenon (BIND `.net`), radium (AD/SSO/`.org`). Verify resolution both domains before bringing up services that depend on names.
5. **SoR:** cesium (MariaDB + Forgejo). Verify DB reachable on 3306.
6. **Supporting infra:** chlorine (CA), benzene (MQ) once its route is restored, steel (DNS secondary — will AXFR from xenon), nas-x if it was powered down.
7. **Leaves:** nitrogen, valence, tungsten, lithium.
8. **SolariNet services on xenon** (once DB + DNS are healthy):
   `systemctl start solarinet-server solarinet-client solarinet-monitor unifipolld alertbridge notify solarinet-snmp.timer`
9. **Panel on lithium:** `systemctl start solari-panel`; wake the panel (dashboard operator → POST /api/panel/command, CONTROL kind 7 arg 0); `arduino-app-cli app start user:solari-glance` as the arduino user.
10. **Close the maintenance window** (from xenon, against MariaDB):
    `UPDATE maintenanceWindow SET status='completed', endsAt=NOW() WHERE reason LIKE 'Session close-out%' AND status='active';`

---

## 8. Troubleshooting playbook (offline, from your phone)

**"A host won't come back after power-on."**
- Is its **switch** up and the **uplink** negotiated? Edge boxes ride the 1G daisy chain — a dead pipette takes Test Tube with it.
- Is it on a UPS that **hunted** off inverter power (§5)? Check the UPS panel; move to wall if it flipped.
- For steel/benzene/helium specifically: they were **down since the 2026-08-06 storm** — treat as needing console/physical attention, not a fresh failure.

**"Names don't resolve."**
- If xenon is up but lookups fail, check the client's DNS is pointed at a live resolver. Pi-holes (10.0.0.254/.253) are **decommissioned** — a client still pointed there will fail. End-state points DHCP at steel + a second resolver.
- Bare hostnames (e.g. `nas-x`) need the DHCP **search domain** `akoria.net`; that fix is applied on LAN but re-verify if you reset UniFi DHCP.

**"cyclotron only links at 2.5G."** Expected — the **2.5G injector caps it**. Swap to the 10G injector (planned teardown task).

**"chemistry Wi-Fi is off / no SSID."** Intentional and permanent. The UDR7 radios were retired after root-causing repeated SoC freezes (Wi-Fi-driver hangs). APs (cyclotron/beaker/flask) carry all wireless. Do not re-enable gateway radios.

**"Something's slow."** Trace its path in §3. Three hops down the 1G edge chain shares one gigabit. If it needs more, home it directly on a laboratory port (§4 rule 2).

**"Is it safe to unplug X?"** UniFi gear: yes, no graceful stop exists — just pull it. Servers: shut down cleanly first (see exception list). NAS-X: it's real storage — quiesce/shut it down properly, don't yank it live.

---

## 9. RackWire (parallel workstream — context)

A separate SolariNet page (built by another session) for **connection/capacity planning**: it inventories what ports each asset offers, lets you draw connections, visualizes the end state, and warns before you overload the network, a UPS, or a circuit. It owns the dashboard docroot sync (www-data). When you get to the reconciliation step — desk zone × switch/port × UPS/outlet — RackWire is the tool that renders the single reconciled view. This document's §3 (ports) and §5 (power) are the raw inputs it consumes.

---

## 10. Where the secrets live (paths only — NOT in this file)

- sudo password: on the workstation (memorized).
- OIDC client secret: `solari-auth.json.bak-*` on xenon (gitignored — never commit).
- Panel principal password: `run/panel.pass`; temp operator: `run/optest.pass` (to be removed).
- UniFi API key: `run/unifi.env`.
- DB creds: `run/db.env`. AD admin pw: `/root/.ad-admin-pw` on radium.

If the phone app needs one of these to help, it will tell you which path to read on the workstation — it will never ask you to paste a secret into the chat.

---

*End of companion. If the network is dark and you're unsure what to touch first: power the EcoFlow, bring up chemistry then laboratory, confirm they link, and work outward from §7. Ask the phone app with what you can see.*
