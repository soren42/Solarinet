"""Tests for the pure formatting helpers."""
import json

from solarinet_mcp.formatting import md_table, md_kv, paginate, to_json


def test_md_table_basic():
    rows = [{"a": 1, "b": "x"}, {"a": 2, "b": "y"}]
    out = md_table(rows, ["a", "b"])
    assert out.splitlines()[0] == "| a | b |"
    assert "| 1 | x |" in out


def test_md_table_empty():
    assert md_table([], ["a"]) == "(no rows)"


def test_md_table_missing_key_blank():
    out = md_table([{"a": 1}], ["a", "b"])
    assert out.strip().endswith("| 1 |  |")


def test_md_kv_nested_json():
    out = md_kv("Node", {"id": "0x1", "disks": [{"mount": "/"}]})
    assert "## Node" in out and "**id**: 0x1" in out and "mount" in out


def test_paginate_has_more():
    p = paginate(items=[1, 2], total=5, offset=0)
    assert p["has_more"] is True and p["next_offset"] == 2 and p["count"] == 2


def test_paginate_last_page():
    p = paginate(items=[1, 2], total=2, offset=0)
    assert p["has_more"] is False and p["next_offset"] is None


def test_to_json_roundtrip():
    assert json.loads(to_json({"k": 1})) == {"k": 1}
