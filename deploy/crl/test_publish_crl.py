from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
import datetime as dt

import publish_crl


def run(*args: str) -> None:
    subprocess.run(args, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class PublishCrlTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory(prefix="solarinet-publish-test-")
        self.root = pathlib.Path(self.temp.name)
        self.ca = self.root / "ca.pem"
        self.key = self.root / "ca.key"
        self.source = self.root / "current.pem"
        self.public = self.root / "public"
        (self.root / "newcerts").mkdir()
        (self.root / "index.txt").touch()
        (self.root / "serial").write_text("1000\n", encoding="ascii")
        (self.root / "crlnumber").write_text("1000\n", encoding="ascii")
        run("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3",
            "-subj", "/CN=Publisher Test CA",
            "-addext", "basicConstraints=critical,CA:TRUE",
            "-addext", "keyUsage=critical,keyCertSign,cRLSign",
            "-keyout", str(self.key), "-out", str(self.ca))
        self.config = self.root / "ca.cnf"
        self.config.write_text(
            "[ ca ]\ndefault_ca = issuer\n[ issuer ]\n"
            f"dir = {self.root}\ndatabase = $dir/index.txt\nnew_certs_dir = $dir/newcerts\n"
            "certificate = $dir/ca.pem\nprivate_key = $dir/ca.key\n"
            "serial = $dir/serial\ncrlnumber = $dir/crlnumber\n"
            "default_md = sha256\ndefault_crl_days = 1\npolicy = policy\n"
            "[ policy ]\ncommonName = supplied\n", encoding="utf-8")
        self.crls: list[pathlib.Path] = []
        for index in range(3):
            output = self.root / f"crl-{index}.pem"
            run("openssl", "ca", "-batch", "-config", str(self.config),
                "-gencrl", "-out", str(output))
            self.crls.append(output)
        self.source.write_bytes(self.crls[0].read_bytes())

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_valid_crl_is_published_with_issuer_scoped_name_and_public_mode(self) -> None:
        target, number, changed = publish_crl.publish(self.source, self.ca, self.public)
        self.assertTrue(changed)
        self.assertEqual(0x1000, number)
        self.assertEqual(".crl.pem", target.name[-8:])
        self.assertEqual(self.source.read_bytes(), target.read_bytes())
        self.assertEqual(0o644, target.stat().st_mode & 0o777)
        target2, number2, changed2 = publish_crl.publish(self.source, self.ca, self.public)
        self.assertEqual((target, number, False), (target2, number2, changed2))

    def test_number_rollback_and_malformed_source_are_rejected(self) -> None:
        self.source.write_bytes(self.crls[2].read_bytes())
        target, _, _ = publish_crl.publish(self.source, self.ca, self.public)
        previous = target.read_bytes()
        self.source.write_bytes(self.crls[0].read_bytes())
        with self.assertRaisesRegex(ValueError, "rollback"):
            publish_crl.publish(self.source, self.ca, self.public)
        self.assertEqual(previous, target.read_bytes())
        self.source.write_bytes(b"not a CRL")
        with self.assertRaisesRegex(ValueError, "exactly one PEM CRL"):
            publish_crl.publish(self.source, self.ca, self.public)

    def test_newer_crl_recovers_from_an_expired_existing_publication(self) -> None:
        expired = self.root / "expired.pem"
        (self.root / "crlnumber").write_text("1000\n", encoding="ascii")
        run("openssl", "ca", "-batch", "-config", str(self.config), "-gencrl",
            "-crl_lastupdate", "200101000000Z", "-crl_nextupdate", "200102000000Z",
            "-out", str(expired))
        issuer_fp, old_number, _, _ = publish_crl.crl_info(expired.read_bytes(), self.ca,
                                                           require_fresh=False)
        target = self.public / f"{issuer_fp}.crl.pem"
        self.public.mkdir()
        target.write_bytes(expired.read_bytes())
        self.source.write_bytes(self.crls[2].read_bytes())
        result, new_number, changed = publish_crl.publish(self.source, self.ca, self.public)
        self.assertEqual(target, result)
        self.assertGreater(new_number, old_number)
        self.assertTrue(changed)
        self.assertEqual(self.source.read_bytes(), target.read_bytes())

    def test_future_this_update_and_overlong_validity_are_rejected(self) -> None:
        future = self.root / "future.pem"
        run("openssl", "ca", "-batch", "-config", str(self.config), "-gencrl",
            "-crl_lastupdate", "300101000000Z", "-crl_nextupdate", "300102000000Z",
            "-out", str(future))
        self.source.write_bytes(future.read_bytes())
        with self.assertRaisesRegex(ValueError, "thisUpdate is in the future"):
            publish_crl.publish(self.source, self.ca, self.public)

        long_lived = self.root / "long-lived.pem"
        self.config.write_text(self.config.read_text(encoding="utf-8").replace(
            "default_crl_days = 1", "default_crl_days = 10"), encoding="utf-8")
        run("openssl", "ca", "-batch", "-config", str(self.config), "-gencrl",
            "-out", str(long_lived))
        self.source.write_bytes(long_lived.read_bytes())
        with self.assertRaisesRegex(ValueError, "seven days"):
            publish_crl.publish(self.source, self.ca, self.public)

    def test_crl_with_less_than_one_hour_remaining_is_rejected(self) -> None:
        almost_expired = self.root / "almost-expired.pem"
        now = dt.datetime.now(dt.timezone.utc)
        last_update = (now - dt.timedelta(minutes=1)).strftime("%y%m%d%H%M%SZ")
        next_update = (now + dt.timedelta(minutes=30)).strftime("%y%m%d%H%M%SZ")
        run("openssl", "ca", "-batch", "-config", str(self.config), "-gencrl",
            "-crl_lastupdate", last_update, "-crl_nextupdate", next_update,
            "-out", str(almost_expired))
        self.source.write_bytes(almost_expired.read_bytes())
        with self.assertRaisesRegex(ValueError, "too close to expiry"):
            publish_crl.publish(self.source, self.ca, self.public)

    def test_service_masks_ca_private_key_and_database_credential(self) -> None:
        unit = pathlib.Path(__file__).with_name("solari-crl-publish.service").read_text(
            encoding="utf-8")
        inaccessible = next(line for line in unit.splitlines()
                            if line.startswith("InaccessiblePaths="))
        self.assertIn("/var/lib/solarinet/pki/ca.key", inaccessible)
        self.assertIn("/etc/solarinet/crl-generator.my.cnf", inaccessible)


if __name__ == "__main__":
    unittest.main()
