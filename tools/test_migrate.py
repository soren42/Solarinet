#!/usr/bin/env python3
"""Safety regression tests for migration history preflight."""
from __future__ import annotations

import hashlib
import pathlib
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import migrate


class MigrationPreflightTests(unittest.TestCase):
    def test_monitoring_stream_includes_crl_ack_node_cascade(self) -> None:
        entries = migrate.migrationFiles("monitoring")
        ids = [migration_id for migration_id, _path, _checksum in entries]
        self.assertIn("035_certificate_crl_acknowledgements", ids)
        self.assertIn("036_crl_acknowledgement_node_cascade", ids)
        self.assertLess(ids.index("035_certificate_crl_acknowledgements"),
                        ids.index("036_crl_acknowledgement_node_cascade"))
        migration = next(path.read_text(encoding="utf-8")
                         for migration_id, path, _checksum in entries
                         if migration_id == "036_crl_acknowledgement_node_cascade")
        self.assertIn("DROP FOREIGN KEY fk_certificateCrlAck_node", migration)
        self.assertIn("REFERENCES node (nodeId) ON DELETE CASCADE", migration)
        checksum = next(checksum for migration_id, _path, checksum in entries
                        if migration_id == "036_crl_acknowledgement_node_cascade")
        snapshot = (migrate.ROOT / "db" / "schema.sql").read_text(encoding="utf-8")
        self.assertIn(f"036_crl_acknowledgement_node_cascade sha256={checksum}", snapshot)
        self.assertIn("FOREIGN KEY (`nodeId`) REFERENCES `node` (`nodeId`) ON DELETE CASCADE",
                      snapshot)

    def test_sor_stream_owns_transactional_outbox_and_cdc_triggers(self) -> None:
        entries = migrate.migrationFiles("sor")
        ids = [migration_id for migration_id, _path, _checksum in entries]
        self.assertIn("022_transactional_outbox", ids)
        self.assertIn("023_sor_cdc_triggers", ids)
        self.assertLess(ids.index("022_transactional_outbox"),
                        ids.index("023_sor_cdc_triggers"))
        outbox = next(path.read_text(encoding="utf-8") for migration_id, path, _ in entries
                      if migration_id == "022_transactional_outbox")
        triggers = next(path.read_text(encoding="utf-8") for migration_id, path, _ in entries
                        if migration_id == "023_sor_cdc_triggers")
        self.assertIn("CREATE TABLE IF NOT EXISTS sor_outbox", outbox)
        self.assertIn("CREATE TABLE IF NOT EXISTS sor_sync_state", outbox)
        self.assertEqual(triggers.count("CREATE TRIGGER"), 21)

    def makeFiles(self, tempDir):
        paths = []
        for migrationId in ("001_first", "002_second", "003_third"):
            path = pathlib.Path(tempDir) / (migrationId + ".sql")
            path.write_text("SELECT 1;", encoding="utf-8")
            paths.append((migrationId, path,
                          hashlib.sha256(path.read_bytes()).hexdigest()))
        return paths

    def test_out_of_order_history_refuses_before_any_schema_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)

            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    return "solari_migration_ledger\n"
                if "SELECT migration_id,checksum,state" in sql:
                    # Migration 002 is present while 001 is absent. The runner
                    # must detect this before creating a ledger or applying 001.
                    return f"002_second\t{paths[1][2]}\tapplied\n"
                raise AssertionError("preflight issued a mutating SQL statement")

            args = SimpleNamespace(database="monitoring", check=False)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(RuntimeError, "out-of-order"):
                    migrate.migrate(args)

            self.assertEqual(3, len(calls))
            self.assertTrue(all("SELECT" in sql for sql in calls))

    def assertLedgerRefusedWithoutMutation(self, ledgerRow, message):
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)
            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    return "solari_migration_ledger\n"
                if "SELECT migration_id,checksum,state" in sql:
                    return ledgerRow(paths)
                raise AssertionError("invalid history triggered a mutation")

            args = SimpleNamespace(database="monitoring", check=False)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(RuntimeError, message):
                    migrate.migrate(args)
            self.assertEqual(3, len(calls))

    def test_checksum_drift_refuses_before_mutation(self) -> None:
        self.assertLedgerRefusedWithoutMutation(
            lambda paths: f"001_first\t{'0' * 64}\tapplied\n",
            "checksum mismatch")

    def test_interrupted_migration_refuses_before_mutation(self) -> None:
        self.assertLedgerRefusedWithoutMutation(
            lambda paths: f"001_first\t{paths[0][2]}\tapplying\n",
            "checksum mismatch or interrupted migration")

    def test_untracked_view_only_database_refuses_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)
            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    if "TABLE_TYPE" in sql:
                        return ""
                    return "legacy_schema_view\n"
                raise AssertionError("untracked database triggered a mutation")

            args = SimpleNamespace(database="monitoring", check=False)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(RuntimeError, "untracked nonempty database"):
                    migrate.migrate(args)
            self.assertEqual(2, len(calls))

    def test_noncanonical_database_collation_refuses_before_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)
            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_uca1400_ai_ci\n"
                raise AssertionError("collation mismatch triggered more SQL")

            args = SimpleNamespace(database="monitoring", check=False)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(RuntimeError, "expected utf8mb4/utf8mb4_unicode_ci"):
                    migrate.migrate(args)
            self.assertEqual(1, len(calls))

    def test_database_defaults_are_read_before_inventory(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)
            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    return ""
                if sql.startswith("CREATE TABLE"):
                    raise AssertionError("check mode must not mutate")
                return ""

            args = SimpleNamespace(database="monitoring", check=True)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                migrate.migrate(args)
            self.assertIn("information_schema.SCHEMATA", calls[0])
            self.assertIn("information_schema.TABLES", calls[1])

    def test_verify_live_rejects_pending_migrations(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)
            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    return "solari_migration_ledger\n"
                if "SELECT migration_id,checksum,state" in sql:
                    return f"001_first\t{paths[0][2]}\tapplied\n"
                raise AssertionError("verification must not mutate the database")

            args = SimpleNamespace(database="monitoring", check=True, verifyLive=True)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(
                        RuntimeError, "live ledger has pending migrations: 002_second, 003_third"):
                    migrate.migrate(args)

            self.assertEqual(3, len(calls))
            self.assertTrue(all(sql.startswith("SELECT") for sql in calls))

    def test_verify_live_requires_existing_ledger(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)

            def fakeMysqlCommand(_args, sql):
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    return ""
                raise AssertionError("verification must not mutate the database")

            args = SimpleNamespace(database="monitoring", check=True, verifyLive=True)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(RuntimeError, "requires an existing migration ledger"):
                    migrate.migrate(args)

    def test_untracked_routine_and_event_only_database_refuses_before_mutation(self) -> None:
        with tempfile.TemporaryDirectory() as tempDir:
            paths = self.makeFiles(tempDir)
            calls = []

            def fakeMysqlCommand(_args, sql):
                calls.append(sql)
                if "information_schema.SCHEMATA" in sql:
                    return "utf8mb4\tutf8mb4_unicode_ci\n"
                if "information_schema.TABLES" in sql:
                    self.assertIn("information_schema.ROUTINES", sql)
                    self.assertIn("information_schema.EVENTS", sql)
                    return "legacy_function\nlegacy_event\n"
                raise AssertionError("untracked database triggered a mutation")

            args = SimpleNamespace(database="monitoring", check=False)
            with patch.object(migrate, "migrationFiles", return_value=paths), \
                    patch.object(migrate, "mysqlCommand", side_effect=fakeMysqlCommand):
                with self.assertRaisesRegex(RuntimeError, "untracked nonempty database"):
                    migrate.migrate(args)
            self.assertEqual(2, len(calls))


if __name__ == "__main__":
    unittest.main()
