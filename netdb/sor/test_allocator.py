#!/usr/bin/env python3
"""
test_allocator.py — pure unit tests for allocator.selectAddress.

No database required: runs on plain python3 (stdlib ipaddress only), so the
off-by-one / range-interaction logic is verifiable anywhere. The DB concurrency
proof (that migration 021's uq_ip_addr_live actually stops duplicate live
allocations) lives separately in test_allocator_db.py, which needs a scratch
MariaDB.

Run:  python3 netdb/sor/test_allocator.py
Exit: 0 all pass, 1 on first failure (with a message).
"""

import ipaddress
import sys

# Import the module under test regardless of CWD.
sys.path.insert(0, __file__.rsplit("/", 1)[0])
from allocator import selectAddress  # noqa: E402


FAILS = []


def check(name, got, want):
    if got != want:
        FAILS.append(f"{name}: got {got!r}, want {want!r}")
    else:
        print(f"  ok  {name}")


def expectRaises(name, fn):
    try:
        fn()
    except Exception:
        print(f"  ok  {name} (raised)")
        return
    FAILS.append(f"{name}: expected an exception, none raised")


def run():
    # --- reserved-address exclusion: network, gateway, broadcast ------------
    # /24, gw .1, nothing used → first host is .2 (.0 net, .1 gw skipped).
    check("skips network+gateway",
          selectAddress("10.1.0.0/24", "10.1.0.1", used=[], ranges=[]),
          "10.1.0.2")

    # No gateway declared → .1 is free and chosen first.
    check("no gateway → .1 free",
          selectAddress("10.1.0.0/24", None, used=[], ranges=[]),
          "10.1.0.1")

    # --- used-address exclusion --------------------------------------------
    check("skips used addresses",
          selectAddress("10.1.0.0/24", "10.1.0.1",
                        used=["10.1.0.2", "10.1.0.3"], ranges=[]),
          "10.1.0.4")

    # used given out of order still skipped (set semantics).
    check("used order-independent",
          selectAddress("10.1.0.0/24", "10.1.0.1",
                        used=["10.1.0.4", "10.1.0.2", "10.1.0.3"], ranges=[]),
          "10.1.0.5")

    # --- dynamic_pool / excluded never allocated ---------------------------
    check("skips dynamic pool",
          selectAddress("10.1.0.0/24", "10.1.0.1", used=[],
                        ranges=[{"kind": "dynamic_pool",
                                 "start": "10.1.0.2", "end": "10.1.0.10"}]),
          "10.1.0.11")

    check("skips excluded range",
          selectAddress("10.1.0.0/24", "10.1.0.1", used=[],
                        ranges=[{"kind": "excluded",
                                 "start": "10.1.0.2", "end": "10.1.0.2"}]),
          "10.1.0.3")

    # --- static_block: if present, allocate ONLY inside it -----------------
    check("static_block restricts allocation",
          selectAddress("10.1.0.0/24", "10.1.0.1", used=[],
                        ranges=[{"kind": "static_block",
                                 "start": "10.1.0.50", "end": "10.1.0.60"}]),
          "10.1.0.50")

    # static_block + an exclusion inside it → first free inside block.
    check("static_block minus exclusion",
          selectAddress("10.1.0.0/24", "10.1.0.1", used=["10.1.0.50"],
                        ranges=[{"kind": "static_block",
                                 "start": "10.1.0.50", "end": "10.1.0.60"},
                                {"kind": "excluded",
                                 "start": "10.1.0.51", "end": "10.1.0.51"}]),
          "10.1.0.52")

    # --- exhaustion → None -------------------------------------------------
    # /30 has hosts .1 and .2; gw .1 + used .2 → nothing left.
    check("exhausted /30 → None",
          selectAddress("10.1.0.0/30", "10.1.0.1", used=["10.1.0.2"], ranges=[]),
          None)

    # static_block fully used → None even though the subnet has other free space.
    check("static_block exhausted → None",
          selectAddress("10.1.0.0/24", "10.1.0.1", used=["10.1.0.50"],
                        ranges=[{"kind": "static_block",
                                 "start": "10.1.0.50", "end": "10.1.0.50"}]),
          None)

    # --- /31 and /30 boundaries (RFC 3021 + small subnets) -----------------
    # /31: both addresses are hosts, no network/broadcast reserve. gw .0 → .1.
    check("/31 gateway .0 → .1",
          selectAddress("10.1.0.0/31", "10.1.0.0", used=[], ranges=[]),
          "10.1.0.1")

    # /31 no gateway → .0 (the lower host).
    check("/31 no gateway → .0",
          selectAddress("10.1.0.0/31", None, used=[], ranges=[]),
          "10.1.0.0")

    # /32 single host, no gateway → the one address.
    check("/32 → the single host",
          selectAddress("10.1.0.5/32", None, used=[], ranges=[]),
          "10.1.0.5")

    # /30 first free is .1 (net .0, broadcast .3 excluded by hosts()).
    check("/30 first host is .1",
          selectAddress("10.1.0.0/30", None, used=[], ranges=[]),
          "10.1.0.1")

    # broadcast is never handed out on a /24.
    got = selectAddress("10.1.0.0/24", "10.1.0.1",
                        used=[str(a) for a in
                              ipaddress.ip_network("10.1.0.0/24").hosts()
                              if str(a) != "10.1.0.255"],
                        ranges=[])
    check("never allocates broadcast", got, None)

    # --- IPv6 rejected explicitly ------------------------------------------
    expectRaises("rejects IPv6 subnet",
                 lambda: selectAddress("fd00::/64", None, used=[], ranges=[]))
    expectRaises("rejects IPv6 in used",
                 lambda: selectAddress("10.1.0.0/24", "10.1.0.1",
                                       used=["fd00::1"], ranges=[]))
    expectRaises("rejects unknown range kind",
                 lambda: selectAddress("10.1.0.0/24", "10.1.0.1", used=[],
                                       ranges=[{"kind": "bogus",
                                                "start": "10.1.0.2",
                                                "end": "10.1.0.2"}]))
    # A reversed range (start > end) is corrupt data, not an empty range — it
    # must be rejected loudly, never silently ignored (which would hand out an
    # address the operator meant to reserve).
    expectRaises("rejects reversed dhcp range",
                 lambda: selectAddress("10.1.0.0/24", "10.1.0.1", used=[],
                                       ranges=[{"kind": "dynamic_pool",
                                                "start": "10.1.0.100",
                                                "end": "10.1.0.10"}]))

    # --- property test: no address is ever handed out twice, and every -----
    # --- returned address satisfies all constraints ------------------------
    _propertyScan()

    if FAILS:
        print("\nFAILURES:")
        for f in FAILS:
            print("  x " + f)
        print(f"\n{len(FAILS)} failed")
        return 1
    print("\nall pure allocator tests passed")
    return 0


def _propertyScan():
    """Repeatedly allocate from a /26 until exhausted; assert every address is
    distinct, valid, in-subnet, and never a reserved/forbidden one. This is the
    invariant the whole allocator exists to uphold."""
    cidr = "10.9.0.0/26"        # 62 usable hosts
    gw = "10.9.0.1"
    net = ipaddress.ip_network(cidr)
    forbidden = [{"kind": "dynamic_pool", "start": "10.9.0.40", "end": "10.9.0.50"},
                 {"kind": "excluded", "start": "10.9.0.10", "end": "10.9.0.10"}]
    used = []
    seen = set()
    while True:
        got = selectAddress(cidr, gw, used=used, ranges=forbidden)
        if got is None:
            break
        ip = ipaddress.ip_address(got)
        if got in seen:
            FAILS.append(f"property: {got} handed out twice")
            return
        if ip not in net or ip == net.network_address or ip == net.broadcast_address:
            FAILS.append(f"property: {got} out of range or net/broadcast")
            return
        if got == gw:
            FAILS.append(f"property: gateway {got} handed out")
            return
        if ipaddress.ip_address("10.9.0.40") <= ip <= ipaddress.ip_address("10.9.0.50"):
            FAILS.append(f"property: {got} inside dynamic pool")
            return
        if got == "10.9.0.10":
            FAILS.append(f"property: {got} was excluded")
            return
        seen.add(got)
        used.append(got)

    # 62 hosts − gateway(.1) − 11 in dynamic pool(.40-.50) − 1 excluded(.10) = 49.
    expected = 62 - 1 - 11 - 1
    if len(seen) != expected:
        FAILS.append(f"property: allocated {len(seen)} addresses, expected {expected}")
    else:
        print(f"  ok  property scan ({len(seen)} distinct valid allocations)")


if __name__ == "__main__":
    raise SystemExit(run())
