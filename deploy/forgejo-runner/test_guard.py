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

    def test_private_https_is_dropped_before_public_https_allowance(self):
        private_drop = 'iifname { "virbr-ci0", "ci-tap0" } ip daddr 10.0.0.0/8 counter drop'
        public_https = 'iifname { "virbr-ci0", "ci-tap0" } tcp dport { 80, 443 } counter accept'
        self.assertIn(private_drop, GUARD)
        self.assertIn(public_https, GUARD)
        self.assertLess(GUARD.index(private_drop), GUARD.index(public_https))
        packet_test = (DEPLOY_DIR / "test_guard_namespace.sh").read_text(encoding="utf-8")
        self.assertIn('s.connect(("10.1.0.200", 443))', packet_test)
        self.assertIn("private_https_drop_rule_packets", packet_test)

    def test_forwarding_into_guest_network_only_allows_established_replies(self):
        forward = GUARD.split("chain forward {", 1)[1].split("    }", 1)[0]
        reply = 'oifname { "virbr-ci0", "ci-tap0" } ct state established,related counter accept'
        reject = 'oifname { "virbr-ci0", "ci-tap0" } counter drop'
        self.assertIn(reply, forward)
        self.assertIn(reject, forward)
        self.assertLess(forward.index(reply), forward.index(reject))


if __name__ == "__main__":
    unittest.main()
