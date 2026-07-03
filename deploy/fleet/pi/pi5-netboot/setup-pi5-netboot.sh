#!/usr/bin/env bash
# =============================================================================
# SolariNet Fleet Provisioning - Raspberry Pi 5 native netboot scaffolder
# =============================================================================
#
# Scaffolds the TFTP boot tree a Raspberry Pi 5 expects for NATIVE network boot,
# from a Pi OS image, keyed by the Pi's serial number. It also prints exactly
# what the DHCP/proxyDHCP (UniFi) must advertise.
#
# WHAT THE PI 5 BOOTLOADER DOES (background)
# ------------------------------------------
# When configured for network boot, the Pi 5's onboard bootloader (EEPROM) does
# DHCP, then TFTPs its boot files from <next-server> out of a directory named
# after the board's SERIAL number (lowercase hex, e.g. 10000000abcd1234). If the
# per-serial dir is absent it falls back to the TFTP root. It fetches, in order:
#   config.txt, start*.elf, fixup*.dat, the kernel (kernel_2712.img on Pi 5),
#   the device tree (*.dtb), overlays, and cmdline.txt. cmdline.txt tells the
#   kernel where its ROOT filesystem is - typically an NFS export served from the
#   same provisioning host (root=/dev/nfs nfsroot=...).
#
# LAYOUT THIS SCRIPT CREATES  (under the TFTP root on benzene)
#   <tftp-root>/<serial>/            <- per-Pi boot dir (config.txt, cmdline.txt,
#                                        kernel, *.elf, *.dat, *.dtb, overlays/)
#   <nfs-root>/<serial>/             <- (optional) the NFS root filesystem tree
#
# IMPORTANT - NETWORK-CONFIG-GATED
# --------------------------------
# This script only STAGES files on disk. Live serving requires:
#   (a) a TFTP server exporting <tftp-root>,
#   (b) an NFS server exporting <nfs-root> (if using NFS root),
#   (c) DHCP/proxyDHCP advertising next-server + the Pi's expectations.
# Those are activated later as part of the provisioning network rollout; this
# script prints the exact values to configure but changes NO network state.
#
# USAGE
#   sudo ./setup-pi5-netboot.sh \
#       --serial 10000000abcd1234 \
#       --hostname pi5-node-07 --fqdn pi5-node-07.lan \
#       --img /srv/solari-provision/images/raspios-lite-arm64.img \
#       [--tftp-root /srv/solari-provision/netboot/tftp] \
#       [--nfs-root  /srv/solari-provision/nfs] \
#       [--server-ip 10.5.2.50] \
#       [--admin-user solari] [--ssh-pubkey "ssh-ed25519 AAAA..."] \
#       [--server-url tls+tcp://benzene.lan:7701] [--server-name benzene] \
#       [--http-base http://10.5.2.50:8080] [--copy-rootfs]
#
# By default the NFS root is NOT populated (just scaffolded) unless --copy-rootfs
# is given, because copying a full rootfs is slow and usually staged separately.
# =============================================================================
set -euo pipefail

log()  { printf '\033[1;36m[pi5-netboot]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[pi5-netboot][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[pi5-netboot][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- defaults ---------------------------------------------------------------
SERIAL=""
HOSTNAME_S=""
FQDN=""
IMG=""
TFTP_ROOT="/srv/solari-provision/netboot/tftp"
NFS_ROOT="/srv/solari-provision/nfs"
SERVER_IP="10.5.2.50"
ADMIN_USER="solari"
SSH_PUBKEY=""
SERVER_URL=""
SERVER_NAME="benzene"
HTTP_BASE="http://10.5.2.50:8080"
COPY_ROOTFS=0

while [ $# -gt 0 ]; do
  case "$1" in
    --serial)      SERIAL="${2:?}"; shift ;;
    --hostname)    HOSTNAME_S="${2:?}"; shift ;;
    --fqdn)        FQDN="${2:?}"; shift ;;
    --img)         IMG="${2:?}"; shift ;;
    --tftp-root)   TFTP_ROOT="${2:?}"; shift ;;
    --nfs-root)    NFS_ROOT="${2:?}"; shift ;;
    --server-ip)   SERVER_IP="${2:?}"; shift ;;
    --admin-user)  ADMIN_USER="${2:?}"; shift ;;
    --ssh-pubkey)  SSH_PUBKEY="${2:?}"; shift ;;
    --server-url)  SERVER_URL="${2:?}"; shift ;;
    --server-name) SERVER_NAME="${2:?}"; shift ;;
    --http-base)   HTTP_BASE="${2:?}"; shift ;;
    --copy-rootfs) COPY_ROOTFS=1 ;;
    -h|--help)     grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# ---- validation -------------------------------------------------------------
[ "$(id -u)" -eq 0 ] || die "must run as root (loop mounts + TFTP tree). Use sudo."
[ -n "${SERIAL}" ]     || die "--serial <pi-serial> is required (see /proc/cpuinfo 'Serial')"
[ -n "${HOSTNAME_S}" ] || die "--hostname is required"
[ -n "${IMG}" ]        || die "--img <raspios.img> is required"
[ -f "${IMG}" ]        || die "image not found: ${IMG}"
[ -n "${FQDN}" ]       || FQDN="${HOSTNAME_S}"
# Serials are lowercase hex; normalize so the dir name matches what the ROM asks.
SERIAL="$(printf '%s' "${SERIAL}" | tr 'A-Z' 'a-z')"
case "${SERIAL}" in
  *[!0-9a-f]*) die "serial '${SERIAL}' is not lowercase hex" ;;
esac
for tool in losetup mount umount; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool missing: $tool"
done

TFTP_DIR="${TFTP_ROOT}/${SERIAL}"
NFS_DIR="${NFS_ROOT}/${SERIAL}"

# ---- teardown trap (same safety model as build-pi-image.sh) -----------------
LOOPDEV=""
MNT_BOOT=""
MNT_ROOT=""
cleanup() {
  set +e
  [ -n "${MNT_ROOT}" ] && mountpoint -q "${MNT_ROOT}" && umount "${MNT_ROOT}"
  [ -n "${MNT_BOOT}" ] && mountpoint -q "${MNT_BOOT}" && umount "${MNT_BOOT}"
  [ -n "${LOOPDEV}" ] && losetup "${LOOPDEV}" >/dev/null 2>&1 && losetup -d "${LOOPDEV}"
  [ -n "${MNT_ROOT}" ] && rmdir "${MNT_ROOT}" 2>/dev/null
  [ -n "${MNT_BOOT}" ] && rmdir "${MNT_BOOT}" 2>/dev/null
}
trap cleanup EXIT INT TERM

# =============================================================================
# 1. loop-mount the Pi OS image (read-only source of boot files + rootfs)
# =============================================================================
log "attaching loop device for ${IMG}"
LOOPDEV="$(losetup --find --partscan --show "${IMG}")"
[ -n "${LOOPDEV}" ] || die "losetup failed for ${IMG}"
for _ in 1 2 3 4 5; do
  [ -b "${LOOPDEV}p1" ] && [ -b "${LOOPDEV}p2" ] && break; sleep 1
done
[ -b "${LOOPDEV}p1" ] || die "boot partition ${LOOPDEV}p1 missing"
[ -b "${LOOPDEV}p2" ] || die "root partition ${LOOPDEV}p2 missing"

MNT_BOOT="$(mktemp -d)"
MNT_ROOT="$(mktemp -d)"
# Mount the boot partition read-only; we only copy FROM it.
mount -o ro "${LOOPDEV}p1" "${MNT_BOOT}" || die "mount boot failed"
mount -o ro "${LOOPDEV}p2" "${MNT_ROOT}" || die "mount root failed"

# =============================================================================
# 2. build the per-serial TFTP boot dir
# =============================================================================
log "scaffolding TFTP dir ${TFTP_DIR}"
mkdir -p "${TFTP_DIR}/overlays"

# Copy the firmware/boot payload the Pi 5 ROM fetches. We copy generously (all
# .elf/.dat/.img/.dtb + overlays) so any Pi 5 firmware revision finds what it
# wants. cp -a preserves attributes; 2>/dev/null tolerates files absent on older
# images.
log "copying boot firmware payload from image"
cp -a "${MNT_BOOT}"/*.elf   "${TFTP_DIR}/" 2>/dev/null || true
cp -a "${MNT_BOOT}"/*.dat   "${TFTP_DIR}/" 2>/dev/null || true
cp -a "${MNT_BOOT}"/*.img   "${TFTP_DIR}/" 2>/dev/null || true
cp -a "${MNT_BOOT}"/*.dtb   "${TFTP_DIR}/" 2>/dev/null || true
cp -a "${MNT_BOOT}"/overlays/. "${TFTP_DIR}/overlays/" 2>/dev/null || true
# config.txt may be regenerated below; copy the stock one as a base if present.
cp -a "${MNT_BOOT}/config.txt" "${TFTP_DIR}/config.txt" 2>/dev/null || true

# ---- config.txt : force 64-bit kernel + Pi 5 kernel image name --------------
# We APPEND a SolariNet stanza rather than rewrite, to preserve stock settings.
log "writing config.txt netboot stanza"
{
  echo ""
  echo "# --- SolariNet Fleet netboot (Pi 5) ---"
  echo "arm_64bit=1"
  echo "kernel=kernel_2712.img"   # Pi 5 (BCM2712) 64-bit kernel image
  echo "enable_uart=1"            # serial console for headless debugging
} >> "${TFTP_DIR}/config.txt"

# ---- cmdline.txt : point root at NFS ---------------------------------------
# Single line, space-separated (Pi requirement). root=/dev/nfs + nfsroot tells
# the kernel to mount its root over NFS from the provisioning host. ip=dhcp lets
# the kernel re-DHCP for the NFS mount.
log "writing cmdline.txt (NFS root)"
cat > "${TFTP_DIR}/cmdline.txt" <<EOF
console=serial0,115200 console=tty1 root=/dev/nfs nfsroot=${SERVER_IP}:${NFS_DIR},vers=4.1,proto=tcp rw ip=dhcp rootwait fsck.repair=yes
EOF

# =============================================================================
# 3. scaffold (optionally populate) the NFS root
# =============================================================================
log "scaffolding NFS root ${NFS_DIR}"
mkdir -p "${NFS_DIR}"
if [ "${COPY_ROOTFS}" -eq 1 ]; then
  # RISKY/slow: mirror the entire image rootfs into the NFS export. rsync keeps
  # it interruptible/resumable; -x keeps us on one filesystem (don't descend
  # into the ro-mounted boot bind).
  command -v rsync >/dev/null 2>&1 || die "--copy-rootfs needs rsync"
  log "copying rootfs into NFS export (this is large/slow) ..."
  rsync -aHAX --numeric-ids --info=progress2 \
    "${MNT_ROOT}/" "${NFS_DIR}/"
  # Point the NFS root's fstab away from SD partitions (root comes via NFS).
  if [ -f "${NFS_DIR}/etc/fstab" ]; then
    log "neutralizing SD-card fstab entries in NFS root"
    sed -i -E 's/^(PARTUUID=.*)$/# \1  (disabled: NFS root)/' "${NFS_DIR}/etc/fstab" || true
  fi
else
  warn "NFS root not populated (pass --copy-rootfs to mirror the image rootfs)."
  warn "Scaffolded empty ${NFS_DIR}; stage the rootfs separately."
fi

# ---- SolariNet provisioning drop-ins into the NFS root (if populated) -------
# Mirror the Pi image path: hostname, admin key, sudo, firstboot env + service.
if [ "${COPY_ROOTFS}" -eq 1 ]; then
  log "seeding SolariNet identity into NFS root"
  printf '%s\n' "${HOSTNAME_S}" > "${NFS_DIR}/etc/hostname"
  mkdir -p "${NFS_DIR}/etc/solari"
  cat > "${NFS_DIR}/etc/solari/firstboot.env" <<EOF
SOLARI_HTTP_BASE=${HTTP_BASE}
SOLARI_ARCH=arm64
SOLARI_HOSTNAME=${HOSTNAME_S}
SOLARI_FQDN=${FQDN}
SOLARI_SERVER_URL=${SERVER_URL}
SOLARI_SERVER_NAME=${SERVER_NAME}
SOLARI_SERVER_IP=${SERVER_IP}
SOLARI_PACKAGES=
EOF
  chmod 0640 "${NFS_DIR}/etc/solari/firstboot.env"
  if [ -n "${SSH_PUBKEY}" ]; then
    mkdir -p "${NFS_DIR}/home/${ADMIN_USER}/.ssh"
    printf '%s\n' "${SSH_PUBKEY}" > "${NFS_DIR}/home/${ADMIN_USER}/.ssh/authorized_keys"
    chmod 700 "${NFS_DIR}/home/${ADMIN_USER}/.ssh"
    chmod 600 "${NFS_DIR}/home/${ADMIN_USER}/.ssh/authorized_keys"
  fi
fi

# =============================================================================
# 4. clean unmount (trap also covers this)
# =============================================================================
sync
umount "${MNT_ROOT}"; MNT_ROOT=""
umount "${MNT_BOOT}"; MNT_BOOT=""
losetup -d "${LOOPDEV}"; LOOPDEV=""

# =============================================================================
# 5. tell the operator exactly what the network must advertise
# =============================================================================
cat <<EOF

$(printf '\033[1;32m')================ Pi 5 netboot staged =================$(printf '\033[0m')
  serial      : ${SERIAL}
  hostname    : ${HOSTNAME_S} (${FQDN})
  TFTP dir    : ${TFTP_DIR}
  NFS export  : ${NFS_DIR} $( [ "${COPY_ROOTFS}" -eq 1 ] && echo '(populated)' || echo '(EMPTY - stage rootfs)')

  --- DHCP / proxyDHCP (UniFi) must advertise -------------------
  next-server (TFTP server) : ${SERVER_IP}
  TFTP root on server       : ${TFTP_ROOT}
  (the Pi 5 ROM requests files from  <TFTP root>/${SERIAL}/ )

  Ensure on ${SERVER_IP}:
    * a TFTP server exports ${TFTP_ROOT}
    * an NFS server exports ${NFS_DIR} (rw, the Pi's IP or subnet)

  On the Pi 5 itself (one-time, to prefer network boot):
    sudo rpi-eeprom-config --edit   # set BOOT_ORDER=0xf21 (try NET early)
    # 0xf21 => try SD (1), then USB (nothing), then NETWORK (2), repeat (f)

  $(printf '\033[1;33m')NOTE: live serving is gated behind the provisioning network
  rollout (TFTP/NFS/DHCP). This script only staged files on disk.$(printf '\033[0m')
=======================================================
EOF

log "done."
