# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import io
import json
import unittest
from unittest.mock import patch

from scratchbird_ai.adapters.http import HttpAdapterError, HttpJsonClient


class _FakeResponse:
    def __init__(self, payload: dict) -> None:
        self._payload = payload

    def read(self) -> bytes:
        return json.dumps(self._payload).encode("utf-8")

    def __enter__(self) -> "_FakeResponse":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        return None


class HttpResilienceTests(unittest.TestCase):
    def test_retryable_compile_request_retries_once(self) -> None:
        attempts = {"count": 0}

        def fake_urlopen(req, timeout):  # type: ignore[no-untyped-def]
            _ = req, timeout
            attempts["count"] += 1
            if attempts["count"] == 1:
                raise OSError("temporary failure")
            return _FakeResponse({"statement_kind": "read", "sblr_hash": "abc", "diagnostics": [], "warnings": []})

        client = HttpJsonClient(
            base_url="http://127.0.0.1:3095",
            timeout_sec=1.0,
            retry_attempts=1,
            retry_backoff_ms=0,
        )

        with patch("scratchbird_ai.adapters.http.request.urlopen", side_effect=fake_urlopen):
            payload = client.request(
                method="POST",
                path="/v1/dialects/native/compile",
                payload={"query_text": "SELECT 1", "context": {}},
            )

        self.assertEqual(payload["sblr_hash"], "abc")
        self.assertEqual(attempts["count"], 2)

    def test_execute_request_does_not_retry(self) -> None:
        attempts = {"count": 0}

        def fake_urlopen(req, timeout):  # type: ignore[no-untyped-def]
            _ = req, timeout
            attempts["count"] += 1
            raise OSError("temporary failure")

        client = HttpJsonClient(
            base_url="http://127.0.0.1:3095",
            timeout_sec=1.0,
            retry_attempts=2,
            retry_backoff_ms=0,
        )

        with patch("scratchbird_ai.adapters.http.request.urlopen", side_effect=fake_urlopen):
            with self.assertRaises(HttpAdapterError):
                client.request(
                    method="POST",
                    path="/v1/dialects/native/execute",
                    payload={"compile_artifact_id": "cmp", "query_text": "SELECT 1", "options": {}},
                )

        self.assertEqual(attempts["count"], 1)

    def test_circuit_breaker_opens_after_repeated_failures(self) -> None:
        clock = {"value": 100.0}

        def fake_clock() -> float:
            return clock["value"]

        def fake_urlopen(req, timeout):  # type: ignore[no-untyped-def]
            _ = req, timeout
            raise OSError("temporary failure")

        client = HttpJsonClient(
            base_url="http://127.0.0.1:3095",
            timeout_sec=1.0,
            retry_attempts=0,
            circuit_breaker_failure_threshold=2,
            circuit_breaker_cooldown_sec=30.0,
            clock_fn=fake_clock,
        )

        with patch("scratchbird_ai.adapters.http.request.urlopen", side_effect=fake_urlopen):
            for _ in range(2):
                with self.assertRaises(HttpAdapterError):
                    client.request(method="GET", path="/v1/dialects/native/schemas")

            with self.assertRaises(HttpAdapterError) as ctx:
                client.request(method="GET", path="/v1/dialects/native/schemas")
            self.assertIn("circuit breaker", str(ctx.exception))

            clock["value"] += 31.0
            with self.assertRaises(HttpAdapterError):
                client.request(method="GET", path="/v1/dialects/native/schemas")


if __name__ == "__main__":
    unittest.main()
