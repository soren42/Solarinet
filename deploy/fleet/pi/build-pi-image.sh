#!/usr/bin/env bash
# =============================================================================
# SolariNet Fleet Provisioning - Raspberry Pi OS image customizer
# =============================================================================
#
# Builds a ready-to-flash Raspberry Pi OS Lite image, pre-seeded with:
#   - hostname / FQDN
#   - an admin user (SSH key, passwordless sudo)
#   - SSH enabled on first boot
#   - a first-boot service that installs the base packages + the SolariNet agent
#     (mirroring installers/solari-firstboot.sh) and stages enrollment.
#
# This is the Pi 4 (and Pi 3 / Zero 2 W) *USB/SD image* path. The Pi 5 native
# network-boot path lives under pi5-netboot/. See ../pi/README.md for the
# decision tree.
#
# It loop-mounts the image's boot + root partitions, edits them, and unmounts
# cleanly. Every risky step (losetup, mount, rm, cp into a mount) is guarded and
# commented; a trap tears down loop devices/mounts on ANY exit.
#
# USAGE
#   sudo ./build-pi-image.sh \
#       --hostname pi-sensor-01 --fqdn pi-sensor-01.lan \
#       --arch arm64 --admin-user solari \
#       --ssh-pubkey "ssh-ed25519 AAAA... admin@ops" \
#       --server-url tls+tcp://benzene.lan:7701 \
#       --server-name benzene --server-ip 10.5.2.50 \
#       --packages "vim htop" \
#       --out /srv/solari-provision/images/pi-sensor-01.img \
#       [--base-img /path/or/https-url/to/raspios-lite.img[.xz]]
#
# NOTES
#   * Requires root (loop mounts). Requires: losetup, mount, curl, and either
#     xz or unxz for compressed base images.
#   * Does NOT flash anything - it only writes an .img file you then dd/rpi-imager.
#   * Idempotent-ish: re-running overwrites --out. It never touches the host's
#     own disks; it only ever mounts the loop device it created.
# =============================================================================
set -euo pipefail

# ---- pretty logging (matches deploy/enrollment/solari-enroll.sh style) ------
log()  { printf '\033[1;36m[pi-image]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[pi-image][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[pi-image][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- defaults ---------------------------------------------------------------
HOSTNAME_S=""
FQDN=""
ARCH="arm64"
ADMIN_USER="solari"
SSH_PUBKEY=""
SERVER_URL=""
SERVER_NAME=""
SERVER_IP=""
PACKAGES=""
OUT=""
BASE_IMG=""

# Default upstream Pi OS Lite images (Bookworm). Overridable with --base-img.
BASE_URL_ARM64="https://downloads.raspberrypi.com/raspios_lite_arm64_latest"
BASE_URL_ARM32="https://downloads.raspberrypi.com/raspios_lite_armhf_latest"

# Where the SolariNet firstboot script is fetched from on first boot. The Pi
# talks to benzene's HTTP server; default matches the brief (nginx :8080).
HTTP_BASE="${SOLARI_HTTP_BASE:-http://10.5.2.50:8080}"

# ---- arg parsing ------------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --hostname)    HOSTNAME_S="${2:?}"; shift ;;
    --fqdn)        FQDN="${2:?}"; shift ;;
    --arch)        ARCH="${2:?}"; shift ;;
    --admin-user)  ADMIN_USER="${2:?}"; shift ;;
    --ssh-pubkey)  SSH_PUBKEY="${2:?}"; shift ;;
    --server-url)  SERVER_URL="${2:?}"; shift ;;
    --server-name) SERVER_NAME="${2:?}"; shift ;;
    --server-ip)   SERVER_IP="${2:?}"; shift ;;
    --packages)    PACKAGES="${2:?}"; shift ;;
    --out)         OUT="${2:?}"; shift ;;
    --base-img)    BASE_IMG="${2:?}"; shift ;;
    --http-base)   HTTP_BASE="${2:?}"; shift ;;
    -h|--help)     grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# ---- validation -------------------------------------------------------------
[ "$(id -u)" -eq 0 ] || die "must run as root (loop mounts). Re-run with sudo."
[ -n "${HOSTNAME_S}" ] || die "--hostname is required"
[ -n "${SSH_PUBKEY}" ] || die "--ssh-pubkey is required (no password login)"
[ -n "${OUT}" ]        || die "--out <file.img> is required"
[ -n "${FQDN}" ]       || FQDN="${HOSTNAME_S}"
case "${ARCH}" in
  arm64|arm32) ;;
  *) die "--arch must be arm64 or arm32 (got '${ARCH}')" ;;
esac
for tool in losetup mount umount curl; do
  command -v "$tool" >/dev/null 2>&1 || die "required tool missing: $tool"
done

# ---- teardown trap ----------------------------------------------------------
# These globals are populated as we set things up; the trap uses them to unwind
# in reverse order no matter where we exit (success, error, or Ctrl-C). This is
# the single most important safety mechanism in this script: a leaked loop
# device or bind mount can wedge the host, so we ALWAYS clean up.
WORKDIR=""
LOOPDEV=""
MNT_BOOT=""
MNT_ROOT=""

cleanup() {
  set +e
  # Unmount in reverse dependency order. 'mountpoint -q' avoids noisy errors.
  [ -n "${MNT_ROOT}" ] && mountpoint -q "${MNT_ROOT}" && umount "${MNT_ROOT}"
  [ -n "${MNT_BOOT}" ] && mountpoint -q "${MNT_BOOT}" && umount "${MNT_BOOT}"
  # Detach the loop device (this also removes the partition maps from -P).
  [ -n "${LOOPDEV}" ] && losetup "${LOOPDEV}" >/dev/null 2>&1 && losetup -d "${LOOPDEV}"
  # Remove scratch mount points (empty dirs only).
  [ -n "${MNT_ROOT}" ] && rmdir "${MNT_ROOT}" 2>/dev/null
  [ -n "${MNT_BOOT}" ] && rmdir "${MNT_BOOT}" 2>/dev/null
  [ -n "${WORKDIR}" ] && [ -d "${WORKDIR}" ] && rm -rf "${WORKDIR}"
}
trap cleanup EXIT INT TERM

WORKDIR="$(mktemp -d /tmp/solari-pi.XXXXXX)"
log "workdir ${WORKDIR}"

# =============================================================================
# 1. obtain the base image
# =============================================================================
# Resolve BASE_IMG into a plain .img at ${WORKDIR}/base.img. Accepts:
#   * a local .img            -> copied
#   * a local .img.xz         -> decompressed
#   * an https URL (.img/.xz) -> downloaded then decompressed if needed
#   * (default) the upstream Pi OS "latest" redirect for the chosen arch
RAW_IMG="${WORKDIR}/base.img"

if [ -z "${BASE_IMG}" ]; then
  [ "${ARCH}" = "arm64" ] && BASE_IMG="${BASE_URL_ARM64}" || BASE_IMG="${BASE_URL_ARM32}"
  log "no --base-img; using upstream Pi OS Lite (${ARCH})"
fi

DL="${WORKDIR}/download"
case "${BASE_IMG}" in
  http://*|https://*)
    log "downloading base image: ${BASE_IMG}"
    # Follow redirects (-L); the "latest" URLs 302 to a versioned .img.xz.
    curl --fail --location --show-error --output "${DL}" "${BASE_IMG}"
    ;;
  *)
    [ -f "${BASE_IMG}" ] || die "base image not found: ${BASE_IMG}"
    log "using local base image: ${BASE_IMG}"
    cp -f "${BASE_IMG}" "${DL}"
    ;;
esac

# Decompress if it's xz-compressed (Pi OS ships .img.xz). Detect by magic bytes
# rather than filename, since the "latest" redirect has no extension.
if head -c 6 "${DL}" | grep -q $'\xFD7zXZ'; then
  log "decompressing xz base image"
  command -v xz >/dev/null 2>&1 || die "xz required to decompress the base image"
  xz --decompress --stdout "${DL}" > "${RAW_IMG}"
else
  mv "${DL}" "${RAW_IMG}"
fi
[ -s "${RAW_IMG}" ] || die "base image is empty after extraction"

# =============================================================================
# 2. grow the image a little so package installs on first boot have room
# =============================================================================
# Pi OS Lite roots are tight. Add 1 GiB of slack (truncate just extends the
# file; the root FS is grown on first boot by Pi OS's own init_resize).
log "padding image by 1 GiB of free space"
truncate -s +1G "${RAW_IMG}"

# =============================================================================
# 3. copy to the output path, then loop-mount THE COPY
# =============================================================================
# We do all edits on ${OUT} so a failure never corrupts a cached base image.
mkdir -p "$(dirname "${OUT}")"
log "writing output image ${OUT}"
cp -f "${RAW_IMG}" "${OUT}"

# ---- loop-mount (RISKY: creates a block device backed by the file) ---------
# losetup -P scans the partition table and creates ${LOOPDEV}p1 (boot, FAT32)
# and ${LOOPDEV}p2 (root, ext4). --show prints the device we grabbed so we only
# ever act on OUR loop device, never a host disk.
log "attaching loop device for ${OUT}"
LOOPDEV="$(losetup --find --partscan --show "${OUT}")"
[ -n "${LOOPDEV}" ] || die "losetup failed to attach ${OUT}"
log "loop device: ${LOOPDEV}"

# Wait for the kernel to create the partition nodes (udev can lag).
for _ in 1 2 3 4 5; do
  [ -b "${LOOPDEV}p1" ] && [ -b "${LOOPDEV}p2" ] && break
  sleep 1
done
[ -b "${LOOPDEV}p1" ] || die "boot partition ${LOOPDEV}p1 did not appear"
[ -b "${LOOPDEV}p2" ] || die "root partition ${LOOPDEV}p2 did not appear"

MNT_BOOT="${WORKDIR}/boot"
MNT_ROOT="${WORKDIR}/root"
mkdir -p "${MNT_BOOT}" "${MNT_ROOT}"

# Mount boot (FAT) and root (ext4). If either fails, the trap detaches the loop.
log "mounting boot (${LOOPDEV}p1) and root (${LOOPDEV}p2)"
mount "${LOOPDEV}p1" "${MNT_BOOT}" || die "could not mount boot partition"
mount "${LOOPDEV}p2" "${MNT_ROOT}" || die "could not mount root partition"

# =============================================================================
# 4. customize the BOOT partition
# =============================================================================
# On Bookworm the boot files live in the FAT partition; some tooling moved them
# under /boot/firmware once running, but on the raw image they are at the FAT
# root. We write to the FAT root (${MNT_BOOT}) which is correct for flashing.

# 4a. enable SSH: the presence of a file named 'ssh' turns on sshd on first boot.
log "enabling SSH (touch /boot/ssh)"
: > "${MNT_BOOT}/ssh"

# 4b. userconf.txt: create the first user headlessly. Format is USER:CRYPT-HASH.
# We set a LOCKED password ('*' cannot match) so the account is SSH-key-only;
# the key itself is installed into the root FS below.
log "writing userconf.txt for user ${ADMIN_USER} (locked password, key-only)"
printf '%s:%s\n' "${ADMIN_USER}" '*' > "${MNT_BOOT}/userconf.txt"

# =============================================================================
# 5. customize the ROOT partition
# =============================================================================
# NOTE: we cannot run ARM binaries in this chroot from an x86_64 builder without
# qemu-user-static/binfmt. To stay portable we do NOT chroot; instead we drop
# files + a systemd first-boot service that does the real work on the Pi itself.

# 5a. hostname + hosts
log "setting hostname ${HOSTNAME_S}"
printf '%s\n' "${HOSTNAME_S}" > "${MNT_ROOT}/etc/hostname"
# Update the 127.0.1.1 line (Debian/Pi convention) to the new hostname.
if grep -qE '^127\.0\.1\.1' "${MNT_ROOT}/etc/hosts" 2>/dev/null; then
  sed -i -E "s/^127\.0\.1\.1.*/127.0.1.1\t${HOSTNAME_S} ${FQDN}/" "${MNT_ROOT}/etc/hosts"
else
  printf '127.0.1.1\t%s %s\n' "${HOSTNAME_S}" "${FQDN}" >> "${MNT_ROOT}/etc/hosts"
fi

# 5b. admin SSH authorized_keys (created before the user exists; firstboot fixes
# ownership once the account is materialized by userconf).
ADMIN_HOME="${MNT_ROOT}/home/${ADMIN_USER}"
log "installing admin SSH key for ${ADMIN_USER}"
mkdir -p "${ADMIN_HOME}/.ssh"
printf '%s\n' "${SSH_PUBKEY}" > "${ADMIN_HOME}/.ssh/authorized_keys"
chmod 700 "${ADMIN_HOME}/.ssh"
chmod 600 "${ADMIN_HOME}/.ssh/authorized_keys"

# 5c. passwordless sudo for the admin user
log "granting passwordless sudo to ${ADMIN_USER}"
printf '%s ALL=(ALL) NOPASSWD:ALL\n' "${ADMIN_USER}" \
  > "${MNT_ROOT}/etc/sudoers.d/010-solari-admin"
chmod 440 "${MNT_ROOT}/etc/sudoers.d/010-solari-admin"

# 5d. drop the SolariNet firstboot env + a systemd oneshot that runs it once.
# We bake the tokens into an env file the service sources. The heavy lifting
# (fetch binary, write client.conf, enrollment marker) is done by the shared
# solari-firstboot.sh, fetched at first boot so the image stays in sync with the
# server-side script.
log "installing SolariNet first-boot service"
mkdir -p "${MNT_ROOT}/etc/solari"
cat > "${MNT_ROOT}/etc/solari/firstboot.env" <<EOF
SOLARI_HTTP_BASE=${HTTP_BASE}
SOLARI_ARCH=${ARCH}
SOLARI_HOSTNAME=${HOSTNAME_S}
SOLARI_FQDN=${FQDN}
SOLARI_SERVER_URL=${SERVER_URL}
SOLARI_SERVER_NAME=${SERVER_NAME}
SOLARI_SERVER_IP=${SERVER_IP}
SOLARI_PACKAGES=${PACKAGES}
EOF
chmod 0640 "${MNT_ROOT}/etc/solari/firstboot.env"

# The oneshot service: on first boot install extra packages, fetch + run
# solari-firstboot.sh, then disable itself so it never runs again.
cat > "${MNT_ROOT}/etc/systemd/system/solari-firstboot.service" <<'EOF'
[Unit]
Description=SolariNet Pi first-boot provisioning
After=network-online.target
Wants=network-online.target
# Guard: only run while the marker is absent. The ExecStart disables the unit,
# but this is belt-and-suspenders against a re-enable.
ConditionPathExists=!/etc/solari/.firstboot-done

[Service]
Type=oneshot
RemainAfterExit=yes
EnvironmentFile=/etc/solari/firstboot.env
ExecStart=/usr/local/sbin/solari-pi-firstboot.sh
ExecStartPost=/bin/systemctl disable solari-firstboot.service

[Install]
WantedBy=multi-user.target
EOF

# Wrapper that installs @@PACKAGES@@ (via apt on Pi OS), then delegates to the
# shared solari-firstboot.sh. Kept tiny; the shared script does the SolariNet
# work so there is one source of truth.
cat > "${MNT_ROOT}/usr/local/sbin/solari-pi-firstboot.sh" <<'EOF'
#!/bin/sh
# SolariNet Pi first-boot wrapper (installed by build-pi-image.sh).
set -eu
LOG_TAG=solari-pi-firstboot
log() { printf '[%s] %s\n' "$LOG_TAG" "$*"; logger -t "$LOG_TAG" "$*" 2>/dev/null || true; }

# Package install (best-effort; do not fail the whole provision on a mirror hiccup).
if [ -n "${SOLARI_PACKAGES:-}" ]; then
  log "installing packages: ${SOLARI_PACKAGES}"
  export DEBIAN_FRONTEND=noninteractive
  apt-get update  || log "apt-get update failed (continuing)"
  # shellcheck disable=SC2086
  apt-get install -y curl ca-certificates sudo ${SOLARI_PACKAGES} \
    || log "package install had errors (continuing)"
else
  apt-get install -y curl ca-certificates sudo >/dev/null 2>&1 || true
fi

# Fetch + run the shared first-boot installer (agent + enrollment marker).
if command -v curl >/dev/null 2>&1; then
  curl --fail --silent --show-error --location \
    "${SOLARI_HTTP_BASE}/installers/solari-firstboot.sh" \
    -o /usr/local/sbin/solari-firstboot.sh && \
  chmod 0755 /usr/local/sbin/solari-firstboot.sh && \
  sh /usr/local/sbin/solari-firstboot.sh || log "solari-firstboot.sh failed"
else
  log "curl missing; cannot fetch solari-firstboot.sh"
fi

# Mark done so the oneshot never repeats.
mkdir -p /etc/solari
: > /etc/solari/.firstboot-done
log "first-boot complete"
EOF
chmod 0755 "${MNT_ROOT}/usr/local/sbin/solari-pi-firstboot.sh"

# Enable the service by creating the multi-user.target want symlink directly
# (we can't run systemctl against the offline image).
log "enabling solari-firstboot.service in the offline image"
mkdir -p "${MNT_ROOT}/etc/systemd/system/multi-user.target.wants"
ln -sf ../solari-firstboot.service \
  "${MNT_ROOT}/etc/systemd/system/multi-user.target.wants/solari-firstboot.service"

# =============================================================================
# 6. sync + unmount (trap will also handle this, but do it explicitly)
# =============================================================================
log "syncing and unmounting"
sync
umount "${MNT_ROOT}"; MNT_ROOT=""
umount "${MNT_BOOT}"; MNT_BOOT=""
losetup -d "${LOOPDEV}"; LOOPDEV=""

log "done. Flash with:  sudo dd if=${OUT} of=/dev/sdX bs=4M conv=fsync status=progress"
log "or use Raspberry Pi Imager and pick 'Use custom image'."
printf '%s\n' "${OUT}"
