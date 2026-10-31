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

| Helper | Fails closed | Writes into the store |
|---|---|---|
| `fetch-images.sh` | yes | re-checks on benzene in the same shell as the download, writes to the resolved store path |
| `fleet-provision.sh` | yes | `installers/solari-firstboot.sh`, re-checked at write time, written to the resolved store path |
| `fleet-image.sh` → `pi/build-pi-image.sh` | yes | the builder checks before downloading and again immediately before `cp`, then writes to the resolved store path |
| `image-refresh.py` | yes | none directly; it goes through `fetch-images.sh` |
| `prov-stage.py` | yes | none directly; it goes through `fleet-provision.sh` |

The check lives in `deploy/fleet/prov-store-guard.sh`. The error reads
`provisioning image store not mounted: …`.

The up-front checks in xenon's Python helpers are a pre-flight only. The check
that counts runs on benzene, in the same shell that writes. That shell writes
to the store path the check resolved (`PROV_STORE_REAL`), never through the
`HTTP_ROOT/<sub>` link. If the link is replaced by a real directory after the
check, the write still lands in the store. If the link stops resolving into a
mounted store before the re-check, the write is refused.

**Hung NAS.** The share is mounted `hard`, so touching a stalled share
blocks. The guard and the migrate run every store touch under a timeout
(`PROV_STORE_TIMEOUT`, default 20 s). On expiry they fail with
`provisioning image store not responding: …`. If `timeout(1)` is not
installed, the guard refuses with an error rather than running unbounded.

**Maintenance lock.** `/srv/solari-provision/netboot/.prov-store-migrate.lock`
(beside the web root, on local disk) is the store maintenance lock:

- **`prov-store-migrate.py --apply`** holds it EXCLUSIVE from before planning
  until the last swap.
- **Writers** (`fetch-images.sh`, the firstboot write, `build-pi-image.sh`)
  take it SHARED in their store check and hold it until they exit.

Each side refuses rather than queueing. A writer waits up to
`PROV_STORE_LOCK_WAIT` seconds (default 10), then fails with
`image store maintenance in progress: …`. The migrate does not wait: it fails
at once with `the image store maintenance lock … is held`. Both are safe to
retry.

If the NAS is down while nginx is serving, the links dangle and clients get a
404. They never get stale local files.

`solari-prov-finish` runs on xenon every timer tick. On each tick it writes
`/run/solari/prov-store.json`, and the dashboard Images panel shows that file
as the store-status chip.

Knobs (environment, on xenon):

- `PROV_STORE` (default `$PROV_ROOT/store`)
- `PROV_STORE_FSTYPE` (default `cifs`). `none` is accepted only under the
  `SOLARI_PROV_LOCAL=1` test seam.
- `PROV_STORE_TIMEOUT` (seconds, default `20`): the bound on each store touch.
- `PROV_STORE_LOCK_WAIT` (seconds, default `10`): how long a writer waits for
  the maintenance lock.

`fleet-image.sh` passes `PROV_STORE`, `PROV_STORE_FSTYPE` and `HTTP_ROOT` to the
builder. It also ships `provisioning-guard.sh` and `prov-store-guard.sh` to
`$PROV_ROOT/` on benzene, next to `pi/`. The builder sources both from there.

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

4. **Make the underlying mountpoint immutable.** When the share is not mounted,
   `/srv/solari-provision/store` is an ordinary directory on benzene's btrfs
   `/srv`. With `+i` set on it, nothing can create entries in it, not even
   root. A write that slips past every check onto the bare mountpoint then
   fails with `EPERM` instead of filling the local disk. Mounting the share
   over an immutable directory still works.

   `chattr` on the live path would act on the CIFS share, so reach the
   underlying directory through a non-recursive bind mount of `/srv`. That
   does not unmount the share or disturb a running install:

   ```sh
   mkdir -p /mnt/srv-under
   mount --bind /srv /mnt/srv-under            # non-recursive: the NAS mount is not carried over
   ls -A /mnt/srv-under/solari-provision/store # must be empty; stop if it is not
   chattr +i /mnt/srv-under/solari-provision/store
   lsattr -d /mnt/srv-under/solari-provision/store   # expect: ----i---------
   umount /mnt/srv-under && rmdir /mnt/srv-under
   ls /srv/solari-provision/store/ && findmnt -t cifs /srv/solari-provision/store   # still mounted
   ```

   To undo it, run the same bind mount and `chattr -i` the directory.

## Migrating existing trees

`deploy/fleet/prov-store-migrate.py` is stdlib-only Python. Run it on benzene
as the owner of the HTTP root (`jason`). Before you start, make sure nothing
is in flight: no `IMAGE_REFRESH`, `PROV_STAGE` or `FLEET_IMAGE` run is queued
or running, and no netboot install is in progress. The maintenance lock
enforces the store-writer part of this:

- `--apply` refuses while a writer holds the lock.
- A writer that starts during the migration refuses.

The lock does not cover a netboot client that is mid-install and reading from
the web root, so that part is still on you. Run the migrate inside tmux,
since copying a few GB to the NAS takes a while.

Two gaps are closed by procedure, not code (Lead decision, cross-lab review
of e53341a). Nothing starts an image writer on a timer: they run only when
someone triggers them from the dashboard. So:

- **Install the release that carries the guard first, then migrate.** Helpers
  from an older release don't take the maintenance lock. Once the new
  release is active, no lock-unaware writer is left, and every writer
  refuses until the migrate has swapped in the links.
- **A NAS that stalls after the first check can hang the migrate.** Only the
  first touch and `findmnt` are time-limited; later disk-usage, traversal
  and copy calls are not. Swaps happen only after every file has been read
  back and verified, so a hang loses nothing. Interrupt it (Ctrl-C, or kill
  it from another shell), wait for the NAS, and re-run: copies that already
  match are skipped.

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
- **Conflicts stop the run before anything happens.** Both `installers` and
  `images` are planned before anything is copied. If any destination file has
  different content, or any in-tree link leaves its tree, the run stops. It
  then reports `nothing copied, nothing swapped (neither installers nor
  images)`.
- **Copy all, verify all, then swap.** The swaps happen only after every copy
  has been read back and verified. If a swap still fails, the error says
  exactly what happened, for example `PARTIAL: swapped installers -> … ;
  NOT swapped: images (still the local directory …)`. If nothing was swapped,
  it says `nothing swapped: …`. A re-run picks up from either state.
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
