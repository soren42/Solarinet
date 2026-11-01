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
# The per-MAC iPXE profile (ipxe/mac-<MAC>.ipxe) is what makes a machine wipe
# itself on its next network boot. Staging first removes any previous one for
# the MAC, writes every installer file and profile.env, and publishes the new
# profile LAST by atomic rename: a staging that fails part-way leaves no
# profile armed (prov-stage.py then disarms whatever its run owns).
#
#   fleet-provision.sh --target <MAC|host> --distro <debian|ubuntu|opensuse>
#                      --arch <x86_64|arm64|arm32> [--release <release_name>]
#                      --hostname <h> [--run-id <prov_runs id>]
#                      [--stack-vars <file.json>]
#                      [--profile <p>] [--role <r>] [--server <url>]
#                      [--server-ip <ip>] [--domain <d>] [--disk <dev>] --op <op>
#
# --release is the prov_images release_name; installer images live release-aware
# at HTTP_ROOT/installers/<distro>/<release>/<arch>/ (fetched by IMAGE_REFRESH).
# Without it a per-distro default is used (with a warning) so the original
# FLEET_PROVISION callers keep working. A per-MAC auto-install is only staged
# for an image that has been fetched + verified (its solari-image.json exists).
# --run-id (PROV_STAGE, prov-stage.py) ties the install to a prov_runs row: the
# installer writes it to the target as the Ansible local fact
# /etc/ansible/facts.d/solari_prov.fact and authorizes the provisioning host's
# control key for ADMIN_USER, so prov-finish.py can recognise THIS install,
# un-stage the per-MAC profile and apply the run's stacks over SSH. A run-id
# install needs a MAC target and carries only the bootstrap package set.
# --stack-vars (a JSON object) is kept operator-side only, as
# PROV_ROOT/profiles/<cfgkey>/stack.json (0600); stack vars are never published
# in the web root (a stale configs/<cfgkey>.stack.json is removed).
# Installer packages: the per-family bootstrap set from fleet-lib.sh, one
# <package> / list item per name; Debian/Ubuntu installs without --run-id also
# get FLEET_CORE_PACKAGES (Debian names) as before.
# The generated boot menu (ipxe/boot.ipxe, written by image-refresh.py) is never
# overwritten; the repo's static zero-image menu is installed only if none exists.
# =============================================================================
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet-lib.sh"
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/provisioning-guard.sh"

TARGET="" DISTRO="" ARCH="" RELEASE="" RUN_ID="" STACK_VARS="" HOSTNAME_="" PROFILE="standard" ROLE="sensor" OP="dashboard" DISK=""
while [ $# -gt 0 ]; do
  case "$1" in
    --target) TARGET="${2:?}"; shift ;;
    --distro) DISTRO="${2:?}"; shift ;;
    --arch)   ARCH="${2:?}"; shift ;;
    --release) RELEASE="${2:?}"; shift ;;
    --run-id) RUN_ID="${2:?}"; shift ;;
    --stack-vars) STACK_VARS="${2:?}"; shift ;;
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
[ -n "${ARCH}" ]     || fdie "--arch required"
[ -n "${HOSTNAME_}" ] || fdie "--hostname required"
[ -n "${DISK}" ] || DISK="${DEFAULT_DISK}"
fleet_require_unverified_override || exit 1

# ---- image identity: distro / release / arch (prov_images vocabulary) --------
case "${DISTRO}" in
  debian|ubuntu|opensuse) ;;
  *) fdie "unsupported distro: ${DISTRO}" ;;
esac
# On-disk image dirs use the prov_images arch (x86_64, arm64, armhf); the node's
# solariClient.<arch> download keeps the client build names (x86_64, arm64, arm32).
case "${ARCH}" in
  x86_64|amd64)  IMG_ARCH=x86_64; ARCH=x86_64 ;;
  arm64|aarch64) IMG_ARCH=arm64;  ARCH=arm64 ;;
  arm32|armhf)   IMG_ARCH=armhf;  ARCH=arm32 ;;
  *) fdie "unsupported arch: ${ARCH} (x86_64|arm64|arm32)" ;;
esac
if [ -z "${RELEASE}" ]; then
  case "${DISTRO}" in debian) RELEASE=12 ;; ubuntu) RELEASE=24.04 ;; opensuse) RELEASE=15.6 ;; esac
  fwarn "--release not given; defaulting to ${DISTRO} ${RELEASE}. Pass --release <prov_images.release_name> to pick the image explicitly."
fi
[[ "${RELEASE}" =~ ^[A-Za-z0-9][A-Za-z0-9._-]{0,47}$ ]] || fdie "invalid --release: ${RELEASE}"
# AutoYaST must name the base product: the Tumbleweed repo also carries Aeon,
# Kalpa and MicroOS, and an unnamed product stops the install on a dialog.
SUSE_PRODUCT=""
if [ "${DISTRO}" = opensuse ]; then
  case "${RELEASE}" in
    tumbleweed)      SUSE_PRODUCT=openSUSE ;;
    15.*|leap*|Leap*) SUSE_PRODUCT=Leap ;;
    *) fdie "no AutoYaST base product known for openSUSE release ${RELEASE} (tumbleweed|15.x)" ;;
  esac
fi
[[ "${HOSTNAME_}" =~ ^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$ ]] || fdie "invalid --hostname: ${HOSTNAME_}"
[[ "${DOMAIN}" =~ ^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$ ]] || fdie "invalid --domain: ${DOMAIN}"
[[ "${ADMIN_USER}" =~ ^[a-z_][a-z0-9_-]{0,31}$ ]] || fdie "invalid ADMIN_USER: ${ADMIN_USER}"
if [ -n "${RUN_ID}" ]; then
  [[ "${RUN_ID}" =~ ^[1-9][0-9]{0,9}$ ]] || fdie "invalid --run-id: ${RUN_ID}"
  is_mac "${TARGET}" || fdie "--run-id needs a MAC --target (per-MAC staged install)"
fi
if [ -n "${STACK_VARS}" ]; then
  [ -f "${STACK_VARS}" ] && [ -r "${STACK_VARS}" ] || fdie "--stack-vars file not readable: ${STACK_VARS}"
  python3 -c 'import json,sys
d = json.load(open(sys.argv[1], encoding="utf-8"))
sys.exit(0 if isinstance(d, dict) else 1)' "${STACK_VARS}" 2>/dev/null \
    || fdie "--stack-vars must be a JSON object: ${STACK_VARS}"
fi
NETBOOTLIB="${FLEET_DIR}/netbootlib.py"
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
PUBKEY=""
if RAWKEY="$(fleet_admin_pubkey)"; then
  PUBKEY="$(fleet_clean_pubkey "${RAWKEY}")" || fdie "operator SSH public key is not a plain OpenSSH public key"
else
  fwarn "no SSH public key found under ~/.ssh; provisioned host will have no admin key"
fi
CONTROL_PUBKEY=""
if [ -n "${RUN_ID}" ]; then
  RAWKEY="$(fleet_control_pubkey)" \
    || fdie "cannot read the provisioning host's control SSH public key (set FLEET_CONTROL_PUBKEY); nothing staged."
  CONTROL_PUBKEY="$(fleet_clean_pubkey "${RAWKEY}")" \
    || fdie "provisioning host control key is not a plain OpenSSH public key; nothing staged."
  [ "${CONTROL_PUBKEY}" != "${PUBKEY}" ] || CONTROL_PUBKEY=""
fi
AUTH_XML="" AUTH_YAML=""
for k in "${PUBKEY}" "${CONTROL_PUBKEY}"; do
  [ -n "${k}" ] || continue
  AUTH_XML="${AUTH_XML:+${AUTH_XML}
}        <listentry>${k}</listentry>"
  AUTH_YAML="${AUTH_YAML:+${AUTH_YAML}
}      - \"${k}\""
done
PWHASH="$(openssl passwd -6 "$(openssl rand -base64 18)")"   # account exists; sshd is key-only

# Installer package set (finding: one <package> per name, family names).
case "${DISTRO}" in
  opensuse) PKGS="${FLEET_BOOTSTRAP_PACKAGES_SUSE}" ;;
  *) PKGS="${FLEET_BOOTSTRAP_PACKAGES_DEB}"
     if [ -z "${RUN_ID}" ]; then
       if [ "${PROFILE}" = "minimal" ]; then PKGS="${PKGS} chrony rsync jq"
       else PKGS="${PKGS} ${FLEET_CORE_PACKAGES}"; fi
     fi ;;
esac
# shellcheck disable=SC2086  # word-split the package list on purpose
render_package_tokens ${PKGS}

export TOK_HOSTNAME="${HOSTNAME_}" TOK_FQDN="${FQDN}" TOK_DOMAIN="${DOMAIN}" \
       TOK_TIMEZONE="${TIMEZONE}" TOK_ADMIN_USER="${ADMIN_USER}" TOK_ADMIN_PWHASH="${PWHASH}" \
       TOK_SSH_PUBKEY="${PUBKEY}" TOK_CONTROL_PUBKEY="${CONTROL_PUBKEY}" \
       TOK_AUTHORIZED_KEYS_XML="${AUTH_XML}" TOK_AUTHORIZED_KEYS_YAML="${AUTH_YAML}" \
       TOK_PROV_RUN_ID="${RUN_ID}" TOK_DISK="${DISK}" \
       TOK_HTTP_BASE="${HTTP_BASE}" TOK_ARCH="${ARCH}" TOK_SERVER_URL="${SERVER_URL}" \
       TOK_SERVER_NAME="${SERVER_NAME}" TOK_SERVER_IP="${SERVER_IP}" \
       TOK_MAC="${CFGKEY}" TOK_DISTRO="${DISTRO}" TOK_SUSE_PRODUCT="${SUSE_PRODUCT}"

WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
CERTTMP="${WORK}/cert"

# ---- the installers must be on the mounted NAS image store -------------------
# HTTP_ROOT/installers is a link into PROV_STORE (prov-store-guard.sh). With the
# store unmounted the image looks "not fetched" and solari-firstboot.sh would be
# written to benzene's local disk under the mountpoint: refuse before staging.
fleet_require_store installers

# ---- the installer image must be fetched + verified before staging ----------
# image-refresh.py (IMAGE_REFRESH) writes solari-image.json into the image dir
# only after every file's sha256 checked out. The per-MAC profile is rendered
# from it, so it uses exactly the paths the generated boot menu uses.
IMG_DIR=""; IMG_META=""
if IMG_DIR="$(python3 "${NETBOOTLIB}" image-dir --distro "${DISTRO}" --release "${RELEASE}" --arch "${IMG_ARCH}" 2>"${WORK}/err")"; then
  IMG_META="$(bssh "cat '${HTTP_ROOT}/${IMG_DIR}/solari-image.json'" 2>/dev/null || true)"
else
  fwarn "no netboot installer for ${DISTRO} ${RELEASE} ${IMG_ARCH}: $(cat "${WORK}/err")"
fi
if [ -n "${MAC}" ]; then
  [ -n "${IMG_META}" ] || fdie "installer image ${DISTRO} ${RELEASE} ${IMG_ARCH} has not been fetched + verified on the provisioning host (${IMG_DIR:-no image dir}) — run IMAGE_REFRESH for it first; nothing staged."
  printf '%s' "${IMG_META}" | python3 "${NETBOOTLIB}" mac-profile --image-json - \
      --distro "${DISTRO}" --release "${RELEASE}" --arch "${IMG_ARCH}" \
      --mac "${MAC}" --hostname "${HOSTNAME_}" --http-base "${HTTP_BASE}" > "${WORK}/mac.ipxe" \
    || fdie "cannot render the per-MAC boot profile for ${DISTRO} ${RELEASE} ${IMG_ARCH}; nothing staged."
elif [ -z "${IMG_META}" ]; then
  fwarn "installer image ${DISTRO} ${RELEASE} ${IMG_ARCH} is not fetched yet — run IMAGE_REFRESH before this host boots."
fi

# ---- mint enrollment cert (never web-exposed; staged in an unguessable dir) --
sign_node_cert "${FQDN}" "${OP}" "${CERTTMP}"
ENROLL_TOKEN="$(openssl rand -hex 16)"
flog "minted enrollment cert CN=client.${FQDN}"

# ---- disarm the previous per-MAC profile before rewriting what it boots -------
if [ -n "${MAC}" ]; then
  bssh "rm -f '${HTTP_ROOT}/ipxe/mac-${MAC}.ipxe'" \
    || fdie "cannot remove the previous per-MAC profile ipxe/mac-${MAC}.ipxe; nothing staged."
fi

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

# ---- top-level boot menu: only bootstrap it ---------------------------------
# ipxe/boot.ipxe is generated from the SoR's fetched images by image-refresh.py;
# never overwrite it. The static zero-image menu is installed only if absent.
render_template "${FLEET_DIR}/netboot/ipxe/boot.ipxe" > "${WORK}/boot.ipxe"
bssh "test -e '${HTTP_ROOT}/ipxe/boot.ipxe' || install -D -m 0644 /dev/stdin '${HTTP_ROOT}/ipxe/boot.ipxe'" < "${WORK}/boot.ipxe"

# ---- stage the first-boot installer + enrollment material -------------------
# solari-firstboot.sh runs on the freshly installed node; the cert material is
# placed under an unguessable enroll dir (LAN-only, removed after first fetch).
# Into the store at its resolved path, re-checked in the same remote shell
# (the up-front fleet_require_store is minutes old by now).
render_template "${FLEET_DIR}/netboot/installers/solari-firstboot.sh" \
  | bput_store installers solari-firstboot.sh 0755 \
  || fdie "cannot write installers/solari-firstboot.sh into the image store (see the [store] line above)"
for f in ca.pem node.pem node.key; do
  [ -f "${CERTTMP}/${f}" ] && bput "${HTTP_ROOT}/enroll/${ENROLL_TOKEN}/${f}" 0640 < "${CERTTMP}/${f}"
done
# also keep an operator-side copy on benzene, outside the web root
bssh "install -d -m 700 ${PROV_ROOT}/profiles/${CFGKEY}"
printf 'hostname=%s\nfqdn=%s\ndistro=%s\nrelease=%s\narch=%s\nrole=%s\nprofile=%s\nmac=%s\nrun_id=%s\nenroll_token=%s\nserver_url=%s\nstaged_at=%s\n' \
  "${HOSTNAME_}" "${FQDN}" "${DISTRO}" "${RELEASE}" "${ARCH}" "${ROLE}" "${PROFILE}" "${CFGKEY}" "${RUN_ID}" "${ENROLL_TOKEN}" "${SERVER_URL}" "$(date -u +%FT%TZ)" \
  | bput "${PROV_ROOT}/profiles/${CFGKEY}/profile.env" 0600
# Stack vars never live in the web root (P4 published them there; drop any).
bssh "rm -f '${HTTP_ROOT}/configs/${CFGKEY}.stack.json'"
if [ -n "${STACK_VARS}" ]; then
  bput "${PROV_ROOT}/profiles/${CFGKEY}/stack.json" 0600 < "${STACK_VARS}"
  flog "kept stack vars operator-side -> profiles/${CFGKEY}/stack.json"
fi
[ -z "${RUN_ID}" ] || flog "install tagged with prov_runs ${RUN_ID}; prov-finish completes it after first boot"
flog "staged enrollment material (enroll token ${ENROLL_TOKEN:0:8}…)"

# ---- arm: publish the per-MAC profile LAST (atomic rename via bput) ----------
if [ -n "${MAC}" ]; then
  bput "${HTTP_ROOT}/ipxe/mac-${MAC}.ipxe" 0644 < "${WORK}/mac.ipxe"
  flog "staged per-MAC auto-install: ipxe/mac-${MAC}.ipxe (${IMG_DIR})"
fi

flog "staged. ${HOSTNAME_} will install ${DISTRO} ${RELEASE}/${IMG_ARCH} on next network boot once PXE is live."
if [ -z "${MAC}" ]; then
  flog "NOTE: no MAC given — boot this machine and pick '${DISTRO} ${RELEASE} ${IMG_ARCH}' from the SolariNet iPXE menu."
fi
