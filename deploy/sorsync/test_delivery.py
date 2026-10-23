"""Consumer fault tests: a target failure must leave its deliveries unacked."""
import types
import threading
import unittest
from unittest import mock

import delivery


class FakeChannel:
    def __init__(self, sequence):
        self.sequence = sequence
        self.acked = []
        self.prefetch = None

    def basic_qos(self, prefetch_count):
        self.prefetch = prefetch_count

    def consume(self, _queue, inactivity_timeout):
        assert inactivity_timeout > 0
        for tag in self.sequence:
            yield ((None if tag is None else types.SimpleNamespace(delivery_tag=tag)),
                   None, None)

    def basic_ack(self, delivery_tag):
        self.acked.append(delivery_tag)


class DeliveryBoundaryTest(unittest.TestCase):
    def test_ack_happens_after_successful_batch_apply(self):
        ch = FakeChannel([1, 2, None])
        observed = []

        def apply():
            observed.append(list(ch.acked))
            return True

        delivery.consume_after_apply(ch, "target", 1, 3600, apply, lambda: True)
        self.assertEqual(100, ch.prefetch)
        self.assertEqual([[], []], observed)  # startup, then batch before any ack
        self.assertEqual([1, 2], ch.acked)

    def test_failed_apply_keeps_entire_batch_unacked(self):
        ch = FakeChannel([1, 2, None])
        calls = 0

        def apply():
            nonlocal calls
            calls += 1
            if calls == 2:
                raise RuntimeError("DNS reload failed")
            return True

        with self.assertRaisesRegex(RuntimeError, "DNS reload failed"):
            delivery.consume_after_apply(ch, "target", 1, 3600, apply, lambda: True)
        self.assertEqual([], ch.acked)

    def test_partial_target_result_is_failure(self):
        ch = FakeChannel([])
        with self.assertRaisesRegex(RuntimeError, "did not confirm"):
            delivery.consume_after_apply(ch, "target", 1, 3600,
                                         lambda: False, lambda: True)
        self.assertEqual([], ch.acked)

    def test_batch_cap_flushes_without_idle_gap(self):
        ch = FakeChannel([1, 2])
        calls = []
        delivery.consume_after_apply(ch, "target", 1, 3600,
                                     lambda: calls.append(True) or True,
                                     lambda: True, max_pending=2)
        self.assertEqual(2, len(calls))  # startup and full batch
        self.assertEqual([1, 2], ch.acked)

    def test_periodic_reconcile_when_no_events_arrive(self):
        ch = FakeChannel([None])
        calls = []
        with mock.patch.object(delivery.time, "monotonic", side_effect=[10, 16, 17]):
            delivery.consume_after_apply(ch, "target", 1, 5,
                                         lambda: calls.append(True) or True,
                                         lambda: True)
        self.assertEqual(2, len(calls))
        self.assertEqual([], ch.acked)

    def test_long_apply_keeps_broker_on_owner_thread_before_ack(self):
        ch = FakeChannel([1, None])
        owner = threading.get_ident()
        started, proceed = threading.Event(), threading.Event()
        calls = []
        heartbeats = []
        def apply():
            self.assertNotEqual(owner, threading.get_ident())
            calls.append(True)
            if len(calls) == 2:
                started.set()
                self.assertTrue(proceed.wait(2))
            return True
        def maintain():
            self.assertEqual(owner, threading.get_ident())
            heartbeats.append(True)
            if started.is_set():
                self.assertEqual([], ch.acked)
                proceed.set()
        delivery.consume_after_apply(ch, "target", 1, 3600, apply, lambda: True,
                                     maintain_connection=maintain)
        self.assertTrue(heartbeats)
        self.assertEqual([1], ch.acked)

    def test_broker_loss_waits_for_inflight_apply_and_does_not_ack(self):
        ch = FakeChannel([1, None])
        started, finish, completed = threading.Event(), threading.Event(), threading.Event()
        calls = []
        def apply():
            calls.append(True)
            if len(calls) == 2:
                started.set()
                self.assertTrue(finish.wait(2))
                completed.set()
            return True
        def maintain():
            if started.is_set():
                finish.set()
                raise RuntimeError("broker connection lost")
        with self.assertRaisesRegex(RuntimeError, "broker connection lost"):
            delivery.consume_after_apply(ch, "target", 1, 3600, apply, lambda: True,
                                         maintain_connection=maintain)
        self.assertTrue(completed.is_set())
        self.assertEqual([], ch.acked)


if __name__ == "__main__":
    unittest.main()
