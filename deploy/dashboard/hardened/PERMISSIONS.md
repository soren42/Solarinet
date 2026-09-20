# PHP→host security boundary — file ownership & permissions (Task #1)

The dashboard (PHP) and the server run as **different uids** so that a PHP
compromise cannot read the CA key, mint certificates, or execute a privileged
control verb. This document is the authoritative permission matrix; the
`verify-boundary.sh` script in this directory asserts it on a live host.

The boundary has three enforced layers. Config files alone are not enough — all
three must hold:

1. **uid split** — `www-solari` (PHP) ≠ `solari` (server). This file.
2. **socket peer-cred ACL** — the C bridge reads `SO_PEERCRED` and refuses every
   privileged verb to the dashboard uid. `server.conf.ctl-fragment`.
3. **request queue** — PHP enqueues privileged actions it cannot run; the server
   drains them. `db/migrations/020_ctl_request_queue.sql` + `serverCtlQueuePoll`.

## Identities

| User         | Role                    | Holds                                   |
|--------------|-------------------------|-----------------------------------------|
| `solari`     | server service          | CA key, server DB grant, PKI, ctl socket|
| `www-solari` | dashboard (php-fpm)     | dashboard store, sessions, socket access |
| `solari-ctl` | group (socket gate)     | membership = right to reach the socket   |

## Ownership matrix

The decisive rule: **no path readable by `www-solari` contains a host secret,
and every host secret is `0600 solari` (or `0640 solari:solari`).**

| Path                                             | Mode | Owner              | www-solari can |
|--------------------------------------------------|------|--------------------|----------------|
| CA private key (`…/pki/ca.key`)                  | 0600 | solari:solari      | **no**         |
| CA cert (`…/pki/ca.pem`)                         | 0644 | solari:solari      | read (public)  |
| server TLS key (`…/pki/server.key`)             | 0600 | solari:solari      | **no**         |
| server env / DB pass (`/etc/solarinet/server.env`)| 0600 | solari:solari    | **no**         |
| server config (`/etc/solarinet/server.conf`)    | 0640 | solari:solari      | **no**         |
| ctl socket (`/run/solari/solariCtl.sock`)       | 0660 | solari:solari-ctl  | connect (group)|
| socket dir (`/run/solari`)                      | 0751 | solari:solari      | traverse only¹ |
| dashboard store (`…/solari-auth.json`)          | 0640 | www-solari:www-solari | read+write  |
| dashboard sessions dir                           | 0700 | www-solari:www-solari | read+write  |
| dashboard code (`/var/www/solarinet`)           | 0755 | root:root          | read (exec)    |

¹ `www-solari` traverses `/run/solari` to reach the socket by name because the
dir is `0751` (`other` gets `--x`, traverse-only) and it opens the socket via its
`0660 solari:solari-ctl` group membership. `www-solari` is in `solari-ctl`, not
`solari`, so it is `other` at this dir — `0750` would deny the traverse and make
the socket unreachable (review F12). `0751` grants traverse but not list/read, so
it cannot enumerate or create siblings. The socket's own `0660` is the real gate.

## DB grants

The dashboard uses a **least-privilege** DB user, not the server's grant.

The decisive rule here mirrors the ownership matrix: **the dashboard DB user has
NO privilege on `ctlRequest`.** PHP reaches the queue exclusively through the ctl
socket (`REQUEST_SUBMIT` to enqueue, `REQUEST_GET` to poll status) — it never
touches the table directly. Denying the grant means even a SQL-injected dashboard
cannot flip a queued row to `claimed`/`done`, forge a `requestedBy`, or delete an
audit row; only the in-process consumer (running as `solari`) writes state.

A database-wide grant cannot express this. MariaDB privileges are **additive
across levels**, and a table-level `REVOKE` only subtracts privileges that were
granted *at the table level* — it cannot mask a `GRANT … ON solarinet.*` (review
F3). So `GRANT ALL ON solarinet.* … ; REVOKE … ON solarinet.ctlRequest …` leaves
the dashboard with full `ctlRequest` access. The grant must be **per-table**, and
`ctlRequest` must simply never appear in it:

```sql
-- server (full):
GRANT ALL PRIVILEGES ON solarinet.* TO 'solari'@'127.0.0.1';
```

```sql
-- dashboard (scoped): per-table DML on every base table EXCEPT ctlRequest, plus
-- SELECT on the views. Generated so it stays correct as migrations add tables —
-- a new table is covered automatically; ctlRequest is the one hard exclusion.
-- Run in the mysql client (\. or SOURCE), or pipe the SELECT output back into mysql.
SELECT CONCAT(
         'GRANT SELECT, INSERT, UPDATE, DELETE ON solarinet.`',
         table_name,
         '` TO ''solari_dash''@''127.0.0.1'';')
  FROM information_schema.tables
 WHERE table_schema = 'solarinet'
   AND table_type   = 'BASE TABLE'
   AND table_name  <> 'ctlRequest';       -- socket-only; never grant to PHP

SELECT CONCAT(
         'GRANT SELECT ON solarinet.`', table_name,
         '` TO ''solari_dash''@''127.0.0.1'';')
  FROM information_schema.tables
 WHERE table_schema = 'solarinet'
   AND table_type   = 'VIEW';

FLUSH PRIVILEGES;
```

After granting, confirm the exclusion held:

```sql
-- Expect ZERO rows. Any row = the dashboard can write the queue → boundary breach.
SELECT * FROM information_schema.table_privileges
 WHERE grantee LIKE '''solari_dash''@%'
   AND table_schema = 'solarinet' AND table_name = 'ctlRequest';
```

The socket ACL is the primary control (PHP is refused every privileged verb by
`SO_PEERCRED`); this grant is defence in depth for the queue's integrity.

## Cutover (no live change until explicitly authorized)

The boundary is **opt-in**: shipping the code with `enforcePeer=false` (default)
changes nothing on an un-migrated host. To enable it on a target host, in order:

1. `systemd-sysusers` with `solarinet-users.sysusers.conf` (create the uids).
2. Move dashboard state to `/var/lib/solarinet-dashboard` (tmpfiles + copy
   `solari-auth.json`, chown www-solari); move server secrets to
   `/etc/solarinet` (chown solari, chmod 0600).
3. Install the hardened `solarinet-server.service` (runs as solari) and the
   `[ctl]` fragment in `server.conf` (`enforcePeer=true`, `dashboardUid=www-solari`,
   `socketGid=solari-ctl`). Restart the server; confirm the socket is
   `0660 solari:solari-ctl`.
4. Install the hardened php-fpm pool (runs as www-solari); reload php-fpm.
5. Run `verify-boundary.sh`. It must report every check PASS before the host is
   considered boundary-enforced.

Rollback is a single step: set `enforcePeer=false` and restart the server — the
peer ACL disengages and every peer is OPERATOR again (legacy behaviour).

## Tracked follow-ups (review findings carried as work, not blockers)

These came out of the cross-lab review. None blocks the boundary shipping as
reviewable files; each has an owner and a trigger.

- **F9 — route rewiring at cutover.** `dashboard/api/routes/control.php` today
  calls `SolariCtl::call()` directly for the privileged verbs (PROVISION,
  DECOMMISSION, RETIRE, DEPLOY, FLEET_PROVISION, FLEET_IMAGE). While
  `enforcePeer=false` those still execute (legacy). The moment step 3 sets
  `enforcePeer=true`, the bridge **refuses** them to the dashboard peer and the
  routes break. So cutover step 3 MUST be paired with rewiring those routes to
  `REQUEST_SUBMIT` (enqueue) + `REQUEST_GET` (poll) — the enqueue path is already
  in `SolariCtl.php`. Until a host is cut over, the routes are intentionally left
  on the direct call so nothing changes on un-migrated hosts. Owner: dashboard.
  Trigger: same change window as `enforcePeer=true`.

- **F2 — accepted risk: no per-request operator-approval gate.** The queue binds
  the operator server-side from `requestedBy` and executes on claim; there is no
  second-human approval step between enqueue and execute. This was reviewed and
  **accepted**: the dashboard is authenticated (Keycloak SSO), the boundary is
  wholly inside the intranet behind a network IDS (to be strengthened in a later
  phase), and an approval gate is over-engineering for the current threat model.
  Documented here so the omission is a decision on record, not an oversight. If a
  future phase widens exposure (external access, multi-tenant operators), revisit
  by adding an `approved` state to `ctlRequest` that the consumer requires before
  claim. Owner: deferred. Trigger: any change to the exposure assumptions above.
