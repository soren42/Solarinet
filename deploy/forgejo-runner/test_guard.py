from pathlib import Path
import re
import unittest


DEPLOY_DIR = Path(__file__).parent
GUARD = (DEPLOY_DIR / "solarinet-ci-guard.nft").read_text(encoding="utf-8")
MATCH = 'iifname { "virbr-ci0", "ci-tap0" }'
# Nftables and libvirt domain XML quote interface-name values in these files.
RESERVED_TAP = re.compile(r"['\"](?:vnet|vif|macvtap|macvlan)[^'\"]*['\"]")
TEXT_SUFFIXES = {".nft", ".service", ".xml", ".sh", ".conf", ".yml", ".yaml"}


class ForgejoRunnerGuardTests(unittest.TestCase):
    def test_uses_nonreserved_pinned_tap_for_input_and_forward_rules(self):
        self.assertGreaterEqual(GUARD.count(MATCH), 20)
        for path in DEPLOY_DIR.iterdir():
            if path.is_file() and path != Path(__file__) and path.suffix in TEXT_SUFFIXES:
                self.assertNotRegex(path.read_text(encoding="utf-8"), RESERVED_TAP, path.name)

    def test_every_private_or_public_egress_rule_matches_runner_tap(self):
        scoped_rules = [
            line for line in GUARD.splitlines()
            if "iifname" in line and not line.lstrip().startswith("#")
        ]
        self.assertGreaterEqual(len(scoped_rules), 20)
        self.assertTrue(all(MATCH in line for line in scoped_rules))


if __name__ == "__main__":
    unittest.main()
