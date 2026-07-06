"""mqtt sender — republish SolariNet notifications to an MQTT broker.

This is the small server-side shim the Tab5 firmware needs. It is a standard
notifyd *sender* plugin (same interface as senders/log.py, senders/push.py):
it takes each notify.events message notifyd already receives from RabbitMQ and
publishes it to an MQTT broker (e.g. Mosquitto) under `notify/<severity>`.

The Tab5 subscribes to `notify/#` over MQTT/TLS (much easier on ESP than AMQP),
so no AMQP client is ever needed on the device.

INSTALL (on the notifyd host, benzene):
  1. cp firmware/tab5/server-shim/mqtt.py  deploy/notify/senders/mqtt.py
  2. Register it in deploy/notify/senders/__init__.py REGISTRY:
         "mqtt": "senders.mqtt",
  3. Enable + route it in notify.conf:
         [senders]
         enabled = log,sms,push,mqtt
         [routing]
         crit = log,sms,push,mqtt
         warn = log,push,mqtt
         info = log,mqtt
         [sender.mqtt]
         host = 10.5.2.50            ; or a dedicated broker host / DNS name
         port = 8883                 ; 8883 TLS, 1883 plaintext (dev only)
         username = notifyd
         password = CHANGE_ME
         topic_prefix = notify
         tls = true
         ca_cert = /etc/solarinet/mqtt-ca.pem   ; broker CA (self-hosted)
         client_id = notifyd-mqtt
  4. pip install paho-mqtt  (into notifyd's venv)
  5. Provision the same broker + a `tab5` MQTT user on the device (see the
     firmware README "Wi-Fi/secret provisioning").

The broker itself (Mosquitto) is the only new infra piece: one small daemon,
one config file, two users (notifyd publish, tab5 subscribe). It can run on
benzene next to RabbitMQ.
"""

import json
import logging

try:
    import paho.mqtt.client as mqtt
except ImportError:
    mqtt = None

log = logging.getLogger("notify.sender.mqtt")

# One persistent client reused across messages (connect lazily, auto-reconnect).
_client = None
_client_key = None


def _get_client(cfg):
    global _client, _client_key
    if mqtt is None:
        log.error("paho-mqtt not installed (pip install paho-mqtt)")
        return None

    host = cfg.get("host", "127.0.0.1") if cfg else "127.0.0.1"
    port = int(cfg.get("port", 1883)) if cfg else 1883
    key = (host, port)
    if _client is not None and _client_key == key:
        return _client

    client_id = (cfg.get("client_id") if cfg else None) or "notifyd-mqtt"
    c = mqtt.Client(client_id=client_id, clean_session=True)

    user = cfg.get("username") if cfg else None
    if user:
        c.username_pw_set(user, cfg.get("password", ""))

    if cfg and str(cfg.get("tls", "false")).lower() in ("1", "true", "yes"):
        ca = cfg.get("ca_cert") or None
        c.tls_set(ca_certs=ca)  # verifies broker cert against this CA

    try:
        c.connect(host, port, keepalive=30)
        c.loop_start()  # background network thread; handles reconnects
    except OSError as exc:
        log.warning("mqtt connect to %s:%s failed: %s", host, port, exc)
        return None

    _client, _client_key = c, key
    return c


def send(message, cfg):
    """notifyd sender entry point. Return True on publish, False on soft fail."""
    client = _get_client(cfg)
    if client is None:
        return False

    severity = (message.get("severity") or "info").strip().lower()
    prefix = (cfg.get("topic_prefix") if cfg else None) or "notify"
    topic = f"{prefix}/{severity}"

    # Forward the same JSON contract the firmware expects. Strip dispatcher-only
    # private fields (keys starting with "_") to keep the on-wire payload clean.
    payload = {k: v for k, v in message.items() if not k.startswith("_")}
    body = json.dumps(payload, separators=(",", ":"))

    try:
        info = client.publish(topic, body, qos=1, retain=False)
        # qos1: don't block forever, but confirm it was queued to the socket.
        info.wait_for_publish(timeout=5)
        ok = info.is_published()
        if not ok:
            log.warning("mqtt publish to %s not confirmed", topic)
        return bool(ok)
    except (ValueError, RuntimeError, OSError) as exc:
        log.warning("mqtt publish to %s failed: %s", topic, exc)
        return False
