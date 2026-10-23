#!/usr/bin/env python3
"""Local authbroker HTTP credential policy tests; no broker connection."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import authbrokerd  # noqa: E402
import loopback_test  # noqa: E402


class HttpTokenTests(unittest.TestCase):
    def test_missing_or_example_token_is_rejected_at_startup(self):
        for token in ("", "   ", "CHANGE_ME_LONG_RANDOM"):
            with self.subTest(token=token):
                with self.assertRaises(ValueError):
                    authbrokerd.validate_http_token(token)

    def test_non_example_token_is_accepted_at_startup(self):
        authbrokerd.validate_http_token("a-long-random-secret")

    def test_empty_configuration_never_authorizes(self):
        self.assertFalse(authbrokerd.http_token_authorized("", "Bearer anything"))
        self.assertFalse(authbrokerd.http_token_authorized("   ", "Bearer anything"))

    def test_bearer_header_must_match_configured_token(self):
        token = "a-long-random-secret"
        self.assertTrue(authbrokerd.http_token_authorized(token, f"Bearer {token}"))
        self.assertFalse(authbrokerd.http_token_authorized(token, ""))
        self.assertFalse(authbrokerd.http_token_authorized(token, "Bearer wrong"))
        self.assertFalse(authbrokerd.http_token_authorized(token, token))


class LoopbackDefaultsTests(unittest.TestCase):
    def test_loopback_defaults_cannot_target_live_broker_or_credentials(self):
        args = loopback_test.parse_args([])
        self.assertEqual("127.0.0.1", args.host)
        self.assertEqual(1883, args.port)
        self.assertEqual("", args.username)
        self.assertEqual("", args.password)

    def test_explicit_fixture_broker_is_supported(self):
        args = loopback_test.parse_args([
            "--host", "127.0.0.1", "--port", "18883",
            "--username", "fixture", "--password", "fixture-only",
        ])
        self.assertEqual(("127.0.0.1", 18883, "fixture", "fixture-only"),
                         (args.host, args.port, args.username, args.password))


if __name__ == "__main__":
    unittest.main()
