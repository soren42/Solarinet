#!/usr/bin/env python3
"""Create broker enrollment configuration from a Tab5-generated public point.

The Tab5 creates and retains its private response key. This tool accepts only
the uncompressed SEC1 public point printed by ``SOLARI:SHOW_KEYS`` and emits
the public PEM block for the broker configuration.
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
    ap.add_argument("--public-point-hex-file", required=True,
                    help="file containing the device's 130-character public point")
    args = ap.parse_args()

    try:
        with open(args.public_point_hex_file, "r", encoding="ascii") as fh:
            point_hex = fh.read().strip()
        point = bytes.fromhex(point_hex)
        if len(point) != 65 or point[0] != 4:
            raise ValueError("expected a 65-byte uncompressed P-256 public point")
        public_key = ec.EllipticCurvePublicKey.from_encoded_point(
            ec.SECP256R1(), point
        )
    except (OSError, UnicodeError, ValueError) as exc:
        print(f"invalid device public point: {exc}", file=sys.stderr)
        return 2

    pub_pem = public_key.public_bytes(
        serialization.Encoding.PEM,
        serialization.PublicFormat.SubjectPublicKeyInfo,
    ).decode("ascii")
    print(f"[device.{args.device_id}]")
    print("pubkey_pem =")
    for line in pub_pem.strip().splitlines():
        print(f"    {line}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
