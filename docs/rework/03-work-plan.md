# Work Plan — graceful shutdown → rework → graceful restart

> **Historical plan — superseded operational facts (2026-10-21).** This July plan is retained
> as a record of the proposed physical rework. Its service boot order, port maps, and DNS
> topology are not a current runbook. In particular, Steel is the inventory-labeled ZimaBlade
> at laboratory port 12 and is unreachable; it is not the active DNS secondary. The Khadas VIM4
> `chlorine` now runs BIND as a secondary for `akoria.net` and generated reverse zones. See
> [the current stabilization baseline](../SolariNet_Stabilization_Baseline_2026-10-20.html)
> before using any operational sequence here.

*Order matters: shut down dependents before dependencies; bring the foundation up first on the way back. **Halt, don't reboot.** Keep the router (chemistry) up until the very end and up first on return — it is DNS/DHCP for everything.*

---

## PHASE 1 — Graceful shutdown (before pulling power)

**A. Silence + freeze the control plane first**
1. On xenon: `sudo systemctl stop notify` *(prevents host-down alert spam as things go dark)*. If tachyon/benzene run notify, stop there too.
2. Note anything mid-write (no long jobs running). SoR (cesium MariaDB) should be idle.

**B. Halt hosts — leaf → core (each: `sudo systemctl poweroff` / `shutdown -h now`)**
3. **Workstations & clients** — Mac (hydrogen), MS-R1, KVMs, Home Assistant, Radio Pi, misc SBCs (amino, tungsten, quanta, photon, boson, plutonium, astatine, neutrino, NanoPi-R1, etc.).
4. **Dependent servers** — nas-x, chlorine (CA and BIND secondary), benzene (if up). Steel is currently unreachable and is not part of the shutdown sequence; handle it after console redeployment.
5. **Core servers, in this order:** cesium (Forgejo/SoR) → radium (AD/DNS/SSO) → **xenon last** (monitoring/DNS primary — you'll lose your dashboard + local tooling when it goes).
6. Confirm each is truly down (no ping) before pulling its power.

**C. Network last**
7. APs (flask, beaker, cyclotron, covalent) → edge switches (Test Tube, slide, pipette + dumb switches) → **laboratory** (core switch) → **chemistry** (gateway) last.
8. Now everything's safe to unplug. Kill the UPS/EcoFlow outputs.

> If you want the internet/router alive while you work the desk, leave **chemistry + one uplink** powered and skip step 7 for it until the very end.

---

## PHASE 2 — The rework (power off)

Physical build per **02-ideal-state.md**: mount the 3U rack under the pull-out (laboratory + cesium + PDU), stack the left shelf (SBC/SFF/KVM), set the right shelf (switches + NAS), wall-mount the monitor, run power strips to the desk back, cable per the network end-state (one switch per zone → laboratory). Wire power per the topology (EcoFlow source → Vertiv/CyberPower/APC/Wallecube branches).

**Test as you go:** EcoFlow-as-source → confirm each downstream UPS passes through without hunting *before* trusting it. Verify airflow around cesium/R410.

---

## PHASE 3 — Graceful restart (foundation → up)

**A. Power + network foundation**
1. EcoFlow on → confirm downstream UPSs settle (no chatter).
2. **chemistry** (gateway) → wait for WAN/internet.
3. **laboratory** (core switch) → then edge switches → then APs. Confirm UniFi shows devices adopting/online.

**B. Core services (dependency order)**
4. **radium** first — it's AD + DNS(akoria.org) + SSO; SSSD-joined hosts need it. Wait for Samba AD DC + Keycloak up (see auth-recovery note below).
5. **xenon** — DNS primary (akoria.net) + dashboard. Then `sudo systemctl restart sor-apply-dns` once MQ is back (step 6).
6. **benzene** — brings **RabbitMQ** back → SoR→DNS live sync resumes. Also the eGPU host.
7. **cesium** — Forgejo + SoR MariaDB.
8. **chlorine** — Khadas VIM4 at `10.7.0.10`; start its authoritative BIND secondary and verify zone transfers from xenon. It also hosts the internal CA.
9. **nas-x** (storage). Do not enable or advertise Steel as DNS until its ZimaBlade has been redeployed and independently verified.

**C. New provisioning**
10. **Ryzen + Intel Arc eGPU** — fresh OS, enroll solariClient, join, name it.
11. **tachyon** — provision, set as fleet NTP, point the notify service at it, re-enable `notify`.

**D. Verify (dashboard is the scoreboard)**
12. SolariNet dashboard `dashboard.akoria.net` green · primary DNS answers at xenon (`10.0.0.20`) and secondary DNS answers at chlorine (`10.7.0.10`) · SSO login works · Forgejo up · RabbitMQ connected (no AMQP errors in `sor-apply-dns` log) · every UPS < ~70%. Steel at laboratory switch port 12 is the inventory-labeled ZimaBlade; it is not currently reachable and is not a DNS resolver.

---

## Gotchas / recovery notes
- **Auth after radium reboot:** if SSO fails, it's usually `samba-ad-dc` failing to start (an IPv6 bind race). Fix: `ssh jason@radium.akoria.org 'sudo systemctl restart samba-ad-dc'`; verify `:636` LDAPS listens. (A resilience drop-in should now auto-restart it.)
- **DNS "won't resolve akoria.net":** xenon's BIND must be up; `akoria.net` apex → 10.0.0.20 comes from `netdb/gen-zones.py`. Re-render with `sudo systemctl restart sor-apply-dns`.
- **cesium is a loud R410** — expect fan noise on boot; that's normal.
- **Don't reboot the UDM-Pro-Max needlessly** — it's the new, healthy gateway; treat it gently.
