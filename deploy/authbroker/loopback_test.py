#!/usr/bin/env python3
"""loopback_test.py — live round-trip test of authbrokerd against real Mosquitto.

Exercises the whole approval path over the REAL broker (default 10.5.2.50:1883,
user 'solari') with a simulated Tab5:

  1. mint an ephemeral ECDSA-P256 "device" keypair
  2. start a Broker that trusts that device's public key
  3. start a fake device: subscribes auth/request/#, signs + publishes a response
  4. call broker.request(...) and assert it resolves to the device's decision

No dashboard, no HTTP, no real Tab5 needed — it proves the MQTT contract and the
signature scheme end to end. Run twice (approve + deny) to check both verdicts.

    python3 loopback_test.py                       # approve, default broker
    python3 loopback_test.py --decision deny
    python3 loopback_test.py --host 10.5.2.50 --port 1883 \
        --username solari --password '...'
"""
from __future__ import annotations

import argparse
import base64
import json
import sys
import threading
import time

import paho.mqtt.client as mqtt
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec

import authbrokerd
from authbrokerd import Broker, canonical_response, PROTO_VERSION


class DeviceSim:
    """A stand-in for the Tab5: answers auth/request/<id> with a signed response."""

    def __init__(self, cfg, device_id, privkey, decision):
        self.cfg = cfg
        self.device_id = device_id
        self.privkey = privkey
        self.decision = decision
        self.prefix = cfg.get("topic_prefix", "auth")
        self.saw_request = threading.Event()
        if hasattr(mqtt, "CallbackAPIVersion"):
            self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2,
                                 client_id=f"sim-{device_id}")
        else:
            self.c = mqtt.Client(client_id=f"sim-{device_id}")
        if cfg.get("username"):
            self.c.username_pw_set(cfg["username"], cfg.get("password", ""))
        self.c.on_connect = self._on_connect
        self.c.on_message = self._on_message

    def _on_connect(self, *a):
        self.c.subscribe(f"{self.prefix}/request/#", qos=1)

    def _on_message(self, client, userdata, msg):
        req = json.loads(msg.payload.decode())
        self.saw_request.set()
        req_id = req["id"]
        print(f"  [device] got request {req_id}: "
              f"{req.get('action')} '{req.get('subject')}' "
              f"from {req.get('source_ip')} -> answering {self.decision.upper()}")
        canonical = canonical_response(PROTO_VERSION, req_id, self.decision,
                                       req["nonce"], self.device_id)
        sig = self.privkey.sign(canonical, ec.ECDSA(hashes.SHA256()))
        resp = {
            "v": PROTO_VERSION,
            "id": req_id,
            "ts": int(time.time()),
            "decision": self.decision,
            "device_id": self.device_id,
            "nonce": req["nonce"],
            "sig": base64.b64encode(sig).decode(),
        }
        client.publish(f"{self.prefix}/response/{req_id}",
                       json.dumps(resp), qos=1)

    def start(self):
        self.c.connect(self.cfg["host"], int(self.cfg.get("port", 1883)), 30)
        self.c.loop_start()

    def stop(self):
        self.c.loop_stop()
        self.c.disconnect()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="10.5.2.50")
    ap.add_argument("--port", type=int, default=1883)
    ap.add_argument("--username", default="solari")
    ap.add_argument("--password", default="Mqtt-solari-n136hko4!9")
    ap.add_argument("--decision", default="approve", choices=["approve", "deny"])
    ap.add_argument("--device-id", default="tab5-loopback")
    args = ap.parse_args()

    if authbrokerd.ec is None or mqtt is None:
        print("need paho-mqtt + cryptography installed", file=sys.stderr)
        return 2

    priv = ec.generate_private_key(ec.SECP256R1())
    pub_pem = priv.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    ).decode()

    cfg = {
        "host": args.host, "port": args.port,
        "username": args.username, "password": args.password,
        "topic_prefix": "auth", "request_ttl_sec": "15",
        "client_id": "authbrokerd-loopback",
        "devices": {args.device_id: serialization.load_pem_public_key(pub_pem.encode())},
    }

    print(f"[test] connecting to mqtt://{args.host}:{args.port} as {args.username}")
    broker = Broker(cfg)
    broker.connect(timeout=10)
    print("[test] broker connected + subscribed")

    device = DeviceSim(cfg, args.device_id, priv, args.decision)
    device.start()
    time.sleep(1.0)  # let the device subscribe before we publish

    print(f"[test] posting approval request (expect device to {args.decision})")
    t0 = time.time()
    result = broker.request(action="login", subject="jason",
                            detail="SolariNet dashboard",
                            source_ip="10.5.2.20", ttl=15)
    dt = time.time() - t0

    device.stop()
    broker.close()

    print(f"[test] resolved in {dt:.2f}s -> {result}")
    ok = result.get("decision") == args.decision and \
        result.get("device_id") == args.device_id
    print("[test] RESULT:", "PASS ✅" if ok else "FAIL ❌")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
