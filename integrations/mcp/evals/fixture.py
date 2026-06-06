"""
Deterministic fixture dataset + injectable clients for the evaluation suite.

The same dataset backs both the REST tools (via httpx.MockTransport) and the DB
tools (via a small in-memory runner), so the 10 evaluation questions in
evaluation.xml are answerable read-only and verify to stable answers regardless
of any live SolariNet deployment.
"""
from __future__ import annotations

import json
from typing import Any

import httpx

from solarinet_mcp.config import DbConfig, RestConfig
from solarinet_mcp.db import DbClient
from solarinet_mcp.rest import RestClient

# ---- the canonical dataset -----------------------------------------------
NODES: list[dict[str, Any]] = [
    {"nodeId": "0x01", "role": "server",  "hostFqdn": "argon.akoria.net",     "state": "up",       "arch": "x86_64", "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:00Z"},
    {"nodeId": "0x02", "role": "server",  "hostFqdn": "neon.akoria.net",      "state": "up",       "arch": "arm64",  "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:00Z"},
    {"nodeId": "0x10", "role": "monitor", "hostFqdn": "vantage1.akoria.net",  "state": "up",       "arch": "arm64",  "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:01Z"},
    {"nodeId": "0x11", "role": "monitor", "hostFqdn": "vantage2.akoria.net",  "state": "degraded", "arch": "armv7",  "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:01Z"},
    {"nodeId": "0x20", "role": "client",  "hostFqdn": "hydrogen.akoria.net",  "state": "up",       "arch": "x86_64", "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:02Z"},
    {"nodeId": "0x21", "role": "client",  "hostFqdn": "helium.akoria.net",    "state": "down",     "arch": "arm64",  "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T21:30:00Z"},
    {"nodeId": "0x22", "role": "client",  "hostFqdn": "lithium.akoria.net",   "state": "up",       "arch": "armv7",  "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:02Z"},
    {"nodeId": "0x23", "role": "client",  "hostFqdn": "beryllium.akoria.net", "state": "degraded", "arch": "x86_64", "osName": "Linux 6.1.0", "lastSeenAt": "2026-06-06T22:00:02Z"},
]

PROBES: list[dict[str, Any]] = [
    {"targetId": "tcp:hydrogen:443", "label": "web",     "proto": "tcp",
     "vantages": [{"monitorNode": "0x10", "outcome": "ok", "rttMicros": 1500},
                  {"monitorNode": "0x11", "outcome": "ok", "rttMicros": 1800}]},
    {"targetId": "icmp:10.42.0.1", "label": "gateway",   "proto": "icmp",
     "vantages": [{"monitorNode": "0x10", "outcome": "ok", "rttMicros": 300},
                  {"monitorNode": "0x11", "outcome": "timeout", "rttMicros": 0}]},
    {"targetId": "udp:10.42.0.53:53", "label": "dns",    "proto": "udp",
     "vantages": [{"monitorNode": "0x10", "outcome": "ok", "rttMicros": 900},
                  {"monitorNode": "0x11", "outcome": "ok", "rttMicros": 950}]},
    {"targetId": "tcp:helium:22", "label": "ssh",        "proto": "tcp",
     "vantages": [{"monitorNode": "0x10", "outcome": "refused", "rttMicros": 0},
                  {"monitorNode": "0x11", "outcome": "refused", "rttMicros": 0}]},
    {"targetId": "tcp:beryllium:8086", "label": "metrics", "proto": "tcp",
     "vantages": [{"monitorNode": "0x10", "outcome": "ok", "rttMicros": 2200},
                  {"monitorNode": "0x11", "outcome": "ok", "rttMicros": 5000}]},
]

# Active alerts only (clearedAt IS NULL).
ALERTS_ACTIVE: list[dict[str, Any]] = [
    {"eventId": 101, "severity": "crit", "nodeId": "0x21", "targetId": None, "detail": "host down",       "firedAt": "2026-06-06T20:00:00Z"},
    {"eventId": 102, "severity": "warn", "nodeId": "0x11", "targetId": None, "detail": "cpu high",        "firedAt": "2026-06-06T21:10:00Z"},
    {"eventId": 103, "severity": "crit", "nodeId": None, "targetId": "tcp:helium:22", "detail": "service refused", "firedAt": "2026-06-06T20:05:00Z"},
    {"eventId": 104, "severity": "warn", "nodeId": "0x23", "targetId": None, "detail": "disk low",        "firedAt": "2026-06-06T21:40:00Z"},
]

SERVER_STATUS = {"primary": "0x01", "failover": "0x02", "leaseEpoch": 5, "expiresAt": "2026-06-06T22:00:15Z"}

# hostHistory keyed by nodeId; columns: sampledAt, cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct
HOST_HISTORY: dict[str, list[tuple]] = {
    "0x20": [("2026-06-06T22:00:00Z", 410, 5242880, 0, 41)],
    "0x21": [("2026-06-06T21:30:00Z", 120, 1048576, 0, 55)],
    "0x22": [("2026-06-06T22:00:00Z", 250, 2097152, 0, 60)],
    "0x23": [("2026-06-06T22:00:00Z", 920, 7340032, 1024, 4)],
}


# ---- REST mock ------------------------------------------------------------
def _ok(data: Any, status: int = 200) -> httpx.Response:
    return httpx.Response(status, json={"ok": True, "data": data, "ts": "2026-06-06T22:00:00Z"})


def _err(message: str, status: int = 404) -> httpx.Response:
    return httpx.Response(status, json={"ok": False, "error": {"code": "not_found", "message": message}})


def _handler(request: httpx.Request) -> httpx.Response:
    path = request.url.path
    if path == "/api/nodes":
        role = request.url.params.get("role")
        data = [n for n in NODES if not role or n["role"] == role]
        return _ok(data)
    if path.startswith("/api/nodes/") and path.endswith("/history"):
        return _ok([])
    if path.startswith("/api/nodes/"):
        nid = path.rsplit("/", 1)[1]
        for n in NODES:
            if n["nodeId"] == nid:
                return _ok(n)
        return _err(f"node {nid}")
    if path == "/api/probes":
        return _ok(PROBES)
    if path == "/api/alerts":
        status = request.url.params.get("status", "active")
        return _ok(ALERTS_ACTIVE if status in ("active", "all") else [])
    if path == "/api/server/status":
        return _ok(SERVER_STATUS)
    if path == "/api/topology":
        return _ok({"nodes": NODES, "edges": []})
    return _err(path)


# ---- DB runner ------------------------------------------------------------
def _db_runner(sql: str, params, max_rows: int):
    low = sql.lower()
    if "from node" in low:
        cols = ["nodeId", "role", "hostFqdn", "state", "lastSeenAt", "arch", "osName"]
        rows = [tuple(n[c] for c in cols) for n in NODES]
        return cols, rows
    if "from hosthistory" in low:
        cols = ["sampledAt", "cpuAvgMilli", "ramUsedKb", "swapUsedKb", "diskMinFreePct"]
        nid = str(params[0]) if params else None
        return cols, list(HOST_HISTORY.get(nid, []))
    if "from alertevent" in low:
        cols = ["eventId", "firedAt", "severity", "detail", "nodeId", "targetId", "metric", "op", "threshold"]
        rows = [(a["eventId"], a["firedAt"], a["severity"], a["detail"], a["nodeId"], a["targetId"], None, None, None)
                for a in ALERTS_ACTIVE]
        return cols, rows
    return ["one"], [(1,)]


def build_rest_client() -> RestClient:
    cfg = RestConfig(base_url="https://c2.fixture", token="evaltoken", timeout=5.0, verify_tls=True)
    return RestClient(cfg, transport=httpx.MockTransport(_handler))


def build_db_client() -> DbClient:
    cfg = DbConfig(host="fixture", port=3306, name="solarinet", user="ro", password="x", timeout=5)
    return DbClient(cfg, runner=_db_runner)
