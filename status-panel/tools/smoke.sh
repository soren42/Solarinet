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

# Negative: duplicate conf keys are refused before any wire traffic.
cp "$work/prov.conf" "$work/dup.conf"
echo "ssid=SecondSsid" >> "$work/dup.conf"
if ./panelProv --dev "$slave" --conf "$work/dup.conf" 2>/dev/null; then
  echo "smoke: duplicate-key conf was NOT refused"; exit 1
fi

./panelProv --dev "$slave" --conf "$work/prov.conf" | tee "$work/prov.out"
# CONTRACT-SW §8: every item prints its PROVACK; "ok" throughout is the
# operator's success signal (runbook Stage 3 relies on this exact output).
staged=$(grep -c "bytes staged" "$work/prov.out" || true)
acked=$(grep -c "bytes staged, PROVACK ok" "$work/prov.out" || true)
[ "$staged" -gt 0 ] && [ "$staged" -eq "$acked" ] || {
  echo "smoke: per-item PROVACK lines missing ($acked/$staged)"; exit 1; }
grep -q "COMMIT PROVACK ok" "$work/prov.out" || {
  echo "smoke: COMMIT PROVACK line missing"; exit 1; }
./panelProv --dev "$slave" --wipe

wait "$fakePid"
fakeStatus=$?
fakePid=
cat "$work/fake.log"
[ "$fakeStatus" -eq 0 ] || { echo "smoke: fakePanel end-state check failed"; exit 1; }
echo "smoke: provision + wipe round-trip OK"

# Fault-injection pass (final review MUST-8): a dropped PROVACK and a forged
# watermark desync in one provisioning run. The tool must ride out the drop
# via timeout+retransmit (handler re-acks idempotently) and recover from the
# genuine OFFSET_MISMATCH by resuming at the handler's watermark.
: > "$work/pty"
./fakePanel --drop-ack 3 --desync 5 > "$work/pty" 2> "$work/fake2.log" &
fakePid=$!
for _ in $(seq 50); do
  [ -s "$work/pty" ] && break
  sleep 0.1
done
slave=$(head -n1 "$work/pty")
[ -n "$slave" ] || { echo "smoke: fault-injection fakePanel produced no pty"; exit 1; }
./panelProv --dev "$slave" --conf "$work/prov.conf" \
  > "$work/prov2.out" 2> "$work/prov2.err"
grep -q "resuming at offset" "$work/prov2.err" || {
  echo "smoke: offset-mismatch resume path not exercised"; exit 1; }
grep -q "COMMIT PROVACK ok" "$work/prov2.out" || {
  echo "smoke: fault-injection provision did not complete"; exit 1; }
./panelProv --dev "$slave" --wipe
wait "$fakePid"
fakeStatus=$?
fakePid=
cat "$work/fake2.log"
[ "$fakeStatus" -eq 0 ] || { echo "smoke: fault-injection end-state check failed"; exit 1; }
echo "smoke: dropped-ack + desync recovery OK"
