"""Regression checks for the Opie systemd isolation contract."""
from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent


class ServiceIsolationTests(unittest.TestCase):
    def test_service_has_dedicated_identity_and_hardening(self):
        unit = (ROOT / "opied.service").read_text()
        self.assertIn("User=solari-opie", unit)
        self.assertIn("Group=solari-opie", unit)
        self.assertIn("ProtectSystem=strict", unit)
        self.assertIn("ProtectHome=true", unit)
        self.assertIn("NoNewPrivileges=true", unit)
        self.assertIn("Environment=PATH=/usr/local/bin:/usr/bin:/bin", unit)
        self.assertIn("PrivateTmp=true", unit)
        self.assertIn("CapabilityBoundingSet=", unit)
        self.assertNotIn("/home/jason", unit)
        self.assertNotIn("run/db.env", unit)

    def test_example_uses_restricted_account_and_private_state(self):
        config = (ROOT / "opie.conf.example").read_text()
        self.assertIn("user = solari-opie", config)
        self.assertIn("file = /var/lib/solari-opie/opie.state.json", config)
        self.assertNotIn("user = solari\n", config)

    def test_grants_are_column_scoped_and_do_not_allow_deletion_or_ddl(self):
        grants = (ROOT / "grants.sql").read_text().upper()
        self.assertIn("REVOKE ALL PRIVILEGES, GRANT OPTION", grants)
        self.assertIn("GRANT SELECT (EVENTID, RULEID, NODEID, SEVERITY, FIREDAT, DETAIL)", grants)
        self.assertIn("INSERT (INCIDENTKEY, TRIGGERKIND, FIRSTEVENTID, NODEID, HOSTFQDN, SEVERITY, STATUS)", grants)
        self.assertIn("UPDATE (STATUS, SUMMARY, ANALYSIS, MODEL, DURATIONSEC, FINISHEDAT)", grants)
        self.assertNotIn("DELETE", grants)
        self.assertNotIn("ALTER", grants)
        self.assertNotIn("GRANT ALL", grants)


if __name__ == "__main__":
    unittest.main()
