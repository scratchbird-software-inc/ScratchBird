# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from scratchbird_ai.structured_logging import StructuredEventLogger


class StructuredLoggingTests(unittest.TestCase):
    def test_emit_and_summarize_events(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            log_path = Path(tmp_dir) / "structured-events.jsonl"
            logger = StructuredEventLogger(path=str(log_path))

            logger.emit(
                event_type="tool_invocation",
                status="success",
                trace_id="tr_1",
                request_id="req_1",
                interface_profile_id="mcp_local_v0",
                security_context={"tenant_id": "tenant_a", "actor_id": "actor_a"},
                duration_ms=12.5,
                attributes={"tool_name": "execute_readonly_query"},
            )
            logger.emit(
                event_type="query_execution",
                status="denied",
                trace_id="tr_2",
                request_id="req_2",
                security_context={"tenant_id": "tenant_a", "actor_id": "actor_a"},
                duration_ms=9.0,
                attributes={
                    "error_code": "E_POLICY_DENY",
                    "policy_rule_id": "RULE-1",
                    "message": "denied",
                },
            )

            rows = logger.load_events()
            self.assertEqual(len(rows), 2)

            summary = logger.summarize(max_recent_errors=5)
            self.assertEqual(summary["total_events"], 2)
            self.assertEqual(summary["status_counts"]["success"], 1)
            self.assertEqual(summary["status_counts"]["denied"], 1)
            self.assertEqual(summary["top_error_codes"]["E_POLICY_DENY"], 1)
            self.assertEqual(summary["top_policy_rule_ids"]["RULE-1"], 1)
            self.assertEqual(summary["recent_errors"][0]["trace_id"], "tr_2")


if __name__ == "__main__":
    unittest.main()
