#!/usr/bin/env bash
# =============================================================================
# fetch-images.sh - download + verify one netboot image into benzene's HTTP root.
#
# Called by deploy/fleet/image-refresh.py (the IMAGE_REFRESH verb), which reads
# the prov_images row, resolves the upstream checksums and passes EVERYTHING as
# arguments; this script has no SoR access and no built-in URL list.
#
#   fetch-images.sh --dest installers/<distro>/<release>/<arch>
#                   --file <name> <url> <sha256>          (one or more)
#                   [--extract <tarball> <member> <out>]  (from a fetched tarball)
#                   [--meta <key> <value>]                (solari-image.json fields)
#
# On benzene (or locally under SOLARI_PROV_LOCAL=1, the test seam) it:
#   1. downloads every --file into a fresh staging dir NEXT TO the image dir
#      (curl --fail --location, https/http only; file:// only under the seam);
#   2. checks each file's sha256 against the expected value — any mismatch or
#      download failure aborts and the current image dir is left untouched;
#   3. extracts --extract members (by exact name, via tar -O: no path writes)
#      and drops the source tarballs;
#   4. writes SHA256SUMS.solari (sha256sum format) over the served files and
#      solari-image.json (the --meta fields + manifest_sha256 + fetched_at);
#   5. atomically swaps the image dir: <arch> is a symlink to the staging dir,
#      replaced with rename(2) (`mv -T`), so a booting client never sees a
#      half-written tree; the previous version dir is then removed.
# Prints `manifest-sha256 <hex>` on stdout (prov_images.sha256); logs to stderr.
#
# Every argument is validated against a strict pattern before use, and the
# remote side is a FIXED script receiving the values as single-quoted
# positional arguments — no operator text is ever spliced into shell code.
# Exit: 0 ok, 1 fetch/verify failure, 2 usage error.
# =============================================================================
set -euo pipefail
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/fleet-lib.sh"

usage() { printf '%s\n' "usage: fetch-images.sh --dest installers/<distro>/<release>/<arch> --file <name> <url> <sha256> [--file ...] [--extract <tarball> <member> <out> ...] [--meta <key> <value> ...]" >&2; exit 2; }
bad() { printf '\033[1;31m[fleet][error]\033[0m %s\n' "$*" >&2; exit 2; }

LOCAL=0; [ "${SOLARI_PROV_LOCAL:-}" = "1" ] && LOCAL=1

RE_DEST='^installers/[a-z0-9][a-z0-9-]{0,31}/[A-Za-z0-9][A-Za-z0-9._-]{0,47}/(x86_64|arm64|armhf|riscv64|i386)$'
RE_NAME='^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$'
RE_SHA='^[0-9a-f]{64}$'
RE_URL='^https?://[A-Za-z0-9.-]+(:[0-9]{1,5})?/[A-Za-z0-9._~/?&=+,%-]*$'
RE_FILE_URL='^file:///[A-Za-z0-9._~/+%-]+$'
RE_MEMBER='^[A-Za-z0-9.][A-Za-z0-9._/-]{0,255}$'
RE_META_KEY='^(distro|release_name|release_label|arch|version|source_url)$'
RE_META_VAL='^([A-Za-z0-9][A-Za-z0-9 ._:/()+,~@%=-]{0,511})?$'
RE_ROOT='^/[A-Za-z0-9._/-]+$'

DEST="" ; RARGS=() ; NAMES=" " ; NFILES=0
while [ $# -gt 0 ]; do
  case "$1" in
    --dest)
      [ $# -ge 2 ] || usage
      [[ "$2" =~ $RE_DEST ]] || bad "invalid --dest: $2"
      DEST="$2"; shift 2 ;;
    --file)
      [ $# -ge 4 ] || usage
      name="$2" url="$3" sha="$4"
      [[ "$name" =~ $RE_NAME ]] || bad "invalid file name: $name"
      case "$name" in SHA256SUMS.solari|solari-image.json) bad "reserved file name: $name" ;; esac
      case "$NAMES" in *" $name "*) bad "duplicate file name: $name" ;; esac
      if [[ "$url" =~ $RE_URL ]]; then :
      elif [ "$LOCAL" = 1 ] && [[ "$url" =~ $RE_FILE_URL ]]; then :
      else bad "invalid or disallowed URL for $name: $url"; fi
      [[ "$sha" =~ $RE_SHA ]] || bad "invalid sha256 for $name"
      NAMES="${NAMES}${name} "; NFILES=$((NFILES + 1))
      RARGS+=(F "$name" "$url" "$sha"); shift 4 ;;
    --extract)
      [ $# -ge 4 ] || usage
      tb="$2" member="$3" out="$4"
      case "$NAMES" in *" $tb "*) ;; *) bad "--extract source $tb is not a --file given earlier" ;; esac
      [[ "$member" =~ $RE_MEMBER ]] || bad "invalid tar member: $member"
      [[ "/$member/" != */../* ]] || bad "tar member may not contain ..: $member"
      [[ "$out" =~ $RE_NAME ]] || bad "invalid output name: $out"
      case "$out" in SHA256SUMS.solari|solari-image.json) bad "reserved file name: $out" ;; esac
      case "$NAMES" in *" $out "*) bad "duplicate file name: $out" ;; esac
      NAMES="${NAMES}${out} "
      RARGS+=(X "$tb" "$member" "$out"); shift 4 ;;
    --meta)
      [ $# -ge 3 ] || usage
      [[ "$2" =~ $RE_META_KEY ]] || bad "unknown --meta key: $2"
      [[ "$3" =~ $RE_META_VAL ]] || bad "unsafe --meta value for $2"
      RARGS+=(M "$2" "$3"); shift 3 ;;
    -h|--help) usage ;;
    *) bad "unknown arg: $1" ;;
  esac
done
[ -n "$DEST" ] || usage
[ "$NFILES" -ge 1 ] || usage
[[ "$HTTP_ROOT" =~ $RE_ROOT ]] && [[ "/$HTTP_ROOT/" != */../* ]] || bad "unsafe HTTP_ROOT: $HTTP_ROOT"

PROTOS="=https,http"; [ "$LOCAL" = 1 ] && PROTOS="=https,http,file"

# ---- the fixed script that runs on benzene ------------------------------------
# argv: <http_root> <curl protos> <dest> then records: F name url sha | X tb member out | M key val
# shellcheck disable=SC2016
REMOTE_SH='set -euo pipefail
umask 022
log() { printf "[fetch] %s\n" "$*" >&2; }
err() { printf "[fetch][error] %s\n" "$*" >&2; exit 1; }
root=$1 protos=$2 dest=$3; shift 3
final="$root/$dest"; parent="${final%/*}"; leaf="${final##*/}"
mkdir -p -- "$parent"
if command -v flock >/dev/null 2>&1; then
  exec 9>"$parent/.$leaf.lock"
  flock -w 900 9 || err "another refresh of $dest holds the lock"
fi
stage="$(mktemp -d -- "$parent/.$leaf.v.XXXXXXXX")"
trap '\''rm -rf -- "$stage"'\'' EXIT
served=(); drop=(); meta=()
while [ $# -gt 0 ]; do
  case "$1" in
    F) name=$2 url=$3 sha=$4; shift 4
       log "downloading $name <- $url"
       curl --fail --location --silent --show-error --retry 3 --connect-timeout 30 \
            --max-time 7200 --proto "$protos" --proto-redir "$protos" \
            -o "$stage/$name" -- "$url" || err "download failed: $name ($url)"
       got="$(sha256sum -- "$stage/$name")"; got="${got%% *}"
       [ "$got" = "$sha" ] || err "sha256 MISMATCH for $name: expected $sha, got $got - refusing"
       log "verified $name sha256=$sha"
       served+=("$name") ;;
    X) tb=$2 member=$3 out=$4; shift 4
       m=""
       while IFS= read -r ent; do
         if [ "$ent" = "$member" ] || [ "$ent" = "./$member" ]; then m="$ent"; break; fi
       done < <(tar -tzf "$stage/$tb") || true
       [ -n "$m" ] || err "$tb has no member $member"
       tar -xzOf "$stage/$tb" -- "$m" > "$stage/$out" || err "cannot extract $member from $tb"
       [ -s "$stage/$out" ] || err "extracted $member from $tb is empty"
       log "extracted $member -> $out"
       served+=("$out"); drop+=("$tb") ;;
    M) meta+=("$2" "$3"); shift 3 ;;
    *) err "bad record $1" ;;
  esac
done
keep=()
for f in "${served[@]}"; do
  skip=0; for d in "${drop[@]+"${drop[@]}"}"; do [ "$f" = "$d" ] && skip=1; done
  if [ "$skip" = 1 ]; then rm -f -- "$stage/$f"; else keep+=("$f"); fi
done
[ "${#keep[@]}" -ge 1 ] || err "nothing to serve"
mapfile -t keep < <(printf "%s\n" "${keep[@]}" | LC_ALL=C sort)
( cd "$stage" && sha256sum -- "${keep[@]}" ) > "$stage/SHA256SUMS.solari"
msha="$(sha256sum -- "$stage/SHA256SUMS.solari")"; msha="${msha%% *}"
{
  printf "{"
  i=0
  while [ "$i" -lt "${#meta[@]}" ]; do
    printf "\"%s\": \"%s\", " "${meta[$i]}" "${meta[$((i + 1))]}"; i=$((i + 2))
  done
  printf "\"manifest_sha256\": \"%s\", \"fetched_at\": \"%s\"}\n" "$msha" "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
} > "$stage/solari-image.json"
chmod 0755 "$stage"; chmod 0644 "$stage"/*
old=""
if [ -L "$final" ]; then
  old="$(readlink -- "$final")"
elif [ -e "$final" ]; then
  legacy="$parent/.$leaf.legacy.$$"; mv -- "$final" "$legacy"; old="${legacy##*/}"
  log "moved legacy real directory aside: $legacy"
fi
ln -s -- "${stage##*/}" "$parent/.$leaf.link.$$"
mv -T -- "$parent/.$leaf.link.$$" "$final"
trap - EXIT
case "$old" in
  ".$leaf."*) case "$old" in */*) ;; *) rm -rf -- "${parent:?}/$old" ;; esac ;;
esac
log "swapped $dest -> ${stage##*/} (manifest sha256 $msha)"
printf "manifest-sha256 %s\n" "$msha"'

# POSIX single-quoting for the remote login shell: every value becomes one literal word.
sq() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

ARGS=("$HTTP_ROOT" "$PROTOS" "$DEST" "${RARGS[@]}")
flog "fetching ${DEST} ($NFILES file(s)) into ${HTTP_ROOT}"
if [ "$LOCAL" = 1 ]; then
  bash -c "$REMOTE_SH" fetch-images "${ARGS[@]}"
else
  cmd="bash -c $(sq "$REMOTE_SH") fetch-images"
  for a in "${ARGS[@]}"; do cmd+=" $(sq "$a")"; done
  bssh "$cmd"
fi
