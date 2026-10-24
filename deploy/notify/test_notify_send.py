from __future__ import annotations

import importlib.util
import io
import json
import os
import pathlib
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stderr
from unittest.mock import patch


SCRIPT = pathlib.Path(__file__).with_name("notify-send.py")


class NotifySendTests(unittest.TestCase):
    def _tempdir(self):
        return tempfile.TemporaryDirectory(prefix="notify-send-test-", dir="/root" if os.geteuid() == 0 else None)

    def _load(self, fake_pika: object) -> types.ModuleType:
        spec = importlib.util.spec_from_file_location("notify_send_under_test", SCRIPT)
        assert spec and spec.loader
        module = importlib.util.module_from_spec(spec)
        with patch.dict(sys.modules, {"pika": fake_pika}):
            spec.loader.exec_module(module)
        return module

    def test_amqp_config_path_is_selectable_for_installed_callers(self) -> None:
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            config.write_text("[amqp]\nurl=amqp://example.invalid/vhost\nexchange=notify.custom\n", encoding="utf-8")
            module = self._load(types.SimpleNamespace())
            self.assertEqual(module.amqp_url(str(config)), ("amqp://example.invalid/vhost", "notify.custom"))

    def test_amqp_config_rejects_permissive_mode(self) -> None:
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            config.write_text("[amqp]\nurl=amqp://example.invalid/vhost\n", encoding="utf-8")
            config.chmod(0o644)
            module = self._load(types.SimpleNamespace())
            with self.assertRaisesRegex(ValueError, "private regular file|safely read"):
                module.amqp_url(str(config))

    def test_invalid_config_error_does_not_echo_secret_text(self) -> None:
        secret = "must-not-appear-in-errors"
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            config.write_text(f"[amqp]\nurl=amqp://example.invalid/vhost\n{secret} bad line\n", encoding="utf-8")
            config.chmod(0o600)
            module = self._load(types.SimpleNamespace())
            with self.assertRaisesRegex(ValueError, "notify config is invalid") as raised:
                module.amqp_url(str(config))
            self.assertNotIn(secret, str(raised.exception))

    def test_amqp_config_rejects_symlink(self) -> None:
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            target = pathlib.Path(directory) / "target.conf"
            target.write_text("[amqp]\nurl=amqp://example.invalid/vhost\n", encoding="utf-8")
            target.chmod(0o600)
            config.symlink_to(target)
            module = self._load(types.SimpleNamespace())
            with self.assertRaisesRegex(ValueError, "private regular file|safely read"):
                module.amqp_url(str(config))

    @unittest.skipUnless(os.geteuid() == 0, "root-only path validation")
    def test_root_amqp_config_rejects_writable_parent_directory(self) -> None:
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            config.write_text("[amqp]\nurl=amqp://example.invalid/vhost\n", encoding="utf-8")
            config.chmod(0o600)
            pathlib.Path(directory).chmod(0o777)
            module = self._load(types.SimpleNamespace())
            try:
                with self.assertRaisesRegex(ValueError, "protected directories"):
                    module.amqp_url(str(config))
            finally:
                pathlib.Path(directory).chmod(0o700)

    @unittest.skipUnless(os.geteuid() == 0, "root-only path validation")
    def test_root_amqp_config_rejects_symlinked_parent(self) -> None:
        with self._tempdir() as directory:
            target = pathlib.Path(directory) / "notify.conf"
            target.write_text("[amqp]\nurl=amqp://example.invalid/vhost\n", encoding="utf-8")
            target.chmod(0o600)
            alias = pathlib.Path("/root") / f".notify-send-path-link-{os.getpid()}"
            alias.symlink_to(directory, target_is_directory=True)
            module = self._load(types.SimpleNamespace())
            try:
                with self.assertRaisesRegex(ValueError, "safely read"):
                    module.amqp_url(str(alias / "notify.conf"))
            finally:
                alias.unlink()

    def test_cli_publishes_message_using_explicit_config(self) -> None:
        observed: dict[str, object] = {}

        class FakeChannel:
            def exchange_declare(self, **kwargs: object) -> None:
                observed["exchange"] = kwargs["exchange"]

            def confirm_delivery(self) -> None:
                observed["confirmed"] = True

            def basic_publish(self, exchange: str, routing_key: str, body: str, properties: object,
                              mandatory: bool) -> None:
                observed.update(exchange=exchange, routing_key=routing_key,
                                message=json.loads(body), mandatory=mandatory)

        class FakeConnection:
            def channel(self) -> FakeChannel:
                return FakeChannel()

            def close(self) -> None:
                observed["closed"] = True

        class FakePika:
            @staticmethod
            def URLParameters(url: str) -> str:
                observed["url"] = url
                return "parameters"

            @staticmethod
            def BlockingConnection(parameters: str) -> FakeConnection:
                observed["parameters"] = parameters
                return FakeConnection()

            class BasicProperties:
                def __init__(self, **kwargs: object) -> None:
                    observed["properties"] = kwargs

        module = self._load(FakePika)
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            config.write_text("[amqp]\nurl=amqp://example.invalid/vhost\nexchange=notify.events\n", encoding="utf-8")
            argv = ["notify-send.py", "--config", str(config), "--severity", "crit",
                    "--title", "expiry alert", "--body", "expires soon", "--source", "solarinet-pki"]
            with patch.object(sys, "argv", argv):
                module.main()
        self.assertEqual(observed["routing_key"], "notify.crit")
        self.assertEqual(observed["exchange"], "notify.events")
        self.assertEqual(observed["message"]["source"], "solarinet-pki")
        self.assertTrue(observed["closed"])

    def test_publisher_error_does_not_echo_broker_credentials(self) -> None:
        secret = "amqp://user:private-pass@example.invalid/"

        class FailingPika:
            @staticmethod
            def URLParameters(url: str) -> str:
                return url

            @staticmethod
            def BlockingConnection(parameters: str) -> object:
                raise RuntimeError(secret)

        module = self._load(FailingPika)
        with self._tempdir() as directory:
            config = pathlib.Path(directory) / "notify.conf"
            config.write_text(f"[amqp]\nurl={secret}\n", encoding="utf-8")
            config.chmod(0o600)
            argv = ["notify-send.py", "--config", str(config), "--severity", "crit", "--title", "test"]
            stderr = io.StringIO()
            with patch.object(sys, "argv", argv), redirect_stderr(stderr):
                with self.assertRaises(SystemExit) as raised:
                    module.main()
            self.assertEqual(raised.exception.code, 2)
            self.assertNotIn(secret, stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
