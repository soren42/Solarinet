# Xenon immutable daemon release

The release archive contains the dashboard payload, server/worker, Xenon client/monitor/SNMP binaries, Python daemon sources, and pinned Python dependencies. Configurations, state, and `run/db.env` remain under `/home/jason/Code/Solarinet` for this cutover. The cutover adds an explicit `state.file` to the existing alertbridge config, preserving its current checkout-side state location; it backs up and restores that config alongside the units. The UniFi unit preserves its prior state-file path through `UNIFI_STATE`. The installer creates a host-native `.venv` in each release from `apps/requirements.txt`; this avoids shipping a machine-specific virtualenv. Installation needs Python `venv`/`ensurepip`, pip, and package-index access. The venv is generated runtime material; manifest validation covers the bundled code and binaries.

## Build and install

Run from a clean, committed source checkout with the pinned I/O dependency prefix used for the existing release build:

```sh
VERSION="$(git rev-parse --short HEAD)"
deploy/build-release-binaries.sh /path/to/pinned-io-prefix build-release
cmake --build build-release --target solariClient solariMonitor solariSnmpPoll -j
python3 deploy/dashboard/package-release.py \
  --version "$VERSION" \
  --binary build-release/src/server/solariServer \
  --scan-worker-binary build-release/src/server/solariScanWorker \
  --client-binary build-release/src/client/solariClient \
  --monitor-binary build-release/src/monitor/solariMonitor \
  --snmp-binary build-release/src/server/solariSnmpPoll \
  --output "deploy/dist/solarinet-daemons-$VERSION.tar.gz"
```

Transfer the archive to Xenon, then install and validate it before the service cutover:

```sh
sudo python3 /home/jason/Code/Solarinet/deploy/dashboard/install-release.py \
  --bundle /tmp/solarinet-daemons-<version>.tar.gz
readlink -f /opt/solarinet/current
sudo test -x /opt/solarinet/current/.venv/bin/python3
```

## Cutover

The cutover saves the existing unit files below `/var/backups/solarinet-immutable-<UTC timestamp>`, installs the release-backed units, reloads systemd, then restarts and checks one unit at a time. On a failed restart or check it restores the saved definitions and attempts to restore each service's prior active state. Keep the printed backup path for rollback.

```sh
sudo /home/jason/Code/Solarinet/deploy/host-configs/xenon/cutover-immutable.sh
sudo /home/jason/Code/Solarinet/deploy/host-configs/xenon/verify-immutable-processes.py
```

Rollback to the saved unit definitions if cutover needs reversal:

```sh
sudo /home/jason/Code/Solarinet/deploy/host-configs/xenon/rollback-immutable.sh \
  /var/backups/solarinet-immutable-<UTC timestamp>
```

The verification script checks the relevant daemon executable, cwd, and Python script arguments against `/home/jason/Code`. A successful check confirms those processes no longer resolve code from the writable checkout; it does not verify application-level connectivity.
