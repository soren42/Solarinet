# SolariNet Fleet — Raspberry Pi 5 native network boot

This directory documents and scaffolds the **Raspberry Pi 5 native netboot**
path. Unlike the Pi 4 fallback (which flashes a customized SD/USB image, see
`../build-pi-image.sh`), a Pi 5 can boot with **no local media** by pulling its
firmware + kernel over TFTP and mounting its root filesystem over NFS from the
provisioning host (`benzene`, `10.5.2.50`).

> **Status: network-config-gated.** The scaffolding script writes files to disk
> only. Live serving requires TFTP + NFS exports and DHCP/proxyDHCP changes that
> are activated later in the provisioning-network rollout. Nothing here changes
> network state.

---

## How Pi 5 network boot works

The Pi 5's onboard bootloader (in EEPROM) can be ordered to try the network:

1. **DHCP** — the Pi requests an address. It reads `next-server` (the TFTP
   server) from the DHCP offer (or from a proxyDHCP responder such as UniFi).
2. **TFTP** — the ROM fetches its boot files from a directory **named after the
   board serial number** (lowercase hex, e.g. `10000000abcd1234`) under the
   TFTP root. If that per-serial directory is missing it falls back to the TFTP
   root itself. Files fetched include:
   - `config.txt` — firmware config (we set `arm_64bit=1`,
     `kernel=kernel_2712.img`).
   - `start*.elf`, `fixup*.dat` — GPU firmware/loader.
   - `kernel_2712.img` — the Pi 5 (BCM2712) 64-bit kernel.
   - `*.dtb` + `overlays/` — device tree + overlays.
   - `cmdline.txt` — kernel command line. **This is where the root filesystem is
     declared.** We point it at NFS:
     `root=/dev/nfs nfsroot=<server-ip>:<nfs-dir>,vers=4.1 rw ip=dhcp rootwait`.
3. **NFS root** — the kernel mounts its root over NFS and boots. The SolariNet
   agent + enrollment come up via the same first-boot mechanism as the image
   path (`/etc/solari/firstboot.env` + `solari-firstboot.service`).

Find a Pi's serial with `cat /proc/cpuinfo | grep Serial` (or read it off the
bootloader diagnostics screen).

---

## TFTP tree layout on benzene

```
/srv/solari-provision/netboot/tftp/            <- TFTP root (next-server target)
└── <serial>/                                  <- one dir per Pi 5, e.g. 10000000abcd1234
    ├── config.txt                             <- stock + SolariNet netboot stanza
    ├── cmdline.txt                            <- NFS root kernel cmdline
    ├── kernel_2712.img
    ├── start*.elf  fixup*.dat
    ├── *.dtb
    └── overlays/

/srv/solari-provision/nfs/
└── <serial>/                                  <- NFS-exported root filesystem for that Pi
```

---

## Usage

Scaffold a Pi 5's boot tree from a Pi OS Lite (arm64) image:

```sh
sudo ./setup-pi5-netboot.sh \
    --serial 10000000abcd1234 \
    --hostname pi5-node-07 --fqdn pi5-node-07.lan \
    --img /srv/solari-provision/images/raspios-lite-arm64.img \
    --server-ip 10.5.2.50 \
    --ssh-pubkey "ssh-ed25519 AAAA... admin@ops" \
    --server-url tls+tcp://benzene.lan:7701 --server-name benzene \
    --copy-rootfs          # omit to scaffold the NFS dir empty (stage rootfs later)
```

The script loop-mounts the image read-only, copies the boot payload into the
per-serial TFTP dir, writes `config.txt`/`cmdline.txt`, scaffolds (and with
`--copy-rootfs`, populates) the NFS export, then **prints exactly what DHCP must
advertise** (next-server/TFTP root) and how to set the Pi's `BOOT_ORDER`.

### One-time on the Pi 5 (prefer network boot)

```sh
sudo rpi-eeprom-config --edit
# set:  BOOT_ORDER=0xf21     # 1=SD, 2=network, f=retry -> try SD then NET, loop
```

Use `BOOT_ORDER=0xf2` to try **network first**, or `0xf21` to try SD then
network (safer while validating).

---

## What must be activated later (gated)

- A **TFTP server** exporting `/srv/solari-provision/netboot/tftp`.
- An **NFS server** exporting `/srv/solari-provision/nfs/<serial>` to the Pi.
- **DHCP/proxyDHCP** (UniFi) advertising `next-server = 10.5.2.50`. The Pi ROM
  supplies its own serial-named path; you generally do **not** set a boot
  filename for Pi native netboot (contrast with x86 iPXE).

Until those are live, `setup-pi5-netboot.sh` is safe to run repeatedly to
pre-stage machines.
