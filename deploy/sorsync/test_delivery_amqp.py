"""Real broker proof; SOLARI_TEST_AMQP_URL must identify an isolated broker."""
import os
import time
import unittest
import uuid
import json
from unittest import mock

try:
    import pika
except ModuleNotFoundError:
    pika = None

import delivery


@unittest.skipUnless(os.environ.get("SOLARI_TEST_AMQP_URL"), "isolated AMQP URL not configured")
class BrokerDeliveryTest(unittest.TestCase):
    def connect(self):
        parameters = pika.URLParameters(os.environ["SOLARI_TEST_AMQP_URL"])
        parameters.heartbeat = 2
        parameters.socket_timeout = 3
        parameters.blocked_connection_timeout = 3
        return pika.BlockingConnection(parameters)

    def setUp(self):
        if pika is None or not hasattr(pika, "BlockingConnection"):
            self.fail("real Pika is required for broker integration tests")
        self.connection = self.connect()
        self.channel = self.connection.channel()
        self.queue = "solarinet.delivery.test." + uuid.uuid4().hex
        self.channel.queue_declare(queue=self.queue, durable=True, auto_delete=False)
        self.exchange = None

    def tearDown(self):
        if not self.connection.is_open:
            self.connection = self.connect()
        channel = self.connection.channel()
        channel.queue_delete(queue=self.queue)
        if self.exchange:
            channel.exchange_delete(exchange=self.exchange)
        self.connection.close()

    def test_slow_apply_keeps_negotiated_heartbeat_and_acks_after_success(self):
        for _ in range(3):
            self.channel.basic_publish(exchange="", routing_key=self.queue, body=b"event")
        calls, pumps = [], []
        running = True
        def apply():
            nonlocal running
            calls.append(True)
            if len(calls) == 2:
                time.sleep(5)                   # exceeds two heartbeat intervals
                running = False
            return True
        def maintain():
            pumps.append(True)
            self.connection.process_data_events(time_limit=0.1)
        delivery.consume_after_apply(self.channel, self.queue, 0.1, 3600, apply,
                                     lambda: running, maintain_connection=maintain)
        self.assertTrue(self.connection.is_open)
        self.assertTrue(pumps)
        self.assertEqual(2, len(calls))
        self.channel.cancel()
        method, _, _ = self.channel.basic_get(self.queue, auto_ack=True)
        self.assertIsNone(method)

    def test_failed_apply_is_redelivered_after_connection_close(self):
        self.channel.basic_publish(exchange="", routing_key=self.queue, body=b"retry-me")
        calls = []
        def apply():
            calls.append(True)
            return len(calls) == 1              # startup succeeds, event apply fails
        with self.assertRaises(RuntimeError):
            delivery.consume_after_apply(self.channel, self.queue, 0.1, 3600, apply,
                                         lambda: True, maintain_connection=lambda:
                                         self.connection.process_data_events(time_limit=0.1))
        self.connection.close()                # broker requeues the unacked delivery
        self.connection = self.connect()
        self.channel = self.connection.channel()
        method, _, body = self.channel.basic_get(self.queue, auto_ack=True)
        self.assertIsNotNone(method)
        self.assertTrue(method.redelivered)
        self.assertEqual(b"retry-me", body)

    def test_mandatory_route_failure_cannot_advance_outbox_checkpoint(self):
        import sor_emitd
        self.exchange = "solarinet.delivery.test." + uuid.uuid4().hex
        self.channel.exchange_declare(exchange=self.exchange, exchange_type="topic", durable=True)
        self.channel.queue_bind(queue=self.queue, exchange=self.exchange, routing_key="sor.entities.insert")
        self.channel.confirm_delivery()
        rows = [{"id": 1, "table_name": "entities", "row_id": 1, "action": "insert", "ts": 1},
                {"id": 2, "table_name": "interfaces", "row_id": 2, "action": "insert", "ts": 2}]
        with mock.patch.object(sor_emitd, "set_checkpoint") as checkpoint:
            with self.assertRaises(pika.exceptions.UnroutableError):
                sor_emitd.publish_batch(self.channel, object(), rows, self.exchange, 0)
            checkpoint.assert_not_called()
            _, _, prefix = self.channel.basic_get(self.queue, auto_ack=True)
            self.assertEqual(1, json.loads(prefix)["outbox_id"])
            self.channel.queue_bind(queue=self.queue, exchange=self.exchange,
                                    routing_key="sor.interfaces.insert")
            self.assertEqual(2, sor_emitd.publish_batch(self.channel, object(), rows, self.exchange, 0))
            checkpoint.assert_called_once()
            ids = []
            for _ in range(2):
                _, _, body = self.channel.basic_get(self.queue, auto_ack=True)
                ids.append(json.loads(body)["outbox_id"])
            self.assertEqual([1, 2], ids)        # confirmed prefix safely repeats


if __name__ == "__main__":
    unittest.main()
