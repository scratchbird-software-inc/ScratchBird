# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""P0 service layer for MCP tool orchestration."""

from __future__ import annotations

import time
from dataclasses import dataclass
from typing import Any

from .adapters.base import DialectAdapter
from .adapters.http import HttpJsonClient, make_http_dialect_adapter
from .adapters.mock import make_mock_dialect_adapter
from .approval_store import ApprovalLedger, ApprovalRecord
from .audit_bundle import (
    create_audit_bundle,
    create_bundle_attestation,
    replay_validate_bundle,
    security_context_hash,
    verify_bundle_attestation,
)
from .capability_matrix import load_capability_matrix
from .compile_repair import build_compile_repair_candidates
from .compatibility import build_compatibility_manifest, negotiate_compatibility
from .contracts import CompileResult, ExecuteResult, QueryResponse
from .deterministic import deterministic_id
from .environment_manifest import build_certification_manifest
from .execution_mode import ApprovalEvidence, validate_approval
from .interface_profiles import INTERFACE_COMPATIBILITY_VERSION, get_interface_profiles
from .operation_streams import LongRunningOperationManager
from .operational_controls import OperationalControlEngine
from .operator_bundle import build_runtime_diagnostics, generate_operator_runbook_bundle
from .plan_introspection import build_plan_response
from .policy import PolicyDeniedError, PolicyDecision, PolicyEngine
from .provider_profiles import get_provider_profile_descriptor, get_provider_profiles
from .registry import ServerRegistry, is_admin_context
from .remote_sessions import RemoteSessionManager
from .retrieval import InMemoryRetrievalStore
from .router import DialectRouter
from .scratchbird_core_surface import (
    ENGINE_MANAGED_CONTRACT_SCAFFOLD_BACKEND,
    build_scratchbird_core_surface_packet,
)
from .settings import RuntimeSettings, load_runtime_settings
from .structured_logging import StructuredEventLogger
from .tool_schema import (
    TOOL_DESCRIPTOR_VERSION,
    ToolContractError,
    TOOL_SCHEMA_VERSION,
    get_tool_descriptors,
    map_exception_to_error,
    normalize_tool_invocation,
    normalize_tool_response,
    require_security_context,
    validate_options,
)


@dataclass(slots=True)
class CompileRecord:
    dialect: str
    query_text: str
    statement_kind: str
    sblr_hash: str
    context: dict[str, Any]
    security_context: dict[str, Any]
    security_context_hash: str


class ScratchBirdAIService:
    """Core orchestration service used by MCP tool handlers."""

    def __init__(
        self,
        *,
        router: DialectRouter,
        policy_engine: PolicyEngine,
        adapters: dict[str, DialectAdapter],
        adapter_mode: str = "mock",
        retrieval_store: InMemoryRetrievalStore | None = None,
        managed_retrieval_store: InMemoryRetrievalStore | None = None,
        runtime_settings: RuntimeSettings | None = None,
        remote_session_manager: RemoteSessionManager | None = None,
        long_running_manager: LongRunningOperationManager | None = None,
        approval_ledger: ApprovalLedger | None = None,
        operational_controls: OperationalControlEngine | None = None,
        registry_manager: ServerRegistry | None = None,
        event_logger: StructuredEventLogger | None = None,
    ) -> None:
        self.router = router
        self.policy_engine = policy_engine
        self.adapters = adapters
        self.adapter_mode = adapter_mode
        self._compile_store: dict[str, CompileRecord] = {}
        self._execution_attempts: dict[str, int] = {}
        self._audit_store: list[dict[str, Any]] = []
        self._retrieval = retrieval_store or InMemoryRetrievalStore()
        self._managed_retrieval = managed_retrieval_store or InMemoryRetrievalStore(
            supported_profiles={"engine_managed_retrieval_v0"},
            backend_kind=ENGINE_MANAGED_CONTRACT_SCAFFOLD_BACKEND,
            allow_where_pushdown=True,
        )
        self._runtime_settings = runtime_settings or RuntimeSettings(adapter_mode=adapter_mode)
        self._remote_sessions = remote_session_manager or RemoteSessionManager(auth_token=None)
        self._long_running = long_running_manager or LongRunningOperationManager()
        self._approval_ledger = approval_ledger or ApprovalLedger(
            path=self._runtime_settings.approval_ledger_path
        )
        self._event_logger = event_logger or StructuredEventLogger(
            path=self._runtime_settings.structured_event_log_path
        )
        self._operational_controls = operational_controls or OperationalControlEngine(
            window_sec=self._runtime_settings.operation_window_sec,
            max_requests_per_window=self._runtime_settings.max_requests_per_window,
            max_mutations_per_window=self._runtime_settings.max_mutations_per_window,
            max_cost_units_per_window=self._runtime_settings.max_cost_units_per_window,
        )
        self._registry = registry_manager or ServerRegistry(
            server_instance_name=self._runtime_settings.server_instance_name,
            deployment_scope=self._runtime_settings.deployment_scope,
            adapter_mode=self._runtime_settings.normalized_mode(),
        )

    def get_capabilities(self) -> dict[str, Any]:
        interface_profiles = get_interface_profiles()
        scratchbird_core_surface = build_scratchbird_core_surface_packet()
        support_flags = self._support_flags(scratchbird_core_surface=scratchbird_core_surface)
        local_registry_entry = self._local_registry_entry(
            interface_profiles=interface_profiles,
            scratchbird_core_surface=scratchbird_core_surface,
            support_flags=support_flags,
        )
        registry_summary = self._registry.summary(local_entry=local_registry_entry)
        return {
            "service": "scratchbird-ai",
            "version": "0.1.0",
            "server_id": local_registry_entry.server_id,
            "capability_manifest_id": local_registry_entry.capability_manifest_id,
            "capability_manifest_version": local_registry_entry.capability_manifest_version,
            "publication_state": local_registry_entry.publication_state,
            "lifecycle_state": local_registry_entry.lifecycle_state,
            "health_state": local_registry_entry.health_state,
            "routing_target": local_registry_entry.routing_target,
            "tool_families": [dict(row) for row in local_registry_entry.tool_families],
            "registry_summary": registry_summary,
            "query_entrypoint_policy": "parser_compiler_first",
            "adapter_mode": self.adapter_mode,
            "tool_schema_version": TOOL_SCHEMA_VERSION,
            "tool_descriptor_version": TOOL_DESCRIPTOR_VERSION,
            "compatibility_version": INTERFACE_COMPATIBILITY_VERSION,
            "compatibility_manifest_version": INTERFACE_COMPATIBILITY_VERSION,
            "interface_profiles": interface_profiles,
            "provider_profiles": get_provider_profiles(),
            "scratchbird_core_surface_packet": scratchbird_core_surface,
            "supports": {
                "registry_governance": True,
                "gateway_routing": True,
                "remote_server_lifecycle": True,
                "metadata": True,
                "compile_execute_split": True,
                "read_only_mode": True,
                "mutation_requires_approval": True,
                "compatibility_negotiation": True,
                "retrieval_catalog": True,
                "provider_generated_embeddings": True,
                "engine_managed_retrieval": True,
                "engine_managed_retrieval_state": scratchbird_core_surface[
                    "engine_managed_retrieval_profile"
                ]["release_interpretation"],
                "engine_managed_retrieval_binding_state": scratchbird_core_surface[
                    "engine_managed_retrieval_profile"
                ]["binding_state"],
                "engine_managed_retrieval_live_validation_state": scratchbird_core_surface[
                    "verification_state"
                ]["ai_repo_live_validation_state"],
                "compile_repair": True,
                "durable_approval_evidence": True,
                "approval_record_workflow": True,
                "operational_controls": True,
                "cost_attribution": True,
                "graph_ops": True,
                "bridge_runtime": True,
                "server_policy_bound_authorization": True,
                "structured_runtime_logging": True,
                "operator_runbook_packaging": True,
                "audit_attestation": True,
                "audit_attestation_modes": [
                    "hmac_sha256",
                    "external_reference",
                    "third_party_hmac_sha256",
                ],
                "operator_target_profiles": list(self._runtime_settings.operator_target_profiles),
                "scratchbird_core_release_ceiling": scratchbird_core_surface[
                    "release_ceiling"
                ],
                "scratchbird_runtime_modes": [
                    row["mode_id"]
                    for row in scratchbird_core_surface["runtime_mode_truth_packet"][
                        "admitted_modes"
                    ]
                ],
                "structured_output_modes": ["none", "json_object", "json_schema"],
                "vector_search": True,
                "hybrid_search": True,
                "canonical_tools": [
                    descriptor["tool_name"] for descriptor in get_tool_descriptors()
                ],
                "canonical_execution_modes": [
                    "ai_analysis",
                    "ai_mutation_pending_approval",
                    "ai_mutation_approved",
                ],
                "security_context_fields": [
                    "tenant_id",
                    "actor_id",
                    "roles",
                    "groups",
                    "grants",
                    "session_id",
                    "context_version",
                ],
                "remote_transports": list(self._runtime_settings.remote_mcp_supported_transports),
                "remote_auth_types": list(self._runtime_settings.remote_mcp_supported_auth_types),
                "authorization_model": "server_policy_grants_groups",
                "legacy_mode_aliases": {
                    "read_only": "ai_analysis",
                    "mutation_with_approval": "ai_mutation_pending_approval",
                },
            },
            "matrix_version": self.router.matrix.get("version", "unknown"),
        }

    def get_tool_descriptors(self) -> dict[str, Any]:
        return {"tools": get_tool_descriptors()}

    def get_provider_profiles(self) -> dict[str, Any]:
        return {"profiles": get_provider_profiles()}

    def get_compatibility_manifest(self) -> dict[str, Any]:
        return build_compatibility_manifest(
            adapter_mode=self.adapter_mode,
            matrix_version=str(self.router.matrix.get("version", "unknown")),
            runtime_settings=self._runtime_settings,
        )

    def export_certification_manifest(self) -> dict[str, Any]:
        compatibility_manifest = self.get_compatibility_manifest()
        return build_certification_manifest(
            settings=self._runtime_settings,
            adapter_mode=self.adapter_mode,
            matrix_version=str(self.router.matrix.get("version", "unknown")),
            compatibility_manifest=compatibility_manifest,
        )

    def _find_retrieval_store(
        self,
        *,
        index_id: str,
        include_deleted: bool = True,
    ) -> InMemoryRetrievalStore | None:
        if self._managed_retrieval.owns_index(index_id, include_deleted=include_deleted):
            return self._managed_retrieval
        if self._retrieval.owns_index(index_id, include_deleted=include_deleted):
            return self._retrieval
        return None

    def create_vector_index(
        self,
        *,
        index_id: str,
        dimension: int,
        security_context: dict[str, Any],
        profile_id: str = "client_supplied_embeddings_v0",
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=True)
        if store is None:
            store = (
                self._managed_retrieval
                if profile_id == "engine_managed_retrieval_v0"
                else self._retrieval
            )
        return store.create_index(
            index_id=index_id,
            dimension=dimension,
            security_context=security_context,
            profile_id=profile_id,
        )

    def list_vector_indexes(
        self,
        *,
        security_context: dict[str, Any],
        include_deleted: bool = False,
    ) -> dict[str, Any]:
        helper = self._retrieval.list_indexes(
            security_context=security_context,
            include_deleted=include_deleted,
        )
        managed = self._managed_retrieval.list_indexes(
            security_context=security_context,
            include_deleted=include_deleted,
        )
        rows = list(helper["indexes"]) + list(managed["indexes"])
        rows.sort(key=lambda row: str(row["index_id"]))
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "list_vector_indexes",
                "index_ids": [row["index_id"] for row in rows],
                "include_deleted": include_deleted,
            },
        )
        return {"indexes": rows, "trace_id": trace_id}

    def describe_vector_index(
        self,
        *,
        index_id: str,
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=True) or self._retrieval
        return store.describe_index(
            index_id=index_id,
            security_context=security_context,
        )

    def negotiate_compatibility(self, request: dict[str, Any] | None = None) -> dict[str, Any]:
        return negotiate_compatibility(
            request,
            adapter_mode=self.adapter_mode,
            matrix_version=str(self.router.matrix.get("version", "unknown")),
            runtime_settings=self._runtime_settings,
        )

    def open_remote_session(self, request: dict[str, Any] | None = None) -> dict[str, Any]:
        return self._remote_sessions.open_session(
            request,
            capability_advertisement=self._remote_capabilities(),
        )

    def get_server_registry(
        self,
        *,
        security_context: dict[str, Any] | None = None,
        include_hidden: bool = False,
    ) -> dict[str, Any]:
        local_entry = self._local_registry_entry()
        entries = self._registry.list_entries(
            local_entry=local_entry,
            security_context=security_context,
            include_hidden=include_hidden,
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "get_server_registry",
                "include_hidden": include_hidden,
                "actor_id": (security_context or {}).get("actor_id"),
                "visible_server_ids": [row["server_id"] for row in entries],
            },
        )
        return {
            "trace_id": trace_id,
            "registry_schema_version": self._registry.schema_version,
            "local_server_id": self._registry.local_server_id,
            "registry_summary": self._registry.summary(local_entry=local_entry, visible_entries=entries),
            "entries": entries,
        }

    def register_remote_server(
        self,
        *,
        server_label: str,
        interface_profile_ids: list[str],
        tool_families: list[str],
        routing_target: str,
        security_context: dict[str, Any],
        publication_state: str = "published",
        metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        entry = self._registry.register_remote_server(
            server_label=server_label,
            interface_profile_ids=interface_profile_ids,
            tool_families=tool_families,
            routing_target=routing_target,
            security_context=security_context,
            publication_state=publication_state,
            metadata=metadata,
        )
        return {
            "trace_id": deterministic_id(
                "tr",
                {
                    "operation": "register_remote_server",
                    "server_id": entry["server_id"],
                    "actor_id": security_context.get("actor_id"),
                },
            ),
            "entry": entry,
        }

    def update_remote_server_lifecycle(
        self,
        *,
        server_id: str,
        action: str,
        security_context: dict[str, Any],
        reason: str | None = None,
    ) -> dict[str, Any]:
        entry = self._registry.update_remote_server_lifecycle(
            server_id=server_id,
            action=action,
            security_context=security_context,
            reason=reason,
        )
        return {
            "trace_id": deterministic_id(
                "tr",
                {
                    "operation": "update_remote_server_lifecycle",
                    "server_id": server_id,
                    "action": action,
                    "actor_id": security_context.get("actor_id"),
                },
            ),
            "entry": entry,
        }

    def report_remote_server_health(
        self,
        *,
        server_id: str,
        health_state: str,
        security_context: dict[str, Any],
        summary: str | None = None,
        metrics: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        entry = self._registry.report_remote_server_health(
            server_id=server_id,
            health_state=health_state,
            security_context=security_context,
            summary=summary,
            metrics=metrics,
        )
        return {
            "trace_id": deterministic_id(
                "tr",
                {
                    "operation": "report_remote_server_health",
                    "server_id": server_id,
                    "health_state": health_state,
                    "actor_id": security_context.get("actor_id"),
                },
            ),
            "entry": entry,
        }

    def resolve_gateway_route(
        self,
        *,
        tool_name: str,
        interface_profile_id: str = "service_internal_v0",
        security_context: dict[str, Any] | None = None,
        preferred_server_id: str | None = None,
    ) -> dict[str, Any]:
        route = self._registry.resolve_route(
            local_entry=self._local_registry_entry(),
            tool_name=tool_name,
            interface_profile_id=interface_profile_id,
            security_context=security_context,
            preferred_server_id=preferred_server_id,
        )
        route["trace_id"] = deterministic_id(
            "tr",
            {
                "operation": "resolve_gateway_route",
                "tool_name": tool_name,
                "interface_profile_id": interface_profile_id,
                "preferred_server_id": preferred_server_id or "",
                "actor_id": (security_context or {}).get("actor_id"),
                "status": route["status"],
            },
        )
        return route

    def invoke_remote_tool(
        self,
        *,
        session_id: str,
        request_id: str,
        method: str,
        params: dict[str, Any] | None = None,
        client_operation_timeout_ms: int | None = None,
        stream_requested: bool = False,
        allow_background_execution: bool = False,
        cancellation_token: str | None = None,
    ) -> dict[str, Any]:
        try:
            session = self._remote_sessions.require_session(session_id)
            invocation_payload = self._build_remote_tool_payload(
                session=session,
                session_id=session_id,
                request_id=request_id,
                method=method,
                params=params,
                client_operation_timeout_ms=client_operation_timeout_ms,
            )
            if stream_requested:
                if session.negotiated_transport != "https_sse_server_stream":
                    raise ToolContractError(
                        error_code="E_STREAM_NOT_SUPPORTED",
                        message=(
                            "stream_requested requires negotiated transport "
                            "https_sse_server_stream"
                        ),
                        policy_rule_id="REMOTE-SESSION-STREAM-001",
                    )
                operation = self._long_running.create_operation(
                    session_id=session_id,
                    request_id=request_id,
                    method=method,
                    trace_id=deterministic_id(
                        "tr",
                        {
                            "session_id": session_id,
                            "request_id": request_id,
                            "method": method,
                            "stream_requested": True,
                        },
                    ),
                    security_context=session.security_context,
                    cancellation_token=cancellation_token,
                )
                self._long_running.mark_running(
                    operation.operation_id,
                    payload={
                        "method": method,
                        "allow_background_execution": bool(allow_background_execution),
                    },
                )
                envelope = self.invoke_tool(
                    payload=invocation_payload,
                    interface_profile_id="streaming_async_v0",
                )
                for notice in envelope["notices"]:
                    self._long_running.record_notice(
                        operation.operation_id,
                        notice=notice,
                    )
                if envelope["status"] == "success":
                    self._long_running.complete(
                        operation.operation_id,
                        payload={
                            "result": envelope["result"],
                            "structured_output": envelope["structured_output"],
                        },
                    )
                    operation_events = self._long_running.get_events(
                        operation_id=operation.operation_id,
                        requested_by=session.security_context,
                    )
                    return {
                        "session_id": session_id,
                        "request_id": request_id,
                        "status": "success",
                        "trace_id": operation.trace_id,
                        "result": None,
                        "error": None,
                        "operation_id": operation.operation_id,
                        "operation_state": operation_events["operation_state"],
                        "stream_channel": operation.stream_channel,
                        "resumable": operation.resumable,
                        "continuation_token": operation.continuation_token,
                        "notices": [],
                    }
                self._long_running.fail(
                    operation.operation_id,
                    payload={"error": envelope["error"]},
                )
                return {
                    "session_id": session_id,
                    "request_id": request_id,
                    "status": "error",
                    "trace_id": operation.trace_id,
                    "result": None,
                    "error": envelope["error"],
                    "operation_id": operation.operation_id,
                    "operation_state": "failed",
                    "stream_channel": operation.stream_channel,
                    "resumable": operation.resumable,
                    "continuation_token": operation.continuation_token,
                    "notices": [],
                }

            envelope = self.invoke_tool(
                payload=invocation_payload,
                interface_profile_id="mcp_remote_v0",
            )
            return {
                "session_id": session_id,
                "request_id": request_id,
                "status": envelope["status"],
                "trace_id": envelope["trace_id"],
                "result": envelope["result"],
                "error": envelope["error"],
                "operation_id": None,
                "operation_state": "completed" if envelope["status"] == "success" else "failed",
                "stream_channel": None,
                "resumable": False,
                "continuation_token": None,
                "notices": envelope["notices"],
            }
        except Exception as exc:
            error = map_exception_to_error(
                exc,
                trace_seed={
                    "session_id": session_id,
                    "request_id": request_id,
                    "method": method,
                    "remote_invocation_error": True,
                },
            )
            return {
                "session_id": session_id,
                "request_id": request_id,
                "status": "error",
                "trace_id": error["trace_id"],
                "result": None,
                "error": error,
                "operation_id": None,
                "operation_state": "failed",
                "stream_channel": None,
                "resumable": False,
                "continuation_token": None,
                "notices": [],
            }

    def close_remote_session(self, *, session_id: str, request_id: str | None = None) -> dict[str, Any]:
        return self._remote_sessions.close_session(session_id=session_id, request_id=request_id)

    def poll_remote_operation(
        self,
        *,
        session_id: str,
        operation_id: str,
        continuation_token: str | None = None,
    ) -> dict[str, Any]:
        try:
            session = self._remote_sessions.require_session(session_id)
            events = self._long_running.get_events(
                operation_id=operation_id,
                requested_by=session.security_context,
                continuation_token=continuation_token,
            )
            return {
                "session_id": session_id,
                "operation_id": operation_id,
                "request_id": events["request_id"],
                "trace_id": events["trace_id"],
                "operation_state": events["operation_state"],
                "stream_channel": events["stream_channel"],
                "resumable": events["resumable"],
                "continuation_token": events["continuation_token"],
                "terminal": events["terminal"],
                "events": events["events"],
                "error": None,
            }
        except Exception as exc:
            error = map_exception_to_error(
                exc,
                trace_seed={
                    "session_id": session_id,
                    "operation_id": operation_id,
                    "poll_remote_operation_error": True,
                },
            )
            return {
                "session_id": session_id,
                "operation_id": operation_id,
                "request_id": None,
                "trace_id": error["trace_id"],
                "operation_state": "failed",
                "stream_channel": None,
                "resumable": False,
                "continuation_token": continuation_token,
                "terminal": True,
                "events": [],
                "error": error,
            }

    def cancel_remote_operation(
        self,
        *,
        session_id: str,
        operation_id: str,
        request_id: str,
        reason: str,
    ) -> dict[str, Any]:
        try:
            session = self._remote_sessions.require_session(session_id)
        except Exception:
            return {
                "session_id": session_id,
                "operation_id": operation_id,
                "request_id": request_id,
                "status": "session_invalid",
                "operation_state": "unknown",
                "trace_id": deterministic_id(
                    "tr",
                    {
                        "session_id": session_id,
                        "operation_id": operation_id,
                        "request_id": request_id,
                        "cancel_session_invalid": True,
                    },
                ),
                "continuation_token": None,
            }
        cancellation = self._long_running.cancel(
            operation_id=operation_id,
            request_id=request_id,
            reason=reason,
            requested_by=session.security_context,
        )
        cancellation["session_id"] = session_id
        return cancellation

    def replay_audit_bundle(
        self,
        *,
        bundle: dict[str, Any],
        security_context: dict[str, Any] | None = None,
        expected_policy_decision: str | None = None,
        expected_policy_rule_id: str | None = None,
        expected_plan_hash: str | None = None,
    ) -> dict[str, Any]:
        if not isinstance(bundle, dict):
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="bundle must be an object",
                policy_rule_id="AUDIT-REPLAY-001",
            )
        normalized_security_context = (
            require_security_context({"security_context": security_context})
            if security_context is not None
            else None
        )
        replay_result = replay_validate_bundle(
            bundle=bundle,
            security_context=normalized_security_context,
            expected_policy_decision=expected_policy_decision,
            expected_policy_rule_id=expected_policy_rule_id,
            expected_plan_hash=expected_plan_hash,
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "replay_audit_bundle",
                "request_id": str(bundle.get("request_id", "")),
                "bundle_hash": str(bundle.get("bundle_hash", "")),
                "outcome": replay_result.outcome,
            },
        )
        return {
            "request_id": str(bundle.get("request_id", "")).strip() or None,
            "trace_id": trace_id,
            "bundle_hash": str(bundle.get("bundle_hash", "")).strip() or None,
            "outcome": replay_result.outcome,
            "reason": replay_result.reason,
            "matches": replay_result.outcome == "REPLAY_MATCH",
        }

    def validate_approval_evidence(
        self,
        *,
        approval_evidence: dict[str, Any],
        security_context: dict[str, Any],
        statement_hash: str,
    ) -> dict[str, Any]:
        if not isinstance(approval_evidence, dict):
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval_evidence must be an object",
                policy_rule_id="MODE-APPROVAL-001",
            )
        normalized_security_context = require_security_context(
            {"security_context": security_context}
        )
        normalized_statement_hash = str(statement_hash).strip()
        if not normalized_statement_hash:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="statement_hash is required",
                policy_rule_id="APPROVAL-VALIDATION-001",
            )
        approval_token = _resolve_approval_token(
            approval_token=None,
            approval_evidence=approval_evidence,
        )
        if not approval_token:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval_token is required for approval validation",
                policy_rule_id="APPROVAL-VALIDATION-002",
            )
        validated = validate_approval(
            approval=ApprovalEvidence(
                approval_token=approval_token,
                approval_id=str(approval_evidence.get("approval_id", "")).strip() or None,
                approved_by=str(approval_evidence.get("approved_by", "")).strip() or None,
                approved_at=str(approval_evidence.get("approved_at", "")).strip() or None,
            ),
            tenant_id=normalized_security_context.get("tenant_id"),
            actor_id=normalized_security_context.get("actor_id"),
            statement_hash=normalized_statement_hash,
        )
        record = self._approval_ledger.validate_or_register(
            approval_token=validated.approval_token or "",
            approval_evidence={
                "approval_id": validated.approval_id,
                "approved_by": validated.approved_by,
                "approved_at": validated.approved_at,
            },
            tenant_id=normalized_security_context.get("tenant_id", ""),
            actor_id=normalized_security_context.get("actor_id", ""),
            statement_hash=normalized_statement_hash,
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "validate_approval_evidence",
                "approval_id": record.approval_id,
                "tenant_id": record.tenant_id,
                "actor_id": record.actor_id,
                "statement_hash": record.statement_hash,
            },
        )
        return {
            "trace_id": trace_id,
            "validation_status": "valid",
            "approval_id": record.approval_id,
            "approval_token_hash": record.approval_token_hash,
            "tenant_id": record.tenant_id,
            "actor_id": record.actor_id,
            "statement_hash": record.statement_hash,
            "approved_by": record.approved_by,
            "approved_at": record.approved_at,
            "expires_at": record.expires_at,
            "revoked_at": record.revoked_at,
            "last_used_at": record.last_used_at,
            "use_count": record.use_count,
        }

    def list_audit_bundles(
        self,
        *,
        security_context: dict[str, Any],
        limit: int = 100,
    ) -> dict[str, Any]:
        normalized_security_context = require_security_context(
            {"security_context": security_context}
        )
        bounded = max(1, min(int(limit or 100), 1000))
        rows = list(self._audit_store[-bounded:])
        if not is_admin_context(normalized_security_context):
            rows = [
                row
                for row in rows
                if row.get("tenant_id") == normalized_security_context.get("tenant_id")
                and row.get("actor_id") == normalized_security_context.get("actor_id")
            ]
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "list_audit_bundles",
                "tenant_id": normalized_security_context.get("tenant_id"),
                "actor_id": normalized_security_context.get("actor_id"),
                "limit": bounded,
                "bundle_count": len(rows),
            },
        )
        self._emit_runtime_event(
            event_type="governance_audit_bundle_list",
            status="success",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "limit": bounded,
                "bundle_count": len(rows),
            },
        )
        return {
            "trace_id": trace_id,
            "limit": bounded,
            "bundle_count": len(rows),
            "bundles": rows,
        }

    def list_approval_records(
        self,
        *,
        security_context: dict[str, Any],
        tenant_id: str | None = None,
        actor_id: str | None = None,
        include_revoked: bool = True,
    ) -> dict[str, Any]:
        normalized_security_context = require_security_context(
            {"security_context": security_context}
        )
        requested_tenant_id = str(tenant_id or "").strip() or None
        requested_actor_id = str(actor_id or "").strip() or None
        if not is_admin_context(normalized_security_context):
            caller_tenant_id = normalized_security_context.get("tenant_id", "")
            caller_actor_id = normalized_security_context.get("actor_id", "")
            if requested_tenant_id is not None and requested_tenant_id != caller_tenant_id:
                raise ToolContractError(
                    error_code="E_POLICY_DENY",
                    message="approval record listing is restricted to the caller tenant",
                    policy_rule_id="APPROVAL-LIST-001",
                )
            if requested_actor_id is not None and requested_actor_id != caller_actor_id:
                raise ToolContractError(
                    error_code="E_POLICY_DENY",
                    message="approval record listing is restricted to the caller actor",
                    policy_rule_id="APPROVAL-LIST-002",
                )
            requested_tenant_id = caller_tenant_id
            requested_actor_id = caller_actor_id

        rows = self._approval_ledger.list_records(
            tenant_id=requested_tenant_id,
            actor_id=requested_actor_id,
            include_revoked=include_revoked,
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "list_approval_records",
                "tenant_id": requested_tenant_id or "",
                "actor_id": requested_actor_id or "",
                "include_revoked": include_revoked,
                "record_count": len(rows),
            },
        )
        self._emit_runtime_event(
            event_type="governance_approval_list",
            status="success",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "record_count": len(rows),
                "include_revoked": include_revoked,
            },
        )
        return {
            "trace_id": trace_id,
            "record_count": len(rows),
            "records": [record.to_dict() for record in rows],
            "summary": self._approval_ledger.summary(),
        }

    def revoke_approval_record(
        self,
        *,
        approval_id: str,
        reason: str,
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        normalized_security_context = self._require_admin_security_context(
            security_context=security_context,
            rule_id="APPROVAL-REVOKE-001",
            message="approval record revocation requires ScratchBird administrative authority",
        )
        approval_id_value = str(approval_id).strip()
        reason_value = str(reason).strip()
        if not approval_id_value or not reason_value:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="approval_id and reason are required",
                policy_rule_id="APPROVAL-REVOKE-002",
            )
        try:
            record = self._approval_ledger.revoke(
                approval_id_value,
                reason=reason_value,
                revoked_by=normalized_security_context.get("actor_id"),
            )
        except KeyError as exc:
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message=str(exc),
                policy_rule_id="APPROVAL-REVOKE-003",
            ) from None
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "revoke_approval_record",
                "approval_id": record.approval_id,
                "revoked_by": normalized_security_context.get("actor_id"),
            },
        )
        self._emit_runtime_event(
            event_type="governance_approval_revoke",
            status="success",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "approval_id": record.approval_id,
                "revoked_by": record.revoked_by,
                "policy_rule_id": "APPROVAL-REVOKE-001",
            },
        )
        return {
            "trace_id": trace_id,
            "record": record.to_dict(),
        }

    def create_audit_attestation(
        self,
        *,
        security_context: dict[str, Any],
        bundle: dict[str, Any] | None = None,
        attestation_mode: str | None = None,
        key_id: str | None = None,
        external_reference: str | None = None,
        metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        normalized_security_context = self._require_admin_security_context(
            security_context=security_context,
            rule_id="AUDIT-ATTEST-001",
            message="audit attestation issuance requires ScratchBird administrative authority",
        )
        selected_bundle = dict(bundle) if isinstance(bundle, dict) else self.latest_audit_bundle()
        if not isinstance(selected_bundle, dict):
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="bundle is required when no audit bundle has been emitted yet",
                policy_rule_id="AUDIT-ATTEST-002",
            )
        mode = str(attestation_mode or self._runtime_settings.audit_attestation_mode).strip() or (
            self._runtime_settings.audit_attestation_mode
        )
        attestor_id = self._runtime_settings.audit_attestation_attestor_id
        attestation_secret = self._runtime_settings.audit_attestation_secret
        effective_external_reference = external_reference
        if mode == "third_party_hmac_sha256":
            attestor_id = (
                self._runtime_settings.audit_attestation_delegated_attestor_id
                or attestor_id
            )
            attestation_secret = self._runtime_settings.audit_attestation_delegated_secret
            if (
                not effective_external_reference
                and self._runtime_settings.audit_attestation_external_reference_base_url
            ):
                base_url = self._runtime_settings.audit_attestation_external_reference_base_url.rstrip(
                    "/"
                )
                bundle_hash = str(selected_bundle.get("bundle_hash", "")).strip()
                if bundle_hash:
                    effective_external_reference = f"{base_url}/bundles/{bundle_hash}"
        try:
            attestation = create_bundle_attestation(
                bundle=selected_bundle,
                attestor_id=attestor_id,
                attestation_mode=mode,
                shared_secret=attestation_secret,
                key_id=key_id,
                external_reference=effective_external_reference,
                metadata=metadata,
            )
        except ValueError as exc:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=str(exc),
                policy_rule_id="AUDIT-ATTEST-003",
            ) from None
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "create_audit_attestation",
                "bundle_hash": selected_bundle.get("bundle_hash"),
                "attestation_mode": mode,
            },
        )
        self._emit_runtime_event(
            event_type="governance_audit_attestation_create",
            status="success",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "attestation_mode": mode,
                "bundle_hash": selected_bundle.get("bundle_hash"),
            },
        )
        return {
            "trace_id": trace_id,
            "bundle_hash": selected_bundle.get("bundle_hash"),
            "attestation": attestation,
        }

    def verify_audit_attestation(
        self,
        *,
        security_context: dict[str, Any],
        bundle: dict[str, Any],
        attestation: dict[str, Any],
        shared_secret: str | None = None,
    ) -> dict[str, Any]:
        normalized_security_context = require_security_context(
            {"security_context": security_context}
        )
        if not isinstance(bundle, dict) or not isinstance(attestation, dict):
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="bundle and attestation must both be objects",
                policy_rule_id="AUDIT-VERIFY-001",
            )
        attestation_mode_value = str(attestation.get("attestation_mode", "")).strip()
        default_secret = (
            self._runtime_settings.audit_attestation_delegated_secret
            if attestation_mode_value == "third_party_hmac_sha256"
            else self._runtime_settings.audit_attestation_secret
        )
        verify_result = verify_bundle_attestation(
            bundle=bundle,
            attestation=attestation,
            shared_secret=shared_secret or default_secret,
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "verify_audit_attestation",
                "bundle_hash": bundle.get("bundle_hash"),
                "outcome": verify_result.outcome,
            },
        )
        self._emit_runtime_event(
            event_type="governance_audit_attestation_verify",
            status="success" if verify_result.verified else "denied",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "bundle_hash": bundle.get("bundle_hash"),
                "outcome": verify_result.outcome,
            },
        )
        return {
            "trace_id": trace_id,
            "bundle_hash": bundle.get("bundle_hash"),
            "outcome": verify_result.outcome,
            "reason": verify_result.reason,
            "verified": verify_result.verified,
        }

    def get_runtime_diagnostics(
        self,
        *,
        security_context: dict[str, Any],
        max_recent_errors: int = 10,
    ) -> dict[str, Any]:
        normalized_security_context = self._require_admin_security_context(
            security_context=security_context,
            rule_id="RUNTIME-DIAGNOSTICS-001",
            message="runtime diagnostics require ScratchBird administrative authority",
        )
        diagnostics = build_runtime_diagnostics(
            settings=self._runtime_settings,
            certification_manifest=self.export_certification_manifest(),
            event_logger=self._event_logger,
            approval_ledger=self._approval_ledger,
            max_recent_errors=max(1, min(int(max_recent_errors or 10), 100)),
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "get_runtime_diagnostics",
                "event_count": diagnostics["event_summary"]["total_events"],
            },
        )
        self._emit_runtime_event(
            event_type="runtime_diagnostics",
            status="success",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "event_count": diagnostics["event_summary"]["total_events"],
                "approval_record_count": diagnostics["approval_summary"]["total_records"],
            },
        )
        diagnostics["trace_id"] = trace_id
        return diagnostics

    def generate_operator_runbook_bundle(
        self,
        *,
        security_context: dict[str, Any],
        output_dir: str | None = None,
        max_recent_errors: int = 10,
        target_profiles: list[str] | None = None,
    ) -> dict[str, Any]:
        normalized_security_context = self._require_admin_security_context(
            security_context=security_context,
            rule_id="RUNTIME-RUNBOOK-001",
            message="operator runbook bundle generation requires ScratchBird administrative authority",
        )
        bundle_payload = generate_operator_runbook_bundle(
            output_dir=output_dir or self._runtime_settings.operator_bundle_output_dir or "",
            settings=self._runtime_settings,
            certification_manifest=self.export_certification_manifest(),
            event_logger=self._event_logger,
            approval_ledger=self._approval_ledger,
            max_recent_errors=max(1, min(int(max_recent_errors or 10), 100)),
            target_profiles=target_profiles,
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "generate_operator_runbook_bundle",
                "output_dir": bundle_payload["output_dir"],
                "status": bundle_payload["status"],
                "target_profiles": (
                    list(target_profiles)
                    if isinstance(target_profiles, list)
                    else list(self._runtime_settings.operator_target_profiles)
                ),
            },
        )
        self._emit_runtime_event(
            event_type="runtime_runbook_bundle",
            status="success",
            trace_id=trace_id,
            security_context=normalized_security_context,
            attributes={
                "output_dir": bundle_payload["output_dir"],
                "status": bundle_payload["status"],
                "target_profile_count": len(bundle_payload.get("packages", {})),
            },
        )
        bundle_payload["trace_id"] = trace_id
        return bundle_payload

    def _build_remote_tool_payload(
        self,
        *,
        session: Any,
        session_id: str,
        request_id: str,
        method: str,
        params: dict[str, Any] | None,
        client_operation_timeout_ms: int | None,
    ) -> dict[str, Any]:
        tool_arguments = dict(params or {})
        for reserved_key in (
            "call_id",
            "client_capabilities",
            "interface_profile_id",
            "method",
            "request_id",
            "requested_transport",
            "session_id",
        ):
            tool_arguments.pop(reserved_key, None)
        session_security_context = dict(session.security_context)
        tool_arguments["security_context"] = session_security_context
        raw_context = tool_arguments.get("context", {})
        context = dict(raw_context) if isinstance(raw_context, dict) else {}
        context["security_context"] = dict(session_security_context)
        tool_arguments["context"] = context

        session_capabilities = dict(session.client_capabilities)
        session_capabilities.update(
            {
                "interface_profile_id": session.interface_profile_id,
                "requested_profile_version": session.negotiated_protocol_version,
                "requested_transport": session.negotiated_transport,
            }
        )
        if client_operation_timeout_ms is not None:
            options = tool_arguments.get("options", {})
            if not isinstance(options, dict):
                options = {}
            options.setdefault("timeout_ms", int(client_operation_timeout_ms))
            tool_arguments["options"] = options
        return {
            "request_id": request_id,
            "call_id": deterministic_id(
                "call",
                {"session_id": session_id, "request_id": request_id, "method": method},
            ),
            "tool_name": method,
            "arguments": tool_arguments,
            "client_capabilities": session_capabilities,
            "security_context": session_security_context,
        }

    def invoke_tool(
        self,
        *,
        payload: dict[str, Any],
        interface_profile_id: str = "service_internal_v0",
        provider_profile_id: str | None = None,
    ) -> dict[str, Any]:
        normalized = normalize_tool_invocation(
            payload=payload,
            interface_profile_id=interface_profile_id,
            provider_profile_id=provider_profile_id,
        )
        return self._invoke_normalized_tool(normalized)

    def invoke_provider_tool(
        self,
        *,
        payload: dict[str, Any],
        provider_profile_id: str,
    ) -> dict[str, Any]:
        request_id = str(payload.get("request_id", "")).strip() or deterministic_id(
            "req",
            {
                "interface_profile_id": "provider_tool_calling_v0",
                "provider_profile_id": provider_profile_id,
                "payload": payload,
            },
        )
        try:
            descriptor = get_provider_profile_descriptor(provider_profile_id)
        except KeyError:
            error = map_exception_to_error(
                ToolContractError(
                    error_code="E_PROVIDER_CONTRACT_UNSUPPORTED",
                    message=f"unknown provider profile: {provider_profile_id}",
                    policy_rule_id="PROVIDER-PROFILE-001",
                ),
                trace_seed={
                    "request_id": request_id,
                    "provider_profile_id": provider_profile_id,
                    "provider_profile_error": True,
                },
            )
            return {
                "request_id": request_id,
                "interface_profile_id": "provider_tool_calling_v0",
                "provider_profile_id": provider_profile_id,
                "trace_id": error["trace_id"],
                "status": "error",
                "result": None,
                "structured_output": None,
                "error": error,
                "notices": [],
            }

        if descriptor.state != "implemented":
            error = map_exception_to_error(
                ToolContractError(
                    error_code="E_PROVIDER_CONTRACT_UNSUPPORTED",
                    message=f"provider profile is not implemented: {provider_profile_id}",
                    policy_rule_id="PROVIDER-PROFILE-002",
                ),
                trace_seed={
                    "request_id": request_id,
                    "provider_profile_id": provider_profile_id,
                    "provider_profile_error": True,
                },
            )
            return {
                "request_id": request_id,
                "interface_profile_id": "provider_tool_calling_v0",
                "provider_profile_id": provider_profile_id,
                "trace_id": error["trace_id"],
                "status": "error",
                "result": None,
                "structured_output": None,
                "error": error,
                "notices": [],
            }

        envelope = self.invoke_tool(
            payload=payload,
            interface_profile_id="provider_tool_calling_v0",
            provider_profile_id=provider_profile_id,
        )
        return {
            "request_id": envelope["request_id"],
            "interface_profile_id": envelope["interface_profile_id"],
            "provider_profile_id": provider_profile_id,
            "trace_id": envelope["trace_id"],
            "status": envelope["status"],
            "result": envelope["result"],
            "structured_output": envelope["structured_output"],
            "error": envelope["error"],
            "notices": envelope["notices"],
        }

    def latest_audit_bundle(self) -> dict[str, Any] | None:
        if not self._audit_store:
            return None
        return self._audit_store[-1]

    def list_dialects(self) -> list[str]:
        return self.router.available_dialects()

    def list_schemas(self, dialect: str, database: str | None = None) -> list[str]:
        self.router.require_capability(dialect, "metadata_introspection")
        return self.adapters[dialect].metadata.list_schemas(database)

    def list_tables(self, dialect: str, schema: str) -> list[str]:
        self.router.require_capability(dialect, "metadata_introspection")
        return self.adapters[dialect].metadata.list_tables(schema)

    def describe_table(self, dialect: str, schema: str, table: str) -> dict[str, Any]:
        self.router.require_capability(dialect, "metadata_introspection")
        return self.adapters[dialect].metadata.describe_table(schema, table)

    def compile_query(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any] | None = None,
    ) -> CompileResult:
        self.router.require_capability(dialect, "read_select")

        adapter = self.adapters[dialect]
        query_context = context or {}
        _validate_compatibility_context(
            self,
            query_context,
            default_profile_id="service_internal_v0",
            default_transport="in_process",
        )
        security_context = _extract_security_context(query_context)
        sec_hash = security_context_hash(security_context)
        compiled, compiled_query_text, compile_repair_warnings = self._compile_with_repair(
            adapter=adapter,
            query_text=query_text,
            query_context=query_context,
        )

        normalized_query = " ".join(compiled_query_text.split())
        compile_artifact_id = deterministic_id(
            "cmp",
            {
                "dialect": dialect,
                "normalized_query_text": normalized_query,
                "security_context_hash": sec_hash,
                "context": query_context,
            },
        )

        statement_kind = compiled.statement_kind
        if statement_kind not in {"read", "mutation", "unknown"}:
            statement_kind = "unknown"

        self._compile_store[compile_artifact_id] = CompileRecord(
            dialect=dialect,
            query_text=compiled_query_text,
            statement_kind=statement_kind,
            sblr_hash=compiled.sblr_hash,
            context=query_context,
            security_context=security_context,
            security_context_hash=sec_hash,
        )

        return CompileResult(
            compile_artifact_id=compile_artifact_id,
            dialect=dialect,
            statement_kind=statement_kind,
            sblr_hash=compiled.sblr_hash,
            diagnostics=compiled.diagnostics,
            warnings=compiled.warnings + compile_repair_warnings,
        )

    def execute_compiled(
        self,
        *,
        compile_artifact_id: str,
        options: dict[str, Any] | None = None,
        mode: str = "read_only",
        approval_token: str | None = None,
        prevalidated_decision: PolicyDecision | None = None,
    ) -> ExecuteResult:
        record = self._compile_store.get(compile_artifact_id)
        if record is None:
            raise KeyError(f"Unknown compile artifact: {compile_artifact_id}")

        is_mutation = record.statement_kind != "read"
        opts = validate_options(options)

        decision = prevalidated_decision
        if decision is None:
            decision = self.policy_engine.enforce(
                mode=mode,
                is_mutation=is_mutation,
                approval_token=approval_token,
                options=opts,
                tenant_id=record.security_context.get("tenant_id"),
                actor_id=record.security_context.get("actor_id"),
                statement_hash=record.sblr_hash,
            )
        elif not decision.allowed:
            raise PolicyDeniedError(
                rule_id=decision.rule_id,
                reason=decision.reason,
                error_code=decision.error_code or "E_POLICY_DENY",
                canonical_mode=decision.canonical_mode,
            )

        if decision.normalized_options:
            opts = dict(decision.normalized_options)
        if "max_rows" in opts and "limit" not in opts:
            opts["limit"] = int(opts["max_rows"])

        required_cap = "write_dml" if is_mutation else "read_select"
        self.router.require_capability(record.dialect, required_cap)

        adapter = self.adapters[record.dialect]
        executed = adapter.executor.execute_compiled(
            compile_artifact_id=compile_artifact_id,
            query_text=record.query_text,
            options=opts,
        )

        attempt_index = self._execution_attempts.get(compile_artifact_id, 0) + 1
        self._execution_attempts[compile_artifact_id] = attempt_index
        execution_artifact_id = deterministic_id(
            "exe",
            {
                "compile_artifact_id": compile_artifact_id,
                "options": opts,
                "execution_mode": decision.canonical_mode,
                "attempt_index": attempt_index,
            },
        )
        return ExecuteResult(
            execution_artifact_id=execution_artifact_id,
            compile_artifact_id=compile_artifact_id,
            rows=executed.rows,
            row_count=len(executed.rows),
            notices=executed.notices,
        )

    def run_query(
        self,
        *,
        request_id: str,
        dialect: str,
        query_text: str,
        mode: str = "read_only",
        options: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
        approval_token: str | None = None,
        approval_evidence: dict[str, Any] | None = None,
    ) -> QueryResponse:
        query_context = dict(context or {})
        started_at = time.perf_counter()
        security_context: dict[str, Any] | None = None
        try:
            security_context = require_security_context(query_context)
        except ToolContractError as exc:
            trace_id = deterministic_id(
                "tr",
                {
                    "request_id": request_id,
                    "error_code": exc.error_code,
                    "rule": exc.policy_rule_id,
                },
            )
            bundle = create_audit_bundle(
                trace_id=trace_id,
                request_id=request_id,
                tenant_id="unknown",
                actor_id="unknown",
                dialect=dialect,
                execution_mode="ai_analysis",
                sql_text_normalized=" ".join(query_text.split()),
                compile_artifact_id=deterministic_id("cmp", {"request_id": request_id, "invalid": True}),
                execution_artifact_id=None,
                plan_json={"operator_tree": {}, "rls_visibility": {"applied": False, "policy_ids": []}},
                plan_hash=deterministic_id("plan", {"request_id": request_id, "invalid": True}).removeprefix("plan_"),
                policy_decision="deny",
                policy_rule_id=exc.policy_rule_id or "SECURITY-CONTEXT-001",
                security_context={"tenant_id": "", "actor_id": "", "roles": [], "context_version": 1},
                approval_id=None,
                approval_token=approval_token,
                error_code=exc.error_code,
                statement_kind="unknown",
                sblr_hash="",
            )
            self._audit_store.append(bundle)
            self._emit_runtime_event(
                event_type="query_execution",
                status=self._event_status_for_exception(exc),
                trace_id=trace_id,
                request_id=request_id,
                security_context={"tenant_id": "", "actor_id": ""},
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "dialect": dialect,
                    "statement_kind": "unknown",
                    "execution_mode": "ai_analysis",
                    "policy_rule_id": exc.policy_rule_id,
                    "error_code": exc.error_code,
                    "message": exc.message,
                },
            )
            raise PolicyDeniedError(
                rule_id=exc.policy_rule_id or "SECURITY-CONTEXT-001",
                reason=exc.message,
                error_code=exc.error_code,
                canonical_mode="ai_analysis",
            ) from None

        query_context["security_context"] = security_context
        validated_options = validate_options(options)
        resolved_approval_token = _resolve_approval_token(
            approval_token=approval_token,
            approval_evidence=approval_evidence,
        )
        operational_cost: dict[str, Any] | None = None
        durable_approval: ApprovalRecord | None = None

        try:
            compiled = self.compile_query(
                dialect=dialect,
                query_text=query_text,
                context=query_context,
            )
        except ToolContractError as exc:
            trace_id = deterministic_id(
                "tr",
                {
                    "request_id": request_id,
                    "error_code": exc.error_code,
                    "rule": exc.policy_rule_id,
                    "compatibility_denied": True,
                },
            )
            bundle = create_audit_bundle(
                trace_id=trace_id,
                request_id=request_id,
                tenant_id=security_context.get("tenant_id", "unknown"),
                actor_id=security_context.get("actor_id", "unknown"),
                dialect=dialect,
                execution_mode="ai_analysis",
                sql_text_normalized=" ".join(query_text.split()),
                compile_artifact_id=deterministic_id("cmp", {"request_id": request_id, "blocked": True}),
                execution_artifact_id=None,
                plan_json={"operator_tree": {}, "rls_visibility": {"applied": False, "policy_ids": []}},
                plan_hash=deterministic_id("plan", {"request_id": request_id, "blocked": True}).removeprefix("plan_"),
                policy_decision="deny",
                policy_rule_id=exc.policy_rule_id or "COMPATIBILITY-NEGOTIATION-001",
                security_context=security_context,
                approval_id=None,
                approval_token=resolved_approval_token,
                error_code=exc.error_code,
                statement_kind="unknown",
                sblr_hash="",
            )
            self._audit_store.append(bundle)
            self._emit_runtime_event(
                event_type="query_execution",
                status=self._event_status_for_exception(exc),
                trace_id=trace_id,
                request_id=request_id,
                security_context=security_context,
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "dialect": dialect,
                    "statement_kind": "unknown",
                    "execution_mode": "ai_analysis",
                    "policy_rule_id": exc.policy_rule_id,
                    "error_code": exc.error_code,
                    "message": exc.message,
                },
            )
            raise
        record = self._compile_store[compiled.compile_artifact_id]
        is_mutation = compiled.statement_kind != "read"

        try:
            decision = self.policy_engine.enforce(
                mode=mode,
                is_mutation=is_mutation,
                approval_token=resolved_approval_token,
                options=validated_options,
                tenant_id=security_context.get("tenant_id"),
                actor_id=security_context.get("actor_id"),
                statement_hash=compiled.sblr_hash,
            )
            if is_mutation:
                durable_approval = self._approval_ledger.validate_or_register(
                    approval_token=resolved_approval_token or "",
                    approval_evidence=approval_evidence,
                    tenant_id=security_context.get("tenant_id", ""),
                    actor_id=security_context.get("actor_id", ""),
                    statement_hash=compiled.sblr_hash,
                )
            operational_cost = self._operational_controls.enforce(
                tenant_id=security_context.get("tenant_id", "unknown"),
                actor_id=security_context.get("actor_id", "unknown"),
                is_mutation=is_mutation,
                options=validated_options,
            ).to_dict()
            opts = dict(decision.normalized_options or validated_options)
            executed = self.execute_compiled(
                compile_artifact_id=compiled.compile_artifact_id,
                options=opts,
                mode=mode,
                approval_token=resolved_approval_token,
                prevalidated_decision=decision,
            )
            trace_id = deterministic_id(
                "tr",
                {
                    "request_id": request_id,
                    "compile_artifact_id": compiled.compile_artifact_id,
                    "execution_artifact_id": executed.execution_artifact_id,
                },
            )
            response = QueryResponse(
                request_id=request_id,
                compile_artifact_id=compiled.compile_artifact_id,
                execution_artifact_id=executed.execution_artifact_id,
                result_rows=executed.rows,
                row_count=executed.row_count,
                notices=executed.notices + (
                    [f"cost_units={operational_cost['cost_units']}"]
                    if operational_cost is not None
                    else []
                ),
                trace_id=trace_id,
                cost_attribution=operational_cost,
            )

            plan_info = self._build_plan_info(
                dialect=dialect,
                query_text=query_text,
                security_context=security_context,
                operator_type=("Mutation" if is_mutation else "Read"),
            )
            approval_id = _resolve_approval_id(
                approval_evidence=approval_evidence,
                approval_token=resolved_approval_token,
                compile_artifact_id=compiled.compile_artifact_id,
                durable_approval=durable_approval,
            )
            bundle = create_audit_bundle(
                trace_id=trace_id,
                request_id=request_id,
                tenant_id=security_context.get("tenant_id", "unknown"),
                actor_id=security_context.get("actor_id", "unknown"),
                dialect=dialect,
                execution_mode=decision.canonical_mode,
                sql_text_normalized=" ".join(query_text.split()),
                compile_artifact_id=compiled.compile_artifact_id,
                execution_artifact_id=executed.execution_artifact_id,
                plan_json=plan_info,
                plan_hash=plan_info["plan_hash"],
                policy_decision="allow",
                policy_rule_id=decision.rule_id,
                security_context=security_context,
                cluster_epoch=int(query_context.get("cluster_epoch", 0) or 0),
                approval_id=approval_id,
                approval_token=resolved_approval_token,
                cost_attribution=operational_cost,
                error_code=None,
                statement_kind=compiled.statement_kind,
                sblr_hash=compiled.sblr_hash,
            )
            self._audit_store.append(bundle)
            self._emit_runtime_event(
                event_type="query_execution",
                status="success",
                trace_id=trace_id,
                request_id=request_id,
                security_context=security_context,
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "dialect": dialect,
                    "statement_kind": compiled.statement_kind,
                    "execution_mode": decision.canonical_mode,
                    "compile_artifact_id": compiled.compile_artifact_id,
                    "execution_artifact_id": executed.execution_artifact_id,
                    "policy_rule_id": decision.rule_id,
                    "row_count": executed.row_count,
                },
            )
            return response
        except PolicyDeniedError as exc:
            trace_id = deterministic_id(
                "tr",
                {
                    "request_id": request_id,
                    "compile_artifact_id": compiled.compile_artifact_id,
                    "denied": True,
                    "rule": exc.rule_id,
                },
            )
            plan_info = self._build_plan_info(
                dialect=dialect,
                query_text=query_text,
                security_context=security_context,
                operator_type=("DeniedMutation" if is_mutation else "DeniedRead"),
            )
            bundle = create_audit_bundle(
                trace_id=trace_id,
                request_id=request_id,
                tenant_id=security_context.get("tenant_id", "unknown"),
                actor_id=security_context.get("actor_id", "unknown"),
                dialect=dialect,
                execution_mode=exc.canonical_mode,
                sql_text_normalized=" ".join(query_text.split()),
                compile_artifact_id=compiled.compile_artifact_id,
                execution_artifact_id=None,
                plan_json=plan_info,
                plan_hash=plan_info["plan_hash"],
                policy_decision="deny",
                policy_rule_id=exc.rule_id,
                security_context=security_context,
                cluster_epoch=int(query_context.get("cluster_epoch", 0) or 0),
                approval_id=(approval_evidence or {}).get("approval_id") if isinstance(approval_evidence, dict) else None,
                approval_token=resolved_approval_token,
                cost_attribution=operational_cost,
                error_code=exc.error_code,
                statement_kind=compiled.statement_kind,
                sblr_hash=compiled.sblr_hash,
            )
            self._audit_store.append(bundle)
            self._emit_runtime_event(
                event_type="query_execution",
                status=self._event_status_for_exception(exc),
                trace_id=trace_id,
                request_id=request_id,
                security_context=security_context,
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "dialect": dialect,
                    "statement_kind": compiled.statement_kind,
                    "execution_mode": exc.canonical_mode,
                    "compile_artifact_id": compiled.compile_artifact_id,
                    "policy_rule_id": exc.rule_id,
                    "error_code": exc.error_code,
                    "message": exc.reason,
                },
            )
            raise
        except ToolContractError as exc:
            trace_id = deterministic_id(
                "tr",
                {
                    "request_id": request_id,
                    "compile_artifact_id": compiled.compile_artifact_id,
                    "operational_or_approval_denied": True,
                },
            )
            plan_info = self._build_plan_info(
                dialect=dialect,
                query_text=query_text,
                security_context=security_context,
                operator_type=("DeniedMutation" if is_mutation else "DeniedRead"),
            )
            bundle = create_audit_bundle(
                trace_id=trace_id,
                request_id=request_id,
                tenant_id=security_context.get("tenant_id", "unknown"),
                actor_id=security_context.get("actor_id", "unknown"),
                dialect=dialect,
                execution_mode="ai_analysis",
                sql_text_normalized=" ".join(query_text.split()),
                compile_artifact_id=compiled.compile_artifact_id,
                execution_artifact_id=None,
                plan_json=plan_info,
                plan_hash=plan_info["plan_hash"],
                policy_decision="deny",
                policy_rule_id=exc.policy_rule_id or "OPS-OR-APPROVAL-001",
                security_context=security_context,
                cluster_epoch=int(query_context.get("cluster_epoch", 0) or 0),
                approval_id=durable_approval.approval_id if durable_approval is not None else None,
                approval_token=resolved_approval_token,
                cost_attribution=operational_cost,
                error_code=exc.error_code,
                statement_kind=compiled.statement_kind,
                sblr_hash=compiled.sblr_hash,
            )
            self._audit_store.append(bundle)
            self._emit_runtime_event(
                event_type="query_execution",
                status=self._event_status_for_exception(exc),
                trace_id=trace_id,
                request_id=request_id,
                security_context=security_context,
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "dialect": dialect,
                    "statement_kind": compiled.statement_kind,
                    "execution_mode": "ai_analysis",
                    "compile_artifact_id": compiled.compile_artifact_id,
                    "policy_rule_id": exc.policy_rule_id,
                    "error_code": exc.error_code,
                    "message": exc.message,
                },
            )
            raise

    def execute_readonly_query(
        self,
        *,
        request_id: str,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        options: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        query_context = dict(context or {})
        query_context["security_context"] = require_security_context(
            {"security_context": security_context}
        )
        response = self.run_query(
            request_id=request_id,
            dialect=dialect,
            query_text=query_text,
            mode="ai_analysis",
            options=options,
            context=query_context,
            approval_token=None,
        )
        return {
            "compile_artifact_id": response.compile_artifact_id,
            "execution_artifact_id": response.execution_artifact_id,
            "result_rows": response.result_rows,
            "row_count": response.row_count,
            "notices": response.notices,
            "trace_id": response.trace_id,
            "cost_attribution": response.cost_attribution,
        }

    def execute_mutation(
        self,
        *,
        request_id: str,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        approval_evidence: dict[str, Any],
        options: dict[str, Any] | None = None,
        context: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        if not isinstance(approval_evidence, dict):
            raise ToolContractError(
                error_code="E_APPROVAL_INVALID",
                message="approval_evidence must be an object",
                policy_rule_id="MODE-APPROVAL-001",
            )
        query_context = dict(context or {})
        query_context["security_context"] = require_security_context(
            {"security_context": security_context}
        )
        response = self.run_query(
            request_id=request_id,
            dialect=dialect,
            query_text=query_text,
            mode="ai_mutation_approved",
            options=options,
            context=query_context,
            approval_token=_resolve_approval_token(
                approval_token=None,
                approval_evidence=approval_evidence,
            ),
            approval_evidence=approval_evidence,
        )
        return {
            "compile_artifact_id": response.compile_artifact_id,
            "execution_artifact_id": response.execution_artifact_id,
            "affected_rows": response.row_count,
            "notices": response.notices,
            "trace_id": response.trace_id,
            "cost_attribution": response.cost_attribution,
        }

    def introspect_plan(
        self,
        *,
        dialect: str,
        security_context: dict[str, Any],
        query_text: str | None = None,
        compile_artifact_id: str | None = None,
    ) -> dict[str, Any]:
        _ = require_security_context({"security_context": security_context})
        provided = [bool(query_text), bool(compile_artifact_id)]
        if sum(1 for flag in provided if flag) != 1:
            raise ToolContractError(
                error_code="E_INVALID_ARGUMENT",
                message="exactly one of query_text or compile_artifact_id is required",
            )

        if query_text is not None:
            compiled = self.compile_query(
                dialect=dialect,
                query_text=query_text,
                context={"security_context": security_context},
            )
            compile_id = compiled.compile_artifact_id
            statement_kind = compiled.statement_kind
            normalized_query = query_text
        else:
            assert compile_artifact_id is not None
            record = self._compile_store.get(compile_artifact_id)
            if record is None:
                raise KeyError(f"Unknown compile artifact: {compile_artifact_id}")
            compile_id = compile_artifact_id
            statement_kind = record.statement_kind
            normalized_query = record.query_text

        plan = self._build_plan_info(
            dialect=dialect,
            query_text=normalized_query,
            security_context=security_context,
            operator_type=("Mutation" if statement_kind != "read" else "Read"),
        )
        trace_id = deterministic_id(
            "tr",
            {
                "operation": "introspect_plan",
                "compile_artifact_id": compile_id,
                "plan_hash": plan["plan_hash"],
            },
        )
        return {
            "compile_artifact_id": compile_id,
            "plan_hash": plan["plan_hash"],
            "plan_version": plan["plan_version"],
            "operator_tree": plan["operator_tree"],
            "rls_visibility": plan["rls_visibility"],
            "estimated_cost": plan["estimated_cost"],
            "trace_id": trace_id,
        }

    def explain_query(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        security_context = _extract_security_context(context or {})
        if not security_context.get("tenant_id") or not security_context.get("actor_id"):
            security_context = {
                "tenant_id": "system",
                "actor_id": "system",
                "roles": ["system"],
                "session_id": "system",
                "context_version": 1,
            }
        return self.introspect_plan(
            dialect=dialect,
            security_context=security_context,
            query_text=query_text,
        )

    def add_embeddings(
        self,
        *,
        index_id: str,
        dimension: int,
        records: list[dict[str, Any]],
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=False) or self._retrieval
        return store.add_embeddings(
            index_id=index_id,
            dimension=dimension,
            records=records,
            security_context=security_context,
        )

    def add_generated_embeddings(
        self,
        *,
        index_id: str,
        dimension: int,
        records: list[dict[str, Any]],
        provider_config: dict[str, Any],
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=False) or self._retrieval
        return store.add_generated_embeddings(
            index_id=index_id,
            dimension=dimension,
            records=records,
            provider_config=provider_config,
            security_context=security_context,
        )

    def delete_embeddings(
        self,
        *,
        index_id: str,
        vector_ids: list[str],
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=False) or self._retrieval
        return store.delete_embeddings(
            index_id=index_id,
            vector_ids=vector_ids,
            security_context=security_context,
        )

    def reindex_vector_index(
        self,
        *,
        index_id: str,
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=True) or self._retrieval
        return store.reindex_index(
            index_id=index_id,
            security_context=security_context,
        )

    def delete_vector_index(
        self,
        *,
        index_id: str,
        security_context: dict[str, Any],
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=True) or self._retrieval
        return store.delete_index(
            index_id=index_id,
            security_context=security_context,
        )

    def vector_search(
        self,
        *,
        index_id: str,
        query_embedding: list[float],
        top_k: int,
        security_context: dict[str, Any],
        filters: dict[str, Any] | None = None,
        include_vectors: bool = False,
    ) -> dict[str, Any]:
        store = self._find_retrieval_store(index_id=index_id, include_deleted=False) or self._retrieval
        return store.vector_search(
            index_id=index_id,
            query_embedding=query_embedding,
            top_k=top_k,
            filters=filters,
            include_vectors=include_vectors,
            security_context=security_context,
        )

    def hybrid_search(
        self,
        *,
        dialect: str,
        query_text: str,
        query_embedding: list[float],
        vector_index_id: str,
        top_k: int,
        security_context: dict[str, Any],
        sql_filter: dict[str, Any] | None = None,
        weights: dict[str, Any] | None = None,
        options: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        _ = validate_options(options)
        store = (
            self._find_retrieval_store(index_id=vector_index_id, include_deleted=False)
            or self._retrieval
        )
        return store.hybrid_search(
            dialect=dialect,
            query_text=query_text,
            query_embedding=query_embedding,
            vector_index_id=vector_index_id,
            sql_filter=sql_filter,
            weights=weights,
            top_k=top_k,
            security_context=security_context,
        )

    def _invoke_normalized_tool(self, normalized: dict[str, Any]) -> dict[str, Any]:
        tool_name = str(normalized["tool_name"])
        arguments = dict(normalized.get("arguments", {}))
        request_id = str(normalized["request_id"])
        call_id = str(normalized["call_id"])
        interface_profile_id = str(normalized["interface_profile_id"])
        started_at = time.perf_counter()
        security_context = (
            dict(normalized["security_context"])
            if isinstance(normalized.get("security_context"), dict)
            else None
        )
        try:
            result = self._dispatch_tool_call(
                tool_name=tool_name,
                request_id=request_id,
                arguments=arguments,
                normalized=normalized,
            )
            trace_id = _extract_trace_id(result) or deterministic_id(
                "tr",
                {"request_id": request_id, "call_id": call_id, "tool_name": tool_name},
            )
            self._emit_runtime_event(
                event_type="tool_invocation",
                status="success",
                trace_id=trace_id,
                request_id=request_id,
                interface_profile_id=interface_profile_id,
                security_context=security_context,
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "tool_name": tool_name,
                    "call_id": call_id,
                    "mode": normalized.get("mode"),
                    "provider_profile_id": normalized.get("provider_profile_id"),
                },
            )
            return normalize_tool_response(
                tool_name=tool_name,
                request_id=request_id,
                call_id=call_id,
                interface_profile_id=interface_profile_id,
                trace_id=trace_id,
                result=result,
            )
        except Exception as exc:
            error = map_exception_to_error(
                exc,
                trace_seed={
                    "request_id": request_id,
                    "call_id": call_id,
                    "tool_name": tool_name,
                    "error": True,
                },
            )
            self._emit_runtime_event(
                event_type="tool_invocation",
                status=self._event_status_for_exception(exc),
                trace_id=error["trace_id"],
                request_id=request_id,
                interface_profile_id=interface_profile_id,
                security_context=security_context,
                duration_ms=(time.perf_counter() - started_at) * 1000.0,
                attributes={
                    "tool_name": tool_name,
                    "call_id": call_id,
                    "mode": normalized.get("mode"),
                    "provider_profile_id": normalized.get("provider_profile_id"),
                    "error_code": error.get("error_code"),
                    "policy_rule_id": error.get("policy_rule_id"),
                    "message": error.get("message"),
                },
            )
            return normalize_tool_response(
                tool_name=tool_name,
                request_id=request_id,
                call_id=call_id,
                interface_profile_id=interface_profile_id,
                trace_id=error["trace_id"],
                error=error,
            )

    def _dispatch_tool_call(
        self,
        *,
        tool_name: str,
        request_id: str,
        arguments: dict[str, Any],
        normalized: dict[str, Any],
    ) -> Any:
        if tool_name == "get_capabilities":
            return self.get_capabilities()
        if tool_name == "get_tool_descriptors":
            return self.get_tool_descriptors()
        if tool_name == "get_provider_profiles":
            return self.get_provider_profiles()
        if tool_name == "get_compatibility_manifest":
            return self.get_compatibility_manifest()
        if tool_name == "export_certification_manifest":
            return self.export_certification_manifest()
        if tool_name == "negotiate_compatibility":
            return self.negotiate_compatibility(arguments.get("request"))
        if tool_name == "open_remote_session":
            return self.open_remote_session(arguments)
        if tool_name == "invoke_remote_tool":
            return self.invoke_remote_tool(
                session_id=str(arguments["session_id"]),
                request_id=str(arguments["request_id"]),
                method=str(arguments["method"]),
                params=arguments.get("params"),
                client_operation_timeout_ms=arguments.get("client_operation_timeout_ms"),
                stream_requested=bool(arguments.get("stream_requested", False)),
                allow_background_execution=bool(
                    arguments.get("allow_background_execution", False)
                ),
                cancellation_token=str(arguments.get("cancellation_token", "")).strip() or None,
            )
        if tool_name == "close_remote_session":
            return self.close_remote_session(
                session_id=str(arguments["session_id"]),
                request_id=str(arguments.get("request_id", "")).strip() or None,
            )
        if tool_name == "poll_remote_operation":
            return self.poll_remote_operation(
                session_id=str(arguments["session_id"]),
                operation_id=str(arguments["operation_id"]),
                continuation_token=str(arguments.get("continuation_token", "")).strip() or None,
            )
        if tool_name == "cancel_remote_operation":
            return self.cancel_remote_operation(
                session_id=str(arguments["session_id"]),
                operation_id=str(arguments["operation_id"]),
                request_id=str(arguments["request_id"]),
                reason=str(arguments["reason"]),
            )
        if tool_name == "get_server_registry":
            return self.get_server_registry(
                security_context=(
                    dict(arguments["security_context"])
                    if isinstance(arguments.get("security_context"), dict)
                    else None
                ),
                include_hidden=bool(arguments.get("include_hidden", False)),
            )
        if tool_name == "register_remote_server":
            return self.register_remote_server(
                server_label=str(arguments["server_label"]),
                interface_profile_ids=list(arguments["interface_profile_ids"]),
                tool_families=list(arguments["tool_families"]),
                routing_target=str(arguments["routing_target"]),
                security_context=dict(arguments["security_context"]),
                publication_state=str(arguments.get("publication_state", "published")),
                metadata=arguments.get("metadata"),
            )
        if tool_name == "update_remote_server_lifecycle":
            return self.update_remote_server_lifecycle(
                server_id=str(arguments["server_id"]),
                action=str(arguments["action"]),
                reason=str(arguments.get("reason", "")).strip() or None,
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "report_remote_server_health":
            return self.report_remote_server_health(
                server_id=str(arguments["server_id"]),
                health_state=str(arguments["health_state"]),
                summary=str(arguments.get("summary", "")).strip() or None,
                metrics=dict(arguments["metrics"]) if isinstance(arguments.get("metrics"), dict) else None,
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "resolve_gateway_route":
            return self.resolve_gateway_route(
                tool_name=str(arguments["tool_name"]),
                interface_profile_id=str(arguments.get("interface_profile_id", "service_internal_v0")),
                preferred_server_id=str(arguments.get("preferred_server_id", "")).strip() or None,
                security_context=(
                    dict(arguments["security_context"])
                    if isinstance(arguments.get("security_context"), dict)
                    else None
                ),
            )
        if tool_name == "list_dialects":
            return {"dialects": self.list_dialects()}
        if tool_name == "list_schemas":
            return {
                "schemas": self.list_schemas(
                    dialect=str(arguments["dialect"]),
                    database=(str(arguments.get("database", "")).strip() or None),
                )
            }
        if tool_name == "list_tables":
            return {
                "tables": self.list_tables(
                    dialect=str(arguments["dialect"]),
                    schema=str(arguments["schema"]),
                )
            }
        if tool_name == "describe_table":
            return self.describe_table(
                dialect=str(arguments["dialect"]),
                schema=str(arguments["schema"]),
                table=str(arguments["table"]),
            )
        if tool_name == "compile_query":
            return self.compile_query(
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                context=arguments.get("context", {}),
            ).to_dict()
        if tool_name == "execute_compiled":
            return self.execute_compiled(
                compile_artifact_id=str(arguments["compile_artifact_id"]),
                options=arguments.get("options"),
                mode=str(arguments.get("mode", normalized.get("mode", "ai_analysis"))),
                approval_token=str(arguments.get("approval_token", "")).strip() or None,
            ).to_dict()
        if tool_name == "execute_readonly_query":
            return self.execute_readonly_query(
                request_id=request_id,
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                security_context=dict(arguments["security_context"]),
                options=arguments.get("options"),
                context=arguments.get("context"),
            )
        if tool_name == "execute_mutation":
            approval_evidence = normalized.get("approval_evidence") or arguments.get("approval_evidence")
            return self.execute_mutation(
                request_id=request_id,
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                security_context=dict(arguments["security_context"]),
                approval_evidence=dict(approval_evidence or {}),
                options=arguments.get("options"),
                context=arguments.get("context"),
            )
        if tool_name == "run_query":
            return self.run_query(
                request_id=request_id,
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                mode=str(arguments.get("mode", normalized.get("mode", "ai_analysis"))),
                options=arguments.get("options"),
                context=arguments.get("context"),
                approval_token=str(arguments.get("approval_token", "")).strip() or None,
            ).to_dict()
        if tool_name == "run_mutation":
            security_context = normalized.get("security_context", {})
            approval_evidence = normalized.get("approval_evidence") or {
                "approval_token": str(arguments.get("approval_token", "")).strip()
            }
            return self.execute_mutation(
                request_id=request_id,
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                security_context=dict(security_context),
                approval_evidence=dict(approval_evidence),
                options=arguments.get("options"),
                context=arguments.get("context"),
            )
        if tool_name == "explain_query":
            return self.explain_query(
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                context=arguments.get("context") or (
                    {"security_context": arguments["security_context"]}
                    if "security_context" in arguments
                    else {}
                ),
            )
        if tool_name == "create_vector_index":
            return self.create_vector_index(
                index_id=str(arguments["index_id"]),
                dimension=int(arguments["dimension"]),
                security_context=dict(arguments["security_context"]),
                profile_id=str(arguments.get("profile_id", "client_supplied_embeddings_v0")),
            )
        if tool_name == "list_vector_indexes":
            return self.list_vector_indexes(
                security_context=dict(arguments["security_context"]),
                include_deleted=bool(arguments.get("include_deleted", False)),
            )
        if tool_name == "describe_vector_index":
            return self.describe_vector_index(
                index_id=str(arguments["index_id"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "add_embeddings":
            return self.add_embeddings(
                index_id=str(arguments["index_id"]),
                dimension=int(arguments["dimension"]),
                records=list(arguments["records"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "add_generated_embeddings":
            return self.add_generated_embeddings(
                index_id=str(arguments["index_id"]),
                dimension=int(arguments["dimension"]),
                records=list(arguments["records"]),
                provider_config=dict(arguments["provider_config"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "delete_embeddings":
            return self.delete_embeddings(
                index_id=str(arguments["index_id"]),
                vector_ids=list(arguments["vector_ids"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "reindex_vector_index":
            return self.reindex_vector_index(
                index_id=str(arguments["index_id"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "delete_vector_index":
            return self.delete_vector_index(
                index_id=str(arguments["index_id"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "vector_search":
            return self.vector_search(
                index_id=str(arguments["index_id"]),
                query_embedding=list(arguments["query_embedding"]),
                top_k=int(arguments["top_k"]),
                security_context=dict(arguments["security_context"]),
                filters=arguments.get("filters"),
                include_vectors=bool(arguments.get("include_vectors", False)),
            )
        if tool_name == "hybrid_search":
            return self.hybrid_search(
                dialect=str(arguments["dialect"]),
                query_text=str(arguments["query_text"]),
                query_embedding=list(arguments["query_embedding"]),
                vector_index_id=str(arguments["vector_index_id"]),
                top_k=int(arguments["top_k"]),
                security_context=dict(arguments["security_context"]),
                sql_filter=arguments.get("sql_filter"),
                weights=arguments.get("weights"),
                options=arguments.get("options"),
            )
        if tool_name == "replay_audit_bundle":
            return self.replay_audit_bundle(
                bundle=dict(arguments["bundle"]),
                security_context=(
                    dict(arguments["security_context"])
                    if isinstance(arguments.get("security_context"), dict)
                    else None
                ),
                expected_policy_decision=str(arguments.get("expected_policy_decision", "")).strip()
                or None,
                expected_policy_rule_id=str(arguments.get("expected_policy_rule_id", "")).strip()
                or None,
                expected_plan_hash=str(arguments.get("expected_plan_hash", "")).strip() or None,
            )
        if tool_name == "list_audit_bundles":
            return self.list_audit_bundles(
                security_context=dict(arguments["security_context"]),
                limit=int(arguments.get("limit", 100) or 100),
            )
        if tool_name == "validate_approval_evidence":
            return self.validate_approval_evidence(
                approval_evidence=dict(arguments["approval_evidence"]),
                security_context=dict(arguments["security_context"]),
                statement_hash=str(arguments["statement_hash"]),
            )
        if tool_name == "list_approval_records":
            return self.list_approval_records(
                security_context=dict(arguments["security_context"]),
                tenant_id=str(arguments.get("tenant_id", "")).strip() or None,
                actor_id=str(arguments.get("actor_id", "")).strip() or None,
                include_revoked=bool(arguments.get("include_revoked", True)),
            )
        if tool_name == "revoke_approval_record":
            return self.revoke_approval_record(
                approval_id=str(arguments["approval_id"]),
                reason=str(arguments["reason"]),
                security_context=dict(arguments["security_context"]),
            )
        if tool_name == "create_audit_attestation":
            return self.create_audit_attestation(
                security_context=dict(arguments["security_context"]),
                bundle=dict(arguments["bundle"]) if isinstance(arguments.get("bundle"), dict) else None,
                attestation_mode=str(arguments.get("attestation_mode", "")).strip() or None,
                key_id=str(arguments.get("key_id", "")).strip() or None,
                external_reference=str(arguments.get("external_reference", "")).strip() or None,
                metadata=dict(arguments["metadata"]) if isinstance(arguments.get("metadata"), dict) else None,
            )
        if tool_name == "verify_audit_attestation":
            return self.verify_audit_attestation(
                security_context=dict(arguments["security_context"]),
                bundle=dict(arguments["bundle"]),
                attestation=dict(arguments["attestation"]),
                shared_secret=str(arguments.get("shared_secret", "")).strip() or None,
            )
        if tool_name == "get_runtime_diagnostics":
            return self.get_runtime_diagnostics(
                security_context=dict(arguments["security_context"]),
                max_recent_errors=int(arguments.get("max_recent_errors", 10) or 10),
            )
        if tool_name == "generate_operator_runbook_bundle":
            return self.generate_operator_runbook_bundle(
                security_context=dict(arguments["security_context"]),
                output_dir=str(arguments.get("output_dir", "")).strip() or None,
                max_recent_errors=int(arguments.get("max_recent_errors", 10) or 10),
                target_profiles=(
                    list(arguments.get("target_profiles", []))
                    if isinstance(arguments.get("target_profiles"), list)
                    else None
                ),
            )
        raise ToolContractError(
            error_code="E_TOOL_NOT_FOUND",
            message=f"unknown tool: {tool_name}",
            policy_rule_id="TOOL-DISPATCH-001",
        )

    def _build_plan_info(
        self,
        *,
        dialect: str,
        query_text: str,
        security_context: dict[str, Any],
        operator_type: str,
    ) -> dict[str, Any]:
        return build_plan_response(
            dialect=dialect,
            query_text=query_text,
            operator_tree={
                "operator_id": "root",
                "operator_type": operator_type,
                "children": [],
            },
            rls_policy_ids=[],
            predicate_hash=deterministic_id(
                "pred", {"security_context": security_context}
            ).removeprefix("pred_"),
            planner_version="v1",
            rls_applied=True,
        )

    def _support_flags(self, *, scratchbird_core_surface: dict[str, Any]) -> dict[str, Any]:
        return {
            "graph_ops": True,
            "bridge_runtime": True,
            "native_admin": True,
            "release_ceiling": scratchbird_core_surface["release_ceiling"],
        }

    def _local_registry_entry(
        self,
        *,
        interface_profiles: list[dict[str, Any]] | None = None,
        scratchbird_core_surface: dict[str, Any] | None = None,
        support_flags: dict[str, Any] | None = None,
    ):
        profiles = interface_profiles or get_interface_profiles()
        core_surface = scratchbird_core_surface or build_scratchbird_core_surface_packet()
        supports = support_flags or self._support_flags(scratchbird_core_surface=core_surface)
        return self._registry.build_local_entry(
            interface_profile_ids=tuple(
                sorted(str(profile["profile_id"]) for profile in profiles)
            ),
            tool_names=tuple(
                sorted(descriptor["tool_name"] for descriptor in get_tool_descriptors())
            ),
            supports=supports,
            matrix_version=str(self.router.matrix.get("version", "unknown")),
            release_ceiling=str(core_surface["release_ceiling"]),
        )

    def _remote_capabilities(self) -> dict[str, Any]:
        capabilities = self.get_capabilities()
        capabilities["interface_profiles"] = [
            profile
            for profile in capabilities["interface_profiles"]
            if profile.get("profile_id") == "mcp_remote_v0"
        ]
        capabilities["session_required"] = True
        capabilities["supports"]["streaming"] = True
        capabilities["supports"]["continuation_tokens"] = True
        capabilities["supports"]["cancellation"] = True
        capabilities["supports"]["remote_transports"] = list(
            self._runtime_settings.remote_mcp_supported_transports
        )
        capabilities["supports"]["remote_auth_types"] = list(
            self._runtime_settings.remote_mcp_supported_auth_types
        )
        capabilities["supports"]["authorization_model"] = "server_policy_grants_groups"
        return capabilities

    def _compile_with_repair(
        self,
        *,
        adapter: DialectAdapter,
        query_text: str,
        query_context: dict[str, Any],
    ) -> tuple[Any, str, list[str]]:
        candidates = build_compile_repair_candidates(
            query_text,
            max_attempts=self._runtime_settings.compile_repair_max_attempts,
        )
        last_exc: Exception | None = None
        for candidate in candidates:
            try:
                compiled = adapter.compiler.compile_query(candidate.query_text, query_context)
                warnings: list[str] = []
                if candidate.strategy_id != "original":
                    warnings.append(
                        f"compile repair applied: {candidate.strategy_id}"
                    )
                return compiled, candidate.query_text, warnings
            except ToolContractError as exc:
                last_exc = exc
                if candidate.strategy_id == "original":
                    continue
                raise
            except Exception as exc:
                last_exc = exc
                if candidate.strategy_id == "original":
                    continue
                raise
        assert last_exc is not None
        raise last_exc

    def _require_admin_security_context(
        self,
        *,
        security_context: dict[str, Any],
        rule_id: str,
        message: str,
    ) -> dict[str, Any]:
        normalized_security_context = require_security_context(
            {"security_context": security_context}
        )
        if not is_admin_context(normalized_security_context):
            raise ToolContractError(
                error_code="E_POLICY_DENY",
                message=message,
                policy_rule_id=rule_id,
            )
        return normalized_security_context

    def _event_status_for_exception(self, exc: Exception) -> str:
        if isinstance(exc, PolicyDeniedError):
            return "denied"
        if isinstance(exc, ToolContractError) and exc.error_code in {
            "E_POLICY_DENY",
            "E_APPROVAL_INVALID",
            "E_LIMIT_EXCEEDED",
        }:
            return "denied"
        return "error"

    def _emit_runtime_event(
        self,
        *,
        event_type: str,
        status: str,
        trace_id: str | None = None,
        request_id: str | None = None,
        interface_profile_id: str | None = None,
        security_context: dict[str, Any] | None = None,
        duration_ms: float | None = None,
        attributes: dict[str, Any] | None = None,
    ) -> None:
        try:
            self._event_logger.emit(
                event_type=event_type,
                status=status,
                trace_id=trace_id,
                request_id=request_id,
                interface_profile_id=interface_profile_id,
                security_context=security_context,
                duration_ms=duration_ms,
                attributes=attributes,
            )
        except Exception:
            return


def _build_adapters(
    *,
    router: DialectRouter,
    settings: RuntimeSettings,
) -> tuple[dict[str, DialectAdapter], str]:
    mode = settings.normalized_mode()
    adapters: dict[str, DialectAdapter] = {}
    http_client: HttpJsonClient | None = None

    if mode in {"http", "hybrid"}:
        http_client = HttpJsonClient(
            base_url=settings.http_base_url,
            timeout_sec=settings.http_timeout_sec,
            api_token=settings.http_api_token,
            retry_attempts=settings.http_retry_attempts,
            retry_backoff_ms=settings.http_retry_backoff_ms,
            circuit_breaker_failure_threshold=settings.http_circuit_breaker_failure_threshold,
            circuit_breaker_cooldown_sec=settings.http_circuit_breaker_cooldown_sec,
        )

    for dialect in router.available_dialects():
        if http_client is not None and settings.should_use_http_for_dialect(dialect):
            adapters[dialect] = make_http_dialect_adapter(dialect=dialect, client=http_client)
        else:
            adapters[dialect] = make_mock_dialect_adapter(dialect)

    return adapters, mode


def build_default_service(settings: RuntimeSettings | None = None) -> ScratchBirdAIService:
    runtime_settings = settings or load_runtime_settings()
    matrix = load_capability_matrix()
    router = DialectRouter(matrix=matrix)
    policy_engine = PolicyEngine()
    adapters, mode = _build_adapters(router=router, settings=runtime_settings)
    remote_session_manager = RemoteSessionManager(
        auth_token=runtime_settings.remote_mcp_auth_token,
        session_ttl_sec=runtime_settings.remote_mcp_session_ttl_sec,
        heartbeat_interval_sec=runtime_settings.remote_mcp_heartbeat_interval_sec,
        supported_protocol_versions=runtime_settings.remote_mcp_protocol_versions,
        supported_auth_types=runtime_settings.remote_mcp_supported_auth_types,
        supported_transports=runtime_settings.remote_mcp_supported_transports,
    )
    retrieval_store = InMemoryRetrievalStore(
        catalog_path=runtime_settings.retrieval_catalog_path,
    )
    managed_retrieval_store = InMemoryRetrievalStore(
        supported_profiles={"engine_managed_retrieval_v0"},
        backend_kind=ENGINE_MANAGED_CONTRACT_SCAFFOLD_BACKEND,
        allow_where_pushdown=True,
    )
    return ScratchBirdAIService(
        router=router,
        policy_engine=policy_engine,
        adapters=adapters,
        adapter_mode=mode,
        retrieval_store=retrieval_store,
        managed_retrieval_store=managed_retrieval_store,
        runtime_settings=runtime_settings,
        remote_session_manager=remote_session_manager,
        approval_ledger=ApprovalLedger(path=runtime_settings.approval_ledger_path),
        operational_controls=OperationalControlEngine(
            window_sec=runtime_settings.operation_window_sec,
            max_requests_per_window=runtime_settings.max_requests_per_window,
            max_mutations_per_window=runtime_settings.max_mutations_per_window,
            max_cost_units_per_window=runtime_settings.max_cost_units_per_window,
        ),
    )


def _resolve_approval_token(
    *,
    approval_token: str | None,
    approval_evidence: dict[str, Any] | None,
) -> str | None:
    token = (approval_token or "").strip()
    if token:
        return token
    if isinstance(approval_evidence, dict):
        candidate = str(approval_evidence.get("approval_token", "")).strip()
        return candidate or None
    return None


def _extract_trace_id(payload: Any) -> str | None:
    if isinstance(payload, dict):
        candidate = payload.get("trace_id")
    else:
        candidate = getattr(payload, "trace_id", None)
    if candidate is None:
        return None
    normalized = str(candidate).strip()
    return normalized or None


def _resolve_approval_id(
    *,
    approval_evidence: dict[str, Any] | None,
    approval_token: str | None,
    compile_artifact_id: str,
    durable_approval: ApprovalRecord | None = None,
) -> str | None:
    if durable_approval is not None:
        return durable_approval.approval_id
    if isinstance(approval_evidence, dict):
        provided = str(approval_evidence.get("approval_id", "")).strip()
        if provided:
            return provided
    if approval_token:
        return deterministic_id(
            "appr",
            {
                "token": approval_token,
                "compile_artifact_id": compile_artifact_id,
            },
        )
    return None


def _extract_security_context(context: dict[str, Any]) -> dict[str, Any]:
    raw = context.get("security_context")
    if isinstance(raw, dict):
        tenant_id = str(raw.get("tenant_id", context.get("tenant_id", ""))).strip()
        actor_id = str(
            raw.get(
                "actor_id",
                context.get("actor_id", context.get("user_id", "")),
            )
        ).strip()
        roles_raw = raw.get("roles", context.get("roles", []))
        roles = [str(role) for role in roles_raw] if isinstance(roles_raw, list) else []
        session_id = str(raw.get("session_id", context.get("session_id", ""))).strip()
        context_version_raw = raw.get("context_version", context.get("context_version", 1))
        try:
            context_version = int(context_version_raw)
        except (TypeError, ValueError):
            context_version = 1
        return {
            "tenant_id": tenant_id,
            "actor_id": actor_id,
            "roles": roles,
            "session_id": session_id,
            "context_version": max(1, context_version),
        }

    tenant_id = str(context.get("tenant_id", "")).strip()
    actor_id = str(context.get("actor_id", context.get("user_id", ""))).strip()
    roles_raw = context.get("roles", [])
    roles = [str(role) for role in roles_raw] if isinstance(roles_raw, list) else []
    session_id = str(context.get("session_id", "")).strip()
    context_version_raw = context.get("context_version", 1)
    try:
        context_version = int(context_version_raw)
    except (TypeError, ValueError):
        context_version = 1
    return {
        "tenant_id": tenant_id,
        "actor_id": actor_id,
        "roles": roles,
        "session_id": session_id,
        "context_version": max(1, context_version),
    }


def _validate_compatibility_context(
    service: ScratchBirdAIService,
    context: dict[str, Any],
    *,
    default_profile_id: str,
    default_transport: str,
) -> None:
    request = _extract_compatibility_request(
        context,
        default_profile_id=default_profile_id,
        default_transport=default_transport,
    )
    if request is None:
        return
    response = service.negotiate_compatibility(request)
    error = response.get("error")
    if isinstance(error, dict):
        raise ToolContractError(
            error_code=str(error.get("error_code", "E_COMPATIBILITY_MISMATCH")),
            message=str(error.get("message", "compatibility negotiation failed")),
            policy_rule_id=str(error.get("reason_code", "COMPATIBILITY-NEGOTIATION-001")),
            trace_id=str(error.get("trace_id", "")) or None,
        )


def _extract_compatibility_request(
    context: dict[str, Any],
    *,
    default_profile_id: str,
    default_transport: str,
) -> dict[str, Any] | None:
    if not isinstance(context, dict):
        return None

    raw = context.get("client_capabilities")
    if raw is None:
        raw = context.get("compatibility_request")
    if raw is None:
        return None
    if not isinstance(raw, dict):
        raise ToolContractError(
            error_code="E_COMPATIBILITY_MISMATCH",
            message="client_capabilities must be an object when provided",
            policy_rule_id="COMPATIBILITY-REQUEST-001",
        )

    request = dict(raw)
    request["interface_profile_id"] = (
        str(request.get("interface_profile_id", "")).strip() or default_profile_id
    )
    request["requested_profile_version"] = (
        str(request.get("requested_profile_version", "")).strip() or "v0"
    )
    request["requested_transport"] = (
        str(request.get("requested_transport", "")).strip() or default_transport
    )
    return request
