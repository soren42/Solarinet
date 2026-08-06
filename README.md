# SolariNet Monitoring

*A three-tier, fault-tolerant network & service monitoring system in portable C99.*

SolariNet observes a private intranet from three vantage points — a per-host **client**, on-the-wire **remote monitors**, and a fusing **server** — and publishes one authoritative picture of health to a PHP command-and-control dashboard. The full design is in [docs/SolariNet_Architecture_and_Plan.html](docs/SolariNet_Architecture_and_Plan.html), which is the single source of truth for the build.

> **Engineering rules.** ISO C99, functions only — no C++, no OOP emulation beyond the opaque-handle pattern. `camelCase` identifiers, K&R braces, four-space indent. No Node.js / server-side JS anywhere in the toolchain; the only non-C runtime is PHP 8.x for the dashboard. All wire data is big-endian and packed field-by-field — never `memcpy` a struct onto the wire.

## Build status by phase

| Phase | Scope | State |
|---|---|---|
| 0 · Foundation | Repo layout, CMake + Makefile, toolchains | ✅ done |
| 1 · libsolari core | common, error, time, log, crypto, framing, TLV, messages | ✅ done |
| 2 · libsolari I/O | config parser + SQLite spool done & tested; nng/mbedTLS transport written to contract (compile-pending vendored nng) | 🔄 in progress |
| 3 · Client + PAL | solariClient across the OS matrix | ⏳ pending |
| 4 · Remote Monitor | probe engine + HRW redundancy | ⏳ pending |
| 5 · Server | ingest, lease failover, MariaDB persistence | ⏳ pending |
| 6 · Dashboard | Design handoff imported to dashboard/public (in-browser React PWA); reconciled vs §11.2 (see dashboard/API_RECONCILIATION.html). PHP/Apache REST+SSE layer still to build. | 🔄 in progress |
| 7 · Hardening | enrollment, packaging, CI, docs | ⏳ pending |

## Building

The Phase 1 core and its test suite have **no external dependencies** and build with any C99 compiler. CMake ≥ 3.16 drives everything; a thin `Makefile` wraps the common invocations.

```
make core      # build libsolari core + tests (zero external deps)
make test      # build and run the full Unity / ctest suite
make clean     # remove all build trees

# Later phases (need vendored nng / mbedTLS / SQLite under third_party/):
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
dashboard/        PHP 8.x data layer (contract only)
db/               MariaDB schema + migrations
config/           sample .conf files
deploy/           systemd units + enrollment scripts
tests/            unit (Unity), integration, fault injection, fixtures
third_party/      vendored deps (nng, mbedTLS, SQLite, cJSON, Unity)
cmake/toolchains/ cross-compile toolchain files
docs/             self-contained HTML documentation
```

## Protocol & tests

Every tier-to-tier message rides the **SolariNet Control Protocol (SCP)**: a versioned, length-prefixed binary frame (32-byte big-endian header + TLV payload + CRC-32). Per the §14 mandate, golden-frame fixtures are written first and treated as immutable for a given `SCP_PROTO_VERSION` — they are the definition of "correct on the wire." Every public `libsolari` function ships with at least one Unity test.

---

SolariNet Monitoring · prepared by Jason C. Kay (N4JCK).
Source mirror: [github.com/soren42/Solarinet](https://github.com/soren42/Solarinet).
Where this README and the architecture document disagree, the architecture document wins.
