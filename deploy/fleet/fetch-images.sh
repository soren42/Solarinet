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
#   fetch-images.sh --dest <same> --prune <current>          (commit: drop old versions)
#   fetch-images.sh --dest <same> --rollback <previous|-> <new>  (undo a swap)
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
#      half-written tree. The previous version dir is KEPT: the caller records
#      the SoR row and regenerates boot.ipxe first, then either commits with
#      --prune <current> (removes every other version dir) or undoes with
#      --rollback <previous> <new> (re-points the symlink at the previous
#      version, or removes it when there was none, and deletes <new>). Both
#      refuse unless the symlink still points where the caller expects.
# Fetch prints `staged <dir>` and `previous <dir|->` before the swap, `swapped`
# after it, and `manifest-sha256 <hex>` (prov_images.sha256) on stdout; logs
# to stderr. Version dirs are named .<arch>.v.XXXXXXXX (.<arch>.legacy.<pid>
# for a pre-symlink real directory moved aside).
#
# Every operation first runs the image-store guard (prov-store-guard.sh) on
# benzene inside the same remote script: it fails closed (exit 1, "provisioning
# image store not mounted ...") unless PROV_STORE is a mounted PROV_STORE_FSTYPE
# filesystem and HTTP_ROOT/installers resolves into it, so a missing NAS mount
# never lets a multi-GB download land on benzene's local disk.
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
RE_VER='^\.(x86_64|arm64|armhf|riscv64|i386)\.(v\.[A-Za-z0-9]{8}|legacy\.[0-9]{1,10})$'

DEST="" ; RARGS=() ; NAMES=" " ; NFILES=0 ; OP=fetch ; OPARGS=()
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
    --prune)
      [ $# -ge 2 ] || usage
      [[ "$2" =~ $RE_VER ]] || bad "invalid version dir: $2"
      OP=prune; OPARGS=("$2"); shift 2 ;;
    --rollback)
      [ $# -ge 3 ] || usage
      [ "$2" = "-" ] || [[ "$2" =~ $RE_VER ]] || bad "invalid previous version dir: $2"
      [[ "$3" =~ $RE_VER ]] || bad "invalid version dir: $3"
      [ "$2" != "$3" ] || bad "--rollback previous and new are the same"
      OP=rollback; OPARGS=("$2" "$3"); shift 3 ;;
    -h|--help) usage ;;
    *) bad "unknown arg: $1" ;;
  esac
done
[ -n "$DEST" ] || usage
if [ "$OP" = fetch ]; then
  [ "$NFILES" -ge 1 ] || usage
else
  [ "${#RARGS[@]}" -eq 0 ] || bad "--$OP takes no --file/--extract/--meta"
  for v in "${OPARGS[@]}"; do
    [ "$v" = "-" ] || [ "${v#."${DEST##*/}".}" != "$v" ] || bad "$v is not a version dir of $DEST"
  done
fi
[[ "$HTTP_ROOT" =~ $RE_ROOT ]] && [[ "/$HTTP_ROOT/" != */../* ]] || bad "unsafe HTTP_ROOT: $HTTP_ROOT"
( fleet_store_knobs ) || exit 2

PROTOS="=https,http"; [ "$LOCAL" = 1 ] && PROTOS="=https,http,file"

# ---- the fixed script that runs on benzene ------------------------------------
# The guard library is prepended, so the store check and the work run in one
# ssh session (no window between "checked" and "writing").
# argv: <http_root> <store> <store fstype> <curl protos> <dest> <op> then
#   fetch:    records F name url sha | X tb member out | M key val
#   prune:    <current>
#   rollback: <previous|-> <new>
# shellcheck disable=SC2016
REMOTE_BODY='set -euo pipefail
umask 022
log() { printf "[fetch] %s\n" "$*" >&2; }
err() { printf "[fetch][error] %s\n" "$*" >&2; exit 1; }
root=$1 store=$2 fstype=$3 protos=$4 dest=$5 op=$6; shift 6
prov_store check "$store" "$fstype" "$root" "${dest%%/*}" || exit 1
final="$root/$dest"; parent="${final%/*}"; leaf="${final##*/}"
mkdir -p -- "$parent"
if command -v flock >/dev/null 2>&1; then
  exec 9>"$parent/.$leaf.lock"
  flock -w 900 9 || err "another refresh of $dest holds the lock"
fi
current() { if [ -L "$final" ]; then readlink -- "$final"; fi; }
if [ "$op" = prune ]; then
  keep=$1
  [ "$(current)" = "$keep" ] || err "$dest does not point at $keep; refusing to prune"
  shopt -s nullglob
  for d in "$parent/.$leaf.v."* "$parent/.$leaf.legacy."*; do
    [ "${d##*/}" = "$keep" ] && continue
    rm -rf -- "$d"; log "pruned ${d##*/}"
  done
  printf "pruned %s\n" "$keep"
  exit 0
fi
if [ "$op" = rollback ]; then
  prev=$1 new=$2
  [ "$(current)" = "$new" ] || err "$dest does not point at $new; refusing to roll back"
  if [ "$prev" = "-" ]; then
    rm -f -- "$final"; log "removed $dest (there was no previous version)"
  else
    [ -d "$parent/$prev" ] && [ ! -L "$parent/$prev" ] || err "previous version $prev is gone"
    ln -s -- "$prev" "$parent/.$leaf.link.$$"
    mv -T -- "$parent/.$leaf.link.$$" "$final"
    log "re-pointed $dest -> $prev"
  fi
  rm -rf -- "${parent:?}/$new"
  printf "rolled-back %s\n" "$prev"
  exit 0
fi
[ "$op" = fetch ] || err "bad op $op"
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
case "$old" in
  "") ;;
  */*) err "$dest points at $old, not a version dir here; fix it by hand" ;;
  ".$leaf.v."*|".$leaf.legacy."*) [ -d "$parent/$old" ] || old="" ;;
  *) err "$dest points at $old, not a version dir here; fix it by hand" ;;
esac
printf "staged %s\nprevious %s\n" "${stage##*/}" "${old:--}"
ln -s -- "${stage##*/}" "$parent/.$leaf.link.$$"
mv -T -- "$parent/.$leaf.link.$$" "$final"
trap - EXIT
printf "swapped\n"
log "swapped $dest -> ${stage##*/} (manifest sha256 $msha); previous ${old:--} kept until --prune"
printf "manifest-sha256 %s\n" "$msha"'
REMOTE_SH="$(cat "${FLEET_DIR}/prov-store-guard.sh")"$'\n'"$REMOTE_BODY"

# POSIX single-quoting for the remote login shell: every value becomes one literal word.
sq() { printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"; }

ARGS=("$HTTP_ROOT" "$PROV_STORE" "$PROV_STORE_FSTYPE" "$PROTOS" "$DEST" "$OP" "${RARGS[@]+"${RARGS[@]}"}" "${OPARGS[@]+"${OPARGS[@]}"}")
if [ "$OP" = fetch ]; then flog "fetching ${DEST} ($NFILES file(s)) into ${HTTP_ROOT}" >&2
else flog "${OP} ${DEST} ${OPARGS[*]}" >&2; fi
if [ "$LOCAL" = 1 ]; then
  bash -c "$REMOTE_SH" fetch-images "${ARGS[@]}"
else
  cmd="bash -c $(sq "$REMOTE_SH") fetch-images"
  for a in "${ARGS[@]}"; do cmd+=" $(sq "$a")"; done
  bssh "$cmd"
fi
