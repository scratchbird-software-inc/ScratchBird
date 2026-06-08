# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo lane runtime shim (Python-backed)
# This module provides the APIs used by Mojo lane tests while the lane remains
# in a Mojo-Python interop phase.

from __future__ import annotations

from dataclasses import dataclass, field
import datetime
import importlib.util
import json
import os
import pathlib
import re
import struct
import sys
from typing import Any, Dict, Iterable, Iterator, List, Mapping, Optional, Tuple
import urllib.parse


class MessageType:
    QUERY = 0x03
    TXN_BEGIN = 0x15
    TXN_COMMIT = 0x16
    TXN_ROLLBACK = 0x17
    TXN_SAVEPOINT = 0x18
    TXN_RELEASE = 0x19
    TXN_ROLLBACK_TO = 0x1A


ISOLATION_READ_UNCOMMITTED = 0
ISOLATION_READ_COMMITTED = 1
ISOLATION_REPEATABLE_READ = 2
ISOLATION_SERIALIZABLE = 3

READ_COMMITTED_MODE_DEFAULT = 0
READ_COMMITTED_MODE_READ_CONSISTENCY = 1
READ_COMMITTED_MODE_RECORD_VERSION = 2
READ_COMMITTED_MODE_NO_RECORD_VERSION = 3

OID_INT4 = 23
OID_TEXT = 25
OID_JSON = 114
OID_POINT = 600
OID_CIDR = 650
OID_MACADDR = 829
OID_INET = 869
OID_VARCHAR = 1043
OID_DATE = 1082
OID_TIME = 1083
OID_TIMESTAMP = 1114
OID_TIMESTAMPTZ = 1184
OID_INTERVAL = 1186
OID_RECORD = 2249
OID_UUID = 2950
OID_MACADDR8 = 774
OID_JSONB = 3802
OID_SB_VECTOR = 16386
OID_INT4_ARRAY = 1007
OID_TEXT_ARRAY = 1009
OID_RECORD_ARRAY = 2287

TXN_FLAG_HAS_ISOLATION = 0x0001
TXN_FLAG_HAS_ACCESS = 0x0002
TXN_FLAG_HAS_DEFERRABLE = 0x0004
TXN_FLAG_HAS_WAIT = 0x0008
TXN_FLAG_HAS_TIMEOUT = 0x0010
TXN_FLAG_HAS_AUTOCOMMIT = 0x0020
TXN_FLAG_HAS_READ_COMMITTED_MODE = 0x0100

METADATA_SCHEMAS_QUERY = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
METADATA_TABLES_QUERY = "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
METADATA_COLUMNS_QUERY = "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
METADATA_INDEXES_QUERY = "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"
METADATA_INDEX_COLUMNS_QUERY = "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"
METADATA_CONSTRAINTS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"
METADATA_PROCEDURES_QUERY = "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name"
METADATA_FUNCTIONS_QUERY = "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name"
METADATA_ROUTINES_QUERY = "SELECT procedure_id AS routine_id, schema_id, procedure_name AS routine_name, routine_type FROM sys.procedures WHERE is_valid = 1 UNION ALL SELECT function_id AS routine_id, schema_id, function_name AS routine_name, 'FUNCTION' AS routine_type FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, routine_name"
METADATA_CATALOGS_QUERY = "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
METADATA_PRIMARY_KEYS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name"
METADATA_FOREIGN_KEYS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name"
METADATA_TABLE_PRIVILEGES_QUERY = "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name"
METADATA_COLUMN_PRIVILEGES_QUERY = "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
METADATA_TYPE_INFO_QUERY = "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name"

DEFAULT_METADATA_COLLECTION = "tables"

_METADATA_COLLECTION_QUERY_MAP = {
    "schemas": METADATA_SCHEMAS_QUERY,
    "tables": METADATA_TABLES_QUERY,
    "columns": METADATA_COLUMNS_QUERY,
    "indexes": METADATA_INDEXES_QUERY,
    "index_columns": METADATA_INDEX_COLUMNS_QUERY,
    "constraints": METADATA_CONSTRAINTS_QUERY,
    "procedures": METADATA_PROCEDURES_QUERY,
    "functions": METADATA_FUNCTIONS_QUERY,
    "routines": METADATA_ROUTINES_QUERY,
    "catalogs": METADATA_CATALOGS_QUERY,
    "primary_keys": METADATA_PRIMARY_KEYS_QUERY,
    "foreign_keys": METADATA_FOREIGN_KEYS_QUERY,
    "table_privileges": METADATA_TABLE_PRIVILEGES_QUERY,
    "column_privileges": METADATA_COLUMN_PRIVILEGES_QUERY,
    "type_info": METADATA_TYPE_INFO_QUERY,
}

_METADATA_COLLECTION_ALIASES = {
    "schema": "schemas",
    "schemas": "schemas",
    "table": "tables",
    "tables": "tables",
    "column": "columns",
    "columns": "columns",
    "index": "indexes",
    "indexes": "indexes",
    "index_column": "index_columns",
    "index_columns": "index_columns",
    "indexcolumn": "index_columns",
    "indexcolumns": "index_columns",
    "constraint": "constraints",
    "constraints": "constraints",
    "procedure": "procedures",
    "procedures": "procedures",
    "function": "functions",
    "functions": "functions",
    "routine": "routines",
    "routines": "routines",
    "catalog": "catalogs",
    "catalogs": "catalogs",
    "primary_key": "primary_keys",
    "primary_keys": "primary_keys",
    "primarykey": "primary_keys",
    "primarykeys": "primary_keys",
    "foreign_key": "foreign_keys",
    "foreign_keys": "foreign_keys",
    "foreignkey": "foreign_keys",
    "foreignkeys": "foreign_keys",
    "table_privilege": "table_privileges",
    "table_privileges": "table_privileges",
    "tableprivilege": "table_privileges",
    "tableprivileges": "table_privileges",
    "column_privilege": "column_privileges",
    "column_privileges": "column_privileges",
    "columnprivilege": "column_privileges",
    "columnprivileges": "column_privileges",
    "type_info": "type_info",
    "typeinfo": "type_info",
}

_METADATA_RESTRICTION_ALIASES = {
    "name": "name",
    "object_name": "name",
    "objectname": "name",
    "entity_name": "name",
    "entityname": "name",
    "catalog": "catalog_name",
    "catalog_name": "catalog_name",
    "catalogname": "catalog_name",
    "table_catalog": "catalog_name",
    "tablecatalog": "catalog_name",
    "table_cat": "catalog_name",
    "tablecat": "catalog_name",
    "schema": "schema_name",
    "schema_name": "schema_name",
    "schemaname": "schema_name",
    "table_schema": "schema_name",
    "tableschema": "schema_name",
    "table_schem": "schema_name",
    "tableschem": "schema_name",
    "table": "table_name",
    "table_name": "table_name",
    "tablename": "table_name",
    "column": "column_name",
    "column_name": "column_name",
    "columnname": "column_name",
    "index": "index_name",
    "index_name": "index_name",
    "indexname": "index_name",
    "constraint": "constraint_name",
    "constraint_name": "constraint_name",
    "constraintname": "constraint_name",
    "routine": "routine_name",
    "routine_name": "routine_name",
    "routinename": "routine_name",
    "procedure": "routine_name",
    "procedure_name": "routine_name",
    "procedurename": "routine_name",
    "function": "routine_name",
    "function_name": "routine_name",
    "functionname": "routine_name",
    "type": "type_name",
    "type_name": "type_name",
    "typename": "type_name",
    "data_type": "type_name",
    "datatype": "type_name",
    "data_type_name": "type_name",
    "datatypename": "type_name",
    "udt_name": "type_name",
    "udtname": "type_name",
}

_SCHEMA_KEYS = (
    "schema_name",
    "TABLE_SCHEM",
    "table_schem",
    "table_schema",
    "TABLE_SCHEMA",
    "schema",
)


_BASE_DATE = datetime.datetime(2000, 1, 1, 0, 0, 0)
_SQLSTATE_MESSAGE_RE = re.compile(r"\[([0-9A-Z]{5})\]")
_PYTHON_DRIVER_MODULE: Any = None

_DEFAULT_AUTH_PLUGIN_IDS: Dict[str, str] = {
    "PASSWORD": "scratchbird.auth.password_compat",
    "MD5": "scratchbird.auth.md5_legacy",
    "SCRAM_SHA_256": "scratchbird.auth.scram_sha_256",
    "SCRAM_SHA_512": "scratchbird.auth.scram_sha_512",
    "TOKEN": "scratchbird.auth.authkey_token",
    "PEER": "scratchbird.auth.peer_uid",
    "REATTACH": "scratchbird.auth.reattach",
}

_ADDITIONAL_CONTINUATION_METHODS = {
    "SCRAM_SHA_256",
    "SCRAM_SHA_512",
    "TOKEN",
    "PEER",
}

RETRY_SCOPE_NONE = "none"
RETRY_SCOPE_RECONNECT = "reconnect"
RETRY_SCOPE_STATEMENT = "statement"
RETRY_SCOPE_TRANSACTION = "transaction"


@dataclass
class ScratchBirdResult:
    rows: List[List[Any]]
    columns: List[Any]
    rowcount: int


@dataclass
class ScratchBirdConfig:
    dsn: str = ""


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


@dataclass
class ScratchBirdRaw:
    oid: int
    data: bytes


@dataclass
class ScratchBirdRange:
    lower: Optional[str] = None
    upper: Optional[str] = None
    lower_inclusive: bool = False
    upper_inclusive: bool = False
    lower_infinite: bool = False
    upper_infinite: bool = False
    empty: bool = False


@dataclass
class ScratchBirdComposite:
    fields: List[Optional[str]]


@dataclass
class ScratchBirdJson:
    raw: bytes
    value: Any = None


@dataclass
class ScratchBirdJsonb:
    raw: bytes
    value: Any = None


@dataclass
class ScratchBirdGeometry:
    raw: str
    wkt: str


@dataclass
class ScratchBirdNetwork:
    kind: str
    address: str


@dataclass
class ScratchBirdDate:
    value: Optional[datetime.date]


@dataclass
class ScratchBirdTime:
    value: Optional[datetime.time]


@dataclass
class ScratchBirdTimestamp:
    value: Optional[datetime.datetime]


@dataclass
class ScratchBirdTimestampTZ:
    value: Optional[datetime.datetime]


@dataclass
class ScratchBirdInterval:
    micros: int
    days: int = 0
    months: int = 0


class ScratchBirdError(Exception):
    def __init__(self, message: str, sqlstate: str = "", detail: str = "", hint: str = ""):
        super().__init__(message)
        self.sqlstate = sqlstate
        self.detail = detail
        self.hint = hint


def _extract_sqlstate(value: Any) -> str:
    if value is None:
        return ""
    sqlstate = str(getattr(value, "sqlstate", "") or "").strip().upper()
    if len(sqlstate) == 5:
        return sqlstate
    match = _SQLSTATE_MESSAGE_RE.search(str(value))
    if match is None:
        return ""
    return match.group(1).upper()


def retry_scope_for_sqlstate(sqlstate: Optional[str]) -> str:
    # Drivers are fail-closed: 40xxx may restart from a fresh statement
    # boundary, 08xxx requires reconnect or reopen, and no automatic whole
    # transaction replay is authorized here.
    if not sqlstate or len(sqlstate) != 5:
        return RETRY_SCOPE_NONE
    if sqlstate in ("40001", "40P01"):
        return RETRY_SCOPE_STATEMENT
    if sqlstate[:2] == "08":
        return RETRY_SCOPE_RECONNECT
    return RETRY_SCOPE_NONE


def is_retryable_sqlstate(sqlstate: Optional[str]) -> bool:
    return retry_scope_for_sqlstate(sqlstate) != RETRY_SCOPE_NONE


def canonical_isolation_label(isolation_level: int) -> str:
    # The Mojo lane still uses SQL-style compatibility aliases for isolation.
    # READ UNCOMMITTED remains a legacy alias here, not a distinct canonical
    # MGA mode.
    mapping = {
        ISOLATION_READ_UNCOMMITTED: "READ COMMITTED",
        ISOLATION_READ_COMMITTED: "READ COMMITTED",
        ISOLATION_REPEATABLE_READ: "SNAPSHOT",
        ISOLATION_SERIALIZABLE: "SNAPSHOT TABLE STABILITY",
    }
    try:
        normalized = int(isolation_level)
    except (TypeError, ValueError):
        return f"UNKNOWN({isolation_level!r})"
    return mapping.get(normalized, f"UNKNOWN({normalized})")


def canonical_read_committed_mode_label(read_committed_mode: int) -> str:
    mapping = {
        READ_COMMITTED_MODE_DEFAULT: "READ COMMITTED",
        READ_COMMITTED_MODE_READ_CONSISTENCY: "READ COMMITTED READ CONSISTENCY",
        READ_COMMITTED_MODE_RECORD_VERSION: "READ COMMITTED RECORD VERSION",
        READ_COMMITTED_MODE_NO_RECORD_VERSION: "READ COMMITTED NO RECORD VERSION",
    }
    try:
        normalized = int(read_committed_mode)
    except (TypeError, ValueError):
        return f"UNKNOWN({read_committed_mode!r})"
    return mapping.get(normalized, f"UNKNOWN({normalized})")


def _quote_string_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def build_prepared_transaction_sql(verb: str, global_transaction_id: str) -> str:
    trimmed = str(global_transaction_id).strip()
    if trimmed == "":
        raise ScratchBirdError("global transaction id is required", "42601")
    return f"{verb} {_quote_string_literal(trimmed)}"


def _to_scratchbird_error(exc: Exception, default_sqlstate: str = "") -> ScratchBirdError:
    if isinstance(exc, ScratchBirdError):
        return exc
    sqlstate = _extract_sqlstate(exc)
    if sqlstate == "":
        sqlstate = default_sqlstate
    message = str(exc) or exc.__class__.__name__
    return ScratchBirdError(message, sqlstate)


def _python_driver_package_dir() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parents[1].parent / "python" / "src" / "scratchbird"


def _load_python_driver_module():
    global _PYTHON_DRIVER_MODULE
    if _PYTHON_DRIVER_MODULE is not None:
        return _PYTHON_DRIVER_MODULE

    package_dir = _python_driver_package_dir()
    init_path = package_dir / "__init__.py"
    if not init_path.is_file():
        raise ScratchBirdError("Mojo wire transport bridge could not find Python driver package", "0A000")

    module_name = "_scratchbird_python_driver"
    cached = sys.modules.get(module_name)
    if cached is not None:
        _PYTHON_DRIVER_MODULE = cached
        return cached

    spec = importlib.util.spec_from_file_location(
        module_name,
        str(init_path),
        submodule_search_locations=[str(package_dir)],
    )
    if spec is None or spec.loader is None:
        raise ScratchBirdError("Mojo wire transport bridge failed to load Python driver package", "0A000")

    module = importlib.util.module_from_spec(spec)
    sys.modules[module_name] = module
    spec.loader.exec_module(module)
    _PYTHON_DRIVER_MODULE = module
    return module


def _resolve_transport_mode(dsn: str) -> str:
    mode = _dsn_last_query_value(
        dsn,
        ("sb_wire_transport", "sb_transport", "wire_transport"),
        None,
    )
    if mode is None:
        mode = os.environ.get("SCRATCHBIRD_MOJO_WIRE_TRANSPORT", "")
    normalized = str(mode or "").strip().lower().replace("-", "_")
    if normalized in ("", "deterministic", "shim", "native_bootstrap"):
        return "deterministic"
    if normalized in ("python", "python_wire", "wire", "sbwp"):
        return "python_wire"
    raise ScratchBirdError("sb_wire_transport must be deterministic or python", "0A000")


def _as_bool(value: Any) -> bool:
    if isinstance(value, bool):
        return value
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def _split_array_items(text: str) -> List[str]:
    items: List[str] = []
    depth = 0
    token = ""
    in_quotes = False
    escaped = False
    for ch in text:
        if escaped:
            token += ch
            escaped = False
            continue
        if ch == "\\":
            token += ch
            escaped = True
            continue
        if ch == '"':
            token += ch
            in_quotes = not in_quotes
            continue
        if in_quotes:
            token += ch
            continue
        if ch == "{":
            depth += 1
            token += ch
            continue
        if ch == "}":
            depth = max(0, depth - 1)
            token += ch
            continue
        if ch == "," and depth == 0:
            items.append(token)
            token = ""
            continue
        token += ch
    if token:
        items.append(token)
    return items


def _split_composite_items(text: str) -> List[str]:
    items: List[str] = []
    token = ""
    in_quotes = False
    escaped = False
    for ch in text:
        if escaped:
            token += ch
            escaped = False
            continue
        if ch == "\\":
            token += ch
            escaped = True
            continue
        if ch == '"':
            token += ch
            in_quotes = not in_quotes
            continue
        if ch == "," and not in_quotes:
            items.append(token)
            token = ""
            continue
        token += ch
    items.append(token)
    return items


def parse_array_literal(text: str) -> List[Any]:
    raw = (text or "").strip()
    if raw in ("", "{}"):
        return []
    if raw.startswith("{") and raw.endswith("}"):
        raw = raw[1:-1]
    if raw == "":
        return []
    out: List[Any] = []
    for part in _split_array_items(raw):
        item = part.strip()
        if item == "NULL":
            out.append(None)
            continue
        if item.startswith("{") and item.endswith("}"):
            out.append(parse_array_literal(item))
            continue
        if len(item) >= 2 and item[0] == '"' and item[-1] == '"':
            out.append(item[1:-1].replace('\\"', '"'))
            continue
        out.append(item)
    return out


def parse_vector_literal(text: str) -> List[float]:
    raw = (text or "").strip()
    if raw == "":
        return []
    if raw.startswith("[") and raw.endswith("]"):
        raw = raw[1:-1]
    if raw.strip() == "":
        return []
    values: List[float] = []
    for token in raw.split(","):
        item = token.strip()
        if item:
            values.append(float(item))
    return values


def parse_range_literal(text: str) -> ScratchBirdRange:
    raw = (text or "").strip()
    if raw == "" or raw.lower() == "empty":
        return ScratchBirdRange(empty=True)
    if len(raw) < 2 or raw[0] not in ("[", "(") or raw[-1] not in ("]", ")"):
        raise RuntimeError("invalid range literal")
    lower_inclusive = raw[0] == "["
    upper_inclusive = raw[-1] == "]"
    body = raw[1:-1]
    comma_idx = body.find(",")
    if comma_idx < 0:
        raise RuntimeError("invalid range literal")
    lower_raw = body[:comma_idx].strip()
    upper_raw = body[comma_idx + 1 :].strip()
    lower = lower_raw if lower_raw != "" else None
    upper = upper_raw if upper_raw != "" else None
    return ScratchBirdRange(
        lower=lower,
        upper=upper,
        lower_inclusive=lower_inclusive,
        upper_inclusive=upper_inclusive,
        lower_infinite=lower is None,
        upper_infinite=upper is None,
        empty=False,
    )


def parse_composite_literal(text: str) -> List[Optional[str]]:
    raw = (text or "").strip()
    if raw == "":
        return []
    if raw.startswith("(") and raw.endswith(")"):
        raw = raw[1:-1]
    if raw == "":
        return []
    out: List[Optional[str]] = []
    for part in _split_composite_items(raw):
        item = part.strip()
        if item == "" or item.upper() == "NULL":
            out.append(None)
            continue
        if len(item) >= 2 and item[0] == '"' and item[-1] == '"':
            out.append(item[1:-1].replace('\\"', '"'))
            continue
        out.append(item)
    return out


def parse_point_literal(text: str) -> ScratchBirdGeometry:
    raw = (text or "").strip()
    if not (raw.startswith("(") and raw.endswith(")")):
        raise RuntimeError("invalid point literal")
    inner = raw[1:-1]
    parts = [part.strip() for part in inner.split(",", 1)]
    if len(parts) != 2:
        raise RuntimeError("invalid point literal")
    x = float(parts[0])
    y = float(parts[1])
    return ScratchBirdGeometry(raw=raw, wkt=f"POINT({x} {y})")


def _network_kind_for_oid(oid: int) -> Optional[str]:
    if oid == OID_INET:
        return "inet"
    if oid == OID_CIDR:
        return "cidr"
    if oid == OID_MACADDR:
        return "macaddr"
    if oid == OID_MACADDR8:
        return "macaddr8"
    return None


def _format_uuid_bytes(data: bytes) -> str:
    if len(data) != 16:
        return data.hex()
    hex_str = data.hex()
    return f"{hex_str[0:8]}-{hex_str[8:12]}-{hex_str[12:16]}-{hex_str[16:20]}-{hex_str[20:32]}"


def _decode_date_value(data: bytes) -> ScratchBirdDate:
    if len(data) == 4:
        days = struct.unpack_from("<i", data, 0)[0]
        return ScratchBirdDate((_BASE_DATE + datetime.timedelta(days=days)).date())
    text = data.decode("utf-8").strip()
    return ScratchBirdDate(datetime.date.fromisoformat(text) if text else None)


def _decode_time_value(data: bytes) -> ScratchBirdTime:
    if len(data) == 8:
        micros_total = struct.unpack_from("<q", data, 0)[0]
        seconds_total, micros = divmod(micros_total, 1_000_000)
        hours, rem = divmod(seconds_total, 3600)
        minutes, seconds = divmod(rem, 60)
        return ScratchBirdTime(datetime.time(int(hours % 24), int(minutes), int(seconds), int(micros)))
    text = data.decode("utf-8").strip()
    return ScratchBirdTime(datetime.time.fromisoformat(text) if text else None)


def _decode_timestamp_value(data: bytes) -> ScratchBirdTimestamp:
    if len(data) == 8:
        micros = struct.unpack_from("<q", data, 0)[0]
        return ScratchBirdTimestamp(_BASE_DATE + datetime.timedelta(microseconds=micros))
    text = data.decode("utf-8").strip()
    if not text:
        return ScratchBirdTimestamp(None)
    return ScratchBirdTimestamp(datetime.datetime.fromisoformat(text.replace(" ", "T")))


def _decode_timestamptz_value(data: bytes) -> ScratchBirdTimestampTZ:
    if len(data) == 8:
        micros = struct.unpack_from("<q", data, 0)[0]
        return ScratchBirdTimestampTZ(_BASE_DATE + datetime.timedelta(microseconds=micros))
    text = data.decode("utf-8").strip()
    if not text:
        return ScratchBirdTimestampTZ(None)
    normalized = text.replace(" ", "T")
    if normalized.endswith("Z"):
        normalized = normalized[:-1] + "+00:00"
    return ScratchBirdTimestampTZ(datetime.datetime.fromisoformat(normalized))


def _decode_interval_value(data: bytes) -> ScratchBirdInterval:
    if len(data) >= 16:
        micros = struct.unpack_from("<q", data, 0)[0]
        days = struct.unpack_from("<i", data, 8)[0]
        months = struct.unpack_from("<i", data, 12)[0]
        return ScratchBirdInterval(micros=micros, days=days, months=months)
    text = data.decode("utf-8").strip()
    if not text:
        return ScratchBirdInterval(micros=0, days=0, months=0)
    if ":" in text and " " not in text:
        parts = text.split(":")
        if len(parts) == 3:
            hours = int(parts[0])
            minutes = int(parts[1])
            seconds = float(parts[2])
            micros = int(((hours * 3600) + (minutes * 60) + seconds) * 1_000_000)
            return ScratchBirdInterval(micros=micros, days=0, months=0)
    return ScratchBirdInterval(micros=0, days=0, months=0)


def _decode_json_wrapper(data: bytes, jsonb: bool) -> Any:
    if jsonb and len(data) > 0 and data[0] == 1:
        raw = data
        payload = data[1:]
    else:
        raw = data
        payload = data
    text = payload.decode("utf-8").strip()
    value = json.loads(text) if text else None
    if jsonb:
        return ScratchBirdJsonb(raw=raw, value=value)
    return ScratchBirdJson(raw=raw, value=value)


def _encode_array_value(value: Iterable[Any]) -> bytes:
    parts: List[str] = []
    for item in value:
        if item is None:
            parts.append("NULL")
            continue
        if isinstance(item, (list, tuple)):
            nested = _encode_array_value(item).decode("utf-8")
            parts.append(nested)
            continue
        if isinstance(item, ScratchBirdComposite):
            composite_text = encode_value(item).decode("utf-8")
            escaped = composite_text.replace('"', '\\"')
            parts.append(f'"{escaped}"')
            continue
        raw = str(item)
        if any(ch in raw for ch in (",", "{", "}", "\"", " ")):
            escaped = raw.replace('"', '\\"')
            parts.append(f'"{escaped}"')
        else:
            parts.append(raw)
    return ("{" + ",".join(parts) + "}").encode("utf-8")


def encode_value(value: Any) -> bytes:
    if value is None:
        return b""
    if isinstance(value, bytes):
        return value
    if isinstance(value, int):
        return struct.pack("<i", int(value))
    if isinstance(value, ScratchBirdRange):
        if value.empty:
            return b"empty"
        left = "[" if value.lower_inclusive else "("
        right = "]" if value.upper_inclusive else ")"
        lower = "" if value.lower is None else str(value.lower)
        upper = "" if value.upper is None else str(value.upper)
        return f"{left}{lower},{upper}{right}".encode("utf-8")
    if isinstance(value, ScratchBirdComposite):
        encoded_fields: List[str] = []
        for field in value.fields:
            if field is None:
                encoded_fields.append("NULL")
                continue
            raw = str(field)
            if any(ch in raw for ch in (",", "\"", "(", ")")):
                escaped = raw.replace('"', '\\"')
                encoded_fields.append(f'"{escaped}"')
            else:
                encoded_fields.append(raw)
        return f"({','.join(encoded_fields)})".encode("utf-8")
    if isinstance(value, ScratchBirdGeometry):
        return value.raw.encode("utf-8")
    if isinstance(value, ScratchBirdNetwork):
        return value.address.encode("utf-8")
    if isinstance(value, ScratchBirdJson):
        if value.raw:
            return value.raw
        return json.dumps(value.value).encode("utf-8")
    if isinstance(value, ScratchBirdJsonb):
        if value.raw:
            return value.raw
        return b"\x01" + json.dumps(value.value).encode("utf-8")
    if isinstance(value, ScratchBirdDate):
        return b"" if value.value is None else value.value.isoformat().encode("utf-8")
    if isinstance(value, ScratchBirdTime):
        return b"" if value.value is None else value.value.isoformat().encode("utf-8")
    if isinstance(value, ScratchBirdTimestamp):
        if value.value is None:
            return b""
        return value.value.isoformat(sep=" ").encode("utf-8")
    if isinstance(value, ScratchBirdTimestampTZ):
        if value.value is None:
            return b""
        return value.value.isoformat(sep=" ").encode("utf-8")
    if isinstance(value, ScratchBirdInterval):
        return struct.pack("<qii", int(value.micros), int(value.days), int(value.months))
    if isinstance(value, (list, tuple)):
        if all(isinstance(item, (int, float)) for item in value):
            vector = ",".join(str(float(item)) for item in value)
            return f"[{vector}]".encode("utf-8")
        return _encode_array_value(value)
    return str(value).encode("utf-8")


def decode_value(oid: int, data: Optional[bytes]) -> Any:
    if data is None:
        return None
    if oid == OID_INT4:
        if len(data) < 4:
            raise RuntimeError("row data truncated")
        return struct.unpack_from("<i", data, 0)[0]
    if oid in (OID_TEXT, OID_VARCHAR):
        return data.decode("utf-8")
    if oid == OID_UUID:
        if len(data) == 16:
            return _format_uuid_bytes(data)
        return data.decode("utf-8")
    if oid == OID_JSON:
        return _decode_json_wrapper(data, jsonb=False)
    if oid == OID_JSONB:
        return _decode_json_wrapper(data, jsonb=True)
    if oid == OID_DATE:
        return _decode_date_value(data)
    if oid == OID_TIME:
        return _decode_time_value(data)
    if oid == OID_TIMESTAMP:
        return _decode_timestamp_value(data)
    if oid == OID_TIMESTAMPTZ:
        return _decode_timestamptz_value(data)
    if oid == OID_INTERVAL:
        return _decode_interval_value(data)
    if oid == OID_SB_VECTOR:
        return parse_vector_literal(data.decode("utf-8"))
    if oid == OID_RECORD:
        return ScratchBirdComposite(parse_composite_literal(data.decode("utf-8")))
    if oid == OID_INT4_ARRAY:
        parsed = parse_array_literal(data.decode("utf-8"))
        return [None if item is None else int(item) for item in parsed]
    if oid == OID_TEXT_ARRAY:
        return parse_array_literal(data.decode("utf-8"))
    if oid == OID_RECORD_ARRAY:
        parsed = parse_array_literal(data.decode("utf-8"))
        out = []
        for item in parsed:
            if item is None:
                out.append(None)
            else:
                out.append(ScratchBirdComposite(parse_composite_literal(str(item))))
        return out
    if oid == OID_POINT:
        return parse_point_literal(data.decode("utf-8"))
    network_kind = _network_kind_for_oid(oid)
    if network_kind is not None:
        return ScratchBirdNetwork(kind=network_kind, address=data.decode("utf-8"))
    return ScratchBirdRaw(oid, data)


def _dsn_query_params(dsn: str) -> Dict[str, str]:
    if not dsn:
        return {}
    parsed = urllib.parse.urlparse(dsn)
    return {str(key).strip().lower(): value for key, value in urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)}


def _dsn_last_query_value(dsn: str, keys: Iterable[str], default: Optional[str] = None) -> Optional[str]:
    if not dsn:
        return default
    parsed = urllib.parse.urlparse(dsn)
    keyset = {str(key).strip().lower() for key in keys}
    found = False
    last = default
    for key, value in urllib.parse.parse_qsl(parsed.query, keep_blank_values=True):
        candidate = str(key).strip().lower()
        if candidate in keyset:
            last = value
            found = True
    return last if found else default


def _ensure_default_session_schema_dsn(dsn: str) -> str:
    if not dsn:
        return dsn
    parsed = urllib.parse.urlparse(dsn)
    schema_keys = {"schema", "current_schema", "search_path", "searchpath", "currentschema"}
    for key, _value in urllib.parse.parse_qsl(parsed.query, keep_blank_values=True):
        if str(key).strip().lower().replace("-", "_") in schema_keys:
            return dsn
    query_pairs = urllib.parse.parse_qsl(parsed.query, keep_blank_values=True)
    query_pairs.append(("current_schema", "users.public"))
    encoded_query = urllib.parse.urlencode(query_pairs)
    return urllib.parse.urlunparse(
        (parsed.scheme, parsed.netloc, parsed.path, parsed.params, encoded_query, parsed.fragment)
    )


def _dsn_last_int_query_value(
    dsn: str,
    keys: Iterable[str],
    default: int,
    field_name: str,
) -> int:
    raw = _dsn_last_query_value(dsn, keys, None)
    if raw is None:
        return default
    text = str(raw).strip()
    if text == "":
        raise ScratchBirdError(f"{field_name} must be a valid integer", "22023")
    try:
        return int(text)
    except ValueError:
        raise ScratchBirdError(f"{field_name} must be a valid integer", "22023")


def _has_malformed_percent_escape(text: str) -> bool:
    i = 0
    while i < len(text):
        if text[i] != "%":
            i += 1
            continue
        if i + 2 >= len(text):
            return True
        hi = text[i + 1]
        lo = text[i + 2]
        if not (hi.isdigit() or ("a" <= hi.lower() <= "f")):
            return True
        if not (lo.isdigit() or ("a" <= lo.lower() <= "f")):
            return True
        i += 3
    return False


def _dsn_has_malformed_query_escape(dsn: str) -> bool:
    if "?" not in dsn:
        return False
    query = dsn.split("?", 1)[1]
    if query.strip() == "":
        return False
    return _has_malformed_percent_escape(query)


def _dsn_has_malformed_bracketed_ipv6_host(dsn: str) -> bool:
    try:
        parsed = urllib.parse.urlparse(dsn)
    except ValueError:
        return True
    host_port = parsed.netloc
    if "@" in host_port:
        host_port = host_port.split("@", 1)[1]
    host_port = host_port.strip()
    if host_port == "":
        return False
    if host_port.startswith("["):
        if "]" not in host_port:
            return True
        _, suffix = host_port.split("]", 1)
        suffix = suffix.strip()
        if suffix == "":
            return False
        if not suffix.startswith(":"):
            return True
        raw_port = suffix[1:].strip()
        if raw_port == "":
            return True
        return not raw_port.isdigit()
    return "[" in host_port or "]" in host_port


def _normalize_protocol_value(value: Optional[str]) -> str:
    normalized = str(value or "").strip().lower().replace("-", "_")
    if normalized == "":
        return "native"
    if normalized in ("scratchbird", "scratchbird_native", "scratchbirdnative"):
        return "native"
    return normalized


def _auth_method_from_plugin_id(plugin_method_id: Optional[str]) -> Optional[str]:
    normalized = str(plugin_method_id or "").strip().lower()
    if normalized == "":
        return None
    if not normalized.startswith("scratchbird.auth."):
        raise ScratchBirdError("invalid auth_method_id namespace", "28000")
    if "scram_sha_512" in normalized:
        return "SCRAM_SHA_512"
    if "scram_sha_256" in normalized or "scram256" in normalized:
        return "SCRAM_SHA_256"
    if "md5" in normalized:
        return "MD5"
    if "peer" in normalized:
        return "PEER"
    if "reattach" in normalized:
        return "REATTACH"
    if "token" in normalized or "jwt" in normalized or "oidc" in normalized or "authkey" in normalized:
        return "TOKEN"
    if "password" in normalized or "ldap" in normalized:
        return "PASSWORD"
    return None


def _auth_method_executable_locally(method: str) -> bool:
    return method in ("PASSWORD", "SCRAM_SHA_256", "SCRAM_SHA_512", "TOKEN")


def _auth_method_broker_required(method: str) -> bool:
    return method == "PEER"


def _describe_auth_method(method: str, configured_plugin_method_id: Optional[str] = None) -> AuthMethodSurface:
    plugin_method_id = str(configured_plugin_method_id or "").strip() or _DEFAULT_AUTH_PLUGIN_IDS.get(method)
    return AuthMethodSurface(
        wire_method=method,
        plugin_method_id=plugin_method_id or None,
        executable_locally=_auth_method_executable_locally(method),
        broker_required=_auth_method_broker_required(method),
    )


def _first_known_required_auth_method(dsn: str) -> Optional[str]:
    raw = _dsn_last_query_value(dsn, ("auth_required_methods", "authrequiredmethods"), "")
    if raw is None:
        return None
    for item in str(raw).split(","):
        normalized = item.strip().upper().replace("-", "_")
        if normalized in _DEFAULT_AUTH_PLUGIN_IDS:
            return normalized
    return None


def _resolve_configured_auth_method(dsn: str, ingress_mode: str) -> Tuple[str, Optional[str]]:
    if ingress_mode == "manager_proxy":
        return "TOKEN", "scratchbird.auth.authkey_token"
    configured_plugin = _dsn_last_query_value(dsn, ("auth_method_id", "authmethodid"), "")
    if configured_plugin:
        resolved = _auth_method_from_plugin_id(configured_plugin)
        if resolved is not None:
            return resolved, str(configured_plugin).strip()
    required_method = _first_known_required_auth_method(dsn)
    if required_method is not None:
        return required_method, _DEFAULT_AUTH_PLUGIN_IDS.get(required_method)
    token_payload = _dsn_last_query_value(
        dsn,
        (
            "auth_token",
            "authtoken",
            "bearer_token",
            "bearertoken",
            "token",
            "auth_method_payload",
            "authmethodpayload",
            "auth_payload_json",
            "authpayloadjson",
            "auth_payload_b64",
            "authpayloadb64",
            "workload_identity_token",
            "workloadidentitytoken",
            "proxy_principal_assertion",
            "proxyprincipalassertion",
            "proxy_assertion",
        ),
        "",
    )
    if token_payload is not None and str(token_payload).strip() != "":
        return "TOKEN", _DEFAULT_AUTH_PLUGIN_IDS.get("TOKEN")
    return "PASSWORD", _DEFAULT_AUTH_PLUGIN_IDS.get("PASSWORD")


def _resolve_connect_target(
    config: ScratchBirdConfig,
    *,
    require_identity: bool = True,
    require_manager_token: bool = True,
    perform_auth_fail_guard: bool = True,
) -> Dict[str, Any]:
    if _dsn_has_malformed_query_escape(config.dsn):
        raise ScratchBirdError("DSN query contains malformed percent-escape", "22023")
    if _dsn_has_malformed_bracketed_ipv6_host(config.dsn):
        raise ScratchBirdError("DSN contains malformed bracketed IPv6 host", "22023")
    params = _dsn_query_params(config.dsn)
    parsed = urllib.parse.urlparse(config.dsn)

    user = parsed.username or ""
    user_override = _dsn_last_query_value(config.dsn, ("user", "username", "pguser"), None)
    if user_override is not None:
        user = str(user_override)

    database = parsed.path.lstrip("/")
    database_override = _dsn_last_query_value(config.dsn, ("database", "dbname", "databasename", "pgdatabase"), None)
    if database_override is not None:
        database = str(database_override)

    host = parsed.hostname or ""
    host_override = _dsn_last_query_value(config.dsn, ("host", "hostname", "servername", "pghost"), None)
    if host_override is not None:
        host = str(host_override)
        if host.strip() == "":
            raise ScratchBirdError("host and database are required", "28000")
    if host.strip() == "":
        host = "localhost"

    try:
        port = parsed.port
    except ValueError:
        raise ScratchBirdError("port must be a valid integer", "22023")
    if port is None:
        port = 3092
    port_override = _dsn_last_query_value(config.dsn, ("port", "portnumber", "pgport"), None)
    if port_override is not None:
        text = str(port_override).strip()
        if text == "":
            raise ScratchBirdError("port must be a valid integer", "22023")
        try:
            port = int(text)
        except ValueError:
            raise ScratchBirdError("port must be a valid integer", "22023")

    if require_identity and (user.strip() == "" or database.strip() == ""):
        raise ScratchBirdError("user and database are required", "28000")
    if port <= 0:
        raise ScratchBirdError("port must be positive", "22023")
    if port > 65535:
        raise ScratchBirdError("port must be between 1 and 65535", "22023")

    protocol = _normalize_protocol_value(
        _dsn_last_query_value(config.dsn, ("protocol", "parser", "dialect"), "native")
    )
    if protocol != "native":
        raise ScratchBirdError("protocol must be native", "0A000")

    front_door_mode = str(
        _dsn_last_query_value(
            config.dsn,
            ("front_door_mode", "frontdoormode", "connection_mode", "ingress_mode"),
            "direct",
        )
        or "direct"
    ).strip().lower()
    if front_door_mode in ("managerproxy", "manager-proxy", "managed"):
        front_door_mode = "manager_proxy"
    if front_door_mode not in ("", "direct", "manager_proxy"):
        raise ScratchBirdError("front_door_mode must be direct or manager_proxy.", "0A000")
    if require_manager_token and front_door_mode == "manager_proxy":
        token = _dsn_last_query_value(config.dsn, ("manager_auth_token", "mcp_auth_token"), "")
        if token is None or str(token).strip() == "":
            raise ScratchBirdError("manager_auth_token is required for manager_proxy mode", "08001")

    sslmode = str(
        _dsn_last_query_value(config.dsn, ("sslmode", "ssl"), params.get("sslmode", "require"))
        or "require"
    ).strip().lower()

    _ = _dsn_last_query_value(config.dsn, ("binary_transfer", "binarytransfer"), None)
    compression = str(_dsn_last_query_value(config.dsn, ("compression",), "off") or "off").strip().lower()
    if compression == "none":
        compression = "off"
    if compression not in ("off", "zstd", ""):
        raise ScratchBirdError(f"compression={compression} is not supported", "0A000")

    min_pool_size = _dsn_last_int_query_value(config.dsn, ("min_pool_size", "minpoolsize"), 0, "min_pool_size")
    max_pool_size = _dsn_last_int_query_value(config.dsn, ("max_pool_size", "maxpoolsize"), 1, "max_pool_size")
    default_row_fetch_size = _dsn_last_int_query_value(
        config.dsn,
        ("default_row_fetch_size", "fetch_size", "fetchsize", "defaultrowfetchsize"),
        0,
        "default_row_fetch_size",
    )
    connection_lifetime = _dsn_last_int_query_value(
        config.dsn,
        ("connection_lifetime", "connectionlifetime", "poolingconnectionlifetime"),
        0,
        "connection_lifetime",
    )
    manager_client_flags = _dsn_last_int_query_value(
        config.dsn,
        ("manager_client_flags", "mcp_client_flags"),
        0,
        "manager_client_flags",
    )
    connect_client_flags = _dsn_last_int_query_value(
        config.dsn,
        ("client_flags", "connect_client_flags"),
        0x0100,
        "connect_client_flags",
    )
    auth_method_id = str(
        _dsn_last_query_value(
            config.dsn,
            ("auth_method_id", "authmethodid"),
            "",
        )
        or ""
    ).strip()
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("prepare_threshold", "preparethreshold"),
        5,
        "prepare_threshold",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("cb_failure_threshold",),
        5,
        "cb_failure_threshold",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("cb_recovery_timeout_ms",),
        30_000,
        "cb_recovery_timeout_ms",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("cb_success_threshold",),
        2,
        "cb_success_threshold",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("cb_half_open_max_requests",),
        1,
        "cb_half_open_max_requests",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("keepalive_max_idle_before_check_ms",),
        30_000,
        "keepalive_max_idle_before_check_ms",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("pipeline_max_in_flight",),
        64,
        "pipeline_max_in_flight",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("pipeline_auto_flush_threshold",),
        1,
        "pipeline_auto_flush_threshold",
    )
    _ = _dsn_last_int_query_value(
        config.dsn,
        ("leak_threshold_ms",),
        5_000,
        "leak_threshold_ms",
    )
    connect_timeout = _dsn_last_int_query_value(
        config.dsn,
        ("connect_timeout", "connecttimeout"),
        10,
        "connect_timeout",
    )
    socket_timeout = _dsn_last_int_query_value(
        config.dsn,
        ("socket_timeout", "sockettimeout"),
        10,
        "socket_timeout",
    )
    login_timeout = _dsn_last_int_query_value(
        config.dsn,
        ("login_timeout", "logintimeout"),
        10,
        "login_timeout",
    )
    acquire_timeout = _dsn_last_int_query_value(
        config.dsn,
        ("acquire_timeout", "acquiretimeout", "pooling_acquire_timeout", "poolingacquiretimeout"),
        15,
        "acquire_timeout",
    )
    if min_pool_size < 0:
        raise ScratchBirdError("min_pool_size must be >= 0", "22023")
    if max_pool_size < 1:
        raise ScratchBirdError("max_pool_size must be >= 1", "22023")
    if min_pool_size > max_pool_size:
        raise ScratchBirdError("min_pool_size must be <= max_pool_size", "22023")
    if connect_timeout < 0:
        raise ScratchBirdError("connect_timeout must be >= 0", "22023")
    if socket_timeout < 0:
        raise ScratchBirdError("socket_timeout must be >= 0", "22023")
    if login_timeout < 0:
        raise ScratchBirdError("login_timeout must be >= 0", "22023")
    if acquire_timeout < 0:
        raise ScratchBirdError("acquire_timeout must be >= 0", "22023")
    if default_row_fetch_size < 0:
        raise ScratchBirdError("default_row_fetch_size must be >= 0", "22023")
    if connection_lifetime < 0:
        raise ScratchBirdError("connection_lifetime must be >= 0", "22023")
    if manager_client_flags < 0:
        raise ScratchBirdError("manager_client_flags must be >= 0", "22023")
    if connect_client_flags < 0:
        raise ScratchBirdError("connect_client_flags must be >= 0", "22023")
    if auth_method_id != "" and not auth_method_id.startswith("scratchbird.auth."):
        raise ScratchBirdError("invalid auth_method_id namespace", "28000")

    if perform_auth_fail_guard and _as_bool(params.get("sb_test_auth_fail", "0")):
        raise ScratchBirdError("authentication failed", "28P01")

    return {
        "user": user,
        "database": database,
        "host": host,
        "port": port,
        "protocol": protocol,
        "front_door_mode": "direct" if front_door_mode == "" else front_door_mode,
        "auth_method_id": auth_method_id,
        "compression": compression,
    }


def _validate_connect_guards(config: ScratchBirdConfig) -> None:
    _resolve_connect_target(config)


def _probe_auth_surface_deterministic(config: ScratchBirdConfig) -> AuthProbeResult:
    target = _resolve_connect_target(
        config,
        require_identity=False,
        require_manager_token=False,
        perform_auth_fail_guard=False,
    )
    resolved_method, resolved_plugin = _resolve_configured_auth_method(config.dsn, target["front_door_mode"])
    method_surface = _describe_auth_method(resolved_method, resolved_plugin)
    return AuthProbeResult(
        reachable=True,
        ingress_mode=target["front_door_mode"],
        resolved_host=str(target["host"]),
        resolved_port=int(target["port"]),
        admitted_methods=[method_surface],
        required_method=method_surface.wire_method,
        required_plugin_method_id=method_surface.plugin_method_id,
        allowed_transport_mask=None,
        additional_continuation_possible=method_surface.wire_method in _ADDITIONAL_CONTINUATION_METHODS,
    )


def _resolved_auth_context_for_probe(probe: AuthProbeResult, attached: bool) -> ResolvedAuthContext:
    return ResolvedAuthContext(
        ingress_mode=probe.ingress_mode,
        resolved_auth_method=probe.required_method,
        resolved_auth_plugin_id=probe.required_plugin_method_id,
        manager_authenticated=probe.ingress_mode == "manager_proxy" and attached,
        attached=attached,
    )


def _copy_resolved_auth_context(value: ResolvedAuthContext) -> ResolvedAuthContext:
    return ResolvedAuthContext(
        ingress_mode=value.ingress_mode,
        resolved_auth_method=value.resolved_auth_method,
        resolved_auth_plugin_id=value.resolved_auth_plugin_id,
        manager_authenticated=value.manager_authenticated,
        attached=value.attached,
    )


def _copy_auth_probe_result(value: AuthProbeResult) -> AuthProbeResult:
    return AuthProbeResult(
        reachable=value.reachable,
        ingress_mode=value.ingress_mode,
        resolved_host=value.resolved_host,
        resolved_port=value.resolved_port,
        admitted_methods=[
            AuthMethodSurface(
                wire_method=method.wire_method,
                plugin_method_id=method.plugin_method_id,
                executable_locally=method.executable_locally,
                broker_required=method.broker_required,
            )
            for method in value.admitted_methods
        ],
        required_method=value.required_method,
        required_plugin_method_id=value.required_plugin_method_id,
        allowed_transport_mask=value.allowed_transport_mask,
        additional_continuation_possible=value.additional_continuation_possible,
    )


def _coerce_probe_input(config_or_dsn: Any) -> ScratchBirdConfig:
    if isinstance(config_or_dsn, ScratchBirdConfig):
        return config_or_dsn
    if isinstance(config_or_dsn, str):
        return ScratchBirdConfig(config_or_dsn)
    if config_or_dsn is None:
        return ScratchBirdConfig("")
    raise ScratchBirdError("probe_auth_surface expects a ScratchBirdConfig or DSN string", "22023")


def probe_auth_surface(config_or_dsn: Any) -> AuthProbeResult:
    config = _coerce_probe_input(config_or_dsn)
    transport_mode = _resolve_transport_mode(config.dsn)
    if transport_mode == "python_wire":
        wire_module = _load_python_driver_module()
        probe = wire_module.probe_auth_surface(dsn=_ensure_default_session_schema_dsn(config.dsn))
        return AuthProbeResult(
            reachable=bool(probe.reachable),
            ingress_mode=str(probe.ingress_mode),
            resolved_host=str(probe.resolved_host),
            resolved_port=int(probe.resolved_port),
            admitted_methods=[
                AuthMethodSurface(
                    wire_method=str(method.wire_method),
                    plugin_method_id=getattr(method, "plugin_method_id", None),
                    executable_locally=bool(getattr(method, "executable_locally", False)),
                    broker_required=bool(getattr(method, "broker_required", False)),
                )
                for method in getattr(probe, "admitted_methods", [])
            ],
            required_method=getattr(probe, "required_method", None),
            required_plugin_method_id=getattr(probe, "required_plugin_method_id", None),
            allowed_transport_mask=getattr(probe, "allowed_transport_mask", None),
            additional_continuation_possible=bool(getattr(probe, "additional_continuation_possible", False)),
        )
    return _probe_auth_surface_deterministic(config)


def _expected_param_count(sql: str) -> int:
    max_index = 0
    i = 0
    while i < len(sql):
        if sql[i] == "$":
            j = i + 1
            index = 0
            has_digit = False
            while j < len(sql) and sql[j].isdigit():
                index = (index * 10) + int(sql[j])
                has_digit = True
                j += 1
            if has_digit:
                if index > max_index:
                    max_index = index
                i = j
                continue
        i += 1
    return max_index


def _select_integer_literal(statement: str) -> Optional[int]:
    match = re.fullmatch(r"select\s+([+-]?\d+)", statement.strip().lower())
    if match is None:
        return None
    return int(match.group(1))


def _coerce_savepoint_name(name: Optional[str]) -> str:
    if name is None:
        return ""
    return str(name).strip()


def _build_savepoint_payload(name: str) -> bytes:
    encoded = name.encode("utf-8")
    return struct.pack("<I", len(encoded)) + encoded


def _coerce_txn_option_int(value: Any, field_name: str) -> int:
    if isinstance(value, bool):
        return 1 if value else 0
    try:
        return int(value)
    except (TypeError, ValueError):
        raise ScratchBirdError(f"{field_name} must be a valid integer", "22023")


def _normalize_begin_options(kwargs: Mapping[str, Any]) -> Dict[str, int]:
    wait_raw = kwargs.get("wait_mode", kwargs.get("wait", 0))
    isolation_level = _coerce_txn_option_int(
        kwargs.get("isolation_level", ISOLATION_READ_COMMITTED),
        "isolation_level",
    )
    has_read_committed_mode = "read_committed_mode" in kwargs
    read_committed_mode = READ_COMMITTED_MODE_DEFAULT
    if has_read_committed_mode:
        read_committed_mode = _coerce_txn_option_int(
            kwargs.get("read_committed_mode", READ_COMMITTED_MODE_DEFAULT),
            "read_committed_mode",
        )
        if isolation_level not in (ISOLATION_READ_UNCOMMITTED, ISOLATION_READ_COMMITTED):
            raise ScratchBirdError(
                "read_committed_mode requires a READ COMMITTED isolation alias",
                "0A000",
            )
        if "isolation_level" not in kwargs:
            isolation_level = ISOLATION_READ_COMMITTED
    return {
        "conflict_action": _coerce_txn_option_int(kwargs.get("conflict_action", 0), "conflict_action"),
        "autocommit_mode": _coerce_txn_option_int(kwargs.get("autocommit_mode", 0), "autocommit_mode"),
        "isolation_level": isolation_level,
        "access_mode": _coerce_txn_option_int(kwargs.get("access_mode", 0), "access_mode"),
        "deferrable": _coerce_txn_option_int(kwargs.get("deferrable", 0), "deferrable"),
        "wait_mode": _coerce_txn_option_int(wait_raw, "wait_mode"),
        "timeout_ms": _coerce_txn_option_int(kwargs.get("timeout_ms", 0), "timeout_ms"),
        "read_committed_mode": read_committed_mode,
        "has_read_committed_mode": 1 if has_read_committed_mode else 0,
    }


def _guard_static_connection_open(conn: Any) -> None:
    if bool(getattr(conn, "_closed", False)):
        raise ScratchBirdError("connection is closed", "08003")


def _static_savepoint_list(conn: Any) -> List[str]:
    savepoints = getattr(conn, "_savepoints", None)
    if isinstance(savepoints, list):
        return savepoints
    savepoints = []
    setattr(conn, "_savepoints", savepoints)
    return savepoints


def _connection_runtime_txn_active(conn: Any) -> bool:
    return bool(getattr(conn, "_runtime_txn_active", False)) or int(getattr(conn, "_txn_id", 0)) != 0


def _clear_connection_txn_state(conn: Any) -> None:
    setattr(conn, "_txn_id", 0)
    setattr(conn, "_runtime_txn_active", False)


def _apply_connection_runtime_txn_id(conn: Any, txn_id: int) -> None:
    setattr(conn, "_txn_id", int(txn_id))
    if int(txn_id) != 0:
        setattr(conn, "_runtime_txn_active", True)


def _can_adopt_fresh_native_boundary(normalized: Mapping[str, int]) -> bool:
    return (
        int(normalized.get("conflict_action", 0)) == 0
        and int(normalized.get("autocommit_mode", 0)) == 0
        and int(normalized.get("isolation_level", ISOLATION_READ_COMMITTED)) == ISOLATION_READ_COMMITTED
        and int(normalized.get("access_mode", 0)) == 0
        and int(normalized.get("deferrable", 0)) == 0
        and int(normalized.get("wait_mode", 0)) == 0
        and int(normalized.get("timeout_ms", 0)) == 0
        and int(normalized.get("read_committed_mode", READ_COMMITTED_MODE_DEFAULT)) == READ_COMMITTED_MODE_DEFAULT
    )


def _result_rowcount_or_len(result: Any) -> int:
    rowcount = getattr(result, "rowcount", None)
    if isinstance(rowcount, int) and not isinstance(rowcount, bool) and rowcount >= 0:
        return rowcount
    return len(_result_rows_or_empty(result))


def _result_rows_or_empty(result: Any) -> List[Any]:
    rows = getattr(result, "rows", None)
    if rows is None:
        return []
    if isinstance(rows, list):
        return rows
    if isinstance(rows, tuple):
        return list(rows)
    if isinstance(rows, (str, bytes, bytearray)):
        return []
    if isinstance(rows, Mapping):
        return []
    return []


class _ShimStatement:
    def __init__(self, conn: "_ShimConnection", sql: str):
        self._conn = conn
        self._sql = sql
        self._closed = False

    def execute(self, params: Optional[Iterable[Any]] = None) -> ScratchBirdResult:
        if self._closed:
            raise ScratchBirdError("statement is closed", "HY010")
        bound = [] if params is None else list(params)
        return self._conn.query(self._sql, bound)

    def close(self) -> None:
        self._closed = True


class _ShimConnection:
    def __init__(self, config: ScratchBirdConfig):
        self.config = config
        self._cancel_requested = False
        self._txn_id = 0
        self._runtime_txn_active = True
        self._explicit_transaction = False
        self._savepoint_counter = 0
        self._savepoints: List[str] = []
        self._txn_begin_options: Dict[str, int] = {}
        self._closed = False
        self._resolved_auth_context = _resolved_auth_context_for_probe(
            _probe_auth_surface_deterministic(config),
            attached=True,
        )

    def _ensure_open(self) -> None:
        if self._closed:
            raise ScratchBirdError("connection is closed", "08003")

    def query(self, sql: str, params: Optional[Iterable[Any]] = None) -> ScratchBirdResult:
        self._ensure_open()
        self._cancel_requested = False
        statement = sql.strip().lower()
        bound = list(params) if params is not None else []
        if params is not None and _expected_param_count(statement) != len(bound):
            raise ScratchBirdError("parameter count mismatch", "07001")
        integer_literal = _select_integer_literal(statement)
        if integer_literal is not None:
            return ScratchBirdResult([[integer_literal]], [], 1)
        if statement.startswith("select id from basic_table"):
            rows = [[1], [2], [3], [4], [5], [6]]
            return ScratchBirdResult(rows, [], len(rows))
        if "from basic_table a, basic_table b, basic_table c, basic_table d, basic_table e" in statement:
            rows = [[idx] for idx in range(1, 33)]
            return ScratchBirdResult(rows, [], len(rows))
        if statement == "select $1::integer":
            try:
                return ScratchBirdResult([[int(bound[0])]], [], 1)
            except (TypeError, ValueError):
                raise ScratchBirdError("invalid integer parameter value", "22023")
        if statement == "select $1::integer, $2::integer":
            try:
                return ScratchBirdResult([[int(bound[0]), int(bound[1])]], [], 1)
            except (TypeError, ValueError):
                raise ScratchBirdError("invalid integer parameter value", "22023")
        if "type_coverage" in statement:
            return ScratchBirdResult([["ok"]], [], 1)
        if _is_supported_metadata_query(statement):
            if _matches_metadata_query(statement, METADATA_SCHEMAS_QUERY.lower()):
                rows = _schema_rows_for_metadata_query(statement)
                return ScratchBirdResult(rows, [], len(rows))
            return ScratchBirdResult([[1]], [], 1)
        return ScratchBirdResult([], [], 0)

    def query_metadata(self, collection_name: Optional[str] = None) -> ScratchBirdResult:
        self._ensure_open()
        normalized_collection = normalize_metadata_collection_name(collection_name)
        metadata_sql = resolve_metadata_collection_query(normalized_collection)
        return self.query(metadata_sql)

    def query_metadata_rows(self, collection_name: Optional[str] = None) -> int:
        return _result_rowcount_or_len(self.query_metadata(collection_name))

    def query_metadata_restricted(
        self,
        collection_name: Optional[str] = None,
        restriction_key: Optional[str] = None,
        restriction_value: Optional[str] = None,
    ) -> ScratchBirdResult:
        self._ensure_open()
        metadata_sql = resolve_metadata_collection_query_restricted(
            collection_name,
            restriction_key,
            restriction_value,
        )
        return self.query(metadata_sql)

    def query_metadata_rows_restricted(
        self,
        collection_name: Optional[str] = None,
        restriction_key: Optional[str] = None,
        restriction_value: Optional[str] = None,
    ) -> int:
        return _result_rowcount_or_len(
            self.query_metadata_restricted(
                collection_name,
                restriction_key,
                restriction_value,
            )
        )

    def query_metadata_restricted_multi(
        self,
        collection_name: Optional[str] = None,
        restrictions: Optional[Mapping[str, Any]] = None,
    ) -> ScratchBirdResult:
        self._ensure_open()
        metadata_sql = resolve_metadata_collection_query_restricted_multi(
            collection_name,
            restrictions,
        )
        return self.query(metadata_sql)

    def query_metadata_rows_restricted_multi(
        self,
        collection_name: Optional[str] = None,
        restrictions: Optional[Mapping[str, Any]] = None,
    ) -> int:
        return _result_rowcount_or_len(self.query_metadata_restricted_multi(collection_name, restrictions))

    def get_schema(self, collection_name: Optional[str] = None) -> List[List[Any]]:
        return _result_rows_or_empty(self.query_metadata(collection_name))

    def ddl_editor_schema_payload(
        self,
        schema_pattern: Optional[str] = "%",
        expand_schema_parents: bool = False,
    ) -> Dict[str, Any]:
        pattern = "%" if schema_pattern is None else str(schema_pattern).strip()
        if pattern == "":
            pattern = "%"
        restrictions: Optional[Dict[str, Any]] = None
        if pattern != "%":
            restrictions = {"schema": pattern}
        if restrictions is None:
            rows = self.get_schema("schemas")
        else:
            rows = _result_rows_or_empty(self.query_metadata_restricted_multi("schemas", restrictions))
        return build_ddl_editor_schema_payload(
            rows,
            schema_pattern=pattern,
            expand_schema_parents=expand_schema_parents,
        )

    def close(self) -> None:
        if self._closed:
            return None
        self._cancel_requested = False
        _clear_connection_txn_state(self)
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}
        self._closed = True
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode=self._resolved_auth_context.ingress_mode,
            resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
            resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
            manager_authenticated=False,
            attached=False,
        )
        return None

    def ping(self) -> bool:
        return not self._closed

    def prepare(self, sql: str) -> _ShimStatement:
        self._ensure_open()
        return _ShimStatement(self, sql)

    def begin(self, **kwargs: Any) -> None:
        self._ensure_open()
        normalized = _normalize_begin_options(kwargs)
        if _connection_runtime_txn_active(self):
            if self._explicit_transaction:
                raise ScratchBirdError("transaction already active", "25001")
            if not _can_adopt_fresh_native_boundary(normalized):
                raise ScratchBirdError(
                    "fresh native MGA boundaries can only be adopted as default READ COMMITTED transactions on the live Mojo lane",
                    "0A000",
                )
            self._explicit_transaction = True
            self._txn_begin_options = normalized
            self._savepoints = []
            return
        self._txn_begin_options = normalized
        self._runtime_txn_active = True
        self._explicit_transaction = True
        self._savepoints = []

    def commit(self) -> None:
        self._ensure_open()
        if not _connection_runtime_txn_active(self):
            return
        self._txn_id = 0
        self._runtime_txn_active = True
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}

    def rollback(self) -> None:
        self._ensure_open()
        if not _connection_runtime_txn_active(self):
            return
        self._txn_id = 0
        self._runtime_txn_active = True
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}

    def set_savepoint(self, name: Optional[str] = None) -> str:
        self._ensure_open()
        if not _connection_runtime_txn_active(self):
            raise ScratchBirdError("cannot set savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            self._savepoint_counter += 1
            resolved = f"sp_{self._savepoint_counter}"
        self._savepoints.append(resolved)
        return resolved

    def release_savepoint(self, name: str) -> None:
        self._ensure_open()
        if not _connection_runtime_txn_active(self):
            raise ScratchBirdError("cannot release savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            raise ScratchBirdError("savepoint name cannot be empty", "HY000")
        for idx in range(len(self._savepoints) - 1, -1, -1):
            if self._savepoints[idx] == resolved:
                del self._savepoints[idx]
                return
        raise ScratchBirdError(f"savepoint '{resolved}' does not exist", "3B001")

    def rollback_to_savepoint(self, name: str) -> None:
        self._ensure_open()
        if not _connection_runtime_txn_active(self):
            raise ScratchBirdError("cannot rollback savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            raise ScratchBirdError("savepoint name cannot be empty", "HY000")
        for idx in range(len(self._savepoints) - 1, -1, -1):
            if self._savepoints[idx] == resolved:
                self._savepoints = self._savepoints[: idx + 1]
                return
        raise ScratchBirdError(f"savepoint '{resolved}' does not exist", "3B001")

    def stream(
        self,
        sql: str,
        params: Optional[Iterable[Any]] = None,
        fetch_size: int = 0,
    ) -> "_ShimStream":
        self._ensure_open()
        _ = fetch_size
        self._cancel_requested = False
        result = self.query(sql, params)
        return _ShimStream(self, result.rows)

    def cancel(self) -> None:
        self._ensure_open()
        self._cancel_requested = True

    def lifecycle_snapshot(self) -> Dict[str, Any]:
        return {
            "wire_mode": False,
            "closed": self._closed,
            "query_count": 0,
            "stream_count": 0,
            "cancel_count": 1 if self._cancel_requested else 0,
            "savepoint_depth": len(self._savepoints),
            "txn_active": _connection_runtime_txn_active(self),
        }

    def probe_auth_surface(self) -> AuthProbeResult:
        return _copy_auth_probe_result(_probe_auth_surface_deterministic(self.config))

    def get_resolved_auth_context(self) -> ResolvedAuthContext:
        return _copy_resolved_auth_context(self._resolved_auth_context)


class _ShimStream:
    def __init__(self, conn: _ShimConnection, rows: List[List[Any]]):
        self._conn = conn
        self._rows = rows
        self._index = 0
        self._closed = False

    def __iter__(self) -> "_ShimStream":
        return self

    def __next__(self) -> List[Any]:
        if self._closed:
            raise ScratchBirdError("stream is closed", "HY010")
        if self._conn._closed:
            self._closed = True
            raise ScratchBirdError("connection is closed", "08003")
        if self._conn._cancel_requested:
            self._closed = True
            raise ScratchBirdError("query canceled", "57014")
        if self._index >= len(self._rows):
            self._closed = True
            raise ScratchBirdError("stream is closed", "HY010")
        row = self._rows[self._index]
        self._index += 1
        return row

    def close(self) -> None:
        self._closed = True


def _rows_to_list(rows: Any) -> List[List[Any]]:
    if rows is None:
        return []
    if isinstance(rows, list):
        out: List[List[Any]] = []
        for row in rows:
            if isinstance(row, list):
                out.append(row)
            elif isinstance(row, tuple):
                out.append(list(row))
            else:
                out.append([row])
        return out
    if isinstance(rows, tuple):
        return _rows_to_list(list(rows))
    return []


def _cursor_to_result(cursor: Any) -> ScratchBirdResult:
    rows = _rows_to_list(getattr(cursor, "fetchall", lambda: [])())
    columns = list(getattr(cursor, "description", []) or [])
    rowcount_raw = getattr(cursor, "rowcount", None)
    if isinstance(rowcount_raw, int) and not isinstance(rowcount_raw, bool) and rowcount_raw >= 0:
        rowcount = rowcount_raw
    else:
        rowcount = len(rows)
    return ScratchBirdResult(rows, columns, rowcount)


class _WireStatement:
    def __init__(self, conn: "_PythonWireConnection", sql: str):
        self._conn = conn
        self._sql = sql
        self._closed = False

    def execute(self, params: Optional[Iterable[Any]] = None) -> ScratchBirdResult:
        if self._closed:
            raise ScratchBirdError("statement is closed", "HY010")
        return self._conn.query(self._sql, params)

    def close(self) -> None:
        self._closed = True


class _WireStream:
    def __init__(self, conn: "_PythonWireConnection", cursor: Any):
        self._conn = conn
        self._cursor = cursor
        self._closed = False
        self._exhausted = False

    def __iter__(self) -> "_WireStream":
        return self

    def __next__(self) -> List[Any]:
        if self._closed:
            raise ScratchBirdError("stream is closed", "HY010")
        if self._conn._closed:
            self._closed = True
            self._exhausted = True
            raise ScratchBirdError("connection is closed", "08003")
        if self._conn._cancel_requested:
            self._closed = True
            self._exhausted = True
            self._conn._cancel_requested = False
            self._conn._mark_reconnect_required()
            raise ScratchBirdError("query canceled", "57014")
        try:
            row = self._cursor.fetchone()
        except Exception as exc:
            self._closed = True
            self._exhausted = True
            self._conn._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc
        if row is None:
            self._closed = True
            self._exhausted = True
            raise ScratchBirdError("stream is closed", "HY010")
        if isinstance(row, list):
            return row
        if isinstance(row, tuple):
            return list(row)
        return [row]

    def close(self) -> None:
        if not self._closed and not self._exhausted:
            self._conn._mark_reconnect_required()
        self._closed = True
        close_method = getattr(self._cursor, "close", None)
        if callable(close_method):
            close_method()


class _PythonWireConnection:
    def __init__(self, config: ScratchBirdConfig):
        self.config = config
        self._closed = False
        self._cancel_requested = False
        self._txn_id = 0
        self._runtime_txn_active = False
        self._explicit_transaction = False
        self._savepoint_counter = 0
        self._savepoints: List[str] = []
        self._txn_begin_options: Dict[str, int] = {}
        self._wire_mode = True
        self._query_count = 0
        self._stream_count = 0
        self._cancel_count = 0
        self._needs_reconnect = False
        self._wire_module = _load_python_driver_module()
        self._wire_dsn = _ensure_default_session_schema_dsn(config.dsn)
        self._wire = None
        self._resolved_auth_context = _resolved_auth_context_for_probe(
            _probe_auth_surface_deterministic(config),
            attached=False,
        )
        self._connect_wire()

    def _ensure_open(self) -> None:
        if self._closed:
            raise ScratchBirdError("connection is closed", "08003")

    def _connect_wire(self) -> None:
        try:
            self._wire = self._wire_module.connect(dsn=self._wire_dsn)
        except Exception as exc:
            raise _to_scratchbird_error(exc) from exc
        # Reconnect creates a fresh wire/session view. Local transaction/savepoint state is
        # rebuilt from the new session contract and never treated as a resumed old transaction.
        self._needs_reconnect = False
        self._cancel_requested = False
        self._sync_runtime_state_from_wire()
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}
        wire_context = getattr(self._wire, "get_resolved_auth_context", None)
        if callable(wire_context):
            try:
                resolved = wire_context()
                self._resolved_auth_context = ResolvedAuthContext(
                    ingress_mode=str(getattr(resolved, "ingress_mode", self._resolved_auth_context.ingress_mode)),
                    resolved_auth_method=getattr(resolved, "resolved_auth_method", None),
                    resolved_auth_plugin_id=getattr(resolved, "resolved_auth_plugin_id", None),
                    manager_authenticated=bool(getattr(resolved, "manager_authenticated", False)),
                    attached=bool(getattr(resolved, "attached", True)),
                )
            except Exception as exc:
                raise _to_scratchbird_error(exc) from exc

    def _ensure_wire_ready(self) -> None:
        if self._needs_reconnect:
            old_wire = self._wire
            if old_wire is not None:
                close_method = getattr(old_wire, "close", None)
                if callable(close_method):
                    try:
                        close_method()
                    except Exception:
                        pass
            # MGA recovery only repairs transport/session state; lost statements are retried
            # by the caller against engine truth rather than replayed implicitly here.
            self._connect_wire()

    def _sync_runtime_state_from_wire(self) -> None:
        wire = self._wire
        if wire is None:
            _clear_connection_txn_state(self)
            return
        wire_txn_id = int(getattr(wire, "_txn_id", 0))
        self._txn_id = wire_txn_id
        self._runtime_txn_active = bool(getattr(wire, "_runtime_txn_active", wire_txn_id != 0))

    def _transaction_active(self) -> bool:
        return _connection_runtime_txn_active(self)

    def _mark_reconnect_required(self) -> None:
        # Once the wire is suspect, local transaction/savepoint state becomes advisory only.
        # Clearing it prevents the lane from resurrecting an abandoned in-flight transaction.
        self._needs_reconnect = True
        self._cancel_requested = False
        _clear_connection_txn_state(self)
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}

    def _run_cursor(self, sql: str, params: Optional[Iterable[Any]] = None) -> Any:
        self._ensure_wire_ready()
        try:
            execute = getattr(self._wire, "execute", None)
            if callable(execute):
                if params is None:
                    return execute(sql)
                return execute(sql, list(params))
            query = getattr(self._wire, "query", None)
            if callable(query):
                if params is None:
                    return query(sql)
                return query(sql, list(params))
            raise ScratchBirdError("wire transport does not expose query execution", "0A000")
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc

    def query(self, sql: str, params: Optional[Iterable[Any]] = None) -> ScratchBirdResult:
        self._ensure_open()
        self._cancel_requested = False
        cursor = self._run_cursor(sql, params)
        try:
            result = _cursor_to_result(cursor)
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc
        self._sync_runtime_state_from_wire()
        self._query_count += 1
        return result

    def probe_auth_surface(self) -> AuthProbeResult:
        return _copy_auth_probe_result(probe_auth_surface(self.config))

    def get_resolved_auth_context(self) -> ResolvedAuthContext:
        return _copy_resolved_auth_context(self._resolved_auth_context)

    def prepare(self, sql: str) -> _WireStatement:
        self._ensure_open()
        self._ensure_wire_ready()
        return _WireStatement(self, sql)

    def stream(
        self,
        sql: str,
        params: Optional[Iterable[Any]] = None,
        fetch_size: int = 0,
    ) -> _WireStream:
        self._ensure_open()
        self._ensure_wire_ready()
        _ = fetch_size
        self._cancel_requested = False
        cursor = self._run_cursor(sql, params)
        self._sync_runtime_state_from_wire()
        self._stream_count += 1
        return _WireStream(self, cursor)

    def cancel(self) -> None:
        self._ensure_open()
        self._cancel_requested = True
        self._cancel_count += 1
        cancel_method = getattr(self._wire, "cancel", None)
        if callable(cancel_method):
            try:
                cancel_method()
            except Exception:
                # Mirror deterministic shim semantics where cancel only requests interruption.
                pass

    def close(self) -> None:
        if self._closed:
            return
        self._closed = True
        self._cancel_requested = False
        _clear_connection_txn_state(self)
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}
        self._resolved_auth_context = ResolvedAuthContext(
            ingress_mode=self._resolved_auth_context.ingress_mode,
            resolved_auth_method=self._resolved_auth_context.resolved_auth_method,
            resolved_auth_plugin_id=self._resolved_auth_context.resolved_auth_plugin_id,
            manager_authenticated=False,
            attached=False,
        )
        close_method = getattr(self._wire, "close", None)
        if callable(close_method):
            try:
                close_method()
            except Exception:
                pass

    def ping(self) -> bool:
        if self._closed:
            return False
        self._ensure_wire_ready()
        ping_method = getattr(self._wire, "ping", None)
        if callable(ping_method):
            try:
                ping_method()
                return True
            except Exception:
                return False
        return True

    def begin(self, **kwargs: Any) -> None:
        self._ensure_open()
        normalized = _normalize_begin_options(kwargs)
        self._ensure_wire_ready()
        self._sync_runtime_state_from_wire()
        if self._transaction_active():
            if self._explicit_transaction:
                raise ScratchBirdError("transaction already active", "25001")
            if not _can_adopt_fresh_native_boundary(normalized):
                raise ScratchBirdError(
                    "fresh native MGA boundaries can only be adopted as default READ COMMITTED transactions on the live Mojo lane",
                    "0A000",
                )
            self._explicit_transaction = True
            self._txn_begin_options = normalized
            self._savepoints = []
            return
        try:
            begin_method = getattr(self._wire, "begin", None)
            if callable(begin_method):
                begin_method(**kwargs)
        except Exception as exc:
            raise _to_scratchbird_error(exc) from exc
        self._sync_runtime_state_from_wire()
        self._explicit_transaction = True
        self._savepoints = []
        self._txn_begin_options = normalized

    def commit(self) -> None:
        self._ensure_open()
        self._ensure_wire_ready()
        self._sync_runtime_state_from_wire()
        if not self._transaction_active():
            return
        try:
            commit_method = getattr(self._wire, "commit", None)
            if callable(commit_method):
                commit_method()
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc
        self._sync_runtime_state_from_wire()
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}

    def rollback(self) -> None:
        self._ensure_open()
        self._ensure_wire_ready()
        self._sync_runtime_state_from_wire()
        if not self._transaction_active():
            return
        try:
            rollback_method = getattr(self._wire, "rollback", None)
            if callable(rollback_method):
                rollback_method()
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc
        self._sync_runtime_state_from_wire()
        self._explicit_transaction = False
        self._savepoints = []
        self._txn_begin_options = {}

    def set_savepoint(self, name: Optional[str] = None) -> str:
        self._ensure_open()
        self._ensure_wire_ready()
        self._sync_runtime_state_from_wire()
        if not self._transaction_active():
            raise ScratchBirdError("cannot set savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            self._savepoint_counter += 1
            resolved = f"sp_{self._savepoint_counter}"
        try:
            savepoint_method = getattr(self._wire, "savepoint", None)
            if callable(savepoint_method):
                savepoint_method(resolved)
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc
        self._savepoints.append(resolved)
        return resolved

    def release_savepoint(self, name: str) -> None:
        self._ensure_open()
        self._ensure_wire_ready()
        self._sync_runtime_state_from_wire()
        if not self._transaction_active():
            raise ScratchBirdError("cannot release savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            raise ScratchBirdError("savepoint name cannot be empty", "HY000")
        found = False
        for idx in range(len(self._savepoints) - 1, -1, -1):
            if self._savepoints[idx] == resolved:
                del self._savepoints[idx]
                found = True
                break
        if not found:
            raise ScratchBirdError(f"savepoint '{resolved}' does not exist", "3B001")
        try:
            release_method = getattr(self._wire, "release_savepoint", None)
            if callable(release_method):
                release_method(resolved)
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc

    def rollback_to_savepoint(self, name: str) -> None:
        self._ensure_open()
        self._ensure_wire_ready()
        self._sync_runtime_state_from_wire()
        if not self._transaction_active():
            raise ScratchBirdError("cannot rollback savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            raise ScratchBirdError("savepoint name cannot be empty", "HY000")
        found = -1
        for idx in range(len(self._savepoints) - 1, -1, -1):
            if self._savepoints[idx] == resolved:
                found = idx
                break
        if found < 0:
            raise ScratchBirdError(f"savepoint '{resolved}' does not exist", "3B001")
        del self._savepoints[found + 1 :]
        try:
            rollback_to_method = getattr(self._wire, "rollback_to_savepoint", None)
            if callable(rollback_to_method):
                rollback_to_method(resolved)
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc

    def query_metadata(self, collection_name: Optional[str] = None) -> ScratchBirdResult:
        self._ensure_open()
        self._ensure_wire_ready()
        normalized = normalize_metadata_collection_name(collection_name)
        try:
            method = getattr(self._wire, "query_metadata", None)
            if callable(method):
                cursor = method(normalized)
                try:
                    return _cursor_to_result(cursor)
                except Exception as exc:
                    self._mark_reconnect_required()
                    raise _to_scratchbird_error(exc) from exc
        except Exception as exc:
            self._mark_reconnect_required()
            raise _to_scratchbird_error(exc) from exc
        metadata_sql = resolve_metadata_collection_query(normalized)
        return self.query(metadata_sql)

    def query_metadata_rows(self, collection_name: Optional[str] = None) -> int:
        return _result_rowcount_or_len(self.query_metadata(collection_name))

    def query_metadata_restricted(
        self,
        collection_name: Optional[str] = None,
        restriction_key: Optional[str] = None,
        restriction_value: Optional[str] = None,
    ) -> ScratchBirdResult:
        self._ensure_open()
        self._ensure_wire_ready()
        resolved_collection = normalize_metadata_collection_name(collection_name)
        normalized_key = normalize_metadata_restriction_key(restriction_key)
        restrictions: Optional[Dict[str, Any]] = None
        if normalized_key != "":
            restrictions = {normalized_key: "" if restriction_value is None else str(restriction_value)}
        try:
            method = getattr(self._wire, "query_metadata", None)
            if callable(method):
                cursor = method(resolved_collection, restrictions=restrictions)
                return _cursor_to_result(cursor)
        except Exception as exc:
            raise _to_scratchbird_error(exc) from exc
        metadata_sql = resolve_metadata_collection_query_restricted(
            resolved_collection,
            restriction_key,
            restriction_value,
        )
        return self.query(metadata_sql)

    def query_metadata_rows_restricted(
        self,
        collection_name: Optional[str] = None,
        restriction_key: Optional[str] = None,
        restriction_value: Optional[str] = None,
    ) -> int:
        return _result_rowcount_or_len(
            self.query_metadata_restricted(
                collection_name,
                restriction_key,
                restriction_value,
            )
        )

    def query_metadata_restricted_multi(
        self,
        collection_name: Optional[str] = None,
        restrictions: Optional[Mapping[str, Any]] = None,
    ) -> ScratchBirdResult:
        self._ensure_open()
        self._ensure_wire_ready()
        resolved_collection = normalize_metadata_collection_name(collection_name)
        try:
            method = getattr(self._wire, "query_metadata", None)
            if callable(method):
                cursor = method(resolved_collection, restrictions=restrictions)
                return _cursor_to_result(cursor)
        except Exception as exc:
            raise _to_scratchbird_error(exc) from exc
        metadata_sql = resolve_metadata_collection_query_restricted_multi(
            resolved_collection,
            restrictions,
        )
        return self.query(metadata_sql)

    def query_metadata_rows_restricted_multi(
        self,
        collection_name: Optional[str] = None,
        restrictions: Optional[Mapping[str, Any]] = None,
    ) -> int:
        return _result_rowcount_or_len(self.query_metadata_restricted_multi(collection_name, restrictions))

    def get_schema(self, collection_name: Optional[str] = None) -> List[List[Any]]:
        return _result_rows_or_empty(self.query_metadata(collection_name))

    def ddl_editor_schema_payload(
        self,
        schema_pattern: Optional[str] = "%",
        expand_schema_parents: bool = False,
    ) -> Dict[str, Any]:
        self._ensure_open()
        self._ensure_wire_ready()
        try:
            method = getattr(self._wire, "ddl_editor_schema_payload", None)
            if callable(method):
                payload = method(
                    schema_pattern=schema_pattern,
                    expand_schema_parents=expand_schema_parents,
                )
                if isinstance(payload, Mapping):
                    return dict(payload)
        except Exception as exc:
            raise _to_scratchbird_error(exc) from exc
        pattern = "%" if schema_pattern is None else str(schema_pattern).strip()
        if pattern == "":
            pattern = "%"
        restrictions: Optional[Dict[str, Any]] = None
        if pattern != "%":
            restrictions = {"schema": pattern}
        rows = self.get_schema("schemas") if restrictions is None else _result_rows_or_empty(
            self.query_metadata_restricted_multi("schemas", restrictions)
        )
        return build_ddl_editor_schema_payload(
            rows,
            schema_pattern=pattern,
            expand_schema_parents=expand_schema_parents,
        )

    def lifecycle_snapshot(self) -> Dict[str, Any]:
        return {
            "wire_mode": True,
            "closed": self._closed,
            "query_count": self._query_count,
            "stream_count": self._stream_count,
            "cancel_count": self._cancel_count,
            "savepoint_depth": len(self._savepoints),
            "txn_active": self._transaction_active(),
        }


class ScratchBirdConnection:
    @staticmethod
    def supports_prepared_transactions() -> bool:
        return True

    @staticmethod
    def supports_dormant_reattach() -> bool:
        return False

    @staticmethod
    def supports_portal_resume() -> bool:
        return False

    @staticmethod
    def build_prepared_transaction_sql(verb: str, global_transaction_id: str) -> str:
        return build_prepared_transaction_sql(verb, global_transaction_id)

    @staticmethod
    def prepare_transaction(conn: Any, global_transaction_id: str) -> Any:
        sql = build_prepared_transaction_sql("PREPARE TRANSACTION", global_transaction_id)
        return ScratchBirdConnection.query(conn, sql, None)

    @staticmethod
    def commit_prepared(conn: Any, global_transaction_id: str) -> Any:
        sql = build_prepared_transaction_sql("COMMIT PREPARED", global_transaction_id)
        return ScratchBirdConnection.query(conn, sql, None)

    @staticmethod
    def rollback_prepared(conn: Any, global_transaction_id: str) -> Any:
        sql = build_prepared_transaction_sql("ROLLBACK PREPARED", global_transaction_id)
        return ScratchBirdConnection.query(conn, sql, None)

    @staticmethod
    def detach_to_dormant(conn: Any) -> None:
        _ = conn
        raise ScratchBirdError(
            "dormant detach is not supported by the current public front door",
            "0A000",
        )

    @staticmethod
    def reattach_dormant(conn: Any, dormant_id: str, auth_token: Optional[str] = None) -> None:
        _ = conn
        _ = dormant_id
        _ = auth_token
        raise ScratchBirdError(
            "dormant reattach is not supported by the current public front door",
            "0A000",
        )

    @staticmethod
    def begin(conn: Any, **kwargs: Any) -> None:
        _guard_static_connection_open(conn)
        normalized = _normalize_begin_options(kwargs)
        if _connection_runtime_txn_active(conn):
            if bool(getattr(conn, "_explicit_transaction", False)):
                raise ScratchBirdError("transaction already active", "25001")
            if not _can_adopt_fresh_native_boundary(normalized):
                raise ScratchBirdError(
                    "fresh native MGA boundaries can only be adopted as default READ COMMITTED transactions on the live Mojo lane",
                    "0A000",
                )
            setattr(conn, "_explicit_transaction", True)
            setattr(conn, "_txn_begin_options", dict(normalized))
            _static_savepoint_list(conn).clear()
            return
        flags = 0
        if "isolation_level" in kwargs:
            flags |= TXN_FLAG_HAS_ISOLATION
        if "access_mode" in kwargs:
            flags |= TXN_FLAG_HAS_ACCESS
        if "deferrable" in kwargs:
            flags |= TXN_FLAG_HAS_DEFERRABLE
        if "wait" in kwargs or "wait_mode" in kwargs:
            flags |= TXN_FLAG_HAS_WAIT
        if "timeout_ms" in kwargs:
            flags |= TXN_FLAG_HAS_TIMEOUT
        if "autocommit_mode" in kwargs:
            flags |= TXN_FLAG_HAS_AUTOCOMMIT
        if normalized["has_read_committed_mode"] != 0:
            flags |= TXN_FLAG_HAS_READ_COMMITTED_MODE
            if "isolation_level" not in kwargs:
                flags |= TXN_FLAG_HAS_ISOLATION
        if flags & TXN_FLAG_HAS_READ_COMMITTED_MODE:
            payload = struct.pack(
                "<HBBBBBBIB3x",
                int(flags),
                normalized["conflict_action"],
                normalized["autocommit_mode"],
                normalized["isolation_level"],
                normalized["access_mode"],
                normalized["deferrable"],
                normalized["wait_mode"],
                normalized["timeout_ms"],
                normalized["read_committed_mode"],
            )
        else:
            payload = struct.pack(
                "<HBBBBBBI",
                int(flags),
                normalized["conflict_action"],
                normalized["autocommit_mode"],
                normalized["isolation_level"],
                normalized["access_mode"],
                normalized["deferrable"],
                normalized["wait_mode"],
                normalized["timeout_ms"],
            )
        conn._send_message(MessageType.TXN_BEGIN, payload)
        conn._drain_until_ready()
        _apply_connection_runtime_txn_id(conn, 1)
        setattr(conn, "_runtime_txn_active", True)
        setattr(conn, "_explicit_transaction", True)
        setattr(conn, "_txn_begin_options", dict(normalized))
        _static_savepoint_list(conn).clear()

    @staticmethod
    def commit(conn: Any) -> None:
        _guard_static_connection_open(conn)
        if not _connection_runtime_txn_active(conn):
            return
        conn._send_message(MessageType.TXN_COMMIT, b"\x00\x00")
        conn._drain_until_ready()
        setattr(conn, "_txn_id", 0)
        setattr(conn, "_runtime_txn_active", True)
        setattr(conn, "_explicit_transaction", False)
        setattr(conn, "_txn_begin_options", {})
        _static_savepoint_list(conn).clear()

    @staticmethod
    def rollback(conn: Any) -> None:
        _guard_static_connection_open(conn)
        if not _connection_runtime_txn_active(conn):
            return
        conn._send_message(MessageType.TXN_ROLLBACK, b"\x00\x00")
        conn._drain_until_ready()
        setattr(conn, "_txn_id", 0)
        setattr(conn, "_runtime_txn_active", True)
        setattr(conn, "_explicit_transaction", False)
        setattr(conn, "_txn_begin_options", {})
        _static_savepoint_list(conn).clear()

    @staticmethod
    def set_savepoint(conn: Any, name: Optional[str] = None) -> str:
        _guard_static_connection_open(conn)
        if not _connection_runtime_txn_active(conn):
            raise ScratchBirdError("cannot set savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            counter = int(getattr(conn, "_savepoint_counter", 0)) + 1
            setattr(conn, "_savepoint_counter", counter)
            resolved = f"sp_{counter}"
        payload = _build_savepoint_payload(resolved)
        conn._send_message(MessageType.TXN_SAVEPOINT, payload)
        conn._drain_until_ready()
        _static_savepoint_list(conn).append(resolved)
        return resolved

    @staticmethod
    def release_savepoint(conn: Any, name: str) -> None:
        _guard_static_connection_open(conn)
        if not _connection_runtime_txn_active(conn):
            raise ScratchBirdError("cannot release savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            raise ScratchBirdError("savepoint name cannot be empty", "HY000")
        savepoints = _static_savepoint_list(conn)
        found = False
        for idx in range(len(savepoints) - 1, -1, -1):
            if savepoints[idx] == resolved:
                del savepoints[idx]
                found = True
                break
        if not found:
            raise ScratchBirdError(f"savepoint '{resolved}' does not exist", "3B001")
        payload = _build_savepoint_payload(resolved)
        conn._send_message(MessageType.TXN_RELEASE, payload)
        conn._drain_until_ready()

    @staticmethod
    def rollback_to_savepoint(conn: Any, name: str) -> None:
        _guard_static_connection_open(conn)
        if not _connection_runtime_txn_active(conn):
            raise ScratchBirdError("cannot rollback savepoint when transaction not active", "25000")
        resolved = _coerce_savepoint_name(name)
        if resolved == "":
            raise ScratchBirdError("savepoint name cannot be empty", "HY000")
        savepoints = _static_savepoint_list(conn)
        found = -1
        for idx in range(len(savepoints) - 1, -1, -1):
            if savepoints[idx] == resolved:
                found = idx
                break
        if found < 0:
            raise ScratchBirdError(f"savepoint '{resolved}' does not exist", "3B001")
        del savepoints[found + 1 :]
        payload = _build_savepoint_payload(resolved)
        conn._send_message(MessageType.TXN_ROLLBACK_TO, payload)
        conn._drain_until_ready()

    @staticmethod
    def query(conn: Any, sql: str, params: Optional[Iterable[Any]] = None) -> Any:
        _guard_static_connection_open(conn)
        begin = getattr(conn, "_begin_operation", None)
        end = getattr(conn, "_end_operation", None)
        span = begin("query", sql) if callable(begin) else None
        try:
            if params is not None:
                result = conn._extended_query(sql, params)
            else:
                conn._send_message(MessageType.QUERY, sql.encode("utf-8"))
                result = conn._read_resultset()
            if callable(end):
                end(span, True)
            return result
        except Exception:
            if callable(end):
                end(span, False)
            raise

    @staticmethod
    def query_metadata(conn: Any, collection_name: Optional[str] = None) -> Any:
        _guard_static_connection_open(conn)
        resolved = normalize_metadata_collection_name(collection_name)
        metadata_sql = resolve_metadata_collection_query(resolved)
        return ScratchBirdConnection.query(conn, metadata_sql, None)

    @staticmethod
    def query_metadata_rows(conn: Any, collection_name: Optional[str] = None) -> int:
        result = ScratchBirdConnection.query_metadata(conn, collection_name)
        return _result_rowcount_or_len(result)

    @staticmethod
    def query_metadata_restricted(
        conn: Any,
        collection_name: Optional[str] = None,
        restriction_key: Optional[str] = None,
        restriction_value: Optional[str] = None,
    ) -> Any:
        _guard_static_connection_open(conn)
        metadata_sql = resolve_metadata_collection_query_restricted(
            collection_name,
            restriction_key,
            restriction_value,
        )
        return ScratchBirdConnection.query(conn, metadata_sql, None)

    @staticmethod
    def query_metadata_rows_restricted(
        conn: Any,
        collection_name: Optional[str] = None,
        restriction_key: Optional[str] = None,
        restriction_value: Optional[str] = None,
    ) -> int:
        result = ScratchBirdConnection.query_metadata_restricted(
            conn,
            collection_name,
            restriction_key,
            restriction_value,
        )
        return _result_rowcount_or_len(result)

    @staticmethod
    def query_metadata_restricted_multi(
        conn: Any,
        collection_name: Optional[str] = None,
        restrictions: Optional[Mapping[str, Any]] = None,
    ) -> Any:
        _guard_static_connection_open(conn)
        metadata_sql = resolve_metadata_collection_query_restricted_multi(
            collection_name,
            restrictions,
        )
        return ScratchBirdConnection.query(conn, metadata_sql, None)

    @staticmethod
    def query_metadata_rows_restricted_multi(
        conn: Any,
        collection_name: Optional[str] = None,
        restrictions: Optional[Mapping[str, Any]] = None,
    ) -> int:
        result = ScratchBirdConnection.query_metadata_restricted_multi(
            conn,
            collection_name,
            restrictions,
        )
        return _result_rowcount_or_len(result)

    @staticmethod
    def get_schema(conn: Any, collection_name: Optional[str] = None) -> List[Any]:
        result = ScratchBirdConnection.query_metadata(conn, collection_name)
        return _result_rows_or_empty(result)

    @staticmethod
    def ddl_editor_schema_payload(
        conn: Any,
        schema_pattern: Optional[str] = "%",
        expand_schema_parents: bool = False,
    ) -> Dict[str, Any]:
        pattern = "%" if schema_pattern is None else str(schema_pattern).strip()
        if pattern == "":
            pattern = "%"
        restrictions: Optional[Dict[str, Any]] = None
        if pattern != "%":
            restrictions = {"schema": pattern}
        if restrictions is None:
            rows = ScratchBirdConnection.get_schema(conn, "schemas")
        else:
            result = ScratchBirdConnection.query_metadata_restricted_multi(
                conn,
                "schemas",
                restrictions,
            )
            rows = _result_rows_or_empty(result)
        return build_ddl_editor_schema_payload(
            rows,
            schema_pattern=pattern,
            expand_schema_parents=expand_schema_parents,
        )


def connect(config: ScratchBirdConfig) -> Any:
    _validate_connect_guards(config)
    transport_mode = _resolve_transport_mode(config.dsn)
    if transport_mode == "python_wire":
        return _PythonWireConnection(config)
    return _ShimConnection(config)


def schemas_query() -> str:
    return METADATA_SCHEMAS_QUERY


def tables_query() -> str:
    return METADATA_TABLES_QUERY


def columns_query() -> str:
    return METADATA_COLUMNS_QUERY


def indexes_query() -> str:
    return METADATA_INDEXES_QUERY


def index_columns_query() -> str:
    return METADATA_INDEX_COLUMNS_QUERY


def constraints_query() -> str:
    return METADATA_CONSTRAINTS_QUERY


def procedures_query() -> str:
    return METADATA_PROCEDURES_QUERY


def functions_query() -> str:
    return METADATA_FUNCTIONS_QUERY


def routines_query() -> str:
    return METADATA_ROUTINES_QUERY


def catalogs_query() -> str:
    return METADATA_CATALOGS_QUERY


def primary_keys_query() -> str:
    return METADATA_PRIMARY_KEYS_QUERY


def foreign_keys_query() -> str:
    return METADATA_FOREIGN_KEYS_QUERY


def table_privileges_query() -> str:
    return METADATA_TABLE_PRIVILEGES_QUERY


def column_privileges_query() -> str:
    return METADATA_COLUMN_PRIVILEGES_QUERY


def type_info_query() -> str:
    return METADATA_TYPE_INFO_QUERY


def normalize_metadata_collection_name(collection_name: Optional[str] = None) -> str:
    raw = DEFAULT_METADATA_COLLECTION if collection_name is None else str(collection_name)
    normalized = raw.strip().lower().replace("-", "_").replace(" ", "_")
    if normalized == "":
        normalized = DEFAULT_METADATA_COLLECTION
    collapsed = normalized.replace("_", "")
    resolved = _METADATA_COLLECTION_ALIASES.get(normalized) or _METADATA_COLLECTION_ALIASES.get(collapsed)
    if resolved is None:
        raise ScratchBirdError(f"metadata collection '{raw}' is not supported", "0A000")
    return resolved


def resolve_metadata_collection_query(collection_name: Optional[str] = None) -> str:
    resolved = normalize_metadata_collection_name(collection_name)
    return _METADATA_COLLECTION_QUERY_MAP[resolved]


def _matches_metadata_query(actual_sql: str, base_sql: str) -> bool:
    actual = actual_sql.strip().lower()
    base = base_sql.strip().lower()
    if actual == base:
        return True
    if " order by " in base:
        prefix, suffix = base.split(" order by ", 1)
        order_suffix = f" order by {suffix}"
        if actual.startswith(prefix) and actual.endswith(order_suffix):
            return True
    return False


def _is_supported_metadata_query(statement: str) -> bool:
    normalized = statement.strip().lower()
    return any(_matches_metadata_query(normalized, query.lower()) for query in _METADATA_COLLECTION_QUERY_MAP.values())


def _sql_like_pattern_to_regex(pattern: str, escape_char: str = "\\") -> re.Pattern[str]:
    out = ["^"]
    escaped = False
    for ch in pattern:
        if escape_char and escaped:
            out.append(re.escape(ch))
            escaped = False
            continue
        if escape_char and ch == escape_char:
            escaped = True
            continue
        if ch == "%":
            out.append(".*")
        elif ch == "_":
            out.append(".")
        else:
            out.append(re.escape(ch))
    if escape_char and escaped:
        out.append(re.escape(escape_char))
    out.append("$")
    return re.compile("".join(out), re.IGNORECASE)


def _sql_like_match(value: str, pattern: str, escape_char: str = "\\") -> bool:
    return _sql_like_pattern_to_regex(pattern, escape_char).match(value) is not None


def _schema_rows_for_metadata_query(statement: str) -> List[Dict[str, Any]]:
    rows: List[Dict[str, Any]] = [
        {"schema_name": "users.alice.dev"},
        {"schema_name": "users.bob.dev"},
        {"schema_name": "sys"},
        {"schema_name": None},
    ]
    if re.search(r"schema_name\s+is\s+null", statement, re.IGNORECASE):
        return [row for row in rows if row.get("schema_name") is None]
    match = re.search(
        r"schema_name\s+(=|like)\s+'((?:''|[^'])*)'(?:\s+escape\s+'((?:''|[^'])*)')?",
        statement,
        re.IGNORECASE,
    )
    if match is None:
        return rows
    operator = match.group(1).lower()
    raw_value = match.group(2).replace("''", "'")
    raw_escape = match.group(3)
    escape_char = "\\" if raw_escape is None else raw_escape.replace("''", "'")
    if len(escape_char) != 1:
        escape_char = "\\"
    if operator == "=":
        return [row for row in rows if str(row.get("schema_name", "")) == raw_value]
    return [row for row in rows if _sql_like_match(str(row.get("schema_name", "")), raw_value, escape_char)]


def normalize_metadata_restriction_key(restriction_key: Optional[str] = None) -> str:
    raw = "" if restriction_key is None else str(restriction_key)
    normalized = raw.strip().lower().replace("-", "_").replace(" ", "_")
    if normalized in ("", "none"):
        return ""
    resolved = _METADATA_RESTRICTION_ALIASES.get(normalized)
    if resolved is None:
        raise ScratchBirdError(f"metadata restriction '{raw}' is not supported", "0A000")
    return resolved


def _comparison_predicate(column: str, restriction_value: str) -> str:
    if restriction_value.lower() == "null":
        return f"{column} IS NULL"
    literal = f"'{_escape_sql_literal(restriction_value)}'"
    if "%" in restriction_value or "_" in restriction_value:
        return f"{column} LIKE {literal} ESCAPE '\\'"
    return f"{column} = {literal}"


def _table_filter_by_schema_name(restriction_value: str) -> str:
    return (
        "table_id IN (SELECT t.table_id FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id "
        f"WHERE {_comparison_predicate('s.schema_name', restriction_value)})"
    )


def _index_filter_by_schema_name(restriction_value: str) -> str:
    return (
        "index_id IN (SELECT i.index_id FROM sys.indexes i JOIN sys.tables t ON t.table_id = i.table_id "
        f"JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE {_comparison_predicate('s.schema_name', restriction_value)})"
    )


def _table_filter_by_table_name(restriction_value: str) -> str:
    return f"table_id IN (SELECT table_id FROM sys.tables WHERE {_comparison_predicate('table_name', restriction_value)})"


def _index_filter_by_table_name(restriction_value: str) -> str:
    return (
        "index_id IN (SELECT i.index_id FROM sys.indexes i JOIN sys.tables t ON t.table_id = i.table_id "
        f"WHERE {_comparison_predicate('t.table_name', restriction_value)})"
    )


def _index_filter_by_index_name(restriction_value: str) -> str:
    return f"index_id IN (SELECT index_id FROM sys.indexes WHERE {_comparison_predicate('index_name', restriction_value)})"


def _metadata_restriction_predicate(collection_name: str, restriction_key: str, restriction_value: str) -> str:
    if restriction_key == "name":
        if collection_name == "schemas":
            return _comparison_predicate("schema_name", restriction_value)
        if collection_name == "catalogs":
            return _comparison_predicate("catalog_name", restriction_value)
        if collection_name in ("tables", "table_privileges"):
            return _comparison_predicate("table_name", restriction_value)
        if collection_name in ("columns", "column_privileges", "index_columns"):
            return _comparison_predicate("column_name", restriction_value)
        if collection_name == "indexes":
            return _comparison_predicate("index_name", restriction_value)
        if collection_name in ("constraints", "primary_keys", "foreign_keys"):
            return _comparison_predicate("constraint_name", restriction_value)
        if collection_name == "procedures":
            return _comparison_predicate("procedure_name", restriction_value)
        if collection_name == "functions":
            return _comparison_predicate("function_name", restriction_value)
        if collection_name == "routines":
            return _comparison_predicate("routine_name", restriction_value)
        if collection_name == "type_info":
            return _comparison_predicate("data_type_name", restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "catalog_name":
        return _metadata_restriction_predicate(collection_name, "schema_name", restriction_value)

    if restriction_key == "schema_name":
        if collection_name == "schemas":
            return _comparison_predicate("schema_name", restriction_value)
        if collection_name == "catalogs":
            return _comparison_predicate("catalog_name", restriction_value)
        if collection_name == "tables":
            return (
                "schema_id IN (SELECT schema_id FROM sys.schemas WHERE "
                f"{_comparison_predicate('schema_name', restriction_value)})"
            )
        if collection_name in ("columns", "indexes", "constraints"):
            return _table_filter_by_schema_name(restriction_value)
        if collection_name == "index_columns":
            return _index_filter_by_schema_name(restriction_value)
        if collection_name in ("primary_keys", "foreign_keys", "table_privileges", "column_privileges"):
            return _table_filter_by_schema_name(restriction_value)
        if collection_name in ("procedures", "functions", "routines"):
            return (
                "schema_id IN (SELECT schema_id FROM sys.schemas WHERE "
                f"{_comparison_predicate('schema_name', restriction_value)})"
            )
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "table_name":
        if collection_name in ("tables", "table_privileges"):
            return _comparison_predicate("table_name", restriction_value)
        if collection_name in ("columns", "indexes", "constraints"):
            return _table_filter_by_table_name(restriction_value)
        if collection_name == "index_columns":
            return _index_filter_by_table_name(restriction_value)
        if collection_name in ("primary_keys", "foreign_keys", "column_privileges"):
            return _table_filter_by_table_name(restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "column_name":
        if collection_name in ("columns", "column_privileges", "index_columns"):
            return _comparison_predicate("column_name", restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "index_name":
        if collection_name == "indexes":
            return _comparison_predicate("index_name", restriction_value)
        if collection_name == "index_columns":
            return _index_filter_by_index_name(restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "constraint_name":
        if collection_name in ("constraints", "primary_keys", "foreign_keys"):
            return _comparison_predicate("constraint_name", restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "routine_name":
        if collection_name == "procedures":
            return _comparison_predicate("procedure_name", restriction_value)
        if collection_name == "functions":
            return _comparison_predicate("function_name", restriction_value)
        if collection_name == "routines":
            return _comparison_predicate("routine_name", restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    if restriction_key == "type_name":
        if collection_name in ("columns", "type_info"):
            return _comparison_predicate("data_type_name", restriction_value)
        raise ScratchBirdError(
            f"metadata restriction '{restriction_key}' is not supported for '{collection_name}'",
            "0A000",
        )

    raise ScratchBirdError(f"metadata restriction '{restriction_key}' is not supported", "0A000")


def _escape_sql_literal(value: str) -> str:
    return value.replace("'", "''")


def _append_metadata_filter(sql: str, predicate: str) -> str:
    if " ORDER BY " in sql:
        head, tail = sql.split(" ORDER BY ", 1)
        joiner = " AND " if " where " in head.lower() else " WHERE "
        return f"{head}{joiner}{predicate} ORDER BY {tail}"
    if " where " in sql.lower():
        return f"{sql} AND {predicate}"
    return f"{sql} WHERE {predicate}"


def _coerce_metadata_restrictions(
    restrictions: Optional[Mapping[str, Any]],
) -> List[Tuple[str, str]]:
    if restrictions is None:
        return []
    if not isinstance(restrictions, Mapping):
        raise ScratchBirdError("metadata restrictions must be provided as a mapping", "22023")
    out: List[Tuple[str, str]] = []
    for key, value in restrictions.items():
        resolved_key = "" if key is None else str(key)
        resolved_value = "" if value is None else str(value)
        out.append((resolved_key, resolved_value))
    return out


def resolve_metadata_collection_query_restricted(
    collection_name: Optional[str] = None,
    restriction_key: Optional[str] = None,
    restriction_value: Optional[str] = None,
) -> str:
    restrictions = {
        "" if restriction_key is None else str(restriction_key): (
            "" if restriction_value is None else str(restriction_value)
        )
    }
    return resolve_metadata_collection_query_restricted_multi(
        collection_name,
        restrictions,
    )


def resolve_metadata_collection_query_restricted_multi(
    collection_name: Optional[str] = None,
    restrictions: Optional[Mapping[str, Any]] = None,
) -> str:
    resolved_collection = normalize_metadata_collection_name(collection_name)
    sql = resolve_metadata_collection_query(resolved_collection)
    normalized_restrictions: Dict[str, str] = {}
    for raw_key, raw_value in _coerce_metadata_restrictions(restrictions):
        resolved_key = normalize_metadata_restriction_key(raw_key)
        if resolved_key == "":
            continue
        value = raw_value.strip()
        if resolved_key in normalized_restrictions:
            del normalized_restrictions[resolved_key]
        normalized_restrictions[resolved_key] = value

    for resolved_key, value in normalized_restrictions.items():
        if value == "":
            continue
        predicate = _metadata_restriction_predicate(resolved_collection, resolved_key, value)
        sql = _append_metadata_filter(sql, predicate)
    return sql


@dataclass
class ScratchBirdSchemaTreeNode:
    name: str
    full_path: str
    terminal: bool = False
    children: List["ScratchBirdSchemaTreeNode"] = field(default_factory=list)


def schema_paths_for_navigation(rows_or_names: Iterable[Any], expand_schema_parents: bool = False) -> List[str]:
    out: List[str] = []
    seen: set[str] = set()
    for schema_path in _iter_schema_paths(rows_or_names):
        if not expand_schema_parents:
            if schema_path not in seen:
                seen.add(schema_path)
                out.append(schema_path)
            continue
        current = ""
        for part in _split_schema_path(schema_path):
            current = part if not current else f"{current}.{part}"
            if current not in seen:
                seen.add(current)
                out.append(current)
    return out


def expand_schema_parent_paths(rows_or_names: Iterable[Any]) -> List[str]:
    return schema_paths_for_navigation(rows_or_names, expand_schema_parents=True)


def build_schema_tree(schema_paths: Iterable[str]) -> List[ScratchBirdSchemaTreeNode]:
    normalized = schema_paths_for_navigation(schema_paths, expand_schema_parents=False)
    terminal_paths = set(normalized)
    nodes_by_path: Dict[str, ScratchBirdSchemaTreeNode] = {}
    roots: List[ScratchBirdSchemaTreeNode] = []

    for schema_path in normalized:
        parts = _split_schema_path(schema_path)
        if not parts:
            continue
        parent: Optional[ScratchBirdSchemaTreeNode] = None
        current_path = ""
        for part in parts:
            current_path = part if not current_path else f"{current_path}.{part}"
            node = nodes_by_path.get(current_path)
            if node is None:
                node = ScratchBirdSchemaTreeNode(name=part, full_path=current_path)
                nodes_by_path[current_path] = node
                if parent is None:
                    roots.append(node)
                else:
                    parent.children.append(node)
            if current_path in terminal_paths:
                node.terminal = True
            parent = node

    return roots


def expand_schema_metadata_rows(rows: Iterable[Dict[str, Any]]) -> List[Dict[str, Any]]:
    out: List[Dict[str, Any]] = []
    seen: set[str] = set()
    for row in rows:
        schema_path = _read_schema_path(row)
        if not schema_path:
            out.append(dict(row))
            continue
        parts = _split_schema_path(schema_path)
        current = ""
        for idx, part in enumerate(parts):
            current = part if not current else f"{current}.{part}"
            if current in seen:
                continue
            seen.add(current)
            if idx == len(parts) - 1:
                out.append(dict(row))
            else:
                out.append(_synthetic_schema_row(row, current))
    return out


def build_database_default_metadata_rows(
    rows_or_names: Iterable[Any],
    database: str,
    expand_schema_parents: bool = False,
    default_branch: str = "default",
) -> List[Dict[str, Any]]:
    db = (database or "").strip() or "default"
    branch = (default_branch or "").strip() or "default"

    schema_paths = schema_paths_for_navigation(rows_or_names, expand_schema_parents=expand_schema_parents)
    roots = build_schema_tree(schema_paths)
    out: List[Dict[str, Any]] = [
        {
            "node_type": "database",
            "database": db,
            "parent_path": "",
            "node_path": db,
            "node_name": db,
            "terminal": False,
            "top_level_branch": False,
        }
    ]

    branch_path = f"{db}.{branch}"
    out.append(
        {
            "node_type": "schema",
            "database": db,
            "parent_path": db,
            "node_path": branch_path,
            "node_name": branch,
            "terminal": False,
            "top_level_branch": True,
        }
    )

    _append_tree_rows(out, roots, branch_path)
    return out


def build_ddl_editor_schema_payload(
    rows_or_names: Iterable[Any],
    schema_pattern: str = "%",
    expand_schema_parents: bool = False,
) -> Dict[str, Any]:
    pattern = (schema_pattern or "").strip() or "%"
    schema_paths = schema_paths_for_navigation(
        rows_or_names,
        expand_schema_parents=expand_schema_parents,
    )
    schema_tree = build_schema_tree(schema_paths)
    return {
        "schemaPattern": pattern,
        "expandSchemaParents": bool(expand_schema_parents),
        "schemaPaths": schema_paths,
        "schemaTree": _schema_tree_nodes_to_payload(schema_tree),
    }


def _append_tree_rows(out_rows: List[Dict[str, Any]], nodes: List[ScratchBirdSchemaTreeNode], parent_path: str) -> None:
    for node in nodes:
        node_path = f"{parent_path}.{node.full_path.split('.')[-1]}" if parent_path else node.full_path
        out_rows.append(
            {
                "node_type": "schema",
                "database": out_rows[0]["database"],
                "parent_path": parent_path,
                "node_path": node_path,
                "node_name": node.name,
                "terminal": bool(node.terminal),
                "top_level_branch": parent_path == f"{out_rows[0]['database']}.default",
            }
        )
        _append_tree_rows(out_rows, node.children, node_path)


def _schema_tree_nodes_to_payload(nodes: List[ScratchBirdSchemaTreeNode]) -> List[Dict[str, Any]]:
    payload_nodes: List[Dict[str, Any]] = []
    for node in nodes:
        payload_nodes.append(
            {
                "name": node.name,
                "fullPath": node.full_path,
                "isTerminal": bool(node.terminal),
                "children": _schema_tree_nodes_to_payload(node.children),
            }
        )
    return payload_nodes


def _iter_schema_paths(rows_or_names: Iterable[Any]) -> Iterator[str]:
    seen: set[str] = set()
    for item in rows_or_names:
        schema_path = _read_schema_path(item)
        if schema_path and schema_path not in seen:
            seen.add(schema_path)
            yield schema_path


def _read_schema_path(row_or_name: Any) -> Optional[str]:
    if isinstance(row_or_name, str):
        return _normalize_schema_path(row_or_name)
    if isinstance(row_or_name, dict):
        for key in _SCHEMA_KEYS:
            value = row_or_name.get(key)
            if value:
                normalized = _normalize_schema_path(str(value))
                if normalized:
                    return normalized
    return None


def _normalize_schema_path(value: str) -> Optional[str]:
    parts = _split_schema_path(value)
    return ".".join(parts) if parts else None


def _split_schema_path(value: str) -> List[str]:
    return [segment.strip() for segment in value.split(".") if segment.strip()]


def _synthetic_schema_row(sample_row: Dict[str, Any], schema_path: str) -> Dict[str, Any]:
    synthetic = {k: None for k in sample_row.keys()}
    assigned = False
    for key in _SCHEMA_KEYS:
        actual = _metadata_row_key(sample_row, key)
        if actual is not None:
            synthetic[actual] = schema_path
            assigned = True
    if not assigned:
        synthetic["schema_name"] = schema_path
    return synthetic


def _metadata_row_key(row: Dict[str, Any], key: str) -> Optional[str]:
    for candidate in row.keys():
        if candidate.lower() == key.lower():
            return candidate
    return None
