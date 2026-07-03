#!/bin/sh
# =============================================================================
# SolariNet Fleet Provisioning - first-boot agent installer  (POSIX sh)
# =============================================================================
#
# Runs ONCE on a freshly installed target (Debian preseed late_command, Ubuntu
# autoinstall late-commands, openSUSE AutoYaST post script, or a Pi firstboot
# service all fetch and execute this). It is intentionally POSIX /bin/sh so the
# same script works under dash, busybox ash (Pi/initramfs), and bash.
#
# WHAT IT DOES
#   1. Pins the SolariNet server name in /etc/hosts (@@SERVER_IP@@ @@SERVER_NAME@@).
#   2. Fetches the architecture-matched solariClient binary and installs it to
#      /usr/local/bin/solariClient.
#   3. Writes /etc/solari/client.conf (INI format matching
#      deploy/dashboard/client.conf.sample).
#   4. Installs + enables the systemd unit (mirrors
#      deploy/systemd/solari-client.service).
#   5. Leaves an enrollment marker /etc/solari/PENDING_ENROLLMENT. Certificate
#      issuance is a separate, operator-approved step (see the clearly delimited
#      "SOLARINET ENROLLMENT HOOK" block below) - the integrator wires the real
#      CA-signing call there (e.g. deploy/enrollment/solari-enroll.sh).
#
# It is idempotent: re-running updates config/binary in place and does not
# duplicate the /etc/hosts pin or re-enroll if a cert already exists.
#
# TOKENS (rendered by the integrator; see netboot/README.md manifest):
#   @@HTTP_BASE@@ @@ARCH@@ @@HOSTNAME@@ @@FQDN@@
#   @@SERVER_URL@@ @@SERVER_NAME@@ @@SERVER_IP@@
# These may ALSO be supplied at runtime via environment variables of the same
# name (without the @@), which is how the Pi firstboot service passes them. The
# rendered literal wins only if the env var is unset.
# =============================================================================

set -eu

# ---- resolve parameters (env override > rendered token) ---------------------
# Using ${VAR:-@@TOKEN@@} means: if the caller exported VAR use it, else fall
# back to the value the render step baked in. This lets the very same file be
# both a rendered artifact AND a generic script driven by env vars.
HTTP_BASE="${SOLARI_HTTP_BASE:-@@HTTP_BASE@@}"
ARCH="${SOLARI_ARCH:-@@ARCH@@}"
HOSTNAME_S="${SOLARI_HOSTNAME:-@@HOSTNAME@@}"
FQDN="${SOLARI_FQDN:-@@FQDN@@}"
SERVER_URL="${SOLARI_SERVER_URL:-@@SERVER_URL@@}"
SERVER_NAME="${SOLARI_SERVER_NAME:-@@SERVER_NAME@@}"
SERVER_IP="${SOLARI_SERVER_IP:-@@SERVER_IP@@}"

CONF_DIR=/etc/solari
BIN_PATH=/usr/local/bin/solariClient
UNIT_PATH=/etc/systemd/system/solari-client.service
LOG_TAG="solari-firstboot"

log()  { printf '[%s] %s\n' "$LOG_TAG" "$*"; logger -t "$LOG_TAG" "$*" 2>/dev/null || true; }
warn() { printf '[%s][warn] %s\n' "$LOG_TAG" "$*" >&2; logger -t "$LOG_TAG" "warn: $*" 2>/dev/null || true; }
die()  { printf '[%s][error] %s\n' "$LOG_TAG" "$*" >&2; logger -t "$LOG_TAG" "error: $*" 2>/dev/null || true; exit 1; }

# Pick whichever HTTP fetcher exists (curl on most; wget on busybox/Pi initramfs).
fetch() { # fetch <url> <dest>
  url="$1"; dest="$2"
  if command -v curl >/dev/null 2>&1; then
    curl --fail --silent --show-error --location "$url" -o "$dest"
  elif command -v wget >/dev/null 2>&1; then
    wget -q -O "$dest" "$url"
  else
    die "neither curl nor wget available to fetch $url"
  fi
}

log "starting first-boot for ${FQDN:-$HOSTNAME_S} (arch=${ARCH})"

# ---- 1. pin the server name in /etc/hosts -----------------------------------
# So the client can resolve @@SERVER_NAME@@ even before DNS is authoritative,
# and so mutual-TLS hostname verification matches the server cert SAN.
if [ -n "${SERVER_IP}" ] && [ -n "${SERVER_NAME}" ]; then
  if grep -qE "[[:space:]]${SERVER_NAME}(\$|[[:space:]])" /etc/hosts 2>/dev/null; then
    log "/etc/hosts already pins ${SERVER_NAME}; leaving as-is"
  else
    log "pinning ${SERVER_IP} ${SERVER_NAME} in /etc/hosts"
    printf '%s\t%s\n' "${SERVER_IP}" "${SERVER_NAME}" >> /etc/hosts
  fi
else
  warn "SERVER_IP/SERVER_NAME not set; skipping /etc/hosts pin"
fi

# ---- 2. install the solariClient binary -------------------------------------
mkdir -p "$(dirname "$BIN_PATH")"
BIN_URL="${HTTP_BASE}/installers/solariClient.${ARCH}"
TMP_BIN="$(mktemp)"
log "fetching client binary ${BIN_URL}"
if fetch "${BIN_URL}" "${TMP_BIN}"; then
  # Install atomically then flip the exec bit; refuse a zero-byte download.
  if [ -s "${TMP_BIN}" ]; then
    install -m 0755 "${TMP_BIN}" "${BIN_PATH}"
    log "installed ${BIN_PATH}"
  else
    warn "downloaded client binary was empty; not installing"
  fi
else
  warn "could not fetch ${BIN_URL}; agent binary NOT installed (enrollment still staged)"
fi
rm -f "${TMP_BIN}"

# ---- 3. create the solari system user + config ------------------------------
# Matches deploy/systemd/solari-client.service (User=solari, Group=solari).
if ! id solari >/dev/null 2>&1; then
  log "creating system user 'solari'"
  useradd --system --no-create-home --shell /usr/sbin/nologin solari \
    2>/dev/null || warn "useradd solari failed (may already exist)"
fi

mkdir -p "${CONF_DIR}"
chmod 0750 "${CONF_DIR}"

# INI config identical in shape to deploy/dashboard/client.conf.sample.
# nodeId is intentionally omitted -> the client derives FNV-1a-64(fqdn|role).
log "writing ${CONF_DIR}/client.conf"
cat > "${CONF_DIR}/client.conf" <<EOF
# SolariNet client agent config - generated by solari-firstboot.sh
[identity]
hostFqdn = ${FQDN:-$HOSTNAME_S}

[server]
primaryUrl = ${SERVER_URL}

[tls]
caFile   = ${CONF_DIR}/ca.pem
certFile = ${CONF_DIR}/node.pem
keyFile  = ${CONF_DIR}/node.key

[schedule]
sampleIntervalSec = 15

[watch]
spoolDb = /var/lib/solari/client-spool.db
EOF
chmod 0644 "${CONF_DIR}/client.conf"

# ---- 4. install + enable the systemd unit -----------------------------------
# A trimmed copy of deploy/systemd/solari-client.service. We only enable (not
# start) it here: without certificates the client cannot connect, so it would
# crash-loop. It starts cleanly after the enrollment hook installs certs and the
# marker is cleared. systemd will bring it up on the next boot regardless.
if command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
  log "installing systemd unit ${UNIT_PATH}"
  cat > "${UNIT_PATH}" <<EOF
[Unit]
Description=SolariNet Client
Documentation=https://github.com/soren42/Solarinet
After=network-online.target
Wants=network-online.target
# Do not start until enrollment material has been delivered.
ConditionPathExists=!${CONF_DIR}/PENDING_ENROLLMENT

[Service]
Type=simple
ExecStart=${BIN_PATH} --config ${CONF_DIR}/client.conf
Restart=always
RestartSec=2
User=solari
Group=solari
StateDirectory=solari
StateDirectoryMode=0750
RuntimeDirectory=solari
RuntimeDirectoryMode=0750
ReadWritePaths=/var/lib/solari
NoNewPrivileges=yes
ProtectSystem=strict
ProtectHome=yes
PrivateTmp=yes
CapabilityBoundingSet=
AmbientCapabilities=
RestrictAddressFamilies=AF_UNIX AF_INET AF_INET6

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload 2>/dev/null || true
  systemctl enable solari-client.service 2>/dev/null \
    || warn "could not enable solari-client.service"
else
  warn "systemd not detected; skipping unit install (wire your init manually)"
fi

# =============================================================================
# ===================  SOLARINET ENROLLMENT HOOK (INTEGRATOR)  =================
# =============================================================================
# Everything above is generic bring-up. The block below is the SolariNet-
# specific enrollment step and is DELIBERATELY delimited so the integrator can
# replace it with the real CA-signing call.
#
# The implemented behavior (per the design brief) is the SIMPLE one: drop a
# marker file recording that this node needs a certificate, and log loudly. No
# private key material or token is embedded in this image.
#
# To wire the REAL enrollment, replace the marker block with, e.g.:
#     export SOLARI_SERVER="${SERVER_NAME}"
#     /usr/local/sbin/solari-enroll.sh --role client --token "<one-time-token>" \
#         --fqdn "${FQDN}"
# where <one-time-token> is delivered out-of-band (cloud-init user-data secret,
# a call back to @@HTTP_BASE@@/enroll?mac=..., or a TPM-bound attestation).
# solari-enroll.sh then writes ${CONF_DIR}/{node.pem,node.key,ca.pem}; after
# that succeeds, clear the marker and start the service:
#     rm -f "${CONF_DIR}/PENDING_ENROLLMENT"
#     systemctl start solari-client.service
# -----------------------------------------------------------------------------
if [ -f "${CONF_DIR}/node.pem" ] && [ -f "${CONF_DIR}/ca.pem" ]; then
  log "certificate material already present; clearing enrollment marker"
  rm -f "${CONF_DIR}/PENDING_ENROLLMENT"
  systemctl start solari-client.service 2>/dev/null || true
else
  log "no certificate yet; staging PENDING_ENROLLMENT marker"
  {
    echo "host=${FQDN:-$HOSTNAME_S}"
    echo "server=${SERVER_NAME}"
    echo "serverUrl=${SERVER_URL}"
    echo "staged=$(date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)"
    echo "note=enrollment material must be delivered (see solari-enroll.sh)"
  } > "${CONF_DIR}/PENDING_ENROLLMENT"
  chmod 0640 "${CONF_DIR}/PENDING_ENROLLMENT"
  warn "PENDING_ENROLLMENT: ${FQDN:-$HOSTNAME_S} needs a client certificate."
  warn "Run enrollment, then: rm ${CONF_DIR}/PENDING_ENROLLMENT && systemctl start solari-client"
fi
# =====================  END SOLARINET ENROLLMENT HOOK  =======================

log "first-boot complete for ${FQDN:-$HOSTNAME_S}"
exit 0
