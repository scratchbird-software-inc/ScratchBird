#!/usr/bin/env python3
# Copyright (c) 2026 ScratchBird Software Inc.
# SPDX-License-Identifier: MPL-2.0

"""Verify that the lab browser exposes WebDriver virtual authenticators."""

from __future__ import annotations

import json
import sys
import urllib.error
import urllib.request


def request_json(method: str, url: str, body: dict[str, object] | None = None) -> dict:
    data = None if body is None else json.dumps(body).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=data,
        method=method,
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        payload = response.read()
    return json.loads(payload) if payload else {}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: browser_webauthn_smoke.py SELENIUM_URL", file=sys.stderr)
        return 2

    base_url = sys.argv[1].rstrip("/")
    session_id = ""
    authenticator_id = ""
    try:
        response = request_json(
            "POST",
            f"{base_url}/session",
            {
                "capabilities": {
                    "alwaysMatch": {
                        "browserName": "chrome",
                        "goog:chromeOptions": {"args": ["--headless=new"]},
                    }
                }
            },
        )
        value = response.get("value", {})
        session_id = str(value.get("sessionId") or response.get("sessionId") or "")
        if not session_id:
            raise RuntimeError(f"WebDriver did not return a session id: {response}")

        response = request_json(
            "POST",
            f"{base_url}/session/{session_id}/webauthn/authenticator",
            {
                "protocol": "ctap2",
                "transport": "usb",
                "hasResidentKey": True,
                "hasUserVerification": True,
                "isUserConsenting": True,
                "isUserVerified": True,
            },
        )
        value = response.get("value", "")
        if isinstance(value, str):
            authenticator_id = value
        elif isinstance(value, dict):
            authenticator_id = str(value.get("authenticatorId", ""))
        if not authenticator_id:
            raise RuntimeError(f"WebDriver did not create an authenticator: {response}")
        print("authentication_lab_webauthn_virtual_authenticator=passed")
        return 0
    except (urllib.error.URLError, json.JSONDecodeError, RuntimeError) as error:
        print(f"authentication_lab_webauthn_error={error}", file=sys.stderr)
        return 1
    finally:
        if session_id and authenticator_id:
            try:
                request_json(
                    "DELETE",
                    f"{base_url}/session/{session_id}/webauthn/authenticator/{authenticator_id}",
                )
            except urllib.error.URLError:
                pass
        if session_id:
            try:
                request_json("DELETE", f"{base_url}/session/{session_id}")
            except urllib.error.URLError:
                pass


if __name__ == "__main__":
    raise SystemExit(main())
