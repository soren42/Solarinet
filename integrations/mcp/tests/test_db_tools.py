"""DB tool tests using an injected fake runner (no MariaDB needed)."""
import json

import pytest

from solarinet_mcp import server
from solarinet_mcp.config import DbConfig
from solarinet_mcp.db import HARD_MAX_ROWS, DbClient, DbError, DbQuery
from solarinet_mcp.formatting import ResponseFormat
from solarinet_mcp.server import FormatOnlyInput, DbHostHistoryInput

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


def test_registry_exposes_fixed_db_tools_without_generic_query():
    names = server.list_tools()
    assert "solarinet_db_query" not in names
    assert "solarinet_db_node_summary" in names
    assert "solarinet_db_active_alerts" in names
    assert "solarinet_db_host_history" in names
    assert len(names) == 26


async def test_db_client_rejects_caller_supplied_sql():
    db = _db()
    with pytest.raises(DbError, match="Unsupported database query"):
        await db.query("SELECT * FROM mysql.user")


async def test_db_client_requires_exact_fixed_query_parameters():
    db = _db()
    with pytest.raises(DbError, match="Invalid parameters"):
        await db.query(DbQuery.NODE_SUMMARY, params=["unexpected"])
    with pytest.raises(DbError, match="Invalid node id"):
        await db.query(DbQuery.HOST_HISTORY, params=["123 OR 1=1"])
    with pytest.raises(DbError, match="Invalid node id"):
        await db.query(DbQuery.HOST_HISTORY, params=["18446744073709551616"])


async def test_db_client_binds_values_and_enforces_sql_and_result_caps():
    calls = []

    def capture(sql, params, max_rows):
        calls.append((sql, params, max_rows))
        return ["sampledAt"], [(i,) for i in range(max_rows + 1)]

    cfg = DbConfig(host="h", port=3306, name="solarinet", user="ro", password="x", timeout=5)
    db = DbClient(cfg, runner=capture)

    result = await db.query(DbQuery.HOST_HISTORY, params=["123"], max_rows=10)
    assert calls[0] == (
        "SELECT sampledAt, cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct "
        "FROM hostHistory WHERE nodeId = %s ORDER BY sampledAt DESC LIMIT 11",
        ("123",),
        10,
    )
    assert result["truncated"] is True
    assert result["row_count"] == 10

    await db.query(DbQuery.NODE_SUMMARY, max_rows=HARD_MAX_ROWS + 50)
    assert calls[1][0].endswith(f"LIMIT {HARD_MAX_ROWS + 1}")
    assert calls[1][2] == HARD_MAX_ROWS
