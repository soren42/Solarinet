#!/bin/bash
# Roll back enable-boundary.sh: restore pool + server.conf, drop the server drop-in.
# Leaves www-solari / solari_dash / copied state in place (inert).
set -euo pipefail
BK=$(cat /root/boundary-backup-latest)
cp -a "$BK/solarinet.conf" /etc/php/8.5/fpm/pool.d/solarinet.conf
cp -a "$BK/server.conf" /home/jason/Code/Solarinet/run/server.conf
rm -f /etc/systemd/system/solarinet-server.service.d/20-ctl-boundary.conf
systemctl daemon-reload
systemctl restart solarinet-server
systemctl reload php8.5-fpm
echo "rolled back from $BK"
