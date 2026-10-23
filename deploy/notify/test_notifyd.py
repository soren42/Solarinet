#!/usr/bin/env python3
"""Notification envelope validation tests; no broker or sender credentials."""
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(__file__))
import notifyd  # noqa: E402


class ParseMessageTests(unittest.TestCase):
    def test_accepts_contract_message_and_omitted_optional_fields(self):
        message = notifyd.parse_message(json.dumps({
            "severity": "crit",
            "title": "Disk pressure",
            "body": "Free space is low.",
            "channel": "auto",
            "to": ["ops@example.test"],
            "source": "monitoring",
            "ts": 1783483080,
        }))
        self.assertEqual("crit", message["severity"])
        self.assertEqual(["ops@example.test"], message["to"])
        self.assertEqual({"title": "Only title"},
                         notifyd.parse_message(b'{"title":"Only title"}'))

    def test_rejects_non_object_or_missing_summary(self):
        for body in (b"[]", b'"text"', b'{}'):
            with self.subTest(body=body), self.assertRaises(ValueError):
                notifyd.parse_message(body)

    def test_rejects_invalid_field_types_and_values(self):
        invalid = (
            {"title": 7},
            {"body": None},
            {"title": "x", "severity": []},
            {"title": "x", "severity": "urgent"},
            {"title": "x", "channel": 1},
            {"title": "x", "channel": "  "},
            {"title": "x", "to": "ops@example.test"},
            {"title": "x", "to": ["ops@example.test", " "]},
            {"title": "x", "source": False},
            {"title": "x", "ts": True},
            {"title": "x", "ts": -1},
            {"title": "x", "ts": 10 ** 1000},
            {"title": "x", "ts": 253402300800},
            {"title": "x", "ts": float("nan")},
            {"title": "x", "ts": float("inf")},
        )
        for index, message in enumerate(invalid):
            with self.subTest(index=index), self.assertRaises(ValueError):
                notifyd.parse_message(json.dumps(message))


if __name__ == "__main__":
    unittest.main()
