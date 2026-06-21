#!/usr/bin/env bash
#
# solari-decommission.sh - tear a SolariNet node down and forget it (Phase 3
# Handoff §4.2 verb CTRL_DECOMMISSION, §7.3 lifecycle semantics).
#
# Decommission is the operation most likely to go wrong destructively, so it is
# guarded carefully, mirroring the server-side discipline:
#
#   * A confirm token (TLV_LIFE_CONFIRM, §4 0x3201). The server issues a fresh
#     single-use token for a node; this helper echoes it. A single stray frame
#     can never wipe a host. Locally we additionally require the operator to
#     type a confirmation phrase unless --yes is given.
#
#   * An explicit wipe scope (TLV_LIFE_WIPE_SCOPE, §4 0x3202) - a bitfield over
#     config|certs|spool|logs|unit - so "remove from monitoring but leave logs
#     for forensics" is expressible.
#
# This is the *node-local* executor: the graceful teardown CTRL_DECOMMISSION
# describes - stop sampling/probing, flush & close the spool, securely erase the
# scoped material, disable and remove the service unit, then exit. It is invoked
# either by the node's own control handler on receiving SCP_MSG_DECOMMISSION, or
# by an operator on the box for a manual/forced teardown. It records intent to
# the server (best-effort) before deleting key material so the node can be marked
# 'retired'.
#
# Usage:
#   ./deploy/enrollment/solari-decommission.sh --role client --confirm-token TOK [options]
#
# Options:
#   --role ROLE          Node role: client | monitor | server (required).
#   --confirm-token TOK  Server-issued single-use confirm token (required unless
#                        --local-force, which is for offline/forced teardown).
#   --wipe-scope LIST    Comma list of: config,certs,spool,logs,unit,all
#                        (default: config,certs,spool,unit - logs kept).
#   --conf-dir DIR       TLS/material + config dir (default: /etc/solari).
#   --spool-db FILE      Spool DB path (default: read from config, else
#                        /var/lib/solari/spool.db).
#   --ctl-socket PATH    solariCtl Unix socket to report retirement to
#                        (default: /run/solari/solariCtl.sock if present).
#   --server URL         Report retirement via HTTPS instead of a local socket.
#   --dry-run            Show what would be wiped; change nothing.
#   --local-force        Skip the server confirm token (offline/forced teardown);
#                        only valid with --yes.
#   --yes                Skip the interactive confirmation prompt.
#   --insecure           Allow curl to skip TLS verification.
#   -h | --help          Show this help.
#
set -euo pipefail

# ---- defaults -------------------------------------------------------------
ROLE=""
CONFIRM_TOKEN=""
WIPE_SCOPE="config,certs,spool,unit"
CONF_DIR="/etc/solari"
SPOOL_DB=""
CTL_SOCKET=""
SERVER_URL=""
DRY_RUN=0
LOCAL_FORCE=0
ASSUME_YES=0
INSECURE=0

log()  { printf '\033[1;36m[decom]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[decom][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[decom][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- arg parsing ----------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --role)           ROLE="${2:?}"; shift ;;
    --confirm-token)  CONFIRM_TOKEN="${2:?}"; shift ;;
    --wipe-scope)     WIPE_SCOPE="${2:?}"; shift ;;
    --conf-dir)       CONF_DIR="${2:?}"; shift ;;
    --spool-db)       SPOOL_DB="${2:?}"; shift ;;
    --ctl-socket)     CTL_SOCKET="${2:?}"; shift ;;
    --server)         SERVER_URL="${2:?}"; shift ;;
    --dry-run)        DRY_RUN=1 ;;
    --local-force)    LOCAL_FORCE=1 ;;
    --yes)            ASSUME_YES=1 ;;
    --insecure)       INSECURE=1 ;;
    -h|--help)        grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# ---- validation -----------------------------------------------------------
case "${ROLE}" in
  client|monitor|server) ;;
  "") die "--role is required (client|monitor|server)" ;;
  *)  die "invalid --role '${ROLE}' (client|monitor|server)" ;;
esac

if [ -z "${CONFIRM_TOKEN}" ] && [ "${LOCAL_FORCE}" -eq 0 ]; then
  die "--confirm-token is required (or --local-force --yes for offline teardown)"
fi
if [ "${LOCAL_FORCE}" -eq 1 ] && [ "${ASSUME_YES}" -eq 0 ]; then
  die "--local-force requires --yes (refusing an unconfirmed forced wipe)"
fi

SERVICE="solari-${ROLE}.service"
CONFIG="${CONF_DIR}/${ROLE}.conf"

# Resolve spool DB from config if not given (key: spoolDb = ...).
if [ -z "${SPOOL_DB}" ]; then
  if [ -f "${CONFIG}" ]; then
    SPOOL_DB="$(grep -E '^[[:space:]]*spoolDb[[:space:]]*=' "${CONFIG}" 2>/dev/null \
                  | head -n1 | sed -E 's|^[^=]*=[[:space:]]*||' || true)"
  fi
  [ -n "${SPOOL_DB}" ] || SPOOL_DB="/var/lib/solari/spool.db"
fi

# ---- scope parsing --------------------------------------------------------
WIPE_CONFIG=0 WIPE_CERTS=0 WIPE_SPOOL=0 WIPE_LOGS=0 WIPE_UNIT=0
IFS=',' read -r -a _scopes <<< "${WIPE_SCOPE}"
for s in "${_scopes[@]}"; do
  case "$(printf '%s' "$s" | tr -d '[:space:]')" in
    config) WIPE_CONFIG=1 ;;
    certs)  WIPE_CERTS=1 ;;
    spool)  WIPE_SPOOL=1 ;;
    logs)   WIPE_LOGS=1 ;;
    unit)   WIPE_UNIT=1 ;;
    all)    WIPE_CONFIG=1; WIPE_CERTS=1; WIPE_SPOOL=1; WIPE_LOGS=1; WIPE_UNIT=1 ;;
    "")     ;;
    *)      die "unknown wipe-scope item: '$s' (config|certs|spool|logs|unit|all)" ;;
  esac
done

# ---- helpers --------------------------------------------------------------
have() { command -v "$1" >/dev/null 2>&1; }

run() {  # echo + execute, or just echo under --dry-run
  if [ "${DRY_RUN}" -eq 1 ]; then
    printf '  [dry-run] %s\n' "$*"
  else
    "$@"
  fi
}

# Securely remove a file: shred if available (best effort on journaled FS), else rm.
secure_rm() {
  local f="$1"
  [ -e "$f" ] || { log "  (absent) $f"; return 0; }
  if [ "${DRY_RUN}" -eq 1 ]; then printf '  [dry-run] securely remove %s\n' "$f"; return 0; fi
  if have shred && [ -f "$f" ]; then
    shred -u -z "$f" 2>/dev/null || rm -f "$f"
  else
    rm -rf "$f"
  fi
  log "  wiped $f"
}

# ---- pre-flight summary + confirmation ------------------------------------
log "Decommission plan for ${ROLE} (${SERVICE})"
log "  wipe config : $([ ${WIPE_CONFIG} -eq 1 ] && echo yes || echo no)  (${CONFIG})"
log "  wipe certs  : $([ ${WIPE_CERTS}  -eq 1 ] && echo yes || echo no)  (${CONF_DIR}/node.{key,pem,csr}, ca.pem)"
log "  wipe spool  : $([ ${WIPE_SPOOL}  -eq 1 ] && echo yes || echo no)  (${SPOOL_DB})"
log "  wipe logs   : $([ ${WIPE_LOGS}   -eq 1 ] && echo yes || echo no)"
log "  remove unit : $([ ${WIPE_UNIT}   -eq 1 ] && echo yes || echo no)  (${SERVICE})"
[ -n "${CONFIRM_TOKEN}" ] && log "  confirm token: present (will be echoed to server for audit)"
[ "${LOCAL_FORCE}" -eq 1 ] && warn "  LOCAL FORCE: proceeding without a server confirm token"

if [ "${DRY_RUN}" -eq 0 ] && [ "${ASSUME_YES}" -eq 0 ]; then
  printf 'Type the role name "%s" to confirm destructive teardown: ' "${ROLE}"
  read -r reply
  [ "${reply}" = "${ROLE}" ] || die "confirmation phrase did not match; aborting"
fi

# ---- 1. stop sampling/probing (stop the service) --------------------------
log "stopping ${SERVICE}"
if have systemctl; then
  run systemctl stop "${SERVICE}" 2>/dev/null || warn "could not stop ${SERVICE} (already stopped?)"
else
  warn "systemctl not present; ensure the node process is stopped"
fi

# ---- 2. report retirement intent to the server (best-effort, BEFORE wipe) -
# The node replies CONTROL_RESULT best-effort before key deletion so the server
# can mark it 'retired' (§4.2 / §7.3). We mirror that with a small REST/socket
# call carrying the echoed confirm token. Failure is non-fatal: a forced/offline
# teardown still proceeds and the operator force-retires server-side.
report_retirement() {
  have curl || { warn "curl absent; skipping server retirement report"; return 0; }
  local nodeId payload
  nodeId="$(grep -E '^[[:space:]]*nodeId[[:space:]]*=' "${CONFIG}" 2>/dev/null \
              | head -n1 | sed -E 's|^[^=]*=[[:space:]]*||' || true)"
  payload="$(jq -cn \
    --arg nodeId "${nodeId}" \
    --arg token  "${CONFIRM_TOKEN}" \
    --arg scope  "${WIPE_SCOPE}" \
    --arg role   "${ROLE}" \
    '{nodeId:$nodeId, confirmToken:$token, wipeScope:$scope, role:$role, result:"decommissioned"}')"

  local -a curl_args=(--fail --silent --show-error --max-time 10
                      -H 'Content-Type: application/json'
                      -X POST --data-binary "${payload}")
  [ "${INSECURE}" -eq 1 ] && curl_args+=(--insecure)

  if [ "${DRY_RUN}" -eq 1 ]; then
    printf '  [dry-run] report retirement (nodeId=%s)\n' "${nodeId:-unknown}"
    return 0
  fi

  if [ -n "${CTL_SOCKET}" ] || [ -S /run/solari/solariCtl.sock ]; then
    local sock="${CTL_SOCKET:-/run/solari/solariCtl.sock}"
    curl "${curl_args[@]}" --unix-socket "${sock}" \
      "http://localhost/api/control/decommission/result" \
      && log "reported retirement via solariCtl ${sock}" \
      || warn "retirement report to solariCtl failed (will rely on force-retire)"
  elif [ -n "${SERVER_URL}" ]; then
    curl "${curl_args[@]}" "${SERVER_URL%/}/api/control/decommission/result" \
      && log "reported retirement to ${SERVER_URL}" \
      || warn "retirement report to server failed (will rely on force-retire)"
  else
    warn "no solariCtl socket or --server given; skipping retirement report"
  fi
}
report_retirement

# ---- 3. flush & close spool, then wipe scoped material --------------------
if [ "${WIPE_SPOOL}" -eq 1 ]; then
  log "wiping spool database"
  secure_rm "${SPOOL_DB}"
  secure_rm "${SPOOL_DB}-wal"
  secure_rm "${SPOOL_DB}-shm"
fi

if [ "${WIPE_CERTS}" -eq 1 ]; then
  log "wiping certificate material"
  secure_rm "${CONF_DIR}/node.key"
  secure_rm "${CONF_DIR}/node.pem"
  secure_rm "${CONF_DIR}/node.csr"
  secure_rm "${CONF_DIR}/ca.pem"
fi

if [ "${WIPE_CONFIG}" -eq 1 ]; then
  log "wiping node configuration"
  secure_rm "${CONFIG}"
  # If the conf dir is now empty of solari material, remove it too.
  if [ "${DRY_RUN}" -eq 0 ] && [ -d "${CONF_DIR}" ] && [ -z "$(ls -A "${CONF_DIR}" 2>/dev/null)" ]; then
    rmdir "${CONF_DIR}" 2>/dev/null && log "  removed empty ${CONF_DIR}"
  fi
fi

if [ "${WIPE_LOGS}" -eq 1 ]; then
  log "wiping logs"
  if have journalctl && [ "${DRY_RUN}" -eq 0 ]; then
    journalctl --rotate 2>/dev/null || true
    journalctl --vacuum-time=1s --unit "${SERVICE}" 2>/dev/null || true
  fi
  secure_rm "/var/log/solari/${ROLE}.log"
fi

# ---- 4. disable & remove the service unit ---------------------------------
if [ "${WIPE_UNIT}" -eq 1 ]; then
  log "disabling and removing ${SERVICE}"
  if have systemctl; then
    run systemctl disable "${SERVICE}" 2>/dev/null || true
  fi
  for d in /etc/systemd/system /usr/local/lib/systemd/system /lib/systemd/system; do
    [ -f "${d}/${SERVICE}" ] && secure_rm "${d}/${SERVICE}"
  done
  if have systemctl && [ "${DRY_RUN}" -eq 0 ]; then
    systemctl daemon-reload 2>/dev/null || true
    systemctl reset-failed "${SERVICE}" 2>/dev/null || true
  fi
fi

if [ "${DRY_RUN}" -eq 1 ]; then
  log "dry-run complete; nothing was changed"
else
  log "decommission complete for ${ROLE} - node teardown finished"
fi
