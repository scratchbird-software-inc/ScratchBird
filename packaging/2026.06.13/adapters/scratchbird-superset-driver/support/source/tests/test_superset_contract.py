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


def _install_superset_stubs() -> None:
    if "superset.constants" in sys.modules:
        return

    superset_mod = types.ModuleType("superset")
    constants_mod = types.ModuleType("superset.constants")
    engine_specs_mod = types.ModuleType("superset.db_engine_specs")
    engine_specs_base_mod = types.ModuleType("superset.db_engine_specs.base")
    sql_mod = types.ModuleType("superset.sql")
    sql_parse_mod = types.ModuleType("superset.sql.parse")

    class TimeGrain:
        SECOND = "SECOND"
        MINUTE = "MINUTE"
        HOUR = "HOUR"
        DAY = "DAY"
        WEEK = "WEEK"
        MONTH = "MONTH"
        QUARTER = "QUARTER"
        YEAR = "YEAR"

    class DatabaseCategory:
        TRADITIONAL_RDBMS = "TRADITIONAL_RDBMS"
        OPEN_SOURCE = "OPEN_SOURCE"

    class BaseEngineSpec:
        pass

    class LimitMethod:
        FORCE_LIMIT = "FORCE_LIMIT"

    constants_mod.TimeGrain = TimeGrain
    engine_specs_base_mod.BaseEngineSpec = BaseEngineSpec
    engine_specs_base_mod.DatabaseCategory = DatabaseCategory
    sql_parse_mod.LimitMethod = LimitMethod

    sys.modules["superset"] = superset_mod
    sys.modules["superset.constants"] = constants_mod
    sys.modules["superset.db_engine_specs"] = engine_specs_mod
    sys.modules["superset.db_engine_specs.base"] = engine_specs_base_mod
    sys.modules["superset.sql"] = sql_mod
    sys.modules["superset.sql.parse"] = sql_parse_mod


def _load_module(name: str, path: pathlib.Path):
    spec = importlib.util.spec_from_file_location(name, path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_superset_modules():
    _install_sqlalchemy_stubs()
    _install_superset_stubs()
    root = pathlib.Path(__file__).resolve().parents[1]
    dialect = _load_module(
        "scratchbird_superset.dialect",
        root / "scratchbird_superset" / "dialect.py",
    )
    engine_spec = _load_module(
        "scratchbird_superset.engine_spec",
        root / "scratchbird_superset" / "engine_spec.py",
    )
    return dialect, engine_spec


class _FakeResult:
    def __init__(self, rows):
        self._rows = list(rows)

    def fetchall(self):
        return list(self._rows)

    def fetchone(self):
        return self._rows[0] if self._rows else None


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

    def exec_driver_sql(self, sql_text):
        self.calls.append((sql_text, {}))
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


def test_create_connect_args_normalizes_jdbc_style_aliases():
    dialect_module, _ = _load_superset_modules()
    dialect = dialect_module.ScratchBirdDialect()

    _, connect_args = dialect.create_connect_args(
        _FakeURL(
            host="db.local",
            port=3092,
            username="alice",
            password="secret",
            database="analytics",
            query={
                "currentSchema": "tenant.analytics",
                "searchPath": "ignored.by.explicit.current",
                "applicationName": "superset",
                "sslRootCert": "/tmp/root.pem",
                "sslCert": "/tmp/client.pem",
                "sslKey": "/tmp/client.key",
                "managerAuthToken": "manager-token",
                "connectTimeout": "5",
                "socketTimeout": "9",
                "frontDoorMode": "manager_proxy",
                "authToken": "opaque-token",
                "authMethodId": "scratchbird.auth.jwt_oidc",
                "authRequiredMethods": "TOKEN,SCRAM_SHA_512",
                "authForbiddenMethods": "MD5",
                "authRequireChannelBinding": "true",
                "workloadIdentityToken": "workload-token",
                "proxyPrincipalAssertion": "proxy-assertion",
                "dormantId": "dormant-1",
                "dormantReattachToken": "reattach-token",
                "fetchSize": "512",
                "connectClientFlags": "analytics,readonly",
            },
        )
    )

    assert connect_args["host"] == "db.local"
    assert connect_args["port"] == 3092
    assert connect_args["database"] == "analytics"
    assert connect_args["schema"] == "tenant.analytics"
    assert connect_args["application_name"] == "superset"
    assert connect_args["sslrootcert"] == "/tmp/root.pem"
    assert connect_args["sslcert"] == "/tmp/client.pem"
    assert connect_args["sslkey"] == "/tmp/client.key"
    assert connect_args["manager_auth_token"] == "manager-token"
    assert connect_args["connect_timeout"] == "5"
    assert connect_args["socket_timeout"] == "9"
    assert connect_args["front_door_mode"] == "manager_proxy"
    assert connect_args["auth_token"] == "opaque-token"
    assert connect_args["auth_method_id"] == "scratchbird.auth.jwt_oidc"
    assert connect_args["auth_required_methods"] == "TOKEN,SCRAM_SHA_512"
    assert connect_args["auth_forbidden_methods"] == "MD5"
    assert connect_args["auth_require_channel_binding"] == "true"
    assert connect_args["workload_identity_token"] == "workload-token"
    assert connect_args["proxy_principal_assertion"] == "proxy-assertion"
    assert connect_args["dormant_id"] == "dormant-1"
    assert connect_args["dormant_reattach_token"] == "reattach-token"
    assert connect_args["fetch_size"] == "512"
    assert connect_args["connect_client_flags"] == "analytics,readonly"


def test_create_connect_args_rejects_invalid_bootstrap_policy_inputs():
    dialect_module, _ = _load_superset_modules()
    dialect = dialect_module.ScratchBirdDialect()

    try:
        dialect.create_connect_args(_FakeURL(query={"frontDoorMode": "sidecar"}))
    except ValueError as exc:
        assert "front_door_mode must be direct or manager_proxy" in str(exc)
    else:
        raise AssertionError("expected invalid front_door_mode failure")

    try:
        dialect.create_connect_args(_FakeURL(query={"authMethodId": "jwt_oidc"}))
    except ValueError as exc:
        assert "auth_method_id must start with scratchbird.auth." in str(exc)
    else:
        raise AssertionError("expected invalid auth_method_id failure")


def test_default_schema_and_index_reflection_use_live_catalog_shape():
    dialect_module, _ = _load_superset_modules()
    dialect = dialect_module.ScratchBirdDialect()
    connection = _FakeConnection(
        {
            "SHOW current_schema": [("tenant.analytics",)],
            "FROM sys.indexes": [
                ("idx_orders_customer", True, "customer_id", 1),
                ("idx_orders_customer", True, "created_at", 2),
                ("idx_orders_status", False, "status", 1),
            ],
        }
    )

    assert dialect.get_default_schema_name(connection) == "tenant.analytics"

    indexes = dialect.get_indexes(connection, "orders", schema="tenant.analytics")
    assert len(indexes) == 2
    by_name = {item["name"]: item for item in indexes}
    assert by_name["idx_orders_customer"]["unique"] is True
    assert by_name["idx_orders_customer"]["column_names"] == ["customer_id", "created_at"]
    assert by_name["idx_orders_status"]["unique"] is False
    assert by_name["idx_orders_status"]["column_names"] == ["status"]


def test_type_mapping_and_engine_spec_publish_supported_surface():
    dialect_module, engine_spec_module = _load_superset_modules()
    mapped = dialect_module._map_type("INTEGER[]")
    assert mapped.__class__.__name__ == "ARRAY"
    assert mapped.item_type.__class__.__name__ == "Integer"

    engine_spec = engine_spec_module.ScratchBirdEngineSpec()
    assert engine_spec.supports_dynamic_schema is True
    assert engine_spec.allows_joins is True
    assert engine_spec.metadata["default_port"] == 3092
