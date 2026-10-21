# Atomic DNS release candidate

`release.py` stages a complete managed generation, validates every zone and the
full BIND configuration, atomically switches one `current` symlink, reloads,
and requires authoritative SOAs plus an expected A/PTR record per zone, or an
NS answer for an NS-only zone. A/PTR checks take priority when available.
Unmanaged zones stay outside the release. BIND views are refused until ownership
can be represented per view.

Generation and `zones/` directory modes are set explicitly so the DNS service
account can traverse root-owned releases even when the applier's umask is `077`.
Zone data and generated declarations are mode `0644`; candidate data is still
validated before it enters a generation.

The helper generates declarations from validated regular zone files. Candidate
configuration files are ignored. Root configuration supplies argv arrays; no
shell command is accepted from a candidate. Expanded BIND configuration can
contain TSIG keys, so its temporary validation file is private and removed
after checking. It is never logged.

The journal records attempted serial high-water marks before changing the
pointer. Rollback restores previous records in a new generation whose serials
advance beyond the failed attempt, allowing AXFR secondaries to follow it.
Removed/reintroduced zones retain their serial history. Interrupted activation
and failed rollback are retried from the recorded rollback generation.

## Current boundary

This is a tested candidate, **not installed on the live DNS services**.
`--bootstrap` creates a baseline from the configured legacy includes, proves
that its SOAs/records match the running server, then replaces their parent
include atomically. The SoR adapter now calls the helpers and records independent
target status; live cutover and steel's actual container adapter remain unverified.
Do not point the existing copy-based applier at the release directories.

The intended live include is:

```conf
include "/etc/bind/solarinet-current/named.conf.managed";
```

Install the helper as immutable root-owned code and its configuration as a
root-owned file that other accounts cannot modify. The CLI rejects writable
configuration and release directories. `xenon.json.example` describes the
observed primary layout. `steel.json.example` is an **unverified adapter**:
steel was unreachable during this audit; confirm container mounts, user mapping,
PID 1 signal handling, command paths, and DNS listeners before using it.
Its bootstrap expects `named.conf.local` to include the legacy
`named.conf.bare` exactly once. The repository's steel candidate config now
carries that include, and `deploy/dns-steel/test_config.py` checks the include
chain with local BIND. This does not verify steel's offline runtime or files.
Candidates must be private directories beneath `incomingRoot`, and each zone
must be owned by `candidateUser`; ownership is checked on the opened descriptor
with symlinks refused. The examples retain the observed legacy `jason` account.
The dedicated service-account cutover remains part of F-04 before release.

## Initial include conversion

Stop and verify the old copy-based writer before cutover. The helper checks
source hashes again after validation, but its release lock cannot lock a legacy
writer that does not participate in that protocol. Review the explicit host /
container path mapping and `legacyIncludes`; the helper refuses missing,
duplicate, or non-primary declarations and leaves legacy zone files intact.

After installing the reviewed root-owned artifact and configuration:

```sh
sudo /usr/local/libexec/solarinet-dns-release --bootstrap \
  --config /etc/solarinet/dns-release.json
```

The parent include backup and cutover journal are private and durable.
Reload/health failure restores the exact parent contents, mode, and ownership.
An interrupted cutover blocks new activation until `--bootstrap` recovers it.
Re-running a completed bootstrap verifies the current served generation.
The initial conversion keeps the same record data and serials, so a failed
conversion does not need the forward-serial rollback used for later data changes.

The state file distinguishes `pending`, `applied`, `failed`, `rollbackFailed`,
and `recovered`; `applied` is written only after served-data verification.
Keep previous releases and serial history until a reviewed retention policy
accounts for every secondary and replay source.

## Verification

```sh
python3 -m unittest discover -s deploy/dns-release -p test_release.py -v
```

Seventeen tests run a private copy of the installed BIND binary on temporary loopback
ports. They exercise activation, removal of retired zones, protection of
unmanaged zones, invalid input, candidate symlinks, reload/health failures,
crash recovery, rollback retries, and serial history across reintroduction.
They leave the production BIND process and AppArmor profile untouched.
