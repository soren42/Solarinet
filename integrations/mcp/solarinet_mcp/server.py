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


# ---- section 6 (C2 capabilities) read inputs ------------------------------
class DiscoveryListInput(_Base):
    status: str = Field(default="new", description="'new' or 'all'.")
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class EnrollmentListInput(_Base):
    status: str | None = Field(
        default=None,
        description="Optional filter: 'token','pending','approved','rejected','expired'.",
    )
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class BuildsInput(_Base):
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class SegmentsInput(_Base):
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class NetgearInput(_Base):
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class TopologyViewInput(_Base):
    view: str = Field(
        default="monitoring",
        description="'monitoring' (server->monitor->target HRW graph) or "
        "'network' (gateway->switch/AP->host LAN hierarchy).",
    )
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


class ConfigReadInput(_Base):
    response_format: ResponseFormat = Field(default=ResponseFormat.MARKDOWN)


# ---- section 6 control inputs ---------------------------------------------
class DiscoveryAdoptInput(_Base):
    disc_id: str = Field(..., min_length=1, description="Discovered candidate id.")
    role: str | None = Field(
        default=None, description="Role to adopt as: 'client' or 'monitor' (for enrollment path)."
    )
    probe_spec: dict[str, Any] | None = Field(
        default=None, description="Optional probe spec to adopt directly (CTRL_ADOPT_TARGET path)."
    )


class DiscoveryIgnoreInput(_Base):
    disc_id: str = Field(..., min_length=1, description="Discovered candidate id to ignore.")


class EnrollmentApproveInput(_Base):
    enr_id: str = Field(..., min_length=1, description="Enrollment id to approve (signs the CSR).")
    confirm: bool = Field(
        default=False,
        description="Must be true to proceed. Approving signs a CSR and enrolls a node.",
    )


class EnrollmentRejectInput(_Base):
    enr_id: str = Field(..., min_length=1, description="Enrollment id to reject.")


class ProvisionInput(_Base):
    node_id: str | None = Field(default=None, description="Target node id (or use enr_id).")
    enr_id: str | None = Field(default=None, description="Enrollment id (or use node_id).")
    config_blob: dict[str, Any] = Field(..., description="Config overlay to converge to.")
    build_id: str | None = Field(default=None, description="Optional buildArtifact id to deploy.")


class DecommissionInput(_Base):
    node_id: str = Field(..., min_length=1, description="Node id to tear down and retire.")
    wipe_scope: list[str] = Field(
        default_factory=list,
        description="What to wipe, e.g. ['config','service','certs']. Empty leaves logs for forensics.",
    )
    confirm: bool = Field(
        default=False,
        description="Must be true. DESTRUCTIVE: wipes the node and marks it retired.",
    )


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


# ---- section 6 (C2 capabilities) read tools -------------------------------
@mcp.tool(name="solarinet_list_discovery", annotations={"title": "Discovery candidates", **_RO})
async def solarinet_list_discovery(params: DiscoveryListInput) -> str:
    """Discovered-but-not-monitored candidates (GET /api/discovery?status=new|all).

    Each item: {discId, host, ip, kind, via, services[], segId, arch, seenCount,
    lastSeenAt, status}. Use solarinet_adopt_discovery / solarinet_ignore_discovery
    to act on one.

    Returns: markdown table or JSON list of discovery candidates.
    """
    try:
        data = await _rest().request("GET", "/api/discovery", params={"status": params.status})
        return _render(data, params.response_format, f"Discovery ({params.status})")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_list_enrollments", annotations={"title": "Enrollments", **_RO})
async def solarinet_list_enrollments(params: EnrollmentListInput) -> str:
    """Pending/decided enrollments (GET /api/enrollments?status=).

    Each item: {enrId, host, ip, role, certFp, status, requestedAt}. Approving an
    enrollment signs the CSR (operator-only) - see solarinet_approve_enrollment.

    Returns: markdown table or JSON list of enrollments.
    """
    try:
        data = await _rest().request("GET", "/api/enrollments", params={"status": params.status})
        return _render(data, params.response_format, "Enrollments")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_list_builds", annotations={"title": "Build registry", **_RO})
async def solarinet_list_builds(params: BuildsInput) -> str:
    """Build registry + per-arch convergence (GET /api/builds): which versions
    exist, how many nodes on each, update-available flags.

    Returns: markdown table or JSON list of build artifacts.
    """
    try:
        data = await _rest().request("GET", "/api/builds")
        return _render(data, params.response_format, "Builds")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_list_segments", annotations={"title": "Network segments", **_RO})
async def solarinet_list_segments(params: SegmentsInput) -> str:
    """Network segments with rollup counts (GET /api/segments):
    {segId, label, cidr, wireless, roll:{up,degraded,down,unknown}}.

    Returns: markdown table or JSON list of segments.
    """
    try:
        data = await _rest().request("GET", "/api/segments")
        return _render(data, params.response_format, "Segments")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_list_netgear", annotations={"title": "Network gear", **_RO})
async def solarinet_list_netgear(params: NetgearInput) -> str:
    """Network gear inventory with attached-node counts and uplink chain
    (GET /api/netgear) - switches/APs/gateways for the LAN-hierarchy view.

    Returns: markdown table or JSON list of network gear.
    """
    try:
        data = await _rest().request("GET", "/api/netgear")
        return _render(data, params.response_format, "Network gear")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_get_topology_view", annotations={"title": "Topology (dual view)", **_RO})
async def solarinet_get_topology_view(params: TopologyViewInput) -> str:
    """Topology graph in the chosen projection
    (GET /api/topology?view=monitoring|network).

    view='monitoring' -> server->monitor->target/client edges (the HRW graph).
    view='network'    -> gateway->switch/AP->host hierarchy with uplink ports,
                         link type, speed, and LLDP flag.

    Returns: JSON or markdown of the topology graph.
    """
    try:
        data = await _rest().request("GET", "/api/topology", params={"view": params.view})
        return _render(data, params.response_format, f"Topology ({params.view})")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_get_config", annotations={"title": "Read global config", **_RO})
async def solarinet_get_config(params: ConfigReadInput) -> str:
    """Read global server config (GET /api/config): tolerances, retention, lease,
    ports, autoDiscover/autoEnroll flags. Read-only here - editing config is a
    deliberately separate, operator-gated dashboard action.

    Returns: markdown record or JSON object.
    """
    try:
        data = await _rest().request("GET", "/api/config")
        return _render(data, params.response_format, "Config")
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


# ===========================================================================
# Control tools (side-effectful)
# ===========================================================================
_CTRL = {"readOnlyHint": False, "destructiveHint": False, "idempotentHint": False, "openWorldHint": True}
# Destructive control: marks node retired / signs CSRs. These mirror the
# dashboard's operator-RBAC + explicit-confirm discipline (section 6, section 9).
_DESTRUCTIVE = {"readOnlyHint": False, "destructiveHint": True, "idempotentHint": False, "openWorldHint": True}


def _require_operator_token() -> str | None:
    """Return an error string if no operator token is configured, else None.

    Destructive endpoints (decommission, enrollment approve) demand the operator
    role server-side; failing fast here gives the agent an actionable message
    instead of a round-trip 403, and mirrors the dashboard's gate.
    """
    if not _rest()._cfg.token:  # noqa: SLF001 - intentional config read
        return (
            "Error [no_operator_token]: this destructive action requires the "
            "operator token. Set SOLARINET_API_TOKEN."
        )
    return None


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


# ---- section 6 control tools ----------------------------------------------
@mcp.tool(name="solarinet_adopt_discovery", annotations={"title": "Adopt discovered", **_CTRL})
async def solarinet_adopt_discovery(params: DiscoveryAdoptInput) -> str:
    """Promote a discovered candidate to monitored
    (POST /api/discovery/{discId}/adopt).

    SIDE EFFECT: creates an enrollment or sends CTRL_ADOPT_TARGET so a monitor
    adds the entity to its live probe schedule. Not destructive. Requires the
    operator role server-side.

    Args: disc_id, optional role (for the enrollment path), optional probe_spec
    (for the direct CTRL_ADOPT_TARGET path).
    Returns: confirmation text or an error.
    """
    body: dict[str, Any] = {}
    if params.role is not None:
        body["role"] = params.role
    if params.probe_spec is not None:
        body["probeSpec"] = params.probe_spec
    try:
        data = await _rest().request(
            "POST", f"/api/discovery/{params.disc_id}/adopt", json_body=body
        )
        return f"Adopted discovered candidate {params.disc_id}.\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_ignore_discovery", annotations={"title": "Ignore discovered", **_CTRL})
async def solarinet_ignore_discovery(params: DiscoveryIgnoreInput) -> str:
    """Mark a discovered candidate ignored, suppressing it from the list
    (POST /api/discovery/{discId}/ignore).

    SIDE EFFECT: sets discovered.status='ignored'. Reversible (it can resurface
    on the next sighting). Not destructive.

    Args: disc_id.
    Returns: confirmation text or an error.
    """
    try:
        data = await _rest().request("POST", f"/api/discovery/{params.disc_id}/ignore")
        return f"Ignored discovered candidate {params.disc_id}.\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_reject_enrollment", annotations={"title": "Reject enrollment", **_CTRL})
async def solarinet_reject_enrollment(params: EnrollmentRejectInput) -> str:
    """Deny a pending enrollment (POST /api/enrollments/{enrId}/reject).

    SIDE EFFECT: sets enrollment.status='rejected'. Not destructive (no node is
    torn down; the CSR is simply not signed). Requires the operator role.

    Args: enr_id.
    Returns: confirmation text or an error.
    """
    try:
        data = await _rest().request("POST", f"/api/enrollments/{params.enr_id}/reject")
        return f"Rejected enrollment {params.enr_id}.\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(name="solarinet_provision", annotations={"title": "Provision node", **_CTRL})
async def solarinet_provision(params: ProvisionInput) -> str:
    """First-time bring-up / re-converge a node (POST /api/control/provision ->
    CTRL_PROVISION).

    SIDE EFFECT: writes config, installs/enables the service unit, begins
    reporting. Idempotent - a re-provision converges to the supplied epoch.
    Requires the operator role.

    Args: node_id OR enr_id (one required), config_blob, optional build_id.
    Returns: confirmation text or an error.
    """
    if not params.node_id and not params.enr_id:
        return "Error [bad_request]: provide either node_id or enr_id."
    body: dict[str, Any] = {"configBlob": params.config_blob}
    if params.node_id:
        body["nodeId"] = params.node_id
    if params.enr_id:
        body["enrId"] = params.enr_id
    if params.build_id is not None:
        body["buildId"] = params.build_id
    try:
        data = await _rest().request("POST", "/api/control/provision", json_body=body)
        target = params.node_id or f"enr {params.enr_id}"
        return f"Provision requested for {target}.\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


# --- destructive: operator token + explicit confirm required ---------------
@mcp.tool(
    name="solarinet_approve_enrollment",
    annotations={"title": "Approve enrollment (operator)", **_DESTRUCTIVE},
)
async def solarinet_approve_enrollment(params: EnrollmentApproveInput) -> str:
    """Operator-sign a CSR so a node becomes enrolled
    (POST /api/enrollments/{enrId}/approve).

    GATED: signing admits a new node to the trust domain, so this mirrors the
    dashboard's operator discipline - it REQUIRES the operator token AND an
    explicit confirm=true. Without both it refuses before any network call.

    Args: enr_id, confirm (must be true).
    Returns: confirmation text or an error.
    """
    gate = _require_operator_token()
    if gate:
        return gate
    if not params.confirm:
        return (
            "Error [confirm_required]: approving an enrollment signs a CSR and "
            "admits a node. Re-call with confirm=true to proceed."
        )
    try:
        data = await _rest().request("POST", f"/api/enrollments/{params.enr_id}/approve")
        return f"Approved enrollment {params.enr_id} (CSR signed).\n\n{to_json(data)}"
    except Exception as exc:  # noqa: BLE001
        return _err(exc)


@mcp.tool(
    name="solarinet_decommission",
    annotations={"title": "Decommission node (operator)", **_DESTRUCTIVE},
)
async def solarinet_decommission(params: DecommissionInput) -> str:
    """Tear down and retire a node (POST /api/control/decommission ->
    SCP_MSG_DECOMMISSION / CTRL_DECOMMISSION with a fresh confirm token).

    DESTRUCTIVE: wipes the requested scope on the target and marks it 'retired';
    the server writes an audit row. This mirrors the dashboard's double-confirm:
    it REQUIRES the operator token AND an explicit confirm=true, refusing before
    any network call otherwise.

    Args: node_id, wipe_scope (e.g. ['config','service','certs']; empty leaves
    logs for forensics), confirm (must be true).
    Returns: confirmation text or an error.
    """
    gate = _require_operator_token()
    if gate:
        return gate
    if not params.confirm:
        return (
            "Error [confirm_required]: decommission is destructive and irreversible. "
            "Re-call with confirm=true to proceed."
        )
    try:
        data = await _rest().request(
            "POST", "/api/control/decommission",
            json_body={"nodeId": params.node_id, "wipeScope": params.wipe_scope, "confirm": True},
        )
        return (
            f"Decommission requested for node {params.node_id} "
            f"(scope={params.wipe_scope or 'monitoring-only'}).\n\n{to_json(data)}"
        )
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
        # section 6 reads
        "solarinet_list_discovery", "solarinet_list_enrollments", "solarinet_list_builds",
        "solarinet_list_segments", "solarinet_list_netgear", "solarinet_get_topology_view",
        "solarinet_get_config",
        # control
        "solarinet_trigger_survey", "solarinet_push_config",
        "solarinet_adopt_discovery", "solarinet_ignore_discovery",
        "solarinet_reject_enrollment", "solarinet_provision",
        # destructive control (operator + confirm)
        "solarinet_approve_enrollment", "solarinet_decommission",
        # DB
        "solarinet_db_query", "solarinet_db_node_summary",
        "solarinet_db_active_alerts", "solarinet_db_host_history",
    ]


def run(cfg: Config | None = None) -> None:
    cfg = cfg or load_config()
    if cfg.transport == "streamable_http":
        mcp.run(transport="streamable_http")
    else:
        mcp.run()
