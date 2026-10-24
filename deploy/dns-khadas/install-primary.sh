#!/bin/sh
set -eu

if [ "$(id -u)" -ne 0 ]; then
    echo "install-primary.sh must run as root" >&2
    exit 1
fi

install -o root -g root -m 0644 /tmp/gen-secondary.py /usr/local/libexec/gen-secondary.py
install -o root -g root -m 0755 /tmp/refresh-secondary-zones.py /usr/local/libexec/solari-dns-secondary-refresh
install -o root -g root -m 0644 /tmp/solari-dns-secondary-refresh.service /etc/systemd/system/solari-dns-secondary-refresh.service
install -o root -g root -m 0644 /tmp/solari-dns-secondary-refresh.path /etc/systemd/system/solari-dns-secondary-refresh.path
install -o root -g root -m 0644 /tmp/solari-dns-secondary-refresh.timer /etc/systemd/system/solari-dns-secondary-refresh.timer

systemctl daemon-reload
systemctl enable --now solari-dns-secondary-refresh.path
systemctl enable --now solari-dns-secondary-refresh.timer
systemctl start solari-dns-secondary-refresh.service
