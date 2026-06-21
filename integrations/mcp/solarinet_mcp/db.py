"""
Read-only MariaDB access for the SolariNet schema (section 10).

Security model: this module NEVER permits a write. Two layers enforce it:
  1. A strict statement guard (validate_read_only) that strips comments, rejects
     anything but a single SELECT/WITH statement, blocks stacked statements and
     INTO OUTFILE/DUMPFILE exfiltration, and caps result rows.
  2. The operator is expected to point SOLARINET_DB_USER at a GRANT SELECT-only
     account, so even a guard bypass cannot mutate data.

Queries run on a worker thread via asyncio.to_thread because PyMySQL is a
synchronous driver; the tool layer stays fully async.
"""
from __future__ import annotations

import asyncio
import re
from typing import Any, Callable, Sequence

from .config import DbConfig

# Default and hard ceilings on returned rows, to protect the agent's context.
DEFAULT_MAX_ROWS = 100
HARD_MAX_ROWS = 1000

# A runner executes (sql, params, max_rows) -> (columns, rows). Injectable for tests.
DbRunner = Callable[[str, Sequence[Any], int], "tuple[list[str], list[tuple]]"]

_COMMENT_BLOCK = re.compile(r"/\*.*?\*/", re.DOTALL)
_COMMENT_LINE = re.compile(r"--[^\n]*")
_COMMENT_HASH = re.compile(r"#[^\n]*")
_FORBIDDEN_SUBSTR = ("into outfile", "into dumpfile")

# Table allow-list (read-only SELECT discipline). Only these SolariNet schema
# tables may be referenced after FROM/JOIN. This is defence-in-depth on top of
# the GRANT SELECT-only DB account: it keeps the agent inside the published
# section 10 schema (and section 5 C2-capability additions) and prevents reads
# of information_schema / mysql / performance_schema and other system catalogs.
#
# Keep this in step with db/migrations (section 5). New section-5 tables added
# here: segment, networkGear, lldpEdge, discovered, enrollment, buildArtifact.
ALLOWED_TABLES: frozenset[str] = frozenset(
    t.lower()
    for t in (
        # --- base section 10 schema ---
        "node",
        "hostCurrent",
        "hostHistory",
        "procCurrent",
        "probeTarget",
        "probeCurrent",
        "probeHistory",
        "alertRule",
        "alertEvent",
        "nodeConfig",
        "serverLease",
        # --- section 5 C2-capability additions (002_c2_capabilities.sql) ---
        "segment",
        "networkGear",
        "lldpEdge",
        "discovered",
        "enrollment",
        "buildArtifact",
    )
)

# Identifiers that introduce a table reference. We only enforce the allow-list
# on names that follow these keywords; CTE names (WITH x AS ...) are collected
# separately and exempted so legitimate WITH...SELECT still works.
_TABLE_REF = re.compile(r"\b(?:from|join)\s+([`\"]?)([A-Za-z_][A-Za-z0-9_]*)\1", re.IGNORECASE)
_CTE_NAME = re.compile(r"\b(?:with|,)\s+([`\"]?)([A-Za-z_][A-Za-z0-9_]*)\1\s+as\s*\(", re.IGNORECASE)


class DbError(Exception):
    """A database access or validation error."""


def validate_read_only(sql: str) -> str:
    """Return the trimmed SQL if it is a single read-only statement, else raise.

    Rules:
      - comments are stripped before analysis
      - must begin with SELECT or WITH (case-insensitive)
      - no stacked statements (a ';' may only appear as an optional trailer)
      - no INTO OUTFILE / INTO DUMPFILE
    """
    if not sql or not sql.strip():
        raise DbError("Empty SQL.")

    # Strip comments so they can't hide a second statement or keyword.
    stripped = _COMMENT_BLOCK.sub(" ", sql)
    stripped = _COMMENT_LINE.sub(" ", stripped)
    stripped = _COMMENT_HASH.sub(" ", stripped)
    stripped = stripped.strip()

    # Allow exactly one optional trailing semicolon.
    if stripped.endswith(";"):
        stripped = stripped[:-1].rstrip()
    if ";" in stripped:
        raise DbError("Only a single statement is allowed (no ';').")

    lowered = stripped.lower()
    if not (lowered.startswith("select") or lowered.startswith("with")):
        raise DbError("Only SELECT (or WITH ... SELECT) queries are permitted.")

    for bad in _FORBIDDEN_SUBSTR:
        if bad in lowered:
            raise DbError(f"Disallowed clause: {bad.upper()}.")

    _enforce_table_allow_list(stripped)

    return stripped


def _enforce_table_allow_list(sql: str) -> None:
    """Reject any FROM/JOIN target that is not an allow-listed SolariNet table.

    CTE names declared in a WITH clause are exempt so WITH...SELECT works. The
    comparison is case-insensitive and backtick/quote tolerant.
    """
    cte_names = {m.group(2).lower() for m in _CTE_NAME.finditer(sql)}
    for m in _TABLE_REF.finditer(sql):
        name = m.group(2)
        low = name.lower()
        if low in cte_names:
            continue
        if low not in ALLOWED_TABLES:
            raise DbError(
                f"Table not in allow-list: {name}. Permitted tables: "
                + ", ".join(sorted(ALLOWED_TABLES))
            )


def _enforce_limit(sql: str, max_rows: int) -> str:
    """Append a LIMIT if the query has none, so unbounded scans can't run away."""
    if re.search(r"\blimit\b", sql, re.IGNORECASE):
        return sql
    return f"{sql} LIMIT {max_rows}"


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
    """Async read-only query interface over the SolariNet MariaDB."""

    def __init__(self, cfg: DbConfig, runner: DbRunner | None = None) -> None:
        self._cfg = cfg
        self._runner = runner  # injected in tests

    @property
    def configured(self) -> bool:
        return self._cfg.configured

    async def query(
        self,
        sql: str,
        params: Sequence[Any] | None = None,
        max_rows: int = DEFAULT_MAX_ROWS,
    ) -> dict[str, Any]:
        """Validate, bound, and execute a read-only query.

        Returns {columns, rows, row_count, truncated}. Raises DbError.
        """
        if not self.configured:
            raise DbError(
                "DB not configured. Set SOLARINET_DB_USER / SOLARINET_DB_PASS "
                "(use a read-only account)."
            )
        max_rows = max(1, min(int(max_rows), HARD_MAX_ROWS))
        safe = validate_read_only(sql)
        safe = _enforce_limit(safe, max_rows)
        runner = self._runner or _default_runner(self._cfg)
        try:
            columns, rows = await asyncio.to_thread(runner, safe, tuple(params or ()), max_rows)
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
