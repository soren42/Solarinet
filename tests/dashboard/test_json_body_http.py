#!/usr/bin/env python3
"""Exercise solari_json_body through PHP's HTTP SAPI and php://input."""
from __future__ import annotations

import http.client
import json
import pathlib
import socket
import subprocess
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOOTSTRAP = ROOT / "dashboard" / "api" / "lib" / "bootstrap.php"


def free_port() -> int:
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def php_router() -> str:
    return f'''<?php
require_once {json.dumps(str(BOOTSTRAP))};
if (($_SERVER['REQUEST_METHOD'] ?? '') === 'GET') {{ echo 'ready'; exit; }}
$body = solari_json_body(4096);
header('X-Test-Handler: reached');
echo json_encode(['ok' => true, 'data' => $body]);
?>'''


def request(port: int, content_type: str, body: bytes) -> tuple[int, str, str | None]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
    try:
        conn.request("POST", "/", body=body,
                     headers={"Content-Type": content_type,
                              "Content-Length": str(len(body))})
        response = conn.getresponse()
        return (response.status, response.read().decode("utf-8", "replace"),
                response.getheader("X-Test-Handler"))
    finally:
        conn.close()


def wait_ready(port: int, process: subprocess.Popen) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"PHP test server exited with status {process.returncode}")
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=0.3)
            conn.request("GET", "/")
            response = conn.getresponse()
            ready = response.status == 200 and response.read() == b"ready"
            conn.close()
            if ready:
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError("PHP test server did not become ready")


def main() -> int:
    port = free_port()
    with tempfile.TemporaryDirectory(prefix="solari-json-body-") as folder:
        router = pathlib.Path(folder) / "router.php"
        router.write_text(php_router(), encoding="utf-8")
        process = subprocess.Popen(
            ["php", "-S", f"127.0.0.1:{port}", "-t", folder, str(router)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        )
        try:
            wait_ready(port, process)
            payload = b'{"sample":true}'
            status, raw, reached = request(port, "application/json; charset=utf-8", payload)
            if status != 200 or json.loads(raw).get("data") != {"sample": True}:
                raise AssertionError(f"valid JSON request failed: HTTP {status}: {raw}")
            if reached != "reached":
                raise AssertionError("valid JSON request did not reach the handler marker")
            for content_type in ("application/jsonp", "text/plain; application/json"):
                status, raw, reached = request(port, content_type, payload)
                if status != 415:
                    raise AssertionError(
                        f"{content_type!r} was not rejected with HTTP 415: {status} {raw}"
                    )
                response = json.loads(raw)
                if response.get("error", {}).get("code") != "unsupported_media_type":
                    raise AssertionError(f"unexpected media-type error envelope: {raw}")
                if reached is not None:
                    raise AssertionError(f"{content_type!r} reached post-parse handler marker")
        finally:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
    print("JSON body HTTP-SAPI integration passed (valid JSON accepted; lookalikes rejected)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
