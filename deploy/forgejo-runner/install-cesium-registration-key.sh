#!/bin/bash
set -euo pipefail

if (( EUID != 0 )); then
  echo 'run as root on Cesium' >&2
  exit 2
fi
for prerequisite in /usr/bin/cat /usr/bin/timeout; do
  [[ -x "$prerequisite" ]] || {
    echo "missing Cesium prerequisite: $prerequisite" >&2
    exit 1
  }
done

source_dir="$(cd "$(dirname "$0")" && pwd)"
read -r key_type key_data key_comment || {
  echo 'read one SSH public key from stdin' >&2
  exit 2
}
[[ "$key_type" == ssh-ed25519 && "$key_data" =~ ^[A-Za-z0-9+/=]+$ ]] || {
  echo 'expected one ed25519 public key' >&2
  exit 2
}

home=/home/jason
ssh_dir="$home/.ssh"
authorized_keys="$ssh_dir/authorized_keys"
benzene_source_ip=10.5.2.50
if ! /usr/bin/sudo -u jason /usr/bin/sudo -n -u forgejo /bin/bash -c true; then
  echo 'Jason must be able to run commands as forgejo with sudo -n on Cesium' >&2
  exit 1
fi
install -d -o jason -g jason -m 0700 "$ssh_dir"
install -o root -g root -m 0755 "$source_dir/register-ephemeral.sh" \
  /usr/local/sbin/solarinet-ci-register-ephemeral

if grep -Fq -- "$key_data" "$authorized_keys" 2>/dev/null; then
  echo 'this key material already appears in authorized_keys; inspect it before changing access' >&2
  exit 1
fi

line="from=\"$benzene_source_ip\",restrict,command=\"/usr/local/sbin/solarinet-ci-register-ephemeral\" $key_type $key_data ${key_comment:-solarinet-ci-register}"
temporary="$(mktemp "$ssh_dir/authorized_keys.XXXXXX")"
trap 'rm -f "$temporary"' EXIT
if [[ -f "$authorized_keys" ]]; then
  cat "$authorized_keys" >"$temporary"
fi
printf '%s\n' "$line" >>"$temporary"
chown jason:jason "$temporary"
chmod 0600 "$temporary"
mv -f "$temporary" "$authorized_keys"
trap - EXIT
echo 'Installed a restricted repository-runner registration key.'
