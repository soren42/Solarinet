#!/usr/bin/env bash
# verify-boundary.sh — assert the PHP→host security boundary holds on this host.
#
# Task #1. Run on a target host AFTER cutover (see PERMISSIONS.md). It proves the
# three enforced layers observably, from the dashboard uid's point of view:
#   1. www-solari CANNOT read the CA key / server env (uid split + file perms).
#   2. The ctl socket is 0660 solari:solari-ctl and www-solari can reach it.
#   3. www-solari is REFUSED a privileged verb over the socket, and CAN enqueue.
#
# Near-read-only: it sends a PING, a privileged verb that MUST be refused (with a
# nonexistent node id), and a REQUEST_GET on a bogus id. With --enqueue it also
# enqueues ONE privileged verb targeting a nonexistent node to prove the write
# path — SUBMIT only accepts privileged verbs and only enqueues (does not
# execute), so this leaves a single benign request that no-ops at consumption. It
# never submits real privileged work against a live target.
#
# Usage:  sudo ./verify-boundary.sh [--enqueue]
# Exit:   0 = every check PASS; 1 = at least one FAIL.

set -u

SRV_CONF="${SOLARI_SERVER_CONF:-/etc/solarinet/server.conf}"
SRV_ENV="${SOLARI_SERVER_ENV:-/etc/solarinet/server.env}"
SOCK="${SOLARI_CTL_SOCK:-/run/solari/solariCtl.sock}"
DASH_USER="${SOLARI_DASH_USER:-www-solari}"

fail=0
pass() { printf '  PASS  %s\n' "$1"; }
bad()  { printf '  FAIL  %s\n' "$1"; fail=1; }

# iniGet SECTION KEY FILE — echo the value of KEY under [SECTION] in an INI file.
# Ignores comments and surrounding whitespace; empty output if unset/missing.
iniGet() {
  local section="$1" key="$2" file="$3"
  [ -r "$file" ] || return 0
  awk -v s="$section" -v k="$key" '
    /^[[:space:]]*[;#]/ { next }
    /^[[:space:]]*\[/   { cur = $0; gsub(/[][[:space:]]/, "", cur); next }
    cur == s {
      line = $0; sub(/[;#].*$/, "", line)
      n = index(line, "=")
      if (n == 0) next
      name = substr(line, 1, n-1); gsub(/[[:space:]]/, "", name)
      if (name == k) {
        val = substr(line, n+1); gsub(/^[[:space:]]+|[[:space:]]+$/, "", val)
        print val; exit
      }
    }' "$file"
}

# The CA key is the decisive secret. Prefer an explicit override; otherwise read
# the ACTUAL configured path ([ca] keyFile) from server.conf so we test the real
# key, not a hardcoded guess that might not exist and would falsely SKIP (F11).
if [ -n "${SOLARI_CA_KEY:-}" ]; then
  CA_KEY="$SOLARI_CA_KEY"
else
  CA_KEY="$(iniGet ca keyFile "$SRV_CONF")"
fi

# runAsDash CMD... — run a command as the dashboard uid.
runAsDash() { runuser -u "$DASH_USER" -- "$@"; }

echo "== boundary verification (dashboard uid: $DASH_USER) =="

# --- layer 1: uid split + file perms ---
echo "[1] host secrets unreadable by $DASH_USER"
if id "$DASH_USER" >/dev/null 2>&1; then
  pass "$DASH_USER exists"
else
  bad "$DASH_USER does not exist (run systemd-sysusers first)"; echo; exit 1
fi

# DECISIVE: the whole boundary exists to keep this secret away from PHP. If we
# cannot locate or test it, we CANNOT assert the boundary — that is a FAIL, never
# a silent SKIP that still lets the run report PASS (review F11).
if [ -z "$CA_KEY" ]; then
  bad "CA key path unknown: [ca] keyFile not found in $SRV_CONF and SOLARI_CA_KEY unset — cannot verify the decisive secret"
elif [ ! -e "$CA_KEY" ]; then
  bad "CA key configured at $CA_KEY but not present — cannot verify $DASH_USER is denied it (host not fully provisioned?)"
elif runAsDash test -r "$CA_KEY" 2>/dev/null; then
  bad "$DASH_USER CAN read the CA key ($CA_KEY) — boundary broken"
else
  pass "$DASH_USER cannot read the CA key ($CA_KEY)"
fi

if [ -e "$SRV_ENV" ]; then
  if runAsDash test -r "$SRV_ENV" 2>/dev/null; then
    bad "$DASH_USER CAN read the server env/DB pass ($SRV_ENV)"
  else
    pass "$DASH_USER cannot read the server env ($SRV_ENV)"
  fi
else
  echo "  SKIP  server env not at $SRV_ENV (set SOLARI_SERVER_ENV)"
fi

# --- layer 2: socket ownership/mode ---
echo "[2] ctl socket ownership & mode"
if [ -S "$SOCK" ]; then
  mode=$(stat -c '%a' "$SOCK")
  owner=$(stat -c '%U:%G' "$SOCK")
  [ "$mode" = "660" ] && pass "socket mode 0660" || bad "socket mode $mode (want 0660)"
  # Owner AND group both matter. Accepting any *:solari-ctl would pass a socket
  # owned by e.g. www-solari:solari-ctl — the dashboard uid owning its own gate,
  # which violates the invariant. Require the operator uid (from [ctl] operatorUid,
  # default solari) as owner. operatorUid may be numeric in the conf; stat prints
  # a name, so fall back to "solari" when the configured value is not a name.
  want_user="${SOLARI_OP_USER:-$(iniGet ctl operatorUid "$SRV_CONF")}"
  case "$want_user" in ''|*[!A-Za-z0-9_-]*) want_user="solari";; esac
  case "$owner" in
    "$want_user":solari-ctl) pass "socket owner $owner (operator:solari-ctl)";;
    *:solari-ctl) bad "socket group ok but owner is $owner (want $want_user:solari-ctl)";;
    *) bad "socket ownership $owner (want $want_user:solari-ctl)";;
  esac
  # Reachability must be proven by an actual connect(): a socket is reached via
  # connect(), NOT read()/write() on its path node. access(2) — what `test -r/-w`
  # calls — disagrees with connect() on a socket inode (observed: it reports no
  # r/w for a group member whose connect() nonetheless succeeds), so `test -r/-w`
  # gives a FALSE negative here. Probe the real thing.
  if command -v socat >/dev/null 2>&1; then
    reach=$(printf 'PING\n' | runAsDash socat -t2 - "UNIX-CONNECT:$SOCK" 2>/dev/null)
    case "$reach" in
      OK*|ERR*) pass "$DASH_USER can reach the socket (connect ok via group solari-ctl: $reach)";;
      *)        bad  "$DASH_USER cannot reach the socket — check solari-ctl membership";;
    esac
  else
    # No socat: layer 3 (which requires socat) will already FAIL, so don't also
    # emit a misleading reachability verdict here.
    echo "  SKIP  socket connect probe needs socat; reachability is asserted in layer 3"
  fi
else
  bad "socket $SOCK not present (is the server running?)"
fi

# --- layer 3: peer-cred ACL over the wire ---
# Requires a line-oriented client. We use `socat`; if absent, skip layer 3 and
# say so (the file-perm + socket checks above already prove the uid split).
# DECISIVE: proving a privileged verb is REFUSED to the dashboard uid is the
# heart of the boundary. If we can't run it (no client, no socket), we can't
# assert it — FAIL, not SKIP, so the run never reports PASS untested (review F11).
echo "[3] peer-cred verb ACL"
if ! command -v socat >/dev/null 2>&1; then
  bad "socat not installed — cannot exercise the socket ACL (install socat and re-run)"
elif [ ! -S "$SOCK" ]; then
  bad "no socket at $SOCK to talk to — cannot verify the verb ACL"
else
  send() { printf '%s\n' "$1" | runAsDash socat -t2 - "UNIX-CONNECT:$SOCK" 2>/dev/null; }
  ping=$(send "PING")
  case "$ping" in
    OK*) pass "PING allowed for $DASH_USER (ordinary verb): $ping";;
    *)   bad "PING did not return OK for $DASH_USER: '$ping'";;
  esac
  # A privileged verb MUST be refused by the PEER-CLASS ACL — which runs BEFORE
  # any handler. The bridge returns exactly "ERR -40 privileged verb must be
  # queued via REQUEST_SUBMIT" (ERR_AUTH_ROLE, solariCtl.c) for the dashboard
  # peer. Accept ONLY that specific refusal: any OTHER ERR (bad node, DB error)
  # means the verb REACHED the handler — i.e. the peer ACL did NOT stop it, a
  # broken boundary masquerading as a pass. Use a nonexistent node id so that even
  # if enforcement were off, no real node is touched by this probe.
  priv=$(send "RETIRE node=2147483647 op=verify")
  case "$priv" in
    "ERR -40 "*|*"must be queued via REQUEST_SUBMIT"*)
          pass "RETIRE refused by peer-class ACL for $DASH_USER: $priv";;
    OK*)  bad "RETIRE was ALLOWED for $DASH_USER — boundary broken: $priv";;
    ERR*) bad "RETIRE reached the handler (got '$priv', not the -40 peer refusal) — peer ACL did NOT stop it";;
    *)    bad "RETIRE gave unexpected reply: '$priv'";;
  esac
  if [ "${1:-}" = "--enqueue" ]; then
    # The rewired privileged routes depend on REQUEST_SUBMIT, not just the read
    # path. REQUEST_GET only proves poll works; an ACL that permits GET but
    # rejects SUBMIT would leave every privileged route unusable yet pass. So
    # exercise the WRITE path. The queue accepts ONLY privileged verbs (ordinary
    # verbs get "ERR -1 only privileged verbs may be queued"), and SUBMIT merely
    # ENQUEUES — it does not execute; the target is validated later, at
    # consumption. So submit a privileged RETIRE against a nonexistent node: the
    # enqueue is accepted (proving the write path), and when the consumer replays
    # it the missing node makes it a harmless no-op failure. Leaves one benign
    # failed request row.
    sub=$(send "REQUEST_SUBMIT verb=RETIRE args=node%3D2147483647 op=verify")
    case "$sub" in
      OK*request=*) pass "REQUEST_SUBMIT accepted for $DASH_USER (enqueue write path works): $sub";;
      "ERR -40 "*)  bad "REQUEST_SUBMIT refused for $DASH_USER by peer ACL — the enqueue path itself is blocked, every privileged route is unusable: $sub";;
      ERR*)         bad "REQUEST_SUBMIT rejected for $DASH_USER: $sub";;
      *)            bad "REQUEST_SUBMIT gave unexpected reply: '$sub'";;
    esac
    # And the poll/read path a caller uses to reconstruct the result.
    got=$(send "REQUEST_GET request=999999999")
    case "$got" in
      OK*|ERR*) pass "REQUEST_GET reachable for $DASH_USER: $got";;
      *)        bad "REQUEST_GET unreachable: '$got'";;
    esac
  fi
fi

echo
if [ "$fail" -eq 0 ]; then
  echo "RESULT: PASS — boundary enforced."
else
  echo "RESULT: FAIL — boundary NOT fully enforced (see FAIL lines above)."
fi
exit "$fail"
