"""Pluggable notification senders.

Sender interface (each sender module must expose this at module scope):

    def send(message: dict, cfg: "configparser.SectionProxy | dict") -> bool:
        '''Attempt delivery.  Return True on success, False on (soft) failure.

        message is the parsed notify.events JSON payload (see README.md's
        "Message contract"), with two dispatcher-added conveniences:
          - message["_to"]:  list[str], resolved recipients for this sender
                              (message["to"] if present, else the sender's
                              configured default_*_to).
          - message["_routing_key"]: the AMQP routing key the message arrived on.

        A sender must NOT raise for expected failure modes (host unreachable,
        transport not configured, etc.) — catch those, log, and return False
        so the dispatcher can move on to the next sender / ack the message.
        Let unexpected exceptions propagate only if they indicate a bug.
        '''

Registry: `load_senders(names)` below maps a name (e.g. "sms") to the
`senders.sms_tachyon` module... actually the module *file* is sms_tachyon.py
but the registry key used in config ([senders] enabled=... and [routing])
is the short name "sms". See REGISTRY below for the name -> module mapping.
"""

import importlib
import logging

log = logging.getLogger("notify.senders")

# short config name -> module name under deploy/notify/senders/
REGISTRY = {
    "log": "senders.log",
    "mqtt": "senders.mqtt",
    "sms": "senders.sms_tachyon",
    "push": "senders.push",
    "apple": "senders.apple",
    "email": "senders.mail",   # file is mail.py — "email" would shadow the stdlib module
}


def load_senders(names):
    """Import and return {short_name: module} for each name in names.

    Unknown names are logged and skipped (never raises) so a typo in
    notify.conf degrades to "fewer senders", not a crash.
    """
    loaded = {}
    for name in names:
        name = name.strip()
        if not name:
            continue
        modpath = REGISTRY.get(name)
        if modpath is None:
            log.error("unknown sender %r in [senders] enabled= (known: %s)",
                       name, ", ".join(sorted(REGISTRY)))
            continue
        try:
            loaded[name] = importlib.import_module(modpath)
        except Exception:
            log.exception("failed to import sender %r (%s)", name, modpath)
    return loaded
