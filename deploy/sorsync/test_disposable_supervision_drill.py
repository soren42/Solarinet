"""Disposable MariaDB/RabbitMQ process restart and target retry drill.

Run directly with `python3 deploy/sorsync/test_disposable_supervision_drill.py`.
Docker containers are uniquely named and removed in tearDown. The test uses the
real schema snapshot, sor_emitd.publish_batch, and delivery.consume_after_apply;
the target operations are idempotent file-backed stand-ins for DNS/Pi-hole.
"""
import json
import os
import pathlib
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import textwrap
import time
import unittest
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[2]
DOCKER = ["sudo", "-n", "docker"]


def available_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


@unittest.skipUnless(shutil.which("docker"), "Docker is required for disposable process drill")
class DisposableSupervisionDrill(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="solari-sorsync-drill-")
        self.root = pathlib.Path(self.temp.name)
        self.addCleanup(self.temp.cleanup)
        suffix = uuid.uuid4().hex[:12]
        self.db_name = "solari-mariadb-" + suffix
        self.mq_name = "solari-rabbit-" + suffix
        self.db_pass = uuid.uuid4().hex
        self.db_port = available_port()
        self.mq_port = available_port()
        self.containers = []
        self.processes = []
        self.run_docker(["run", "--detach", "--name", self.db_name, "-e",
                         "MARIADB_ROOT_PASSWORD=" + self.db_pass, "-p",
                         f"127.0.0.1:{self.db_port}:3306", "mariadb:11.4"])
        self.containers.append(self.db_name)
        self.addCleanup(self.remove_container, self.db_name)
        self.run_docker(["run", "--detach", "--name", self.mq_name, "-p",
                         f"127.0.0.1:{self.mq_port}:5672", "rabbitmq:4.1"])
        self.containers.append(self.mq_name)
        self.addCleanup(self.remove_container, self.mq_name)
        self.wait_container(self.db_name, "mariadb-admin", "ping", "-h127.0.0.1", "-uroot",
                            "--silent", env={"MYSQL_PWD": self.db_pass})
        self.wait_container(self.mq_name, "rabbitmq-diagnostics", "-q", "check_running")
        env = {**os.environ, "MYSQL_PWD": self.db_pass}
        self.run_docker(["exec", "-e", "MYSQL_PWD=" + self.db_pass, self.db_name, "mariadb", "-h127.0.0.1", "-uroot", "-e",
                         "CREATE DATABASE solarinet_drill CHARACTER SET utf8mb4"], env=env)
        schema = (ROOT / "netdb/sor/schema.snapshot.sql").read_text(encoding="utf-8")
        loaded = subprocess.run(DOCKER + ["exec", "-i", "-e", "MYSQL_PWD=" + self.db_pass,
                                          self.db_name, "mariadb", "-h127.0.0.1", "-uroot",
                                          "solarinet_drill"], input=schema, text=True,
                               capture_output=True, timeout=120, env=env)
        self.assertEqual(0, loaded.returncode, "disposable SoR schema import failed")
        self.db_url = f"127.0.0.1:{self.db_port}"
        self.amqp = f"amqp://guest:guest@127.0.0.1:{self.mq_port}/%2F"
        self.queue = "sor.drill." + suffix
        self.exchange = "sor.events"
        self.events = self.root / "events.json"
        self.target_a = self.root / "dns-target.json"
        self.target_b = self.root / "pihole-target.json"
        self.failure_marker = self.root / "fail-target-b-once"
        self.write_workers()

    def run_docker(self, args, **kwargs):
        result = subprocess.run(DOCKER + args, capture_output=True, text=True,
                                timeout=kwargs.pop("timeout", 30), **kwargs)
        self.assertEqual(0, result.returncode,
                         "disposable Docker operation failed: " + args[0] + " " + result.stderr[-1000:])
        return result.stdout.strip()

    def remove_container(self, name):
        subprocess.run(DOCKER+["rm","--force",name],capture_output=True,text=True,timeout=30)

    def wait_container(self, name, *command, env=None, timeout=90):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            env_args = ["-e", "MYSQL_PWD=" + env["MYSQL_PWD"]] if env and "MYSQL_PWD" in env else []
            result = subprocess.run(DOCKER + ["exec", *env_args, name, *command], capture_output=True,
                                    text=True, timeout=10, env={**os.environ, **(env or {})})
            if result.returncode == 0:
                return
            time.sleep(0.5)
        self.fail("disposable service did not become ready: " + name)

    def write_workers(self):
        producer = self.root / "producer.py"
        producer.write_text(textwrap.dedent('''\
            import configparser, json, pathlib, signal, sys, time
            import sor_emitd
            cfg=json.loads(pathlib.Path(sys.argv[1]).read_text())
            config=configparser.ConfigParser(interpolation=None)
            config.read_dict({"sor":{"host":"127.0.0.1","port":str(cfg["db_port"]),
                "user":"root","password":cfg["db_pass"],"name":"solarinet_drill"},
                "amqp":{"url":cfg["amqp"],"exchange":"sor.events",
                "poll_interval_sec":"0.1","reconnect_delay_sec":"0.2","batch":"500"}})
            sor_emitd.load_cfg=lambda:config
            original=sor_emitd.publish_batch
            class Gate:
                def __init__(self, channel): self.channel=channel; self.count=0
                def __getattr__(self,name): return getattr(self.channel,name)
                def basic_publish(self,*args,**kwargs):
                    result=self.channel.basic_publish(*args,**kwargs); self.count+=1
                    if self.count==1 and cfg.get("gate") and not pathlib.Path(cfg["release"]).exists():
                        pathlib.Path(cfg["gate"]).touch()
                        while not pathlib.Path(cfg["release"]).exists(): time.sleep(.02)
                    return result
            def gated_publish(channel,db,rows,exchange,checkpoint,optional=frozenset()):
                return original(Gate(channel),db,rows,exchange,checkpoint,optional)
            sor_emitd.publish_batch=gated_publish
            def stop(*_): sor_emitd._run=False
            signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
            sor_emitd.run()
        '''), encoding="utf-8")
        consumer = self.root / "consumer.py"
        consumer.write_text(textwrap.dedent('''\
            import json, pathlib, sys, time
            import pika
            import delivery
            cfg=json.loads(pathlib.Path(sys.argv[1]).read_text())
            if pathlib.Path(cfg["retry_hold"]).exists():
                pathlib.Path(cfg["retry_ready"]).touch()
                while not pathlib.Path(cfg["retry_release"]).exists(): time.sleep(.02)
            conn=pika.BlockingConnection(pika.URLParameters(cfg["amqp"]))
            ch=conn.channel(); ch.exchange_declare(exchange="sor.events",exchange_type="topic",durable=True)
            ch.queue_declare(queue=cfg["queue"],durable=True)
            ch.queue_bind(queue=cfg["queue"],exchange="sor.events",routing_key="sor.entities.*")
            seen=[]; calls=0
            class Observe:
                def __init__(self, channel): self.channel=channel
                def __getattr__(self,name): return getattr(self.channel,name)
                def consume(self,*args,**kwargs):
                    for method,props,body in self.channel.consume(*args,**kwargs):
                        if method is not None:
                            event=json.loads(body); seen.append(event["outbox_id"])
                            with pathlib.Path(cfg["proof"]).open("a") as proof:
                                proof.write(json.dumps({"id":event["outbox_id"],
                                    "redelivered":method.redelivered})+"\\n")
                        yield method,props,body
            def apply():
                global calls
                calls += 1
                for path in (pathlib.Path(cfg["a"]),pathlib.Path(cfg["b"])):
                    current=set(json.loads(path.read_text())) if path.exists() else set()
                    if str(path)==cfg["b"] and seen and pathlib.Path(cfg["failure"]).exists():
                        pathlib.Path(cfg["failure"]).unlink(); return False
                    current.update(seen); path.write_text(json.dumps(sorted(current)))
                if calls == 1: pathlib.Path(cfg["ready"]).touch()
                return True
            delivery.consume_after_apply(Observe(ch),cfg["queue"],.1,0,apply,lambda:True,
                maintain_connection=lambda:conn.process_data_events(time_limit=.1))
        '''), encoding="utf-8")

    def start_supervisor(self, script, config_path, name):
        supervisor = self.root / (name + "-supervisor.py")
        supervisor.write_text(textwrap.dedent('''\
            import pathlib, signal, subprocess, sys, time
            script, config, pidfile = sys.argv[1:]
            stopping=False
            child=None
            def stop(*_):
                global stopping
                stopping=True
                if child is not None and child.poll() is None: child.terminate()
            signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
            while True:
                child=subprocess.Popen([sys.executable,script,config])
                pathlib.Path(pidfile).write_text(str(child.pid))
                code=child.wait()
                if stopping: break
                if code == 0: break
                time.sleep(.2) # mirrors Restart=on-failure + RestartSec
        '''), encoding="utf-8")
        log = (self.root / (name + ".log")).open("w")
        process = subprocess.Popen([sys.executable, str(supervisor), str(script),
                                    str(config_path), str(self.root / (name + ".pid"))],
                                   cwd=ROOT, env={**os.environ,
                                       "PYTHONPATH": f"{ROOT / 'deploy/sorsync'}:{ROOT / 'deploy/sorsync'}"},
                                   stdout=log, stderr=subprocess.STDOUT)
        log.close()
        self.processes.append(process)
        return process

    def config(self, **values):
        path = self.root / ("worker-" + uuid.uuid4().hex + ".json")
        path.write_text(json.dumps({"db_port": self.db_port, "db_pass": self.db_pass,
            "amqp": self.amqp, "queue": self.queue, "a": str(self.target_a),
            "b": str(self.target_b), "failure": str(self.failure_marker),
            "ready": str(self.root / "consumer-ready"),
            "proof": str(self.root / "deliveries.jsonl"),
            "retry_hold": str(self.root / "retry-hold"),
            "retry_ready": str(self.root / "retry-ready"),
            "retry_release": str(self.root / "retry-release"), **values}))
        return path

    def test_emitter_restart_broker_restart_and_atomic_multi_target_ack(self):
        import pika
        import pymysql
        conn=pymysql.connect(host="127.0.0.1",port=self.db_port,user="root",password=self.db_pass,
                             database="solarinet_drill",autocommit=True)
        with conn.cursor() as cur:
            cur.execute("INSERT INTO sor_sync_state(k,v) VALUES('emit_checkpoint',0) ON DUPLICATE KEY UPDATE v=0")
            cur.executemany("INSERT INTO sor_outbox(table_name,row_id,action) VALUES('entities',%s,'insert')",
                            [(i,) for i in range(1,5)])
            cur.execute("SELECT id FROM sor_outbox ORDER BY id")
            expected=[int(row[0]) for row in cur.fetchall()]
        broker=pika.BlockingConnection(pika.URLParameters(self.amqp)); ch=broker.channel()
        ch.exchange_declare(exchange=self.exchange,exchange_type="topic",durable=True)
        ch.queue_declare(queue=self.queue,durable=True)
        ch.queue_bind(queue=self.queue,exchange=self.exchange,routing_key="sor.entities.*")
        broker.close()

        gate=self.root/"publisher-gated"; release=self.root/"publisher-release"
        producer_cfg=self.config(gate=str(gate),release=str(release))
        supervisor=self.start_supervisor(self.root/"producer.py",producer_cfg,"emitter")
        deadline=time.monotonic()+30
        while not gate.exists() and time.monotonic()<deadline: time.sleep(.05)
        self.assertTrue(gate.exists(),"emitter did not reach the mid-batch fault point: " +
                        (self.root/"emitter.log").read_text()[-3000:])
        with conn.cursor() as cur:
            cur.execute("SELECT v FROM sor_sync_state WHERE k='emit_checkpoint'")
            self.assertEqual(0, int(cur.fetchone()[0]), "checkpoint advanced before the batch was confirmed")
        pid=int((self.root/"emitter.pid").read_text()); os.kill(pid,signal.SIGKILL)
        killed_emitter_pid = pid
        release.touch()  # the restarted emitter passes the one-shot fault point
        deadline=time.monotonic()+30
        while True:
            with conn.cursor() as cur:
                cur.execute("SELECT v FROM sor_sync_state WHERE k='emit_checkpoint'")
                checkpoint=int(cur.fetchone()[0])
            if checkpoint>=expected[-1] or time.monotonic()>=deadline: break
            time.sleep(.1)
        self.assertEqual(expected[-1],checkpoint,"supervisor did not finish outbox after emitter SIGKILL")
        self.assertNotEqual(killed_emitter_pid, int((self.root/"emitter.pid").read_text()),
                            "Restart=on-failure supervisor did not relaunch the emitter")
        conn.close()
        consumer_cfg=self.config()
        consumer=self.start_supervisor(self.root/"consumer.py",consumer_cfg,"applier")
        ready=self.root/"consumer-ready"
        deadline=time.monotonic()+30
        while not ready.exists() and time.monotonic()<deadline: time.sleep(.05)
        self.assertTrue(ready.exists(),"applier did not complete startup reconciliation")
        original_applier_pid = int((self.root/"applier.pid").read_text())

        # The consumer process remains under supervision across a broker outage.
        self.run_docker(["stop",self.mq_name])
        self.run_docker(["start",self.mq_name])
        self.wait_container(self.mq_name,"rabbitmq-diagnostics","-q","check_running")
        conn=pymysql.connect(host="127.0.0.1",port=self.db_port,user="root",password=self.db_pass,
                             database="solarinet_drill",autocommit=True)
        with conn.cursor() as cur:
            cur.execute("INSERT INTO sor_outbox(table_name,row_id,action) VALUES('entities',99,'insert')")
            expected.append(int(cur.lastrowid))
        conn.close()
        retry_hold=self.root/"retry-hold"
        retry_hold.touch()
        self.failure_marker.touch()
        # The same sor_emitd.run process reconnects and publishes this row.
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            if self.target_a.exists() and self.target_b.exists():
                a=json.loads(self.target_a.read_text()); b=json.loads(self.target_b.read_text())
                if a==expected and b==expected: break
            if (self.root/"retry-ready").exists():
                probe=pika.BlockingConnection(pika.URLParameters(self.amqp)); probe_ch=probe.channel()
                method, _props, body=probe_ch.basic_get(self.queue,auto_ack=False)
                if method is not None:
                    self.assertTrue(method.redelivered,"second-target failure must requeue without ack")
                    self.assertEqual(expected[-1],json.loads(body)["outbox_id"])
                    probe_ch.basic_nack(method.delivery_tag,requeue=True)
                    (self.root/"retry-release").touch()
                    (self.root/"retry-ready").unlink(missing_ok=True)
                probe.close()
            time.sleep(.1)
        self.assertTrue((self.root/"retry-release").exists(),"failed delivery was not observed requeued")
        self.assertTrue(self.target_a.exists() and self.target_b.exists(),"targets never converged")
        self.assertEqual(expected,json.loads(self.target_a.read_text()),"DNS stub lost/duplicated events")
        self.assertEqual(expected,json.loads(self.target_b.read_text()),"Pi-hole stub lost/duplicated events")
        self.assertNotEqual(original_applier_pid, int((self.root/"applier.pid").read_text()),
                            "broker/target fault did not exercise supervised applier restart")
        check=pika.BlockingConnection(pika.URLParameters(self.amqp)); verify=check.channel()
        self.assertIsNone(verify.basic_get(self.queue,auto_ack=False)[0],"delivery remained unacked after both targets recovered")
        verify.queue_delete(self.queue); check.close()
        self.assertIsNone(supervisor.poll(), "emitter daemon supervisor should remain active after recovery")
        self.assertIsNone(consumer.poll(), "applier supervisor should stay active after recovery")

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
                try: process.wait(timeout=5)
                except subprocess.TimeoutExpired: process.kill(); process.wait(timeout=5)
        for name in reversed(self.containers):
            subprocess.run(DOCKER+["rm","--force",name],capture_output=True,text=True,timeout=30)
        self.temp.cleanup()


if __name__ == "__main__":
    unittest.main(verbosity=2)
