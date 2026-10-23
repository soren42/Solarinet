#!/usr/bin/env python3
"""Local authbroker HTTP credential policy tests; no broker connection."""
import os
import sys
import base64
import hashlib
import subprocess
import struct
import tempfile
import unittest
import time

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

sys.path.insert(0, os.path.dirname(__file__))
import authbrokerd  # noqa: E402
import loopback_test  # noqa: E402


def _request():
    return {
        "v": authbrokerd.PROTO_VERSION,
        "kid": "test-key",
        "id": "a" * 32,
        "ts": int(time.time()),
        "ttl": 60,
        "action": "login",
        "subject": "jason",
        "detail": "SolariNet dashboard",
        "source_ip": "10.5.2.20",
        "nonce": base64.b64encode(b"n" * 32).decode("ascii"),
        "device": "tab5-desk",
    }


class ApprovalEnvelopeTests(unittest.TestCase):
    def setUp(self):
        self.signer = ec.generate_private_key(ec.SECP256R1())

    def test_request_signature_binds_every_display_field(self):
        signed = authbrokerd.sign_request(self.signer, _request())
        self.assertTrue(authbrokerd.verify_request_signature(
            self.signer.public_key(), signed
        ))
        for field in ("kid", "id", "action", "subject", "detail", "source_ip",
                      "nonce", "device", "ts", "ttl"):
            changed = dict(signed)
            value = changed[field]
            changed[field] = value + "x" if isinstance(value, str) else value + 1
            with self.subTest(field=field):
                self.assertFalse(authbrokerd.verify_request_signature(
                    self.signer.public_key(), changed
                ))

    def test_length_prefix_prevents_newline_boundary_collisions(self):
        left = _request()
        left["subject"], left["detail"] = "alice\nadmin", ""
        right = _request()
        right["subject"], right["detail"] = "alice", "admin\n"
        self.assertNotEqual(authbrokerd.canonical_request(left),
                            authbrokerd.canonical_request(right))

    def test_canonical_request_accepts_empty_optional_and_maximum_fields(self):
        request = {
            "v": 2, "kid": "primary",
            "id": "0123456789abcdef0123456789abcdef",
            "ts": 1761220800, "ttl": 60, "action": "login",
            "subject": "s" * 128, "detail": "d" * 256, "source_ip": "",
            "nonce": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=", "device": "",
        }
        canonical = authbrokerd.canonical_request(request)
        fields = authbrokerd._request_fields(request)
        offset = len(authbrokerd.REQUEST_DOMAIN)
        for field in fields:
            size = struct.unpack(">I", canonical[offset:offset + 4])[0]
            offset += 4
            encoded = field.encode("utf-8")
            self.assertEqual(size, len(encoded))
            self.assertEqual(canonical[offset:offset + size], encoded)
            offset += size
        self.assertEqual(offset, len(canonical))

        request["subject"] = "s" * 129
        with self.assertRaisesRegex(ValueError, "invalid request field: subject"):
            authbrokerd.canonical_request(request)

    def test_canonical_v2_matches_firmware_golden_vectors(self):
        request = {
            "v": 2,
            "kid": "test-key",
            "id": "0123456789abcdef0123456789abcdef",
            "ts": 1761220800,
            "ttl": 60,
            "action": "login",
            "subject": "alice\nadmin",
            "detail": "SolariNet π",
            "source_ip": "10.0.0.7",
            "nonce": "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=",
            "device": "tab5-desk",
        }
        request_expected = (
            "534f4c4152492d415554482d524551554553540002000000013200000008746573742d6b65790000002030313233343536373839"
            "616263646566303132333435363738396162636465660000000a3137363132323038303000000002"
            "3630000000056c6f67696e0000000b616c6963650a61646d696e0000000c536f6c6172694e657420"
            "cf800000000831302e302e302e370000002c41414141414141414141414141414141414141414141"
            "4141414141414141414141414141414141414141413d00000009746162352d6465736b"
        )
        self.assertEqual(request_expected, authbrokerd.canonical_request(request).hex())
        response_expected = (
            "534f4c4152492d415554482d524553504f4e53450002000000013200000020303132333435363738"
            "396162636465663031323334353637383961626364656600000007617070726f76650000002c4141"
            "41414141414141414141414141414141414141414141414141414141414141414141414141414141"
            "413d00000009746162352d6465736b00000040313131313131313131313131313131313131313131"
            "31313131313131313131313131313131313131313131313131313131313131313131313131313131"
            "313131"
        )
        self.assertEqual(response_expected, authbrokerd.canonical_response(
            2, request["id"], "approve", request["nonce"], "tab5-desk", "1" * 64
        ).hex())

    def test_unsigned_broker_cannot_publish_approval_requests(self):
        broker = authbrokerd.Broker({"devices": {}, "request_signing_key": None})
        with self.assertRaisesRegex(RuntimeError, "signing key"):
            broker.request("login", "jason")

    def test_request_publish_uses_configured_key_id_and_signature(self):
        signer = ec.generate_private_key(ec.SECP256R1())

        class CaptureClient:
            def __init__(self):
                self.published = None

            def publish(self, topic, payload, qos):
                self.published = (topic, payload, qos)

        client = CaptureClient()
        broker = authbrokerd.Broker({
            "devices": {}, "topic_prefix": "auth", "request_ttl_sec": 1,
            "request_signing_key": signer,
            "request_signing_key_id": "rotation_2",
        })
        broker._client = client
        broker._connected.set()
        result = broker.request("login", "jason", ttl=1)
        self.assertEqual(result["decision"], "timeout")
        topic, payload, qos = client.published
        envelope = authbrokerd.json.loads(payload)
        self.assertEqual(topic, f"auth/request/{envelope['id']}")
        self.assertEqual(qos, 1)
        self.assertEqual(envelope["kid"], "rotation_2")
        self.assertTrue(authbrokerd.verify_request_signature(
            signer.public_key(), envelope
        ))

    def test_request_fails_without_connected_mqtt_and_leaves_no_pending_state(self):
        signer = ec.generate_private_key(ec.SECP256R1())
        broker = authbrokerd.Broker({
            "devices": {}, "request_signing_key": signer,
            "request_signing_key_id": "primary",
        })
        with self.assertRaisesRegex(RuntimeError, "not connected"):
            broker.request("login", "jason")
        self.assertFalse(broker._pending)

    def test_enrollment_tool_accepts_only_device_public_point(self):
        key = ec.generate_private_key(ec.SECP256R1())
        point = key.public_key().public_bytes(
            serialization.Encoding.X962,
            serialization.PublicFormat.UncompressedPoint,
        ).hex()
        with tempfile.TemporaryDirectory() as td:
            point_path = os.path.join(td, "device-public-point.txt")
            with open(point_path, "w", encoding="ascii") as fh:
                fh.write(point + "\n")
            result = subprocess.run(
                [sys.executable, os.path.join(os.path.dirname(__file__),
                 "enroll_device.py"), "tab5-desk", "--public-point-hex-file",
                 point_path], capture_output=True, text=True, check=True,
            )
        self.assertIn("[device.tab5-desk]", result.stdout)
        self.assertIn("BEGIN PUBLIC KEY", result.stdout)
        self.assertNotIn("PRIVATE", result.stdout)

    def test_config_requires_valid_request_signing_key_id(self):
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
        with tempfile.TemporaryDirectory() as td:
            key_path = os.path.join(td, "request-key.pem")
            config_path = os.path.join(td, "authbroker.conf")
            with open(key_path, "wb") as fh:
                fh.write(pem)
            os.chmod(key_path, 0o600)
            for key_id, expected in (("primary", "primary"), ("BAD ID", None)):
                with open(config_path, "w", encoding="utf-8") as fh:
                    fh.write("[http]\nport = 9444\n\n")
                    fh.write("[broker]\n")
                    fh.write(f"request_signing_key_id = {key_id}\n")
                    fh.write(f"request_signing_key_file = {key_path}\n")
                if expected:
                    cfg = authbrokerd.load_config(config_path)
                    self.assertEqual(cfg["request_signing_key_id"], expected)
                else:
                    with self.assertRaisesRegex(SystemExit, "request_signing_key_id"):
                        authbrokerd.load_config(config_path)

    def test_response_must_bind_to_pending_request_digest(self):
        device_key = ec.generate_private_key(ec.SECP256R1())
        broker = authbrokerd.Broker({"devices": {"tab5-desk": device_key.public_key()}})
        req_id = "b" * 32
        nonce = "fixture-nonce"
        expected_digest = "1" * 64
        pending = authbrokerd.PendingRequest(
            req_id, nonce, "tab5-desk", 60, expected_digest
        )
        broker._pending[req_id] = pending

        for digest in ("2" * 64, expected_digest):
            body = {
                "v": authbrokerd.PROTO_VERSION,
                "id": req_id,
                "decision": "approve",
                "device_id": "tab5-desk",
                "nonce": nonce,
                "req_digest": digest,
            }
            canonical = authbrokerd.canonical_response(
                body["v"], req_id, body["decision"], nonce,
                body["device_id"], digest,
            )
            body["sig"] = base64.b64encode(
                device_key.sign(canonical, ec.ECDSA(hashes.SHA256()))
            ).decode("ascii")
            broker._handle_response(
                f"auth/response/{req_id}",
                authbrokerd.json.dumps(body).encode("utf-8"),
            )
            self.assertEqual(digest == expected_digest, pending.event.is_set())

    def test_response_from_wrong_device_is_rejected(self):
        target_key = ec.generate_private_key(ec.SECP256R1())
        other_key = ec.generate_private_key(ec.SECP256R1())
        broker = authbrokerd.Broker({
            "devices": {"tab5-desk": target_key.public_key(),
                        "tab5-other": other_key.public_key()}
        })
        req_id, nonce, digest = "c" * 32, "nonce", "2" * 64
        pending = authbrokerd.PendingRequest(req_id, nonce, "tab5-desk", 60, digest)
        broker._pending[req_id] = pending
        body = self._signed_response(other_key, req_id, nonce, "tab5-other", digest)
        broker._handle_response(f"auth/response/{req_id}",
                                authbrokerd.json.dumps(body).encode())
        self.assertFalse(pending.event.is_set())

    def test_expired_response_is_rejected_and_resolved_request_ignores_replay(self):
        device_key = ec.generate_private_key(ec.SECP256R1())
        broker = authbrokerd.Broker({"devices": {"tab5-desk": device_key.public_key()}})
        req_id, nonce, digest = "d" * 32, "nonce", "3" * 64
        pending = authbrokerd.PendingRequest(req_id, nonce, "tab5-desk", 1, digest)
        pending.created -= 2
        broker._pending[req_id] = pending
        body = self._signed_response(device_key, req_id, nonce, "tab5-desk", digest)
        payload = authbrokerd.json.dumps(body).encode()
        broker._handle_response(f"auth/response/{req_id}", payload)
        self.assertFalse(pending.event.is_set())

        pending.created = authbrokerd.time.time()
        broker._handle_response(f"auth/response/{req_id}", payload)
        self.assertTrue(pending.event.is_set())
        result = dict(pending.result)
        replay = dict(body, decision="deny")
        replay["sig"] = base64.b64encode(
            device_key.sign(
                authbrokerd.canonical_response(
                    authbrokerd.PROTO_VERSION, req_id, "deny", nonce,
                    "tab5-desk", digest,
                ),
                ec.ECDSA(hashes.SHA256()),
            )
        ).decode("ascii")
        broker._handle_response(f"auth/response/{req_id}",
                                authbrokerd.json.dumps(replay).encode())
        self.assertEqual(pending.result, result)

    @staticmethod
    def _signed_response(device_key, req_id, nonce, device_id, digest):
        body = {
            "v": authbrokerd.PROTO_VERSION, "id": req_id,
            "decision": "approve", "device_id": device_id,
            "nonce": nonce, "req_digest": digest,
        }
        canonical = authbrokerd.canonical_response(
            body["v"], req_id, body["decision"], nonce, device_id, digest
        )
        body["sig"] = base64.b64encode(
            device_key.sign(canonical, ec.ECDSA(hashes.SHA256()))
        ).decode("ascii")
        return body

    def test_request_signing_key_requires_owner_only_permissions(self):
        pem = self.signer.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.PKCS8,
            serialization.NoEncryption(),
        )
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "request-signing.pem")
            with open(path, "wb") as fh:
                fh.write(pem)
            os.chmod(path, 0o644)
            with self.assertRaisesRegex(ValueError, "owner-only"):
                authbrokerd.load_request_signing_key(path)
            os.chmod(path, 0o600)
            loaded = authbrokerd.load_request_signing_key(path)
            self.assertEqual("secp256r1", loaded.curve.name)

    def test_mqtt_requires_tls_outside_explicit_loopback_development(self):
        with self.assertRaisesRegex(ValueError, "loopback"):
            authbrokerd.validate_mqtt_transport(
                "10.5.2.50", False, "", True, "solari", "secret"
            )
        authbrokerd.validate_mqtt_transport(
            "127.0.0.1", False, "", True, "", ""
        )
        with self.assertRaisesRegex(ValueError, "CA certificate"):
            authbrokerd.validate_mqtt_transport(
                "benzene.akoria.net", True, "", False, "solari", "secret"
            )
        with self.assertRaisesRegex(ValueError, "credentials"):
            authbrokerd.validate_mqtt_transport(
                "benzene.akoria.net", True, "/etc/ssl/certs/ca-certificates.crt",
                False, "", "",
            )


class HttpTokenTests(unittest.TestCase):
    def test_missing_or_example_token_is_rejected_at_startup(self):
        for token in ("", "   ", "CHANGE_ME_LONG_RANDOM"):
            with self.subTest(token=token):
                with self.assertRaises(ValueError):
                    authbrokerd.validate_http_token(token)

    def test_non_example_token_is_accepted_at_startup(self):
        authbrokerd.validate_http_token("a-long-random-secret")

    def test_empty_configuration_never_authorizes(self):
        self.assertFalse(authbrokerd.http_token_authorized("", "Bearer anything"))
        self.assertFalse(authbrokerd.http_token_authorized("   ", "Bearer anything"))

    def test_bearer_header_must_match_configured_token(self):
        token = "a-long-random-secret"
        self.assertTrue(authbrokerd.http_token_authorized(token, f"Bearer {token}"))
        self.assertFalse(authbrokerd.http_token_authorized(token, ""))
        self.assertFalse(authbrokerd.http_token_authorized(token, "Bearer wrong"))
        self.assertFalse(authbrokerd.http_token_authorized(token, token))


class LoopbackDefaultsTests(unittest.TestCase):
    def test_loopback_defaults_cannot_target_live_broker_or_credentials(self):
        args = loopback_test.parse_args([])
        self.assertEqual("127.0.0.1", args.host)
        self.assertEqual(1883, args.port)
        self.assertEqual("", args.username)
        self.assertEqual("", args.password)

    def test_explicit_fixture_broker_is_supported(self):
        args = loopback_test.parse_args([
            "--host", "127.0.0.1", "--port", "18883",
            "--username", "fixture", "--password", "fixture-only",
        ])
        self.assertEqual(("127.0.0.1", 18883, "fixture", "fixture-only"),
                         (args.host, args.port, args.username, args.password))


if __name__ == "__main__":
    unittest.main()
