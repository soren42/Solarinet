"""Tests for the read-only SQL guard - the security-critical layer."""
import pytest

from solarinet_mcp.db import ALLOWED_TABLES, DbError, validate_read_only, _enforce_limit


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


@pytest.mark.parametrize("table", [
    # section 5 C2-capability additions must be readable
    "segment", "networkGear", "lldpEdge", "discovered", "enrollment", "buildArtifact",
])
def test_allow_list_includes_new_tables(table):
    assert table.lower() in ALLOWED_TABLES
    # and a SELECT against each passes the guard
    out = validate_read_only(f"SELECT * FROM {table}")
    assert out.lower().startswith("select")


@pytest.mark.parametrize("table", [
    # base section 10 tables stay readable (regression)
    "node", "hostHistory", "alertEvent", "probeTarget", "nodeConfig", "serverLease",
])
def test_allow_list_keeps_base_tables(table):
    assert validate_read_only(f"SELECT * FROM {table} WHERE 1=1")


@pytest.mark.parametrize("sql", [
    "SELECT * FROM information_schema.tables",
    "SELECT user, password FROM mysql.user",
    "SELECT * FROM performance_schema.threads",
    "SELECT * FROM secrets",
    "SELECT n.* FROM node n JOIN mysql.user u ON 1=1",  # disallowed via JOIN
])
def test_allow_list_rejects_unlisted_tables(sql):
    with pytest.raises(DbError):
        validate_read_only(sql)


def test_allow_list_permits_join_of_allowed_tables():
    sql = ("SELECT e.eventId FROM alertEvent e "
           "LEFT JOIN alertRule r ON e.ruleId = r.ruleId")
    assert validate_read_only(sql)


def test_allow_list_exempts_cte_names():
    sql = "WITH recent AS (SELECT * FROM discovered) SELECT * FROM recent"
    assert validate_read_only(sql)


def test_enforce_limit_adds_when_missing():
    assert _enforce_limit("SELECT * FROM node", 50).endswith("LIMIT 50")


def test_enforce_limit_keeps_existing():
    sql = "SELECT * FROM node LIMIT 5"
    assert _enforce_limit(sql, 50) == sql
