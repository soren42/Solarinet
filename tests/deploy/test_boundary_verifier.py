#!/usr/bin/env python3
"""Fail-closed checks for the PHP-to-host boundary verifier."""

import os
import pwd
import subprocess
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
VERIFIER = ROOT / "deploy/dashboard/hardened/verify-boundary.sh"


class BoundaryVerifierTests(unittest.TestCase):
    def test_missing_server_environment_is_a_failure_not_a_skip(self):
        missing = "/tmp/solarinet-boundary-test-no-such-file"
        env = os.environ.copy()
        env.update({
            "SOLARI_SERVER_CONF": missing,
            "SOLARI_SERVER_ENV": missing,
            "SOLARI_CA_KEY": missing,
            "SOLARI_CTL_SOCK": missing,
            "SOLARI_DASH_USER": pwd.getpwuid(os.getuid()).pw_name,
        })

        result = subprocess.run(
            ["/bin/bash", str(VERIFIER)],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            check=False,
        )

        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("server env not found at", result.stdout)
        self.assertIn("RESULT: FAIL", result.stdout)
        self.assertNotIn("RESULT: PASS", result.stdout)


if __name__ == "__main__":
    unittest.main()
