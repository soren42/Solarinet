#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
network_name=solarinet-ci

if sudo virsh net-info "$network_name" 2>/dev/null | grep -q '^Active:.*yes'; then
  echo "$network_name is already active; inspect it before changing its definition." >&2
  exit 1
fi

sudo install -D -o root -g root -m 0644 \
  "$script_dir/solarinet-ci-guard.nft" \
  /etc/solarinet-ci/solarinet-ci-guard.nft
sudo install -D -o root -g root -m 0644 \
  "$script_dir/solarinet-ci-guard.service" \
  /etc/systemd/system/solarinet-ci-guard.service
sudo install -D -o root -g root -m 0644 \
  "$script_dir/solarinet-ci-network.service" \
  /etc/systemd/system/solarinet-ci-network.service

sudo systemctl daemon-reload
sudo virsh net-define "$script_dir/solarinet-ci.network.xml"
sudo virsh net-autostart --disable "$network_name"
sudo systemctl enable --now solarinet-ci-guard.service
sudo systemctl enable --now solarinet-ci-network.service

sudo systemctl --no-pager --full status \
  solarinet-ci-guard.service solarinet-ci-network.service
sudo virsh net-info "$network_name"
