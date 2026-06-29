# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

import scratchbird_native


def _require(condition: Bool, message: String) raises:
    if not condition:
        raise Error(message)


def _assert_connect_guard(dsn: String, expected_sqlstate: String, expected_fragment: String) raises:
    var cfg = scratchbird_native.ScratchBirdConfig(dsn)
    try:
        _ = scratchbird_native.connect(cfg)
        raise Error("expected connect guard to reject DSN")
    except e:
        var message = String(e)
        var sqlstate = scratchbird_native.extract_sqlstate(message)
        _require(
            sqlstate == expected_sqlstate,
            "expected sqlstate '" + expected_sqlstate + "', got '" + sqlstate + "'",
        )
        _require(
            expected_fragment in message,
            "expected guard message containing '" + expected_fragment + "', got '" + message + "'",
        )


def _assert_metadata_guard(collection_name: String, expected_sqlstate: String, expected_fragment: String) raises:
    try:
        _ = scratchbird_native.normalize_metadata_collection_name(collection_name)
        raise Error("expected metadata guard to reject collection")
    except e:
        var message = String(e)
        var sqlstate = scratchbird_native.extract_sqlstate(message)
        _require(
            sqlstate == expected_sqlstate,
            "expected metadata sqlstate '" + expected_sqlstate + "', got '" + sqlstate + "'",
        )
        _require(
            expected_fragment in message,
            "expected metadata guard containing '" + expected_fragment + "', got '" + message + "'",
        )


def _assert_metadata_restriction_guard(
    collection_name: String,
    restriction_key: String,
    expected_sqlstate: String,
    expected_fragment: String,
) raises:
    try:
        _ = scratchbird_native.resolve_metadata_collection_query_restricted(
            collection_name,
            restriction_key,
            "x",
        )
        raise Error("expected metadata restriction guard to reject restriction")
    except e:
        var message = String(e)
        var sqlstate = scratchbird_native.extract_sqlstate(message)
        _require(
            sqlstate == expected_sqlstate,
            "expected metadata restriction sqlstate '" + expected_sqlstate + "', got '" + sqlstate + "'",
        )
        _require(
            expected_fragment in message,
            "expected metadata restriction guard containing '" + expected_fragment + "', got '" + message + "'",
        )


def _assert_metadata_restriction_count_guard(collection_name: String) raises:
    var keys = List[String]()
    keys.append("schema")
    var values = List[String]()
    try:
        _ = scratchbird_native.resolve_metadata_collection_query_restricted_multi(
            collection_name,
            keys,
            values,
        )
        raise Error("expected metadata restriction count guard to reject mismatch")
    except e:
        var message = String(e)
        var sqlstate = scratchbird_native.extract_sqlstate(message)
        _require(
            sqlstate == "07001",
            "expected metadata restriction count guard sqlstate '07001', got '" + sqlstate + "'",
        )


def _assert_config_session_pooling_manager_extensions() raises:
    var cfg_session_overrides = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&role=app_role&application_name=app_client&autocommit=false&readonly=true&current_schema=analytics&default_row_fetch_size=128"
    )
    _require(cfg_session_overrides.role == "app_role", "role parse mismatch")
    _require(cfg_session_overrides.application_name == "app_client", "application_name parse mismatch")
    _require(not cfg_session_overrides.auto_commit, "autocommit parse mismatch")
    _require(cfg_session_overrides.read_only, "readonly parse mismatch")
    _require(cfg_session_overrides.current_schema == "analytics", "current_schema parse mismatch")
    _require(cfg_session_overrides.default_row_fetch_size == 128, "default_row_fetch_size parse mismatch")
    var cfg_prepare_threshold_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&preparethreshold=7"
    )
    _require(cfg_prepare_threshold_alias.prepare_threshold == 7, "preparethreshold alias mismatch")
    var cfg_rewrite_batched_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&rewritebatchedinserts=true"
    )
    _require(cfg_rewrite_batched_alias.rewrite_batched_inserts, "rewritebatchedinserts alias mismatch")
    var cfg_logger_aliases = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&logLevel=debug&log_file=/tmp/sb_mojo_native.log"
    )
    _require(cfg_logger_aliases.logger_level == "DEBUG", "logLevel alias normalization mismatch")
    _require(cfg_logger_aliases.logger_file == "/tmp/sb_mojo_native.log", "log_file alias mismatch")
    var cfg_session_aliases = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&applicationname=alias_client&auto_commit=false&read_only=true&searchPath=ops&fetchSize=64"
    )
    _require(cfg_session_aliases.application_name == "alias_client", "applicationname alias mismatch")
    _require(not cfg_session_aliases.auto_commit, "auto_commit alias mismatch")
    _require(cfg_session_aliases.read_only, "read_only alias mismatch")
    _require(cfg_session_aliases.current_schema == "ops", "searchPath alias mismatch")
    _require(cfg_session_aliases.default_row_fetch_size == 64, "fetchSize alias mismatch")
    var cfg_session_bool_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&autocommit=false&auto_commit=true&readonly=true&read_only=false"
    )
    _require(cfg_session_bool_precedence.auto_commit, "autocommit alias last-key precedence mismatch")
    _require(not cfg_session_bool_precedence.read_only, "readonly alias last-key precedence mismatch")
    var cfg_session_jdbc_aliases = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&currentSchema=finance&defaultRowFetchSize=96"
    )
    _require(cfg_session_jdbc_aliases.current_schema == "finance", "currentSchema alias mismatch")
    _require(cfg_session_jdbc_aliases.default_row_fetch_size == 96, "defaultRowFetchSize alias mismatch")
    var cfg_metadata_expand_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&metadata_expand_schema_parents=true"
    )
    _require(cfg_metadata_expand_alias.metadata_expand_schema_parents, "metadata_expand_schema_parents alias mismatch")
    var cfg_pooling_overrides = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&tcpkeepalive=false&pooling=false&min_pool_size=2&maxpoolsize=12&connection_lifetime=45"
    )
    _require(not cfg_pooling_overrides.tcp_keepalive, "tcpkeepalive parse mismatch")
    _require(not cfg_pooling_overrides.pooling_enabled, "pooling parse mismatch")
    _require(cfg_pooling_overrides.min_pool_size == 2, "min_pool_size parse mismatch")
    _require(cfg_pooling_overrides.max_pool_size == 12, "maxpoolsize parse mismatch")
    _require(cfg_pooling_overrides.connection_lifetime_s == 45, "connection_lifetime parse mismatch")
    var cfg_manager_overrides = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&front_door_mode=manager_proxy&manager_auth_token=token_123&manager_username=mgr_user&manager_database=mgr_db&manager_connection_profile=mgr_profile&mcp_client_intent=mgr_intent&mcp_client_flags=7&mcp_auth_fast_path=false"
    )
    _require(cfg_manager_overrides.manager_auth_token == "token_123", "manager_auth_token parse mismatch")
    _require(cfg_manager_overrides.manager_username == "mgr_user", "manager_username parse mismatch")
    _require(cfg_manager_overrides.manager_database == "mgr_db", "manager_database parse mismatch")
    _require(cfg_manager_overrides.manager_connection_profile == "mgr_profile", "manager_connection_profile parse mismatch")
    _require(cfg_manager_overrides.manager_client_intent == "mgr_intent", "manager_client_intent alias mismatch")
    _require(cfg_manager_overrides.manager_client_flags == 7, "manager_client_flags alias mismatch")
    _require(not cfg_manager_overrides.manager_auth_fast_path, "manager_auth_fast_path alias mismatch")
    var cfg_auth_overrides = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connect_client_flags=257&auth_method_id=scratchbird.auth.proxy_principal_assertion&auth_method_payload=opaque&auth_payload_json=%7B%22subject%22%3A%22alice%22%7D&auth_payload_b64=YWJj&auth_provider_profile=corp_primary&auth_required_methods=SCRAM_SHA_256%2CTOKEN&auth_forbidden_methods=MD5&auth_require_channel_binding=true&workload_identity_token=jwt-token&proxy_principal_assertion=signed-assertion"
    )
    _require(cfg_auth_overrides.connect_client_flags == 257, "connect_client_flags parse mismatch")
    _require(
        cfg_auth_overrides.auth_method_id == "scratchbird.auth.proxy_principal_assertion",
        "auth_method_id parse mismatch",
    )
    _require(cfg_auth_overrides.auth_method_payload == "opaque", "auth_method_payload parse mismatch")
    _require(
        cfg_auth_overrides.auth_payload_json == "{\"subject\":\"alice\"}",
        "auth_payload_json parse mismatch",
    )
    _require(cfg_auth_overrides.auth_payload_b64 == "YWJj", "auth_payload_b64 parse mismatch")
    _require(cfg_auth_overrides.auth_provider_profile == "corp_primary", "auth_provider_profile parse mismatch")
    _require(
        cfg_auth_overrides.auth_required_methods == "SCRAM_SHA_256,TOKEN",
        "auth_required_methods parse mismatch",
    )
    _require(cfg_auth_overrides.auth_forbidden_methods == "MD5", "auth_forbidden_methods parse mismatch")
    _require(cfg_auth_overrides.auth_require_channel_binding, "auth_require_channel_binding parse mismatch")
    _require(
        cfg_auth_overrides.workload_identity_token == "jwt-token",
        "workload_identity_token parse mismatch",
    )
    _require(
        cfg_auth_overrides.proxy_principal_assertion == "signed-assertion",
        "proxy_principal_assertion parse mismatch",
    )
    var cfg_manager_defaults = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require"
    )
    _require(cfg_manager_defaults.manager_connection_profile == "SBsql", "manager_connection_profile default mismatch")
    _require(cfg_manager_defaults.manager_client_intent == "SBsql", "manager_client_intent default mismatch")
    _require(cfg_manager_defaults.manager_auth_fast_path, "manager_auth_fast_path default mismatch")
    var cfg_mcp_database_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&mcp_database=alias_mgr_db"
    )
    _require(cfg_mcp_database_alias.manager_database == "alias_mgr_db", "mcp_database alias mismatch")
    var cfg_ssl_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?ssl=disable"
    )
    _require(cfg_ssl_alias.sslmode == "disable", "ssl alias mismatch")
    var cfg_ssl_materials = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sslrootcert=/tmp/ca.pem&sslcert=/tmp/client.crt&sslkey=/tmp/client.key&sslpassword=secret"
    )
    _require(cfg_ssl_materials.ssl_root_cert == "/tmp/ca.pem", "sslrootcert alias mismatch")
    _require(cfg_ssl_materials.ssl_cert == "/tmp/client.crt", "sslcert alias mismatch")
    _require(cfg_ssl_materials.ssl_key == "/tmp/client.key", "sslkey alias mismatch")
    _require(cfg_ssl_materials.ssl_password == "secret", "sslpassword alias mismatch")
    var cfg_ssl_materials_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&ssl_root_cert=/tmp/ca_a.pem&sslrootcert=/tmp/ca_b.pem&ssl_cert=/tmp/client_a.crt&sslcert=/tmp/client_b.crt&ssl_key=/tmp/client_a.key&sslkey=/tmp/client_b.key&ssl_password=first&sslpassword=second"
    )
    _require(
        cfg_ssl_materials_precedence.ssl_root_cert == "/tmp/ca_b.pem",
        "ssl_root_cert alias last-key precedence mismatch",
    )
    _require(
        cfg_ssl_materials_precedence.ssl_cert == "/tmp/client_b.crt",
        "ssl_cert alias last-key precedence mismatch",
    )
    _require(
        cfg_ssl_materials_precedence.ssl_key == "/tmp/client_b.key",
        "ssl_key alias last-key precedence mismatch",
    )
    _require(
        cfg_ssl_materials_precedence.ssl_password == "second",
        "ssl_password alias last-key precedence mismatch",
    )
    var cfg_compression_none = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&compression=none"
    )
    _require(cfg_compression_none.compression == "off", "compression=none normalization mismatch")
    var cfg_compression_zstd = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&compression=zstd"
    )
    _require(cfg_compression_zstd.compression == "zstd", "compression=zstd parse mismatch")
    var cfg_compression_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&compression=off&compression=zstd"
    )
    _require(cfg_compression_precedence.compression == "zstd", "compression last-key precedence mismatch")
    var cfg_query_decode = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&application_name=app%20client&role=ops+admin&manager_auth_token=a%2Bb"
    )
    _require(cfg_query_decode.application_name == "app client", "application_name percent decode mismatch")
    _require(cfg_query_decode.role == "ops admin", "role plus decode mismatch")
    _require(cfg_query_decode.manager_auth_token == "a+b", "manager_auth_token percent decode mismatch")
    var cfg_binarytransfer_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&binarytransfer=false"
    )
    _require(not cfg_binarytransfer_alias.binary_transfer, "binarytransfer alias mismatch")
    var binarytransfer_alias_conn = scratchbird_native.connect(cfg_binarytransfer_alias)
    binarytransfer_alias_conn.close()
    var compression_zstd_conn = scratchbird_native.connect(cfg_compression_zstd)
    compression_zstd_conn.close()

    var manager_overrides_conn = scratchbird_native.connect(cfg_manager_overrides)
    manager_overrides_conn.close()


def _assert_config_parsing_extensions() raises:
    var cfg_default_port = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost/testdb?sslmode=require&binary_transfer=true"
    )
    _require(cfg_default_port.port == 3092, "default port parse mismatch")
    var cfg_host_omitted = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@/testdb?sslmode=require"
    )
    _require(cfg_host_omitted.host == "localhost", "host omitted default mismatch")
    _require(cfg_host_omitted.port == 3092, "host omitted default port mismatch")
    var cfg_ipv6 = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@[::1]:3092/testdb?sslmode=require"
    )
    _require(cfg_ipv6.host == "::1", "ipv6 host parse mismatch")
    _require(cfg_ipv6.port == 3092, "ipv6 port parse mismatch")
    var cfg_endpoint_override = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&host=proxy.local&port=4100"
    )
    _require(cfg_endpoint_override.host == "proxy.local", "query host override parse mismatch")
    _require(cfg_endpoint_override.port == 4100, "query port override parse mismatch")
    var cfg_timeout_override = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connecttimeout=11&socket_timeout=22&logintimeout=33&acquiretimeout=44"
    )
    _require(cfg_timeout_override.connect_timeout_s == 11, "connect timeout override mismatch")
    _require(cfg_timeout_override.socket_timeout_s == 22, "socket timeout override mismatch")
    _require(cfg_timeout_override.login_timeout_s == 33, "login timeout override mismatch")
    _require(cfg_timeout_override.acquire_timeout_s == 44, "acquire timeout override mismatch")
    var cfg_pooling_acquire_timeout = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&poolingacquiretimeout=52"
    )
    _require(
        cfg_pooling_acquire_timeout.acquire_timeout_s == 52,
        "pooling acquire timeout alias mismatch",
    )
    var cfg_timeout_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connect_timeout=5&connecttimeout=6&socket_timeout=7&sockettimeout=8&login_timeout=9&logintimeout=10&acquire_timeout=11&poolingacquiretimeout=12"
    )
    _require(cfg_timeout_precedence.connect_timeout_s == 6, "connect timeout last-alias precedence mismatch")
    _require(cfg_timeout_precedence.socket_timeout_s == 8, "socket timeout last-alias precedence mismatch")
    _require(cfg_timeout_precedence.login_timeout_s == 10, "login timeout last-alias precedence mismatch")
    _require(cfg_timeout_precedence.acquire_timeout_s == 12, "acquire timeout last-alias precedence mismatch")
    var cfg_endpoint_identity_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://localhost:3092/testdb?sslmode=require&user=u1&username=u2&pguser=u3&password=p1&passwd=p2&pgpassword=p3&host=h1&hostname=h2&servername=h3&pghost=h4&database=db1&dbname=db2&databaseName=db3&pgdatabase=db4&port=4100&portNumber=4101&pgport=4102"
    )
    _require(cfg_endpoint_identity_precedence.user == "u3", "user alias last-key precedence mismatch")
    _require(cfg_endpoint_identity_precedence.password == "p3", "password alias last-key precedence mismatch")
    _require(cfg_endpoint_identity_precedence.host == "h4", "host alias last-key precedence mismatch")
    _require(cfg_endpoint_identity_precedence.database == "db4", "database alias last-key precedence mismatch")
    _require(cfg_endpoint_identity_precedence.port == 4102, "port alias last-key precedence mismatch")
    var cfg_manager_token_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&front_door_mode=managerproxy&manager_auth_token=token_a&mcp_auth_token=token_b"
    )
    _require(
        cfg_manager_token_precedence.manager_auth_token == "token_b",
        "manager token alias last-key precedence mismatch",
    )
    var cfg_protocol_parser = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&parser=native"
    )
    _require(cfg_protocol_parser.protocol == "native", "parser alias protocol mismatch")
    var cfg_protocol_dialect = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&dialect=native"
    )
    _require(cfg_protocol_dialect.protocol == "native", "dialect alias protocol mismatch")
    var cfg_protocol_scratchbird = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&protocol=scratchbird"
    )
    _require(cfg_protocol_scratchbird.protocol == "native", "scratchbird protocol normalization mismatch")
    var cfg_protocol_scratchbird_native = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&protocol=scratchbird-native"
    )
    _require(cfg_protocol_scratchbird_native.protocol == "native", "scratchbird-native protocol normalization mismatch")
    var cfg_mode_precedence = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&front_door_mode=direct&connection_mode=manager_proxy&ingress_mode=managed"
    )
    _require(cfg_mode_precedence.front_door_mode == "manager_proxy", "front_door_mode query-order precedence mismatch")
    var cfg_mode_last_front_door = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&ingress_mode=managed&connection_mode=manager_proxy&front_door_mode=direct"
    )
    _require(cfg_mode_last_front_door.front_door_mode == "direct", "front_door_mode last-write precedence mismatch")
    var cfg_frontdoormode_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&frontdoormode=managed&manager_auth_token=mode_token"
    )
    _require(cfg_frontdoormode_alias.front_door_mode == "manager_proxy", "frontdoormode alias normalization mismatch")
    var cfg_connection_mode_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connection_mode=manager-proxy&manager_auth_token=mode_token"
    )
    _require(cfg_connection_mode_alias.front_door_mode == "manager_proxy", "connection_mode alias normalization mismatch")
    var cfg_ingress_mode_alias = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&ingress_mode=managerproxy&manager_auth_token=mode_token"
    )
    _require(cfg_ingress_mode_alias.front_door_mode == "manager_proxy", "ingress_mode alias normalization mismatch")
    var cfg_password_colon = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pa:ss@localhost:3092/testdb?sslmode=require"
    )
    _require(cfg_password_colon.password == "pa:ss", "password-with-colon parse mismatch")
    var cfg_credential_override = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&user=api_user&password=api_pass"
    )
    _require(cfg_credential_override.user == "api_user", "query user override mismatch")
    _require(cfg_credential_override.password == "api_pass", "query password override mismatch")
    var cfg_credential_override_hostonly = scratchbird_native.ScratchBirdConfig(
        "scratchbird://localhost:3092/testdb?sslmode=require&user=host_user&password=host_pass"
    )
    _require(cfg_credential_override_hostonly.user == "host_user", "host-only user override mismatch")
    _require(cfg_credential_override_hostonly.password == "host_pass", "host-only password override mismatch")
    var cfg_alias_override = scratchbird_native.ScratchBirdConfig(
        "scratchbird://localhost:3092/?sslmode=require&username=alias_user&passwd=alias_pass&hostname=alias.host&port=4101&dbname=alias_db"
    )
    _require(cfg_alias_override.user == "alias_user", "username alias mismatch")
    _require(cfg_alias_override.password == "alias_pass", "passwd alias mismatch")
    _require(cfg_alias_override.host == "alias.host", "hostname alias mismatch")
    _require(cfg_alias_override.port == 4101, "alias port mismatch")
    _require(cfg_alias_override.database == "alias_db", "dbname alias mismatch")
    var cfg_pg_alias_override = scratchbird_native.ScratchBirdConfig(
        "scratchbird://localhost:3092/?sslmode=require&pguser=pg_user&pgpassword=pg_pass&pghost=pg.host&pgport=4102&pgdatabase=pg_db"
    )
    _require(cfg_pg_alias_override.user == "pg_user", "pguser alias mismatch")
    _require(cfg_pg_alias_override.password == "pg_pass", "pgpassword alias mismatch")
    _require(cfg_pg_alias_override.host == "pg.host", "pghost alias mismatch")
    _require(cfg_pg_alias_override.port == 4102, "pgport alias mismatch")
    _require(cfg_pg_alias_override.database == "pg_db", "pgdatabase alias mismatch")
    var cfg_jdbc_alias_override = scratchbird_native.ScratchBirdConfig(
        "scratchbird://localhost:3092/?sslmode=require&user=jdbc_user&password=jdbc_pass&servername=jdbc.host&portNumber=4103&databaseName=jdbc_db"
    )
    _require(cfg_jdbc_alias_override.host == "jdbc.host", "servername alias mismatch")
    _require(cfg_jdbc_alias_override.port == 4103, "portnumber alias mismatch")
    _require(cfg_jdbc_alias_override.database == "jdbc_db", "databasename alias mismatch")
    _assert_config_session_pooling_manager_extensions()

    var manager_conn_mode_conn = scratchbird_native.connect(cfg_connection_mode_alias)
    manager_conn_mode_conn.close()
    var manager_ingress_mode_conn = scratchbird_native.connect(cfg_ingress_mode_alias)
    manager_ingress_mode_conn.close()
    var manager_frontdoor_alias_conn = scratchbird_native.connect(cfg_frontdoormode_alias)
    manager_frontdoor_alias_conn.close()
    var host_omitted_conn = scratchbird_native.connect(cfg_host_omitted)
    host_omitted_conn.close()
    var credential_override_conn = scratchbird_native.connect(cfg_credential_override)
    credential_override_conn.close()
    var credential_override_hostonly_conn = scratchbird_native.connect(cfg_credential_override_hostonly)
    credential_override_hostonly_conn.close()
    var alias_override_conn = scratchbird_native.connect(cfg_alias_override)
    alias_override_conn.close()
    var pg_alias_override_conn = scratchbird_native.connect(cfg_pg_alias_override)
    pg_alias_override_conn.close()
    var jdbc_alias_override_conn = scratchbird_native.connect(cfg_jdbc_alias_override)
    jdbc_alias_override_conn.close()
    var manager_token_precedence_conn = scratchbird_native.connect(cfg_manager_token_precedence)
    manager_token_precedence_conn.close()


def main() raises:
    var cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&binary_transfer=true"
    )
    _require(cfg.user == "user", "user parse mismatch")
    _require(cfg.password == "pass", "password parse mismatch")
    _require(cfg.host == "localhost", "host parse mismatch")
    _require(cfg.port == 3092, "port parse mismatch")
    _require(cfg.database == "testdb", "database parse mismatch")
    _require(cfg.connect_timeout_s == 30, "connect timeout default mismatch")
    _require(cfg.socket_timeout_s == 0, "socket timeout default mismatch")
    _require(cfg.login_timeout_s == 30, "login timeout default mismatch")
    _require(cfg.acquire_timeout_s == 30, "acquire timeout default mismatch")
    _require(cfg.auto_commit, "autocommit default mismatch")
    _require(not cfg.read_only, "readonly default mismatch")
    _require(cfg.current_schema == "users.public", "current_schema default mismatch")
    _require(cfg.default_row_fetch_size == 0, "default_row_fetch_size default mismatch")
    _require(cfg.prepare_threshold == 5, "prepare_threshold default mismatch")
    _require(not cfg.rewrite_batched_inserts, "rewrite_batched_inserts default mismatch")
    _require(cfg.logger_level == "OFF", "logger_level default mismatch")
    _require(cfg.logger_file == "", "logger_file default mismatch")
    _require(cfg.ssl_root_cert == "", "ssl_root_cert default mismatch")
    _require(cfg.ssl_cert == "", "ssl_cert default mismatch")
    _require(cfg.ssl_key == "", "ssl_key default mismatch")
    _require(cfg.ssl_password == "", "ssl_password default mismatch")
    _require(not cfg.metadata_expand_schema_parents, "metadata_expand_schema_parents default mismatch")
    _require(cfg.tcp_keepalive, "tcpkeepalive default mismatch")
    _require(cfg.pooling_enabled, "pooling default mismatch")
    _require(cfg.min_pool_size == 0, "min_pool_size default mismatch")
    _require(cfg.max_pool_size == 10, "max_pool_size default mismatch")
    _require(cfg.connection_lifetime_s == 30, "connection_lifetime default mismatch")
    _require(cfg.manager_client_flags == 0, "manager_client_flags default mismatch")
    _require(cfg.connect_client_flags == 256, "connect_client_flags default mismatch")
    _require(cfg.auth_method_id == "", "auth_method_id default mismatch")
    _require(cfg.auth_method_payload == "", "auth_method_payload default mismatch")
    _require(cfg.auth_payload_json == "", "auth_payload_json default mismatch")
    _require(cfg.auth_payload_b64 == "", "auth_payload_b64 default mismatch")
    _require(cfg.auth_provider_profile == "", "auth_provider_profile default mismatch")
    _require(cfg.auth_required_methods == "", "auth_required_methods default mismatch")
    _require(cfg.auth_forbidden_methods == "", "auth_forbidden_methods default mismatch")
    _require(not cfg.auth_require_channel_binding, "auth_require_channel_binding default mismatch")
    _require(cfg.workload_identity_token == "", "workload_identity_token default mismatch")
    _require(cfg.proxy_principal_assertion == "", "proxy_principal_assertion default mismatch")
    _require(cfg.protocol == "native", "protocol default mismatch")
    _assert_config_parsing_extensions()

    var conn = scratchbird_native.connect(cfg)
    _require(conn.connection_id == "user@localhost:3092/testdb", "connection_id format mismatch")
    _require(conn.ping(), "ping should return true")
    _require(conn.query("SELECT 1") == 1, "SELECT 1 should return 1")
    _require(conn.query("SELECT * FROM type_coverage") == 1, "type_coverage stub should return success")
    _require(conn.query("SELECT id FROM basic_table ORDER BY id") == 6, "basic_table query rowcount mismatch")
    conn.commit()
    conn.rollback()
    conn.begin()
    try:
        conn.begin()
        raise Error("expected nested transaction begin to fail")
    except e:
        _require("25001" in String(e), "nested transaction should report 25001")
    conn.rollback()
    conn.begin()
    var auto_savepoint = conn.set_savepoint()
    _require(auto_savepoint == "sp_1", "generated savepoint name mismatch")
    _require(conn.set_savepoint("named_sp") == "named_sp", "named savepoint mismatch")
    _ = conn.set_savepoint("tail_sp")
    conn.rollback_to_savepoint("named_sp")
    try:
        conn.release_savepoint("tail_sp")
        raise Error("expected rolled-back savepoint release to fail")
    except e:
        _require("3B001" in String(e), "missing savepoint should report 3B001")
    conn.release_savepoint("named_sp")
    conn.commit()
    try:
        _ = conn.set_savepoint()
        raise Error("expected inactive savepoint guard")
    except e:
        _require("25000" in String(e), "inactive savepoint should report 25000")
    conn.commit()
    var p1 = List[String]()
    p1.append("42")
    _require(conn.query_with_params("SELECT $1::INTEGER", p1) == 42, "single-parameter query mismatch")
    var p2 = List[String]()
    p2.append("5")
    p2.append("7")
    _require(conn.query_with_params("SELECT $1::INTEGER, $2::INTEGER", p2) == 12, "two-parameter query mismatch")
    var stmt = conn.prepare("SELECT $1::INTEGER, $2::INTEGER")
    _require(stmt.execute(p2) == 12, "prepared execute mismatch")
    try:
        _ = conn.query_with_params("SELECT $1::INTEGER, $2::INTEGER", p1)
        raise Error("expected parameter mismatch")
    except e:
        _require("07001" in String(e), "parameter mismatch should include 07001")
    var p_bad = List[String]()
    p_bad.append("not_int")
    var p_bad_stmt = List[String]()
    p_bad_stmt.append("not_int")
    p_bad_stmt.append("2")
    try:
        _ = conn.query_with_params("SELECT $1::INTEGER", p_bad)
        raise Error("expected invalid integer parameter guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "22023",
            "invalid parameter guard should expose 22023",
        )
    try:
        _ = stmt.execute(p1)
        raise Error("expected prepared mismatch")
    except e:
        _require("07001" in String(e), "prepared mismatch should include 07001")
    try:
        _ = stmt.execute(p_bad_stmt)
        raise Error("expected prepared invalid integer guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "22023",
            "prepared invalid parameter guard should expose 22023",
        )
    stmt.close()
    try:
        _ = stmt.execute(p2)
        raise Error("expected closed statement guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "HY010",
            "closed statement guard should expose HY010",
        )
    stmt.close()
    _require(
        conn.query_metadata("table") == scratchbird_native.METADATA_TABLES_QUERY,
        "metadata table alias mismatch",
    )
    _require(
        conn.query_metadata("schemas") == scratchbird_native.METADATA_SCHEMAS_QUERY,
        "metadata schemas query mismatch",
    )
    _require(
        conn.query_metadata("index_columns") == scratchbird_native.METADATA_INDEX_COLUMNS_QUERY,
        "metadata index_columns query mismatch",
    )
    _require(
        conn.query_metadata("typeinfo") == scratchbird_native.METADATA_TYPE_INFO_QUERY,
        "metadata typeinfo alias mismatch",
    )
    _require(
        conn.query_metadata("routines") == scratchbird_native.METADATA_ROUTINES_QUERY,
        "metadata routines query mismatch",
    )
    _require(
        conn.query_metadata_rows("table") == 1,
        "metadata table execution rowcount mismatch",
    )
    _require(
        conn.query_metadata_rows("typeinfo") == 1,
        "metadata typeinfo execution rowcount mismatch",
    )
    var restricted_tables = conn.query_metadata_restricted("table", "name", "orders")
    _require(
        restricted_tables
        == "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 AND table_name = 'orders' ORDER BY table_name",
        "restricted metadata table query mismatch",
    )
    _require(
        conn.query_metadata_rows_restricted("table", "name", "orders") == 1,
        "restricted metadata table execution rowcount mismatch",
    )
    var multi_keys = List[String]()
    multi_keys.append("schema")
    multi_keys.append("table")
    var multi_values = List[String]()
    multi_values.append("public")
    multi_values.append("orders")
    var multi_tables = conn.query_metadata_restricted_multi("tables", multi_keys, multi_values)
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name = 'public')" in multi_tables,
        "multi restriction query should include schema predicate",
    )
    _require(
        "table_name = 'orders'" in multi_tables,
        "multi restriction query should include table predicate",
    )
    _require(
        conn.query_metadata_rows_restricted_multi("tables", multi_keys, multi_values) == 1,
        "multi restriction rowcount mismatch",
    )
    var wildcard_tables = conn.query_metadata_restricted("tables", "table", "ord%")
    _require(
        "table_name LIKE 'ord%'" in wildcard_tables,
        "table wildcard restriction should use LIKE predicate",
    )
    var escaped_wildcard_tables = conn.query_metadata_restricted("tables", "table", "ord\\%")
    _require(
        "table_name LIKE 'ord\\%' ESCAPE '\\'" in escaped_wildcard_tables,
        "escaped table wildcard restriction should preserve ESCAPE semantics",
    )
    _require(
        conn.query_metadata_rows_restricted("tables", "table", "ord%") == 1,
        "table wildcard restriction rowcount mismatch",
    )
    var restricted_schema = conn.query_metadata_restricted("schema", "schema", "acme'schema")
    _require(
        "schema_name = 'acme''schema'" in restricted_schema,
        "restricted metadata schema query should escape SQL literals",
    )
    var null_restricted_schema = conn.query_metadata_restricted("schema", "schema", "null")
    _require(
        "schema_name IS NULL" in null_restricted_schema,
        "null schema restriction should emit IS NULL predicate",
    )
    var restricted_columns_schema = conn.query_metadata_restricted("columns", "schema", "public")
    _require(
        "table_id IN (SELECT t.table_id FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE s.schema_name = 'public')"
        in restricted_columns_schema,
        "columns schema restriction should map through table-schema subquery",
    )
    var wildcard_columns_schema = conn.query_metadata_restricted("columns", "schema", "pub%")
    _require(
        "s.schema_name LIKE 'pub%'" in wildcard_columns_schema,
        "columns schema wildcard restriction should use LIKE predicate",
    )
    var escaped_wildcard_columns_schema = conn.query_metadata_restricted("columns", "schema", "pub\\_%")
    _require(
        "s.schema_name LIKE 'pub\\_%' ESCAPE '\\'" in escaped_wildcard_columns_schema,
        "columns escaped wildcard restriction should preserve ESCAPE semantics",
    )
    _require(
        conn.query_metadata_rows_restricted("columns", "schema", "public") == 1,
        "columns schema restriction rowcount mismatch",
    )
    var restricted_indexes_table = conn.query_metadata_restricted("indexes", "table", "orders")
    _require(
        "table_id IN (SELECT table_id FROM sys.tables WHERE table_name = 'orders')" in restricted_indexes_table,
        "indexes table restriction should map through table-name subquery",
    )
    var restricted_index_columns_table = conn.query_metadata_restricted("index_columns", "table", "orders")
    _require(
        "index_id IN (SELECT i.index_id FROM sys.indexes i JOIN sys.tables t ON t.table_id = i.table_id WHERE t.table_name = 'orders')"
        in restricted_index_columns_table,
        "index_columns table restriction should map through index-table subquery",
    )
    var restricted_tables_catalog = conn.query_metadata_restricted("tables", "catalog", "public")
    _require(
        "schema_id IN (SELECT schema_id FROM sys.schemas WHERE schema_name = 'public')" in restricted_tables_catalog,
        "catalog restriction should normalize through schema predicate",
    )
    var restricted_index_columns_index = conn.query_metadata_restricted("index_columns", "index", "idx_orders")
    _require(
        "index_id IN (SELECT index_id FROM sys.indexes WHERE index_name LIKE 'idx_orders' ESCAPE '\\')" in restricted_index_columns_index,
        "index_columns index restriction should map through index-name subquery",
    )
    var restricted_constraints = conn.query_metadata_restricted("constraints", "constraint", "orders_pk")
    _require(
        "constraint_name LIKE 'orders_pk' ESCAPE '\\'" in restricted_constraints,
        "constraint restriction should target constraint_name",
    )
    var restricted_routines = conn.query_metadata_restricted("routines", "routine", "orders_upsert")
    _require(
        "routine_name LIKE 'orders_upsert' ESCAPE '\\'" in restricted_routines,
        "routine restriction should target routine_name",
    )
    var restricted_columns_type = conn.query_metadata_restricted("columns", "type", "INTEGER")
    _require(
        "data_type_name = 'INTEGER'" in restricted_columns_type,
        "type restriction should target data_type_name",
    )
    _require(
        conn.query_metadata_rows_restricted("routines", "schema", "public") == 1,
        "routines schema restriction rowcount mismatch",
    )
    _require(
        conn.query_metadata_restricted("tables", "table_name", "") == scratchbird_native.METADATA_TABLES_QUERY,
        "empty restriction value should not mutate metadata query",
    )
    _require(
        scratchbird_native.normalize_metadata_collection_name("column") == "columns",
        "metadata column alias mismatch",
    )
    _require(
        scratchbird_native.normalize_metadata_collection_name("foreignkey") == "foreign_keys",
        "metadata foreignkey alias mismatch",
    )
    _require(
        scratchbird_native.normalize_metadata_collection_name("tableprivileges") == "table_privileges",
        "metadata tableprivileges alias mismatch",
    )
    _require(
        scratchbird_native.normalize_metadata_restriction_key("TABLE_SCHEM") == "schema_name",
        "metadata restriction alias mismatch for TABLE_SCHEM",
    )
    _require(
        scratchbird_native.normalize_metadata_restriction_key("tableSchem") == "schema_name",
        "metadata restriction alias mismatch for tableSchem",
    )
    _require(
        scratchbird_native.normalize_metadata_restriction_key("tableCatalog") == "catalog_name",
        "metadata restriction alias mismatch for tableCatalog",
    )
    _require(
        scratchbird_native.normalize_metadata_restriction_key("column") == "column_name",
        "metadata restriction alias mismatch for column",
    )
    _require(
        scratchbird_native.normalize_metadata_restriction_key("dataTypeName") == "type_name",
        "metadata restriction alias mismatch for dataTypeName",
    )
    var duplicate_keys = List[String]()
    duplicate_keys.append("table")
    duplicate_keys.append("tableName")
    var duplicate_values = List[String]()
    duplicate_values.append("orders")
    duplicate_values.append("customers")
    var duplicate_sql = scratchbird_native.resolve_metadata_collection_query_restricted_multi(
        "tables",
        duplicate_keys,
        duplicate_values,
    )
    _require(
        "table_name = 'customers'" in duplicate_sql,
        "metadata duplicate-alias restriction should keep last table predicate",
    )
    _require(
        "table_name = 'orders'" not in duplicate_sql,
        "metadata duplicate-alias restriction should drop overridden table predicate",
    )
    var duplicate_empty_keys = List[String]()
    duplicate_empty_keys.append("table")
    duplicate_empty_keys.append("table_name")
    var duplicate_empty_values = List[String]()
    duplicate_empty_values.append("orders")
    duplicate_empty_values.append("")
    var duplicate_empty_sql = scratchbird_native.resolve_metadata_collection_query_restricted_multi(
        "tables",
        duplicate_empty_keys,
        duplicate_empty_values,
    )
    _require(
        duplicate_empty_sql == scratchbird_native.METADATA_TABLES_QUERY,
        "metadata duplicate-alias restriction should drop predicate when last value is empty",
    )
    _assert_metadata_guard("unsupported_collection", "0A000", "not supported")
    _assert_metadata_restriction_guard("tables", "unsupported_restriction", "0A000", "not supported")
    _assert_metadata_restriction_guard("tables", "column", "0A000", "not supported for 'tables'")
    _assert_metadata_restriction_count_guard("tables")

    var stream = conn.stream("SELECT id FROM basic_table ORDER BY id", 1)
    _require(stream.next(conn) == 1, "stream first row mismatch")
    _require(stream.next(conn) == 2, "stream second row mismatch")
    stream.close()
    try:
        _ = stream.next(conn)
        raise Error("expected closed stream to fail")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "HY010",
            "closed stream should report HY010",
        )

    var long_stream = conn.stream(
        "SELECT a.id FROM basic_table a, basic_table b, basic_table c, basic_table d, basic_table e",
        1,
    )
    _ = long_stream.next(conn)
    conn.cancel()
    try:
        _ = long_stream.next(conn)
        raise Error("expected cancelled stream to fail")
    except e:
        _require("57014" in String(e), "cancelled stream should report 57014")
    var post_cancel = conn.stream("SELECT id FROM basic_table ORDER BY id", 1)
    _require(post_cancel.next(conn) == 1, "post-cancel stream should recover on next operation")
    post_cancel.close()
    var metrics = conn.telemetry.get_metrics()
    _require(len(metrics) > 0 and "total_queries=" in metrics[0], "telemetry metrics should be recorded")
    _require("count=" in conn.telemetry.operation_metrics("query"), "query operation metrics should be recorded")
    _require(not conn.circuit_breaker.is_open(), "circuit breaker should remain closed")
    _require(conn.keepalive_tracker.last_activity_ms > 0, "keepalive tracker should mark activity")
    _require(conn.query_pipeline.completed_count() > 0, "pipeline should record completed requests")
    _require(conn.leak_detector.get_active_count() == 1, "leak detector should track active checkout")
    var active_stream_on_close = conn.stream("SELECT id FROM basic_table ORDER BY id", 1)

    var keepalive_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&keepalive_max_idle_before_check_ms=0"
    )
    var keepalive_conn = scratchbird_native.connect(keepalive_cfg)
    _ = keepalive_conn.query("SELECT 1")
    keepalive_conn.operation_clock_ms += 2
    _ = keepalive_conn.query("SELECT 1")
    _require(keepalive_conn.ping_count >= 1, "keepalive validation should trigger ping")
    keepalive_conn.close()

    var pipeline_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&pipeline_max_in_flight=0"
    )
    var pipeline_conn = scratchbird_native.connect(pipeline_cfg)
    try:
        _ = pipeline_conn.query("SELECT 1")
        raise Error("expected pipeline capacity guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "54000",
            "pipeline capacity guard should expose 54000",
        )
    pipeline_conn.close()

    var breaker_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&cb_failure_threshold=2"
    )
    var breaker_conn = scratchbird_native.connect(breaker_cfg)
    try:
        _ = breaker_conn.query("SELECT unsupported_query")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "0A000",
            "first breaker failure should preserve unsupported query sqlstate",
        )
    try:
        _ = breaker_conn.query("SELECT unsupported_query")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "0A000",
            "second breaker failure should preserve unsupported query sqlstate",
        )
    try:
        _ = breaker_conn.query("SELECT 1")
        raise Error("expected circuit breaker guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08006",
            "circuit breaker guard should expose 08006",
        )
    breaker_conn.close()

    var leak_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&leak_threshold_ms=0"
    )
    var leak_conn = scratchbird_native.connect(leak_cfg)
    _ = leak_conn.query("SELECT 1")
    leak_conn.close()
    _require(
        len(leak_conn.leak_detector.get_warnings()) > 0,
        "leak detector should record warning when threshold is zero",
    )

    var pipeline_auto_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&pipeline_auto_flush=true&pipeline_auto_flush_threshold=1"
    )
    var pipeline_auto_conn = scratchbird_native.connect(pipeline_auto_cfg)
    _ = pipeline_auto_conn.query("SELECT 1")
    _ = pipeline_auto_conn.query("SELECT 1")
    _require(pipeline_auto_conn.query_pipeline.pending_count() == 0, "auto-flush pipeline should not retain pending work")
    _require(
        pipeline_auto_conn.query_pipeline.completed_count() >= 2,
        "auto-flush pipeline should complete queued requests",
    )
    pipeline_auto_conn.close()

    var pipeline_manual_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&pipeline_auto_flush=false&pipeline_max_in_flight=2"
    )
    var pipeline_manual_conn = scratchbird_native.connect(pipeline_manual_cfg)
    _ = pipeline_manual_conn.query("SELECT 1")
    _ = pipeline_manual_conn.query("SELECT 1")
    _require(
        pipeline_manual_conn.query_pipeline.pending_count() == 2,
        "manual pipeline should retain pending requests until flush/close",
    )
    try:
        _ = pipeline_manual_conn.query("SELECT 1")
        raise Error("expected manual pipeline capacity guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "54000",
            "manual pipeline capacity guard should expose 54000",
        )
    pipeline_manual_conn.close()
    _require(
        pipeline_manual_conn.query_pipeline.completed_count() >= 2,
        "manual pipeline close should flush retained requests",
    )

    var breaker_recovery_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&cb_failure_threshold=1&cb_recovery_timeout_ms=2&cb_success_threshold=2&cb_half_open_max_requests=1"
    )
    var breaker_recovery_conn = scratchbird_native.connect(breaker_recovery_cfg)
    try:
        _ = breaker_recovery_conn.query("SELECT unsupported_query")
        raise Error("expected initial breaker failure")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "0A000",
            "initial breaker failure should preserve unsupported query sqlstate",
        )
    _require(breaker_recovery_conn.circuit_breaker.is_open(), "breaker should open after threshold failure")
    try:
        _ = breaker_recovery_conn.query("SELECT 1")
        raise Error("expected breaker-open guard before recovery timeout")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08006",
            "breaker-open guard should expose 08006",
        )
    breaker_recovery_conn.operation_clock_ms += 3
    _ = breaker_recovery_conn.query("SELECT 1")
    _require(
        breaker_recovery_conn.circuit_breaker.is_half_open(),
        "first recovery success should leave breaker half-open with success threshold > 1",
    )
    _ = breaker_recovery_conn.query("SELECT 1")
    _require(
        not breaker_recovery_conn.circuit_breaker.is_open(),
        "recovery successes should close breaker",
    )
    breaker_recovery_conn.close()

    try:
        _ = conn.query("SELECT unsupported_query")
        raise Error("expected unsupported query to fail")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "0A000",
            "unsupported query should expose 0A000",
        )

    try:
        _ = conn.stream("SELECT unsupported_stream_query", 1)
        raise Error("expected unsupported stream query to fail")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "0A000",
            "unsupported stream query should expose 0A000",
        )
    conn.close()
    _require(not conn.ping(), "ping should return false after close")
    _require(conn.leak_detector.get_active_count() == 0, "leak detector should release checkout on close")
    _require(not conn.query_pipeline.running, "pipeline should stop on close")
    try:
        _ = conn.query("SELECT 1")
        raise Error("expected query-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "query-on-closed guard should expose 08003",
        )
    try:
        conn.begin()
        raise Error("expected begin-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "begin-on-closed guard should expose 08003",
        )
    try:
        _ = conn.stream("SELECT 1", 1)
        raise Error("expected stream-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "stream-on-closed guard should expose 08003",
        )
    try:
        _ = active_stream_on_close.next(conn)
        raise Error("expected active-stream-on-closed-connection guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "active-stream-on-closed-connection should expose 08003",
        )
    try:
        conn.commit()
        raise Error("expected commit-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "commit-on-closed guard should expose 08003",
        )
    try:
        conn.rollback()
        raise Error("expected rollback-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "rollback-on-closed guard should expose 08003",
        )
    try:
        conn.cancel()
        raise Error("expected cancel-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "cancel-on-closed guard should expose 08003",
        )
    try:
        _ = conn.query_metadata("table")
        raise Error("expected metadata-on-closed guard")
    except e:
        _require(
            scratchbird_native.extract_sqlstate(String(e)) == "08003",
            "metadata-on-closed guard should expose 08003",
        )

    var manager_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=manager_proxy&manager_auth_token=mode_token"
    )
    _require(manager_cfg.front_door_mode == "manager_proxy", "front_door_mode manager_proxy mismatch")
    var manager_conn = scratchbird_native.connect(manager_cfg)
    manager_conn.close()
    var manager_dash_cfg = scratchbird_native.ScratchBirdConfig(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=manager-proxy&manager_auth_token=mode_token"
    )
    _require(manager_dash_cfg.front_door_mode == "manager_proxy", "front_door_mode manager-proxy normalization mismatch")
    var manager_dash_conn = scratchbird_native.connect(manager_dash_cfg)
    manager_dash_conn.close()

    var ssl_disable_conn = scratchbird_native.connect(
        scratchbird_native.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?sslmode=disable"
        )
    )
    ssl_disable_conn.close()
    var ssl_alias_disable_conn = scratchbird_native.connect(
        scratchbird_native.ScratchBirdConfig(
            "scratchbird://user:pass@localhost:3092/testdb?ssl=disable"
        )
    )
    ssl_alias_disable_conn.close()
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?compression=gzip",
        "0A000",
        "compression=gzip is not supported",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?application_name=bad%ZZ",
        "22023",
        "DSN query contains malformed percent-escape",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@[::1/testdb?sslmode=require",
        "22023",
        "DSN contains malformed bracketed IPv6 host",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&port=abc",
        "22023",
        "port must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&port=4100&portNumber=bad",
        "22023",
        "port must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connecttimeout=abc",
        "22023",
        "connect_timeout must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connect_timeout=5&connecttimeout=abc",
        "22023",
        "connect_timeout must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&defaultRowFetchSize=abc",
        "22023",
        "default_row_fetch_size must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&default_row_fetch_size=64&fetchSize=abc",
        "22023",
        "default_row_fetch_size must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&manager_client_flags=abc",
        "22023",
        "manager_client_flags must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connect_client_flags=abc",
        "22023",
        "connect_client_flags must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&auth_method_id=invalid.namespace",
        "28000",
        "invalid auth_method_id namespace",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&prepare_threshold=5&preparethreshold=abc",
        "22023",
        "prepare_threshold must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connection_lifetime=30&poolingconnectionlifetime=abc",
        "22023",
        "connection_lifetime must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&manager_client_flags=1&mcp_client_flags=abc",
        "22023",
        "manager_client_flags must be a valid integer",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?protocol=sql",
        "0A000",
        "protocol must be native",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sb_test_auth_fail=true",
        "28P01",
        "authentication failed",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?front_door_mode=invalid",
        "0A000",
        "front_door_mode must be direct or manager_proxy.",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?frontdoormode=invalid",
        "0A000",
        "front_door_mode must be direct or manager_proxy.",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&front_door_mode=manager_proxy",
        "08001",
        "manager_proxy mode requires manager_auth_token",
    )
    _assert_connect_guard(
        "scratchbird://@localhost:3092/?sslmode=require",
        "28000",
        "user and database are required",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@:3092/testdb?sslmode=require",
        "28000",
        "host and database are required",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&port=0",
        "22023",
        "port must be positive",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&port=70000",
        "22023",
        "port must be between 1 and 65535",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connecttimeout=-1",
        "22023",
        "connect_timeout must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&sockettimeout=-1",
        "22023",
        "socket_timeout must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&login_timeout=-1",
        "22023",
        "login_timeout must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&acquire_timeout=-1",
        "22023",
        "acquire_timeout must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&default_row_fetch_size=-1",
        "22023",
        "default_row_fetch_size must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&min_pool_size=-1",
        "22023",
        "min_pool_size must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&max_pool_size=0",
        "22023",
        "max_pool_size must be >= 1",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&min_pool_size=5&max_pool_size=2",
        "22023",
        "min_pool_size must be <= max_pool_size",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&connection_lifetime=-1",
        "22023",
        "connection_lifetime must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&manager_client_flags=-1",
        "22023",
        "manager_client_flags must be >= 0",
    )
    _assert_connect_guard(
        "scratchbird://user:pass@localhost:3092/testdb?sslmode=require&client_flags=-1",
        "22023",
        "connect_client_flags must be >= 0",
    )

    print("Mojo native bootstrap tests OK")
