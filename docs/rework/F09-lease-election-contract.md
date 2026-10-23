# F-09 lease election contract

**Status:** implementation candidate; not deployed.
**Observed defect:** a live standby claim can increment `serverLease.leaseEpoch` even though the current holder remains unchanged.
**Scope:** the single-row MariaDB election in `serverDbLeaseClaim` and its live database test. No interface, schema, or deployment change.

## Required behavior

1. The first claimant creates the lease with epoch 1.
2. The current holder renews an unexpired lease without changing its epoch.
3. A different node claiming an unexpired lease loses and leaves holder, expiry, and epoch unchanged.
4. Once a lease is expired, the first claimant atomically becomes holder and advances the epoch exactly once, including when the same node identity reclaims it after expiry.
5. A later renewal by the new holder preserves that new epoch.

The epoch is a fencing token. A losing attempt must never invalidate the active holder's token. An expired same-node reacquisition must receive a new token so work from the prior lease generation can be distinguished.

## Verification

The disposable MariaDB integration test will exercise creation, renewal, a losing claimant through a second database handle, unchanged holder/epoch on loss, forced expiry, takeover, and exactly one epoch advance. It must run only against a disposable database; `test_server_db_live` already refuses the production database name.

**Verified 2026-10-21:** the complete live-DB suite passed 14/14 against a
disposable MariaDB 11.8.6 container bound to loopback port 33106. The lease case
now repeats eight expiry races with two barrier-released threads, each opening an
independent MariaDB handle. Every race produced one winner and exactly one epoch
advance. The container was removed after the run; the production database and
live server lease were not used.

The new process-level test also passed against that disposable database. Two
real `solariServer` processes shared three loopback listeners: server A held
epoch 1 while server B stayed standby without changing the lease; after A's
SIGTERM shutdown, B acquired epoch 2 and bound all three listeners. The test
cleared its lease row and the database container was removed. The harness only
accepts names with the dedicated `solarinet_lease_process_*` prefix. CI
provisions its own migrated scratch schema and runs this process test after the
full server suite.

## Release gate

This is an HA behavior change. Before deployment, require independent cross-lab review of the contract and diff, then rehearse rollback on a disposable MariaDB. The current authoring environment's Claude OAuth review attempt failed because the session expired, so the candidate is unreviewed until a reviewer can complete that gate.

## Unverified

- Dynamic failover behavior on the live pair, including deployed unit settings,
  host/network timing, and service restart policy.
- Cross-lab review and rollback rehearsal.
