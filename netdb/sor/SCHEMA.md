# SolariNet System of Record (SoR) — Schema Design

Database: **`sor`** on cesium (10.1.0.200, MariaDB 12.3, InnoDB, utf8mb4).
DDL: [`schema.sql`](schema.sql) — validated end-to-end against a throwaway
`sor_val` schema on cesium (full load, idempotent re-run, view smoke test).

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
                         locations (self-tree: site>room>rack>position)
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
| 2 | `locations` | Physical hierarchy site/room/rack/position (self-referencing tree) |
| 3 | `hardware_models` | Product catalog: make + model + device class |
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
