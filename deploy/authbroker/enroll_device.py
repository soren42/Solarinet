#!/usr/bin/env python3
"""enroll_device.py — mint an ECDSA-P256 approval keypair for a Tab5.

Prints two artifacts:
  * the PRIVATE key (goes ONTO the device, into encrypted NVS — see the firmware
    README "device-key enrollment"); never store it on the broker host.
  * the PUBLIC key PEM, to paste into authbroker.conf under [device.<id>].

Usage:
    ./enroll_device.py tab5-desk
    ./enroll_device.py tab5-desk --out-dir ./keys      # also write files

The curve is secp256r1 (NIST P-256); the device signs approval responses with
ECDSA/SHA-256 and the broker verifies with the public key only.
"""
from __future__ import annotations

import argparse
import sys

try:
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
except ImportError:
    print("python-cryptography required: pip install cryptography", file=sys.stderr)
    sys.exit(1)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("device_id", help="e.g. tab5-desk (matches [device.<id>])")
    ap.add_argument("--out-dir", help="also write <id>.key / <id>.pub here")
    args = ap.parse_args()

    key = ec.generate_private_key(ec.SECP256R1())
    priv_pem = key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.PKCS8,
        serialization.NoEncryption(),
    ).decode()
    # Raw 32-byte private scalar (what the firmware stores in NVS, hex).
    priv_scalar = key.private_numbers().private_value.to_bytes(32, "big").hex()
    pub_pem = key.public_key().public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    ).decode()
    # Uncompressed SEC1 point (65 bytes, 0x04 || X || Y) — handy for the device.
    pub_raw = key.public_key().public_bytes(
        serialization.Encoding.X962,
        serialization.PublicFormat.UncompressedPoint,
    ).hex()

    print(f"# device_id: {args.device_id}")
    print("# ---- PRIVATE (device only; NVS 'dev/privkey', hex scalar) ----")
    print(priv_scalar)
    print("# ---- PRIVATE (PKCS8 PEM, if you prefer to import a PEM) ----")
    print(priv_pem.strip())
    print()
    print(f"# ---- Paste into authbroker.conf ----")
    print(f"[device.{args.device_id}]")
    print("pubkey_pem =")
    for line in pub_pem.strip().splitlines():
        print(f"    {line}")
    print()
    print(f"# device public point (uncompressed, for firmware cross-check):")
    print(f"#   {pub_raw}")

    if args.out_dir:
        import os
        os.makedirs(args.out_dir, exist_ok=True)
        kp = os.path.join(args.out_dir, f"{args.device_id}.key")
        pp = os.path.join(args.out_dir, f"{args.device_id}.pub")
        with open(kp, "w") as fh:
            fh.write(priv_pem)
        os.chmod(kp, 0o600)
        with open(pp, "w") as fh:
            fh.write(pub_pem)
        print(f"# wrote {kp} (0600) and {pp}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
