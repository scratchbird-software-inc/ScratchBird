# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Remote MCP session and auth lifecycle primitives."""

from __future__ import annotations

from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from typing import Any

from .deterministic import deterministic_id
from .tool_schema import ToolContractError, require_security_context


REMOTE_PROTOCOL_VERSION = "v0"


@dataclass(slots=True)
class RemoteSession:
    session_id: str
    server_id: str
    capability_manifest_id: str
    interface_profile_id: str
    negotiated_protocol_version: str
    negotiated_transport: str
    session_expires_at: str
    heartbeat_interval_sec: int
    trace_seed: str
    security_context: dict[str, Any]
    client_id: str
    client_version: str
    client_capabilities: dict[str, Any]
    auth_subject: str


def _utc_now() -> datetime:
    return datetime.now(timezone.utc)


def _iso_utc(value: datetime) -> str:
    return value.astimezone(timezone.utc).isoformat().replace("+00:00", "Z")


class RemoteSessionManager:
    def __init__(
        self,
        *,
        auth_token: str | None,
        session_ttl_sec: int = 900,
        heartbeat_interval_sec: int = 30,
        supported_protocol_versions: tuple[str, ...] = (REMOTE_PROTOCOL_VERSION,),
        supported_auth_types: tuple[str, ...] = (
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
        ),
        supported_transports: tuple[str, ...] = (
            "https_json_request_response",
            "https_sse_server_stream",
            "websocket_bidirectional",
        ),
    ) -> None:
        self.auth_token = (auth_token or "").strip() or None
        self.session_ttl_sec = max(60, int(session_ttl_sec))
        self.heartbeat_interval_sec = max(5, int(heartbeat_interval_sec))
        self.supported_protocol_versions = supported_protocol_versions or (REMOTE_PROTOCOL_VERSION,)
        self.supported_auth_types = supported_auth_types or (
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
        self.supported_transports = supported_transports or (
            "https_json_request_response",
            "https_sse_server_stream",
            "websocket_bidirectional",
        )
        self._sessions: dict[str, RemoteSession] = {}
        self._closed_sessions: set[str] = set()

    def open_session(
        self,
        request: dict[str, Any] | None,
        *,
        capability_advertisement: dict[str, Any],
        now_utc: datetime | None = None,
    ) -> dict[str, Any]:
        payload = dict(request or {})
        request_id = str(payload.get("request_id", "")).strip() or deterministic_id(
            "req",
            {"operation": "remote_session_open", "payload": payload},
        )
        interface_profile_id = str(payload.get("interface_profile_id", "")).strip() or "mcp_remote_v0"
        if interface_profile_id != "mcp_remote_v0":
            raise ToolContractError(
                error_code="E_INTERFACE_PROFILE_UNSUPPORTED",
                message=f"unsupported remote interface profile: {interface_profile_id}",
                policy_rule_id="REMOTE-SESSION-001",
            )

        protocol_version = str(payload.get("protocol_version", "")).strip() or REMOTE_PROTOCOL_VERSION
        if protocol_version not in set(self.supported_protocol_versions):
            raise ToolContractError(
                error_code="E_COMPONENT_VERSION_UNSUPPORTED",
                message=f"unsupported remote protocol version: {protocol_version}",
                policy_rule_id="REMOTE-SESSION-002",
            )

        requested_transport = str(payload.get("requested_transport", "")).strip() or "https_json_request_response"
        if requested_transport not in set(self.supported_transports):
            raise ToolContractError(
                error_code="E_TRANSPORT_PROFILE_UNSUPPORTED",
                message=f"unsupported remote transport: {requested_transport}",
                policy_rule_id="REMOTE-SESSION-003",
            )

        auth_envelope = payload.get("auth_envelope")
        security_context = self._authenticate(
            auth_envelope,
            payload.get("security_context_hint"),
        )

        client_capabilities = payload.get("client_capabilities", {})
        if not isinstance(client_capabilities, dict):
            raise ToolContractError(
                error_code="E_TOOL_INPUT_INVALID",
                message="client_capabilities must be an object",
                policy_rule_id="REMOTE-SESSION-004",
            )

        timestamp = now_utc or _utc_now()
        expires_at = timestamp + timedelta(seconds=self.session_ttl_sec)
        client_id = str(payload.get("client_id", "")).strip() or "unknown-client"
        client_version = str(payload.get("client_version", "")).strip() or "unknown"
        session_id = deterministic_id(
            "sess",
            {
                "request_id": request_id,
                "client_id": client_id,
                "client_version": client_version,
                "protocol_version": protocol_version,
                "requested_transport": requested_transport,
                "opened_at": _iso_utc(timestamp),
            },
        )
        trace_seed = deterministic_id(
            "trseed",
            {"session_id": session_id, "client_id": client_id, "transport": requested_transport},
        )
        auth_subject = str(security_context.get("actor_id", client_id)).strip() or client_id
        server_id = str(capability_advertisement.get("server_id", "")).strip() or "srv_unknown"
        capability_manifest_id = (
            str(capability_advertisement.get("capability_manifest_id", "")).strip() or "mf_unknown"
        )

        session = RemoteSession(
            session_id=session_id,
            server_id=server_id,
            capability_manifest_id=capability_manifest_id,
            interface_profile_id=interface_profile_id,
            negotiated_protocol_version=protocol_version,
            negotiated_transport=requested_transport,
            session_expires_at=_iso_utc(expires_at),
            heartbeat_interval_sec=self.heartbeat_interval_sec,
            trace_seed=trace_seed,
            security_context=security_context,
            client_id=client_id,
            client_version=client_version,
            client_capabilities=client_capabilities,
            auth_subject=auth_subject,
        )
        self._sessions[session_id] = session
        self._closed_sessions.discard(session_id)

        return {
            "request_id": request_id,
            "session_id": session.session_id,
            "server_id": session.server_id,
            "capability_manifest_id": session.capability_manifest_id,
            "interface_profile_id": session.interface_profile_id,
            "negotiated_protocol_version": session.negotiated_protocol_version,
            "negotiated_transport": session.negotiated_transport,
            "session_expires_at": session.session_expires_at,
            "heartbeat_interval_sec": session.heartbeat_interval_sec,
            "capability_advertisement": capability_advertisement,
            "trace_seed": session.trace_seed,
            "warnings": [],
        }

    def require_session(
        self,
        session_id: str,
        *,
        now_utc: datetime | None = None,
    ) -> RemoteSession:
        session = self._sessions.get(session_id)
        if session is None:
            raise ToolContractError(
                error_code="E_SESSION_REQUIRED",
                message=f"unknown remote session: {session_id}",
                policy_rule_id="REMOTE-SESSION-005",
            )
        expires_at = datetime.fromisoformat(session.session_expires_at.replace("Z", "+00:00"))
        if expires_at <= (now_utc or _utc_now()):
            del self._sessions[session_id]
            self._closed_sessions.add(session_id)
            raise ToolContractError(
                error_code="E_SESSION_REQUIRED",
                message=f"expired remote session: {session_id}",
                policy_rule_id="REMOTE-SESSION-006",
            )
        return session

    def close_session(
        self,
        *,
        session_id: str,
        request_id: str | None = None,
    ) -> dict[str, Any]:
        rid = (request_id or "").strip() or deterministic_id(
            "req", {"operation": "remote_session_close", "session_id": session_id}
        )
        existed = session_id in self._sessions
        if existed:
            del self._sessions[session_id]
            self._closed_sessions.add(session_id)
        return {
            "request_id": rid,
            "session_id": session_id,
            "status": "closed" if existed else "already_closed",
        }

    def _authenticate(self, auth_envelope: Any, security_context_hint: Any = None) -> dict[str, Any]:
        if not isinstance(auth_envelope, dict):
            raise ToolContractError(
                error_code="E_POLICY_DENY",
                message="auth_envelope must be an object",
                policy_rule_id="REMOTE-AUTH-001",
            )
        auth_type = self._infer_auth_type(auth_envelope)
        if auth_type not in set(self.supported_auth_types):
            raise ToolContractError(
                error_code="E_PROVIDER_CONTRACT_UNSUPPORTED",
                message=f"unsupported remote auth type: {auth_type}",
                policy_rule_id="REMOTE-AUTH-002",
            )

        if self.auth_token is not None and auth_type in {
            "bearer",
            "oauth2_access_token",
            "jwt_bearer",
        }:
            token = self._extract_presented_token(auth_envelope)
            if not token or token != self.auth_token:
                raise ToolContractError(
                    error_code="E_POLICY_DENY",
                    message="remote auth token rejected",
                    policy_rule_id="REMOTE-AUTH-003",
                )

        if not self._has_auth_material(auth_envelope, auth_type):
            raise ToolContractError(
                error_code="E_POLICY_DENY",
                message=f"missing remote auth material for auth_type={auth_type}",
                policy_rule_id="REMOTE-AUTH-004",
            )
        security_source = auth_envelope.get(
            "security_context",
            auth_envelope.get("security_context_hint", security_context_hint),
        )
        return require_security_context({"security_context": security_source})

    @staticmethod
    def _infer_auth_type(auth_envelope: dict[str, Any]) -> str:
        auth_type = str(auth_envelope.get("auth_type", "")).strip()
        if auth_type:
            return auth_type
        if any(str(auth_envelope.get(field, "")).strip() for field in ("token", "access_token", "jwt")):
            return "bearer"
        return "preauthenticated_context"

    @staticmethod
    def _extract_presented_token(auth_envelope: dict[str, Any]) -> str:
        for field in ("token", "access_token", "jwt"):
            value = str(auth_envelope.get(field, "")).strip()
            if value:
                return value
        return ""

    @classmethod
    def _has_auth_material(cls, auth_envelope: dict[str, Any], auth_type: str) -> bool:
        token_value = cls._extract_presented_token(auth_envelope)
        security_context = auth_envelope.get("security_context") or auth_envelope.get(
            "security_context_hint"
        )
        if auth_type in {"bearer", "oauth2_access_token", "jwt_bearer"}:
            return bool(token_value) or isinstance(security_context, dict)
        if auth_type == "workload_identity":
            return bool(str(auth_envelope.get("workload_identity", "")).strip()) or bool(
                token_value
            )
        if auth_type == "proxy_principal":
            return bool(str(auth_envelope.get("proxy_principal", "")).strip()) or bool(
                str(auth_envelope.get("principal", "")).strip()
            )
        if auth_type == "ldap_bind":
            return bool(str(auth_envelope.get("bind_dn", "")).strip())
        if auth_type == "kerberos_gssapi":
            return bool(str(auth_envelope.get("kerberos_principal", "")).strip())
        if auth_type == "radius_pap":
            return bool(str(auth_envelope.get("radius_username", "")).strip())
        if auth_type == "pam_conversation":
            return bool(str(auth_envelope.get("pam_service", "")).strip())
        if auth_type == "preauthenticated_context":
            return isinstance(security_context, dict)
        return bool(
            str(auth_envelope.get("subject", "")).strip()
            or str(auth_envelope.get("principal", "")).strip()
            or str(auth_envelope.get("credential_id", "")).strip()
            or str(auth_envelope.get("workload_identity", "")).strip()
            or str(auth_envelope.get("proxy_principal", "")).strip()
            or str(auth_envelope.get("bind_dn", "")).strip()
            or str(auth_envelope.get("kerberos_principal", "")).strip()
            or str(auth_envelope.get("radius_username", "")).strip()
            or str(auth_envelope.get("pam_service", "")).strip()
            or token_value
            or isinstance(security_context, dict)
        )
