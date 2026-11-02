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

    def execute(self, query, params=None):
        self.sql.append((query, params))
        self.rowcount = 1 if query.startswith("UPDATE hardware_models") else 0
        if query.startswith("SELECT discId"):
            self.rows = []
        elif query.startswith("SELECT hostFqdn"):
            self.rows = [{"hostFqdn": "alpha.akoria.net", "osName": "Debian 13", "arch": "arm64"}]
        elif query.startswith("SELECT id FROM sources"):
            self.rows = [{"id": 7}]
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


if __name__ == "__main__":
    unittest.main()
