#!/usr/bin/env python3
"""Subprocess consumer used to exercise a crash at the target/ack boundary."""
import configparser
import json
import pathlib
import subprocess
import sys
import time

import pika

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import sor_apply_dnsd  # noqa: E402
from delivery import consume_after_apply  # noqa: E402


class AcknowledgementsComplete(Exception):
    pass


class AckGate:
    def __init__(self, channel, options):
        self.channel = channel
        self.options = options
        self.count = 0

    def __getattr__(self, name):
        return getattr(self.channel, name)

    def basic_ack(self, *args, **kwargs):
        state_path = pathlib.Path(self.options["state_path"])
        state = json.loads(state_path.read_text(encoding="utf-8"))
        primary = state["targets"]["primary"]
        answer = subprocess.run(
            [self.options["dig"], "@127.0.0.1", "-p", str(self.options["bind_port"]),
             self.options["hostname"] + "." + self.options["zone"], "A", "+short"],
            capture_output=True, text=True, timeout=3, check=False)
        proof = {
            "targetApplied": primary.get("phase") == "applied" and
                             primary.get("desiredSource") == primary.get("appliedSource"),
            "servedAnswer": answer.returncode == 0 and
                            answer.stdout.strip() == self.options["address"],
        }
        if not all(proof.values()):
            raise RuntimeError("DNS target proof is incomplete; refusing broker acknowledgement")
        with pathlib.Path(self.options["proof_path"]).open("a", encoding="utf-8") as output:
            output.write(json.dumps(proof, sort_keys=True) + "\n")
            output.flush()
        if self.options["hold_before_ack"]:
            pathlib.Path(self.options["gate_path"]).touch()
            while True:
                time.sleep(0.05)
        result = self.channel.basic_ack(*args, **kwargs)
        self.count += 1
        if self.count == self.options["expected_acks"]:
            pathlib.Path(self.options["done_path"]).touch()
            raise AcknowledgementsComplete
        return result


def main(config_path):
    options = json.loads(pathlib.Path(config_path).read_text(encoding="utf-8"))
    config = configparser.ConfigParser(interpolation=None)
    config.read_dict(options["applier_config"])
    connection = pika.BlockingConnection(pika.URLParameters(options["amqp_url"]))
    try:
        channel = connection.channel()
        channel.exchange_declare(exchange=options["exchange"], exchange_type="topic", durable=True)
        channel.queue_declare(queue=options["queue"], durable=True)
        for key in sor_apply_dnsd.DNS_KEYS:
            channel.queue_bind(exchange=options["exchange"], queue=options["queue"], routing_key=key)
        wrapped = AckGate(channel, options)
        try:
            consume_after_apply(
                wrapped, options["queue"], 0.1, 0,
                lambda: sor_apply_dnsd.apply(config), lambda: True,
                max_pending=10,
                maintain_connection=lambda: connection.process_data_events(time_limit=0.1))
        except AcknowledgementsComplete:
            return 0
        return 1
    finally:
        if connection.is_open:
            connection.close()


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1]))
