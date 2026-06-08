# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""MCP server entrypoint and tool registration for ScratchBird AI."""

from __future__ import annotations

from typing import Any, Callable
from uuid import uuid4

from .service import ScratchBirdAIService, build_default_service
from .tool_schema import map_exception_to_error

try:
    from mcp.server.fastmcp import FastMCP
except ImportError:  # pragma: no cover - runtime optional dependency
    FastMCP = None


def _tool_call(
    *,
    tool_name: str,
    payload: dict[str, Any],
    fn: Callable[[], Any],
) -> Any:
    try:
        return fn()
    except Exception as exc:
        return map_exception_to_error(exc, trace_seed={"tool": tool_name, "payload": payload})


def create_server(service: ScratchBirdAIService | None = None):
    if FastMCP is None:
        raise RuntimeError(
            "MCP runtime not installed. Install optional dependency: pip install .[mcp]"
        )

    svc = service or build_default_service()
    mcp = FastMCP("scratchbird-ai", json_response=True)

    @mcp.tool()
    def get_capabilities() -> dict:
        return _tool_call(
            tool_name="get_capabilities",
            payload={},
            fn=lambda: svc.get_capabilities(),
        )

    @mcp.tool()
    def get_tool_descriptors() -> dict:
        return _tool_call(
            tool_name="get_tool_descriptors",
            payload={},
            fn=lambda: svc.get_tool_descriptors(),
        )

    @mcp.tool()
    def get_provider_profiles() -> dict:
        return _tool_call(
            tool_name="get_provider_profiles",
            payload={},
            fn=lambda: svc.get_provider_profiles(),
        )

    @mcp.tool()
    def get_compatibility_manifest() -> dict:
        return _tool_call(
            tool_name="get_compatibility_manifest",
            payload={},
            fn=lambda: svc.get_compatibility_manifest(),
        )

    @mcp.tool()
    def export_certification_manifest() -> dict:
        return _tool_call(
            tool_name="export_certification_manifest",
            payload={},
            fn=lambda: svc.export_certification_manifest(),
        )

    @mcp.tool()
    def negotiate_compatibility(request: dict | None = None) -> dict:
        payload = {"request": request or {}}
        return _tool_call(
            tool_name="negotiate_compatibility",
            payload=payload,
            fn=lambda: svc.negotiate_compatibility(request or {}),
        )

    @mcp.tool()
    def open_remote_session(
        auth_envelope: dict,
        request_id: str = "",
        interface_profile_id: str = "mcp_remote_v0",
        protocol_version: str = "v0",
        requested_transport: str = "https_json_request_response",
        client_id: str = "unknown-client",
        client_version: str = "unknown",
        client_capabilities: dict | None = None,
        security_context_hint: dict | None = None,
    ) -> dict:
        payload = {
            "request_id": request_id,
            "interface_profile_id": interface_profile_id,
            "protocol_version": protocol_version,
            "requested_transport": requested_transport,
            "client_id": client_id,
            "client_version": client_version,
            "client_capabilities": client_capabilities or {},
            "auth_envelope": auth_envelope,
            "security_context_hint": security_context_hint or {},
        }
        return _tool_call(
            tool_name="open_remote_session",
            payload=payload,
            fn=lambda: svc.open_remote_session(payload),
        )

    @mcp.tool()
    def invoke_remote_tool(
        session_id: str,
        request_id: str,
        method: str,
        params: dict | None = None,
        client_operation_timeout_ms: int | None = None,
        stream_requested: bool = False,
        allow_background_execution: bool = False,
        cancellation_token: str = "",
    ) -> dict:
        payload = {
            "session_id": session_id,
            "request_id": request_id,
            "method": method,
            "params": params or {},
            "client_operation_timeout_ms": client_operation_timeout_ms,
            "stream_requested": stream_requested,
            "allow_background_execution": allow_background_execution,
            "cancellation_token_present": bool(cancellation_token),
        }
        return _tool_call(
            tool_name="invoke_remote_tool",
            payload=payload,
            fn=lambda: svc.invoke_remote_tool(
                session_id=session_id,
                request_id=request_id,
                method=method,
                params=params or {},
                client_operation_timeout_ms=client_operation_timeout_ms,
                stream_requested=stream_requested,
                allow_background_execution=allow_background_execution,
                cancellation_token=cancellation_token or None,
            ),
        )

    @mcp.tool()
    def close_remote_session(session_id: str, request_id: str = "") -> dict:
        payload = {
            "session_id": session_id,
            "request_id": request_id,
        }
        return _tool_call(
            tool_name="close_remote_session",
            payload=payload,
            fn=lambda: svc.close_remote_session(
                session_id=session_id,
                request_id=request_id or None,
            ),
        )

    @mcp.tool()
    def poll_remote_operation(session_id: str, operation_id: str, continuation_token: str = "") -> dict:
        payload = {
            "session_id": session_id,
            "operation_id": operation_id,
            "continuation_token": continuation_token,
        }
        return _tool_call(
            tool_name="poll_remote_operation",
            payload=payload,
            fn=lambda: svc.poll_remote_operation(
                session_id=session_id,
                operation_id=operation_id,
                continuation_token=continuation_token or None,
            ),
        )

    @mcp.tool()
    def cancel_remote_operation(
        session_id: str,
        operation_id: str,
        request_id: str,
        reason: str,
    ) -> dict:
        payload = {
            "session_id": session_id,
            "operation_id": operation_id,
            "request_id": request_id,
            "reason": reason,
        }
        return _tool_call(
            tool_name="cancel_remote_operation",
            payload=payload,
            fn=lambda: svc.cancel_remote_operation(
                session_id=session_id,
                operation_id=operation_id,
                request_id=request_id,
                reason=reason,
            ),
        )

    @mcp.tool()
    def get_server_registry(
        security_context: dict | None = None,
        include_hidden: bool = False,
    ) -> dict:
        payload = {
            "security_context": security_context or {},
            "include_hidden": include_hidden,
        }
        return _tool_call(
            tool_name="get_server_registry",
            payload=payload,
            fn=lambda: svc.get_server_registry(
                security_context=security_context or None,
                include_hidden=include_hidden,
            ),
        )

    @mcp.tool()
    def register_remote_server(
        server_label: str,
        interface_profile_ids: list[str],
        tool_families: list[str],
        routing_target: str,
        security_context: dict,
        publication_state: str = "published",
        metadata: dict | None = None,
    ) -> dict:
        payload = {
            "server_label": server_label,
            "interface_profile_ids": interface_profile_ids,
            "tool_families": tool_families,
            "routing_target": routing_target,
            "security_context": security_context,
            "publication_state": publication_state,
            "metadata": metadata or {},
        }
        return _tool_call(
            tool_name="register_remote_server",
            payload=payload,
            fn=lambda: svc.register_remote_server(
                server_label=server_label,
                interface_profile_ids=interface_profile_ids,
                tool_families=tool_families,
                routing_target=routing_target,
                security_context=security_context,
                publication_state=publication_state,
                metadata=metadata or None,
            ),
        )

    @mcp.tool()
    def update_remote_server_lifecycle(
        server_id: str,
        action: str,
        security_context: dict,
        reason: str = "",
    ) -> dict:
        payload = {
            "server_id": server_id,
            "action": action,
            "security_context": security_context,
            "reason": reason,
        }
        return _tool_call(
            tool_name="update_remote_server_lifecycle",
            payload=payload,
            fn=lambda: svc.update_remote_server_lifecycle(
                server_id=server_id,
                action=action,
                security_context=security_context,
                reason=reason or None,
            ),
        )

    @mcp.tool()
    def report_remote_server_health(
        server_id: str,
        health_state: str,
        security_context: dict,
        summary: str = "",
        metrics: dict | None = None,
    ) -> dict:
        payload = {
            "server_id": server_id,
            "health_state": health_state,
            "security_context": security_context,
            "summary": summary,
            "metrics": metrics or {},
        }
        return _tool_call(
            tool_name="report_remote_server_health",
            payload=payload,
            fn=lambda: svc.report_remote_server_health(
                server_id=server_id,
                health_state=health_state,
                security_context=security_context,
                summary=summary or None,
                metrics=metrics or None,
            ),
        )

    @mcp.tool()
    def resolve_gateway_route(
        tool_name: str,
        interface_profile_id: str = "service_internal_v0",
        preferred_server_id: str = "",
        security_context: dict | None = None,
    ) -> dict:
        payload = {
            "tool_name": tool_name,
            "interface_profile_id": interface_profile_id,
            "preferred_server_id": preferred_server_id,
            "security_context": security_context or {},
        }
        return _tool_call(
            tool_name="resolve_gateway_route",
            payload=payload,
            fn=lambda: svc.resolve_gateway_route(
                tool_name=tool_name,
                interface_profile_id=interface_profile_id,
                preferred_server_id=preferred_server_id or None,
                security_context=security_context or None,
            ),
        )

    @mcp.tool()
    def list_dialects() -> Any:
        return _tool_call(
            tool_name="list_dialects",
            payload={},
            fn=lambda: {"dialects": svc.list_dialects()},
        )

    @mcp.tool()
    def list_schemas(dialect: str, database: str = "") -> Any:
        return _tool_call(
            tool_name="list_schemas",
            payload={"dialect": dialect, "database": database},
            fn=lambda: {
                "schemas": svc.list_schemas(dialect=dialect, database=database or None),
            },
        )

    @mcp.tool()
    def list_tables(dialect: str, schema: str) -> Any:
        return _tool_call(
            tool_name="list_tables",
            payload={"dialect": dialect, "schema": schema},
            fn=lambda: {"tables": svc.list_tables(dialect=dialect, schema=schema)},
        )

    @mcp.tool()
    def describe_table(dialect: str, schema: str, table: str) -> Any:
        return _tool_call(
            tool_name="describe_table",
            payload={"dialect": dialect, "schema": schema, "table": table},
            fn=lambda: svc.describe_table(dialect=dialect, schema=schema, table=table),
        )

    @mcp.tool()
    def compile_query(dialect: str, query_text: str, context: dict | None = None) -> Any:
        payload = {
            "dialect": dialect,
            "query_text": query_text,
            "context": context or {},
        }
        return _tool_call(
            tool_name="compile_query",
            payload=payload,
            fn=lambda: svc.compile_query(
                dialect=dialect,
                query_text=query_text,
                context=context or {},
            ).to_dict(),
        )

    @mcp.tool()
    def execute_compiled(
        compile_artifact_id: str,
        options: dict | None = None,
        mode: str = "ai_analysis",
        approval_token: str = "",
    ) -> Any:
        payload = {
            "compile_artifact_id": compile_artifact_id,
            "options": options or {},
            "mode": mode,
            "approval_token_present": bool(approval_token),
        }
        return _tool_call(
            tool_name="execute_compiled",
            payload=payload,
            fn=lambda: svc.execute_compiled(
                compile_artifact_id=compile_artifact_id,
                options=options or {},
                mode=mode,
                approval_token=approval_token or None,
            ).to_dict(),
        )

    @mcp.tool()
    def execute_readonly_query(
        dialect: str,
        query_text: str,
        security_context: dict,
        options: dict | None = None,
        context: dict | None = None,
    ) -> Any:
        request_id = f"req_{uuid4().hex}"
        payload = {
            "request_id": request_id,
            "dialect": dialect,
            "query_text": query_text,
            "security_context": security_context,
            "options": options or {},
            "context": context or {},
        }
        return _tool_call(
            tool_name="execute_readonly_query",
            payload=payload,
            fn=lambda: svc.execute_readonly_query(
                request_id=request_id,
                dialect=dialect,
                query_text=query_text,
                security_context=security_context,
                options=options or {},
                context=context or {},
            ),
        )

    @mcp.tool()
    def execute_mutation(
        dialect: str,
        query_text: str,
        security_context: dict,
        approval_evidence: dict,
        options: dict | None = None,
        context: dict | None = None,
    ) -> Any:
        request_id = f"req_{uuid4().hex}"
        payload = {
            "request_id": request_id,
            "dialect": dialect,
            "query_text": query_text,
            "security_context": security_context,
            "approval_evidence": approval_evidence,
            "options": options or {},
            "context": context or {},
        }
        return _tool_call(
            tool_name="execute_mutation",
            payload=payload,
            fn=lambda: svc.execute_mutation(
                request_id=request_id,
                dialect=dialect,
                query_text=query_text,
                security_context=security_context,
                approval_evidence=approval_evidence,
                options=options or {},
                context=context or {},
            ),
        )

    @mcp.tool()
    def run_query(
        dialect: str,
        query_text: str,
        mode: str = "ai_analysis",
        options: dict | None = None,
        context: dict | None = None,
        approval_token: str = "",
    ) -> Any:
        request_id = f"req_{uuid4().hex}"
        payload = {
            "request_id": request_id,
            "dialect": dialect,
            "query_text": query_text,
            "mode": mode,
            "options": options or {},
            "context": context or {},
            "approval_token_present": bool(approval_token),
        }
        return _tool_call(
            tool_name="run_query",
            payload=payload,
            fn=lambda: svc.run_query(
                request_id=request_id,
                dialect=dialect,
                query_text=query_text,
                mode=mode,
                options=options or {},
                context=context or {},
                approval_token=approval_token or None,
            ).to_dict(),
        )

    @mcp.tool()
    def run_mutation(
        dialect: str,
        query_text: str,
        approval_token: str,
        options: dict | None = None,
        context: dict | None = None,
    ) -> Any:
        security_context = {}
        if isinstance(context, dict):
            raw = context.get("security_context", context)
            if isinstance(raw, dict):
                security_context = raw
        request_id = f"req_{uuid4().hex}"
        payload = {
            "request_id": request_id,
            "dialect": dialect,
            "query_text": query_text,
            "security_context": security_context,
            "options": options or {},
            "context": context or {},
            "approval_token_present": bool(approval_token),
        }
        return _tool_call(
            tool_name="run_mutation",
            payload=payload,
            fn=lambda: svc.execute_mutation(
                request_id=request_id,
                dialect=dialect,
                query_text=query_text,
                security_context=security_context,
                approval_evidence={"approval_token": approval_token},
                options=options or {},
                context=context or {},
            ),
        )

    @mcp.tool()
    def explain_query(
        dialect: str,
        query_text: str,
        security_context: dict | None = None,
        context: dict | None = None,
    ) -> Any:
        payload = {
            "dialect": dialect,
            "query_text": query_text,
            "security_context": security_context or {},
            "context": context or {},
        }
        return _tool_call(
            tool_name="explain_query",
            payload=payload,
            fn=lambda: svc.introspect_plan(
                dialect=dialect,
                query_text=query_text,
                security_context=security_context or {},
            ),
        )

    @mcp.tool()
    def create_vector_index(
        index_id: str,
        dimension: int,
        security_context: dict,
        profile_id: str = "client_supplied_embeddings_v0",
    ) -> Any:
        payload = {
            "index_id": index_id,
            "dimension": dimension,
            "security_context": security_context,
            "profile_id": profile_id,
        }
        return _tool_call(
            tool_name="create_vector_index",
            payload=payload,
            fn=lambda: svc.create_vector_index(
                index_id=index_id,
                dimension=dimension,
                security_context=security_context,
                profile_id=profile_id,
            ),
        )

    @mcp.tool()
    def list_vector_indexes(
        security_context: dict,
        include_deleted: bool = False,
    ) -> Any:
        payload = {
            "security_context": security_context,
            "include_deleted": include_deleted,
        }
        return _tool_call(
            tool_name="list_vector_indexes",
            payload=payload,
            fn=lambda: svc.list_vector_indexes(
                security_context=security_context,
                include_deleted=include_deleted,
            ),
        )

    @mcp.tool()
    def describe_vector_index(index_id: str, security_context: dict) -> Any:
        payload = {
            "index_id": index_id,
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="describe_vector_index",
            payload=payload,
            fn=lambda: svc.describe_vector_index(
                index_id=index_id,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def add_embeddings(
        index_id: str,
        dimension: int,
        records: list[dict],
        security_context: dict,
    ) -> Any:
        payload = {
            "index_id": index_id,
            "dimension": dimension,
            "record_count": len(records),
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="add_embeddings",
            payload=payload,
            fn=lambda: svc.add_embeddings(
                index_id=index_id,
                dimension=dimension,
                records=records,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def add_generated_embeddings(
        index_id: str,
        dimension: int,
        records: list[dict],
        provider_config: dict,
        security_context: dict,
    ) -> Any:
        provider_payload = dict(provider_config)
        if "api_key" in provider_payload:
            provider_payload["api_key"] = "***"
        payload = {
            "index_id": index_id,
            "dimension": dimension,
            "record_count": len(records),
            "provider_config": provider_payload,
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="add_generated_embeddings",
            payload=payload,
            fn=lambda: svc.add_generated_embeddings(
                index_id=index_id,
                dimension=dimension,
                records=records,
                provider_config=provider_config,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def delete_embeddings(index_id: str, vector_ids: list[str], security_context: dict) -> Any:
        payload = {
            "index_id": index_id,
            "vector_ids": vector_ids,
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="delete_embeddings",
            payload=payload,
            fn=lambda: svc.delete_embeddings(
                index_id=index_id,
                vector_ids=vector_ids,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def reindex_vector_index(index_id: str, security_context: dict) -> Any:
        payload = {
            "index_id": index_id,
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="reindex_vector_index",
            payload=payload,
            fn=lambda: svc.reindex_vector_index(
                index_id=index_id,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def delete_vector_index(index_id: str, security_context: dict) -> Any:
        payload = {
            "index_id": index_id,
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="delete_vector_index",
            payload=payload,
            fn=lambda: svc.delete_vector_index(
                index_id=index_id,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def vector_search(
        index_id: str,
        query_embedding: list[float],
        top_k: int,
        security_context: dict,
        filters: dict | None = None,
        include_vectors: bool = False,
    ) -> Any:
        payload = {
            "index_id": index_id,
            "top_k": top_k,
            "security_context": security_context,
            "filters": filters or {},
            "include_vectors": include_vectors,
        }
        return _tool_call(
            tool_name="vector_search",
            payload=payload,
            fn=lambda: svc.vector_search(
                index_id=index_id,
                query_embedding=query_embedding,
                top_k=top_k,
                security_context=security_context,
                filters=filters or {},
                include_vectors=include_vectors,
            ),
        )

    @mcp.tool()
    def hybrid_search(
        dialect: str,
        query_text: str,
        query_embedding: list[float],
        vector_index_id: str,
        top_k: int,
        security_context: dict,
        sql_filter: dict | None = None,
        weights: dict | None = None,
        options: dict | None = None,
    ) -> Any:
        payload = {
            "dialect": dialect,
            "query_text": query_text,
            "vector_index_id": vector_index_id,
            "top_k": top_k,
            "security_context": security_context,
            "sql_filter": sql_filter or {},
            "weights": weights or {},
            "options": options or {},
        }
        return _tool_call(
            tool_name="hybrid_search",
            payload=payload,
            fn=lambda: svc.hybrid_search(
                dialect=dialect,
                query_text=query_text,
                query_embedding=query_embedding,
                vector_index_id=vector_index_id,
                top_k=top_k,
                security_context=security_context,
                sql_filter=sql_filter or {},
                weights=weights or {},
                options=options or {},
            ),
        )

    @mcp.tool()
    def replay_audit_bundle(
        bundle: dict,
        security_context: dict | None = None,
        expected_policy_decision: str = "",
        expected_policy_rule_id: str = "",
        expected_plan_hash: str = "",
    ) -> Any:
        payload = {
            "bundle": bundle,
            "security_context": security_context or {},
            "expected_policy_decision": expected_policy_decision,
            "expected_policy_rule_id": expected_policy_rule_id,
            "expected_plan_hash": expected_plan_hash,
        }
        return _tool_call(
            tool_name="replay_audit_bundle",
            payload=payload,
            fn=lambda: svc.replay_audit_bundle(
                bundle=bundle,
                security_context=security_context,
                expected_policy_decision=expected_policy_decision or None,
                expected_policy_rule_id=expected_policy_rule_id or None,
                expected_plan_hash=expected_plan_hash or None,
            ),
        )

    @mcp.tool()
    def list_audit_bundles(
        security_context: dict,
        limit: int = 100,
    ) -> Any:
        payload = {
            "security_context": security_context,
            "limit": limit,
        }
        return _tool_call(
            tool_name="list_audit_bundles",
            payload=payload,
            fn=lambda: svc.list_audit_bundles(
                security_context=security_context,
                limit=limit,
            ),
        )

    @mcp.tool()
    def validate_approval_evidence(
        approval_evidence: dict,
        security_context: dict,
        statement_hash: str,
    ) -> Any:
        payload = {
            "approval_evidence": approval_evidence,
            "security_context": security_context,
            "statement_hash": statement_hash,
        }
        return _tool_call(
            tool_name="validate_approval_evidence",
            payload=payload,
            fn=lambda: svc.validate_approval_evidence(
                approval_evidence=approval_evidence,
                security_context=security_context,
                statement_hash=statement_hash,
            ),
        )

    @mcp.tool()
    def list_approval_records(
        security_context: dict,
        tenant_id: str = "",
        actor_id: str = "",
        include_revoked: bool = True,
    ) -> Any:
        payload = {
            "security_context": security_context,
            "tenant_id": tenant_id,
            "actor_id": actor_id,
            "include_revoked": include_revoked,
        }
        return _tool_call(
            tool_name="list_approval_records",
            payload=payload,
            fn=lambda: svc.list_approval_records(
                security_context=security_context,
                tenant_id=tenant_id or None,
                actor_id=actor_id or None,
                include_revoked=include_revoked,
            ),
        )

    @mcp.tool()
    def revoke_approval_record(
        approval_id: str,
        reason: str,
        security_context: dict,
    ) -> Any:
        payload = {
            "approval_id": approval_id,
            "reason": reason,
            "security_context": security_context,
        }
        return _tool_call(
            tool_name="revoke_approval_record",
            payload=payload,
            fn=lambda: svc.revoke_approval_record(
                approval_id=approval_id,
                reason=reason,
                security_context=security_context,
            ),
        )

    @mcp.tool()
    def create_audit_attestation(
        security_context: dict,
        bundle: dict | None = None,
        attestation_mode: str = "",
        key_id: str = "",
        external_reference: str = "",
        metadata: dict | None = None,
    ) -> Any:
        payload = {
            "security_context": security_context,
            "bundle": bundle or {},
            "attestation_mode": attestation_mode,
            "key_id": key_id,
            "external_reference": external_reference,
            "metadata": metadata or {},
        }
        return _tool_call(
            tool_name="create_audit_attestation",
            payload=payload,
            fn=lambda: svc.create_audit_attestation(
                security_context=security_context,
                bundle=bundle,
                attestation_mode=attestation_mode or None,
                key_id=key_id or None,
                external_reference=external_reference or None,
                metadata=metadata or None,
            ),
        )

    @mcp.tool()
    def verify_audit_attestation(
        security_context: dict,
        bundle: dict,
        attestation: dict,
        shared_secret: str = "",
    ) -> Any:
        payload = {
            "security_context": security_context,
            "bundle": bundle,
            "attestation": attestation,
            "shared_secret_present": bool(shared_secret),
        }
        return _tool_call(
            tool_name="verify_audit_attestation",
            payload=payload,
            fn=lambda: svc.verify_audit_attestation(
                security_context=security_context,
                bundle=bundle,
                attestation=attestation,
                shared_secret=shared_secret or None,
            ),
        )

    @mcp.tool()
    def get_runtime_diagnostics(
        security_context: dict,
        max_recent_errors: int = 10,
    ) -> Any:
        payload = {
            "security_context": security_context,
            "max_recent_errors": max_recent_errors,
        }
        return _tool_call(
            tool_name="get_runtime_diagnostics",
            payload=payload,
            fn=lambda: svc.get_runtime_diagnostics(
                security_context=security_context,
                max_recent_errors=max_recent_errors,
            ),
        )

    @mcp.tool()
    def generate_operator_runbook_bundle(
        security_context: dict,
        output_dir: str = "",
        max_recent_errors: int = 10,
        target_profiles: list[str] | None = None,
    ) -> Any:
        payload = {
            "security_context": security_context,
            "output_dir": output_dir,
            "max_recent_errors": max_recent_errors,
            "target_profiles": target_profiles or [],
        }
        return _tool_call(
            tool_name="generate_operator_runbook_bundle",
            payload=payload,
            fn=lambda: svc.generate_operator_runbook_bundle(
                security_context=security_context,
                output_dir=output_dir or None,
                max_recent_errors=max_recent_errors,
                target_profiles=target_profiles,
            ),
        )

    return mcp


def main() -> None:
    server = create_server()
    server.run()


if __name__ == "__main__":
    main()
