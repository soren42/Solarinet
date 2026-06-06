"""
Shared response-formatting helpers.

Every data-returning tool supports two formats (see best practices):
  - "json": complete machine-readable payload (json.dumps).
  - "markdown": compact human-readable rendering.

These helpers are pure functions over already-fetched data so they are trivially
unit-testable without any network or DB.
"""
from __future__ import annotations

import json
from enum import Enum
from typing import Any, Iterable, Mapping, Sequence


class ResponseFormat(str, Enum):
    """Output format for tool responses."""

    MARKDOWN = "markdown"
    JSON = "json"


def to_json(payload: Any) -> str:
    """Serialize payload as pretty JSON with stable key order off (insertion)."""
    return json.dumps(payload, indent=2, default=str)


def md_table(rows: Sequence[Mapping[str, Any]], columns: Sequence[str]) -> str:
    """Render a list of dict rows as a GitHub-flavored markdown table.

    Only the named columns are shown, in order. Missing keys render blank.
    Returns "(no rows)" for an empty sequence.
    """
    if not rows:
        return "(no rows)"
    header = "| " + " | ".join(columns) + " |"
    sep = "| " + " | ".join("---" for _ in columns) + " |"
    body_lines = []
    for r in rows:
        cells = []
        for c in columns:
            v = r.get(c, "")
            cells.append("" if v is None else str(v))
        body_lines.append("| " + " | ".join(cells) + " |")
    return "\n".join([header, sep, *body_lines])


def md_kv(title: str, data: Mapping[str, Any]) -> str:
    """Render a single record as a markdown heading plus bullet key/values."""
    lines = [f"## {title}", ""]
    for k, v in data.items():
        if isinstance(v, (dict, list)):
            v = to_json(v)
        lines.append(f"- **{k}**: {v}")
    return "\n".join(lines)


def paginate(items: Sequence[Any], total: int, offset: int) -> dict[str, Any]:
    """Build the standard pagination envelope around a page of items."""
    count = len(items)
    has_more = total > offset + count
    return {
        "total": total,
        "count": count,
        "offset": offset,
        "items": list(items),
        "has_more": has_more,
        "next_offset": (offset + count) if has_more else None,
    }


def join_sections(sections: Iterable[str]) -> str:
    """Join non-empty markdown sections with blank lines between them."""
    return "\n\n".join(s for s in sections if s)
