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

  request  auth/request/<id>   (broker -> device, unsigned; not a secret)
  response auth/response/<id>  (device -> broker, ECDSA-P256/SHA-256 signed)

  canonical signing input (both sides build it identically, UTF-8):
      "<v>\n<id>\n<decision>\n<nonce>\n<device_id>"

The broker holds only the device's PUBLIC key, so a broker compromise cannot
forge an approval — only the Tab5, holding the private key in encrypted NVS, can.

Run:
    authbrokerd.py --config authbroker.conf         # the daemon
    authbrokerd.py --config authbroker.conf --check # config + key sanity, no serve
See loopback_test.py for a live round-trip test against the real Mosquitto.
"""
from __future__ import annotations

import argparse
import base64
import configparser
import json
import logging
import os
import secrets
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

PROTO_VERSION = 1


# --------------------------------------------------------------------------- #
# Signing helpers (shared with the device simulator in loopback_test.py)
# --------------------------------------------------------------------------- #
def canonical_response(version: int, req_id: str, decision: str,
                       nonce: str, device_id: str) -> bytes:
    """The exact bytes the device signs and the broker verifies.

    Field order is fixed and every field is newline-delimited. The device
    firmware (approvals.cpp) builds the identical string before signing.
    """
    return "\n".join(
        [str(version), req_id, decision, nonce, device_id]
    ).encode("utf-8")


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
    __slots__ = ("id", "nonce", "device", "created", "ttl", "event", "result")

    def __init__(self, req_id: str, nonce: str, device: str, ttl: int):
        self.id = req_id
        self.nonce = nonce
        self.device = device          # target device id ("" = any enrolled)
        self.created = time.time()
        self.ttl = ttl
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

        try:
            body = json.loads(payload.decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            log.warning("malformed response for %s", req_id)
            return

        decision = str(body.get("decision", ""))
        device_id = str(body.get("device_id", ""))
        nonce = str(body.get("nonce", ""))
        sig = str(body.get("sig", ""))
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
        canonical = canonical_response(version, req_id, decision, nonce, device_id)
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
        ttl = int(ttl or self.request_ttl)
        req_id = uuid.uuid4().hex
        nonce = base64.b64encode(secrets.token_bytes(32)).decode()
        pending = PendingRequest(req_id, nonce, device, ttl)
        with self._lock:
            self._pending[req_id] = pending

        envelope = {
            "v": PROTO_VERSION,
            "id": req_id,
            "ts": int(time.time()),
            "ttl": ttl,
            "action": action,
            "subject": subject,
            "detail": detail,
            "source_ip": source_ip,
            "nonce": nonce,
        }
        if device:
            envelope["device"] = device
        topic = f"{self.topic_prefix}/request/{req_id}"
        self._client.publish(topic, json.dumps(envelope), qos=1)
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
                if not token:
                    return True  # no token configured => open (dev only)
                got = self.headers.get("Authorization", "")
                want = f"Bearer {token}"
                return secrets.compare_digest(got, want)

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
                length = int(self.headers.get("Content-Length", 0))
                try:
                    body = json.loads(self.rfile.read(length) or b"{}")
                except ValueError:
                    self._json(400, {"error": "bad_json"})
                    return
                subject = str(body.get("subject", "")).strip()
                if not subject:
                    self._json(400, {"error": "subject_required"})
                    return
                result = broker.request(
                    action=str(body.get("action", "login")),
                    subject=subject,
                    detail=str(body.get("detail", "")),
                    source_ip=str(body.get("source_ip", "")),
                    device=str(body.get("device", "")),
                    ttl=body.get("ttl"),
                )
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

    mqtt_s = cp["mqtt"] if cp.has_section("mqtt") else {}
    http_s = cp["http"] if cp.has_section("http") else {}
    broker_s = cp["broker"] if cp.has_section("broker") else {}

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
        "tls": mqtt_s.get("tls", "false"),
        "ca_cert": mqtt_s.get("ca_cert"),
        "client_id": mqtt_s.get("client_id", "authbrokerd"),
        "topic_prefix": broker_s.get("topic_prefix", "auth"),
        "request_ttl_sec": broker_s.get("request_ttl_sec", "60"),
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

    if args.check:
        print(f"OK: {len(cfg['devices'])} enrolled device(s): "
              f"{', '.join(cfg['devices']) or '(none)'}")
        print(f"    MQTT {cfg['host']}:{cfg['port']} "
              f"topic '{cfg['topic_prefix']}/*'")
        print(f"    HTTP {cfg['http_bind']}:{cfg['http_port']} "
              f"token={'set' if cfg['http_token'] else 'OPEN (dev)'}")
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
