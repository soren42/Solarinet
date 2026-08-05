"""Web Push sender for installed SolariNet dashboards.

[sender.webpush] config (notify.conf):
    db_host = 127.0.0.1
    db_port = 3306
    db_name = solarinet
    db_user = solarinet
    db_pass = CHANGE_ME
    vapid_private_key = /home/jason/Code/Solarinet/run/vapid_private.pem
    vapid_subject = mailto:admin@solarian.net

Every active browser subscription receives each message; message["_to"] is
intentionally ignored. Delivery failures are isolated per subscription. A 404
or 410 response means the push service has discarded that endpoint, so it is
removed from push_subscriptions.
"""
import json
import logging
import os
import subprocess

try:
    from pywebpush import WebPushException, webpush
except ImportError:  # handled as a soft sender failure below
    WebPushException = Exception
    webpush = None

log = logging.getLogger("notify.sender.webpush")


def _db_config(cfg):
    return {
        "host": cfg.get("db_host", "127.0.0.1"),
        "port": int(cfg.get("db_port", "3306") or 3306),
        "database": cfg.get("db_name", "solarinet"),
        "user": cfg.get("db_user", ""),
        "password": cfg.get("db_pass", ""),
    }


def _rows(db):
    """Read subscriptions, preferring PyMySQL but retaining a mysql-client fallback."""
    try:
        import pymysql  # optional; notifyd's base image need not carry it
        conn = pymysql.connect(**db, connect_timeout=10, read_timeout=15, write_timeout=15)
        try:
            with conn.cursor() as cur:
                cur.execute("SELECT endpoint, p256dh, auth FROM push_subscriptions")
                return cur.fetchall()
        finally:
            conn.close()
    except ImportError:
        pass

    env = os.environ.copy()
    env["MYSQL_PWD"] = db["password"]
    proc = subprocess.run([
        "mysql", "--batch", "--skip-column-names", "--raw", "--connect-timeout=10",
        "-h", db["host"], "-P", str(db["port"]), "-u", db["user"], db["database"],
        "-e", "SELECT endpoint, p256dh, auth FROM push_subscriptions",
    ], capture_output=True, text=True, timeout=20, env=env, check=True)
    return [tuple(line.split("\t", 2)) for line in proc.stdout.splitlines() if line]


def _delete(db, endpoint):
    """Delete a permanently invalid endpoint without putting passwords in argv."""
    try:
        import pymysql
        conn = pymysql.connect(**db, connect_timeout=10, read_timeout=15, write_timeout=15)
        try:
            with conn.cursor() as cur:
                cur.execute("DELETE FROM push_subscriptions WHERE endpoint = %s", (endpoint,))
            conn.commit()
        finally:
            conn.close()
        return
    except ImportError:
        pass

    # mysql's CLI has no bind parameters. SQL single-quote escaping keeps this
    # fallback limited to the endpoint literal supplied by our own database.
    quoted = endpoint.replace("\\", "\\\\").replace("'", "\\'")
    env = os.environ.copy()
    env["MYSQL_PWD"] = db["password"]
    subprocess.run([
        "mysql", "--batch", "--connect-timeout=10", "-h", db["host"], "-P", str(db["port"]),
        "-u", db["user"], db["database"], "-e",
        "DELETE FROM push_subscriptions WHERE endpoint = '" + quoted + "'",
    ], capture_output=True, text=True, timeout=20, env=env, check=True)


def _status(exc):
    response = getattr(exc, "response", None)
    return getattr(response, "status_code", None) or getattr(response, "status", None)


def send(message, cfg):
    if cfg is None:
        log.error("webpush sender has no [sender.webpush] config section; skipping")
        return False
    if webpush is None:
        log.error("webpush sender: pywebpush is not installed; skipping")
        return False
    try:
        db = _db_config(cfg)
    except (TypeError, ValueError):
        log.error("webpush sender: invalid database configuration; skipping")
        return False
    key = cfg.get("vapid_private_key", "")
    subject = cfg.get("vapid_subject", "")
    if not db["user"] or not key or not subject:
        log.error("webpush sender: database credentials, vapid_private_key, and vapid_subject are required; skipping")
        return False

    try:
        subscriptions = _rows(db)
    except Exception as exc:  # noqa: BLE001 - sender contract is fail-soft
        log.warning("webpush: could not load subscriptions: %r", exc)
        return False

    if not subscriptions:
        # No installed dashboards have enabled Web Push yet. That's not a
        # failure — there is simply nothing to deliver — so return True so the
        # dispatcher acks and doesn't log a phantom "webpush failed" on every
        # alert. Real delivery failures below still return False as usual.
        log.debug("webpush: no subscriptions; nothing to send")
        return True

    payload = json.dumps({
        "title": message.get("title") or "SolariNet",
        "body": message.get("body") or "",
        "severity": message.get("severity") or "info",
        "tag": message.get("tag") or ("solarinet-" + str(message.get("severity") or "info")),
        "url": message.get("url") or "/",
    })
    ok_any = False
    for endpoint, p256dh, auth in subscriptions:
        try:
            webpush(
                subscription_info={"endpoint": endpoint, "keys": {"p256dh": p256dh, "auth": auth}},
                data=payload,
                vapid_private_key=key,
                vapid_claims={"sub": subject},
            )
            ok_any = True
        except WebPushException as exc:
            if _status(exc) in (404, 410):
                try:
                    _delete(db, endpoint)
                    log.info("webpush: pruned expired subscription")
                except Exception as delete_exc:  # noqa: BLE001
                    log.warning("webpush: failed to prune expired subscription: %r", delete_exc)
            else:
                log.warning("webpush: delivery failed: %r", exc)
        except Exception as exc:  # noqa: BLE001 - one endpoint must not stop the rest
            log.warning("webpush: delivery failed: %r", exc)
    return ok_any
