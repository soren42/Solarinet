# SolariNet Fleet — Raspberry Pi provisioning

Two ways to bring a Raspberry Pi into the fleet. Pick based on the board and
whether you want local media.

| | **Pi 5 native netboot** | **Customized image (Pi 4 / 3 / Zero 2 W)** |
|---|---|---|
| Local media | none (TFTP + NFS root) | SD card or USB (flashed) |
| Script | [`pi5-netboot/setup-pi5-netboot.sh`](pi5-netboot/setup-pi5-netboot.sh) | [`build-pi-image.sh`](build-pi-image.sh) |
| Best for | Pi 5, diskless/immutable, mass re-provision | anything pre-Pi-5, air-gapped, one-offs |
| Depends on | TFTP+NFS+DHCP (gated rollout) | just a flasher (`dd`/rpi-imager) |
| SolariNet bring-up | `firstboot.env` + `solari-firstboot.service` on the NFS root | same, baked into the image |

Both paths converge on the **same first-boot mechanism**: an
`/etc/solari/firstboot.env` file and a `solari-firstboot.service` oneshot that
fetches and runs `../netboot/installers/solari-firstboot.sh`, which installs the
`solariClient` agent, writes `/etc/solari/client.conf`, and stages
`/etc/solari/PENDING_ENROLLMENT`.

---

## Decision tree

```
Is the board a Raspberry Pi 5?
├── yes → is the provisioning network (TFTP/NFS/DHCP) live?
│         ├── yes → pi5-netboot/setup-pi5-netboot.sh   (diskless, preferred)
│         └── no  → build-pi-image.sh (SD/USB image now; switch to netboot later)
└── no  → build-pi-image.sh   (Pi 4/3/Zero 2 W → flash SD or USB)
```

---

## Quick start — customized image (Pi 4 fallback)

```sh
sudo ./build-pi-image.sh \
    --hostname pi-sensor-01 --fqdn pi-sensor-01.lan \
    --arch arm64 --admin-user solari \
    --ssh-pubkey "ssh-ed25519 AAAA... admin@ops" \
    --server-url tls+tcp://benzene.lan:7701 \
    --server-name benzene --server-ip 10.5.2.50 \
    --packages "vim htop" \
    --out /srv/solari-provision/images/pi-sensor-01.img
# then: sudo dd if=...pi-sensor-01.img of=/dev/sdX bs=4M conv=fsync status=progress
```

Downloads the Pi OS Lite base if `--base-img` is omitted, loop-mounts boot+root,
enables SSH, sets the admin user (key-only, passwordless sudo), hostname, and
the SolariNet first-boot service, then emits the `.img` path. `--arch arm32`
builds the `armhf` variant for 32-bit boards.

## Quick start — Pi 5 netboot

See [`pi5-netboot/README.md`](pi5-netboot/README.md).

---

## Requirements

- Root (loop mounts).
- `losetup`, `mount`, `curl`; `xz` for compressed base images; `rsync` for
  `--copy-rootfs` on the Pi 5 path.
- Runs on the provisioning host (`benzene`). Both scripts clean up their loop
  devices/mounts on any exit via a `trap`.
