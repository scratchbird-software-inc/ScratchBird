# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Runtime configuration for ScratchBird AI service wiring."""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


_ALLOWED_MODES = {"mock", "http", "hybrid"}
_DEFAULT_OPERATOR_TARGET_PROFILES = (
    "listener_direct",
    "manager_proxy",
    "local_ipc",
    "embedded_local_only",
)


def _parse_csv(value: str) -> tuple[str, ...]:
    parts = [item.strip() for item in value.split(",")]
    return tuple(item for item in parts if item)


def _default_runtime_root() -> Path:
    configured = os.getenv("SCRATCHBIRD_AI_RUNTIME_ROOT", "").strip()
    if configured:
        return Path(configured).expanduser()
    state_home = os.getenv("XDG_STATE_HOME", "").strip()
    if state_home:
        return Path(state_home).expanduser() / "scratchbird-ai" / "runtime"
    return Path.home() / ".local" / "state" / "scratchbird-ai" / "runtime"


@dataclass(slots=True)
class RuntimeSettings:
    adapter_mode: str = "mock"
    server_instance_name: str = "scratchbird-ai"
    deployment_scope: str = "local"
    http_base_url: str = "http://127.0.0.1:3095"
    http_timeout_sec: float = 10.0
    http_retry_attempts: int = 1
    http_retry_backoff_ms: int = 100
    http_circuit_breaker_failure_threshold: int = 3
    http_circuit_breaker_cooldown_sec: float = 30.0
    http_api_token: str | None = None
    http_dialects: tuple[str, ...] = ("native",)
    retrieval_catalog_path: str | None = None
    approval_ledger_path: str | None = str(
        _default_runtime_root() / "approval_ledger.json"
    )
    structured_event_log_path: str | None = str(
        _default_runtime_root() / "structured_events.jsonl"
    )
    operator_bundle_output_dir: str | None = str(
        _default_runtime_root() / "operator_bundle"
    )
    operator_target_profiles: tuple[str, ...] = _DEFAULT_OPERATOR_TARGET_PROFILES
    audit_attestation_mode: str = "hmac_sha256"
    audit_attestation_secret: str | None = None
    audit_attestation_attestor_id: str = "scratchbird-ai-local-attestor"
    audit_attestation_delegated_secret: str | None = None
    audit_attestation_delegated_attestor_id: str | None = None
    audit_attestation_external_reference_base_url: str | None = None
    compile_repair_max_attempts: int = 3
    operation_window_sec: int = 60
    max_requests_per_window: int = 100
    max_mutations_per_window: int = 20
    max_cost_units_per_window: int = 1000
    supported_server_versions: tuple[str, ...] = ("native-http-bridge-preview",)
    supported_parser_compiler_versions: tuple[str, ...] = ("native-only",)
    supported_driver_runtime_versions: tuple[str, ...] = ("builtin",)
    remote_mcp_auth_token: str | None = None
    remote_mcp_session_ttl_sec: int = 900
    remote_mcp_heartbeat_interval_sec: int = 30
    remote_mcp_protocol_versions: tuple[str, ...] = ("v0",)
    remote_mcp_supported_auth_types: tuple[str, ...] = (
        "bearer",
        "oauth2_access_token",
        "jwt_bearer",
        "workload_identity",
        "proxy_principal",
        "ldap_bind",
        "kerberos_gssapi",
        "radius_pap",
        "pam_conversation",
        "preauthenticated_context",
    )
    remote_mcp_supported_transports: tuple[str, ...] = (
        "https_json_request_response",
        "https_sse_server_stream",
        "websocket_bidirectional",
    )

    def normalized_mode(self) -> str:
        mode = self.adapter_mode.strip().lower()
        return mode if mode in _ALLOWED_MODES else "mock"

    def should_use_http_for_dialect(self, dialect: str) -> bool:
        mode = self.normalized_mode()
        if mode == "mock":
            return False
        if mode == "http":
            return True
        # hybrid mode
        return dialect in set(self.http_dialects)



def load_runtime_settings() -> RuntimeSettings:
    mode = os.getenv("SCRATCHBIRD_AI_ADAPTER_MODE", "mock").strip().lower()
    if mode not in _ALLOWED_MODES:
        mode = "mock"

    server_instance_name = os.getenv(
        "SCRATCHBIRD_AI_SERVER_INSTANCE_NAME",
        "scratchbird-ai",
    ).strip() or "scratchbird-ai"
    deployment_scope = os.getenv(
        "SCRATCHBIRD_AI_DEPLOYMENT_SCOPE",
        "local",
    ).strip() or "local"

    base_url = os.getenv("SCRATCHBIRD_AI_HTTP_BASE_URL", "http://127.0.0.1:3095").strip()

    timeout_raw = os.getenv("SCRATCHBIRD_AI_HTTP_TIMEOUT_SEC", "10")
    try:
        timeout = float(timeout_raw)
    except ValueError:
        timeout = 10.0

    retry_attempts_raw = os.getenv("SCRATCHBIRD_AI_HTTP_RETRY_ATTEMPTS", "1")
    try:
        retry_attempts = int(retry_attempts_raw)
    except ValueError:
        retry_attempts = 1

    retry_backoff_raw = os.getenv("SCRATCHBIRD_AI_HTTP_RETRY_BACKOFF_MS", "100")
    try:
        retry_backoff_ms = int(retry_backoff_raw)
    except ValueError:
        retry_backoff_ms = 100

    circuit_threshold_raw = os.getenv(
        "SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_FAILURE_THRESHOLD",
        "3",
    )
    try:
        circuit_threshold = int(circuit_threshold_raw)
    except ValueError:
        circuit_threshold = 3

    circuit_cooldown_raw = os.getenv(
        "SCRATCHBIRD_AI_HTTP_CIRCUIT_BREAKER_COOLDOWN_SEC",
        "30",
    )
    try:
        circuit_cooldown = float(circuit_cooldown_raw)
    except ValueError:
        circuit_cooldown = 30.0

    api_token = os.getenv("SCRATCHBIRD_AI_HTTP_API_TOKEN", "").strip() or None
    retrieval_catalog_path = os.getenv("SCRATCHBIRD_AI_RETRIEVAL_CATALOG_PATH", "").strip() or None
    approval_ledger_path = os.getenv(
        "SCRATCHBIRD_AI_APPROVAL_LEDGER_PATH",
        str(_default_runtime_root() / "approval_ledger.json"),
    ).strip() or None
    structured_event_log_path = os.getenv(
        "SCRATCHBIRD_AI_STRUCTURED_EVENT_LOG_PATH",
        str(_default_runtime_root() / "structured_events.jsonl"),
    ).strip() or None
    operator_bundle_output_dir = os.getenv(
        "SCRATCHBIRD_AI_OPERATOR_BUNDLE_OUTPUT_DIR",
        str(_default_runtime_root() / "operator_bundle"),
    ).strip() or None
    operator_target_profiles = _parse_csv(
        os.getenv(
            "SCRATCHBIRD_AI_OPERATOR_TARGET_PROFILES",
            ",".join(_DEFAULT_OPERATOR_TARGET_PROFILES),
        )
    ) or _DEFAULT_OPERATOR_TARGET_PROFILES
    audit_attestation_mode = os.getenv(
        "SCRATCHBIRD_AI_AUDIT_ATTESTATION_MODE",
        "hmac_sha256",
    ).strip() or "hmac_sha256"
    audit_attestation_secret = (
        os.getenv("SCRATCHBIRD_AI_AUDIT_ATTESTATION_SECRET", "").strip() or None
    )
    audit_attestation_attestor_id = os.getenv(
        "SCRATCHBIRD_AI_AUDIT_ATTESTATION_ATTESTOR_ID",
        "scratchbird-ai-local-attestor",
    ).strip() or "scratchbird-ai-local-attestor"
    audit_attestation_delegated_secret = (
        os.getenv("SCRATCHBIRD_AI_AUDIT_DELEGATED_ATTESTATION_SECRET", "").strip() or None
    )
    audit_attestation_delegated_attestor_id = (
        os.getenv("SCRATCHBIRD_AI_AUDIT_DELEGATED_ATTESTATION_ATTESTOR_ID", "").strip()
        or None
    )
    audit_attestation_external_reference_base_url = (
        os.getenv("SCRATCHBIRD_AI_AUDIT_EXTERNAL_REFERENCE_BASE_URL", "").strip() or None
    )

    compile_repair_attempts_raw = os.getenv("SCRATCHBIRD_AI_COMPILE_REPAIR_MAX_ATTEMPTS", "3")
    try:
        compile_repair_attempts = int(compile_repair_attempts_raw)
    except ValueError:
        compile_repair_attempts = 3

    window_sec_raw = os.getenv("SCRATCHBIRD_AI_OPERATION_WINDOW_SEC", "60")
    try:
        operation_window_sec = int(window_sec_raw)
    except ValueError:
        operation_window_sec = 60

    max_requests_raw = os.getenv("SCRATCHBIRD_AI_MAX_REQUESTS_PER_WINDOW", "100")
    try:
        max_requests_per_window = int(max_requests_raw)
    except ValueError:
        max_requests_per_window = 100

    max_mutations_raw = os.getenv("SCRATCHBIRD_AI_MAX_MUTATIONS_PER_WINDOW", "20")
    try:
        max_mutations_per_window = int(max_mutations_raw)
    except ValueError:
        max_mutations_per_window = 20

    max_cost_raw = os.getenv("SCRATCHBIRD_AI_MAX_COST_UNITS_PER_WINDOW", "1000")
    try:
        max_cost_units_per_window = int(max_cost_raw)
    except ValueError:
        max_cost_units_per_window = 1000

    dialects_raw = os.getenv(
        "SCRATCHBIRD_AI_HTTP_DIALECTS",
        "native",
    )
    dialects = _parse_csv(dialects_raw)
    supported_server_versions = _parse_csv(
        os.getenv(
            "SCRATCHBIRD_AI_SUPPORTED_SERVER_VERSIONS",
            "native-http-bridge-preview",
        )
    )
    supported_parser_compiler_versions = _parse_csv(
        os.getenv(
            "SCRATCHBIRD_AI_SUPPORTED_PARSER_COMPILER_VERSIONS",
            "native-only",
        )
    )
    supported_driver_runtime_versions = _parse_csv(
        os.getenv(
            "SCRATCHBIRD_AI_SUPPORTED_DRIVER_RUNTIME_VERSIONS",
            "builtin",
        )
    )

    remote_auth_token = os.getenv("SCRATCHBIRD_AI_REMOTE_MCP_AUTH_TOKEN", "").strip() or None

    ttl_raw = os.getenv("SCRATCHBIRD_AI_REMOTE_MCP_SESSION_TTL_SEC", "900")
    try:
        remote_ttl = int(ttl_raw)
    except ValueError:
        remote_ttl = 900

    heartbeat_raw = os.getenv("SCRATCHBIRD_AI_REMOTE_MCP_HEARTBEAT_INTERVAL_SEC", "30")
    try:
        remote_heartbeat = int(heartbeat_raw)
    except ValueError:
        remote_heartbeat = 30

    protocol_versions = _parse_csv(
        os.getenv("SCRATCHBIRD_AI_REMOTE_MCP_PROTOCOL_VERSIONS", "v0")
    )
    supported_auth_types = _parse_csv(
        os.getenv(
            "SCRATCHBIRD_AI_REMOTE_MCP_SUPPORTED_AUTH_TYPES",
            (
                "bearer,oauth2_access_token,jwt_bearer,workload_identity,"
                "proxy_principal,ldap_bind,kerberos_gssapi,radius_pap,"
                "pam_conversation,preauthenticated_context"
            ),
        )
    )
    supported_transports = _parse_csv(
        os.getenv(
            "SCRATCHBIRD_AI_REMOTE_MCP_SUPPORTED_TRANSPORTS",
            "https_json_request_response,https_sse_server_stream,websocket_bidirectional",
        )
    )

    return RuntimeSettings(
        adapter_mode=mode,
        server_instance_name=server_instance_name,
        deployment_scope=deployment_scope,
        http_base_url=base_url,
        http_timeout_sec=timeout,
        http_retry_attempts=max(0, retry_attempts),
        http_retry_backoff_ms=max(0, retry_backoff_ms),
        http_circuit_breaker_failure_threshold=max(1, circuit_threshold),
        http_circuit_breaker_cooldown_sec=max(1.0, circuit_cooldown),
        http_api_token=api_token,
        http_dialects=dialects,
        retrieval_catalog_path=retrieval_catalog_path,
        approval_ledger_path=approval_ledger_path,
        structured_event_log_path=structured_event_log_path,
        operator_bundle_output_dir=operator_bundle_output_dir,
        operator_target_profiles=operator_target_profiles,
        audit_attestation_mode=audit_attestation_mode,
        audit_attestation_secret=audit_attestation_secret,
        audit_attestation_attestor_id=audit_attestation_attestor_id,
        audit_attestation_delegated_secret=audit_attestation_delegated_secret,
        audit_attestation_delegated_attestor_id=audit_attestation_delegated_attestor_id,
        audit_attestation_external_reference_base_url=(
            audit_attestation_external_reference_base_url
        ),
        compile_repair_max_attempts=max(1, compile_repair_attempts),
        operation_window_sec=max(1, operation_window_sec),
        max_requests_per_window=max(1, max_requests_per_window),
        max_mutations_per_window=max(1, max_mutations_per_window),
        max_cost_units_per_window=max(1, max_cost_units_per_window),
        supported_server_versions=supported_server_versions or ("native-http-bridge-preview",),
        supported_parser_compiler_versions=(
            supported_parser_compiler_versions or ("native-only",)
        ),
        supported_driver_runtime_versions=(
            supported_driver_runtime_versions or ("builtin",)
        ),
        remote_mcp_auth_token=remote_auth_token,
        remote_mcp_session_ttl_sec=remote_ttl,
        remote_mcp_heartbeat_interval_sec=remote_heartbeat,
        remote_mcp_protocol_versions=protocol_versions or ("v0",),
        remote_mcp_supported_auth_types=(
            supported_auth_types
            or (
                "bearer",
                "oauth2_access_token",
                "jwt_bearer",
                "workload_identity",
                "proxy_principal",
                "ldap_bind",
                "kerberos_gssapi",
                "radius_pap",
                "pam_conversation",
                "preauthenticated_context",
            )
        ),
        remote_mcp_supported_transports=(
            supported_transports
            or (
                "https_json_request_response",
                "https_sse_server_stream",
                "websocket_bidirectional",
            )
        ),
    )
