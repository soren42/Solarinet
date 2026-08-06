#!/usr/bin/env python3
"""Focused D4/A11-A13 checks; deliberately no live DB or MQ dependency."""
import os
import sys
import types
import unittest

sys.path.insert(0, os.path.dirname(__file__))

# The production bridge imports its DB/MQ clients at module load. These tests
# replace them before import so the disposition and checkpoint contract remains
# runnable on a bare Python installation.
if 'pika' not in sys.modules:
    pika = types.ModuleType('pika')
    exceptions = types.ModuleType('pika.exceptions')
    for name in ('UnroutableError', 'NackError', 'AMQPError'):
        setattr(exceptions, name, type(name, (Exception,), {}))
    pika.exceptions = exceptions
    pika.BasicProperties = object
    sys.modules['pika'] = pika
    sys.modules['pika.exceptions'] = exceptions
if 'pymysql' not in sys.modules:
    pymysql = types.ModuleType('pymysql')
    pymysql.cursors = types.SimpleNamespace(DictCursor=object)
    sys.modules['pymysql'] = pymysql

import alertbridge  # noqa: E402


class FakeCursor:
    def __init__(self, db):
        self.db = db
        self.params = ()
        self.query = ""

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        return False

    def execute(self, query, params=()):
        self.query = query
        self.params = params

    def fetchone(self):
        return {'count': 2}

    def fetchall(self):
        if 'FROM alertEvent e' not in self.query:
            return []
        checkpoint = int(self.params[0])
        return [row for row in self.db.rows if int(row['eventId']) > checkpoint]


class FakeDb:
    def __init__(self, rows):
        self.rows = rows

    def cursor(self):
        return FakeCursor(self)


def row(event_id, disposition=None, event_kind=None):
    return {
        'eventId': event_id, 'nodeId': None, 'targetId': None,
        'severity': 'crit', 'detail': None, 'hostFqdn': 'test',
        'metric': None, 'disposition': disposition, 'eventKind': event_kind,
    }


class DispositionTests(unittest.TestCase):
    def setUp(self):
        self.original_publish = alertbridge.publish
        self.original_save_state = alertbridge.save_state
        alertbridge._disposition_columns_present = None
        self.published = []
        alertbridge.publish = lambda ch, exchange, msg: self.published.append(msg['body'])

    def tearDown(self):
        alertbridge.publish = self.original_publish
        alertbridge.save_state = self.original_save_state

    def bridge(self, rows, state, saves):
        pending = iter(saves)
        alertbridge.save_state = lambda path, value: next(pending)
        return alertbridge.bridge_alerts(
            FakeDb(rows), object(), 'exchange', 'source', state, 32, '/unused', (set(), set(), False)
        )

    def test_a11_suppression_persists_before_later_publish_failure(self):
        rows = [row(1, 'suppress', 'fired'), row(2, 'publish', 'fired')]
        state = {'checkpoint': 0}
        self.bridge(rows, state, [True, False])
        self.assertEqual(1, state['checkpoint'])
        self.assertEqual(['eventId=2'], self.published)

        # On restart event 1 is already terminal and is never sent to MQ; event
        # 2 replays because its broker-confirmed delivery was not checkpointed.
        self.bridge(rows, state, [True])
        self.assertEqual(2, state['checkpoint'])
        self.assertEqual(['eventId=2', 'eventId=2'], self.published)

    def test_a12_cleared_rows_mirror_the_opened_incident_disposition(self):
        self.assertEqual('suppress', alertbridge.resolve_outcome(row(10, 'suppress', 'fired'), (set(), set(), False)))
        self.assertEqual('suppress', alertbridge.resolve_outcome(row(11, 'suppress', 'cleared'), (set(), set(), False)))
        self.assertEqual('publish', alertbridge.resolve_outcome(row(20, 'publish', 'fired'), (set(), set(), False)))
        self.assertEqual('publish', alertbridge.resolve_outcome(row(21, 'publish', 'cleared'), (set(), set(), False)))

        rows = [row(10, 'suppress', 'fired'), row(11, 'suppress', 'cleared'),
                row(20, 'publish', 'fired'), row(21, 'publish', 'cleared')]
        state = {'checkpoint': 0}
        self.bridge(rows, state, [True, True, True, True])
        self.assertEqual(['eventId=20', 'eventId=21'], self.published)
        self.bridge(rows, state, [])
        self.assertEqual(['eventId=20', 'eventId=21'], self.published)

    def test_a13_audit_always_wins_over_publish_disposition(self):
        state = {'checkpoint': 0}
        self.bridge([row(30, 'publish', 'audit')], state, [True])
        self.assertEqual(30, state['checkpoint'])
        self.assertEqual([], self.published)

    def test_legacy_null_disposition_keeps_maintenance_behavior(self):
        legacy = row(40)
        self.assertEqual('publish', alertbridge.resolve_outcome(legacy, (set(), set(), False)))
        legacy['nodeId'] = 7
        self.assertEqual('suppress', alertbridge.resolve_outcome(legacy, ({'7'}, set(), False)))

    def test_m6_maintenance_gates_publish_dispositions_too(self):
        # Review R2-S1: disposition == 'publish' is a CANDIDATE; a host under
        # an active maintenance window stays suppressed regardless of tier.
        # (Round-1 M6 was exactly this short-circuit; pin it.)
        candidate = row(50, 'publish', 'fired')
        candidate['nodeId'] = 7
        self.assertEqual('suppress', alertbridge.resolve_outcome(candidate, ({'7'}, set(), False)))
        self.assertEqual('publish', alertbridge.resolve_outcome(candidate, (set(), set(), False)))
        # explicit suppress must never be resurrected by an empty window set
        self.assertEqual('suppress', alertbridge.resolve_outcome(row(51, 'suppress', 'fired'), (set(), set(), False)))


if __name__ == '__main__':
    unittest.main()
