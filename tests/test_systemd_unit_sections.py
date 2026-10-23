#!/usr/bin/env python3
"""Check systemd directives whose section placement affects runtime policy."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
SECTIONED_DIRECTIVES = {
    "StartLimitIntervalSec": "Unit",
    "StartLimitInterval": "Unit",
    "StartLimitBurst": "Unit",
    "StartLimitAction": "Unit",
}


def misplaced_directives():
    problems = []
    units = sorted(ROOT.rglob("*.service"))
    for path in units:
        # Do not inspect generated build trees or downloaded vendor sources.
        rel = path.relative_to(ROOT)
        if any(part.startswith("build") or part in {"third_party", "vendor"}
               for part in rel.parts[:-1]):
            continue
        section = None
        for line_no, raw in enumerate(path.read_text(errors="replace").splitlines(), 1):
            line = raw.strip()
            if not line or line.startswith(("#", ";")):
                continue
            if line.startswith("[") and line.endswith("]"):
                section = line[1:-1]
                continue
            key, sep, _ = line.partition("=")
            expected = SECTIONED_DIRECTIVES.get(key.strip()) if sep else None
            if expected and section != expected:
                problems.append(f"{rel}:{line_no}: {key.strip()} must be in [{expected}], found [{section}]")
    return units, problems


class SystemdUnitSectionTests(unittest.TestCase):
    def test_start_limit_directives_are_in_unit_section(self):
        units, problems = misplaced_directives()
        self.assertGreater(len(units), 0, "no systemd service units discovered")
        self.assertEqual([], problems, "\n".join(problems))


if __name__ == "__main__":
    units, problems = misplaced_directives()
    if problems:
        print("\n".join(problems))
    else:
        print(f"systemd unit section checks passed across {len(units)} service files")
    unittest.main(argv=[__file__], exit=False)
