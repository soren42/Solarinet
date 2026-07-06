"""log sender — the always-works fallback.

Writes one line per notification to a local file (default
/var/log/solarinet/notify.log) and to the Python logger, so it also lands in
journald when notifyd runs under systemd. This sender should basically never
fail; if the file can't be written, we still count on the journald/stderr
copy so the notification isn't silently lost.
"""

import datetime
import json
import logging
import os

log = logging.getLogger("notify.sender.log")


def send(message, cfg):
    line = _format(message)

    # Always emit to the Python logger -> journald under systemd.
    log.info("NOTIFY %s", line)

    path = (cfg.get("path") if cfg else None) or "/var/log/solarinet/notify.log"
    try:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "a", encoding="utf-8") as fh:
            fh.write(line + "\n")
    except OSError as exc:
        # Journald copy above already happened, so this is a degraded-but-ok
        # outcome, not a hard failure of the sender.
        log.warning("could not write %s: %s", path, exc)

    return True


def _format(message):
    ts = message.get("ts")
    when = (
        datetime.datetime.fromtimestamp(ts, tz=datetime.timezone.utc).isoformat()
        if isinstance(ts, (int, float))
        else datetime.datetime.now(tz=datetime.timezone.utc).isoformat()
    )
    fields = {
        "ts": when,
        "severity": message.get("severity", "info"),
        "source": message.get("source", "unknown"),
        "routing_key": message.get("_routing_key", ""),
        "title": message.get("title", ""),
        "body": message.get("body", ""),
        "to": message.get("_to", []),
    }
    return json.dumps(fields, sort_keys=True)
