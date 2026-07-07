# SolariNet backups

Nightly MariaDB dumps and optional Forgejo dumps for the host-health remediation
backup unit.

## Components
| File | Role |
|---|---|
| `solari-backup.sh` | One-shot backup runner: dumps configured MariaDB DBs, optionally runs `forgejo dump`, prunes old matching backups. |
| `backup.conf.example` | Per-host config template for DB list, target directory, retention, and credential sources. |
| `solari-backup@.service` | Templated systemd service; `%i` maps to `/etc/solari-backup-%i.conf`. |
| `solari-backup@.timer` | Templated nightly timer, 03:30 with jitter. |

## Hosts
| Host | Backups | Target |
|---|---|---|
| **cesium** | MariaDB `sor` plus Forgejo once relocated | `/data/backups/` on **sdb1**, a different physical spindle/mount than the live datadir on sda2 |
| **xenon** | MariaDB `solarinet` | Its own durable backup path |

Retention is 14 daily backups by default. Files older than `RETENTION_DAYS` are
pruned only when they match the backup naming patterns; unrelated files in the
target directory are left alone.

## Install
```
sudo install -d -m 0755 /opt/solari-backups
sudo install -m 0755 solari-backup.sh /opt/solari-backups/solari-backup.sh

sudo cp backup.conf.example /etc/solari-backup-cesium.conf
sudo $EDITOR /etc/solari-backup-cesium.conf
sudo chmod 600 /etc/solari-backup-cesium.conf

sudo cp solari-backup@.service solari-backup@.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now solari-backup@cesium.timer
```

Use `/etc/solari-backup-xenon.conf` and
`systemctl enable --now solari-backup@xenon.timer` on xenon. Config files may
reference `/root/.mysql-backup-pw`, `MYSQL_ENV_FILE`, or a root-readable MySQL
defaults file; keep any file containing secrets or secret paths owned by root and
`chmod 600`. Do not put real passwords in this repository.

The service has `OnFailure=solari-notify-failure@%i.service` as a placeholder for
Unit D's notification bridge. The backup script exits nonzero on failures and does
not publish notifications itself.

## Restore
Restore a database dump:
```
gunzip < /data/backups/sor-2026-07-06.sql.gz | mysql sor
gunzip < /path/to/backups/solarinet-2026-07-06.sql.gz | mysql solarinet
```

For Forgejo, stop Forgejo, restore the dump with Forgejo's documented restore
procedure for the deployed version, verify `app.ini` and repository paths, then
start Forgejo again. Keep the database and repository/filesystem restore points
from the same backup date.
