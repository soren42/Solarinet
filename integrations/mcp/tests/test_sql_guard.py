"""Tests for the read-only SQL guard - the security-critical layer."""
import pytest

from solarinet_mcp.db import DbError, validate_read_only, _enforce_limit


@pytest.mark.parametrize("sql", [
    "SELECT * FROM node",
    "select nodeId from node where role='monitor'",
    "  SELECT 1  ",
    "WITH x AS (SELECT 1) SELECT * FROM x",
    "SELECT * FROM node;",                       # single trailing semicolon ok
    "SELECT * FROM node -- comment\n",
])
def test_accepts_read_only(sql):
    out = validate_read_only(sql)
    assert out.lower().startswith(("select", "with"))


@pytest.mark.parametrize("sql", [
    "INSERT INTO node VALUES (1)",
    "UPDATE node SET state='down'",
    "DELETE FROM node",
    "DROP TABLE node",
    "ALTER TABLE node ADD x INT",
    "TRUNCATE node",
    "GRANT ALL ON *.* TO bad",
    "CREATE TABLE x (a int)",
    "REPLACE INTO node VALUES (1)",
    "CALL someproc()",
])
def test_rejects_writes(sql):
    with pytest.raises(DbError):
        validate_read_only(sql)


def test_rejects_stacked_statements():
    with pytest.raises(DbError):
        validate_read_only("SELECT * FROM node; DROP TABLE node")


def test_rejects_comment_hidden_stack():
    # A second statement hidden after a line comment must not slip through.
    with pytest.raises(DbError):
        validate_read_only("SELECT 1 -- ok\n; DROP TABLE node")


def test_rejects_into_outfile():
    with pytest.raises(DbError):
        validate_read_only("SELECT * FROM node INTO OUTFILE '/tmp/x'")


def test_rejects_empty():
    with pytest.raises(DbError):
        validate_read_only("   ")


def test_enforce_limit_adds_when_missing():
    assert _enforce_limit("SELECT * FROM node", 50).endswith("LIMIT 50")


def test_enforce_limit_keeps_existing():
    sql = "SELECT * FROM node LIMIT 5"
    assert _enforce_limit(sql, 50) == sql
