#!/usr/bin/env python3
"""Exercise the operator revoke-cert CLI framing against a local socket stub."""
from __future__ import annotations

import argparse
import pathlib
import socket
import subprocess
import tempfile
import threading


def roundTrip(binary: pathlib.Path, args: list[str], response: str) -> tuple[bytes, subprocess.CompletedProcess[str]]:
    with tempfile.TemporaryDirectory(prefix="solari-ctl-cli-") as tempDir:
        socketPath = pathlib.Path(tempDir) / "ctl.sock"
        listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        listener.bind(str(socketPath))
        listener.listen(1)
        received: list[bytes] = []

        def serve() -> None:
            conn, _ = listener.accept()
            with conn:
                data = bytearray()
                while b"\n" not in data:
                    chunk = conn.recv(4096)
                    if not chunk:
                        break
                    data.extend(chunk)
                received.append(bytes(data))
                conn.sendall(response.encode("ascii") + b"\n")

        thread = threading.Thread(target=serve, daemon=True)
        thread.start()
        result = subprocess.run(
            [str(binary), "--sock", str(socketPath), *args],
            check=False, capture_output=True, text=True, timeout=5,
        )
        thread.join(timeout=5)
        listener.close()
        if thread.is_alive():
            raise AssertionError("local control socket stub did not finish")
        if len(received) != 1:
            raise AssertionError(f"expected one request, got {len(received)}")
        return received[0], result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    args = parser.parse_args()
    fp = "a" * 64
    common = ["revoke-cert", "--op", "tester", "--node", "42",
              "--serial", "ABCDEF", "--fingerprint", fp,
              "--reason", "keyCompromise"]

    request, result = roundTrip(args.binary, common, "OK confirm=98123")
    expected = (f"REVOKE_CERT op=tester node=42 serial=ABCDEF fingerprint={fp} "
                "reason=keyCompromise\n").encode("ascii")
    assert request == expected, request
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "OK confirm=98123"

    request, result = roundTrip(args.binary, common + ["--confirm", "98123"],
                                "OK recorded=1 tls_effective=0")
    assert request == expected[:-1] + b" confirm=98123\n", request
    assert result.returncode == 0, result.stderr
    assert result.stdout.strip() == "OK recorded=1 tls_effective=0"

    invalid = subprocess.run(
        [str(args.binary), "--sock", "/unused", *common,
         "--op", "bad operator"],
        check=False, capture_output=True, text=True, timeout=5,
    )
    assert invalid.returncode == 2
    assert "separator" in invalid.stderr
    print("revoke-cert CLI request framing and confirmation retry passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
