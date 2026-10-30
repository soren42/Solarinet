"""Fault tests for the outbox checkpoint and publisher-confirm boundary."""
import sys
import types
import unittest
import configparser
from unittest.mock import patch

# The publishing decision needs no broker or DB client. Supply only the import
# shape on minimal CI runners; a production venv exercises the real pika class.
try:
    import pika  # noqa: F401
except ModuleNotFoundError:
    pika_stub = types.ModuleType("pika")
    pika_stub.BasicProperties = lambda **kwargs: types.SimpleNamespace(**kwargs)
    sys.modules["pika"] = pika_stub
try:
    import pymysql  # noqa: F401
except ModuleNotFoundError:
    sys.modules["pymysql"] = types.ModuleType("pymysql")

import sor_emitd


ROWS = [
    {"id": 11, "table_name": "entities", "row_id": 4, "action": "update", "ts": 1},
    {"id": 12, "table_name": "dns_records", "row_id": 8, "action": "insert", "ts": 2},
]


class FakeDb:
    def __init__(self, checkpoint=10, fail=False):
        self.checkpoint = checkpoint
        self.fail = fail
        self.writes = []

    def cursor(self):
        return self

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False

    def execute(self, _sql, params):
        if self.fail:
            raise RuntimeError("database disconnected")
        self.writes.append(params[0])
        self.checkpoint = params[0]


class FakeChannel:
    def __init__(self, fail_at=None):
        self.fail_at = fail_at
        self.published = []

    def basic_publish(self, **kwargs):
        self.published.append(kwargs)
        if len(self.published) == self.fail_at:
            raise RuntimeError("broker returned message or NACKed publish")


class PublishBoundaryTest(unittest.TestCase):
    def test_emitter_uses_its_separate_database_identity(self):
        config = configparser.ConfigParser()
        config.read_string("""
[sor]
host = base-host
port = 3306
user = read-only
password = read-secret
name = sor
[sor_emit]
host = emitter-host
port = 3307
user = emitter
password = emitter-secret
name = emitter_db
""")
        with patch.object(sor_emitd.pymysql, "connect", create=True) as connect:
            sor_emitd.db_connect(config)
        self.assertEqual("emitter", connect.call_args.kwargs["user"])
        self.assertEqual("emitter-secret", connect.call_args.kwargs["password"])
        self.assertEqual("emitter-host", connect.call_args.kwargs["host"])
        self.assertEqual(3307, connect.call_args.kwargs["port"])

    def test_confirmed_batch_advances_checkpoint_once(self):
        db, ch = FakeDb(), FakeChannel()
        self.assertEqual(12, sor_emitd.publish_batch(ch, db, ROWS, "sor.events", 10))
        self.assertEqual([12], db.writes)
        self.assertTrue(all(p["mandatory"] for p in ch.published))
        self.assertTrue(all(p["properties"].delivery_mode == 2 for p in ch.published))

    def test_broker_failure_replays_confirmed_prefix(self):
        db, first = FakeDb(), FakeChannel(fail_at=2)
        with self.assertRaises(RuntimeError):
            sor_emitd.publish_batch(first, db, ROWS, "sor.events", 10)
        self.assertEqual(10, db.checkpoint)
        self.assertEqual([], db.writes)
        second = FakeChannel()
        self.assertEqual(12, sor_emitd.publish_batch(second, db, ROWS, "sor.events", 10))
        self.assertEqual(2, len(second.published))
        self.assertEqual(12, db.checkpoint)

    def test_database_failure_does_not_report_success(self):
        db = FakeDb(fail=True)
        with self.assertRaises(RuntimeError):
            sor_emitd.publish_batch(FakeChannel(), db, ROWS, "sor.events", 10)
        self.assertEqual(10, db.checkpoint)

    def test_optional_tables_publish_unmandatory_and_advance(self):
        rows = ROWS + [{"id": 13, "table_name": "vlans", "row_id": 2, "action": "insert", "ts": 3}]
        db, ch = FakeDb(), FakeChannel()
        optional = sor_emitd.optional_tables(configparser.ConfigParser())
        self.assertEqual(13, sor_emitd.publish_batch(ch, db, rows, "sor.events", 10, optional))
        self.assertEqual([True, True, False], [p["mandatory"] for p in ch.published])
        self.assertEqual([13], db.writes)

    def test_optional_tables_config_override(self):
        config = configparser.ConfigParser()
        config.read_string("[amqp]\noptional_tables = vlans , \n")
        self.assertEqual(frozenset({"vlans"}), sor_emitd.optional_tables(config))
        config.read_string("[amqp]\noptional_tables =\n")
        self.assertEqual(frozenset(), sor_emitd.optional_tables(config))

    def test_empty_batch_keeps_checkpoint(self):
        db = FakeDb()
        self.assertEqual(10, sor_emitd.publish_batch(FakeChannel(), db, [], "sor.events", 10))
        self.assertEqual([], db.writes)


if __name__ == "__main__":
    unittest.main()
