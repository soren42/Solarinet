#!/usr/bin/env python3
"""Prove SoR worker grants on an explicitly disposable loopback MariaDB."""

from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import uuid

import pymysql


ROOT = Path(__file__).resolve().parents[2]
GRANTS = ROOT / "deploy/sorsync/sor-runtime-grants.sql"
DB = os.environ.get("SOR_RUNTIME_TEST_DB", "sor_test")
PORT = int(os.environ.get("SOR_RUNTIME_TEST_PORT", "3306"))
USER_HOST = os.environ.get("SOR_RUNTIME_TEST_USER_HOST", "127.0.0.1")
TOKEN = uuid.uuid4().hex[:12]
USERS = {role: f"srt_{role}_{TOKEN}" for role in ("ro", "emit", "discovery", "unifi")}
PASSWORDS = {role: f"runtime_{uuid.uuid4().hex}" for role in USERS}
sys.path.insert(0, str(ROOT / "deploy/unifi"))
import unifi_network_sor as unifi_importer  # noqa: E402


def admin(sql: str) -> None:
    if os.environ.get("SOR_RUNTIME_TEST_ADMIN_SOCKET") == "1":
        command = ["sudo", "-n", "mariadb"]
    else:
        command = ["mariadb", "--protocol=TCP", "--host=127.0.0.1", "--port",
                   str(PORT), "--user", os.environ.get("SOR_RUNTIME_TEST_ADMIN_USER", "root")]
    result = subprocess.run(command, input=sql, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            check=False)
    if result.returncode:
        raise RuntimeError("scratch MariaDB admin operation failed: " + result.stdout[-1000:])
    return result.stdout


def assert_denied(connection, sql: str) -> None:
    try:
        with connection.cursor() as cursor:
            cursor.execute(sql)
    except pymysql.err.OperationalError as exc:
        if exc.args and exc.args[0] in (1142, 1143, 1227):
            return
        raise
    raise AssertionError("least-privilege user unexpectedly succeeded: " + sql.split()[0])


def connect(role: str):
    return pymysql.connect(host="127.0.0.1", port=PORT, user=USERS[role],
                           password=PASSWORDS[role], database=DB,
                           charset="utf8mb4", autocommit=False)


def admin_query_command() -> list[str]:
    query = "SELECT marker FROM _solarinet_test_scratch WHERE marker='solarinet-ci-scratch-v1'"
    if os.environ.get("SOR_RUNTIME_TEST_ADMIN_SOCKET") == "1":
        return ["sudo", "-n", "mariadb", DB, "-N", "-B", "-e", query]
    return ["mariadb", "--protocol=TCP", "--host=127.0.0.1", "--port", str(PORT),
            "--user", os.environ.get("SOR_RUNTIME_TEST_ADMIN_USER", "root"), DB,
            "--batch", "--skip-column-names", "--execute", query]


def main() -> int:
    if (DB not in {"sor_test", "solarinet_stage", "solarinet_sorsync_ci"}
            or os.environ.get("SOR_RUNTIME_TEST_ALLOW_CREATE_USERS") != "1"
            or USER_HOST not in {"127.0.0.1", "%"}
            or os.environ.get("SOR_RUNTIME_TEST_ADMIN_HOST") != "127.0.0.1"):
        raise SystemExit("refusing SoR runtime grant test outside an opted-in loopback scratch DB")
    marker = subprocess.run(
        admin_query_command(),
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if marker.returncode or marker.stdout.strip() != "solarinet-ci-scratch-v1":
        raise SystemExit("refusing SoR runtime grant test without the disposable-schema sentinel")

    created: list[str] = []
    try:
        setup = []
        for role, user in USERS.items():
            if not re.fullmatch(r"srt_[a-z]+_[0-9a-f]{12}", user):
                raise SystemExit("invalid generated fixture principal")
            setup.append(f"CREATE USER '{user}'@'{USER_HOST}' IDENTIFIED BY '{PASSWORDS[role]}';")
            created.append(user)
        admin("\n".join(setup) + "\n")

        sql = GRANTS.read_text(encoding="utf-8")
        sql = sql.replace("`sor`.", f"`{DB}`.")
        for role, user in USERS.items():
            sql = sql.replace(f"'solari_sor_{role}'@'10.0.0.0/255.0.0.0'",
                              f"'{user}'@'{USER_HOST}'")
        if "'solari_sor_" in sql or "`sor`." in sql:
            raise SystemExit("runtime grant template rewrite incomplete")
        admin(sql)
        for user in USERS.values():
            grants = admin(f"SHOW GRANTS FOR '{user}'@'{USER_HOST}';")
            if any(" ON *.* " in line and not line.startswith("GRANT USAGE ")
                   for line in grants.splitlines()):
                raise AssertionError("runtime identity received a global privilege")

        ro = connect("ro")
        emit = connect("emit")
        discovery = connect("discovery")
        try:
            with ro.cursor() as c:
                for query in ("SELECT name FROM dns_zones LIMIT 1",
                              "SELECT host, ip, role FROM v_dns_hosts LIMIT 1",
                              "SELECT label, ip FROM v_dns_ifaces LIMIT 1",
                              "SELECT alias, target_host FROM v_dns_cnames LIMIT 1"):
                    c.execute(query)
            assert_denied(ro, "UPDATE dns_zones SET name=name WHERE 1=0")
            assert_denied(ro, "CREATE TABLE __runtime_ro_forbidden (id INT)")

            with emit.cursor() as c:
                c.execute("SELECT id FROM sor_outbox ORDER BY id DESC LIMIT 1")
                c.execute("SELECT v FROM sor_sync_state WHERE k='runtime-grant-test'")
                c.execute("INSERT INTO sor_sync_state(k,v) VALUES ('runtime-grant-test','1') "
                          "ON DUPLICATE KEY UPDATE v=VALUES(v)")
                c.execute("UPDATE sor_sync_state SET v='2' WHERE k='runtime-grant-test'")
            assert_denied(emit, "DELETE FROM sor_sync_state WHERE k='runtime-grant-test'")
            assert_denied(emit, "CREATE TABLE __runtime_emit_forbidden (id INT)")

            with discovery.cursor() as c:
                c.execute("SELECT id FROM sources WHERE slug='solarinet_monitor' LIMIT 1")
                row = c.fetchone()
                if not row:
                    raise AssertionError("scratch SoR lacks the solarinet_monitor source")
                source_id = row[0]
                c.execute("SELECT entity_id FROM ip_addresses WHERE deleted_at IS NULL LIMIT 1")
                entity_name = "grant-test-" + TOKEN
                # Keep the address fixture out of the active DNS renderer view;
                # it is deliberately IPv6 while the current zone generator emits A records.
                c.execute("INSERT INTO entities(name,entity_type,lifecycle,source_id,asserted_kind) "
                          "VALUES (%s,'other','retired',%s,'machine')", (entity_name, source_id))
                entity_id = c.lastrowid
                c.execute("INSERT INTO ip_addresses(entity_id,address,is_primary,source_id,asserted_kind) "
                          "VALUES (%s,INET6_ATON('2001:db8::dead:beef'),1,%s,'machine')",
                          (entity_id, source_id))
            assert_denied(discovery, "UPDATE entities SET name=name WHERE 1=0")
            assert_denied(discovery, "DELETE FROM ip_addresses WHERE 1=0")
            assert_denied(discovery, "CREATE TABLE __runtime_discovery_forbidden (id INT)")
        finally:
            for connection in (ro, emit, discovery):
                connection.rollback()
                connection.close()

        # Exercise the actual UniFi importer through its split-identity config.
        site_id = "runtime-" + TOKEN
        network_id = "network-" + TOKEN
        config_dir = tempfile.TemporaryDirectory(prefix="sor-runtime-config-", dir="/dev/shm")
        config_path = Path(config_dir.name) / "sorsync.conf"
        config_path.write_text(
            "[sor]\nhost = 127.0.0.1\nport = %d\nuser = %s\npassword = unused\nname = %s\n\n"
            "[sor_unifi]\nhost = 127.0.0.1\nport = %d\nuser = %s\npassword = %s\nname = %s\n"
            % (PORT, USERS["ro"], DB, PORT, USERS["unifi"], PASSWORDS["unifi"], DB),
            encoding="utf-8")
        uni = None
        try:
            uni = unifi_importer._db_connect(str(config_path))
            with uni.cursor() as c:
                c.execute("SELECT vlan_tag FROM vlans")
                used_vlans = {row[0] for row in c.fetchall()}
                vlan_tag = next((tag for tag in range(4094, 3000, -1) if tag not in used_vlans), None)
                if vlan_tag is None:
                    raise AssertionError("scratch schema has no free VLAN fixture tag")
                c.execute("SELECT cidr FROM subnets")
                used_cidrs = {row[0] for row in c.fetchall()}
                cidr = next((f"198.18.{octet}.0/24" for octet in range(256)
                             if f"198.18.{octet}.0/24" not in used_cidrs), None)
                if cidr is None:
                    raise AssertionError("scratch schema has no free subnet fixture")
            gateway = cidr.split("/")[0].rsplit(".", 1)[0] + ".1"
            prefix = gateway.rsplit(".", 1)[0]
            site = {"id": site_id, "name": "SoR runtime grant fixture"}
            detail = {"id": network_id, "name": "runtime-" + TOKEN,
                      "vlanId": vlan_tag, "enabled": True, "management": "UNMANAGED",
                      "default": False,
                      "ipv4Configuration": {"hostIpAddress": gateway, "prefixLength": 24,
                                            "dhcpConfiguration": {
                                                "mode": "SERVER", "leaseTimeSeconds": 3600,
                                                "ipAddressRange": {"start": prefix + ".10",
                                                                    "stop": prefix + ".50"}}}}
            record = unifi_importer.normalize_network(site, detail)
            snapshot = {"site_id": site_id, "networks": [record]}
            unifi_importer.apply_snapshots(uni, [snapshot])
            unifi_importer.apply_snapshots(uni, [snapshot])
            assert_denied(uni, "DELETE FROM networks WHERE 1=0")
            assert_denied(uni, "CREATE TABLE __runtime_unifi_forbidden (id INT)")
        finally:
            if uni is not None:
                uni.close()
            safe_site = site_id.replace("'", "")
            cleanup_sql = f"""USE `{DB}`;
DELETE FROM `dhcp_ranges` WHERE id IN (SELECT subject_id FROM `external_refs`
 WHERE source_id=(SELECT id FROM `sources` WHERE slug='unifi') AND external_id LIKE '{safe_site}:%'
 AND subject_table='dhcp_ranges');
DELETE FROM `subnets` WHERE id IN (SELECT subject_id FROM `external_refs`
 WHERE source_id=(SELECT id FROM `sources` WHERE slug='unifi') AND external_id LIKE '{safe_site}:%'
 AND subject_table='subnets');
DELETE FROM `networks` WHERE id IN (SELECT subject_id FROM `external_refs`
 WHERE source_id=(SELECT id FROM `sources` WHERE slug='unifi') AND external_id LIKE '{safe_site}:%'
 AND subject_table='networks');
DELETE FROM `vlans` WHERE id IN (SELECT subject_id FROM `external_refs`
 WHERE source_id=(SELECT id FROM `sources` WHERE slug='unifi') AND external_id LIKE '{safe_site}:%'
 AND subject_table='vlans');
DELETE FROM `external_refs` WHERE source_id=(SELECT id FROM `sources` WHERE slug='unifi')
 AND external_id LIKE '{safe_site}:%';
"""
            admin(cleanup_sql)
            config_dir.cleanup()
        print("SoR runtime roles: read-only render, outbox checkpoint, discovery insert, UniFi upsert; excess DML and DDL denied")
        return 0
    finally:
        if created:
            admin("\n".join(f"DROP USER IF EXISTS '{user}'@'{USER_HOST}';" for user in created) + "\n")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"SoR runtime grant test failed: {type(exc).__name__}: {exc}", file=sys.stderr)
        raise
