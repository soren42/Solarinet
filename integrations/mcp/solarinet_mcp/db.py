"""
Read-only MariaDB access for the SolariNet schema (section 10).

Security model: this module NEVER permits a write. Two layers enforce it:
  1. The runtime client exposes only a closed set of fixed, reviewed queries;
     callers can pass values but cannot submit SQL or identifiers.
  2. The operator is expected to point SOLARINET_DB_USER at a GRANT SELECT-only
     account, keeping database privileges narrow even if application code changes.

Queries run on a worker thread via asyncio.to_thread because PyMySQL is a
synchronous driver; the tool layer stays fully async.
"""
from __future__ import annotations

import asyncio
from enum import Enum
from typing import Any, Callable, Sequence

from .config import DbConfig

# Default and hard ceilings on returned rows, to protect the agent's context.
DEFAULT_MAX_ROWS = 100
HARD_MAX_ROWS = 1000
MAX_NODE_ID = 18_446_744_073_709_551_615

# A runner executes (sql, params, max_rows) -> (columns, rows). Injectable for tests.
DbRunner = Callable[[str, Sequence[Any], int], "tuple[list[str], list[tuple]]"]


class DbQuery(str, Enum):
    """Supported database reads. MCP callers cannot supply SQL text."""

    NODE_SUMMARY = "node_summary"
    ACTIVE_ALERTS = "active_alerts"
    HOST_HISTORY = "host_history"


_FIXED_QUERIES: dict[DbQuery, tuple[str, int]] = {
    DbQuery.NODE_SUMMARY: (
        "SELECT nodeId, role, hostFqdn, state, lastSeenAt, arch, osName "
        "FROM node ORDER BY role, hostFqdn LIMIT {limit}", 0,
    ),
    DbQuery.ACTIVE_ALERTS: (
        "SELECT e.eventId, e.firedAt, e.severity, e.detail, e.nodeId, e.targetId, "
        "r.metric, r.op, r.threshold FROM alertEvent e "
        "LEFT JOIN alertRule r ON e.ruleId = r.ruleId "
        "WHERE e.clearedAt IS NULL ORDER BY e.firedAt DESC LIMIT {limit}", 0,
    ),
    DbQuery.HOST_HISTORY: (
        "SELECT sampledAt, cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct "
        "FROM hostHistory WHERE nodeId = %s ORDER BY sampledAt DESC LIMIT {limit}", 1,
    ),
}


class DbError(Exception):
    """A database access or validation error."""


def _default_runner(cfg: DbConfig) -> DbRunner:
    """Build a PyMySQL-backed runner. Imported lazily so tests need no driver."""

    def run(sql: str, params: Sequence[Any], max_rows: int) -> tuple[list[str], list[tuple]]:
        import pymysql  # local import: only needed for real DB access

        conn = pymysql.connect(
            host=cfg.host,
            port=cfg.port,
            user=cfg.user,
            password=cfg.password or "",
            database=cfg.name,
            connect_timeout=cfg.timeout,
            read_timeout=cfg.timeout,
            cursorclass=pymysql.cursors.Cursor,
        )
        try:
            with conn.cursor() as cur:
                cur.execute(sql, tuple(params))
                columns = [d[0] for d in cur.description] if cur.description else []
                rows = cur.fetchmany(max_rows + 1)  # +1 to detect truncation
            return columns, list(rows)
        finally:
            conn.close()

    return run


class DbClient:
    """Async read-only interface over a closed set of SolariNet queries."""

    def __init__(self, cfg: DbConfig, runner: DbRunner | None = None) -> None:
        self._cfg = cfg
        self._runner = runner  # injected in tests

    @property
    def configured(self) -> bool:
        return self._cfg.configured

    async def query(
        self,
        query: DbQuery,
        params: Sequence[Any] | None = None,
        max_rows: int = DEFAULT_MAX_ROWS,
    ) -> dict[str, Any]:
        """Execute one fixed query with bound values and a capped row count.

        Returns {columns, rows, row_count, truncated}. Raises DbError.
        """
        if not self.configured:
            raise DbError(
                "DB not configured. Set SOLARINET_DB_USER / SOLARINET_DB_PASS "
                "(use a read-only account)."
            )
        max_rows = max(1, min(int(max_rows), HARD_MAX_ROWS))
        # Keep arbitrary SQL out of this interface entirely. The MCP tools map
        # to fixed, reviewed statements; only values (never identifiers or SQL
        # fragments) may be supplied by a caller.
        if not isinstance(query, DbQuery):
            raise DbError("Unsupported database query.")
        template, expected_params = _FIXED_QUERIES[query]
        values = tuple(params or ())
        if len(values) != expected_params:
            raise DbError("Invalid parameters for database query.")
        if query is DbQuery.HOST_HISTORY:
            node_id = values[0]
            if not isinstance(node_id, str) or not node_id.isascii() or not node_id.isdecimal():
                raise DbError("Invalid node id.")
            if len(node_id) > 20 or int(node_id) > MAX_NODE_ID:
                raise DbError("Invalid node id.")
        safe = template.format(limit=max_rows + 1)
        runner = self._runner or _default_runner(self._cfg)
        try:
            columns, rows = await asyncio.to_thread(runner, safe, values, max_rows)
        except DbError:
            raise
        except Exception as exc:  # driver errors, connection failures, etc.
            raise DbError(f"Query failed: {type(exc).__name__}: {exc}") from exc

        truncated = len(rows) > max_rows
        if truncated:
            rows = rows[:max_rows]
        dict_rows = [dict(zip(columns, r)) for r in rows]
        return {
            "columns": columns,
            "rows": dict_rows,
            "row_count": len(dict_rows),
            "truncated": truncated,
        }
