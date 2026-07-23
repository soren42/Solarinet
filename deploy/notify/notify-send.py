#!/usr/bin/env python3
"""notify-send — publish a one-off message to notify.events (-> notifyd -> senders).

For an agent/operator to grab the user's attention out-of-band. Routing follows
notify.conf by severity (info=log/mqtt, warn/crit also -> Apple iMessage).

  notify-send.py --severity warn --title "..." --body "..." [--source claude]

AMQP url is read from deploy/notify/notify.conf ([amqp] url).
"""
import argparse
import configparser
import json
import os
import sys

import pika

HERE = os.path.dirname(os.path.abspath(__file__))


def amqp_url():
    c = configparser.ConfigParser()
    c.read(os.path.join(HERE, "notify.conf"))
    url = c.get("amqp", "url", fallback="").strip()
    if not url:
        sys.exit("no [amqp] url in deploy/notify/notify.conf")
    return url, c.get("amqp", "exchange", fallback="notify.events").strip()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--severity", default="warn", choices=["info", "warn", "crit"])
    p.add_argument("--title", required=True)
    p.add_argument("--body", default="")
    p.add_argument("--source", default="claude")
    a = p.parse_args()

    url, exchange = amqp_url()
    msg = {"title": a.title, "body": a.body, "severity": a.severity, "source": a.source}
    conn = pika.BlockingConnection(pika.URLParameters(url))
    ch = conn.channel()
    ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
    ch.confirm_delivery()
    ch.basic_publish(exchange, f"notify.{a.severity}", json.dumps(msg),
                     properties=pika.BasicProperties(delivery_mode=2,
                                                     content_type="application/json"),
                     mandatory=True)
    conn.close()
    print(f"sent [{a.severity}] {a.title!r} -> {exchange}")


if __name__ == "__main__":
    main()
