#!/usr/bin/env python3
"""alertbridge — SolariNet alert->MQ bridge + dead-man's-switch.

Implements HOST_HEALTH_CONTRACT §6 (Unit D). Two responsibilities, one
fail-soft loop:

1. BRIDGE — poll the `alertEvent` table in the xenon `solarinet` MariaDB for
   rows newer than a persisted checkpoint (max eventId processed) and publish
   each new fired event to the RabbitMQ topic exchange `notify.events` with
   routing key `notify.<severity>` and a `{title, body, severity, source}`
   JSON payload per deploy/notify/README.md's message contract. At-least-once:
   the checkpoint advances only after a successful publish, so a crash between
   publish and checkpoint re-sends rather than drops. Idempotent by eventId —
   already-checkpointed events are never re-published.

2. DEAD-MAN'S-SWITCH — each cycle, compute the last-report age per known node
   from `hostCurrent` (host clients) and `probeCurrent` (monitor vantages). A
   node that WAS reporting but has gone silent beyond max(120s, 3x sample
   interval) gets ONE synthetic `crit` "node <fqdn> stopped reporting" pushed
   to `notify.events`; it is re-armed (and an `info` recovery emitted) when it
   starts reporting again. This is the "monitor the monitor" guard that would
   have caught cesium's 26h silence.

The loop never crashes on a transient DB/MQ error: it logs, tears down, and
reconnects after a delay. Config in alertbridge.conf (see the .example).
"""
import argparse
import configparser
import json
import os
import signal
import sys
import time
from urllib.parse import urlsplit, urlunsplit

import pika
import pika.exceptions
import pymysql

# Message-level publish failures we retry (checkpoint NOT advanced). Connection-
# level pika errors also subclass AMQPError; treating them as a delivery failure
# here is safe because the outer loop reconnects on the next process_data_events.
PUBLISH_FAILURES = (
    pika.exceptions.UnroutableError,   # mandatory=True: broker returned the msg
    pika.exceptions.NackError,         # publisher confirms: broker nacked
    pika.exceptions.AMQPError,
)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_CONF = os.environ.get("ALERTBRIDGE_CONF",
                              os.path.join(SCRIPT_DIR, "alertbridge.conf"))
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} alertbridge: {msg}",
          flush=True)


def redact_amqp_url(url):
    """Return the amqp url with any user:pass@ password masked, safe to log.

    Never expose the broker credential in logs. On any parse trouble we fall
    back to a fully-opaque string rather than risk echoing the raw url (which
    still carries the password)."""
    try:
        p = urlsplit(url)
        if not p.password:
            return url  # nothing secret to hide
        host = p.hostname or ""
        if p.port:
            host = f"{host}:{p.port}"
        userinfo = f"{p.username or ''}:***"
        return urlunsplit((p.scheme, f"{userinfo}@{host}", p.path, p.query, p.fragment))
    except Exception:  # noqa: BLE001 — a malformed url must never leak via repr
        return "<amqp url unparseable; redacted>"


# --------------------------------------------------------------------------- #
# config + state                                                              #
# --------------------------------------------------------------------------- #
def load_cfg(path):
    c = configparser.ConfigParser()
    if not c.read(path):
        log(f"FATAL: cannot read config {path} "
            f"(copy alertbridge.conf.example to alertbridge.conf)")
        sys.exit(1)
    return c


def db_password(c):
    """DB password from config, else the SOLARI_DB_PASS env var.

    The systemd unit sources run/db.env (which `export`s SOLARI_DB_PASS), so
    the password need not live in this config at all — same convention as
    deploy/systemd/solarinet-snmp.service and deploy/deploy-monitor.sh.
    """
    pw = c.get("db", "password", fallback="").strip()
    if pw and not pw.startswith("CHANGE"):
        return pw
    env = os.environ.get("SOLARI_DB_PASS")
    if env:
        return env
    log("WARNING: no DB password in config or $SOLARI_DB_PASS")
    return pw


def db_connect(c):
    return pymysql.connect(
        host=c.get("db", "host", fallback="127.0.0.1"),
        port=c.getint("db", "port", fallback=3306),
        user=c.get("db", "user", fallback="solari"),
        password=db_password(c),
        database=c.get("db", "name", fallback="solarinet"),
        autocommit=True, connect_timeout=10,
        read_timeout=30, write_timeout=30,
        cursorclass=pymysql.cursors.DictCursor,
    )


def state_path(c):
    return c.get("state", "file",
                 fallback=os.path.join(SCRIPT_DIR, "alertbridge.state.json"))


def load_state(path):
    try:
        with open(path, "r") as f:
            s = json.load(f)
    except FileNotFoundError:
        s = {}
    except (OSError, ValueError) as e:
        log(f"WARNING: unreadable state {path}: {e!r}; starting fresh")
        s = {}
    s.setdefault("checkpoint", 0)      # max alertEvent.eventId published
    s.setdefault("silent", {})         # nodeId(str) -> {fqdn, sinceTs} while alerted
    return s


def save_state(path, state):
    """Atomically persist state (write temp + fsync + rename) so a crash
    mid-write can't corrupt the checkpoint. Returns True on success, False if
    the state could not be durably persisted — callers must stop advancing the
    checkpoint on False, or a restart would replay already-delivered events."""
    tmp = f"{path}.tmp"
    try:
        with open(tmp, "w") as f:
            json.dump(state, f)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
        return True
    except OSError as e:
        log(f"WARNING: could not save state {path}: {e!r}")
        return False


# --------------------------------------------------------------------------- #
# MQ publish                                                                   #
# --------------------------------------------------------------------------- #
def publish(ch, exchange, message):
    """Publish one notify-contract message; routing key notify.<severity>.

    The channel runs in publisher-confirm mode (ch.confirm_delivery()), so this
    call BLOCKS until the broker acks the message and returns None, or raises:
      - UnroutableError if mandatory=True and no queue is bound (msg returned),
      - NackError if the broker nacks (e.g. disk-alarm),
      - another AMQPError on a connection/channel fault.
    A raise here therefore means the event is NOT delivered; the caller must not
    advance the checkpoint."""
    severity = (message.get("severity") or "info").strip().lower()
    if severity not in ("info", "warn", "crit"):
        severity = "info"
    ch.basic_publish(
        exchange=exchange,
        routing_key=f"notify.{severity}",
        body=json.dumps(message),
        properties=pika.BasicProperties(delivery_mode=2,
                                        content_type="application/json"),
        mandatory=True,   # broker returns (-> UnroutableError) if nothing bound
    )


# --------------------------------------------------------------------------- #
# (1) bridge: alertEvent -> notify.events                                      #
# --------------------------------------------------------------------------- #
def bridge_alerts(db, ch, exchange, source, state, batch, spath):
    """Publish alertEvent rows past the checkpoint. For EACH row we (a) publish
    with publisher confirms + mandatory, (b) advance the checkpoint only after
    the broker CONFIRMS delivery, and (c) durably save_state() that new
    checkpoint before touching the next row. This makes a kill mid-batch unable
    to re-send an already-delivered event (exactly the "replay on restart" bug):
    the on-disk checkpoint never lags a confirmed publish, and never leads an
    unconfirmed one.

    Failure handling, both fail-soft (never crash the loop):
      - publish raises (unroutable / nack / any AMQP error): the event is NOT
        delivered -> do not advance, stop the batch, retry next cycle.
      - save_state() fails: the event IS delivered but we can't persist the new
        checkpoint -> revert the in-memory checkpoint to the last persisted
        value and stop the batch, so we neither publish more rows on an
        unpersisted checkpoint (unbounded replay on restart) nor silently lose
        the fact we haven't caught up. Retry next cycle."""
    persisted_cp = int(state["checkpoint"])
    with db.cursor() as cur:
        cur.execute(
            "SELECT e.eventId, e.severity, e.detail, e.targetId, "
            "       UNIX_TIMESTAMP(e.firedAt) AS firedTs, "
            "       n.hostFqdn, n.role, "
            "       r.metric, r.op, r.threshold "
            "FROM alertEvent e "
            "LEFT JOIN node n ON n.nodeId = e.nodeId "
            "LEFT JOIN alertRule r ON r.ruleId = e.ruleId "
            "WHERE e.eventId > %s ORDER BY e.eventId LIMIT %s",
            (persisted_cp, batch))
        rows = cur.fetchall()

    published = 0
    for r in rows:
        eid = int(r["eventId"])
        try:
            msg = event_to_message(r, source)
            publish(ch, exchange, msg)   # blocks for broker confirm; raises on failure
        except PUBLISH_FAILURES as e:
            log(f"DELIVERY FAILURE eventId={eid}: {e!r}; not advancing "
                f"checkpoint (still {persisted_cp}), retry next cycle")
            break
        except Exception as e:  # noqa: BLE001 — one bad row must not stall the loop
            log(f"ERROR building/publishing eventId={eid}: {e!r}; "
                f"stopping batch, will retry from checkpoint={persisted_cp}")
            break
        # Confirmed delivered. Advance + durably persist BEFORE the next row.
        state["checkpoint"] = eid
        if not save_state(spath, state):
            state["checkpoint"] = persisted_cp   # keep in-memory == on-disk
            log(f"ERROR: checkpoint {eid} delivered but not persisted; stopping "
                f"batch this cycle to avoid unbounded replay, retry next cycle")
            break
        persisted_cp = eid
        published += 1

    if published:
        log(f"bridged {published} alertEvent(s); checkpoint={persisted_cp}")
    return published


def event_to_message(r, source):
    """Map an alertEvent row to a notify-contract {title, body, severity,
    source} message."""
    severity = (r.get("severity") or "info").strip().lower()
    who = r.get("hostFqdn") or "unknown-node"
    metric = r.get("metric")

    if metric:
        op = {"gt": ">", "lt": "<", "eq": "=",
              "transition": "->"}.get(r.get("op"), r.get("op") or "")
        thr = r.get("threshold")
        thr = "" if thr is None else (f"{thr:g}" if isinstance(thr, float) else str(thr))
        title = f"{who}: {metric} {op} {thr}".rstrip()
    else:
        title = f"{who}: alert fired"

    parts = []
    if r.get("detail"):
        parts.append(str(r["detail"]))
    if r.get("targetId"):
        parts.append(f"target={r['targetId']}")
    parts.append(f"eventId={r['eventId']}")
    body = "; ".join(parts)

    return {"title": title, "body": body, "severity": severity, "source": source}


# --------------------------------------------------------------------------- #
# (2) dead-man's-switch                                                        #
# --------------------------------------------------------------------------- #
def node_ages(db):
    """Return {nodeId: {"fqdn": str, "age": int_seconds}} — the freshest
    last-report age per node, drawn from hostCurrent (host clients) and
    probeCurrent (monitor vantages). A node present in both takes the smaller
    (most recent) age. Only nodes that HAVE reported at least once appear, so
    a never-seen node can't trip the switch."""
    ages = {}

    def fold(nid, fqdn, age):
        if nid is None or age is None:
            return
        nid = str(nid)
        age = int(age)
        cur = ages.get(nid)
        if cur is None or age < cur["age"]:
            ages[nid] = {"fqdn": fqdn or f"node:{nid}", "age": age}

    with db.cursor() as cur:
        # host clients
        cur.execute(
            "SELECT h.nodeId, n.hostFqdn, "
            "       TIMESTAMPDIFF(SECOND, h.sampledAt, UTC_TIMESTAMP()) AS age "
            "FROM hostCurrent h LEFT JOIN node n ON n.nodeId = h.nodeId")
        for r in cur.fetchall():
            fold(r["nodeId"], r["hostFqdn"], r["age"])
        # monitor vantages: freshest probe sample per monitorNode
        cur.execute(
            "SELECT p.monitorNode AS nodeId, n.hostFqdn, "
            "       TIMESTAMPDIFF(SECOND, MAX(p.sampledAt), UTC_TIMESTAMP()) AS age "
            "FROM probeCurrent p LEFT JOIN node n ON n.nodeId = p.monitorNode "
            "GROUP BY p.monitorNode, n.hostFqdn")
        for r in cur.fetchall():
            fold(r["nodeId"], r["hostFqdn"], r["age"])
    return ages


def dead_mans_switch(db, ch, exchange, source, state, threshold, spath):
    """Emit ONE crit per node that has gone silent past `threshold`; re-arm
    (and emit an info recovery) when it reports again.

    Every state transition is committed only after the corresponding notify has
    been CONFIRMED delivered, and is then durably persisted:
      - arming: publish the crit first; only mark the node silent (and persist)
        after the broker confirms. A failed/undelivered crit leaves the node
        un-armed so we retry next cycle rather than swallowing the alert.
      - recovery: publish the info first; only clear the silent flag (and
        persist) after confirmation. If the recovery can't be delivered or
        persisted we keep the node ARMED and retry later, so we never drop the
        "resumed reporting" signal or re-fire a duplicate crit."""
    ages = node_ages(db)
    silent = state["silent"]

    for nid, info in ages.items():
        age = info["age"]
        fqdn = info["fqdn"]
        if age > threshold:
            if nid not in silent:
                # newly silent -> fire once
                msg = {
                    "title": f"node {fqdn} stopped reporting",
                    "body": (f"No report from {fqdn} for {age}s "
                             f"(threshold {threshold}s). Last-known monitoring "
                             f"data is stale; the node or its agent may be down."),
                    "severity": "crit",
                    "source": source,
                }
                try:
                    publish(ch, exchange, msg)   # confirmed before we arm
                except Exception as e:  # noqa: BLE001 — fail-soft: retry next cycle
                    log(f"ERROR emitting dead-man crit for {fqdn}: {e!r}; "
                        f"leaving node un-armed, retry later")
                    continue
                silent[nid] = {"fqdn": fqdn, "sinceTs": int(time.time())}
                if not save_state(spath, state):
                    del silent[nid]   # not persisted -> not armed; retry later
                    log(f"ERROR: could not persist dead-man arm for {fqdn}; "
                        f"retry later")
                    return
                log(f"DEAD-MAN: {fqdn} silent {age}s > {threshold}s -> crit emitted")
        else:
            if nid in silent:
                # recovered -> re-arm and announce
                msg = {
                    "title": f"node {fqdn} resumed reporting",
                    "body": f"{fqdn} is reporting again (age {age}s).",
                    "severity": "info",
                    "source": source,
                }
                try:
                    publish(ch, exchange, msg)   # confirmed before we re-arm
                except Exception as e:  # noqa: BLE001 — keep armed, retry later
                    log(f"ERROR emitting dead-man recovery for {fqdn}: {e!r}; "
                        f"keeping node armed, retry later")
                    continue
                prev = silent[nid]
                del silent[nid]   # only after the recovery is confirmed delivered
                if not save_state(spath, state):
                    silent[nid] = prev   # keep armed until we can persist
                    log(f"ERROR: could not persist dead-man recovery for {fqdn}; "
                        f"keeping node armed, retry later")
                    return
                log(f"DEAD-MAN: {fqdn} recovered (age {age}s) -> re-armed")


# --------------------------------------------------------------------------- #
# main loop                                                                    #
# --------------------------------------------------------------------------- #
def run(conf_path):
    c = load_cfg(conf_path)
    exchange = c.get("amqp", "exchange", fallback="notify.events")
    url = c.get("amqp", "url")
    # The durable queue + binding notifyd owns; declaring them defensively (see
    # below) matches deploy/notify/notify.conf defaults.
    notify_queue = c.get("amqp", "queue", fallback="notify.dispatch")
    notify_binding = c.get("amqp", "binding_key", fallback="notify.#")
    poll = c.getfloat("bridge", "poll_interval_sec", fallback=10.0)
    batch = c.getint("bridge", "batch", fallback=500)
    source = c.get("bridge", "source", fallback="alertbridge")
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)

    sample_interval = c.getint("deadman", "sample_interval_sec", fallback=15)
    threshold = max(120, 3 * sample_interval)
    deadman_enabled = c.getboolean("deadman", "enabled", fallback=True)

    # Parse the AMQP url ONCE, up front, and never log the raw url (it carries
    # the broker password). Guard the parse so a malformed url can't dump the
    # credential via an exception repr.
    url_safe = redact_amqp_url(url)
    try:
        amqp_params = pika.URLParameters(url)
    except Exception:  # noqa: BLE001 — do NOT interpolate the exception (may echo the url)
        log(f"FATAL: cannot parse amqp url ({url_safe}); check [amqp] url in config")
        sys.exit(2)

    spath = state_path(c)
    log(f"config {conf_path}; amqp={url_safe} exchange={exchange} poll={poll}s "
        f"deadman_threshold={threshold}s state={spath}")

    while _run:
        db = mq = ch = None
        try:
            db = db_connect(c)
            mq = pika.BlockingConnection(amqp_params)
            ch = mq.channel()
            # Publisher confirms: basic_publish now BLOCKS for a broker ack and
            # raises on nack/return, so we only ever advance the checkpoint after
            # an event is truly on the broker (no silent alert loss).
            ch.confirm_delivery()
            # notifyd declares the same durable topic exchange; asserting it
            # here is harmless and lets the bridge come up before notifyd.
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            # Defensively declare the durable queue + binding notifyd expects, so
            # a message published before notifyd has ever run isn't dropped by the
            # topic exchange (which discards messages matching no binding). With
            # mandatory=True this is belt-and-suspenders — we do both.
            ch.queue_declare(queue=notify_queue, durable=True)
            ch.queue_bind(exchange=exchange, queue=notify_queue,
                          routing_key=notify_binding)

            state = load_state(spath)
            log(f"connected; checkpoint={state['checkpoint']} "
                f"tracked_silent={list(state['silent'])}")

            while _run:
                # Both helpers advance + durably persist state per confirmed
                # publish, so a kill mid-cycle can't replay delivered events.
                bridge_alerts(db, ch, exchange, source, state, batch, spath)
                if deadman_enabled:
                    dead_mans_switch(db, ch, exchange, source, state, threshold, spath)
                # keep AMQP heartbeats alive, then idle
                mq.process_data_events(0)
                time.sleep(poll)
        except Exception as e:  # noqa: BLE001 — daemon: log + reconnect, never die
            log(f"error: {e!r}; reconnecting in {reconnect}s")
            time.sleep(reconnect)
        finally:
            for x in (ch, mq, db):
                try:
                    x and x.close()
                except Exception:
                    pass
    log("stopped")


def _stop(*_):
    global _run
    _run = False
    log("shutting down")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--config", default=DEFAULT_CONF)
    args = ap.parse_args()
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    run(args.config)


if __name__ == "__main__":
    main()
