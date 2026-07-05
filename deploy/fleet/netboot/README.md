# SolariNet Fleet — netboot & unattended install

Netboot-driven, unattended OS installation for x86_64 (BIOS + UEFI) and arm64
machines, plus the shared first-boot agent installer used by every path
(including the Pi image builders under `../pi/`).

The whole tree is rendered per-target and rsync'd to the provisioning host
**`benzene`** (openSUSE, `10.5.2.50`), where nginx serves the HTTP artifacts at
**`http://10.5.2.50:8080/`** and a TFTP server exports the small iPXE binaries.

> Templates here contain `@@PLACEHOLDER@@` tokens. A render step (written by the
> integrator) substitutes them per-target and writes the concrete artifacts into
> the serving tree. See the **token manifest** below for every token.

## Go-live: iPXE loaders + which DHCP advertises them

`build-ipxe-loaders.sh` builds the iPXE loaders **with an embedded chain script**
and installs them into benzene's TFTP root, then stages `solari-tftp.service`
(TFTP-only, no DHCP). Embedding `chain …/boot.ipxe` into the loader is what makes
the **UniFi DHCP** path (Option B) work: UniFi advertises a single boot filename
and does not vary its reply by user-class, so a *stock* iPXE loader would re-request
the same filename forever (a boot loop). The embedded script fetches our HTTP menu
the instant iPXE starts; `|| exit` falls back to local disk for un-staged machines.

```sh
# build + install loaders, stage the TFTP-only service:
deploy/fleet/netboot/build-ipxe-loaders.sh
# ...and go live (enable service + open firewall):
deploy/fleet/netboot/build-ipxe-loaders.sh --enable
```

Then pick ONE DHCP source (they must not both answer PXE):
- **Option A — proxyDHCP on benzene**: `staging/go-live.sh` (does DHCP *and* TFTP,
  with per-arch boot-file selection). Leave `solari-tftp.service` disabled.
- **Option B — UniFi DHCP**: set Network → LAN → DHCP → Network Boot →
  next-server `10.5.2.50`, filename `ipxe-x86_64.efi`; run `solari-tftp.service`
  for the file transfer. UniFi has only one filename field, so the embedded-script
  loaders above are required for BIOS/arm64 clients to reach the menu.

---

## Boot flow (x86 + arm64 via iPXE)

```
Firmware PXE (BIOS/UEFI)
   │  DHCP: next-server=benzene, filename=undionly.kpxe | ipxe.efi | ipxe-arm64.efi
   ▼
iPXE binary (from TFTP root)
   │  re-DHCP as user-class "iPXE" → handed boot script URL (DHCP opt 67)
   ▼
http://10.5.2.50:8080/ipxe/boot.ipxe          (rendered from ipxe/boot.ipxe)
   │
   ├─ STAGE 1  chain to  ipxe/mac-<mac>.ipxe   (per-MAC staged auto-install?)
   │      • exists  → unattended install of the staged distro/arch  → DONE
   │      • 404     → fall through (safe: no staged profile = no wipe)
   │
   ├─ STAGE 2  interactive menu (operator picks distro × arch)
   │      → loads installers/<distro>/<arch>/{kernel,initrd}
   │      → kernel cmdline points at the rendered unattended config:
   │           Debian    preseed/url=…/configs/<mac>.preseed
   │           Ubuntu    ds=nocloud-net;s=…/configs/ubuntu/<mac>/
   │           openSUSE  autoyast=…/configs/<mac>.xml
   │
   └─ STAGE 3  safe default → boot from local disk (menu timeout / any failure)
```

Every unattended installer runs, in its late/post-install hook,
`installers/solari-firstboot.sh` inside the freshly installed system: it pins the
server in `/etc/hosts`, installs `solariClient`, writes `/etc/solari/client.conf`,
installs+enables the systemd unit, and stages `/etc/solari/PENDING_ENROLLMENT`.

---

## Per-MAC staging model (dashboard-driven)

To pre-stage a machine for **auto-install on next boot**, the dashboard renders
`ipxe/mac-profile.ipxe.tmpl` and writes it to:

```
http/ipxe/mac-<aa-bb-cc-dd-ee-ff>.ipxe      # <mac> = iPXE ${net0/mac:hexhyp}
```

`boot.ipxe` chains to that file **first**. Because a missing file simply 404s and
falls through to the local-disk default, staging is opt-in and a non-staged
machine is **never** wiped. The install is effectively **one-shot**: after the
target's firstboot reports success (marker `PENDING_ENROLLMENT` appears/clears),
the dashboard should delete the per-MAC file so a reboot doesn't reinstall.

---

## Directory layout on benzene

```
/srv/solari-provision/netboot/
├── tftp/                                   # TFTP root (small iPXE loaders only)
│   ├── undionly.kpxe                       # BIOS iPXE
│   ├── ipxe.efi / snponly.efi              # x86_64 UEFI iPXE
│   └── ipxe-arm64.efi                      # arm64 UEFI iPXE
└── http/                                   # nginx docroot → http://10.5.2.50:8080/
    ├── ipxe/
    │   ├── boot.ipxe                       # rendered from ipxe/boot.ipxe
    │   └── mac-<mac>.ipxe                  # rendered per staged machine
    ├── installers/
    │   ├── debian/<arch>/{linux,initrd.gz}
    │   ├── ubuntu/<arch>/{vmlinuz,initrd}
    │   ├── opensuse/<arch>/{linux,initrd,repo/…}
    │   ├── solari-firstboot.sh             # shared first-boot installer
    │   └── solariClient.<arch>             # agent binaries (x86_64/arm64/arm32)
    └── configs/
        ├── <mac>.preseed                   # Debian
        ├── <mac>.xml                       # openSUSE AutoYaST
        └── ubuntu/<mac>/{user-data,meta-data}   # Ubuntu nocloud (dir datasource)
```

> **Ubuntu needs a directory datasource.** `nocloud-net` fetches both
> `user-data` (rendered from `ubuntu.autoinstall.tmpl`) **and** a `meta-data`
> file (may be minimal but must exist, e.g. `instance-id` + `local-hostname`).
> The render step must write both; an absent `meta-data` hangs the installer.

---

## Token manifest

Every `@@TOKEN@@` used across the templates, what it means, and an example.
Tokens without a value for a given target are simply left unused by that file.

| Token | Meaning | Example |
|---|---|---|
| `@@HTTP_BASE@@` | nginx base URL for all HTTP artifacts (no trailing slash) | `http://10.5.2.50:8080` |
| `@@HOSTNAME@@` | short hostname of the target | `web-07` |
| `@@FQDN@@` | fully-qualified domain name of the target | `web-07.lan` |
| `@@DOMAIN@@` | DNS domain (Debian/AutoYaST hostname split) | `lan` |
| `@@ADMIN_USER@@` | admin account to create (SSH key, passwordless sudo) | `solari` |
| `@@ADMIN_PWHASH@@` | crypt(3) SHA-512 hash for the admin (and root) account; `*`/`!` to lock password login | `$6$abc$…` |
| `@@SSH_PUBKEY@@` | admin's SSH public key (single line) | `ssh-ed25519 AAAA… ops` |
| `@@TIMEZONE@@` | IANA timezone | `America/New_York` |
| `@@DISK@@` | **explicit** target install disk (never "first disk") | `/dev/sda` |
| `@@PACKAGES@@` | space-separated extra base packages | `vim htop chrony` |
| `@@DISTRO@@` | distro family for the per-MAC dispatch | `debian` / `ubuntu` / `opensuse` |
| `@@ARCH@@` | target architecture (matches `solariClient.<arch>` + installer dir) | `amd64` / `arm64` / `arm32` |
| `@@MAC@@` | booting NIC MAC, lowercase hyphen form (`${net0/mac:hexhyp}`) | `aa-bb-cc-dd-ee-ff` |
| `@@SERVER_URL@@` | SolariNet server ingest URL for `client.conf` `primaryUrl` | `tls+tcp://benzene.lan:7701` |
| `@@SERVER_NAME@@` | server hostname pinned in `/etc/hosts` / TLS SAN | `benzene` |
| `@@SERVER_IP@@` | server IP for the `/etc/hosts` pin | `10.5.2.50` |

Notes:
- `@@ARCH@@` doubles as the installer subdirectory (`installers/<distro>/<arch>/`)
  **and** the agent-binary suffix (`installers/solariClient.<arch>`). Keep the
  naming consistent between the render step and the artifacts you stage.
- `solari-firstboot.sh` also accepts each token at **runtime** via a
  `SOLARI_<TOKEN>` env var (e.g. `SOLARI_HTTP_BASE`), which is how the Pi
  first-boot service drives the same script without rendering.

---

## Files in this tree

| File | Role |
|---|---|
| `ipxe/boot.ipxe` | top-level iPXE menu + per-MAC chain + safe local-disk default |
| `ipxe/mac-profile.ipxe.tmpl` | per-MAC staged auto-install entry (template) |
| `configs/debian.preseed.tmpl` | Debian 12 preseed |
| `configs/ubuntu.autoinstall.tmpl` | Ubuntu 24.04 autoinstall (`user-data`) |
| `configs/opensuse.autoyast.xml.tmpl` | openSUSE Leap/Tumbleweed AutoYaST |
| `installers/solari-firstboot.sh` | shared POSIX-sh first-boot agent installer |

---

## What is immediately usable vs network-config-gated

**Immediately usable (no network changes):**
- Rendering configs from the templates.
- Building Pi images (`../pi/build-pi-image.sh`) and scaffolding Pi 5 netboot
  trees (`../pi/pi5-netboot/`).
- Serving artifacts over HTTP/TFTP from `benzene` once nginx/TFTP are up.
- The first-boot installer + enrollment staging (marker-based).

**Network-config-gated (activated in the later provisioning-network rollout):**
- **proxyDHCP / `next-server` advertisement** (UniFi) that steers PXE clients to
  benzene and hands out the iPXE boot filename / `boot.ipxe` URL. Nothing
  netboots until this is live.
- NFS/DHCP for the Pi 5 diskless root (see `../pi/pi5-netboot/README.md`).

**Integrator-owned (stubs to wire):**
- The **render step** that substitutes `@@TOKEN@@`s and lays out the serving
  tree (including Ubuntu's `meta-data`).
- The **real enrollment call** in `solari-firstboot.sh` (clearly delimited
  `SOLARINET ENROLLMENT HOOK` block) — swap the `PENDING_ENROLLMENT` marker for
  `deploy/enrollment/solari-enroll.sh` with an out-of-band one-time token.
