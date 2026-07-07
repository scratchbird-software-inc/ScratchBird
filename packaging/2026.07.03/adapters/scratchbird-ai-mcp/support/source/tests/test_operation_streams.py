# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import unittest

from scratchbird_ai.operation_streams import LongRunningOperationManager
from scratchbird_ai.tool_schema import ToolContractError


class LongRunningOperationManagerTests(unittest.TestCase):
    def setUp(self) -> None:
        self.manager = LongRunningOperationManager()
        self.security_context = {
            "tenant_id": "tenant_a",
            "actor_id": "actor_a",
            "roles": ["analyst"],
            "session_id": "sess_1",
            "context_version": 1,
        }

    def test_operation_lifecycle_emits_deterministic_event_order(self) -> None:
        operation = self.manager.create_operation(
            session_id="sess_1",
            request_id="req_stream_1",
            method="execute_readonly_query",
            trace_id="tr_stream_1",
            security_context=self.security_context,
        )
        self.manager.mark_running(operation.operation_id)
        self.manager.complete(operation.operation_id, payload={"row_count": 1})

        events = self.manager.get_events(
            operation_id=operation.operation_id,
            requested_by=self.security_context,
        )
        self.assertEqual(events["operation_state"], "completed")
        self.assertEqual(
            [event["event_type"] for event in events["events"]],
            ["accepted", "progress", "completed"],
        )
        self.assertEqual(
            [event["sequence_no"] for event in events["events"]],
            [1, 2, 3],
        )

    def test_cancel_running_operation_with_same_actor(self) -> None:
        operation = self.manager.create_operation(
            session_id="sess_1",
            request_id="req_stream_cancel",
            method="execute_readonly_query",
            trace_id="tr_stream_cancel",
            security_context=self.security_context,
        )
        self.manager.mark_running(operation.operation_id)

        response = self.manager.cancel(
            operation_id=operation.operation_id,
            request_id="req_cancel",
            reason="user_cancelled",
            requested_by=self.security_context,
        )
        self.assertEqual(response["status"], "accepted")
        self.assertEqual(response["operation_state"], "cancelled")

    def test_cancel_running_operation_rejects_unauthorized_actor(self) -> None:
        operation = self.manager.create_operation(
            session_id="sess_1",
            request_id="req_stream_unauth",
            method="execute_readonly_query",
            trace_id="tr_stream_unauth",
            security_context=self.security_context,
        )
        self.manager.mark_running(operation.operation_id)

        response = self.manager.cancel(
            operation_id=operation.operation_id,
            request_id="req_cancel_unauth",
            reason="user_cancelled",
            requested_by={
                "tenant_id": "tenant_a",
                "actor_id": "actor_b",
                "roles": ["analyst"],
            },
        )
        self.assertEqual(response["status"], "unauthorized_cancellation")
        self.assertEqual(response["operation_state"], "running")

    def test_invalid_continuation_token_fails_closed(self) -> None:
        operation = self.manager.create_operation(
            session_id="sess_1",
            request_id="req_stream_cont",
            method="execute_readonly_query",
            trace_id="tr_stream_cont",
            security_context=self.security_context,
        )

        with self.assertRaises(ToolContractError) as ctx:
            self.manager.get_events(
                operation_id=operation.operation_id,
                requested_by=self.security_context,
                continuation_token="cont:wrong:2",
            )
        self.assertEqual(ctx.exception.error_code, "E_CONTINUATION_INVALID")


if __name__ == "__main__":
    unittest.main()
