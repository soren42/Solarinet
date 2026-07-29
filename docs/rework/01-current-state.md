# Current State — barebones fleet (2026-07-29)

*What is actually running right now, before the desk/network rework. Everything else in the fleet is powered down.*

## Network (all up)
| Device | Role | IP | Notes |
|--------|------|----|-------|
| **chemistry** | Gateway — UDM-Pro-Max | 10.0.0.1 | New; replaced the failed UDR7. No built-in Wi-Fi. WAN: GFiber (public 136.57.196.188) + failover |
| **laboratory** | Core switch — USW-Pro-Max-16 (non-PoE) | 10.0.0.3 | 10G SFP+ backbone to gateway; SFP+17 → XGS AP |
| Test Tube · slide · pipette | Edge switches — USM8P (1G, PoE) | .33/.32/.31 | Daisy-chained; carry APs + family-room gear |
| flask · beaker | APs — U7 Pro | .8 / .7 | |
| **cyclotron** | AP — U7 Pro XGS (10G-capable) | .6 | On laboratory SFP+17, **2.5G today** (interim injector) |
| covalent | Wireless building bridge | .160 | Craft ↔ family-room link |

9/9 UniFi devices online · ~41 wireless clients (household).

## Servers (currently up)
| Host | Role | IP | Serves |
|------|------|----|--------|
| **xenon** | Monitoring + web + DNS primary | 10.0.0.20 | SolariNet dashboard `:9443`, DNS (akoria.net BIND), landing page `akoria.net/`, the `sor-apply-dns` daemon |
| **cesium** | Git + system-of-record | 10.1.0.200 | Forgejo `:3000`, SoR MariaDB (source of truth). Dell R410 — loud/hot |
| **radium** | Identity + DNS | 10.1.0.10 | Samba AD DC, Keycloak SSO `sso.akoria.org:8443`, akoria.org DNS, SSSD. Pi CM5 |
| **chlorine** | Internal CA | 10.7.0.10 | step-ca PKI |
| **nas-x** | Storage (NAS) | 10.0.0.10 | Fleet storage |

## Currently DOWN (deployed, to bring back)
- **steel** (10.0.0.11) — DNS secondary/resolver + RPZ ad-block (Zima). *Its being down is why only xenon answers DNS today.*
- **benzene** (10.5.2.50) — eGPU host **+ RabbitMQ broker**. *MQ down = the SoR→DNS CDC pipeline is paused (initial-render still works; live auto-sync doesn't).*

## Known caveats today
- **RabbitMQ down** (on benzene) → SoR changes don't auto-propagate to DNS. Force with `sudo systemctl restart sor-apply-dns` on xenon.
- **steel down** → single DNS server (xenon). Clients pointed only at steel would fail.
- **cyclotron/XGS at 2.5G**, not 10G, until the 10G PoE++ injector goes in.
- The broader fleet (amino, tungsten, quanta, photon, boson, plutonium, astatine, neutrino, Home Assistant, KVMs, etc.) is powered off.
