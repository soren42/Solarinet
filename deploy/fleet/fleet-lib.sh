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
# The NAS image store (installers/ + images/ live there; HTTP_ROOT links into
# it) and its required filesystem type — see prov-store-guard.sh. "none" skips
# the mount check and is accepted only under the SOLARI_PROV_LOCAL=1 test seam.
: "${PROV_STORE:=${PROV_ROOT}/store}"
: "${PROV_STORE_FSTYPE:=cifs}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# solariServer exports SOLARI_CTL_SOCK / SOLARI_CA_PEM to the helpers it spawns
# (an installed release has no run/ tree); the run/ paths are the dev fallback.
: "${CTL_SOCK:=${SOLARI_CTL_SOCK:-${REPO_ROOT}/run/solariCtl.sock}}"
: "${CA_PEM:=${SOLARI_CA_PEM:-${REPO_ROOT}/run/pki/ca.pem}}"
# Shared by fleet-provision.sh and fleet-image.sh after this library is sourced.
# shellcheck disable=SC2034
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
# shellcheck disable=SC2034
FLEET_CORE_PACKAGES="sudo curl wget ca-certificates gnupg htop tmux git vim rsync jq unzip lsof chrony python3 net-tools openssh-server"

# The INSTALLER's own package set: just enough for first boot to be reachable
# and manageable by Ansible (sshd, sudo, a fetcher, CA roots, python3). Per
# family because the names differ; each name becomes its own <package> /
# YAML list item / pkgsel word (render_package_tokens). Core and every other
# stack arrive after the install via prov-finish.py (SoR stacks over SSH).
# openSUSE: python3 is a capability provided by the versioned interpreter
# (python3xx), not a package NAME on Tumbleweed, so it is not an AutoYaST
# <package> (an unresolvable name stops an unattended install); the AutoYaST
# chroot script installs it with zypper and prov-finish.py ensures it again.
# shellcheck disable=SC2034
FLEET_BOOTSTRAP_PACKAGES_SUSE="openssh sudo curl ca-certificates"
# shellcheck disable=SC2034
FLEET_BOOTSTRAP_PACKAGES_DEB="openssh-server sudo curl ca-certificates python3"
# solariClient's shared libraries (the release build links nng, mbedTLS 3.x,
# cJSON and SQLite), as Tumbleweed soname packages. They go in the profile's
# <packages> so YaST installs them in its own transaction: a zypper call from
# the chroot script loses to YaST's libzypp lock. Leap 15 ships mbedTLS 2.x,
# so no Leap set exists; the agent cannot run there.
# shellcheck disable=SC2034
FLEET_AGENT_LIBS_TUMBLEWEED="libnng1 libmbedtls21 libmbedcrypto16 libmbedx509-7 libcjson1 libsqlite3-0"

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
# write stdin to a remote path (creates parent dir), optional mode. Atomic: the
# data lands under a temp name in the same directory and is renamed into place,
# so a reader (an iPXE client fetching a profile) never sees a partial file.
bput() { # <remote-path> [mode]
  local dst="$1" mode="${2:-0644}" tmp="$1.tmp.$$"
  bssh "install -D -m ${mode} /dev/stdin '${tmp}' && mv -f '${tmp}' '${dst}' || { rm -f '${tmp}'; exit 1; }"
}

# ---- image store guard ------------------------------------------------------
# fleet_store_knobs: refuse unsafe PROV_STORE / PROV_STORE_FSTYPE values (and
# "none" outside the test seam) before they reach a remote command line.
fleet_store_knobs() {
  [[ "$PROV_STORE" =~ ^/[A-Za-z0-9._/-]+$ ]] && [[ "/$PROV_STORE/" != */../* ]] \
    || fdie "unsafe PROV_STORE: $PROV_STORE"
  [[ "$PROV_STORE_FSTYPE" =~ ^[a-z0-9.]{1,16}$ ]] || fdie "unsafe PROV_STORE_FSTYPE: $PROV_STORE_FSTYPE"
  if [ "$PROV_STORE_FSTYPE" = none ] && [ "${SOLARI_PROV_LOCAL:-}" != "1" ]; then
    fdie "PROV_STORE_FSTYPE=none is only allowed under SOLARI_PROV_LOCAL=1"
  fi
}
# fleet_require_store <sub>... — fail closed unless the NAS store is mounted on
# benzene and each HTTP_ROOT/<sub> resolves into it (prov-store-guard.sh).
fleet_require_store() {
  fleet_store_knobs
  local guard cmd a
  guard="$(cat "${FLEET_DIR}/prov-store-guard.sh")"$'\n''prov_store "$@"'
  cmd="sh -c $(fleet_sq "$guard") prov-store check"
  for a in "$PROV_STORE" "$PROV_STORE_FSTYPE" "$HTTP_ROOT" "$@"; do cmd+=" $(fleet_sq "$a")"; done
  bssh "$cmd" || fdie "provisioning image store check failed on ${BENZENE} (see the [store] line above)"
}
# bput_store <sub> <rel-path> [mode] < data — write stdin into the NAS image
# store at <sub>/<rel-path>. The store check, the shared maintenance lock and
# the write run in ONE remote shell, and the destination is built from the
# RESOLVED store path (PROV_STORE_REAL), never through HTTP_ROOT/<sub>: a check
# made earlier in the run proves nothing about the link by the time we write.
bput_store() {
  local sub="$1" rel="$2" mode="${3:-0644}" body cmd a
  fleet_store_knobs
  [[ "$sub" =~ ^(installers|images)$ ]] || fdie "bput_store: bad store subdir: $sub"
  [[ "$rel" =~ ^[A-Za-z0-9._][A-Za-z0-9._/-]*$ ]] && [[ "/$rel/" != */../* ]] \
    || fdie "bput_store: unsafe path: $rel"
  [[ "$mode" =~ ^0[0-7]{3}$ ]] || fdie "bput_store: bad mode: $mode"
  # shellcheck disable=SC2016
  body='prov_store check "$1" "$2" "$3" "$4" || exit 1
dst="$PROV_STORE_REAL/$4/$5"; tmp="${dst%/*}/.${dst##*/}.tmp.$$"
install -D -m "$6" /dev/stdin "$tmp" && mv -f "$tmp" "$dst" || { rm -f "$tmp"; exit 1; }'
  cmd="sh -c $(fleet_sq "$(cat "${FLEET_DIR}/prov-store-guard.sh")"$'\n'"$body") prov-store"
  for a in "$PROV_STORE" "$PROV_STORE_FSTYPE" "$HTTP_ROOT" "$sub" "$rel" "$mode"; do cmd+=" $(fleet_sq "$a")"; done
  bssh "$cmd"
}
# POSIX single-quoting for the remote login shell: one literal word per value.
fleet_sq() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

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
  cp "${CA_PEM}" "${out}/ca.pem" 2>/dev/null || \
    fwarn "${CA_PEM} not found; ca.pem will be missing"
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

# fleet_clean_pubkey <key-line> : print "type base64 [comment]" or fail.
# Keys are substituted into XML, YAML and single-quoted shell, so only the
# OpenSSH public-key alphabet passes; the comment is reduced to safe chars.
fleet_clean_pubkey() {
  local t b c
  read -r t b c <<<"$1" || true
  [[ "${t}" =~ ^(ssh-ed25519|ssh-rsa|ecdsa-sha2-nistp(256|384|521)|sk-ssh-ed25519@openssh\.com|sk-ecdsa-sha2-nistp256@openssh\.com)$ ]] || return 1
  [[ "${b}" =~ ^[A-Za-z0-9+/]+={0,2}$ ]] || return 1
  c="$(printf '%s' "${c}" | tr -cd 'A-Za-z0-9@._+:-')"
  printf '%s %s%s' "${t}" "${b}" "${c:+ ${c}}"
}

# The provisioning host's Ansible control key (benzene runs the playbooks as
# its BENZENE login user). Authorized for ADMIN_USER on PROV_STAGE installs so
# prov-finish.py can reach the fresh host. FLEET_CONTROL_PUBKEY overrides.
fleet_control_pubkey() {
  if [ -n "${FLEET_CONTROL_PUBKEY:-}" ]; then
    printf '%s\n' "${FLEET_CONTROL_PUBKEY}"
    return 0
  fi
  # shellcheck disable=SC2016  # expanded by the remote shell
  bssh 'for k in "$HOME/.ssh/id_ed25519.pub" "$HOME/.ssh/id_ecdsa.pub" "$HOME/.ssh/id_rsa.pub"; do [ -f "$k" ] && { head -n 1 "$k"; exit 0; }; done; exit 1'
}

# render_package_tokens <name...> : export TOK_PACKAGES (space-separated, for
# pkgsel/include), TOK_PACKAGES_XML (one <package> per name) and
# TOK_PACKAGES_YAML (one "- name" list item per name). Duplicates are dropped
# in order; a name outside the distro package alphabet is refused.
render_package_tokens() {
  local p seen=" " words="" xml="" yaml=""
  for p in "$@"; do
    [[ "${p}" =~ ^[a-z0-9][a-z0-9.+_-]{0,99}$ ]] || fdie "invalid package name: ${p}"
    case "${seen}" in *" ${p} "*) continue ;; esac
    seen="${seen}${p} "
    words="${words:+${words} }${p}"
    xml="${xml:+${xml}
}      <package>${p}</package>"
    yaml="${yaml:+${yaml}
}    - ${p}"
  done
  export TOK_PACKAGES="${words}" TOK_PACKAGES_XML="${xml}" TOK_PACKAGES_YAML="${yaml}"
}
