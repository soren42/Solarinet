#!/usr/bin/env python3
"""notifyd — SolariNet notification dispatcher.

Consumes JSON alert messages from RabbitMQ's durable topic exchange
`notify.events` (published by monitoring/dashboard/etc. with routing keys
like `notify.sms`, `notify.push`, `notify.log`) and fans each one out to a
configurable set of pluggable senders (see senders/__init__.py for the
sender interface). See README.md for the full message contract and
architecture.

Requires: pika (pip install pika). If pika isn't installed system-wide,
either `pip install --user pika` or run inside a venv — see README.md.

Usage:
    python3 notifyd.py [--config /path/to/notify.conf]

Config defaults to notify.conf next to this script (see notify.conf.example).
"""

import argparse
import configparser
import json
import logging
import os
import signal
import sys
import time

try:
    import pika
except ImportError:
    pika = None

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from senders import load_senders  # noqa: E402

log = logging.getLogger("notifyd")

DEFAULT_CONFIG_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "notify.conf")


class Dispatcher:
    def __init__(self, cfg):
        self.cfg = cfg
        enabled = [s.strip() for s in cfg.get("senders", "enabled", fallback="log").split(",")]
        self.senders = load_senders(enabled)
        if "log" not in self.senders:
            # The log sender is the durable fallback; if it failed to load
            # (bad name/typo aside) something is badly wrong, but we still
            # try to run with whatever loaded rather than refuse to start.
            log.warning("log sender is not enabled/loaded — notifications will "
                        "have no local audit trail")

        self.default_channel = cfg.get("defaults", "default_channel", fallback="log")
        self.default_sms_to = _split_csv(cfg.get("defaults", "default_sms_to", fallback=""))
        self.default_push_to = _split_csv(cfg.get("defaults", "default_push_to", fallback=""))
        self.default_apple_to = _split_csv(cfg.get("defaults", "default_apple_to", fallback=""))
        self.default_email_to = _split_csv(cfg.get("defaults", "default_email_to", fallback=""))

        self.routing = {}
        if cfg.has_section("routing"):
            for sev, val in cfg.items("routing"):
                self.routing[sev] = [s.strip() for s in val.split(",") if s.strip()]

    def senders_for(self, message, routing_key):
        """Decide which sender names should handle this message."""
        channel = (message.get("channel") or "auto").strip().lower()
        if channel and channel != "auto":
            if channel in self.senders:
                return [channel]
            # An explicit but unknown/unloaded channel: fall back to routing
            # by severity so the message isn't silently dropped, but still
            # complain loudly since this points at a config/caller mismatch.
            log.warning("message requested channel=%r which is not enabled; "
                        "falling back to severity routing", channel)

        severity = (message.get("severity") or "info").strip().lower()
        names = self.routing.get(severity)
        if names:
            return [n for n in names if n in self.senders]

        return [self.default_channel] if self.default_channel in self.senders else list(self.senders)

    def recipients_for(self, sender_name, message):
        to = message.get("to")
        if to:
            return list(to)
        if sender_name == "sms":
            return list(self.default_sms_to)
        if sender_name == "push":
            return list(self.default_push_to)
        if sender_name == "apple":
            return list(self.default_apple_to)
        if sender_name == "email":
            return list(self.default_email_to)
        return []

    def dispatch(self, message, routing_key):
        """Run every applicable sender for one message.

        Returns True if at least one sender succeeded (message is
        considered "delivered enough" to ack), False if every sender
        that ran failed (caller decides ack/requeue policy).
        """
        names = self.senders_for(message, routing_key)
        if not names:
            log.error("no sender available for message %r (severity=%s, channel=%s)",
                      message.get("title"), message.get("severity"), message.get("channel"))
            return False

        message = dict(message)
        message["_routing_key"] = routing_key

        any_ok = False
        for name in names:
            mod = self.senders.get(name)
            if mod is None:
                continue
            per_sender_msg = dict(message)
            per_sender_msg["_to"] = self.recipients_for(name, message)
            section = f"sender.{name}"
            sender_cfg = self.cfg[section] if self.cfg.has_section(section) else {}
            try:
                ok = mod.send(per_sender_msg, sender_cfg)
            except Exception:
                log.exception("sender %r raised while handling %r", name, message.get("title"))
                ok = False
            any_ok = any_ok or ok
            log.info("sender=%s title=%r result=%s", name, message.get("title"), "ok" if ok else "failed")
        return any_ok


def _split_csv(s):
    return [x.strip() for x in s.split(",") if x.strip()]


def load_config(path):
    if not os.path.exists(path):
        raise SystemExit(
            f"config not found: {path}\n"
            f"copy notify.conf.example to notify.conf and edit it (see README.md)"
        )
    cfg = configparser.ConfigParser()
    cfg.read(path)
    return cfg


def parse_message(body):
    data = json.loads(body)
    if not isinstance(data, dict):
        raise ValueError("message body is not a JSON object")
    # Minimal contract validation — title/body are the only fields we treat
    # as required for a message to be worth delivering at all.
    if "title" not in data and "body" not in data:
        raise ValueError("message has neither 'title' nor 'body'")
    return data


class Notifyd:
    def __init__(self, cfg):
        self.cfg = cfg
        self.dispatcher = Dispatcher(cfg)
        self._stop = False

    def request_stop(self, *_args):
        log.info("stop requested")
        self._stop = True

    def run_forever(self):
        delay = self.cfg.getint("amqp", "reconnect_delay_sec", fallback=5)
        while not self._stop:
            try:
                self._run_once()
            except KeyboardInterrupt:
                break
            except Exception:
                log.exception("connection loop error; reconnecting in %ss", delay)
            if self._stop:
                break
            time.sleep(delay)

    def _run_once(self):
        if pika is None:
            raise RuntimeError("pika is not installed (pip install pika)")

        url = self.cfg.get("amqp", "url")
        exchange = self.cfg.get("amqp", "exchange", fallback="notify.events")
        queue = self.cfg.get("amqp", "queue", fallback="notify.dispatch")
        binding_keys = _split_csv(self.cfg.get("amqp", "binding_keys", fallback="notify.#"))

        params = pika.URLParameters(url)
        conn = pika.BlockingConnection(params)
        try:
            ch = conn.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.queue_declare(queue=queue, durable=True)
            for key in binding_keys:
                ch.queue_bind(exchange=exchange, queue=queue, routing_key=key)
            ch.basic_qos(prefetch_count=10)

            log.info("connected; exchange=%s queue=%s bindings=%s", exchange, queue, binding_keys)

            def on_message(channel, method, _properties, body):
                routing_key = method.routing_key
                try:
                    message = parse_message(body)
                except (ValueError, json.JSONDecodeError) as exc:
                    log.error("dropping unparsable message on %s: %s", routing_key, exc)
                    channel.basic_ack(delivery_tag=method.delivery_tag)
                    return

                ok = self.dispatcher.dispatch(message, routing_key)
                if ok:
                    channel.basic_ack(delivery_tag=method.delivery_tag)
                else:
                    # Every sender failed (e.g. transient Tachyon outage with
                    # sms as the only sender for this severity). Nack without
                    # requeue: the log sender's failure path already writes a
                    # warning, and endless requeue would spin on a message
                    # nothing can currently deliver. Operators can raise
                    # queue TTL/DLX in RabbitMQ later if replay is wanted.
                    log.error("all senders failed for %r; dropping (see logs above)",
                              message.get("title"))
                    channel.basic_ack(delivery_tag=method.delivery_tag)

            ch.basic_consume(queue=queue, on_message_callback=on_message)

            while not self._stop:
                conn.process_data_events(time_limit=1)
        finally:
            try:
                conn.close()
            except Exception:
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default=DEFAULT_CONFIG_PATH)
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )

    cfg = load_config(args.config)
    daemon = Notifyd(cfg)

    signal.signal(signal.SIGTERM, daemon.request_stop)
    signal.signal(signal.SIGINT, daemon.request_stop)

    daemon.run_forever()
    log.info("stopped")


if __name__ == "__main__":
    main()
