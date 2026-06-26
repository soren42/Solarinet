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
# Certs are issued via CSR signing: a key + CSR are generated for the target and
# the CSR is signed by the server's internal CA over the solariCtl SIGN verb. The
# CA private key never touches this script (it stays in the CA/server process),
# so the CA can later relocate to a dedicated host (server [ca] mode=remote).
#
# Usage:
#   deploy/remote-deploy.sh --host [user@]HOST [options]
#
# Adaptive: one SSH round-trip probes the target (privilege, arch, init system,
# libc, package manager, writable dirs) and the deploy adapts to it — systemd,
# OpenWrt/procd, sysvinit, cron @reboot, or bare background; root-login or sudo;
# apt or opkg; and auto-chosen writable install dirs on odd/immutable layouts.
#
# Options:
#   --host [user@]HOST   target (required). Needs key auth + (root or sudo).
#   --server URL         server ingest URL (default: tls+tcp://<this-hostname>:7701).
#                        Dial by a NAME in the server cert SAN — older mbedTLS does
#                        not match IP SANs. The name is pinned in the target hosts file.
#   --server-ip IP       IP to pin the server name to (default: this host's LAN IP).
#   --fqdn NAME          target FQDN for its node id + cert CN (default: target hostname).
#   --ca-dir DIR         local internal-CA dir with ca.pem (default: run/pki).
#   --bin PATH           client binary to deploy (default: by target arch, see below).
#   --conf-dir DIR       remote config/material dir (default: auto-probed writable dir).
#   --remote-bin PATH    remote binary path (default: <auto-probed bindir>/solariClient).
#   --interval SEC       sample interval (default: 15).
#   --dry-run            print the plan; change nothing locally or remotely.
#   -h | --help          this help.
#
# Binary selection: --bin wins; else deploy/dist/<arch>/solariClient (build with
# deploy/cross-build-client.sh <arch>); else, when the target arch equals this
# host's, build-io/src/client/solariClient. Arch ∈ x86_64/arm64/arm32/mips/mipsel.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

HOST=""; SERVER_URL=""; SERVER_IP=""; FQDN=""; CA_DIR="${REPO_ROOT}/run/pki"; BIN=""
CONF_DIR=""; REMOTE_BIN=""; INTERVAL=15; DRY=0   # bin/conf dirs: empty -> auto-probe
CTL_SOCK="${REPO_ROOT}/run/solariCtl.sock"; OP="${USER:-deploy}"
SUDO="sudo"; T_INIT="systemd"; T_LIBC="glibc"; T_PKG="none"; STATE_DIR=""

log()  { printf '\033[1;36m[deploy]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[deploy][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[deploy][error]\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --host)       HOST="${2:?}"; shift ;;
    --server)     SERVER_URL="${2:?}"; shift ;;
    --server-ip)  SERVER_IP="${2:?}"; shift ;;
    --fqdn)       FQDN="${2:?}"; shift ;;
    --ca-dir)     CA_DIR="${2:?}"; shift ;;
    --bin)        BIN="${2:?}"; shift ;;
    --conf-dir)   CONF_DIR="${2:?}"; shift ;;
    --remote-bin) REMOTE_BIN="${2:?}"; shift ;;
    --interval)   INTERVAL="${2:?}"; shift ;;
    --ctl-sock)   CTL_SOCK="${2:?}"; shift ;;
    --op)         OP="${2:?}"; shift ;;
    --dry-run)    DRY=1 ;;
    -h|--help)    grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

[ -n "${HOST}" ] || die "--host is required (try --help)"
command -v openssl >/dev/null 2>&1 || die "openssl is required locally"
command -v python3 >/dev/null 2>&1 || die "python3 is required locally (for the CA SIGN call)"
# Only the CA ROOT cert is needed here (to ship to the client so it trusts the
# server). The CA PRIVATE key never touches this script — the server's CA signs
# the CSR over the solariCtl socket (SIGN verb).
[ -f "${CA_DIR}/ca.pem" ] || die "CA root not found: ${CA_DIR}/ca.pem"
# Dial the server by NAME, not IP: older mbedTLS (e.g. 2.28 on Debian bookworm /
# UniFiOS / many appliances) does NOT match IP-address SANs, so an IP URL fails
# TLS on those clients. Default to the server hostname (which is in the cert SAN)
# and pin it -> the server's LAN IP in the target's /etc/hosts below, so it
# resolves regardless of LAN DNS. Override the name with --server, the pinned IP
# with --server-ip.
[ "${SERVER_URL}" ] || SERVER_URL="tls+tcp://$(hostname):7701"
SERVER_HOST="${SERVER_URL#*://}"; SERVER_HOST="${SERVER_HOST%%:*}"
[ -n "${SERVER_IP}" ] || SERVER_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"

SSH() { ssh -o BatchMode=yes "${HOST}" "$@"; }

# ---- probe the target: privilege, arch, fqdn, init, libc, pkg, writable dirs
# One round-trip describes the host so the rest of the deploy can adapt instead
# of assuming systemd + /usr/local/bin + apt. Heterogeneous fleet: Debian/Pi,
# OpenWrt/procd (Ubiquiti MIPS), root-login appliances, immutable rootfs, etc.
log "querying ${HOST}"
T_UID="$(SSH 'id -u' 2>/dev/null | tr -d '\r' || true)"
if [ -z "${T_UID}" ]; then
  if [ "${DRY}" -eq 1 ]; then warn "cannot ssh to ${HOST}; assuming defaults for dry-run"; T_UID=0; else
    die "cannot ssh to ${HOST} (BatchMode). Set up key auth first."; fi
fi
[ "${T_UID}" = "0" ] && SUDO="" || SUDO="sudo"

# Pipe the probe script to the remote shell (sh -s) via a quoted heredoc: nothing
# is expanded locally, so the remote evaluates $(...)/$d with no quoting hazards.
PROBE="$(SSH "${SUDO} sh -s" <<'PROBESH' 2>/dev/null || true
echo arch=$(uname -m)
echo fqdn=$(hostname -f 2>/dev/null || hostname 2>/dev/null)
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then echo init=systemd
elif [ -x /sbin/procd ] && [ -d /etc/init.d ]; then echo init=procd
elif [ -d /etc/init.d ] && command -v update-rc.d >/dev/null 2>&1; then echo init=sysvinit
elif command -v crontab >/dev/null 2>&1; then echo init=cron
else echo init=none; fi
if ls /lib/ld-musl-* >/dev/null 2>&1; then echo libc=musl; else echo libc=glibc; fi
if command -v apt-get >/dev/null 2>&1; then echo pkg=apt
elif command -v opkg >/dev/null 2>&1; then echo pkg=opkg
else echo pkg=none; fi
for d in /usr/local/bin /opt/bin /usr/sbin /data/bin /overlay/bin; do
  if ( mkdir -p "$d" && touch "$d/.swt" ) >/dev/null 2>&1; then rm -f "$d/.swt"; echo bindir=$d; break; fi
done
for d in /etc/solari /opt/solari /data/solari /overlay/solari /persistent/solari; do
  if ( mkdir -p "$d" && touch "$d/.swt" ) >/dev/null 2>&1; then rm -f "$d/.swt"; echo confdir=$d; break; fi
done
PROBESH
)"
prb() { printf '%s\n' "${PROBE}" | sed -n "s/^$1=//p" | head -1; }

RAW_ARCH="$(prb arch)"; [ -n "${RAW_ARCH}" ] || RAW_ARCH="$(uname -m)"
T_INIT="$(prb init)"; [ -n "${T_INIT}" ] || T_INIT=systemd
T_LIBC="$(prb libc)"; [ -n "${T_LIBC}" ] || T_LIBC=glibc
T_PKG="$(prb pkg)";   [ -n "${T_PKG}" ]  || T_PKG=none
[ -n "${FQDN}" ] || FQDN="$(prb fqdn | tr -d '\r')"
[ -n "${FQDN}" ] || { [ "${DRY}" -eq 1 ] && FQDN="${HOST##*@}"; }
[ -n "${FQDN}" ] || die "could not determine target FQDN; pass --fqdn"

case "${RAW_ARCH}" in
  x86_64|amd64)        ARCH=x86_64 ;;
  aarch64|arm64)       ARCH=arm64 ;;
  armv7l|armhf|armv6l) ARCH=arm32 ;;
  mips)                ARCH=mips ;;
  mipsel|mipsle)       ARCH=mipsel ;;
  *) die "unsupported target arch: ${RAW_ARCH}" ;;
esac

# install locations: explicit flags win; else the probed writable dirs
if [ -z "${REMOTE_BIN}" ]; then
  bd="$(prb bindir)"
  [ -n "${bd}" ] || { [ "${DRY}" -eq 1 ] && bd=/usr/local/bin; }
  [ -n "${bd}" ] || die "no writable bin dir on ${HOST} (immutable rootfs?). Pass --remote-bin <dir>/solariClient, or use a container deploy."
  REMOTE_BIN="${bd}/solariClient"
fi
if [ -z "${CONF_DIR}" ]; then
  CONF_DIR="$(prb confdir)"
  [ -n "${CONF_DIR}" ] || { [ "${DRY}" -eq 1 ] && CONF_DIR=/etc/solari; }
  [ -n "${CONF_DIR}" ] || die "no writable config dir on ${HOST}. Pass --conf-dir <dir>."
fi
STATE_DIR="${CONF_DIR}"   # spool + logs live with the config (a known-writable dir)
[ -n "${SUDO}" ] && PRIV=sudo || PRIV=root
log "target: ${FQDN} (${ARCH}, init=${T_INIT}, libc=${T_LIBC}, pkg=${T_PKG}, ${PRIV})"
log "install: bin=${REMOTE_BIN} conf=${CONF_DIR}"

# ---- resolve the client binary --------------------------------------------
LOCAL_ARCH="$(uname -m)"; LOCAL_ARCH=${LOCAL_ARCH/amd64/x86_64}
if [ -z "${BIN}" ]; then
  if [ -x "${REPO_ROOT}/deploy/dist/${ARCH}/solariClient" ]; then
    BIN="${REPO_ROOT}/deploy/dist/${ARCH}/solariClient"
  elif [ "${ARCH}" = "${LOCAL_ARCH}" ] && [ -x "${REPO_ROOT}/build-io/src/client/solariClient" ]; then
    BIN="${REPO_ROOT}/build-io/src/client/solariClient"
  else
    die "no client binary for ${ARCH}. Build one with: deploy/cross-build-client.sh ${ARCH} (-> deploy/dist/${ARCH}/solariClient), or pass --bin."
  fi
fi
log "binary: ${BIN}"
[ "${T_PKG}" = "none" ] && [ "${T_LIBC}" = "musl" ] && \
  warn "musl host with no package manager — a glibc dynamic binary will NOT run here; a static (musl) build is required (cross-build matrix, pending)."

# ---- key + CSR locally; CA signs it over solariCtl (SIGN) -----------------
# The CA private key stays in the CA/server process; we only send a CSR and get
# a signed cert back. (Swap CTL_SOCK for a remote CA endpoint when the CA moves.)
TMP="$(mktemp -d)"; trap 'rm -rf "${TMP}"' EXIT
umask 077
openssl ecparam -name prime256v1 -genkey -noout -out "${TMP}/node.key"
openssl req -new -key "${TMP}/node.key" -subj "/CN=client.${FQDN}/O=SolariNet/OU=client" -out "${TMP}/node.csr"
cp "${CA_DIR}/ca.pem" "${TMP}/ca.pem"

[ -S "${CTL_SOCK}" ] || die "solariCtl socket not found: ${CTL_SOCK} (is the server running? pass --ctl-sock)"
log "requesting cert from CA via ${CTL_SOCK} (op=${OP})"
SIGN_RC=0
python3 - "${CTL_SOCK}" "${TMP}/node.csr" "${TMP}/node.pem" "${OP}" <<'PY' || SIGN_RC=$?
import sys, socket, urllib.parse
sock, csrf, outf, op = sys.argv[1:5]
csr = open(csrf).read()
s = socket.socket(socket.AF_UNIX); s.connect(sock)
s.sendall(("SIGN op=%s csr=%s\n" % (op, urllib.parse.quote(csr, safe=""))).encode())
buf = b""
while b"\n" not in buf:
    d = s.recv(65536)
    if not d: break
    buf += d
reply = buf.decode(errors="replace").rstrip("\n")
if not reply.startswith("OK cert="):
    sys.stderr.write("CA SIGN failed: %s\n" % reply); sys.exit(2)
open(outf, "w").write(urllib.parse.unquote(reply[len("OK cert="):]))
PY
[ "${SIGN_RC}" -eq 0 ] && [ -s "${TMP}/node.pem" ] || die "CA did not return a signed certificate (RC=${SIGN_RC})"
log "CA signed cert CN=client.${FQDN}"

# ---- render client.conf -----------------------------------------------------
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
spoolDb = ${STATE_DIR}/client-spool.db
EOF

# ---- render autostart artifacts (one per init system; install picks one) ----
# The init.d/launcher scripts contain literal shell ($1, $BIN, ...), so they use
# quoted heredocs + __PLACEHOLDER__ substitution rather than direct expansion.
render() { sed -e "s#__BIN__#${REMOTE_BIN}#g" -e "s#__CONF__#${CONF_DIR}#g" -e "s#__STATE__#${STATE_DIR}#g"; }

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

cat > "${TMP}/solarinet.procd" <<'EOF'
#!/bin/sh /etc/rc.common
# OpenWrt/procd init script for the SolariNet client.
USE_PROCD=1
START=95
STOP=10
start_service() {
  procd_open_instance
  procd_set_param command __BIN__ --config __CONF__/client.conf --loop
  procd_set_param respawn
  procd_set_param stdout 1
  procd_set_param stderr 1
  procd_close_instance
}
EOF

cat > "${TMP}/solarinet.sysv" <<'EOF'
#!/bin/sh
### BEGIN INIT INFO
# Provides:          solarinet-client
# Required-Start:    $network $remote_fs
# Required-Stop:     $network $remote_fs
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: SolariNet client agent
### END INIT INFO
PIDFILE=/var/run/solarinet-client.pid
BIN=__BIN__
ARGS="--config __CONF__/client.conf --loop"
case "$1" in
  start)   start-stop-daemon --start --background --make-pidfile --pidfile "$PIDFILE" --exec "$BIN" -- $ARGS ;;
  stop)    start-stop-daemon --stop --pidfile "$PIDFILE" 2>/dev/null; rm -f "$PIDFILE" ;;
  restart) "$0" stop; sleep 1; "$0" start ;;
  status)  { [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE")" 2>/dev/null && echo running; } || { echo stopped; exit 1; } ;;
  *) echo "usage: $0 {start|stop|restart|status}"; exit 1 ;;
esac
EOF

cat > "${TMP}/solarinet-run.sh" <<'EOF'
#!/bin/sh
# Idempotent launcher: cron @reboot / */5 supervision, or manual background use.
BIN=__BIN__
ps 2>/dev/null | grep -v grep | grep -q "solariClient --config" && exit 0
exec "$BIN" --config __CONF__/client.conf --loop >> __STATE__/client.log 2>&1
EOF

for f in solarinet.procd solarinet.sysv solarinet-run.sh; do
  render < "${TMP}/${f}" > "${TMP}/${f}.r" && mv "${TMP}/${f}.r" "${TMP}/${f}"
done

# ---- push + install -------------------------------------------------------
do_push() {  # localfile remotepath mode
  if [ "${DRY}" -eq 1 ]; then echo "  scp $1 -> ${HOST}:$2 (mode $3)"; return; fi
  scp -q -o BatchMode=yes "$1" "${HOST}:/tmp/.solari_push"
  SSH "${SUDO} install -D -m $3 /tmp/.solari_push '$2' && rm -f /tmp/.solari_push"
}
do_remote() { if [ "${DRY}" -eq 1 ]; then echo "  ssh ${HOST} ${SUDO} $*"; else SSH "${SUDO} $*"; fi; }

# Install + (re)start under whichever init system the probe found. Sets STATUS_CMD.
install_autostart() {
  case "${T_INIT}" in
    systemd)
      do_push "${TMP}/solarinet-client.service" /etc/systemd/system/solarinet-client.service 0644
      do_remote "systemctl daemon-reload"
      do_remote "systemctl enable solarinet-client"
      # restart (not just enable --now): a re-deploy leaves the old process — and
      # thus the old config/cert — running unless we explicitly restart.
      do_remote "systemctl restart solarinet-client"
      STATUS_CMD="systemctl status solarinet-client ; journalctl -u solarinet-client -f" ;;
    procd)
      do_push "${TMP}/solarinet.procd" /etc/init.d/solarinet-client 0755
      do_remote "/etc/init.d/solarinet-client enable"
      do_remote "/etc/init.d/solarinet-client restart"
      STATUS_CMD="/etc/init.d/solarinet-client status ; logread -e solariClient" ;;
    sysvinit)
      do_push "${TMP}/solarinet.sysv" /etc/init.d/solarinet-client 0755
      do_remote "update-rc.d solarinet-client defaults >/dev/null 2>&1 || true"
      do_remote "sh -c '/etc/init.d/solarinet-client restart || /etc/init.d/solarinet-client start'"
      STATUS_CMD="/etc/init.d/solarinet-client status" ;;
    cron)
      do_push "${TMP}/solarinet-run.sh" "${STATE_DIR}/solarinet-run.sh" 0755
      do_remote "sh -c '(crontab -l 2>/dev/null | grep -v solarinet-run.sh; echo \"@reboot ${STATE_DIR}/solarinet-run.sh\"; echo \"*/5 * * * * ${STATE_DIR}/solarinet-run.sh\") | crontab -'"
      do_remote "sh -c '${STATE_DIR}/solarinet-run.sh >/dev/null 2>&1 &'"
      STATUS_CMD="tail -f ${STATE_DIR}/client.log" ;;
    *)
      do_push "${TMP}/solarinet-run.sh" "${STATE_DIR}/solarinet-run.sh" 0755
      do_remote "sh -c 'kill \$(cat ${STATE_DIR}/client.pid 2>/dev/null) 2>/dev/null; nohup ${STATE_DIR}/solarinet-run.sh >/dev/null 2>&1 & echo \$! > ${STATE_DIR}/client.pid'"
      warn "no init system on ${HOST}; started in background only (will NOT survive reboot). Hook ${STATE_DIR}/solarinet-run.sh into the device's startup."
      STATUS_CMD="tail -f ${STATE_DIR}/client.log" ;;
  esac
}

log "deploying to ${HOST}$([ "${DRY}" -eq 1 ] && printf ' (dry-run)')"

# Pin the server name -> IP in /etc/hosts when dialed by name (guarantees
# resolution + lets older mbedTLS verify the DNS SAN). /etc/hosts may be RO on
# some appliances, so this is best-effort, not fatal.
case "${SERVER_HOST}" in
  ""|*[!0-9.]*)  # a hostname (or empty) — not a bare IPv4 literal
    if [ -n "${SERVER_IP}" ] && [ -n "${SERVER_HOST}" ]; then
      log "pinning ${SERVER_HOST} -> ${SERVER_IP} in ${HOST}:/etc/hosts (TLS SAN)"
      do_remote "sh -c 'sed -i \"/[[:space:]]${SERVER_HOST}\\$/d\" /etc/hosts 2>/dev/null; printf \"%s %s\\n\" \"${SERVER_IP}\" \"${SERVER_HOST}\" >> /etc/hosts 2>/dev/null || true'"
    fi ;;
  *) warn "server dialed by IP (${SERVER_HOST}); clients with older mbedTLS (no IP-SAN match) will fail TLS — prefer a --server name in the cert SAN." ;;
esac

do_push "${BIN}"             "${REMOTE_BIN}"           0755
do_push "${TMP}/ca.pem"      "${CONF_DIR}/ca.pem"      0644
do_push "${TMP}/node.pem"    "${CONF_DIR}/node.pem"    0644
do_push "${TMP}/node.key"    "${CONF_DIR}/node.key"    0600
do_push "${TMP}/client.conf" "${CONF_DIR}/client.conf" 0644
do_remote "mkdir -p '${STATE_DIR}'"

# Runtime shared libs for a dynamically-linked client (best-effort; a static
# build needs none). soname package names vary across releases.
case "${T_PKG}" in
  apt)  do_remote "sh -c 'apt-get update -y >/dev/null 2>&1; apt-get install -y libnng1 libsqlite3-0 libcjson1 libmbedtls14 2>/dev/null || apt-get install -y libnng1 libsqlite3-0 libcjson1 libmbedtls12 2>/dev/null || true'" ;;
  opkg) do_remote "sh -c 'opkg update >/dev/null 2>&1; opkg install libnng libmbedtls libsqlite3 libcjson 2>/dev/null || true'" ;;
esac

install_autostart

log "done (init=${T_INIT}). On ${HOST}: ${STATUS_CMD}"
log "the node should appear in the dashboard within one sample interval (${INTERVAL}s)."
