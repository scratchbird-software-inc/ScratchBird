# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""ScratchBird native wire protocol (SBWP v1.1)."""

from __future__ import annotations

import struct
from dataclasses import dataclass
from typing import Dict, List, Optional, Tuple

PROTOCOL_MAGIC_BYTES = b"SBWP"
PROTOCOL_VERSION_MAJOR = 1
PROTOCOL_VERSION_MINOR = 1
PROTOCOL_VERSION = (PROTOCOL_VERSION_MAJOR << 8) | PROTOCOL_VERSION_MINOR

HEADER_SIZE = 40
MAX_MESSAGE_SIZE = 1024 * 1024 * 1024


class MessageType:
    STARTUP = 0x01
    AUTH_RESPONSE = 0x02
    QUERY = 0x03
    PARSE = 0x04
    BIND = 0x05
    DESCRIBE = 0x06
    EXECUTE = 0x07
    CLOSE = 0x08
    SYNC = 0x09
    FLUSH = 0x0A
    CANCEL = 0x0B
    TERMINATE = 0x0C
    COPY_DATA = 0x0D
    COPY_DONE = 0x0E
    COPY_FAIL = 0x0F
    SBLR_EXECUTE = 0x10
    SUBSCRIBE = 0x11
    UNSUBSCRIBE = 0x12
    FEDERATED_QUERY = 0x13
    STREAM_CONTROL = 0x14
    TXN_BEGIN = 0x15
    TXN_COMMIT = 0x16
    TXN_ROLLBACK = 0x17
    TXN_SAVEPOINT = 0x18
    TXN_RELEASE = 0x19
    TXN_ROLLBACK_TO = 0x1A
    PING = 0x1B
    SET_OPTION = 0x1C
    CLUSTER_AUTH = 0x1D
    ATTACH_CREATE = 0x1E
    ATTACH_DETACH = 0x1F
    ATTACH_LIST = 0x20

    AUTH_REQUEST = 0x40
    AUTH_OK = 0x41
    AUTH_CONTINUE = 0x42
    READY = 0x43
    ROW_DESCRIPTION = 0x44
    DATA_ROW = 0x45
    COMMAND_COMPLETE = 0x46
    EMPTY_QUERY = 0x47
    ERROR = 0x48
    NOTICE = 0x49
    PARSE_COMPLETE = 0x4A
    BIND_COMPLETE = 0x4B
    CLOSE_COMPLETE = 0x4C
    PORTAL_SUSPENDED = 0x4D
    NO_DATA = 0x4E
    PARAMETER_STATUS = 0x4F
    PARAMETER_DESCRIPTION = 0x50
    COPY_IN_RESPONSE = 0x51
    COPY_OUT_RESPONSE = 0x52
    COPY_BOTH_RESPONSE = 0x53
    NOTIFICATION = 0x54
    FUNCTION_RESULT = 0x55
    NEGOTIATE_VERSION = 0x56
    SBLR_COMPILED = 0x57
    QUERY_PLAN = 0x58
    STREAM_READY = 0x59
    STREAM_DATA = 0x5A
    STREAM_END = 0x5B
    TXN_STATUS = 0x5C
    PONG = 0x5D
    CLUSTER_AUTH_OK = 0x5E
    FEDERATED_RESULT = 0x5F
    QUERY_PROGRESS = 0x60
    HEARTBEAT = 0x80
    EXTENSION = 0x81


class AuthMethod:
    OK = 0
    PASSWORD = 1
    MD5 = 2
    SCRAM_SHA_256 = 3
    SCRAM_SHA_512 = 4
    TOKEN = 5
    PEER = 6
    REATTACH = 7
    CERTIFICATE = 8
    GSSAPI = 9
    SSPI = 10
    LDAP = 11
    SAML = 12
    OIDC = 13
    MFA_TOTP = 14
    CLUSTER_PKI = 15


MSG_FLAG_COMPRESSED = 0x01
MSG_FLAG_CONTINUED = 0x02
MSG_FLAG_FINAL = 0x04
MSG_FLAG_URGENT = 0x08
MSG_FLAG_ENCRYPTED = 0x10
MSG_FLAG_CHECKSUM = 0x20

FEATURE_COMPRESSION = 1 << 0
FEATURE_STREAMING = 1 << 1
FEATURE_SBLR = 1 << 2
FEATURE_FEDERATION = 1 << 3
FEATURE_NOTIFICATIONS = 1 << 4
FEATURE_QUERY_PLAN = 1 << 5
FEATURE_BATCH = 1 << 6
FEATURE_PIPELINE = 1 << 7
FEATURE_BINARY_COPY = 1 << 8
FEATURE_SAVEPOINTS = 1 << 9

# COPY format codes
COPY_FORMAT_TEXT = 0
COPY_FORMAT_BINARY = 1
FEATURE_2PC = 1 << 10
FEATURE_CHECKSUMS = 1 << 11

QUERY_FLAG_DESCRIBE_ONLY = 0x01
QUERY_FLAG_NO_PORTAL = 0x02
QUERY_FLAG_BINARY_RESULT = 0x04
QUERY_FLAG_INCLUDE_PLAN = 0x08
QUERY_FLAG_RETURN_SBLR = 0x10
QUERY_FLAG_NO_CACHE = 0x20

ISOLATION_READ_UNCOMMITTED = 0
ISOLATION_READ_COMMITTED = 1
ISOLATION_REPEATABLE_READ = 2
ISOLATION_SERIALIZABLE = 3

READ_COMMITTED_MODE_DEFAULT = 0
READ_COMMITTED_MODE_READ_CONSISTENCY = 1
READ_COMMITTED_MODE_RECORD_VERSION = 2
READ_COMMITTED_MODE_NO_RECORD_VERSION = 3

TXN_FLAG_HAS_ISOLATION = 0x0001
TXN_FLAG_HAS_ACCESS = 0x0002
TXN_FLAG_HAS_DEFERRABLE = 0x0004
TXN_FLAG_HAS_WAIT = 0x0008
TXN_FLAG_HAS_TIMEOUT = 0x0010
TXN_FLAG_HAS_AUTOCOMMIT = 0x0020
TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100

STREAM_START = 0
STREAM_PAUSE = 1
STREAM_RESUME = 2
STREAM_CANCEL = 3
STREAM_ACK = 4

SUB_TYPE_CHANNEL = 0
SUB_TYPE_TABLE = 1
SUB_TYPE_QUERY = 2
SUB_TYPE_EVENT = 3

AUTH_PARAM_METHOD_ID = "auth_method_id"
AUTH_PARAM_METHOD_PAYLOAD = "auth_method_payload"
AUTH_PARAM_PAYLOAD_JSON = "auth_payload_json"
AUTH_PARAM_PAYLOAD_B64 = "auth_payload_b64"
AUTH_PARAM_PROVIDER_PROFILE = "auth_provider_profile"
AUTH_PARAM_REQUIRED_METHODS = "auth_required_methods"
AUTH_PARAM_FORBIDDEN_METHODS = "auth_forbidden_methods"
AUTH_PARAM_REQUIRE_CHANNEL_BINDING = "auth_require_channel_binding"
AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN = "workload_identity_token"
AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION = "proxy_principal_assertion"


@dataclass
class AuthPluginSelection:
    method_id: str = ""
    method_payload: str = ""
    payload_json: str = ""
    payload_b64: str = ""
    provider_profile: str = ""
    required_methods: str = ""
    forbidden_methods: str = ""
    require_channel_binding: bool = False
    workload_identity_token: str = ""
    proxy_principal_assertion: str = ""


def apply_auth_plugin_selection(params: Dict[str, str], selection: AuthPluginSelection) -> None:
    method_id = selection.method_id.strip()
    if method_id and not method_id.startswith("scratchbird.auth."):
        raise ValueError("invalid auth_method_id namespace")
    if method_id:
        params[AUTH_PARAM_METHOD_ID] = method_id
    if selection.method_payload:
        params[AUTH_PARAM_METHOD_PAYLOAD] = selection.method_payload
    if selection.payload_json:
        params[AUTH_PARAM_PAYLOAD_JSON] = selection.payload_json
    if selection.payload_b64:
        params[AUTH_PARAM_PAYLOAD_B64] = selection.payload_b64
    if selection.provider_profile:
        params[AUTH_PARAM_PROVIDER_PROFILE] = selection.provider_profile
    if selection.required_methods:
        params[AUTH_PARAM_REQUIRED_METHODS] = selection.required_methods
    if selection.forbidden_methods:
        params[AUTH_PARAM_FORBIDDEN_METHODS] = selection.forbidden_methods
    if selection.require_channel_binding:
        params[AUTH_PARAM_REQUIRE_CHANNEL_BINDING] = "1"
    if selection.workload_identity_token:
        params[AUTH_PARAM_WORKLOAD_IDENTITY_TOKEN] = selection.workload_identity_token
    if selection.proxy_principal_assertion:
        params[AUTH_PARAM_PROXY_PRINCIPAL_ASSERTION] = selection.proxy_principal_assertion


@dataclass
class MessageHeader:
    msg_type: int
    flags: int
    length: int
    sequence: int
    attachment_id: bytes
    txn_id: int


@dataclass
class Message:
    header: MessageHeader
    payload: bytes


@dataclass
class ColumnInfo:
    name: str
    table_oid: int
    column_index: int
    type_oid: int
    type_size: int
    type_modifier: int
    format: int
    nullable: bool


@dataclass
class ColumnValue:
    data: Optional[bytes]


@dataclass
class ParamValue:
    format: int
    data: Optional[bytes]


def encode_message(header: MessageHeader, payload: bytes) -> bytes:
    if len(header.attachment_id) != 16:
        raise ValueError("attachment_id must be 16 bytes")
    out = bytearray(HEADER_SIZE + len(payload))
    out[0:4] = PROTOCOL_MAGIC_BYTES
    out[4] = PROTOCOL_VERSION_MAJOR
    out[5] = PROTOCOL_VERSION_MINOR
    out[6] = header.msg_type
    out[7] = header.flags
    struct.pack_into("<I", out, 8, len(payload))
    struct.pack_into("<I", out, 12, header.sequence)
    out[16:32] = header.attachment_id
    struct.pack_into("<Q", out, 32, header.txn_id)
    out[HEADER_SIZE:] = payload
    return bytes(out)


def decode_header(data: bytes) -> MessageHeader:
    if len(data) != HEADER_SIZE:
        raise ValueError("invalid header length")
    if data[0:4] != PROTOCOL_MAGIC_BYTES:
        raise ValueError(f"invalid protocol magic: {data[:8].hex()}")
    major = data[4]
    minor = data[5]
    if major != PROTOCOL_VERSION_MAJOR or minor != PROTOCOL_VERSION_MINOR:
        raise ValueError("unsupported protocol version")
    length = struct.unpack_from("<I", data, 8)[0]
    if length > MAX_MESSAGE_SIZE:
        raise ValueError("payload too large")
    sequence = struct.unpack_from("<I", data, 12)[0]
    attachment_id = data[16:32]
    txn_id = struct.unpack_from("<Q", data, 32)[0]
    return MessageHeader(data[6], data[7], length, sequence, attachment_id, txn_id)


def build_startup_payload(features: int, params: Dict[str, str]) -> bytes:
    param_bytes = build_param_list(params)
    out = bytearray()
    out += struct.pack("<B", PROTOCOL_VERSION_MAJOR)
    out += struct.pack("<B", PROTOCOL_VERSION_MINOR)
    out += struct.pack("<H", 0)
    out += struct.pack("<Q", features)
    out += param_bytes
    return bytes(out)


def _append_lp_string(out: bytearray, value: str) -> None:
    encoded = value.encode("utf-8")
    out += struct.pack("<I", len(encoded))
    out += encoded


def build_p1_startup_payload(client_features: int,
                             required_features: int,
                             params: Dict[str, str]) -> bytes:
    out = bytearray()
    out += struct.pack("<H", PROTOCOL_VERSION)
    out += struct.pack("<H", PROTOCOL_VERSION)
    out += struct.pack("<I", 0)
    out += struct.pack("<Q", client_features)
    out += struct.pack("<Q", required_features)
    out += struct.pack("<Q", 0)
    out += b"\x11" * 16
    out += b"\x00" * 16
    out += b"\x00" * 16
    out += struct.pack("<I", len(params))
    for key, value in sorted(params.items()):
        _append_lp_string(out, key)
        out += b"\x01\x00"
        encoded = value.encode("utf-8")
        out += struct.pack("<I", len(encoded))
        out += encoded
    out += struct.pack("<I", 0)
    return bytes(out)


def build_param_list(params: Dict[str, str]) -> bytes:
    parts = bytearray()
    for key, value in params.items():
        parts += key.encode("utf-8") + b"\x00"
        parts += value.encode("utf-8") + b"\x00"
    parts += b"\x00"
    return bytes(parts)


def parse_auth_request(payload: bytes) -> Tuple[int, bytes]:
    if len(payload) < 4:
        raise ValueError("auth request truncated")
    method = payload[0]
    return method, payload[4:]


def parse_auth_continue(payload: bytes) -> Tuple[int, int, bytes]:
    if len(payload) < 8:
        raise ValueError("auth continue truncated")
    method = payload[0]
    stage = payload[1]
    data_len = struct.unpack_from("<I", payload, 4)[0]
    if 8 + data_len > len(payload):
        raise ValueError("auth continue truncated")
    return method, stage, payload[8 : 8 + data_len]


def parse_auth_ok(payload: bytes) -> Tuple[bytes, bytes]:
    if len(payload) < 20:
        raise ValueError("auth ok truncated")
    session_id = payload[0:16]
    info_len = struct.unpack_from("<I", payload, 16)[0]
    if 20 + info_len > len(payload):
        raise ValueError("auth ok truncated")
    return session_id, payload[20 : 20 + info_len]


def build_query_payload(sql: str, flags: int, max_rows: int, timeout_ms: int) -> bytes:
    sql_bytes = sql.encode("utf-8") + b"\x00"
    out = bytearray()
    out += struct.pack("<I", flags)
    out += struct.pack("<I", max_rows)
    out += struct.pack("<I", timeout_ms)
    out += sql_bytes
    return bytes(out)


def build_parse_payload(statement_name: str, sql: str, param_types: List[int]) -> bytes:
    name_bytes = statement_name.encode("utf-8")
    sql_bytes = sql.encode("utf-8")
    out = bytearray()
    out += struct.pack("<I", len(name_bytes)) + name_bytes
    out += struct.pack("<I", len(sql_bytes)) + sql_bytes
    out += struct.pack("<H", len(param_types))
    out += struct.pack("<H", 0)
    for oid in param_types:
        out += struct.pack("<I", oid)
    return bytes(out)


def build_bind_payload(portal_name: str, statement_name: str, params: List[ParamValue], result_formats: List[int]) -> bytes:
    portal_bytes = portal_name.encode("utf-8")
    stmt_bytes = statement_name.encode("utf-8")
    out = bytearray()
    out += struct.pack("<I", len(portal_bytes)) + portal_bytes
    out += struct.pack("<I", len(stmt_bytes)) + stmt_bytes
    out += struct.pack("<H", len(params))
    for param in params:
        out += struct.pack("<H", param.format)
    out += struct.pack("<H", len(params))
    out += struct.pack("<H", 0)
    for param in params:
        if param.data is None:
            out += struct.pack("<i", -1)
        else:
            out += struct.pack("<i", len(param.data))
            out += param.data
    out += struct.pack("<H", len(result_formats))
    for fmt in result_formats:
        out += struct.pack("<H", fmt)
    return bytes(out)


def build_execute_payload(portal_name: str, max_rows: int) -> bytes:
    portal_bytes = portal_name.encode("utf-8")
    out = bytearray()
    out += struct.pack("<I", len(portal_bytes)) + portal_bytes
    out += struct.pack("<I", max_rows)
    return bytes(out)


def build_describe_payload(describe_type: int, name: str) -> bytes:
    name_bytes = name.encode("utf-8")
    out = bytearray()
    out.append(describe_type)
    out += b"\x00\x00\x00"
    out += struct.pack("<I", len(name_bytes)) + name_bytes
    return bytes(out)


def build_cancel_payload(cancel_type: int, target_sequence: int) -> bytes:
    return struct.pack("<II", cancel_type, target_sequence)


def build_sblr_execute_payload(sblr_hash: int, sblr_bytecode: Optional[bytes], params: List[ParamValue]) -> bytes:
    bytecode = sblr_bytecode or b""
    out = bytearray()
    out += struct.pack("<Q", sblr_hash)
    out += struct.pack("<I", len(bytecode))
    out += struct.pack("<H", len(params))
    out += struct.pack("<H", 0)
    if bytecode:
        out += bytecode
    for param in params:
        if param.data is None:
            out += struct.pack("<i", -1)
        else:
            out += struct.pack("<i", len(param.data))
            out += param.data
    return bytes(out)


def build_subscribe_payload(subscribe_type: int, channel: str, filter_expr: str = "") -> bytes:
    channel_bytes = channel.encode("utf-8")
    filter_bytes = filter_expr.encode("utf-8")
    out = bytearray()
    out.append(subscribe_type)
    out += b"\x00\x00\x00"
    out += struct.pack("<I", len(channel_bytes)) + channel_bytes
    out += struct.pack("<I", len(filter_bytes)) + filter_bytes
    return bytes(out)


def build_unsubscribe_payload(channel: str) -> bytes:
    channel_bytes = channel.encode("utf-8")
    return struct.pack("<I", len(channel_bytes)) + channel_bytes


def build_txn_begin_payload(
    flags: int,
    conflict_action: int,
    autocommit_mode: int,
    isolation_level: int,
    access_mode: int,
    deferrable: int,
    wait_mode: int,
    timeout_ms: int,
    read_committed_mode: int = 0,
) -> bytes:
    if flags & TXN_FLAG_HAS_READ_COMMITTED_MODE:
        return struct.pack(
            "<HBBBBBBIB3x",
            flags,
            conflict_action,
            autocommit_mode,
            isolation_level,
            access_mode,
            deferrable,
            wait_mode,
            timeout_ms,
            read_committed_mode,
        )
    return struct.pack(
        "<HBBBBBBI",
        flags,
        conflict_action,
        autocommit_mode,
        isolation_level,
        access_mode,
        deferrable,
        wait_mode,
        timeout_ms,
    )


def build_txn_commit_payload(flags: int) -> bytes:
    return struct.pack("<B3x", flags)


def build_txn_rollback_payload(flags: int) -> bytes:
    return struct.pack("<B3x", flags)


def build_txn_savepoint_payload(name: str) -> bytes:
    name_bytes = name.encode("utf-8")
    return struct.pack("<I", len(name_bytes)) + name_bytes


def build_txn_release_payload(name: str) -> bytes:
    return build_txn_savepoint_payload(name)


def build_txn_rollback_to_payload(name: str) -> bytes:
    return build_txn_savepoint_payload(name)


def build_set_option_payload(name: str, value: str) -> bytes:
    name_bytes = name.encode("utf-8")
    value_bytes = value.encode("utf-8")
    out = bytearray()
    out += struct.pack("<I", len(name_bytes)) + name_bytes
    out += struct.pack("<I", len(value_bytes)) + value_bytes
    return bytes(out)


def build_stream_control_payload(control_type: int, window_size: int, timeout_ms: int) -> bytes:
    return struct.pack("<B3xII", control_type, window_size, timeout_ms)


def build_attach_create_payload(emulation_mode: str, db_name: str) -> bytes:
    mode_bytes = emulation_mode.encode("utf-8")
    db_bytes = db_name.encode("utf-8")
    out = bytearray()
    out += struct.pack("<I", len(mode_bytes)) + mode_bytes
    out += struct.pack("<I", len(db_bytes)) + db_bytes
    return bytes(out)


def parse_ready(payload: bytes) -> Tuple[int, int, int]:
    if len(payload) >= 76 and payload[56] in (0x49, 0x54, 0x45, 0x52, 0x41):
        txn_id = struct.unpack_from("<Q", payload, 48)[0]
        status = 1 if payload[56] in (0x54, 0x45) else 0
        return status, txn_id, txn_id
    if len(payload) < 20:
        raise ValueError("ready truncated")
    status = payload[0]
    txn_id = struct.unpack_from("<Q", payload, 4)[0]
    visibility = struct.unpack_from("<Q", payload, 12)[0]
    return status, txn_id, visibility


def parse_txn_status(payload: bytes) -> Tuple[str, int]:
    if len(payload) < 12:
        raise ValueError("txn status truncated")
    status = chr(payload[0])
    txn_id = struct.unpack_from("<Q", payload, 4)[0]
    return status, txn_id


def parse_parameter_statuses(payload: bytes) -> List[Tuple[str, str]]:
    if len(payload) < 8:
        raise ValueError("parameter status truncated")

    count = struct.unpack_from("<I", payload, 0)[0]
    if 0 < count <= 256:
        offset = 4
        values: List[Tuple[str, str]] = []
        try:
            for _ in range(count):
                if offset + 4 > len(payload):
                    raise ValueError("parameter status truncated")
                name_len = struct.unpack_from("<I", payload, offset)[0]
                offset += 4
                if offset + name_len + 7 > len(payload):
                    raise ValueError("parameter status truncated")
                name = payload[offset : offset + name_len].decode("utf-8", errors="replace")
                offset += name_len
                offset += 3
                value_len = struct.unpack_from("<I", payload, offset)[0]
                offset += 4
                if offset + value_len > len(payload):
                    raise ValueError("parameter status truncated")
                value = payload[offset : offset + value_len].decode("utf-8", errors="replace")
                offset += value_len
                values.append((name, value))
            if offset == len(payload):
                return values
        except ValueError:
            pass

    name_len = struct.unpack_from("<I", payload, 0)[0]
    name_start = 4
    name_end = name_start + name_len
    if name_end + 4 > len(payload):
        raise ValueError("parameter status truncated")
    value_len = struct.unpack_from("<I", payload, name_end)[0]
    value_start = name_end + 4
    value_end = value_start + value_len
    if value_end > len(payload):
        raise ValueError("parameter status truncated")
    name = payload[name_start:name_end].decode("utf-8", errors="replace")
    value = payload[value_start:value_end].decode("utf-8", errors="replace")
    return [(name, value)]


def parse_parameter_status(payload: bytes) -> Tuple[str, str]:
    statuses = parse_parameter_statuses(payload)
    if not statuses:
        raise ValueError("parameter status truncated")
    return statuses[0]


def parse_parameter_description(payload: bytes) -> List[int]:
    if len(payload) < 4:
        raise ValueError("parameter description truncated")
    count = struct.unpack_from("<H", payload, 0)[0]
    offset = 4
    types: List[int] = []
    for _ in range(count):
        if offset + 4 > len(payload):
            raise ValueError("parameter description truncated")
        types.append(struct.unpack_from("<I", payload, offset)[0])
        offset += 4
    return types


def parse_row_description(payload: bytes) -> List[ColumnInfo]:
    if len(payload) < 4:
        raise ValueError("row description truncated")
    count = struct.unpack_from("<H", payload, 0)[0]
    offset = 4
    columns: List[ColumnInfo] = []
    for _ in range(count):
        if offset + 4 > len(payload):
            raise ValueError("row description truncated")
        name_len = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        if offset + name_len > len(payload):
            raise ValueError("row description truncated")
        name = payload[offset : offset + name_len].decode("utf-8", errors="replace")
        offset += name_len
        if offset + 18 > len(payload):
            raise ValueError("row description truncated")
        table_oid = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        column_index = struct.unpack_from("<H", payload, offset)[0]
        offset += 2
        type_oid = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        type_size = struct.unpack_from("<h", payload, offset)[0]
        offset += 2
        type_modifier = struct.unpack_from("<i", payload, offset)[0]
        offset += 4
        fmt = payload[offset]
        offset += 1
        nullable = payload[offset] == 1
        offset += 1
        offset += 2
        columns.append(ColumnInfo(name, table_oid, column_index, type_oid, type_size, type_modifier, fmt, nullable))
    return columns


def parse_data_row(payload: bytes, column_count: int) -> List[ColumnValue]:
    if len(payload) < 4:
        raise ValueError("row data truncated")
    count = struct.unpack_from("<H", payload, 0)[0]
    null_bytes = struct.unpack_from("<H", payload, 2)[0]
    if count != column_count:
        raise ValueError("row data column count mismatch")
    offset = 4
    if offset + null_bytes > len(payload):
        raise ValueError("row data truncated")
    null_bitmap = payload[offset : offset + null_bytes]
    offset += null_bytes
    values: List[ColumnValue] = []
    for idx in range(count):
        byte_index = idx // 8
        bit_index = idx % 8
        is_null = byte_index < len(null_bitmap) and (null_bitmap[byte_index] & (1 << bit_index))
        if is_null:
            values.append(ColumnValue(None))
            continue
        if offset + 4 > len(payload):
            raise ValueError("row data truncated")
        length = struct.unpack_from("<i", payload, offset)[0]
        offset += 4
        if length < 0:
            values.append(ColumnValue(None))
            continue
        if offset + length > len(payload):
            raise ValueError("row data truncated")
        data = payload[offset : offset + length]
        offset += length
        values.append(ColumnValue(data))
    return values


def parse_command_complete(payload: bytes) -> Tuple[int, int, int, str]:
    if len(payload) < 20:
        raise ValueError("command complete truncated")
    command_type = payload[0]
    rows = struct.unpack_from("<Q", payload, 4)[0]
    last_id = struct.unpack_from("<Q", payload, 12)[0]
    tag_bytes = payload[20:]
    if b"\x00" in tag_bytes:
        tag_bytes = tag_bytes.split(b"\x00", 1)[0]
    tag = tag_bytes.decode("utf-8", errors="replace")
    return command_type, rows, last_id, tag


def parse_notification(payload: bytes) -> Tuple[int, str, bytes, Optional[str], Optional[int]]:
    if len(payload) < 12:
        raise ValueError("notification truncated")
    offset = 0
    process_id = struct.unpack_from("<I", payload, offset)[0]
    offset += 4
    channel_len = struct.unpack_from("<I", payload, offset)[0]
    offset += 4
    if offset + channel_len + 4 > len(payload):
        raise ValueError("notification truncated")
    channel = payload[offset : offset + channel_len].decode("utf-8", errors="replace")
    offset += channel_len
    payload_len = struct.unpack_from("<I", payload, offset)[0]
    offset += 4
    if offset + payload_len > len(payload):
        raise ValueError("notification truncated")
    data = payload[offset : offset + payload_len]
    offset += payload_len
    change_type = None
    row_id = None
    if offset + 1 <= len(payload):
        change_type = chr(payload[offset])
        offset += 1
        if offset + 8 <= len(payload):
            row_id = struct.unpack_from("<Q", payload, offset)[0]
    return process_id, channel, data, change_type, row_id


def parse_notice(payload: bytes) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    offset = 0
    while offset < len(payload):
        code = payload[offset]
        offset += 1
        if code == 0:
            break
        end = payload.find(b"\x00", offset)
        if end < 0:
            raise ValueError("notice truncated")
        fields[chr(code)] = payload[offset:end].decode("utf-8", errors="replace")
        offset = end + 1
    return fields


def parse_query_plan(payload: bytes) -> Tuple[int, int, int, int, bytes]:
    if len(payload) < 32:
        raise ValueError("query plan truncated")
    plan_format = struct.unpack_from("<I", payload, 0)[0]
    plan_length = struct.unpack_from("<I", payload, 4)[0]
    planning_time_us = struct.unpack_from("<Q", payload, 8)[0]
    estimated_rows = struct.unpack_from("<Q", payload, 16)[0]
    estimated_cost = struct.unpack_from("<Q", payload, 24)[0]
    start = 32
    if start + plan_length > len(payload):
        raise ValueError("query plan truncated")
    plan_data = payload[start : start + plan_length]
    return plan_format, planning_time_us, estimated_rows, estimated_cost, plan_data


def parse_query_progress(payload: bytes) -> Tuple[int, int]:
    if len(payload) < 16:
        raise ValueError("query progress truncated")
    rows_processed = struct.unpack_from("<Q", payload, 0)[0]
    bytes_processed = struct.unpack_from("<Q", payload, 8)[0]
    return rows_processed, bytes_processed


def parse_sblr_compiled(payload: bytes) -> Tuple[int, int, bytes]:
    if len(payload) < 16:
        raise ValueError("sblr compiled truncated")
    sblr_hash = struct.unpack_from("<Q", payload, 0)[0]
    sblr_version = struct.unpack_from("<I", payload, 8)[0]
    sblr_length = struct.unpack_from("<I", payload, 12)[0]
    if 16 + sblr_length > len(payload):
        raise ValueError("sblr compiled truncated")
    bytecode = payload[16 : 16 + sblr_length]
    return sblr_hash, sblr_version, bytecode


# ============================================================================
# COPY Message Builders (SBWP 1.1)
# ============================================================================

def build_copy_data_payload(data: bytes) -> bytes:
    """Build a CopyData message payload."""
    return data


def build_copy_done_payload() -> bytes:
    """Build a CopyDone message payload (empty)."""
    return b""


def build_copy_fail_payload(error_message: str) -> bytes:
    """Build a CopyFail message payload."""
    msg_bytes = error_message.encode("utf-8")
    return struct.pack("<I", len(msg_bytes)) + msg_bytes


# ============================================================================
# COPY Message Parsers (SBWP 1.1)
# ============================================================================

@dataclass
class CopyInResponse:
    format: int
    window_bytes: int


@dataclass
class CopyOutResponse:
    format: int
    column_count: int
    column_formats: List[int]


@dataclass
class CopyBothResponse:
    format: int
    window_bytes: int


@dataclass
class CopyData:
    data: bytes


@dataclass
class CopyFailInfo:
    error_message: str


def parse_copy_in_response(payload: bytes) -> CopyInResponse:
    """Parse a CopyInResponse message from the server."""
    if len(payload) < 5:
        raise ValueError("copy in response truncated")
    fmt = payload[0]
    window = struct.unpack_from("<I", payload, 1)[0]
    return CopyInResponse(fmt, window)


def parse_copy_out_response(payload: bytes) -> CopyOutResponse:
    """Parse a CopyOutResponse message from the server."""
    if len(payload) < 3:
        raise ValueError("copy out response truncated")
    fmt = payload[0]
    col_count = struct.unpack_from("<H", payload, 1)[0]
    offset = 3
    col_formats = []
    for _ in range(col_count):
        if offset + 4 > len(payload):
            raise ValueError("copy out response truncated")
        col_formats.append(struct.unpack_from("<I", payload, offset)[0])
        offset += 4
    return CopyOutResponse(fmt, col_count, col_formats)


def parse_copy_both_response(payload: bytes) -> CopyBothResponse:
    """Parse a CopyBothResponse message from the server."""
    response = parse_copy_in_response(payload)
    return CopyBothResponse(response.format, response.window_bytes)


def parse_copy_data(payload: bytes) -> CopyData:
    """Parse a CopyData message from the server."""
    return CopyData(payload)


def parse_copy_fail(payload: bytes) -> CopyFailInfo:
    """Parse a CopyFail message from the server."""
    if len(payload) < 4:
        raise ValueError("copy fail truncated")
    msg_len = struct.unpack_from("<I", payload, 0)[0]
    if 4 + msg_len > len(payload):
        raise ValueError("copy fail truncated")
    msg = payload[4:4 + msg_len].decode("utf-8", errors="replace")
    return CopyFailInfo(msg)


def parse_error_message(payload: bytes) -> Tuple[str, str, str, str, str]:
    offset = 0
    severity = ""
    sqlstate = ""
    message = ""
    detail = ""
    hint = ""
    while offset < len(payload):
        field = payload[offset]
        offset += 1
        if field == 0:
            break
        start = offset
        while offset < len(payload) and payload[offset] != 0:
            offset += 1
        if offset >= len(payload):
            break
        value = payload[start:offset].decode("utf-8", errors="replace")
        offset += 1
        if field == ord("S"):
            severity = value
        elif field == ord("C"):
            sqlstate = value
        elif field == ord("M"):
            message = value
        elif field == ord("D"):
            detail = value
        elif field == ord("H"):
            hint = value
    return severity, sqlstate, message, detail, hint
