#!/usr/bin/env bash
# =============================================================================
# fleet-provision.sh - stage an unattended bare-metal OS install.
#
# Runs on the SolariNet server (xenon), invoked by the FLEET_PROVISION bridge
# verb. It mints the node's enrollment cert (via solariCtl SIGN), renders the
# distro's unattended-install config + a per-MAC iPXE auto-boot entry from the
# templates in deploy/fleet/netboot/, and pushes them to the provisioning host
# (benzene). The machine then installs itself on its next network boot.
#
# NON-DESTRUCTIVE: this only writes files on benzene. Live PXE requires the DHCP
# switch (proxyDHCP or UniFi options) which activates separately. Until then the
# entry is staged and inert.
#
#   fleet-provision.sh --target <MAC|host> --distro <debian|ubuntu|opensuse>
#                      --arch <x86_64|arm64|arm32> --hostname <h>
#                      [--profile <p>] [--role <r>] [--server <url>]
#                      [--server-ip <ip>] [--domain <d>] [--disk <dev>] --op <op>
# =============================================================================
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet-lib.sh"

TARGET="" DISTRO="" ARCH="" HOSTNAME_="" PROFILE="standard" ROLE="sensor" OP="dashboard" DISK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --target) TARGET="${2:?}"; shift ;;
    --distro) DISTRO="${2:?}"; shift ;;
    --arch)   ARCH="${2:?}"; shift ;;
    --hostname) HOSTNAME_="${2:?}"; shift ;;
    --profile) PROFILE="${2:?}"; shift ;;
    --role)   ROLE="${2:?}"; shift ;;
    --server) SERVER_URL="${2:?}"; shift ;;
    --server-ip) SERVER_IP="${2:?}"; shift ;;
    --domain) DOMAIN="${2:?}"; shift ;;
    --disk)   DISK="${2:?}"; shift ;;
    --op)     OP="${2:?}"; shift ;;
    *) fdie "unknown arg: $1" ;;
  esac
  shift
done
[ -n "${TARGET}" ]   || fdie "--target required"
[ -n "${DISTRO}" ]   || fdie "--distro required"
[ -n "${ARCH}" ]     || fdie "--hostname required"
[ -n "${HOSTNAME_}" ] || fdie "--hostname required"
[ -n "${DISK}" ] || DISK="${DEFAULT_DISK}"
# SERVER_NAME follows the server URL host unless explicitly set
SERVER_NAME="$(printf '%s' "${SERVER_URL}" | sed -E 's#^[a-z+]+://##; s#:[0-9]+$##')"

FQDN="${HOSTNAME_}.${DOMAIN}"
flog "provisioning ${HOSTNAME_} (${DISTRO}/${ARCH}) target=${TARGET} profile=${PROFILE} role=${ROLE}"

# ---- config key: per-MAC when we have a MAC, else per-host (manual boot) -----
if is_mac "${TARGET}"; then
  MAC="$(mac_hexhyp "${TARGET}")"; CFGKEY="${MAC}"
  flog "per-MAC auto-install staging: ${MAC}"
else
  MAC=""; CFGKEY="host-$(printf '%s' "${HOSTNAME_}" | tr -c 'A-Za-z0-9._-' '_')"
  fwarn "target '${TARGET}' is not a MAC; rendering config keyed ${CFGKEY} but the per-MAC auto-boot chain needs a MAC — this host must pick the install from the iPXE menu manually."
fi

# ---- gather render values ---------------------------------------------------
PUBKEY="$(fleet_admin_pubkey || true)"
[ -n "${PUBKEY}" ] || fwarn "no SSH public key found under ~/.ssh; provisioned host will have no admin key"
PWHASH="$(openssl passwd -6 "$(openssl rand -base64 18)")"   # account exists; sshd is key-only
PKGS="${FLEET_CORE_PACKAGES}"
[ "${PROFILE}" = "minimal" ] && PKGS="sudo curl ca-certificates openssh-server chrony rsync jq"

export TOK_HOSTNAME="${HOSTNAME_}" TOK_FQDN="${FQDN}" TOK_DOMAIN="${DOMAIN}" \
       TOK_TIMEZONE="${TIMEZONE}" TOK_ADMIN_USER="${ADMIN_USER}" TOK_ADMIN_PWHASH="${PWHASH}" \
       TOK_SSH_PUBKEY="${PUBKEY}" TOK_DISK="${DISK}" TOK_PACKAGES="${PKGS}" \
       TOK_HTTP_BASE="${HTTP_BASE}" TOK_ARCH="${ARCH}" TOK_SERVER_URL="${SERVER_URL}" \
       TOK_SERVER_NAME="${SERVER_NAME}" TOK_SERVER_IP="${SERVER_IP}" \
       TOK_MAC="${CFGKEY}" TOK_DISTRO="${DISTRO}"

# ---- mint enrollment cert (never web-exposed; staged in an unguessable dir) --
CERTTMP="$(mktemp -d)"; trap 'rm -rf "${CERTTMP}"' EXIT
sign_node_cert "${FQDN}" "${OP}" "${CERTTMP}"
ENROLL_TOKEN="$(openssl rand -hex 16)"
flog "minted enrollment cert CN=client.${FQDN}"

# ---- render + push the distro unattended-install config ---------------------
case "${DISTRO}" in
  debian)
    render_template "${FLEET_DIR}/netboot/configs/debian.preseed.tmpl" \
      | bput "${HTTP_ROOT}/configs/${CFGKEY}.preseed" 0644 ;;
  opensuse)
    render_template "${FLEET_DIR}/netboot/configs/opensuse.autoyast.xml.tmpl" \
      | bput "${HTTP_ROOT}/configs/${CFGKEY}.xml" 0644 ;;
  ubuntu)
    render_template "${FLEET_DIR}/netboot/configs/ubuntu.autoinstall.tmpl" \
      | bput "${HTTP_ROOT}/configs/ubuntu/${CFGKEY}/user-data" 0644
    printf 'instance-id: %s\nlocal-hostname: %s\n' "${CFGKEY}" "${HOSTNAME_}" \
      | bput "${HTTP_ROOT}/configs/ubuntu/${CFGKEY}/meta-data" 0644 ;;
  *) fdie "unsupported distro: ${DISTRO}" ;;
esac
flog "rendered ${DISTRO} unattended-install config -> configs/${CFGKEY}"

# ---- render + push the top-level boot menu (idempotent) + per-MAC entry ------
render_template "${FLEET_DIR}/netboot/ipxe/boot.ipxe" | bput "${HTTP_ROOT}/ipxe/boot.ipxe" 0644
if [ -n "${MAC}" ]; then
  render_template "${FLEET_DIR}/netboot/ipxe/mac-profile.ipxe.tmpl" \
    | bput "${HTTP_ROOT}/ipxe/mac-${MAC}.ipxe" 0644
  flog "staged per-MAC auto-install: ipxe/mac-${MAC}.ipxe"
fi

# ---- stage the first-boot installer + enrollment material -------------------
# solari-firstboot.sh runs on the freshly installed node; the cert material is
# placed under an unguessable enroll dir (LAN-only, removed after first fetch).
render_template "${FLEET_DIR}/netboot/installers/solari-firstboot.sh" \
  | bput "${HTTP_ROOT}/installers/solari-firstboot.sh" 0755
for f in ca.pem node.pem node.key; do
  [ -f "${CERTTMP}/${f}" ] && bput "${HTTP_ROOT}/enroll/${ENROLL_TOKEN}/${f}" 0640 < "${CERTTMP}/${f}"
done
# also keep an operator-side copy on benzene, outside the web root
bssh "install -d -m 700 ${PROV_ROOT}/profiles/${CFGKEY}"
printf 'hostname=%s\nfqdn=%s\ndistro=%s\narch=%s\nrole=%s\nprofile=%s\nmac=%s\nenroll_token=%s\nserver_url=%s\nstaged_at=%s\n' \
  "${HOSTNAME_}" "${FQDN}" "${DISTRO}" "${ARCH}" "${ROLE}" "${PROFILE}" "${CFGKEY}" "${ENROLL_TOKEN}" "${SERVER_URL}" "$(date -u +%FT%TZ)" \
  | bput "${PROV_ROOT}/profiles/${CFGKEY}/profile.env" 0600
flog "staged enrollment material (enroll token ${ENROLL_TOKEN:0:8}…)"

# ---- installer kernels present? (fetched during go-live prep) ---------------
if ! bssh "test -s ${HTTP_ROOT}/installers/${DISTRO}/${ARCH}/*" 2>/dev/null; then
  fwarn "netboot kernel/initrd for ${DISTRO}/${ARCH} not yet on benzene — run deploy/fleet/fetch-images.sh (go-live prep) before the target boots."
fi

flog "staged. ${HOSTNAME_} will install ${DISTRO}/${ARCH} on next network boot once PXE is live."
if [ -z "${MAC}" ]; then
  flog "NOTE: no MAC given — boot this machine and pick '${DISTRO}/${ARCH}' from the SolariNet iPXE menu."
fi
