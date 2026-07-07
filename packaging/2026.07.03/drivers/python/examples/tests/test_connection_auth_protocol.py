# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

from __future__ import annotations

import socket
import struct

import pytest

from scratchbird import errors
from scratchbird.connection import Connection, ConnectionConfig, connect, probe_auth_surface
import scratchbird.connection as connection_mod
from scratchbird.dsn import parse_dsn
from scratchbird.protocol import (
    AuthMethod,
    FEATURE_COMPRESSION,
    FEATURE_STREAMING,
    MessageHeader,
    MessageType,
    PROTOCOL_VERSION,
    build_p1_startup_payload,
    parse_parameter_statuses,
    parse_ready,
)


def make_auth_request_payload(method: int) -> bytes:
    return bytes([method, 0, 0, 0])


def make_auth_continue_payload(method: int, data: bytes, stage: int = 1) -> bytes:
    return bytes([method, stage, 0, 0]) + struct.pack("<I", len(data)) + data


def make_auth_ok_payload(info: bytes = b"") -> bytes:
    return (b"\x11" * 16) + struct.pack("<I", len(info)) + info


def test_parse_dsn_kv_supports_semicolon_tokens():
    params = parse_dsn("host=server;port=4000;dbname=mydb;username=me")
    assert params["host"] == "server"
    assert params["port"] == "4000"
    assert params["dbname"] == "mydb"
    assert params["username"] == "me"


def test_connect_maps_common_conn_aliases(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect(
        "host=server;port=4000;dbname=mydb;username=me;password=secret;front_door_mode=managed;"
        "protocol=scratchbird_native;connecttimeout=5;sockettimeout=7;applicationname=app;binarytransfer=off",
    )
    cfg = captured["cfg"]
    assert cfg.host == "server"
    assert cfg.port == 4000
    assert cfg.database == "mydb"
    assert cfg.user == "me"
    assert cfg.password == "secret"
    assert cfg.front_door_mode == "manager_proxy"
    assert cfg.protocol == "native"
    assert cfg.connect_timeout == 5
    assert cfg.socket_timeout == 7
    assert cfg.application_name == "app"
    assert cfg.binary_transfer is False


def test_connect_accepts_non_native_protocol_hints(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect(
        "host=server;port=4000;dbname=mydb;username=me;password=secret;"
        "protocol=jdbc;parser=postgresql;dialect=odbc",
    )
    cfg = captured["cfg"]
    assert cfg.protocol == "native"


def test_connect_maps_metadata_expand_schema_parents_aliases(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect(
        "host=server;port=4000;dbname=mydb;username=me;"
        "metadataExpandSchemaParents=true;dbeaver_expand_schema_parents=false;"
        "metadata_expand_schema_parents=1",
    )
    cfg = captured["cfg"]
    assert cfg.metadata_expand_schema_parents is True


def test_connect_maps_current_schema_aliases(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect(
        "host=server;port=4000;dbname=mydb;username=me;current_schema=public",
    )
    cfg = captured["cfg"]
    assert cfg.schema == "public"


def test_connect_accepts_disable_tls_and_zstd_compression(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect(
        "host=server;port=4000;dbname=mydb;username=me;password=secret;"
        "sslmode=disable;binarytransfer=off;compression=zstd",
    )
    cfg = captured["cfg"]
    assert cfg.sslmode == "disable"
    assert cfg.binary_transfer is False
    assert cfg.compression == "zstd"


def test_connect_rejects_unknown_compression_mode():
    with pytest.raises(errors.InterfaceError, match="compression must be off or zstd"):
        connect("host=server;port=4000;dbname=mydb;username=me;compression=gzip")


def test_connect_captures_auth_plugin_startup_fields(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect(
        "scratchbird://alice:secret@localhost:3092/mydb?"
        "connect_client_flags=257&auth_method_id=scratchbird.auth.oidc&auth_method_payload=opaque"
        "&auth_payload_json=%7B%22aud%22%3A%22sb%22%7D&auth_payload_b64=YWJj"
        "&auth_provider_profile=corp&auth_required_methods=SCRAM_SHA_256%2CTOKEN"
        "&auth_forbidden_methods=MD5&auth_require_channel_binding=true"
        "&workload_identity_token=jwt-token&proxy_principal_assertion=signed-assertion",
    )
    cfg = captured["cfg"]
    assert cfg.connect_client_flags == 257
    assert cfg.auth_method_id == "scratchbird.auth.oidc"
    assert cfg.auth_method_payload == "opaque"
    assert cfg.auth_payload_json == '{"aud":"sb"}'
    assert cfg.auth_payload_b64 == "YWJj"
    assert cfg.auth_provider_profile == "corp"
    assert cfg.auth_required_methods == "SCRAM_SHA_256,TOKEN"
    assert cfg.auth_forbidden_methods == "MD5"
    assert cfg.auth_require_channel_binding is True
    assert cfg.workload_identity_token == "jwt-token"
    assert cfg.proxy_principal_assertion == "signed-assertion"


def test_connect_captures_generic_auth_token_alias(monkeypatch):
    captured = {}

    class FakeConnection:
        def __init__(self, config):
            captured["cfg"] = config

    monkeypatch.setattr(connection_mod, "Connection", FakeConnection)
    connect("scratchbird://alice@localhost:3092/mydb?auth_token=bearer-value")
    assert captured["cfg"].auth_token == "bearer-value"


def test_build_startup_params_includes_auth_plugin_selection():
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        user="alice",
        database="db1",
        role="analyst",
        application_name="pytest",
        connect_client_flags=257,
        auth_method_id="scratchbird.auth.oidc",
        auth_method_payload="opaque",
        auth_payload_json='{"aud":"sb"}',
        auth_payload_b64="YWJj",
        auth_provider_profile="corp",
        auth_required_methods="SCRAM_SHA_256,TOKEN",
        auth_forbidden_methods="MD5",
        auth_require_channel_binding=True,
        workload_identity_token="jwt-token",
        proxy_principal_assertion="signed-assertion",
    )

    params = conn._build_startup_params()
    assert params["database"] == "db1"
    assert params["user"] == "alice"
    assert params["client_flags"] == "257"
    assert params["role"] == "analyst"
    assert params["application_name"] == "pytest"
    assert params["auth_method_id"] == "scratchbird.auth.oidc"
    assert params["auth_method_payload"] == "opaque"
    assert params["auth_payload_json"] == '{"aud":"sb"}'
    assert params["auth_payload_b64"] == "YWJj"
    assert params["auth_provider_profile"] == "corp"
    assert params["auth_required_methods"] == "SCRAM_SHA_256,TOKEN"
    assert params["auth_forbidden_methods"] == "MD5"
    assert params["auth_require_channel_binding"] == "1"
    assert params["workload_identity_token"] == "jwt-token"
    assert params["proxy_principal_assertion"] == "signed-assertion"


def test_p1_startup_payload_uses_current_version_window():
    payload = build_p1_startup_payload(
        client_features=FEATURE_STREAMING,
        required_features=0,
        params={"database": "db1", "user": "alice"},
    )

    assert struct.unpack_from("<H", payload, 0)[0] == PROTOCOL_VERSION
    assert struct.unpack_from("<H", payload, 2)[0] == PROTOCOL_VERSION
    assert struct.unpack_from("<Q", payload, 8)[0] == FEATURE_STREAMING
    assert struct.unpack_from("<Q", payload, 16)[0] == 0
    assert struct.unpack_from("<I", payload, 80)[0] == 2


def test_parse_p1_parameter_statuses_and_ready():
    statuses = bytearray()
    statuses += struct.pack("<I", 2)
    for name, value in (("protocol.selected_version", "1.1"), ("security.generation", "refreshed")):
        name_bytes = name.encode("utf-8")
        value_bytes = value.encode("utf-8")
        statuses += struct.pack("<I", len(name_bytes)) + name_bytes
        statuses += b"\x01\x00\x00"
        statuses += struct.pack("<I", len(value_bytes)) + value_bytes

    assert parse_parameter_statuses(bytes(statuses)) == [
        ("protocol.selected_version", "1.1"),
        ("security.generation", "refreshed"),
    ]

    ready = bytearray()
    ready += b"\x11" * 16
    ready += b"\x22" * 16
    ready += b"\x00" * 16
    ready += struct.pack("<Q", 99)
    ready += b"T"
    ready += b"\x01"
    ready += struct.pack("<H", PROTOCOL_VERSION)
    ready += struct.pack("<Q", FEATURE_COMPRESSION)
    ready += struct.pack("<I", 0)
    ready += struct.pack("<I", 0)

    assert parse_ready(bytes(ready)) == (1, 99, 99)


def test_startup_and_auth_rejects_invalid_auth_plugin_namespace():
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        user="alice",
        database="db1",
        auth_method_id="invalid.namespace",
    )
    conn._parameters = {}
    conn._authed = False

    with pytest.raises(errors.InterfaceError, match="invalid auth_method_id namespace"):
        conn._startup_and_auth()


def test_probe_auth_surface_reports_direct_auth_negotiation_requirements(monkeypatch):
    queue = [
        (
            MessageHeader(MessageType.AUTH_REQUEST, 0, 4, 1, b"\x00" * 16, 0),
            make_auth_request_payload(AuthMethod.SCRAM_SHA_512),
        )
    ]

    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        host="db.example.internal",
        port=4090,
        user="alice",
        database="db1",
        sslmode="disable",
    )
    conn._resolved_auth_context = connection_mod.ResolvedAuthContext(
        ingress_mode="direct",
        resolved_auth_method=None,
        resolved_auth_plugin_id=None,
        manager_authenticated=False,
        attached=False,
    )
    conn._send_message = lambda *_args, **_kwargs: None
    conn._recv_message = lambda: queue.pop(0)
    conn._handle_async = lambda *_args, **_kwargs: False
    conn._open_socket = lambda require_identity=False, require_manager_token=False: None
    conn._disconnect_socket_for_reconnect = lambda: None

    result = conn.probe_auth_surface()

    assert result.reachable is True
    assert result.ingress_mode == "direct"
    assert result.resolved_host == "db.example.internal"
    assert result.resolved_port == 4090
    assert result.required_method == "SCRAM_SHA_512"
    assert result.required_plugin_method_id == "scratchbird.auth.scram_sha_512"
    assert result.additional_continuation_possible is True
    assert len(result.admitted_methods) == 1
    assert result.admitted_methods[0].wire_method == "SCRAM_SHA_512"
    assert result.admitted_methods[0].executable_locally is True


def test_probe_auth_surface_reports_manager_proxy_token_bootstrap(monkeypatch):
    frames = [
        (connection_mod.MCP_MSG_STATUS_RESPONSE, b""),
        (connection_mod.MCP_MSG_AUTH_CHALLENGE, b""),
    ]

    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        host="mgr.example.internal",
        port=3092,
        front_door_mode="manager_proxy",
        sslmode="disable",
        manager_username="ops",
    )
    conn._resolved_auth_context = connection_mod.ResolvedAuthContext(
        ingress_mode="manager_proxy",
        resolved_auth_method=None,
        resolved_auth_plugin_id=None,
        manager_authenticated=False,
        attached=False,
    )
    conn._open_socket = lambda require_identity=False, require_manager_token=False: None
    conn._disconnect_socket_for_reconnect = lambda: None
    conn._send_manager_frame = lambda *_args, **_kwargs: None
    conn._recv_manager_frame = lambda: frames.pop(0)

    result = conn.probe_auth_surface()

    assert result.reachable is True
    assert result.ingress_mode == "manager_proxy"
    assert result.required_method == "TOKEN"
    assert result.required_plugin_method_id == "scratchbird.auth.authkey_token"
    assert result.additional_continuation_possible is True
    assert len(result.admitted_methods) == 1
    assert result.admitted_methods[0].wire_method == "TOKEN"


def test_top_level_probe_auth_surface_uses_driver_config_builder(monkeypatch):
    captured = {}

    def fake_probe(self):
        captured["config"] = self._config
        return connection_mod.AuthProbeResult(
            reachable=True,
            ingress_mode=self._config.front_door_mode,
            resolved_host=self._config.host,
            resolved_port=self._config.port,
            admitted_methods=[],
            required_method=None,
            required_plugin_method_id=None,
            allowed_transport_mask=None,
            additional_continuation_possible=False,
        )

    monkeypatch.setattr(connection_mod.Connection, "probe_auth_surface", fake_probe)

    result = probe_auth_surface(
        "scratchbird://alice@localhost:3092/mydb?front_door_mode=managed&auth_token=manager-bearer"
    )

    assert result.reachable is True
    assert captured["config"].front_door_mode == "manager_proxy"
    assert captured["config"].auth_token == "manager-bearer"


def test_connect_rejects_manager_proxy_without_token_before_socket(monkeypatch):
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        user="alice",
        database="db1",
        front_door_mode="manager_proxy",
        manager_auth_token=None,
    )

    def fail_create_connection(*_args, **_kwargs):
        raise AssertionError("socket.create_connection should not be called")

    monkeypatch.setattr(connection_mod.socket, "create_connection", fail_create_connection)

    with pytest.raises(errors.InterfaceError, match="manager_proxy mode requires manager_auth_token"):
        conn._connect()


def test_connect_sslmode_disable_uses_plain_socket_without_tls(monkeypatch):
    class DummySocket:
        def __init__(self):
            self.timeout = None
            self.closed = False

        def setsockopt(self, *_args, **_kwargs):
            return None

        def settimeout(self, value):
            self.timeout = value

        def gettimeout(self):
            return self.timeout

        def close(self):
            self.closed = True

    class DummyKeepalive:
        def __init__(self):
            self.registered = None

        def register(self, conn_id, callback):
            self.registered = (conn_id, callback)
            return object()

    raw_sock = DummySocket()
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        host="127.0.0.1",
        port=3092,
        user="alice",
        password="secret",
        database="db1",
        protocol="jdbc",
        sslmode="disable",
        binary_transfer=False,
        compression="zstd",
    )
    conn._conn_id = "conn-test-disable-tls"
    conn._keepalive = DummyKeepalive()
    conn._connected = False
    conn._socket = None
    conn._startup_and_auth = lambda: None
    conn._apply_schema = lambda: None

    monkeypatch.setattr(connection_mod.socket, "create_connection", lambda *_args, **_kwargs: raw_sock)
    monkeypatch.setattr(
        connection_mod.ssl,
        "create_default_context",
        lambda: pytest.fail("TLS context should not be created when sslmode=disable"),
    )

    conn._connect()

    assert conn._socket is raw_sock
    assert conn._connected is True
    assert conn._config.protocol == "native"
    assert conn._config.sslmode == "disable"
    assert conn._config.compression == "zstd"
    assert conn._keepalive.registered is not None


def test_startup_features_include_zstd_without_streaming_when_binary_transfer_disabled():
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(
        user="alice",
        database="db1",
        password="secret",
        compression="zstd",
        binary_transfer=False,
    )
    conn._parameters = {}
    conn._authed = False
    conn._attachment_id = b"\x00" * 16
    conn._txn_id = 0

    sent = []
    conn._send_message = lambda msg_type, payload, flags=0, force_zero=False: sent.append(
        (msg_type, payload, flags, force_zero)
    )

    auth_ok_payload = b"\x11" * 16 + struct.pack("<I", 0)
    ready_payload = bytearray(20)
    ready_payload[0] = 0
    struct.pack_into("<Q", ready_payload, 4, 0)
    struct.pack_into("<Q", ready_payload, 12, 0)

    queue = [
        (MessageHeader(MessageType.AUTH_OK, 0, len(auth_ok_payload), 1, b"\xAA" * 16, 0), auth_ok_payload),
        (MessageHeader(MessageType.READY, 0, len(ready_payload), 2, b"\xAA" * 16, 0), bytes(ready_payload)),
    ]
    conn._recv_message = lambda: queue.pop(0)

    conn._startup_and_auth()

    startup = sent[0]
    assert startup[0] == MessageType.STARTUP
    features = struct.unpack_from("<Q", startup[1], 8)[0]
    assert features & FEATURE_COMPRESSION
    assert (features & FEATURE_STREAMING) == 0


def test_startup_and_auth_negotiates_scram_sha_512_and_records_resolved_auth_context(monkeypatch):
    sent = []
    queue = [
        (
            MessageHeader(MessageType.AUTH_REQUEST, 0, 4, 1, b"\x00" * 16, 0),
            make_auth_request_payload(AuthMethod.SCRAM_SHA_512),
        ),
        (
            MessageHeader(MessageType.AUTH_CONTINUE, 0, 8 + len(b"server-first"), 2, b"\x00" * 16, 0),
            make_auth_continue_payload(AuthMethod.SCRAM_SHA_512, b"server-first"),
        ),
        (
            MessageHeader(MessageType.AUTH_OK, 0, 20, 3, b"\xAA" * 16, 0),
            make_auth_ok_payload(),
        ),
        (
            MessageHeader(MessageType.READY, 0, len(bytes(20)), 4, b"\xAA" * 16, 0),
            bytes(20),
        ),
    ]

    class FakeScram:
        def __init__(self, username: str, digest: str = "sha256"):
            assert username == "alice"
            assert digest == "sha512"

        def client_first_message(self) -> str:
            return "client-first"

        def handle_server_first(self, password: str, server_first: str) -> str:
            assert password == "secret"
            assert server_first == "server-first"
            return "client-final"

        def verify_server_final(self, _server_final: str) -> None:
            return None

    monkeypatch.setattr(connection_mod, "ScramExchange", FakeScram)

    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(user="alice", password="secret", database="db1")
    conn._parameters = {}
    conn._authed = False
    conn._attachment_id = b"\x00" * 16
    conn._txn_id = 0
    conn._runtime_txn_active = False
    conn._resolved_auth_context = connection_mod.ResolvedAuthContext(
        ingress_mode="direct",
        resolved_auth_method=None,
        resolved_auth_plugin_id=None,
        manager_authenticated=False,
        attached=False,
    )
    conn._send_message = lambda msg_type, payload, flags=0, force_zero=False: sent.append(
        (msg_type, payload, flags, force_zero)
    )
    conn._recv_message = lambda: queue.pop(0)
    conn._handle_async = lambda *_args, **_kwargs: False
    conn._apply_runtime_ready_state = lambda *_args, **_kwargs: None
    conn._apply_runtime_txn_id = lambda *_args, **_kwargs: None

    conn._startup_and_auth()

    assert sent[1] == (MessageType.AUTH_RESPONSE, b"client-first", 0, True)
    assert sent[2] == (MessageType.AUTH_RESPONSE, b"client-final", 0, True)
    resolved = conn.get_resolved_auth_context()
    assert resolved.resolved_auth_method == "SCRAM_SHA_512"
    assert resolved.resolved_auth_plugin_id == "scratchbird.auth.scram_sha_512"


def test_startup_and_auth_sends_token_auth_payload_and_records_resolved_auth_context():
    sent = []
    queue = [
        (
            MessageHeader(MessageType.AUTH_REQUEST, 0, 4, 1, b"\x00" * 16, 0),
            make_auth_request_payload(AuthMethod.TOKEN),
        ),
        (
            MessageHeader(MessageType.AUTH_OK, 0, 20, 2, b"\xAA" * 16, 0),
            make_auth_ok_payload(),
        ),
        (
            MessageHeader(MessageType.READY, 0, len(bytes(20)), 3, b"\xAA" * 16, 0),
            bytes(20),
        ),
    ]

    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(user="alice", database="db1", auth_token="opaque-token")
    conn._parameters = {}
    conn._authed = False
    conn._attachment_id = b"\x00" * 16
    conn._txn_id = 0
    conn._runtime_txn_active = False
    conn._resolved_auth_context = connection_mod.ResolvedAuthContext(
        ingress_mode="direct",
        resolved_auth_method=None,
        resolved_auth_plugin_id=None,
        manager_authenticated=False,
        attached=False,
    )
    conn._send_message = lambda msg_type, payload, flags=0, force_zero=False: sent.append(
        (msg_type, payload, flags, force_zero)
    )
    conn._recv_message = lambda: queue.pop(0)
    conn._handle_async = lambda *_args, **_kwargs: False
    conn._apply_runtime_ready_state = lambda *_args, **_kwargs: None
    conn._apply_runtime_txn_id = lambda *_args, **_kwargs: None

    conn._startup_and_auth()

    assert sent[1] == (MessageType.AUTH_RESPONSE, b"opaque-token", 0, True)
    resolved = conn.get_resolved_auth_context()
    assert resolved.resolved_auth_method == "TOKEN"
    assert resolved.resolved_auth_plugin_id == "scratchbird.auth.authkey_token"


def test_startup_and_auth_fails_closed_for_peer_auth():
    queue = [
        (
            MessageHeader(MessageType.AUTH_REQUEST, 0, 4, 1, b"\x00" * 16, 0),
            make_auth_request_payload(AuthMethod.PEER),
        )
    ]

    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(user="alice", database="db1")
    conn._parameters = {}
    conn._authed = False
    conn._attachment_id = b"\x00" * 16
    conn._txn_id = 0
    conn._runtime_txn_active = False
    conn._resolved_auth_context = connection_mod.ResolvedAuthContext(
        ingress_mode="direct",
        resolved_auth_method=None,
        resolved_auth_plugin_id=None,
        manager_authenticated=False,
        attached=False,
    )
    conn._send_message = lambda *_args, **_kwargs: None
    conn._recv_message = lambda: queue.pop(0)
    conn._handle_async = lambda *_args, **_kwargs: False

    with pytest.raises(errors.NotSupportedError, match="0A000"):
        conn._startup_and_auth()


def test_read_exact_maps_socket_timeout_to_operational_error():
    conn = Connection.__new__(Connection)

    class _TimeoutSocket:
        def recv(self, _size):
            raise socket.timeout("timed out")

    conn._socket = _TimeoutSocket()
    with pytest.raises(errors.OperationalError, match="08006"):
        conn._read_exact(8)


def test_read_exact_maps_timeout_to_query_canceled_when_cancel_pending():
    conn = Connection.__new__(Connection)

    class _TimeoutSocket:
        def __init__(self):
            self._timeout = None

        def recv(self, _size):
            raise socket.timeout("timed out")

        def gettimeout(self):
            return self._timeout

        def settimeout(self, value):
            self._timeout = value

    conn._socket = _TimeoutSocket()
    conn._cancel_requested = True
    conn._cancel_socket_timeout = None

    with pytest.raises(errors.OperationalError, match="57014"):
        conn._read_exact(8)
    assert conn._cancel_requested is False


def test_read_exact_maps_oserror_to_operational_error():
    conn = Connection.__new__(Connection)

    class _FailingSocket:
        def recv(self, _size):
            raise OSError("socket read failure")

    conn._socket = _FailingSocket()
    with pytest.raises(errors.OperationalError, match="08006"):
        conn._read_exact(8)


def test_connect_maps_timeout_to_operational_error(monkeypatch):
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(user="alice", database="db1")

    def fail_create_connection(*_args, **_kwargs):
        raise TimeoutError("connect timed out")

    monkeypatch.setattr(connection_mod.socket, "create_connection", fail_create_connection)

    with pytest.raises(errors.OperationalError, match="08001"):
        conn._connect()


def test_connect_maps_oserror_to_operational_error(monkeypatch):
    conn = Connection.__new__(Connection)
    conn._config = ConnectionConfig(user="alice", database="db1")

    def fail_create_connection(*_args, **_kwargs):
        raise OSError("connection refused")

    monkeypatch.setattr(connection_mod.socket, "create_connection", fail_create_connection)

    with pytest.raises(errors.OperationalError, match="08001"):
        conn._connect()
