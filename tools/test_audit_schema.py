#!/usr/bin/env python3
"""Read-only schema audit normalization and reporting tests."""
from __future__ import annotations

import io
import pathlib
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import audit_schema


class SchemaAuditTests(unittest.TestCase):
    def test_snapshot_body_removes_only_generated_manifest(self) -> None:
        snapshot = (
            "-- GENERATED FILE: tools/export_schema.py\n"
            "--   001_base sha256=abc\n\n"
            "/*M!999999\\- enable sandbox mode */\n"
            "CREATE TABLE sample (id INT);\n"
        )
        self.assertEqual(
            "/*M!999999\\- enable sandbox mode */\nCREATE TABLE sample (id INT);\n",
            audit_schema.snapshotBody(snapshot),
        )

    def test_live_auto_increment_counter_is_ignored_but_column_is_preserved(self) -> None:
        sql = "CREATE TABLE sample (id INT NOT NULL AUTO_INCREMENT) AUTO_INCREMENT=912;\n"
        normalized = audit_schema.normalizeSchemaDdl(sql)
        self.assertIn("id INT NOT NULL AUTO_INCREMENT", normalized)
        self.assertNotIn("AUTO_INCREMENT=912", normalized)

    def test_mariadb_version_trigger_delimiter_linebreak_is_normalized(self) -> None:
        sql = "INSERT INTO t VALUES ('x') \n*/;;\n"
        self.assertEqual("INSERT INTO t VALUES ('x') */;;\n",
                         audit_schema.normalizeSchemaDdl(sql))

    def test_matching_audit_is_explicitly_limited_to_schema_shape(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            snapshot = root / "db" / "schema.sql"
            snapshot.parent.mkdir()
            snapshot.write_text("-- generated\nCREATE TABLE sample (id INT);\n",
                                encoding="utf-8")
            output = io.StringIO()
            args = SimpleNamespace(database="monitoring")
            with patch.object(audit_schema, "ROOT", root), \
                    patch.object(audit_schema, "liveDump",
                                 return_value="CREATE TABLE sample (id INT);\n"):
                self.assertTrue(audit_schema.audit(args, output))
            self.assertIn("does not establish migration history", output.getvalue())

    def test_drift_returns_false_and_a_unified_diff(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = pathlib.Path(temp_dir)
            snapshot = root / "db" / "schema.sql"
            snapshot.parent.mkdir()
            snapshot.write_text("-- generated\nCREATE TABLE expected (id INT);\n",
                                encoding="utf-8")
            output = io.StringIO()
            args = SimpleNamespace(database="monitoring")
            with patch.object(audit_schema, "ROOT", root), \
                    patch.object(audit_schema, "liveDump",
                                 return_value="CREATE TABLE actual (id INT);\n"):
                self.assertFalse(audit_schema.audit(args, output))
            self.assertIn("--- candidate-snapshot", output.getvalue())
            self.assertIn("+++ live-schema", output.getvalue())
            self.assertIn("DIFFERENT", output.getvalue())

    def test_dump_omits_runner_ledger_only_when_present(self) -> None:
        args = SimpleNamespace(defaultsFile=None, host=None, port=None, user=None,
                               dbName="solarinet")
        with patch.object(audit_schema, "runQuery", return_value="1") as query, \
                patch.object(audit_schema.subprocess, "run",
                             return_value=SimpleNamespace(returncode=0, stdout="SQL\n",
                                                          stderr="")) as run:
            self.assertEqual("SQL\n", audit_schema.liveDump(args))
        self.assertIn("TABLE_TYPE='BASE TABLE'", query.call_args.args[1])
        command = run.call_args.args[0]
        self.assertIn("--ignore-table=solarinet.solari_migration_ledger", command)
        self.assertIn("--routines", command)
        self.assertIn("--events", command)

    def test_dump_does_not_ignore_absent_migration_ledger(self) -> None:
        args = SimpleNamespace(defaultsFile=None, host=None, port=None, user=None,
                               dbName="solarinet")
        with patch.object(audit_schema, "runQuery", return_value="0"), \
                patch.object(audit_schema.subprocess, "run",
                             return_value=SimpleNamespace(returncode=0, stdout="SQL\n",
                                                          stderr="")) as run:
            audit_schema.liveDump(args)
        self.assertFalse(any(argument.startswith("--ignore-table=")
                             for argument in run.call_args.args[0]))


if __name__ == "__main__":
    unittest.main()
