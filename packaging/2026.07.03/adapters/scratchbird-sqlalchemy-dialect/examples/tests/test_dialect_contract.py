# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import importlib.util
import pathlib
import sys
import types

import pytest


def _install_sqlalchemy_stubs() -> None:
    if "sqlalchemy" in sys.modules:
        return

    sqlalchemy_mod = types.ModuleType("sqlalchemy")
    sa_types_mod = types.ModuleType("sqlalchemy.types")
    sa_engine_mod = types.ModuleType("sqlalchemy.engine")
    sa_engine_default_mod = types.ModuleType("sqlalchemy.engine.default")

    class _TypeBase:
        def __init__(self, *args, **kwargs):
            self.args = args
            self.kwargs = kwargs

    type_names = [
        "Boolean",
        "SmallInteger",
        "Integer",
        "BigInteger",
        "Float",
        "Numeric",
        "String",
        "Text",
        "Date",
        "Time",
        "DateTime",
        "Uuid",
        "JSON",
        "LargeBinary",
    ]
    for type_name in type_names:
        setattr(sa_types_mod, type_name, type(type_name, (_TypeBase,), {}))

    class ARRAY(_TypeBase):
        def __init__(self, item_type, *args, **kwargs):
            super().__init__(item_type, *args, **kwargs)
            self.item_type = item_type

    sa_types_mod.ARRAY = ARRAY
    sa_types_mod.TypeEngine = _TypeBase

    class DefaultDialect:
        pass

    sa_engine_default_mod.DefaultDialect = DefaultDialect

    def text(sql_text: str):
        return sql_text

    sqlalchemy_mod.text = text
    sqlalchemy_mod.types = sa_types_mod
    sa_engine_mod.default = sa_engine_default_mod

    sys.modules["sqlalchemy"] = sqlalchemy_mod
    sys.modules["sqlalchemy.types"] = sa_types_mod
    sys.modules["sqlalchemy.engine"] = sa_engine_mod
    sys.modules["sqlalchemy.engine.default"] = sa_engine_default_mod


def _load_dialect_module():
    _install_sqlalchemy_stubs()
    root = pathlib.Path(__file__).resolve().parents[1]
    module_path = root / "scratchbird_sqlalchemy" / "dialect.py"
    spec = importlib.util.spec_from_file_location("scratchbird_sqlalchemy.dialect", module_path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _FakeResult:
    def __init__(self, rows):
        self._rows = rows

    def fetchall(self):
        return list(self._rows)


class _FakeConnection:
    def __init__(self, responses):
        self.responses = responses
        self.calls = []

    def execute(self, sql, params):
        sql_text = str(sql)
        self.calls.append((sql_text, dict(params)))
        for key, rows in self.responses.items():
            if key in sql_text:
                return _FakeResult(rows)
        return _FakeResult([])


class _FakeURL:
    def __init__(self, *, host="localhost", port=3092, username=None, password=None, database=None, query=None):
        self.host = host
        self.port = port
        self.username = username
        self.password = password
        self.database = database
        self.query = query or {}


def test_create_connect_args_maps_aliases_and_enforces_binary_tls_guardrails():
    module = _load_dialect_module()
    dialect = module.ScratchBirdDialect()

    url = _FakeURL(
        host="db.local",
        port=3999,
        username="alice",
        password="secret",
        database="maindb",
        query={
            "connectTimeout": "5",
            "socketTimeout": "9",
            "binaryTransfer": "true",
            "sslmode": "require",
            "application_name": "sqlalchemy-test",
            "frontDoorMode": "manager_proxy",
            "managerAuthToken": "manager-token",
            "authToken": "opaque-token",
            "authMethodId": "scratchbird.auth.jwt_oidc",
            "authRequiredMethods": "TOKEN,SCRAM_SHA_512",
            "authForbiddenMethods": "MD5",
            "authRequireChannelBinding": "true",
            "workloadIdentityToken": "workload-token",
            "proxyPrincipalAssertion": "proxy-assertion",
            "dormantId": "dormant-1",
            "dormantReattachToken": "reattach-token",
            "fetchSize": "250",
            "connectClientFlags": "readonly,analytics",
        },
    )

    _, connect_args = dialect.create_connect_args(url)
    assert connect_args["host"] == "db.local"
    assert connect_args["port"] == 3999
    assert connect_args["user"] == "alice"
    assert connect_args["password"] == "secret"
    assert connect_args["database"] == "maindb"
    assert connect_args["connect_timeout"] == "5"
    assert connect_args["socket_timeout"] == "9"
    assert connect_args["binary_transfer"] is True
    assert connect_args["front_door_mode"] == "manager_proxy"
    assert connect_args["manager_auth_token"] == "manager-token"
    assert connect_args["auth_token"] == "opaque-token"
    assert connect_args["auth_method_id"] == "scratchbird.auth.jwt_oidc"
    assert connect_args["auth_required_methods"] == "TOKEN,SCRAM_SHA_512"
    assert connect_args["auth_forbidden_methods"] == "MD5"
    assert connect_args["auth_require_channel_binding"] == "true"
    assert connect_args["workload_identity_token"] == "workload-token"
    assert connect_args["proxy_principal_assertion"] == "proxy-assertion"
    assert connect_args["dormant_id"] == "dormant-1"
    assert connect_args["dormant_reattach_token"] == "reattach-token"
    assert connect_args["fetch_size"] == "250"
    assert connect_args["connect_client_flags"] == "readonly,analytics"

    with pytest.raises(ValueError, match="binary_transfer=false"):
        dialect.create_connect_args(_FakeURL(query={"binaryTransfer": "false"}))

    with pytest.raises(ValueError, match="sslmode=disable"):
        dialect.create_connect_args(_FakeURL(query={"sslmode": "disable"}))

    with pytest.raises(ValueError, match="front_door_mode must be direct or manager_proxy"):
        dialect.create_connect_args(_FakeURL(query={"frontDoorMode": "sidecar"}))

    with pytest.raises(ValueError, match="auth_method_id must start with scratchbird.auth."):
        dialect.create_connect_args(_FakeURL(query={"authMethodId": "jwt_oidc"}))


def test_get_columns_returns_reflection_keys_and_schema_filter():
    module = _load_dialect_module()
    dialect = module.ScratchBirdDialect()

    connection = _FakeConnection(
        {
            "FROM sys.columns": [
                ("id", "INTEGER", False, None, True),
                ("name", "TEXT", True, "'anon'", False),
            ]
        }
    )

    columns = dialect.get_columns(connection, "accounts", schema="users")
    assert len(columns) == 2
    assert set(columns[0].keys()) == {"name", "type", "nullable", "default", "autoincrement"}
    assert columns[0]["name"] == "id"
    assert columns[0]["nullable"] is False
    assert columns[0]["autoincrement"] is True
    assert columns[1]["name"] == "name"
    assert columns[1]["default"] == "'anon'"

    _, params = connection.calls[0]
    assert params == {"table": "accounts", "schema": "users"}


def test_schema_qualified_table_and_index_reflection():
    module = _load_dialect_module()
    dialect = module.ScratchBirdDialect()

    connection = _FakeConnection(
        {
            "FROM sys.tables": [("orders",), ("order_items",)],
            "FROM sys.indexes": [
                ("idx_orders_customer", True, "customer_id", 1),
                ("idx_orders_customer", True, "created_at", 2),
                ("idx_orders_status", False, "status", 1),
            ],
        }
    )

    table_names = dialect.get_table_names(connection, schema="sales")
    assert table_names == ["orders", "order_items"]
    first_sql, first_params = connection.calls[0]
    assert "s.schema_name = :schema" in first_sql
    assert first_params == {"schema": "sales"}

    indexes = dialect.get_indexes(connection, "orders", schema="sales")
    assert len(indexes) == 2
    by_name = {item["name"]: item for item in indexes}
    assert by_name["idx_orders_customer"]["unique"] is True
    assert by_name["idx_orders_customer"]["column_names"] == ["customer_id", "created_at"]
    assert by_name["idx_orders_status"]["unique"] is False


def test_pk_and_fk_reflection_contract():
    module = _load_dialect_module()
    dialect = module.ScratchBirdDialect()

    connection = _FakeConnection(
        {
            "constraint_type = 'PRIMARY KEY'": [
                ("id", "pk_orders"),
            ],
            "constraint_type = 'FOREIGN KEY'": [
                (
                    "fk_orders_user",
                    "user_id",
                    "sys",
                    "users",
                    "id",
                    "CASCADE",
                    "RESTRICT",
                )
            ],
        }
    )

    pk = dialect.get_pk_constraint(connection, "orders", schema="sales")
    assert pk["name"] == "pk_orders"
    assert pk["constrained_columns"] == ["id"]

    fks = dialect.get_foreign_keys(connection, "orders", schema="sales")
    assert len(fks) == 1
    fk = fks[0]
    assert fk["name"] == "fk_orders_user"
    assert fk["constrained_columns"] == ["user_id"]
    assert fk["referred_schema"] == "sys"
    assert fk["referred_table"] == "users"
    assert fk["referred_columns"] == ["id"]
    assert fk["options"]["onupdate"] == "CASCADE"
    assert fk["options"]["ondelete"] == "RESTRICT"
