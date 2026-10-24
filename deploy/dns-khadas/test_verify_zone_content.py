#!/usr/bin/env python3
"""Tests for private hash parsing and primary/secondary comparison results."""
import importlib.util
import pathlib
import unittest
from unittest import mock


SCRIPT = pathlib.Path(__file__).with_name("verify-zone-content.py")
SPEC = importlib.util.spec_from_file_location("verify_zone_content", SCRIPT)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class ZoneContentVerifierTest(unittest.TestCase):
    def test_accepts_only_unique_zone_digest_rows(self):
        digest = "a" * 64
        self.assertEqual({"dashboard": digest},
                         VERIFY.read_hashes("dashboard\t" + digest + "\n"))
        with self.assertRaisesRegex(ValueError, "invalid digest"):
            VERIFY.read_hashes("dashboard\tbad\n")
        with self.assertRaisesRegex(ValueError, "invalid digest"):
            VERIFY.read_hashes("dashboard\t" + digest + "\ndashboard\t" + digest)

    def test_reports_matching_canonical_zone_hashes(self):
        digest = "b" * 64
        with mock.patch.object(VERIFY, "remote_hashes", side_effect=[
            {"dashboard": digest}, {"dashboard": digest}
        ]):
            self.assertEqual((1, []), VERIFY.verify("xenon", "chlorine", "jason"))

    def test_reports_only_zone_names_for_content_mismatch(self):
        with mock.patch.object(VERIFY, "remote_hashes", side_effect=[
            {"dashboard": "a" * 64, "rpz.akoria": "b" * 64},
            {"dashboard": "a" * 64, "rpz.akoria": "c" * 64},
        ]):
            self.assertEqual((2, ["rpz.akoria"]),
                             VERIFY.verify("xenon", "chlorine", "jason"))

    def test_reports_inventory_mismatch_without_record_data(self):
        with mock.patch.object(VERIFY, "remote_hashes", side_effect=[
            {"dashboard": "a" * 64}, {"dashboard": "a" * 64, "rpz.akoria": "b" * 64},
        ]):
            count, differences = VERIFY.verify("xenon", "chlorine", "jason")
        self.assertEqual(1, count)
        self.assertEqual(["secondary extra: rpz.akoria"], differences)


if __name__ == "__main__":
    unittest.main()
