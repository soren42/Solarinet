#!/usr/bin/env python3
"""Keep the hardened PHP-FPM pool compatible with dashboard integrations."""

import re
import unittest
from pathlib import Path, PurePosixPath


ROOT = Path(__file__).resolve().parents[2]
POOL = ROOT / "deploy/dashboard/hardened/php-fpm-solarinet.conf"
TMPFILES = ROOT / "deploy/dashboard/hardened/solarinet-dashboard.tmpfiles.conf"
SOR_GRANTS = ROOT / "deploy/dashboard/hardened/sor-dashboard-grants.sql"
APACHE_CONFIGS = [
    ROOT / "deploy/dashboard/apache-solarinet.conf",
    ROOT / "deploy/host-configs/xenon/apache-solarinet.conf",
]
API = ROOT / "dashboard/api"

OPTIONAL_ENV = {"SOLARI_DB_DSN", "SOLARI_DB_SOCK", "SOLARI_ALLOWED_ORIGINS"}
SECRET_ENV = {"SOLARI_DB_PASS", "SOR_DB_PASS", "FORGEJO_TOKEN", "KC_SVC_SECRET"}
PATH_ENV = {
    "SOLARI_AUTH_FILE",
    "SOLARI_CTL_SOCK",
    "SOLARI_AUTH_RATE_DIR",
    "SOLARINET_CA_PEM",
    "STEPCA_CA_FILE",
    "SOLARI_RPZ_ZONE_FILE",
    "SOLARI_UPLOAD_DIR",
    "SOLARI_FLEET_CATALOG",
}


class HardenedFpmContractTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pool = POOL.read_text()
        cls.tmpfiles = TMPFILES.read_text()
        cls.php = "\n".join(path.read_text(errors="ignore") for path in API.rglob("*.php"))
        cls.exports = dict(re.findall(r"^\s*env\[([A-Z][A-Z0-9_]*)\]\s*=\s*(.*?)\s*$", cls.pool, re.M))

    def test_php_environment_dependencies_are_configured_or_explicitly_optional(self):
        used = set(re.findall(r"getenv\(\s*['\"]([A-Z][A-Z0-9_]*)['\"]", self.php))
        self.assertEqual(used - set(self.exports) - OPTIONAL_ENV, set())
        self.assertIn("Optional overrides left unset", self.pool)

    def test_sensitive_values_remain_install_time_placeholders(self):
        for name in SECRET_ENV:
            with self.subTest(name=name):
                self.assertTrue(self.exports[name].startswith("__REPLACE_"))

    def test_pool_uses_the_scoped_dashboard_sor_account(self):
        self.assertEqual(self.exports["SOR_DB_USER"], "solari_dashboard")
        grants = SOR_GRANTS.read_text()
        self.assertIn("'solari_dashboard'@'10.0.0.20'", grants)
        self.assertNotIn("GRANT ALL", grants.upper())

    def test_file_paths_stay_inside_open_basedir(self):
        match = re.search(r"^\s*php_admin_value\[open_basedir\]\s*=\s*(.+)$", self.pool, re.M)
        self.assertIsNotNone(match)
        allowed = [PurePosixPath(path) for path in match.group(1).split(":")]
        for name in PATH_ENV:
            with self.subTest(name=name):
                value = PurePosixPath(self.exports[name])
                self.assertTrue(any(value == root or root in value.parents for root in allowed),
                                f"{name}={value} is outside open_basedir")

    def test_upload_state_directory_is_created_for_the_pool_user(self):
        entries = [line.split() for line in self.tmpfiles.splitlines()
                   if line.strip() and not line.lstrip().startswith("#")]
        self.assertIn(
            ["d", "/var/lib/solarinet-dashboard/uploads", "0700", "www-solari", "www-solari", "-", "-"],
            entries,
        )

    def test_apache_php_entry_uses_the_deployed_root_owned_api(self):
        for path in APACHE_CONFIGS:
            with self.subTest(path=path.name):
                config = path.read_text()
                expected = (
                    '/opt/solarinet/current/dashboard/api/index.php'
                    if path == APACHE_CONFIGS[0]
                    else '/var/www/solarinet/api/index.php'
                )
                self.assertIn(f'SCRIPT_FILENAME "{expected}"', config)
                self.assertNotIn("/home/jason/Code/Solarinet/dashboard/api", config)


if __name__ == "__main__":
    unittest.main()
