"""Node OS/architecture reconciliation is isolated from production databases."""
import configparser
import pathlib
import sys
import unittest
from unittest import mock

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import sor_reconcile_discovery as reconcile_module


class Cursor:
    def __init__(self, rows=()):
        self.rows = list(rows)
        self.sql = []
        self.lastrowid = 41
        self.rowcount = 0
        self.existing_os_id = None
        self.os_row = None
        self.node_arch = "arm64"

    def execute(self, query, params=None):
        self.sql.append((query, params))
        self.rowcount = 1 if query.startswith("UPDATE hardware_models") else 0
        if query.startswith("SELECT discId"):
            self.rows = []
        elif query.startswith("SELECT hostFqdn"):
            self.rows = [{"hostFqdn": "alpha.akoria.net", "osName": "Debian 13", "arch": self.node_arch}]
        elif query.startswith("SELECT id FROM sources"):
            self.rows = [{"id": 7}]
        elif query.startswith("SELECT id FROM operating_systems"):
            self.rows = [self.os_row] if self.os_row else []
        elif query.startswith("SELECT e.id"):
            self.rows = [{"id": 99, "os_id": self.existing_os_id}]
        else:
            self.rows = []

    def fetchall(self):
        return self.rows

    def fetchone(self):
        return self.rows[0] if self.rows else None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        pass


class Connection:
    def __init__(self, cursor):
        self._cursor = cursor
        self.commits = self.rollbacks = 0

    def cursor(self):
        return self._cursor

    def commit(self):
        self.commits += 1

    def rollback(self):
        self.rollbacks += 1

    def close(self):
        pass


class NodeFactsReconciliationTest(unittest.TestCase):
    def setUp(self):
        self.mon_cursor, self.sor_cursor = Cursor(), Cursor()
        self.mon, self.sor = Connection(self.mon_cursor), Connection(self.sor_cursor)
        config = configparser.ConfigParser()
        config["discovery"] = {"mon_user": "test", "mon_password": "test"}
        self.config = config

    def invoke(self, dry_run=False):
        with mock.patch.object(reconcile_module.pymysql, "connect", side_effect=[self.mon, self.sor]), \
             mock.patch.object(reconcile_module, "connect_sor", return_value=self.sor), \
             mock.patch.object(reconcile_module, "log"):
            reconcile_module.reconcile(self.config, dry_run)

    def test_enrolled_node_facts_update_os_and_null_model_arch(self):
        self.invoke()
        statements = [sql for sql, _ in self.sor_cursor.sql]
        self.assertTrue(any(sql.startswith("INSERT INTO operating_systems") for sql in statements))
        self.assertTrue(any(sql.startswith("UPDATE entities SET os_id") for sql in statements))
        self.assertTrue(any("SET hm.arch=%s WHERE e.id=%s AND hm.arch IS NULL" in sql
                            for sql in statements))
        self.assertEqual(1, self.sor.commits)

    def test_dry_run_does_not_write_or_commit(self):
        self.invoke(dry_run=True)
        self.assertFalse(any(sql.startswith(("INSERT", "UPDATE")) for sql, _ in self.sor_cursor.sql))
        self.assertEqual(0, self.sor.commits)
        self.assertEqual(1, self.sor.rollbacks)

    def test_arch_only_change_emits_one_entity_cdc_update(self):
        self.sor_cursor.existing_os_id = 41
        self.invoke()
        statements = [sql for sql, _ in self.sor_cursor.sql]
        self.assertFalse(any(sql.startswith("UPDATE entities SET os_id") for sql in statements))
        self.assertTrue(any(sql.startswith("UPDATE entities SET asserted_at") for sql in statements))

    def test_existing_os_row_is_reused_not_duplicated(self):
        self.sor_cursor.os_row = {"id": 5}
        self.sor_cursor.existing_os_id = 5
        self.invoke()
        statements = [sql for sql, _ in self.sor_cursor.sql]
        self.assertFalse(any(sql.startswith("INSERT INTO operating_systems") for sql in statements))
        self.assertFalse(any(sql.startswith("UPDATE entities SET os_id") for sql in statements))

    def test_new_os_row_gets_linux_family(self):
        self.invoke()
        insert = [p for sql, p in self.sor_cursor.sql if sql.startswith("INSERT INTO operating_systems")]
        self.assertEqual("linux", insert[0][0])

    def test_uname_arch_is_normalized(self):
        self.sor_cursor.node_arch = "aarch64"
        self.invoke()
        params = [p for sql, p in self.sor_cursor.sql if sql.startswith("UPDATE hardware_models")]
        self.assertEqual("arm64", params[0][0])


class DiscoveryCursor(Cursor):
    """Monitoring/SoR double for the adopted-discovery insertion path."""

    def __init__(self):
        super().__init__()
        self.ip_row = None
        self.name_taken = False

    def execute(self, query, params=None):
        self.sql.append((query, params))
        self.rowcount = 0
        if query.startswith("SELECT discId"):
            self.rows = [{"discId": 81, "ip": "10.0.0.254", "host": None,
                          "mdnsName": "hole.local", "deviceRole": None}]
        elif query.startswith("SELECT id FROM sources"):
            self.rows = [{"id": 7}]
        elif query.startswith("SELECT entity_id, deleted_at FROM ip_addresses"):
            self.rows = [self.ip_row] if self.ip_row else []
        elif query.startswith("SELECT 1 FROM entities"):
            self.rows = [{"1": 1}] if self.name_taken else []
        else:
            self.rows = []


class DiscoveryAdoptionTest(unittest.TestCase):
    def setUp(self):
        self.mon_cursor, self.sor_cursor = DiscoveryCursor(), DiscoveryCursor()
        self.mon, self.sor = Connection(self.mon_cursor), Connection(self.sor_cursor)
        config = configparser.ConfigParser()
        config["discovery"] = {"mon_user": "test", "mon_password": "test"}
        self.config = config

    invoke = NodeFactsReconciliationTest.invoke

    def inserts(self):
        return [sql for sql, _ in self.sor_cursor.sql if sql.startswith("INSERT INTO entities")]

    def test_free_ip_and_name_is_adopted(self):
        self.invoke()
        self.assertEqual(1, len(self.inserts()))

    def test_tombstoned_ip_is_not_resurrected(self):
        self.sor_cursor.ip_row = {"entity_id": 21, "deleted_at": "2026-11-02 17:51:10"}
        self.invoke()
        self.assertEqual([], self.inserts())

    def test_name_colliding_with_host_or_alias_is_not_adopted(self):
        self.sor_cursor.name_taken = True
        self.invoke()
        self.assertEqual([], self.inserts())


if __name__ == "__main__":
    unittest.main()
