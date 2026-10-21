#!/usr/bin/env python3
"""Exercise RabbitMQ's persistent-volume upgrade and snapshot restore path.

The test creates uniquely named local containers and volumes. It reuses one
volume through 3.13.7 -> 4.2.9 -> 4.3.7 -> 4.4.0, then starts a pre-4.4 copy on
4.3.7 to verify rollback by restoring the snapshot. It never uses the live
broker or its data.
"""

from __future__ import annotations

import os
import shlex
import socket
import subprocess
import time
import unittest
import uuid

import pika


INITIAL = (
    "3.13.7",
    "rabbitmq:3.13.7-management@sha256:e582c0bc7766f3342496d8485efb5a1df782b5ce3886ad017e2eaae442311f69",
)
RABBITMQ_42 = (
    "4.2.9",
    "rabbitmq:4.2-management@sha256:7ce498399b07a9594a3d23e5a7920edf233edb888e82503c35357c23cefc2e61",
)
RABBITMQ_437 = ("4.3.7", "solari-rabbitmq-upgrade:4.3.7")
RABBITMQ_44 = (
    "4.4.0",
    "rabbitmq:4@sha256:71be6ca71cd3ad92f29b354ec381b1ee587894aaaac21f185b8cf43fb50d3feb",
)
VHOST = "solari"
USERNAME = "solarinet_upgrade_fixture"
PASSWORD = "upgrade_fixture_only"
EXCHANGE = "solari.upgrade.fixture"
QUEUE = "solari.upgrade.fixture"
ROUTING_KEY = "sor.entities.insert"


@unittest.skipUnless(
    os.environ.get("SOLARI_RUN_BROKER_UPGRADE_TEST") == "1",
    "run in the dedicated isolated Docker upgrade step",
)
class BrokerUpgradePathTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.docker = shlex.split(os.environ.get("SOLARI_TEST_DOCKER", "docker"))
        cls.suffix = uuid.uuid4().hex[:12]
        cls.volume = f"solari-rmq-upgrade-{cls.suffix}"
        cls.snapshot_volume = f"solari-rmq-upgrade-pre44-{cls.suffix}"
        cls.container = f"solari-rmq-upgrade-{cls.suffix}"
        cls.host_port = None
        cls.addClassCleanup(
            cls.docker_cmd, "volume", "rm", "-f", cls.snapshot_volume, check=False)
        cls.addClassCleanup(
            cls.docker_cmd, "volume", "rm", "-f", cls.volume, check=False)
        cls.addClassCleanup(cls.docker_cmd, "rm", "-f", cls.container, check=False)
        cls.docker_cmd("volume", "create", cls.volume)
        cls.docker_cmd(
            "run", "--rm", "--user", "root", "--entrypoint", "sh",
            "--volume", f"{cls.volume}:/var/lib/rabbitmq",
            INITIAL[1], "-ec",
            "od -An -N32 -tx1 /dev/urandom | tr -d ' \\n' > /var/lib/rabbitmq/.erlang.cookie; "
            "chown rabbitmq:rabbitmq /var/lib/rabbitmq/.erlang.cookie; "
            "chmod 0400 /var/lib/rabbitmq/.erlang.cookie",
        )

    @classmethod
    def docker_cmd(cls, *args, check=True, capture_output=True):
        return subprocess.run(
            [*cls.docker, *args],
            check=check,
            text=True,
            capture_output=capture_output,
            timeout=90,
        )

    @classmethod
    def start_release(cls, image, expected_version, volume=None):
        data_volume = volume or cls.volume
        cls.docker_cmd(
            "run", "--detach", "--name", cls.container,
            "--hostname", "solari-upgrade-test",
            "--publish", "127.0.0.1::5672",
            "--volume", f"{data_volume}:/var/lib/rabbitmq",
            image,
        )
        deadline = time.monotonic() + 180
        last_error = "container has not started"
        while time.monotonic() < deadline:
            state = cls.docker_cmd("inspect", "--format", "{{.State.Status}}", cls.container,
                                   check=False)
            if state.returncode or state.stdout.strip() != "running":
                last_error = state.stderr.strip() or state.stdout.strip()
                if state.stdout.strip() == "exited":
                    break
            else:
                health = cls.docker_cmd(
                    "exec", cls.container, "rabbitmq-diagnostics", "-q", "check_running",
                    check=False)
                if health.returncode == 0:
                    version = cls.docker_cmd(
                        "exec", cls.container, "rabbitmq-diagnostics", "-q", "server_version")
                    actual_version = version.stdout.strip()
                    if actual_version != expected_version:
                        raise AssertionError(
                            f"expected RabbitMQ {expected_version}, got {actual_version}")
                    mapping = cls.docker_cmd("port", cls.container, "5672/tcp").stdout.strip()
                    cls.host_port = int(mapping.rsplit(":", 1)[1])
                    cls.wait_for_connection()
                    return
                last_error = health.stderr.strip() or health.stdout.strip()
            time.sleep(1)
        logs = cls.docker_cmd("logs", cls.container, check=False).stdout[-6000:]
        raise AssertionError(
            f"RabbitMQ {expected_version} did not become ready: {last_error}\n{logs}")

    @classmethod
    def stop_release(cls):
        cls.docker_cmd("stop", "--time", "30", cls.container)
        cls.docker_cmd("rm", cls.container)
        cls.host_port = None

    @classmethod
    def snapshot_stopped_volume(cls):
        cls.docker_cmd("volume", "create", cls.snapshot_volume)
        cls.docker_cmd(
            "run", "--rm", "--user", "root", "--entrypoint", "sh",
            "--volume", f"{cls.volume}:/source",
            "--volume", f"{cls.snapshot_volume}:/snapshot",
            INITIAL[1], "-ec", "cp -a /source/. /snapshot/",
        )

    @classmethod
    def wait_for_connection(cls):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            try:
                with socket.create_connection(("127.0.0.1", cls.host_port), timeout=0.5):
                    return
            except OSError:
                time.sleep(0.25)
        raise AssertionError("published AMQP port did not accept connections")

    @classmethod
    def connect(cls):
        return pika.BlockingConnection(pika.ConnectionParameters(
            host="127.0.0.1", port=cls.host_port, virtual_host=VHOST,
            credentials=pika.PlainCredentials(USERNAME, PASSWORD),
            connection_attempts=10, retry_delay=0.5, socket_timeout=3,
            blocked_connection_timeout=5, heartbeat=10,
        ))

    @classmethod
    def control(cls, *args):
        return cls.docker_cmd("exec", cls.container, "rabbitmqctl", *args)

    def assert_no_alarms(self):
        check = self.docker_cmd(
            "exec", self.container, "rabbitmq-diagnostics", "-q", "check_local_alarms")
        self.assertEqual(0, check.returncode, check.stderr)

    def assert_queue_count(self, expected):
        connection = self.connect()
        try:
            channel = connection.channel()
            method = channel.queue_declare(queue=QUEUE, passive=True).method
            self.assertEqual(expected, method.message_count)
        finally:
            connection.close()

    def publish_event(self, body):
        connection = self.connect()
        try:
            channel = connection.channel()
            channel.confirm_delivery()
            channel.basic_publish(
                exchange=EXCHANGE,
                routing_key=ROUTING_KEY,
                body=body,
                properties=pika.BasicProperties(delivery_mode=2),
                mandatory=True,
            )
        finally:
            connection.close()

    def consume_one(self, expected_body):
        connection = self.connect()
        try:
            channel = connection.channel()
            method, properties, body = channel.basic_get(queue=QUEUE, auto_ack=False)
            self.assertIsNotNone(method, "persistent test event disappeared during upgrade")
            self.assertEqual(expected_body, body.decode("utf-8"))
            channel.basic_ack(method.delivery_tag)
        finally:
            connection.close()

    def assert_test_topology_removed(self):
        connection = self.connect()
        try:
            channel = connection.channel()
            channel.queue_delete(queue=QUEUE)
            channel.exchange_delete(exchange=EXCHANGE)
        finally:
            connection.close()

    def test_upgrade_and_pre44_snapshot_restore(self):
        version, image = INITIAL
        self.start_release(image, version)
        try:
            self.control("add_vhost", VHOST)
            self.control("add_user", USERNAME, PASSWORD)
            self.control("set_permissions", "-p", VHOST, USERNAME, ".*", ".*", ".*")
            self.control("enable_feature_flag", "all")
            initial_flags = self.control("list_feature_flags", "name", "state").stdout
            self.assertRegex(initial_flags, r"khepri_db\s+disabled")

            connection = self.connect()
            try:
                channel = connection.channel()
                channel.exchange_declare(exchange=EXCHANGE, exchange_type="topic", durable=True)
                channel.queue_declare(
                    queue=QUEUE, durable=True,
                    arguments={"x-queue-type": "classic"},
                )
                channel.queue_bind(exchange=EXCHANGE, queue=QUEUE, routing_key=ROUTING_KEY)
            finally:
                connection.close()
            self.publish_event("before-upgrade-1")
            self.publish_event("before-upgrade-2")

            self.stop_release()
            self.start_release(RABBITMQ_42[1], RABBITMQ_42[0])
            self.assert_no_alarms()
            self.control("enable_feature_flag", "all")
            flags_42 = self.control("list_feature_flags", "name", "state").stdout
            self.assertRegex(flags_42, r"khepri_db\s+enabled")
            self.assert_queue_count(2)
            self.consume_one("before-upgrade-1")
            self.publish_event("during-4.2.9")
            self.assert_queue_count(2)

            self.stop_release()
            self.start_release(RABBITMQ_437[1], RABBITMQ_437[0])
            self.assert_no_alarms()
            self.control("enable_feature_flag", "all")
            self.assert_queue_count(2)
            self.consume_one("before-upgrade-2")
            self.publish_event("during-4.3.7")
            self.assert_queue_count(2)

            # Snapshot only while the 4.3.7 node is stopped. RabbitMQ 4.4.0
            # writes a new classic-message format that cannot be downgraded.
            self.stop_release()
            self.snapshot_stopped_volume()
            self.start_release(RABBITMQ_44[1], RABBITMQ_44[0])
            self.assert_no_alarms()
            self.control("enable_feature_flag", "all")
            self.assert_queue_count(2)
            self.consume_one("during-4.2.9")
            self.publish_event("during-4.4.0")
            self.assert_queue_count(2)

            # Verify 4.4 writes survive a container restart on its own volume.
            self.stop_release()
            self.start_release(RABBITMQ_44[1], RABBITMQ_44[0])
            self.assert_no_alarms()
            self.assert_queue_count(2)
            self.consume_one("during-4.3.7")
            self.consume_one("during-4.4.0")
            self.assert_queue_count(0)

            # Restore the pre-4.4 snapshot on the last reversible release.
            self.stop_release()
            self.start_release(RABBITMQ_437[1], RABBITMQ_437[0], self.snapshot_volume)
            self.assert_no_alarms()
            self.assert_queue_count(2)
            self.consume_one("during-4.2.9")
            self.consume_one("during-4.3.7")
            self.assert_queue_count(0)
            self.assert_test_topology_removed()
        finally:
            self.docker_cmd("rm", "-f", self.container, check=False)


if __name__ == "__main__":
    unittest.main(verbosity=2)
