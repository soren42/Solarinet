"""Regression checks for Opie's model tool boundary."""
import configparser
import os
import unittest
from unittest.mock import Mock, patch

import opied


class InvestigationToolBoundaryTests(unittest.TestCase):
    def test_only_vetted_probe_is_granted(self):
        cfg = configparser.ConfigParser()
        cfg.read_dict({"opie": {"mcp_config": "/test/opie-mcp.json", "timeout_sec": "1"}})
        completed = type("Completed", (), {"stdout": "<<<OPIE_SUMMARY>>>ok<<<OPIE_ANALYSIS>>>done", "stderr": "", "returncode": 0})()

        with patch.dict(os.environ, {
            "SOLARI_DB_PASS": "db-test-secret",
            "ANTHROPIC_API_KEY": "api-test-secret",
            "ANTHROPIC_AUTH_TOKEN": "auth-test-secret",
            "CLAUDE_CODE_OAUTH_TOKEN": "oauth-test-secret",
            "OPIE_TEST_SAFE_ENV": "preserved",
        }), patch.object(opied.subprocess, "run", return_value=completed) as run:
            result = opied.investigate(cfg, "safe prompt")

        self.assertIsNone(result[-1])
        args = run.call_args.args[0]
        self.assertEqual(args[args.index("--tools") + 1], "")
        self.assertIn("--strict-mcp-config", args)
        self.assertEqual(args[args.index("--setting-sources") + 1], "user")
        self.assertEqual(args[args.index("--allowedTools") + 1], "mcp__opie__probe")
        denied = args[args.index("--disallowedTools") + 1:]
        self.assertIn("Bash", denied)
        self.assertIn("Read", denied)
        self.assertIn("Grep", denied)
        self.assertIn("Glob", denied)
        child_env = run.call_args.kwargs["env"]
        self.assertNotIn("SOLARI_DB_PASS", child_env)
        self.assertNotIn("ANTHROPIC_API_KEY", child_env)
        self.assertNotIn("ANTHROPIC_AUTH_TOKEN", child_env)
        self.assertNotIn("CLAUDE_CODE_OAUTH_TOKEN", child_env)
        self.assertEqual(child_env["OPIE_TEST_SAFE_ENV"], "preserved")

    def test_database_user_fallback_is_dedicated(self):
        cfg = configparser.ConfigParser()
        cfg.read_dict({"db": {"password": "test-only"}})
        driver = Mock()
        with patch.object(opied, "pymysql", driver):
            opied.connect_db(cfg)
        self.assertEqual(driver.connect.call_args.kwargs["user"], "solari-opie")


if __name__ == "__main__":
    unittest.main()
