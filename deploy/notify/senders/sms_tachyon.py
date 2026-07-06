"""sms sender — dispatches via a Tachyon SBC (tachyon.akoria.net / 10.6.6.10),
a Particle Tachyon board with an onboard cellular modem.

OPEN QUESTION (see README.md "Open question: Tachyon SMS"): which mechanism
actually sends the text is not yet decided. Rather than guess, this module
exposes one small, well-documented seam —

    send_sms(number: str, text: str, cfg) -> bool

— and two candidate backends behind it, selected by notify.conf's
[sender.sms] method=. Both are stubbed (they log what they *would* run and
return False) until someone has shell access on Tachyon to confirm which
modem interface is actually present and pick one:

  * mmcli_ssh — assumes ModemManager is running on Tachyon and SMS goes via
    `mmcli --messaging --create-sms ... && mmcli --sms=N --send`, run over
    SSH. This is the more "batteries included" path if ModemManager already
    manages the modem (common on Linux boards with a cellular HAT/modem).

  * at_ssh — assumes no ModemManager, and talks to the modem's AT-command
    serial port (e.g. /dev/ttyUSB2) directly: AT+CMGF=1 (text mode),
    AT+CMGS="<number>", then the body + Ctrl-Z. Lower-level, works with
    basically any modem, more fragile (need the right port + timing).

  * A third option not stubbed here: Particle's own cloud API (if Tachyon is
    claimed to a Particle account and has a device-side function that sends
    SMS via Particle's cellular stack). Worth checking before building out
    the raw-serial path — Particle publishes docs for `Particle.publish`
    triggering a hardware SMS helper on some SoMs. Left out because it needs
    Particle account/device-ID details this codebase doesn't have yet.

Whichever backend is chosen, finishing this sender is: SSH to Tachyon,
confirm the mechanism, replace the relevant _send_via_* body below (currently
`log.warning(...); return False`) with the real subprocess call, and fill in
[sender.sms] in notify.conf.

This sender never raises for connectivity/config problems: a message is
still recorded by the log sender, so a broken/unreachable Tachyon degrades
notification delivery rather than crashing notifyd.
"""

import logging
import subprocess

log = logging.getLogger("notify.sender.sms")

SSH_TIMEOUT_SEC = 10


def send(message, cfg):
    if not cfg:
        log.error("sms sender has no [sender.sms] config section; skipping")
        return False

    numbers = message.get("_to") or []
    if not numbers:
        log.warning("sms sender: no recipients for %r; skipping", message.get("title"))
        return False

    text = _format_text(message)
    ok = True
    for number in numbers:
        if not send_sms(number, text, cfg):
            ok = False
    return ok


def send_sms(number, text, cfg):
    """The seam: send one SMS. Returns True on confirmed send.

    `cfg` is the [sender.sms] section (configparser.SectionProxy or dict-like
    with .get). Dispatches to the configured method= backend.
    """
    method = (cfg.get("method") or "mmcli_ssh").strip()
    host = cfg.get("host") or "tachyon.akoria.net"

    if method == "mmcli_ssh":
        return _send_via_mmcli_ssh(host, number, text, cfg)
    if method == "at_ssh":
        return _send_via_at_ssh(host, number, text, cfg)

    log.error("sms sender: unknown method %r (expected mmcli_ssh or at_ssh)", method)
    return False


def _ssh_base(host, cfg):
    user = cfg.get("ssh_user") or "solari"
    key = cfg.get("ssh_key") or ""
    cmd = ["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8"]
    if key:
        cmd += ["-i", key]
    cmd.append(f"{user}@{host}")
    return cmd


def _send_via_mmcli_ssh(host, number, text, cfg):
    """Candidate 1: ModemManager over SSH.

    Real sequence would be roughly:
      modem = cfg.get("modem_index") or `mmcli -L` parsed for the first modem
      sms_path = mmcli -m <modem> --messaging --create-sms="number=<number>,text=<text>"
      mmcli -m <modem> --sms=<sms_path> --send

    TODO(tachyon-sms): confirm ModemManager is actually running on Tachyon
    (`systemctl status ModemManager` over SSH) and that the modem shows up in
    `mmcli -L` before wiring this up for real.
    """
    remote_cmd = (
        "echo 'mmcli_ssh backend not implemented yet - "
        "see deploy/notify/senders/sms_tachyon.py'"
    )
    log.warning(
        "sms sender (mmcli_ssh): NOT IMPLEMENTED — would ssh to %s and run "
        "mmcli --messaging --create-sms/--send for %s (%d chars). "
        "Dry-run command: %s",
        host, number, len(text), " ".join(_ssh_base(host, cfg) + [remote_cmd]),
    )
    return False


def _send_via_at_ssh(host, number, text, cfg):
    """Candidate 2: raw AT commands over SSH to a serial port.

    Real sequence would be roughly (via e.g. `socat` or a small python/expect
    script run remotely, since AT+CMGS needs a Ctrl-Z terminator mid-stream):
      AT+CMGF=1
      AT+CMGS="<number>"
      <text><Ctrl-Z>

    TODO(tachyon-sms): confirm the modem's AT port (at_device=, likely one of
    /dev/ttyUSB0-3 — modems typically expose several ports, only one of which
    accepts AT+CMGS) and baud (at_baud=) before wiring this up for real.
    """
    device = cfg.get("at_device") or "/dev/ttyUSB2"
    baud = cfg.get("at_baud") or "115200"
    log.warning(
        "sms sender (at_ssh): NOT IMPLEMENTED — would ssh to %s and send AT+CMGF=1 / "
        "AT+CMGS to %s @ %s baud for %s (%d chars)",
        host, device, baud, number, len(text),
    )
    return False


def _format_text(message):
    title = message.get("title", "")
    body = message.get("body", "")
    sev = message.get("severity", "info").upper()
    text = f"[{sev}] {title}: {body}" if title else f"[{sev}] {body}"
    # Keep it single-segment-ish; Tachyon backend can still split if needed.
    return text[:480]


def _run_ssh(host, cfg, remote_cmd):
    """Shared helper the real backends will use once implemented."""
    cmd = _ssh_base(host, cfg) + [remote_cmd]
    try:
        proc = subprocess.run(
            cmd, capture_output=True, text=True, timeout=SSH_TIMEOUT_SEC, check=False,
        )
    except (OSError, subprocess.SubprocessError) as exc:
        log.warning("sms sender: ssh to %s failed: %s", host, exc)
        return False, ""
    if proc.returncode != 0:
        log.warning(
            "sms sender: ssh to %s exited %d: %s",
            host, proc.returncode, proc.stderr.strip(),
        )
        return False, proc.stdout
    return True, proc.stdout
