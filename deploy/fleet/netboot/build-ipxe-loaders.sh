#!/usr/bin/env bash
# =============================================================================
# build-ipxe-loaders.sh - build iPXE loaders with an EMBEDDED chain script and
# install them into benzene's TFTP root, then stage a TFTP-only dnsmasq service.
#
# WHY EMBEDDED SCRIPT: a stock iPXE binary, once it re-does DHCP, expects the
# DHCP server to hand it the boot-script URL (option 67 when user-class=iPXE).
# UniFi's DHCP (Option B) advertises ONE boot filename and does NOT vary the
# reply by user-class, so a stock loader re-requests the SAME filename forever
# (a boot loop). Embedding `chain http://…/boot.ipxe` INTO the loader removes
# that dependency: the moment iPXE starts it fetches our menu over HTTP. `|| exit`
# means an un-staged machine falls back to local disk, never a wipe.
#
# This scripts the "Option B (UniFi DHCP)" go-live path documented in
# docs/SolariNet_Fleet_Provisioning.html — the counterpart to the proxyDHCP
# responder (Option A). It is idempotent and backs up any loaders it replaces.
#
#   deploy/fleet/netboot/build-ipxe-loaders.sh [--http-base URL] [--enable]
#
#   --http-base URL   nginx base that serves boot.ipxe (default from fleet-lib).
#   --enable          also enable+start solari-tftp.service and open the firewall.
# =============================================================================
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/fleet-lib.sh"

HTTP_BASE_ARG=""; DO_ENABLE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --http-base) HTTP_BASE_ARG="${2:?}"; shift ;;
    --enable)    DO_ENABLE=1 ;;
    *) fdie "unknown arg: $1" ;;
  esac; shift
done
BOOT_URL="${HTTP_BASE_ARG:-${HTTP_BASE}}/ipxe/boot.ipxe"
TFTP_ROOT="${PROV_ROOT}/netboot/tftp"

flog "building embedded-script iPXE loaders (chain → ${BOOT_URL}) on ${BENZENE}"

bssh "set -e
  # toolchain (openSUSE): iPXE needs xz-devel for the BIOS zbin compressor.
  command -v gcc >/dev/null || sudo zypper -n install -y gcc make >/dev/null 2>&1 || true
  [ -e /usr/include/lzma.h ] || sudo zypper -n install -y xz-devel >/dev/null 2>&1 || true
  mkdir -p ~/build && cd ~/build
  [ -d ipxe ] || git clone --depth 1 https://github.com/ipxe/ipxe.git >/dev/null 2>&1
  cat > ipxe/src/solari-embed.ipxe <<EOF
#!ipxe
dhcp
chain --autofree ${BOOT_URL} || exit
EOF
  cd ipxe/src
  make -j\$(nproc) \
    bin-x86_64-efi/ipxe.efi bin-i386-efi/ipxe.efi bin/undionly.kpxe \
    EMBED=solari-embed.ipxe >/dev/null 2>&1
  echo built:; ls -la bin-x86_64-efi/ipxe.efi bin-i386-efi/ipxe.efi bin/undionly.kpxe"

flog "installing loaders into ${TFTP_ROOT} (backing up originals to .orig/)"
bssh "set -e
  cd '${TFTP_ROOT}'
  mkdir -p .orig
  for f in ipxe-x86_64.efi ipxe-i386.efi pxelinux.0; do [ -f \".orig/\$f\" ] || [ ! -f \"\$f\" ] || cp -p \"\$f\" \".orig/\$f\"; done
  install -m 644 ~/build/ipxe/src/bin-x86_64-efi/ipxe.efi ipxe-x86_64.efi
  install -m 644 ~/build/ipxe/src/bin-i386-efi/ipxe.efi   ipxe-i386.efi
  install -m 644 ~/build/ipxe/src/bin/undionly.kpxe       pxelinux.0
  install -m 644 ~/build/ipxe/src/bin/undionly.kpxe       undionly.kpxe
  printf 'embed check %s: ' ipxe-x86_64.efi
  grep -a -o 'chain --autofree http[^ ]*' ipxe-x86_64.efi | head -1"

# Stage the TFTP-only service (no DHCP; pairs with UniFi Option B). Distinct from
# the proxyDHCP responder (solari-netboot.service) which does DHCP + TFTP.
flog "staging solari-tftp.service (TFTP only, no DHCP)"
bssh "set -e
  sudo tee /etc/solari-provision/dnsmasq-tftp.conf >/dev/null <<EOF
# SolariNet TFTP-only server — pairs with UniFi DHCP boot options (Option B).
# No DHCP, no DNS: serves the iPXE loaders in the tftp root, nothing else.
port=0
interface=eno1
bind-dynamic
enable-tftp
tftp-root=${TFTP_ROOT}
EOF
  sudo tee /etc/systemd/system/solari-tftp.service >/dev/null <<EOF
[Unit]
Description=SolariNet TFTP-only netboot file server (UniFi Option B; no DHCP)
Documentation=file:/srv/solari-provision/staging/go-live.sh
After=network-online.target
Wants=network-online.target
Conflicts=solari-netboot.service
[Service]
Type=simple
ExecStart=/usr/sbin/dnsmasq --keep-in-foreground --conf-file=/etc/solari-provision/dnsmasq-tftp.conf
Restart=on-failure
[Install]
WantedBy=multi-user.target
EOF
  sudo systemctl daemon-reload
  echo staged"

if [ "${DO_ENABLE}" -eq 1 ]; then
  flog "enabling solari-tftp.service + opening TFTP firewall"
  bssh "set -e
    sudo firewall-cmd --permanent --add-service=tftp >/dev/null 2>&1 || true
    sudo firewall-cmd --reload >/dev/null 2>&1 || true
    sudo systemctl enable --now solari-tftp.service
    sleep 1; systemctl is-active solari-tftp.service"
  flog "TFTP-only netboot server is LIVE. Set UniFi DHCP → Network Boot:"
else
  flog "staged (not enabled). To go live: rerun with --enable. Then set UniFi DHCP → Network Boot:"
fi
cat <<EOF
    Enabled:               yes
    Server (next-server):  ${BENZENE_IP}
    Filename:              ipxe-x86_64.efi
  Loaders present for UEFI x86_64 / i386 / BIOS; the embedded script chains to
  ${BOOT_URL} regardless of the DHCP-advertised filename (avoids the boot loop).
EOF
