# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Canonical long-running operation and streaming event support."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from .deterministic import deterministic_id
from .tool_schema import ToolContractError, require_security_context


LONG_RUNNING_INTERFACE_PROFILE_ID = "streaming_async_v0"
TERMINAL_OPERATION_STATES = {"completed", "failed", "cancelled", "expired"}
_PARTIAL_RESULT_MODES = {"append", "replace", "final"}
_VALID_STATE_TRANSITIONS = {
    "accepted": {"accepted", "running", "failed", "cancel_requested", "cancelled", "expired"},
    "running": {
        "running",
        "partially_completed",
        "completed",
        "failed",
        "cancel_requested",
        "cancelled",
        "expired",
    },
    "partially_completed": {
        "partially_completed",
        "completed",
        "failed",
        "cancel_requested",
        "cancelled",
        "expired",
    },
    "cancel_requested": {"cancel_requested", "cancelled", "failed", "expired"},
    "completed": {"completed"},
    "failed": {"failed"},
    "cancelled": {"cancelled"},
    "expired": {"expired"},
}


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _iso_utc(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


def _continuation_token(operation_id: str, next_sequence_no: int) -> str:
    return f"cont:{operation_id}:{max(1, int(next_sequence_no))}"


@dataclass(slots=True)
class OperationEvent:
    operation_id: str
    request_id: str
    trace_id: str
    sequence_no: int
    event_type: str
    operation_state: str
    timestamp_utc: str
    payload: Any

    def to_dict(self) -> dict[str, Any]:
        return {
            "operation_id": self.operation_id,
            "request_id": self.request_id,
            "trace_id": self.trace_id,
            "sequence_no": self.sequence_no,
            "event_type": self.event_type,
            "operation_state": self.operation_state,
            "timestamp_utc": self.timestamp_utc,
            "payload": self.payload,
        }


@dataclass(slots=True)
class LongRunningOperation:
    operation_id: str
    session_id: str
    request_id: str
    interface_profile_id: str
    method: str
    trace_id: str
    security_context: dict[str, Any]
    operation_state: str
    stream_channel: str
    resumable: bool
    continuation_token: str
    created_at: str
    updated_at: str
    cancellation_token: str | None = None
    events: list[OperationEvent] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return {
            "operation_id": self.operation_id,
            "session_id": self.session_id,
            "request_id": self.request_id,
            "interface_profile_id": self.interface_profile_id,
            "method": self.method,
            "trace_id": self.trace_id,
            "security_context": dict(self.security_context),
            "operation_state": self.operation_state,
            "stream_channel": self.stream_channel,
            "resumable": self.resumable,
            "continuation_token": self.continuation_token,
            "created_at": self.created_at,
            "updated_at": self.updated_at,
            "cancellation_token": self.cancellation_token,
            "events": [event.to_dict() for event in self.events],
        }


class LongRunningOperationManager:
    def __init__(self) -> None:
        self._operations: dict[str, LongRunningOperation] = {}

    def create_operation(
        self,
        *,
        session_id: str,
        request_id: str,
        method: str,
        trace_id: str,
        security_context: dict[str, Any],
        interface_profile_id: str = LONG_RUNNING_INTERFACE_PROFILE_ID,
        cancellation_token: str | None = None,
        resumable: bool = True,
        now_utc: datetime | None = None,
    ) -> LongRunningOperation:
        timestamp = now_utc or _utc_now()
        operation_id = deterministic_id(
            "op",
            {
                "session_id": session_id,
                "request_id": request_id,
                "method": method,
                "trace_id": trace_id,
                "interface_profile_id": interface_profile_id,
            },
        )
        operation = LongRunningOperation(
            operation_id=operation_id,
            session_id=session_id,
            request_id=request_id,
            interface_profile_id=interface_profile_id,
            method=method,
            trace_id=trace_id,
            security_context=require_security_context({"security_context": security_context}),
            operation_state="accepted",
            stream_channel=f"stream:{operation_id}",
            resumable=bool(resumable),
            continuation_token=_continuation_token(operation_id, 1),
            created_at=_iso_utc(timestamp),
            updated_at=_iso_utc(timestamp),
            cancellation_token=(str(cancellation_token).strip() or None) if cancellation_token else None,
        )
        self._operations[operation_id] = operation
        self._append_event(
            operation,
            event_type="accepted",
            operation_state="accepted",
            payload={"method": method},
            now_utc=timestamp,
        )
        return operation

    def mark_running(
        self,
        operation_id: str,
        *,
        payload: dict[str, Any] | None = None,
        now_utc: datetime | None = None,
    ) -> LongRunningOperation:
        operation = self._require_operation(operation_id)
        self._append_event(
            operation,
            event_type="progress",
            operation_state="running",
            payload=payload or {"phase": "running"},
            now_utc=now_utc,
        )
        return operation

    def record_notice(
        self,
        operation_id: str,
        *,
        notice: str,
        now_utc: datetime | None = None,
    ) -> LongRunningOperation:
        operation = self._require_operation(operation_id)
        self._append_event(
            operation,
            event_type="notice",
            operation_state=operation.operation_state,
            payload={"message": str(notice)},
            now_utc=now_utc,
        )
        return operation

    def record_partial_result(
        self,
        operation_id: str,
        *,
        partial_result_mode: str,
        payload: Any,
        now_utc: datetime | None = None,
    ) -> LongRunningOperation:
        normalized_mode = str(partial_result_mode).strip() or "append"
        if normalized_mode not in _PARTIAL_RESULT_MODES:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unsupported partial result mode: {partial_result_mode}",
                policy_rule_id="STREAM-RESULT-001",
            )
        operation = self._require_operation(operation_id)
        next_state = "completed" if normalized_mode == "final" else "partially_completed"
        self._append_event(
            operation,
            event_type="partial_result",
            operation_state=next_state,
            payload={"partial_result_mode": normalized_mode, "result": payload},
            now_utc=now_utc,
        )
        return operation

    def complete(
        self,
        operation_id: str,
        *,
        payload: Any,
        now_utc: datetime | None = None,
    ) -> LongRunningOperation:
        operation = self._require_operation(operation_id)
        self._append_event(
            operation,
            event_type="completed",
            operation_state="completed",
            payload=payload,
            now_utc=now_utc,
        )
        return operation

    def fail(
        self,
        operation_id: str,
        *,
        payload: Any,
        now_utc: datetime | None = None,
    ) -> LongRunningOperation:
        operation = self._require_operation(operation_id)
        self._append_event(
            operation,
            event_type="failed",
            operation_state="failed",
            payload=payload,
            now_utc=now_utc,
        )
        return operation

    def cancel(
        self,
        *,
        operation_id: str,
        request_id: str,
        reason: str,
        requested_by: dict[str, Any],
        now_utc: datetime | None = None,
    ) -> dict[str, Any]:
        operation = self._operations.get(operation_id)
        if operation is None:
            return {
                "operation_id": operation_id,
                "request_id": request_id,
                "status": "unknown_operation",
                "operation_state": "unknown",
                "trace_id": deterministic_id(
                    "tr",
                    {"operation_id": operation_id, "request_id": request_id, "cancel_unknown": True},
                ),
                "continuation_token": None,
            }

        if not self._is_authorized(operation, requested_by):
            return {
                "operation_id": operation.operation_id,
                "request_id": request_id,
                "status": "unauthorized_cancellation",
                "operation_state": operation.operation_state,
                "trace_id": operation.trace_id,
                "continuation_token": operation.continuation_token,
            }

        if operation.operation_state in TERMINAL_OPERATION_STATES:
            return {
                "operation_id": operation.operation_id,
                "request_id": request_id,
                "status": "already_terminal",
                "operation_state": operation.operation_state,
                "trace_id": operation.trace_id,
                "continuation_token": operation.continuation_token,
            }

        timestamp = now_utc or _utc_now()
        requested_by_context = require_security_context({"security_context": requested_by})
        self._append_event(
            operation,
            event_type="checkpoint",
            operation_state="cancel_requested",
            payload={
                "reason": str(reason).strip() or "cancel_requested",
                "requested_by": requested_by_context["actor_id"],
            },
            now_utc=timestamp,
        )
        self._append_event(
            operation,
            event_type="cancelled",
            operation_state="cancelled",
            payload={
                "reason": str(reason).strip() or "cancel_requested",
                "requested_by": requested_by_context["actor_id"],
            },
            now_utc=timestamp,
        )
        return {
            "operation_id": operation.operation_id,
            "request_id": request_id,
            "status": "accepted",
            "operation_state": operation.operation_state,
            "trace_id": operation.trace_id,
            "continuation_token": operation.continuation_token,
        }

    def get_events(
        self,
        *,
        operation_id: str,
        requested_by: dict[str, Any],
        continuation_token: str | None = None,
    ) -> dict[str, Any]:
        operation = self._require_operation(operation_id)
        if not self._is_authorized(operation, requested_by):
            raise ToolContractError(
                error_code="E_POLICY_DENY",
                message=f"unauthorized operation access: {operation_id}",
                policy_rule_id="STREAM-AUTH-001",
            )
        start_sequence_no = self._parse_continuation_token(
            operation_id=operation_id,
            continuation_token=continuation_token,
        )
        events = [
            event.to_dict() for event in operation.events if event.sequence_no >= start_sequence_no
        ]
        return {
            "operation_id": operation.operation_id,
            "session_id": operation.session_id,
            "request_id": operation.request_id,
            "trace_id": operation.trace_id,
            "stream_channel": operation.stream_channel,
            "operation_state": operation.operation_state,
            "resumable": operation.resumable,
            "continuation_token": operation.continuation_token,
            "terminal": operation.operation_state in TERMINAL_OPERATION_STATES,
            "events": events,
        }

    def _append_event(
        self,
        operation: LongRunningOperation,
        *,
        event_type: str,
        operation_state: str,
        payload: Any,
        now_utc: datetime | None = None,
    ) -> None:
        current_state = operation.operation_state
        allowed_states = _VALID_STATE_TRANSITIONS.get(current_state, {current_state})
        if operation_state not in allowed_states:
            raise ToolContractError(
                error_code="E_INVALID_OPERATION_STATE",
                message=(
                    f"invalid operation state transition: {current_state} -> {operation_state}"
                ),
                policy_rule_id="STREAM-STATE-001",
            )
        timestamp = now_utc or _utc_now()
        sequence_no = len(operation.events) + 1
        event = OperationEvent(
            operation_id=operation.operation_id,
            request_id=operation.request_id,
            trace_id=operation.trace_id,
            sequence_no=sequence_no,
            event_type=str(event_type),
            operation_state=operation_state,
            timestamp_utc=_iso_utc(timestamp),
            payload=payload,
        )
        operation.events.append(event)
        operation.operation_state = operation_state
        operation.updated_at = event.timestamp_utc
        operation.continuation_token = _continuation_token(operation.operation_id, sequence_no + 1)

    def _parse_continuation_token(
        self,
        *,
        operation_id: str,
        continuation_token: str | None,
    ) -> int:
        if continuation_token is None:
            return 1
        token = str(continuation_token).strip()
        if not token:
            return 1
        prefix = f"cont:{operation_id}:"
        if not token.startswith(prefix):
            raise ToolContractError(
                error_code="E_CONTINUATION_INVALID",
                message=f"invalid continuation token for operation: {operation_id}",
                policy_rule_id="STREAM-CONT-001",
            )
        try:
            sequence_no = int(token.removeprefix(prefix))
        except ValueError:
            raise ToolContractError(
                error_code="E_CONTINUATION_INVALID",
                message=f"invalid continuation token sequence for operation: {operation_id}",
                policy_rule_id="STREAM-CONT-002",
            ) from None
        return max(1, sequence_no)

    def _require_operation(self, operation_id: str) -> LongRunningOperation:
        operation = self._operations.get(operation_id)
        if operation is None:
            raise ToolContractError(
                error_code="E_CONTINUATION_INVALID",
                message=f"unknown operation: {operation_id}",
                policy_rule_id="STREAM-OP-001",
            )
        return operation

    def _is_authorized(
        self,
        operation: LongRunningOperation,
        requested_by: dict[str, Any],
    ) -> bool:
        requester = require_security_context({"security_context": requested_by})
        same_tenant = requester["tenant_id"] == operation.security_context.get("tenant_id")
        same_actor = requester["actor_id"] == operation.security_context.get("actor_id")
        is_admin = "admin" in requester.get("roles", [])
        return same_tenant and (same_actor or is_admin)
