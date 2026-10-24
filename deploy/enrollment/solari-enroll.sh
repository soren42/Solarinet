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
#      through the bearer-authenticated HTTPS route, which delegates to
#      solariCtl without exposing the CA key.
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
#   --fqdn FQDN          This node's FQDN (default: hostname -f).
#   --conf-dir DIR       TLS/material dir (default: /etc/solari).
#   --config FILE        Node config to stamp nodeId into
#                        (default: ${conf-dir}/${role}.conf).
#   --key-bits N         RSA key size when not using EC (default: EC P-256).
#   --rsa                Use RSA-3072 instead of the default EC P-256 key.
#   --csr-only           Generate key+CSR and stop (print CSR path); no submit.
#   --insecure           Allow curl to skip TLS verification (bootstrap only).
#   --force              Regenerate key/CSR and overwrite installed material.
#   --resume             Recover an installed cert whose server ACK failed; only
#                        succeeds if the server returns the exact installed cert.
#   -h | --help          Show this help.
#
set -euo pipefail

# ---- defaults -------------------------------------------------------------
ROLE=""
TOKEN=""
SERVER_URL="https://${SOLARI_SERVER:-localhost}"
FQDN=""
CONF_DIR="/etc/solari"
CONFIG=""
KEY_BITS=3072
USE_RSA=0
CSR_ONLY=0
INSECURE=0
FORCE=0
RESUME=0
TEMP_KEY_FILE=""
WORK_KEY_FILE=""

log()  { printf '\033[1;36m[enroll]\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[1;33m[enroll][warn]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[enroll][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- arg parsing ----------------------------------------------------------
while [ $# -gt 0 ]; do
  case "$1" in
    --role)        ROLE="${2:?}"; shift ;;
    --token)       TOKEN="${2:?}"; shift ;;
    --server)      SERVER_URL="${2:?}"; shift ;;
    --fqdn)        FQDN="${2:?}"; shift ;;
    --conf-dir)    CONF_DIR="${2:?}"; shift ;;
    --config)      CONFIG="${2:?}"; shift ;;
    --key-bits)    KEY_BITS="${2:?}"; shift ;;
    --rsa)         USE_RSA=1 ;;
    --csr-only)    CSR_ONLY=1 ;;
    --insecure)    INSECURE=1 ;;
    --force)       FORCE=1 ;;
    --resume)      RESUME=1 ;;
    -h|--help)     grep '^#' "$0" | grep -v '^#!' | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
  shift
done

if [ "${RESUME}" -eq 1 ] && { [ "${FORCE}" -eq 1 ] || [ "${CSR_ONLY}" -eq 1 ]; }; then
  die "--resume cannot be combined with --force or --csr-only"
fi

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
WORK_KEY_FILE="${KEY_FILE}"
CSR_FILE="${CONF_DIR}/node.csr"
CERT_FILE="${CONF_DIR}/node.pem"
CA_FILE="${CONF_DIR}/ca.pem"

# CN's first label encodes the role consumed by server ingest authorization.
# The server identity contract is <role>.<fqdn>, e.g. monitor.beryl.akoria.net.
CERT_CN="${ROLE}.${FQDN}"

# ---- privilege / dirs -----------------------------------------------------
if [ ! -d "${CONF_DIR}" ]; then
  log "creating ${CONF_DIR}"
  mkdir -p "${CONF_DIR}" || die "cannot create ${CONF_DIR} (need root?)"
fi
chmod 0750 "${CONF_DIR}" 2>/dev/null || true

cleanup_temp_key() {
  if [ -n "${TEMP_KEY_FILE}" ] && [ -e "${TEMP_KEY_FILE}" ]; then
    rm -f "${TEMP_KEY_FILE}"
  fi
}
trap cleanup_temp_key EXIT

# ---- key + CSR ------------------------------------------------------------
generate_key_and_csr() {
  if [ -f "${KEY_FILE}" ] && [ "${FORCE}" -eq 0 ]; then
    log "reusing existing private key ${KEY_FILE} (--force to regenerate)"
  else
    if [ "${FORCE}" -eq 1 ] && [ -f "${CERT_FILE}" ]; then
      TEMP_KEY_FILE="$(mktemp "${CONF_DIR}/.node.key.XXXXXXXX")" || die "cannot create staged private key"
      WORK_KEY_FILE="${TEMP_KEY_FILE}"
    fi
    log "generating private key ${WORK_KEY_FILE}"
    umask 077
    if [ "${USE_RSA}" -eq 1 ]; then
      openssl genpkey -algorithm RSA \
        -pkeyopt "rsa_keygen_bits:${KEY_BITS}" -out "${WORK_KEY_FILE}"
    else
      openssl genpkey -algorithm EC \
        -pkeyopt ec_paramgen_curve:P-256 -out "${WORK_KEY_FILE}"
    fi
    chmod 0600 "${WORK_KEY_FILE}"
  fi

  log "generating CSR ${CSR_FILE} (CN=${CERT_CN})"
  openssl req -new -key "${WORK_KEY_FILE}" -out "${CSR_FILE}" \
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
# /api/enrollments endpoint. The response
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
                      -X POST --data-binary @-)
  [ "${INSECURE}" -eq 1 ] && curl_args+=(--insecure)

  log "submitting CSR to ${SERVER_URL}/api/enrollments"
  resp="$(printf '%s' "${payload}" | curl "${curl_args[@]}" \
           "${SERVER_URL%/}/api/enrollments")" \
    || die "enrollment submission failed (HTTPS endpoint)"

  printf '%s' "${resp}"
}

# Revoke the bearer only after the leaf and trust anchor are installed.
ack_enrollment() {
  local payload resp status
  payload="$(jq -cn --arg token "${TOKEN}" '{token:$token}')"
  local -a curl_args=(--fail --silent --show-error
                      -H 'Content-Type: application/json'
                      -X POST --data-binary @-)
  [ "${INSECURE}" -eq 1 ] && curl_args+=(--insecure)
  resp="$(printf '%s' "${payload}" | curl "${curl_args[@]}" \
    "${SERVER_URL%/}/api/enrollments/ack")" || {
      warn "certificate installed, but server acknowledgement failed; retry enrollment with the same token"
      return 0
    }
  status="$(printf '%s' "${resp}" | jq -r '.status // .data.status // empty')"
  if [ "${status}" != "acknowledged" ]; then
    warn "certificate installed, but server did not confirm token revocation"
  fi
}

# ---- install --------------------------------------------------------------
install_material() {
  local resp="$1" status nodeId cert ca staging cert_pub key_pub had_ca=0 key_committed=0

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

  # Validate into a private staging directory before touching installed files.
  # This also protects an existing identity when --force is used and the server
  # returns malformed, unrelated, or incorrectly signed certificate material.
  staging="$(mktemp -d "${CONF_DIR}/.enroll.XXXXXXXX")" || die "cannot create certificate staging directory"
  chmod 0700 "${staging}"
  printf '%s\n' "${cert}" > "${staging}/node.pem"
  printf '%s\n' "${ca}"   > "${staging}/ca.pem"
  chmod 0600 "${staging}/node.pem" "${staging}/ca.pem"

  if ! openssl x509 -in "${staging}/node.pem" -noout >/dev/null 2>&1; then
    rm -rf "${staging}"
    die "server response contained an invalid certificate"
  fi
  if ! openssl x509 -in "${staging}/ca.pem" -noout >/dev/null 2>&1; then
    rm -rf "${staging}"
    die "server response contained an invalid CA certificate"
  fi
  if ! openssl verify -CAfile "${staging}/ca.pem" "${staging}/node.pem" >/dev/null 2>&1; then
    rm -rf "${staging}"
    die "issued certificate does not verify against the returned CA"
  fi

  cert_pub="$(openssl x509 -in "${staging}/node.pem" -pubkey -noout \
    | openssl pkey -pubin -outform DER 2>/dev/null \
    | openssl dgst -sha256 | awk '{print $NF}')" || {
      rm -rf "${staging}"
      die "cannot read issued certificate public key"
    }
  key_pub="$(openssl pkey -in "${WORK_KEY_FILE}" -pubout -outform DER 2>/dev/null \
    | openssl dgst -sha256 | awk '{print $NF}')" || {
      rm -rf "${staging}"
      die "cannot read local private key public key"
    }
  if [ -z "${cert_pub}" ] || [ "${cert_pub}" != "${key_pub}" ]; then
    rm -rf "${staging}"
    die "issued certificate does not match the local private key"
  fi

  # A prior run may have installed the leaf and then lost the server ACK. In
  # resume mode, accept only a byte-identical X.509 leaf from the same token;
  # never replace a working identity during recovery.
  if [ "${RESUME}" -eq 1 ] && [ -f "${CERT_FILE}" ]; then
    local installed_fp returned_fp
    if ! openssl x509 -in "${CERT_FILE}" -noout >/dev/null 2>&1 ||
       ! openssl verify -CAfile "${CA_FILE}" "${CERT_FILE}" >/dev/null 2>&1; then
      rm -rf "${staging}"
      die "installed certificate is invalid; refusing enrollment resume"
    fi
    installed_fp="$(openssl x509 -in "${CERT_FILE}" -noout -fingerprint -sha256 | sed 's/^[^=]*=//')"
    returned_fp="$(openssl x509 -in "${staging}/node.pem" -noout -fingerprint -sha256 | sed 's/^[^=]*=//')"
    if [ -z "${installed_fp}" ] || [ "${installed_fp}" != "${returned_fp}" ]; then
      rm -rf "${staging}"
      die "server returned a different certificate; refusing to replace installed identity during resume"
    fi
    rm -rf "${staging}"
    stamp_node_id "${nodeId}"
    ack_enrollment
    log "resume complete (installed certificate unchanged, nodeId=${nodeId:-unknown}, role=${ROLE})"
    return 0
  fi

  chmod 0644 "${staging}/node.pem" "${staging}/ca.pem"
  if [ -e "${CA_FILE}" ]; then
    cp -p "${CA_FILE}" "${staging}/previous-ca.pem" || {
      rm -rf "${staging}"
      die "cannot preserve the installed CA before replacement"
    }
    had_ca=1
  fi
  if [ -n "${TEMP_KEY_FILE}" ] && [ -e "${KEY_FILE}" ]; then
    cp -p "${KEY_FILE}" "${staging}/previous-key.pem" || {
      rm -rf "${staging}"
      die "cannot preserve the installed private key before replacement"
    }
  fi
  # Publish the trust anchor first and the leaf certificate last. The client
  # treats node.pem as the enrollment-complete marker, so it never sees a leaf
  # before its CA is installed.
  log "installing CA root ${CA_FILE}"
  if ! mv -f "${staging}/ca.pem" "${CA_FILE}"; then
    rm -rf "${staging}"
    die "cannot install the CA certificate"
  fi
  if [ -n "${TEMP_KEY_FILE}" ]; then
    log "installing renewed private key ${KEY_FILE}"
    if ! mv -f "${TEMP_KEY_FILE}" "${KEY_FILE}"; then
      local rollback_failed=0
      if [ "${had_ca}" -eq 1 ]; then
        mv -f "${staging}/previous-ca.pem" "${CA_FILE}" || {
          warn "could not restore the previous CA certificate"
          rollback_failed=1
        }
      else
        rm -f "${CA_FILE}" || rollback_failed=1
      fi
      if [ "${rollback_failed}" -eq 1 ]; then
        die "cannot install the renewed key and CA rollback failed; recovery files retained in ${staging}"
      fi
      rm -rf "${staging}"
      die "cannot install the renewed private key; restored the previous CA state"
    fi
    key_committed=1
    TEMP_KEY_FILE=""
  fi
  log "installing signed certificate ${CERT_FILE}"
  if ! mv -f "${staging}/node.pem" "${CERT_FILE}"; then
    local rollback_failed=0
    if [ "${had_ca}" -eq 1 ]; then
      mv -f "${staging}/previous-ca.pem" "${CA_FILE}" || {
        warn "could not restore the previous CA certificate"
        rollback_failed=1
      }
    else
      rm -f "${CA_FILE}" || rollback_failed=1
    fi
    if [ -e "${staging}/previous-key.pem" ]; then
      mv -f "${staging}/previous-key.pem" "${KEY_FILE}" || {
        warn "could not restore the previous private key"
        rollback_failed=1
      }
    elif [ "${key_committed}" -eq 1 ]; then
      rm -f "${KEY_FILE}" || rollback_failed=1
    fi
    if [ "${rollback_failed}" -eq 1 ]; then
      die "cannot install the issued certificate and rollback failed; recovery files retained in ${staging}"
    fi
    rm -rf "${staging}"
    die "cannot install the issued certificate; restored the previous CA state"
  fi
  rm -f "${staging}/previous-ca.pem" "${staging}/previous-key.pem"
  rmdir "${staging}"

  stamp_node_id "${nodeId}"
  ack_enrollment
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

if [ -f "${CERT_FILE}" ] && [ "${FORCE}" -eq 0 ] && [ "${RESUME}" -eq 0 ] && [ "${CSR_ONLY}" -eq 0 ]; then
  die "a certificate already exists at ${CERT_FILE} (use --force to re-enroll or --resume to retry a failed server acknowledgement)"
fi

if [ "${RESUME}" -eq 1 ] && [ -f "${CERT_FILE}" ]; then
  [ -f "${KEY_FILE}" ] || die "cannot resume enrollment without the installed private key"
  [ -f "${CA_FILE}" ] || die "cannot resume enrollment without the installed CA certificate"
  openssl verify -CAfile "${CA_FILE}" "${CERT_FILE}" >/dev/null 2>&1 || die "installed certificate does not verify against the installed CA"
  existing_pub="$(openssl x509 -in "${CERT_FILE}" -pubkey -noout 2>/dev/null | openssl pkey -pubin -outform DER 2>/dev/null | openssl dgst -sha256 | awk '{print $NF}')" || die "cannot read installed certificate public key"
  key_pub="$(openssl pkey -in "${KEY_FILE}" -pubout -outform DER 2>/dev/null | openssl dgst -sha256 | awk '{print $NF}')" || die "cannot read local private key public key"
  [ -n "${existing_pub}" ] && [ "${existing_pub}" = "${key_pub}" ] || die "installed certificate does not match the local private key"
fi

generate_key_and_csr

if [ "${CSR_ONLY}" -eq 1 ]; then
  if [ -n "${TEMP_KEY_FILE}" ]; then
    if [ -f "${CERT_FILE}" ]; then
      die "cannot use --csr-only --force while a certificate is installed; use a complete forced renewal"
    fi
    mv -f "${TEMP_KEY_FILE}" "${KEY_FILE}" || die "cannot install generated private key"
    TEMP_KEY_FILE=""
    WORK_KEY_FILE="${KEY_FILE}"
  fi
  log "CSR generated: ${CSR_FILE}"
  log "fingerprint: $(csr_fingerprint)"
  log "(--csr-only) skipping submission"
  exit 0
fi

command -v jq >/dev/null 2>&1 || die "jq is required for submission/install"
response="$(submit_enrollment)"
install_material "${response}"
