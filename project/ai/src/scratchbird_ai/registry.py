# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Registry, gateway, and lifecycle primitives for ScratchBird AI surfaces."""

from __future__ import annotations

from dataclasses import dataclass, field
from datetime import datetime, timezone
from typing import Any

from .deterministic import deterministic_id
from .tool_schema import ToolContractError


REGISTRY_SCHEMA_VERSION = "2026-04-20"
CAPABILITY_MANIFEST_VERSION = REGISTRY_SCHEMA_VERSION

PUBLICATION_STATES = {"published", "hidden"}
LIFECYCLE_STATES = {"enabled", "disabled", "draining", "retired"}
HEALTH_STATES = {"healthy", "degraded", "unknown", "failed", "retired"}
ROUTING_TARGETS = {
    "http_bridge_runtime",
    "local_service",
    "manager_control",
    "native_control",
    "remote_server",
}

ADMIN_ROLES = {"sysarch", "sysadmin", "cluster_admin"}
ADMIN_GRANTS = {
    "admin:manage",
    "admin:listener_control",
    "mcp:registry:admin",
    "mcp:registry:write",
}

TOOL_FAMILY_BY_TOOL_NAME = {
    "get_capabilities": "capabilities",
    "get_tool_descriptors": "capabilities",
    "get_provider_profiles": "capabilities",
    "get_compatibility_manifest": "capabilities",
    "export_certification_manifest": "governance",
    "negotiate_compatibility": "gateway_routing",
    "open_remote_session": "remote_session",
    "invoke_remote_tool": "remote_session",
    "close_remote_session": "remote_session",
    "poll_remote_operation": "remote_session",
    "cancel_remote_operation": "remote_session",
    "get_server_registry": "registry_governance",
    "register_remote_server": "registry_governance",
    "update_remote_server_lifecycle": "registry_governance",
    "report_remote_server_health": "registry_governance",
    "resolve_gateway_route": "gateway_routing",
    "list_dialects": "metadata",
    "list_schemas": "metadata",
    "list_tables": "metadata",
    "describe_table": "metadata",
    "compile_query": "query_read",
    "execute_compiled": "query_read",
    "execute_readonly_query": "query_read",
    "execute_mutation": "query_mutation",
    "run_query": "query_read",
    "run_mutation": "query_mutation",
    "explain_query": "query_read",
    "create_vector_index": "retrieval",
    "list_vector_indexes": "retrieval",
    "describe_vector_index": "retrieval",
    "add_embeddings": "retrieval",
    "add_generated_embeddings": "retrieval",
    "delete_embeddings": "retrieval",
    "reindex_vector_index": "retrieval",
    "delete_vector_index": "retrieval",
    "vector_search": "retrieval",
    "hybrid_search": "retrieval",
    "replay_audit_bundle": "governance",
    "list_audit_bundles": "governance",
    "validate_approval_evidence": "governance",
    "list_approval_records": "governance",
    "verify_audit_attestation": "governance",
    "revoke_approval_record": "operator_governance",
    "create_audit_attestation": "operator_governance",
    "get_runtime_diagnostics": "operator_governance",
    "generate_operator_runbook_bundle": "operator_governance",
}

TOOL_FAMILY_DEFINITIONS: dict[str, dict[str, Any]] = {
    "capabilities": {
        "visibility_scope": "public",
        "execution_scope": "open",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "metadata": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "query_read": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "query_mutation": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
        "approval_required": True,
    },
    "retrieval": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "governance": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "operator_governance": {
        "visibility_scope": "admin_only",
        "execution_scope": "admin_only",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "remote_session": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "registry_governance": {
        "visibility_scope": "admin_only",
        "execution_scope": "admin_only",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "gateway_routing": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "local_service",
        "availability_state": "implemented",
    },
    "graph_ops": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "native_control",
        "availability_state": "catalog_only",
    },
    "branch_ops": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "native_control",
        "availability_state": "unsupported",
    },
    "bridge_runtime": {
        "visibility_scope": "authenticated",
        "execution_scope": "server_policy",
        "routing_target": "http_bridge_runtime",
        "availability_state": "catalog_only",
    },
    "native_admin": {
        "visibility_scope": "admin_only",
        "execution_scope": "admin_only",
        "routing_target": "manager_control",
        "availability_state": "catalog_only",
    },
}


def _utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def _normalize_string_list(value: Any) -> tuple[str, ...]:
    if value is None:
        return ()
    if not isinstance(value, (list, tuple)):
        raise ToolContractError(
            error_code="E_TOOL_INPUT_INVALID",
            message="expected a list of strings",
            policy_rule_id="REGISTRY-INPUT-001",
        )
    normalized: list[str] = []
    for item in value:
        text = str(item).strip()
        if text:
            normalized.append(text)
    return tuple(sorted(dict.fromkeys(normalized)))


def _normalized_membership(value: dict[str, Any] | None, field: str) -> set[str]:
    if not isinstance(value, dict):
        return set()
    raw = value.get(field, [])
    if not isinstance(raw, list):
        return set()
    return {str(item).strip().lower() for item in raw if str(item).strip()}


def is_authenticated_context(security_context: dict[str, Any] | None) -> bool:
    if not isinstance(security_context, dict):
        return False
    return bool(str(security_context.get("tenant_id", "")).strip()) and bool(
        str(security_context.get("actor_id", "")).strip()
    )


def is_admin_context(security_context: dict[str, Any] | None) -> bool:
    roles = _normalized_membership(security_context, "roles")
    grants = _normalized_membership(security_context, "grants")
    return bool(roles & ADMIN_ROLES) or bool(grants & ADMIN_GRANTS)


def require_registry_admin(security_context: dict[str, Any] | None) -> None:
    if not is_admin_context(security_context):
        raise ToolContractError(
            error_code="E_POLICY_DENY",
            message="registry management requires ScratchBird administrative authority",
            policy_rule_id="REGISTRY-AUTH-001",
        )


def _compute_manifest_id(
    *,
    server_id: str,
    interface_profile_ids: tuple[str, ...],
    tool_families: list[dict[str, Any]],
    metadata: dict[str, Any] | None = None,
) -> str:
    return deterministic_id(
        "mf",
        {
            "server_id": server_id,
            "interface_profile_ids": interface_profile_ids,
            "tool_families": tool_families,
            "metadata": metadata or {},
            "manifest_version": CAPABILITY_MANIFEST_VERSION,
        },
    )


def build_local_tool_families(
    *,
    tool_names: tuple[str, ...],
    supports: dict[str, Any],
) -> list[dict[str, Any]]:
    tool_names_set = set(tool_names)
    rows: list[dict[str, Any]] = []
    for family_id, family in TOOL_FAMILY_DEFINITIONS.items():
        mapped_tools = sorted(
            tool_name
            for tool_name, mapped_family in TOOL_FAMILY_BY_TOOL_NAME.items()
            if mapped_family == family_id and tool_name in tool_names_set
        )
        availability_state = str(family["availability_state"])
        publication_state = "published"
        if family_id == "branch_ops":
            publication_state = "hidden"
        if family_id == "graph_ops" and not supports.get("graph_ops", False):
            publication_state = "hidden"
            availability_state = "unsupported"
        if family_id == "bridge_runtime" and not supports.get("bridge_runtime", False):
            publication_state = "hidden"
            availability_state = "unsupported"
        if family_id == "native_admin" and not supports.get("native_admin", True):
            publication_state = "hidden"
            availability_state = "unsupported"
        if family_id not in {"graph_ops", "branch_ops", "bridge_runtime", "native_admin"} and not mapped_tools:
            continue
        rows.append(
            {
                "tool_family": family_id,
                "publication_state": publication_state,
                "availability_state": availability_state,
                "routing_target": family["routing_target"],
                "visibility_scope": family["visibility_scope"],
                "execution_authority_model": (
                    "admin_required"
                    if family["execution_scope"] == "admin_only"
                    else ("open" if family["execution_scope"] == "open" else "server_policy_grants_groups")
                ),
                "tool_names": mapped_tools,
                "approval_required": bool(family.get("approval_required", False)),
            }
        )
    return rows


@dataclass(slots=True)
class RegisteredServer:
    server_id: str
    server_label: str
    server_kind: str
    routing_target: str
    interface_profile_ids: tuple[str, ...]
    capability_manifest_id: str
    capability_manifest_version: str
    publication_state: str
    lifecycle_state: str
    health_state: str
    tool_families: list[dict[str, Any]]
    governance_scope: str
    execution_authority_model: str
    metadata: dict[str, Any] = field(default_factory=dict)
    registered_at: str | None = None
    last_reported_at: str | None = None

    def to_dict(self) -> dict[str, Any]:
        return {
            "server_id": self.server_id,
            "server_label": self.server_label,
            "server_kind": self.server_kind,
            "routing_target": self.routing_target,
            "interface_profile_ids": list(self.interface_profile_ids),
            "capability_manifest_id": self.capability_manifest_id,
            "capability_manifest_version": self.capability_manifest_version,
            "publication_state": self.publication_state,
            "lifecycle_state": self.lifecycle_state,
            "health_state": self.health_state,
            "tool_families": [dict(row) for row in self.tool_families],
            "governance_scope": self.governance_scope,
            "execution_authority_model": self.execution_authority_model,
            "metadata": dict(self.metadata),
            "registered_at": self.registered_at,
            "last_reported_at": self.last_reported_at,
        }


class ServerRegistry:
    """Registry of the local MCP server surface plus optional remote inventory."""

    def __init__(
        self,
        *,
        server_instance_name: str,
        deployment_scope: str,
        adapter_mode: str,
    ) -> None:
        self.schema_version = REGISTRY_SCHEMA_VERSION
        self.server_instance_name = server_instance_name.strip() or "scratchbird-ai"
        self.deployment_scope = deployment_scope.strip() or "local"
        self.adapter_mode = adapter_mode.strip() or "mock"
        self.local_server_id = deterministic_id(
            "srv",
            {
                "service": "scratchbird-ai",
                "instance_name": self.server_instance_name,
                "deployment_scope": self.deployment_scope,
                "adapter_mode": self.adapter_mode,
            },
        )
        self._remote_servers: dict[str, RegisteredServer] = {}

    def build_local_entry(
        self,
        *,
        interface_profile_ids: tuple[str, ...],
        tool_names: tuple[str, ...],
        supports: dict[str, Any],
        matrix_version: str,
        release_ceiling: str,
    ) -> RegisteredServer:
        tool_families = build_local_tool_families(tool_names=tool_names, supports=supports)
        metadata = {
            "service": "scratchbird-ai",
            "deployment_scope": self.deployment_scope,
            "adapter_mode": self.adapter_mode,
            "matrix_version": matrix_version,
            "release_ceiling": release_ceiling,
        }
        return RegisteredServer(
            server_id=self.local_server_id,
            server_label=self.server_instance_name,
            server_kind="local_service",
            routing_target="local_service",
            interface_profile_ids=interface_profile_ids,
            capability_manifest_id=_compute_manifest_id(
                server_id=self.local_server_id,
                interface_profile_ids=interface_profile_ids,
                tool_families=tool_families,
                metadata=metadata,
            ),
            capability_manifest_version=CAPABILITY_MANIFEST_VERSION,
            publication_state="published",
            lifecycle_state="enabled",
            health_state="healthy",
            tool_families=tool_families,
            governance_scope="shared_catalog",
            execution_authority_model="server_policy_grants_groups",
            metadata=metadata,
            registered_at=_utc_now_iso(),
            last_reported_at=_utc_now_iso(),
        )

    def summary(
        self,
        *,
        local_entry: RegisteredServer,
        visible_entries: list[dict[str, Any]] | None = None,
    ) -> dict[str, Any]:
        entries = visible_entries if visible_entries is not None else self.list_entries(local_entry=local_entry)
        return {
            "registry_schema_version": self.schema_version,
            "local_server_id": self.local_server_id,
            "registered_server_count": len(self._remote_servers) + 1,
            "visible_server_count": len(entries),
            "remote_server_count": len(self._remote_servers),
            "published_server_count": sum(
                1 for row in entries if str(row.get("publication_state")) == "published"
            ),
            "healthy_server_count": sum(
                1 for row in entries if str(row.get("health_state")) == "healthy"
            ),
        }

    def list_entries(
        self,
        *,
        local_entry: RegisteredServer,
        security_context: dict[str, Any] | None = None,
        include_hidden: bool = False,
    ) -> list[dict[str, Any]]:
        admin = is_admin_context(security_context)
        rows: list[dict[str, Any]] = []
        for entry in [local_entry, *sorted(self._remote_servers.values(), key=lambda row: row.server_id)]:
            rendered = self._render_entry(
                entry,
                security_context=security_context,
                include_hidden=include_hidden,
                admin=admin,
            )
            if rendered is not None:
                rows.append(rendered)
        return rows

    def register_remote_server(
        self,
        *,
        server_label: str,
        interface_profile_ids: Any,
        tool_families: Any,
        routing_target: str,
        security_context: dict[str, Any] | None,
        publication_state: str = "published",
        metadata: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        require_registry_admin(security_context)
        normalized_label = str(server_label).strip()
        if not normalized_label:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="server_label is required",
                policy_rule_id="REGISTRY-INPUT-002",
            )
        normalized_profiles = _normalize_string_list(interface_profile_ids)
        if not normalized_profiles:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="interface_profile_ids must include at least one profile",
                policy_rule_id="REGISTRY-INPUT-003",
            )
        normalized_families = _normalize_string_list(tool_families)
        if not normalized_families:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="tool_families must include at least one family",
                policy_rule_id="REGISTRY-INPUT-004",
            )
        if str(routing_target) not in ROUTING_TARGETS:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unsupported routing_target: {routing_target}",
                policy_rule_id="REGISTRY-INPUT-005",
            )
        if publication_state not in PUBLICATION_STATES:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unsupported publication_state: {publication_state}",
                policy_rule_id="REGISTRY-INPUT-006",
            )
        family_rows: list[dict[str, Any]] = []
        for family_id in normalized_families:
            definition = TOOL_FAMILY_DEFINITIONS.get(family_id)
            if definition is None:
                raise ToolContractError(
                    error_code="E_TOOL_INPUT_INVALID",
                    message=f"unsupported tool_family: {family_id}",
                    policy_rule_id="REGISTRY-INPUT-007",
                )
            family_rows.append(
                {
                    "tool_family": family_id,
                    "publication_state": publication_state,
                    "availability_state": definition["availability_state"],
                    "routing_target": str(routing_target),
                    "visibility_scope": definition["visibility_scope"],
                    "execution_authority_model": (
                        "admin_required"
                        if definition["execution_scope"] == "admin_only"
                        else (
                            "open"
                            if definition["execution_scope"] == "open"
                            else "server_policy_grants_groups"
                        )
                    ),
                    "tool_names": [],
                    "approval_required": bool(definition.get("approval_required", False)),
                }
            )
        server_id = deterministic_id(
            "srv",
            {
                "server_label": normalized_label,
                "routing_target": routing_target,
                "interface_profile_ids": normalized_profiles,
                "tool_families": normalized_families,
            },
        )
        if server_id in self._remote_servers:
            raise ToolContractError(
                error_code="E_COMPONENT_ALREADY_EXISTS",
                message=f"remote server already registered: {server_id}",
                policy_rule_id="REGISTRY-REGISTER-001",
            )
        metadata_payload = dict(metadata or {})
        now_utc = _utc_now_iso()
        entry = RegisteredServer(
            server_id=server_id,
            server_label=normalized_label,
            server_kind="remote_server",
            routing_target=str(routing_target),
            interface_profile_ids=normalized_profiles,
            capability_manifest_id=_compute_manifest_id(
                server_id=server_id,
                interface_profile_ids=normalized_profiles,
                tool_families=family_rows,
                metadata=metadata_payload,
            ),
            capability_manifest_version=CAPABILITY_MANIFEST_VERSION,
            publication_state=publication_state,
            lifecycle_state="enabled",
            health_state="unknown",
            tool_families=family_rows,
            governance_scope="remote_inventory",
            execution_authority_model="server_policy_grants_groups",
            metadata=metadata_payload,
            registered_at=now_utc,
            last_reported_at=now_utc,
        )
        self._remote_servers[server_id] = entry
        return entry.to_dict()

    def update_remote_server_lifecycle(
        self,
        *,
        server_id: str,
        action: str,
        security_context: dict[str, Any] | None,
        reason: str | None = None,
    ) -> dict[str, Any]:
        require_registry_admin(security_context)
        entry = self._remote_servers.get(str(server_id))
        if entry is None:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unknown remote server: {server_id}",
                policy_rule_id="REGISTRY-LIFECYCLE-001",
            )
        normalized_action = str(action).strip().lower()
        if normalized_action == "enable":
            if entry.lifecycle_state == "retired":
                raise ToolContractError(
                    error_code="E_POLICY_DENY",
                    message="retired remote server cannot be re-enabled",
                    policy_rule_id="REGISTRY-LIFECYCLE-002",
                )
            entry.lifecycle_state = "enabled"
        elif normalized_action == "disable":
            entry.lifecycle_state = "disabled"
        elif normalized_action == "drain":
            entry.lifecycle_state = "draining"
        elif normalized_action == "retire":
            entry.lifecycle_state = "retired"
            entry.health_state = "retired"
        elif normalized_action == "publish":
            entry.publication_state = "published"
        elif normalized_action == "hide":
            entry.publication_state = "hidden"
        else:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unsupported lifecycle action: {action}",
                policy_rule_id="REGISTRY-LIFECYCLE-003",
            )
        if reason:
            entry.metadata["last_lifecycle_reason"] = str(reason)
        entry.last_reported_at = _utc_now_iso()
        return entry.to_dict()

    def report_remote_server_health(
        self,
        *,
        server_id: str,
        health_state: str,
        security_context: dict[str, Any] | None,
        summary: str | None = None,
        metrics: dict[str, Any] | None = None,
    ) -> dict[str, Any]:
        require_registry_admin(security_context)
        entry = self._remote_servers.get(str(server_id))
        if entry is None:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unknown remote server: {server_id}",
                policy_rule_id="REGISTRY-HEALTH-001",
            )
        normalized_state = str(health_state).strip()
        if normalized_state not in HEALTH_STATES:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unsupported health_state: {health_state}",
                policy_rule_id="REGISTRY-HEALTH-002",
            )
        if entry.lifecycle_state == "retired" and normalized_state != "retired":
            raise ToolContractError(
                error_code="E_POLICY_DENY",
                message="retired remote server may only report retired health state",
                policy_rule_id="REGISTRY-HEALTH-003",
            )
        entry.health_state = normalized_state
        if summary:
            entry.metadata["health_summary"] = str(summary)
        if metrics:
            entry.metadata["health_metrics"] = dict(metrics)
        entry.last_reported_at = _utc_now_iso()
        return entry.to_dict()

    def resolve_route(
        self,
        *,
        local_entry: RegisteredServer,
        tool_name: str,
        interface_profile_id: str,
        security_context: dict[str, Any] | None = None,
        preferred_server_id: str | None = None,
    ) -> dict[str, Any]:
        normalized_tool_name = str(tool_name).strip()
        if normalized_tool_name not in TOOL_FAMILY_BY_TOOL_NAME:
            raise ToolContractError(
                error_code="E_TOOL_NOT_FOUND",
                message=f"unknown tool: {normalized_tool_name}",
                policy_rule_id="REGISTRY-ROUTE-001",
            )
        server_id = str(preferred_server_id or "").strip()
        entry = local_entry if not server_id or server_id == local_entry.server_id else self._remote_servers.get(server_id)
        if entry is None:
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message=f"unknown server_id: {server_id}",
                policy_rule_id="REGISTRY-ROUTE-002",
            )
        rendered = self._render_entry(
            entry,
            security_context=security_context,
            include_hidden=True,
            admin=is_admin_context(security_context),
        )
        assert rendered is not None
        family_id = TOOL_FAMILY_BY_TOOL_NAME[normalized_tool_name]
        family_row = None
        for row in rendered["tool_families"]:
            if row["tool_family"] == family_id:
                family_row = row
                break
        if family_row is None:
            return {
                "status": "unsupported",
                "server_id": rendered["server_id"],
                "server_label": rendered["server_label"],
                "tool_family": family_id,
                "interface_profile_id": interface_profile_id,
                "routing_target": rendered["routing_target"],
                "capability_manifest_id": rendered["capability_manifest_id"],
                "publication_state": rendered["publication_state"],
                "lifecycle_state": rendered["lifecycle_state"],
                "health_state": rendered["health_state"],
                "request_visibility_state": "hidden",
                "execution_state": "unsupported",
                "resolution_mode": "not_supported",
                "proxy_execution_supported": False,
            }
        if interface_profile_id and interface_profile_id not in set(rendered["interface_profile_ids"]):
            return {
                "status": "unsupported",
                "server_id": rendered["server_id"],
                "server_label": rendered["server_label"],
                "tool_family": family_id,
                "interface_profile_id": interface_profile_id,
                "routing_target": rendered["routing_target"],
                "capability_manifest_id": rendered["capability_manifest_id"],
                "publication_state": rendered["publication_state"],
                "lifecycle_state": rendered["lifecycle_state"],
                "health_state": rendered["health_state"],
                "request_visibility_state": family_row["request_visibility_state"],
                "execution_state": "unsupported",
                "resolution_mode": "not_supported",
                "proxy_execution_supported": False,
            }
        if family_row["request_visibility_state"] != "visible":
            status = "hidden" if family_row["request_visibility_state"] == "hidden" else "denied"
            return {
                "status": status,
                "server_id": rendered["server_id"],
                "server_label": rendered["server_label"],
                "tool_family": family_id,
                "interface_profile_id": interface_profile_id,
                "routing_target": rendered["routing_target"],
                "capability_manifest_id": rendered["capability_manifest_id"],
                "publication_state": rendered["publication_state"],
                "lifecycle_state": rendered["lifecycle_state"],
                "health_state": rendered["health_state"],
                "request_visibility_state": family_row["request_visibility_state"],
                "execution_state": family_row["execution_state"],
                "resolution_mode": "not_routable",
                "proxy_execution_supported": False,
            }
        if rendered["lifecycle_state"] in {"disabled", "draining", "retired"}:
            return {
                "status": rendered["lifecycle_state"],
                "server_id": rendered["server_id"],
                "server_label": rendered["server_label"],
                "tool_family": family_id,
                "interface_profile_id": interface_profile_id,
                "routing_target": rendered["routing_target"],
                "capability_manifest_id": rendered["capability_manifest_id"],
                "publication_state": rendered["publication_state"],
                "lifecycle_state": rendered["lifecycle_state"],
                "health_state": rendered["health_state"],
                "request_visibility_state": family_row["request_visibility_state"],
                "execution_state": family_row["execution_state"],
                "resolution_mode": "not_routable",
                "proxy_execution_supported": False,
            }
        if family_row["availability_state"] == "unsupported":
            return {
                "status": "unsupported",
                "server_id": rendered["server_id"],
                "server_label": rendered["server_label"],
                "tool_family": family_id,
                "interface_profile_id": interface_profile_id,
                "routing_target": rendered["routing_target"],
                "capability_manifest_id": rendered["capability_manifest_id"],
                "publication_state": rendered["publication_state"],
                "lifecycle_state": rendered["lifecycle_state"],
                "health_state": rendered["health_state"],
                "request_visibility_state": family_row["request_visibility_state"],
                "execution_state": family_row["execution_state"],
                "resolution_mode": "not_supported",
                "proxy_execution_supported": False,
            }
        if rendered["server_kind"] == "remote_server":
            return {
                "status": "inventory_only",
                "server_id": rendered["server_id"],
                "server_label": rendered["server_label"],
                "tool_family": family_id,
                "interface_profile_id": interface_profile_id,
                "routing_target": rendered["routing_target"],
                "capability_manifest_id": rendered["capability_manifest_id"],
                "publication_state": rendered["publication_state"],
                "lifecycle_state": rendered["lifecycle_state"],
                "health_state": rendered["health_state"],
                "request_visibility_state": family_row["request_visibility_state"],
                "execution_state": family_row["execution_state"],
                "resolution_mode": "external_inventory",
                "proxy_execution_supported": False,
            }
        return {
            "status": "allowed",
            "server_id": rendered["server_id"],
            "server_label": rendered["server_label"],
            "tool_family": family_id,
            "interface_profile_id": interface_profile_id,
            "routing_target": rendered["routing_target"],
            "capability_manifest_id": rendered["capability_manifest_id"],
            "publication_state": rendered["publication_state"],
            "lifecycle_state": rendered["lifecycle_state"],
            "health_state": rendered["health_state"],
            "request_visibility_state": family_row["request_visibility_state"],
            "execution_state": family_row["execution_state"],
            "resolution_mode": "local_execution",
            "proxy_execution_supported": True,
        }

    def _render_entry(
        self,
        entry: RegisteredServer,
        *,
        security_context: dict[str, Any] | None,
        include_hidden: bool,
        admin: bool,
    ) -> dict[str, Any] | None:
        authenticated = is_authenticated_context(security_context)
        if entry.publication_state == "hidden" and not include_hidden and not admin:
            return None
        rendered_families: list[dict[str, Any]] = []
        for family in entry.tool_families:
            visibility_scope = str(family["visibility_scope"])
            if visibility_scope == "public":
                request_visibility_state = "visible"
            elif visibility_scope == "authenticated":
                request_visibility_state = "visible" if authenticated else "hidden"
            else:
                request_visibility_state = "visible" if admin else "hidden"
            if entry.publication_state == "hidden" and not admin:
                request_visibility_state = "hidden"
            if request_visibility_state == "hidden" and not include_hidden and not admin:
                continue
            execution_state = str(family["execution_authority_model"])
            if request_visibility_state != "visible":
                execution_state = "hidden"
            elif entry.lifecycle_state in {"disabled", "draining", "retired"}:
                execution_state = entry.lifecycle_state
            elif str(family["availability_state"]) == "unsupported":
                execution_state = "unsupported"
            elif str(family["execution_authority_model"]) == "admin_required" and not admin:
                execution_state = "admin_required"
            elif str(family["execution_authority_model"]) == "open":
                execution_state = "allowed"
            else:
                execution_state = "policy_controlled"
            family_row = dict(family)
            family_row["request_visibility_state"] = request_visibility_state
            family_row["execution_state"] = execution_state
            rendered_families.append(family_row)
        if not rendered_families and not admin and not include_hidden:
            return None
        rendered = entry.to_dict()
        rendered["tool_families"] = rendered_families
        rendered["route_state"] = (
            "available"
            if entry.lifecycle_state == "enabled" and entry.health_state in {"healthy", "degraded", "unknown"}
            else entry.lifecycle_state
        )
        return rendered
