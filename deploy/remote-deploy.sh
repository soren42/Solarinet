#!/usr/bin/env bash
#
# remote-deploy.sh - push the SolariNet client agent to a remote host over SSH.
#
# Installs and starts a reporting client on a target system (NAS, pihole,
# chemistry, ...): detects the target arch, places the right client binary,
# issues a per-host client certificate from the internal CA, drops a client.conf
# + the CA/cert/key, installs a systemd unit, and starts it. The host then
# self-registers (HELLO) and reports host metrics + listening services.
#
# Why a directly-issued cert (not enrollment): the server's CSR-signing CA path
# is still a fail-closed stub, so — like the local client — we mint the client
# cert here from the internal CA. Swap to the token/CSR enrollment flow
# (deploy/enrollment/solari-enroll.sh) once server-side signing is wired.
#
# Usage:
#   deploy/remote-deploy.sh --host [user@]HOST [options]
#
# Options:
#   --host [user@]HOST   target (required). The user needs passwordless sudo.
#   --server URL         server ingest URL (default: tls+tcp://<this-fqdn>:7701).
#                        Must be a name/IP present in the server cert SAN.
#   --fqdn NAME          target FQDN for its node id + cert CN
#                        (default: the target's `hostname -f`).
#   --ca-dir DIR         local internal-CA dir with ca.pem + ca.key
#                        (default: run/pki).
#   --bin PATH           client binary to deploy (default: by target arch, see below).
#   --conf-dir DIR       remote config/material dir (default: /etc/solari).
#   --remote-bin PATH    remote binary path (default: /usr/local/bin/solariClient).
#   --interval SEC       sample interval (default: 15).
#   --dry-run            print the plan; change nothing locally or remotely.
#   -h | --help          this help.
#
# Binary selection: --bin wins; else deploy/dist/<arch>/solariClient; else, when
# the target arch equals this host's, build-io/src/client/solariClient. Build
# other arches with the cross toolchains in cmake/toolchains/ and drop the result
# in deploy/dist/<arch>/.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST=""; SERVER_URL=""; FQDN=""; CA_DIR="${REPO_ROOT}/run/pki"; BIN=""
CONF_DIR="/etc/solari"; REMOTE_BIN="/usr/local/bin/solariClient"; INTERVAL=15; DRY=0

log()  { printf '\033[1;36m[deploy]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[deploy][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[deploy][error]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --host)       HOST="${2:?}"; shift ;;
    --server)     SERVER_URL="${2:?}"; shift ;;
    --fqdn)       FQDN="${2:?}"; shift ;;
    --ca-dir)     CA_DIR="${2:?}"; shift ;;
    --bin)        BIN="${2:?}"; shift ;;
    --conf-dir)   CONF_DIR="${2:?}"; shift ;;
    --remote-bin) REMOTE_BIN="${2:?}"; shift ;;
    --interval)   INTERVAL="${2:?}"; shift ;;
    --dry-run)    DRY=1 ;;
    -h|--help)    grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

[ -n "${HOST}" ] || die "--host is required (try --help)"
command -v openssl >/dev/null 2>&1 || die "openssl is required locally"
[ -f "${CA_DIR}/ca.pem" ] && [ -f "${CA_DIR}/ca.key" ] || die "CA not found in ${CA_DIR} (need ca.pem + ca.key)"
[ "${SERVER_URL}" ] || SERVER_URL="tls+tcp://$(hostname -f 2>/dev/null || hostname):7701"

SSH() { ssh -o BatchMode=yes "${HOST}" "$@"; }

# ---- target arch + fqdn ---------------------------------------------------
log "querying ${HOST}"
RAW_ARCH="$(SSH 'uname -m' 2>/dev/null || true)"
if [ -z "${RAW_ARCH}" ]; then
  if [ "${DRY}" -eq 1 ]; then warn "cannot ssh to ${HOST}; assuming local arch for dry-run"; RAW_ARCH="$(uname -m)"; else
    die "cannot ssh to ${HOST} (BatchMode). Set up key auth first."; fi
fi
case "${RAW_ARCH}" in
  x86_64|amd64)   ARCH=x86_64 ;;
  aarch64|arm64)  ARCH=arm64 ;;
  armv7l|armhf)   ARCH=arm32 ;;
  *) die "unsupported target arch: ${RAW_ARCH}" ;;
esac
[ -n "${FQDN}" ] || FQDN="$(SSH 'hostname -f 2>/dev/null || hostname' 2>/dev/null | tr -d '\r' || true)"
[ -n "${FQDN}" ] || { [ "${DRY}" -eq 1 ] && FQDN="${HOST##*@}"; }
[ -n "${FQDN}" ] || die "could not determine target FQDN; pass --fqdn"
log "target: ${FQDN} (${ARCH})"

# ---- resolve the client binary --------------------------------------------
LOCAL_ARCH="$(uname -m)"; LOCAL_ARCH=${LOCAL_ARCH/amd64/x86_64}
if [ -z "${BIN}" ]; then
  if [ -x "${REPO_ROOT}/deploy/dist/${ARCH}/solariClient" ]; then
    BIN="${REPO_ROOT}/deploy/dist/${ARCH}/solariClient"
  elif [ "${ARCH}" = "${LOCAL_ARCH}" ] && [ -x "${REPO_ROOT}/build-io/src/client/solariClient" ]; then
    BIN="${REPO_ROOT}/build-io/src/client/solariClient"
  else
    die "no client binary for ${ARCH}. Cross-build with cmake/toolchains/linux-${ARCH}.cmake and place it in deploy/dist/${ARCH}/solariClient, or pass --bin."
  fi
fi
log "binary: ${BIN}"

# ---- issue a per-host client cert from the internal CA --------------------
TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
umask 077
openssl ecparam -name prime256v1 -genkey -noout -out "${TMP}/node.key"
openssl req -new -key "${TMP}/node.key" -subj "/CN=client.${FQDN}/O=SolariNet/OU=client" -out "${TMP}/node.csr"
openssl x509 -req -in "${TMP}/node.csr" -CA "${CA_DIR}/ca.pem" -CAkey "${CA_DIR}/ca.key" \
  -CAcreateserial -days 825 -sha256 \
  -extfile <(printf 'basicConstraints=CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\nextendedKeyUsage=clientAuth\nsubjectAltName=DNS:%s\n' "${FQDN}") \
  -out "${TMP}/node.pem" 2>/dev/null
cp "${CA_DIR}/ca.pem" "${TMP}/ca.pem"
log "issued client cert CN=client.${FQDN}"

# ---- render client.conf + unit --------------------------------------------
cat > "${TMP}/client.conf" <<EOF
[identity]
hostFqdn = ${FQDN}

[server]
primaryUrl = ${SERVER_URL}

[tls]
caFile   = ${CONF_DIR}/ca.pem
certFile = ${CONF_DIR}/node.pem
keyFile  = ${CONF_DIR}/node.key

[schedule]
sampleIntervalSec = ${INTERVAL}

[watch]
spoolDb = /var/lib/solari/client-spool.db
EOF

cat > "${TMP}/solarinet-client.service" <<EOF
[Unit]
Description=SolariNet client agent (host metrics -> server ingest)
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
ExecStart=${REMOTE_BIN} --config ${CONF_DIR}/client.conf --loop
Restart=on-failure
RestartSec=3
[Install]
WantedBy=multi-user.target
EOF

# ---- push + install -------------------------------------------------------
do_push() {  # localfile remotepath mode
  if [ "${DRY}" -eq 1 ]; then echo "  scp $1 -> ${HOST}:$2 (mode $3)"; return; fi
  scp -q -o BatchMode=yes "$1" "${HOST}:/tmp/.solari_push"
  SSH "sudo install -D -m $3 /tmp/.solari_push '$2' && rm -f /tmp/.solari_push"
}
do_remote() { if [ "${DRY}" -eq 1 ]; then echo "  ssh ${HOST} sudo $*"; else SSH "sudo $*"; fi; }

log "deploying to ${HOST}${DRY:+ (dry-run)}"
do_push "${BIN}"                          "${REMOTE_BIN}"               0755
do_push "${TMP}/ca.pem"                   "${CONF_DIR}/ca.pem"          0644
do_push "${TMP}/node.pem"                 "${CONF_DIR}/node.pem"        0644
do_push "${TMP}/node.key"                 "${CONF_DIR}/node.key"        0600
do_push "${TMP}/client.conf"              "${CONF_DIR}/client.conf"     0644
do_push "${TMP}/solarinet-client.service" /etc/systemd/system/solarinet-client.service 0644
do_remote "mkdir -p /var/lib/solari"
do_remote "systemctl daemon-reload"
do_remote "systemctl enable --now solarinet-client"

log "done. On ${HOST}: systemctl status solarinet-client ; journalctl -u solarinet-client -f"
log "the node should appear in the dashboard within one sample interval (${INTERVAL}s)."
