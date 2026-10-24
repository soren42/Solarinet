#!/bin/bash
set -euo pipefail

# Intended as an OpenSSH forced command on Cesium. The key holder can only
# register/update this one ephemeral repository runner and receive its UUID.
secret="$(/usr/bin/timeout 10 /usr/bin/cat)"
[[ "$secret" =~ ^534f4c4152494349[0-9a-f]{24}$ ]] || {
  echo 'invalid runner secret' >&2
  exit 2
}

printf '%s' "$secret" | /usr/bin/sudo -n -u forgejo /bin/bash -c '
  set -euo pipefail
  secret_file="$(mktemp /var/tmp/solarinet-ci-secret.XXXXXX)"
  trap '\''rm -f "$secret_file"'\'' EXIT
  chmod 0600 "$secret_file"
  cat >"$secret_file"
  /usr/bin/forgejo \
    --config /etc/forgejo/conf/app.ini \
    --work-path /var/lib/forgejo \
    forgejo-cli actions register \
    --name solarinet-ci-ephemeral \
    --scope jason/Solarinet \
    --labels solarinet-ci-host \
    --ephemeral \
    --secret-file "$secret_file"
'
