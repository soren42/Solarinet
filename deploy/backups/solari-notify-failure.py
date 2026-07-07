#!/usr/bin/env python3
"""Publish a fail-soft SolariNet backup failure notification."""

import argparse
import configparser
import json
import os
import socket
import sys
import time

try:
    import pika
except ImportError:
    pika = None


def warn(message):
    print(f"solari-notify-failure: warning: {message}", file=sys.stderr)


def candidate_config_paths(explicit_path):
    script_dir = os.path.dirname(os.path.realpath(__file__))
    paths = []
    if explicit_path:
        paths.append(explicit_path)
    paths.extend(
        [
            os.path.normpath(os.path.join(script_dir, "..", "notify", "notify.conf")),
            os.path.join(script_dir, "notify.conf"),
            "/etc/solari-notify.conf",
        ]
    )
    return paths


def load_config(explicit_path):
    tried = []
    for path in candidate_config_paths(explicit_path):
        if path in tried:
            continue
        tried.append(path)
        try:
            with open(path, "r", encoding="utf-8") as fh:
                cfg = configparser.ConfigParser()
                cfg.read_file(fh)
                return cfg, path
        except FileNotFoundError:
            continue
        except OSError as exc:
            warn(f"config unreadable: {path}: {exc}")
            continue
        except configparser.Error as exc:
            warn(f"config invalid: {path}: {exc}")
            continue
    raise RuntimeError(f"config not found/readable; tried: {', '.join(tried)}")


def publish_failure(unit, config_path):
    if pika is None:
        warn("pika is not installed; skipping AMQP notification")
        return

    cfg, loaded_path = load_config(config_path)
    url = cfg.get("amqp", "url", fallback="").strip()
    exchange = cfg.get("amqp", "exchange", fallback="notify.events").strip()
    if not url:
        warn(f"missing [amqp] url in {loaded_path}; skipping AMQP notification")
        return
    if not exchange:
        warn(f"missing [amqp] exchange in {loaded_path}; skipping AMQP notification")
        return

    host = socket.gethostname()
    message = f"{unit} failed"
    body = {
        "severity": "warn",
        "channel": "auto",
        "source": "solari-backup",
        "title": "SolariNet backup failed",
        "message": message,
        "body": f"{message} on {host}",
        "unit": unit,
        "host": host,
        "ts": int(time.time()),
    }

    params = pika.URLParameters(url)
    for attr, value in (
        ("connection_attempts", 1),
        ("retry_delay", 0),
        ("socket_timeout", 5),
        ("stack_timeout", 5),
        ("blocked_connection_timeout", 5),
    ):
        try:
            setattr(params, attr, value)
        except Exception:
            pass

    conn = pika.BlockingConnection(params)
    try:
        channel = conn.channel()
        channel.basic_publish(
            exchange=exchange,
            routing_key="notify.warn",
            body=json.dumps(body, separators=(",", ":")),
            properties=pika.BasicProperties(
                content_type="application/json",
                delivery_mode=2,
            ),
        )
    finally:
        try:
            conn.close()
        except Exception:
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--unit", required=True, help="failed systemd unit name")
    parser.add_argument("--config", help="path to notify.conf")
    args = parser.parse_args()

    try:
        publish_failure(args.unit, args.config)
    except Exception as exc:
        warn(str(exc))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
