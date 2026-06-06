"""
Command-line entry point: `python -m solarinet_mcp`.

  --check   Print configuration status and the registered tool list, then exit
            WITHOUT starting the stdio loop. Safe for CI / smoke tests.
  (default) Start the MCP server on the configured transport (stdio by default).
"""
from __future__ import annotations

import argparse
import sys

from .config import load_config
from .server import list_tools, run


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="solarinet_mcp", description="SolariNet MCP server")
    parser.add_argument(
        "--check", action="store_true",
        help="Print config + tool list and exit (do not start the server).",
    )
    args = parser.parse_args(argv)

    if args.check:
        cfg = load_config()
        print("SolariNet MCP server")
        print(f"  transport : {cfg.transport}" + (f" (port {cfg.port})" if cfg.transport == "streamable_http" else ""))
        print(f"  REST API  : {'configured -> ' + cfg.rest.base_url if cfg.rest.configured else 'NOT configured (set SOLARINET_API_BASE)'}")
        print(f"  REST auth : {'token set' if cfg.rest.token else 'no token (control actions will fail)'}")
        print(f"  DB        : {'configured -> ' + cfg.db.user + '@' + cfg.db.host + ':' + str(cfg.db.port) + '/' + cfg.db.name if cfg.db.configured else 'NOT configured (set SOLARINET_DB_USER/PASS)'}")
        tools = list_tools()
        print(f"  tools ({len(tools)}):")
        for t in tools:
            print(f"    - {t}")
        return 0

    run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
