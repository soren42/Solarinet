# SolariNet System of Record (SoR) — Schema Design

Database: **`sor`** on cesium (10.1.0.200, MariaDB 12.3, InnoDB, utf8mb4).
DDL: [`schema.sql`](schema.sql) — validated end-to-end against a throwaway
`sor_val` schema on cesium (full load, idempotent re-run, view smoke test).
The current fully applied schema snapshot is [`schema.snapshot.sql`](schema.snapshot.sql),
generated from `schema.sql` plus every SoR migration by
`python3 tools/export_schema.py --database sor`. Install and upgrade through
`tools/migrate.py`; the snapshot is not the ledgered migration path.
Migrations 022–023 own the transactional outbox tables and CDC triggers used by
`deploy/sorsync`; fresh schemas must not install those objects through the old
standalone SQL bootstrap.
Likewise, `schema.sql` may declare the migration-028 association table, but the
registry is not operational until `tools/migrate.py` applies migration 028 and
installs its entity synchronization triggers. Never provision a fresh SoR by
running `schema.sql` alone.

The SoR is the authoritative store for the whole homelab: inventory,
network, identity, configuration, and current state. It is **never populated
by hand** — it is seeded from today's live systems of record and thereafter
flows bidirectionally:

```
 dashboard edit ──► SoR ──► renderers/appliers ──► network change
 discovery/sync ──► SoR ◄── monitoring detection
```

Everything downstream (BIND zones, AD reconciliation, Pi-hole config,
monitoring targets) becomes a *view rendered from the SoR*, exactly the way
`netdb/gen-zones.py` already renders zones from the interim YAML.

---

## 1. Entity-relationship model

### The supertype: `entities`

Everything the homelab tracks — server, workstation, network gear, appliance,
IoT device, peripheral, VM, container, application, service, vehicle — is one
row in `entities` (discriminator `entity_type`). Subtype detail tables hang
off it 1:1 where a type needs extra columns:

- `applications` — deployed software products (vendor, version, install kind)
- `services` — network functions (dns/dhcp/ldap/git/... + criticality)

Physical things link to the hardware/location axis; compute things link to an
OS; everything can link to an owner:

```
                         locations (self-tree: property>floor>room>section>storage_unit)
                              ▲
                              │
 hardware_models ◄── hardware_units ◄──┐
 (make/model/type)   (serial units)    │
                                       │            users ◄─── group_members ───► groups
 operating_systems ◄───────────────┐   │              ▲         (M:N + nesting)
                                   │   │              │ owner
                              ┌────┴───┴──────────────┴─┐
                              │        ENTITIES         │◄──────────────┐
                              │  (the supertype hub)    │               │
                              └───┬────┬────┬────┬──────┘               │
              1:1 subtype rows ───┤    │    │    │                      │
        applications, services ◄──┘    │    │    └──► monitoring_state (1:1 current health)
                                       │    │                           │
                                       │    └──► config_items (per-entity/scope k=v)
                                       │                                │
                                       └──► relationships (typed M:N graph:
                                            contains/runs_on/depends_on/member_of/
                                            connects_to/backs_up/manages/monitors/...)
```

### The network axis

```
 vlans ◄── networks ◄── subnets ◄── dhcp_ranges
   ▲                       ▲
   │                       │ (containment, app-maintained)
 interfaces ◄──────── ip_addresses ────► dns_records ───► dns_zones
   ▲   ▲ parent_interface_id  │ entity_id      │ target_entity_id
   │   └── (vlan/bridge/bond) ▼                ▼
 entities              entities            entities
   ▲
 interconnects (endpoint A/B = interface preferred, entity fallback;
                copper/fiber/moca/wireless/lldp_adjacency/logical)
```

- **`interfaces`** covers physical NICs, wifi radios, switch ports, and
  logical constructs (VLAN subinterface, bridge, bond) via `if_kind` +
  `parent_interface_id`.
- **`interconnects`** covers both patch cables (human-asserted, with
  label/length) and discovered LLDP adjacencies (machine-asserted from the
  monitoring DB) in one table, distinguished by `link_kind` + provenance.
- **`ip_addresses`** is the IPAM core: one live row per address
  (`UNIQUE(address, deleted_at)`), typed by `assignment`
  (static/dhcp_reservation/dhcp_dynamic/vip/gateway/reserved), flagged
  `is_primary` for the entity's identity address. Addresses attach to an
  interface when known, or directly to an entity when not (most netdb hosts
  start this way; discovery upgrades them to interface-level later).
- **`dns_records`**: A/PTR rows that are derivable from IPAM carry
  `is_generated=1` and an `ip_address_id` back-reference; functional CNAMEs
  carry `target_entity_id` so an alias survives the target's IP changing.

### Identity axis

`users` (people, service accounts, AD machine accounts — keyed to AD by
`objectGUID`/`objectSid`) and `groups`, joined by `group_members`, which
supports both user membership and AD-style nested groups
(`CHECK` enforces exactly one member kind per row).

### Trust & secrets axis

- `certificates` — X.509 **metadata** keyed by SHA-256 fingerprint;
  `certificate_bindings` (M:N) records which entity serves/trusts each cert
  and where the files live.
- `secrets` — an **inventory of secrets, never the material**: name, kind,
  non-reversible fingerprint, and a pointer to where the material actually
  lives (`vault_entity_id` + `locator`, e.g. cesium + `/root/.mariadb-root-pw`),
  plus rotation/expiry dates for alerting. `config_items.value_type='secret_ref'`
  + `secret_id` lets configuration reference a secret without storing it.

### Everything else

- `repositories` — Forgejo repos, linked to the forge service entity and,
  where possible, a directory user.
- `service_endpoints` — proto/port/URL a service listens on; feeds the
  monitoring probe-target generator.
- `external_refs` — the sync engine's correlation table: (source, SoR row) →
  the row's native ID in that source (UniFi `_id`, Forgejo repo id, AD GUID,
  monitoring `node.id`). This is what makes bidirectional sync idempotent.

---

### Locations and storage units

Locations use `property`, `floor`, `room`, `section`, and `storage_unit`. Roots must be properties; the ordinary transitions are property→floor→room→section. Storage units may sit under any level and may contain other storage units. The database enforces these rules and rejects parent cycles.

Live sibling names are unique, including at the root. Soft-deleted rows do not claim a name. `container_types` is an editable, provenance-tracked soft-delete catalog; `locations.container_type_id` is optional and retained when a type is retired. `assigned_project_id` is an optional project assignment cleared when its project is hard-deleted.

## 2. Provenance & audit strategy

**Provenance is a column set, present on every table** (except the append-only
`audit_log` itself):

| column | meaning |
|---|---|
| `source_id` → `sources` | which system asserted the current row state |
| `asserted_kind` (`human`/`machine`) | whether a person or automation asserted it |
| `asserted_at` | when the source last (re-)asserted it |

`sources` is a seeded registry (`manual`, `dashboard`, `netdb`, `samba_ad`,
`bind`, `forgejo`, `solarinet_monitor`, `unifi`, `pihole`), each classified as
`seed` / `sync` / `discovery` / `human`. Rule of thumb for conflict
resolution in the sync engine: **human assertions win over machine assertions
for intent fields** (name, role, lifecycle); **machine assertions win for
observed fields** (last_seen, negotiated speed, discovered adjacency).

**Audit is a separate append-only table**, `audit_log`:
`(table_name, row_id, action, actor_user_id | actor_label, source_id,
asserted_kind, old_values JSON, new_values JSON, note, changed_at)`.

It is written by the **application layer** (dashboard PHP + sync jobs), not by
triggers — deliberately, so every write path can attach the *real* actor and a
change reason, which triggers cannot know. The write helper in the dashboard
and each sync job wraps `INSERT/UPDATE/soft-delete + audit_log` in one
transaction. `old_values`/`new_values` carry only the changed columns.

**Soft delete** (`deleted_at`) on every table keeps retired facts queryable;
live-uniqueness constraints include `deleted_at` (`UNIQUE(name, deleted_at)`)
so a retired name can be reused. Hard deletes are exceptional and still
audit-logged (`action='hard_delete'`).

---

## 3. Seed-source mapping

| Source | SoR tables | Notes |
|---|---|---|
| **netdb** (`akoria-hosts.yml`) | `entities` (one per `hosts{}` key; `role`→`role`, `status`→`lifecycle`), `hardware_models`+`hardware_units` (parsed from `hw`), `ip_addresses` (`ip`→`is_primary=1`, static), `interfaces`+`ip_addresses` (`ifaces{}`: `tungsten-alt` → entity `tungsten`, interface `alt`, secondary IP), `dns_records` (`cnames{}` → CNAME rows with `target_entity_id`), `entities` lifecycle=`planned` + `ip_addresses` assignment=`reserved` (`reserved{}`), `networks` (from the comment groupings: core/production/IoT/peripherals/personal/netmgmt/security) | The one-time seed; after cutover the YAML is retired and `sources.netdb` becomes historical provenance |
| **Samba AD** (akoria.org, radium) | `users` (kind=`person`/`service_account`/`machine_account`, `ad_guid`, `ad_sid`, `upn`), `groups`, `group_members` (incl. nested groups), `dns_zones` (akoria.org as `authority='external'`) | Correlate on `objectGUID`; hourly sync. AD computer accounts also correlate to `entities` via `external_refs` |
| **BIND zones** (akoria.net + reverse, xenon) | `dns_zones` (`authority='sor_rendered'`), `dns_records` — any record in the live zones not derivable from netdb is absorbed as an explicit row | Reverse zones are not stored as rows; PTRs are `is_generated` from `ip_addresses` |
| **Forgejo** (cesium) | `repositories`, `users` (Forgejo accounts correlate to AD users where possible), `external_refs` (Forgejo ids) | The Forgejo *service* itself is an entity `runs_on` cesium |
| **SolariNet monitoring DB** (xenon) | `monitoring_state` (from `hostCurrent`/`node`), `entities` correlation via `external_refs` (`node.id`), `interconnects` `link_kind='lldp_adjacency'` (from `lldpEdge`), `interfaces` enrichment (from `gearInterfaceCurrent`), `hardware_models` enrichment (from `networkGear`/discovery) | History tables (`hostHistory`, `probeHistory`) STAY in the monitoring DB; SoR keeps current state only |
| **UniFi** (chemistry) | `vlans`, `networks`, `subnets`, `dhcp_ranges`, `ip_addresses` (leases → `dhcp_dynamic`, fixed-IP entries → `dhcp_reservation`), `interfaces` (device ports, `switch_port`), `hardware_units` (device serials), `external_refs` (UniFi `_id`s) | UniFi devices correlate to existing netdb-seeded entities by MAC/IP |
| **Pi-holes** (helium, mercury) | `config_items` (scope=`pihole`: upstreams, local overrides, adlists as config), `dns_records` (any custom local records not already in the SoR) | After cutover, Pi-hole custom DNS is *rendered from* the SoR instead |

Correlation order during seeding: netdb first (it defines canonical entity
names), then UniFi/monitoring/AD attach to those entities by MAC → IP → name,
recording their native IDs in `external_refs` so later syncs are exact-match.

---

## 4. How `gen-zones.py load_source()` reads the SoR

`load_source()` is the single seam in the generator. The schema ships three
views that reproduce the YAML shapes exactly (with IPv4 normalized from
INET6's v4-mapped form back to dotted quad):

- **`v_dns_hosts`** `(host, ip, role, lifecycle)` — live entities with their
  `is_primary` address; excludes `retired`/`decommissioned`.
- **`v_dns_ifaces`** `(label, ip)` — non-primary addresses bound to a named
  interface, labeled `<entity>-<iface>` (`tungsten-alt`, `radium-wifi`).
- **`v_dns_cnames`** `(alias, target_host)` — CNAME rows joined to their
  target entity's canonical name.

Drop-in replacement body:

```python
import pymysql

def load_source():
    """Source-of-truth boundary. Returns (domain, hosts, ifaces, cnames)."""
    cn = pymysql.connect(host="10.1.0.200", user="solari",
                         password="<from config>", database="sor")
    with cn, cn.cursor() as c:
        c.execute("SELECT name FROM dns_zones WHERE kind='forward' "
                  "AND authority='sor_rendered' AND deleted_at IS NULL LIMIT 1")
        domain = c.fetchone()[0]
        c.execute("SELECT host, ip, role FROM v_dns_hosts")
        hosts  = {h: {"ip": ip, "role": role} for h, ip, role in c.fetchall()}
        c.execute("SELECT label, ip FROM v_dns_ifaces")
        ifaces = dict(c.fetchall())
        c.execute("SELECT alias, target_host FROM v_dns_cnames "
                  "WHERE target_host IS NOT NULL")
        cnames = dict(c.fetchall())
    return domain, hosts, ifaces, cnames
```

Nothing downstream of `load_source()` changes: `forward()`, `reverses()`, and
`named_conf()` keep working, PTR zones keep being derived per /24, and the
serial policy stays in the generator. DNS is now a rendered view of the SoR.

---

## 5. Table inventory (30 tables + 3 views)

| # | Table | Purpose |
|---|---|---|
| 1 | `sources` | Registry of systems allowed to assert facts (provenance targets); pre-seeded |
| 2 | `locations` | Physical hierarchy property/floor/room/section/storage_unit (self-referencing tree) |
| 3 | `hardware_models` | Product catalog: make + model + device class + CPU `arch` (031; NULL = unknown) |
| 4 | `hardware_units` | Individual serial-numbered units of a model, placed at a location |
| 5 | `operating_systems` | OS catalog (family, name, version, EOL) |
| 6 | `entities` | **Supertype hub**: every server/device/appliance/app/service |
| 7 | `applications` | 1:1 subtype: deployed software (vendor/version/install kind) |
| 8 | `services` | 1:1 subtype: network functions (kind, criticality) |
| 9 | `vlans` | 802.1Q VLAN registry (from UniFi) |
| 10 | `networks` | Named logical segments (core/production/IoT/...) |
| 11 | `subnets` | IP prefixes — the IPAM containers (CIDR + parsed net/prefix) |
| 12 | `dhcp_ranges` | Dynamic pools / static blocks / exclusions per subnet |
| 13 | `interfaces` | NICs, radios, switch ports, VLAN/bridge/bond logicals |
| 14 | `interconnects` | Cables and logical/LLDP links between interfaces (or entities) |
| 15 | `ip_addresses` | IPAM: one live row per address; assignment type; primary flag |
| 16 | `dns_zones` | Forward/reverse zones; `sor_rendered` vs `external` authority |
| 17 | `dns_records` | RRs (A/AAAA/CNAME/PTR/SRV/TXT/...); generated vs asserted |
| 18 | `users` | People + service/machine accounts, keyed to Samba AD |
| 19 | `groups` | AD security/distribution groups + local RBAC roles |
| 20 | `group_members` | M:N membership incl. nested groups |
| 21 | `certificates` | X.509 metadata by SHA-256 fingerprint (no key material) |
| 22 | `certificate_bindings` | M:N: which entity serves/trusts which cert, and where |
| 23 | `secrets` | Secret **inventory**: kind, fingerprint, where it lives, rotation |
| 24 | `repositories` | Git repos mirrored from Forgejo |
| 25 | `service_endpoints` | Proto/port/URL a service listens on |
| 26 | `monitoring_state` | Current health per entity (history stays in monitoring DB) |
| 27 | `config_items` | Per-entity/scope config k=v; secret values via `secret_id` |
| 28 | `relationships` | Typed entity graph (contains/runs_on/depends_on/...) |
| 29 | `external_refs` | (source, SoR row) → native ID in the source; sync correlation |
| 30 | `audit_log` | Append-only who/what/when/old→new change history |
| — | `v_dns_hosts` / `v_dns_ifaces` / `v_dns_cnames` | Renderer-facing views matching the YAML shapes |

The inventory above is `schema.sql`. Migration-owned tables are listed with
their migration: RackWire in §7, provisioning images and stacks (029) in §8,
and the broader product catalog (033) below.

## 6. Conventions

- PKs: unsigned auto-increment surrogates; big tables use `BIGINT UNSIGNED`.
- IPs: native `INET6` (v4 stored v4-mapped; views normalize on the way out).
- FKs everywhere; `ON DELETE RESTRICT` by default, `CASCADE` only for
  owned detail/junction rows, `SET NULL` for optional pointers.
- `ENUM` for small closed vocabularies (extending an ENUM is a cheap
  `ALTER ... MODIFY` and keeps invalid states unrepresentable).
- Live-uniqueness: `UNIQUE(..., deleted_at)` so soft-deleted rows free names.
- `schema.sql` is idempotent (`CREATE TABLE IF NOT EXISTS`, guarded ALTER,
  `INSERT IGNORE` seeds, `CREATE OR REPLACE VIEW`); future schema changes go
  in numbered migration files alongside it.

---

## 7. RackWire additions (`migrations/020_rackwire.sql`)

RackWire is the browser-based network, power, and interconnect planner
(`dashboard/public/rackwire/`); rack layouts are optional.
Migration 020 gives it plan storage and gives the SoR a power model, per
CONTRACT-RW v1.0 §1. Power deliberately reuses the existing shape rather than
opening a parallel domain: **power ports are `interfaces` rows, power cords are
`interconnects` rows.** No new edge tables.

| Table | Purpose |
|---|---|
| `rw_plans` | One row per named plan; `plan_json` is `RackWire.getPlan()` verbatim (`JSON_VALID` CHECK). Authoritative store; the browser keeps localStorage for `file://` and offline. |
| `rw_plan_versions` | Append-only snapshots behind `saveVersion()`/`restoreVersion()`. No `updated_at`/`deleted_at` — same posture as `barcode_scans`. The 25-snapshot cap lives in the API, not the schema. |
| `power_circuits` | Branch circuits and PDU/UPS rails as a self-referencing tree (`parent_circuit_id`), placed via `location_id`. Carries volts/amps/phase/breaker label; the 80% continuous-load derate is computed, not stored. |
| `rw_model_groups` | Port geometry per `hardware_models` row: `groups_json` is the RackWire library `groups[]` array verbatim, `device_json` the device-level electrical extras (drawW, budgetW, poeBudgetW, eff, isInternet, isBattery). One **live** row per model; `library_id` lets the client merge over its static seed by id, server winning. |

Changes to existing tables:

- `interconnects.link_kind` gains `'power'` — a power cord is an ordinary
  interconnect between a `power_outlet` and a `power_inlet`.
- Migration 027 adds `media_standard` and `jacket_color` to `interconnects`;
  RackWire commits and reads them alongside label, length, and notes.
- `interfaces.if_kind` gains `'power_inlet'` and `'power_outlet'`.
- `interfaces` gains nullable `circuit_id` (FK → `power_circuits`, `ON DELETE
  SET NULL`), `watts_rated`, and `poe_class`. Purely network interfaces leave
  all three NULL.
- `sources` gains a `rackwire` row (`kind='human'`). Everything the planner
  commits is written with that `source_id` and `asserted_kind='human'`, so the
  commit path can refuse to touch rows it does not own and machine
  `lldp_adjacency` rows stay read-only context.

Two conventions differ slightly from §6 and are intentional:

- **Live-uniqueness uses a generated column, not `deleted_at`.** New tables carry
  `live_flag TINYINT AS (IF(deleted_at IS NULL, 1, NULL)) PERSISTENT` and key on
  `(…, live_flag)`. `UNIQUE(name, deleted_at)` only collapses *simultaneous*
  deletions; `live_flag` lets a name be retired and reused any number of times.
  Introduced in 015; 020 follows it.
- **ENUM values are appended, never inserted or reordered**, so stored ordinals
  of existing rows are untouched. Each ENUM `ALTER` is guarded by an
  `information_schema.COLUMNS` probe under `PREPARE`/`EXECUTE`, so re-running the
  migration skips the (table-rewriting) alter entirely.

No CDC triggers are attached to the new RackWire tables — RackWire writes use a
dedicated API and the sorsync bypass is intentional. Writes to `interfaces` do
still fire the existing CDC triggers; appliers diff before acting. Migrations
022 and 023 create the ledgered transactional outbox and core SoR CDC triggers.
For the provisioning taxonomy added in migration 021, migration 024 adds
transactional
outbox triggers to `sources`, `functions`, `entity_functions`, and
`function_incompatibility`. Migration 025 adds source-reported network state,
a default-off allocator gate, and the approving operator/time audit fields. The
dashboard review endpoint requires an enabled segment with exactly one live
IPv4 subnet; source configuration changes revoke approval and clear its audit
fields. Migration 026 adds outbox triggers
to the UniFi-imported `vlans`, `dhcp_ranges`, and `external_refs`; the existing
core trigger set already covers `networks` and `subnets`. The append-only
`inventory_snapshots` table is not tracked. Scratch integration tests verify
the event stream and roll back fixture rows and outbox entries together. These
candidate migrations have not been applied to the live SoR.

Applied on cesium only (`sudo mariadb sor < netdb/sor/migrations/020_rackwire.sql`);
**cesium replicates to benzene**, so never apply it on benzene directly. Validate
against a scratch schema first — never point tests at live `sor`.

Migration 028 is authored but not yet applied to the live SoR. It adds the
current one-to-one association registry between physical hardware units and
network entities. Entity triggers keep the registry consistent with existing
writers; dashboard association actions are explicitly operator selected and
audited. The CI scratch migration check applies it twice to verify idempotence.
Migration 027 adds the RackWire `media_standard` and `jacket_color` values to
`interconnects`. The operator preflight and recovery sequence for candidate
migrations 024–028
is documented in §14 of the [Provisioning Plan](../../docs/SolariNet_Provisioning_Plan.html)
and its [PDF](../../docs/SolariNet_Provisioning_Plan.pdf); the manual SQL is
kept under `rollback/`, outside the forward migration stream.

## 8. Provisioning images and stacks (`migrations/029_provisioning_images_stacks.sql`)

Migration 029 adds the catalog behind the dashboard's Images and Stacks
screens and the fleet stack generator (`deploy/fleet/stackgen.py`). It is
authored but **not applied to the live SoR**. Manual rollback:
[`rollback/029-down.sql`](rollback/029-down.sql). It refuses unless the
tables hold nothing but the untouched seed.

| Table | Purpose |
|---|---|
| `prov_images` | Installable OS images. Each row is one real distro × release × arch (`release_name`; `RELEASE` is reserved). It records `boot_method` (`ipxe` / `pi_netboot` / `media`), source and checksum URLs, fetch state (`version`, `sha256`, `fetched_at`), `install_template`, `refresh_policy` and `netboot_verified`. One live row per `(distro, release_name, arch)`. CHECKs enforce hex sha256 and that `pi_netboot` is arm-only. |
| `prov_software` | The shared software pool: every installable entry is defined **once**. `item_type` ∈ `pkg`, `repo`, `artifact`, `script`, `build`, `role`. Columns: per-family names (`name_suse` / `name_debian` / `name_fedora`), optional `applies_families` / `applies_arches` SETs (NULL = all), `version_pin`, `ansible_role`, `source_url`. The type-specific shape lives in a `spec` JSON object. One live row per `logical_name`. |
| `prov_software_artifacts` | Per-arch download for an `artifact` entry: `url` and a mandatory `sha256` (arch `any` is allowed). One live row per `(software_id, arch)`; cascades with its entry. |
| `prov_software_requires` | Entry → entry requirement edges: the one way to express build dependencies and repo-before-pkg. One live row per pair; a CHECK rejects self-requirement, cycles are rejected by the generator. Deleting the dependent cascades its edges; deleting a required entry is RESTRICTed. |
| `prov_software_conflicts` | Entries that must never be installed together. Stored once, canonically: `software_lo_id < software_hi_id` (a CHECK, which also rejects the self-pair). One live row per pair; cascades with either side. |
| `prov_stacks` | Named, ordered bundles of pool entries (`slug`, `name`, `description`, `locked`, `sort_order`). `core` is locked and always rendered first. |
| `prov_stack_members` | Stack → pool entry. `item_order` NULL = the type default (repo 100, pkg 200, artifact 300, build 400, script 500, role 600); `sort_order` breaks ties. One live row per `(stack_id, software_id)`; the same entry may be a member of many stacks. Deleting a stack cascades its members; deleting an entry a stack still includes is RESTRICTed. |
| `prov_stack_deps` | Stack → stack dependency edges. A CHECK rejects self-dependency; cycles are rejected by the generator. |
| `prov_runs` | Append-only audit of reimage/apply runs and (031) image refreshes. Request columns are immutable (trigger, NULL-safe), and a terminal status is final. A run can never return to `queued`, and rows cannot be deleted. Shape (031 `ck_prov_runs_shape`): `reimage` = entity + image, `apply` = entity, no image, `refresh` = image, no entity. `stack_slugs` must be a JSON array. |

Why a pool: one row per piece of software means a version pin, sha256 or
repo key is changed in one place, a host never installs the same thing
twice because two stacks list it, and conflicts and requirements are facts
about software rather than about a stack.

Family-name convention for `prov_software`:

- On `pkg` entries the column holds the real package name for that family. NULL means the entry is unsupported on that family, and rendering it there is a hard error unless `applies_families` excludes the family.
- On non-`pkg` entries the column must be `'='` (supported) or NULL.

Other CHECKs on `prov_software`:

- `version_pin` must be one safe path component (`[A-Za-z0-9][A-Za-z0-9._+~-]*`, max 64), because it becomes a directory name on hosts.
- `spec` must be NULL on `pkg` and `role` entries and is required on `repo`, `script` and `build`.
- A `script` entry must give either a non-empty inline `body` or a remote `url` with a lowercase-hex `sha256`; JSON `null` is not a value.
- `logical_name` carries no control characters (C0, DEL, C1, U+2028/U+2029).
- A `build` entry needs a supported `system` (matched case-sensitively, `COLLATE utf8mb4_bin`, so `CMake` is refused) and one of two sources:
  - `source.git`, pinned by a full 40- or 64-hex commit SHA in `version_pin`. An optional `source.tag` is display-only.
  - `source.tarball` with a `sha256`.
- A `custom` build carries two non-empty commands, `build` and `install`. Both run as the unprivileged admin user; `install` writes under `$DESTDIR$PREFIX`. Other systems carry neither. The old single `commands` key is refused, and so is `spec.build_deps`: build dependencies are `prov_software_requires` edges.
- A guard `check` must be a non-empty argv list, and every element must be a string (the first non-empty). It runs without a shell: as the script's `run_as` user for scripts, and as the unprivileged admin user for builds. No JSON function can range over array elements inside a CHECK (`JSON_TABLE` needs a subquery, and `JSON_SEARCH` also matches numbers, booleans and null), so the CHECK matches the `JSON_COMPACT` text against `["str"(,"str")*]`.
- A `repo` entry's per-family blocks must be objects whose fields cannot inject a line into a sources.list or `.repo` file:
  - `url` and `key_url` are single tokens with no whitespace or control characters.
  - `fingerprint` has no control characters.
  - `suite` and each of `components` match `[A-Za-z0-9._/-]+` and do not start with `-`. A JSON-escaped `\/` is accepted, because PHP's `json_encode` emits it.
  - `debian` blocks need both `suite` and `components`.
- An `artifact` or `build` entry needs `version_pin`.
- A `role` entry needs `ansible_role`, and only a role carries one.

All hex and format CHECKs use case-sensitive `(?-i)…\z` patterns. Under `utf8mb4_unicode_ci`, a plain `REGEXP` is case-insensitive, and `$` also matches before a trailing newline. JSON predicates are wrapped in `IFNULL(…, 0)` because MariaDB passes a CHECK that evaluates to NULL.

The generator adds further hard errors on top of these:

- an unmapped package
- a missing per-arch artifact or sha256
- a tarball without sha256
- a git build not pinned by a full commit SHA
- a requirement that does not apply to the target family/arch, or that is listed later in run order and cannot be pulled forward (only a pkg can)
- two conflicting entries that would both be installed; the error names both entries and the stacks (or the dependent, for an implicit requirement) that pulled them in
- a requirement cycle, a self-requirement, or a pair that both requires and conflicts
- a `repo_ref` key or `spec.build_deps` (both superseded by requires edges)
- a secret-looking env name. This is a heuristic on the name only: case-insensitive, token-boundary (split on `_`, `/`, `-`) matching against password, passwd, pass, secret, token, key, apikey, credential(s), jwt, bearer, auth, private, session, cookie, signature and dsn, plus a substring match for password, passwd, credential and apikey. It catches names like `DB_PASSWORD`, `GITHUB_TOKEN` and `DBPASSWORD`, but not `MONKEY` or `BYPASS`. Whatever a variable is called, secret values must never be stored in `spec`.
- a dependency cycle
- an unknown stack slug

Within a stack, default step order is repo → pkg → artifact → build → script →
role (`item_order` overrides it). Stacks render in dependency order, `core`
first. Each pool entry is installed **once**, at its first position; a later
stack listing it again is reported in `solari_stack_shared` and skipped.
Requirements resolve against the pool, never by name inside a stack:

- already installed → nothing to do;
- not listed by any selected stack → installed just before the dependent and
  reported in `solari_stack_implicit` (recursively);
- listed later in run order → a pkg is pulled forward; anything else is an
  error (fix `item_order` or the stack dependencies);
- not applicable to the target → hard error.

A build's `build_deps` output is its pkg requirements' family names in install
order. Conflicts are checked over the installed set after resolution, so an
entry skipped as not applicable does not conflict. Rendered ids (repo files,
artifact and build directories, script names, key files) derive from the
entry's `logical_name` alone, so they do not change with which stack installs
it first. The `stack-items` role consumes the same variables as before plus
the two report lists.

The `stack-items` role never runs a build-controlled command as root. Fetch,
configure, compile and install all run as the unprivileged admin user, in a
build directory that user owns; those steps refuse to run if `id -u` is 0.
Every install is staged into `<build>/_stage` via `DESTDIR` (exported for
all systems, and passed on the `make` command line for make and autotools):

| System | Staged install (as the admin user) |
|---|---|
| cmake | `cmake --install` |
| meson | `meson install` |
| autotools | `make DESTDIR=… install` |
| make | `make PREFIX=<prefix> DESTDIR=… install` |
| custom | the item's `install` command, which must write under `$DESTDIR$PREFIX` |

The only root step is the role's `files/stage_install.py`, a data-only copier.
It installs `<stage><prefix>` into the prefix without following any symlink,
whether in the stage or already in the destination. Files land root-owned at
0755 or 0644, with setuid and setgid dropped. A staged symlink must point
inside the prefix. Devices, fifos, sockets and anything staged outside the
prefix are refused. A build whose install ignores `DESTDIR` stages nothing and
is refused too. An item that needs root-time install logic must be a `script`
item, the explicit and reviewed path for root code.

`functions` is unchanged. `functions.package_bundle` MAY name a `prov_stacks`
slug; nothing enforces that link yet.

029 itself attaches no CDC. Migration 030 (`migrations/030_provisioning_cdc.sql`,
manual rollback [`rollback/030-down.sql`](rollback/030-down.sql)) adds
AFTER INSERT/UPDATE/DELETE outbox triggers to all nine tables (27 triggers,
`trg_<table>_ai/_au/_ad`). Nothing consumes these tables yet, so they are
`optional_tables` in `sor_emitd`. **Deploy order:** ship the updated
`sor_emitd.py` (and any explicit `optional_tables` in the deployed
`sorsync.conf`) and restart `sor-emit` BEFORE applying 030, or the first
unroutable `prov_*` event stalls the emit checkpoint for every table. Apply as
DB root on cesium. `prov_runs` deletes are refused by the 029 guard, so its
delete trigger never fires today. Rolling back 029 drops these triggers with the
tables; re-apply 030 after any 029 forward recovery.

Migration 031 (`migrations/031_provisioning_arch_refresh.sql`, manual rollback
[`rollback/031-down.sql`](rollback/031-down.sql)) adds the nullable
`hardware_models.arch` ENUM (exactly the `prov_images.arch` vocabulary,
appended only in lockstep; NULL = unknown). It is the first arch source for a
reimage target; the agent-reported `node.arch` is the fallback, and a reimage
is refused while the target arch is unknown or differs from the image. It
also adds `prov_runs.mode='refresh'` (IMAGE_REFRESH records a run): `entity_id`
becomes NULL-able and the 029 `ck_prov_runs_image` CHECK is replaced by
`ck_prov_runs_shape` (see the `prov_runs` row above). No trigger is created or
replaced; the 029 guards compare with `<=>`, so a refresh run's NULL
`entity_id` is immutable too, and the 030 CDC triggers publish refresh runs
like any other. `hardware_models` carries no CDC. **Deploy order:** apply 031
(as DB root on cesium) before activating the release that reads `arch` or
writes `refresh` runs, and before applying the dashboard grant file (its
column-scoped `UPDATE (arch, …)` on `hardware_models` names the new column).
The rollback refuses while any refresh run or non-NULL `arch` exists; since
`prov_runs` refuses deletes, a recorded refresh run makes 031 effectively
forward-only.

Migration 033 (`migrations/033_inventory_catalog.sql`, compensating rollback
[`rollback/033-down.sql`](rollback/033-down.sql)) adds the broad `product_types`
catalog and separate `product_barcodes` namespace. Every existing
`hardware_models` row backfills to a `product_types` row with the same ID;
`printer`/`scanner` map to `peripheral`, `other` to `other`, and all remaining
legacy `hw_type` values to `hardware`. Brand/model/notes are copied and legacy
columns remain available. Unit rows link to the mapped product type without
changing `model_id`; no commercial barcode associations are created. New
product-type provenance uses the dashboard source plus
`lookup_source='migration:033'` and a `hardware_models:<id>` lookup reference.

The migration adds nullable unit/model product-type links and nullable
licence-reference fields. `installed_on_unit_id` references another unit with
`ON DELETE SET NULL`; insert/update guards reject self-reference and targets
whose product type is software, and prevent changing an active target's kind to
software. Licence fields hold counts, expiry, and a reference only, never a key
or token. System `barcodes.subject_table` now
accepts `product_types` through the `InvCodes::SUBJECTS` allowlist; commercial
codes remain in `product_barcodes`.

The rollback refuses when catalog values differ from the exact backfill, when
product barcodes or system barcodes for product types exist, when model/unit
links have changed, or when licence/installation fields are populated. It is
for a controlled maintenance window after catalog writers are disabled; DDL
autocommits. No product types are seeded from UI mockups.

The seed is inserted once and never resurrected; a soft-deleted seed row stays
deleted on re-run:

- Pool: the 19 `solari_core_packages` from
  `deploy/fleet/ansible/group_vars/all.yml`, mapped through `solari_pkg_map`;
  the `dev-tools role` entry; and the `powerlevel10k pin` script pinned to
  commit `ff0311157d6b24fea21aa70699783f362b0f554f`, which requires the role
  (it checks out the clone the role creates).
- `core`: the 19 packages, in `all.yml` order. `dev-tools`: the role, then
  the pin at `item_order` 700.
- No conflict pair: none is real among the seeded entries (`chrony` would
  conflict with `ntp`, which is not in the pool).
- Six images, including openSUSE Tumbleweed x86_64 over iPXE. No checksums or
  versions are seeded.

Tests:

- `tests/netdb/test_prov_images_stacks.sh` runs on a scratch DB. It covers every `prov_software` CHECK with a paired accept/reject, live-unique names and members, requires and canonical conflicts, triggers, seed-once, `stackgen --from-db` against the fixture (plus a DB conflict pair and a soft-deleted member), and rollback with forward recovery.
- `tests/deploy/test_stackgen.py` covers the generator: dedupe-once, shared and implicit reporting, conflicts (direct and via an implicit requirement), pool requirement resolution, and the golden comparison of core against `all.yml`.
- `tests/deploy/test_stack_items_ansible.sh` runs the Ansible role locally.
