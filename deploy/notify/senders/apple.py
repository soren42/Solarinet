"""apple sender — native cross-device notifications via iMessage.

Relays each notify.events message to a Mac over SSH and runs osascript to send
an iMessage from that Mac's signed-in Apple ID to the configured recipient(s).
iCloud/Messages then fans it out to every Apple device on the account
(iPhone / iPad / Watch / Mac) as a notification — no Apple Developer account,
push certificate, or third-party service required.

[sender.apple] config (notify.conf):
    ssh          = user@mac-host        # a Mac, always-on + logged into a GUI
                                         # session, Messages signed into the
                                         # account that should relay the alerts
    ssh_opts     = -o ConnectTimeout=8 -o BatchMode=yes   # optional
Recipients come from message["_to"] (message["to"], else [defaults]
default_apple_to) — Apple IDs (you@icloud.com) and/or phone numbers (+1555...).

Mac-side setup (once):
  * Messages.app signed in; send yourself a test iMessage so the buddy resolves.
  * Grant the SSH login Automation control of Messages
    (System Settings > Privacy & Security > Automation), or osascript is blocked.
  * Keep the user logged into a GUI session — osascript+Messages need one.

Never raises for expected failures (Mac unreachable, not configured) — logs and
returns False so the log/mqtt senders still record the event.
"""
import logging
import subprocess

log = logging.getLogger("notify.sender.apple")


def _esc(s):
    """Escape a Python string for an AppleScript double-quoted literal."""
    return s.replace("\\", "\\\\").replace('"', '\\"')


def _format(message):
    """One-line iMessage body: '[SEV] title: body' (newlines flattened)."""
    sev = (message.get("severity") or "info").strip().upper()
    title = (message.get("title") or "SolariNet").strip()
    body = (message.get("body") or "").strip()
    text = f"[{sev}] {title}"
    if body:
        text += f": {body}"
    return " ".join(text.split())  # collapse any newlines/runs of whitespace


def _applescript(buddy, text):
    return (
        'tell application "Messages"\n'
        '  set svc to 1st account whose service type = iMessage\n'
        f'  send "{_esc(text)}" to participant "{_esc(buddy)}" of svc\n'
        'end tell\n'
    )


def send(message, cfg):
    if cfg is None:
        log.error("apple sender has no [sender.apple] config section; skipping")
        return False
    ssh_host = cfg.get("ssh", "") or cfg.get("ssh_host", "")
    if not ssh_host:
        log.error("apple sender: [sender.apple] ssh= (the relay Mac) not set; skipping")
        return False
    recipients = message.get("_to") or []
    if not recipients:
        log.warning("apple sender: no recipients for %r; skipping", message.get("title"))
        return False

    ssh_opts = (cfg.get("ssh_opts", "") or
                "-o ConnectTimeout=8 -o BatchMode=yes -o StrictHostKeyChecking=accept-new").split()
    text = _format(message)
    ok_any = False
    for r in recipients:
        try:
            # osascript reads the script from stdin ('-'); ssh forwards our stdin
            # to it, so the recipient/text are embedded in the script — no remote
            # shell arg-quoting to get wrong.
            proc = subprocess.run(
                ["ssh", *ssh_opts, ssh_host, "osascript", "-"],
                input=_applescript(r, text),
                capture_output=True, text=True, timeout=30)
            if proc.returncode == 0:
                ok_any = True
                log.info("apple: iMessage sent to %s", r)
            else:
                log.warning("apple: osascript failed for %s (rc=%d): %s",
                            r, proc.returncode, (proc.stderr or "").strip()[:200])
        except Exception as e:  # noqa: BLE001 — soft-fail per sender contract
            log.warning("apple: send to %s failed: %r", r, e)
    return ok_any
