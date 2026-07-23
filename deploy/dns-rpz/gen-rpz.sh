#!/bin/sh
# gen-rpz.sh — build a BIND RPZ ad-block zone from StevenBlack's hosts list.
# Replaces the Pi-holes' filtering. Runs on xenon (RPZ primary); steel AXFRs it.
# Usage: gen-rpz.sh /etc/bind/zones/db.rpz.akoria
set -eu
OUT="${1:-/etc/bind/zones/db.rpz.akoria}"
SRC="https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"
TMP="$(mktemp)"; TMPZ="$(mktemp)"
trap 'rm -f "$TMP" "$TMPZ"' EXIT
curl -fsS --max-time 60 "$SRC" -o "$TMP"
# Monotonic serial: YYYYMMDDnn, bump if today's already used
DATE="$(date +%Y%m%d)00"
PREV="$(awk '/; serial/{print $1; exit}' "$OUT" 2>/dev/null || echo 0)"
if [ "$PREV" -ge "$DATE" ] 2>/dev/null; then SERIAL=$((PREV+1)); else SERIAL=$((DATE+1)); fi
{
  printf '$TTL 60\n@ IN SOA localhost. root.localhost. ( %s 3600 600 604800 60 )\n  IN NS localhost.\n' "$SERIAL"
  # 0.0.0.0 <domain>  ->  <domain> CNAME .   (RPZ NXDOMAIN action)
  awk '/^0\.0\.0\.0[ \t]/ && $2 != "0.0.0.0" {print $2" CNAME ."}' "$TMP" | sort -u
} > "$TMPZ"
COUNT=$(grep -c 'CNAME .' "$TMPZ" || true)
mv "$TMPZ" "$OUT"; chmod 644 "$OUT"
echo "rpz: $COUNT blocked domains, serial $SERIAL -> $OUT"
