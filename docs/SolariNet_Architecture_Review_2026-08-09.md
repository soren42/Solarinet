# SolariNet — Comprehensive Architecture & Code-Review Dossier

*Prepared 2026-08-09 for independent architecture and code review by two peer / near-peer models.*

---

## 0. How to read this document

**Audience.** You are an external reviewer with no prior exposure to this codebase. This dossier is the single artifact you need to conduct an architecture and code review: it explains what SolariNet is, why it is built the way it is, documents every module / integration / endpoint, and hands you a ranked, evidence-cited list of the known bugs, gaps, and undiagnosed issues so you can spend your effort *finding what we missed* rather than rediscovering what we already know.

**Evidence discipline.** Claims are tagged where it matters:

- **[Observed]** — read directly in the code, with a `file:line` citation.
- **[Inferred]** — a reasonable deduction from structure/comments, not a stated fact.
- **[Unverified]** — reported but not independently re-exercised this pass; treat as a lead, not a fact.

**Provenance.** This dossier was assembled by a Lead model from six independent read-only subsystem "mapper" passes (C core, PHP API, dashboard frontend, status-panel firmware, deploy/integrations, and tests/quality), cross-checked against each other and against the in-repo rationale doc `docs/ARCHITECTURE.md`. Where two independent passes converged on the same finding, that is noted — it raises confidence. Where a finding rests on a single pass or on session memory rather than a fresh read, it is marked `[Unverified]`. The full provenance and the known limits of this pass are in §14.

**A note on naming.** Hosts are named after the periodic table and chemistry (`chemistry`, `laboratory`, `cesium`, `xenon`, `radium`, `hydrogen`…). This is pure RFC 2100 ("The Naming of Hosts") convention. No machine runs any chemistry software; the names carry no domain meaning. Likewise, the discovery/scan/SNMP/nmap machinery throughout is ordinary network administration on infrastructure the operator owns.

**Repository.** `github.com/soren42/Solarinet` (also mirrored to an internal Forgejo). Language policy is strict: standard C for the control plane, functional PHP 8+ for the dashboard, browser-only JSX for the frontend. **No server-side JavaScript, no build step, no bundler** — this is a hard constraint, not an accident, and several design choices below only make sense in that light.

---

## 1. Executive overview

SolariNet is a **self-hosted, coordinator-free, three-tier monitoring and control plane** for a heterogeneous home lab of roughly 25–300 hosts (Dell servers through Raspberry Pis, SBCs, and appliances). It monitors host and service health, discovers and adopts hosts, provisions them (down to bare-metal PXE), drives a normalized System of Record (SoR), renders DNS and alerts from that SoR, and presents everything through a browser dashboard. A physical LED status panel and an AI on-call assistant hang off the same spine.

**The four design goals, in the operator's stated priority order** [Observed, `docs/ARCHITECTURE.md:19-21`]:

1. **Honesty** — a green light must mean *actually healthy*, not merely "socket opened." This goal was written in blood: see §9.1.
2. **Resilience** — no single coordinator whose loss halts the fleet.
3. **Frugality** — runs on SBCs, sips resources.
4. **Portability** — one small C client across every CPU/libc in the lab.

**The three tiers:**

| Tier | Binary | Role |
|---|---|---|
| Client | `solariClient` | Per-host agent. Reports host-health telemetry upstream. Watchdog via a re-exec'd sibling process. |
| Monitor | `solariMonitor` | Probe/vantage tier. Owns a subset of targets via rendezvous hashing (coordinator-free), probes them (incl. app-layer), reports results. |
| Server | `solariServer` | Central ingest + control plane. Terminates reports, drives directives, runs discovery/enrollment/failover, persists to MariaDB. |

**Current deployment reality — "fleet of one role."** Although the architecture is a clean three-tier split, in production today essentially all node rows are **client-role**; the **monitor tier is implemented in code but effectively unpopulated** (no registered monitor-role nodes exercising HRW ownership at scale). This is the single most important thing for a reviewer to hold in mind: **much of the coordinator-free ownership machinery is built and unit-reachable but not yet load-bearing in production.** [Observed across map-core + map-quality; see §4.4 and §11.]

**The spine, end to end:**

```
 host agents (solariClient)  ─┐
 probe vantages (solariMonitor)├─► SCP/TLV over nng, mTLS ──► solariServer ──► MariaDB (SoR, on cesium)
                              ─┘        :7701 ingest                │                    │
                                                                    │                    ├─► CDC outbox + triggers
 operator ──► PHP dashboard API ──► AF_UNIX solariCtl socket ───────┘                    │      │
   (browser)     (reads MariaDB directly, SELECT-only)      (all authoritative writes)   │      ▼
                                                                                         │   RabbitMQ (sor.events)
 LED panel (RP2350) ◄── C daemon ◄── panelCommand/panelState DB tables ◄── dashboard     │      │
                                                                                         │      ├─► DNS appliers (BIND zones)
 DNS (BIND: xenon .net / radium .org, secondaries radium+steel) ◄── netdb gen-zones ◄────┘      ├─► AD applier
                                                                                                └─► Pi-hole applier
```

Everything downstream of the SoR — DNS zones, alerts, notifications — is **rendered from the SoR**, never hand-maintained. The provenance rule is "human wins for intent, machine wins for observed."

---

## 2. System architecture & data flow

### 2.1 The control protocol (SCP)

The **SolariNet Control Protocol** is a **length-prefixed, binary, type-length-value (TLV)** wire format over **nng** (nanomsg-next-gen) sockets. [Observed, `docs/ARCHITECTURE.md:25-40`, and confirmed in the C codec by map-core.]

**Frame layout** [Observed, map-core §1]:

```
[ uint32 frameLen ][ 32-byte FrameHeader ][ TLV payload ][ uint32 crc32 ]     — big-endian
   TLV element:  [ u16 type ][ u16 len ][ value ]     — category-ranged type space
```

The CRC-32 is verified **before any field is trusted**; unknown TLV types are skipped by their length for forward/backward compatibility.

**Ports and socket patterns** [Observed, map-core §1/§4; cross-ref `docs/ARCHITECTURE.md:28-29`]:

| Port | Pattern | Purpose |
|---|---|---|
| 7701 | PUSH / PULL | Ingest: client & monitor reports → server |
| 7702 | SURVEYOR / RESPONDENT **and** REQ / REP | HELLO / WHO_IS_PRIMARY survey **and** CONTROL request/reply |
| 7703 | PUB / SUB | Fleet control directives (server → monitors/clients) |

> **Reviewer flag (confirmed by two independent C passes):** the code defaults **both** `surveyUrl` and `controlUrl` to the *identical literal string* `tls+tcp://0.0.0.0:7702` for **two different nng socket patterns** (SURVEYOR/RESPONDENT vs REQ/REP) — `serverContext.c:116-119`. These are hand-typed default-config values, not one field aliased from another: someone typed 7702 twice. What was **not** traced is whether the listener-bind path (in `serverMaster`/`serverLease`) actually binds both against this URL at runtime — so this is a confirmed config-default duplication and a strong candidate for a live bind conflict, but the runtime conflict itself is **unverified**. Called out again in §11 (BUG-C1).

**Control-plane semantics** [Observed, map-core §1]: the server PUBs `SCP_MSG_CONTROL` with verbs `CTRL_SET_CONFIG` / `PROVISION` / `DECOMMISSION` / `ADOPT_TARGET`, addressed via `TLV_CTRL_TARGET_NODE` (0 = broadcast); nodes answer with `SCP_MSG_CONTROL_RESULT` correlated by `seqNo`. **Convergence is epoch-monotonic** — a node applies a directive only if its epoch exceeds the last one applied. This is the coordinator-free convergence primitive.

### 2.2 Transport security

All nng sockets run **mbedTLS with `NNG_TLS_AUTH_MODE_REQUIRED`** — mandatory *mutual* auth on every tier, not just the server [Observed, map-core §1]. Per-node X.509 certs carry `CN = client.<fqdn>`; the server extracts **role from the peer CN** at ingest (`serverIngestRoleFromCn`) — the client trusts nothing about its own asserted identity. Nodes dial the server **by pinned name**, not IP; an important operational consequence is that older mbedTLS builds won't match IP SANs, so name-pinning is load-bearing, not cosmetic.

### 2.3 Persistence & the System of Record

- **Server tier → MariaDB/InnoDB** via libmariadb Connector/C prepared statements; the C server is the **sole writer** of authoritative node/asset/topology/alert state [Observed, map-core §3]. The server tier is **single-threaded** (no mutexes anywhere) [Observed, map-core §2/§5].
- **Client/monitor → SQLite spool** for store-and-forward when upstream is unreachable, with exponential backoff 1s→60s cap [Observed, map-core §2].
- **The SoR proper** (`sor` DB on `cesium`, 10.1.0.200) is normalized truth: entities, IPs, DNS, hardware, inventory, with a provenance model (`source_id` / `asserted_kind` / `asserted_at`).

### 2.4 CDC-over-bus (SoR → the world)

The SoR fans out to its render targets through **change-data-capture over a message bus** [Observed, map-ops]:

```
SoR write ──► sor_outbox + AFTER triggers ──► sor_emitd ──► RabbitMQ (sor.events)
                                                                 │
              ┌──────────────────────────────────────────────────┼───────────────┐
              ▼                          ▼                        ▼               ▼
         DNS applier                 AD applier             Pi-hole applier   (future appliers)
      (BIND zones via netdb)     (Samba AD records)       (resolver/filter)
```

At-least-once delivery; idempotency via debounce + **full-rerender-and-diff** at each applier. The reverse direction (`sor_reconcile_discovery.py`) folds discovery observations back into the SoR, but **only for rows already `status='adopted'`** — discovery cannot silently create authoritative entities.

---

## 3. The C control-plane core

*Source: `src/client`, `src/monitor`, `src/server`, `include/`, `lib/solari`, `db/`. Standard C99, CMake-built, zero-dependency by default with every optional subsystem behind an opt-in flag.*

### 3.1 Build system & opt-in subsystems

CMake gates each capability so the base client compiles with no third-party deps [Observed, map-core §5, `CMakeLists.txt`]:

| Flag | Pulls in |
|---|---|
| `SOLARI_WITH_IO` | nng + mbedTLS (network transport) |
| `SOLARI_WITH_SQLITE` | store-and-forward spool |
| `SOLARI_WITH_JSON` | cJSON |
| `SOLARI_WITH_DISCOVERY_TOOLS` | nmap / avahi enrichment |
| `SOLARI_WITH_SNMP` | net-snmp |

This is what makes the "one small C client across every CPU/libc" goal achievable — a Pi Zero client links almost nothing.

### 3.2 Shared library (`lib/solari`, `include/`)

| Module | Responsibility |
|---|---|
| SCP frame/TLV codec | Wire encode/decode, CRC-32 framing |
| `solariJson.c` | cJSON wrapper; note `\uXXXX` escape handling at `:126` |
| crypto | mbedTLS context setup, mutual auth, cert load |
| config | shared `.conf` key=value parser |
| log / time | leveled logging; monotonic clock helpers |
| spool (SQLite) | store-and-forward, exponential backoff 1s→60s |
| reporter | push-or-spool abstraction used by client + monitor |
| net | nng transport wrapper (push/pull/pub/sub/req/rep) |

### 3.3 `solariClient`

Per-host agent. Only `platLinux.c` implements the platform-abstraction layer `platOS.h`; **other PAL backends are absent/stubbed** — the abstraction exists for a future cross-platform client but only Linux is real today [Observed, map-core §2/§5]. Reliability pattern: a **re-exec'd watchdog sibling** (`--watchdog-of PID`).

### 3.4 `solariMonitor`

Linux-only probe tier. Round loop [Observed, map-core §2, `src/monitor/main.c`]: gossip tick → prune dead peers → build fleet view → `runRound` (probe HRW-owned targets) → report send → control poll (which doubles as the inter-round sleep in reporting mode).

- **Config defaults** (`monitorConfigDefaults`): 30 s rounds, 5 probes, 1 s timeout, replication factor 2. `MONITOR_MAX_TARGETS = 64`; `monitorAddTarget` is idempotent by `targetId` and returns `ERR_BUFFER_FULL` at the cap.
- **Probing** (`probeNet.c`): hand-rolled BSD-socket TCP/UDP/ICMP plus app-layer checks (`appCheckDns/Ldap/Http/Mysql/Amqp`); 64 RTT samples; unprivileged ICMP via `SOCK_DGRAM` with `SOCK_RAW` fallback. **`probeLinkToPeer` is an unmarked stub — returns a zeroed struct** [Observed, map-core §2/§6].
- **Gossip** (`monitorGossip.c`): PULL listener + lazily (re)dialed PUSH sender exchanging `SCP_MSG_PEER_ALIVE`.

### 3.5 `solariServer`

Single-threaded ingest + control. Run loop [Observed, map-core §2, `src/server/main.c`]: lease tick → drain ingest (`SERVER_INGEST_BURST=64`) → service control (REP) → operator bridge.

Selected translation units and their notable properties [Observed, map-core §2]:

| File | Lines | Notes / flags |
|---|---|---|
| `serverDb.c` | 2432 | MariaDB access, mostly prepared statements. `conns[16]` declared but `dbConn()` always returns `conns[0]` — **vestigial pool** (BUG-C2). `MYSQL_OPT_RECONNECT=1`. |
| `serverIngest.c` | — | Cert-CN → role auth; 256-source × 1024-deep dedup ring; dispatch. |
| `serverControl.c` | — | Builds CONTROL/SURVEY; parses CONTROL_RESULT. **No correlationId validation** `:250-293` (BUG-C3); **missing `TLV_ERROR_CODE` treated as success** `:238-246` (BUG-C4); static, non-persisted `seqNo`. |
| `serverProvision.c` | 769 | Enrollment state machine + CSR signing. **4 self-documented CONTRACT GAPS at `:742-769`** (BUG-C6). |
| `serverLease.c` | 340 | Active/standby failover. **Lease-win-but-bind-fail orphan risk** (BUG-C7). |
| `serverMaster.c` | 325 | HELLO/WELCOME + health fusion. **`node.state` overloaded** for liveness *and* role (BUG-C8). |
| `serverScan.c` | — | Native connect()-scan; `SCAN_MAX_HOSTS=4096` enforced `:111-116` (the documented /20 discovery cap). |
| `serverSnmp.c` | — | `snmpbulkwalk` shell-out. **Community defaults to `"public"`** `:121` (BUG-C9); **dead first-pass loop** `:248-271` (BUG-C10). |
| `serverTopology.c` | 1271 | TOPOLOGY_REPORT ingest + LLDP-over-SNMP. **Node self-reports its own `networkGear.mgmtIp` with no cross-check** (BUG-C11). |
| `solariCtl.c` | 1381 | AF_UNIX admin bridge (see §3.6). |

### 3.6 The `solariCtl` local bridge — the PHP↔C trust boundary

This is the **single most security-relevant seam in the system** and deserves a reviewer's close attention, because the entire dashboard security model rests on the claim "PHP holds no CA material and performs no authoritative writes; everything goes through solariCtl." [Observed, map-core §2 + map-api §4.]

- **Transport:** AF_UNIX socket (default `/run/solari/solariCtl.sock`), one-line text request → one-line reply (`VERB k=v…\n` → `OK [k=v…]` / `ERR <code> <msg>`). The PHP side `rawurlencode`s any value with non-token chars before framing.
- **RBAC:** `ctlCheckRbac` at `:392-404` is **attribution-only** — for a destructive verb it checks only that an `op=<string>` argument is *present and non-empty*; it never looks the named operator up against any user/role/permission table (there is no ACL anywhere in the C tree) (BUG-C12). The file header states this intent plainly: "The PHP tier authenticates the human and adds this field." So the effective posture is **"PHP gates, C attributes."** If the AF_UNIX socket were reachable by anything other than the trusted PHP process, this "RBAC" provides *zero* access control. The repeated in-code claim that solariCtl is the authoritative enforcement point is **not literally true**. [Confirmed by both C passes.]
- **Inbound read buffer is unframed and single-shot (BUG-C18 — confirmed, was an open question).** `CTL_REQ_CAP = 4096` (`:114`), `CTL_REPLY_CAP = 16384` (`:116`), over an `AF_UNIX SOCK_STREAM` socket (`:1034`). The read path `ctlServiceOne` (`:1118-1146`) is a **single, non-looping `read()`** — no loop-until-`\n`, no length prefix, no "line too long" detection, no check that the read ended on a newline. Two concrete failure modes: **(a)** a `cfg=<json>`/`payload=<blob>` value pushing the request past 4095 bytes is **silently truncated** into a missing/corrupt key with no error; **(b)** on `SOCK_STREAM`, if PHP's single logical write lands as multiple kernel writes, one `read()` returns only the first fragment, `ctlHandleLine` processes a syntactically-incomplete line as complete, and the remainder is dropped at `close(cfd)` (`:1160`). `CTL_REPLY_CAP` bounds only the *outbound* signed-cert reply and does nothing for this. This is exactly the seam both the PHP and C passes independently flagged for `/api/config` and `/api/nodes/{id}/config` POSTs.
- **CONTROL verb range-check gap (BUG-C13, now precise).** The CONTROL handler (`:906-925`) bounds the numeric verb only to `1–255` (`verbNum == 0 || verbNum > 0xFF`), then casts straight into `serverControlBuild`. But the `solariCtrlVerb` enum (`solariMsg.h:150-159`) defines only **9 legal values** (`CTRL_SET_CONFIG=1` … `CTRL_ADOPT_TARGET=9`). Values **10–255 pass the check** and broadcast a syntactically-valid but semantically-undefined directive to the fleet — safety then depends entirely on each receiver's default-case handling (`clientControl.c`/`monitorControl.c`), which should be confirmed to reject unknown verbs.
- **DECOMMISSION confirm-token is re-derived, not stored** `:1370-1381` (BUG-C14) — if the "one-time" token is deterministically recomputed rather than issued-and-stored, it is predictable/replayable rather than a genuine nonce. The PHP side implements a two-round-trip confirm-token handshake (§5.1) that assumes the C side treats the token as a real secret; **this assumption should be verified.**
- **CSR signing fails closed** — the real signer (`:80-92`) compiles in only with `SOLARI_HAVE_MBEDTLS_X509`; absent it, `serverCtlSignCsr` returns `ERR_TLS` rather than minting an unsigned cert. The CA **private key** is touched only here — never in PHP — and in-memory path copies are `memset`-zeroed on close (`:1108-1113`). This is the strong half of the trust model that offsets C12.
- **`caMode=remote` not implemented** `:1211` (BUG-C15).

### 3.7 Ownership model — HRW / rendezvous hashing

Coordinator-free target ownership [Observed, map-core §2, `src/monitor/monitorPeer.c`]:

- **Weight function:** `hrwWeight = FNV-1a-64(targetId ‖ nodeId)`.
- **Ownership test** (`monitorOwnsTarget`): count peers with a higher weight for this target; this monitor owns the target **iff** that count `< replFactor` (k). This yields **k-of-n redundancy** (default k=2) as a pure function of `(target, node)` — no election, no coordinator.
- **Peer churn:** swap-remove prune on peer death; a TTL-pruned peer registry fed by `PEER_ALIVE` gossip (`monitorGossip.c`) rebuilds the fleet view each round. The server keeps an identical mirror (`serverProvision.c:167-197`, `provHrwWeight`/`provHrwTopOwner`) so it can predict the same owner a monitor would compute when dispatching adoption.
- **Dead-man's-switch — located and confirmed implemented, but *not* in the C core.** It lives in `deploy/alertbridge/alertbridge.py` (a Python systemd daemon on xenon), which is why the C-tree reads found nothing — `serverAlert.c` only does rule-severity mapping. `dead_mans_switch()` (`alertbridge.py:414-488`) fires strictly on *age of last sample vs. a threshold*, never on a sampled value. Full mechanism, threshold formula, emission path, and maintenance-window suppression are documented in §9.1. Tracked as INC9 in §11 (now **resolved**).

> **Reviewer flag — the central caveat, refined.** The HRW machinery is not stubbed: multi-monitor ownership is **fully implemented and functional when the `MONITOR_WITH_REPORTING` build is enabled** (`src/monitor/main.c:132-186` wires gossip); without that flag, or with no `gossipUrl` configured, a monitor degrades *by design* to fleet-of-one and owns all its targets (`main.c:187-201`). So the gap is **deployment/registration, not implementation** — production runs no populated monitor mesh, so true k-of-n HRW is **not exercised at scale today** even though the code path exists and works. Treat the ownership model as *coded and functional, not yet proven under a real multi-monitor fleet*.

### 3.8 Data model & migrations

- **Migrations** live in `db/migrations/`, numbered 001–019 [Observed, map-core §3].
  - **001** is the baseline and is also the *only* thing reflected in `db/schema.sql` — **`schema.sql` is stale**; the true current shape is 001 + all migrations applied in order (BUG-C17).
  - **There is a genuine duplicate `016`** — `016_panel_control.sql` and `016_push_subscriptions.sql` both claim the number (independent branches). Currently inert (disjoint tables, both `CREATE TABLE IF NOT EXISTS`), but there is **no migration runner enforcing uniqueness/order** — a latent landmine (BUG-C16).
  - **The highest migration present is `019` (`019_panel_screen_config.sql`); there is no `020`.** RackWire's migration-020 backend (per operational memory) is **not in this tree** as of the 2026-08-09 scan — see §8.3.
- **Retention:** `db/maintenance/retention.sql` + `roll_history.sql`, but **`gearInterfaceHistory` is not wired into the roller** — unbounded growth for that one table (BUG-C16b).
- **Seed:** `db/seed/demo.sql`.

---

## 4. Dashboard backend — the PHP API

*Source: `dashboard/api/**`. PHP 8+, no framework, no build step, camelCase house style. Served **live** from the repo via php-fpm `SCRIPT_FILENAME` — **every source edit is production.** [Observed, map-api.]*

### 4.1 Shape & conventions

- **Uniform envelope:** `{ok:true,data,ts}` / `{ok:false,error:{code,message}}` (`lib/Response.php:21,37`).
- **Global auth gate:** the front controller (`index.php:40-49`) requires an established session for every path except `/api/auth/*`; the router itself does not enforce auth.
- **Minimal router** (~130 lines): regex `{name}` capture, first-match-wins, explicit 404-vs-405, path normalization anchored on the first `/api/` substring to survive SAPI mount quirks.
- **Wire discipline:** raw units only (KB, micros, permille); all display formatting pushed to the SPA. **BIGINT ids are coerced to strings** (`Coerce::id`) because FNV-1a-64 ids overflow JS `Number.MAX_SAFE_INTEGER`.

### 4.2 Complete endpoint catalogue

**Auth (the only unauthenticated group), `routes/auth.php`:**

| Method | Path | Purpose |
|---|---|---|
| POST | `/api/auth/login` | username/password → session |
| POST | `/api/auth/logout` | tear down session |
| GET | `/api/auth/config` | login-screen capability probe (SSO on/off, directory block; non-secret) |
| GET | `/api/auth/oidc/login` | 302 to Keycloak authz |
| GET | `/api/auth/oidc/callback` | IdP redirect target, establishes session |
| GET | `/api/auth/whoami` | SPA boot check (session) |

**Read layer (`routes.php`, GET, viewer role unless noted):**

| Path | File:line | Purpose |
|---|---|---|
| `/api/summary` | `summary.php:15` | header-tile counts + lease state |
| `/api/panel` | `panel.php:292` | LED-panel 5 s snapshot; role-gated extra fields |
| `/api/nodes` | `nodes.php:16` | fleet roster + 40-sample sparkline |
| `/api/nodes/{id}` | `nodes.php:76` | node detail: metrics, procs, alerts |
| `/api/nodes/{id}/history` | `nodes.php:176` | metric time series (whitelisted column) |
| `/api/probes` | `probes.php:21` | reachability matrix, per-vantage rollup |
| `/api/alerts?status=` | `alerts.php:32` | alert events (active/history/all) |
| `/api/rules` | `alerts.php:98` | alert rule list |
| `/api/maintenance?status=` | `maintenance.php:29` | maintenance windows |
| `/api/opie`, `/api/opie/{reportId}` | `opie.php:15,63` | AI on-call reports (read-only) |
| `/api/topology?view=monitoring\|network` | `topology.php:106` | dual-hierarchy graph |
| `/api/segments`, `/api/netgear` | `topology.php:16,58` | network segments / gear inventory (legacy/basic) |
| `/api/gear`, `/api/gear/{id}/interfaces`, `/api/gear/{id}/history` | `gear.php:22,60,93` | SNMP gear roster + interface counters/series |
| `/api/discovery?status=` | `discovery.php:22` | discovered hosts + LLDP neighbor + tombstone flag |
| `/api/forgejo` | (prior) | Forgejo repo/commit/PR summary, fail-soft |
| `/api/ca` | (prior) | step-ca + local mTLS CA inventory, fail-soft |
| `/api/identity` | (prior) | Keycloak realm summary, fail-soft |
| `/api/push/*` | `push.php` | Web Push subscribe/VAPID (VAPID pubkey hardcoded) |
| `/api/enrollments?status=` | `provisioning.php` | enrollment tokens (csrPem never returned) |
| `/api/builds` | `provisioning.php` | build-artifact registry + version distribution |
| `/api/inventory/*` | `inventory.php` (1260 L) | physical asset/rack inventory |
| `/api/inv-codes/*` | `inv_codes.php` (1039 L) | barcode/label codes (must load after inventory.php — reuses `Inv`) |
| `/api/dns/*` | `dns.php` (561 L) | DNS zone views, fail-soft |
| `/api/rackwire/*` | `rackwire.php` (1272 L) | connection planner; **writes `Sor::db()` directly** (exception, §5.3) |

**Mutation layer (`routes_mutations.php`, POST + SSE):**

| Path | File:line | Purpose / gating |
|---|---|---|
| `/api/discovery/scan` | `discovery_mut.php` | CIDR-validated scan → `DISCOVER` |
| `/api/discovery/{id}/adopt`\|`/ignore` | `discovery_mut.php` | → `ADOPT` / `IGNORE`; adopt mirrors `Sor::upsertHost()` |
| `/api/enrollments/{id}/approve` | `enrollments_mut.php` | **destructive** (C signs CSR); operator + confirm |
| `/api/enrollments/{id}/reject` | `enrollments_mut.php` | **no role gate** (BUG-P2) |
| `/api/control/provision` | `control.php:20` | → `PROVISION`; **no explicit PHP role gate** (BUG-P3) |
| `/api/control/decommission` | `control.php:55` | **irreversible**; operator + confirm + bridge confirm-token + non-empty `wipeScope[]` |
| `/api/nodes/{id}/retire` | `control.php:97` | → `RETIRE`; operator + confirm |
| `/api/nodes/{id}/criticality` | `control.php:115` | tier 0–4 → `CRIT_SET`; operator |
| `/api/control/survey` | `control.php:133` | → `SURVEY`; **no explicit PHP role gate** (BUG-P3) |
| `/api/control/deploy` (POST) / (GET) | `control.php:150,173` | remote agent deploy → `DEPLOY` (operator) / tail log (viewer) |
| `/api/control/fleet-catalog` | `control.php:197` | provisioning reference data |
| `/api/control/fleet-provision` (POST/GET) | `control.php:236,304` | bare-metal PXE → `FLEET_PROVISION`; tail log |
| `/api/control/fleet-image` | `control.php:275` | Pi image build → `FLEET_IMAGE` |
| `/api/config` (GET/POST) | `config.php` | global config; POST validates → `CONFIG_SET` |
| `/api/nodes/{id}/config` (GET/POST) | `config.php` | per-node desired config, epoch bump → `PROVISION` |
| `/api/rules/{id}` / `/delete` | `config.php` | whitelisted rule edit → `RULE_SET` / `RULE_DEL` |
| `/api/pools` (GET/POST), `/api/pools/{id}`, `/delete` | `pools.php` | pool CRUD → `POOL_NEW/SET/DEL` (poolId=1 protected) |
| `/api/assets[...]` (many) | `assets.php` | asset detail/patch/remove/lifecycle/purge/criticality/target-remove |
| `/api/alerts/ack`, `/ack-all` | `alerts.php:120,148` | **direct `Db::exec`, not solariCtl** (dashboard-layer concept, BUG-P4) |
| `/api/maintenance` (+`/{id}/cancel`) | `maintenance.php:77,158` | **direct `Db::exec`** on solarinet DB (BUG-P4) |
| `/api/panel/command` | `panel.php:772` | queue panel CONTROL; 16-cap via `FOR UPDATE`; rejects `panel` principal |
| `/api/panel/state`, `/config` | `panel.php:826,889` | panel daemon reports; **only** the `panel` service principal |
| `/api/stream` | `stream.php:41` | **SSE**, ≤50 s bounded poll bridge, 3 event types; `session_write_close()` immediately (fixed a lock-starvation incident 2026-08-05) |

The blast-radius-scaled confirm ladder is worth calling out: simple destructive ops need role+`confirm:true`; **decommission** needs role+confirm+a bridge-issued one-time token (two round trips)+non-empty wipeScope; **purge** needs `admin` + exact `confirmName` string match (documented as "UX, not authority" since C re-verifies).

### 4.3 Auth & RBAC [Observed, map-api §2]

- **Local mode:** `lib/Auth.php`, bcrypt creds in `solari-auth.json` (project root, 0600), PHP session (`solari_sess`, HttpOnly, SameSite=Lax, Secure on HTTPS), `session_regenerate_id(true)` on login. `SOLARI_LOCAL_LOGIN=1` gates whether local login UI shows; the endpoint remains as a break-glass path.
- **OIDC/Keycloak SSO:** `lib/Oidc.php`, additive + config-gated. **Hand-rolled RS256/384/512 JWT verification** via ext-openssl (JWK→PEM built manually, no library) — discovery + JWKS fetched with `verify_peer=true` (secure, with an explicit "do not disable" comment). State+nonce CSRF/replay protection. Group/role claims map to admin/operator/viewer.
- **Directory (LDAP/AD):** `Auth::directoryAuthenticate()` is a **non-functional, fail-closed stub** (`Auth.php:141-152`, explicit `TODO(next pass)`). This is the one genuine, connected piece of incompleteness in the PHP tier.
- **RBAC:** `lib/Operator.php` — viewer/operator/admin resolved **only** from the session principal (an explicit hardening fix over an earlier design that trusted spoofable `REMOTE_USER`/`SOLARI_ROLE` headers).
- **Service principal quirk:** a local user literally named `panel` (role viewer) is granted write to 3 panel endpoints **by identity check**, not role — effectively "role + identity-based feature flags," an ad-hoc exception to the 3-role model [Inferred, map-api §5].

### 4.4 Data access [Observed, map-api §3]

Two DBs, two clients: `lib/Db.php` → control-plane `solarinet` DB; `lib/Sor.php` → `sor` DB on cesium. Every query is a bound prepared statement; **user-influenced column/metric names are always whitelist-mapped** before interpolation. A **feature-detection pattern** (`information_schema` probes, per-request cached) supports rolling migrations without downtime — but it is fragile: a **trailing-comma variant of this exact pattern took `/api/panel` down for 5 hours on 2026-08-06**, now fixed by mandating leading commas (BUG-P5; the pattern recurs in `nodes.php`/`assets.php`/`discovery.php` and each site should be audited). `/api/panel` wraps its reads in `START TRANSACTION WITH CONSISTENT SNAPSHOT, READ ONLY` for a coherent point-in-time.

---

## 5. Dashboard backend — design decisions & the read/write split

### 5.1 The central invariant [Observed, map-api §5]

**GET routes are SELECT-only against MariaDB; POST routes delegate authoritative writes to the C bridge over solariCtl.** The stated payoff: PHP holds **no CA/cert material** and writes **no monitoring tables**. This is the security spine of the dashboard.

### 5.2 …and its documented exceptions

The invariant is not literally total, and the exceptions are the reviewer's checklist:

- **(a)** `alertEvent.ackedAt/ackedBy` and `maintenanceWindow` CRUD are dashboard-layer concepts written **directly via `Db::exec`** (BUG-P4). Narrow and documented, but it means "solariCtl is the sole writer of privileged state" is not literally true.
- **(b)** `inventory.php` / `inv_codes.php` / `rackwire.php` write the **SoR directly** via `Sor::db()`, treating the SoR as authoritative rather than a mirror (§5.3).
- **(c)** On the C side, the enforcement the invariant leans on is **attribution, not authorization** (BUG-C12). The real enforcement is in PHP (§4.3). A reviewer should decide whether that is an acceptable trust model or a latent hole if a future non-PHP caller ever reaches the socket.

### 5.3 Other deliberate choices [Observed, map-api §5]

- **php-fpm runs as the repo owner (`jason`), not `www-data`** — so PHP can read the 0600 credential file and the jason-owned AF_UNIX socket without loosening perms.
- **Fail-soft for every external integration** (Ca/Forgejo/Identity/DNS reads/Sor mirror writes) vs **fail-hard** for SoR-as-authoritative writes (inventory family).
- **SSE is synthesized**, not a true bridge subscription: `stream.php` tails MariaDB tables (new `alertEvent` rows, `node.lastSeenAt` advances, `probeCurrent` samples) since a resume cursor, because solariCtl has no PUB/SUBSCRIBE verb reachable from PHP without a persistent worker. This is the seam to replace when solariCtl grows SUBSCRIBE.

### 5.4 Integrations exposed by the PHP tier [Observed, map-api §4]

`lib/SolariCtl.php` (the bridge); `lib/Ca.php` (step-ca, **TLS verify disabled** — BUG-P1); `lib/Forgejo.php` (read-only, fail-soft); `lib/Identity.php` (Keycloak admin, `client_credentials`, TLS verify **on**); `lib/Dig.php` (DNS via `dig` proc_open fan-out, server whitelist mandatory); `lib/Label.php` (barcode via `tools/label_render.py`, bytes-only, transport unbuilt); `routes/push.php` (Web Push).

---

## 6. Dashboard frontend

*Source: `dashboard/public/**`. React/JSX **transpiled in-browser** via vendored Babel-standalone. No build step, no bundler, no server-side JS. [Observed, map-ui.]*

### 6.1 The no-build mechanism

`index.html:22-44` loads vendored React + Babel-standalone and every module as `<script type="text/babel">` in a **hand-maintained load order**. Modules are IIFEs that hang exports off `window`; there is a global `window.SOLARI` data object and client-side routing via a route-state switch. A PWA service worker (`sw.js`, `CACHE_VERSION solari-v5`) provides offline caching.

> **Reviewer flag:** the hand-maintained load order is real coupling — it exists in **three places** and a mismatch caused a stale-cache incident (`abcd00b`). `inv_codes.php`'s client counterpart must load after `inventory.php`'s. This is the structural cost of the no-build choice (§9.3 explains why the choice is nonetheless deliberate and defensible).

### 6.2 Component/module inventory [Observed, map-ui]

`app.jsx` (shell + routing), `screens.jsx` … `screens8.jsx` (feature pages, accreted over time), `screens-panel.jsx`, `screens-rackwire.jsx`, `layout.jsx`, `components.jsx`, `icons.jsx`, `data.jsx`, `api.jsx` (the sole GET/POST adapter; subscribes to the 3 SSE event names; polls `/api/panel` at 5 s).

### 6.3 Feature catalogue

Fleet monitoring, reachability matrix, dual-view topology, discovery/adopt, provisioning (incl. bare-metal), config editing, Opie (AI on-call), lifecycle + 5-tier criticality, physical inventory, maintenance windows, Git (Forgejo) view, CA view, DNS view, barcode/label, panel control, RackWire planner, command palette, theme switching, SSO login, PWA/offline.

### 6.4 Design system

- **Keystone rule:** status hues (green/amber/red) are **reserved for status** and never used decoratively — the one inviolable color rule (§9.5).
- **Quiet-healthy:** a healthy fleet is visually calm; color enters only when attention is warranted.
- **Self-hosted IBM Plex** (no CDN — consistent with no external deps).
- **Dark-first dual theme.**

### 6.5 Frontend risks [Observed, map-ui]

- Dead `PlannedPage` code.
- 3-place load-order coupling (above).
- Unguarded `localStorage.getItem` in places.
- **Accessibility gaps:** zero ARIA in `screens4/6/7`.

---

## 7. Status-panel subsystem

*Source: `firmware/` + panel daemon. A Galactic Unicorn 53×11 RGB LED matrix on an RP2350. [Observed, map-panel.]*

### 7.1 Shape

Three binaries + a shared `protocol.c`: the **daemon** `solariPanel.c` (233 L) bridges dashboard↔serial; the **firmware** `main.c` (654 L, 40 ms tick); shared `protocol.h/.c` (297/215 L). A separate UNO Q 8×13 "glance" heartbeat panel is out of this subsystem's scope.

### 7.2 Wire protocol

Length-framed with a **strict version-equality byte** (mismatched versions **hard-fail**, deliberately) plus v1-additive trailing extensions; CONTROL kinds 1–9; SNAPSHOT / STATE / CONFIG / HELLO / EVENT / LOG frames; **CRC16-CCITT + magic-byte resync**. The dashboard↔daemon leg uses the DB-table protocol (`panelCommand`/`panelState`/`panelScreenConfig`), polled every 5 s (§4.2), *not* solariCtl.

### 7.3 Features

Screens A/B/C/D × 3 = 12 views; per-pool alarm score, worst-wins; flash persistence with 2000 ms debounce (requires `PICO_FLASH_ASSUME_CORE1_SAFE` — a subtle build flag whose omission silently refused flash writes); a Vol+ help view.

### 7.4 Design rationale & risks

- **Fail-dark**, **consumption ≠ application** for CONTROL (a command being read off the queue is distinct from it being applied), **SOURCE_DATE_EPOCH reproducible builds**, C++ confined to `panelHw.cpp`.
- **Risks:** a residual false-resync path in the protocol; the daemon does **no kind-range check** on incoming CONTROL ("R3"); an A1 ordinal-underflow (fixed, found via ASAN); link-flapping (fixed); the version-skew hard-fail is intentional but operationally brittle across firmware upgrades; `panelHist.c` / `main.c` have thin test coverage.

### 7.5 Tab5 (`firmware/tab5/`) — explicitly not production

A separate ESP32/IDF password-manager/TOTP/BLE-HID device sharing the repo. Heavily TODO-marked: `blehid` stubbed, `mqttbus` CA-pinning is a TODO (currently insecure), `vault` Argon2 is a TODO. **Not production-ready; should be reviewed as a prototype, or scoped out of the review entirely.**

---

## 8. Deploy, integrations, netdb & RackWire

*Source: `deploy/**`, `netdb/`. Python-for-glue is an explicit, bounded exception to the C/PHP language policy; systemd-uniform; fail-soft idiom; atomic state files; provenance-first. [Observed, map-ops.]*

### 8.1 Service inventory

`alertbridge`, `authbroker`, `backups`, `discovery`, `dns-*`, `enrollment`, `fleet`, `maintenance`, `nfc-2fa`, `notify`, `opie`, `sorsync`, `unifi`, `mcp`, `netdb`, RackWire — each a systemd unit.

### 8.2 Integration map & risks

- **UniFi API:** `skip-verify` is the **default** (BUG-O2) — flagged for reviewers; also `unifipolld` has exception-handling gaps, a 60 s freshness overrun, and unverified TLS.
- **RabbitMQ:** three uses (sor.events CDC, notify.events, alert bridge).
- **Keycloak/authbroker:** `authbrokerd.py:288-291` — **HTTP auth open-by-default** (BUG-O1), the highest-severity ops finding.
- **Opie:** `opied.py` executes **LLM-triggered shell** (BUG-O3) — the AI on-call can run commands; the guardrails here need a hard look.
- **MCP:** a **regex-based SQL guard** (BUG-O4) — regex is a weak boundary for SQL safety.
- **DNS (netdb):** `load_source()` is the single seam between the SoR/YAML source of truth and `gen-zones.py` → BIND zones; **monotonic serial discipline** is required for AXFR (a serial-reset bug previously stopped secondaries from transferring). Secondaries: `radium` + `steel`.
- **Cross-cutting:** the fail-soft / atomic-write / provenance patterns are **copy-pasted** across services rather than shared, so a fix to one doesn't propagate (BUG-O5 class).

### 8.3 RackWire

Client-side rack cabling/power planner. Backend = migration 020 + `rackwire.php`. **Confirmed: migration 020 is NOT in `db/migrations/` as of 2026-08-09** (a fresh scan found nothing matching `02*`; highest is 019). `rackwire.php` (1272 L) *is* present in the PHP tree and writes `Sor::db()` directly (§4.2/§5.2). So the RackWire schema and its API code are **out of sync in this checkout** — either the migration hasn't landed on this branch or it lives elsewhere. A reviewer of RackWire should treat its server side as *deployed-but-not-schema-tracked-here* and confirm the live SoR shape independently.

### 8.4 No-CSRF, repo-wide

There is **no CSRF protection anywhere** in the dashboard [Observed, map-ops]. Mitigated somewhat by SameSite=Lax session cookies and the operator-only threat model, but it is a genuine repo-wide gap a reviewer should weigh.

---

## 9. Design & UI rationale (why it is the way it is)

### 9.1 "Socket-open is a lie" — the honesty goal, written in blood

On 2026-07-07, `cesium`/Forgejo was silently down for ~26 h while every naïve check read "green" (the port answered). The remediation defined the health philosophy [Observed, `docs/ARCHITECTURE.md` + map-quality]: **host-health telemetry** (the client reports what it actually sees), **HTTP status-aware probes** (200 ≠ merely "connected"), the **dead-man's-switch** (silence → synthetic crit), an **alert→MQ→Apple push bridge**, and nightly backups. Honesty is goal #1 because its absence already cost a day of blindness.

**The dead-man's-switch — where it actually lives** [Observed, dedicated read of `deploy/alertbridge/alertbridge.py`]. It is deliberately *outside* the C control plane: a Python daemon, `deploy/alertbridge/alertbridge.py`, run as a systemd unit on xenon (`alertbridge.service`, `Restart=on-failure`, `RestartSec=5`). This placement is why the C reviewers found nothing — and is itself a design choice worth noting (the honesty backstop is a separate process from the thing being watched).

- **Trigger** (`dead_mans_switch()`, `alertbridge.py:414-488`): fires strictly on *age of last sample*, never on a value. `node_ages()` (`:378-411`) computes `TIMESTAMPDIFF(SECOND, sampledAt, UTC_TIMESTAMP())` over `hostCurrent.sampledAt` (clients) and `MAX(probeCurrent.sampledAt)` per monitor (vantages), joined on `node.hostFqdn`; when a node appears in both, the freshest age wins. A never-seen node cannot trip it (`:382-383`).
- **Threshold** (`:507-509`): `max(120, 3 × sample_interval_sec)`, where `sample_interval_sec` is a single bridge-wide config knob (`[deadman]` in `alertbridge.conf`, default 15 → 120 s floor). **Not per-target/per-node** — a reviewer-relevant limitation for a fleet with heterogeneous reporting cadences.
- **Emission** (`:445-452`, `:162-182`): publishes to RabbitMQ topic exchange **`notify.events`**, routing key **`notify.crit`** — with publisher confirms, and only marks a node "silent" in a persisted JSON state file *after* the broker confirms (`:453-465`). Recovery emits an `info` and re-arms (`:466-488`).
- **Cadence**: called every cycle of the daemon's main loop (`:558`), cadence = `[bridge] poll_interval_sec` (default 10 s).
- **Maintenance-window suppression** (ties to `db/migrations/014_maintenance_windows.sql:3-6,9-25`): `maintenance_targets()` (`:188-213`) reads `maintenanceWindow` rows `status IN ('scheduled','active') AND NOW() BETWEEN startsAt AND endsAt`; `_suppressed()` (`:215-224`) matches by nodeId or by IP embedded in a probe targetId; suppressed nodes are skipped and a pre-existing arm is dropped silently so it only re-fires after the window if still silent (`:431-439`).

> **Design observation for reviewers (not a bug):** the dead-man's-switch emits **only** to the push bus (`notify.events`/`notify.crit`), **not** to the `alertEvent` table. So a "node stopped reporting" event reaches the operator's phone but does **not** appear in `/api/alerts` (which reads `alertEvent`) or the dashboard alert history. Whether the flagship honesty signal should be invisible in the dashboard's own alert view is a legitimate design question to put to the reviewers.

### 9.2 Why C, length-prefixed binary TLV, and nng [Observed, `docs/ARCHITECTURE.md:31-40`]

- **Binary TLV over JSON/HTTP:** unambiguous framing (length prefix — no delimiter escaping, no read-until-newline on binary), forward/backward compat (unknown tags skipped by length — essential for a piecemeal-upgraded heterogeneous fleet), and cheapness on an SBC (no JSON parser, fixed-buffer encode). Frugality + portability are first-class.
- **nng:** survey/respond, pub/sub, req/rep as native patterns; a small C client links one library rather than reimplementing messaging.
- **Strict version-equality (panel) / epoch-monotonic convergence (fleet):** the fleet upgrades piecemeal, so the wire format must evolve *without* lockstep — TLV handles additive change, and where semantics can't be allowed to drift (the panel), the version byte hard-fails rather than guessing.

### 9.3 Why no build step / in-browser JSX [Observed, CLAUDE.md + language-policy]

Server-side JS and build steps are **forbidden by policy**. The payoff is that the dashboard is *the repo* — there is no artifact to build, stage, or drift from source; an edit is live. The cost (paid deliberately) is the hand-maintained script load order (§6.1) and in-browser transpile overhead. For a self-hosted, single-operator tool this trades build-time convenience for zero deployment surface — a coherent choice given the constraints, though a reviewer may reasonably challenge the load-order fragility.

### 9.4 Why coordinator-free HRW [Observed, `docs/ARCHITECTURE.md` + map-core]

Resilience goal #2: no coordinator whose loss stops the fleet. HRW makes ownership a pure function of `(target, node)`, so any monitor can independently compute who owns what with no election and no shared state — k-of-n redundancy falls out of "top-k scorers." The tradeoff (§3.7): the code is complete and functional under the `MONITOR_WITH_REPORTING` build, but production runs no populated monitor mesh, so it's *unproven at scale* — the gap is deployment, not implementation.

### 9.5 Why the keystone color rule [Observed, map-ui]

Status hues are reserved for status so that color *means* something — a calm screen is a healthy fleet, and any color is a signal, not decoration. This is what makes "quiet-healthy" legible at a glance and is the UI counterpart to the honesty goal.

---

## 10. Testing & verification

*Source: map-quality.*

| Layer | Harness |
|---|---|
| C core | Unity |
| Status panel | custom C harnesses |
| Dashboard JSX | Node scripts |
| PHP API routes | PHP scripts |

**How to run** is documented per layer. **Live-DB test gotcha:** `test_server_db_live` refuses `SOLARI_DB_NAME=solarinet`; it needs a `solarinet_stage` clone and `SOLARI_TEST_DB=1`.

**Coverage gaps a reviewer should weigh** [Observed, map-quality]:

- Most PHP API routes are **untested**.
- No framebuffer-parity fixture for the panel renderer.
- Deploy Python is **untested**.
- `main.c` (server) is in **no test binary**.
- **Two compiled test binaries are committed to git** (build artifacts in VCS).
- `panelHist.c` / panel `main.c` thin coverage (§7.4).

---

## 11. Known bugs & undiagnosed issues (consolidated, ranked)

Ranked by a blend of severity and blast radius. IDs are stable so reviewers can reference them. "Undiagnosed" items are marked ⚠.

### Security / correctness — high priority

| ID | Where | Issue |
|---|---|---|
| **C12** | `solariCtl.c:392-403` | RBAC is **attribution-only** — the C bridge audits but does not authorize. The dashboard's "solariCtl is the enforcement point" claim is not literally true; enforcement is really in PHP. Decide if that trust model holds. |
| **O1** | `authbrokerd.py:288-291` | authbroker **HTTP auth open-by-default**. |
| **O3** | `opied.py` | Opie executes **LLM-triggered shell** — review the guardrails. |
| **C4** | `serverControl.c:238-246` | Missing `TLV_ERROR_CODE` in a CONTROL_RESULT is **treated as success** — fails open, should fail closed. |
| **C3** | `serverControl.c:250-293` | CONTROL_RESULT **correlationId not validated**; with a static non-persisted `seqNo`, a restart can cause seqNo reuse and cross-talk. |
| **C14** | `solariCtl.c:1370-1381` | DECOMMISSION confirm-token **re-derived, not stored** — potentially predictable/replayable rather than a nonce. |
| **C18** | `solariCtl.c:114,1118-1146` | **Confirmed (was open in §14):** unframed single-shot `read()` on `SOCK_STREAM`, `CTL_REQ_CAP=4096`, no newline-completeness check. Large `cfg=`/`payload=` blobs **silently truncate**; a fragmented stream write **desyncs** the line protocol. Directly reachable from `/api/config` + `/api/nodes/{id}/config` POSTs. |
| **C11** | `serverTopology.c` | Node **self-asserts its own `mgmtIp`** into topology with no cross-check — false-topology injection path. |
| **P1** | `lib/Ca.php` | step-ca client **disables TLS verification** (deliberate for internal CA; MITM-on-segment could feed forged CA data to the UI). Contrast with Identity.php, which keeps verify on. |
| **O2** | UniFi client | `skip-verify` **default**; `unifipolld` also has unverified TLS. |
| **O4** | MCP | **regex-based SQL guard** — weak boundary. |
| **—** | repo-wide | **No CSRF protection anywhere** (§8.4). |

### Correctness / operational — medium

| ID | Where | Issue |
|---|---|---|
| **P3** | `control.php:20,133` | `/api/control/provision` and `/survey` use best-effort `Operator::name()` **not** `requireOperator()` — no PHP-side role gate (unlike every sibling). Enforcement relies entirely on the C bridge, which per C12 doesn't gate. **This pair is the most concrete "who can actually call this?" question in the review.** |
| **P2** | `enrollments_mut.php` | enrollment **reject has no role gate** (approve does) — any viewer can reject pending enrollments (availability/nuisance). |
| **C1** | `serverContext.c:116-119` | `controlUrl` and `surveyUrl` **both default to the identical literal `:7702`** for two different socket patterns (confirmed a duplicated hand-typed default, not an alias). ⚠ Runtime bind-conflict itself unverified — the listener-bind path wasn't traced. |
| **C13** | `solariCtl.c:906-925` | CONTROL numeric verb bounded only to `1–255`, but only **9 verbs are legal** (`solariMsg.h:150-159`); values 10–255 broadcast a semantically-undefined directive fleet-wide. Receiver default-case handling should be confirmed. |
| **C7** | `serverLease.c` | ⚠ **lease-win-but-bind-fail** leaves an orphan primary (lease held, service not bound); no reconciliation path noted. |
| **C8** | `serverMaster.c` | `node.state` **overloaded** for liveness *and* role — "down vs standby" ambiguity class. |
| **P5** | `panel.php` + others | feature-detection SQL fragment is **comma-placement-fragile** (caused a 5 h outage 2026-08-06); audit every reuse site. |
| **P4** | `alerts.php`, `maintenance.php` | direct `Db::exec` writes **bypass** the "PHP never writes privileged state" invariant (narrow, documented). |
| **C16** | `db/migrations/` | **duplicate `016`** with no runner enforcing uniqueness/order — inert today, latent landmine. |
| **C17** | `db/schema.sql` | **stale** (reflects migration 001 only) — do not bootstrap or document from it. |
| **C2** | `serverDb.c` | vestigial `conns[16]` pool (`dbConn()` always returns `conns[0]`) — misleading, not a live bug given single-threaded server. |
| **C9** | `serverSnmp.c:121` | SNMP community defaults to **`"public"`**. |
| **C16b** | `roll_history.sql` | `gearInterfaceHistory` **not wired into retention** — unbounded growth. |
| **P6** | `control.php:173`, `:304` | deploy/provision **log-tail GETs are viewer-accessible** — may leak internal hostnames/IPs/command output to non-operators. |

### Incompleteness (built-but-not-finished)

| ID | Where | Issue |
|---|---|---|
| **INC1** | `Auth.php:141-152` | LDAP/AD directory auth is a **fail-closed stub** (`TODO(next pass)`). |
| **INC2** | `probeNet.c` | `probeLinkToPeer` **unmarked stub** (zeroed struct). |
| **INC3** | `serverProvision.c:742-769` | **4 self-documented CONTRACT GAPS** in the enrollment state machine — surface verbatim to reviewers. |
| **INC4** | `serverSnmp.c:248-271` | ⚠ **dead/no-op first-pass loop** — confirm it isn't swallowing a real bulk-walk. |
| **INC5** | `solariCtl.c:1211` | `caMode=remote` **not implemented**. |
| **INC6** | monitor tier | **monitor-role registry / true k-of-n HRW not populated in production** (§3.7) — the biggest architectural gap. |
| **INC7** | `firmware/tab5/**` | Tab5 device broadly **stubbed** (blehid, mqtt CA-pinning, vault Argon2) — prototype, not production. |
| **INC8** | `lib/Label.php` | printer **transport unbuilt** (`LabelTransport` future). |
| **INC9** | ~~C tree (absent)~~ → `alertbridge.py:414-488` | **RESOLVED — implemented, not missing.** Dead-man's-switch lives in the `deploy/alertbridge` Python daemon on xenon, not the C core; confirmed firing on absence-of-data (§9.1). Two residual notes for reviewers (design, not defects): threshold is a **single bridge-wide knob**, not per-target; and it emits to `notify.events` **only**, not `alertEvent` — so silent-node events are invisible in `/api/alerts`. |

### Live-fleet / environmental (state as last observed, not code defects)

From map-quality's operational sweep (point-in-time; **verify against live state before acting**) [Unverified]: `benzene`/`steel`/`helium` down since a storm; a stray `lithium` DHCP lease; `chemistry` tier drifted 3→4; an alert episode re-arm P1; some tier-2 hosts unreachable. These are environment, not code, but they color which subsystems are currently exercisable.

---

## 12. Outstanding features (roadmap)

From the rationale doc's "known issues & pending capabilities" and map-quality's outstanding table [Observed, `docs/ARCHITECTURE.md:162-171` + map-quality]:

- **Monitor-role registry + true k-of-n HRW** (turn the coordinator-free design load-bearing) — top priority.
- **First-class decommission** lifecycle (beyond the current directive).
- **One-button provisioning** end to end.
- **Dual-hierarchy topology** as a first-class model.
- **Network segments** as first-class entities.
- **Durable C2 config** (persisted `seqNo`, survives restart — ties to C3).
- **Static-musl client tier** (portability endgame for exotic libc hosts).
- **Standalone WiFi mode** (#8), **webpush frontend port**, **audible alarm-ACK leg + tier-4 page test**, **removal of the temporary `optest` operator**, **PANEL_LUX calibration**, **VLAN support**, **DR Pi peers**, **app-layer probe expansion**, housekeeping.

---

## 13. Where a reviewer's effort is best spent

If your review time is bounded, these are the highest-leverage questions, in order:

1. **The solariCtl trust boundary (C12 + C18 + P3 + C14).** "PHP authorizes, C attributes" (C12) is only sound if nothing but PHP can reach the socket — and the inbound path is unframed/single-shot (C18), so it can be truncated or desynced by a large or fragmented request. Combined with two POST endpoints having *no* PHP-side role gate (P3), this whole seam deserves the most scrutiny in the review.
2. **CONTROL_RESULT correctness (C3 + C4).** Fail-open on missing error code + unvalidated correlation + non-persisted seqNo is a plausible source of silent convergence bugs — exactly the "green light is a lie" failure class the project exists to prevent.
3. **The dead-man's-switch's visibility split (INC9, resolved).** It *is* implemented (`alertbridge.py`, §9.1) — but it emits only to the push bus, never to `alertEvent`, so the flagship honesty signal is absent from the dashboard's own alert view; and its threshold is one fleet-wide knob rather than per-target. Both are design calls worth a second opinion.
4. **The unproven-at-scale ownership model (INC6).** Coded and functional under a build flag, but never run as a populated mesh. What breaks when the monitor tier is actually deployed?
5. **The no-CSRF + open-by-default-auth cluster (O1, O2, §8.4).** Operator-only threat model vs. reality of a browser tool on a LAN.
6. **Opie's shell execution (O3).** An LLM with a shell is the sharpest edge in the repo.

---

## 14. Provenance, method & limits of this pass

**Method.** This dossier synthesizes six independent read-only mapper passes over disjoint subsystems — C core; PHP API; JSX frontend; status-panel firmware; deploy/integrations/netdb/RackWire; tests/quality — plus a **seventh, independent second read of the C core** commissioned specifically to cross-check the first. Each produced a `file:line`-cited report, checked against the others and against `docs/ARCHITECTURE.md`. No code was modified. Where two passes independently reached the same finding, that convergence is noted in-text and raises confidence; where the second C pass **corrected** the first (fresh cited reads vs. session-memory recall), the corrected version is what appears above. Notable corrections the second pass forced: the dead-man's-switch is *not* in the C core (INC9), the solariCtl buffer-cap is a *confirmed* bug rather than an open question (C18), migration 020 is *confirmed absent*, and the monitor mesh is *functional under a build flag* rather than merely coded.

**Known limits — read these before trusting a specific claim:**

- **`serverContext.c:166-167` "copy-paste fallback"** was flagged by the first C pass from session memory and **not re-confirmed** by the second pass [Unverified] — re-open the file if it matters. (Migration one-liners 001–019, by contrast, were freshly re-read and are in §3.8-adjacent notes.)
- **`db/schema.sql` is stale** (C17) — never treat it as the current schema.
- **RackWire's migration-020 backend** is **confirmed absent** from this checkout (§8.3), while `rackwire.php` is present — the schema and its API are out of sync here; confirm the live SoR shape independently.
- **The "live-fleet / environmental" items (§11)** are point-in-time operational observations, not code-review findings, and will have drifted.
- **Where the dead-man's-switch runs (INC9) — RESOLVED** by a dedicated follow-up read: `deploy/alertbridge/alertbridge.py:414-488` (§9.1). The residual open items are design questions (push-only emission, single-knob threshold), not location uncertainty.
- **The solariCtl CONTROL-verb receiver behavior** (does `clientControl.c`/`monitorControl.c` reject verbs 10–255? — C13) was flagged from the sender side but the receiver default-case was not traced.
- **`solariServer` port/CLI/env surface** was not exhaustively enumerated (time-budgeted).
- Line numbers are accurate as of the 2026-08-09 tree; the dashboard is served live from the repo, so frontend/PHP line numbers can drift with any edit.

*End of dossier.*
