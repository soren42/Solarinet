#!/usr/bin/env python3
"""SoR → isolated DNS generation → verified Xenon primary and DNS secondaries.

Every configured target is retried at startup, after events, and periodically.
Broker deliveries remain unacknowledged until all target helpers prove serving.
Requires the reviewed root-owned DNS release helper and completed bootstrap.
"""
import configparser
import datetime
import fcntl
import hashlib
import ipaddress
import json
import os
import pathlib
import re
import signal
import stat
import subprocess
import sys
import tempfile
import time

import pika
from delivery import consume_after_apply

CONF = os.environ.get("SORSYNC_CONF",
                      os.path.join(os.path.dirname(os.path.abspath(__file__)), "sorsync.conf"))
DNS_KEYS = ["sor.entities.*", "sor.ip_addresses.*", "sor.interfaces.*",
            "sor.dns_records.*", "sor.dns_zones.*", "sor.networks.*", "sor.subnets.*"]
_run = True


def log(msg):
    print(f"{time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime())} sor_apply_dns: {msg}", flush=True)


def secondary_targets(c):
    """Return validated, configured DNS secondaries in stable name order."""
    prefix = "dns.secondary."
    targets = []
    addresses = set()
    for section in sorted(name for name in c.sections() if name.startswith(prefix)):
        name = section[len(prefix):]
        if not re.fullmatch(r"[a-z][a-z0-9-]{0,31}", name):
            raise ValueError("invalid DNS secondary target name")
        address = str(ipaddress.IPv4Address(c.get(section, "address")))
        ssh_host = c.get(section, "ssh_host")
        helper = c.get(section, "update_helper",
                       fallback="/usr/local/sbin/solari-bind9-update-zones")
        if not re.fullmatch(r"[A-Za-z0-9_.@:\[\]-]+", ssh_host) or ssh_host.startswith("-"):
            raise ValueError(f"invalid SSH destination for DNS secondary {name}")
        if not re.fullmatch(r"/[A-Za-z0-9_./-]+", helper) or ".." in pathlib.PurePosixPath(helper).parts:
            raise ValueError(f"invalid update helper path for DNS secondary {name}")
        if address in addresses:
            raise ValueError("DNS secondary addresses must be unique")
        addresses.add(address)
        targets.append({"name": name, "address": address,
                        "ssh_host": ssh_host, "update_helper": helper})
    if not targets:
        raise ValueError("at least one DNS secondary target must be configured")
    return targets


def load_cfg():
    c = configparser.ConfigParser()
    if not c.read(CONF):
        log(f"FATAL: cannot read {CONF}")
        sys.exit(1)
    return c


def render(c, output):
    """Render from SoR into a private draft, reading serials from applied zones."""
    previous = c.get("dns", "applied_zone_dir")
    if not os.path.isdir(previous):
        raise ValueError("applied DNS generation is missing; complete release bootstrap")
    env = dict(os.environ)
    for option, variable in (("host", "SOR_DB_HOST"), ("port", "SOR_DB_PORT"),
                             ("user", "SOR_DB_USER"), ("name", "SOR_DB_NAME"),
                             ("password", "SOR_DB_PASSWORD")):
        env[variable] = c.get("sor", option)
    env["NETDB_SOURCE"] = "sor"
    py = c.get("dns", "render_python")
    zone_dir = c.get("dns", "runtime_zone_dir", fallback="/etc/bind/solarinet-current/zones")
    result = subprocess.run([py, c.get("dns", "gen_zones"), "--source", "sor",
                             "--output", str(output), "--previous", previous,
                             "--zone-dir", zone_dir], env=env, capture_output=True,
                            text=True, timeout=120)
    if result.returncode:
        diagnostic = (result.stderr or result.stdout or "").strip()
        password = env.get("SOR_DB_PASSWORD", "")
        if password:
            diagnostic = diagnostic.replace(password, "[redacted]")
        log(f"SoR DNS render failed with exit {result.returncode}: {diagnostic[-1000:]}")
        return False
    gen_bare = c.get("dns", "gen_bare", fallback="")
    if gen_bare:
        secondaries = secondary_targets(c)
        domain = c.get("dns", "domain", fallback="akoria.net")
        if not re.fullmatch(r"[A-Za-z0-9.-]+", domain):
            raise ValueError("invalid configured forward domain")
        command = [py, gen_bare, str(output / ("db." + domain)), str(output),
                   "--previous", previous, "--zone-dir", zone_dir]
        for target in secondaries:
            command.extend(("--transfer", target["address"]))
        result = subprocess.run(command,
                                capture_output=True, text=True, timeout=60)
        if result.returncode:
            diagnostic = (result.stderr or result.stdout or "").strip()
            log(f"bare-zone render failed with exit {result.returncode}: {diagnostic[-1000:]}")
            return False
    return True


def source_hash(paths):
    """Identify the desired input snapshot independently of target serial rebasing."""
    if not paths:
        raise ValueError("rendered target contains no zone files")
    digest = hashlib.sha256()
    for path in sorted(paths):
        if path.is_symlink() or not path.is_file():
            raise ValueError("rendered zone is not a regular file")
        digest.update(path.name.encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def target_status(c, name, values):
    """Durably merge one target's pending/applied state without logging secrets."""
    path = pathlib.Path(c.get("dns", "target_state_file"))
    if path.is_symlink():
        raise ValueError("DNS target state file must not be a symlink")
    if path.parent.is_symlink():
        raise ValueError("DNS target state directory must not be a symlink")
    parent = path.parent.stat()
    if parent.st_uid != os.geteuid() or parent.st_mode & 0o077:
        raise ValueError("DNS target state directory must be private and owned by the applier")
    lock_path = path.with_suffix(path.suffix + ".lock")
    lock_fd = os.open(lock_path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    lock_info = os.fstat(lock_fd)
    if not stat.S_ISREG(lock_info.st_mode) or lock_info.st_uid != os.geteuid() or lock_info.st_mode & 0o077:
        os.close(lock_fd)
        raise ValueError("DNS target state lock must be private and owned by the applier")
    with os.fdopen(lock_fd, "r+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if path.exists():
            state_fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            state_info = os.fstat(state_fd)
            if not stat.S_ISREG(state_info.st_mode) or state_info.st_uid != os.geteuid() or state_info.st_mode & 0o077:
                os.close(state_fd)
                raise ValueError("DNS target state file must be private and owned by the applier")
            with os.fdopen(state_fd, "r", encoding="utf-8") as source:
                state = json.load(source)
        else:
            state = {"version": 1, "targets": {}}
        if not isinstance(state, dict) or state.get("version") != 1 or not isinstance(state.get("targets"), dict):
            raise ValueError("invalid DNS target state")
        entry = state["targets"].setdefault(name, {})
        if not isinstance(entry, dict):
            raise ValueError("invalid DNS target entry")
        entry.update(values)
        fd, temporary = tempfile.mkstemp(prefix=".targets-", dir=path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as output:
                json.dump(state, output, sort_keys=True)
                output.flush()
                os.fsync(output.fileno())
            os.replace(temporary, path)
            directory = os.open(path.parent, os.O_RDONLY)
            try:
                os.fsync(directory)
            finally:
                os.close(directory)
        finally:
            if os.path.exists(temporary):
                os.unlink(temporary)


def proof(result, target):
    """Accept only an explicit verified applied generation from the root helper."""
    if result.returncode:
        raise RuntimeError(f"{target} release failed with exit {result.returncode}")
    state = json.loads(result.stdout)
    if not isinstance(state, dict):
        raise RuntimeError(f"{target} helper returned an invalid proof")
    applied = state.get("applied")
    if state.get("phase") != "applied" or state.get("desired") != applied or not isinstance(applied, str) or not re.fullmatch(r"[0-9a-f]{64}", applied):
        raise RuntimeError(f"{target} helper did not prove a verified applied generation")
    return applied


def local_release(c, output):
    result = subprocess.run(["sudo", "-n", "--", c.get("dns", "release_helper"),
                             "--config", c.get("dns", "release_config"), str(output)],
                            capture_output=True, text=True, timeout=300)
    return proof(result, "primary")


def refresh_secondary(c, target, desired):
    """Update one secondary's managed zone list and verify its served SOAs."""
    helper = c.get("dns", "secondary_refresh_helper",
                   fallback="/usr/local/libexec/solari-dns-secondary-refresh")
    primary = c.get("dns", "primary_address", fallback="10.0.0.20")
    primary_config = c.get("dns", "primary_config", fallback="/etc/bind/named.conf.akoria")
    additional = [value.strip() for value in c.get(
        "dns", "primary_additional_configs",
        fallback="/etc/bind/named.conf.bare,/etc/bind/named.conf.local,/etc/bind/named.conf.rpz"
    ).split(",") if value.strip()]
    timeout = c.getint("dns", "secondary_refresh_timeout_sec", fallback=150)
    if not 30 <= timeout <= 600:
        raise ValueError("secondary refresh timeout must be between 30 and 600 seconds")
    command = [helper, "--primary-config", primary_config, "--primary", primary,
               "--secondary", target["address"], "--ssh-host", target["ssh_host"],
               "--helper", target["update_helper"]]
    for path in additional:
        command.extend(("--additional-config", path))
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(f"secondary refresh failed for {target['name']} with exit {result.returncode}")
    # The refresh helper only exits successfully after every generated zone
    # serves the primary SOA; desired identifies the input snapshot verified.
    return desired


def apply_target(c, name, desired, operation):
    """Keep an event pending until both target proof and its local record persist."""
    now = datetime.datetime.now(datetime.timezone.utc).isoformat()
    target_status(c, name, {"phase": "pending", "desiredSource": desired, "lastAttempt": now})
    try:
        applied = operation()
        target_status(c, name, {"phase": "applied", "appliedSource": desired,
                               "appliedGeneration": applied, "lastSuccess": datetime.datetime.now(datetime.timezone.utc).isoformat(),
                               "error": None})
        return True
    except Exception as error:
        target_status(c, name, {"phase": "failed", "error": str(error)})
        log(f"{name} apply failed; broker delivery remains unacknowledged")
        return False


def apply(c):
    incoming = pathlib.Path(c.get("dns", "incoming_root"))
    info = incoming.stat()
    if incoming.is_symlink() or info.st_uid != os.geteuid() or info.st_mode & 0o077:
        raise ValueError("DNS incoming root must be private and owned by the applier")
    with tempfile.TemporaryDirectory(prefix="generation-", dir=incoming) as folder:
        output = pathlib.Path(folder)
        if not render(c, output):
            return False
        zones = sorted(output.glob("db.*"))
        secondaries = secondary_targets(c)
        primary_hash = source_hash(zones)
        # Retire the former mandatory direct-copy target so its last-applied
        # row cannot be mistaken for current desired state after cutover.
        target_status(c, "steel", {"phase": "retired", "desiredSource": None,
                                   "appliedSource": None, "appliedGeneration": None,
                                   "error": "direct Steel target retired; use configured secondaries"})
        primary_ok = apply_target(c, "primary", primary_hash, lambda: local_release(c, output))
        # Secondaries depend on the primary release. Each configured target is
        # refreshed and SOA-verified; the event stays pending until all succeed.
        secondaries_ok = True
        for target in secondaries:
            name = "secondary:" + target["name"]
            if not primary_ok:
                target_status(c, name, {"phase": "failed", "desiredSource": primary_hash,
                                        "error": "primary release has not been verified"})
                secondaries_ok = False
                continue
            ok = apply_target(c, name, primary_hash,
                              lambda target=target: refresh_secondary(c, target, primary_hash))
            secondaries_ok = ok and secondaries_ok
        return primary_ok and secondaries_ok


def run():
    c = load_cfg()
    exchange = c.get("amqp", "exchange", fallback="sor.events")
    debounce = c.getfloat("dns", "debounce_sec", fallback=4.0)
    reconcile = c.getfloat("dns", "reconcile_interval_sec", fallback=3600.0)
    reconnect = c.getfloat("amqp", "reconnect_delay_sec", fallback=5.0)
    queue = c.get("amqp", "dns_queue", fallback="sor.apply.dns")
    if not re.fullmatch(r"[A-Za-z0-9_.-]{1,200}", queue):
        raise ValueError("invalid DNS consumer queue name")
    while _run:
        conn = None
        try:
            conn = pika.BlockingConnection(pika.URLParameters(c.get("amqp", "url")))
            ch = conn.channel()
            ch.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
            ch.queue_declare(queue=queue, durable=True)
            for key in DNS_KEYS:
                ch.queue_bind(exchange=exchange, queue=queue, routing_key=key)
            consume_after_apply(ch, queue, debounce, reconcile, lambda: apply(c), lambda: _run,
                                maintain_connection=lambda: conn.process_data_events(time_limit=0.25))
        except Exception as error:
            log(f"error: {error!r}; reconnecting in {reconnect}s")
            if _run:
                time.sleep(reconnect)
        finally:
            if conn is not None:
                try:
                    conn.close()               # requeues every unacknowledged event
                except Exception:
                    pass


def _stop(*_):
    global _run
    _run = False
    log("shutting down")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    run()
