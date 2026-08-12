#!/bin/sh
# smoke.sh — end-to-end panelProv <-> fakePanel exercise over a pty.
#
# Generates THROWAWAY credential blobs at runtime (random bytes standing in
# for DER — the fake panel installs an accept-all crypto seam, since the
# handler itself is fail-closed and would refuse a NULL one), provisions,
# then wipes. Nothing here is a real secret and nothing is committed to git
# (workdir is mktemp).
set -eu
cd "$(dirname "$0")"

make -s

work=$(mktemp -d)
trap 'rm -rf "$work"; [ -n "${fakePid:-}" ] && kill "$fakePid" 2>/dev/null || true' EXIT

head -c 21 /dev/urandom | base64 | head -c 16 > "$work/psk.txt"
head -c 600 /dev/urandom > "$work/ca.der"
head -c 900 /dev/urandom > "$work/client.der"
head -c 300 /dev/urandom > "$work/client.key.der"
# panelProv refuses group/other-readable secret files.
chmod 600 "$work/psk.txt" "$work/client.key.der"
cat > "$work/prov.conf" <<EOF
ssid=SolariNet-SmokeTest
pskFile=$work/psk.txt
serverHost=xenon.akoria.net
serverPort=8443
caCert=$work/ca.der
clientCert=$work/client.der
clientKey=$work/client.key.der
ntpHost=chlorine.akoria.net
EOF

./fakePanel > "$work/pty" 2> "$work/fake.log" &
fakePid=$!
# Wait for the pty path (line 1 of fakePanel's stdout).
for _ in $(seq 50); do
  [ -s "$work/pty" ] && break
  sleep 0.1
done
slave=$(head -n1 "$work/pty")
[ -n "$slave" ] || { echo "smoke: fakePanel produced no pty"; exit 1; }

./panelProv --dev "$slave" --conf "$work/prov.conf"
./panelProv --dev "$slave" --wipe

wait "$fakePid"
fakeStatus=$?
fakePid=
cat "$work/fake.log"
[ "$fakeStatus" -eq 0 ] || { echo "smoke: fakePanel end-state check failed"; exit 1; }
echo "smoke: provision + wipe round-trip OK"
