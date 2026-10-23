"""Config precedence tests for separate SoR worker identities."""

import configparser
import unittest
from unittest.mock import patch

from db_config import database_options
import sor_reconcile_discovery


class DatabaseOptionsTests(unittest.TestCase):
    def test_discovery_connection_uses_its_separate_identity(self):
        config = configparser.ConfigParser()
        config.read_string("""
[sor]
host = base-host
port = 3306
user = read-only
password = read-secret
name = sor
[sor_discovery]
host = discovery-host
port = 3308
user = discovery-writer
password = discovery-secret
name = discovery_db
""")
        with patch.object(sor_reconcile_discovery.pymysql, "connect") as connect:
            sor_reconcile_discovery.connect_sor(config)
        self.assertEqual("discovery-writer", connect.call_args.kwargs["user"])
        self.assertEqual("discovery-secret", connect.call_args.kwargs["password"])
        self.assertEqual("discovery-host", connect.call_args.kwargs["host"])

    def test_worker_section_overrides_shared_fallback(self):
        config = configparser.ConfigParser()
        config.read_string("""
[sor]
host = 10.1.0.200
port = 3306
user = legacy
password = legacy-secret
name = sor
[sor_emit]
host = emit-host
port = 3307
user = emitter
password = emitter-secret
name = sor_emit_db
""")
        self.assertEqual({"host": "emit-host", "port": 3307, "user": "emitter",
                          "password": "emitter-secret", "name": "sor_emit_db"},
                         database_options(config, "sor_emit"))

    def test_missing_worker_section_uses_legacy_values(self):
        config = configparser.ConfigParser()
        config.read_string("""
[sor]
user = legacy
password = legacy-secret
""")
        self.assertEqual("legacy", database_options(config, "sor_discovery")["user"])

    def test_missing_password_fails_closed(self):
        config = configparser.ConfigParser()
        config.read_string("[sor]\nuser = legacy\n")
        with self.assertRaisesRegex(ValueError, "database password is missing"):
            database_options(config, "sor_emit")

    def test_empty_worker_host_does_not_fall_back_silently(self):
        config = configparser.ConfigParser()
        config.read_string("""
[sor]
host = 10.1.0.200
user = legacy
password = legacy-secret
[sor_emit]
host =
""")
        with self.assertRaisesRegex(ValueError, "database host is missing"):
            database_options(config, "sor_emit")


if __name__ == "__main__":
    unittest.main()
