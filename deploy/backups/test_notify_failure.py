#!/usr/bin/env python3
"""Tests for fail-soft backup failure notification delivery."""

from contextlib import redirect_stderr
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch


SCRIPT = Path(__file__).with_name("solari-notify-failure.py")
SPEC = importlib.util.spec_from_file_location("solari_notify_failure", SCRIPT)
notify = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(notify)


class NotifyFailureTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="solari-notify-failure-")
        self.config = Path(self.tmp.name) / "notify.conf"
        self.config.write_text(
            "[amqp]\nurl=amqp://backup:secret-value@broker/solari\nexchange=notify.events\n",
            encoding="utf-8",
        )

    def tearDown(self):
        self.tmp.cleanup()

    def test_publishes_durable_warning_to_configured_exchange(self):
        channel = Mock()
        connection = Mock()
        connection.channel.return_value = channel
        fake_pika = types.SimpleNamespace(
            URLParameters=Mock(return_value=object()),
            BlockingConnection=Mock(return_value=connection),
            BasicProperties=lambda **kwargs: types.SimpleNamespace(**kwargs),
        )

        with patch.object(notify, "pika", fake_pika):
            notify.publish_failure("solari-backup@xenon.service", str(self.config))

        fake_pika.BlockingConnection.assert_called_once()
        kwargs = channel.basic_publish.call_args.kwargs
        self.assertEqual(kwargs["exchange"], "notify.events")
        self.assertEqual(kwargs["routing_key"], "notify.warn")
        self.assertEqual(kwargs["properties"].delivery_mode, 2)
        message = json.loads(kwargs["body"])
        self.assertEqual(message["unit"], "solari-backup@xenon.service")
        self.assertEqual(message["source"], "solari-backup")
        self.assertNotIn("secret-value", kwargs["body"])
        connection.close.assert_called_once()

    def test_missing_pika_is_fail_soft(self):
        output = io.StringIO()
        with patch.object(notify, "pika", None), redirect_stderr(output):
            notify.publish_failure("solari-backup@xenon.service", str(self.config))
        self.assertIn("pika is not installed", output.getvalue())
        self.assertNotIn("secret-value", output.getvalue())

    def test_connection_error_does_not_log_amqp_credentials(self):
        fake_pika = types.SimpleNamespace(
            URLParameters=Mock(return_value=object()),
            BlockingConnection=Mock(side_effect=RuntimeError("amqp://backup:secret-value@broker/solari refused")),
            BasicProperties=lambda **kwargs: types.SimpleNamespace(**kwargs),
        )
        output = io.StringIO()
        argv = [str(SCRIPT), "--unit", "solari-backup@xenon.service"]
        with patch.object(notify, "pika", fake_pika), patch.object(sys, "argv", argv), \
                patch.dict(os.environ, {"SOLARI_NOTIFY_CONFIG": str(self.config)}), redirect_stderr(output):
            result = notify.main()
        self.assertEqual(result, 0)
        self.assertIn("details suppressed", output.getvalue())
        self.assertNotIn("secret-value", output.getvalue())


if __name__ == "__main__":
    unittest.main()
