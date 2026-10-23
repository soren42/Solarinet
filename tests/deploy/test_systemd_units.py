"""Systemd unit-file checks for deployment artifacts."""
from pathlib import Path
import unittest


REPO = Path(__file__).resolve().parents[2]
DEPLOY = REPO / "deploy"


class SystemdUnitTest(unittest.TestCase):
    def test_start_limit_directives_are_scoped_to_unit_section(self):
        failures = []
        unit_files = sorted(DEPLOY.rglob("*.service"))
        self.assertTrue(unit_files, "no deploy/*.service files found")

        for path in unit_files:
            section = ""
            for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                line = raw.strip()
                if line.startswith("[") and line.endswith("]"):
                    section = line[1:-1]
                elif line.startswith(("StartLimitIntervalSec=", "StartLimitBurst=")):
                    if section != "Unit":
                        failures.append(f"{path.relative_to(REPO)}:{number}: {line} in [{section}]")

        self.assertEqual(failures, [], "\n".join(failures))


if __name__ == "__main__":
    unittest.main()
