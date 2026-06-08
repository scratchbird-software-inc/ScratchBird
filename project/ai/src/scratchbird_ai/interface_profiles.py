# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Canonical interface profile descriptors for ScratchBird AI."""

from __future__ import annotations

from dataclasses import dataclass
from typing import Any


INTERFACE_COMPATIBILITY_VERSION = "2026-03-07"


@dataclass(slots=True, frozen=True)
class InterfaceProfileDescriptor:
    profile_id: str
    family: str
    version: str
    state: str
    transport: str
    session_model: str
    auth_model: str
    operation_set: tuple[str, ...]
    streaming_mode: str
    compatibility_version: str = INTERFACE_COMPATIBILITY_VERSION
    evidence_gate: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "profile_id": self.profile_id,
            "family": self.family,
            "version": self.version,
            "state": self.state,
            "transport": self.transport,
            "session_model": self.session_model,
            "auth_model": self.auth_model,
            "operation_set": list(self.operation_set),
            "streaming_mode": self.streaming_mode,
            "compatibility_version": self.compatibility_version,
            "evidence_gate": self.evidence_gate,
        }


_CANONICAL_READ_OPS = (
    "discover_capabilities",
    "get_compatibility_manifest",
    "export_certification_manifest",
    "negotiate_compatibility",
    "discover_dialects",
    "discover_metadata",
    "compile_query",
    "execute_compiled",
    "execute_readonly_query",
    "explain_query",
)

_RETRIEVAL_QUERY_OPS = (
    "list_vector_indexes",
    "describe_vector_index",
    "vector_search",
    "hybrid_search",
)

_RETRIEVAL_WRITE_OPS = (
    "create_vector_index",
    "add_embeddings",
    "add_generated_embeddings",
    "delete_embeddings",
    "reindex_vector_index",
    "delete_vector_index",
)

_RETRIEVAL_OPS = _RETRIEVAL_QUERY_OPS + _RETRIEVAL_WRITE_OPS

_MUTATION_OPS = ("execute_mutation",)

_REMOTE_SESSION_OPS = (
    "open_remote_session",
    "invoke_remote_tool",
    "close_remote_session",
    "poll_remote_operation",
    "cancel_remote_operation",
)

_GOVERNANCE_OPS = (
    "replay_audit_bundle",
    "list_audit_bundles",
    "validate_approval_evidence",
    "list_approval_records",
    "verify_audit_attestation",
)

_OPERATOR_GOVERNANCE_OPS = (
    "revoke_approval_record",
    "create_audit_attestation",
    "get_runtime_diagnostics",
    "generate_operator_runbook_bundle",
)

_REGISTRY_OPS = (
    "get_server_registry",
    "register_remote_server",
    "update_remote_server_lifecycle",
    "report_remote_server_health",
    "resolve_gateway_route",
)


_INTERFACE_PROFILES = (
    InterfaceProfileDescriptor(
        profile_id="service_internal_v0",
        family="service_internal",
        version="v0",
        state="implemented",
        transport="in_process",
        session_model="request_scoped",
        auth_model="forwarded_security_context",
        operation_set=(
            _CANONICAL_READ_OPS
            + _MUTATION_OPS
            + _RETRIEVAL_OPS
            + _REGISTRY_OPS
            + _GOVERNANCE_OPS
            + _OPERATOR_GOVERNANCE_OPS
        ),
        streaming_mode="request_response",
        evidence_gate="EVID-03",
    ),
    InterfaceProfileDescriptor(
        profile_id="mcp_local_v0",
        family="mcp_local",
        version="v0",
        state="implemented",
        transport="stdio_jsonrpc",
        session_model="process_local_tool_session",
        auth_model="forwarded_security_context",
        operation_set=(
            _CANONICAL_READ_OPS
            + _MUTATION_OPS
            + _RETRIEVAL_OPS
            + _REGISTRY_OPS
            + _GOVERNANCE_OPS
            + _OPERATOR_GOVERNANCE_OPS
        ),
        streaming_mode="request_response",
        evidence_gate="EVID-03",
    ),
    InterfaceProfileDescriptor(
        profile_id="mcp_remote_v0",
        family="mcp_remote",
        version="v0",
        state="implemented",
        transport="https_json_request_response",
        session_model="remote_session_bound",
        auth_model="token_or_session_bound_identity",
        operation_set=(
            _REMOTE_SESSION_OPS
            + _CANONICAL_READ_OPS
            + _MUTATION_OPS
            + _RETRIEVAL_OPS
            + _REGISTRY_OPS
            + _GOVERNANCE_OPS
            + _OPERATOR_GOVERNANCE_OPS
        ),
        streaming_mode="server_stream",
    ),
    InterfaceProfileDescriptor(
        profile_id="langchain_v0",
        family="framework_adapter",
        version="v0",
        state="implemented",
        transport="in_process_sdk",
        session_model="adapter_request_scoped",
        auth_model="forwarded_security_context",
        operation_set=_CANONICAL_READ_OPS + _MUTATION_OPS + _RETRIEVAL_QUERY_OPS,
        streaming_mode="request_response",
    ),
    InterfaceProfileDescriptor(
        profile_id="llamaindex_v0",
        family="framework_adapter",
        version="v0",
        state="implemented",
        transport="in_process_sdk",
        session_model="adapter_request_scoped",
        auth_model="forwarded_security_context",
        operation_set=_CANONICAL_READ_OPS + _MUTATION_OPS + _RETRIEVAL_QUERY_OPS,
        streaming_mode="request_response",
    ),
    InterfaceProfileDescriptor(
        profile_id="semantic_kernel_v0",
        family="framework_adapter",
        version="v0",
        state="implemented",
        transport="in_process_sdk",
        session_model="adapter_request_scoped",
        auth_model="forwarded_security_context",
        operation_set=_CANONICAL_READ_OPS + _MUTATION_OPS + _RETRIEVAL_QUERY_OPS,
        streaming_mode="request_response",
    ),
    InterfaceProfileDescriptor(
        profile_id="provider_tool_calling_v0",
        family="provider_tool_calling",
        version="v0",
        state="implemented",
        transport="provider_http_api",
        session_model="provider_request_scoped",
        auth_model="provider_auth_plus_forwarded_security_context",
        operation_set=_CANONICAL_READ_OPS + _MUTATION_OPS + _RETRIEVAL_OPS,
        streaming_mode="request_response",
        evidence_gate="EVID-13",
    ),
    InterfaceProfileDescriptor(
        profile_id="streaming_async_v0",
        family="streaming_async",
        version="v0",
        state="implemented",
        transport="event_stream",
        session_model="operation_session",
        auth_model="forwarded_security_context",
        operation_set=(
            "execute_readonly_query",
            "execute_mutation",
            "explain_query",
            "poll_remote_operation",
            "cancel_remote_operation",
        )
        + _RETRIEVAL_QUERY_OPS,
        streaming_mode="server_stream",
    ),
    InterfaceProfileDescriptor(
        profile_id="retrieval_ingest_v0",
        family="retrieval_ingest",
        version="v0",
        state="implemented",
        transport="request_response",
        session_model="request_scoped",
        auth_model="forwarded_security_context",
        operation_set=_RETRIEVAL_OPS,
        streaming_mode="request_response",
    ),
    InterfaceProfileDescriptor(
        profile_id="governance_certification_v0",
        family="governance_certification",
        version="v0",
        state="implemented",
        transport="request_response",
        session_model="request_scoped",
        auth_model="forwarded_security_context",
        operation_set=(
            "execute_mutation",
        )
        + _GOVERNANCE_OPS
        + _OPERATOR_GOVERNANCE_OPS
        + _REGISTRY_OPS
        + (
            "export_certification_manifest",
        ),
        streaming_mode="request_response",
    ),
)


def get_interface_profiles() -> list[dict[str, Any]]:
    return [profile.to_dict() for profile in _INTERFACE_PROFILES]


def get_interface_profile_descriptors() -> tuple[InterfaceProfileDescriptor, ...]:
    return _INTERFACE_PROFILES
