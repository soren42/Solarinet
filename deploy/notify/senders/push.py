"""push sender — interface-only stub.

No transport exists yet. This is a placeholder for a future
SolariNet-dashboard / Tab5 push surface (e.g. a websocket fan-out from the
dashboard server, or a small push-gateway the dashboard polls). Wiring it up
is out of scope for the initial notification service; this module exists so
[senders] enabled=push and per-severity routing referencing "push" don't
crash, and so the seam is obvious when that surface is built.
"""

import logging

log = logging.getLogger("notify.sender.push")


def send(message, cfg):
    endpoint = (cfg.get("endpoint") if cfg else None) or ""
    if not endpoint:
        log.debug(
            "push sender not configured (sender.push.endpoint empty); "
            "dropping push for %r (log sender is the durable record)",
            message.get("title"),
        )
        return False

    # TODO: implement once the dashboard/Tab5 push surface exists. Candidate
    # shape: POST message JSON to `endpoint`, expect 2xx.
    log.warning("push sender has no transport implemented yet (endpoint=%s)", endpoint)
    return False
