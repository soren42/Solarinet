#!/usr/bin/env python3
"""authbrokerd — SolariNet Authenticator approval broker.

Bridges dashboard / Keycloak authentication events to the M5Stack Tab5
("SolariNet Authenticator") over MQTT and waits for a push-approval decision.

    dashboard login  --HTTP-->  authbrokerd  --MQTT-->  Tab5  (Approve / Deny)
         proceeds    <--HTTP--  authbrokerd  <--MQTT--  Tab5  (signed response)

The broker is deliberately dependency-light: MQTT via paho, HTTP via the stdlib,
signature verification via `cryptography`. It is the ONLY component that needs to
be reachable by both the dashboard (HTTP) and the broker (MQTT); the Tab5 never
talks to the dashboard directly.

Message contract and signing scheme: see README.md. In short:

  request  auth/request/<id>   (broker -> device, ECDSA-P256/SHA-256 signed)
  response auth/response/<id>  (device -> broker, signed and request-bound)

Both sides use domain-separated, length-prefixed UTF-8 canonical fields; see
deploy/authbroker/README.md and docs/rework/F06-approval-binding-contract.md.

The broker holds each device's PUBLIC key and a separate request-signing key.
Compromise of that broker key can forge prompts, but cannot forge an approval:
only the Tab5 holds its response-signing private key in encrypted NVS.

Run:
    authbrokerd.py --config authbroker.conf         # the daemon
    authbrokerd.py --config authbroker.conf --check # config + key sanity, no serve
See loopback_test.py for a live round-trip test against the real Mosquitto.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import configparser
import hashlib
import ipaddress
import json
import logging
import os
import re
import secrets
import stat
import struct
import sys
import threading
import time
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import paho.mqtt.client as mqtt
except ImportError:  # pragma: no cover - surfaced at runtime
    mqtt = None

try:
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.exceptions import InvalidSignature
except ImportError:  # pragma: no cover
    ec = None

log = logging.getLogger("authbrokerd")

PROTO_VERSION = 2
REQUEST_DOMAIN = b"SOLARI-AUTH-REQUEST\x00\x02"
RESPONSE_DOMAIN = b"SOLARI-AUTH-RESPONSE\x00\x02"


def http_token_authorized(token: str, authorization: str) -> bool:
    """Authorize a bearer request only when a non-empty token is configured."""
    if not token or not token.strip():
        return False
    return secrets.compare_digest(authorization, f"Bearer {token}")


def validate_http_token(token: str) -> None:
    """Refuse to start the HTTP service with an empty or example token."""
    if not token or not token.strip() or token.strip() == "CHANGE_ME_LONG_RANDOM":
        raise ValueError("[http] token must be set to a non-example secret")


# --------------------------------------------------------------------------- #
# Signing helpers (shared with the device simulator in loopback_test.py)
# --------------------------------------------------------------------------- #
def _canonical_fields(domain: bytes, fields: list[str]) -> bytes:
    """Encode UTF-8 fields with explicit lengths to avoid delimiter ambiguity."""
    out = bytearray(domain)
    for value in fields:
        raw = value.encode("utf-8")
        if len(raw) > 0xFFFFFFFF:
            raise ValueError("canonical field too large")
        out.extend(struct.pack(">I", len(raw)))
        out.extend(raw)
    return bytes(out)


def _request_fields(envelope: dict) -> list[str]:
    if type(envelope.get("v")) is not int or envelope["v"] != PROTO_VERSION:
        raise ValueError("unsupported request version")
    if type(envelope.get("ts")) is not int or envelope["ts"] <= 0:
        raise ValueError("invalid request timestamp")
    if type(envelope.get("ttl")) is not int or not 1 <= envelope["ttl"] <= 120:
        raise ValueError("invalid request TTL")
    limits = {
        "kid": 14, "id": 32, "action": 32, "subject": 128, "detail": 256,
        "source_ip": 64, "nonce": 64, "device": 64,
    }
    values: dict[str, str] = {}
    for name, max_bytes in limits.items():
        value = envelope.get(name, "" if name == "device" else None)
        if not isinstance(value, str) or len(value.encode("utf-8")) > max_bytes:
            raise ValueError(f"invalid request field: {name}")
        values[name] = value
    if len(values["id"]) != 32 or any(c not in "0123456789abcdef" for c in values["id"]):
        raise ValueError("invalid request id")
    if not re.fullmatch(r"[a-z0-9_-]{1,14}", values["kid"]):
        raise ValueError("invalid broker key ID")
    if not values["action"] or not values["subject"] or not values["nonce"]:
        raise ValueError("missing request field")
    return [
        str(envelope["v"]), values["kid"], values["id"], str(envelope["ts"]),
        str(envelope["ttl"]), values["action"], values["subject"],
        values["detail"], values["source_ip"], values["nonce"], values["device"],
    ]


def canonical_request(envelope: dict) -> bytes:
    """Canonical bytes authenticated before the device renders a request."""
    return _canonical_fields(REQUEST_DOMAIN, _request_fields(envelope))


def sign_request(private_key, envelope: dict) -> dict:
    """Return a request envelope with a broker ECDSA-P256 signature."""
    signed = dict(envelope)
    signed["sig"] = base64.b64encode(
        private_key.sign(canonical_request(signed), ec.ECDSA(hashes.SHA256()))
    ).decode("ascii")
    return signed


def verify_request_signature(public_key, envelope: dict) -> bool:
    try:
        signature = base64.b64decode(envelope["sig"], validate=True)
        public_key.verify(signature, canonical_request(envelope), ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError, TypeError, KeyError, binascii.Error):
        return False


def canonical_response(version: int, req_id: str, decision: str,
                       nonce: str, device_id: str, req_digest: str) -> bytes:
    """Bytes signed by the Tab5, including the authenticated request digest."""
    return _canonical_fields(RESPONSE_DOMAIN, [
        str(version), req_id, decision, nonce, device_id, req_digest,
    ])


def load_request_signing_key(path: str):
    """Load a private P-256 key from a regular owner-only PEM file."""
    if ec is None:
        raise RuntimeError("python-cryptography is required for request signing")
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    fd = os.open(path, flags)
    try:
        info = os.fstat(fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid() or
                stat.S_IMODE(info.st_mode) & 0o077):
            raise ValueError("request signing key must be a regular owner-only file")
        with os.fdopen(fd, "rb", closefd=False) as fh:
            pem = fh.read(16_385)
        if len(pem) > 16_384:
            raise ValueError("request signing key file is too large")
    finally:
        os.close(fd)
    key = serialization.load_pem_private_key(pem, password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise ValueError("request signing key must use P-256")
    return key


def validate_mqtt_transport(host: str, tls: bool, ca_cert: str,
                            allow_plaintext_dev: bool, username: str,
                            password: str) -> None:
    """Require authenticated, CA-verified MQTT except explicit loopback tests."""
    is_loopback = host.strip().lower() == "localhost"
    try:
        is_loopback = is_loopback or ipaddress.ip_address(host).is_loopback
    except ValueError:
        pass
    if tls:
        if not ca_cert or not os.path.isfile(ca_cert):
            raise ValueError("MQTT TLS requires an existing CA certificate file")
        if not username or not password:
            raise ValueError("MQTT TLS requires broker credentials")
    elif not (is_loopback and allow_plaintext_dev):
        raise ValueError("plaintext MQTT is allowed only for explicit loopback development")


def load_public_key(pem_or_path: str):
    """Load an EC public key from a PEM string or a path to a PEM file."""
    if ec is None:
        raise RuntimeError("python-cryptography is required for verification")
    data = pem_or_path
    if "BEGIN" not in pem_or_path and os.path.exists(pem_or_path):
        with open(pem_or_path, "rb") as fh:
            data = fh.read().decode()
    return serialization.load_pem_public_key(data.encode())


def verify_signature(pubkey, canonical: bytes, sig_b64: str) -> bool:
    """Verify an ECDSA-P256/SHA-256 DER signature (base64) over `canonical`."""
    try:
        sig = base64.b64decode(sig_b64)
        pubkey.verify(sig, canonical, ec.ECDSA(hashes.SHA256()))
        return True
    except (InvalidSignature, ValueError, TypeError):
        return False


# --------------------------------------------------------------------------- #
# Broker core
# --------------------------------------------------------------------------- #
class PendingRequest:
    __slots__ = ("id", "nonce", "device", "created", "ttl", "req_digest", "event", "result")

    def __init__(self, req_id: str, nonce: str, device: str, ttl: int,
                 req_digest: str):
        self.id = req_id
        self.nonce = nonce
        self.device = device          # target device id ("" = any enrolled)
        self.created = time.time()
        self.ttl = ttl
        self.req_digest = req_digest
        self.event = threading.Event()
        self.result: dict | None = None


class Broker:
    """MQTT-connected approval broker. HTTP is a thin wrapper on top (serve())."""

    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.topic_prefix = cfg.get("topic_prefix", "auth")
        self.request_ttl = int(cfg.get("request_ttl_sec", 60))
        self._pending: dict[str, PendingRequest] = {}
        self._lock = threading.Lock()
        self._devices = cfg["devices"]   # {device_id: public_key}
        self._client = None
        self._connected = threading.Event()

    # ---- MQTT -----------------------------------------------------------
    def connect(self, timeout: float = 10.0) -> None:
        validate_mqtt_transport(
            self.cfg.get("host", "127.0.0.1"),
            bool(self.cfg.get("tls", False)),
            self.cfg.get("ca_cert", ""),
            bool(self.cfg.get("allow_plaintext_dev", False)),
            self.cfg.get("username", ""),
            self.cfg.get("password", ""),
        )
        if mqtt is None:
            raise RuntimeError("paho-mqtt not installed (pip install paho-mqtt)")
        cid = self.cfg.get("client_id", "authbrokerd")
        # paho 2.x requires an explicit callback API version; stay 1.x-compatible.
        if hasattr(mqtt, "CallbackAPIVersion"):
            client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=cid)
        else:  # paho 1.x
            client = mqtt.Client(client_id=cid)
        user = self.cfg.get("username")
        if user:
            client.username_pw_set(user, self.cfg.get("password", ""))
        if str(self.cfg.get("tls", "false")).lower() in ("1", "true", "yes"):
            client.tls_set(ca_certs=self.cfg.get("ca_cert") or None)
        client.on_connect = self._on_connect
        client.on_message = self._on_message
        client.connect(self.cfg["host"], int(self.cfg.get("port", 1883)),
                       keepalive=30)
        client.loop_start()
        self._client = client
        if not self._connected.wait(timeout):
            raise TimeoutError("MQTT connect timed out")

    def _on_connect(self, client, userdata, flags, reason_code, properties=None):
        rc = getattr(reason_code, "value", reason_code)
        if rc != 0:
            log.error("MQTT connect failed rc=%s", reason_code)
            return
        topic = f"{self.topic_prefix}/response/#"
        client.subscribe(topic, qos=1)
        log.info("MQTT connected; subscribed %s", topic)
        self._connected.set()

    def _on_message(self, client, userdata, msg):
        try:
            self._handle_response(msg.topic, msg.payload)
        except Exception:  # never let a bad frame kill the network thread
            log.exception("error handling %s", msg.topic)

    def _handle_response(self, topic: str, payload: bytes) -> None:
        parts = topic.split("/")
        if len(parts) != 3 or parts[1] != "response":
            return
        req_id = parts[2]
        with self._lock:
            pending = self._pending.get(req_id)
        if pending is None:
            log.info("response for unknown/expired request %s (ignored)", req_id)
            return
        if pending.event.is_set():
            log.info("response for resolved request %s (ignored)", req_id)
            return

        try:
            body = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            log.warning("malformed response for %s", req_id)
            return

        decision = str(body.get("decision", ""))
        device_id = str(body.get("device_id", ""))
        nonce = str(body.get("nonce", ""))
        sig = str(body.get("sig", ""))
        req_digest = str(body.get("req_digest", ""))
        version = int(body.get("v", 0))

        # --- security checks: id, nonce, version, decision, freshness, sig ---
        if version != PROTO_VERSION:
            log.warning("reject %s: bad version %s", req_id, version)
            return
        if body.get("id") != req_id:
            log.warning("reject %s: id/topic mismatch", req_id)
            return
        if not secrets.compare_digest(nonce, pending.nonce):
            log.warning("reject %s: nonce mismatch (replay?)", req_id)
            return
        if decision not in ("approve", "deny"):
            log.warning("reject %s: bad decision %r", req_id, decision)
            return
        if not secrets.compare_digest(req_digest, pending.req_digest):
            log.warning("reject %s: request digest mismatch", req_id)
            return
        if time.time() - pending.created > pending.ttl:
            log.warning("reject %s: response arrived after TTL", req_id)
            return

        pubkey = self._devices.get(device_id)
        if pubkey is None:
            log.warning("reject %s: unknown device %r", req_id, device_id)
            return
        if pending.device and pending.device != device_id:
            log.warning("reject %s: response from non-target device %r",
                        req_id, device_id)
            return
        canonical = canonical_response(
            version, req_id, decision, nonce, device_id, req_digest
        )
        if not verify_signature(pubkey, canonical, sig):
            log.warning("reject %s: SIGNATURE INVALID from %s", req_id, device_id)
            return

        log.info("request %s -> %s by %s", req_id, decision.upper(), device_id)
        with self._lock:
            pending.result = {"decision": decision, "device_id": device_id,
                              "id": req_id}
            pending.event.set()

    # ---- request lifecycle ---------------------------------------------
    def request(self, action: str, subject: str, detail: str = "",
                source_ip: str = "", device: str = "",
                ttl: int | None = None) -> dict:
        """Publish an approval request and block until answered or TTL expires.

        Returns {"decision": "approve"|"deny"|"timeout", ...}. This is what the
        dashboard's HTTP call ultimately receives.
        """
        if self.cfg.get("request_signing_key") is None:
            raise RuntimeError("approval request signing key is not configured")
        request_signing_key_id = self.cfg.get("request_signing_key_id", "")
        if (not isinstance(request_signing_key_id, str) or
                not re.fullmatch(r"[a-z0-9_-]{1,14}", request_signing_key_id)):
            raise RuntimeError("approval request signing key ID is not configured")
        ttl = int(ttl if ttl is not None else self.request_ttl)
        if not 1 <= ttl <= 120:
            raise ValueError("approval request TTL must be between 1 and 120 seconds")
        if self._client is None or not self._connected.is_set():
            raise RuntimeError("MQTT broker is not connected")
        req_id = uuid.uuid4().hex
        nonce = base64.b64encode(secrets.token_bytes(32)).decode()
        envelope = {
            "v": PROTO_VERSION,
            "kid": request_signing_key_id,
            "id": req_id,
            "ts": int(time.time()),
            "ttl": ttl,
            "action": action,
            "subject": subject,
            "detail": detail,
            "source_ip": source_ip,
            "nonce": nonce,
            "device": device,
        }
        canonical = canonical_request(envelope)
        req_digest = hashlib.sha256(canonical).hexdigest()
        envelope["sig"] = base64.b64encode(
            self.cfg["request_signing_key"].sign(canonical, ec.ECDSA(hashes.SHA256()))
        ).decode("ascii")
        pending = PendingRequest(req_id, nonce, device, ttl, req_digest)
        with self._lock:
            self._pending[req_id] = pending
        topic = f"{self.topic_prefix}/request/{req_id}"
        try:
            published = self._client.publish(topic, json.dumps(envelope), qos=1)
            if getattr(published, "rc", 0) != 0:
                raise RuntimeError("MQTT publish returned an error")
        except Exception as exc:
            with self._lock:
                self._pending.pop(req_id, None)
            raise RuntimeError("unable to publish approval request") from exc
        log.info("published %s (%s '%s' from %s) ttl=%ss",
                 topic, action, subject, source_ip or "?", ttl)

        answered = pending.event.wait(ttl)
        with self._lock:
            self._pending.pop(req_id, None)
        if answered and pending.result:
            return pending.result
        return {"decision": "timeout", "id": req_id}

    def close(self) -> None:
        if self._client is not None:
            self._client.loop_stop()
            self._client.disconnect()

    # ---- HTTP -----------------------------------------------------------
    def serve(self, bind: str, port: int, token: str) -> None:
        validate_http_token(token)
        broker = self

        class Handler(BaseHTTPRequestHandler):
            server_version = "authbrokerd/1.0"

            def _json(self, code: int, obj: dict) -> None:
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _authorized(self) -> bool:
                got = self.headers.get("Authorization", "")
                return http_token_authorized(token, got)

            def do_GET(self):  # noqa: N802
                if self.path == "/healthz":
                    self._json(200, {"ok": True,
                                     "connected": broker._connected.is_set()})
                    return
                self._json(404, {"error": "not_found"})

            def do_POST(self):  # noqa: N802
                if self.path != "/auth/request":
                    self._json(404, {"error": "not_found"})
                    return
                if not self._authorized():
                    self._json(401, {"error": "unauthorized"})
                    return
                try:
                    length = int(self.headers.get("Content-Length", 0))
                except ValueError:
                    self._json(400, {"error": "bad_content_length"})
                    return
                if length < 0:
                    self._json(400, {"error": "bad_content_length"})
                    return
                if length > 8192:
                    self._json(413, {"error": "request_too_large"})
                    return
                try:
                    body = json.loads(self.rfile.read(length) or b"{}")
                except ValueError:
                    self._json(400, {"error": "bad_json"})
                    return
                if not isinstance(body, dict):
                    self._json(400, {"error": "object_required"})
                    return
                if not broker._connected.is_set():
                    self._json(503, {"error": "broker_unavailable"})
                    return
                string_fields = ("action", "subject", "detail", "source_ip", "device")
                if any(name in body and not isinstance(body[name], str)
                       for name in string_fields):
                    self._json(400, {"error": "string_fields_required"})
                    return
                ttl = body.get("ttl")
                if ttl is not None and (type(ttl) is not int or not 1 <= ttl <= 120):
                    self._json(400, {"error": "invalid_ttl"})
                    return
                subject = str(body.get("subject", "")).strip()
                if not subject:
                    self._json(400, {"error": "subject_required"})
                    return
                try:
                    result = broker.request(
                        action=body.get("action", "login"),
                        subject=subject,
                        detail=body.get("detail", ""),
                        source_ip=body.get("source_ip", ""),
                        device=body.get("device", ""),
                        ttl=ttl,
                    )
                except ValueError as exc:
                    self._json(400, {"error": "invalid_request", "detail": str(exc)})
                    return
                except (RuntimeError, OSError):
                    log.exception("approval request unavailable")
                    self._json(503, {"error": "broker_unavailable"})
                    return
                code = 200 if result["decision"] in ("approve", "deny") else 202
                self._json(code, result)

            def log_message(self, fmt, *args):  # route to logging, not stderr
                log.debug("http %s", fmt % args)

        httpd = ThreadingHTTPServer((bind, port), Handler)
        log.info("HTTP API listening on http://%s:%s", bind, port)
        httpd.serve_forever()


# --------------------------------------------------------------------------- #
# Config
# --------------------------------------------------------------------------- #
def load_config(path: str) -> dict:
    cp = configparser.ConfigParser()
    if not cp.read(path):
        raise SystemExit(f"config not found: {path}")

    defaults = cp["DEFAULT"]
    mqtt_s = cp["mqtt"] if cp.has_section("mqtt") else defaults
    http_s = cp["http"] if cp.has_section("http") else defaults
    broker_s = cp["broker"] if cp.has_section("broker") else defaults
    signing_key_path = broker_s.get("request_signing_key_file", "").strip()
    if not signing_key_path:
        raise SystemExit("[broker] request_signing_key_file is required")
    try:
        request_signing_key = load_request_signing_key(signing_key_path)
    except (OSError, ValueError) as exc:
        raise SystemExit(f"invalid request signing key: {exc}") from exc
    request_signing_key_id = broker_s.get("request_signing_key_id", "").strip()
    if not re.fullmatch(r"[a-z0-9_-]{1,14}", request_signing_key_id):
        raise SystemExit("[broker] request_signing_key_id must match [a-z0-9_-]{1,14}")

    devices: dict[str, object] = {}
    for section in cp.sections():
        if not section.startswith("device."):
            continue
        dev_id = section.split(".", 1)[1]
        pem = cp[section].get("pubkey_pem") or cp[section].get("pubkey")
        if not pem:
            raise SystemExit(f"[{section}] missing pubkey_pem")
        devices[dev_id] = load_public_key(pem)

    return {
        "host": mqtt_s.get("host", "127.0.0.1"),
        "port": mqtt_s.get("port", "1883"),
        "username": mqtt_s.get("username"),
        "password": mqtt_s.get("password"),
        "tls": mqtt_s.getboolean("tls", fallback=False),
        "ca_cert": mqtt_s.get("ca_cert"),
        "client_id": mqtt_s.get("client_id", "authbrokerd"),
        "topic_prefix": broker_s.get("topic_prefix", "auth"),
        "request_ttl_sec": broker_s.get("request_ttl_sec", "60"),
        "request_signing_key": request_signing_key,
        "request_signing_key_id": request_signing_key_id,
        "request_signing_key_file": signing_key_path,
        "allow_plaintext_dev": broker_s.getboolean(
            "allow_plaintext_dev", fallback=False
        ),
        "http_bind": http_s.get("bind", "127.0.0.1"),
        "http_port": int(http_s.get("port", "9444")),
        "http_token": http_s.get("token", ""),
        "devices": devices,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="SolariNet Authenticator broker")
    ap.add_argument("--config", required=True)
    ap.add_argument("--check", action="store_true",
                    help="validate config + keys and exit (no MQTT/HTTP)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args(argv)

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    cfg = load_config(args.config)

    try:
        validate_http_token(cfg["http_token"])
        validate_mqtt_transport(
            cfg["host"], cfg["tls"], cfg["ca_cert"],
            cfg["allow_plaintext_dev"], cfg["username"], cfg["password"],
        )
    except ValueError as exc:
        ap.error(str(exc))

    if args.check:
        print(f"OK: {len(cfg['devices'])} enrolled device(s): "
              f"{', '.join(cfg['devices']) or '(none)'}")
        print(f"    MQTT {cfg['host']}:{cfg['port']} "
              f"topic '{cfg['topic_prefix']}/*' "
              f"transport={'TLS' if cfg['tls'] else 'loopback development only'}")
        print(f"    HTTP {cfg['http_bind']}:{cfg['http_port']} token=set")
        print("    Request signing key: owner-only P-256 key loaded")
        return 0

    if not cfg["devices"]:
        log.warning("no [device.*] sections: every response will be rejected")

    broker = Broker(cfg)
    broker.connect()
    try:
        broker.serve(cfg["http_bind"], cfg["http_port"], cfg["http_token"])
    except KeyboardInterrupt:
        pass
    finally:
        broker.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
