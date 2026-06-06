#!/usr/bin/env python3
"""
SolariNet MCP server.

Exposes the SolariNet command-and-control surface to an LLM agent through three
groups of tools:

  * REST read  - the section 11.2 dashboard API (fleet roster, node detail,
                 probe reachability matrix, alerts, topology, failover status).
  * Control    - side-effectful POSTs (trigger a survey, stage a config push)
                 that round-trip through the dashboard to solariCtl.
  * DB direct  - read-only SQL against the section 10 MariaDB schema.

Configuration is entirely environment-driven (see config.py). Transport is
stdio by default (a local operator/agent tool); set SOLARINET_MCP_TRANSPORT=
streamable_http to serve remotely.
"""
from __future__ import annotations

from typing import Any, Mapping

from pydantic import BaseModel, ConfigDict, Field
from mcp.server.fastmcp import FastMCP

from .config import Config, load_config
from .db import DEFAULT_MAX_ROWS, HARD_MAX_ROWS, DbClient, DbError
from .formatting import ResponseFormat, md_kv, md_table, to_json
from .rest import RestClient, SolariApiError

mcp = FastMCP("solarinet_mcp")

# ---- client wiring (overridable in tests) --------------------------------
_REST: RestClient | None = None
_DB: DbClient | None = None


def configure(rest: RestClient | None = None, db: DbClient | None = None) -> None:
    """Inject pre-built clients (used by tests). If unset, clients are built
    lazily from the environment on first use."""
    global _REST, _DB
    _REST = rest
    _DB = db


def _rest() -> RestClient:
    global _REST
    if _REST is None:
        _REST = RestClient(load_config().rest)
    return _REST


def _db() -> DbClient:
    global _DB
    if _DB is None:
        _DB = DbClient(load_config().db)
    return _DB


# ---- generic rendering ----------------------------------------------------
def _render(data: Any, fmt: ResponseFormat, title: str) -> str:
    """Render arbitrary JSON-ish data as JSON or compact markdown."""
    if fmt == ResponseFormat.JSON:
        return to_json(data)
    if isinstance(data, list):
        if data and isinstance(data[0], Mapping):
            cols: list[str] = []
            for row in data:
                for k in row.keys():
                    if k not in cols:
                        cols.append(k)
            return f"# {title} ({len(data)})\n\n" + md_table(data, cols)
        return f"# {title} ({len(data)})\n\n" + to_json(data)
    if isinstance(data, Mapping):
        return md_kv(title, data)
    return f"# {title}\n\n{data}"


def _err(exc: Exception) -> str:
    if isinstance(exc, SolariApiError):
        code = f" [{exc.code}]" if exc.code else ""
        return f"Error{code}: {exc.message}"
    if isinstance(exc, DbError):
        return f"Error: {exc}"
    return f"Error: unexpected {type(exc).__name__}: {exc}"


# ===========================================================================
# Input models
# ===========================================================================
class _Base(BaseModel):
    model_config = ConfigDict(str_strip_whitespace=True, extra="forbid")


class ListNodesInput(_Base):
    limit: int = Field(default=50, ge=1, le=500, description="Max nodes to return.")
    offset: int = Field(default=0, ge=0, description="Pagination offset.")
    role: str | None = Field(
        default=None, description="Optional role filter: 'client', 'monitor', or 'server'."
    )
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class NodeIdInput(_Base):
    node_id: str = Field(..., min_length=1, description="Node id (decimal or 0x-hex).")
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class NodeHistoryInput(_Base):
    node_id: str = Field(..., min_length=1, description="Node id.")
    metric: str = Field(..., min_length=1, description="Metric name, e.g. 'cpuAvgMilli', 'ramUsedKb'.")
    from_ts: str | None = Field(default=None, description="ISO-8601 start (inclusive).")
    to_ts: str | None = Field(default=None, description="ISO-8601 end (inclusive).")
    response_format: ResponseFormat = Field(default=ResponseFormat.JSON)


class ProbesInput(_Base):
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class ProbeHistoryInput(_Base):
    target_id: str = Field(..., min_length=1, description="Probe target id, e.g. 'tcp:hydrogen:443'.")
    from_ts: str | None = Field(default=None, description="ISO-8601 start.")
    to_ts: str | None = Field(default=None, description="ISO-8601 end.")
    response_format: ResponseFormat = Field(default=ResponseFormat.JSON)


class AlertsInput(_Base):
    status: str = Field(default="active", description="'active' or 'all'.")
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class FormatOnlyInput(_Base):
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class SurveyInput(_Base):
    scope: str = Field(
        default="all",
        description="'all' to survey the whole fleet, or a specific node id.",
    )


class PushConfigInput(_Base):
    node_id: str = Field(..., min_length=1, description="Target node id.")
    config_blob: dict[str, Any] = Field(..., description="Config overlay to stage (JSON object).")


class DbQueryInput(_Base):
    sql: str = Field(..., min_length=1, description="A single read-only SELECT/WITH query.")
    max_rows: int = Field(default=DEFAULT_MAX_ROWS, ge=1, le=HARD_MAX_ROWS, description="Row cap.")
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class DbHostHistoryInput(_Base):
    node_id: str = Field(..., min_length=1, description="Node id (decimal).")
    limit: int = Field(default=50, ge=1, le=HARD_MAX_ROWS, description="Max history rows.")
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


# ===========================================================================
# REST read tools
# ===========================================================================
_RO = {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True, "openWorldHint": True}


@mcp.tool(name="solarinet_list_nodes", annotations={"title": "List nodes", **_RO})
async def solarinet_list_nodes(params: ListNodesInput) -> str:
    """List the SolariNet fleet roster with rollup state (GET /api/nodes).

    Returns each node's {nodeId, role, hostFqdn, state, lastSeenAt, arch, osName}.
    Use this to answer "what nodes exist / which are down". For one node's full
    metrics use solarinet_get_node instead.

    Returns: markdown table or JSON list of node summary objects.
    """
    try:
        data = await _rest().request(
            "GET", "/api/nodes",
            params={"limit": params.limit, "offset": params.offset, "role": params.role},
        )
        return _render(data, params.response_format, "Nodes")
    except Exception as exc:  # noqa: BLE001 - surfaced as tool error text
        return _err(exc)


@mcp.tool(name="solarinet_get_node", annotations={"title": "Get node detail", **_RO})
async def solarinet_get_node(params: NodeIdInput) -> str:
    """Full current host metrics + watched procs + recent alerts for one node
    (GET /api/nodes/{id}).

    Returns: markdown record or JSON object for the node.
    """
    try:
        data = await _rest().request("GET", f"/api/nodes/{params.node_id}")
        return _render(data, params.response_format, f"Node {params.node_id}")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_get_node_history", annotations={"title": "Node metric history", **_RO})
async def solarinet_get_node_history(params: NodeHistoryInput) -> str:
    """Time series for one metric on one node
    (GET /api/nodes/{id}/history?metric=&from=&to=).

    Returns: JSON (default) or markdown series of {sampledAt, value} points.
    """
    try:
        data = await _rest().request(
            "GET", f"/api/nodes/{params.node_id}/history",
            params={"metric": params.metric, "from": params.from_ts, "to": params.to_ts},
        )
        return _render(data, params.response_format, f"{params.metric} history")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_list_probes", annotations={"title": "Probe reachability", **_RO})
async def solarinet_list_probes(params: ProbesInput) -> str:
    """Probe targets with per-vantage current state (GET /api/probes) - the
    multi-monitor reachability matrix.

    Returns: markdown table or JSON list of probe-target states.
    """
    try:
        data = await _rest().request("GET", "/api/probes")
        return _render(data, params.response_format, "Probes")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_get_probe_history", annotations={"title": "Probe history", **_RO})
async def solarinet_get_probe_history(params: ProbeHistoryInput) -> str:
    """RTT/loss time series per vantage for one probe target
    (GET /api/probes/{targetId}/history).

    Returns: JSON (default) or markdown time series.
    """
    try:
        data = await _rest().request(
            "GET", f"/api/probes/{params.target_id}/history",
            params={"from": params.from_ts, "to": params.to_ts},
        )
        return _render(data, params.response_format, f"Probe {params.target_id} history")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_list_alerts", annotations={"title": "List alerts", **_RO})
async def solarinet_list_alerts(params: AlertsInput) -> str:
    """Alert events with rule + node context (GET /api/alerts?status=active|all).

    Returns: markdown table or JSON list of alert events.
    """
    try:
        data = await _rest().request("GET", "/api/alerts", params={"status": params.status})
        return _render(data, params.response_format, f"Alerts ({params.status})")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_get_topology", annotations={"title": "Topology map", **_RO})
async def solarinet_get_topology(params: FormatOnlyInput) -> str:
    """Nodes plus monitor->target assignment edges (HRW result) for the live map
    (GET /api/topology).

    Returns: JSON or markdown of the topology graph.
    """
    try:
        data = await _rest().request("GET", "/api/topology")
        return _render(data, params.response_format, "Topology")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_server_status", annotations={"title": "Failover status", **_RO})
async def solarinet_server_status(params: FormatOnlyInput) -> str:
    """Current failover state (GET /api/server/status):
    {primary, failover, leaseEpoch, expiresAt}.

    Returns: markdown record or JSON object.
    """
    try:
        data = await _rest().request("GET", "/api/server/status")
        return _render(data, params.response_format, "Server status")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


# ===========================================================================
# Control tools (side-effectful)
# ===========================================================================
_CTRL = {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": False, "openWorldHint": True}


@mcp.tool(name="solarinet_trigger_survey", annotations={"title": "Trigger survey", **_CTRL})
async def solarinet_trigger_survey(params: SurveyInput) -> str:
    """Demand an immediate report round from the fleet (POST /api/control/survey).

    SIDE EFFECT: this causes real network activity - monitors/clients produce an
    out-of-schedule report round. Requires the operator role (auth token).

    Args: scope = "all" or a specific node id.
    Returns: confirmation text or an error.
    """
    try:
        data = await _rest().request(
            "POST", "/api/control/survey", json_body={"scope": params.scope}
        )
        return f"Survey requested for scope='{params.scope}'.\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_push_config", annotations={"title": "Stage config push", **_CTRL})
async def solarinet_push_config(params: PushConfigInput) -> str:
    """Stage a configuration push to a node (POST /api/control/config).

    SIDE EFFECT: stages a new config epoch the target node will converge to.
    Requires the operator role. Verify the node id and blob before calling.

    Args: node_id, config_blob (a JSON object overlay).
    Returns: confirmation text (incl. accepted epoch if returned) or an error.
    """
    try:
        data = await _rest().request(
            "POST", "/api/control/config",
            json_body={"nodeId": params.node_id, "configBlob": params.config_blob},
        )
        return f"Config push staged for node {params.node_id}.\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


# ===========================================================================
# DB direct (read-only) tools
# ===========================================================================
_DBRO = {"readOnlyHint": True, "destructiveHint": False, "idempotentHint": True, "openWorldHint": True}


def _render_db(result: dict[str, Any], fmt: ResponseFormat, title: str) -> str:
    if fmt == ResponseFormat.JSON:
        return to_json(result)
    note = "  _(truncated)_" if result.get("truncated") else ""
    table = md_table(result["rows"], result["columns"]) if result["columns"] else "(no columns)"
    return f"# {title} - {result['row_count']} row(s){note}\n\n{table}"


@mcp.tool(name="solarinet_db_query", annotations={"title": "Read-only SQL", **_DBRO})
async def solarinet_db_query(params: DbQueryInput) -> str:
    """Run a single read-only SQL query against the SolariNet MariaDB (section 10).

    ONLY SELECT / WITH...SELECT is permitted; the server rejects DML/DDL, stacked
    statements, and INTO OUTFILE/DUMPFILE, and caps rows. Key tables: node,
    hostCurrent, hostHistory, procCurrent, probeTarget, probeCurrent,
    probeHistory, alertRule, alertEvent, nodeConfig, serverLease.

    Args: sql (one SELECT), max_rows (cap), response_format.
    Returns: markdown table or JSON {columns, rows, row_count, truncated}.
    """
    try:
        result = await _db().query(params.sql, max_rows=params.max_rows)
        return _render_db(result, params.response_format, "Query result")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_db_node_summary", annotations={"title": "Node summary (DB)", **_DBRO})
async def solarinet_db_node_summary(params: FormatOnlyInput) -> str:
    """Fleet roster straight from the DB: counts and per-node state from `node`.

    Returns: markdown table or JSON of {nodeId, role, hostFqdn, state, lastSeenAt}.
    """
    sql = (
        "SELECT nodeId, role, hostFqdn, state, lastSeenAt, arch, osName "
        "FROM node ORDER BY role, hostFqdn"
    )
    try:
        result = await _db().query(sql, max_rows=HARD_MAX_ROWS)
        return _render_db(result, params.response_format, "Node summary")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_db_active_alerts", annotations={"title": "Active alerts (DB)", **_DBRO})
async def solarinet_db_active_alerts(params: FormatOnlyInput) -> str:
    """Currently-firing alerts (clearedAt IS NULL) joined to their rule/node.

    Returns: markdown table or JSON of active alert events, newest first.
    """
    sql = (
        "SELECT e.eventId, e.firedAt, e.severity, e.detail, e.nodeId, e.targetId, "
        "r.metric, r.op, r.threshold "
        "FROM alertEvent e LEFT JOIN alertRule r ON e.ruleId = r.ruleId "
        "WHERE e.clearedAt IS NULL ORDER BY e.firedAt DESC"
    )
    try:
        result = await _db().query(sql, max_rows=HARD_MAX_ROWS)
        return _render_db(result, params.response_format, "Active alerts")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_db_host_history", annotations={"title": "Host history (DB)", **_DBRO})
async def solarinet_db_host_history(params: DbHostHistoryInput) -> str:
    """Recent hostHistory rows for one node (CPU/RAM/swap/disk over time).

    Args: node_id (decimal), limit.
    Returns: markdown table or JSON, newest sample first.
    """
    sql = (
        "SELECT sampledAt, cpuAvgMilli, ramUsedKb, swapUsedKb, diskMinFreePct "
        "FROM hostHistory WHERE nodeId = %s ORDER BY sampledAt DESC"
    )
    try:
        # node_id is bound as a parameter; accept decimal ids here.
        result = await _db().query(sql, params=[params.node_id], max_rows=params.limit)
        return _render_db(result, params.response_format, f"Host history (node {params.node_id})")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


# ===========================================================================
# Entry point
# ===========================================================================
def list_tools() -> list[str]:
    """Return registered tool names (used by --check; avoids starting stdio)."""
    return [
        "solarinet_list_nodes", "solarinet_get_node", "solarinet_get_node_history",
        "solarinet_list_probes", "solarinet_get_probe_history", "solarinet_list_alerts",
        "solarinet_get_topology", "solarinet_server_status",
        "solarinet_trigger_survey", "solarinet_push_config",
        "solarinet_db_query", "solarinet_db_node_summary",
        "solarinet_db_active_alerts", "solarinet_db_host_history",
    ]


def run(cfg: Config | None = None) -> None:
    cfg = cfg or load_config()
    if cfg.transport == "streamable_http":
        mcp.run(transport="streamable_http")
    else:
        mcp.run()
