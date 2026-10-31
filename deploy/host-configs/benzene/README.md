# benzene — provisioning image store (carbon NAS)

OS images and installers are large, so they live on the carbon NAS (RAID5),
not on benzene's local disk. The NAS share is mounted at
`/srv/solari-provision/store`. The netboot HTTP root serves it through two
whole-directory symlinks:

```
/srv/solari-provision/netboot/http/installers -> /srv/solari-provision/store/installers
/srv/solari-provision/netboot/http/images     -> /srv/solari-provision/store/images
```

Everything else stays local and small: `ipxe/`, `configs/`, `enroll/`, the
profiles and the TFTP tree. nginx needs no change, because it follows
symlinks by default.

## Files

| File | Installs to | Purpose |
|---|---|---|
| `fstab.prov-store` | one line appended to `/etc/fstab` | CIFS share, systemd automount, `nofail` |
| `solari-prov-store-mount.service` | `/etc/systemd/system/` | at boot, waits up to 15 min for the NAS (it can come up after benzene following a power loss) |
| *(not in git)* `/etc/cifs-nas-carbon.cred` | root, `0600` | SMB credentials |

## Fail-closed contract

These helpers refuse to run unless the store is a mounted `cifs` filesystem
and both links resolve into it. An unmounted mountpoint is just an empty
directory on the root disk, and writing a 4 GB image there would silently
fill it.

| Helper | Fails closed |
|---|---|
| `fetch-images.sh` | yes |
| `fleet-provision.sh` | yes |
| `fleet-image.sh` | yes |
| `image-refresh.py` | yes |
| `prov-stage.py` | yes |

The check lives in `deploy/fleet/prov-store-guard.sh`. The error reads
`provisioning image store not mounted: …`.

If the NAS is down while nginx is serving, the links dangle and clients get a
404. They never get stale local files.

`solari-prov-finish` runs on xenon every timer tick. On each tick it writes
`/run/solari/prov-store.json`, and the dashboard Images panel shows that file
as the store-status chip.

Knobs (environment, on xenon):

- `PROV_STORE` (default `$PROV_ROOT/store`)
- `PROV_STORE_FSTYPE` (default `cifs`). `none` is accepted only under the
  `SOLARI_PROV_LOCAL=1` test seam.

## Install (as root on benzene)

1. **Credentials file.** Never commit it, never paste it into a log or a
   ticket.

   ```sh
   install -m 0600 -o root -g root /dev/null /etc/cifs-nas-carbon.cred
   ${EDITOR:-vi} /etc/cifs-nas-carbon.cred
   ```

   The file holds exactly these keys, one per line, `key=value`:

   ```
   username=
   password=
   domain=
   ```

   `domain=` is optional; drop it if the NAS account is local.

   Check the mode with `stat -c '%a %U' /etc/cifs-nas-carbon.cred`. It should
   print `600 root`. Do not `cat` the file.

2. **Mountpoint and fstab.**

   ```sh
   mkdir -p /srv/solari-provision/store
   grep -q ' /srv/solari-provision/store ' /etc/fstab || \
     grep -v '^#' fstab.prov-store >> /etc/fstab
   systemctl daemon-reload
   ls /srv/solari-provision/store/ && findmnt -t cifs /srv/solari-provision/store
   ```

3. **Boot wait unit.**

   ```sh
   install -m 0644 solari-prov-store-mount.service /etc/systemd/system/
   systemctl daemon-reload
   systemctl enable --now solari-prov-store-mount.service
   systemctl status solari-prov-store-mount.service
   ```

## Migrating existing trees

`deploy/fleet/prov-store-migrate.py` is stdlib-only Python. Run it on benzene
as the owner of the HTTP root (`jason`). Before you start, make sure nothing
is in flight: no `IMAGE_REFRESH`, `PROV_STAGE` or `FLEET_IMAGE` run is queued
or running, and no netboot install is in progress. Run it inside tmux, since
copying a few GB to the NAS takes a while.

```sh
scp deploy/fleet/prov-store-migrate.py benzene:/tmp/
ssh benzene
tmux new -s prov-store
python3 /tmp/prov-store-migrate.py --selftest-only   # store semantics on the real share
python3 /tmp/prov-store-migrate.py                   # dry run (default): sizes, conflicts
python3 /tmp/prov-store-migrate.py --apply           # copy + read-back sha256 + swap in links
python3 /tmp/prov-store-migrate.py                   # re-run: should report both subs linked
```

What `--apply` does and guarantees:

- **Resumable.** Files already in the store with the same size and sha256 are
  skipped, so after an interruption you just run it again.
- **Conflicts stop the run.** If a destination has different content, nothing
  is swapped.
- **Originals are kept.** They move to
  `/srv/solari-provision/netboot/.<sub>.pre-store.<stamp>`, which sits beside
  the web root and is not served.
- **Sparse files lose their holes.** `images/pitest1-arm64.img` is a 4 GB
  sparse file and copies as a full 4 GB.

To verify, run these from xenon:

```sh
curl -sI http://10.5.2.50:8080/installers/solari-firstboot.sh | head -1   # HTTP_BASE
curl -sI http://10.5.2.50:8080/images/ | head -1
```

Then start one image refresh from the dashboard.

Once a refresh and a netboot install have worked from the store, reclaim the
local disk:

```sh
python3 /tmp/prov-store-migrate.py --purge           # dry run
python3 /tmp/prov-store-migrate.py --purge --apply   # re-hashes every file vs the store, then deletes
```

### Rollback

```sh
cd /srv/solari-provision/netboot/http
rm installers images                                  # the symlinks only
mv ../.installers.pre-store.<stamp> installers
mv ../.images.pre-store.<stamp>     images
```

This only works before `--purge --apply`. Rolling back also means setting
`PROV_STORE_FSTYPE` back to something the helpers accept locally. There is
no local-disk mode outside the test seam, which is deliberate. So in practice,
rolling back means reverting this release.
