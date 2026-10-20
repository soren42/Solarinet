#!/usr/bin/env python3
"""
test_allocator_db.py — concurrency proof for allocator.allocate against a
SCRATCH MariaDB (set up + torn down by test_allocator_db.sh).

The point of this test is to demonstrate the second-review BLOCKER #3 fix:
migration 021's uq_ip_addr_live UNIQUE makes it IMPOSSIBLE for two live
ip_addresses rows to share an address, so N threads racing to allocate from the
same small subnet produce exactly the free-count of DISTINCT addresses and zero
duplicates — even when the per-subnet lock is deliberately removed.

Three scenarios:
  1. Locked allocate() (the shipping path): N workers, each its own connection
     and transaction, all allocating from one /27. Expect: every success is a
     distinct address, count == free host count, remainder correctly exhaust.
  2. No-lock stress: bypass the subnet FOR UPDATE and hammer raw INSERTs of the
     SAME address from many threads. Expect: exactly ONE commits, the rest get
     an IntegrityError. This is the constraint itself doing the work.
  3. Direct duplicate insert: two live rows, same address → IntegrityError.
     The bare proof the constraint exists and bites.

Run via the harness (needs SOR_TEST_DB + the sor venv):
    netdb/sor/test_allocator_db.sh
"""

import os
import sys
import threading

import pymysql

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import allocator  # noqa: E402


DB = os.environ.get("SOR_TEST_DB", "sor_test")
FAILS = []


def conn():
    # allocate() requires READ COMMITTED (see its docstring): under the server
    # default REPEATABLE READ the free-address scan reads a snapshot frozen at
    # transaction start and concurrent allocation spins to exhaustion. Set it on
    # the connection before its first statement via init_command.
    return pymysql.connect(host="127.0.0.1", user="sor_test",
                           password="sor_test_ephemeral", database=DB,
                           charset="utf8mb4", autocommit=False,
                           init_command="SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")


def note(ok, msg):
    if ok:
        print(f"  ok  {msg}")
    else:
        FAILS.append(msg)
        print(f"  x   {msg}")


# ---------------------------------------------------------------------------
# Fixtures — a tiny network/subnet/function, all provisioner-sourced
# ---------------------------------------------------------------------------

def setupFixtures(c):
    """Create one network, one /27 subnet with a gateway, wire the 'web'
    function's home network to it. Returns (networkId, subnetId, functionSlug).

    /27 = 10.42.0.0/27 → hosts .1-.30. Gateway .1 reserved → 29 allocatable.
    """
    cur = c.cursor()
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]

    cur.execute(
        "INSERT INTO networks (name, purpose, source_id, asserted_kind) "
        "VALUES ('alloc_test', 'allocator concurrency test', %s, 'human')", (src,))
    networkId = cur.lastrowid

    cur.execute(
        "INSERT INTO subnets (network_id, cidr, net_address, prefix_len, gateway_ip, "
        " source_id, asserted_kind) VALUES "
        "(%s, '10.42.0.0/27', CAST('10.42.0.0' AS INET6), 27, CAST('10.42.0.1' AS INET6), "
        " %s, 'human')", (networkId, src))
    subnetId = cur.lastrowid

    # Wire the 'web' tag's home network to our test segment.
    cur.execute("UPDATE functions SET network_id=%s WHERE slug='web'", (networkId,))
    c.commit()
    return networkId, subnetId, "web"


def teardownFixtures(c, networkId, subnetId):
    cur = c.cursor()
    cur.execute("DELETE FROM ip_addresses WHERE subnet_id=%s", (subnetId,))
    cur.execute("UPDATE functions SET network_id=NULL WHERE slug='web'")
    cur.execute("DELETE FROM subnets WHERE id=%s", (subnetId,))
    cur.execute("DELETE FROM networks WHERE id=%s", (networkId,))
    c.commit()


# ---------------------------------------------------------------------------
# Scenario 1 — locked allocate(), N racing workers on one /27
# ---------------------------------------------------------------------------

def scenarioLockedRace(functionSlug, workers=40):
    results = []
    errors = []
    lock = threading.Lock()

    def worker():
        c = conn()
        try:
            addr = allocator.allocate(c, functionSlug, isPrimary=False)
            c.commit()
            with lock:
                results.append(addr)
        except RuntimeError as e:
            # exhaustion is a legitimate outcome once the /27 is full
            with lock:
                errors.append(str(e))
            c.rollback()
        except Exception as e:  # noqa: BLE001
            with lock:
                errors.append(f"UNEXPECTED {type(e).__name__}: {e}")
            c.rollback()
        finally:
            c.close()

    threads = [threading.Thread(target=worker) for _ in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    distinct = set(results)
    note(len(distinct) == len(results),
         f"locked race: {len(results)} allocations, {len(distinct)} distinct "
         f"(no duplicates)")
    # /27 with gateway .1 reserved → 29 allocatable hosts.
    note(len(results) == 29,
         f"locked race: allocated exactly 29 addresses (got {len(results)})")
    unexpected = [e for e in errors if e.startswith("UNEXPECTED")]
    note(not unexpected, f"locked race: no unexpected errors ({unexpected[:2]})")


# ---------------------------------------------------------------------------
# Scenario 2 — no-lock stress: many threads INSERT the SAME address
# ---------------------------------------------------------------------------

def scenarioNoLockSameAddress(subnetId, workers=30):
    """Deliberately bypass the allocator's subnet lock and have every thread try
    to INSERT the identical address. The uq_ip_addr_live UNIQUE must let exactly
    one win. This isolates the CONSTRAINT as the guarantee, not the lock."""
    addr = "10.42.0.9"
    committed = []
    integrity = []
    other = []
    lock = threading.Lock()

    def worker():
        c = conn()
        cur = c.cursor()
        try:
            cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
            src = cur.fetchone()[0]
            cur.execute(
                "INSERT INTO ip_addresses (address, subnet_id, assignment, "
                " is_primary, source_id, asserted_kind, asserted_at) "
                "VALUES (CAST(%s AS INET6), %s, 'static', 0, %s, 'machine', NOW())",
                (addr, subnetId, src))
            c.commit()
            with lock:
                committed.append(1)
        except pymysql.err.IntegrityError:
            with lock:
                integrity.append(1)
            c.rollback()
        except Exception as e:  # noqa: BLE001
            with lock:
                other.append(f"{type(e).__name__}: {e}")
            c.rollback()
        finally:
            c.close()

    threads = [threading.Thread(target=worker) for _ in range(workers)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    note(sum(committed) == 1,
         f"no-lock same-address: exactly 1 commit (got {sum(committed)})")
    note(len(integrity) == workers - 1,
         f"no-lock same-address: {len(integrity)} IntegrityErrors "
         f"(expected {workers - 1})")
    note(not other, f"no-lock same-address: no other errors ({other[:2]})")


# ---------------------------------------------------------------------------
# Scenario 3 — bare direct duplicate insert → IntegrityError
# ---------------------------------------------------------------------------

def scenarioDirectDuplicate(subnetId):
    c = conn()
    cur = c.cursor()
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]
    cur.execute(
        "INSERT INTO ip_addresses (address, subnet_id, assignment, is_primary, "
        " source_id, asserted_kind, asserted_at) "
        "VALUES (CAST('10.42.0.20' AS INET6), %s, 'static', 0, %s, 'machine', NOW())",
        (subnetId, src))
    c.commit()
    dup_raised = False
    try:
        cur.execute(
            "INSERT INTO ip_addresses (address, subnet_id, assignment, is_primary, "
            " source_id, asserted_kind, asserted_at) "
            "VALUES (CAST('10.42.0.20' AS INET6), %s, 'static', 0, %s, 'machine', NOW())",
            (subnetId, src))
        c.commit()
    except pymysql.err.IntegrityError:
        dup_raised = True
        c.rollback()
    note(dup_raised, "direct duplicate live address → IntegrityError")

    # And prove a soft-deleted row does NOT block re-allocating the address.
    cur.execute("UPDATE ip_addresses SET deleted_at=NOW() "
                "WHERE address=CAST('10.42.0.20' AS INET6) AND deleted_at IS NULL")
    c.commit()
    reused = False
    try:
        cur.execute(
            "INSERT INTO ip_addresses (address, subnet_id, assignment, is_primary, "
            " source_id, asserted_kind, asserted_at) "
            "VALUES (CAST('10.42.0.20' AS INET6), %s, 'static', 0, %s, 'machine', NOW())",
            (subnetId, src))
        c.commit()
        reused = True
    except pymysql.err.IntegrityError:
        c.rollback()
    note(reused, "soft-deleted address can be re-allocated (live_flag NULL-distinct)")
    c.close()


# ---------------------------------------------------------------------------
# Scenario 4 — taxonomy-driven allocate(): primary derivation, the
# one-primary-IP-per-entity constraint, non-race re-raise, and the SAVEPOINT
# guarantee that a failed insert never rolls back the caller's prior work.
# This is the proof of the two review BLOCKER fixes in allocate().
# ---------------------------------------------------------------------------

def _makeTaggedEntity(c, functionSlug, primary=True):
    """Create a live entity tagged with functionSlug (primary by default).
    Returns the entity id. Committed so other connections can see it."""
    cur = c.cursor()
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]
    cur.execute(
        "INSERT INTO entities (name, entity_type, source_id, asserted_kind) "
        "VALUES ('alloc_test_host', 'server', %s, 'machine')", (src,))
    entityId = cur.lastrowid
    cur.execute("SELECT id FROM functions WHERE slug=%s AND deleted_at IS NULL",
                (functionSlug,))
    fnId = cur.fetchone()[0]
    cur.execute(
        "INSERT INTO entity_functions (entity_id, function_id, is_primary, "
        " tag_order, source_id, asserted_kind) VALUES (%s, %s, %s, 0, %s, 'machine')",
        (entityId, fnId, 1 if primary else 0, src))
    c.commit()
    return entityId


def scenarioEntityPrimaryAndSavepoint(functionSlug, subnetId):
    """Blocker-fix proof. Uses a taxonomy-tagged entity."""
    setupC = conn()
    entityId = _makeTaggedEntity(setupC, functionSlug, primary=True)
    setupC.close()

    # --- 4a: primary derivation + one-primary-IP + non-race re-raise + savepoint
    c = conn()
    cur = c.cursor()
    # Caller's prior work in THIS transaction: a sentinel network row. If a
    # failed allocate() rolled back the whole transaction, this would vanish.
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]
    cur.execute(
        "INSERT INTO networks (name, purpose, source_id, asserted_kind) "
        "VALUES ('sentinel_prior_work', 'savepoint proof', %s, 'human')", (src,))
    sentinelId = cur.lastrowid

    addr1 = allocator.allocate(c, functionSlug, entityId=entityId, isPrimary=True)
    note(True, f"entity primary allocate: derived + assigned {addr1}")

    # Second primary for the SAME entity must fail on uq_ip_addr_primary_entity —
    # and must be RE-RAISED (IntegrityError), not silently retried as a race.
    raised = None
    try:
        allocator.allocate(c, functionSlug, entityId=entityId, isPrimary=True)
    except pymysql.err.IntegrityError as e:
        raised = e
    except Exception as e:  # noqa: BLE001
        raised = e
    note(isinstance(raised, pymysql.err.IntegrityError),
         f"second primary IP for entity → IntegrityError re-raised "
         f"(got {type(raised).__name__ if raised else 'no error'})")

    # The caller's sentinel + the first address must both survive the failed
    # insert — ROLLBACK TO SAVEPOINT undid only the doomed insert.
    cur.execute("SELECT COUNT(*) FROM networks WHERE id=%s AND deleted_at IS NULL",
                (sentinelId,))
    note(cur.fetchone()[0] == 1,
         "caller's prior work survived the failed allocate (savepoint, not full rollback)")
    cur.execute("SELECT COUNT(*) FROM ip_addresses "
                "WHERE address=CAST(%s AS INET6) AND deleted_at IS NULL", (addr1,))
    note(cur.fetchone()[0] == 1, "first (primary) address survived too")
    c.rollback()  # discard sentinel + addr1; keep the DB clean for later scenarios
    c.close()

    # --- 4b: taxonomy membership is enforced (entity lacks the requested tag)
    c = conn()
    otherTag = "auth" if functionSlug != "auth" else "web"
    rejected = None
    try:
        allocator.allocate(c, otherTag, entityId=entityId, isPrimary=False)
    except ValueError as e:
        rejected = e
    except Exception as e:  # noqa: BLE001
        rejected = e
    note(isinstance(rejected, ValueError),
         f"allocate for a tag the entity does not carry → ValueError "
         f"(got {type(rejected).__name__ if rejected else 'no error'})")
    c.rollback()
    c.close()

    # cleanup the entity (CASCADE drops its entity_functions)
    cc = conn()
    ccur = cc.cursor()
    ccur.execute("DELETE FROM ip_addresses WHERE entity_id=%s", (entityId,))
    ccur.execute("DELETE FROM entities WHERE id=%s", (entityId,))
    cc.commit()
    cc.close()


def scenarioRetryBranch(functionSlug, subnetId):
    """Deterministically exercise allocate()'s retry branch: make the first
    free-address scan LIE (report the subnet empty) while .2 is already taken,
    forcing exactly one uq_ip_addr_live collision → ROLLBACK TO SAVEPOINT →
    recompute → succeed on .3. Proves: retry happens, it is savepoint-scoped
    (caller's prior work survives), and it converges."""
    # Pre-occupy .2 with a committed live row (visible to the allocator's scan).
    pre = conn()
    pcur = pre.cursor()
    pcur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = pcur.fetchone()[0]
    pcur.execute(
        "INSERT INTO ip_addresses (address, subnet_id, assignment, is_primary, "
        " source_id, asserted_kind, asserted_at) "
        "VALUES (CAST('10.42.0.2' AS INET6), %s, 'static', 0, %s, 'machine', NOW())",
        (subnetId, src))
    pre.commit()
    pre.close()

    calls = [0]
    real = allocator._usedAndRanges

    def lyingUsedAndRanges(cur, sid):
        # First call: pretend the subnet is wide open so the allocator picks .2
        # (which is actually taken) → the INSERT collides. Later calls: truth.
        # Return the real cidr/gateway (fixture is 10.42.0.0/27 gw .1) with an
        # empty used-set so selectAddress lands on .2.
        calls[0] += 1
        if calls[0] == 1:
            return "10.42.0.0/27", "10.42.0.1", set(), []
        return real(cur, sid)

    c = conn()
    cur = c.cursor()
    # Caller's prior work again, to prove the retry's savepoint scope.
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]
    cur.execute(
        "INSERT INTO networks (name, purpose, source_id, asserted_kind) "
        "VALUES ('sentinel_retry', 'retry savepoint proof', %s, 'human')", (src,))
    sentinelId = cur.lastrowid

    allocator._usedAndRanges = lyingUsedAndRanges
    try:
        addr = allocator.allocate(c, functionSlug, isPrimary=False)
    finally:
        allocator._usedAndRanges = real

    note(addr == "10.42.0.3",
         f"retry branch: collided on .2, recovered to .3 (got {addr})")
    note(calls[0] == 2,
         f"retry branch: recomputed exactly once (scan called {calls[0]}x)")
    cur.execute("SELECT COUNT(*) FROM networks WHERE id=%s AND deleted_at IS NULL",
                (sentinelId,))
    note(cur.fetchone()[0] == 1,
         "retry branch: caller's prior work survived the savepoint rollback")
    c.rollback()
    c.close()


def _makeInterface(c, entityId, name="eth0"):
    """Create a live interface owned by entityId. Returns interface id."""
    cur = c.cursor()
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]
    cur.execute(
        "INSERT INTO interfaces (entity_id, name, if_kind, source_id, asserted_kind) "
        "VALUES (%s, %s, 'physical', %s, 'machine')", (entityId, name, src))
    c.commit()
    return cur.lastrowid


def scenarioInterfaceDerivation(functionSlug, subnetId):
    """Second-review BLOCKER B2 residual: an interface-owned primary written with
    entityId=None must NOT bypass the taxonomy check + one-primary unique. Prove
    allocate() derives the owning entity from the interface, and that a mismatched
    entityId/interfaceId pair is rejected."""
    setupC = conn()
    entityId = _makeTaggedEntity(setupC, functionSlug, primary=True)
    ifaceId = _makeInterface(setupC, entityId)
    otherEntity = _makeTaggedEntity(setupC, functionSlug, primary=True)
    setupC.close()

    # 6a: primary via interfaceId only (entityId=None) → entity derived, row owned.
    c = conn()
    cur = c.cursor()
    addr = allocator.allocate(c, functionSlug, interfaceId=ifaceId,
                              entityId=None, isPrimary=True)
    cur.execute("SELECT entity_id FROM ip_addresses "
                "WHERE address=CAST(%s AS INET6) AND deleted_at IS NULL", (addr,))
    note(cur.fetchone()[0] == entityId,
         "interface-only primary: entity_id derived from interface (not left NULL)")
    c.commit()

    # 6b: a SECOND primary via the same interface must now collide on
    # uq_ip_addr_primary_entity — the bypass is closed, the unique bites.
    c2 = conn()
    raised = None
    try:
        allocator.allocate(c2, functionSlug, interfaceId=ifaceId,
                           entityId=None, isPrimary=True)
    except Exception as e:  # noqa: BLE001
        raised = e
    note(isinstance(raised, pymysql.err.IntegrityError),
         f"second interface-derived primary → IntegrityError "
         f"(got {type(raised).__name__ if raised else 'no error'})")
    c2.rollback(); c2.close()

    # 6c: entityId that does not own the interface → ValueError (caller bug).
    c3 = conn()
    mism = None
    try:
        allocator.allocate(c3, functionSlug, interfaceId=ifaceId,
                           entityId=otherEntity, isPrimary=True)
    except Exception as e:  # noqa: BLE001
        mism = e
    note(isinstance(mism, ValueError),
         f"entityId not owning interfaceId → ValueError "
         f"(got {type(mism).__name__ if mism else 'no error'})")
    c3.rollback(); c3.close()

    # cleanup
    cc = conn(); ccur = cc.cursor()
    ccur.execute("DELETE FROM ip_addresses WHERE entity_id IN (%s,%s)", (entityId, otherEntity))
    ccur.execute("DELETE FROM interfaces WHERE id=%s", (ifaceId,))
    ccur.execute("DELETE FROM entities WHERE id IN (%s,%s)", (entityId, otherEntity))
    cc.commit(); cc.close()


def scenarioPrimaryTransitions(functionSlug, subnetId):
    """Second-review N1: prove the one-live-primary-per-entity invariant holds
    across the transition paths a generated column + UNIQUE must cover —
    is_primary UPDATE, soft-delete/reactivate — and that a hard entity-delete under
    a live primary is now blocked by fk_ip_entity ON DELETE RESTRICT (D1 fix)."""
    setupC = conn()
    entityId = _makeTaggedEntity(setupC, functionSlug, primary=True)
    setupC.close()

    src = None
    c = conn(); cur = c.cursor()
    cur.execute("SELECT id FROM sources WHERE slug='provisioner'")
    src = cur.fetchone()[0]

    def insertAddr(addr, primary, entity=entityId):
        cur.execute(
            "INSERT INTO ip_addresses (address, subnet_id, entity_id, assignment, "
            " is_primary, source_id, asserted_kind, asserted_at) "
            "VALUES (CAST(%s AS INET6), %s, %s, 'static', %s, %s, 'machine', NOW())",
            (addr, subnetId, entity, 1 if primary else 0, src))
        c.commit()
        return cur.lastrowid

    # A live primary + a live secondary, both owned by the entity.
    primId = insertAddr("10.42.0.5", primary=True)
    secId = insertAddr("10.42.0.6", primary=False)

    # N1a: promoting the secondary to primary (UPDATE 0→1) while a live primary
    # exists must be rejected by uq_ip_addr_primary_entity — the unique bites on
    # UPDATE, not only INSERT.
    raised = None
    try:
        cur.execute("UPDATE ip_addresses SET is_primary=1 WHERE id=%s", (secId,))
        c.commit()
    except pymysql.err.IntegrityError as e:
        raised = e; c.rollback()
    note(isinstance(raised, pymysql.err.IntegrityError),
         f"UPDATE second row is_primary 0→1 → IntegrityError "
         f"(got {type(raised).__name__ if raised else 'no error'})")

    # N1b: soft-delete the live primary → its flag goes NULL → a new primary for
    # the same entity is allowed; then reactivating the old one collides.
    cur.execute("UPDATE ip_addresses SET deleted_at=NOW() WHERE id=%s", (primId,))
    c.commit()
    newPrimOk = False
    try:
        newPrimId = insertAddr("10.42.0.7", primary=True)
        newPrimOk = True
    except pymysql.err.IntegrityError:
        c.rollback()
    note(newPrimOk, "soft-deleting a primary frees the slot for a new primary")

    react = None
    try:
        cur.execute("UPDATE ip_addresses SET deleted_at=NULL WHERE id=%s", (primId,))
        c.commit()
    except pymysql.err.IntegrityError as e:
        react = e; c.rollback()
    note(isinstance(react, pymysql.err.IntegrityError),
         f"reactivating a primary while another is live → IntegrityError "
         f"(got {type(react).__name__ if react else 'no error'})")
    c.close()

    # N1c: entity hard-delete while a live primary still points at it. Migration
    # 021 §6e now makes fk_ip_entity ON DELETE RESTRICT (was SET NULL), so the DB
    # BLOCKS the delete instead of silently orphaning the primary. This is the D1
    # fix: "live primary ⇒ entity_id IS NOT NULL" is a real invariant, not just
    # allocator+writer discipline. RESTRICT refuses the parent delete while ANY
    # ip_addresses row (live or soft-deleted) references the entity.
    dc = conn(); dcur = dc.cursor()
    # primId (.5) is soft-deleted, newPrimId (.7) is the live primary of entityId.
    dcur.execute("SELECT COUNT(*) FROM ip_addresses "
                 "WHERE entity_id=%s AND is_primary=1 AND deleted_at IS NULL", (entityId,))
    liveBefore = dcur.fetchone()[0]
    blocked = None
    try:
        dcur.execute("DELETE FROM entities WHERE id=%s", (entityId,))
        dc.commit()
    except pymysql.err.IntegrityError as e:
        blocked = e; dc.rollback()
    note(isinstance(blocked, pymysql.err.IntegrityError) and liveBefore == 1,
         f"hard entity-delete with a live primary → IntegrityError (FK RESTRICT) "
         f"(got {type(blocked).__name__ if blocked else 'no error'})")
    # The live primary is untouched — still owned, still live. No orphan created.
    dcur.execute("SELECT COUNT(*) FROM ip_addresses "
                 "WHERE is_primary=1 AND entity_id IS NULL AND deleted_at IS NULL "
                 "AND subnet_id=%s", (subnetId,))
    note(dcur.fetchone()[0] == 0,
         "RESTRICT left no orphan live primary (entity_id never nulled)")
    # N1d: once the referencing IPs are gone, the entity deletes cleanly — RESTRICT
    # blocks only while children exist, it is not a permanent lock. Assert the row
    # counts, not just the absence of an exception: a 0-row DELETE also raises
    # nothing, and would make a "cleared" flag lie if the entity had actually been
    # removed earlier (e.g. RESTRICT silently not enforced in N1c).
    dcur.execute("SELECT COUNT(*) FROM entities WHERE id=%s", (entityId,))
    stillPresent = dcur.fetchone()[0]          # must be 1: N1c's DELETE was blocked
    dcur.execute("DELETE FROM ip_addresses WHERE entity_id=%s", (entityId,))
    dc.commit()
    cleared = False
    deletedRows = 0
    try:
        dcur.execute("DELETE FROM entities WHERE id=%s", (entityId,))
        deletedRows = dcur.rowcount          # must be exactly 1: the entity was real
        dc.commit()
        cleared = True
    except pymysql.err.IntegrityError:
        dc.rollback()
    note(cleared and stillPresent == 1 and deletedRows == 1,
         f"entity survived the RESTRICT block (present={stillPresent}) and deletes "
         f"cleanly once its ip_addresses rows are removed first (rows={deletedRows})")

    dcur.execute("DELETE FROM ip_addresses WHERE subnet_id=%s", (subnetId,))
    dc.commit(); dc.close()


def scenarioReparentLock(functionSlug, subnetId):
    """Third-review BLOCKER B2: a concurrent interface reparent between the
    entity-derivation read and the INSERT would key the new primary to the wrong
    entity. The fix makes _entityIdForInterface read the interface row FOR UPDATE,
    holding the lock through the INSERT until the caller commits. Prove the lock is
    actually taken: an allocate() in flight (uncommitted) must block a concurrent
    UPDATE of that interface's entity_id, and release it on commit/rollback."""
    setupC = conn()
    entityA = _makeTaggedEntity(setupC, functionSlug, primary=True)
    entityB = _makeTaggedEntity(setupC, functionSlug, primary=True)
    ifaceId = _makeInterface(setupC, entityA)
    setupC.close()

    # c1: allocate a primary via the interface but DO NOT commit — allocate() does
    # not commit (caller owns the boundary), so the FOR UPDATE lock on the
    # interface row is still held after it returns.
    c1 = conn(); cur1 = c1.cursor()
    addr = allocator.allocate(c1, functionSlug, interfaceId=ifaceId,
                              entityId=None, isPrimary=True)
    cur1.execute("SELECT entity_id FROM ip_addresses "
                 "WHERE address=CAST(%s AS INET6) AND deleted_at IS NULL", (addr,))
    note(cur1.fetchone()[0] == entityA,
         "in-flight allocate() inserted the primary owned by the interface's entity")

    # c2: a concurrent reparent of the SAME interface must block on c1's row lock.
    # Cap the wait at 1s so a genuine lock shows up as a timeout rather than hanging
    # the test; a missing lock would let the UPDATE commit immediately.
    c2 = conn(); cur2 = c2.cursor()
    cur2.execute("SET SESSION innodb_lock_wait_timeout=1")
    lockErr = None
    try:
        cur2.execute("UPDATE interfaces SET entity_id=%s WHERE id=%s",
                     (entityB, ifaceId))
        c2.commit()
    except pymysql.err.OperationalError as e:
        lockErr = e; c2.rollback()
    # 1205 = ER_LOCK_WAIT_TIMEOUT: the reparent was held off by allocate()'s lock.
    note(lockErr is not None and lockErr.args[0] == 1205,
         f"concurrent reparent blocked while allocate() holds the interface lock "
         f"(got {lockErr.args[0] if lockErr else 'no error'})")

    # Release c1's lock; the reparent then succeeds — the lock was transient, not a
    # deadlock, and serialized the two writers rather than losing the update.
    c1.rollback(); c1.close()
    cur2.execute("UPDATE interfaces SET entity_id=%s WHERE id=%s", (entityB, ifaceId))
    c2.commit()
    cur2.execute("SELECT entity_id FROM interfaces WHERE id=%s", (ifaceId,))
    note(cur2.fetchone()[0] == entityB,
         "reparent succeeds once allocate() releases the interface lock")
    c2.close()

    # cleanup (IPs first: fk_ip_entity is now ON DELETE RESTRICT)
    cc = conn(); ccur = cc.cursor()
    ccur.execute("DELETE FROM ip_addresses WHERE subnet_id=%s", (subnetId,))
    ccur.execute("DELETE FROM interfaces WHERE id=%s", (ifaceId,))
    ccur.execute("DELETE FROM entities WHERE id IN (%s,%s)", (entityA, entityB))
    cc.commit(); cc.close()


def scenarioReparentSnapshot(functionSlug, subnetId):
    """Fourth-review B3 disposition — CHARACTERIZE the accepted boundary of the B2
    lock, so the limit is a tested fact rather than a silent assumption.

    _entityIdForInterface FOR UPDATE closes the *in-flight* TOCTOU: a reparent
    cannot interleave between the entity read and the INSERT within one allocate()
    txn (scenario 8 proves the lock). It does NOT, and is not meant to, keep an
    already-COMMITTED ip_addresses.entity_id in sync with a LATER reparent of the
    interface — entity_id is snapshotted at allocation time. Reconciling existing
    allocations to a reparented interface is a distinct data operation (a sibling
    of the Phase-4 retirement/reparent transaction), out of scope for the
    allocator. This test asserts that documented behaviour: after allocate()
    commits, a subsequent reparent leaves the prior allocation pointing at the
    original owner. If a future reparent-reconcile changes this, this test should
    change with it — deliberately, not by surprise."""
    setupC = conn()
    entityA = _makeTaggedEntity(setupC, functionSlug, primary=True)
    entityB = _makeTaggedEntity(setupC, functionSlug, primary=True)
    ifaceId = _makeInterface(setupC, entityA)
    setupC.close()

    # Allocate a primary via the interface and COMMIT — a settled allocation.
    c1 = conn()
    addr = allocator.allocate(c1, functionSlug, interfaceId=ifaceId,
                              entityId=None, isPrimary=True)
    c1.commit(); c1.close()

    # Reparent the interface to entityB, in its own committed txn (no allocate()
    # lock is held now, so this just succeeds).
    c2 = conn(); cur2 = c2.cursor()
    cur2.execute("UPDATE interfaces SET entity_id=%s WHERE id=%s", (entityB, ifaceId))
    c2.commit()

    # The already-allocated address still names entityA: the snapshot did not
    # follow the reparent. This is the accepted boundary, not a bug.
    cur2.execute("SELECT entity_id FROM ip_addresses "
                 "WHERE address=CAST(%s AS INET6) AND deleted_at IS NULL", (addr,))
    owner = cur2.fetchone()[0]
    note(owner == entityA,
         f"post-commit reparent does NOT rewrite a settled allocation's owner "
         f"(entity_id stayed {entityA}, got {owner}) — reconcile is a separate op")
    c2.close()

    # cleanup (IPs first: fk_ip_entity is ON DELETE RESTRICT)
    cc = conn(); ccur = cc.cursor()
    ccur.execute("DELETE FROM ip_addresses WHERE subnet_id=%s", (subnetId,))
    ccur.execute("DELETE FROM interfaces WHERE id=%s", (ifaceId,))
    ccur.execute("DELETE FROM entities WHERE id IN (%s,%s)", (entityA, entityB))
    cc.commit(); cc.close()


def main():
    c = conn()
    networkId, subnetId, functionSlug = setupFixtures(c)
    try:
        print("scenario 1: locked allocate() race")
        scenarioLockedRace(functionSlug)
        # reset addresses for the isolated constraint scenarios
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        print("scenario 2: no-lock same-address stress")
        scenarioNoLockSameAddress(subnetId)
        print("scenario 3: direct duplicate + soft-delete reuse")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioDirectDuplicate(subnetId)
        print("scenario 4: entity primary derivation + one-primary + savepoint")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioEntityPrimaryAndSavepoint(functionSlug, subnetId)
        print("scenario 5: retry branch (savepoint-scoped recompute)")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioRetryBranch(functionSlug, subnetId)
        print("scenario 6: interface-derived entity (B2 bypass closed)")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioInterfaceDerivation(functionSlug, subnetId)
        print("scenario 7: one-primary transitions (UPDATE / soft-delete / RESTRICT)")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioPrimaryTransitions(functionSlug, subnetId)
        print("scenario 8: concurrent interface reparent blocked by FOR UPDATE (B2)")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioReparentLock(functionSlug, subnetId)
        print("scenario 9: post-commit reparent snapshot boundary (B3 characterize)")
        teardownFixtures(c, networkId, subnetId)
        networkId, subnetId, functionSlug = setupFixtures(c)
        scenarioReparentSnapshot(functionSlug, subnetId)
    finally:
        teardownFixtures(c, networkId, subnetId)
        c.close()

    if FAILS:
        print(f"\n{len(FAILS)} DB check(s) FAILED")
        return 1
    print("\nall DB concurrency proofs passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
