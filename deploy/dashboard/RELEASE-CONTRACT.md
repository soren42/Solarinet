# Dashboard release contract

## Problem

The production services expect `/opt/solarinet/bin/solariServer`, the
lease-fenced `/opt/solarinet/bin/solariScanWorker`, and the API
expects `/opt/solarinet/dashboard/api`, but the deployment guide copied the SPA
separately. That could leave the browser and API on different revisions, and
there was no rehearsed rollback path.

## Design

Package an explicit allowlist: the built server and scan worker binaries,
`dashboard/api`, and `dashboard/public`. The archive includes a version and SHA-256 manifest and
contains no runtime state, configuration, credentials, or CA material. Install
each release immutably below `/opt/solarinet/releases/<version>` and switch one
`current` symlink atomically. Activating a retained version is the rollback.
The server, API, and SPA all resolve through that same symlink.
The package requires the embedded RackWire application entry point so a missing
or uninitialized RackWire submodule cannot produce a partial dashboard release.

The installer rejects unsafe archive paths, symlinks, undeclared files, and
hash mismatches. It stages and verifies the full tree before making it active.
The installer does not restart services; service reload remains an explicit
operator step after the activated tree passes local validation.

The SHA-256 manifest detects accidental or incomplete payload changes; it is not
a publisher signature. The packager refuses a dirty Git worktree by default and
records the source revision plus `sourceTreeDirty: false`. `--allow-dirty` is
only for preflight bundles; the installer refuses those bundles unless the same
explicit opt-in is passed. Production builds must use a committed, clean source
tree and must not pass `--allow-dirty`. Build and transfer the archive through
the trusted repository workflow.

## Acceptance

- A bundle contains only the allowlisted runtime tree and release metadata.
- An install under a temporary root validates hashes and switches `current` to
  the new version.
- A second install leaves the prior release intact; activating that version
  atomically restores the prior `current` target.
- Invalid paths, symlinks, duplicate versions, and altered file contents fail
  without changing the active release.
- Dirty-source bundles are rejected by default at packaging and installation;
  explicit preflight opt-in is recorded in release metadata.
- PHP-FPM and Apache point into `/opt/solarinet/current`; the service binary
  uses the same root.
- The worker service on both server hosts uses the worker binary from that same
  release symlink. Enable it alongside the server; admitted jobs remain visibly
  queued when the worker service is absent.

## Verification status

The temporary-root release suite passes **9 tests**, including clean install and
rollback, dirty-source opt-in, tampered payload rejection, path traversal and
symlink rejection, and RackWire entrypoint presence. CI runs this suite through
`python3 -m unittest discover -s deploy/dashboard -p 'test_release.py' -v`.
Independent cross-lab review, production host cutover, service health checks,
and live rollback remain release gates.
