#!/usr/bin/env python3
"""opied — Opie, the AI on-call SA.

Watches the SolariNet alertEvent stream. When a failure crosses the trigger
threshold (any crit, or a storm of warns), Opie launches an Opus-4.8 (medium)
investigation that is confined to READ-ONLY diagnostics via `opie-probe`, writes
a root-cause + recommendations report to `opieReport`, and pushes a tight summary
to notify.events (-> iMessage). The full writeup lives in the dashboard.

Design (see deploy/opie/README.md):
  * Trigger: severity=crit ALWAYS; warn only when >= STORM_COUNT related warns land
    within STORM_WINDOW_SEC on the same node. Per-incident dedup via opieReport's
    UNIQUE incidentKey. Hourly cap so a cascade can't run up cost.
  * Investigate: `claude -p --model <opus> --allowedTools "Bash(opie-probe:*) Read Grep Glob"`.
    That allowlist is the ENTIRE tool surface — opie-probe only runs vetted
    read-only commands, so an investigation cannot change anything.
  * Fail-soft: never crash the loop; a failed investigation marks the report
    'failed' and moves on.
"""
import configparser
import json
import os
import re
import subprocess
import sys
import time
from datetime import datetime, timezone

try:
    import pymysql
except ImportError:
    pymysql = None
try:
    import pika
except ImportError:
    pika = None

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONF_PATH = os.environ.get("OPIE_CONF", os.path.join(SCRIPT_DIR, "opie.conf"))


def log(msg):
    print(f"{datetime.now(timezone.utc):%Y-%m-%dT%H:%M:%SZ} opie: {msg}", flush=True)


def load_config():
    if not os.path.exists(CONF_PATH):
        log(f"FATAL: config {CONF_PATH} missing (copy opie.conf.example)")
        sys.exit(1)
    c = configparser.ConfigParser(inline_comment_prefixes=("#", ";"))
    c.read(CONF_PATH)
    return c


def db_password(cfg):
    return cfg.get("db", "password", fallback="").strip() or os.environ.get("SOLARI_DB_PASS", "")


def connect_db(cfg):
    return pymysql.connect(
        host=cfg.get("db", "host", fallback="127.0.0.1"),
        port=cfg.getint("db", "port", fallback=3306),
        user=cfg.get("db", "user", fallback="solari"),
        password=db_password(cfg),
        database=cfg.get("db", "name", fallback="solarinet"),
        autocommit=True, connect_timeout=8, cursorclass=pymysql.cursors.DictCursor,
    )


# ---- state (checkpoint of last alertEvent eventId seen) --------------------- #
def state_path(cfg):
    return cfg.get("state", "file", fallback=os.path.join(SCRIPT_DIR, "opie.state.json"))


def load_state(cfg):
    try:
        with open(state_path(cfg)) as f:
            return json.load(f)
    except Exception:
        return {"checkpoint": 0}


def save_state(cfg, st):
    tmp = state_path(cfg) + ".tmp"
    with open(tmp, "w") as f:
        json.dump(st, f)
    os.replace(tmp, state_path(cfg))


# ---- notify.events summary push -------------------------------------------- #
def publish_summary(cfg, report):
    url = cfg.get("amqp", "url", fallback="").strip()
    if not url or pika is None:
        log("WARNING: no amqp url / pika; skipping iMessage summary")
        return
    exchange = cfg.get("amqp", "exchange", fallback="notify.events")
    sev = report.get("severity") or "warn"
    body = {
        "title": f"Opie: {report.get('hostFqdn') or 'incident'} — {report.get('one_liner','analysis ready')}",
        "body": report.get("summary") or "Root-cause analysis ready in the dashboard.",
        "severity": "warn" if sev == "warn" else "crit",
        "source": "opie",
    }
    try:
        conn = pika.BlockingConnection(pika.URLParameters(url))
        ch = conn.channel()
        ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
        ch.confirm_delivery()
        ch.basic_publish(exchange, f"notify.{body['severity']}", json.dumps(body),
                         properties=pika.BasicProperties(delivery_mode=2), mandatory=True)
        conn.close()
        log(f"summary pushed for report {report.get('reportId')}")
    except Exception as e:
        log(f"summary publish failed: {e!r}")


# ---- trigger evaluation ----------------------------------------------------- #
def new_events(conn, checkpoint, batch):
    with conn.cursor() as cur:
        cur.execute(
            "SELECT e.eventId, e.ruleId, e.nodeId, e.severity, e.firedAt, e.detail, "
            "       r.metric, r.scope, n.hostFqdn "
            "FROM alertEvent e "
            "LEFT JOIN alertRule r ON r.ruleId=e.ruleId "
            "LEFT JOIN node n ON n.nodeId=e.nodeId "
            "WHERE e.eventId > %s AND e.detail LIKE 'FIRED%%' "
            "ORDER BY e.eventId ASC LIMIT %s",
            (checkpoint, batch),
        )
        return cur.fetchall()


def warn_storm(conn, nodeId, window_sec, count):
    """True if >= count warn FIRED events on this node within the window."""
    with conn.cursor() as cur:
        cur.execute(
            "SELECT COUNT(*) c FROM alertEvent "
            "WHERE nodeId=%s AND severity='warn' AND detail LIKE 'FIRED%%' "
            "AND firedAt >= (NOW() - INTERVAL %s SECOND)",
            (nodeId, window_sec),
        )
        return (cur.fetchone() or {}).get("c", 0) >= count


def incident_key(ev, kind):
    # One investigation per (node, metric-family, ~15-min bucket) for crit;
    # per (node, storm, bucket) for a warn storm.
    fired = ev["firedAt"]
    bucket = int(fired.replace(tzinfo=timezone.utc).timestamp() // 900) if hasattr(fired, "replace") else 0
    metric = (ev.get("metric") or "unknown").split(".")[0]
    node = ev.get("nodeId") or "0"
    return f"{node}:{'storm' if kind=='warn-storm' else metric}:{bucket}"


def already_open(conn, key):
    with conn.cursor() as cur:
        cur.execute("SELECT reportId FROM opieReport WHERE incidentKey=%s", (key,))
        return cur.fetchone() is not None


def hourly_count(conn):
    with conn.cursor() as cur:
        cur.execute("SELECT COUNT(*) c FROM opieReport WHERE startedAt >= (NOW() - INTERVAL 1 HOUR)")
        return (cur.fetchone() or {}).get("c", 0)


def open_report(conn, key, kind, ev):
    with conn.cursor() as cur:
        cur.execute(
            "INSERT INTO opieReport (incidentKey, triggerKind, firstEventId, nodeId, hostFqdn, severity, status) "
            "VALUES (%s,%s,%s,%s,%s,%s,'investigating')",
            (key, kind, ev["eventId"], ev.get("nodeId"), ev.get("hostFqdn"), ev.get("severity")),
        )
        return cur.lastrowid


def close_report(conn, reportId, status, summary, analysis, model, dur):
    with conn.cursor() as cur:
        cur.execute(
            "UPDATE opieReport SET status=%s, summary=%s, analysis=%s, model=%s, "
            "durationSec=%s, finishedAt=NOW() WHERE reportId=%s",
            (status, summary, analysis, model, dur, reportId),
        )


# ---- the investigation (Opus, read-only) ----------------------------------- #
PROMPT = """You are **Opie**, the akoria homelab's permanent AI on-call SA. A monitoring alert just
fired and you follow up on the raw notification with a fast, high-level root-cause analysis and
concrete recommendations — exactly what a good on-call sysadmin does for the team.

## The alert
- host: {host}
- severity: {severity}
- metric: {metric}
- detail: {detail}
- fired: {fired}
- recent related alerts on this host:
{related}

## How to investigate
Use ONLY the `opie-probe` tool. It runs vetted READ-ONLY diagnostics on a host and is your entire
toolbox (you cannot and must not change anything). Invoke it as shell commands, e.g.:
  opie-probe {host} failed-units
  opie-probe {host} unit certbot.service
  opie-probe {host} logs certbot.service
  opie-probe {host} kernel
  opie-probe {host} disk
  opie-probe {host} smart sda
  opie-probe {host} dmesg-errors
  opie-probe {host} net
  opie-probe {host} load
  opie-probe {host} service-conf <unit>
Checks: failed-units, unit <u>, logs <u>, kernel, disk, mounts, smart <dev>, net, proc, load,
dmesg-errors, service-conf <u>, ping. Run as many as you need to reach a confident root cause;
follow the evidence. Be efficient — a handful of well-chosen probes beats twenty.

## Output — end your response with EXACTLY these two fenced blocks and nothing after:
<<<OPIE_SUMMARY>>>
A 2-3 sentence plain summary for an iMessage: what broke, the most likely cause, and the single
most important next action. No markdown.
<<<OPIE_ANALYSIS>>>
Full markdown: **What happened**, **Root cause** (with the evidence you found), **Blast radius /
what's safe**, and a numbered **Recommendations** list (specific, actionable, least-risk first).
"""


def build_prompt(conn, ev):
    related = "  (none)"
    with conn.cursor() as cur:
        cur.execute(
            "SELECT severity, detail, firedAt FROM alertEvent WHERE nodeId=%s "
            "AND firedAt >= (NOW() - INTERVAL 30 MINUTE) ORDER BY eventId DESC LIMIT 12",
            (ev.get("nodeId"),),
        )
        rows = cur.fetchall()
    if rows:
        related = "\n".join(f"  - [{r['severity']}] {r['firedAt']} {r['detail'][:80]}" for r in rows)
    return PROMPT.format(
        host=ev.get("hostFqdn") or "unknown", severity=ev.get("severity"),
        metric=ev.get("metric"), detail=ev.get("detail"), fired=ev.get("firedAt"), related=related,
    )


def investigate(cfg, prompt):
    model = cfg.get("opie", "model", fallback="claude-opus-4-8")
    timeout = cfg.getint("opie", "timeout_sec", fallback=600)
    probe = cfg.get("opie", "probe_cmd", fallback="opie-probe")
    allowed = f"Bash({probe}:*) Read Grep Glob"
    cmd = ["claude", "-p", prompt, "--model", model, "--allowedTools", allowed]
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout,
                          cwd=cfg.get("opie", "workdir", fallback=SCRIPT_DIR))
    dur = int(time.time() - t0)
    out = proc.stdout or ""
    if proc.returncode != 0:
        return None, None, model, dur, f"claude rc={proc.returncode}: {(proc.stderr or '')[:300]}"
    summary, analysis = parse_output(out)
    if not summary and not analysis:
        return None, None, model, dur, "no OPIE_SUMMARY/ANALYSIS markers in output"
    return summary, analysis, model, dur, None


def parse_output(out):
    m = re.search(r"<<<OPIE_SUMMARY>>>(.*?)<<<OPIE_ANALYSIS>>>(.*)$", out, re.S)
    if m:
        return m.group(1).strip(), m.group(2).strip()
    # tolerant fallback: summary marker only
    m = re.search(r"<<<OPIE_SUMMARY>>>(.*)$", out, re.S)
    if m:
        return m.group(1).strip()[:500], out.strip()
    return "", ""


def handle(cfg, conn, ev):
    kind = "crit" if ev.get("severity") == "crit" else None
    if kind is None:
        sc = cfg.getint("trigger", "storm_count", fallback=3)
        sw = cfg.getint("trigger", "storm_window_sec", fallback=600)
        if ev.get("severity") == "warn" and warn_storm(conn, ev.get("nodeId"), sw, sc):
            kind = "warn-storm"
    if kind is None:
        return  # warn that isn't a storm -> no investigation
    key = incident_key(ev, kind)
    if already_open(conn, key):
        return
    cap = cfg.getint("trigger", "hourly_cap", fallback=8)
    if hourly_count(conn) >= cap:
        log(f"hourly cap {cap} reached; skipping {key}")
        return
    reportId = open_report(conn, key, kind, ev)
    log(f"investigating {key} (report {reportId}) trigger={kind} host={ev.get('hostFqdn')}")
    try:
        summary, analysis, model, dur, err = investigate(cfg, build_prompt(conn, ev))
    except subprocess.TimeoutExpired:
        summary, analysis, model, dur, err = None, None, None, None, "investigation timed out"
    except Exception as e:  # noqa: BLE001
        summary, analysis, model, dur, err = None, None, None, None, f"exception: {e!r}"
    if err:
        close_report(conn, reportId, "failed", (err or "")[:500], None, model, dur)
        log(f"report {reportId} FAILED: {err}")
        return
    close_report(conn, reportId, "done", summary[:500], analysis, model, dur)
    log(f"report {reportId} done in {dur}s ({model})")
    publish_summary(cfg, {"reportId": reportId, "hostFqdn": ev.get("hostFqdn"),
                          "severity": ev.get("severity"), "summary": summary,
                          "one_liner": (summary.split(".")[0][:60] if summary else "analysis ready")})


def main():
    if pymysql is None:
        log("FATAL: pymysql not installed"); sys.exit(1)
    cfg = load_config()
    poll = cfg.getint("opie", "poll_interval_sec", fallback=15)
    batch = cfg.getint("opie", "batch", fallback=50)
    log(f"starting; poll={poll}s model={cfg.get('opie','model',fallback='claude-opus-4-8')}")
    while True:
        try:
            conn = connect_db(cfg)
            st = load_state(cfg)
            evs = new_events(conn, st.get("checkpoint", 0), batch)
            for ev in evs:
                try:
                    handle(cfg, conn, ev)
                except Exception as e:  # noqa: BLE001 — never let one event kill the loop
                    log(f"handle error on event {ev.get('eventId')}: {e!r}")
                st["checkpoint"] = ev["eventId"]
                save_state(cfg, st)
            conn.close()
        except Exception as e:  # noqa: BLE001
            log(f"loop error: {e!r}")
        time.sleep(poll)


if __name__ == "__main__":
    main()
