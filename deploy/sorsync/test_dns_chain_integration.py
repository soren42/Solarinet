"""Exercise a real SoR CDC event through RabbitMQ and the DNS applier.

Run only against an explicitly selected loopback ``solarinet_*`` scratch
database and an isolated RabbitMQ URL. The renderer, release CLI, root-owned
release tree, and private loopback BIND service account are exercised directly.
"""
import configparser
import importlib.util
import json
import os
import pathlib
import pwd
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unittest
import uuid

import pika
import pymysql

import sor_apply_dnsd
import sor_emitd
from delivery import consume_after_apply


ROOT = pathlib.Path(__file__).resolve().parents[2]
DNS_KEYS = sor_apply_dnsd.DNS_KEYS
RELEASE_SPEC = importlib.util.spec_from_file_location(
    "dns_release", ROOT / "deploy/dns-release/release.py")
dns_release = importlib.util.module_from_spec(RELEASE_SPEC)
RELEASE_SPEC.loader.exec_module(dns_release)


def zone_text(origin, serial, address):
    return (f"$ORIGIN {origin}.\n$TTL 60\n"
            f"@ IN SOA ns.{origin}. hostmaster.{origin}. ( {serial} 60 60 300 60 )\n"
            f"@ IN NS ns.{origin}.\n@ IN A {address}\nns IN A 127.0.0.1\n")


class LoopbackBind:
    """Private named process and release tree inside a temporary directory."""
    def __init__(self, root):
        self.root = root / "bind"
        self.root.mkdir(mode=0o700)
        self.log = None
        self.process = None
        self.releases = None
        self.managed = None
        self.config_path = None
        self.gate_enabled = None
        self.gate_ready = None
        self.gate_release = None

    def start(self, incoming):
        for command in ("named", "named-checkconf", "named-checkzone", "dig"):
            if shutil.which(command) is None:
                raise RuntimeError(f"{command} is required for the DNS chain integration")
        self.releases = self.root / "releases"
        self.root.parent.chmod(0o755)
        self.root.chmod(0o755)
        self.managed = self.root / "managed"
        self.managed.mkdir(mode=0o755)
        self.current = self.managed / "current"
        self.main = self.root / "named.conf"
        self.config_path = self.root / "release-config.json"
        self.reload_script = self.root / "reload-gate.py"
        self.gate_enabled = self.root / "reload-gate-enabled"
        self.gate_ready = self.root / "reload-gate-ready"
        self.gate_release = self.root / "reload-gate-release"
        with socket.socket() as reservation:
            reservation.bind(("127.0.0.1", 0))
            self.port = reservation.getsockname()[1]
        self.config = {
            "releaseRoot": str(self.releases), "runtimeRoot": str(self.releases),
            "currentLink": str(self.current), "checkZoneCommand": ["named-checkzone"],
            "expandCommand": ["named-checkconf", "-p", str(self.main)],
            "checkConfigCommand": ["named-checkconf", "-z"],
            "queryAddress": "127.0.0.1", "queryPort": self.port, "healthTimeout": 8,
            "incomingRoot": str(incoming),
            "candidateUser": pwd.getpwuid(os.getuid()).pw_name,
            "digCommand": ["dig"],
        }
        seed = self.root / "seed"
        seed.mkdir()
        (seed / "db.seed.invalid").write_text(
            zone_text("seed.invalid", 2026102101, "192.0.2.1"), encoding="utf-8")
        generation, manifest = dns_release.stageGeneration(self.config, seed)
        self.current.symlink_to(generation)
        runtime = self.root / "runtime"
        runtime.mkdir(mode=0o700)
        self.main.write_text(
            f'options {{ directory "{runtime}"; '
            f'listen-on port {self.port} {{ 127.0.0.1; }}; '
            'listen-on-v6 { none; }; recursion no; dnssec-validation no; '
            f'pid-file "{runtime}/named.pid"; '
            f'session-keyfile "{runtime}/session.key"; }};\n'
            'controls {};\n'
            f'include "{self.current}/named.conf.managed";\n', encoding="utf-8")
        self.main.chmod(0o644)
        binary = self.root / "named-test"
        shutil.copy2(shutil.which("named"), binary)
        self.config["checkZoneCommand"] = [shutil.which("named-checkzone")]
        self.config["expandCommand"] = [shutil.which("named-checkconf"), "-p", str(self.main)]
        self.config["checkConfigCommand"] = [shutil.which("named-checkconf"), "-z"]
        self.config["digCommand"] = [shutil.which("dig")]
        self.config_path.write_text(json.dumps(self.config), encoding="utf-8")
        self.root_command(["chown", "-R", "root:root", str(self.releases)])
        self.root_command(["chown", "root:root", str(self.managed)])
        self.root_command(["chmod", "0755", str(self.releases), str(self.managed)])
        self.root_command(["chown", "root:root", str(self.config_path)])
        self.root_command(["chmod", "0600", str(self.config_path)])
        self.log = (self.root / "named.log").open("w")
        self.root_command(["chown", "bind:bind", str(runtime)])
        self.root_command(["chown", "bind:bind", str(self.root / "named.log")])
        self.root_command(["chmod", "0644", str(self.root / "named.log")])
        self.process = subprocess.Popen(
            ["sudo", "-n", "--", str(binary), "-g", "-c", str(self.main),
             "-u", "bind", "-n", "1", "-U", "1"],
            stdout=self.log, stderr=subprocess.STDOUT)
        self.reload_script.write_text(
            "import os, pathlib, signal, sys, time\n"
            "enabled, ready, release = map(pathlib.Path, sys.argv[1:4])\n"
            "if enabled.exists():\n"
            "    ready.touch()\n"
            "    deadline = time.monotonic() + 15\n"
            "    while not release.exists() and time.monotonic() < deadline:\n"
            "        time.sleep(0.02)\n"
            "    if not release.exists():\n"
            "        raise SystemExit(2)\n"
            "os.kill(int(sys.argv[4]), signal.SIGHUP)\n", encoding="utf-8")
        self.reload_script.chmod(0o644)
        self.config["reloadCommand"] = [sys.executable, str(self.reload_script),
                                         str(self.gate_enabled), str(self.gate_ready),
                                         str(self.gate_release), str(self.process.pid)]
        # Reload command is part of the trusted root config too.
        self.config_path.unlink()
        self.config_path.write_text(json.dumps(self.config), encoding="utf-8")
        self.root_command(["chown", "root:root", str(self.config_path)])
        self.root_command(["chmod", "0600", str(self.config_path)])
        try:
            dns_release.verifyServed(self.config, manifest)
        except Exception:
            self.log.flush()
            raise RuntimeError((self.root / "named.log").read_text()[-3000:])

    @staticmethod
    def root_command(arguments):
        result = subprocess.run(["sudo", "-n", "--", *arguments],
                                capture_output=True, text=True, timeout=10)
        if result.returncode:
            raise RuntimeError(f"isolated root setup failed: {arguments[0]}")

    def stop(self):
        if self.process is not None and self.process.poll() is None:
            try:
                self.process.terminate()
            except PermissionError:
                self.root_command(["kill", "-TERM", str(self.process.pid)])
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
        if self.log is not None:
            self.log.close()
        if self.config_path is not None and self.config_path.exists():
            account = pwd.getpwuid(os.getuid())
            owner = f"{account.pw_uid}:{account.pw_gid}"
            for path in (self.releases, self.managed, self.root / "runtime"):
                if path is not None and path.exists():
                    self.root_command(["chown", "-R", owner, str(path)])
            self.root_command(["chown", owner, str(self.config_path)])


def test_settings():
    if os.environ.get("SOLARI_TEST_SOR_DB") != "1":
        raise unittest.SkipTest("set SOLARI_TEST_SOR_DB=1 for isolated SoR integration")
    host = os.environ.get("SOR_DB_HOST", "")
    port = int(os.environ.get("SOR_DB_PORT", "3306"))
    name = os.environ.get("SOR_DB_NAME", "")
    user = os.environ.get("SOR_DB_USER", "root")
    password = os.environ.get("SOR_DB_PASS", "")
    broker = os.environ.get("SOLARI_TEST_AMQP_URL", "")
    if host not in {"127.0.0.1", "localhost"} or not re.fullmatch(r"solarinet_[a-z0-9_]+", name):
        raise RuntimeError("integration requires a loopback solarinet_* scratch DB")
    if not broker.startswith("amqp://") or "127.0.0.1" not in broker:
        raise RuntimeError("integration requires a loopback SOLARI_TEST_AMQP_URL")
    return host, port, name, user, password, broker


def connect_db(settings):
    host, port, name, user, password, _broker = settings
    return pymysql.connect(host=host, port=port, user=user, password=password,
                           database=name, autocommit=True, charset="utf8mb4",
                           cursorclass=pymysql.cursors.DictCursor)


class AckTrackingChannel:
    """Observe each consumer ack and the persisted target proof at that point."""
    def __init__(self, channel, target_state, expected, acknowledged, proofs, dns_check):
        self._channel = channel
        self._target_state = target_state
        self._expected = expected
        self._acknowledged = acknowledged
        self._proofs = proofs
        self._dns_check = dns_check

    def __getattr__(self, name):
        return getattr(self._channel, name)

    def basic_ack(self, *args, **kwargs):
        state = json.loads(self._target_state.read_text(encoding="utf-8"))
        primary = state["targets"]["primary"]
        secondary = state["targets"]["secondary:chlorine"]
        retired_steel = state["targets"].get("steel", {})
        self._proofs.append(primary.get("phase") == "applied" and
                             primary.get("desiredSource") == primary.get("appliedSource") and
                             secondary.get("phase") == "applied" and
                             secondary.get("desiredSource") == secondary.get("appliedSource") and
                             retired_steel.get("phase") == "retired" and
                             self._dns_check())
        result = self._channel.basic_ack(*args, **kwargs)
        if len(self._proofs) >= self._expected["count"]:
            self._acknowledged.set()
        return result


class SoRDnsChainIntegrationTest(unittest.TestCase):
    def test_trigger_to_confirmed_dns_target_before_broker_ack(self):
        settings = test_settings()
        db = connect_db(settings)
        suffix = uuid.uuid4().hex[:10]
        source_slug = "dns-chain-" + suffix
        hostname = "chain-" + suffix
        zone = "chain-" + suffix + ".invalid"
        address = "192.0.2.77"
        exchange = "sor.events"
        queue = "solarinet.dns-chain." + suffix
        temp = pathlib.Path(tempfile.mkdtemp(prefix="solarinet-dns-chain-"))
        incoming = temp / "incoming"
        applied = temp / "applied-zones"
        state_dir = temp / "state"
        for folder in (incoming, applied, state_dir):
            folder.mkdir(mode=0o700)
        bind = LoopbackBind(temp)
        target_state = state_dir / "targets.json"
        secondary_refresh = temp / "secondary-refresh"
        secondary_refresh.write_text(
            "#!/usr/bin/env python3\n"
            "import sys\n"
            "args = sys.argv[1:]\n"
            "required = ('--secondary', '192.0.2.12', '--ssh-host', 'jason@chlorine')\n"
            "raise SystemExit(0 if all(item in args for item in required) else 2)\n",
            encoding="utf-8")
        secondary_refresh.chmod(0o755)
        consumer_thread = None
        crash_worker_process = None
        retry_worker_process = None
        stop = threading.Event()
        initial_apply = threading.Event()
        target_applied = threading.Event()
        acknowledged = threading.Event()
        disconnect_consumer = threading.Event()
        disconnect_observed = threading.Event()
        expected_acks = {"count": 0}
        proof_before_ack = []
        consumer_errors = []
        apply_count = {"value": 0}
        entity_id = None
        address_id = None
        nameserver_ids = []
        nameserver_address_ids = []
        zone_id = None
        source_id = None
        checkpoint = 0

        cfg = configparser.ConfigParser(interpolation=None)
        cfg.read_dict({
            "sor": {"host": settings[0], "port": str(settings[1]), "user": settings[3],
                    "password": settings[4], "name": settings[2]},
            "amqp": {"url": settings[5], "exchange": exchange},
            "dns": {"incoming_root": str(incoming), "applied_zone_dir": str(applied),
                    "runtime_zone_dir": str(temp / "runtime-zones"),
                    "gen_zones": str(ROOT / "netdb/gen-zones.py"),
                    "render_python": sys.executable,
                    "target_state_file": str(target_state),
                    "release_helper": str(temp / "release-helper"),
                    "release_config": str(temp / "release-config.json"),
                    "secondary_refresh_helper": str(secondary_refresh),
                    "debounce_sec": "0.1",
                    "reconcile_interval_sec": "0"},
            "dns.secondary.chlorine": {"address": "192.0.2.12",
                                        "ssh_host": "jason@chlorine"},
        })

        def run_consumer(errors, ack_event, interrupt_connection):
            connection = None
            try:
                connection = pika.BlockingConnection(pika.URLParameters(settings[5]))
                channel = connection.channel()
                channel.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
                channel.queue_declare(queue=queue, durable=True)
                for key in DNS_KEYS:
                    channel.queue_bind(exchange=exchange, queue=queue, routing_key=key)
                def apply_current_state():
                    result = sor_apply_dnsd.apply(cfg)
                    apply_count["value"] += 1
                    if apply_count["value"] == 1:
                        initial_apply.set()
                    if apply_count["value"] >= 2:
                        target_applied.set()
                    return result

                def maintain_connection():
                    if interrupt_connection and disconnect_consumer.is_set():
                        # Let the owner thread close in its finally block after
                        # the in-flight apply finishes; close() here would wait
                        # for Pika's handshake while that callback is unwinding.
                        disconnect_observed.set()
                        raise RuntimeError("injected RabbitMQ connection loss during DNS apply")
                    connection.process_data_events(time_limit=0.1)

                consume_after_apply(
                    AckTrackingChannel(channel, target_state, expected_acks,
                                       ack_event, proof_before_ack,
                                       lambda: subprocess.run(
                                           ["dig", "@127.0.0.1", "-p", str(bind.port),
                                            hostname + "." + zone, "A", "+short"],
                                           capture_output=True, text=True, timeout=3
                                       ).stdout.strip() == address),
                    queue, 0.1, 0, apply_current_state,
                    lambda: not stop.is_set(), max_pending=10,
                    maintain_connection=maintain_connection,
                )
            except BaseException as error:  # propagate thread failures to the test
                errors.append(error)
            finally:
                if connection is not None and connection.is_open:
                    connection.close()

        def consumer():
            run_consumer(consumer_errors, acknowledged, True)

        def launch_process_worker(hold_before_ack, worker_name):
            worker_config = {
                "applier_config": {section: dict(cfg[section]) for section in cfg.sections()},
                "amqp_url": settings[5], "exchange": exchange, "queue": queue,
                "state_path": str(target_state), "proof_path": str(temp / (worker_name + ".proof")),
                "gate_path": str(temp / (worker_name + ".gate")),
                "done_path": str(temp / (worker_name + ".done")),
                "hold_before_ack": hold_before_ack, "expected_acks": expected_acks["count"],
                "dig": shutil.which("dig"), "bind_port": bind.port,
                "hostname": hostname, "zone": zone, "address": address,
            }
            config_path = temp / (worker_name + ".json")
            config_path.write_text(json.dumps(worker_config), encoding="utf-8")
            config_path.chmod(0o600)
            env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"),
                   "HOME": os.environ.get("HOME", "/tmp"), "LANG": "C.UTF-8"}
            return subprocess.Popen(
                [sys.executable, str(ROOT / "deploy/sorsync/dns_chain_crash_worker.py"),
                 str(config_path)], cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

        try:
            bind.start(incoming)
            cfg.set("dns", "release_helper", str(ROOT / "deploy/dns-release/release.py"))
            cfg.set("dns", "release_config", str(bind.config_path))
            with db.cursor() as cur:
                cur.execute("INSERT INTO sources (slug,display_name,kind) VALUES (%s,%s,'human')",
                            (source_slug, "DNS chain fixture"))
                source_id = cur.lastrowid
                cur.execute("INSERT INTO dns_zones (name,kind,authority,source_id) VALUES (%s,'forward','sor_rendered',%s)",
                            (zone, source_id))
                zone_id = cur.lastrowid
                # netdb/gen-zones.py publishes these three nameservers relative
                # to the rendered forward zone, so the scratch SoR needs their
                # address records for BIND's in-bailiwick NS checks.
                for index, (name, ip) in enumerate((
                        ("xenon", "192.0.2.10"), ("radium", "192.0.2.11"),
                        ("chlorine", "192.0.2.12"))):
                    cur.execute("INSERT INTO entities (name,entity_type,role,lifecycle,source_id) "
                                "VALUES (%s,'server','DNS chain nameserver','active',%s)",
                                (name, source_id))
                    ns_id = cur.lastrowid
                    nameserver_ids.append(ns_id)
                    cur.execute("INSERT INTO ip_addresses (address,entity_id,is_primary,source_id) "
                                "VALUES (%s,%s,1,%s)", (ip, ns_id, source_id))
                    nameserver_address_ids.append(cur.lastrowid)
                # Skip the setup zone event. The later entity and address writes
                # are the event under test and are emitted from CDC triggers.
                cur.execute("SELECT COALESCE(MAX(id),0) AS high FROM sor_outbox")
                checkpoint = int(cur.fetchone()["high"])
                sor_emitd.set_checkpoint(db, checkpoint)

            consumer_thread = threading.Thread(target=consumer, daemon=True)
            consumer_thread.start()
            self.assertTrue(initial_apply.wait(20), "consumer did not complete startup reconcile")
            if consumer_errors:
                raise consumer_errors[0]

            with db.cursor() as cur:
                cur.execute("INSERT INTO entities (name,entity_type,role,lifecycle,source_id) "
                            "VALUES (%s,'server','DNS chain test','active',%s)", (hostname, source_id))
                entity_id = cur.lastrowid
                cur.execute("INSERT INTO ip_addresses (address,entity_id,is_primary,source_id) "
                            "VALUES (%s,%s,1,%s)", (address, entity_id, source_id))
                address_id = cur.lastrowid
                cur.execute("SELECT id,table_name,row_id,action,UNIX_TIMESTAMP(changed_at) AS ts "
                            "FROM sor_outbox WHERE id>%s ORDER BY id", (checkpoint,))
                rows = cur.fetchall()
            self.assertEqual({"entities", "ip_addresses"}, {row["table_name"] for row in rows})
            expected_acks["count"] = len(rows)

            bind.gate_enabled.touch()
            broker = pika.BlockingConnection(pika.URLParameters(settings[5]))
            try:
                publisher = broker.channel()
                publisher.exchange_declare(exchange=exchange, exchange_type="topic", durable=True)
                publisher.confirm_delivery()
                published_through = sor_emitd.publish_batch(publisher, db, rows, exchange, checkpoint)
            finally:
                broker.close()

            self.assertEqual(int(rows[-1]["id"]), published_through)
            self.assertEqual(published_through, sor_emitd.get_checkpoint(db),
                             "publisher checkpoint must follow broker confirms")
            gate_deadline = time.monotonic() + 20
            while not bind.gate_ready.exists() and time.monotonic() < gate_deadline:
                time.sleep(0.02)
            self.assertTrue(bind.gate_ready.exists(),
                            "release helper did not reach the gated BIND reload")
            disconnect_consumer.set()
            self.assertTrue(disconnect_observed.wait(5),
                            "consumer did not observe the injected broker loss during apply")
            bind.gate_release.touch()
            self.assertTrue(target_applied.wait(30), "DNS target apply did not complete after the event")
            consumer_thread.join(timeout=20)
            if consumer_thread.is_alive():
                frame = sys._current_frames().get(consumer_thread.ident)
                self.fail("interrupted consumer did not exit: " +
                          ("".join(traceback.format_stack(frame)) if frame else "no thread frame"))
            self.assertEqual(1, len(consumer_errors))
            self.assertIn("injected RabbitMQ connection loss", str(consumer_errors[0]))
            self.assertFalse(acknowledged.is_set(), "a lost connection cannot acknowledge the event")
            self.assertEqual([], proof_before_ack, "the disconnected delivery was acknowledged")

            redelivery = pika.BlockingConnection(pika.URLParameters(settings[5]))
            try:
                redelivery_ch = redelivery.channel()
                message = redelivery_ch.basic_get(queue=queue, auto_ack=False)
                self.assertIsNotNone(message[0], "the unacknowledged event was not requeued")
                self.assertTrue(message[0].redelivered, "RabbitMQ did not mark the delivery as redelivered")
                redelivery_ch.basic_nack(delivery_tag=message[0].delivery_tag, requeue=True)
            finally:
                redelivery.close()

            bind.gate_enabled.unlink(missing_ok=True)
            crash_worker_process = launch_process_worker(True, "crash-worker")
            crash_gate = temp / "crash-worker.gate"
            gate_deadline = time.monotonic() + 40
            while (not crash_gate.exists() and crash_worker_process.poll() is None and
                   time.monotonic() < gate_deadline):
                time.sleep(0.02)
            self.assertTrue(crash_gate.exists(), "consumer did not reach the pre-ack crash gate")
            crash_proof = json.loads((temp / "crash-worker.proof").read_text(encoding="utf-8").splitlines()[0])
            self.assertTrue(crash_proof["targetApplied"] and crash_proof["servedAnswer"],
                            "target state and served DNS must be proven before the crash")
            self.assertFalse((temp / "crash-worker.done").exists(),
                             "the crash fixture must stop before acknowledging")
            crash_worker_process.kill()
            self.assertEqual(-9, crash_worker_process.wait(timeout=10),
                             "consumer process must be killed while waiting before acknowledgement")

            after_crash = pika.BlockingConnection(pika.URLParameters(settings[5]))
            try:
                after_crash_ch = after_crash.channel()
                for _ in range(expected_acks["count"]):
                    message = after_crash_ch.basic_get(queue=queue, auto_ack=False)
                    self.assertIsNotNone(message[0], "process crash lost an unacknowledged event")
                    self.assertTrue(message[0].redelivered,
                                    "RabbitMQ did not mark the process-crashed delivery redelivered")
                    after_crash_ch.basic_nack(delivery_tag=message[0].delivery_tag, requeue=True)
            finally:
                after_crash.close()

            retry_worker_process = launch_process_worker(False, "retry-worker")
            retry_result = retry_worker_process.wait(timeout=40)
            self.assertEqual(0, retry_result, "restarted consumer failed to acknowledge verified retries")
            retry_proofs = [json.loads(line) for line in
                            (temp / "retry-worker.proof").read_text(encoding="utf-8").splitlines()]
            self.assertEqual(expected_acks["count"], len(retry_proofs))
            self.assertTrue(all(item["targetApplied"] and item["servedAnswer"] for item in retry_proofs),
                            "restarted consumer must verify the target before every ack")
            consumer_thread.join(timeout=20)
            answer = subprocess.run(
                ["dig", "@127.0.0.1", "-p", str(bind.port), hostname + "." + zone,
                 "A", "+short"], capture_output=True, text=True, timeout=3)
            self.assertEqual(address, answer.stdout.strip(),
                             "the real SoR renderer's host record must be served")
            state = json.loads(target_state.read_text(encoding="utf-8"))
            primary = state["targets"]["primary"]
            self.assertEqual("applied", primary["phase"])
            self.assertEqual(primary["desiredSource"], primary["appliedSource"])
            self.assertEqual("retired", state["targets"]["steel"]["phase"])
            self.assertRegex(primary["appliedGeneration"], r"\A[0-9a-f]{64}\Z")

            verify = pika.BlockingConnection(pika.URLParameters(settings[5]))
            try:
                verify_ch = verify.channel()
                message = verify_ch.basic_get(queue=queue, auto_ack=False)
                self.assertIsNone(message[0], "successful target proof must precede broker acknowledgement")
                verify_ch.queue_delete(queue=queue)
            finally:
                verify.close()
        finally:
            if bind.gate_release is not None:
                bind.gate_release.touch(exist_ok=True)
            disconnect_consumer.set()
            stop.set()
            if consumer_thread is not None and consumer_thread.is_alive():
                consumer_thread.join(timeout=5)
            for worker in (crash_worker_process, retry_worker_process):
                if worker is not None and worker.poll() is None:
                    worker.kill()
                    worker.wait(timeout=5)
            try:
                cleanup = connect_db(settings)
                with cleanup.cursor() as cur:
                    if entity_id is not None:
                        cur.execute("DELETE FROM ip_addresses WHERE entity_id=%s", (entity_id,))
                        cur.execute("DELETE FROM entities WHERE id=%s", (entity_id,))
                    for ns_id in nameserver_ids:
                        cur.execute("DELETE FROM ip_addresses WHERE entity_id=%s", (ns_id,))
                        cur.execute("DELETE FROM entities WHERE id=%s", (ns_id,))
                    if zone_id is not None:
                        cur.execute("DELETE FROM dns_zones WHERE id=%s", (zone_id,))
                    cur.execute("DELETE FROM sor_outbox WHERE (table_name='entities' AND row_id=%s) "
                                "OR (table_name='ip_addresses' AND row_id=%s) "
                                "OR (table_name='dns_zones' AND row_id=%s)",
                                (entity_id or 0, address_id or 0, zone_id or 0))
                    for ns_id, ns_address_id in zip(nameserver_ids, nameserver_address_ids):
                        cur.execute("DELETE FROM sor_outbox WHERE "
                                    "(table_name='entities' AND row_id=%s) OR "
                                    "(table_name='ip_addresses' AND row_id=%s)",
                                    (ns_id, ns_address_id))
                    cur.execute("DELETE FROM sources WHERE id=%s", (source_id,))
                cleanup.close()
            finally:
                try:
                    cleanup_broker = pika.BlockingConnection(pika.URLParameters(settings[5]))
                    try:
                        cleanup_broker.channel().queue_delete(queue=queue)
                    finally:
                        cleanup_broker.close()
                except Exception:
                    pass
                db.close()
                bind.stop()
                shutil.rmtree(temp, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
