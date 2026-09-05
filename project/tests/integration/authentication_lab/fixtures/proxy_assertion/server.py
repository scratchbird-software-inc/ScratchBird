# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

import base64
import hashlib
import hmac
import json
import os
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


SIGNING_KEY = os.environ.get(
    "SB_AUTH_LAB_PROXY_KEY", "scratchbird-proxy-test-key"
).encode("utf-8")


def encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def assertion(scenario: str) -> dict[str, object]:
    now = int(time.time())
    claims: dict[str, object] = {
        "iss": "proxy.scratchbird.test",
        "sub": "alice",
        "aud": "scratchbird",
        "iat": now,
        "exp": now + 300,
        "jti": "scratchbird-proxy-valid-1",
        "groups": ["database-users", "analytics"],
    }
    if scenario == "expired":
        claims["iat"] = now - 600
        claims["exp"] = now - 300
    elif scenario == "wrong_audience":
        claims["aud"] = "not-scratchbird"
    elif scenario == "replay":
        claims["jti"] = "scratchbird-proxy-replayed"

    payload = json.dumps(claims, separators=(",", ":"), sort_keys=True).encode()
    signature = hmac.new(SIGNING_KEY, payload, hashlib.sha256).digest()
    if scenario == "bad_signature":
        signature = b"invalid-signature"
    return {
        "algorithm": "HMAC-SHA256",
        "payload": encode(payload),
        "signature": encode(signature),
        "scenario": scenario,
    }


class Handler(BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/health":
            self.send_json({"status": "ready"})
            return
        if parsed.path != "/assertion":
            self.send_error(404)
            return
        scenario = urllib.parse.parse_qs(parsed.query).get("scenario", ["valid"])[0]
        allowed = {"valid", "expired", "wrong_audience", "bad_signature", "replay"}
        if scenario not in allowed:
            self.send_error(400, "unknown scenario")
            return
        self.send_json(assertion(scenario))

    def send_json(self, body: dict[str, object]) -> None:
        encoded = json.dumps(body, separators=(",", ":")).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def log_message(self, format_string: str, *args: object) -> None:
        print(format_string % args, flush=True)


ThreadingHTTPServer(("0.0.0.0", 8080), Handler).serve_forever()
