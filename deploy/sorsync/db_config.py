"""Resolve separated SoR runtime identities with backwards-compatible fallback."""

from __future__ import annotations


def database_options(config, section: str) -> dict[str, str | int]:
    """Read a complete worker identity, or legacy [sor] when absent."""
    active_section = section if config.has_section(section) else "sor"
    values: dict[str, str | int] = {}
    for key, default in (("host", "10.1.0.200"), ("port", 3306),
                         ("user", "solari"), ("password", None), ("name", "sor")):
        value = config.get(active_section, key, fallback=default)
        values[key] = int(value) if key == "port" else value
    for key in ("host", "user", "name"):
        if not isinstance(values[key], str) or not values[key].strip():
            raise ValueError(f"database {key} is missing in [{section}] and [sor]")
    if not isinstance(values["password"], str) or not values["password"]:
        raise ValueError(f"database password is missing in [{section}] and [sor]")
    if not 1 <= values["port"] <= 65535:
        raise ValueError(f"database port is invalid in [{section}] and [sor]")
    return values
