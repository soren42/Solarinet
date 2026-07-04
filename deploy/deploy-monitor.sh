#!/usr/bin/env bash
# =============================================================================
# deploy-monitor.sh - deploy a SolariNet monitor "outpost" (reachability prober)
# to a host (local or remote). The outpost probes ICMP/TCP/UDP targets from its
# vantage and reports RTT/jitter/loss to the server over mutual TLS.
#
# It enrolls a monitor cert (CN=monitor.<name> — the server derives role from the
# CN and only a monitor may send MONITOR_REPORT), renders monitor.conf (targets
# pulled from the probeTarget table by default), installs the binary + a systemd
# unit, and starts it. Portable: the same flow targets x86_64 servers today and
# Pi Zero 2 W-class arm64 outposts (build deploy/dist/arm64/solariMonitor).
#
#   deploy/deploy-monitor.sh --name <outpost-fqdn> [--host [user@]HOST]
#       [--server tls+tcp://NAME:7701] [--server-ip IP] [--interval SEC]
#       [--bin PATH] [--op OP]
#   (omit --host to install locally.)
# =============================================================================
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
cd "${REPO_ROOT}"

NAME="" HOST="" SERVER_URL="" SERVER_IP="" INTERVAL=30 BIN="" OP="${USER:-deploy}"
CTL_SOCK="${REPO_ROOT}/run/solariCtl.sock"
while [ $# -gt 0 ]; do
  case "$1" in
    --name) NAME="${2:?}"; shift ;;
    --host) HOST="${2:?}"; shift ;;
    --server) SERVER_URL="${2:?}"; shift ;;
    --server-ip) SERVER_IP="${2:?}"; shift ;;
    --interval) INTERVAL="${2:?}"; shift ;;
    --bin) BIN="${2:?}"; shift ;;
    --op) OP="${2:?}"; shift ;;
    -h|--help) grep '^#' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown arg: $1" >&2; exit 1 ;;
  esac; shift
done
log(){ printf '\033[1;36m[monitor]\033[0m %s\n' "$*"; }
die(){ printf '\033[1;31m[monitor][error]\033[0m %s\n' "$*" >&2; exit 1; }

[ -n "${NAME}" ] || die "--name <outpost-fqdn> required (e.g. benzene-mon)"
[ -n "${SERVER_URL}" ] || SERVER_URL="tls+tcp://$(hostname):7701"
SERVER_HOST="${SERVER_URL#*://}"; SERVER_HOST="${SERVER_HOST%%:*}"
[ -n "${SERVER_IP}" ] || SERVER_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"

# ---- resolve the monitor binary by target arch ------------------------------
if [ -z "${BIN}" ]; then
  if [ -n "${HOST}" ]; then
    RAW_ARCH="$(ssh -o BatchMode=yes "${HOST}" 'uname -m' 2>/dev/null || echo x86_64)"
  else RAW_ARCH="$(uname -m)"; fi
  case "${RAW_ARCH}" in
    x86_64|amd64) A=x86_64 ;; aarch64|arm64) A=arm64 ;; armv7l|armhf) A=arm32 ;;
    *) die "unsupported arch ${RAW_ARCH}" ;;
  esac
  if [ -x "deploy/dist/${A}/solariMonitor" ]; then BIN="deploy/dist/${A}/solariMonitor"
  elif [ "${A}" = "$(uname -m | sed 's/amd64/x86_64/')" ] && [ -x build-io/src/monitor/solariMonitor ]; then
    BIN="build-io/src/monitor/solariMonitor"
  else die "no solariMonitor for ${A}; build with a bookworm container (see deploy/dist/) or pass --bin"; fi
fi
# Bundle a TLS-enabled libnng next to the binary when present — some distros
# (e.g. openSUSE) ship an nng whose TLS transport is ABI-incompatible with our
# build, so we carry our own and LD_LIBRARY_PATH it. Makes the outpost portable.
LIBNNG="$(dirname "${BIN}")/libnng.so.1"; LIBDIR="/usr/local/lib/solari"
log "outpost ${NAME} -> ${HOST:-local} (arch bin ${BIN}), server ${SERVER_URL}"

# ---- enroll a monitor cert (CN=monitor.<name>) via the bridge SIGN -----------
TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
umask 077
[ -S "${CTL_SOCK}" ] || die "solariCtl socket not found: ${CTL_SOCK} (server running?)"
openssl ecparam -name prime256v1 -genkey -noout -out "${TMP}/node.key"
openssl req -new -key "${TMP}/node.key" -subj "/CN=monitor.${NAME}/O=SolariNet/OU=monitor" -out "${TMP}/node.csr"
cp "${REPO_ROOT}/run/pki/ca.pem" "${TMP}/ca.pem"
# solariCtlClient (standard C) speaks the SIGN protocol; no python.
CTL_CLIENT=""
for c in "${REPO_ROOT}/build-snmp/src/server/solariCtlClient" \
         "${REPO_ROOT}/build-nosnmp/src/server/solariCtlClient" \
         "${REPO_ROOT}/build/src/server/solariCtlClient" \
         "$(command -v solariCtlClient 2>/dev/null || true)"; do
  [ -n "${c}" ] && [ -x "${c}" ] && CTL_CLIENT="${c}" && break
done
[ -n "${CTL_CLIENT}" ] || die "solariCtlClient not found; build the server tree (target solariCtlClient)"
"${CTL_CLIENT}" --sock "${CTL_SOCK}" sign --op "${OP}" \
  --csr "${TMP}/node.csr" --out "${TMP}/node.pem" || die "CA SIGN failed"
[ -s "${TMP}/node.pem" ] || die "no signed cert"
log "enrolled monitor cert CN=monitor.${NAME}"

# ---- render monitor.conf (targets from the probeTarget table) ---------------
CONF_DIR="/etc/solari"; STATE_DIR="/var/lib/solari"
{
  printf '[identity]\nhostFqdn = %s\n\n' "${NAME}"
  printf '[server]\nprimaryUrl = %s\n\n' "${SERVER_URL}"
  printf '[tls]\ncaFile   = %s/ca.pem\ncertFile = %s/node.pem\nkeyFile  = %s/node.key\n\n' "${CONF_DIR}" "${CONF_DIR}" "${CONF_DIR}"
  printf '[probe]\nroundIntervalSec = %s\nspoolDb = %s/monitor-spool.db\n' "${INTERVAL}" "${STATE_DIR}"
  if [ -f "${REPO_ROOT}/run/db.env" ]; then
    # shellcheck disable=SC1091
    . "${REPO_ROOT}/run/db.env"
    mariadb -h 127.0.0.1 -u solari -p"${SOLARI_DB_PASS}" solarinet -N \
      -e "SELECT proto, host, port FROM probeTarget ORDER BY targetId;" 2>/dev/null \
      | while read -r proto host port; do
          if [ "${proto}" = "icmp" ]; then printf 'target = icmp:%s\n' "${host}"
          else printf 'target = %s:%s:%s\n' "${proto}" "${host}" "${port}"; fi
        done
  fi
} > "${TMP}/monitor.conf"
NT="$(grep -c '^target' "${TMP}/monitor.conf" || true)"
log "rendered monitor.conf with ${NT} targets"

# ---- unit ------------------------------------------------------------------
cat > "${TMP}/solarinet-monitor.service" <<EOF
[Unit]
Description=SolariNet monitor outpost (reachability probes -> server)
After=network-online.target
Wants=network-online.target
[Service]
Type=simple
Environment=LD_LIBRARY_PATH=${LIBDIR}
ExecStart=/usr/local/bin/solariMonitor --config ${CONF_DIR}/monitor.conf --loop
Restart=on-failure
RestartSec=5
[Install]
WantedBy=multi-user.target
EOF

# ---- install (local or remote) ---------------------------------------------
if [ -z "${HOST}" ]; then
  SUDO="sudo"; [ "$(id -u)" = 0 ] && SUDO=""
  ${SUDO} install -D -m 0755 "${BIN}" /usr/local/bin/solariMonitor
  [ -f "${LIBNNG}" ] && ${SUDO} install -D -m 0644 "${LIBNNG}" "${LIBDIR}/libnng.so.1"
  ${SUDO} install -D -m 0644 "${TMP}/ca.pem"   "${CONF_DIR}/ca.pem"
  ${SUDO} install -D -m 0644 "${TMP}/node.pem" "${CONF_DIR}/node.pem"
  ${SUDO} install -D -m 0600 "${TMP}/node.key" "${CONF_DIR}/node.key"
  ${SUDO} install -D -m 0644 "${TMP}/monitor.conf" "${CONF_DIR}/monitor.conf"
  ${SUDO} install -D -m 0644 "${TMP}/solarinet-monitor.service" /etc/systemd/system/solarinet-monitor.service
  ${SUDO} mkdir -p "${STATE_DIR}"
  ${SUDO} systemctl daemon-reload
  ${SUDO} systemctl enable --now solarinet-monitor
  ${SUDO} systemctl restart solarinet-monitor
else
  SSH(){ ssh -o BatchMode=yes "${HOST}" "$@"; }
  put(){ scp -q -o BatchMode=yes "$1" "${HOST}:/tmp/.solari_m"; SSH "sudo install -D -m $3 /tmp/.solari_m '$2' && rm -f /tmp/.solari_m"; }
  # pin the server name so older mbedTLS verifies the DNS SAN (not an IP)
  case "${SERVER_HOST}" in
    ""|*[!0-9.]*) [ -n "${SERVER_IP}" ] && SSH "sudo sh -c 'sed -i \"/[[:space:]]${SERVER_HOST}\\$/d\" /etc/hosts; printf \"%s %s\\n\" \"${SERVER_IP}\" \"${SERVER_HOST}\" >> /etc/hosts'" || true ;;
  esac
  # runtime libs (best-effort across pkg managers)
  SSH "sudo sh -c 'command -v apt-get >/dev/null && apt-get install -y libnng1 libmbedtls14 libsqlite3-0 libcjson1 2>/dev/null; command -v zypper >/dev/null && zypper --non-interactive install libnng1 libmbedtls14 sqlite3 libcjson1 2>/dev/null; true'" || true
  put "${BIN}"                          /usr/local/bin/solariMonitor 0755
  [ -f "${LIBNNG}" ] && put "${LIBNNG}" "${LIBDIR}/libnng.so.1"      0644
  put "${TMP}/ca.pem"                   "${CONF_DIR}/ca.pem"         0644
  put "${TMP}/node.pem"                 "${CONF_DIR}/node.pem"       0644
  put "${TMP}/node.key"                 "${CONF_DIR}/node.key"       0600
  put "${TMP}/monitor.conf"             "${CONF_DIR}/monitor.conf"   0644
  put "${TMP}/solarinet-monitor.service" /etc/systemd/system/solarinet-monitor.service 0644
  SSH "sudo mkdir -p ${STATE_DIR}; sudo systemctl daemon-reload; sudo systemctl enable solarinet-monitor; sudo systemctl restart solarinet-monitor"
fi
log "done. outpost ${NAME} deployed to ${HOST:-local}; probes report every ${INTERVAL}s."
