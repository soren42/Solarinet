#!/usr/bin/env bash
# =============================================================================
# fleet-lib.sh - shared config + helpers for the SolariNet fleet-provisioning
# scripts (fleet-provision.sh, fleet-image.sh). Sourced, not executed.
#
# These run on the SolariNet server host (xenon): they mint enrollment certs via
# the local solariCtl bridge, render the netboot/Ansible templates, and push the
# results to the provisioning host (benzene) over SSH. Nothing here touches the
# live network — netboot only goes live when the DHCP switch is flipped.
# =============================================================================
set -euo pipefail

# ---- topology ---------------------------------------------------------------
: "${BENZENE:=jason@benzene.akoria.net}"          # provisioning host (ssh target)
: "${BENZENE_IP:=10.5.2.50}"                        # its LAN IP (netboot HTTP base)
: "${HTTP_PORT:=8080}"
: "${HTTP_BASE:=http://${BENZENE_IP}:${HTTP_PORT}}"
: "${PROV_ROOT:=/srv/solari-provision}"
: "${HTTP_ROOT:=${PROV_ROOT}/netboot/http}"
: "${CTL_SOCK:=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/run/solariCtl.sock}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FLEET_DIR="${REPO_ROOT}/deploy/fleet"

# ---- provisioning defaults (overridable per call) ---------------------------
: "${DOMAIN:=akoria.net}"
: "${TIMEZONE:=Etc/UTC}"
: "${ADMIN_USER:=solari-admin}"
: "${DEFAULT_DISK:=/dev/sda}"
: "${SERVER_URL:=tls+tcp://xenon:7701}"
: "${SERVER_NAME:=xenon}"
: "${SERVER_IP:=10.0.0.20}"

# THE shared core package stack — kept identical to the Ansible contract in
# deploy/fleet/ansible/group_vars/all.yml (solari_core_packages). One logical
# set, applied on every distro + arch.
FLEET_CORE_PACKAGES="sudo curl wget ca-certificates gnupg htop tmux git vim rsync jq unzip lsof chrony python3 net-tools openssh-server"

# ---- logging (matches the PHP GET log-tail markers) -------------------------
flog()  { printf '\033[1;36m[fleet]\033[0m %s\n' "$*"; }
fwarn() { printf '\033[1;33m[fleet][warn]\033[0m %s\n' "$*" >&2; }
fdie()  { printf '\033[1;31m[fleet][error]\033[0m %s\n' "$*" >&2; exit 1; }

# ---- ssh/scp to benzene -----------------------------------------------------
# The provisioning host prints a fastfetch login banner on the session's STDERR
# (pam_motd, below the shell-rc level) on every SSH connection. Strip just that
# one banner line from stderr via a process substitution; stdout passes through
# untouched (so bput's piped data and command reads are unaffected) and ssh's
# real exit status is preserved so `|| fdie` still works.
bssh() {
  ssh -o BatchMode=yes "${BENZENE}" "$@" 2> >(grep -vE 'Kernel: Linux.*CPU:' >&2)
}
# write stdin to a remote path (creates parent dir), optional mode
bput() { # <remote-path> [mode]
  local dst="$1" mode="${2:-0644}"
  bssh "install -D -m ${mode} /dev/stdin '${dst}'"
}

# ---- MAC normalization ------------------------------------------------------
# accepts aa:bb:.., aa-bb-.., aabbcc.. -> lowercase hyphen form aa-bb-cc-dd-ee-ff
mac_hexhyp() {
  printf '%s' "$1" | tr 'A-F' 'a-f' | tr -d ':-' \
    | sed -E 's/(..)(..)(..)(..)(..)(..)/\1-\2-\3-\4-\5-\6/'
}
is_mac() { printf '%s' "$1" | grep -qiE '^([0-9a-f]{2}[:-]?){5}[0-9a-f]{2}$'; }

# ---- template rendering (safe literal @@TOKEN@@ substitution) ---------------
# render_template <template-file>  (tokens supplied via T_* env, see below).
# Uses python3 for literal replace so SSH keys / URLs with / & special chars are
# never misinterpreted (unlike sed).
render_template() {
  local tpl="$1"
  python3 - "$tpl" <<'PY'
import os, sys
tpl = open(sys.argv[1]).read()
for k, v in os.environ.items():
    if k.startswith("TOK_"):
        tpl = tpl.replace("@@" + k[4:] + "@@", v)
sys.stdout.write(tpl)
PY
}

# ---- enrollment cert minting via the solariCtl SIGN verb --------------------
# sign_node_cert <fqdn> <op> <out-dir> : writes ca.pem, node.pem, node.key.
# The CA private key never leaves the server process; we only send a CSR.
sign_node_cert() {
  local fqdn="$1" op="$2" out="$3"
  mkdir -p "${out}"; chmod 700 "${out}"
  [ -S "${CTL_SOCK}" ] || fdie "solariCtl socket not found: ${CTL_SOCK} (server running?)"
  ( umask 077
    openssl ecparam -name prime256v1 -genkey -noout -out "${out}/node.key"
    openssl req -new -key "${out}/node.key" \
      -subj "/CN=client.${fqdn}/O=SolariNet/OU=client" -out "${out}/node.csr" )
  cp "${REPO_ROOT}/run/pki/ca.pem" "${out}/ca.pem" 2>/dev/null || \
    fwarn "run/pki/ca.pem not found; ca.pem will be missing"
  python3 - "${CTL_SOCK}" "${out}/node.csr" "${out}/node.pem" "${op}" <<'PY'
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
  [ -s "${out}/node.pem" ] || fdie "CA did not return a signed certificate"
  rm -f "${out}/node.csr"
}

# operator SSH public key injected into provisioned hosts (so we can reach them)
fleet_admin_pubkey() {
  local k
  for k in "${HOME}/.ssh/id_ed25519.pub" "${HOME}/.ssh/id_rsa.pub" "${HOME}"/.ssh/*.pub; do
    [ -f "$k" ] && { cat "$k"; return 0; }
  done
  return 1
}
