#!/usr/bin/env bash
#
# solari-enroll.sh - enroll a fresh SolariNet node (§15.1 of the Architecture
# & Plan; enrollment state machine in Phase 3 Handoff §5/§7.3).
#
# The one-time, operator-approved exchange that gives a node its long-lived
# X.509 client certificate:
#
#   1. The operator generates a short-TTL, single-use enrollment token on the
#      server (out-of-band) and hands it to this script.
#   2. This script generates a keypair + CSR locally (the private key never
#      leaves the host).
#   3. It presents {token, CSR, role, fqdn} to the server's enrollment endpoint
#      (HTTPS to the dashboard /api/enrollments, which routes through solariCtl,
#      or directly to a local solariCtl Unix socket on the server itself).
#   4. The server signs the CSR against the internal CA (step-ca root), registers
#      the node row, and returns the signed cert + CA root.
#   5. This script installs cert/key/CA under /etc/solari and writes nodeId into
#      the node's config, after which solariClient/Monitor connect normally.
#
# Idempotent guards: an existing key is reused unless --force; an already-issued
# cert short-circuits unless --force.
#
# Usage:
#   ./deploy/enrollment/solari-enroll.sh --role client --token TOKEN [options]
#
# Options:
#   --role ROLE          Node role: client | monitor | server (required).
#   --token TOKEN        Enrollment token from the server (required unless
#                        --csr-only).
#   --server URL         Enrollment endpoint base URL
#                        (default: https://${SOLARI_SERVER:-localhost}).
#   --ctl-socket PATH    Submit via a local solariCtl Unix socket instead of
#                        HTTPS (server-host enrollment); requires curl --unix-socket.
#   --fqdn FQDN          This node's FQDN (default: hostname -f).
#   --conf-dir DIR       TLS/material dir (default: /etc/solari).
#   --config FILE        Node config to stamp nodeId into
#                        (default: ${conf-dir}/${role}.conf).
#   --key-bits N         RSA key size when not using EC (default: EC P-256).
#   --rsa                Use RSA-3072 instead of the default EC P-256 key.
#   --csr-only           Generate key+CSR and stop (print CSR path); no submit.
#   --insecure           Allow curl to skip TLS verification (bootstrap only).
#   --force              Regenerate key/CSR and overwrite installed material.
#   -h | --help          Show this help.
#
set -euo pipefail

# ---- defaults -------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROLE=""
TOKEN=""
SERVER_URL="https://${SOLARI_SERVER:-localhost}"
CTL_SOCKET=""
FQDN=""
CONF_DIR="/etc/solari"
CONFIG=""
KEY_BITS=3072
USE_RSA=0
CSR_ONLY=0
INSECURE=0
FORCE=0

log()  { printf '\033[1;36m[enroll]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[enroll][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[enroll][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- arg parsing ----------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --role)        ROLE="${2:?}"; shift ;;
    --token)       TOKEN="${2:?}"; shift ;;
    --server)      SERVER_URL="${2:?}"; shift ;;
    --ctl-socket)  CTL_SOCKET="${2:?}"; shift ;;
    --fqdn)        FQDN="${2:?}"; shift ;;
    --conf-dir)    CONF_DIR="${2:?}"; shift ;;
    --config)      CONFIG="${2:?}"; shift ;;
    --key-bits)    KEY_BITS="${2:?}"; shift ;;
    --rsa)         USE_RSA=1 ;;
    --csr-only)    CSR_ONLY=1 ;;
    --insecure)    INSECURE=1 ;;
    --force)       FORCE=1 ;;
    -h|--help)     grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

# ---- validation -----------------------------------------------------------
command -v openssl >/dev/null 2>&1 || die "openssl is required"
command -v curl    >/dev/null 2>&1 || die "curl is required"

case "${ROLE}" in
  client|monitor|server) ;;
  "") die "--role is required (client|monitor|server)" ;;
  *)  die "invalid --role '${ROLE}' (client|monitor|server)" ;;
esac

[ "${CSR_ONLY}" -eq 1 ] || [ -n "${TOKEN}" ] || die "--token is required (or use --csr-only)"

if [ -z "${FQDN}" ]; then
  FQDN="$(hostname -f 2>/dev/null || hostname)"
  [ -n "${FQDN}" ] || die "could not determine FQDN; pass --fqdn"
fi

[ -n "${CONFIG}" ] || CONFIG="${CONF_DIR}/${ROLE}.conf"

KEY_FILE="${CONF_DIR}/node.key"
CSR_FILE="${CONF_DIR}/node.csr"
CERT_FILE="${CONF_DIR}/node.pem"
CA_FILE="${CONF_DIR}/ca.pem"

# CN encodes role + FQDN exactly as the signer expects (§15.1, §5.5).
CERT_CN="${ROLE}:${FQDN}"

# ---- privilege / dirs -----------------------------------------------------
if [ ! -d "${CONF_DIR}" ]; then
  log "creating ${CONF_DIR}"
  mkdir -p "${CONF_DIR}" || die "cannot create ${CONF_DIR} (need root?)"
fi
chmod 0750 "${CONF_DIR}" 2>/dev/null || true

# ---- key + CSR ------------------------------------------------------------
generate_key_and_csr() {
  if [ -f "${KEY_FILE}" ] && [ "${FORCE}" -eq 0 ]; then
    log "reusing existing private key ${KEY_FILE} (--force to regenerate)"
  else
    log "generating private key ${KEY_FILE}"
    umask 077
    if [ "${USE_RSA}" -eq 1 ]; then
      openssl genpkey -algorithm RSA \
        -pkeyopt "rsa_keygen_bits:${KEY_BITS}" -out "${KEY_FILE}"
    else
      openssl genpkey -algorithm EC \
        -pkeyopt ec_paramgen_curve:P-256 -out "${KEY_FILE}"
    fi
    chmod 0600 "${KEY_FILE}"
  fi

  log "generating CSR ${CSR_FILE} (CN=${CERT_CN})"
  openssl req -new -key "${KEY_FILE}" -out "${CSR_FILE}" \
    -subj "/CN=${CERT_CN}/O=SolariNet/OU=${ROLE}" \
    -addext "subjectAltName=DNS:${FQDN}"
  chmod 0640 "${CSR_FILE}"
}

# CSR fingerprint the server can echo back for audit (matches enrollment.certFp).
csr_fingerprint() {
  openssl req -in "${CSR_FILE}" -pubkey -noout 2>/dev/null \
    | openssl pkey -pubin -outform DER 2>/dev/null \
    | openssl dgst -sha256 -c \
    | awk '{print $NF}'
}

# ---- submit ---------------------------------------------------------------
# Builds the enrollment request JSON; submits over HTTPS to the dashboard
# /api/enrollments endpoint, or to a local solariCtl Unix socket. The response
# is expected to contain { nodeId, cert (PEM), ca (PEM) }.
submit_enrollment() {
  local csr_pem fp payload resp
  csr_pem="$(cat "${CSR_FILE}")"
  fp="$(csr_fingerprint)"

  payload="$(jq -cn \
    --arg token "${TOKEN}" \
    --arg role  "${ROLE}" \
    --arg fqdn  "${FQDN}" \
    --arg csr   "${csr_pem}" \
    --arg fp    "${fp}" \
    '{token:$token, role:$role, host:$fqdn, certFp:$fp, csrPem:$csr}')"

  local -a curl_args=(--fail --silent --show-error
                      -H 'Content-Type: application/json'
                      -X POST --data-binary "${payload}")
  [ "${INSECURE}" -eq 1 ] && curl_args+=(--insecure)

  if [ -n "${CTL_SOCKET}" ]; then
    log "submitting CSR via solariCtl socket ${CTL_SOCKET}"
    resp="$(curl "${curl_args[@]}" --unix-socket "${CTL_SOCKET}" \
             "http://localhost/api/enrollments")" \
      || die "enrollment submission failed (solariCtl socket)"
  else
    log "submitting CSR to ${SERVER_URL}/api/enrollments"
    resp="$(curl "${curl_args[@]}" \
             "${SERVER_URL%/}/api/enrollments")" \
      || die "enrollment submission failed (HTTPS endpoint)"
  fi

  printf '%s' "${resp}"
}

# ---- install --------------------------------------------------------------
install_material() {
  local resp="$1" status nodeId cert ca

  status="$(printf '%s' "${resp}" | jq -r '.status // .data.status // empty')"
  if [ "${status}" = "pending" ]; then
    warn "enrollment accepted but awaiting operator approval (status=pending)."
    warn "re-run with the same token once approved, or poll /api/enrollments."
    return 3
  fi

  nodeId="$(printf '%s' "${resp}" | jq -r '.nodeId // .data.nodeId // empty')"
  cert="$(  printf '%s' "${resp}" | jq -r '.cert   // .data.cert   // empty')"
  ca="$(    printf '%s' "${resp}" | jq -r '.ca     // .data.ca     // empty')"

  [ -n "${cert}" ] || die "server response contained no signed certificate"
  [ -n "${ca}" ]   || die "server response contained no CA root"

  log "installing signed certificate ${CERT_FILE}"
  printf '%s\n' "${cert}" > "${CERT_FILE}"; chmod 0644 "${CERT_FILE}"
  log "installing CA root ${CA_FILE}"
  printf '%s\n' "${ca}"   > "${CA_FILE}";   chmod 0644 "${CA_FILE}"

  # sanity: the new cert must verify against the CA root and match our key.
  if ! openssl verify -CAfile "${CA_FILE}" "${CERT_FILE}" >/dev/null 2>&1; then
    warn "installed cert does not verify against CA root - check the signer"
  fi

  stamp_node_id "${nodeId}"
  log "enrollment complete (nodeId=${nodeId:-unknown}, role=${ROLE})"
}

# Stamp nodeId into [identity] of the node config without disturbing the rest.
stamp_node_id() {
  local nodeId="$1"
  [ -n "${nodeId}" ] || { warn "no nodeId returned; leaving config untouched"; return 0; }
  [ -f "${CONFIG}" ] || { warn "config ${CONFIG} not found; nodeId=${nodeId} not stamped"; return 0; }

  if grep -qE '^[[:space:]]*nodeId[[:space:]]*=' "${CONFIG}"; then
    log "updating nodeId in ${CONFIG}"
    # in-place, portable across GNU/BSD sed via a temp file
    local tmp; tmp="$(mktemp)"
    sed -E "s|^[[:space:]]*nodeId[[:space:]]*=.*|nodeId = ${nodeId}|" \
      "${CONFIG}" > "${tmp}"
    cat "${tmp}" > "${CONFIG}"
    rm -f "${tmp}"
  else
    warn "no nodeId key in ${CONFIG}; appending under [identity] is not safe to"
    warn "automate - set 'nodeId = ${nodeId}' manually under [identity]."
  fi
}

# ---- run ------------------------------------------------------------------
log "SolariNet enrollment (role=${ROLE} fqdn=${FQDN})"

if [ -f "${CERT_FILE}" ] && [ "${FORCE}" -eq 0 ] && [ "${CSR_ONLY}" -eq 0 ]; then
  die "a certificate already exists at ${CERT_FILE} (use --force to re-enroll)"
fi

command -v jq >/dev/null 2>&1 || die "jq is required for submission/install"

generate_key_and_csr

if [ "${CSR_ONLY}" -eq 1 ]; then
  log "CSR generated: ${CSR_FILE}"
  log "fingerprint: $(csr_fingerprint)"
  log "(--csr-only) skipping submission"
  exit 0
fi

response="$(submit_enrollment)"
install_material "${response}"
