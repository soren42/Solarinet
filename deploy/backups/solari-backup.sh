#!/usr/bin/env bash
set -Eeuo pipefail
umask 077

usage() {
    echo "usage: $0 [config-file]" >&2
}

ACTIVE_TMP=
cleanup_tmp() {
    if [ -n "${ACTIVE_TMP:-}" ] && [ -f "$ACTIVE_TMP" ]; then
        rm -f -- "$ACTIVE_TMP"
    fi
}

on_error() {
    rc=$?
    cleanup_tmp
    echo "solari-backup: failed line ${BASH_LINENO[0]} exit=${rc}" >&2
    exit "$rc"
}
trap cleanup_tmp EXIT
trap on_error ERR

resolve_config() {
    if [ "$#" -gt 1 ]; then
        usage
        exit 64
    fi
    if [ "$#" -eq 1 ]; then
        printf '%s\n' "$1"
    elif [ -n "${SOLARI_BACKUP_CONF:-}" ]; then
        printf '%s\n' "$SOLARI_BACKUP_CONF"
    elif [ -f ./backup.conf ]; then
        printf '%s\n' "./backup.conf"
    else
        printf '%s\n' "/etc/solari-backup.conf"
    fi
}

resolve_bin() {
    bin=$1
    if [ -z "$bin" ]; then
        return 1
    fi
    case "$bin" in
        */*) [ -x "$bin" ] && printf '%s\n' "$bin" ;;
        *) command -v "$bin" 2>/dev/null ;;
    esac
}

bytes_of() {
    wc -c < "$1" | tr -d ' '
}

validate_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        *) return 0 ;;
    esac
}

require_secret_file_safe() {
    path=$1
    label=$2
    if [ ! -f "$path" ]; then
        echo "solari-backup: $label is not a regular file: $path" >&2
        exit 66
    fi

    stat_out=$(stat -c '%u %a' "$path" 2>/dev/null) || {
        echo "solari-backup: cannot stat $label: $path" >&2
        exit 66
    }
    owner_uid=${stat_out%% *}
    mode=${stat_out#* }
    if [ "$owner_uid" != "0" ]; then
        echo "solari-backup: $label must be owned by root: $path" >&2
        exit 66
    fi
    case "$mode" in
        400|600) ;;
        *)
            echo "solari-backup: $label must have mode 0600 or 0400: $path (mode $mode)" >&2
            exit 66
            ;;
    esac
}

validate_artifact_prefix() {
    case "$1" in
        ''|*/*|*'..'*|*'*'*|*'?'*|*'['*|*']'*)
            return 1
            ;;
        *)
            return 0
            ;;
    esac
}

shell_quote_args() {
    quoted=
    for arg in "$@"; do
        printf -v q '%q' "$arg"
        quoted="${quoted}${quoted:+ }${q}"
    done
    printf '%s\n' "$quoted"
}

run_as_user() {
    user=$1
    shift
    if command -v runuser >/dev/null 2>&1; then
        runuser -u "$user" -- "$@"
    else
        su - "$user" -s /bin/sh -c "$(shell_quote_args "$@")"
    fi
}

CONFIG_FILE=$(resolve_config "$@")
lock_dir=/run/lock
if [ ! -d "$lock_dir" ] || [ ! -w "$lock_dir" ]; then
    lock_dir=${TMPDIR:-/tmp}
fi
lock_instance=$(basename -- "$CONFIG_FILE")
lock_instance=${lock_instance%.conf}
if [ -z "$lock_instance" ]; then
    lock_instance=default
fi
lockfile="${lock_dir}/solari-backup-${lock_instance}.lock"
if ! exec 9>"$lockfile"; then
    echo "solari-backup: cannot open lock file: $lockfile" >&2
    exit 75
fi
if ! flock -n 9; then
    echo "solari-backup: another run is already in progress for $CONFIG_FILE (lock: $lockfile)" >&2
    exit 75
fi

if [ ! -r "$CONFIG_FILE" ]; then
    echo "solari-backup: config not readable: $CONFIG_FILE" >&2
    exit 66
fi
require_secret_file_safe "$CONFIG_FILE" "config"

# Defaults; the config file may override them.
DB_NAMES=
TARGET_DIR=
RETENTION_DAYS=14
MYSQL_HOST=
MYSQL_PORT=
MYSQL_USER=
MYSQL_PWD=${MYSQL_PWD:-}
MYSQL_PASSWORD_FILE=
MYSQL_ENV_FILE=
MYSQL_DEFAULTS_EXTRA_FILE=
MYSQLDUMP_BIN=mysqldump
FORGEJO_BIN=forgejo
FORGEJO_CONFIG=
FORGEJO_WORK_PATH=
FORGEJO_RUN_AS=

# shellcheck source=/dev/null
. "$CONFIG_FILE"

if [ -n "${MYSQL_ENV_FILE:-}" ]; then
    if [ ! -r "$MYSQL_ENV_FILE" ]; then
        echo "solari-backup: MYSQL_ENV_FILE not readable: $MYSQL_ENV_FILE" >&2
        exit 66
    fi
    require_secret_file_safe "$MYSQL_ENV_FILE" "MYSQL_ENV_FILE"
    set -a
    # shellcheck source=/dev/null
    . "$MYSQL_ENV_FILE"
    set +a
fi

if [ -n "${MYSQL_PASSWORD_FILE:-}" ]; then
    if [ ! -r "$MYSQL_PASSWORD_FILE" ]; then
        echo "solari-backup: MYSQL_PASSWORD_FILE not readable: $MYSQL_PASSWORD_FILE" >&2
        exit 66
    fi
    require_secret_file_safe "$MYSQL_PASSWORD_FILE" "MYSQL_PASSWORD_FILE"
    IFS= read -r MYSQL_PWD < "$MYSQL_PASSWORD_FILE"
fi
export MYSQL_PWD

if [ -z "${DB_NAMES:-}" ]; then
    echo "solari-backup: DB_NAMES is required" >&2
    exit 64
fi
if [ -z "${TARGET_DIR:-}" ] || [ "$TARGET_DIR" = "/" ]; then
    echo "solari-backup: TARGET_DIR is required and must not be /" >&2
    exit 64
fi
if ! validate_uint "$RETENTION_DAYS"; then
    echo "solari-backup: RETENTION_DAYS must be a non-negative integer" >&2
    exit 64
fi
if [ ! -d "$TARGET_DIR" ] || [ ! -w "$TARGET_DIR" ]; then
    echo "solari-backup: target dir missing or unwritable: $TARGET_DIR" >&2
    exit 73
fi
TARGET_DIR=$(cd "$TARGET_DIR" && pwd -P)
if [ -z "$TARGET_DIR" ] || [ "$TARGET_DIR" = "/" ]; then
    echo "solari-backup: resolved TARGET_DIR is unsafe: $TARGET_DIR" >&2
    exit 64
fi
if [ -n "${MYSQL_DEFAULTS_EXTRA_FILE:-}" ] && [ ! -r "$MYSQL_DEFAULTS_EXTRA_FILE" ]; then
    echo "solari-backup: MYSQL_DEFAULTS_EXTRA_FILE not readable: $MYSQL_DEFAULTS_EXTRA_FILE" >&2
    exit 66
fi
if [ -n "${MYSQL_DEFAULTS_EXTRA_FILE:-}" ]; then
    require_secret_file_safe "$MYSQL_DEFAULTS_EXTRA_FILE" "MYSQL_DEFAULTS_EXTRA_FILE"
fi

MYSQLDUMP_PATH=$(resolve_bin "$MYSQLDUMP_BIN" || true)
if [ -z "$MYSQLDUMP_PATH" ]; then
    echo "solari-backup: mysqldump not found: $MYSQLDUMP_BIN" >&2
    exit 69
fi
DATE=$(date +%F)
results=
DB_LIST=()

for db in $DB_NAMES; do
    if ! validate_artifact_prefix "$db"; then
        echo "solari-backup: unsafe database name for filename: $db" >&2
        exit 64
    fi
    DB_LIST+=("$db")
done

for db in "${DB_LIST[@]}"; do
    dest="${TARGET_DIR}/${db}-${DATE}.sql.gz"
    tmp="${dest}.tmp.$$"
    ACTIVE_TMP=$tmp
    dump_cmd=("$MYSQLDUMP_PATH")
    if [ -n "${MYSQL_DEFAULTS_EXTRA_FILE:-}" ]; then
        dump_cmd+=("--defaults-extra-file=$MYSQL_DEFAULTS_EXTRA_FILE")
    fi
    dump_cmd+=(--single-transaction --quick --routines --events)
    [ -n "${MYSQL_HOST:-}" ] && dump_cmd+=("--host=$MYSQL_HOST")
    [ -n "${MYSQL_PORT:-}" ] && dump_cmd+=("--port=$MYSQL_PORT")
    [ -n "${MYSQL_USER:-}" ] && dump_cmd+=("--user=$MYSQL_USER")
    dump_cmd+=("$db")

    if "${dump_cmd[@]}" | gzip -c > "$tmp"; then
        if [ ! -s "$tmp" ]; then
            rm -f -- "$tmp"
            ACTIVE_TMP=
            echo "solari-backup: dump produced empty file for $db" >&2
            exit 74
        fi
        mv -f -- "$tmp" "$dest"
        chmod 600 "$dest"
        ACTIVE_TMP=
    else
        rm -f -- "$tmp"
        ACTIVE_TMP=
        echo "solari-backup: mysqldump failed for $db" >&2
        exit 74
    fi
    results="${results}${results:+ }${db}=$(bytes_of "$dest")B"
done

forgejo_result=
prune_forgejo=0
if [ -n "${FORGEJO_CONFIG:-}" ] && [ -f "$FORGEJO_CONFIG" ]; then
    prune_forgejo=1
    FORGEJO_PATH=$(resolve_bin "${FORGEJO_BIN:-forgejo}" || true)
    if [ -n "$FORGEJO_PATH" ]; then
        dest="${TARGET_DIR}/forgejo-${DATE}.zip"
        tmp="${dest}.tmp.$$"
        ACTIVE_TMP=$tmp
        forgejo_cmd=("$FORGEJO_PATH" dump --config "$FORGEJO_CONFIG" --file "$tmp")
        [ -n "${FORGEJO_WORK_PATH:-}" ] && forgejo_cmd+=(--work-path "$FORGEJO_WORK_PATH")
        if [ -n "${FORGEJO_RUN_AS:-}" ]; then
            if ! run_as_user "$FORGEJO_RUN_AS" "${forgejo_cmd[@]}"; then
                rm -f -- "$tmp"
                ACTIVE_TMP=
                echo "solari-backup: Forgejo dump failed" >&2
                exit 74
            fi
        elif ! "${forgejo_cmd[@]}"; then
            rm -f -- "$tmp"
            ACTIVE_TMP=
            echo "solari-backup: Forgejo dump failed" >&2
            exit 74
        fi
        if [ ! -s "$tmp" ]; then
            rm -f -- "$tmp"
            ACTIVE_TMP=
            echo "solari-backup: Forgejo dump produced empty file" >&2
            exit 74
        fi
        mv -f -- "$tmp" "$dest"
        chmod 600 "$dest"
        ACTIVE_TMP=
        forgejo_result=" forgejo=$(bytes_of "$dest")B"
    fi
fi

pruned=0
for db in "${DB_LIST[@]}"; do
    while IFS= read -r old; do
        rm -f -- "$old"
        pruned=$((pruned + 1))
    done < <(
        find "$TARGET_DIR" -maxdepth 1 -type f \
            -name "${db}-[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9].sql.gz" \
            -mtime +"$RETENTION_DAYS" -print
    )
done
if [ "$prune_forgejo" -eq 1 ]; then
    while IFS= read -r old; do
        rm -f -- "$old"
        pruned=$((pruned + 1))
    done < <(
        find "$TARGET_DIR" -maxdepth 1 -type f \
            -name 'forgejo-[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9].zip' \
            -mtime +"$RETENTION_DAYS" -print
    )
fi

echo "solari-backup: ok date=${DATE} target=${TARGET_DIR} backups=${results}${forgejo_result} pruned=${pruned}"
