"""DB tool tests using an injected fake runner (no MariaDB needed)."""
import json

import pytest

from solarinet_mcp import server
from solarinet_mcp.config import DbConfig
from solarinet_mcp.db import DbClient
from solarinet_mcp.formatting import ResponseFormat
from solarinet_mcp.server import FormatOnlyInput, DbQueryInput, DbHostHistoryInput

NODE_ROWS = [
    ("0x01", "server", "argon.akoria.net", "up", "2026-06-06", "x86_64", "Linux"),
    ("0x02", "monitor", "vantage1.akoria.net", "degraded", "2026-06-06", "arm64", "Linux"),
]


def fake_runner(sql, params, max_rows):
    """Return canned columns/rows based on the query's target table."""
    low = sql.lower()
    if "from node" in low:
        cols = ["nodeId", "role", "hostFqdn", "state", "lastSeenAt", "arch", "osName"]
        return cols, NODE_ROWS
    if "from hosthistory" in low:
        cols = ["sampledAt", "cpuAvgMilli", "ramUsedKb", "swapUsedKb", "diskMinFreePct"]
        return cols, [("2026-06-06T22:00:00Z", 410, 5242880, 0, 41)]
    if "from alertevent" in low:
        cols = ["eventId", "firedAt", "severity", "detail", "nodeId", "targetId", "metric", "op", "threshold"]
        return cols, [(7, "2026-06-06T21:00:00Z", "crit", "loss>500", "0x02", "tcp:h:443", "lossPermille", "gt", 500)]
    return ["one"], [(1,)]


def _db():
    cfg = DbConfig(host="h", port=3306, name="solarinet", user="ro", password="x", timeout=5)
    return DbClient(cfg, runner=fake_runner)


@pytest.fixture(autouse=True)
def reset_clients():
    yield
    server.configure(None, None)


async def test_node_summary_markdown():
    server.configure(db=_db())
    out = await server.solarinet_db_node_summary(FormatOnlyInput())
    assert "argon.akoria.net" in out and "| nodeId |" in out


async def test_db_query_select_ok():
    server.configure(db=_db())
    out = await server.solarinet_db_query(
        DbQueryInput(sql="SELECT * FROM node", response_format=ResponseFormat.JSON)
    )
    parsed = json.loads(out)
    assert parsed["row_count"] == 2 and parsed["columns"][0] == "nodeId"


async def test_db_query_rejects_write():
    server.configure(db=_db())
    out = await server.solarinet_db_query(DbQueryInput(sql="DROP TABLE node"))
    assert out.startswith("Error") and "SELECT" in out


async def test_db_host_history_binds_param():
    server.configure(db=_db())
    out = await server.solarinet_db_host_history(DbHostHistoryInput(node_id="123", limit=10))
    assert "cpuAvgMilli" in out


async def test_db_active_alerts():
    server.configure(db=_db())
    out = await server.solarinet_db_active_alerts(FormatOnlyInput(response_format=ResponseFormat.JSON))
    assert json.loads(out)["rows"][0]["severity"] == "crit"


async def test_db_unconfigured_errors():
    cfg = DbConfig(host="h", port=3306, name="solarinet", user=None, password=None, timeout=5)
    server.configure(db=DbClient(cfg, runner=fake_runner))
    out = await server.solarinet_db_node_summary(FormatOnlyInput())
    assert "not configured" in out.lower()
