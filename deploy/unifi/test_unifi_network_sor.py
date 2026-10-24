#!/usr/bin/env python3
"""UniFi network import contract tests; DB cases require a disposable SoR DB."""

import ipaddress
import os
import sys
import tempfile
import unittest
import uuid
from pathlib import Path
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent.parent / "netdb" / "sor"))

import unifi_network_sor as importer  # noqa: E402
import allocator  # noqa: E402


def site_doc(site_id="site-a"):
    return {"id": site_id, "name": "Test Site"}


def network_doc(network_id="network-a", name="unifi-test-a", vlan=3201,
                address="198.51.100.1", prefix=24, pool=("198.51.100.100", "198.51.100.200")):
    dhcp = {"mode": "NONE"}
    if pool:
        dhcp = {"mode": "SERVER", "leaseTimeSeconds": 86400,
                "ipAddressRange": {"start": pool[0], "stop": pool[1]}}
    return {
        "id": network_id,
        "name": name,
        "vlanId": vlan,
        "enabled": True,
        "management": "UNMANAGED",
        "default": False,
        "ipv4Configuration": {
            "hostIpAddress": address,
            "prefixLength": prefix,
            "dhcpConfiguration": dhcp,
        },
    }


class FakeClient:
    def __init__(self):
        self.calls = []

    def get(self, path):
        self.calls.append(path)
        if path == "/proxy/network/integration/v1/sites?offset=0&limit=200":
            return {"data": [site_doc()], "totalCount": 1}
        if path == "/proxy/network/integration/v1/sites/site-a/networks?offset=0&limit=200":
            return {"data": [{"id": "network-a", "name": "unifi-test-a"}], "totalCount": 1}
        if path == "/proxy/network/integration/v1/sites/site-a/networks/network-a":
            return network_doc()
        raise AssertionError(f"unexpected UniFi API path: {path}")


class UniFiNetworkNormalizeTests(unittest.TestCase):
    def test_client_from_env_uses_connection_ip_and_trust_file(self):
        with tempfile.NamedTemporaryFile() as cafile, \
             patch.dict(os.environ, {
                 "UNIFI_GW": "https://controller.invalid",
                 "UNIFI_API_KEY": "fixture-key",
                 "UNIFI_CA_FILE": cafile.name,
                 "UNIFI_CONNECT_IP": "10.0.0.1",
             }, clear=True), \
             patch.object(importer, "UniFiClient") as client:
            importer._client_from_env()
        client.assert_called_once_with("https://controller.invalid", "fixture-key",
                                       cafile.name, connect_ip="10.0.0.1")

    def test_live_shape_ipv4_and_large_dhcp_pool(self):
        row = importer.normalize_network(site_doc(), {
            "id": "network-live",
            "name": "Default",
            "enabled": True,
            "isDefault": True,
            "management": "GATEWAY",
            "vlanId": 1,
            "ipv4Configuration": {
                "hostIpAddress": "10.0.0.1",
                "prefixLength": 8,
                "dhcpConfiguration": {
                    "mode": "SERVER",
                    "leaseTimeSeconds": 86400,
                    "ipAddressRange": {"start": "10.1.0.26", "stop": "10.7.161.117"},
                },
            },
        })
        self.assertEqual("10.0.0.0/8", row["cidr"])
        self.assertEqual("10.0.0.1", row["gateway_ip"])
        self.assertEqual("10.1.0.26", row["range_start"])
        self.assertEqual("10.7.161.117", row["range_end"])

    def test_rejects_dhcp_range_outside_subnet(self):
        detail = network_doc()
        detail["ipv4Configuration"]["dhcpConfiguration"]["ipAddressRange"]["stop"] = "203.0.113.5"
        with self.assertRaisesRegex(ValueError, "outside"):
            importer.normalize_network(site_doc(), detail)

    def test_fetch_snapshot_uses_paged_overview_and_detail(self):
        client = FakeClient()
        result = importer.fetch_snapshot(client)
        self.assertEqual(1, len(result))
        self.assertEqual("198.51.100.0/24", result[0]["networks"][0]["cidr"])
        self.assertIn("/proxy/network/integration/v1/sites/site-a/networks/network-a", client.calls)

    def test_empty_network_list_refuses_snapshot(self):
        class EmptyClient(FakeClient):
            def get(self, path):
                if path == "/proxy/network/integration/v1/sites/site-a/networks?offset=0&limit=200":
                    return {"data": [], "totalCount": 0}
                return super().get(path)

        with self.assertRaisesRegex(RuntimeError, "refusing an empty import"):
            importer.fetch_snapshot(EmptyClient())


DB_ENABLED = bool(os.environ.get("SOR_TEST_DB"))


@unittest.skipUnless(DB_ENABLED, "set SOR_TEST_DB to a disposable migrated MariaDB schema")
class UniFiNetworkImportDbTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if importer.pymysql is None or not hasattr(importer.pymysql, "connect"):
            raise RuntimeError("PyMySQL is required for UniFi SoR integration tests")
        cls.connection = importer.pymysql.connect(
            host=os.environ.get("SOR_TEST_HOST", "127.0.0.1"),
            port=int(os.environ.get("SOR_TEST_PORT", "3306")),
            user=os.environ.get("SOR_TEST_USER", "sor_test"),
            password=os.environ.get("SOR_TEST_PASSWORD", "sor_test_ephemeral"),
            database=os.environ["SOR_TEST_DB"], charset="utf8mb4", autocommit=False)

    @classmethod
    def tearDownClass(cls):
        cls.connection.close()

    def setUp(self):
        self.site_id = "test-" + uuid.uuid4().hex
        self.prefix = self.site_id + ":%"
        self.source_id = self._source_id("unifi")
        self.provisioner_id = self._source_id("provisioner")
        self.owned = {"networks": set(), "subnets": set(), "dhcp_ranges": set(), "vlans": set()}
        self.foreign_networks = set()
        self.foreign_vlans = set()
        first_vlan = self._available_vlan()
        second_vlan = self._available_vlan({first_vlan})
        self.networks = [
            importer.normalize_network(site_doc(self.site_id), network_doc(vlan=first_vlan)),
            importer.normalize_network(site_doc(self.site_id), network_doc(
                "network-b", "unifi-test-b", second_vlan,
                "203.0.113.1", 24, ("203.0.113.100", "203.0.113.150"))),
        ]

    def tearDown(self):
        cur = self.connection.cursor()
        try:
            cur.execute("SELECT id,subject_table,subject_id FROM external_refs "
                        "WHERE source_id=%s AND external_id LIKE %s", (self.source_id, self.prefix))
            refs = cur.fetchall()
            for _, table, subject_id in refs:
                if table in self.owned:
                    self.owned[table].add(subject_id)
            cur.execute("DELETE FROM external_refs WHERE source_id=%s AND external_id LIKE %s",
                        (self.source_id, self.prefix))
            for table in ("dhcp_ranges", "subnets", "networks", "vlans"):
                if self.owned[table]:
                    placeholders = ",".join(["%s"] * len(self.owned[table]))
                    cur.execute(f"DELETE FROM `{table}` WHERE id IN ({placeholders})", tuple(self.owned[table]))
            for network_id in self.foreign_networks:
                cur.execute("DELETE FROM networks WHERE id=%s", (network_id,))
            for vlan_id in self.foreign_vlans:
                cur.execute("DELETE FROM vlans WHERE id=%s", (vlan_id,))
            self.connection.commit()
        except Exception:
            self.connection.rollback()
            raise
        finally:
            cur.close()

    def _source_id(self, slug):
        cur = self.connection.cursor()
        cur.execute("SELECT id FROM sources WHERE slug=%s AND deleted_at IS NULL", (slug,))
        row = cur.fetchone()
        cur.close()
        if not row:
            self.fail(f"required source {slug!r} is absent from test schema")
        return row[0]

    def _available_vlan(self, excluding=()):
        cur = self.connection.cursor()
        cur.execute("SELECT vlan_tag FROM vlans WHERE deleted_at IS NULL")
        used = {row[0] for row in cur.fetchall()}
        cur.close()
        used.update(excluding)
        for tag in range(4094, 3000, -1):
            if tag not in used:
                return tag
        self.fail("no free VLAN fixture tag")

    def _snapshot(self, networks=None):
        return {"site_id": self.site_id, "networks": self.networks if networks is None else networks}

    def _row(self, table, external_id, live=True):
        cur = self.connection.cursor()
        deleted_clause = "AND deleted_at IS NULL" if live else ""
        cur.execute("SELECT subject_id FROM external_refs WHERE source_id=%s AND subject_table=%s "
                    f"AND external_id=%s {deleted_clause} ORDER BY id DESC LIMIT 1",
                    (self.source_id, table, external_id))
        row = cur.fetchone()
        cur.close()
        self.assertIsNotNone(row, f"missing live {table} external ref {external_id}")
        self.owned[table].add(row[0])
        return row[0]

    def _value(self, sql, args):
        cur = self.connection.cursor()
        cur.execute(sql, args)
        value = cur.fetchone()[0]
        cur.close()
        return value

    def test_import_is_idempotent_and_config_changes_revoke_allocator_approval(self):
        importer.apply_snapshots(self.connection, [self._snapshot()])
        first_id = self._row("networks", f"{self.site_id}:network-a")
        second_id = self._row("networks", f"{self.site_id}:network-b")
        subnet_id = self._row("subnets", f"{self.site_id}:network-a:ipv4")
        dhcp_id = self._row("dhcp_ranges", f"{self.site_id}:network-a:ipv4:dhcp")
        vlan_id = self._row("vlans", f"{self.site_id}:vlan:{self.networks[0]['vlan_id']}")
        second_vlan_id = self._row("vlans", f"{self.site_id}:vlan:{self.networks[1]['vlan_id']}")
        self.assertEqual(0, self._value("SELECT allocator_eligible FROM networks WHERE id=%s", (first_id,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM dhcp_ranges WHERE id=%s AND deleted_at IS NULL", (dhcp_id,)))

        cur = self.connection.cursor()
        cur.execute("UPDATE networks SET allocator_eligible=1,allocator_approved_by='test-operator',"
                    "allocator_approved_at=UTC_TIMESTAMP() WHERE id=%s", (first_id,))
        self.connection.commit()
        importer.apply_snapshots(self.connection, [self._snapshot()])
        self.assertEqual(1, self._value("SELECT allocator_eligible FROM networks WHERE id=%s", (first_id,)))
        self.assertEqual("test-operator", self._value(
            "SELECT allocator_approved_by FROM networks WHERE id=%s", (first_id,)))
        self.assertEqual(2, self._value("SELECT COUNT(*) FROM networks WHERE id IN (%s,%s) AND deleted_at IS NULL",
                                        (first_id, second_id)))

        self.networks[0]["range_start"] = "198.51.100.101"
        self.networks[0]["range_end"] = "198.51.100.201"
        importer.apply_snapshots(self.connection, [self._snapshot()])
        self.assertEqual(0, self._value("SELECT allocator_eligible FROM networks WHERE id=%s", (first_id,)))
        self.assertIsNone(self._value("SELECT allocator_approved_by FROM networks WHERE id=%s", (first_id,)))
        self.assertIsNone(self._value("SELECT allocator_approved_at FROM networks WHERE id=%s", (first_id,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM dhcp_ranges WHERE id=%s AND deleted_at IS NULL", (dhcp_id,)))

        cur.execute("UPDATE networks SET allocator_eligible=1,allocator_approved_by='test-operator',"
                    "allocator_approved_at=UTC_TIMESTAMP() WHERE id=%s", (first_id,))
        self.connection.commit()
        self.networks[0]["gateway_ip"] = "198.51.100.2"
        self.networks[0]["cidr"] = "198.51.100.0/24"
        importer.apply_snapshots(self.connection, [self._snapshot()])
        self.assertEqual(0, self._value("SELECT allocator_eligible FROM networks WHERE id=%s", (first_id,)))
        self.assertIsNone(self._value("SELECT allocator_approved_by FROM networks WHERE id=%s", (first_id,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM subnets WHERE id=%s AND deleted_at IS NULL", (subnet_id,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM vlans WHERE id=%s AND deleted_at IS NULL", (vlan_id,)))

        importer.apply_snapshots(self.connection, [self._snapshot([self.networks[0]])])
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM networks WHERE id=%s AND deleted_at IS NOT NULL", (second_id,)))
        second_subnet = self._row("subnets", f"{self.site_id}:network-b:ipv4", live=False)
        second_dhcp = self._row("dhcp_ranges", f"{self.site_id}:network-b:ipv4:dhcp", live=False)
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM subnets WHERE id=%s AND deleted_at IS NOT NULL", (second_subnet,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM dhcp_ranges WHERE id=%s AND deleted_at IS NOT NULL", (second_dhcp,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM vlans WHERE id=%s AND deleted_at IS NOT NULL", (second_vlan_id,)))

    def test_new_overlapping_prefix_revokes_existing_network_approval(self):
        importer.apply_snapshots(self.connection, [self._snapshot()])
        network_a = self._row("networks", f"{self.site_id}:network-a")
        network_b = self._row("networks", f"{self.site_id}:network-b")
        cur = self.connection.cursor()
        cur.execute("UPDATE networks SET allocator_eligible=1,allocator_approved_by='test-operator',"
                    "allocator_approved_at=UTC_TIMESTAMP() WHERE id=%s", (network_b,))
        self.connection.commit()

        overlapping = dict(self.networks[0])
        overlapping.update({
            "cidr": "203.0.113.128/25", "gateway_ip": "203.0.113.129", "prefix_len": 25,
            "range_start": "203.0.113.130", "range_end": "203.0.113.140",
        })
        importer.apply_snapshots(self.connection, [self._snapshot([overlapping, self.networks[1]])])
        self.assertEqual(0, self._value("SELECT allocator_eligible FROM networks WHERE id=%s", (network_b,)))
        self.assertIsNone(self._value("SELECT allocator_approved_by FROM networks WHERE id=%s", (network_b,)))
        self.assertEqual(1, self._value("SELECT COUNT(*) FROM subnets WHERE network_id=%s "
                                       "AND cidr='203.0.113.128/25' AND deleted_at IS NULL", (network_a,)))

    def test_external_reference_cannot_take_over_another_sources_network(self):
        importer.apply_snapshots(self.connection, [self._snapshot([self.networks[0]])])
        imported_id = self._row("networks", f"{self.site_id}:network-a")
        cur = self.connection.cursor()
        cur.execute("INSERT INTO networks (name,purpose,source_id,asserted_kind) "
                    "VALUES (%s,'foreign mapped-row fixture',%s,'human')",
                    ("mapped-foreign-" + self.site_id, self.provisioner_id))
        foreign_id = cur.lastrowid
        self.foreign_networks.add(foreign_id)
        cur.execute("UPDATE external_refs SET subject_id=%s WHERE source_id=%s "
                    "AND subject_table='networks' AND external_id=%s AND deleted_at IS NULL",
                    (foreign_id, self.source_id, f"{self.site_id}:network-a"))
        self.connection.commit()
        with self.assertRaisesRegex(importer.ImportConflict, "different SoR source"):
            importer.apply_snapshots(self.connection, [self._snapshot([self.networks[0]])])
        self.assertEqual(self.provisioner_id,
                         self._value("SELECT source_id FROM networks WHERE id=%s", (foreign_id,)))

        cur.execute("UPDATE external_refs SET subject_id=%s WHERE source_id=%s "
                    "AND subject_table='networks' AND external_id=%s AND deleted_at IS NULL",
                    (imported_id, self.source_id, f"{self.site_id}:network-a"))
        self.connection.commit()

    def test_foreign_source_name_conflict_is_rejected_without_takeover(self):
        cur = self.connection.cursor()
        cur.execute("INSERT INTO networks (name,purpose,source_id,asserted_kind) "
                    "VALUES (%s,'foreign-source fixture',%s,'human')",
                    ("foreign-" + self.site_id, self.provisioner_id))
        foreign_id = cur.lastrowid
        self.foreign_networks.add(foreign_id)
        self.connection.commit()
        collision = importer.normalize_network(site_doc(self.site_id), network_doc(
            "foreign-net", "foreign-" + self.site_id, None, None, None, None))
        with self.assertRaises(importer.ImportConflict):
            importer.apply_snapshots(self.connection, [self._snapshot([collision])])
        self.assertEqual(self.provisioner_id,
                         self._value("SELECT source_id FROM networks WHERE id=%s", (foreign_id,)))
        self.assertEqual(0, self._value("SELECT COUNT(*) FROM external_refs WHERE source_id=%s AND external_id LIKE %s",
                                        (self.source_id, self.prefix)))

    def test_default_wide_dhcp_pool_blocks_allocation_until_operator_approval(self):
        record = importer.normalize_network(site_doc(self.site_id), {
            "id": "default-network",
            "name": "Default",
            "enabled": True,
            "isDefault": True,
            "management": "GATEWAY",
            "vlanId": 1,
            "ipv4Configuration": {
                "hostIpAddress": "10.0.0.1",
                "prefixLength": 8,
                "dhcpConfiguration": {
                    "mode": "SERVER",
                    "leaseTimeSeconds": 86400,
                    "ipAddressRange": {"start": "10.1.0.26", "stop": "10.7.161.117"},
                },
            },
        })
        importer.apply_snapshots(self.connection, [self._snapshot([record])])
        network_id = self._row("networks", f"{self.site_id}:default-network")
        subnet_id = self._row("subnets", f"{self.site_id}:default-network:ipv4")
        dhcp_id = self._row("dhcp_ranges", f"{self.site_id}:default-network:ipv4:dhcp")
        self.assertEqual(0, self._value(
            "SELECT allocator_eligible FROM networks WHERE id=%s", (network_id,)))
        self.assertIsNone(self._value(
            "SELECT allocator_approved_by FROM networks WHERE id=%s", (network_id,)))
        self.assertIsNone(self._value(
            "SELECT allocator_approved_at FROM networks WHERE id=%s", (network_id,)))
        self.assertEqual("10.0.0.0/8", self._value(
            "SELECT cidr FROM subnets WHERE id=%s", (subnet_id,)))
        cur = self.connection.cursor()
        cur.execute("SELECT CAST(range_start AS CHAR),CAST(range_end AS CHAR),kind "
                    "FROM dhcp_ranges WHERE id=%s", (dhcp_id,))
        start, end, kind = cur.fetchone()
        self.assertEqual("10.1.0.26", importer._stored_ip(start))
        self.assertEqual("10.7.161.117", importer._stored_ip(end))
        self.assertEqual("dynamic_pool", kind)

        cur.execute("SELECT id,network_id FROM functions WHERE slug='web' AND deleted_at IS NULL")
        function_id, previous_network_id = cur.fetchone()
        overlap_network_id = None
        overlap_subnet_id = None
        try:
            cur.execute("UPDATE functions SET network_id=%s WHERE id=%s", (network_id, function_id))
            cur.execute("SET SESSION TRANSACTION ISOLATION LEVEL READ COMMITTED")
            self.connection.commit()
            with self.assertRaisesRegex(LookupError, "not approved"):
                allocator.allocate(self.connection, "web", isPrimary=False)
            self.assertEqual(0, self._value(
                "SELECT COUNT(*) FROM ip_addresses WHERE subnet_id=%s AND deleted_at IS NULL",
                (subnet_id,)))

            cur.execute("UPDATE networks SET allocator_eligible=1,"
                        "allocator_approved_by='test-operator',"
                        "allocator_approved_at=UTC_TIMESTAMP() WHERE id=%s", (network_id,))
            self.connection.commit()
            address = allocator.allocate(self.connection, "web", isPrimary=False)
            self.assertEqual("10.0.0.2", address)
            address_value = int(ipaddress.ip_address(address))
            self.assertFalse(int(ipaddress.ip_address("10.1.0.26")) <= address_value
                             <= int(ipaddress.ip_address("10.7.161.117")))
            self.connection.commit()

            # Corrupt/stale approval state must still fail closed if a different
            # SoR writer introduces an overlapping live prefix without revoking
            # this approval. Normal UniFi imports clear the approval themselves.
            cur.execute("INSERT INTO networks (name,purpose,source_id,asserted_kind,allocator_eligible) "
                        "VALUES (%s,'allocator overlap guard fixture',%s,'human',1)",
                        ("alloc-overlap-" + uuid.uuid4().hex[:12], self.provisioner_id))
            overlap_network_id = cur.lastrowid
            cur.execute(
                "INSERT INTO subnets (network_id,cidr,net_address,prefix_len,gateway_ip,source_id,asserted_kind) "
                "VALUES (%s,'10.0.0.0/24',CAST('10.0.0.0' AS INET6),24,"
                "CAST('10.0.0.1' AS INET6),%s,'human')",
                (overlap_network_id, self.provisioner_id))
            overlap_subnet_id = cur.lastrowid
            self.connection.commit()
            with self.assertRaisesRegex(LookupError, "overlaps live IPv4 subnet"):
                allocator.allocate(self.connection, "web", isPrimary=False)
        finally:
            if overlap_subnet_id is not None:
                cur.execute("DELETE FROM subnets WHERE id=%s", (overlap_subnet_id,))
            if overlap_network_id is not None:
                cur.execute("DELETE FROM networks WHERE id=%s", (overlap_network_id,))
            cur.execute("UPDATE functions SET network_id=%s WHERE id=%s",
                        (previous_network_id, function_id))
            cur.execute("DELETE FROM ip_addresses WHERE subnet_id=%s", (subnet_id,))
            self.connection.commit()


if __name__ == "__main__":
    unittest.main()
