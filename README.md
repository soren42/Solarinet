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

## Protocol & tests

Every tier-to-tier message rides the **SolariNet Control Protocol (SCP)**: a versioned, length-prefixed binary frame (32-byte big-endian header + TLV payload + CRC-32). Per the §14 mandate, golden-frame fixtures are written first and treated as immutable for a given `SCP_PROTO_VERSION` — they are the definition of "correct on the wire." Every public `libsolari` function ships with at least one Unity test.

---

SolariNet Monitoring · prepared by Jason Kay.
Source mirror: [github.com/soren42/Solarinet](https://github.com/soren42/Solarinet).
Where this README and the architecture document disagree, the architecture document wins.
