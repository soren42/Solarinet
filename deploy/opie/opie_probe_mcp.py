#!/usr/bin/env python3
"""Narrow MCP bridge for Opie's vetted host probe.

The model receives one structured tool and no shell or general file tools.
This trusted server validates inputs and invokes the fixed probe executable
with an argv list; it never evaluates model-provided shell text.
"""
from __future__ import annotations

import os
import re
import subprocess
from typing import Literal

from mcp.server.fastmcp import FastMCP

ProbeCheck = Literal[
    "failed-units", "unit", "logs", "kernel", "disk", "mounts", "smart",
    "net", "proc", "load", "dmesg-errors", "service", "ping",
]
_ARG_CHECKS = {"unit", "logs", "smart", "service"}
_CHECKS = {
    "failed-units", "unit", "logs", "kernel", "disk", "mounts", "smart",
    "net", "proc", "load", "dmesg-errors", "service", "ping",
}
_HOST = re.compile(r"\A[a-zA-Z0-9.-]{1,253}\Z")
_ARG = re.compile(r"\A[a-zA-Z0-9._@:/-]{1,128}\Z")
_MAX_OUTPUT = 24000

mcp = FastMCP("opie")


@mcp.tool(name="probe", description="Run one fixed read-only diagnostic against an allowlisted SolariNet host.")
def probe(host: str, check: ProbeCheck, argument: str = "") -> str:
    """Execute a vetted diagnostic; empty argument is required for checks without parameters."""
    if check not in _CHECKS:
        return "Error: unsupported diagnostic."
    if not _HOST.fullmatch(host):
        return "Error: invalid host name."
    if check in _ARG_CHECKS:
        if not _ARG.fullmatch(argument):
            return f"Error: {check} requires one valid argument."
    elif argument:
        return f"Error: {check} does not accept an argument."

    executable = os.environ.get("OPIE_PROBE_CMD", "/usr/local/bin/opie-probe")
    try:
        result = subprocess.run(
            [executable, host, check, argument],
            capture_output=True,
            text=True,
            timeout=50,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return "Error: diagnostic timed out."
    except OSError as exc:
        return f"Error: diagnostic could not start ({type(exc).__name__})."

    output = (result.stdout or "")
    if result.stderr:
        output += ("\n" if output else "") + result.stderr
    output = output[:_MAX_OUTPUT]
    if not output:
        output = "(no diagnostic output)"
    if result.returncode != 0:
        return f"Diagnostic exited with status {result.returncode}.\n{output}"
    return output


if __name__ == "__main__":
    mcp.run()
