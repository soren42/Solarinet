# SolariNet Monitoring

*A three-tier, fault-tolerant network & service monitoring system in portable C99.*

SolariNet observes a private intranet from three vantage points — a per-host **client**, on-the-wire **remote monitors**, and a fusing **server** — and publishes one authoritative picture of health to a PHP command-and-control dashboard. The full design is in [docs/SolariNet_Architecture_and_Plan.html](docs/SolariNet_Architecture_and_Plan.html), which is the single source of truth for the build.

> **Engineering rules.** The control plane is ISO C99, functions only — no C++, no OOP emulation beyond the opaque-handle pattern. `camelCase` identifiers, K&R braces, four-space indent. The dashboard uses PHP 8.x and browser JavaScript; operational integrations use Python. No Node.js server or bundler is required. All wire data is big-endian and packed field-by-field — never `memcpy` a struct onto the wire.

## Current status

The C client, monitor, server, PHP API, browser dashboard, SoR, and deployment
integrations have implementations and run on the intranet. The original phase
table became stale as those components were built. **Implementation is not the
same as release readiness:** the [stabilization workflow](docs/SolariNet_Stabilization_Integration_Workflow_2026-08-09.md)
freezes new feature work until authorization, identity, durable delivery,
schema, freshness, and fault-injection gates pass. The [October 2026 baseline](docs/SolariNet_Stabilization_Baseline_2026-10-20.html)
records what was actually checked and what remains unverified.

The standalone [`scanner-ingest`](../scanner-ingest/README.md) app is an
authorized inventory-intake subproject tracked as integration workstream I5.
The standalone app keeps its own SQLite capture store. Its export can be reviewed
and imported through an operator-only dashboard flow with explicit mapping,
preview, and a transactional insert-only apply into the SoR. This physical
asset intake is separate from SolariNet's **real-time network discovery**:
operators start a bounded asynchronous scan in Discovery, the native scan
worker records reviewable host candidates, and an explicit adoption sends the
host and address through monitoring and SoR. RackWire links to that discovery
workflow and also uses the shared inventory catalog for planning. A discovered
network host is not automatically a physical inventory asset. See the
[scanner import guide](docs/SolariNet_Inventory_Scanner_Import.html) and its
[Letter PDF](docs/SolariNet_Inventory_Scanner_Import.pdf), plus the
[stabilization baseline](docs/SolariNet_Stabilization_Baseline_2026-10-20.html)
for verification limits. No production deployment has occurred. A read-only
live preview on 2026-10-20 found two scanner location labels without exact SoR
matches and did not write records. The scanner app has a local Git repository,
but no release remote is configured yet.

The UniFi API integration continues to poll device health and rates for the
monitoring panel. Its separate topology collector can now compare VLAN,
network, subnet, and DHCP observations with the SoR in a read-only transaction;
the observed controller `/8` versus SoR `/24` mismatch remains an import gate.
See the [UniFi topology guide](docs/SolariNet_UniFi_Topology_Discovery.html)
and its [Letter PDF](docs/SolariNet_UniFi_Topology_Discovery.pdf).

## Building

The core and its test suite have **no external dependencies** and build with any C99 compiler. The full client, monitor, and server build uses nng, mbedTLS, SQLite, cJSON, and MariaDB Connector/C. CMake ≥ 3.16 drives the build; a thin `Makefile` wraps common invocations.

```
make core      # build libsolari core + tests (zero external deps)
make test      # build and run the full Unity / ctest suite
make clean     # remove all build trees

# Full components (require the development libraries described below):
make client    # solariClient   (all OS targets)
make monitor   # solariMonitor  (Linux, all arch)
make server    # solariServer   (x86_64 / arm64)

# Cross-compilation:
make cross-arm64    make cross-arm32    make windows
```

Or invoke CMake directly:

```
cmake -S . -B build -DSOLARI_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

On a fresh Linux host (e.g. a dedicated build/deploy server), one script installs the toolchain + all dependencies (mbedTLS, nng, SQLite, cJSON, MariaDB Connector/C), then configures, builds, and runs the suites including the SQLite spool and the nng transport loopback:

```
./deploy/bootstrap-xenon.sh            # system packages where available
./deploy/bootstrap-xenon.sh --source-deps   # build nng + mbedTLS from pinned source
./deploy/bootstrap-xenon.sh --help
```

Build options: `-DSOLARI_WITH_SQLITE=ON` adds the store-and-forward spool, `-DSOLARI_WITH_IO=ON` adds the nng/mbedTLS transport, `-DSOLARI_WITH_JSON=ON` enables the cJSON config overlay. The bare core needs none of them.

## Repository layout

```
include/solari/   public libsolari headers (the contract)
lib/solari/       libsolari implementation
src/client/       solariClient + platform abstraction (plat/)
src/monitor/      solariMonitor probe engine
src/server/       solariServer ingest / lease / DB / control
dashboard/        PHP 8.x API + browser dashboard
db/               MariaDB schema + migrations
deploy/           systemd units, integrations, enrollment and sample configs
tests/            unit (Unity), integration, fault injection, fixtures
third_party/      vendored deps (nng, mbedTLS, SQLite, cJSON, Unity)
cmake/toolchains/ cross-compile toolchain files
docs/             self-contained HTML documentation
```

## Database installation and schema snapshots

For an empty database, use the ledgered migrator for the full monitoring or SoR
stream. Create each target with the canonical character set and collation; the
migrator refuses other defaults before it writes. Supply credentials with a
protected MariaDB option file or local socket authentication; do not pass
passwords on the command line.

```sh
mariadb -e 'CREATE DATABASE NEW_MONITORING_DB CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci; CREATE DATABASE NEW_SOR_DB CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;'
python3 tools/migrate.py --database monitoring --db-name NEW_MONITORING_DB
python3 tools/migrate.py --database sor --db-name NEW_SOR_DB --defaults-file /path/to/protected.cnf
```

Existing databases without a migration ledger are refused until their schema
history is audited; follow [the migration runbook](docs/SolariNet_Migration_Runbook_2026-10-20.html).
Regenerate the schema-only reference snapshots with `python3 tools/export_schema.py`
and `python3 tools/export_schema.py --database sor`. Export creates and drops a
temporary database. The generated snapshots are not an install or upgrade path.

## Protocol & tests

Every tier-to-tier message rides the **SolariNet Control Protocol (SCP)**: a versioned, length-prefixed binary frame (32-byte big-endian header + TLV payload + CRC-32). Per the §14 mandate, golden-frame fixtures are written first and treated as immutable for a given `SCP_PROTO_VERSION` — they are the definition of "correct on the wire." Every public `libsolari` function ships with at least one Unity test.

---

SolariNet Monitoring · prepared by Jason Kay.
Source mirror: [github.com/soren42/Solarinet](https://github.com/soren42/Solarinet).
Where this README and the architecture document disagree, the architecture document wins.
