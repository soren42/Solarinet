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


def scene(systems, pools, alert=None, base=None):
    return {"history": [sample(i, systems, pools, alert, base) for i in range(60)]}


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
    alert = {"alertId": 9, "episodeId": 9, "severity": 1,
             "subject": "STEEL.AKORIA. HEALTH.FAILEDUN.",
             "detail": "CHECK HEALTH.FAILEDUN. ON STEEL.AKORIA."}
    scenes = {
        "live9": scene(live, POOLS, alert, {"alarm": True}),
        "small3": scene(live[:3], POOLS[:1]),
        "big64": scene(fleet(64, down=2, degraded=5), POOLS,
                       base={"rx": 400000, "tx": 180000, "score": 120}),
    }
    for name, body in scenes.items():
        (d / f"{name}.json").write_text(json.dumps(body))


if __name__ == "__main__":
    main(sys.argv[1])
