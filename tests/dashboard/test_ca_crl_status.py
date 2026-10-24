#!/usr/bin/env python3
"""Exercise the fail-soft CRL rollout API projection with fixture rows."""
from __future__ import annotations

import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest


ROOT = Path(__file__).resolve().parents[2]
PHP = shutil.which("php")


@unittest.skipUnless(PHP, "PHP is required")
class CaCrlStatusTests(unittest.TestCase):
    def query(self, *, fail: bool = False, expired: bool = False,
              no_next_update: bool = False) -> dict:
        env = os.environ.copy()
        env["STEPCA_URL"] = "http://step-ca-disabled.invalid"
        script = r'''class Db {
  public static function rows(string $q): array {
    if (strpos($q, 'FROM certificateCrlState') !== false) {
      if (getenv('TEST_CRL_DB_FAIL') === '1') throw new RuntimeException('migration missing');
      return [['issuerFp' => str_repeat('a', 64), 'crlNumber' => '12',
        'crlFp' => str_repeat('f', 64), 'generatedAt' => '2026-10-23 12:00:00',
        'nextUpdate' => getenv('TEST_CRL_NO_NEXT_UPDATE') === '1' ? null : '2026-10-24 12:00:00',
        'expired' => getenv('TEST_CRL_EXPIRED') === '1' || getenv('TEST_CRL_NO_NEXT_UPDATE') === '1' ? 1 : 0]];
    }
    if (strpos($q, 'FROM node n') !== false) return [
      ['nodeId'=>'1','hostFqdn'=>'current.akoria.net','role'=>'client','state'=>'up','issuerFp'=>str_repeat('a',64),'lastSeenAt'=>'2026-10-23 12:00:00','staleSec'=>15],
      ['nodeId'=>'2','hostFqdn'=>'stale.akoria.net','role'=>'monitor','state'=>'up','issuerFp'=>str_repeat('a',64),'lastSeenAt'=>'2026-10-23 12:00:00','staleSec'=>20],
      ['nodeId'=>'3','hostFqdn'=>'pending.akoria.net','role'=>'client','state'=>'up','issuerFp'=>str_repeat('a',64),'lastSeenAt'=>'2026-10-23 12:00:00','staleSec'=>30],
      ['nodeId'=>'4','hostFqdn'=>'offline.akoria.net','role'=>'monitor','state'=>'up','issuerFp'=>str_repeat('a',64),'lastSeenAt'=>'2026-10-23 11:40:00','staleSec'=>1000],
      ['nodeId'=>'5','hostFqdn'=>'down.akoria.net','role'=>'client','state'=>'down','issuerFp'=>str_repeat('a',64),'lastSeenAt'=>'2026-10-23 12:00:00','staleSec'=>15],
      ['nodeId'=>'6','hostFqdn'=>'unknown-issuer.akoria.net','role'=>'monitor','state'=>'up','issuerFp'=>null,'lastSeenAt'=>'2026-10-23 12:00:00','staleSec'=>15],
      ['nodeId'=>'7','hostFqdn'=>'different-issuer.akoria.net','role'=>'client','state'=>'up','issuerFp'=>str_repeat('b',64),'lastSeenAt'=>'2026-10-23 12:00:00','staleSec'=>15],
    ];
    if (strpos($q, 'FROM certificateCrlAcknowledgement a') !== false) return [
      ['nodeId'=>'1','issuerFp'=>str_repeat('a',64),'crlNumber'=>'12','crlFp'=>str_repeat('f',64),'acknowledgedAt'=>'2026-10-23 12:01:00'],
      ['nodeId'=>'2','issuerFp'=>str_repeat('a',64),'crlNumber'=>'11','crlFp'=>str_repeat('e',64),'acknowledgedAt'=>'2026-10-23 11:00:00'],
      ['nodeId'=>'5','issuerFp'=>str_repeat('a',64),'crlNumber'=>'12','crlFp'=>str_repeat('f',64),'acknowledgedAt'=>'2026-10-23 12:01:00'],
    ];
    return [];
  }
}
require 'dashboard/api/lib/Coerce.php';
require 'dashboard/api/lib/Ca.php';
echo json_encode(Ca::summary()['crl'], JSON_THROW_ON_ERROR);'''
        if fail:
            env["TEST_CRL_DB_FAIL"] = "1"
        if expired:
            env["TEST_CRL_EXPIRED"] = "1"
        if no_next_update:
            env["TEST_CRL_NO_NEXT_UPDATE"] = "1"
        result = subprocess.run([PHP, "-r", script], cwd=ROOT, env=env,
                                check=True, text=True, capture_output=True, timeout=10)
        return json.loads(result.stdout)

    def test_generation_counts_and_peer_states_are_explicit(self) -> None:
        result = self.query()
        self.assertTrue(result["available"])
        self.assertIn("current leaf issuer", result["scope"])
        generation, = result["generations"]
        self.assertEqual("12", generation["crlNumber"])
        self.assertEqual({"current": 1, "stale": 1, "pending": 1, "offline": 2, "expired": 0}, generation["counts"])
        self.assertEqual(2, generation["excludedPeerCount"])
        self.assertEqual(1, generation["unknownIssuerCount"])
        self.assertEqual(1, generation["otherIssuerCount"])
        self.assertEqual(["unknown_issuer", "different_issuer"],
                         [peer["reason"] for peer in generation["excludedPeers"]])
        self.assertTrue(generation["peers"][0]["acknowledgementCurrent"])
        self.assertEqual(["current", "stale", "pending", "offline", "offline"],
                         [peer["status"] for peer in generation["peers"]])

    def test_missing_acknowledgement_table_degrades_without_failing_endpoint(self) -> None:
        result = self.query(fail=True)
        self.assertFalse(result["available"])
        self.assertEqual([], result["generations"])
        self.assertIn("unavailable", result["reason"].lower())

    def test_expired_generation_never_counts_matching_receipt_as_current(self) -> None:
        generation, = self.query(expired=True)["generations"]
        self.assertTrue(generation["expired"])
        self.assertEqual(0, generation["counts"]["current"])
        self.assertEqual(1, generation["counts"]["expired"])
        self.assertEqual("expired", generation["peers"][0]["status"])
        self.assertFalse(generation["peers"][0]["acknowledgementCurrent"])

    def test_missing_expiry_is_never_reported_as_current(self) -> None:
        generation, = self.query(no_next_update=True)["generations"]
        self.assertTrue(generation["expired"])
        self.assertEqual(0, generation["counts"]["current"])
        self.assertEqual(1, generation["counts"]["expired"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
