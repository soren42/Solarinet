import json
import os
import shutil
import ssl
import subprocess
import sys
import threading
import tempfile
import types
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import URLError

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
        if '/statistics/latest' in path: raise OSError('unsupported stats')
        if '/stat/device' in path: raise OSError('classic endpoint unavailable')
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

    def test_classic_port_rates_topology_and_cycles(self):
        devices = [
            {'mac': 'aa:00', 'model': 'USW Pro Max', 'uplink': {'uplink_mac': 'bb:00'},
             'port_table': [{'port_idx': 7, 'name': 'Uplink', 'is_uplink': True, 'up': True,
                             'rx_bytes-r': 1250, 'tx_bytes-r': 2500, 'rx_bytes': 10000, 'tx_bytes': 20000}]},
            {'mac': 'bb:00', 'model': 'UDM Pro', 'uplink': {'uplink_mac': 'aa:00', 'rx_rate': 1200000, 'tx_rate': 3400000},
             'port_table': []},
        ]
        rows = unifipolld.classic_device_rows(devices)
        port = rows['unifi-aa:00']['ports'][0]
        self.assertEqual(10, port['inRateKbps'])
        self.assertEqual(20, port['outRateKbps'])
        self.assertEqual(7, port['ifIndex'])
        self.assertEqual(1, port['isUplink'])
        self.assertEqual(1200, rows['unifi-bb:00']['wanInRateKbps'])
        self.assertEqual('unifi-bb:00', rows['unifi-aa:00']['uplinkGearId'])
        self.assertIsNone(rows['unifi-bb:00']['uplinkGearId'])

    def test_classic_failure_does_not_stop_integration_path(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = unifipolld.poll_once(FakeClient(), 'unused', os.path.join(directory, 'state.json'), True)
            self.assertEqual(1, len(rows))
            self.assertEqual('unifi-one', rows[0]['gearId'])
            self.assertEqual([], rows[0]['ports'])

    def test_fail_soft_poll_error(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertFalse(unifipolld.poll_cycle(FakeClient(True), 'unused', os.path.join(directory, 'state.json'), True))

    @unittest.skipUnless(shutil.which('openssl'), 'openssl is required for the local TLS fixture')
    def test_connect_ip_preserves_tls_hostname_and_verification(self):
        with tempfile.TemporaryDirectory() as directory:
            cert = os.path.join(directory, 'controller.crt')
            key = os.path.join(directory, 'controller.key')
            subprocess.run([
                'openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes', '-days', '1',
                '-keyout', key, '-out', cert, '-subj', '/CN=controller.test',
                '-addext', 'subjectAltName=DNS:controller.test',
            ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

            class Handler(BaseHTTPRequestHandler):
                def do_GET(self):
                    body = json.dumps({
                        'host': self.headers.get('Host'),
                        'key_present': self.headers.get('X-API-KEY') == 'fixture-key',
                    }).encode()
                    self.send_response(200)
                    self.send_header('Content-Type', 'application/json')
                    self.send_header('Content-Length', str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)

                def log_message(self, *_args):
                    pass

            server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(cert, key)
            server.socket = context.wrap_socket(server.socket, server_side=True)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                url = f'https://controller.test:{server.server_port}'
                client = unifipolld.UniFiClient(url, 'fixture-key', cert, connect_ip='127.0.0.1')
                result = client.get('/health')
                self.assertEqual(f'controller.test:{server.server_port}', result['host'])
                self.assertTrue(result['key_present'])

                wrong_name = unifipolld.UniFiClient(
                    f'https://wrong.test:{server.server_port}', 'fixture-key', cert,
                    connect_ip='127.0.0.1')
                with self.assertRaises(URLError):
                    wrong_name.get('/health')
            finally:
                server.shutdown()
                server.server_close()
                thread.join(timeout=2)

    def test_connect_ip_rejects_hostnames(self):
        with self.assertRaisesRegex(ValueError, 'UNIFI_CONNECT_IP'):
            unifipolld.UniFiClient('https://controller.test', 'fixture-key', connect_ip='controller.test')


if __name__ == '__main__': unittest.main()
