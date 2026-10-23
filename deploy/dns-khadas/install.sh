#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "install.sh must run as root" >&2
    exit 1
fi

apt-get update
DEBIAN_FRONTEND=noninteractive apt-get install -y bind9

# Do not leave the package's default recursive resolver enabled. The isolated
# SolariNet service has its own explicit authoritative-only options.
systemctl disable --now named.service >/dev/null 2>&1 || true
systemctl disable --now bind9.service >/dev/null 2>&1 || true

install -d -o root -g root -m 0755 /etc/bind/solarinet-secondary
install -o root -g root -m 0644 /tmp/named.conf /etc/bind/solarinet-secondary/named.conf
install -o root -g root -m 0644 /tmp/options.conf /etc/bind/solarinet-secondary/options.conf
install -o root -g root -m 0644 /tmp/zones.conf /etc/bind/solarinet-secondary/zones.conf
install -o root -g root -m 0755 /tmp/update-zones.py /usr/local/sbin/solari-bind9-update-zones
install -d -o bind -g bind -m 0750 /var/cache/bind/solarinet-secondary
install -o root -g root -m 0644 /tmp/solari-bind9.service /etc/systemd/system/solari-bind9.service

named-checkconf /etc/bind/solarinet-secondary/named.conf
systemctl daemon-reload
systemctl enable --now solari-bind9.service
