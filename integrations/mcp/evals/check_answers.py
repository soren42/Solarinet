#!/usr/bin/env python3
"""
Self-check for the evaluation suite.

Derives each answer in evaluation.xml from the fixture dataset *through the MCP
tools* and asserts it matches. This proves every question is (a) answerable with
the implemented tools and (b) has the stable answer recorded in the XML.

Run:  python evals/check_answers.py
"""
from __future__ import annotations

import asyncio
import json
import os
import sys
import xml.etree.ElementTree as ET

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from solarinet_mcp import server  # noqa: E402
from solarinet_mcp.formatting import ResponseFormat  # noqa: E402
from solarinet_mcp.server import (  # noqa: E402
    ListNodesInput, AlertsInput, ProbesInput, FormatOnlyInput,
    DbHostHistoryInput,
)
from evals import fixture  # noqa: E402


async def _json(coro):
    return json.loads(await coro)


async def derive_answers() -> list[str]:
    server.configure(rest=fixture.build_rest_client(), db=fixture.build_db_client())
    JSON = ResponseFormat.JSON

    nodes = await _json(server.solarinet_list_nodes(ListNodesInput(response_format=JSON)))
    by_id = {n["nodeId"]: n for n in nodes}

    # 1: client count
    a1 = str(sum(1 for n in nodes if n["role"] == "client"))

    # 2: failover host
    status = await _json(server.solarinet_server_status(FormatOnlyInput(response_format=JSON)))
    a2 = by_id[status["failover"]]["hostFqdn"]

    # 3: degraded count
    a3 = str(sum(1 for n in nodes if n["state"] == "degraded"))

    # 4: the single down host
    a4 = next(n["hostFqdn"] for n in nodes if n["state"] == "down")

    # 5: active crit alert count
    alerts = await _json(server.solarinet_list_alerts(AlertsInput(status="active", response_format=JSON)))
    a5 = str(sum(1 for a in alerts if a["severity"] == "crit"))

    # 6: target refused from every vantage
    probes = await _json(server.solarinet_list_probes(ProbesInput(response_format=JSON)))
    a6 = next(p["targetId"] for p in probes
              if all(v["outcome"] == "refused" for v in p["vantages"]))

    # 7: gateway probe outcome from monitor 0x11
    gw = next(p for p in probes if p["targetId"] == "icmp:10.42.0.1")
    a7 = next(v["outcome"] for v in gw["vantages"] if v["monitorNode"] == "0x11")

    # 8: client with lowest diskMinFreePct (DB host history per client)
    best_host, best_disk = None, None
    for n in nodes:
        if n["role"] != "client":
            continue
        hist = await _json(server.solarinet_db_host_history(
            DbHostHistoryInput(node_id=n["nodeId"], response_format=JSON)))
        for row in hist["rows"]:
            d = row["diskMinFreePct"]
            if best_disk is None or d < best_disk:
                best_disk, best_host = d, n["hostFqdn"]
    a8 = best_host

    # 9: vantage1 arch
    a9 = next(n["arch"] for n in nodes if n["hostFqdn"] == "vantage1.akoria.net")

    # 10: lease epoch
    a10 = str(status["leaseEpoch"])

    return [a1, a2, a3, a4, a5, a6, a7, a8, a9, a10]


def load_expected() -> list[str]:
    xml_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "evaluation.xml")
    tree = ET.parse(xml_path)
    return [qa.find("answer").text.strip() for qa in tree.findall("qa_pair")]


def main() -> int:
    expected = load_expected()
    derived = asyncio.run(derive_answers())
    if len(expected) != len(derived):
        print(f"COUNT MISMATCH: {len(expected)} expected vs {len(derived)} derived")
        return 1
    ok = True
    for i, (e, d) in enumerate(zip(expected, derived), 1):
        status = "PASS" if e == d else "FAIL"
        if e != d:
            ok = False
        print(f"  Q{i:<2} [{status}] expected={e!r} derived={d!r}")
    print("-" * 40)
    print(f"{'ALL EVAL ANSWERS VERIFIED' if ok else 'EVAL VERIFICATION FAILED'} "
          f"({sum(1 for e,d in zip(expected,derived) if e==d)}/{len(expected)})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
