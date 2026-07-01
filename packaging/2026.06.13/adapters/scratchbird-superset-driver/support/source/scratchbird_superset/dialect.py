# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

"""SQLAlchemy dialect for ScratchBird (SBWP v1.1)."""

from __future__ import annotations

from typing import Any, Dict, Iterable, List, Optional

from sqlalchemy import text, types
from sqlalchemy.engine.default import DefaultDialect


_CONNECT_ARG_ALIASES = {
    "application_name": "application_name",
    "applicationname": "application_name",
    "currentschema": "schema",
    "schema": "schema",
    "searchpath": "search_path",
    "search_path": "search_path",
    "ssl": "sslmode",
    "sslmode": "sslmode",
    "sslrootcert": "sslrootcert",
    "sslcert": "sslcert",
    "sslkey": "sslkey",
    "sslpassword": "sslpassword",
    "connecttimeout": "connect_timeout",
    "connect_timeout": "connect_timeout",
    "sockettimeout": "socket_timeout",
    "socket_timeout": "socket_timeout",
    "binarytransfer": "binary_transfer",
    "binary_transfer": "binary_transfer",
    "fetchsize": "fetch_size",
    "fetch_size": "fetch_size",
    "connectclientflags": "connect_client_flags",
    "connect_client_flags": "connect_client_flags",
    "frontdoormode": "front_door_mode",
    "front_door_mode": "front_door_mode",
    "managerauthtoken": "manager_auth_token",
    "manager_auth_token": "manager_auth_token",
    "managerusername": "manager_username",
    "manager_username": "manager_username",
    "managerdatabase": "manager_database",
    "manager_database": "manager_database",
    "managerconnectionprofile": "manager_connection_profile",
    "manager_connection_profile": "manager_connection_profile",
    "managerclientintent": "manager_client_intent",
    "manager_client_intent": "manager_client_intent",
    "managerclientflags": "manager_client_flags",
    "manager_client_flags": "manager_client_flags",
    "managerauthfastpath": "manager_auth_fast_path",
    "manager_auth_fast_path": "manager_auth_fast_path",
    "authtoken": "auth_token",
    "auth_token": "auth_token",
    "authmethodid": "auth_method_id",
    "auth_method_id": "auth_method_id",
    "authmethodpayload": "auth_method_payload",
    "auth_method_payload": "auth_method_payload",
    "authpayloadjson": "auth_payload_json",
    "auth_payload_json": "auth_payload_json",
    "authpayloadb64": "auth_payload_b64",
    "auth_payload_b64": "auth_payload_b64",
    "authproviderprofile": "auth_provider_profile",
    "auth_provider_profile": "auth_provider_profile",
    "authrequiredmethods": "auth_required_methods",
    "auth_required_methods": "auth_required_methods",
    "authforbiddenmethods": "auth_forbidden_methods",
    "auth_forbidden_methods": "auth_forbidden_methods",
    "authrequirechannelbinding": "auth_require_channel_binding",
    "auth_require_channel_binding": "auth_require_channel_binding",
    "workloadidentitytoken": "workload_identity_token",
    "workload_identity_token": "workload_identity_token",
    "proxyprincipalassertion": "proxy_principal_assertion",
    "proxy_principal_assertion": "proxy_principal_assertion",
    "dormantid": "dormant_id",
    "dormant_id": "dormant_id",
    "dormantreattachtoken": "dormant_reattach_token",
    "dormant_reattach_token": "dormant_reattach_token",
}


_TYPE_MAP = {
    "BOOLEAN": types.Boolean(),
    "SMALLINT": types.SmallInteger(),
    "INTEGER": types.Integer(),
    "INT": types.Integer(),
    "BIGINT": types.BigInteger(),
    "INT8": types.BigInteger(),
    "REAL": types.Float(),
    "FLOAT": types.Float(),
    "DOUBLE": types.Float(),
    "DOUBLE PRECISION": types.Float(),
    "NUMERIC": types.Numeric(),
    "DECIMAL": types.Numeric(),
    "CHAR": types.String(),
    "CHARACTER": types.String(),
    "VARCHAR": types.String(),
    "CHARACTER VARYING": types.String(),
    "TEXT": types.Text(),
    "DATE": types.Date(),
    "TIME": types.Time(),
    "TIME WITH TIME ZONE": types.Time(timezone=True),
    "TIME WITHOUT TIME ZONE": types.Time(),
    "TIMESTAMP": types.DateTime(),
    "TIMESTAMPTZ": types.DateTime(timezone=True),
    "TIMESTAMP WITH TIME ZONE": types.DateTime(timezone=True),
    "TIMESTAMP WITHOUT TIME ZONE": types.DateTime(),
    "UUID": types.Uuid(),
    "JSON": types.JSON(),
    "JSONB": types.JSON(),
    "BYTEA": types.LargeBinary(),
    "BLOB": types.LargeBinary(),
    "ARRAY": types.ARRAY(types.String()),
    "VECTOR": types.ARRAY(types.Float()),
    "GEOMETRY": types.LargeBinary(),
    "GEOGRAPHY": types.LargeBinary(),
    "COMPOSITE": types.String(),
    "RECORD": types.String(),
    "ROW": types.String(),
    "RANGE": types.String(),
    "TSVECTOR": types.Text(),
    "TSQUERY": types.Text(),
    "INET": types.String(),
    "CIDR": types.String(),
    "MACADDR": types.String(),
    "BIT": types.LargeBinary(),
    "BIT VARYING": types.LargeBinary(),
    "XML": types.Text(),
    "INTERVAL": types.String(),
    "MONEY": types.Numeric(),
    "UNKNOWN": types.LargeBinary(),
}


def _normalize_type(type_name: Optional[str]) -> str:
    if not type_name:
        return ""
    normalized = type_name.strip().upper().replace("\t", " ").replace("\n", " ")
    while "  " in normalized:
        normalized = normalized.replace("  ", " ")
    if "(" in normalized:
        normalized = normalized.split("(", 1)[0].strip()
    return normalized


def _map_type(type_name: Optional[str]) -> types.TypeEngine:
    normalized = _normalize_type(type_name)
    if normalized.endswith("[]"):
        return types.ARRAY(_map_type(normalized[:-2]))
    mapped = _TYPE_MAP.get(normalized)
    if mapped is not None:
        return mapped
    return types.String()


def _query_value(value: Any) -> Any:
    if isinstance(value, (list, tuple)):
        return value[0] if value else None
    return value


def _is_falsey(value: Any) -> bool:
    return str(value).strip().lower() in {"false", "0", "no", "off"}


def _is_truthy(value: Any) -> bool:
    return str(value).strip().lower() in {"true", "1", "yes", "on"}


def _canonical_connect_arg_key(raw_key: Any) -> str:
    key = str(raw_key).strip()
    return _CONNECT_ARG_ALIASES.get(key.lower(), key)


def _normalize_ssl_alias(raw_key: Any, value: Any) -> Any:
    if str(raw_key).strip().lower() != "ssl":
        return value
    if _is_truthy(value):
        return "require"
    if _is_falsey(value):
        return "disable"
    return value


class ScratchBirdDialect(DefaultDialect):
    name = "scratchbird"
    driver = "sbwp"
    paramstyle = "named"
    supports_statement_cache = True
    supports_sane_rowcount = True
    supports_sane_multi_rowcount = False
    supports_native_boolean = True
    supports_native_decimal = True
    supports_native_uuid = True

    @classmethod
    def dbapi(cls):
        import scratchbird

        return scratchbird

    def get_default_schema_name(self, connection):
        try:
            result = connection.exec_driver_sql("SHOW current_schema")
            row = result.fetchone()
            if row:
                for value in row:
                    if value:
                        return str(value)
        except Exception:
            pass
        return "users.public"

    def create_connect_args(self, url):
        connect_args: Dict[str, Any] = {}
        connect_args["host"] = url.host or "localhost"
        connect_args["port"] = int(url.port or 3092)
        if url.username:
            connect_args["user"] = url.username
        if url.password:
            connect_args["password"] = url.password
        if url.database:
            connect_args["database"] = url.database

        for raw_key, raw_value in url.query.items():
            value = _query_value(raw_value)
            if value is None:
                continue

            value = _normalize_ssl_alias(raw_key, value)
            key = _canonical_connect_arg_key(raw_key)

            if key == "binary_transfer":
                if _is_falsey(value):
                    raise ValueError("binary_transfer=false is not supported")
                connect_args["binary_transfer"] = True if _is_truthy(value) else value
                continue
            if key == "sslmode" and str(value).strip().lower() == "disable":
                raise ValueError("sslmode=disable is not supported")
            if key == "compression" and str(value).strip().lower() == "zstd":
                raise ValueError("compression=zstd is not supported")
            if key == "front_door_mode":
                normalized = str(value).strip().lower()
                if normalized not in {"direct", "manager_proxy"}:
                    raise ValueError("front_door_mode must be direct or manager_proxy")
                value = normalized
            if key == "auth_method_id":
                value_text = str(value).strip()
                if value_text and not value_text.startswith("scratchbird.auth."):
                    raise ValueError("auth_method_id must start with scratchbird.auth.")
                value = value_text

            if key == "schema" and "schema" in connect_args:
                continue
            connect_args[key] = value

        return [], connect_args

    def get_schema_names(self, connection, **kw):
        rows = connection.exec_driver_sql(
            "SELECT schema_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
        ).fetchall()
        return [row[0] for row in rows]

    def get_table_names(self, connection, schema: Optional[str] = None, **kw):
        sql = (
            "SELECT t.table_name "
            "FROM sys.tables t "
            "JOIN sys.schemas s ON s.schema_id = t.schema_id "
            "WHERE t.is_valid = 1"
        )
        params: Dict[str, Any] = {}
        if schema:
            sql += " AND s.schema_name = :schema"
            params["schema"] = schema
        sql += " ORDER BY t.table_name"
        rows = connection.execute(text(sql), params).fetchall()
        return [row[0] for row in rows]

    def get_view_names(self, connection, schema: Optional[str] = None, **kw):
        sql = (
            "SELECT t.table_name "
            "FROM sys.tables t "
            "JOIN sys.schemas s ON s.schema_id = t.schema_id "
            "WHERE t.is_valid = 1 AND t.table_type = 'VIEW'"
        )
        params: Dict[str, Any] = {}
        if schema:
            sql += " AND s.schema_name = :schema"
            params["schema"] = schema
        sql += " ORDER BY t.table_name"
        try:
            rows = connection.execute(text(sql), params).fetchall()
        except Exception:
            return []
        return [row[0] for row in rows]

    def get_columns(self, connection, table_name: str, schema: Optional[str] = None, **kw):
        params: Dict[str, Any] = {"table": table_name}
        sql = (
            "SELECT c.column_name, c.data_type_name, c.is_nullable, c.default_value "
            "FROM sys.columns c "
            "JOIN sys.tables t ON t.table_id = c.table_id "
            "JOIN sys.schemas s ON s.schema_id = t.schema_id "
            "WHERE c.is_valid = 1 AND t.is_valid = 1 AND t.table_name = :table"
        )
        if schema:
            sql += " AND s.schema_name = :schema"
            params["schema"] = schema
        sql += " ORDER BY c.ordinal_position"
        rows = connection.execute(text(sql), params).fetchall()
        columns = []
        for row in rows:
            col_name = row[0]
            data_type_name = row[1]
            nullable = bool(row[2]) if row[2] is not None else True
            default_value = row[3]
            sqltype = _map_type(str(data_type_name))
            columns.append(
                {
                    "name": col_name,
                    "type": sqltype,
                    "nullable": nullable,
                    "default": default_value,
                }
            )
        return columns

    def get_pk_constraint(self, connection, table_name: str, schema: Optional[str] = None, **kw):
        sql = (
            "SELECT kcu.column_name, tc.constraint_name "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "  ON tc.constraint_name = kcu.constraint_name "
            " AND tc.table_schema = kcu.table_schema "
            " AND tc.table_name = kcu.table_name "
            "WHERE tc.constraint_type = 'PRIMARY KEY' AND tc.table_name = :table"
        )
        params: Dict[str, Any] = {"table": table_name}
        if schema:
            sql += " AND tc.table_schema = :schema"
            params["schema"] = schema
        sql += " ORDER BY kcu.ordinal_position"
        try:
            rows = connection.execute(text(sql), params).fetchall()
        except Exception:
            return {"constrained_columns": [], "name": None}
        columns = [row[0] for row in rows]
        name = rows[0][1] if rows else None
        return {"constrained_columns": columns, "name": name}

    def get_foreign_keys(self, connection, table_name: str, schema: Optional[str] = None, **kw):
        sql = (
            "SELECT tc.constraint_name, kcu.column_name, "
            "ccu.table_schema, ccu.table_name, ccu.column_name, "
            "rc.update_rule, rc.delete_rule "
            "FROM information_schema.table_constraints tc "
            "JOIN information_schema.key_column_usage kcu "
            "  ON tc.constraint_name = kcu.constraint_name "
            " AND tc.table_schema = kcu.table_schema "
            " AND tc.table_name = kcu.table_name "
            "JOIN information_schema.constraint_column_usage ccu "
            "  ON ccu.constraint_name = tc.constraint_name "
            " AND ccu.constraint_schema = tc.table_schema "
            "LEFT JOIN information_schema.referential_constraints rc "
            "  ON rc.constraint_name = tc.constraint_name "
            " AND rc.constraint_schema = tc.table_schema "
            "WHERE tc.constraint_type = 'FOREIGN KEY' AND tc.table_name = :table"
        )
        params: Dict[str, Any] = {"table": table_name}
        if schema:
            sql += " AND tc.table_schema = :schema"
            params["schema"] = schema
        sql += " ORDER BY kcu.ordinal_position"
        try:
            rows = connection.execute(text(sql), params).fetchall()
        except Exception:
            return []
        keys: Dict[str, Dict[str, Any]] = {}
        for row in rows:
            name = row[0]
            fk_col = row[1]
            pk_schema = row[2]
            pk_table = row[3]
            pk_col = row[4]
            on_update = row[5]
            on_delete = row[6]
            entry = keys.setdefault(
                name,
                {
                    "name": name,
                    "constrained_columns": [],
                    "referred_schema": pk_schema,
                    "referred_table": pk_table,
                    "referred_columns": [],
                    "options": {},
                },
            )
            entry["constrained_columns"].append(fk_col)
            entry["referred_columns"].append(pk_col)
            if on_update:
                entry["options"]["onupdate"] = on_update
            if on_delete:
                entry["options"]["ondelete"] = on_delete
        return list(keys.values())

    def get_indexes(self, connection, table_name: str, schema: Optional[str] = None, **kw):
        sql = (
            "SELECT i.index_name, i.is_unique, c.column_name, ic.ordinal_position "
            "FROM sys.indexes i "
            "JOIN sys.tables t ON t.table_id = i.table_id "
            "JOIN sys.schemas s ON s.schema_id = t.schema_id "
            "LEFT JOIN sys.index_columns ic ON ic.index_id = i.index_id "
            "LEFT JOIN sys.columns c ON c.column_id = ic.column_id "
            "WHERE t.table_name = :table AND i.is_valid = 1"
        )
        params: Dict[str, Any] = {"table": table_name}
        if schema:
            sql += " AND s.schema_name = :schema"
            params["schema"] = schema
        sql += " ORDER BY i.index_name, ic.ordinal_position"
        try:
            rows = connection.execute(text(sql), params).fetchall()
        except Exception:
            return []
        indexes_by_name: Dict[str, Dict[str, Any]] = {}
        for row in rows:
            entry = indexes_by_name.setdefault(
                row[0],
                {
                    "name": row[0],
                    "column_names": [],
                    "unique": bool(row[1]),
                },
            )
            if row[2] is not None:
                entry["column_names"].append(row[2])
        return list(indexes_by_name.values())
