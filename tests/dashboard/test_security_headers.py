#!/usr/bin/env python3
"""Guard the dashboard's shipped Apache response-header contract."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
VHOST = ROOT / "deploy/dashboard/apache-solarinet.conf"
RACKWIRE = ROOT / "dashboard/public/rackwire/index.html"


class DashboardSecurityHeaderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = VHOST.read_text(encoding="utf-8")

    def test_transport_and_browser_hardening_headers_are_present(self):
        expected = {
            "X-Content-Type-Options": "nosniff",
            "X-Frame-Options": "SAMEORIGIN",
            "Referrer-Policy": "no-referrer",
            "Strict-Transport-Security": "max-age=15552000",
            "Permissions-Policy": (
                "camera=(self), microphone=(), geolocation=(), payment=(), usb=()"
            ),
        }
        for name, value in expected.items():
            with self.subTest(header=name):
                line = rf'^\s*Header always set {re.escape(name)} "{re.escape(value)}"\s*$'
                self.assertRegex(self.config, re.compile(line, re.MULTILINE))

    def test_spa_csp_is_scoped_and_preserves_runtime_contract(self):
        match = re.search(
            r'<LocationMatch "([^"]+)">\s*'
            r'Header always set Content-Security-Policy "([^"]+)"\s*'
            r'</LocationMatch>',
            self.config,
        )
        self.assertIsNotNone(match)
        self.assertEqual(match.group(1), r"^/(?:index\.html)?$")
        directives = {
            item.split(None, 1)[0]: item.split(None, 1)[1]
            for item in match.group(2).split(";")
        }
        self.assertEqual(directives["default-src"], "'self'")
        self.assertEqual(directives["object-src"], "'none'")
        self.assertIn("'self'", directives["script-src"])
        self.assertIn("'unsafe-eval'", directives["script-src"])
        self.assertNotIn("'unsafe-inline'", directives["script-src"])
        self.assertIn("'unsafe-inline'", directives["style-src"])
        self.assertIn("frame-ancestors", directives)
        self.assertIn("connect-src", directives)

    def test_permissions_policy_matches_rackwire_camera_feature(self):
        app = RACKWIRE.read_text(encoding="utf-8")
        self.assertIn("navigator.mediaDevices.getUserMedia", app)
        self.assertIn("camera=(self)", self.config)
        self.assertIn("microphone=()", self.config)
        self.assertIn("geolocation=()", self.config)


if __name__ == "__main__":
    unittest.main()
