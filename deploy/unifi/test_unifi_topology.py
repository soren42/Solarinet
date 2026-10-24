#!/usr/bin/env python3
import io
import os
import types
import unittest
from contextlib import redirect_stderr, redirect_stdout
from datetime import datetime
from pathlib import Path
import sys
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
import unifi_topology as topology

SITE_A = "88f7af54-98f8-306a-a1c7-c9349722b1f6"
SITE_B = "ad08da81-539b-4f86-8b41-5898e33a169d"
NETWORK_A = "edda7ac9-4f4e-475c-bd77-8f24c1037cd3"
NETWORK_B = "d2ed20b4-a6ee-4c21-9278-eaab4e1e875e"


class FakeClient:
    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def get(self, path):
        self.calls.append(path)
        key = path.split("?", 1)[0]
        value = self.responses[key]
        return value.pop(0) if isinstance(value, list) else value


def site():
    return {"id": SITE_A, "name": "Default"}


def summary(network_id=NETWORK_A):
    return {"id": network_id, "name": "Default", "vlanId": 1}


def detail(network_id=NETWORK_A, cidr="10.0.0.1/8", start="10.1.0.26", stop="10.7.161.117"):
    host, prefix = cidr.split("/")
    return {"id": network_id, "name": "Default", "vlanId": 1, "enabled": True,
            "management": "GATEWAY", "ipv4Configuration": {"hostIpAddress": host,
            "prefixLength": int(prefix), "dhcpConfiguration": {"mode": "SERVER",
            "ipAddressRange": {"start": start, "stop": stop}}}}


class TopologyTests(unittest.TestCase):
    def test_cli_requires_trust_or_explicit_insecure_opt_in(self):
        with patch.dict(os.environ, {"UNIFI_GW": "https://controller.invalid",
                                     "UNIFI_API_KEY": "fixture-key"}, clear=True):
            with self.assertRaises(SystemExit) as error:
                topology.main([])
        self.assertEqual(2, error.exception.code)

    def test_cli_warns_when_insecure_tls_is_explicitly_enabled(self):
        stderr, stdout = io.StringIO(), io.StringIO()
        with patch.dict(os.environ, {"UNIFI_GW": "https://controller.invalid",
                                     "UNIFI_API_KEY": "fixture-key"}, clear=True), \
             patch.object(topology.unifipolld, "UniFiClient") as client, \
             patch.object(topology, "collect_topology", return_value={"sites": []}), \
             redirect_stderr(stderr), redirect_stdout(stdout):
            self.assertEqual(0, topology.main(["--allow-insecure-tls"]))
        self.assertIn("certificate verification is disabled", stderr.getvalue())
        self.assertIn("API key is sent over an unverified connection", stderr.getvalue())
        self.assertNotIn("fixture-key", stderr.getvalue())
        client.assert_called_once_with("https://controller.invalid", "fixture-key", None, connect_ip=None)

    def test_cli_verified_ca_does_not_emit_insecure_warning(self):
        stderr = io.StringIO()
        with patch.dict(os.environ, {"UNIFI_GW": "https://controller.invalid",
                                     "UNIFI_API_KEY": "fixture-key",
                                     "UNIFI_CA_FILE": "/run/secrets/unifi-ca.pem"}, clear=True), \
             patch.object(topology.unifipolld, "UniFiClient"), \
             patch.object(topology, "collect_topology", return_value={"sites": []}), \
             redirect_stderr(stderr), redirect_stdout(io.StringIO()):
            self.assertEqual(0, topology.main([]))
        self.assertNotIn("certificate verification is disabled", stderr.getvalue())

    def test_cli_passes_connection_ip_without_replacing_gateway_name(self):
        with patch.dict(os.environ, {"UNIFI_GW": "https://controller.invalid",
                                     "UNIFI_API_KEY": "fixture-key",
                                     "UNIFI_CA_FILE": "/run/secrets/unifi-ca.pem",
                                     "UNIFI_CONNECT_IP": "10.0.0.1"}, clear=True), \
             patch.object(topology.unifipolld, "UniFiClient") as client, \
             patch.object(topology, "collect_topology", return_value={"sites": []}), \
             redirect_stderr(io.StringIO()), redirect_stdout(io.StringIO()):
            self.assertEqual(0, topology.main([]))
        client.assert_called_once_with("https://controller.invalid", "fixture-key",
                                       "/run/secrets/unifi-ca.pem", connect_ip="10.0.0.1")

    def test_reads_site_network_pages_and_network_details(self):
        client = FakeClient({
            "/proxy/network/integration/v1/sites": {"count": 1, "data": [site()], "limit": 100, "offset": 0, "totalCount": 1},
            f"/proxy/network/integration/v1/sites/{SITE_A}/networks": {"count": 1, "data": [summary()], "limit": 100, "offset": 0, "totalCount": 1},
            f"/proxy/network/integration/v1/sites/{SITE_A}/networks/{NETWORK_A}": detail(),
        })
        result = topology.collect_topology(client)
        network = result["sites"][0]["networks"][0]
        self.assertTrue(result["collected_at"].endswith("Z"))
        datetime.fromisoformat(result["collected_at"].replace("Z", "+00:00"))
        self.assertEqual("10.0.0.0/8", network["subnet"])
        self.assertEqual("10.0.0.1", network["gateway_ip"])
        self.assertEqual({"mode": "SERVER", "start": "10.1.0.26", "stop": "10.7.161.117"}, network["dhcp"])
        self.assertEqual(3, len(client.calls))

    def test_paginates_all_results(self):
        class PagingClient:
            def get(self, path):
                if path.endswith("/sites?offset=0&limit=100"):
                    return {"count": 1, "data": [{"id": SITE_A, "name": "one"}], "limit": 100, "offset": 0, "totalCount": 2}
                if path.endswith("/sites?offset=1&limit=100"):
                    return {"count": 1, "data": [{"id": SITE_B, "name": "two"}], "limit": 100, "offset": 1, "totalCount": 2}
                raise AssertionError(path)
        self.assertEqual(2, len(topology.list_all(PagingClient(), "/sites", "sites")))

    def test_gateway_url_requires_https_and_uuid_ids_are_canonicalized(self):
        self.assertEqual("https://controller.local", topology.validate_gateway_url("https://controller.local/"))
        for url in ("http://controller.local", "https://user:pass@controller.local", "https://controller.local/path?q=1"):
            with self.assertRaises(topology.TopologyError):
                topology.validate_gateway_url(url)
        self.assertEqual(SITE_A, topology.validate_uuid(SITE_A.upper(), "site"))

    def test_rejects_partial_pages_and_multi_site_ambiguity(self):
        class EmptyPageClient:
            def get(self, path): return {"count": 0, "data": [], "limit": 100, "offset": 0, "totalCount": 1}
        with self.assertRaisesRegex(topology.TopologyError, "stopped before"):
            topology.list_all(EmptyPageClient(), "/sites", "sites")
        client = FakeClient({"/proxy/network/integration/v1/sites": {"count": 2, "data": [site(), {"id": SITE_B}], "limit": 100, "offset": 0, "totalCount": 2}})
        with self.assertRaisesRegex(topology.TopologyError, "UNIFI_SITE_ID"):
            topology.collect_topology(client)

    def test_rejects_repeated_rows_and_bad_pagination_metadata(self):
        class RepeatedPageClient:
            def get(self, path):
                offset = 0 if "offset=0" in path else 1
                return {"count": 1, "data": [{"id": SITE_A}], "limit": 1,
                        "offset": offset, "totalCount": 2}
        with self.assertRaisesRegex(topology.TopologyError, "duplicate ID"):
            topology.list_all(RepeatedPageClient(), "/sites", "sites", page_size=1)
        class OversizedPageClient:
            def get(self, path):
                return {"count": 2, "data": [{"id": SITE_A}, {"id": SITE_B}], "limit": 1,
                        "offset": 0, "totalCount": 2}
        with self.assertRaisesRegex(topology.TopologyError, "pagination metadata"):
            topology.list_all(OversizedPageClient(), "/sites", "sites", page_size=1)

    def test_rejects_bad_dhcp_range_and_detail_id(self):
        with self.assertRaisesRegex(topology.TopologyError, "outside or conflicts"):
            topology.normalise_network(site(), summary(), detail(start="10.0.0.1", stop="10.2.0.1"))
        with self.assertRaisesRegex(topology.TopologyError, "outside or conflicts"):
            topology.normalise_network(site(), summary(), detail(start="10.0.0.0", stop="10.0.0.2"))
        with self.assertRaisesRegex(topology.TopologyError, "detail id mismatch"):
            topology.normalise_network(site(), summary(), detail(NETWORK_B))
        with self.assertRaisesRegex(topology.TopologyError, "outside or conflicts"):
            topology.normalise_network(site(), summary(), detail(cidr="10.0.0.3/8", start="10.0.0.2", stop="10.0.0.4"))

    def test_rejects_incomplete_network_metadata_and_empty_inventory(self):
        incomplete = detail()
        incomplete.pop("enabled")
        with self.assertRaisesRegex(topology.TopologyError, "enabled state"):
            topology.normalise_network(site(), summary(), incomplete)
        client = FakeClient({
            "/proxy/network/integration/v1/sites": {"count": 1, "data": [site()], "limit": 100, "offset": 0, "totalCount": 1},
            f"/proxy/network/integration/v1/sites/{SITE_A}/networks": {"count": 0, "data": [], "limit": 100, "offset": 0, "totalCount": 0},
        })
        with self.assertRaisesRegex(topology.TopologyError, "no networks"):
            topology.collect_topology(client)
        invalid_mode = detail()
        invalid_mode["ipv4Configuration"]["dhcpConfiguration"]["mode"] = "NONE"
        with self.assertRaisesRegex(topology.TopologyError, "unsupported DHCP mode"):
            topology.normalise_network(site(), summary(), invalid_mode)
        relay = detail()
        relay["ipv4Configuration"]["dhcpConfiguration"] = {"mode": "RELAY", "serverIpAddress": "10.0.0.2"}
        with self.assertRaisesRegex(topology.TopologyError, "DHCP relay"):
            topology.normalise_network(site(), summary(), relay)

    def test_reports_equal_and_overlapping_sor_prefixes(self):
        snapshot = {"sites": [{"name": "Default", "networks": [{"name": "Default", "subnet": "10.0.0.0/8"}]}]}
        conflicts = topology.subnet_conflicts(snapshot, [{"cidr": "10.1.0.0/24"}, {"cidr": "192.168.0.0/24"}])
        self.assertEqual(["overlap"], [row["relation"] for row in conflicts])

    def test_reports_internal_source_overlaps(self):
        snapshot = {"sites": [{"name": "Default", "networks": [
            {"name": "A", "subnet": "10.0.0.0/8"},
            {"name": "B", "subnet": "10.1.0.0/16"},
        ]}]}
        self.assertEqual("overlap", topology.internal_subnet_conflicts(snapshot)[0]["relation"])

    def test_sor_comparison_blocks_wide_source_prefix_and_dhcp_overlap(self):
        snapshot = {"sites": [{"name": "Default", "networks": [{
            "id": NETWORK_A, "name": "Default", "vlan_tag": 1,
            "subnet": "10.0.0.0/8", "gateway_ip": "10.0.0.1",
            "dhcp": {"mode": "SERVER", "start": "10.1.0.26", "stop": "10.7.161.117"},
        }]}]}
        subnets = [{"id": n, "cidr": f"10.{n}.0.0/24", "network_id": n,
                    "network_name": f"segment-{n}", "gateway_ip": f"10.{n}.0.1"} for n in range(1, 17)]
        sor = {"vlans": [], "networks": [], "subnets": subnets,
               "dhcp_ranges": [{"id": 5, "subnet_id": 1, "range_start": "10.1.0.100",
                                "range_end": "10.1.0.120", "kind": "dynamic_pool"}],
               "external_refs": []}
        first = topology.compare_sor(snapshot, sor)
        second = topology.compare_sor(snapshot, sor)
        self.assertEqual(first, second, "comparison output is stable across repeated planning")
        self.assertFalse(first["write_performed"])
        row = first["networks"][0]
        self.assertEqual("review_required", row["status"])
        self.assertEqual(16, len(row["overlapping_sor_subnets"]))
        self.assertTrue(all(item["relation"] == "overlap" for item in row["overlapping_sor_subnets"]))
        self.assertEqual("segment-1", row["overlapping_sor_subnets"][0]["network_name"])
        self.assertEqual(["overlap"], [item["relation"] for item in row["overlapping_sor_dhcp_ranges"]])
        self.assertIn("source_prefix_overlaps_sor_subnets", row["review_reasons"])
        self.assertIn("no_exact_sor_subnet", row["review_reasons"])
        self.assertEqual(16, len(sor["subnets"]), "planning must preserve all existing subnets")

    def test_sor_comparison_requires_exact_correlation_and_checks_gateway_dhcp_vlan(self):
        snapshot = {"sites": [{"name": "Default", "networks": [{
            "id": NETWORK_A, "name": "lan", "vlan_tag": 10,
            "subnet": "192.0.2.0/24", "gateway_ip": "192.0.2.1",
            "dhcp": {"mode": "SERVER", "start": "192.0.2.50", "stop": "192.0.2.100"},
        }]}]}
        sor = {"vlans": [{"id": 2, "vlan_tag": 10, "name": "lan"}],
               "networks": [{"id": 7, "name": "lan", "vlan_id": 3}],
               "subnets": [{"id": 9, "cidr": "192.0.2.0/24", "network_id": 7,
                            "gateway_ip": "192.0.2.254"}],
               "dhcp_ranges": [{"id": 11, "subnet_id": 9, "range_start": "192.0.2.60",
                                "range_end": "192.0.2.100", "kind": "dynamic_pool"}],
               "external_refs": [{"subject_table": "networks", "subject_id": 7,
                                  "external_id": NETWORK_A}]}
        row = topology.compare_sor(snapshot, sor)["networks"][0]
        self.assertEqual("review_required", row["status"])
        self.assertIn("network_vlan_assignment_mismatch", row["review_reasons"])
        self.assertIn("gateway_mismatch", row["review_reasons"])
        self.assertIn("dhcp_pool_overlaps_sor_range", row["review_reasons"])
        self.assertIn("dhcp_range_mismatch", row["review_reasons"])
        self.assertNotIn("network_external_ref_missing", row["review_reasons"])
        snapshot["source_subnet_conflicts"] = [{"relation": "overlap"}]
        self.assertIn("source_snapshot_has_internal_overlap",
                      topology.compare_sor(snapshot, sor)["networks"][0]["review_reasons"])

    def test_sor_reader_uses_read_only_transaction_and_selects_only(self):
        class Cursor:
            def __init__(self): self.statements = []; self.rows = []
            def __enter__(self): return self
            def __exit__(self, *args): return False
            def execute(self, statement):
                self.statements.append(statement)
                self.rows = ([{"id": 2}] if "FROM vlans " in statement else
                             [{"id": 3}] if "FROM networks " in statement else
                             [{"id": 4, "cidr": "192.0.2.0/24"}] if "FROM subnets " in statement else
                             [{"id": 5}] if "FROM dhcp_ranges " in statement else
                             [{"subject_table": "networks"}] if "FROM external_refs " in statement else [])
            def fetchall(self): return self.rows

        class Connection:
            def __init__(self): self.cursor_value = Cursor(); self.began = False; self.committed = False; self.rolled_back = False; self.closed = False
            def cursor(self, *_): return self.cursor_value
            def begin(self): self.began = True
            def commit(self): self.committed = True
            def rollback(self): self.rolled_back = True
            def close(self): self.closed = True

        connection = Connection()
        captured = {}
        def connect(**kwargs):
            captured.update(kwargs)
            return connection
        fake_pymysql = types.SimpleNamespace(connect=connect,
                                             cursors=types.SimpleNamespace(DictCursor=object))

        with patch.dict(os.environ, {"SOR_DB_HOST": "127.0.0.1", "SOR_DB_USER": "fixture",
                                     "SOR_DB_PASSWORD": "fixture-only"}), \
             patch.object(topology.unifipolld, "pymysql", fake_pymysql):
            state = topology.load_sor_state()
        self.assertTrue(connection.began and connection.committed and connection.closed)
        self.assertFalse(connection.rolled_back)
        self.assertFalse(captured["autocommit"])
        self.assertEqual("127.0.0.1", captured["host"])
        self.assertEqual("fixture-only", captured["password"])
        statements = connection.cursor_value.statements
        self.assertEqual("SET TRANSACTION READ ONLY", statements[0])
        self.assertTrue(all(statement.startswith(("SELECT", "SET TRANSACTION")) for statement in statements))
        self.assertEqual(1, len(state["subnets"]))


if __name__ == "__main__":
    unittest.main()
