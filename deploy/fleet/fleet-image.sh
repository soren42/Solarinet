#!/usr/bin/env bash
# =============================================================================
# fleet-image.sh - build a customized bootable image (Raspberry Pi USB/SD).
#
# Runs on the SolariNet server (xenon), invoked by the FLEET_IMAGE bridge verb.
# It mints the node's enrollment cert, syncs the Pi imaging scripts to the
# provisioning host (benzene), and runs the image build there. The resulting
# .img is written under benzene's HTTP root so it can be downloaded and flashed.
#
# NON-DESTRUCTIVE: writes only under /srv/solari-provision on benzene.
#
#   fleet-image.sh --hostname <h> --arch <arm64|arm32> [--distro raspios]
#                  [--profile <p>] [--role <r>] [--server <url>]
#                  [--server-ip <ip>] [--domain <d>] --op <op>
# =============================================================================
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet-lib.sh"

HOSTNAME_="" ARCH="" DISTRO="raspios" PROFILE="standard" ROLE="sensor" OP="dashboard"
while [ $# -gt 0 ]; do
  case "$1" in
    --hostname) HOSTNAME_="${2:?}"; shift ;;
    --arch)   ARCH="${2:?}"; shift ;;
    --distro) DISTRO="${2:?}"; shift ;;
    --profile) PROFILE="${2:?}"; shift ;;
    --role)   ROLE="${2:?}"; shift ;;
    --server) SERVER_URL="${2:?}"; shift ;;
    --server-ip) SERVER_IP="${2:?}"; shift ;;
    --domain) DOMAIN="${2:?}"; shift ;;
    --op)     OP="${2:?}"; shift ;;
    *) fdie "unknown arg: $1" ;;
  esac
  shift
done
[ -n "${HOSTNAME_}" ] || fdie "--hostname required"
case "${ARCH}" in arm64|arm32) ;; *) fdie "image builds are arm64/arm32 (Pi) only";; esac
SERVER_NAME="$(printf '%s' "${SERVER_URL}" | sed -E 's#^[a-z+]+://##; s#:[0-9]+$##')"
FQDN="${HOSTNAME_}.${DOMAIN}"
OUT_IMG="${HTTP_ROOT}/images/${HOSTNAME_}-${ARCH}.img"

flog "building ${DISTRO}/${ARCH} image for ${HOSTNAME_} (profile=${PROFILE} role=${ROLE})"

# ---- mint enrollment cert + stage it under an unguessable enroll dir --------
CERTTMP="$(mktemp -d)"; trap 'rm -rf "${CERTTMP}"' EXIT
sign_node_cert "${FQDN}" "${OP}" "${CERTTMP}"
ENROLL_TOKEN="$(openssl rand -hex 16)"
for f in ca.pem node.pem node.key; do
  [ -f "${CERTTMP}/${f}" ] && bput "${HTTP_ROOT}/enroll/${ENROLL_TOKEN}/${f}" 0640 < "${CERTTMP}/${f}"
done
flog "minted enrollment cert CN=client.${FQDN} (enroll token ${ENROLL_TOKEN:0:8}…)"

# ---- sync the Pi imaging scripts + client binary to benzene -----------------
bssh "install -d -m 755 ${PROV_ROOT}/pi ${PROV_ROOT}/pi/bin"
rsync -a -e "ssh -o BatchMode=yes" "${FLEET_DIR}/pi/" "${BENZENE}:${PROV_ROOT}/pi/" >/dev/null
CLIENT_BIN="${REPO_ROOT}/deploy/dist/${ARCH}/solariClient"
if [ -x "${CLIENT_BIN}" ]; then
  rsync -a -e "ssh -o BatchMode=yes" "${CLIENT_BIN}" "${BENZENE}:${PROV_ROOT}/pi/bin/solariClient.${ARCH}" >/dev/null
  flog "shipped solariClient.${ARCH} to benzene"
else
  fwarn "no prebuilt client at ${CLIENT_BIN}; build with deploy/cross-build-client.sh ${ARCH} (image will install the agent at firstboot if fetchable)"
fi

PKGS="${FLEET_CORE_PACKAGES}"
[ "${PROFILE}" = "minimal" ] && PKGS="sudo curl ca-certificates openssh-server chrony rsync jq"
PUBKEY="$(fleet_admin_pubkey || true)"

# ---- run the build on benzene (needs loop-mount; jason has passwordless sudo) -
flog "running image build on benzene (this downloads the base image and can take several minutes)…"
bssh "sudo bash ${PROV_ROOT}/pi/build-pi-image.sh \
    --hostname $(printf %q "${HOSTNAME_}") --fqdn $(printf %q "${FQDN}") --arch ${ARCH} \
    --admin-user $(printf %q "${ADMIN_USER}") --ssh-pubkey $(printf %q "${PUBKEY}") \
    --server-url $(printf %q "${SERVER_URL}") --server-name $(printf %q "${SERVER_NAME}") \
    --server-ip $(printf %q "${SERVER_IP}") --packages $(printf %q "${PKGS}") \
    --http-base $(printf %q "${HTTP_BASE}/enroll/${ENROLL_TOKEN}") \
    --out $(printf %q "${OUT_IMG}")" \
  || fdie "image build failed on benzene (see log above)"

SIZE="$(bssh "du -h ${OUT_IMG} 2>/dev/null | cut -f1" || echo '?')"
flog "done. image ready: ${OUT_IMG} (${SIZE})"
flog "download + flash: curl -O ${HTTP_BASE}/images/${HOSTNAME_}-${ARCH}.img ; then write to SD/USB with your imager."
