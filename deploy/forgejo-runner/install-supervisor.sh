#!/bin/bash
set -euo pipefail

source_dir="$(cd "$(dirname "$0")" && pwd)"
for prerequisite in bash cat curl getent grep install ip nft openssl ps python3 qemu-img sha256sum ssh ssh-keygen stty systemctl timeout virsh virt-install xorriso; do
  command -v "$prerequisite" >/dev/null 2>&1 || {
    echo "missing Benzene prerequisite: $prerequisite" >&2
    exit 1
  }
done
id qemu >/dev/null 2>&1 || {
  echo 'expected libvirt QEMU account qemu on Benzene' >&2
  exit 1
}
install -d -o root -g root -m 0755 /usr/local/lib/solarinet-ci
install -o root -g root -m 0755 "$source_dir/supervisor.sh" /usr/local/lib/solarinet-ci/supervisor.sh
install -o root -g root -m 0755 "$source_dir/serial-capture.py" /usr/local/lib/solarinet-ci/serial-capture.py
install -o root -g root -m 0644 "$source_dir/guest-runner.sh" /usr/local/lib/solarinet-ci/guest-runner.sh
install -o root -g root -m 0644 "$source_dir/render-user-data.py" /usr/local/lib/solarinet-ci/render-user-data.py
install -o root -g root -m 0644 "$source_dir/solarinet-ci-guard.service" /etc/systemd/system/solarinet-ci-guard.service
install -o root -g root -m 0644 "$source_dir/solarinet-ci-network.service" /etc/systemd/system/solarinet-ci-network.service
install -o root -g root -m 0644 "$source_dir/solarinet-ci-supervisor.service" /etc/systemd/system/solarinet-ci-supervisor.service
install -D -o root -g root -m 0644 "$source_dir/solarinet-ci-guard.nft" \
  /etc/solarinet-ci/solarinet-ci-guard.nft
install -D -o root -g root -m 0644 "$source_dir/solarinet-ci.network.xml" \
  /etc/solarinet-ci/solarinet-ci.network.xml
install -d -o root -g root -m 0755 /etc/systemd/system/nftables.service.d
install -o root -g root -m 0644 "$source_dir/solarinet-ci-nftables.service.d.conf" \
  /etc/systemd/system/nftables.service.d/solarinet-ci-guard.conf

install -d -o root -g root -m 0700 /etc/solarinet-ci
if [[ ! -e /etc/solarinet-ci/registration_key ]]; then
  ssh-keygen -q -t ed25519 -N '' -C solarinet-ci-register -f /etc/solarinet-ci/registration_key
fi
chmod 0600 /etc/solarinet-ci/registration_key
chmod 0644 /etc/solarinet-ci/registration_key.pub
install -o root -g root -m 0600 "$source_dir/cesium.known_hosts" /etc/solarinet-ci/known_hosts

systemctl daemon-reload
virsh net-define /etc/solarinet-ci/solarinet-ci.network.xml
virsh net-autostart --disable solarinet-ci
if systemctl is-active --quiet solarinet-ci-guard.service; then
  systemctl reload solarinet-ci-guard.service
else
  systemctl enable --now solarinet-ci-guard.service
fi
systemctl enable solarinet-ci-network.service
systemctl restart solarinet-ci-network.service
systemctl enable solarinet-ci-supervisor.service
printf 'Registration public key (install on Cesium with forced command):\n'
cat /etc/solarinet-ci/registration_key.pub
printf '\nSupervisor installed but not started.\n'
