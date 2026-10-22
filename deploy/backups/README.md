# SolariNet backups

Nightly MariaDB dumps and optional Forgejo dumps for the host-health remediation
backup unit.

See the standalone [backup and restore runbook](../../docs/SolariNet_Backup_and_Restore_Runbook_2026-10-20.html)
and its paired PDF for the operator procedure and current host evidence.

## Components
| File | Role |
|---|---|
| `solari-backup.sh` | One-shot backup runner: dumps configured MariaDB DBs, optionally runs `forgejo dump`, prunes old matching backups. |
| `solari-notify-failure.py` | Fail-soft `OnFailure` publisher to the existing `notify.events` exchange. Connection errors are suppressed so AMQP credentials cannot leak to journald. |
| `backup.conf.example` | Per-host config template for DB list, target directory, retention, and credential sources. |
| `solari-backup@.service` | Templated systemd service; `%i` maps to `/etc/solari-backup-%i.conf`. |
| `solari-backup@.timer` | Templated nightly timer, 03:30 with jitter. |

## Hosts
| Host | Backups | Target |
|---|---|---|
| **cesium** | MariaDB `sor` plus Forgejo once relocated | `/data/backups/` on **sdb1**, a different physical spindle/mount than the live datadir on sda2 |
| **xenon** | MariaDB `solarinet` | Candidate off-host NAS-X home-share target is documented in `backup.conf.xenon.example` and `mnt-solari\x2dbackups.mount`; NAS directory and effective permissions are not yet provisioned or verified. `/nas/Shared` remains unsafe as mounted. |

Retention is 14 daily backups by default. Files older than `RETENTION_DAYS` are
pruned only when they match the backup naming patterns; unrelated files in the
target directory are left alone.

Before publishing a dump, the runner fully decodes the gzip stream and rejects
an empty SQL payload. The completed artifact is then renamed into place.

The target must be a pre-created directory owned by root with exact mode
`0700`. Each artifact is root-owned mode `0600`; temporary files are created
uniquely and checked before publication. This prevents a writable shared mount
from silently exposing database contents. `MYSQLDUMP_RUN_AS` optionally runs
the dump command as a local Unix account for socket authentication; use this
only when that account has the needed database access. Run the hermetic
root-owned runner tests with:

```sh
sudo -n python3 -m unittest discover -s deploy/backups -p 'test_*.py' -v
```

The tests cover a valid compressed dump, rejection of a successful-but-empty
dump, refusal of a world-accessible target, unprivileged dump execution,
preservation of the previous artifact after a failed dump, cleanup of failed
Forgejo temporary storage, refusal to write when a required mount is absent,
and retention that leaves unrelated files untouched.
The companion notification tests cover durable AMQP publication, the missing
Pika fallback, and credential-safe logging. CI runs both suites.

## Install
```
sudo install -d -m 0755 /opt/solari-backups
sudo install -m 0755 solari-backup.sh /opt/solari-backups/solari-backup.sh
sudo install -m 0755 solari-notify-failure.py /opt/solari-backups/solari-notify-failure.py

sudo cp backup.conf.example /etc/solari-backup-cesium.conf
sudo $EDITOR /etc/solari-backup-cesium.conf
sudo chmod 600 /etc/solari-backup-cesium.conf

sudo install -o root -g root -m 0600 /dev/null /etc/solari-notify-failure-cesium.conf
sudo $EDITOR /etc/solari-notify-failure-cesium.conf
sudo cp solari-backup@.service solari-backup@.timer solari-notify-failure@.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now solari-backup@cesium.timer
```

Install `/etc/solari-notify-failure-HOST.conf` as `root:root` mode `0600` with
`SOLARI_NOTIFY_PYTHON` pointing to a Python environment that has Pika and
`SOLARI_NOTIFY_CONFIG` pointing to the host's protected `notify.conf`. On Xenon,
the active notifier environment is `/home/jason/Code/Solarinet/deploy/notify/.venv/bin/python3`
and its current config is `/home/jason/Code/Solarinet/deploy/notify/notify.conf`.
For example, Xenon's file contains those two assignments with the paths above;
the file contains paths only, not credentials.
On other hosts, install Pika into a dedicated venv or use the host package and
set the interpreter accordingly. Without Pika or a valid notification config,
the helper logs a warning and exits successfully; backups still report their
own failure through systemd.

Use `/etc/solari-backup-xenon.conf` and
`systemctl enable --now solari-backup@xenon.timer` on xenon. Config files may
reference `/root/.mysql-backup-pw`, `MYSQL_ENV_FILE`, or a root-readable MySQL
defaults file; keep any file containing secrets or secret paths owned by root and
`chmod 600`. Do not put real passwords in this repository.

The backup service uses `OnFailure=solari-notify-failure@%i.service`. The helper
sends a `warn` event to the configured exchange; failures in notification
delivery are fail-soft and logged without AMQP exception details. The backup
script itself exits nonzero on failure and does not publish notifications.

## Restore
Restore a database dump:
```
gunzip < /data/backups/sor-2026-07-06.sql.gz | mysql sor
gunzip < /path/to/backups/solarinet-2026-07-06.sql.gz | mysql solarinet
```

On 2026-10-21 the latest Cesium SoR backup (`sor-2026-10-21.sql.gz`, 31,297
compressed bytes) was streamed into a disposable local MariaDB instance. The
gzip integrity check passed; restore completed; all 41 base tables passed
`mariadb-check`, with four views and 24 triggers present. This verified that
SoR artifact only. Xenon's `solari-backup@xenon.timer`, its
per-host config, and its configured script are absent at the documented paths;
no matching `solarinet` dump was found in the checked backup roots. Xenon still
needs a verified recurring backup target and installed backup runner before
monitoring-schema adoption.

Xenon's mounted `/nas/Shared` is CIFS (`//nas-x.local/Shared`) with the mount
root observed as `root:root` mode `0755`; it contains personal directories and
is rejected by the runner. Read-only access to NAS-X confirmed the
`personal_folder` share and that Jason's home directory is mode `0700`. A
candidate dedicated share directory `SolariNetBackups` is configured in the
Xenon examples. The CIFS mount uses the existing root-only credential file
`/etc/cifs-nas-x.cred`, a share-relative `prefixpath`, and private requested
modes. The mount and backup runner are fail-closed: the Xenon config requires
the target to be a mounted filesystem, and the Xenon-only systemd drop-in
requires that mount. The directory has not been created on the NAS; CIFS
effective ownership/mode and an actual artifact written through the backup
runner remain unverified. Do
not enable this target until those checks pass.
`/var/backups/solarinet` exists as `root:root` mode `0750` but contains only a
dashboard rollback file; it is not the dedicated mode-`0700` database target.
The root filesystem is the local NVMe with 372 GiB free, and `solarinet` is
about 658 MiB. A separate directory on that NVMe is a practical local recovery
target, but it cannot protect against NVMe or host failure. MariaDB socket login
as OS user `mysql` resolved to DB user `mysql@localhost`, but that account was
denied for the dump operation. TCP root login was also denied. A root
Unix-socket `mysqldump` succeeded in the isolated restore drill below. No Xenon
config, target directory, mount, runner, or timer was installed during this
audit.

Xenon's 2026-10-21 recheck found the backup timer inactive, the per-host config
and installed runner absent, and no mounted target or matching archive. NAS-X
SSH timed out, so the share could not be rechecked at that time; its provisioning
and effective permissions remain unverified.

On 2026-10-22, a root Unix-socket `mysqldump` of the live Xenon `solarinet`
database streamed through gzip into a disposable local MariaDB 11.8.6
container. Source and restored instance each reported 29 base tables and zero
views, triggers, routines, or events. `mariadb-check` passed every restored
table. The source was read-only; no dump file was retained, and the container
was removed. This proves a logical dump/restore path, not the installed backup
runner, scheduled backups, off-host retention, or unattended authentication.

For Forgejo, stop Forgejo, restore the dump with Forgejo's documented restore
procedure for the deployed version, verify `app.ini` and repository paths, then
start Forgejo again. Keep the database and repository/filesystem restore points
from the same backup date.
