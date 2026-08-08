#!/usr/bin/env bash
# chemistry-fix-tonight.sh — STAGED runbook for the UDR7 Wi-Fi-freeze fix.
# Run AFTER Jason has moved the master-bedroom AP into the craft room and it
# shows online. Every step verifies + can roll back. Reversible throughout.
#
#   ./chemistry-fix-tonight.sh precheck    # coverage snapshot (read-only)
#   ./chemistry-fix-tonight.sh firmware    # check + apply UniFi OS update IF available
#   ./chemistry-fix-tonight.sh radios-off  # snapshot + cut gateway wifi0/1/2
#   ./chemistry-fix-tonight.sh verify      # post-cut: clients migrated, internet up
#   ./chemistry-fix-tonight.sh rollback    # re-enable radios from snapshot
set -uo pipefail
cd "$(dirname "$0")"
set -a; source run/unifi.env 2>/dev/null; set +a
export KEY="${UNIFI_API_KEY:-${UI_API_KEY:-${X_API_KEY:-}}}"
BASE="${UNIFI_URL:-https://10.0.0.1}"
SITE=default
export DEV_MAC=1c:0b:8b:e4:73:1a           # chemistry (UDMA67A)
export SNAP=/home/jason/chemistry-watch/radio_table.snapshot.json
API="$BASE/proxy/network/api/s/$SITE"
TMP=/tmp/chem_devices.json
fetch(){ curl -sk -H "X-API-KEY: $KEY" "$API/stat/device" -o "$TMP" 2>/dev/null; }

precheck(){
  fetch
  echo "=== AP fleet online + client distribution ==="
  python3 - "$TMP" <<'PY'
import sys,json
d=json.load(open(sys.argv[1]))
for x in d.get("data",[]):
    if x.get("type") in ("uap","udm") or x.get("model")=="UDMA67A":
        c = x.get("user-num_sta") or x.get("num_sta")
        print(f"  {x.get('model'):8} state={x.get('state')} clients={c}  {x.get('name')}")
PY
  echo "--- proceed only when flask, beaker, AND the relocated craft-room AP are state=1 ---"
}

firmware(){
  # CONFIRMED CURRENT (UniFi OS 5.1.19 is latest per the console) — no update
  # exists, step is a no-op. Left here only to re-verify nothing changed.
  fetch
  echo "=== firmware (expected: no update; already current) ==="
  python3 - "$TMP" <<'PY'
import sys,json
d=json.load(open(sys.argv[1]))
for x in d.get("data",[]):
    if x.get("model")=="UDMA67A":
        up=x.get("upgradable")
        print(f"  version={x.get('version')} upgradable={up}")
        print("  -> SKIP (confirmed current)" if not up else "  -> update appeared; review before applying")
PY
}

# Night 2: after adopting the U7 Pro XGS on laboratory SFP+17, confirm it links at 10G.
xgs_verify(){
  fetch
  echo "=== U7 Pro XGS uplink speed (want 10000) ==="
  python3 - "$TMP" <<'PY'
import sys,json
d=json.load(open(sys.argv[1]))
for x in d.get("data",[]):
    if "XGS" in (x.get("model","")+ (x.get("name") or "")) or x.get("model")=="U7PROXGS":
        up=x.get("uplink") or {}
        print(f"  {x.get('name')} ({x.get('model')}) uplink speed={up.get('speed')} port={up.get('uplink_remote_port')} state={x.get('state')}")
    if x.get("model")=="USW-Pro-Max-16" or x.get("model")=="USPM16":
        for p in x.get("port_table",[]):
            if p.get("port_idx")==17 and p.get("media")=="SFP+":
                print(f"  laboratory SFP+17: speed={p.get('speed')} up={p.get('up')}")
PY
  echo "  (10000 = success; 1000/2500 = injector or transceiver bottleneck — recheck the chain)"
}

radios_off(){
  fetch
  echo "=== snapshot radio_table -> $SNAP ==="
  python3 - "$TMP" <<'PY'
import sys,json,os
d=json.load(open(sys.argv[1]))
for x in d.get("data",[]):
    if x.get("model")=="UDMA67A":
        json.dump(x.get("radio_table",[]), open(os.environ["SNAP"],"w"), indent=2)
        print("  snapshotted", len(x.get("radio_table",[])), "radios for rollback")
PY
  echo "=== cut wifi0/wifi1/wifi2 ==="
  echo "  FINALIZE LIVE: UniFi 10.x per-radio disable. Two proven paths, pick what the"
  echo "  live device accepts (both reversible):"
  echo "   A) App: Devices > chemistry > Radios > toggle each band Off (10s, guaranteed)."
  echo "   B) API PUT rest/device/<id> with radio_table rebuilt from the snapshot, each"
  echo "      radio marked disabled — validate response, then run 'verify'."
  echo "  Do NOT leave this half-done: if clients don't re-home in 60s, run 'rollback'."
}

verify(){
  fetch
  echo "=== gateway radios should carry 0 stations; APs absorb them ==="
  python3 - "$TMP" <<'PY'
import sys,json
d=json.load(open(sys.argv[1]))
for x in d.get("data",[]):
    if x.get("model")=="UDMA67A":
        for r in x.get("radio_table_stats",[]):
            print(f"  chemistry {r.get('name')}: num_sta={r.get('num_sta')} state={r.get('state')}")
    if x.get("type")=="uap":
        print(f"  AP {x.get('name')}: clients={x.get('user-num_sta')}")
PY
  echo "=== WAN path up? ==="
  ssh -o BatchMode=yes -o ConnectTimeout=6 root@10.0.0.1 'ping -c1 -W2 1.1.1.1 >/dev/null 2>&1 && echo "  WAN OK" || echo "  WAN DOWN"' 2>/dev/null
  echo "=== chemistry-watch capturing? ==="; systemctl is-active chemistry-watch
}

rollback(){
  if [ -s "$SNAP" ]; then
    echo "  restore radios: PUT rest/device/<id> with radio_table from $SNAP (run interactively), or app toggle back On"
  else
    echo "  NO snapshot at $SNAP — re-enable via app Devices>chemistry>Radios"
  fi
}

case "${1:-precheck}" in
  precheck) precheck ;; firmware) firmware ;; radios-off) radios_off ;;
  verify) verify ;; rollback) rollback ;; xgs-verify) xgs_verify ;;
  *) echo "usage: $0 {precheck|firmware|radios-off|verify|rollback|xgs-verify}"; exit 2 ;;
esac
