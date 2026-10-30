#!/usr/bin/env python3
"""Exercise the SoR dashboard grant file against an explicitly disposable MariaDB."""

import os
import re
import subprocess
import sys
from pathlib import Path

import pymysql


ROOT = Path(__file__).resolve().parents[2]
GRANTS = ROOT / "deploy/dashboard/hardened/sor-dashboard-grants.sql"
DB = os.environ.get("SOR_DASHBOARD_TEST_DB", "sor_test")
TEST_USER = os.environ.get("SOR_DASHBOARD_TEST_USER", "sor_dash_test")
if not re.fullmatch(r"(?:sor_dash_test|sor_dash_ci_[0-9]{1,16})", TEST_USER):
    raise SystemExit("Refusing unexpected dashboard fixture account name.")
TEST_HOST = os.environ.get("SOR_DASHBOARD_TEST_USER_HOST", "%")
TEST_PASSWORD = "sor_dash_test_ephemeral"
TEST_PORT = int(os.environ.get("SOR_DASHBOARD_TEST_PORT", "3306"))


def admin_command():
    """Build the explicitly selected loopback admin transport command."""
    if os.environ.get("SOR_DASHBOARD_ADMIN_SOCKET") == "1":
        return ["sudo", "-n", "mariadb"]
    command = [
        "mariadb", "--protocol=TCP", "--host=127.0.0.1",
        "--port", os.environ.get("SOR_DASHBOARD_ADMIN_PORT", str(TEST_PORT)),
        "--user", os.environ.get("SOR_DASHBOARD_ADMIN_USER", "root"),
    ]
    admin_password = os.environ.get("SOR_DASHBOARD_ADMIN_PASSWORD")
    if admin_password:
        command.append(f"--password={admin_password}")
    return command


EXPECTED = {
    "audit_log": {"INSERT"},
    "barcode_scans": {"SELECT", "INSERT"},
    "barcodes": {"SELECT", "INSERT", "UPDATE"},
    "dhcp_ranges": {"SELECT"},
    "dns_records": {"SELECT"},
    "dns_zones": {"SELECT"},
    "entities": {"SELECT", "INSERT", "UPDATE"},
    "entity_hardware_unit_links": {"SELECT"},
    "hardware_models": {"SELECT", "INSERT"},
    "hardware_units": {"SELECT", "INSERT", "UPDATE"},
    "interconnects": {"SELECT", "INSERT", "UPDATE"},
    "interfaces": {"SELECT"},
    "ip_addresses": {"SELECT", "INSERT", "UPDATE"},
    "locations": {"SELECT", "INSERT", "UPDATE"},
    "networks": {"SELECT", "UPDATE"},
    "project_allocations": {"SELECT", "INSERT", "UPDATE"},
    "projects": {"SELECT", "INSERT", "UPDATE"},
    "receipts": {"SELECT", "INSERT", "UPDATE"},
    "rw_model_groups": {"SELECT", "INSERT", "UPDATE"},
    "rw_plan_versions": {"SELECT", "INSERT", "DELETE"},
    "rw_plans": {"SELECT", "INSERT", "UPDATE"},
    "sources": {"SELECT"},
    "subnets": {"SELECT"},
    "vlans": {"SELECT"},
    "v_dns_cnames": {"SELECT"},
    "v_dns_hosts": {"SELECT"},
    "v_dns_ifaces": {"SELECT"},
    "v_project_shopping": {"SELECT"},
}
DENIED = {"certificates", "config_items", "external_refs", "secrets", "users", "groups", "relationships"}


def apply_grants():
    if DB not in {"sor_test", "solarinet_stage"}:
        raise SystemExit("Refusing grant checks outside an allowlisted scratch schema.")
    if os.environ.get("SOR_DASHBOARD_TEST_ALLOW_CREATE_USER") != "1":
        raise SystemExit("Refusing to create the fixture account without explicit disposable-DB opt-in.")
    if TEST_HOST not in {"127.0.0.1", "localhost", "%"}:
        raise SystemExit("Refusing fixture account host outside the test allowlist.")
    grant_lines = [
        line for line in GRANTS.read_text().splitlines()
        if line.lstrip().startswith("GRANT ")
    ]
    sql = "\n".join(grant_lines) + "\n"
    schema_token = "`sor`."
    principal_token = "'solari_dashboard'@'10.0.0.20'"
    schema_count = sql.count(schema_token)
    principal_count = sql.count(principal_token)
    if len(grant_lines) != len(EXPECTED) or schema_count != len(grant_lines) or principal_count != len(grant_lines):
        raise SystemExit("Grant template drifted; refusing incomplete scratch rewrite.")
    sql = sql.replace(schema_token, f"`{DB}`.", schema_count)
    sql = sql.replace(principal_token, f"'{TEST_USER}'@'{TEST_HOST}'", principal_count)
    if schema_token in sql or principal_token in sql or "`sor`." in sql:
        raise SystemExit("Grant template still contains production identifiers after rewrite.")
    admin_host = os.environ.get("SOR_DASHBOARD_ADMIN_HOST")
    if admin_host != "127.0.0.1":
        raise SystemExit("Refusing admin grant connection outside loopback.")
    command = admin_command()
    result = subprocess.run(
        command, input=sql, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    if result.returncode:
        print(result.stdout, file=sys.stderr, end="")
        raise SystemExit(result.returncode)


def create_fixture_user():
    """Create a unique-to-this-run principal; never change an existing account."""
    command = admin_command()
    sql = f"CREATE USER '{TEST_USER}'@'{TEST_HOST}' IDENTIFIED BY '{TEST_PASSWORD}';\n"
    result = subprocess.run(
        command, input=sql, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    if result.returncode:
        print(result.stdout, file=sys.stderr, end="")
        raise SystemExit("Refusing to alter an existing dashboard fixture account.")


def validate_scratch_identity():
    if DB not in {"sor_test", "solarinet_stage"}:
        raise SystemExit("Refusing grant checks outside an allowlisted scratch schema.")
    if os.environ.get("SOR_DASHBOARD_TEST_ALLOW_CREATE_USER") != "1":
        raise SystemExit("Refusing fixture account changes without explicit disposable-DB opt-in.")
    if os.environ.get("SOR_DASHBOARD_ADMIN_HOST") != "127.0.0.1":
        raise SystemExit("Refusing admin grant connection outside loopback.")
    command = admin_command() + [
        "--database", DB, "--batch", "--skip-column-names", "--silent",
        "--execute", "SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'",
    ]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, check=False)
    if result.returncode or result.stdout.strip() != "solarinet-ci-scratch-v1":
        raise SystemExit("Refusing grant test: loopback schema lacks the disposable scratch sentinel.")


def drop_fixture_user():
    command = admin_command()
    result = subprocess.run(
        command, input=f"DROP USER IF EXISTS '{TEST_USER}'@'{TEST_HOST}';\n", text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False,
    )
    if result.returncode:
        print("fixture user cleanup failed: " + result.stdout, file=sys.stderr, end="")
        raise SystemExit(result.returncode)


def main():
    validate_scratch_identity()
    created_user = False
    try:
        create_fixture_user()
        created_user = True
        apply_grants()
        db = pymysql.connect(
            host="127.0.0.1", user=TEST_USER, password=TEST_PASSWORD,
            port=TEST_PORT, database=DB, charset="utf8mb4", autocommit=True,
        )
    except BaseException:
        if created_user:
            drop_fixture_user()
        raise
    try:
        cur = db.cursor()
        cur.execute(
            "SELECT TABLE_NAME, PRIVILEGE_TYPE FROM information_schema.TABLE_PRIVILEGES "
            "WHERE GRANTEE=%s AND TABLE_SCHEMA=%s",
            (f"'{TEST_USER}'@'{TEST_HOST}'", DB),
        )
        actual = {}
        for table, privilege in cur.fetchall():
            actual.setdefault(table, set()).add(privilege.upper())
        assert actual == EXPECTED, f"grant matrix mismatch: {actual!r}"
        print(f"  ok  exact table grant matrix ({sum(map(len, EXPECTED.values()))} privileges)")

        cur.execute(
            "SELECT PRIVILEGE_TYPE FROM information_schema.USER_PRIVILEGES "
            "WHERE GRANTEE=%s",
            (f"'{TEST_USER}'@'{TEST_HOST}'",),
        )
        global_privileges = {row[0].upper() for row in cur.fetchall()}
        assert global_privileges <= {"USAGE"}, f"unexpected global privileges: {global_privileges}"
        print("  ok  no global privileges")

        readable = [table for table, privileges in EXPECTED.items() if "SELECT" in privileges]
        for table in readable:
            cur.execute(f"SELECT * FROM `{table}` LIMIT 0")
        print(f"  ok  SELECT access for {len(readable)} used tables/views")
        try:
            cur.execute("SELECT * FROM `audit_log` LIMIT 0")
        except pymysql.err.OperationalError as exc:
            assert exc.args[0] == 1142, f"unexpected audit_log read error: {exc!r}"
        else:
            raise AssertionError("audit_log must be append-only for the dashboard")
        print("  ok  audit_log is append-only")

        for table, privileges in EXPECTED.items():
            if "INSERT" in privileges:
                db.begin()
                try:
                    cur.execute(f"INSERT INTO `{table}` (id) VALUES (NULL)")
                except pymysql.err.OperationalError as exc:
                    assert exc.args[0] != 1142, f"INSERT privilege denied on {table}: {exc!r}"
                finally:
                    db.rollback()
            if "UPDATE" in privileges:
                cur.execute(f"UPDATE `{table}` SET id=id WHERE 1=0")
            if "DELETE" in privileges:
                cur.execute(f"DELETE FROM `{table}` WHERE 1=0")
        print("  ok  no-op DML probes for every granted write privilege")

        for table in DENIED:
            try:
                cur.execute(f"SELECT * FROM `{table}` LIMIT 0")
            except pymysql.err.OperationalError as exc:
                assert exc.args[0] == 1142, f"{table}: unexpected denial {exc!r}"
            else:
                raise AssertionError(f"dashboard unexpectedly has SELECT on {table}")
        print(f"  ok  sensitive SoR tables denied ({len(DENIED)} checks)")
    finally:
        db.close()
        if os.environ.get("SOR_DASHBOARD_TEST_KEEP_USER") != "1":
            drop_fixture_user()


if __name__ == "__main__":
    main()
