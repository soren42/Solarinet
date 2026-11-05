#!/usr/bin/env bash
# Publish the SolariNet manuals and the docs portal into xenon's docroot.
# Usage: sudo deploy/docs/install-docs.sh [DOCROOT]   (default /var/www/html)
# Run from a clean checkout; the published revision is that checkout's HEAD.
set -euo pipefail
cd "$(dirname "$0")/../.."
DOCROOT="${1:-/var/www/html}"
DEST="$DOCROOT/docs"
MANUALS=(SolariNet_Quick_Start_Guide SolariNet_Panel_Screen_Guide SolariNet_User_Manual SolariNet_Developer_Manual SolariNet_API_Expansion_Manual)

[[ -d "$DOCROOT" ]] || { echo "install-docs: no docroot $DOCROOT" >&2; exit 2; }
for m in "${MANUALS[@]}"; do
  for ext in html pdf; do
    [[ -f "docs/manuals/$m.$ext" ]] || { echo "install-docs: missing docs/manuals/$m.$ext" >&2; exit 1; }
  done
  odd=$(pdfinfo -f 1 -l 9999 "docs/manuals/$m.pdf" | /usr/bin/grep '^Page .* size:' | /usr/bin/grep -vc '612 x 792' || true)
  [[ "$odd" == 0 ]] || { echo "install-docs: $m.pdf has non-Letter pages" >&2; exit 1; }
done
# Run as root on a user-owned clone: trust this checkout only.
g() { git -c safe.directory="$PWD" "$@"; }
rev="$(g rev-parse --short HEAD)"
[[ -z "$(g status --porcelain -- docs/manuals deploy/docs)" ]] || { echo "install-docs: docs tree is dirty" >&2; exit 1; }

stage="$(mktemp -d "$DOCROOT/.docs-stage.XXXXXX")"
trap 'rm -rf "$stage"' EXIT
install -m 0644 deploy/docs/index.html "$stage/index.html"
{
  printf '{"revision": "%s", "published": "%s", "docs": {' "$rev" "$(date -u +%Y-%m-%d)"
  sep=""
  for m in "${MANUALS[@]}"; do
    install -m 0644 "docs/manuals/$m.html" "docs/manuals/$m.pdf" "$stage/"
    pages=$(pdfinfo "docs/manuals/$m.pdf" | awk '/^Pages:/{print $2}')
    bytes=$(stat -c %s "docs/manuals/$m.pdf")
    printf '%s"%s": {"pages": %s, "bytes": %s}' "$sep" "$m" "$pages" "$bytes"; sep=", "
  done
  printf '}}\n'
} > "$stage/manifest.json"
chmod 0644 "$stage/manifest.json"
chmod 0755 "$stage"
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$stage/manifest.json"

# Swap the whole directory so readers never see a half-published set.
if [[ -d "$DEST" ]]; then
  old="$DEST.old.$$"
  mv "$DEST" "$old"
  mv "$stage" "$DEST" || { mv "$old" "$DEST"; echo "install-docs: swap failed; previous docs restored" >&2; exit 1; }
  rm -rf "$old"
else
  mv "$stage" "$DEST"
fi
trap - EXIT
echo "install-docs: published rev $rev to $DEST (${#MANUALS[@]} manuals)"
