# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""Connection implementation for ScratchBird Python driver (SBWP v1.1)."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

import socket
import ssl
import os
import select
import struct

from . import errors
from .circuit_breaker import CircuitBreaker, CircuitBreakerConfig, CircuitBreakerError
from .keepalive import KeepaliveManager, KeepaliveConfig
from .leak_detection import LeakDetector, LeakDetectionConfig
from .telemetry import TelemetryCollector, TelemetryConfig
from .dsn import (
    parse_dsn,
    normalize_compression_mode,
    normalize_front_door_mode,
    normalize_native_protocol,
    normalize_ssl_mode,
)
from .cursor import Cursor
from .protocol import (
    AuthMethod,
    MessageType,
    MSG_FLAG_URGENT,
    FEATURE_COMPRESSION,
    FEATURE_STREAMING,
    FEATURE_SBLR,
    FEATURE_NOTIFICATIONS,
    FEATURE_QUERY_PLAN,
    FEATURE_BINARY_COPY,
    QUERY_FLAG_DESCRIBE_ONLY,
    QUERY_FLAG_INCLUDE_PLAN,
    QUERY_FLAG_RETURN_SBLR,
    QUERY_FLAG_NO_CACHE,
    ISOLATION_READ_UNCOMMITTED,
    ISOLATION_READ_COMMITTED,
    ISOLATION_REPEATABLE_READ,
    ISOLATION_SERIALIZABLE,
    READ_COMMITTED_MODE_DEFAULT,
    READ_COMMITTED_MODE_READ_CONSISTENCY,
    READ_COMMITTED_MODE_RECORD_VERSION,
    READ_COMMITTED_MODE_NO_RECORD_VERSION,
    TXN_FLAG_HAS_ACCESS,
    TXN_FLAG_HAS_AUTOCOMMIT,
    TXN_FLAG_HAS_DEFERRABLE,
    TXN_FLAG_HAS_ISOLATION,
    TXN_FLAG_HAS_TIMEOUT,
    TXN_FLAG_HAS_WAIT,
    TXN_FLAG_HAS_READ_COMMITTED_MODE,
    COPY_FORMAT_TEXT,
    COPY_FORMAT_BINARY,
    HEADER_SIZE,
    ColumnInfo,
    MessageHeader,
    encode_message,
    decode_header,
    build_bind_payload,
    build_cancel_payload,
    build_describe_payload,
    build_execute_payload,
    build_parse_payload,
    build_query_payload,
    build_sblr_execute_payload,
    build_subscribe_payload,
    build_unsubscribe_payload,
    build_txn_begin_payload,
    build_txn_commit_payload,
    build_txn_rollback_payload,
    build_txn_savepoint_payload,
    build_txn_release_payload,
    build_txn_rollback_to_payload,
    build_set_option_payload,
    build_stream_control_payload,
    build_attach_create_payload,
    build_p1_startup_payload,
    AUTH_PARAM_METHOD_ID,
    AUTH_PARAM_METHOD_PAYLOAD,
    AUTH_PARAM_PAYLOAD_JSON,
    AUTH_PARAM_PAYLOAD_B64,
    AUTH_PARAM_PROVIDER_PROFILE,
    AUTH_PARAM_REQUIRED_METHODS,
    AUTH_PARAM_FORBIDDEN_METHODS,
    AUTH_PARAM_REQUIRE_CHANNEL_BINDING,
    AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN,
    AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION,
    AuthPluginSelection,
    apply_auth_plugin_selection,
    build_copy_data_payload,
    build_copy_done_payload,
    build_copy_fail_payload,
    build_native_rowset_payload,
    parse_auth_continue,
    parse_auth_ok,
    parse_auth_request,
    parse_command_complete,
    parse_copy_in_response,
    parse_copy_out_response,
    parse_data_row,
    parse_error_message,
    parse_notice,
    parse_notification,
    parse_query_plan,
    parse_query_progress,
    parse_sblr_compiled,
    parse_parameter_description,
    parse_parameter_statuses,
    parse_ready,
    parse_txn_status,
    parse_row_description,
)
from .scram import ScramExchange
from .sql import normalize_callable_query, normalize_query, split_top_level_statements
from .metadata import (
    build_ddl_editor_schema_payload,
    filter_rows_for_collection_family,
    filter_rows_by_restrictions,
    normalize_collection_name,
    resolve_collection_query,
)
from .types import FORMAT_BINARY, decode_value, encode_param

MANAGER_PROTOCOL_MAGIC = 0x42444253  # SBDB
MANAGER_PROTOCOL_VERSION = 0x0101
MANAGER_HEADER_SIZE = 12
MANAGER_MAX_PAYLOAD_SIZE = 16 * 1024 * 1024
MCP_PROTOCOL_VERSION = 0x0100

MCP_MSG_CONNECT_RESPONSE = 0x02
MCP_MSG_AUTH_CHALLENGE = 0x12
MCP_MSG_AUTH_RESPONSE = 0x11
MCP_MSG_STATUS_RESPONSE = 0x64
MCP_MSG_HELLO = 0x65
MCP_MSG_AUTH_START = 0x66
MCP_MSG_AUTH_CONTINUE = 0x67
MCP_MSG_DB_CONNECT = 0x69
MCP_AUTH_METHOD_TOKEN = 4

CANONICAL_ISOLATION_READ_COMMITTED = "READ COMMITTED"
CANONICAL_ISOLATION_READ_COMMITTED_READ_CONSISTENCY = "READ COMMITTED READ CONSISTENCY"
CANONICAL_ISOLATION_SNAPSHOT = "SNAPSHOT"
CANONICAL_ISOLATION_SNAPSHOT_TABLE_STABILITY = "SNAPSHOT TABLE STABILITY"

DEFAULT_AUTH_PLUGIN_IDS = {
    AuthMethod.PASSWORD: "scratchbird.auth.password_compat",
    AuthMethod.MD5: "scratchbird.auth.md5_legacy",
    AuthMethod.SCRAM_SHA_256: "scratchbird.auth.scram_sha_256",
    AuthMethod.SCRAM_SHA_512: "scratchbird.auth.scram_sha_512",
    AuthMethod.TOKEN: "scratchbird.auth.authkey_token",
    AuthMethod.PEER: "scratchbird.auth.peer_uid",
    AuthMethod.REATTACH: "scratchbird.auth.reattach",
}


def canonical_isolation_label(isolation_level: int) -> str:
    """Return the canonical MGA meaning of a Python isolation alias byte.

    The Python lane still uses the protocol's SQL-style compatibility aliases:

    - READ UNCOMMITTED is only a legacy compatibility alias here
    - READ COMMITTED maps to canonical READ COMMITTED
    - REPEATABLE READ maps to canonical SNAPSHOT
    - SERIALIZABLE maps to canonical SNAPSHOT TABLE STABILITY

    A distinct READ COMMITTED READ CONSISTENCY selector is not exposed in this
    lane yet.
    """

    mapping = {
        ISOLATION_READ_UNCOMMITTED: CANONICAL_ISOLATION_READ_COMMITTED,
        ISOLATION_READ_COMMITTED: CANONICAL_ISOLATION_READ_COMMITTED,
        ISOLATION_REPEATABLE_READ: CANONICAL_ISOLATION_SNAPSHOT,
        ISOLATION_SERIALIZABLE: CANONICAL_ISOLATION_SNAPSHOT_TABLE_STABILITY,
    }
    try:
        normalized = int(isolation_level)
    except (TypeError, ValueError):
        return f"UNKNOWN({isolation_level!r})"
    return mapping.get(normalized, f"UNKNOWN({normalized})")


def canonical_read_committed_mode_label(read_committed_mode: int) -> str:
    """Return the canonical MGA meaning of a read-committed mode selector."""

    mapping = {
        READ_COMMITTED_MODE_DEFAULT: CANONICAL_ISOLATION_READ_COMMITTED,
        READ_COMMITTED_MODE_READ_CONSISTENCY: CANONICAL_ISOLATION_READ_COMMITTED_READ_CONSISTENCY,
        READ_COMMITTED_MODE_RECORD_VERSION: "READ COMMITTED RECORD VERSION",
        READ_COMMITTED_MODE_NO_RECORD_VERSION: "READ COMMITTED NO RECORD VERSION",
    }
    try:
        normalized = int(read_committed_mode)
    except (TypeError, ValueError):
        return f"UNKNOWN({read_committed_mode!r})"
    return mapping.get(normalized, f"UNKNOWN({normalized})")


def _split_copy_csv_line(line: str) -> List[str]:
    fields: List[str] = []
    field = []
    in_quote = False
    i = 0
    while i <= len(line):
        ch = line[i] if i < len(line) else ","
        if ch == '"':
            if in_quote and i + 1 < len(line) and line[i + 1] == '"':
                field.append('"')
                i += 2
                continue
            in_quote = not in_quote
            i += 1
            continue
        if ch == "," and not in_quote:
            fields.append("".join(field))
            field.clear()
            i += 1
            continue
        field.append(ch)
        i += 1
    if in_quote:
        raise errors.ProgrammingError("malformed CSV COPY input")
    return fields


def _copy_text_rows_to_native_frame(data: bytes, column_types: Optional[Sequence[int | str]] = None) -> bytes:
    if data.startswith(b"SBNR"):
        return data
    try:
        text = data.decode("utf-8")
    except UnicodeDecodeError as exc:
        raise errors.ProgrammingError("binary COPY input is not a native rowset frame or UTF-8 COPY text") from exc
    lines = [line.rstrip("\r") for line in text.splitlines() if line.strip()]
    if not lines:
        raise errors.ProgrammingError("COPY input contains no rows")

    first = lines[0]
    if ";" in first and "=" in first:
        columns: List[str] = []
        rows: List[List[Optional[object]]] = []
        for line in lines:
            fields = []
            for item in line.split(";"):
                if not item:
                    continue
                name, sep, value = item.partition("=")
                if not sep or not name:
                    raise errors.ProgrammingError("malformed canonical COPY field")
                fields.append((name, None if value.upper() == "NULL" else value))
            if not fields:
                continue
            if not columns:
                columns = [name for name, _ in fields]
            elif [name for name, _ in fields] != columns:
                raise errors.ProgrammingError("COPY input changed row shape mid-stream")
            rows.append([value for _, value in fields])
        return build_native_rowset_payload(columns, rows, column_types)

    columns = [column.strip() for column in _split_copy_csv_line(first)]
    if not columns or any(not column for column in columns):
        raise errors.ProgrammingError("CSV COPY input requires a non-empty header row")
    rows = []
    for line in lines[1:]:
        values = _split_copy_csv_line(line)
        if len(values) != len(columns):
            raise errors.ProgrammingError("CSV COPY row shape mismatch")
        rows.append([None if value == "" or value.upper() == "NULL" else value for value in values])
    if not rows:
        raise errors.ProgrammingError("CSV COPY input contains no data rows")
    return build_native_rowset_payload(columns, rows, column_types)


@dataclass
class ConnectionConfig:
    host: str = "localhost"
    port: int = 3092
    front_door_mode: str = "direct"
    protocol: str = "native"
    database: Optional[str] = None
    user: Optional[str] = None
    password: Optional[str] = None
    schema: Optional[str] = None
    metadata_expand_schema_parents: bool = False
    sslmode: str = "require"
    sslrootcert: Optional[str] = None
    sslcert: Optional[str] = None
    sslkey: Optional[str] = None
    sslpassword: Optional[str] = None
    connect_timeout: int = 30
    socket_timeout: int = 0
    application_name: Optional[str] = None
    role: Optional[str] = None
    binary_transfer: bool = True
    compression: str = "off"
    manager_auth_token: Optional[str] = None
    manager_username: Optional[str] = None
    manager_database: Optional[str] = None
    manager_connection_profile: str = "SBsql"
    manager_client_intent: str = "SBsql"
    manager_client_flags: int = 0
    manager_auth_fast_path: bool = True
    connect_client_flags: int = 0x0100
    auth_token: Optional[str] = None
    auth_method_id: Optional[str] = None
    auth_method_payload: Optional[str] = None
    auth_payload_json: Optional[str] = None
    auth_payload_b64: Optional[str] = None
    auth_provider_profile: Optional[str] = None
    auth_required_methods: Optional[str] = None
    auth_forbidden_methods: Optional[str] = None
    auth_require_channel_binding: bool = False
    workload_identity_token: Optional[str] = None
    proxy_principal_assertion: Optional[str] = None
    dormant_id: Optional[str] = None
    dormant_reattach_token: Optional[str] = None
    extra: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class AuthMethodSurface:
    wire_method: str
    plugin_method_id: Optional[str]
    executable_locally: bool
    broker_required: bool


@dataclass(frozen=True)
class AuthProbeResult:
    reachable: bool
    ingress_mode: str
    resolved_host: str
    resolved_port: int
    admitted_methods: List[AuthMethodSurface]
    required_method: Optional[str]
    required_plugin_method_id: Optional[str]
    allowed_transport_mask: Optional[int]
    additional_continuation_possible: bool


@dataclass(frozen=True)
class ResolvedAuthContext:
    ingress_mode: str
    resolved_auth_method: Optional[str]
    resolved_auth_plugin_id: Optional[str]
    manager_authenticated: bool
    attached: bool


def _auth_method_name(method: int) -> Optional[str]:
    mapping = {
        AuthMethod.PASSWORD: "PASSWORD",
        AuthMethod.MD5: "MD5",
        AuthMethod.SCRAM_SHA_256: "SCRAM_SHA_256",
        AuthMethod.SCRAM_SHA_512: "SCRAM_SHA_512",
        AuthMethod.TOKEN: "TOKEN",
        AuthMethod.PEER: "PEER",
        AuthMethod.REATTACH: "REATTACH",
    }
    return mapping.get(method)


def _auth_plugin_id_for_method(method: int, configured_method_id: Optional[str]) -> Optional[str]:
    if configured_method_id and configured_method_id.strip():
        return configured_method_id.strip()
    return DEFAULT_AUTH_PLUGIN_IDS.get(method)


def _auth_method_executable_locally(method: int) -> bool:
    return method in (
        AuthMethod.PASSWORD,
        AuthMethod.SCRAM_SHA_256,
        AuthMethod.SCRAM_SHA_512,
        AuthMethod.TOKEN,
    )


def _auth_method_broker_required(method: int) -> bool:
    return method == AuthMethod.PEER


def _describe_auth_method(method: int, configured_method_id: Optional[str]) -> Optional[AuthMethodSurface]:
    wire_method = _auth_method_name(method)
    if wire_method is None:
        return None
    return AuthMethodSurface(
        wire_method=wire_method,
        plugin_method_id=_auth_plugin_id_for_method(method, configured_method_id),
        executable_locally=_auth_method_executable_locally(method),
        broker_required=_auth_method_broker_required(method),
    )


def _resolve_token_auth_payload(config: ConnectionConfig) -> Optional[bytes]:
    if config.auth_token:
        return config.auth_token.encode("utf-8")
    if config.auth_method_payload:
        return config.auth_method_payload.encode("utf-8")
    if config.auth_payload_b64:
        import base64

        return base64.b64decode(config.auth_payload_b64.encode("ascii"))
    if config.auth_payload_json:
        return config.auth_payload_json.encode("utf-8")
    if config.workload_identity_token:
        return config.workload_identity_token.encode("utf-8")
    if config.proxy_principal_assertion:
        return config.proxy_principal_assertion.encode("utf-8")
    return None


def _prepare_connection_params(dsn=None, user=None, password=None, host=None, database=None, **kwargs):
    params = {}
    params.update(parse_dsn(dsn))

    for key, value in kwargs.items():
        params[key] = value

    if user is not None:
        params["user"] = user
    if password is not None:
        params["password"] = password
    if host is not None:
        params["host"] = host
    if database is not None:
        params["database"] = database
    return params


def _build_connection_config(params: Dict[str, Any]) -> ConnectionConfig:
    cfg = ConnectionConfig()
    cfg.host = params.get("host", cfg.host)
    cfg.port = int(params.get("port", cfg.port))
    try:
        cfg.protocol = normalize_native_protocol(params.get("protocol", params.get("parser", params.get("dialect"))))
        cfg.front_door_mode = normalize_front_door_mode(
            params.get(
                "front_door_mode",
                params.get(
                    "frontdoormode",
                    params.get("frontDoorMode", params.get("connection_mode", params.get("ingress_mode"))),
                ),
            )
        )
    except ValueError as exc:
        raise errors.InterfaceError(str(exc)) from exc
    cfg.database = params.get("database", params.get("dbname", cfg.database))
    cfg.user = params.get("user", params.get("username", cfg.user))
    cfg.password = params.get("password", cfg.password)
    cfg.schema = params.get(
        "schema",
        params.get(
            "current_schema",
            params.get(
                "currentSchema",
                params.get(
                    "search_path",
                    params.get("searchpath", params.get("currentschema", cfg.schema)),
                ),
            ),
        ),
    )
    raw_expand_schema_parents = params.get(
        "metadata_expand_schema_parents",
        params.get(
            "metadataexpandschemaparents",
            params.get(
                "metadataExpandSchemaParents",
                params.get(
                    "expandschemaparents",
                    params.get(
                        "expand_schema_parents",
                        params.get(
                            "dbeaverexpandschemaparents",
                            params.get("dbeaver_expand_schema_parents", cfg.metadata_expand_schema_parents),
                        ),
                    ),
                ),
            ),
        ),
    )
    if isinstance(raw_expand_schema_parents, str):
        cfg.metadata_expand_schema_parents = raw_expand_schema_parents.lower() in ("1", "true", "yes", "on")
    else:
        cfg.metadata_expand_schema_parents = bool(raw_expand_schema_parents)
    try:
        cfg.sslmode = normalize_ssl_mode(str(params.get("sslmode", params.get("ssl", cfg.sslmode))))
    except ValueError as exc:
        raise errors.InterfaceError(str(exc)) from exc
    cfg.sslrootcert = params.get("sslrootcert", cfg.sslrootcert)
    cfg.sslcert = params.get("sslcert", cfg.sslcert)
    cfg.sslkey = params.get("sslkey", cfg.sslkey)
    cfg.sslpassword = params.get("sslpassword", cfg.sslpassword)
    cfg.connect_timeout = int(params.get("connect_timeout", params.get("connecttimeout", cfg.connect_timeout)))
    cfg.socket_timeout = int(params.get("socket_timeout", params.get("sockettimeout", cfg.socket_timeout)))
    cfg.application_name = params.get("application_name", params.get("applicationname", cfg.application_name))
    cfg.role = params.get("role", cfg.role)
    raw_binary = params.get("binary_transfer", params.get("binarytransfer", cfg.binary_transfer))
    if isinstance(raw_binary, str):
        cfg.binary_transfer = raw_binary.lower() in ("1", "true", "yes", "on")
    else:
        cfg.binary_transfer = bool(raw_binary)
    try:
        cfg.compression = normalize_compression_mode(str(params.get("compression", cfg.compression) or "off"))
    except ValueError as exc:
        raise errors.InterfaceError(str(exc)) from exc
    cfg.manager_auth_token = params.get("manager_auth_token", params.get("mcp_auth_token", cfg.manager_auth_token))
    cfg.manager_username = params.get("manager_username", params.get("mcp_username", cfg.manager_username))
    cfg.manager_database = params.get("manager_database", params.get("mcp_database", cfg.manager_database))
    cfg.manager_connection_profile = params.get(
        "manager_connection_profile", params.get("mcp_connection_profile", cfg.manager_connection_profile)
    ) or "SBsql"
    cfg.manager_client_intent = params.get(
        "manager_client_intent", params.get("mcp_client_intent", cfg.manager_client_intent)
    ) or "SBsql"
    raw_manager_flags = params.get("manager_client_flags", params.get("mcp_client_flags"))
    if raw_manager_flags is not None:
        try:
            cfg.manager_client_flags = int(raw_manager_flags)
        except ValueError:
            cfg.manager_client_flags = 0
    raw_connect_flags = params.get("client_flags", params.get("connect_client_flags"))
    if raw_connect_flags is not None:
        try:
            cfg.connect_client_flags = int(raw_connect_flags)
        except ValueError:
            cfg.connect_client_flags = 0x0100
    cfg.auth_token = params.get(
        "auth_token",
        params.get(
            "authtoken",
            params.get(
                "bearer_token",
                params.get(
                    "bearertoken",
                    params.get("token", cfg.auth_token),
                ),
            ),
        ),
    )
    raw_fast_path = params.get("manager_auth_fast_path", params.get("mcp_auth_fast_path"))
    if raw_fast_path is not None:
        if isinstance(raw_fast_path, str):
            cfg.manager_auth_fast_path = raw_fast_path.lower() in ("1", "true", "yes", "on")
        else:
            cfg.manager_auth_fast_path = bool(raw_fast_path)
    cfg.auth_method_id = params.get(AUTH_PARAM_METHOD_ID, params.get("authmethodid", cfg.auth_method_id))
    cfg.auth_method_payload = params.get(AUTH_PARAM_METHOD_PAYLOAD, params.get("authmethodpayload", cfg.auth_method_payload))
    cfg.auth_payload_json = params.get(AUTH_PARAM_PAYLOAD_JSON, params.get("authpayloadjson", cfg.auth_payload_json))
    cfg.auth_payload_b64 = params.get(AUTH_PARAM_PAYLOAD_B64, params.get("authpayloadb64", cfg.auth_payload_b64))
    cfg.auth_provider_profile = params.get(
        AUTH_PARAM_PROVIDER_PROFILE,
        params.get("authproviderprofile", cfg.auth_provider_profile),
    )
    cfg.auth_required_methods = params.get(
        AUTH_PARAM_REQUIRED_METHODS,
        params.get("authrequiredmethods", cfg.auth_required_methods),
    )
    cfg.auth_forbidden_methods = params.get(
        AUTH_PARAM_FORBIDDEN_METHODS,
        params.get("authforbiddenmethods", cfg.auth_forbidden_methods),
    )
    raw_require_channel_binding = params.get(
        AUTH_PARAM_REQUIRE_CHANNEL_BINDING,
        params.get("authrequirechannelbinding", cfg.auth_require_channel_binding),
    )
    if isinstance(raw_require_channel_binding, str):
        cfg.auth_require_channel_binding = raw_require_channel_binding.lower() in ("1", "true", "yes", "on")
    else:
        cfg.auth_require_channel_binding = bool(raw_require_channel_binding)
    cfg.workload_identity_token = params.get(
        AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN,
        params.get("workloadidentitytoken", cfg.workload_identity_token),
    )
    cfg.proxy_principal_assertion = params.get(
        AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION,
        params.get("proxyprincipalassertion", params.get("proxy_assertion", cfg.proxy_principal_assertion)),
    )
    cfg.dormant_id = params.get("dormant_id", params.get("dormantid", cfg.dormant_id))
    cfg.dormant_reattach_token = params.get(
        "dormant_reattach_token",
        params.get("dormantreattachtoken", cfg.dormant_reattach_token),
    )
    cfg.extra = {
        k: v
        for k, v in params.items()
        if k not in {
            "host",
            "port",
            "front_door_mode",
            "frontdoormode",
            "frontDoorMode",
            "connection_mode",
            "ingress_mode",
            "database",
            "dbname",
            "protocol",
            "parser",
            "dialect",
            "user",
            "username",
            "password",
            "schema",
            "search_path",
            "searchpath",
            "currentschema",
            "metadata_expand_schema_parents",
            "metadataexpandschemaparents",
            "metadataExpandSchemaParents",
            "expandschemaparents",
            "expand_schema_parents",
            "dbeaverexpandschemaparents",
            "dbeaver_expand_schema_parents",
            "sslmode",
            "ssl",
            "sslrootcert",
            "sslcert",
            "sslkey",
            "sslpassword",
            "connect_timeout",
            "connecttimeout",
            "socket_timeout",
            "sockettimeout",
            "application_name",
            "applicationname",
            "role",
            "binary_transfer",
            "binarytransfer",
            "compression",
            "manager_auth_token",
            "mcp_auth_token",
            "manager_username",
            "mcp_username",
            "manager_database",
            "mcp_database",
            "manager_connection_profile",
            "mcp_connection_profile",
            "manager_client_intent",
            "mcp_client_intent",
            "manager_client_flags",
            "mcp_client_flags",
            "manager_auth_fast_path",
            "mcp_auth_fast_path",
            "client_flags",
            "connect_client_flags",
            "auth_token",
            "authtoken",
            "bearer_token",
            "bearertoken",
            "token",
            AUTH_PARAM_METHOD_ID,
            "authmethodid",
            AUTH_PARAM_METHOD_PAYLOAD,
            "authmethodpayload",
            AUTH_PARAM_PAYLOAD_JSON,
            "authpayloadjson",
            AUTH_PARAM_PAYLOAD_B64,
            "authpayloadb64",
            AUTH_PARAM_PROVIDER_PROFILE,
            "authproviderprofile",
            AUTH_PARAM_REQUIRED_METHODS,
            "authrequiredmethods",
            AUTH_PARAM_FORBIDDEN_METHODS,
            "authforbiddenmethods",
            AUTH_PARAM_REQUIRE_CHANNEL_BINDING,
            "authrequirechannelbinding",
            AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN,
            "workloadidentitytoken",
            AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION,
            "proxyprincipalassertion",
            "proxy_assertion",
            "dormant_id",
            "dormantid",
            "dormant_reattach_token",
            "dormantreattachtoken",
        }
    }

    return cfg


def connect(dsn=None, user=None, password=None, host=None, database=None, **kwargs):
    params = _prepare_connection_params(dsn, user, password, host, database, **kwargs)
    return Connection(_build_connection_config(params))


def probe_auth_surface(dsn=None, user=None, password=None, host=None, database=None, **kwargs) -> AuthProbeResult:
    params = _prepare_connection_params(dsn, user, password, host, database, **kwargs)
    conn = Connection.__new__(Connection)
    conn._config = _build_connection_config(params)
    conn._session_schema = _normalize_session_schema(conn._config.schema)
    conn._config.schema = conn._session_schema
    conn._closed = False
    conn._cursors = []
    conn._autocommit = True
    conn._warnings = None
    conn._socket = None
    conn._connected = False
    conn._sequence = 0
    conn._attachment_id = b"\x00" * 16
    conn._txn_id = 0
    conn._runtime_txn_active = False
    conn._portal_resume_pending = False
    conn._authed = False
    conn._parameters = {}
    conn._notification_handlers = []
    conn._prefetched_message = None
    conn._last_plan = None
    conn._last_notice = None
    conn._last_query_progress = None
    conn._last_sblr = None
    conn._conn_id = f"probe-{id(conn)}"
    conn._cancel_requested = False
    conn._cancel_socket_timeout = None
    conn._cancel_timeout_seconds = 0.2
    conn._keepalive = None
    conn._keepalive_tracker = None
    conn._skip_schema_apply_once = False
    conn._batched_insert_statement_cache = True
    conn._prepared_exec_counter = 0
    conn._prepared_exec_cache = {}
    conn._circuit_breaker = None
    conn._telemetry = None
    conn._leak_detector = None
    conn._leak_guard = None
    conn._resolved_auth_context = ResolvedAuthContext(
        ingress_mode=conn._config.front_door_mode,
        resolved_auth_method=None,
        resolved_auth_plugin_id=None,
        manager_authenticated=False,
        attached=False,
    )
    return conn.probe_auth_surface()


class Connection:
    def __init__(self, config: ConnectionConfig):
        self._config = config
        self._session_schema = _normalize_session_schema(self._config.schema)
        self._config.schema = self._session_schema
        self._closed = False
        self._cursors = []
        self._autocommit = True
        self._warnings = None
        self._socket = None
        self._connected = False
        self._sequence = 0
        self._attachment_id = b"\x00" * 16
        self._txn_id = 0
        self._runtime_txn_active = False
        self._portal_resume_pending = False
        self._authed = False
        self._parameters: Dict[str, str] = {}
        self._notification_handlers = []
        self._prefetched_message = None
        self._last_plan = None
        self._last_notice = None
        self._last_query_progress = None
        self._last_sblr = None
        self._conn_id = f"conn-{id(self)}"
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode=self._config.front_door_mode,
            resolved_auth_method=None,
            resolved_auth_plugin_id=None,
            manager_authenticated=False,
            attached=False,
        )
        self._cancel_requested = False
        self._cancel_socket_timeout = None
        self._cancel_timeout_seconds = 0.2
        self._keepalive_tracker = None
        self._skip_schema_apply_once = False
        self._batched_insert_statement_cache = True
        self._prepared_exec_counter = 0
        self._prepared_exec_cache: Dict[Tuple[str, Tuple[int, ...]], Tuple[str, int]] = {}
        self._circuit_breaker = CircuitBreaker(CircuitBreakerConfig(), name=self._conn_id)
        self._telemetry = TelemetryCollector(TelemetryConfig())
        self._keepalive = KeepaliveManager(KeepaliveConfig())
        self._keepalive.start()
        self._leak_detector = LeakDetector(LeakDetectionConfig())
        self._leak_detector.start()
        self._leak_guard = self._leak_detector.checkout(self._conn_id, {"driver": "python"})
        self._connect()

    def get_resolved_auth_context(self) -> ResolvedAuthContext:
        return ResolvedAuthContext(
            ingress_mode=self._resolved_auth_context.ingress_mode,
            resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
            resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
            manager_authenticated=self._resolved_auth_context.manager_authenticated,
            attached=self._resolved_auth_context.attached,
        )

    def route_evidence_snapshot(self) -> Dict[str, Any]:
        """Return advisory live-route evidence for cross-route gates.

        This does not confer transaction visibility, finality, or security
        authority on the driver; it only exposes state already published by the
        engine/listener connection protocol for external equivalence checks.
        """
        auth = self.get_resolved_auth_context()
        return {
            "live_route_executed": self._connected and not self._closed,
            "attachment_id": self._attachment_id.hex(),
            "local_transaction_id": int(self._txn_id),
            "snapshot_visible_through_local_transaction_id": self._parameters.get(
                "session.snapshot_visible_through_local_transaction_id", ""),
            "transaction_snapshot_id": (
                "mga-route-snapshot:"
                + str(self._config.database or "")
                + ":"
                + str(self._parameters.get(
                    "session.snapshot_visible_through_local_transaction_id",
                    str(int(self._txn_id))))
                + ":local-txn:"
                + str(int(self._txn_id))
            ),
            "runtime_transaction_active": self._transaction_active(),
            "autocommit": self._autocommit,
            "database_name": self._config.database,
            "host": self._config.host,
            "port": int(self._config.port or 0),
            "username": self._config.user,
            "role": self._config.role,
            "schema": self._session_schema,
            "front_door_mode": self._config.front_door_mode,
            "security_epoch_available": bool(self._parameters.get("security.generation")),
            "security_epoch": self._parameters.get("security.generation", ""),
            "authenticated_user_uuid": self._parameters.get("session.authenticated_user_uuid", ""),
            "auth_provider_family": self._parameters.get("session.auth_provider_family", ""),
            "principal_claim": self._parameters.get("session.principal_claim", ""),
            "parameter_status_names": sorted(self._parameters.keys()),
            "resolved_auth_context": {
                "ingress_mode": auth.ingress_mode,
                "resolved_auth_method": auth.resolved_auth_method,
                "resolved_auth_plugin_id": auth.resolved_auth_plugin_id,
                "manager_authenticated": auth.manager_authenticated,
                "attached": auth.attached,
            },
        }

    def _reset_resolved_auth_context(self) -> None:
        try:
            ingress_mode = normalize_front_door_mode(self._config.front_door_mode)
        except ValueError:
            ingress_mode = self._config.front_door_mode
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode=ingress_mode,
            resolved_auth_method=None,
            resolved_auth_plugin_id=None,
            manager_authenticated=False,
            attached=False,
        )

    def _startup_features(self) -> int:
        features = FEATURE_SBLR | FEATURE_NOTIFICATIONS | FEATURE_QUERY_PLAN
        if self._config.compression.lower() == "zstd":
            features |= FEATURE_COMPRESSION
        if self._config.binary_transfer:
            features |= FEATURE_STREAMING
        return features

    def _startup_required_features(self) -> int:
        return 0

    def probe_auth_surface(self) -> AuthProbeResult:
        self._reset_resolved_auth_context()
        self._open_socket(require_identity=False, require_manager_token=False)
        try:
            resolved_host = self._config.host or "localhost"
            resolved_port = int(self._config.port or 3092)
            if self._config.front_door_mode == "manager_proxy":
                return self._probe_manager_auth_surface(resolved_host, resolved_port)
            return self._probe_direct_auth_surface(resolved_host, resolved_port)
        finally:
            self._disconnect_socket_for_reconnect()

    def _open_socket(self, require_identity: bool = True, require_manager_token: bool = True) -> None:
        try:
            self._config.protocol = normalize_native_protocol(self._config.protocol)
            self._config.front_door_mode = normalize_front_door_mode(self._config.front_door_mode)
            self._config.sslmode = normalize_ssl_mode(self._config.sslmode)
            self._config.compression = normalize_compression_mode(self._config.compression)
        except ValueError as exc:
            raise errors.InterfaceError(str(exc)) from exc
        if require_identity and (not self._config.user or not self._config.database):
            raise errors.InterfaceError("user and database are required")
        if require_manager_token and self._config.front_door_mode == "manager_proxy" and not self._config.manager_auth_token:
            raise errors.InterfaceError("manager_proxy mode requires manager_auth_token")
        try:
            raw_sock = socket.create_connection(
                (self._config.host, self._config.port),
                timeout=self._config.connect_timeout,
            )
        except TimeoutError as exc:
            raise errors.OperationalError("[08001] connection timed out") from exc
        except OSError as exc:
            raise errors.OperationalError(f"[08001] failed to connect: {exc}") from exc
        raw_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        raw_sock.settimeout(self._config.socket_timeout or None)

        sslmode = self._config.sslmode
        if sslmode == "disable":
            self._socket = raw_sock
            return

        ctx = ssl.create_default_context()
        ctx.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx.maximum_version = ssl.TLSVersion.TLSv1_3
        if sslmode in ("verify-ca", "verify-full"):
            ctx.check_hostname = sslmode == "verify-full"
            ctx.verify_mode = ssl.CERT_REQUIRED
        else:
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        if self._config.sslrootcert:
            ctx.load_verify_locations(self._config.sslrootcert)
        if self._config.sslcert and self._config.sslkey:
            ctx.load_cert_chain(self._config.sslcert, self._config.sslkey, password=self._config.sslpassword)

        try:
            self._socket = ctx.wrap_socket(raw_sock, server_hostname=self._config.host)
        except ssl.SSLError as exc:
            raw_sock.close()
            raise errors.OperationalError(f"[08001] TLS handshake failed: {exc}") from exc

    def _probe_direct_auth_surface(self, resolved_host: str, resolved_port: int) -> AuthProbeResult:
        payload = build_p1_startup_payload(
            self._startup_features(),
            self._startup_required_features(),
            self._build_startup_params())
        self._send_message(MessageType.STARTUP, payload, force_zero=True)

        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.NEGOTIATE_VERSION:
                continue
            if header.msg_type == MessageType.AUTH_REQUEST:
                method, _ = parse_auth_request(payload)
                method_surface = _describe_auth_method(method, self._config.auth_method_id)
                return AuthProbeResult(
                    reachable=True,
                    ingress_mode="direct",
                    resolved_host=resolved_host,
                    resolved_port=resolved_port,
                    admitted_methods=[method_surface] if method_surface is not None else [],
                    required_method=method_surface.wire_method if method_surface is not None else None,
                    required_plugin_method_id=method_surface.plugin_method_id if method_surface is not None else None,
                    allowed_transport_mask=None,
                    additional_continuation_possible=method in (
                        AuthMethod.SCRAM_SHA_256,
                        AuthMethod.SCRAM_SHA_512,
                        AuthMethod.TOKEN,
                        AuthMethod.PEER,
                    ),
                )
            if header.msg_type in (MessageType.AUTH_OK, MessageType.READY):
                return AuthProbeResult(
                    reachable=True,
                    ingress_mode="direct",
                    resolved_host=resolved_host,
                    resolved_port=resolved_port,
                    admitted_methods=[],
                    required_method=None,
                    required_plugin_method_id=None,
                    allowed_transport_mask=None,
                    additional_continuation_possible=False,
                )
            if header.msg_type == MessageType.ERROR:
                self._raise_protocol_error(payload)

    def _probe_manager_auth_surface(self, resolved_host: str, resolved_port: int) -> AuthProbeResult:
        manager_user = self._config.manager_username or self._config.user or "admin"

        hello_payload = struct.pack("<HH", MCP_PROTOCOL_VERSION, int(self._config.manager_client_flags or 0) & 0xFFFF)
        self._send_manager_frame(MCP_MSG_HELLO, hello_payload)
        msg_type, _ = self._recv_manager_frame()
        if msg_type != MCP_MSG_STATUS_RESPONSE:
            raise errors.OperationalError("[08P01] expected MCP hello status response")

        auth_start = bytearray()
        auth_start += self._pack_lpreface(manager_user)
        auth_start += struct.pack("<B", MCP_AUTH_METHOD_TOKEN)
        auth_start += struct.pack("<I", 0)
        self._send_manager_frame(MCP_MSG_AUTH_START, bytes(auth_start))
        msg_type, _ = self._recv_manager_frame()

        return AuthProbeResult(
            reachable=True,
            ingress_mode="manager_proxy",
            resolved_host=resolved_host,
            resolved_port=resolved_port,
            admitted_methods=[
                AuthMethodSurface(
                    wire_method="TOKEN",
                    plugin_method_id="scratchbird.auth.authkey_token",
                    executable_locally=True,
                    broker_required=False,
                )
            ],
            required_method="TOKEN",
            required_plugin_method_id="scratchbird.auth.authkey_token",
            allowed_transport_mask=None,
            additional_continuation_possible=msg_type == MCP_MSG_AUTH_CHALLENGE,
        )

    def _connect(self) -> None:
        self._reset_resolved_auth_context()
        self._open_socket()
        if self._config.front_door_mode == "manager_proxy":
            self._perform_manager_connect()
        self._startup_and_auth()
        if getattr(self, "_skip_schema_apply_once", False):
            self._skip_schema_apply_once = False
        else:
            self._apply_schema()
        self._connected = True
        if getattr(self, "_keepalive", None) and getattr(self, "_keepalive_tracker", None):
            self._keepalive.unregister(self._conn_id)
            self._keepalive_tracker = None
        if getattr(self, "_keepalive", None):
            self._keepalive_tracker = self._keepalive.register(
                self._conn_id,
                self._ping_for_keepalive,
            )
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode=self._resolved_auth_context.ingress_mode,
            resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
            resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
            manager_authenticated=self._resolved_auth_context.manager_authenticated,
            attached=True,
        )

    def _ping_for_keepalive(self) -> bool:
        try:
            self.ping()
            return True
        except Exception:
            return False

    def close(self) -> None:
        if not self._closed:
            self._closed = True
            self._connected = False
            self._resolved_auth_context = ResolvedAuthContext(
                ingress_mode=self._resolved_auth_context.ingress_mode,
                resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
                resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
                manager_authenticated=self._resolved_auth_context.manager_authenticated,
                attached=False,
            )
            self._portal_resume_pending = False
            self._prepared_exec_cache.clear()
            self._clear_transaction_state()
            self._clear_cancel_timeout()
            try:
                if self._keepalive:
                    self._keepalive.unregister(self._conn_id)
                    self._keepalive.stop()
            except Exception:
                pass
            try:
                if self._leak_guard:
                    self._leak_guard.release()
                if self._leak_detector:
                    self._leak_detector.stop()
            except Exception:
                pass
            if self._socket:
                try:
                    self._send_message(MessageType.TERMINATE, b"")
                except Exception:
                    pass
                try:
                    self._socket.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
                try:
                    self._socket.close()
                except OSError:
                    pass
                self._socket = None

    def _disconnect_socket_for_reconnect(self) -> None:
        self._connected = False
        self._authed = False
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode=self._resolved_auth_context.ingress_mode,
            resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
            resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
            manager_authenticated=self._resolved_auth_context.manager_authenticated,
            attached=False,
        )
        self._portal_resume_pending = False
        self._attachment_id = b"\x00" * 16
        self._parameters.clear()
        self._prepared_exec_cache.clear()
        self._clear_transaction_state()
        self._clear_cancel_timeout()
        if getattr(self, "_keepalive", None) and getattr(self, "_keepalive_tracker", None):
            try:
                self._keepalive.unregister(self._conn_id)
            except Exception:
                pass
            self._keepalive_tracker = None
        if self._socket:
            try:
                self._socket.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            try:
                self._socket.close()
            except OSError:
                pass
            self._socket = None

    def _reconnect_with_dormant_params(
        self,
        dormant_id: str,
        dormant_reattach_token: str,
    ) -> None:
        prior_dormant_id = self._config.dormant_id
        prior_dormant_token = self._config.dormant_reattach_token
        prior_skip_schema = self._skip_schema_apply_once
        self._config.dormant_id = dormant_id
        self._config.dormant_reattach_token = dormant_reattach_token
        self._skip_schema_apply_once = True
        self._disconnect_socket_for_reconnect()
        try:
            self._connect()
        except Exception:
            self._disconnect_socket_for_reconnect()
            raise
        finally:
            self._config.dormant_id = prior_dormant_id
            self._config.dormant_reattach_token = prior_dormant_token
            self._skip_schema_apply_once = prior_skip_schema

    def _begin_operation(self, name: str, sql: Optional[str] = None):
        if self._circuit_breaker and not self._circuit_breaker.allow_request():
            raise CircuitBreakerError("circuit breaker is open")
        if getattr(self, "_keepalive_tracker", None):
            self._keepalive_tracker.mark_active()
        span = None
        if self._telemetry:
            span = self._telemetry.start_span(name)
            if span is not None and sql:
                sql_text = sql
                if self._telemetry.config.sanitize_queries:
                    sql_text = self._telemetry.sanitize_query(sql_text)
                span.with_attribute("sql", sql_text)
        return span

    def _end_operation(self, span, success: bool) -> None:
        if self._circuit_breaker:
            if success:
                self._circuit_breaker.record_success()
            else:
                self._circuit_breaker.record_failure()
        if self._telemetry and span is not None:
            self._telemetry.end_span(span, success=success)

    def commit(self) -> None:
        self._ensure_open()
        if not self._transaction_active():
            return
        payload = build_txn_commit_payload(0)
        self._send_message(MessageType.TXN_COMMIT, payload)
        self._drain_until_ready()
        self._drain_immediate_reopen_boundary()

    def rollback(self) -> None:
        self._ensure_open()
        if not self._transaction_active():
            return
        payload = build_txn_rollback_payload(0)
        self._send_message(MessageType.TXN_ROLLBACK, payload)
        self._drain_until_ready()
        self._drain_immediate_reopen_boundary()

    def supports_prepared_transactions(self) -> bool:
        return True

    def supports_dormant_reattach(self) -> bool:
        return True

    def prepare_transaction(self, gid: str) -> None:
        self._ensure_open()
        self._execute_command(self._build_prepared_transaction_sql("PREPARE TRANSACTION", gid))

    def commit_prepared(self, gid: str) -> None:
        self._ensure_open()
        self._execute_command(self._build_prepared_transaction_sql("COMMIT PREPARED", gid))

    def rollback_prepared(self, gid: str) -> None:
        self._ensure_open()
        self._execute_command(self._build_prepared_transaction_sql("ROLLBACK PREPARED", gid))

    def detach_to_dormant(self) -> Tuple[str, str]:
        self._ensure_open()
        self._parameters.pop("dormant_id", None)
        self._parameters.pop("dormant_reattach_token", None)
        self.attach_detach()
        dormant_id = self._parameters.get("dormant_id")
        reattach_token = self._parameters.get("dormant_reattach_token")
        if not dormant_id or not reattach_token:
            raise errors.OperationalError(
                "[08006] expected dormant detach identifiers from the server"
            )
        return (
            _normalize_uuid_text(dormant_id, "dormant_id"),
            _normalize_uuid_text(reattach_token, "dormant_reattach_token"),
        )

    def reattach_dormant(self, dormant_id: str, auth_token: Optional[str] = None) -> None:
        self._ensure_open()
        if auth_token is None:
            raise errors.ProgrammingError(
                "[42601] dormant reattach requires the engine-issued auth token"
            )
        self._reconnect_with_dormant_params(
            _normalize_uuid_text(dormant_id, "dormant_id"),
            _normalize_uuid_text(auth_token, "dormant_reattach_token"),
        )

    def begin(self, **kwargs) -> None:
        self._ensure_open()
        flags = 0
        # Python currently exposes the protocol's SQL-style isolation aliases.
        # canonical_isolation_label(...) documents the MGA meaning of each byte.
        isolation = kwargs.get("isolation_level", ISOLATION_READ_COMMITTED)
        read_committed_mode = kwargs.get("read_committed_mode", None)
        if read_committed_mode is not None:
            if isolation not in (ISOLATION_READ_UNCOMMITTED, ISOLATION_READ_COMMITTED):
                raise errors.NotSupportedError(
                    "read_committed_mode requires a READ COMMITTED isolation alias"
                )
            flags |= TXN_FLAG_HAS_READ_COMMITTED_MODE
            if "isolation_level" not in kwargs:
                isolation = ISOLATION_READ_COMMITTED
                flags |= TXN_FLAG_HAS_ISOLATION
        if "isolation_level" in kwargs:
            flags |= TXN_FLAG_HAS_ISOLATION
        if "access_mode" in kwargs:
            flags |= TXN_FLAG_HAS_ACCESS
        if "deferrable" in kwargs:
            flags |= TXN_FLAG_HAS_DEFERRABLE
        if "wait" in kwargs:
            flags |= TXN_FLAG_HAS_WAIT
        if "timeout_ms" in kwargs:
            flags |= TXN_FLAG_HAS_TIMEOUT
        if "autocommit_mode" in kwargs:
            flags |= TXN_FLAG_HAS_AUTOCOMMIT
        payload = build_txn_begin_payload(
            flags,
            kwargs.get("conflict_action", 0),
            kwargs.get("autocommit_mode", 0),
            isolation,
            kwargs.get("access_mode", 0),
            1 if kwargs.get("deferrable") else 0,
            1 if kwargs.get("wait") else 0,
            kwargs.get("timeout_ms", 0),
            read_committed_mode if read_committed_mode is not None else READ_COMMITTED_MODE_DEFAULT,
        )
        self._send_message(MessageType.TXN_BEGIN, payload)
        self._drain_until_ready()

    def savepoint(self, name: str) -> None:
        self._ensure_open()
        if not self._transaction_active():
            raise errors.ProgrammingError("savepoint requires an active transaction")
        payload = build_txn_savepoint_payload(self._normalize_savepoint_name(name))
        self._send_message(MessageType.TXN_SAVEPOINT, payload)
        self._drain_until_ready()

    def release_savepoint(self, name: str) -> None:
        self._ensure_open()
        if not self._transaction_active():
            raise errors.ProgrammingError("release_savepoint requires an active transaction")
        payload = build_txn_release_payload(self._normalize_savepoint_name(name))
        self._send_message(MessageType.TXN_RELEASE, payload)
        self._drain_until_ready()

    def rollback_to_savepoint(self, name: str) -> None:
        self._ensure_open()
        if not self._transaction_active():
            raise errors.ProgrammingError("rollback_to_savepoint requires an active transaction")
        payload = build_txn_rollback_to_payload(self._normalize_savepoint_name(name))
        self._send_message(MessageType.TXN_ROLLBACK_TO, payload)
        self._drain_until_ready()

    def set_option(self, name: str, value: str) -> None:
        self._ensure_open()
        payload = build_set_option_payload(name, value)
        self._send_message(MessageType.SET_OPTION, payload)
        self._drain_until_ready()

    def ping(self) -> None:
        self._ensure_open()
        self._send_message(MessageType.PING, b"")
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.PONG:
                return
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                return
            if header.msg_type == MessageType.ERROR:
                self._raise_protocol_error(payload)

    def is_valid(self, timeout_ms: int = 0) -> bool:
        if self._closed:
            return False
        try:
            timeout_value = int(timeout_ms)
        except (TypeError, ValueError) as exc:
            raise errors.ProgrammingError("timeout_ms must be an integer") from exc
        if timeout_value < 0:
            raise errors.ProgrammingError("timeout_ms must be >= 0")

        sock = getattr(self, "_socket", None)
        original_timeout = None
        timeout_applied = False
        if timeout_value > 0 and sock is not None and hasattr(sock, "gettimeout") and hasattr(sock, "settimeout"):
            try:
                original_timeout = sock.gettimeout()
                sock.settimeout(timeout_value / 1000.0)
                timeout_applied = True
            except Exception:
                timeout_applied = False
        try:
            self.ping()
            return True
        except Exception:
            return False
        finally:
            if timeout_applied:
                try:
                    sock.settimeout(original_timeout)
                except Exception:
                    pass

    def subscribe(self, channel: str, sub_type: int = 0, filter_expr: str = "") -> None:
        self._ensure_open()
        payload = build_subscribe_payload(sub_type, channel, filter_expr)
        self._send_message(MessageType.SUBSCRIBE, payload)
        self._drain_until_ready()

    def unsubscribe(self, channel: str) -> None:
        self._ensure_open()
        payload = build_unsubscribe_payload(channel)
        self._send_message(MessageType.UNSUBSCRIBE, payload)
        self._drain_until_ready()

    def execute_sblr(self, sblr_hash: int, sblr_bytecode: Optional[bytes] = None, params=None):
        self._ensure_open()
        span = self._begin_operation("execute_sblr", "")
        param_values = []
        if params:
            for param in params:
                value, _ = encode_param(param)
                param_values.append(value)
        payload = build_sblr_execute_payload(sblr_hash, sblr_bytecode, param_values)
        try:
            self._send_message(MessageType.SBLR_EXECUTE, payload)
            self._send_message(MessageType.SYNC, b"")
            self._end_operation(span, True)
        except Exception:
            self._end_operation(span, False)
            raise
        return ResultStream(self, 0)

    def stream_control(self, control_type: int, window_size: int, timeout_ms: int) -> None:
        self._ensure_open()
        payload = build_stream_control_payload(control_type, window_size, timeout_ms)
        self._send_message(MessageType.STREAM_CONTROL, payload)

    def attach_create(self, emulation_mode: str, db_name: str) -> None:
        self._ensure_open()
        payload = build_attach_create_payload(emulation_mode, db_name)
        self._send_message(MessageType.ATTACH_CREATE, payload)
        self._drain_until_ready()

    def attach_detach(self) -> None:
        self._ensure_open()
        self._send_message(MessageType.ATTACH_DETACH, b"")
        self._drain_until_ready()

    def attach_list(self):
        self._ensure_open()
        self._send_message(MessageType.ATTACH_LIST, b"")
        self._send_message(MessageType.SYNC, b"")
        return ResultStream(self, 0)

    def on_notification(self, handler) -> None:
        self._notification_handlers.append(handler)

    def last_plan(self):
        return self._last_plan

    def last_sblr(self):
        return self._last_sblr

    def cursor(self) -> Cursor:
        self._ensure_open()
        cur = Cursor(self)
        self._cursors.append(cur)
        return cur

    def execute(self, sql: str, params=None) -> Cursor:
        cur = self.cursor()
        cur.execute(sql, params)
        return cur

    def query_multi(self, sql: str, params=None):
        self._ensure_open()
        split_results = self._execute_multi_statement_query(sql, params)
        if split_results is not None:
            return split_results
        cur = self.execute(sql, params)
        results = []
        while True:
            rows = cur.fetchall()
            row_count = int(cur.rowcount) if isinstance(cur.rowcount, int) else -1
            results.append(
                {
                    "rows": rows,
                    "rowCount": row_count,
                    "fields": cur.description or [],
                    "command": cur.statusmessage,
                    "lastId": cur.lastrowid,
                }
            )
            if cur.nextset() is None:
                break
        return results

    def execute_multi(self, sql: str, params=None):
        return self.query_multi(sql, params)

    def execute_with_generated_keys(self, sql: str, params=None):
        cur = self.execute(sql, params)
        cur.fetchall()
        return cur.get_generated_keys()

    def native_sql(self, sql: str, params=None) -> str:
        self._ensure_open()
        if sql is None:
            raise errors.ProgrammingError("sql is required")
        try:
            normalized_sql, _ = normalize_query(sql, params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc
        return normalized_sql

    def native_callable_sql(self, sql: str, params=None) -> str:
        self._ensure_open()
        if sql is None:
            raise errors.ProgrammingError("sql is required")
        try:
            normalized_sql, _ = normalize_callable_query(sql, params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc
        return normalized_sql

    def call(self, sql: str, params=None) -> Cursor:
        self._ensure_open()
        if sql is None:
            raise errors.ProgrammingError("sql is required")
        try:
            normalized_sql, ordered_params = normalize_callable_query(sql, params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc
        return self.execute(normalized_sql, ordered_params)

    def executemany(self, sql: str, seq_of_params) -> Cursor:
        cur = self.cursor()
        cur.executemany(sql, seq_of_params)
        return cur

    def execute_batch(self, sql: str, batch_params):
        self._ensure_open()
        if sql is None:
            raise errors.ProgrammingError("sql is required")
        if batch_params is None:
            raise errors.ProgrammingError("batch_params is required")
        items = []
        total_row_count = 0
        for index, params in enumerate(batch_params):
            cur = self.execute(sql, params)
            cur.fetchall()
            row_count = int(cur.rowcount) if isinstance(cur.rowcount, int) else -1
            if row_count > 0:
                total_row_count += row_count
            items.append(
                {
                    "index": index,
                    "rowCount": row_count,
                    "fields": cur.description or [],
                    "command": cur.statusmessage,
                    "lastId": cur.lastrowid,
                }
            )
        return {
            "items": items,
            "totalRowCount": total_row_count,
        }

    def query_batch(self, sql: str, batch_params):
        return self.execute_batch(sql, batch_params)

    def query_metadata(self, collection_name: str = "tables", restrictions: Optional[Dict[str, Any]] = None) -> Cursor:
        self._ensure_open()
        normalized_collection = self._normalize_metadata_collection(collection_name)
        metadata_sql = resolve_collection_query(normalized_collection)
        try:
            cur = self.execute(metadata_sql)
        except errors.ProgrammingError as exc:
            if self._metadata_collection_missing(exc):
                return self._cursor_from_rows([], description=[])
            raise
        needs_family_filter = normalized_collection in {"primary_keys", "foreign_keys", "procedures", "functions"}
        if not restrictions and not needs_family_filter:
            return cur

        rows = cur.fetchall()
        column_names = self._metadata_column_names(cur.description)
        rows = filter_rows_for_collection_family(
            rows,
            normalized_collection,
            column_names=column_names,
        )
        if not restrictions:
            return self._cursor_from_rows(
                rows,
                description=cur.description,
                statusmessage=cur.statusmessage,
                lastrowid=cur.lastrowid,
            )
        try:
            filtered_rows = filter_rows_by_restrictions(
                rows,
                restrictions,
                collection_name=normalized_collection,
                column_names=column_names,
            )
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc
        return self._cursor_from_rows(
            filtered_rows,
            description=cur.description,
            statusmessage=cur.statusmessage,
            lastrowid=cur.lastrowid,
        )

    def get_schema(self, collection_name: str = "tables", restrictions: Optional[Dict[str, Any]] = None):
        cur = self.query_metadata(collection_name, restrictions=restrictions)
        return cur.fetchall()

    def schemas(self, catalog: Optional[str] = None):
        restrictions = self._metadata_restrictions(catalog=catalog)
        return self.get_schema("schemas", restrictions=restrictions)

    def tables(self, schema: Optional[str] = None, table: Optional[str] = None, table_type: Optional[str] = None):
        restrictions = self._metadata_restrictions(schema=schema, table=table, type=table_type)
        return self.get_schema("tables", restrictions=restrictions)

    def columns(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        column: Optional[str] = None,
        column_type: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(schema=schema, table=table, column=column, type=column_type)
        return self.get_schema("columns", restrictions=restrictions)

    def indexes(self, schema: Optional[str] = None, table: Optional[str] = None, index: Optional[str] = None):
        restrictions = self._metadata_restrictions(schema=schema, table=table, index=index)
        return self.get_schema("indexes", restrictions=restrictions)

    def index_columns(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        index: Optional[str] = None,
        column: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(schema=schema, table=table, index=index, column=column)
        return self.get_schema("index_columns", restrictions=restrictions)

    def constraints(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        constraint: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(schema=schema, table=table, constraint=constraint)
        return self.get_schema("constraints", restrictions=restrictions)

    def catalogs(self, catalog: Optional[str] = None):
        restrictions = self._metadata_restrictions(catalog=catalog)
        return self.get_schema("catalogs", restrictions=restrictions)

    def primary_keys(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        constraint: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, table=table, constraint=constraint)
        return self.get_schema("primary_keys", restrictions=restrictions)

    def foreign_keys(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        constraint: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, table=table, constraint=constraint)
        return self.get_schema("foreign_keys", restrictions=restrictions)

    def procedures(
        self,
        schema: Optional[str] = None,
        procedure: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, procedure=procedure)
        return self.get_schema("procedures", restrictions=restrictions)

    def functions(
        self,
        schema: Optional[str] = None,
        function: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, function=function)
        return self.get_schema("functions", restrictions=restrictions)

    def routines(
        self,
        schema: Optional[str] = None,
        routine: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, routine=routine)
        return self.get_schema("routines", restrictions=restrictions)

    def table_privileges(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, table=table)
        return self.get_schema("table_privileges", restrictions=restrictions)

    def column_privileges(
        self,
        schema: Optional[str] = None,
        table: Optional[str] = None,
        column: Optional[str] = None,
        catalog: Optional[str] = None,
    ):
        restrictions = self._metadata_restrictions(catalog=catalog, schema=schema, table=table, column=column)
        return self.get_schema("column_privileges", restrictions=restrictions)

    def type_info(self, type_name: Optional[str] = None):
        restrictions = self._metadata_restrictions(type=type_name)
        return self.get_schema("type_info", restrictions=restrictions)

    def ddl_editor_schema_payload(
        self,
        schema_pattern: Optional[str] = None,
        expand_schema_parents: Optional[bool] = None,
    ) -> Dict[str, Any]:
        restrictions = self._metadata_restrictions(schema=schema_pattern)
        cur = self.query_metadata("schemas", restrictions=restrictions)
        rows = cur.fetchall()
        column_names = self._metadata_column_names(cur.description)
        expand = (
            bool(self._config.metadata_expand_schema_parents)
            if expand_schema_parents is None
            else bool(expand_schema_parents)
        )
        return build_ddl_editor_schema_payload(
            rows,
            schema_pattern=schema_pattern,
            expand_schema_parents=expand,
            column_names=column_names,
        )

    def get_session_schema(self) -> Optional[str]:
        return self._session_schema

    def set_session_schema(self, schema: Optional[str]) -> None:
        self._ensure_open()
        if schema is not None and not isinstance(schema, str):
            raise errors.ProgrammingError("schema must be a string or None")
        normalized = _normalize_session_schema(schema)
        if normalized == self._session_schema:
            return
        self._session_schema = normalized
        self._config.schema = normalized
        statement = _build_schema_statement(normalized or "users.public")
        if statement:
            self._execute_command(statement)

    def setinputsizes(self, sizes) -> None:
        self._ensure_open()

    def setoutputsize(self, size, column=None) -> None:
        self._ensure_open()

    def _ensure_open(self) -> None:
        if self._closed:
            raise errors.InterfaceError("connection is closed")

    def _apply_runtime_txn_id(self, txn_id: int) -> None:
        txn = int(txn_id)
        if txn > 0:
            self._txn_id = txn
            self._runtime_txn_active = True
        else:
            self._txn_id = 0

    def _apply_runtime_ready_state(self, status: int, txn_id: int) -> None:
        txn = int(txn_id)
        if int(status) != 0:
            # READY status is authoritative for native transaction activity.
            # Live listeners also publish `current_txn_id`, so the session
            # stays always-in-transaction even as COMMIT / ROLLBACK reopen the
            # next boundary for the caller.
            self._txn_id = txn
            self._runtime_txn_active = True
        else:
            self._clear_transaction_state()

    def _clear_transaction_state(self) -> None:
        self._txn_id = 0
        self._runtime_txn_active = False

    def _transaction_active(self) -> bool:
        return self._runtime_txn_active or self._txn_id != 0

    def _normalize_savepoint_name(self, name: str) -> str:
        if not isinstance(name, str):
            raise errors.ProgrammingError("savepoint name must be a string")
        normalized = name.strip()
        if not normalized:
            raise errors.ProgrammingError("savepoint name is required")
        return normalized

    def _build_prepared_transaction_sql(self, verb: str, gid: str) -> str:
        if not isinstance(gid, str) or not gid.strip():
            raise errors.ProgrammingError("[42601] global transaction id is required")
        escaped = gid.strip().replace("'", "''")
        return f"{verb} '{escaped}'"

    def _normalize_metadata_collection(self, collection_name: str) -> str:
        try:
            return normalize_collection_name(collection_name)
        except ValueError as exc:
            raise errors.NotSupportedError(str(exc)) from exc

    def _metadata_column_names(self, description) -> Sequence[str]:
        if not description:
            return []

        column_names = []
        for column in description:
            if isinstance(column, (tuple, list)) and column:
                column_names.append(str(column[0]))
            elif hasattr(column, "name"):
                column_names.append(str(column.name))
            elif column is not None:
                column_names.append(str(column))
        return column_names

    def _cursor_from_rows(self, rows, *, description=None, statusmessage=None, lastrowid=None) -> Cursor:
        cur = Cursor(self)
        cur._results = list(rows)
        cur._pos = 0
        cur._stream = None
        if description:
            cur.description = list(description)
        elif cur._results and isinstance(cur._results[0], dict):
            cur.description = [(key, None, None, None, None, None, True) for key in cur._results[0].keys()]
        else:
            cur.description = None
        cur.rowcount = len(cur._results)
        cur.statusmessage = statusmessage
        cur.lastrowid = lastrowid
        return cur

    @staticmethod
    def _metadata_restrictions(**kwargs) -> Optional[Dict[str, Any]]:
        restrictions = {key: value for key, value in kwargs.items() if value is not None}
        return restrictions or None

    @staticmethod
    def _metadata_collection_missing(exc: Exception) -> bool:
        message = str(exc).lower()
        return "table or view not found" in message or "object not found" in message

    def _handle_async(self, header: MessageHeader, payload: bytes) -> bool:
        if header.msg_type == MessageType.PARAMETER_STATUS:
            for name, value in parse_parameter_statuses(payload):
                self._parameters[name] = value
                if name == "attachment_id":
                    parsed = _parse_uuid_bytes(value)
                    if parsed is not None:
                        self._attachment_id = parsed
                if name == "current_txn_id":
                    parsed = _parse_uint64(value)
                    if parsed is not None:
                        self._apply_runtime_txn_id(parsed)
            return True
        if header.msg_type == MessageType.NOTIFICATION:
            notice = parse_notification(payload)
            for handler in self._notification_handlers:
                handler(notice)
            return True
        if header.msg_type == MessageType.NOTICE:
            self._last_notice = parse_notice(payload)
            return True
        if header.msg_type == MessageType.QUERY_PLAN:
            self._last_plan = parse_query_plan(payload)
            return True
        if header.msg_type == MessageType.QUERY_PROGRESS:
            self._last_query_progress = parse_query_progress(payload)
            return True
        if header.msg_type == MessageType.SBLR_COMPILED:
            self._last_sblr = parse_sblr_compiled(payload)
            return True
        if header.msg_type == MessageType.TXN_STATUS:
            status, txn_id = parse_txn_status(payload)
            if status == "T":
                self._txn_id = int(txn_id)
                self._runtime_txn_active = True
            else:
                self._clear_transaction_state()
            return True
        return False

    @property
    def closed(self) -> bool:
        return self._closed

    @property
    def autocommit(self) -> bool:
        return self._autocommit

    @autocommit.setter
    def autocommit(self, value: bool) -> None:
        self._ensure_open()
        next_value = bool(value)
        if self._autocommit == next_value:
            return
        if next_value and self._transaction_active():
            self.commit()
        self._autocommit = next_value

    def cancel(self) -> None:
        self._ensure_open()
        self._cancel_requested = True
        self._arm_cancel_timeout()
        payload = build_cancel_payload(0, 0)
        try:
            self._send_message(MessageType.CANCEL, payload, MSG_FLAG_URGENT)
        except Exception:
            self._clear_cancel_timeout()
            raise

    def _send_manager_frame(self, msg_type: int, payload: bytes) -> None:
        if not self._socket:
            raise errors.InterfaceError("no active socket")
        header = struct.pack(
            "<IHBBI",
            MANAGER_PROTOCOL_MAGIC,
            MANAGER_PROTOCOL_VERSION,
            msg_type,
            0,
            len(payload),
        )
        self._socket.sendall(header + payload)

    def _recv_manager_frame(self) -> tuple[int, bytes]:
        header = self._read_exact(MANAGER_HEADER_SIZE)
        magic, version, msg_type, _flags, payload_len = struct.unpack("<IHBBI", header)
        if magic != MANAGER_PROTOCOL_MAGIC:
            raise errors.OperationalError("manager frame magic mismatch")
        if version != MANAGER_PROTOCOL_VERSION:
            raise errors.OperationalError("manager frame version mismatch")
        if payload_len > MANAGER_MAX_PAYLOAD_SIZE:
            raise errors.OperationalError("manager payload too large")
        payload = self._read_exact(payload_len) if payload_len else b""
        return msg_type, payload

    @staticmethod
    def _pack_lpreface(text: str) -> bytes:
        encoded = text.encode("utf-8")
        return struct.pack("<I", len(encoded)) + encoded

    def _perform_manager_connect(self) -> None:
        if not self._config.manager_auth_token:
            raise errors.InterfaceError("manager_proxy mode requires manager_auth_token")

        manager_user = self._config.manager_username or self._config.user or "admin"
        manager_database = self._config.manager_database or self._config.database or ""
        manager_profile = self._config.manager_connection_profile or "SBsql"
        manager_intent = self._config.manager_client_intent or "SBsql"
        manager_flags = int(self._config.manager_client_flags or 0) & 0xFFFF
        auth_fast_path = self._config.manager_auth_fast_path is not False

        hello_payload = struct.pack("<HH", MCP_PROTOCOL_VERSION, manager_flags)
        self._send_manager_frame(MCP_MSG_HELLO, hello_payload)
        msg_type, _ = self._recv_manager_frame()
        if msg_type != MCP_MSG_STATUS_RESPONSE:
            raise errors.OperationalError("expected MCP hello status response")

        auth_start = bytearray()
        auth_start += self._pack_lpreface(manager_user)
        auth_start += struct.pack("<B", MCP_AUTH_METHOD_TOKEN)
        if auth_fast_path:
            token = self._config.manager_auth_token.encode("utf-8")
            auth_start += struct.pack("<I", len(token)) + token
        else:
            auth_start += struct.pack("<I", 0)

        self._send_manager_frame(MCP_MSG_AUTH_START, bytes(auth_start))
        msg_type, payload = self._recv_manager_frame()
        if msg_type == MCP_MSG_AUTH_CHALLENGE:
            token = self._config.manager_auth_token.encode("utf-8")
            self._send_manager_frame(MCP_MSG_AUTH_CONTINUE, struct.pack("<I", len(token)) + token)
            msg_type, payload = self._recv_manager_frame()

        if msg_type != MCP_MSG_AUTH_RESPONSE:
            raise errors.OperationalError("expected MCP auth response")
        if len(payload) < 1 + 4 + 256:
            raise errors.OperationalError("truncated MCP auth response")
        if payload[0] != 0:
            err_text = payload[5 : 5 + 256].split(b"\x00", 1)[0].decode("utf-8", errors="replace")
            raise errors.OperationalError(err_text or "MCP authentication failed")

        nonce = os.urandom(16)
        db_connect = bytearray(b"MCP1")
        db_connect += self._pack_lpreface(manager_database)
        db_connect += self._pack_lpreface(manager_profile)
        db_connect += self._pack_lpreface(manager_intent)
        db_connect += struct.pack("<H", len(nonce))
        db_connect += nonce

        self._send_manager_frame(MCP_MSG_DB_CONNECT, bytes(db_connect))
        msg_type, payload = self._recv_manager_frame()
        if msg_type != MCP_MSG_CONNECT_RESPONSE:
            raise errors.OperationalError("expected MCP connect response")
        if len(payload) < 1 + 2 + 2 + 16 + 64 + 32:
            raise errors.OperationalError("truncated MCP connect response")
        if payload[0] != 0:
            err_text = "MCP database connect failed"
            err_offset = 1 + 2 + 2 + 16 + 64 + 32
            if len(payload) >= err_offset + 4:
                err_len = struct.unpack_from("<I", payload, err_offset)[0]
                start = err_offset + 4
                end = start + err_len
                if len(payload) >= end:
                    err_text = payload[start:end].decode("utf-8", errors="replace")
            raise errors.OperationalError(err_text)
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode="manager_proxy",
            resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
            resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
            manager_authenticated=True,
            attached=self._resolved_auth_context.attached,
        )

    def _startup_and_auth(self) -> None:
        if not hasattr(self, "_resolved_auth_context"):
            self._reset_resolved_auth_context()
        self._authed = False
        self._parameters.clear()
        try:
            params = self._build_startup_params()
        except ValueError as exc:
            raise errors.InterfaceError(str(exc)) from exc
        payload = build_p1_startup_payload(
            self._startup_features(),
            self._startup_required_features(),
            params)
        self._send_message(MessageType.STARTUP, payload, force_zero=True)

        scram: Optional[ScramExchange] = None

        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.NEGOTIATE_VERSION:
                continue
            if header.msg_type == MessageType.AUTH_REQUEST:
                method, _ = parse_auth_request(payload)
                if method == AuthMethod.OK:
                    continue
                self._resolved_auth_context = ResolvedAuthContext(
                    ingress_mode=self._resolved_auth_context.ingress_mode,
                    resolved_auth_method=_auth_method_name(method),
                    resolved_auth_plugin_id=_auth_plugin_id_for_method(method, self._config.auth_method_id),
                    manager_authenticated=self._resolved_auth_context.manager_authenticated,
                    attached=self._resolved_auth_context.attached,
                )
                if method == AuthMethod.PASSWORD:
                    password_bytes = (self._config.password or "").encode("utf-8")
                    self._send_message(MessageType.AUTH_RESPONSE, password_bytes, force_zero=True)
                    continue
                if method in (AuthMethod.SCRAM_SHA_256, AuthMethod.SCRAM_SHA_512):
                    if scram is None:
                        scram = ScramExchange(
                            self._config.user or "",
                            digest="sha512" if method == AuthMethod.SCRAM_SHA_512 else "sha256",
                        )
                    client_first = scram.client_first_message().encode("utf-8")
                    self._send_message(MessageType.AUTH_RESPONSE, client_first, force_zero=True)
                    continue
                if method == AuthMethod.TOKEN:
                    token_payload = _resolve_token_auth_payload(self._config)
                    if token_payload is None:
                        raise errors.OperationalError(
                            "[28000] TOKEN authentication requires auth_token, auth_method_payload, auth_payload_json, "
                            "auth_payload_b64, workload_identity_token, or proxy_principal_assertion"
                        )
                    self._send_message(MessageType.AUTH_RESPONSE, token_payload, force_zero=True)
                    continue
                if method == AuthMethod.MD5:
                    raise errors.NotSupportedError(
                        "[0A000] MD5 authentication is admitted by the server but not executable in the Python lane"
                    )
                if method == AuthMethod.PEER:
                    raise errors.NotSupportedError(
                        "[0A000] PEER authentication requires broker or platform assistance in the Python lane"
                    )
                if method == AuthMethod.REATTACH:
                    raise errors.NotSupportedError(
                        "[0A000] REATTACH authentication negotiation is not executable through the generic Python auth lane"
                    )
                raise errors.NotSupportedError("[0A000] unsupported auth method")
            if header.msg_type == MessageType.AUTH_CONTINUE:
                method, _, data = parse_auth_continue(payload)
                if method not in (AuthMethod.SCRAM_SHA_256, AuthMethod.SCRAM_SHA_512):
                    raise errors.NotSupportedError("[0A000] unsupported auth continuation")
                if scram is None:
                    raise errors.OperationalError("SCRAM state missing")
                server_first = data.decode("utf-8", errors="replace")
                client_final = scram.handle_server_first(self._config.password or "", server_first)
                self._send_message(
                    MessageType.AUTH_RESPONSE,
                    client_final.encode("utf-8"),
                    force_zero=True,
                )
                continue
            if header.msg_type == MessageType.AUTH_OK:
                _, info = parse_auth_ok(payload)
                self._attachment_id = header.attachment_id
                self._apply_runtime_txn_id(header.txn_id)
                self._authed = True
                if scram and info.startswith(b"v="):
                    scram.verify_server_final(info.decode("utf-8", errors="replace"))
                continue
            if header.msg_type == MessageType.PARAMETER_STATUS:
                self._handle_async(header, payload)
                continue
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                if (
                    self._resolved_auth_context.resolved_auth_method is None
                    and self._config.dormant_id
                ):
                    self._resolved_auth_context = ResolvedAuthContext(
                        ingress_mode=self._resolved_auth_context.ingress_mode,
                        resolved_auth_method="REATTACH",
                        resolved_auth_plugin_id=_auth_plugin_id_for_method(
                            AuthMethod.REATTACH,
                            self._config.auth_method_id,
                        ),
                        manager_authenticated=self._resolved_auth_context.manager_authenticated,
                        attached=self._resolved_auth_context.attached,
                    )
                return
            if header.msg_type == MessageType.ERROR:
                self._raise_protocol_error(payload)
            continue

    def _build_startup_params(self) -> Dict[str, str]:
        params = {
            "database": self._config.database or "",
            "user": self._config.user or "",
            "client_flags": str(int(self._config.connect_client_flags or 0x0100) & 0xFFFF),
        }
        if bool(self._config.dormant_id) != bool(self._config.dormant_reattach_token):
            raise ValueError(
                "dormant_id and dormant_reattach_token must be provided together"
            )
        if self._config.role:
            params["role"] = self._config.role
        if self._config.application_name:
            params["application_name"] = self._config.application_name
        if self._config.dormant_id:
            params["dormant_id"] = self._config.dormant_id
            params["dormant_reattach_token"] = self._config.dormant_reattach_token or ""
        selection = AuthPluginSelection(
            method_id=self._config.auth_method_id or "",
            method_payload=self._config.auth_method_payload or "",
            payload_json=self._config.auth_payload_json or "",
            payload_b64=self._config.auth_payload_b64 or "",
            provider_profile=self._config.auth_provider_profile or "",
            required_methods=self._config.auth_required_methods or "",
            forbidden_methods=self._config.auth_forbidden_methods or "",
            require_channel_binding=bool(self._config.auth_require_channel_binding),
            workload_identity_token=self._config.workload_identity_token or "",
            proxy_principal_assertion=self._config.proxy_principal_assertion or "",
        )
        apply_auth_plugin_selection(params, selection)
        return params

    def _send_message(self, msg_type: int, payload: bytes, flags: int = 0, force_zero: bool = False) -> None:
        if not self._socket:
            raise errors.InterfaceError("no active socket")
        attachment = self._attachment_id if self._authed and not force_zero else b"\x00" * 16
        txn_id = self._txn_id if self._authed and not force_zero else 0
        header = MessageHeader(
            msg_type=msg_type,
            flags=flags,
            length=len(payload),
            sequence=self._sequence,
            attachment_id=attachment,
            txn_id=txn_id,
        )
        self._sequence = (self._sequence + 1) & 0xFFFFFFFF
        data = encode_message(header, payload)
        self._socket.sendall(data)

    def _recv_message(self):
        if self._prefetched_message is not None:
            message = self._prefetched_message
            self._prefetched_message = None
            return message
        header_bytes = self._read_exact(HEADER_SIZE)
        header = decode_header(header_bytes)
        payload = self._read_exact(header.length) if header.length else b""
        if getattr(self, "_cancel_requested", False):
            self._clear_cancel_timeout()
        return header, payload

    def _stash_prefetched_message(self, header: MessageHeader, payload: bytes) -> None:
        self._prefetched_message = (header, payload)

    def _read_exact(self, n: int) -> bytes:
        if not self._socket:
            raise errors.InterfaceError("no active socket")
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self._socket.recv(n - len(buf))
            except TimeoutError as exc:
                if getattr(self, "_cancel_requested", False):
                    self._clear_cancel_timeout()
                    raise errors.OperationalError("[57014] query canceled") from exc
                raise errors.OperationalError("[08006] socket timeout while reading from server") from exc
            except OSError as exc:
                if getattr(self, "_cancel_requested", False):
                    self._clear_cancel_timeout()
                    raise errors.OperationalError("[57014] query canceled") from exc
                raise errors.OperationalError(f"[08006] socket read failed: {exc}") from exc
            if not chunk:
                raise errors.OperationalError("connection closed")
            buf.extend(chunk)
        return bytes(buf)

    def _arm_cancel_timeout(self) -> None:
        sock = getattr(self, "_socket", None)
        if not sock:
            return
        timeout_window = float(getattr(self, "_cancel_timeout_seconds", 0.2))
        try:
            current_timeout = sock.gettimeout()
        except OSError:
            return
        if getattr(self, "_cancel_socket_timeout", None) is None:
            self._cancel_socket_timeout = current_timeout
        if current_timeout is None or current_timeout > timeout_window:
            try:
                sock.settimeout(timeout_window)
            except OSError:
                pass

    def _clear_cancel_timeout(self) -> None:
        self._cancel_requested = False
        prior_timeout = getattr(self, "_cancel_socket_timeout", None)
        sock = getattr(self, "_socket", None)
        if sock and prior_timeout is not None:
            try:
                sock.settimeout(prior_timeout)
            except OSError:
                pass
        self._cancel_socket_timeout = None

    def _execute_command(self, sql: str) -> None:
        span = self._begin_operation("execute_command", sql)
        self._send_simple_query(sql)
        try:
            self._drain_until_ready()
            self._end_operation(span, True)
        except Exception:
            self._end_operation(span, False)
            raise

    def _apply_schema(self) -> None:
        schema = self._session_schema or ""
        if not schema or schema.lower() == "public":
            return
        statement = _build_schema_statement(schema)
        if statement:
            self._execute_command(statement)

    def _send_simple_query(self, sql: str, max_rows: int = 0) -> None:
        self._portal_resume_pending = False
        flags = QUERY_FLAG_BINARY_RESULT if self._config.binary_transfer else 0
        payload = build_query_payload(sql, flags, max_rows, 0)
        self._send_message(MessageType.QUERY, payload)

    def _send_extended_query(self, sql: str, params, max_rows: int = 0) -> None:
        self._portal_resume_pending = False
        param_values = []
        param_types = []
        for param in params:
            value, oid = encode_param(param)
            param_values.append(value)
            param_types.append(oid)
        parse_payload = build_parse_payload("", sql, param_types)
        self._send_message(MessageType.PARSE, parse_payload)
        param_count = self._describe_statement("")
        if param_count >= 0 and param_count != len(param_types):
            raise errors.ProgrammingError("parameter count mismatch (07001)")
        result_formats = [FORMAT_BINARY] if self._config.binary_transfer else []
        bind_payload = build_bind_payload("", "", param_values, result_formats)
        self._send_message(MessageType.BIND, bind_payload)
        exec_payload = build_execute_payload("", max_rows)
        self._send_message(MessageType.EXECUTE, exec_payload)
        if max_rows == 0:
            self._send_message(MessageType.SYNC, b"")

    def _next_prepared_exec_name(self) -> str:
        self._prepared_exec_counter += 1
        return f"__py_exec_{self._prepared_exec_counter}"

    def _send_cached_extended_query(self, sql: str, params, max_rows: int = 0) -> None:
        self._portal_resume_pending = False
        param_values = []
        param_types = []
        for param in params:
            value, oid = encode_param(param)
            param_values.append(value)
            param_types.append(oid)

        cache_key = (sql, tuple(param_types))
        cached = self._prepared_exec_cache.get(cache_key)
        if cached is None:
            statement_name = self._next_prepared_exec_name()
            parse_payload = build_parse_payload(statement_name, sql, param_types)
            self._send_message(MessageType.PARSE, parse_payload)
            param_count = self._describe_statement(statement_name)
            if param_count >= 0 and param_count != len(param_types):
                raise errors.ProgrammingError("parameter count mismatch (07001)")
            self._prepared_exec_cache[cache_key] = (statement_name, len(param_types))
        else:
            statement_name, expected_count = cached
            if expected_count != len(param_types):
                del self._prepared_exec_cache[cache_key]
                raise errors.ProgrammingError("parameter count mismatch (07001)")

        result_formats = [FORMAT_BINARY] if self._config.binary_transfer else []
        bind_payload = build_bind_payload("", statement_name, param_values, result_formats)
        self._send_message(MessageType.BIND, bind_payload)
        exec_payload = build_execute_payload("", max_rows)
        self._send_message(MessageType.EXECUTE, exec_payload)
        if max_rows == 0:
            self._send_message(MessageType.SYNC, b"")

    def _execute_query(self, sql: str, params=None, max_rows: int = 0):
        try:
            normalized_sql, ordered = normalize_query(sql, params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc
        span = self._begin_operation("execute_query", normalized_sql)
        try:
            if ordered:
                self._send_extended_query(normalized_sql, ordered, max_rows)
            else:
                self._send_simple_query(normalized_sql, max_rows)
            self._end_operation(span, True)
        except Exception:
            self._end_operation(span, False)
            raise
        return ResultStream(self, max_rows)

    def _execute_multi_statement_query(self, sql: str, params=None):
        if params is None:
            return None
        try:
            normalized_sql, ordered = normalize_query(sql, params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc

        if not ordered:
            return None

        statements = split_top_level_statements(normalized_sql)
        if len(statements) <= 1:
            return None

        results = []
        for statement in statements:
            remapped_sql, remapped_params = self._remap_statement_params(statement, ordered)
            stream = self._execute_query(remapped_sql, remapped_params, 0)
            rows = []
            while True:
                row = stream.read_row()
                if row is None:
                    break
                rows.append(row)
            results.append(
                {
                    "rows": rows,
                    "rowCount": stream.rowcount if stream.rowcount is not None else -1,
                    "fields": self._result_fields_from_stream(stream),
                    "command": getattr(stream, "command", None),
                    "lastId": getattr(stream, "lastrowid", None),
                }
            )
        return results

    def _remap_statement_params(self, sql: str, params: Sequence[Any]) -> Tuple[str, List[Any]]:
        if not params:
            return sql, []

        result: List[str] = []
        in_single = False
        in_double = False
        remap: Dict[int, int] = {}
        ordered_indexes: List[int] = []
        i = 0
        while i < len(sql):
            ch = sql[i]
            if ch == "'" and not in_double:
                in_single = not in_single
                result.append(ch)
                i += 1
                continue
            if ch == '"' and not in_single:
                in_double = not in_double
                result.append(ch)
                i += 1
                continue
            if not in_single and not in_double and ch == "$" and i + 1 < len(sql) and sql[i + 1].isdigit():
                j = i + 1
                while j < len(sql) and sql[j].isdigit():
                    j += 1
                original_index = int(sql[i + 1 : j])
                if original_index not in remap:
                    remap[original_index] = len(ordered_indexes) + 1
                    ordered_indexes.append(original_index)
                result.append(f"${remap[original_index]}")
                i = j
                continue
            result.append(ch)
            i += 1

        remapped_params: List[Any] = []
        for original_index in ordered_indexes:
            if original_index < 1 or original_index > len(params):
                raise errors.ProgrammingError("parameter count mismatch (07001)")
            remapped_params.append(params[original_index - 1])
        return "".join(result), remapped_params

    def _result_fields_from_stream(self, stream) -> List[Tuple[Any, ...]]:
        columns = getattr(stream, "columns", None) or []
        return [
            (
                col.name,
                col.type_oid,
                None,
                col.type_modifier or None,
                None,
                None,
                col.nullable,
            )
            for col in columns
        ]

    def _execute_cached_query_shape(self, sql: str, params=None, max_rows: int = 0):
        try:
            normalized_sql, ordered = normalize_query(sql, params)
        except ValueError as exc:
            raise errors.ProgrammingError(str(exc)) from exc
        span = self._begin_operation("execute_query", normalized_sql)
        try:
            if ordered:
                self._send_cached_extended_query(normalized_sql, ordered, max_rows)
            else:
                self._send_simple_query(normalized_sql, max_rows)
            self._end_operation(span, True)
        except Exception:
            self._end_operation(span, False)
            raise
        return ResultStream(self, max_rows)

    def _describe_statement(self, statement_name: str) -> int:
        describe_payload = build_describe_payload(ord("S"), statement_name)
        self._send_message(MessageType.DESCRIBE, describe_payload)
        self._send_message(MessageType.SYNC, b"")
        param_count = -1
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.PARAMETER_DESCRIPTION:
                param_count = len(parse_parameter_description(payload))
            elif header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                return param_count

    def _drain_until_ready(self) -> None:
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                self._portal_resume_pending = False
                return

    def _drain_immediate_reopen_boundary(self) -> None:
        sock = getattr(self, "_socket", None)
        if sock is None:
            return
        while True:
            try:
                readable, _, _ = select.select([sock], [], [], 0)
            except (OSError, ValueError):
                return
            if not readable:
                return
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                self._portal_resume_pending = False
                continue
            self._stash_prefetched_message(header, payload)
            return

    def _allow_portal_resume(self) -> None:
        self._portal_resume_pending = True

    def _resume_suspended_portal(self, page_size: int) -> None:
        if not self._portal_resume_pending:
            raise errors.OperationalError("[55000] portal resume requires explicit suspended state")
        self._portal_resume_pending = False
        exec_payload = build_execute_payload("", page_size)
        self._send_message(MessageType.EXECUTE, exec_payload)

    def _build_protocol_error(self, payload: bytes) -> Exception:
        try:
            _, sqlstate, message, detail, hint = parse_error_message(payload)
        except ValueError:
            return errors.DatabaseError("query failed")
        parts = []
        if message:
            parts.append(message)
        if detail:
            parts.append(f"DETAIL: {detail}")
        if hint:
            parts.append(f"HINT: {hint}")
        text = "\n".join(parts) if parts else "query failed"
        if sqlstate:
            text = f"[{sqlstate}] {text}"
            return _map_sqlstate(sqlstate)(text)
        return errors.DatabaseError(text)

    def _drain_error_ready_boundary(self) -> None:
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                self._portal_resume_pending = False
                return

    def _raise_protocol_error(self, payload: bytes) -> None:
        raise self._build_protocol_error(payload)

    def _raise_protocol_error_and_sync(self, payload: bytes) -> None:
        exc = self._build_protocol_error(payload)
        self._drain_error_ready_boundary()
        raise exc

    def copy_in(
        self,
        sql: str,
        data: bytes,
        format: int = COPY_FORMAT_TEXT,
        column_types: Optional[Sequence[int | str]] = None,
    ) -> int:
        """Execute a COPY FROM operation, sending data to the server.
        
        Args:
            sql: The COPY SQL statement (e.g., "COPY table FROM STDIN")
            data: The data to copy in bytes
            format: COPY_FORMAT_TEXT or COPY_FORMAT_BINARY
            column_types: Optional fixed-shape native rowset type vector for binary COPY
            
        Returns:
            Number of rows copied
        """
        self._ensure_open()
        span = self._begin_operation("copy_in", sql)
        
        # Send COPY SQL as a query
        try:
            self._send_simple_query(sql)
        except Exception:
            self._end_operation(span, False)
            raise
        
        # Wait for CopyInResponse
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._end_operation(span, False)
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.COPY_IN_RESPONSE:
                response = parse_copy_in_response(payload)
                break
            if header.msg_type == MessageType.READY:
                self._end_operation(span, False)
                raise errors.OperationalError("expected COPY IN response")

        if format == COPY_FORMAT_BINARY or response.format == COPY_FORMAT_BINARY:
            data = _copy_text_rows_to_native_frame(data, column_types)
        
        # Send data in chunks
        offset = 0
        chunk_size = 65536  # 64KB chunks
        while offset < len(data):
            chunk = data[offset:offset + chunk_size]
            payload = build_copy_data_payload(chunk)
            self._send_message(MessageType.COPY_DATA, payload)
            offset += len(chunk)
        
        # Send CopyDone
        self._send_message(MessageType.COPY_DONE, build_copy_done_payload())
        
        # Wait for CommandComplete
        rows_copied = 0
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._end_operation(span, False)
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.COMMAND_COMPLETE:
                _, rows_copied, _, _ = parse_command_complete(payload)
                break
            if header.msg_type == MessageType.READY:
                self._end_operation(span, False)
                raise errors.OperationalError("expected CommandComplete after COPY")
        
        # Wait for Ready
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                self._end_operation(span, True)
                return int(rows_copied)

    def copy_out(self, sql: str, format: int = COPY_FORMAT_TEXT) -> bytes:
        """Execute a COPY TO operation, receiving data from the server.
        
        Args:
            sql: The COPY SQL statement (e.g., "COPY table TO STDOUT")
            format: COPY_FORMAT_TEXT or COPY_FORMAT_BINARY
            
        Returns:
            The copied data as bytes
        """
        self._ensure_open()
        span = self._begin_operation("copy_out", sql)
        
        # Send COPY SQL as a query
        try:
            self._send_simple_query(sql)
        except Exception:
            self._end_operation(span, False)
            raise
        
        # Wait for CopyOutResponse
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._end_operation(span, False)
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.COPY_OUT_RESPONSE:
                response = parse_copy_out_response(payload)
                _ = response
                break
            if header.msg_type == MessageType.READY:
                self._end_operation(span, False)
                raise errors.OperationalError("expected COPY OUT response")
        
        # Collect data until CopyDone
        chunks = []
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._end_operation(span, False)
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.COPY_DATA:
                chunks.append(payload)
            elif header.msg_type == MessageType.COPY_DONE:
                break
            elif header.msg_type == MessageType.COPY_FAIL:
                self._end_operation(span, False)
                raise errors.OperationalError("COPY failed on server side")
            elif header.msg_type == MessageType.READY:
                self._end_operation(span, False)
                raise errors.OperationalError("unexpected READY during COPY")
        
        # Wait for CommandComplete
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._end_operation(span, False)
                self._raise_protocol_error_and_sync(payload)
            if header.msg_type == MessageType.COMMAND_COMPLETE:
                _ = parse_command_complete(payload)
                break
            if header.msg_type == MessageType.READY:
                raise errors.OperationalError("expected CommandComplete after COPY")
        
        # Wait for Ready
        while True:
            header, payload = self._recv_message()
            if self._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._apply_runtime_ready_state(status, txn_id)
                self._end_operation(span, True)
                return b"".join(chunks)


def _build_schema_statement(schema: str) -> str:
    trimmed = schema.strip()
    if not trimmed:
        return ""
    if "," in trimmed:
        parts = [part.strip() for part in trimmed.split(",") if part.strip()]
        if not parts:
            return ""
        quoted = ", ".join(_quote_identifier(part) for part in parts)
        return f"SET SEARCH_PATH TO {quoted}"
    return f"SET SCHEMA {_quote_identifier(trimmed)}"


def _normalize_session_schema(schema: Optional[Any]) -> Optional[str]:
    if schema is None:
        return None
    trimmed = str(schema).strip()
    if not trimmed:
        return None
    if trimmed.lower() == "public":
        return "users.public"
    return trimmed


def _quote_identifier(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def _map_sqlstate(sqlstate: str):
    if len(sqlstate) == 5:
        full_map = {
            "01000": errors.Warning,
            "02000": errors.DatabaseError,
            "08001": errors.OperationalError,
            "08003": errors.OperationalError,
            "08004": errors.OperationalError,
            "08006": errors.OperationalError,
            "08P01": errors.OperationalError,
            "0A000": errors.NotSupportedError,
            "22001": errors.DataError,
            "22003": errors.DataError,
            "22007": errors.DataError,
            "22012": errors.DataError,
            "22023": errors.DataError,
            "22P02": errors.DataError,
            "22P03": errors.DataError,
            "23000": errors.IntegrityError,
            "23502": errors.IntegrityError,
            "23503": errors.IntegrityError,
            "23505": errors.IntegrityError,
            "23514": errors.IntegrityError,
            "28000": errors.OperationalError,
            "28P01": errors.OperationalError,
            "40001": errors.DatabaseError,
            "40P01": errors.DatabaseError,
            "42501": errors.ProgrammingError,
            "42601": errors.ProgrammingError,
            "42703": errors.ProgrammingError,
            "42704": errors.ProgrammingError,
            "42710": errors.ProgrammingError,
            "42883": errors.ProgrammingError,
            "42P01": errors.ProgrammingError,
            "42P07": errors.ProgrammingError,
            "53P00": errors.OperationalError,
            "53100": errors.OperationalError,
            "53200": errors.OperationalError,
            "53300": errors.OperationalError,
            "54000": errors.OperationalError,
            "57014": errors.OperationalError,
            "57P01": errors.OperationalError,
            "57P03": errors.OperationalError,
            "58000": errors.InternalError,
            "XX000": errors.InternalError,
        }
        mapped = full_map.get(sqlstate)
        if mapped:
            return mapped
        class_map = {
            "01": errors.Warning,
            "02": errors.DatabaseError,
            "08": errors.OperationalError,
            "0A": errors.NotSupportedError,
            "22": errors.DataError,
            "23": errors.IntegrityError,
            "28": errors.OperationalError,
            "40": errors.DatabaseError,
            "42": errors.ProgrammingError,
            "53": errors.OperationalError,
            "54": errors.OperationalError,
            "57": errors.OperationalError,
            "58": errors.InternalError,
            "XX": errors.InternalError,
        }
        mapped = class_map.get(sqlstate[:2])
        if mapped:
            return mapped
    return errors.DatabaseError


QUERY_FLAG_BINARY_RESULT = 0x04


class ResultStream:
    def __init__(self, connection: Connection, page_size: int = 0):
        self._connection = connection
        self._page_size = page_size
        self.columns = []
        self.rowcount = -1
        self.lastrowid = None
        self.command = None
        self.completion_count = 0
        self._done = False
        self._has_next_result_set = False
        self._result_set_boundary = False
        self._prefetched_message = None
        self._prefetched_rows = []
        self._response_started = False
        self._ignored_stray_ready = False

    def prime_metadata(self) -> None:
        if self._done or self._result_set_boundary or self.columns:
            return
        while True:
            header, payload = self._recv_message()
            if self._connection._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._fail_with_protocol_error(payload)
            if header.msg_type == MessageType.ROW_DESCRIPTION:
                self._response_started = True
                self.columns = parse_row_description(payload)
                return
            if header.msg_type == MessageType.DATA_ROW:
                self._response_started = True
                self._prefetched_rows.append(self._decode_data_row(payload))
                return
            if header.msg_type == MessageType.COMMAND_COMPLETE:
                self._response_started = True
                _, rows_affected, last_id, tag = parse_command_complete(payload)
                self.rowcount = int(rows_affected)
                self.lastrowid = int(last_id)
                self.command = tag
                self.completion_count += 1
                self._mark_result_set_boundary()
                return
            if header.msg_type == MessageType.PORTAL_SUSPENDED:
                self._connection._allow_portal_resume()
                self._connection._resume_suspended_portal(self._page_size)
                continue
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._connection._apply_runtime_ready_state(status, txn_id)
                self._connection._portal_resume_pending = False
                if not self._response_started and not self._ignored_stray_ready:
                    # Native rollback/commit can publish a fresh-session reopen
                    # boundary before the next statement response begins.
                    # Ignore one READY frame that arrives before any query
                    # result material so the actual statement response is not
                    # misclassified as empty.
                    self._ignored_stray_ready = True
                    continue
                self._done = True
                return

    def read_row(self):
        if self._done:
            return None
        if self._result_set_boundary:
            return None
        if self._prefetched_rows:
            return self._prefetched_rows.pop(0)
        while True:
            header, payload = self._recv_message()
            if self._connection._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.ERROR:
                self._fail_with_protocol_error(payload)
            if header.msg_type == MessageType.ROW_DESCRIPTION:
                self._response_started = True
                self.columns = parse_row_description(payload)
            elif header.msg_type == MessageType.DATA_ROW:
                self._response_started = True
                return self._decode_data_row(payload)
            elif header.msg_type == MessageType.COMMAND_COMPLETE:
                self._response_started = True
                _, rows_affected, last_id, tag = parse_command_complete(payload)
                self.rowcount = int(rows_affected)
                self.lastrowid = int(last_id)
                self.command = tag
                self.completion_count += 1
                self._mark_result_set_boundary()
                return None
            elif header.msg_type == MessageType.PORTAL_SUSPENDED:
                self._connection._allow_portal_resume()
                self._connection._resume_suspended_portal(self._page_size)
            elif header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._connection._apply_runtime_ready_state(status, txn_id)
                self._connection._portal_resume_pending = False
                if not self._response_started and not self._ignored_stray_ready:
                    self._ignored_stray_ready = True
                    continue
                self._done = True
                return None

    def has_next_result_set(self) -> bool:
        return self._has_next_result_set

    def next_result_set(self) -> bool:
        if self._done or not self._has_next_result_set:
            return False
        self._has_next_result_set = False
        self._result_set_boundary = False
        self._response_started = False
        self._ignored_stray_ready = False
        self.columns = []
        self.rowcount = -1
        self.lastrowid = None
        self.command = None
        return True

    def _recv_message(self):
        if self._prefetched_message is not None:
            msg = self._prefetched_message
            self._prefetched_message = None
            return msg
        return self._connection._recv_message()

    def _fail_with_protocol_error(self, payload: bytes):
        self._done = True
        self._has_next_result_set = False
        self._result_set_boundary = False
        self._prefetched_message = None
        self._prefetched_rows = []
        self._connection._raise_protocol_error_and_sync(payload)

    def _mark_result_set_boundary(self) -> None:
        while True:
            header, payload = self._connection._recv_message()
            if self._connection._handle_async(header, payload):
                continue
            if header.msg_type == MessageType.READY:
                status, txn_id, _ = parse_ready(payload)
                self._connection._apply_runtime_ready_state(status, txn_id)
                self._connection._portal_resume_pending = False
                self._done = True
                self._has_next_result_set = False
                self._result_set_boundary = False
                return
            self._prefetched_message = (header, payload)
            self._has_next_result_set = True
            self._result_set_boundary = True
            return

    def _decode_data_row(self, payload: bytes):
        column_count = len(self.columns)
        if column_count == 0 and len(payload) >= 2:
            column_count = struct.unpack_from("<H", payload, 0)[0]
        values = parse_data_row(payload, column_count)
        if not self.columns:
            self.columns = [
                ColumnInfo(
                    name=f"column{idx + 1}",
                    table_oid=0,
                    column_index=idx + 1,
                    type_oid=0,
                    type_size=0,
                    type_modifier=0,
                    format=FORMAT_BINARY,
                    nullable=True,
                )
                for idx in range(len(values))
            ]
        decoded = []
        for idx, value in enumerate(values):
            if idx < len(self.columns):
                col = self.columns[idx]
                decoded.append(decode_value(col.type_oid, value.data, col.format))
            else:
                decoded.append(decode_value(0, value.data, FORMAT_BINARY))
        return tuple(decoded)


def _parse_uuid_bytes(value: str) -> Optional[bytes]:
    hex_value = value.replace("-", "").strip()
    if len(hex_value) != 32:
        return None
    try:
        return bytes.fromhex(hex_value)
    except ValueError:
        return None


def _normalize_uuid_text(value: str, field_name: str) -> str:
    parsed = _parse_uuid_bytes(value)
    if parsed is None:
        raise errors.ProgrammingError(f"[42601] {field_name} must be UUID text")
    hex_value = parsed.hex()
    return (
        f"{hex_value[0:8]}-"
        f"{hex_value[8:12]}-"
        f"{hex_value[12:16]}-"
        f"{hex_value[16:20]}-"
        f"{hex_value[20:32]}"
    )


def _parse_uint64(value: str) -> Optional[int]:
    try:
        return int(value.strip())
    except (ValueError, TypeError):
        return None
