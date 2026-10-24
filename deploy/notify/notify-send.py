#!/usr/bin/python3 -I
"""notify-send — publish a one-off message to notify.events (-> notifyd -> senders).

For an agent/operator to grab the user's attention out-of-band. Routing follows
notify.conf by severity (info=log/mqtt, warn/crit also -> Apple iMessage).

  notify-send.py --severity warn --title "..." --body "..." [--source claude]

AMQP url is read from deploy/notify/notify.conf ([amqp] url), or a path passed
with --config for installed callers.
"""
import argparse
import configparser
import json
import os
import pathlib
import stat
import sys

import pika

HERE = os.path.dirname(os.path.abspath(__file__))


def amqp_url(config_path):
    path = pathlib.Path(config_path)
    if not path.is_absolute() or ".." in path.parts or path.name in {"", ".", ".."}:
        raise ValueError("notify config path must be absolute and contain no parent traversal")
    directory_fd = None
    file_fd = None
    try:
        if os.geteuid() == 0:
            directory_fd = os.open("/", os.O_RDONLY | os.O_DIRECTORY)
            for component in path.parts[1:-1]:
                child_fd = os.open(component, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW,
                                   dir_fd=directory_fd)
                parent_info = os.fstat(child_fd)
                if (parent_info.st_uid != 0
                        or parent_info.st_mode & (stat.S_IWGRP | stat.S_IWOTH)):
                    os.close(child_fd)
                    raise ValueError("root notify config path must use root-owned, protected directories")
                os.close(directory_fd)
                directory_fd = child_fd
            file_fd = os.open(path.name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=directory_fd)
        else:
            file_fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
        info = os.fstat(file_fd)
        if (not stat.S_ISREG(info.st_mode) or info.st_uid != os.geteuid()
                or info.st_mode & 0o077):
            raise ValueError("notify config must be a private regular file owned by the service user")
        with os.fdopen(file_fd, "r", encoding="utf-8") as stream:
            file_fd = None
            contents = stream.read()
    except OSError as exc:
        raise ValueError("notify config cannot be safely read") from exc
    finally:
        if file_fd is not None:
            os.close(file_fd)
        if directory_fd is not None:
            os.close(directory_fd)
    c = configparser.ConfigParser()
    try:
        c.read_string(contents)
    except configparser.Error as exc:
        raise ValueError("notify config is invalid") from exc
    url = c.get("amqp", "url", fallback="").strip()
    if not url:
        raise ValueError("notify config has no AMQP URL")
    return url, c.get("amqp", "exchange", fallback="notify.events").strip()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--severity", default="warn", choices=["info", "warn", "crit"])
    p.add_argument("--title", required=True)
    p.add_argument("--body", default="")
    p.add_argument("--source", default="claude")
    p.add_argument("--config", default=os.path.join(HERE, "notify.conf"))
    a = p.parse_args()

    try:
        url, exchange = amqp_url(a.config)
    except ValueError as exc:
        p.exit(2, f"{p.prog}: {exc}\n")
    conn = None
    try:
        msg = {"title": a.title, "body": a.body, "severity": a.severity, "source": a.source}
        conn = pika.BlockingConnection(pika.URLParameters(url))
        ch = conn.channel()
        ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
        ch.confirm_delivery()
        ch.basic_publish(exchange, f"notify.{a.severity}", json.dumps(msg),
                         properties=pika.BasicProperties(delivery_mode=2,
                                                         content_type="application/json"),
                         mandatory=True)
    except Exception:
        p.exit(2, f"{p.prog}: notification publishing failed\n")
    finally:
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass
    print(f"sent [{a.severity}] {a.title!r} -> {exchange}")


if __name__ == "__main__":
    main()
