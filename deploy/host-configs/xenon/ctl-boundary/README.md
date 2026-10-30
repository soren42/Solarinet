# xenon ctl boundary cutover (applied 2026-10-30)

`enable-boundary.sh` turned on the PHP→host boundary on xenon:

- php-fpm pool `solarinet` runs as `www-solari` (state in `/var/lib/solarinet-dashboard`)
- ctl socket `/run/solari/solariCtl.sock`, `0660 jason:solari-ctl` (dir `0751`)
- `server.conf [ctl] enforcePeer=true dashboardUid=www-solari socketGid=solari-ctl`
- dashboard DB user `solari_dash`: per-table DML on `solarinet`, no `ctlRequest`
  (password in `/etc/solarinet/dashboard-db.pass`, root 0600)

Deviation from `deploy/dashboard/hardened/PERMISSIONS.md`: the server stays
`User=jason` (operator uid) because remote deploy uses jason's SSH keys and the
runtime tree lives under `~/Code/Solarinet/run`. Moving the server to `solari`
is a separate later step.

**After any migration that adds a `solarinet` table**, re-run the grant block
(step 3) or the dashboard will get `SELECT command denied` on the new table.

Rollback: `sudo rollback-boundary.sh` (restores pool + server.conf from
`/root/boundary-backup-latest`, removes the server drop-in).
