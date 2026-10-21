"""Restart an isolated RabbitMQ application under the live DNS consumer.

This is an integration test, not a production health probe. It runs only when
CI explicitly supplies both the isolated AMQP URL and its disposable broker
container ID. The test stops and starts RabbitMQ inside that container, then
proves the still-running daemon reconciles and consumes an event after return.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import uuid

import pika


ROOT = pathlib.Path(__file__).resolve().parents[2]
BROKER_URL = os.environ.get("SOLARI_TEST_AMQP_URL", "")
BROKER_CONTAINER = os.environ.get("SOLARI_TEST_BROKER_CONTAINER", "")


@unittest.skipUnless(BROKER_URL and BROKER_CONTAINER,
                     "requires isolated AMQP URL and disposable RabbitMQ container")
class BrokerRecoveryTest(unittest.TestCase):
    def setUp(self):
        if not shutil.which("docker"):
            self.fail("Docker CLI is required to stop/restart the disposable CI broker")
        if not BROKER_CONTAINER.isalnum():
            self.fail("broker container ID must be an alphanumeric Docker ID")
        self.docker = ["docker"]
        probe = subprocess.run(self.docker + ["info"], capture_output=True, text=True, timeout=15)
        if probe.returncode and shutil.which("sudo"):
            self.docker = ["sudo", "-n", "docker"]
        image = subprocess.run(
            self.docker + ["inspect", "--format", "{{.Config.Image}}", BROKER_CONTAINER],
            capture_output=True, text=True, timeout=15)
        if image.returncode or not image.stdout.strip().startswith("rabbitmq:"):
            self.fail("control target must be an inspectable RabbitMQ container")
        self.suffix = uuid.uuid4().hex
        self.exchange = "solarinet.recovery." + self.suffix
        self.queue = "solarinet.recovery." + self.suffix
        self.temp = tempfile.TemporaryDirectory(prefix="solarinet-broker-recovery-")
        self.root = pathlib.Path(self.temp.name)
        self.applies = self.root / "applies.jsonl"
        self.worker_log = self.root / "consumer.log"
        self.worker = self.root / "consumer.py"
        self.config = self.root / "consumer.json"
        self.worker.write_text(
            "import configparser, json, pathlib, sys\n"
            "import sor_apply_dnsd as daemon\n"
            "cfg = configparser.ConfigParser(interpolation=None)\n"
            "cfg.read_dict(json.loads(pathlib.Path(sys.argv[1]).read_text()))\n"
            "state = pathlib.Path(sys.argv[2])\n"
            "def fake_apply(_cfg):\n"
            "    with state.open('a', encoding='utf-8') as out:\n"
            "        out.write('applied\\n'); out.flush()\n"
            "    return True\n"
            "daemon.load_cfg = lambda: cfg\n"
            "daemon.apply = fake_apply\n"
            "daemon.run()\n", encoding="utf-8")

        from urllib.parse import urlsplit
        parsed = urlsplit(BROKER_URL)
        self.assertEqual("amqp", parsed.scheme)
        self.assertIn(parsed.hostname, {"127.0.0.1", "localhost"},
                      "broker URL must point to a loopback disposable broker")
        self.assertTrue(parsed.username and parsed.password,
                        "isolated fixture must use explicit test credentials")
        self.host = parsed.hostname
        self.port = parsed.port or 5672
        self.config.write_text(json.dumps({
            "amqp": {
                "url": BROKER_URL,
                "exchange": self.exchange,
                "dns_queue": self.queue,
                "reconnect_delay_sec": "0.2",
            },
            "dns": {"debounce_sec": "0.05", "reconcile_interval_sec": "0"},
        }), encoding="utf-8")
        self.process = None

    def broker_command(self, action):
        if action not in {"stop_app", "start_app"}:
            raise ValueError("invalid RabbitMQ control action")
        return subprocess.run(
            self.docker + ["exec", BROKER_CONTAINER, "rabbitmqctl", action],
            capture_output=True, text=True, timeout=30)

    def broker_running(self):
        return subprocess.run(
            self.docker + ["exec", BROKER_CONTAINER, "rabbitmq-diagnostics", "-q", "check_running"],
            capture_output=True, text=True, timeout=10)

    def container_command(self, action):
        if action not in {"stop", "start"}:
            raise ValueError("invalid disposable broker container action")
        arguments = (["stop", "--time", "10", BROKER_CONTAINER] if action == "stop"
                     else ["start", BROKER_CONTAINER])
        return subprocess.run(self.docker + arguments,
                              capture_output=True, text=True, timeout=30)

    def container_state(self):
        return subprocess.run(
            self.docker + ["inspect", "--format", "{{.State.Status}}", BROKER_CONTAINER],
            capture_output=True, text=True, timeout=15)

    def wait_for_broker(self, timeout=30):
        deadline = time.monotonic() + timeout
        last = None
        while time.monotonic() < deadline:
            last = self.broker_running()
            if last.returncode == 0:
                return
            time.sleep(0.2)
        self.fail("RabbitMQ did not become fully running: " +
                  ((last.stdout + last.stderr) if last else "no diagnostic result"))

    def connect(self):
        params = pika.URLParameters(BROKER_URL)
        params.connection_attempts = 2
        params.retry_delay = 0.2
        params.socket_timeout = 4
        params.blocked_connection_timeout = 4
        return pika.BlockingConnection(params)

    def wait_for_applies(self, minimum, timeout=25):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.process is not None and self.process.poll() is not None:
                raise AssertionError(f"DNS consumer exited unexpectedly ({self.process.returncode})")
            try:
                count = len(self.applies.read_text(encoding="utf-8").splitlines())
            except FileNotFoundError:
                count = 0
            if count >= minimum:
                return
            time.sleep(0.1)
        try:
            diagnostics = self.worker_log.read_text(encoding="utf-8")[-6000:]
        except FileNotFoundError:
            diagnostics = "consumer log was not created"
        self.fail(f"consumer did not complete {minimum} apply passes\n{diagnostics}")

    def wait_for_consumer(self, connection, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                channel = connection.channel()
                result = channel.queue_declare(queue=self.queue, passive=True)
                channel.close()
                if result.method.consumer_count > 0:
                    return
            except pika.exceptions.AMQPError:
                pass
            time.sleep(0.1)
        self.fail("DNS consumer did not register after broker recovery")

    def test_daemon_reconnects_after_rabbitmq_application_and_container_restart(self):
        self.wait_for_broker()
        connection = self.connect()
        channel = connection.channel()
        channel.exchange_declare(exchange=self.exchange, exchange_type="topic", durable=True)
        channel.queue_declare(queue=self.queue, durable=True)
        for key in ("sor.entities.*", "sor.ip_addresses.*", "sor.interfaces.*",
                    "sor.dns_records.*", "sor.dns_zones.*", "sor.networks.*",
                    "sor.subnets.*"):
            channel.queue_bind(exchange=self.exchange, queue=self.queue, routing_key=key)
        channel.close()
        connection.close()

        with self.worker_log.open("a", encoding="utf-8") as log:
            self.process = subprocess.Popen(
                [sys.executable, str(self.worker), str(self.config), str(self.applies)],
                cwd=ROOT, env={**os.environ, "PYTHONPATH": str(ROOT / "deploy/sorsync")},
                stdout=log, stderr=subprocess.STDOUT, text=True)
        try:
            self.wait_for_applies(1)
            broker = self.connect()
            try:
                self.wait_for_consumer(broker)
            finally:
                broker.close()

            stopped = self.broker_command("stop_app")
            # Rabbit's own diagnostic confirms the app is down. The daemon
            # must remain alive and retry instead of exiting independently.
            down = self.broker_running()
            confirmed_down = down.returncode != 0 and (
                "not running" in down.stdout.lower() + down.stderr.lower() or
                "not fully booted" in down.stdout.lower() + down.stderr.lower())
            self.assertTrue(confirmed_down,
                                "RabbitMQ must be stopped during the outage; "
                                f"stop={stopped.stdout}{stopped.stderr} check={down.stdout}{down.stderr}")
            self.assertIsNone(self.process.poll(), "daemon must stay alive through broker downtime")

            started = self.broker_command("start_app")
            try:
                self.wait_for_broker()
            except AssertionError as error:
                self.fail(f"RabbitMQ application restart did not recover; "
                          f"start={started.stdout}{started.stderr} check={error}")
            self.wait_for_applies(2)

            stopped_container = self.container_command("stop")
            self.assertEqual(0, stopped_container.returncode,
                             "disposable RabbitMQ container must stop cleanly")
            state = self.container_state()
            self.assertEqual("exited", state.stdout.strip(),
                             f"broker container must be down; {state.stdout}{state.stderr}")
            self.assertIsNone(self.process.poll(),
                              "DNS consumer must stay alive through full broker-container downtime")

            started_container = self.container_command("start")
            self.assertEqual(0, started_container.returncode,
                             "disposable RabbitMQ container must start cleanly")
            self.wait_for_broker()
            self.wait_for_applies(3)

            broker = self.connect()
            try:
                self.wait_for_consumer(broker)
                publisher = broker.channel()
                publisher.confirm_delivery()
                publisher.basic_publish(
                    exchange=self.exchange, routing_key="sor.entities.insert",
                    body=json.dumps({"fixture": self.suffix}).encode(), mandatory=True)
                publisher.close()
            finally:
                broker.close()
            self.wait_for_applies(4)

            verify = self.connect()
            try:
                check = verify.channel()
                message = check.basic_get(queue=self.queue, auto_ack=False)
                self.assertIsNone(message[0], "recovered consumer must acknowledge the fixture event")
                check.queue_delete(queue=self.queue)
                check.exchange_delete(exchange=self.exchange)
                check.close()
            finally:
                verify.close()
        finally:
            # Restore the disposable broker even if an assertion fails mid-test.
            state = self.container_state()
            if state.returncode == 0 and state.stdout.strip() != "running":
                self.container_command("start")
                self.wait_for_broker()
            else:
                running = self.broker_running()
                if running.returncode:
                    self.broker_command("start_app")
                    self.wait_for_broker()
            if self.process is not None and self.process.poll() is None:
                self.process.terminate()
                try:
                    self.process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    self.process.kill()
                    self.process.wait(timeout=5)
            try:
                cleanup = self.connect()
                channel = cleanup.channel()
                channel.queue_delete(queue=self.queue)
                channel.exchange_delete(exchange=self.exchange)
                cleanup.close()
            except Exception:
                pass
            self.temp.cleanup()


if __name__ == "__main__":
    unittest.main()
