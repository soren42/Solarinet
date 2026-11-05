#!/usr/bin/env python3
"""Write simulator scenes: a replayable history of /api/panel-shaped samples.

Names mirror the live fleet so a contact sheet reads like the wall. The numbers
are illustrative, not measured, and the alerts are the text shapes the server
produces.
"""
import json
import math
import random
import sys
from pathlib import Path

LIVE = [
    ("XENON", 0, 1), ("CESIUM", 0, 1), ("RADIUM", 0, 1), ("CHLORINE", 0, 1),
    ("STEEL", 1, 1), ("HOLE", 1, 1), ("NITROGEN", 2, 1), ("TUNGSTEN", 2, 1),
    ("VALENCE", 3, 1),
]
POOLS = ["CORE INFRA", "CORE SVC", "MONITORING", "NETWORK", "UNASSIGNED"]


def sample(i, systems, pools, alert=None, base=None):
    rng = random.Random(1000 + i)
    base = base or {}
    roll = {"up": 0, "degraded": 0, "down": 0, "unknown": 0, "maint": 0}
    sysOut = []
    for name, pool, tier, state, load in systems:
        roll[state] += 1
        sysOut.append({"name": name, "pool": pool, "tier": tier, "state": state,
                       "loadPct": max(0, min(100, load + rng.randint(-4, 4)))})
    poolOut = []
    for p, pname in enumerate(pools):
        mem = [s for s in sysOut if s["pool"] == p]
        counts = {k: sum(1 for s in mem if s["state"] == k) for k in roll}
        poolOut.append({"name": pname, "tier": p, **counts, "total": len(mem),
                        "loadPct": sum(s["loadPct"] for s in mem) // max(1, len(mem))})
    wave = math.sin(i / 6.0)
    out = {
        "ts": 1793000000 + i * 2, "seq": 1000 + i,
        "score": base.get("score", 40),
        "stateRoll": roll,
        "alerts": base.get("alerts", {"info": 2, "warn": 98, "crit": 0}),
        "meanLoadPct": sum(s["loadPct"] for s in sysOut) // max(1, len(sysOut)),
        "rxKbps": int(base.get("rx", 800) * (1 + 0.3 * wave)),
        "txKbps": int(base.get("tx", 400) * (1 - 0.2 * wave)),
        "rttTenthMs": base.get("rtt", 34) + rng.randint(-3, 3),
        "lossPermille": base.get("loss", 0),
        "pools": poolOut, "systems": sysOut,
        "alarmActive": base.get("alarm", False),
        "dataStale": False,
        "gear": base.get("gear", [
            {"role": 0, "state": 1, "rxLevel": 2, "txLevel": 2},
            {"role": 1, "state": 1, "rxLevel": 0, "txLevel": 0},
            {"role": 2, "state": 1, "rxLevel": 0, "txLevel": 0},
            {"role": 3, "state": 1, "rxLevel": 0, "txLevel": 0}]),
    }
    if alert:
        out["topAlert"] = alert
    return out


ALERT_HOSTS = [("STEEL", "NO RESPONSE", 2, 1), ("HOLE", "2 SVCS FAILED", 1, 2),
               ("NITROGEN", "DISK READONLY", 2, 3), ("TUNGSTEN", "DISK FAILING", 1, 4),
               ("CESIUM", "KERNEL ERRORS", 1, 5), ("RADIUM", "SAMBA FAILED", 1, 2)]

LINKS = [
    ("WAN", "wan", None, 1200, 3200), ("BACKUP", "wan", None, 0, 0),
    ("CHEMISTRY", "trunk", 0, 900, 2800), ("SLIDE", "trunk", 2, 420, 900),
    ("PIPETTE", "trunk", 3, 40, 110), ("LABORATORY", "wifi", 2, 12, 60),
    ("XENON", "host", 3, 145, 741), ("CESIUM", "host", 3, 90, 210),
    ("LITHIUM", "host", 4, 3, 9), ("BENZENE", "host", 2, None, None),
]


def v2(i, count, rng, heavy=False):
    """The CONTRACT-V2 §4 keys for one sample. Rates wander so history has shape."""
    scale = 1.0 + 0.7 * math.sin(i / 5.0) + 0.3 * math.sin(i / 1.7)
    def jig(v):
        return None if v is None else int(v * max(0.05, scale))
    links = []
    for name, kind, parent, up, down in LINKS:
        links.append({"name": name, "kind": kind, "state": "ok", "parent": parent,
                      "upKbps": jig(up), "downKbps": jig(down)})
    probes = []
    for p, name in enumerate(["WAN GW", "1.1.1.1", "XENON", "CESIUM", ".198", ".253"]):
        dead = name in (".198", ".253")
        lost = (p == 1 and 20 <= i <= 23)
        probes.append({"name": name, "lossPermille": 1000 if dead or lost else 0,
                       "state": "unknown" if dead else "ok",
                       "rttTenthMs": 0 if dead or lost else
                           int((20 + p * 30) * (2.6 if p == 1 and 30 <= i <= 36 else 1)) + rng.randint(0, 6)})
    items = [{"severity": sev, "code": code, "ageMin": [4, 95, 180, 2880, 17, 64][k],
              "host": host, "msg": msg}
             for k, (host, msg, sev, code) in enumerate(ALERT_HOSTS)]
    warn = [0] * 52
    crit = [0] * 52
    for k in (4, 5, 6, 21, 30, 31, 44, 50):
        warn[k] = 1 + (k * 7) % 12
    for k in (5, 31, 50):
        crit[k] = 1 + k % 4
    return {
        "links": links, "probes": probes,
        "alertList": {"total": 14, "faultCounts": [1, 2, 4, 1, 1, 1, 1], "items": items},
        "alertHeat": {"warn": warn, "crit": crit},
        "totals": {"wanUpKbps": jig(1200), "wanDownKbps": jig(3200),
                   "lanUpKbps": jig(9000 if heavy else 600), "lanDownKbps": jig(12000 if heavy else 900)},
    }


def scene(systems, pools, alert=None, base=None, tail=0):
    hist = []
    for i in range(60):
        rng = random.Random(9000 + i)
        s = sample(i, systems, pools, alert, base)
        s.update(v2(i, len(systems), rng, heavy=len(systems) > 20))
        for k, y in enumerate(s["systems"]):
            agentless = k % 4 == 3
            y["memPct"] = None if agentless else 20 + (k * 13) % 72
            y["diskPct"] = None if agentless else 15 + (k * 29) % 80
            y["faults"] = 0
            y["rxKbps"] = None if agentless else (k * 37 + 3) * 11 % 900
            y["txKbps"] = None if agentless else (k * 53 + 5) * 7 % 700
        tailRng = random.Random(77)
        s["popTail"] = [(0 << 5) | tailRng.randint(1, 26) for _ in range(tail)]
        hist.append(s)
    return {"history": hist}


def fleet(n, down=0, degraded=0):
    rng = random.Random(n)
    out = []
    for i in range(n):
        state = "down" if i < down else "degraded" if i < down + degraded else "up"
        out.append((f"H{i:03d}", i % 5, i % 3, state, rng.randint(3, 70)))
    return out


def main(dest):
    d = Path(dest)
    d.mkdir(parents=True, exist_ok=True)
    live = [(n, p, t, "up", (17 * k + 6) % 45) for k, (n, p, t) in enumerate(LIVE)]
    alert = {"alertId": 9, "episodeId": 9, "severity": 2,
             "subject": "STEEL", "detail": "NO RESPONSE"}
    scenes = {
        "live9": scene(live, POOLS, alert, {"alarm": True}),
        "small3": scene(live[:3], POOLS[:1]),
        "big64": scene(fleet(64, down=2, degraded=5), POOLS,
                       base={"rx": 400000, "tx": 180000, "score": 120}),
        "huge200": scene(fleet(64, down=3, degraded=6), POOLS, tail=136,
                         base={"rx": 900000, "tx": 500000, "score": 140}),
    }
    for name, body in scenes.items():
        (d / f"{name}.json").write_text(json.dumps(body))


if __name__ == "__main__":
    main(sys.argv[1])
