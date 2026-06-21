"""Tests for the section-6 (C2 capabilities) MCP tools.

All exercised against an injected httpx.MockTransport - no live API/DB. Covers
the new discovery/enrollment/builds/segments/netgear/topology(view)/config reads
and the guarded control tools (adopt/ignore/reject/provision and the destructive
approve/decommission that require the operator token + explicit confirm).
"""
import json

import httpx
import pytest

from solarinet_mcp import server
from solarinet_mcp.config import RestConfig
from solarinet_mcp.rest import RestClient
from solarinet_mcp.formatting import ResponseFormat
from solarinet_mcp.server import (
    DiscoveryListInput, EnrollmentListInput, BuildsInput, SegmentsInput,
    NetgearInput, TopologyViewInput, ConfigReadInput,
    DiscoveryAdoptInput, DiscoveryIgnoreInput, EnrollmentRejectInput,
    EnrollmentApproveInput, ProvisionInput, DecommissionInput,
)

DISCOVERY = [
    {"discId": 11, "host": "printer.lab", "ip": "10.42.20.9", "kind": "host", "via": "mdns",
     "services": ["ipp:631"], "segId": "lab", "arch": None, "seenCount": 3,
     "lastSeenAt": "2026-06-20T10:00:00Z", "status": "new"},
]
ENROLLMENTS = [
    {"enrId": 5, "host": "newmon.akoria.net", "ip": "10.42.10.5", "role": "monitor",
     "certFp": "AA:BB", "status": "pending", "requestedAt": "2026-06-20T09:00:00Z"},
]
BUILDS = [
    {"buildId": 1, "arch": "x86_64", "os": "linux", "version": "1.4.2", "channel": "stable",
     "nodesOnVersion": 7, "updateAvailable": False},
]
SEGMENTS = [
    {"segId": "lab", "label": "Lab", "cidr": "10.42.20.0/24", "wireless": False,
     "roll": {"up": 4, "degraded": 1, "down": 0, "unknown": 0}},
]
NETGEAR = [
    {"gearId": "sw-core-01", "name": "Core", "kind": "switch", "segId": "core",
     "uplinkGearId": "gw", "attachedNodes": 12},
]
TOPO_MON = {"view": "monitoring", "nodes": [{"nodeId": "0x01"}], "edges": [{"from": "0x01", "to": "0x02"}]}
TOPO_NET = {"view": "network", "gear": NETGEAR, "edges": []}
CONFIG = {"toleranceCpuMilli": 800, "retentionDays": 90, "leaseSeconds": 15,
          "autoDiscover": True, "autoEnroll": False}


def _env(data, ok=True, status=200):
    if ok:
        return httpx.Response(status, json={"ok": True, "data": data, "ts": "2026-06-20T10:00:00Z"})
    return httpx.Response(status, json={"ok": False, "error": {"code": "err", "message": data}})


def _handler(request: httpx.Request) -> httpx.Response:
    path = request.url.path
    method = request.method
    has_auth = "authorization" in {k.lower() for k in request.headers.keys()}
    # --- reads ---
    if path == "/api/discovery" and method == "GET":
        return _env(DISCOVERY)
    if path == "/api/enrollments" and method == "GET":
        return _env(ENROLLMENTS)
    if path == "/api/builds":
        return _env(BUILDS)
    if path == "/api/segments":
        return _env(SEGMENTS)
    if path == "/api/netgear":
        return _env(NETGEAR)
    if path == "/api/topology" and method == "GET":
        view = request.url.params.get("view", "monitoring")
        return _env(TOPO_NET if view == "network" else TOPO_MON)
    if path == "/api/config" and method == "GET":
        return _env(CONFIG)
    # --- control ---
    if path == "/api/discovery/11/adopt" and method == "POST":
        return _env({"adopted": True, "discId": 11, "body": json.loads(request.content or b"{}")})
    if path == "/api/discovery/11/ignore" and method == "POST":
        return _env({"ignored": True, "discId": 11})
    if path == "/api/enrollments/5/reject" and method == "POST":
        return _env({"rejected": True, "enrId": 5})
    if path == "/api/enrollments/5/approve" and method == "POST":
        if not has_auth:
            return _env("operator role required", ok=False, status=403)
        return _env({"approved": True, "enrId": 5})
    if path == "/api/control/provision" and method == "POST":
        return _env({"provisioned": True, "body": json.loads(request.content or b"{}")})
    if path == "/api/control/decommission" and method == "POST":
        if not has_auth:
            return _env("operator role required", ok=False, status=403)
        return _env({"decommissioned": True, "body": json.loads(request.content or b"{}")})
    return _env("unhandled " + path, ok=False, status=404)


def _client(token=None):
    cfg = RestConfig(base_url="https://c2.test", token=token, timeout=5.0, verify_tls=True)
    return RestClient(cfg, transport=httpx.MockTransport(_handler))


@pytest.fixture(autouse=True)
def reset_clients():
    yield
    server.configure(None, None)


# ---- reads ----------------------------------------------------------------
async def test_list_discovery():
    server.configure(rest=_client())
    out = await server.solarinet_list_discovery(DiscoveryListInput())
    assert "printer.lab" in out and "| discId |" in out


async def test_list_enrollments_json():
    server.configure(rest=_client())
    out = await server.solarinet_list_enrollments(
        EnrollmentListInput(response_format=ResponseFormat.JSON)
    )
    assert json.loads(out)[0]["role"] == "monitor"


async def test_list_builds():
    server.configure(rest=_client())
    out = await server.solarinet_list_builds(BuildsInput())
    assert "1.4.2" in out


async def test_list_segments():
    server.configure(rest=_client())
    out = await server.solarinet_list_segments(SegmentsInput())
    assert "10.42.20.0/24" in out


async def test_list_netgear():
    server.configure(rest=_client())
    out = await server.solarinet_list_netgear(NetgearInput())
    assert "sw-core-01" in out


async def test_topology_view_monitoring_and_network():
    server.configure(rest=_client())
    mon = await server.solarinet_get_topology_view(
        TopologyViewInput(view="monitoring", response_format=ResponseFormat.JSON)
    )
    net = await server.solarinet_get_topology_view(
        TopologyViewInput(view="network", response_format=ResponseFormat.JSON)
    )
    assert json.loads(mon)["view"] == "monitoring"
    assert json.loads(net)["view"] == "network"


async def test_get_config():
    server.configure(rest=_client())
    out = await server.solarinet_get_config(ConfigReadInput(response_format=ResponseFormat.JSON))
    assert json.loads(out)["autoEnroll"] is False


# ---- non-destructive control ----------------------------------------------
async def test_adopt_discovery_forwards_role_and_spec():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_adopt_discovery(
        DiscoveryAdoptInput(disc_id="11", role="monitor", probe_spec={"type": "tcp", "port": 443})
    )
    assert "Adopted" in out
    sent = json.loads(out.split("\n\n", 1)[1])["body"]
    assert sent["role"] == "monitor" and sent["probeSpec"]["port"] == 443


async def test_ignore_discovery():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_ignore_discovery(DiscoveryIgnoreInput(disc_id="11"))
    assert "Ignored" in out


async def test_reject_enrollment():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_reject_enrollment(EnrollmentRejectInput(enr_id="5"))
    assert "Rejected" in out


async def test_provision_requires_a_target():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_provision(ProvisionInput(config_blob={"a": 1}))
    assert out.startswith("Error") and "node_id or enr_id" in out


async def test_provision_with_node_id():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_provision(
        ProvisionInput(node_id="0x05", config_blob={"a": 1}, build_id="1")
    )
    assert "Provision requested" in out
    sent = json.loads(out.split("\n\n", 1)[1])["body"]
    assert sent["nodeId"] == "0x05" and sent["buildId"] == "1"


# ---- destructive: operator token + explicit confirm -----------------------
async def test_approve_enrollment_needs_operator_token():
    server.configure(rest=_client(token=None))
    out = await server.solarinet_approve_enrollment(
        EnrollmentApproveInput(enr_id="5", confirm=True)
    )
    assert out.startswith("Error") and "operator token" in out


async def test_approve_enrollment_needs_confirm():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_approve_enrollment(
        EnrollmentApproveInput(enr_id="5", confirm=False)
    )
    assert out.startswith("Error") and "confirm=true" in out


async def test_approve_enrollment_succeeds_with_token_and_confirm():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_approve_enrollment(
        EnrollmentApproveInput(enr_id="5", confirm=True)
    )
    assert "Approved enrollment 5" in out


async def test_decommission_needs_operator_token():
    server.configure(rest=_client(token=None))
    out = await server.solarinet_decommission(
        DecommissionInput(node_id="0x05", wipe_scope=["config"], confirm=True)
    )
    assert out.startswith("Error") and "operator token" in out


async def test_decommission_needs_confirm():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_decommission(
        DecommissionInput(node_id="0x05", wipe_scope=["config"], confirm=False)
    )
    assert out.startswith("Error") and "confirm=true" in out


async def test_decommission_succeeds_and_sends_confirm_token():
    server.configure(rest=_client(token="t"))
    out = await server.solarinet_decommission(
        DecommissionInput(node_id="0x05", wipe_scope=["config", "certs"], confirm=True)
    )
    assert "Decommission requested for node 0x05" in out
    sent = json.loads(out.split("\n\n", 1)[1])["body"]
    assert sent["confirm"] is True and sent["wipeScope"] == ["config", "certs"]


def test_new_tools_are_registered():
    names = server.list_tools()
    for n in (
        "solarinet_list_discovery", "solarinet_list_enrollments", "solarinet_list_builds",
        "solarinet_list_segments", "solarinet_list_netgear", "solarinet_get_topology_view",
        "solarinet_get_config", "solarinet_adopt_discovery", "solarinet_ignore_discovery",
        "solarinet_reject_enrollment", "solarinet_provision",
        "solarinet_approve_enrollment", "solarinet_decommission",
    ):
        assert n in names
