"""
Configuration for the SolariNet MCP server.

All configuration comes from environment variables so no secrets ever live in
source or on the command line (matching the SolariNet convention in section 13,
where the server password is supplied via SOLARI_DB_PASS rather than a file).

REST layer (the section 11.2 dashboard API):
    SOLARINET_API_BASE   Base URL of the PHP dashboard API,
                         e.g. "https://c2.akoria.net" (no trailing /api).
    SOLARINET_API_TOKEN  Bearer token for the API. Required for control POSTs
                         (operator role); recommended for reads too.
    SOLARINET_API_TIMEOUT  Per-request timeout in seconds (default 30).
    SOLARINET_API_VERIFY_TLS  "0" to disable TLS verification (lab only).

DB layer (read-only access to the MariaDB the C server writes):
    SOLARINET_DB_HOST    MariaDB host (default 127.0.0.1).
    SOLARINET_DB_PORT    MariaDB port (default 3306).
    SOLARINET_DB_NAME    Database name (default solarinet).
    SOLARINET_DB_USER    DB user; should be a READ-ONLY account.
    SOLARINET_DB_PASS    DB password.
    SOLARINET_DB_TIMEOUT DB connect/read timeout seconds (default 10).

Transport:
    SOLARINET_MCP_TRANSPORT  "stdio" (default) or "streamable_http".
    SOLARINET_MCP_PORT       Port for streamable_http (default 8765).
"""
from __future__ import annotations

import os
from dataclasses import dataclass


def _get_int(name: str, default: int) -> int:
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return int(raw)
    except ValueError:
        return default


def _get_bool(name: str, default: bool) -> bool:
    raw = os.environ.get(name)
    if raw is None:
        return default
    return raw.strip().lower() not in ("0", "false", "no", "off", "")


@dataclass(frozen=True)
class RestConfig:
    base_url: str
    token: str | None
    timeout: float
    verify_tls: bool

    @property
    def configured(self) -> bool:
        return bool(self.base_url)


@dataclass(frozen=True)
class DbConfig:
    host: str
    port: int
    name: str
    user: str | None
    password: str | None
    timeout: int

    @property
    def configured(self) -> bool:
        return bool(self.user)


@dataclass(frozen=True)
class Config:
    rest: RestConfig
    db: DbConfig
    transport: str
    port: int


def load_config() -> Config:
    """Build a Config from the current environment. Pure read of os.environ."""
    rest = RestConfig(
        base_url=os.environ.get("SOLARINET_API_BASE", "").rstrip("/"),
        token=os.environ.get("SOLARINET_API_TOKEN") or None,
        timeout=float(_get_int("SOLARINET_API_TIMEOUT", 30)),
        verify_tls=_get_bool("SOLARINET_API_VERIFY_TLS", True),
    )
    db = DbConfig(
        host=os.environ.get("SOLARINET_DB_HOST", "127.0.0.1"),
        port=_get_int("SOLARINET_DB_PORT", 3306),
        name=os.environ.get("SOLARINET_DB_NAME", "solarinet"),
        user=os.environ.get("SOLARINET_DB_USER") or None,
        password=os.environ.get("SOLARINET_DB_PASS") or None,
        timeout=_get_int("SOLARINET_DB_TIMEOUT", 10),
    )
    transport = os.environ.get("SOLARINET_MCP_TRANSPORT", "stdio").strip().lower()
    if transport not in ("stdio", "streamable_http"):
        transport = "stdio"
    port = _get_int("SOLARINET_MCP_PORT", 8765)
    return Config(rest=rest, db=db, transport=transport, port=port)
