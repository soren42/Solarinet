#!/usr/bin/env python3
"""
allocator.py — IP-by-function address allocation for the SolariNet provisioning
subsystem (docs/SolariNet_Provisioning_Plan.html §6).

Given a function tag, the allocator finds the tag's home subnet and hands back
the next free static address in it, honouring reserved addresses (network,
broadcast, gateway), dhcp_ranges (never allocate from a dynamic_pool or an
excluded range; allocate ONLY from static_block ranges when any exist), and
every address already live in ip_addresses. IPv4-only for now — v6 is out of
scope this phase and is rejected explicitly rather than mishandled.

Two layers, deliberately split so the hard part is testable without a database:

  selectAddress(...)   PURE. No DB, no I/O. Given a subnet CIDR, gateway, the set
                       of used addresses, and the dhcp ranges, it returns the
                       next free address (or None). All the off-by-one and
                       range-interaction logic lives here and is unit-tested
                       against plain python3 with no MariaDB.

  allocate(conn, ...)  DB. Resolves the function's home subnet, gathers used
                       addresses and ranges under a row lock, calls
                       selectAddress, and INSERTs the ip_addresses row inside the
                       caller's transaction. The DB's uq_ip_addr_live UNIQUE
                       (migration 021) is the real guarantee: two racing
                       allocations cannot both commit the same address — the
                       loser gets an IntegrityError and retries.

House style: camelCase; functions over classes; INET6 columns written via
CAST(%s AS INET6) exactly as netdb/sor/seed.py does.
"""

import ipaddress
import uuid


# ---------------------------------------------------------------------------
# Pure selection layer — no database, fully unit-testable
# ---------------------------------------------------------------------------

def _v4(addr):
    """Parse an address into an IPv4Address; raise on genuine IPv6.

    Input:  addr  str/int/IPv4Address — dotted-quad, or the v4-mapped IPv6 form
            MariaDB's native INET6 column renders (e.g. '::ffff:10.42.0.20').
    Output: IPv4Address
    Raises: ValueError if the value is a genuine IPv6 address or unparseable.
            IPv6 is out of scope this phase; rejecting loudly beats silently
            allocating junk.

    The v4-mapped normalization is essential: the SoR stores every address in an
    INET6 column, so a v4 address round-trips through the DB as '::ffff:a.b.c.d'.
    We fold that back to a plain IPv4Address; only NOT-v4-mapped v6 is rejected.
    """
    if isinstance(addr, ipaddress.IPv4Address):
        return addr
    ip = ipaddress.ip_address(addr)
    if ip.version == 4:
        return ip
    mapped = getattr(ip, "ipv4_mapped", None)
    if mapped is not None:
        return mapped
    raise ValueError(f"IPv6 not supported this phase: {addr}")


def selectAddress(cidr, gateway, used, ranges, reserveHostZero=True):
    """Return the next free static IPv4 address in `cidr`, or None if exhausted.

    Input:
      cidr     str  the subnet in CIDR text, e.g. "10.1.0.0/24"
      gateway  str/None  the gateway address to reserve, or None
      used     iterable of address strings already taken (live ip_addresses)
      ranges   iterable of dicts {kind, start, end}; kind in
               {'dynamic_pool','static_block','excluded'}, start/end inclusive
               dotted-quad strings. Semantics:
                 - dynamic_pool / excluded: never allocate inside them
                 - static_block: if ANY exist, allocate ONLY inside them
                   (the operator has carved out where statics may live)
      reserveHostZero  reserve network + broadcast (True for real subnets).
               For /31 and /32 there is no network/broadcast to reserve and this
               is ignored (RFC 3021 / single-host).

    Output: the chosen address as a str, or None when nothing is free.

    The scan is lowest-address-first and deterministic, so a given SoR state
    always yields the same next address — important for reproducible tests and
    for an operator predicting what a host will get.
    """
    net = ipaddress.ip_network(cidr, strict=False)
    if net.version != 4:
        raise ValueError(f"IPv6 subnet not supported this phase: {cidr}")

    usedSet = {_v4(u) for u in used}
    gw = _v4(gateway) if gateway is not None else None

    # Partition ranges by kind, as (lo, hi) inclusive IPv4Address pairs.
    # A reversed range (start > end) would make `lo <= ip <= hi` never true,
    # SILENTLY disabling the exclusion — so a bad imported range could let the
    # allocator hand out an address that DHCP also owns. Reject it loudly.
    blocks = {"dynamic_pool": [], "static_block": [], "excluded": []}
    for r in ranges:
        kind = r["kind"]
        if kind not in blocks:
            raise ValueError(f"unknown dhcp range kind: {kind}")
        lo, hi = _v4(r["start"]), _v4(r["end"])   # _v4 also enforces same family (v4)
        if lo > hi:
            raise ValueError(f"reversed dhcp range ({kind}): {r['start']} > {r['end']}")
        blocks[kind].append((lo, hi))

    staticBlocks = blocks["static_block"]
    forbidden = blocks["dynamic_pool"] + blocks["excluded"]

    def inAny(ip, spans):
        return any(lo <= ip <= hi for lo, hi in spans)

    # For /31 and /32 every address is a host address (no net/broadcast reserve).
    # Iterate LAZILY — a broad prefix (e.g. a /8) has millions of addresses and
    # materializing them with list() could exhaust memory before the first
    # candidate is even returned.
    wholeSubnet = net.prefixlen >= 31
    hosts = iter(net) if wholeSubnet else net.hosts()

    for ip in hosts:
        if not wholeSubnet:
            # network & broadcast are excluded by hosts(); guard anyway for clarity.
            if ip == net.network_address or ip == net.broadcast_address:
                continue
        if gw is not None and ip == gw:
            continue                      # gateway is reserved
        if ip in usedSet:
            continue                      # already allocated / observed live
        if inAny(ip, forbidden):
            continue                      # inside a dynamic pool or exclusion
        if staticBlocks and not inAny(ip, staticBlocks):
            continue                      # static blocks defined → must be inside one
        return str(ip)

    return None                           # subnet exhausted for statics


# ---------------------------------------------------------------------------
# Database layer — resolves the subnet, locks, inserts inside the caller's txn
# ---------------------------------------------------------------------------

def _sourceId(cur, slug):
    """Resolve a sources.id from its slug, or raise if the source is unknown."""
    cur.execute("SELECT id FROM sources WHERE slug=%s AND deleted_at IS NULL", (slug,))
    row = cur.fetchone()
    if not row:
        raise LookupError(f"source slug not registered: {slug}")
    return row[0]


def _homeSubnetForFunction(cur, functionSlug):
    """Return (subnetId, cidr, gatewayStr) for a function tag's home subnet.

    The function's home network (functions.network_id) selects the segment; the
    subnet is the live IPv4 prefix in that segment. Raises if the tag has no home
    network wired yet (network_id NULL — expected until UniFi import lands) or the
    segment has no IPv4 subnet.
    """
    cur.execute(
        "SELECT network_id FROM functions "
        "WHERE slug=%s AND deleted_at IS NULL", (functionSlug,))
    frow = cur.fetchone()
    if not frow:
        raise LookupError(f"unknown function tag: {functionSlug}")
    networkId = frow[0]
    if networkId is None:
        raise LookupError(
            f"function '{functionSlug}' has no home network wired "
            "(functions.network_id is NULL — wire it after UniFi import)")

    # Read every live subnet in the segment and pick the IPv4 one by PARSING the
    # cidr text — NOT prefix_len <= 32, which an IPv6 /32 also satisfies. Read the
    # gateway via CAST(... AS CHAR) (INET6_NTOA returns NULL on the native INET6
    # type); _v4() normalizes the v4-mapped render downstream. NULL gateway → None.
    # If the segment has more than one IPv4 subnet the choice is ambiguous — error
    # rather than silently taking an arbitrary ORDER BY id row; an explicit
    # function→subnet map (plan §6) resolves that in a later phase.
    cur.execute(
        "SELECT id, cidr, CAST(gateway_ip AS CHAR) "
        "FROM subnets WHERE network_id=%s AND deleted_at IS NULL ORDER BY id", (networkId,))
    v4subnets = []
    for sid, cidr, gw in cur.fetchall():
        try:
            if ipaddress.ip_network(cidr, strict=False).version == 4:
                v4subnets.append((sid, cidr, gw))
        except ValueError:
            continue                      # unparseable cidr text — skip, don't crash
    if not v4subnets:
        raise LookupError(f"no live IPv4 subnet in the home network of '{functionSlug}'")
    if len(v4subnets) > 1:
        cidrs = ", ".join(c for _, c, _ in v4subnets)
        raise LookupError(
            f"ambiguous home subnet for '{functionSlug}': {len(v4subnets)} live IPv4 "
            f"subnets in the segment ({cidrs}) — needs an explicit function-subnet map")
    return v4subnets[0]


def _usedAndRanges(cur, subnetId):
    """Read a subnet's metadata + live addresses + dhcp ranges, holding its lock.

    The subnet row is locked FOR UPDATE so concurrent allocators into the same
    subnet serialize here rather than racing the read-then-insert. The cidr and
    gateway are read from that SAME locked row — not passed in from an earlier
    unlocked read — so a concurrent change to the subnet's prefix or gateway
    cannot leave us allocating against stale metadata (TOCTOU). Any writer that
    mutates a subnet's cidr/gateway or its dhcp_ranges MUST take this same
    FOR UPDATE lock first; that convention is what makes the ranges read below
    (a plain read under READ COMMITTED) see a consistent picture.

    Returns (cidr, gatewayStr, usedList, rangesList) — the last two suitable for
    selectAddress. Raises LookupError if the subnet vanished (soft-deleted)
    between selection and lock.
    """
    # Lock the subnet row — this is the per-subnet allocation mutex — and read its
    # authoritative cidr/gateway in the same statement, under the lock.
    cur.execute(
        "SELECT cidr, CAST(gateway_ip AS CHAR) FROM subnets "
        "WHERE id=%s AND deleted_at IS NULL FOR UPDATE", (subnetId,))
    srow = cur.fetchone()
    if not srow:
        raise LookupError(f"subnet {subnetId} vanished before allocation (soft-deleted?)")
    cidr, gateway = srow[0], srow[1]

    # Every live address whose subnet_id matches, OR (belt-and-braces) that falls
    # inside the prefix even if subnet_id was never backfilled by a direct writer.
    # Native INET6: read via CAST(... AS CHAR), compare via CAST(%s AS INET6)
    # (INET6_NTOA/INET6_ATON do not work on the native type). _v4() folds the
    # v4-mapped render back to dotted-quad downstream.
    net = ipaddress.ip_network(cidr, strict=False)
    # A plain (consistent) read is correct here ONLY because allocate() enforces
    # READ COMMITTED isolation: under READ COMMITTED each statement takes a fresh
    # read view, so this scan — run after we acquired the subnet lock above — sees
    # every address a prior lock-holder committed while we were queued. Under the
    # server default REPEATABLE READ it would instead be served from the snapshot
    # frozen at transaction start (by _sourceId etc.), miss those addresses, and
    # collide; that is exactly what the isolation check in allocate() rejects.
    cur.execute(
        "SELECT CAST(address AS CHAR) FROM ip_addresses "
        "WHERE deleted_at IS NULL AND ("
        "  subnet_id=%s "
        "  OR address BETWEEN CAST(%s AS INET6) AND CAST(%s AS INET6))",
        (subnetId, str(net.network_address), str(net.broadcast_address)))
    used = [r[0] for r in cur.fetchall()]

    # FOR UPDATE locks the existing range rows for the duration of this txn, so a
    # concurrent allocator that also holds the subnet lock cannot mutate a range
    # under us. It does NOT lock the gap, so a brand-new range INSERT is serialized
    # only by the subnet-row lock we took above — hence the standing convention:
    # every dhcp_ranges writer must first SELECT ... FOR UPDATE its subnets row.
    # Locking existing rows here is the enforceable half of that convention.
    cur.execute(
        "SELECT kind, CAST(range_start AS CHAR), CAST(range_end AS CHAR) "
        "FROM dhcp_ranges WHERE subnet_id=%s AND deleted_at IS NULL FOR UPDATE",
        (subnetId,))
    ranges = [{"kind": k, "start": s, "end": e} for k, s, e in cur.fetchall()]
    return cidr, gateway, used, ranges


def _entityIdForInterface(cur, interfaceId):
    """Return the entity that owns an interface (interfaces.entity_id, NOT NULL).

    An interface is always owned by exactly one entity (the column is a NOT NULL
    FK), so an address attached to an interface has a canonical owning entity even
    when the caller passed entityId=None. Deriving it here is what closes the
    bypass where an interface-owned primary skipped the taxonomy check and the
    one-live-primary-per-entity unique (which are both keyed on the entity).

    Raises LookupError if the interface does not exist or is soft-deleted.

    FOR UPDATE is essential, not a nicety: the derived entity_id is persisted at
    the INSERT later in the same transaction. Without the row lock, a concurrent
    reparent (UPDATE interfaces SET entity_id=...) committing between this read and
    our INSERT would leave the new primary keyed to the entity we read, not the one
    that now owns the interface — a silent cross-entity misassignment that also
    dodges the one-primary-per-entity unique. Locking the interface row here holds
    the reparent off until our transaction commits (or rolls back).
    """
    cur.execute(
        "SELECT entity_id FROM interfaces "
        "WHERE id=%s AND deleted_at IS NULL FOR UPDATE", (interfaceId,))
    row = cur.fetchone()
    if not row:
        raise LookupError(f"unknown or deleted interface: {interfaceId}")
    return row[0]


def _reconcileEntity(cur, entityId, interfaceId, isPrimary):
    """Determine the canonical owning entity for an allocation, closing B2.

    An address can be tied to an entity directly (entityId) and/or through an
    interface (interfaceId → interfaces.entity_id). Both routes must agree, and a
    primary (identity) address MUST have a canonical entity so the taxonomy check
    and the uq_ip_addr_primary_entity unique actually apply to it. Returns the
    resolved entityId (possibly derived from the interface).

      * interfaceId given → its owning entity is authoritative; if entityId was
        also given and disagrees, that is a caller bug → error.
      * primary with no resolvable entity → error (would create an entity-less
        primary that silently escapes the one-primary invariant).
    """
    if interfaceId is not None:
        ifaceEntity = _entityIdForInterface(cur, interfaceId)
        if entityId is not None and entityId != ifaceEntity:
            raise ValueError(
                f"entity {entityId} does not own interface {interfaceId} "
                f"(interface belongs to entity {ifaceEntity})")
        entityId = ifaceEntity
    if isPrimary and entityId is None:
        raise ValueError(
            "a primary (identity) allocation requires an owning entity — pass "
            "entityId or an interfaceId whose entity can be derived; an "
            "entity-less primary would escape the one-primary-per-entity invariant")
    return entityId


def _resolveFunctionForEntity(cur, entityId, functionSlug, isPrimary):
    """Reconcile the requested function against the entity's taxonomy membership.

    The taxonomy — not a free-choice argument — decides which segment an entity's
    address comes from, so an allocation for a tagged entity must be consistent
    with entity_functions:
      * entityId None      → nothing to reconcile; use functionSlug verbatim
                             (parking an address with no owner).
      * isPrimary True     → the identity address MUST come from the entity's
                             PRIMARY tag. Derive it; if functionSlug was also
                             given and disagrees, that is a caller bug → error.
                             An entity with no primary tag cannot get an identity
                             address → error (tag it first).
      * isPrimary False    → a secondary address; functionSlug must be a tag the
                             entity actually carries → else error.

    Returns the effective function slug to allocate from.
    """
    if entityId is None:
        if not functionSlug:
            raise ValueError("functionSlug is required when entityId is None")
        return functionSlug

    cur.execute(
        "SELECT f.slug, ef.is_primary "
        "FROM entity_functions ef JOIN functions f ON f.id = ef.function_id "
        "WHERE ef.entity_id = %s AND ef.deleted_at IS NULL", (entityId,))
    memberships = cur.fetchall()
    if not memberships:
        raise LookupError(
            f"entity {entityId} carries no function tags — tag it before allocating")
    tags = {slug for slug, _ in memberships}
    primary = next((slug for slug, isp in memberships if isp == 1), None)

    if isPrimary:
        if primary is None:
            raise LookupError(
                f"entity {entityId} has no PRIMARY function tag — cannot allocate "
                "its identity address")
        if functionSlug and functionSlug != primary:
            raise ValueError(
                f"entity {entityId} primary tag is '{primary}', not '{functionSlug}' "
                "— a primary allocation must use the entity's primary tag")
        return primary

    # secondary: must be a tag the entity holds
    if not functionSlug:
        raise ValueError("functionSlug is required for a non-primary allocation")
    if functionSlug not in tags:
        raise ValueError(
            f"entity {entityId} does not carry tag '{functionSlug}' "
            f"(has: {', '.join(sorted(tags))})")
    return functionSlug


def allocate(conn, functionSlug, entityId=None, interfaceId=None,
             isPrimary=True, sourceSlug="provisioner", assignment="static",
             maxRetries=5):
    """Allocate the next free static IPv4 for an entity/function and INSERT it.

    Input:
      conn          an open DB-API connection (pymysql), autocommit False, running
                    at READ COMMITTED isolation (see the transaction contract)
      functionSlug  the function tag to allocate from; when entityId is given it
                    is reconciled against the entity's taxonomy (may be derived)
      entityId      owning entity id, or None (address parked without an owner)
      interfaceId   owning interface id, or None
      isPrimary     mark the row is_primary (drives the A/PTR record). For a
                    tagged entity this MUST match its primary function.
      sourceSlug    provenance source (default 'provisioner')
      assignment    ip_addresses.assignment value (default 'static')
      maxRetries    retries when a concurrent allocator wins the address race

    Output: the allocated address string.
    Raises: LookupError / ValueError (taxonomy or subnet problems), RuntimeError
            (subnet exhausted or the race was lost maxRetries times), or a
            non-duplicate integrity error propagated unchanged.

    Transaction contract: this does NOT commit — the caller owns the boundary —
    and, critically, it NEVER rolls back the whole transaction. A failed INSERT
    is undone with ROLLBACK TO SAVEPOINT so the caller's earlier work (e.g. the
    entity row it just created) survives. Only a genuine address race
    (uq_ip_addr_live duplicate) is retried; every other integrity failure —
    including a second primary address for the entity (uq_ip_addr_primary_entity)
    or an FK violation — is re-raised immediately rather than masked as a race.

    Isolation: the connection MUST be at READ COMMITTED. An allocator reads free
    state, then writes; under the server default REPEATABLE READ the read view is
    frozen at transaction start, so after we take the subnet lock our free-address
    scan would still be reading a snapshot that predates other allocators' commits
    — picking addresses already taken, colliding, and (since ROLLBACK TO SAVEPOINT
    does not refresh the read view) spinning to exhaustion. READ COMMITTED gives
    each statement a fresh view, so the post-lock scan sees the latest committed
    addresses. We verify it rather than silently misbehave.

    The isolation MUST be set at SESSION (or global) scope, not per-statement. The
    precondition check reads @@tx_isolation, which reports the SESSION value; a
    one-shot `SET TRANSACTION ISOLATION LEVEL ...` (which changes only the *next*
    transaction and leaves @@tx_isolation reading the session default) would let
    the check pass or fail out of step with the transaction actually running. Set
    it on the connection before its first statement and leave it there, e.g.
    pymysql init_command='SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED'.
    (The truly-active level lives in INNODB_TRX.TRX_ISOLATION_LEVEL, but reading it
    needs an open transaction plus PROCESS privilege — disproportionate here, so we
    contract for SESSION scope instead of probing per-transaction.)

    Residual isolation risk, and why it is accepted: a caller could pass the
    @@tx_isolation check yet run the actual transaction at REPEATABLE READ via a
    one-shot `SET TRANSACTION` on a pooled connection. Two things make this a
    fail-SAFE residual rather than a data hole. (1) The failure mode is loud, not
    silent: at RR the post-lock scan misses concurrently-committed addresses,
    picks a taken one, the uq_ip_addr_live UNIQUE rejects the INSERT, ROLLBACK TO
    SAVEPOINT does not refresh the RR snapshot so the retry re-picks the same
    address, and allocate() exhausts maxRetries into a RuntimeError — never a bad
    write. (2) The tempting "fix" — making the used-address scan a locking
    current read (FOR UPDATE) so it sees committed rows under RR — is not viable:
    a locking read of rows changed since the RR snapshot raises error 1020
    ("Record has changed since last read"), verified empirically on 11.8. So the
    scan stays a plain read and READ COMMITTED remains a genuine precondition, not
    a stylistic choice. The autocommit=False guard above closes the other half of
    this surface (autocommit would drop the locks regardless of isolation).

    Concurrency: the subnet row is locked FOR UPDATE before the free-address
    scan, so two allocators into the same subnet serialize; the uq_ip_addr_live
    UNIQUE is the backstop for a writer coming through a different subnet row.
    """
    # Import here so the pure selectAddress layer needs no DB driver installed.
    import pymysql

    cur = conn.cursor()

    # Enforce the autocommit precondition BEFORE the isolation check. allocate()
    # takes a subnet-row lock (FOR UPDATE) and an interface-row lock and holds
    # them across the INSERT so the caller can commit the whole unit atomically;
    # it also uses a SAVEPOINT to unwind a losing INSERT without dropping those
    # locks. Under autocommit every statement commits immediately: FOR UPDATE
    # releases its lock at once (no serialization) and SAVEPOINT/ROLLBACK TO is
    # meaningless. That is silent corruption under concurrency, so refuse it.
    # get_autocommit() reflects the live session state, which is what matters —
    # a caller may have flipped it after connect.
    if conn.get_autocommit():
        raise RuntimeError(
            "allocate() requires autocommit=False; the connection is in "
            "autocommit mode, which releases the FOR UPDATE subnet/interface "
            "locks immediately and defeats the SAVEPOINT retry. Disable "
            "autocommit and commit once after allocate() returns")

    # Enforce the isolation precondition — see the "Isolation" note above. A wrong
    # level is a silent correctness bug under concurrency, so fail loudly.
    # Read @@tx_isolation, NOT @@transaction_isolation: the latter was only added
    # in MariaDB 11.1.1, and this schema targets MariaDB >= 10.7. @@tx_isolation
    # exists on every targeted version (verified on 10.7 and 11.8) and returns the
    # same value format ("READ-COMMITTED"). It reports the SESSION level, so the
    # contract requires SESSION-scope isolation (see the docstring); a per-statement
    # SET TRANSACTION would make this check read out of step with the active txn.
    cur.execute("SELECT @@tx_isolation")
    iso = (cur.fetchone()[0] or "").replace("_", "-").upper()
    if iso != "READ-COMMITTED":
        raise RuntimeError(
            f"allocate() requires READ COMMITTED isolation, got {iso or 'unknown'}; "
            "set SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED on the "
            "connection before use (REPEATABLE READ makes concurrent allocation "
            "spin to exhaustion — see the docstring)")

    sourceId = _sourceId(cur, sourceSlug)
    # Resolve the canonical owning entity FIRST (may be derived from interfaceId),
    # so the taxonomy check below and the DB one-primary unique both key on it — a
    # primary can no longer slip through with entity_id NULL. See _reconcileEntity.
    entityId = _reconcileEntity(cur, entityId, interfaceId, isPrimary)
    effectiveSlug = _resolveFunctionForEntity(cur, entityId, functionSlug, isPrimary)
    subnetId = _homeSubnetForFunction(cur, effectiveSlug)[0]

    # A savepoint name distinct from anything a caller is plausibly using, so our
    # ROLLBACK TO cannot clobber the caller's own savepoint. A full-width uuid4 hex
    # nonce, not a truncation of the cursor id: two live cursors (or nested
    # allocate() calls sharing a connection) could collide a 24-bit value, and a
    # collision would silently retarget the ROLLBACK TO at the wrong savepoint.
    sp = "sp_solari_alloc_" + uuid.uuid4().hex
    DUP_ENTRY = 1062                      # MySQL/MariaDB ER_DUP_ENTRY
    lastErr = None
    for _ in range(maxRetries):
        # cidr/gateway are read UNDER the subnet lock (inside _usedAndRanges), not
        # before it — otherwise a concurrent re-CIDR/re-gateway of the subnet would
        # leave us allocating against stale metadata (TOCTOU). The lock makes the
        # metadata + free-address read a single atomic view.
        cidr, gateway, used, ranges = _usedAndRanges(cur, subnetId)
        chosen = selectAddress(cidr, gateway, used, ranges)
        if chosen is None:
            raise RuntimeError(
                f"no free static address in subnet {cidr} for '{effectiveSlug}'")
        cur.execute("SAVEPOINT " + sp)
        try:
            cur.execute(
                "INSERT INTO ip_addresses "
                "(address, subnet_id, interface_id, entity_id, assignment, "
                " is_primary, source_id, asserted_kind, asserted_at) "
                "VALUES (CAST(%s AS INET6), %s, %s, %s, %s, %s, %s, 'machine', NOW())",
                (chosen, subnetId, interfaceId, entityId, assignment,
                 1 if isPrimary else 0, sourceId))
            cur.execute("RELEASE SAVEPOINT " + sp)
            return chosen
        except pymysql.err.IntegrityError as e:
            # Undo ONLY the failed insert; keep the caller's prior work + the
            # subnet lock held before the savepoint. RELEASE after ROLLBACK TO so
            # no stale savepoint of ours lingers across retries.
            cur.execute("ROLLBACK TO SAVEPOINT " + sp)
            cur.execute("RELEASE SAVEPOINT " + sp)
            code = e.args[0] if e.args else None
            if code == DUP_ENTRY and "uq_ip_addr_live" in str(e):
                lastErr = e               # genuine address race — recompute + retry
                continue
            raise                         # anything else is a real error, not a race
        except Exception:
            # Non-integrity failure of the INSERT (deadlock 1213, lock-wait
            # timeout 1205, driver/connection error, ...). Unwind our savepoint
            # so it does not linger on the connection, then re-raise unchanged so
            # the caller sees the original error. Cleanup is best-effort: if the
            # transaction was already rolled back (a deadlock does this) the
            # savepoint is gone and ROLLBACK TO would itself raise — swallow that
            # so the ORIGINAL exception is what propagates, not the cleanup's.
            try:
                cur.execute("ROLLBACK TO SAVEPOINT " + sp)
                cur.execute("RELEASE SAVEPOINT " + sp)
            except Exception:
                pass
            raise

    raise RuntimeError(
        f"allocation for '{effectiveSlug}' lost the address race {maxRetries}x: {lastErr}")


if __name__ == "__main__":
    # Tiny smoke of the pure layer so `python3 allocator.py` does something useful.
    demo = selectAddress("10.1.0.0/29", "10.1.0.1",
                         used=["10.1.0.2"],
                         ranges=[{"kind": "excluded", "start": "10.1.0.3", "end": "10.1.0.3"}])
    print(f"next free in 10.1.0.0/29 (gw .1, .2 used, .3 excluded): {demo}")
