import os
import sys
import tempfile
import types
import unittest

if 'pymysql' not in sys.modules:
    pymysql = types.ModuleType('pymysql')
    pymysql.cursors = types.SimpleNamespace(DictCursor=object)
    pymysql.MySQLError = Exception
    sys.modules['pymysql'] = pymysql
sys.path.insert(0, os.path.dirname(__file__))
import unifipolld  # noqa: E402


class FakeClient:
    def __init__(self, fail=False): self.fail = fail
    def get(self, path):
        if self.fail: raise OSError('timeout')
        if path.endswith('/sites'): return {'data': [{'id': 'site'}]}
        return {'data': [{'id': 'one', 'name': 'router', 'model': 'UDM Pro', 'online': True, 'statistics': {'rxBytes': 1000, 'txBytes': 2000}}]}


class UniFiPollTests(unittest.TestCase):
    def test_role_mapping(self):
        self.assertEqual('gateway', unifipolld.model_kind('UDM Pro Max'))
        self.assertEqual('switch', unifipolld.model_kind('USW Pro Max 16'))
        self.assertEqual('hub', unifipolld.model_kind('USW Ultra'))
        self.assertEqual('ap', unifipolld.model_kind('U7 Pro'))
        self.assertEqual('wanBackup', unifipolld.model_kind('U5G Backup'))

    def test_rate_derivation(self):
        self.assertEqual(8, unifipolld.derive_rate_kbps(1000, 11000, 10))
        self.assertEqual(0, unifipolld.derive_rate_kbps(11000, 1000, 10))

    def test_fail_soft_poll_error(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertFalse(unifipolld.poll_cycle(FakeClient(True), 'unused', os.path.join(directory, 'state.json'), True))


if __name__ == '__main__': unittest.main()
