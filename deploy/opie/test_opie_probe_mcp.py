"""Tests for the only model-visible Opie diagnostic tool."""
import asyncio
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

try:
    import mcp
    import opie_probe_mcp
    from mcp import ClientSession, StdioServerParameters
    from mcp.client.stdio import stdio_client
except ImportError as exc:
    raise unittest.SkipTest("Install deploy/opie/requirements.txt for MCP tool tests") from exc


class ProbeMcpTests(unittest.TestCase):
    def test_stdio_server_exposes_only_the_probe_tool(self):
        async def inspect_server():
            params = StdioServerParameters(
                command=sys.executable,
                args=[str(Path(__file__).with_name("opie_probe_mcp.py"))],
                # The MCP client intentionally gives child processes a
                # restricted environment that omits PYTHONPATH. Preserve the
                # package location used by this interpreter so the server can
                # import the same installed MCP SDK in CI and virtualenvs.
                env={"PYTHONPATH": str(Path(mcp.__file__).resolve().parent.parent)},
            )
            async with stdio_client(params) as (read_stream, write_stream):
                async with ClientSession(read_stream, write_stream) as session:
                    await session.initialize()
                    return await session.list_tools()

        result = asyncio.run(inspect_server())
        self.assertEqual([tool.name for tool in result.tools], ["probe"])
        self.assertEqual(set(result.tools[0].inputSchema["properties"]), {"host", "check", "argument"})

    def test_valid_request_uses_fixed_argv_without_shell(self):
        result = Mock(stdout="service active", stderr="", returncode=0)
        with patch.dict(os.environ, {"OPIE_PROBE_CMD": "/usr/local/bin/opie-probe"}), \
             patch.object(opie_probe_mcp.subprocess, "run", return_value=result) as run:
            output = opie_probe_mcp.probe("xenon", "unit", "ssh.service")
        self.assertEqual(output, "service active")
        run.assert_called_once_with(
            ["/usr/local/bin/opie-probe", "xenon", "unit", "ssh.service"],
            capture_output=True, text=True, timeout=50, check=False,
        )

    def test_host_and_argument_validation_happen_before_subprocess(self):
        with patch.object(opie_probe_mcp.subprocess, "run") as run:
            self.assertIn("invalid host", opie_probe_mcp.probe("10.0.0.1;id", "ping"))
            self.assertIn("valid argument", opie_probe_mcp.probe("xenon", "unit", "ssh.service;id"))
            self.assertIn("does not accept", opie_probe_mcp.probe("xenon", "ping", "extra"))
        run.assert_not_called()

    def test_runtime_errors_are_bounded_and_do_not_expose_exception_text(self):
        with patch.object(opie_probe_mcp.subprocess, "run", side_effect=FileNotFoundError("private path")):
            output = opie_probe_mcp.probe("xenon", "ping")
        self.assertEqual(output, "Error: diagnostic could not start (FileNotFoundError).")


if __name__ == "__main__":
    unittest.main()
