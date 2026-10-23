#!/bin/bash
set -euo pipefail
trap 'rc=$?; /usr/bin/systemctl poweroff --force || true; exit "$rc"' ERR

runner_version='13.2.0'
runner_sha256='fadaec897f5e6641c363f87ecaf75f866f99c81b7097ec6b433e4c48123fcad1'
node_version='24.10.0'
node_sha256='2642f4428869aca32443660fd71b3918e2be1277a899bdcaeb64c93b54b5af17'

systemctl mask --now serial-getty@ttyS0.service
install -d -o runner -g runner -m 0700 /var/lib/solarinet-ci
install -d -o runner -g runner -m 0700 /var/lib/runner/work
install -d -o root -g root -m 0755 /usr/local/lib/solarinet-ci
chown runner:runner /etc/solarinet-ci/runner-token /etc/solarinet-ci/runner-uuid
chmod 0600 /etc/solarinet-ci/runner-token /etc/solarinet-ci/runner-uuid

curl --fail --location --retry 4 --retry-delay 2 \
  --connect-timeout 15 --max-time 1800 \
  "https://code.forgejo.org/forgejo/runner/releases/download/v${runner_version}/forgejo-runner-${runner_version}-linux-amd64" \
  -o /usr/local/bin/forgejo-runner
printf '%s  %s\n' "$runner_sha256" /usr/local/bin/forgejo-runner | sha256sum --check --status
chmod 0755 /usr/local/bin/forgejo-runner

node_archive="/tmp/node-v${node_version}-linux-x64.tar.xz"
curl --fail --location --retry 4 --retry-delay 2 \
  --connect-timeout 15 --max-time 900 \
  "https://nodejs.org/dist/v${node_version}/node-v${node_version}-linux-x64.tar.xz" \
  -o "$node_archive"
printf '%s  %s\n' "$node_sha256" "$node_archive" | sha256sum --check --status
install -d -o root -g root -m 0755 /usr/local/lib/nodejs
tar -xJf "$node_archive" -C /usr/local/lib/nodejs
node_dir="/usr/local/lib/nodejs/node-v${node_version}-linux-x64"
ln -sfn "$node_dir/bin/node" /usr/local/bin/node
ln -sfn "$node_dir/bin/npm" /usr/local/bin/npm
ln -sfn "$node_dir/bin/npx" /usr/local/bin/npx
rm -f "$node_archive"
node --version

cat >/usr/local/lib/solarinet-ci/execute-one-job <<'EOF'
#!/bin/bash
set -u
cd /var/lib/solarinet-ci || exit 1
set -uo pipefail
/usr/local/bin/forgejo-runner one-job \
  --url http://10.1.0.200:3000 \
  --uuid "$(cat /etc/solarinet-ci/runner-uuid)" \
  --token-url file:///etc/solarinet-ci/runner-token \
  --label solarinet-ci-host:host \
  --wait 2>&1 | while IFS= read -r log_line; do
    if [[ "$log_line" == *"single task poller successfully fetched one task"* ]]; then
      printf '\nSOLARINET_CI_STARTED\n' | sudo -n /usr/bin/tee /dev/ttyS0 >/dev/null
    fi
    printf '%s\n' "$log_line" >&2
  done
rc=${PIPESTATUS[0]}
printf '\nSOLARINET_CI_EXIT=%s\n' "$rc" | sudo -n /usr/bin/tee /dev/ttyS0 >/dev/null
sync
sleep 1
sudo -n /usr/bin/systemctl poweroff || true
exit "$rc"
EOF
chmod 0755 /usr/local/lib/solarinet-ci/execute-one-job

cat >/etc/systemd/system/solarinet-ci-runner.service <<'EOF'
[Unit]
Description=Single-job Solarinet Forgejo runner
After=network-online.target docker.service
Wants=network-online.target docker.service

[Service]
Type=oneshot
User=runner
Group=runner
SupplementaryGroups=docker
WorkingDirectory=/var/lib/solarinet-ci
ExecStart=/usr/local/lib/solarinet-ci/execute-one-job
TimeoutStartSec=infinity
EOF

systemctl daemon-reload
systemctl enable docker.service
systemctl start docker.service
systemctl stop solarinet-ci-bootstrap-deadline.timer 2>/dev/null || true
systemctl start solarinet-ci-runner.service
