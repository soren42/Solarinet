"""Pin the RabbitMQ Quadlet's deployment and credential boundary."""
from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CONTAINER = ROOT / "deploy/rabbitmq/solarinet-rabbitmq.container"
EXAMPLE = ROOT / "deploy/rabbitmq/rabbitmq.env.example"
GITIGNORE = ROOT / ".gitignore"
README = ROOT / "deploy/rabbitmq/README.md"


def parse_quadlet(path):
    sections = {}
    section = None
    for raw in path.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if line.startswith("[") and line.endswith("]"):
            section = line[1:-1]
            sections.setdefault(section, {})
            continue
        if section is None or "=" not in line:
            raise AssertionError(f"invalid Quadlet line: {line}")
        key, value = line.split("=", 1)
        sections[section].setdefault(key, []).append(value)
    return sections


class RabbitMQQuadletContractTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sections = parse_quadlet(CONTAINER)
        cls.container = cls.sections["Container"]
        cls.service = cls.sections["Service"]

    def test_image_is_versioned_digest_and_never_pulled_implicitly(self):
        image = self.container["Image"][0]
        self.assertRegex(
            image,
            r"^docker\.io/library/rabbitmq:3\.13\.7-management@sha256:[0-9a-f]{64}$",
        )
        self.assertEqual(self.container["Pull"], ["never"])
        self.assertEqual(self.container["ContainerName"], ["rabbitmq"])

    def test_existing_persistent_data_and_listener_contract_is_preserved(self):
        self.assertEqual(self.container["Volume"], ["rabbitmq-data:/var/lib/rabbitmq"])
        self.assertEqual(self.container["PublishPort"], ["5672:5672", "15672:15672"])

    def test_health_failure_restarts_service_under_systemd(self):
        self.assertIn("rabbitmq-diagnostics -q check_running", self.container["HealthCmd"][0])
        self.assertEqual(self.container["HealthOnFailure"], ["kill"])
        self.assertEqual(self.service["Restart"], ["always"])
        self.assertTrue(int(self.service["TimeoutStopSec"][0]) >= 60)

    def test_credentials_are_external_and_example_is_placeholder_only(self):
        self.assertEqual(self.container["EnvironmentFile"], ["/etc/solarinet/rabbitmq.env"])
        manifest = CONTAINER.read_text(encoding="utf-8")
        for key in ("RABBITMQ_DEFAULT_USER", "RABBITMQ_DEFAULT_PASS", "RABBITMQ_DEFAULT_VHOST"):
            self.assertNotIn(key, manifest)
        example = EXAMPLE.read_text(encoding="utf-8")
        self.assertRegex(example, r"(?m)^RABBITMQ_DEFAULT_PASS=REPLACE_WITH_[A-Z_]+$")
        self.assertIn("deploy/rabbitmq/rabbitmq.env", GITIGNORE.read_text(encoding="utf-8"))

    def test_cutover_and_rollback_preserve_the_named_volume(self):
        guide = README.read_text(encoding="utf-8")
        self.assertIn("podman update --restart-policy=no rabbitmq", guide)
        self.assertIn("podman rename rabbitmq rabbitmq-rollback-20261022", guide)
        self.assertIn("podman update --restart-policy=unless-stopped rabbitmq", guide)
        self.assertIn("without deleting `rabbitmq-data`", guide)


if __name__ == "__main__":
    unittest.main()
