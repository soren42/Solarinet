"""REST tool tests using an injected httpx.MockTransport (no live server)."""
import json

import httpx
import pytest

from solarinet_mcp import server
from solarinet_mcp.config import RestConfig
from solarinet_mcp.rest import RestClient
from solarinet_mcp.formatting import ResponseFormat
from solarinet_mcp.server import (
    ListNodesInput, NodeIdInput, AlertsInput, FormatOnlyInput, SurveyInput,
)

NODES = [
    {"nodeId": "0x01", "role": "server", "hostFqdn": "argon.akoria.net", "state": "up",
     "lastSeenAt": "2026-06-06T22:00:00Z", "arch": "x86_64", "osName": "Linux 6.1.0"},
    {"nodeId": "0x02", "role": "monitor", "hostFqdn": "vantage1.akoria.net", "state": "degraded",
     "lastSeenAt": "2026-06-06T22:00:01Z", "arch": "arm64", "osName": "Linux 6.1.0"},
]
ALERTS = [
    {"eventId": 7, "severity": "crit", "detail": "loss>500", "nodeId": "0x02", "firedAt": "2026-06-06T21:00:00Z"},
]


def _envelope(data, ok=True, status=200):
    if ok:
        body = {"ok": True, "data": data, "ts": "2026-06-06T22:00:00Z"}
    else:
        body = {"ok": False, "error": {"code": "not_found", "message": data}}
    return httpx.Response(status, json=body)


def _handler(request: httpx.Request) -> httpx.Response:
    path = request.url.path
    if path == "/api/nodes":
        return _envelope(NODES)
    if path == "/api/nodes/0x02":
        return _envelope(NODES[1])
    if path == "/api/nodes/0x99":
        return _envelope("no such node", ok=False, status=404)
    if path == "/api/alerts":
        return _envelope(ALERTS)
    if path == "/api/server/status":
        return _envelope({"primary": "0x01", "failover": "0x03", "leaseEpoch": 5,
                          "expiresAt": "2026-06-06T22:00:15Z"})
    if path == "/api/control/survey":
        if "authorization" not in {k.lower() for k in request.headers.keys()}:
            return _envelope("operator role required", ok=False, status=403)
        return _envelope({"requested": True, "scope": json.loads(request.content)["scope"]})
    return _envelope("unhandled", ok=False, status=404)


def _client(token=None):
    cfg = RestConfig(base_url="https://c2.test", token=token, timeout=5.0, verify_tls=True)
    return RestClient(cfg, transport=httpx.MockTransport(_handler))


@pytest.fixture(autouse=True)
def reset_clients():
    yield
    server.configure(None, None)


async def test_list_nodes_markdown():
    server.configure(rest=_client())
    out = await server.solarinet_list_nodes(ListNodesInput())
    assert "argon.akoria.net" in out and "vantage1.akoria.net" in out
    assert "| nodeId |" in out  # markdown table header


async def test_list_nodes_json():
    server.configure(rest=_client())
    out = await server.solarinet_list_nodes(ListNodesInput(response_format=ResponseFormat.JSON))
    parsed = json.loads(out)
    assert isinstance(parsed, list) and parsed[0]["hostFqdn"] == "argon.akoria.net"


async def test_get_node():
    server.configure(rest=_client())
    out = await server.solarinet_get_node(NodeIdInput(node_id="0x02"))
    assert "vantage1.akoria.net" in out and "degraded" in out


async def test_get_node_404_is_actionable_error():
    server.configure(rest=_client())
    out = await server.solarinet_get_node(NodeIdInput(node_id="0x99"))
    assert out.startswith("Error") and "not_found" in out


async def test_server_status():
    server.configure(rest=_client())
    out = await server.solarinet_server_status(FormatOnlyInput(response_format=ResponseFormat.JSON))
    assert json.loads(out)["primary"] == "0x01"


async def test_survey_requires_auth():
    server.configure(rest=_client(token=None))
    out = await server.solarinet_trigger_survey(SurveyInput(scope="all"))
    # The API reports authorization failures via the ok=false envelope.
    assert out.startswith("Error") and "operator role required" in out


async def test_survey_with_token_succeeds():
    server.configure(rest=_client(token="secret"))
    out = await server.solarinet_trigger_survey(SurveyInput(scope="all"))
    assert "Survey requested" in out and "all" in out


async def test_unconfigured_rest_errors_clearly():
    server.configure(rest=RestClient(RestConfig(base_url="", token=None, timeout=5.0, verify_tls=True)))
    out = await server.solarinet_list_nodes(ListNodesInput())
    assert "not configured" in out.lower()
