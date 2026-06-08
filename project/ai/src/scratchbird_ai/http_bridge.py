# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""HTTP bridge service that exposes ScratchBird compile/execute/metadata routes."""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
from dataclasses import asdict, dataclass, field, replace
from datetime import date, datetime, time
from decimal import Decimal
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Protocol
from urllib import parse
from uuid import UUID

from .service import ScratchBirdAIService, build_default_service
from .settings import load_runtime_settings


DEFAULT_DIALECTS = ("native",)
DEFAULT_REQUEST_MAX_BYTES = 2 * 1024 * 1024
_DEFAULT_SERVER_SETUP = "listener-only"

_SERVER_SETUP_ALIASES = {
    "listener-only": "listener-only",
    "listener_only": "listener-only",
    "listener": "listener-only",
    "inet_listener": "listener-only",
    "inet": "listener-only",
    "tcp": "listener-only",
    "tcp_listener": "listener-only",
    "network": "listener-only",
    "managed": "managed",
    "manager": "managed",
    "manager_proxy": "managed",
    "manager-proxy": "managed",
    "mcp": "managed",
    "ipc-only": "ipc-only",
    "ipc_only": "ipc-only",
    "ipc": "ipc-only",
    "local_ipc": "ipc-only",
    "local-ipc": "ipc-only",
    "local": "ipc-only",
    "embedded": "embedded",
    "inproc": "embedded",
    "in-process": "embedded",
    "in_process": "embedded",
}


def _parse_csv(raw: str) -> tuple[str, ...]:
    return tuple(part.strip().lower() for part in raw.split(",") if part.strip())


def _env_int(name: str, default: int) -> int:
    value = os.getenv(name, str(default)).strip()
    try:
        return int(value)
    except ValueError:
        return default


def _parse_bool(raw: str | None, default: bool) -> bool:
    if raw is None:
        return default
    lowered = raw.strip().lower()
    if lowered in {"1", "true", "yes", "on"}:
        return True
    if lowered in {"0", "false", "no", "off"}:
        return False
    return default


def _first_nonempty_env(*names: str) -> str:
    for name in names:
        value = os.getenv(name)
        if value is not None:
            stripped = value.strip()
            if stripped:
                return stripped
    return ""


def _normalize_server_setup(raw: str) -> str:
    normalized = raw.strip().lower()
    if not normalized:
        return _DEFAULT_SERVER_SETUP
    return _SERVER_SETUP_ALIASES.get(normalized, _DEFAULT_SERVER_SETUP)


def _has_manager_token_in_dsn(dsn: str) -> bool:
    lowered = dsn.strip().lower()
    if not lowered:
        return False
    return (
        "manager_auth_token=" in lowered
        or "mcp_auth_token=" in lowered
        or "managerauthtoken=" in lowered
    )


def _sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def _classify_statement_kind(query_text: str) -> str:
    lowered = query_text.strip().lower()
    if not lowered:
        return "unknown"

    match = re.match(r"[a-z]+", lowered)
    if not match:
        return "unknown"

    keyword = match.group(0)
    if keyword in {"select", "with", "show", "describe", "desc", "explain", "match"}:
        return "read"
    if keyword in {
        "insert",
        "update",
        "delete",
        "merge",
        "create",
        "alter",
        "drop",
        "truncate",
        "grant",
        "revoke",
        "set",
        "call",
        "execute",
    }:
        return "mutation"
    return "unknown"


def _json_safe(value: Any) -> Any:
    if value is None or isinstance(value, (str, int, float, bool)):
        return value
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, (datetime, date, time)):
        return value.isoformat()
    if isinstance(value, Decimal):
        return str(value)
    if isinstance(value, UUID):
        return str(value)
    if isinstance(value, list):
        return [_json_safe(item) for item in value]
    if isinstance(value, tuple):
        return [_json_safe(item) for item in value]
    if isinstance(value, dict):
        return {str(k): _json_safe(v) for k, v in value.items()}
    return str(value)


@dataclass(slots=True)
class BridgeCompileResult:
    statement_kind: str
    sblr_hash: str
    diagnostics: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


@dataclass(slots=True)
class BridgeExecuteResult:
    rows: list[dict[str, Any]]
    notices: list[str] = field(default_factory=list)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


class BridgeBackend(Protocol):
    def compile_query(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any],
    ) -> BridgeCompileResult:
        ...

    def execute_query(
        self,
        *,
        dialect: str,
        query_text: str,
        options: dict[str, Any],
        compile_artifact_id: str,
    ) -> BridgeExecuteResult:
        ...

    def list_schemas(self, *, dialect: str, database: str | None = None) -> list[str]:
        ...

    def list_tables(self, *, dialect: str, schema: str) -> list[str]:
        ...

    def describe_table(self, *, dialect: str, schema: str, table: str) -> dict[str, Any]:
        ...


class BridgeError(RuntimeError):
    def __init__(self, *, status_code: int, message: str) -> None:
        super().__init__(message)
        self.status_code = status_code
        self.message = message


@dataclass(slots=True)
class BridgeSettings:
    host: str = "127.0.0.1"
    port: int = 3095
    api_token: str | None = None
    request_max_bytes: int = DEFAULT_REQUEST_MAX_BYTES
    strict_compile: bool = False
    default_dsn: str = ""
    enabled_dialects: tuple[str, ...] = DEFAULT_DIALECTS
    dialect_dsns: dict[str, str] = field(default_factory=dict)
    python_driver_src: str = ""
    server_setup: str = _DEFAULT_SERVER_SETUP
    transport_mode: str = ""
    front_door_mode: str = ""
    ipc_method: str = ""
    ipc_path: str = ""
    manager_auth_token: str | None = None
    manager_username: str | None = None
    manager_database: str | None = None
    manager_connection_profile: str = "native_v3"
    manager_client_intent: str = "native_v3"
    manager_client_flags: int = 0
    manager_auth_fast_path: bool = True
    auth_method_id: str = ""
    auth_method_payload: str = ""
    auth_payload_json: str = ""
    auth_payload_b64: str = ""
    auth_provider_profile: str = ""
    auth_required_methods: tuple[str, ...] = ()
    auth_forbidden_methods: tuple[str, ...] = ()
    auth_require_channel_binding: bool = False
    workload_identity_token: str | None = None
    proxy_principal_assertion: str | None = None
    ldap_bind_dn: str | None = None
    kerberos_spn: str | None = None
    radius_username: str | None = None
    pam_service: str | None = None

    @classmethod
    def from_env(cls) -> BridgeSettings:
        dialect_dsns: dict[str, str] = {}
        for key, value in os.environ.items():
            if not key.startswith("SCRATCHBIRD_AI_BRIDGE_DSN_"):
                continue
            suffix = key.removeprefix("SCRATCHBIRD_AI_BRIDGE_DSN_").strip()
            if not suffix:
                continue
            dialect = suffix.lower()
            dsn = value.strip()
            if dsn:
                dialect_dsns[dialect] = dsn

        default_dsn = os.getenv("SCRATCHBIRD_AI_BRIDGE_DEFAULT_DSN", "").strip()
        enabled = _parse_csv(
            os.getenv(
                "SCRATCHBIRD_AI_BRIDGE_DIALECTS",
                ",".join(DEFAULT_DIALECTS),
            )
        )
        if not enabled:
            enabled = DEFAULT_DIALECTS

        strict_raw = os.getenv("SCRATCHBIRD_AI_BRIDGE_STRICT_COMPILE", "0").strip().lower()
        strict_compile = strict_raw in {"1", "true", "yes", "on"}

        manager_flags = 0
        manager_flags_raw = _first_nonempty_env(
            "SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_FLAGS",
            "SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_FLAGS",
        )
        if manager_flags_raw:
            try:
                manager_flags = int(manager_flags_raw)
            except ValueError:
                manager_flags = 0
            manager_flags = max(0, min(65535, manager_flags))

        manager_auth_fast_path = True
        if "SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_FAST_PATH" in os.environ:
            manager_auth_fast_path = _parse_bool(
                os.getenv("SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_FAST_PATH"),
                True,
            )
        elif "SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_FAST_PATH" in os.environ:
            manager_auth_fast_path = _parse_bool(
                os.getenv("SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_FAST_PATH"),
                True,
            )

        server_setup_raw = _first_nonempty_env(
            "SCRATCHBIRD_AI_BRIDGE_SERVER_SETUP",
            "SCRATCHBIRD_AI_BRIDGE_CONNECTION_SETUP",
            "SCRATCHBIRD_AI_BRIDGE_SERVER_MODE",
        )
        server_setup = _normalize_server_setup(server_setup_raw)
        auth_required_methods = _parse_csv(
            _first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_REQUIRED_METHODS",
                "SCRATCHBIRD_AI_BRIDGE_AUTH_REQUIRED",
            )
        )
        auth_forbidden_methods = _parse_csv(
            _first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_FORBIDDEN_METHODS",
                "SCRATCHBIRD_AI_BRIDGE_AUTH_FORBIDDEN",
            )
        )
        auth_require_channel_binding = _parse_bool(
            os.getenv("SCRATCHBIRD_AI_BRIDGE_AUTH_REQUIRE_CHANNEL_BINDING"),
            False,
        )

        return cls(
            host=os.getenv("SCRATCHBIRD_AI_BRIDGE_HOST", "127.0.0.1").strip(),
            port=_env_int("SCRATCHBIRD_AI_BRIDGE_PORT", 3095),
            api_token=os.getenv("SCRATCHBIRD_AI_BRIDGE_API_TOKEN", "").strip() or None,
            request_max_bytes=_env_int(
                "SCRATCHBIRD_AI_BRIDGE_REQUEST_MAX_BYTES",
                DEFAULT_REQUEST_MAX_BYTES,
            ),
            strict_compile=strict_compile,
            default_dsn=default_dsn,
            enabled_dialects=enabled,
            dialect_dsns=dialect_dsns,
            python_driver_src=os.getenv("SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC", "").strip(),
            server_setup=server_setup,
            transport_mode=os.getenv("SCRATCHBIRD_AI_BRIDGE_TRANSPORT_MODE", "").strip(),
            front_door_mode=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_FRONT_DOOR_MODE",
                "SCRATCHBIRD_AI_BRIDGE_FRONTDOORMODE",
                "SCRATCHBIRD_AI_BRIDGE_CONNECTION_MODE",
                "SCRATCHBIRD_AI_BRIDGE_INGRESS_MODE",
            ),
            ipc_method=os.getenv("SCRATCHBIRD_AI_BRIDGE_IPC_METHOD", "").strip(),
            ipc_path=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_IPC_PATH",
                "SCRATCHBIRD_AI_BRIDGE_SOCKET_PATH",
                "SCRATCHBIRD_AI_BRIDGE_PIPE_NAME",
            ),
            manager_auth_token=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN",
                "SCRATCHBIRD_AI_BRIDGE_MCP_AUTH_TOKEN",
            )
            or None,
            manager_username=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_MANAGER_USERNAME",
                "SCRATCHBIRD_AI_BRIDGE_MCP_USERNAME",
            )
            or None,
            manager_database=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_MANAGER_DATABASE",
                "SCRATCHBIRD_AI_BRIDGE_MCP_DATABASE",
            )
            or None,
            manager_connection_profile=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_MANAGER_CONNECTION_PROFILE",
                "SCRATCHBIRD_AI_BRIDGE_MCP_CONNECTION_PROFILE",
            )
            or "native_v3",
            manager_client_intent=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_MANAGER_CLIENT_INTENT",
                "SCRATCHBIRD_AI_BRIDGE_MCP_CLIENT_INTENT",
            )
            or "native_v3",
            manager_client_flags=manager_flags,
            manager_auth_fast_path=manager_auth_fast_path,
            auth_method_id=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_METHOD_ID",
                "SCRATCHBIRD_AI_BRIDGE_DB_AUTH_METHOD_ID",
            ),
            auth_method_payload=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_METHOD_PAYLOAD",
                "SCRATCHBIRD_AI_BRIDGE_DB_AUTH_METHOD_PAYLOAD",
            ),
            auth_payload_json=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_PAYLOAD_JSON",
                "SCRATCHBIRD_AI_BRIDGE_DB_AUTH_PAYLOAD_JSON",
            ),
            auth_payload_b64=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_PAYLOAD_B64",
                "SCRATCHBIRD_AI_BRIDGE_DB_AUTH_PAYLOAD_B64",
            ),
            auth_provider_profile=_first_nonempty_env(
                "SCRATCHBIRD_AI_BRIDGE_AUTH_PROVIDER_PROFILE",
                "SCRATCHBIRD_AI_BRIDGE_DB_AUTH_PROVIDER_PROFILE",
            ),
            auth_required_methods=auth_required_methods,
            auth_forbidden_methods=auth_forbidden_methods,
            auth_require_channel_binding=auth_require_channel_binding,
            workload_identity_token=(
                _first_nonempty_env(
                    "SCRATCHBIRD_AI_BRIDGE_WORKLOAD_IDENTITY_TOKEN",
                    "SCRATCHBIRD_AI_BRIDGE_DB_WORKLOAD_IDENTITY_TOKEN",
                )
                or None
            ),
            proxy_principal_assertion=(
                _first_nonempty_env(
                    "SCRATCHBIRD_AI_BRIDGE_PROXY_PRINCIPAL_ASSERTION",
                    "SCRATCHBIRD_AI_BRIDGE_DB_PROXY_PRINCIPAL_ASSERTION",
                )
                or None
            ),
            ldap_bind_dn=(
                _first_nonempty_env(
                    "SCRATCHBIRD_AI_BRIDGE_LDAP_BIND_DN",
                    "SCRATCHBIRD_AI_BRIDGE_DB_LDAP_BIND_DN",
                )
                or None
            ),
            kerberos_spn=(
                _first_nonempty_env(
                    "SCRATCHBIRD_AI_BRIDGE_KERBEROS_SPN",
                    "SCRATCHBIRD_AI_BRIDGE_DB_KERBEROS_SPN",
                )
                or None
            ),
            radius_username=(
                _first_nonempty_env(
                    "SCRATCHBIRD_AI_BRIDGE_RADIUS_USERNAME",
                    "SCRATCHBIRD_AI_BRIDGE_DB_RADIUS_USERNAME",
                )
                or None
            ),
            pam_service=(
                _first_nonempty_env(
                    "SCRATCHBIRD_AI_BRIDGE_PAM_SERVICE",
                    "SCRATCHBIRD_AI_BRIDGE_DB_PAM_SERVICE",
                )
                or None
            ),
        )

    def require_enabled_dialect(self, dialect: str) -> None:
        normalized = dialect.strip().lower()
        enabled_set = set(self.enabled_dialects)
        if normalized not in enabled_set:
            enabled_list = ", ".join(sorted(enabled_set)) if enabled_set else "<none>"
            raise BridgeError(
                status_code=404,
                message=(
                    f"Unsupported dialect '{dialect}'. "
                    f"Bridge is configured for enabled dialects: {enabled_list}"
                ),
            )

    def resolve_dsn(self, dialect: str) -> str:
        normalized = dialect.strip().lower()
        self.require_enabled_dialect(normalized)

        if normalized in self.dialect_dsns:
            return self.dialect_dsns[normalized]
        if self.default_dsn:
            return self.default_dsn
        raise BridgeError(
            status_code=404,
            message=f"No bridge DSN configured for dialect: {dialect}",
        )

    def resolve_connect_kwargs(self, dialect: str) -> dict[str, Any]:
        normalized = dialect.strip().lower()
        dsn = self.resolve_dsn(normalized)
        setup = _normalize_server_setup(self.server_setup)

        connect_kwargs: dict[str, Any] = {
            "dsn": dsn,
            "protocol": "native",
        }

        if self.transport_mode:
            connect_kwargs["transport_mode"] = self.transport_mode
        if self.front_door_mode:
            connect_kwargs["front_door_mode"] = self.front_door_mode
        if self.auth_method_id:
            connect_kwargs["auth_method_id"] = self.auth_method_id
        if self.auth_method_payload:
            connect_kwargs["auth_method_payload"] = self.auth_method_payload
        if self.auth_payload_json:
            connect_kwargs["auth_payload_json"] = self.auth_payload_json
        if self.auth_payload_b64:
            connect_kwargs["auth_payload_b64"] = self.auth_payload_b64
        if self.auth_provider_profile:
            connect_kwargs["auth_provider_profile"] = self.auth_provider_profile
        if self.auth_required_methods:
            connect_kwargs["auth_required_methods"] = list(self.auth_required_methods)
        if self.auth_forbidden_methods:
            connect_kwargs["auth_forbidden_methods"] = list(self.auth_forbidden_methods)
        if self.auth_require_channel_binding:
            connect_kwargs["auth_require_channel_binding"] = True
        if self.workload_identity_token:
            connect_kwargs["workload_identity_token"] = self.workload_identity_token
        if self.proxy_principal_assertion:
            connect_kwargs["proxy_principal_assertion"] = self.proxy_principal_assertion
        if self.ldap_bind_dn:
            connect_kwargs["ldap_bind_dn"] = self.ldap_bind_dn
        if self.kerberos_spn:
            connect_kwargs["kerberos_spn"] = self.kerberos_spn
        if self.radius_username:
            connect_kwargs["radius_username"] = self.radius_username
        if self.pam_service:
            connect_kwargs["pam_service"] = self.pam_service

        overlap = set(self.auth_required_methods) & set(self.auth_forbidden_methods)
        if overlap:
            raise BridgeError(
                status_code=400,
                message=(
                    "Invalid auth pinning profile: methods appear in both required "
                    f"and forbidden sets ({', '.join(sorted(overlap))})."
                ),
            )

        if setup == "managed":
            connect_kwargs.setdefault("transport_mode", "managed")
            connect_kwargs.setdefault("front_door_mode", "manager_proxy")
        elif setup == "listener-only":
            connect_kwargs.setdefault("transport_mode", "inet_listener")
            connect_kwargs.setdefault("front_door_mode", "direct")
        elif setup == "ipc-only":
            connect_kwargs.setdefault("transport_mode", "local_ipc")
            connect_kwargs.setdefault("front_door_mode", "direct")
        elif setup == "embedded":
            connect_kwargs.setdefault("transport_mode", "embedded")
            connect_kwargs.setdefault("front_door_mode", "direct")
            # Embedded mode is non-shared: single private connection without server front-door.
            connect_kwargs["shared"] = False
            connect_kwargs["connection_scope"] = "private"
            connect_kwargs["embedded_single_connection"] = True

        if self.ipc_method:
            connect_kwargs["ipc_method"] = self.ipc_method
        if self.ipc_path:
            connect_kwargs["ipc_path"] = self.ipc_path

        effective_front_door_mode = str(connect_kwargs.get("front_door_mode", "")).strip().lower()
        effective_transport = str(connect_kwargs.get("transport_mode", "")).strip().lower()
        if effective_front_door_mode in {"managed", "manager_proxy", "manager-proxy"}:
            connect_kwargs["front_door_mode"] = "manager_proxy"
            connect_kwargs["transport_mode"] = "managed"
            effective_front_door_mode = "manager_proxy"
            effective_transport = "managed"
        elif effective_transport == "managed":
            connect_kwargs["front_door_mode"] = "manager_proxy"
            effective_front_door_mode = "manager_proxy"

        manager_mode = effective_front_door_mode in {
            "managed",
            "manager_proxy",
            "manager-proxy",
        } or effective_transport == "managed"

        if manager_mode:
            if self.manager_auth_token:
                connect_kwargs["manager_auth_token"] = self.manager_auth_token
            if self.manager_username:
                connect_kwargs["manager_username"] = self.manager_username
            if self.manager_database:
                connect_kwargs["manager_database"] = self.manager_database
            connect_kwargs["manager_connection_profile"] = (
                self.manager_connection_profile or "native_v3"
            )
            connect_kwargs["manager_client_intent"] = self.manager_client_intent or "native_v3"
            connect_kwargs["manager_client_flags"] = max(0, min(65535, self.manager_client_flags))
            connect_kwargs["manager_auth_fast_path"] = bool(self.manager_auth_fast_path)
            if "manager_auth_token" not in connect_kwargs and not _has_manager_token_in_dsn(dsn):
                raise BridgeError(
                    status_code=400,
                    message=(
                        "Managed setup requires manager_auth_token "
                        "(set SCRATCHBIRD_AI_BRIDGE_MANAGER_AUTH_TOKEN or include in DSN)."
                    ),
                )

        return connect_kwargs


class ScratchBirdDriverBackend:
    """Bridge backend that uses the ScratchBird Python driver for live queries.

    SQL text enters parser/wire adapter flow via driver APIs. Engine execution remains
    SBLR-based in ScratchBird core execution boundary.
    """

    def __init__(self, settings: BridgeSettings) -> None:
        self.settings = settings

        driver_src = settings.python_driver_src
        if driver_src and driver_src not in sys.path:
            sys.path.insert(0, driver_src)

        try:
            import scratchbird as scratchbird_module
        except ImportError as exc:
            raise RuntimeError(
                "Unable to import scratchbird Python driver. "
                "Set SCRATCHBIRD_AI_BRIDGE_PYTHON_DRIVER_SRC or install the package."
            ) from exc

        self._scratchbird = scratchbird_module
        try:
            from scratchbird import protocol as protocol_module
        except ImportError:
            protocol_module = None
        self._protocol = protocol_module

    def compile_query(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any],
    ) -> BridgeCompileResult:
        self.settings.resolve_dsn(dialect)
        statement_kind = _classify_statement_kind(query_text)
        fallback_hash = hashlib.sha256(
            f"{dialect}\n{query_text}".encode("utf-8")
        ).hexdigest()

        warnings: list[str] = []
        diagnostics: list[str] = []
        sblr_hash = fallback_hash

        # Compile probe is performed only for read statements to avoid accidental side effects.
        if statement_kind == "read":
            try:
                server_hash = self._probe_compile_sblr_hash(
                    dialect=dialect,
                    query_text=query_text,
                    context=context,
                )
                if server_hash:
                    sblr_hash = server_hash
                else:
                    warnings.append("Compile probe returned no SBLR hash; using fallback hash.")
            except Exception as exc:
                if self.settings.strict_compile:
                    raise BridgeError(status_code=400, message=f"Compile probe failed: {exc}") from exc
                warnings.append(f"Compile probe unavailable: {exc}")
        else:
            warnings.append("Mutation/unknown compile path uses local statement classification only.")

        return BridgeCompileResult(
            statement_kind=statement_kind,
            sblr_hash=sblr_hash,
            diagnostics=diagnostics,
            warnings=warnings,
        )

    def execute_query(
        self,
        *,
        dialect: str,
        query_text: str,
        options: dict[str, Any],
        compile_artifact_id: str,
    ) -> BridgeExecuteResult:
        del compile_artifact_id
        params = options.get("params")
        max_rows_value = options.get("max_rows", 0)
        try:
            max_rows = int(max_rows_value)
        except (TypeError, ValueError):
            max_rows = 0

        columns, rows, rowcount = self._run_query(
            dialect=dialect,
            sql=query_text,
            params=params,
            max_rows=max_rows,
        )
        out_rows = [self._row_to_dict(columns, row) for row in rows]
        notices: list[str] = []
        if rowcount is not None and rowcount >= 0:
            notices.append(f"rowcount={rowcount}")
        if max_rows > 0 and len(out_rows) >= max_rows:
            notices.append(f"max_rows={max_rows} limit reached")
        return BridgeExecuteResult(rows=out_rows, notices=notices)

    @staticmethod
    def _is_native_metadata_dialect(dialect: str) -> bool:
        normalized = dialect.strip().lower()
        return normalized in {"native", "scratchbird"}

    def list_schemas(self, *, dialect: str, database: str | None = None) -> list[str]:
        del database
        if self._is_native_metadata_dialect(dialect):
            attempts = [
                "SELECT schema_name FROM sys.catalog.schemas ORDER BY schema_name",
            ]
        else:
            attempts = [
                "SELECT schema_name FROM information_schema.schemata ORDER BY schema_name",
                "SELECT DISTINCT table_schema AS schema_name FROM information_schema.tables "
                "ORDER BY schema_name",
                "SELECT TRIM(rdb$owner_name) AS schema_name FROM rdb$relations "
                "WHERE rdb$system_flag = 0 GROUP BY rdb$owner_name ORDER BY rdb$owner_name",
            ]
        return self._query_first_column_with_fallbacks(dialect=dialect, attempts=attempts)

    def list_tables(self, *, dialect: str, schema: str) -> list[str]:
        schema_lit = _sql_literal(schema)
        if self._is_native_metadata_dialect(dialect):
            attempts = [
                "SELECT t.table_name FROM sys.catalog.tables t "
                "JOIN sys.catalog.schemas s ON t.schema_id = s.schema_id "
                f"WHERE s.schema_name = {schema_lit} ORDER BY t.table_name",
            ]
        else:
            attempts = [
                "SELECT table_name FROM information_schema.tables "
                f"WHERE table_schema = {schema_lit} ORDER BY table_name",
                "SELECT TRIM(rdb$relation_name) AS table_name FROM rdb$relations "
                "WHERE rdb$system_flag = 0 ORDER BY rdb$relation_name",
            ]
        return self._query_first_column_with_fallbacks(dialect=dialect, attempts=attempts)

    def describe_table(self, *, dialect: str, schema: str, table: str) -> dict[str, Any]:
        schema_lit = _sql_literal(schema)
        table_lit = _sql_literal(table)
        if self._is_native_metadata_dialect(dialect):
            attempts = [
                "SELECT c.column_name, c.data_type_name AS data_type, c.is_nullable "
                "FROM sys.catalog.columns c "
                "JOIN sys.catalog.tables t ON c.table_id = t.table_id "
                "JOIN sys.catalog.schemas s ON t.schema_id = s.schema_id "
                f"WHERE s.schema_name = {schema_lit} AND t.table_name = {table_lit} "
                "ORDER BY c.ordinal_position",
            ]
        else:
            attempts = [
                "SELECT column_name, data_type, is_nullable FROM information_schema.columns "
                f"WHERE table_schema = {schema_lit} AND table_name = {table_lit} "
                "ORDER BY ordinal_position",
                "SELECT TRIM(rf.rdb$field_name) AS column_name, "
                "TRIM(f.rdb$field_type) AS data_type, "
                "CASE WHEN rf.rdb$null_flag = 1 THEN 'NO' ELSE 'YES' END AS is_nullable "
                "FROM rdb$relation_fields rf "
                "JOIN rdb$fields f ON rf.rdb$field_source = f.rdb$field_name "
                f"WHERE TRIM(rf.rdb$relation_name) = UPPER({table_lit}) "
                "ORDER BY rf.rdb$field_position",
            ]

        last_error: Exception | None = None
        for sql in attempts:
            try:
                columns, rows, _ = self._run_query(dialect=dialect, sql=sql, params=None, max_rows=0)
            except Exception as exc:
                last_error = exc
                continue
            if not rows:
                continue
            col_index = {name.lower(): idx for idx, name in enumerate(columns)}
            result_columns: list[dict[str, Any]] = []
            for row in rows:
                name_idx = col_index.get("column_name", 0)
                type_idx = col_index.get("data_type", 1 if len(row) > 1 else 0)
                null_idx = col_index.get("is_nullable", 2 if len(row) > 2 else 0)

                name = str(row[name_idx]).strip()
                type_name = str(row[type_idx]).strip()
                nullable_text = str(row[null_idx]).strip().lower()
                nullable = nullable_text in {"yes", "true", "1", "nullable"}
                result_columns.append(
                    {
                        "name": name,
                        "type": type_name,
                        "nullable": nullable,
                    }
                )
            return {
                "dialect": dialect,
                "schema": schema,
                "table": table,
                "columns": result_columns,
            }

        if last_error is not None:
            raise BridgeError(status_code=501, message=f"Describe table unsupported: {last_error}") from last_error
        return {
            "dialect": dialect,
            "schema": schema,
            "table": table,
            "columns": [],
        }

    def _query_first_column_with_fallbacks(self, *, dialect: str, attempts: list[str]) -> list[str]:
        last_error: Exception | None = None
        for sql in attempts:
            try:
                _, rows, _ = self._run_query(dialect=dialect, sql=sql, params=None, max_rows=0)
            except Exception as exc:
                last_error = exc
                continue
            out: list[str] = []
            seen: set[str] = set()
            for row in rows:
                if not row:
                    continue
                value = str(row[0]).strip()
                if not value or value in seen:
                    continue
                seen.add(value)
                out.append(value)
            return out
        if last_error is not None:
            raise BridgeError(status_code=501, message=f"Metadata query unsupported: {last_error}") from last_error
        return []

    def _probe_compile_sblr_hash(
        self,
        *,
        dialect: str,
        query_text: str,
        context: dict[str, Any],
    ) -> str | None:
        protocol = self._protocol
        if protocol is None:
            raise RuntimeError("scratchbird.protocol unavailable")

        timeout_ms_raw = context.get("timeout_ms", 0)
        try:
            timeout_ms = int(timeout_ms_raw)
        except (TypeError, ValueError):
            timeout_ms = 0

        flags = protocol.QUERY_FLAG_DESCRIBE_ONLY
        flags |= protocol.QUERY_FLAG_INCLUDE_PLAN
        flags |= protocol.QUERY_FLAG_RETURN_SBLR
        if bool(context.get("no_cache")):
            flags |= protocol.QUERY_FLAG_NO_CACHE

        conn = self._connect(dialect)
        try:
            payload = protocol.build_query_payload(query_text, flags, 0, timeout_ms)
            conn._send_message(protocol.MessageType.QUERY, payload)
            conn._send_message(protocol.MessageType.SYNC, b"")
            conn._drain_until_ready()
            sblr = conn.last_sblr()
            if isinstance(sblr, tuple) and sblr and isinstance(sblr[0], int):
                return f"{sblr[0]:016x}"
            return None
        finally:
            conn.close()

    def _run_query(
        self,
        *,
        dialect: str,
        sql: str,
        params: Any,
        max_rows: int,
    ) -> tuple[list[str], list[tuple[Any, ...]], int]:
        conn = self._connect(dialect)
        try:
            cursor = conn.cursor()
            cursor.execute(sql, params)
            description = cursor.description or []
            columns = [str(col[0]) for col in description]
            rows: list[tuple[Any, ...]] = []

            while True:
                batch = cursor.fetchmany(256)
                if not batch:
                    break
                for row in batch:
                    rows.append(tuple(row))
                    if max_rows > 0 and len(rows) >= max_rows:
                        break
                if max_rows > 0 and len(rows) >= max_rows:
                    break

            rowcount = int(cursor.rowcount) if isinstance(cursor.rowcount, int) else -1
            return columns, rows, rowcount
        finally:
            conn.close()

    def _row_to_dict(self, columns: list[str], row: tuple[Any, ...]) -> dict[str, Any]:
        if columns and len(columns) == len(row):
            return {columns[idx]: _json_safe(value) for idx, value in enumerate(row)}

        out: dict[str, Any] = {}
        for idx, value in enumerate(row):
            key = columns[idx] if idx < len(columns) and columns[idx] else f"col_{idx + 1}"
            out[key] = _json_safe(value)
        return out

    def _connect(self, dialect: str):
        connect_kwargs = self.settings.resolve_connect_kwargs(dialect)
        try:
            return self._scratchbird.connect(**connect_kwargs)
        except Exception as exc:
            raise BridgeError(status_code=503, message=f"Connection failed for dialect {dialect}: {exc}") from exc


@dataclass(slots=True)
class ScratchBirdBridgeApp:
    settings: BridgeSettings
    backend: BridgeBackend
    service: ScratchBirdAIService | None = None


def _bridge_loopback_base_url(settings: BridgeSettings) -> str:
    host = settings.host.strip() or "127.0.0.1"
    if host in {"0.0.0.0", "::", "[::]"}:
        host = "127.0.0.1"
    return f"http://{host}:{settings.port}"


def build_remote_mcp_bridge_service(settings: BridgeSettings) -> ScratchBirdAIService:
    runtime_settings = replace(
        load_runtime_settings(),
        adapter_mode="http",
        http_base_url=_bridge_loopback_base_url(settings),
        http_api_token=settings.api_token,
        http_dialects=settings.enabled_dialects or DEFAULT_DIALECTS,
        remote_mcp_auth_token=settings.api_token,
    )
    return build_default_service(settings=runtime_settings)


class _BridgeHandler(BaseHTTPRequestHandler):
    app: ScratchBirdBridgeApp

    server_version = "ScratchBirdAIHTTPBridge/0.1"

    def do_GET(self) -> None:  # noqa: N802
        self._dispatch("GET")

    def do_POST(self) -> None:  # noqa: N802
        self._dispatch("POST")

    def log_message(self, fmt: str, *args: Any) -> None:
        del fmt, args
        return

    def _dispatch(self, method: str) -> None:
        try:
            self._authorize()
            parsed = parse.urlparse(self.path)
            segments = [parse.unquote(part) for part in parsed.path.split("/") if part]
            query_params = parse.parse_qs(parsed.query)

            if method == "GET" and segments == ["healthz"]:
                self._send_json(200, {"status": "ok"})
                return

            if len(segments) >= 2 and segments[0] == "v1" and segments[1] == "mcp":
                self._dispatch_remote(method, segments[2:], query_params)
                return

            if len(segments) < 3 or segments[0] != "v1" or segments[1] != "dialects":
                raise BridgeError(status_code=404, message=f"Route not found: {self.path}")

            dialect = segments[2].strip().lower()
            self.app.settings.require_enabled_dialect(dialect)
            tail = segments[3:]

            if method == "POST" and tail == ["compile"]:
                doc = self._read_json_body()
                query_text = doc.get("query_text")
                context = doc.get("context", {})
                if not isinstance(query_text, str) or not query_text.strip():
                    raise BridgeError(status_code=400, message="compile request requires query_text string")
                if not isinstance(context, dict):
                    raise BridgeError(status_code=400, message="compile request context must be object")
                compile_result = self.app.backend.compile_query(
                    dialect=dialect,
                    query_text=query_text,
                    context=context,
                )
                self._send_json(200, compile_result.to_dict())
                return

            if method == "POST" and tail == ["execute"]:
                doc = self._read_json_body()
                compile_artifact_id = doc.get("compile_artifact_id")
                query_text = doc.get("query_text")
                options = doc.get("options", {})
                if not isinstance(compile_artifact_id, str) or not compile_artifact_id:
                    raise BridgeError(
                        status_code=400,
                        message="execute request requires compile_artifact_id string",
                    )
                if not isinstance(query_text, str) or not query_text.strip():
                    raise BridgeError(status_code=400, message="execute request requires query_text string")
                if not isinstance(options, dict):
                    raise BridgeError(status_code=400, message="execute request options must be object")
                execute_result = self.app.backend.execute_query(
                    dialect=dialect,
                    compile_artifact_id=compile_artifact_id,
                    query_text=query_text,
                    options=options,
                )
                self._send_json(200, execute_result.to_dict())
                return

            if method == "GET" and tail == ["schemas"]:
                raw_database = query_params.get("database", [])
                database = raw_database[0] if raw_database else None
                schemas = self.app.backend.list_schemas(dialect=dialect, database=database)
                self._send_json(200, {"schemas": schemas})
                return

            if method == "GET" and len(tail) == 3 and tail[0] == "schemas" and tail[2] == "tables":
                schema = tail[1]
                tables = self.app.backend.list_tables(dialect=dialect, schema=schema)
                self._send_json(200, {"tables": tables})
                return

            if method == "GET" and len(tail) == 4 and tail[0] == "schemas" and tail[2] == "tables":
                schema = tail[1]
                table = tail[3]
                table_description = self.app.backend.describe_table(
                    dialect=dialect,
                    schema=schema,
                    table=table,
                )
                self._send_json(200, table_description)
                return

            raise BridgeError(status_code=404, message=f"Route not found: {self.path}")
        except BridgeError as exc:
            self._send_json(exc.status_code, {"error": {"message": exc.message}})
        except Exception as exc:  # pragma: no cover - defensive fallback
            self._send_json(500, {"error": {"message": f"internal bridge error: {exc}"}})

    def _dispatch_remote(
        self,
        method: str,
        segments: list[str],
        query_params: dict[str, list[str]],
    ) -> None:
        service = self.app.service
        if service is None:
            raise BridgeError(status_code=501, message="Remote MCP transport is not configured")

        if method == "POST" and segments == ["session", "open"]:
            doc = self._read_json_body()
            result = service.open_remote_session(doc)
            self._send_json(200, result)
            return

        if len(segments) >= 2 and segments[0] == "sessions":
            session_id = segments[1]

            if method == "POST" and len(segments) == 3 and segments[2] == "invoke":
                doc = self._read_json_body()
                request_id = str(doc.get("request_id", "")).strip()
                method_name = str(doc.get("method", "")).strip()
                if not request_id or not method_name:
                    raise BridgeError(
                        status_code=400,
                        message="remote invoke requires request_id and method",
                    )
                result = service.invoke_remote_tool(
                    session_id=session_id,
                    request_id=request_id,
                    method=method_name,
                    params=doc.get("params"),
                    client_operation_timeout_ms=doc.get("client_operation_timeout_ms"),
                    stream_requested=bool(doc.get("stream_requested", False)),
                    allow_background_execution=bool(
                        doc.get("allow_background_execution", False)
                    ),
                    cancellation_token=str(doc.get("cancellation_token", "")).strip() or None,
                )
                self._send_json(200, result)
                return

            if method == "POST" and len(segments) == 3 and segments[2] == "close":
                doc = self._read_json_body()
                result = service.close_remote_session(
                    session_id=session_id,
                    request_id=str(doc.get("request_id", "")).strip() or None,
                )
                self._send_json(200, result)
                return

            if len(segments) >= 4 and segments[2] == "operations":
                operation_id = segments[3]

                if method == "GET" and len(segments) == 4:
                    continuation_values = query_params.get("continuation_token", [])
                    continuation_token = continuation_values[0] if continuation_values else None
                    result = service.poll_remote_operation(
                        session_id=session_id,
                        operation_id=operation_id,
                        continuation_token=continuation_token,
                    )
                    self._send_json(200, result)
                    return

                if method == "GET" and len(segments) == 5 and segments[4] == "events":
                    continuation_values = query_params.get("continuation_token", [])
                    continuation_token = continuation_values[0] if continuation_values else None
                    result = service.poll_remote_operation(
                        session_id=session_id,
                        operation_id=operation_id,
                        continuation_token=continuation_token,
                    )
                    if result.get("error") is not None:
                        raise BridgeError(
                            status_code=400,
                            message=str(result["error"].get("message", "remote event stream failed")),
                        )
                    self._send_sse(result)
                    return

                if method == "POST" and len(segments) == 5 and segments[4] == "cancel":
                    doc = self._read_json_body()
                    request_id = str(doc.get("request_id", "")).strip()
                    reason = str(doc.get("reason", "")).strip()
                    if not request_id or not reason:
                        raise BridgeError(
                            status_code=400,
                            message="remote cancellation requires request_id and reason",
                        )
                    result = service.cancel_remote_operation(
                        session_id=session_id,
                        operation_id=operation_id,
                        request_id=request_id,
                        reason=reason,
                    )
                    self._send_json(200, result)
                    return

        raise BridgeError(status_code=404, message=f"Route not found: {self.path}")

    def _authorize(self) -> None:
        token = self.app.settings.api_token
        if not token:
            return
        auth_header = self.headers.get("Authorization", "").strip()
        token_headers = (
            self.headers.get("X-ScratchBird-Auth-Token", "").strip(),
            self.headers.get("X-API-Key", "").strip(),
        )
        authorized = False
        if auth_header:
            _scheme, _sep, credential = auth_header.partition(" ")
            authorized = bool(credential.strip()) and credential.strip() == token
        if not authorized:
            authorized = any(candidate == token for candidate in token_headers if candidate)
        if not authorized:
            raise BridgeError(status_code=401, message="Unauthorized")

    def _read_json_body(self) -> dict[str, Any]:
        raw_length = self.headers.get("Content-Length", "0")
        try:
            length = int(raw_length)
        except ValueError as exc:
            raise BridgeError(status_code=400, message="Invalid Content-Length") from exc

        if length < 0:
            raise BridgeError(status_code=400, message="Invalid Content-Length")
        if length > self.app.settings.request_max_bytes:
            raise BridgeError(status_code=413, message="Request body too large")

        payload = self.rfile.read(length) if length else b"{}"
        if not payload:
            return {}

        try:
            decoded = json.loads(payload.decode("utf-8"))
        except Exception as exc:
            raise BridgeError(status_code=400, message=f"Invalid JSON payload: {exc}") from exc

        if not isinstance(decoded, dict):
            raise BridgeError(status_code=400, message="JSON payload must be an object")
        return decoded

    def _send_json(self, status_code: int, doc: dict[str, Any]) -> None:
        body = json.dumps(doc, separators=(",", ":"), ensure_ascii=True).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_sse(self, poll_result: dict[str, Any]) -> None:
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.send_header("Connection", "close")
        self.end_headers()

        session_id = str(poll_result.get("session_id", "")).strip() or None
        for event in poll_result.get("events", []):
            event_doc = dict(event)
            if session_id is not None:
                event_doc["session_id"] = session_id
            event_name = str(event_doc.get("event_type", "message")).strip() or "message"
            self.wfile.write(f"event: {event_name}\n".encode("utf-8"))
            self.wfile.write(
                f"data: {json.dumps(event_doc, separators=(',', ':'), ensure_ascii=True)}\n\n".encode(
                    "utf-8"
                )
            )

        terminal_doc = {
            "session_id": session_id,
            "operation_id": poll_result.get("operation_id"),
            "request_id": poll_result.get("request_id"),
            "trace_id": poll_result.get("trace_id"),
            "operation_state": poll_result.get("operation_state"),
            "continuation_token": poll_result.get("continuation_token"),
            "terminal": bool(poll_result.get("terminal", False)),
        }
        self.wfile.write(b"event: stream_end\n")
        self.wfile.write(
            f"data: {json.dumps(terminal_doc, separators=(',', ':'), ensure_ascii=True)}\n\n".encode(
                "utf-8"
            )
        )
        self.wfile.flush()


def build_http_server(
    *,
    app: ScratchBirdBridgeApp,
) -> ThreadingHTTPServer:
    handler_cls = type("ScratchBirdBridgeHandler", (_BridgeHandler,), {"app": app})
    return ThreadingHTTPServer((app.settings.host, app.settings.port), handler_cls)


def run_http_bridge(
    *,
    settings: BridgeSettings | None = None,
    backend: BridgeBackend | None = None,
) -> None:
    runtime_settings = settings or BridgeSettings.from_env()
    runtime_backend = backend or ScratchBirdDriverBackend(runtime_settings)
    app = ScratchBirdBridgeApp(
        settings=runtime_settings,
        backend=runtime_backend,
        service=build_remote_mcp_bridge_service(runtime_settings),
    )
    server = build_http_server(app=app)
    server.serve_forever()


def main() -> None:
    run_http_bridge()


if __name__ == "__main__":
    main()
