# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Compatibility manifest and negotiation helpers for ScratchBird AI."""

from __future__ import annotations

from typing import Any

from .deterministic import deterministic_id
from .interface_profiles import (
    INTERFACE_COMPATIBILITY_VERSION,
    get_interface_profile_descriptors,
    get_interface_profiles,
)
from .scratchbird_core_surface import build_scratchbird_core_surface_packet
from .settings import RuntimeSettings


SERVICE_RELEASE_VERSION = "0.1.0"
COMPATIBILITY_MANIFEST_VERSION = INTERFACE_COMPATIBILITY_VERSION
COMPATIBILITY_RELEASE_DATE = "2026-04-18"


def build_compatibility_manifest(
    *,
    adapter_mode: str,
    matrix_version: str,
    runtime_settings: RuntimeSettings | None = None,
) -> dict[str, Any]:
    settings = runtime_settings or RuntimeSettings(adapter_mode=adapter_mode)
    scratchbird_core_surface = build_scratchbird_core_surface_packet()
    profile_entries = []
    for profile in get_interface_profiles():
        profile_entries.append(
            {
                "component": profile["profile_id"],
                "component_version": profile["version"],
                "support_state": "supported" if profile["state"] == "implemented" else "unsupported",
                "required_conditions": [],
                "failure_reason_code": (
                    None if profile["state"] == "implemented" else "INTERFACE-NOT-IMPLEMENTED"
                ),
                "evidence_gate": profile["evidence_gate"],
            }
        )

    return {
        "manifest_version": COMPATIBILITY_MANIFEST_VERSION,
        "release_version": SERVICE_RELEASE_VERSION,
        "release_date": COMPATIBILITY_RELEASE_DATE,
        "compatibility_version": INTERFACE_COMPATIBILITY_VERSION,
        "scratchbird_core_surface_packet": scratchbird_core_surface,
        "interface_profiles": profile_entries,
        "server_runtime_support": [
            {
                "component": "scratchbird_server",
                "version_range": ",".join(settings.supported_server_versions),
                "support_state": "supported",
                "required_conditions": ["authorization_enforced_by_server_policy"],
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            }
        ],
        "parser_compiler_support": [
            {
                "component": "native_parser_compiler",
                "version_range": ",".join(settings.supported_parser_compiler_versions),
                "support_state": "supported",
                "required_conditions": ["dialect=native"],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            }
        ],
        "driver_runtime_support": [
            {
                "component": "mcp_local_runtime",
                "version_range": ",".join(settings.supported_driver_runtime_versions),
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            },
            {
                "component": "http_bridge_runtime",
                "version_range": ",".join(settings.supported_driver_runtime_versions),
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            },
        ],
        "transport_support": [
            {
                "component": "in_process",
                "component_version": "v0",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            },
            {
                "component": "stdio_jsonrpc",
                "component_version": "v0",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            },
            {
                "component": "https_json_request_response",
                "component_version": "v0",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            },
            {
                "component": "https_sse_server_stream",
                "component_version": "v0",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            },
            {
                "component": "websocket_bidirectional",
                "component_version": "v0",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            },
        ],
        "remote_session_auth_support": [
            {
                "component": auth_type,
                "component_version": "v0",
                "support_state": "supported",
                "required_conditions": ["authorization_enforced_by_server_policy"],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            }
            for auth_type in settings.remote_mcp_supported_auth_types
        ],
        "native_control_surface_support": [
            {
                "component": "graph_ops",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            },
            {
                "component": "remote_mcp",
                "support_state": "supported",
                "required_conditions": ["authorization_enforced_by_server_policy"],
                "failure_reason_code": None,
                "evidence_gate": "EVID-03",
            },
            {
                "component": "http_bridge_runtime",
                "support_state": "supported",
                "required_conditions": [],
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            },
        ],
        "scratchbird_runtime_modes": [
            {
                "component": row["mode_id"],
                "component_version": scratchbird_core_surface["packet_version"],
                "support_state": row["support_state"],
                "required_conditions": list(row["required_conditions"]),
                "failure_reason_code": None,
                "evidence_gate": "EVID-02",
            }
            for row in scratchbird_core_surface["runtime_mode_truth_packet"]["admitted_modes"]
        ],
        "scratchbird_retrieval_metadata_discovery": {
            "catalog_namespace": scratchbird_core_surface["retrieval_metadata_discovery_packet"][
                "catalog_namespace"
            ],
            "relations": list(
                scratchbird_core_surface["retrieval_metadata_discovery_packet"]["relations"]
            ),
            "required_semantics": list(
                scratchbird_core_surface["retrieval_metadata_discovery_packet"][
                    "required_semantics"
                ]
            ),
        },
        "notes": [
            f"adapter_mode={adapter_mode}",
            f"matrix_version={matrix_version}",
            (
                "ScratchBird core owns the bounded current-tree retrieval and runtime truth; "
                "this checkout still requires a refreshed test database before live evidence "
                "can be regenerated"
            ),
            "unknown interface profiles or transports fail closed",
            "remote MCP transport is served by the HTTP bridge runtime and preserves session-bound identity context",
            "authorization is enforced by ScratchBird server policy, grants, and group membership rather than by AI-layer capability walls",
            "server, parser/compiler, and driver runtime versions fail closed when declared and unsupported",
        ],
    }


def negotiate_compatibility(
    request: dict[str, Any] | None,
    *,
    adapter_mode: str,
    matrix_version: str,
    runtime_settings: RuntimeSettings | None = None,
) -> dict[str, Any]:
    payload = dict(request or {})
    settings = runtime_settings or RuntimeSettings(adapter_mode=adapter_mode)
    manifest = build_compatibility_manifest(
        adapter_mode=adapter_mode,
        matrix_version=matrix_version,
        runtime_settings=settings,
    )
    profiles = {profile.profile_id: profile for profile in get_interface_profile_descriptors()}

    request_id = str(payload.get("request_id", "")).strip() or deterministic_id(
        "req",
        {
            "operation": "negotiate_compatibility",
            "adapter_mode": adapter_mode,
            "payload": payload,
        },
    )
    requested_profile = str(payload.get("interface_profile_id", "")).strip() or "service_internal_v0"
    requested_version = str(payload.get("requested_profile_version", "")).strip() or "v0"
    requested_transport = str(payload.get("requested_transport", "")).strip()
    client_component_versions = payload.get("client_component_versions", {})
    server_component_versions = payload.get("server_component_versions", {})
    driver_runtime_versions = payload.get("driver_runtime_versions", {})

    decisions: list[dict[str, Any]] = []
    warnings: list[str] = []

    profile = profiles.get(requested_profile)
    if profile is None:
        return _blocked_response(
            request_id=request_id,
            requested_profile=requested_profile,
            requested_transport=requested_transport,
            decisions=decisions,
            warnings=warnings,
            error_code="E_INTERFACE_PROFILE_UNSUPPORTED",
            reason_code="INTERFACE-PROFILE-UNKNOWN",
            message=f"Unsupported interface profile: {requested_profile}",
        )

    decisions.append(
        {
            "domain": "interface_profile",
            "component": profile.profile_id,
            "requested": requested_version,
            "resolved": profile.version,
            "support_state": "supported" if profile.state == "implemented" else "unsupported",
            "reason_code": None if profile.state == "implemented" else "INTERFACE-NOT-IMPLEMENTED",
        }
    )
    if profile.state != "implemented":
        return _blocked_response(
            request_id=request_id,
            requested_profile=profile.profile_id,
            requested_transport=requested_transport or profile.transport,
            decisions=decisions,
            warnings=warnings,
            error_code="E_INTERFACE_PROFILE_UNSUPPORTED",
            reason_code="INTERFACE-NOT-IMPLEMENTED",
            message=f"Interface profile is not implemented: {profile.profile_id}",
        )

    if requested_version != profile.version:
        return _blocked_response(
            request_id=request_id,
            requested_profile=profile.profile_id,
            requested_transport=requested_transport or profile.transport,
            decisions=decisions,
            warnings=warnings,
            error_code="E_INTERFACE_PROFILE_UNSUPPORTED",
            reason_code="INTERFACE-VERSION-MISMATCH",
            message=(
                f"Requested profile version {requested_version} is not supported for "
                f"{profile.profile_id}"
            ),
        )

    supported_transports = (
        settings.remote_mcp_supported_transports
        if profile.profile_id == "mcp_remote_v0"
        else (profile.transport,)
    )
    supported_transport_set = set(supported_transports)
    resolved_transport = (
        requested_transport
        if requested_transport and requested_transport in supported_transport_set
        else supported_transports[0]
    )
    if requested_transport:
        decisions.append(
            {
                "domain": "transport_profile",
                "component": requested_transport,
                "requested": requested_transport,
                "resolved": resolved_transport,
                "support_state": (
                    "supported" if requested_transport in supported_transport_set else "unsupported"
                ),
                "reason_code": (
                    None if requested_transport in supported_transport_set else "TRANSPORT-MISMATCH"
                ),
            }
        )
        if requested_transport not in supported_transport_set:
            return _blocked_response(
                request_id=request_id,
                requested_profile=profile.profile_id,
                requested_transport=requested_transport,
                decisions=decisions,
                warnings=warnings,
                error_code="E_TRANSPORT_PROFILE_UNSUPPORTED",
                reason_code="TRANSPORT-MISMATCH",
                message=(
                    f"Requested transport {requested_transport} is not supported for "
                    f"{profile.profile_id}"
                ),
            )
    else:
        decisions.append(
            {
                "domain": "transport_profile",
                "component": resolved_transport,
                "requested": resolved_transport,
                "resolved": resolved_transport,
                "support_state": "supported",
                "reason_code": None,
            }
        )

    repo_version = ""
    if isinstance(client_component_versions, dict):
        repo_version = str(
            client_component_versions.get(
                "scratchbird_ai",
                client_component_versions.get("repo_release", ""),
            )
        ).strip()
    if repo_version and repo_version != SERVICE_RELEASE_VERSION:
        decisions.append(
            {
                "domain": "repo_release",
                "component": "scratchbird_ai",
                "requested": repo_version,
                "resolved": SERVICE_RELEASE_VERSION,
                "support_state": "unsupported",
                "reason_code": "REPO-RELEASE-MISMATCH",
            }
        )
        return _blocked_response(
            request_id=request_id,
            requested_profile=profile.profile_id,
            requested_transport=resolved_transport,
            decisions=decisions,
            warnings=warnings,
            error_code="E_COMPONENT_VERSION_UNSUPPORTED",
            reason_code="REPO-RELEASE-MISMATCH",
            message=(
                f"Client repo release {repo_version} is not supported by "
                f"{SERVICE_RELEASE_VERSION}"
            ),
        )
    decisions.append(
        {
            "domain": "repo_release",
            "component": "scratchbird_ai",
            "requested": repo_version or SERVICE_RELEASE_VERSION,
            "resolved": SERVICE_RELEASE_VERSION,
            "support_state": "supported",
            "reason_code": None,
        }
    )

    if isinstance(server_component_versions, dict):
        server_version = str(server_component_versions.get("scratchbird_server", "")).strip()
        parser_version = str(
            server_component_versions.get("native_parser_compiler", "")
        ).strip()
        if server_version:
            supported_server_versions = set(settings.supported_server_versions)
            decisions.append(
                {
                    "domain": "server_runtime",
                    "component": "scratchbird_server",
                    "requested": server_version,
                    "resolved": ",".join(settings.supported_server_versions),
                    "support_state": (
                        "supported" if server_version in supported_server_versions else "unsupported"
                    ),
                    "reason_code": (
                        None if server_version in supported_server_versions else "SERVER-RUNTIME-MISMATCH"
                    ),
                }
            )
            if server_version not in supported_server_versions:
                return _blocked_response(
                    request_id=request_id,
                    requested_profile=profile.profile_id,
                    requested_transport=resolved_transport,
                    decisions=decisions,
                    warnings=warnings,
                    error_code="E_SERVER_RUNTIME_UNSUPPORTED",
                    reason_code="SERVER-RUNTIME-MISMATCH",
                    message=f"Unsupported ScratchBird server version: {server_version}",
                )
        if parser_version:
            supported_parser_versions = set(settings.supported_parser_compiler_versions)
            decisions.append(
                {
                    "domain": "parser_compiler_runtime",
                    "component": "native_parser_compiler",
                    "requested": parser_version,
                    "resolved": ",".join(settings.supported_parser_compiler_versions),
                    "support_state": (
                        "supported" if parser_version in supported_parser_versions else "unsupported"
                    ),
                    "reason_code": (
                        None
                        if parser_version in supported_parser_versions
                        else "PARSER-COMPILER-VERSION-MISMATCH"
                    ),
                }
            )
            if parser_version not in supported_parser_versions:
                return _blocked_response(
                    request_id=request_id,
                    requested_profile=profile.profile_id,
                    requested_transport=resolved_transport,
                    decisions=decisions,
                    warnings=warnings,
                    error_code="E_PARSER_COMPILER_UNSUPPORTED",
                    reason_code="PARSER-COMPILER-VERSION-MISMATCH",
                    message=f"Unsupported parser/compiler version: {parser_version}",
                )

    if isinstance(driver_runtime_versions, dict):
        for component in ("mcp_local_runtime", "http_bridge_runtime"):
            requested_driver_version = str(driver_runtime_versions.get(component, "")).strip()
            if not requested_driver_version:
                continue
            supported_driver_versions = set(settings.supported_driver_runtime_versions)
            decisions.append(
                {
                    "domain": "driver_runtime",
                    "component": component,
                    "requested": requested_driver_version,
                    "resolved": ",".join(settings.supported_driver_runtime_versions),
                    "support_state": (
                        "supported"
                        if requested_driver_version in supported_driver_versions
                        else "unsupported"
                    ),
                    "reason_code": (
                        None
                        if requested_driver_version in supported_driver_versions
                        else "DRIVER-RUNTIME-MISMATCH"
                    ),
                }
            )
            if requested_driver_version not in supported_driver_versions:
                return _blocked_response(
                    request_id=request_id,
                    requested_profile=profile.profile_id,
                    requested_transport=resolved_transport,
                    decisions=decisions,
                    warnings=warnings,
                    error_code="E_DRIVER_RUNTIME_UNSUPPORTED",
                    reason_code="DRIVER-RUNTIME-MISMATCH",
                    message=f"Unsupported driver/runtime version for {component}: {requested_driver_version}",
                )

    return {
        "request_id": request_id,
        "manifest_version": manifest["manifest_version"],
        "negotiation_status": "supported",
        "resolved_interface_profile_version": profile.version,
        "resolved_transport": resolved_transport,
        "compatibility_decisions": decisions,
        "warnings": warnings,
        "error": None,
    }


def _blocked_response(
    *,
    request_id: str,
    requested_profile: str,
    requested_transport: str,
    decisions: list[dict[str, Any]],
    warnings: list[str],
    error_code: str,
    reason_code: str,
    message: str,
) -> dict[str, Any]:
    trace_id = deterministic_id(
        "tr",
        {
            "operation": "negotiate_compatibility",
            "request_id": request_id,
            "profile": requested_profile,
            "transport": requested_transport,
            "error_code": error_code,
            "reason_code": reason_code,
        },
    )
    return {
        "request_id": request_id,
        "manifest_version": COMPATIBILITY_MANIFEST_VERSION,
        "negotiation_status": "blocked",
        "resolved_interface_profile_version": None,
        "resolved_transport": None,
        "compatibility_decisions": decisions,
        "warnings": warnings,
        "error": {
            "error_code": error_code,
            "reason_code": reason_code,
            "message": message,
            "trace_id": trace_id,
        },
    }
