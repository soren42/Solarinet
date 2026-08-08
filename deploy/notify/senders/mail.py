"""email sender — notifications via iCloud SMTP from a dedicated source address.

Sends each notify.events message as an email from a dedicated Apple ID
(e.g. admin@solarian.net) to the configured recipient(s), so alerts arrive as a
distinct, VIP-able, custom-tone-able source on every device — and, unlike the
iMessage relay, they work even when no Mac is awake (no device in the loop).

Auth uses an Apple **app-specific password** (never the account's real password);
generate one at appleid.apple.com and put it in notify.conf (gitignored).

[sender.email] config (notify.conf):
    smtp_host = smtp.mail.me.com     # iCloud
    smtp_port = 587                  # STARTTLS
    smtp_user = admin@solarian.net
    smtp_pass = xxxx-xxxx-xxxx-xxxx   # app-specific password
    from_addr = SolariNet <admin@solarian.net>   # optional; defaults to smtp_user
Recipients come from message["_to"] (message["to"], else [defaults] default_email_to)
— email addresses.

Never raises for expected failures (SMTP unreachable, auth, not configured) — logs
and returns False so the log/mqtt/other senders still record the event.
"""
import logging
import smtplib
import ssl
from email.message import EmailMessage

log = logging.getLogger("notify.sender.email")


def _format(message):
    sev = (message.get("severity") or "info").strip().upper()
    title = (message.get("title") or "SolariNet").strip()
    body = (message.get("body") or "").strip()
    subject = f"[{sev}] {title}"
    text = title + (("\n\n" + body) if body else "")
    return subject, text


def send(message, cfg):
    if cfg is None:
        log.error("email sender has no [sender.email] config section; skipping")
        return False
    host = cfg.get("smtp_host", "smtp.mail.me.com")
    try:
        port = int(cfg.get("smtp_port", "587") or 587)
    except ValueError:
        port = 587
    user = cfg.get("smtp_user", "")
    pw = cfg.get("smtp_pass", "")
    if not user or not pw:
        log.error("email sender: smtp_user / smtp_pass not set in [sender.email]; skipping")
        return False
    from_addr = cfg.get("from_addr", "") or user
    recipients = message.get("_to") or []
    if not recipients:
        log.warning("email sender: no recipients for %r; skipping", message.get("title"))
        return False

    subject, text = _format(message)
    ok_any = False
    try:
        ctx = ssl.create_default_context()
        with smtplib.SMTP(host, port, timeout=30) as s:
            s.starttls(context=ctx)
            s.login(user, pw)
            for r in recipients:
                try:
                    msg = EmailMessage()
                    msg["From"] = from_addr
                    msg["To"] = r
                    msg["Subject"] = subject
                    msg.set_content(text)
                    s.send_message(msg)
                    ok_any = True
                    log.info("email: sent to %s", r)
                except Exception as e:  # noqa: BLE001 — per-recipient soft-fail
                    log.warning("email: send to %s failed: %r", r, e)
    except Exception as e:  # noqa: BLE001 — soft-fail per sender contract
        log.warning("email: SMTP connect/login failed (%s:%s): %r", host, port, e)
    return ok_any
