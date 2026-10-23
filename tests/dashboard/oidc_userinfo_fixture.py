#!/usr/bin/env python3
"""HTTPS OIDC UserInfo fixture for the dashboard session integration test."""
from __future__ import annotations

import http.server
import json
import pathlib
import ssl
import sys


state_file = pathlib.Path(sys.argv[3])


class Handler(http.server.BaseHTTPRequestHandler):
    def respond(self, status: int, value: dict[str, object]) -> None:
        body = json.dumps(value).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/.well-known/openid-configuration":
            origin = f"https://127.0.0.1:{self.server.server_port}"
            self.respond(200, {
                "issuer": origin,
                "authorization_endpoint": origin + "/authorize",
                "token_endpoint": origin + "/token",
                "jwks_uri": origin + "/jwks",
                "userinfo_endpoint": origin + "/userinfo",
            })
            return
        if self.path != "/userinfo":
            self.respond(404, {"error": "not_found"})
            return
        if self.headers.get("Authorization") not in {
                "Bearer access-current", "Bearer access-refreshed"}:
            self.respond(401, {"error": "invalid_token"})
            return
        state = json.loads(state_file.read_text(encoding="utf-8"))
        if state.get("oversized"):
            self.respond(200, {"sub": "subject-1", "groups": ["/domain admins"],
                               "padding": "x" * (1024 * 1024)})
            return
        self.respond(int(state.get("status", 200)), {
            "sub": state.get("sub", "subject-1"),
            "preferred_username": "alice",
            "name": "Alice Example",
            "groups": state.get("groups", []),
        })

    def do_POST(self) -> None:
        if self.path != "/token":
            self.respond(404, {"error": "not_found"})
            return
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        self.respond(200, {
            "access_token": "access-refreshed",
            "token_type": "Bearer",
            "refresh_token": "refresh-rotated",
            "expires_in": 300,
        })

    def log_message(self, _format: str, *_args: object) -> None:
        return


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(sys.argv[1], sys.argv[2])
server.socket = context.wrap_socket(server.socket, server_side=True)
print(server.server_port, flush=True)
server.serve_forever()
