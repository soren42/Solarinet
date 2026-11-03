#!/usr/bin/env bash
# Render a self-contained manual HTML to a US Letter PDF beside it.
# Usage: tools/docs/render-pdf.sh docs/manuals/Name.html [more.html ...]
# Page size and margins come from the document's own @page rule.
set -euo pipefail
CHROME="${SOLARI_DOCS_CHROME:-$HOME/.cache/ms-playwright/chromium_headless_shell-1248/chrome-headless-shell-linux64/chrome-headless-shell}"
[[ -x "$CHROME" ]] || { echo "render-pdf: headless Chromium not found; set SOLARI_DOCS_CHROME" >&2; exit 2; }
for html in "$@"; do
  src="$(realpath "$html")"; out="${src%.html}.pdf"
  # --no-sandbox: local, repo-authored files only; this host has no usable userns sandbox.
  "$CHROME" --no-sandbox --no-pdf-header-footer --virtual-time-budget=5000 \
    --print-to-pdf="$out" "file://$src" >/dev/null 2>&1
  bad=$(pdfinfo -f 1 -l 9999 "$out" | /usr/bin/grep -c '^Page .* size:' || true)
  odd=$(pdfinfo -f 1 -l 9999 "$out" | /usr/bin/grep '^Page .* size:' | /usr/bin/grep -vc '612 x 792' || true)
  pages=$(pdfinfo "$out" | awk '/^Pages:/{print $2}')
  [[ "$odd" == 0 ]] || { echo "render-pdf: $out has $odd non-Letter pages" >&2; exit 1; }
  echo "rendered $out ($pages pages, all US Letter; checked $bad)"
done
