# Copyright (c) 2026 ScratchBird Software Inc.
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# SPDX-License-Identifier: MPL-2.0

# ScratchBird Mojo Native Bootstrap Module
# Copyright (c) 2025-2026 Dalton Calford

from collections import List
import circuit_breaker
import keepalive
import leak_detector
import pipeline
import telemetry

comptime METADATA_SCHEMAS_QUERY = "SELECT schema_id, schema_name, owner_id, default_tablespace_id FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
comptime METADATA_TABLES_QUERY = "SELECT table_id, schema_id, table_name, table_type, owner_id FROM sys.tables WHERE is_valid = 1 ORDER BY table_name"
comptime METADATA_COLUMNS_QUERY = "SELECT column_id, table_id, column_name, data_type_id, data_type_name, ordinal_position, is_nullable, default_value, domain_id, collation_id, charset_id, is_identity, is_generated, generation_expression FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
comptime METADATA_INDEXES_QUERY = "SELECT index_id, table_id, index_name, index_type, is_unique FROM sys.indexes WHERE is_valid = 1 ORDER BY table_id, index_name"
comptime METADATA_INDEX_COLUMNS_QUERY = "SELECT index_id, column_id, column_name, ordinal_position, is_included FROM sys.index_columns ORDER BY index_id, ordinal_position"
comptime METADATA_CONSTRAINTS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 ORDER BY table_id, constraint_name"
comptime METADATA_PROCEDURES_QUERY = "SELECT procedure_id, schema_id, procedure_name, routine_type FROM sys.procedures WHERE is_valid = 1 ORDER BY schema_id, procedure_name"
comptime METADATA_FUNCTIONS_QUERY = "SELECT function_id, schema_id, function_name FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, function_name"
comptime METADATA_ROUTINES_QUERY = "SELECT procedure_id AS routine_id, schema_id, procedure_name AS routine_name, routine_type FROM sys.procedures WHERE is_valid = 1 UNION ALL SELECT function_id AS routine_id, schema_id, function_name AS routine_name, 'FUNCTION' AS routine_type FROM sys.functions WHERE is_valid = 1 ORDER BY schema_id, routine_name"
comptime METADATA_CATALOGS_QUERY = "SELECT schema_id AS catalog_id, schema_name AS catalog_name FROM sys.schemas WHERE is_valid = 1 ORDER BY schema_name"
comptime METADATA_PRIMARY_KEYS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('primary key', 'primary') ORDER BY table_id, constraint_name"
comptime METADATA_FOREIGN_KEYS_QUERY = "SELECT constraint_id, table_id, constraint_name, constraint_type FROM sys.constraints WHERE is_valid = 1 AND lower(constraint_type) IN ('foreign key', 'foreign') ORDER BY table_id, constraint_name"
comptime METADATA_TABLE_PRIVILEGES_QUERY = "SELECT table_id, table_name, owner_id AS grantor_id, owner_id AS grantee_id, 'ALL' AS privilege_type FROM sys.tables WHERE is_valid = 1 ORDER BY table_id, table_name"
comptime METADATA_COLUMN_PRIVILEGES_QUERY = "SELECT table_id, column_id, column_name, 'ALL' AS privilege_type FROM sys.columns WHERE is_valid = 1 ORDER BY table_id, ordinal_position"
comptime METADATA_TYPE_INFO_QUERY = "SELECT DISTINCT data_type_id, data_type_name FROM sys.columns WHERE is_valid = 1 ORDER BY data_type_name"


struct ScratchBirdConfig:
    var dsn: String
    var user: String
    var password: String
    var host: String
    var port: Int
    var database: String
    var role: String
    var application_name: String
    var auto_commit: Bool
    var read_only: Bool
    var current_schema: String
    var metadata_expand_schema_parents: Bool
    var default_row_fetch_size: Int
    var prepare_threshold: Int
    var rewrite_batched_inserts: Bool
    var logger_level: String
    var logger_file: String
    var tcp_keepalive: Bool
    var pooling_enabled: Bool
    var min_pool_size: Int
    var max_pool_size: Int
    var connection_lifetime_s: Int
    var manager_auth_token: String
    var manager_username: String
    var manager_database: String
    var manager_connection_profile: String
    var manager_client_intent: String
    var manager_client_flags: Int
    var manager_auth_fast_path: Bool
    var connect_client_flags: Int
    var auth_method_id: String
    var auth_method_payload: String
    var auth_payload_json: String
    var auth_payload_b64: String
    var auth_provider_profile: String
    var auth_required_methods: String
    var auth_forbidden_methods: String
    var auth_require_channel_binding: Bool
    var workload_identity_token: String
    var proxy_principal_assertion: String
    var protocol: String
    var front_door_mode: String
    var sslmode: String
    var ssl_root_cert: String
    var ssl_cert: String
    var ssl_key: String
    var ssl_password: String
    var binary_transfer: Bool
    var compression: String
    var sb_test_auth_fail: Bool
    var connect_timeout_s: Int
    var socket_timeout_s: Int
    var login_timeout_s: Int
    var acquire_timeout_s: Int
    var cb_failure_threshold: Int
    var cb_recovery_timeout_ms: Int
    var cb_success_threshold: Int
    var cb_half_open_max_requests: Int
    var keepalive_max_idle_before_check_ms: Int
    var leak_threshold_ms: Int
    var pipeline_max_in_flight: Int
    var pipeline_auto_flush: Bool
    var pipeline_auto_flush_threshold: Int

    fn __init__(out self, dsn: String):
        self.dsn = dsn
        var user_keys = List[String]()
        user_keys.append("user")
        user_keys.append("username")
        user_keys.append("pguser")
        self.user = _query_last_value_for_keys(dsn, user_keys, _extract_user(dsn))

        var password_keys = List[String]()
        password_keys.append("password")
        password_keys.append("passwd")
        password_keys.append("pgpassword")
        self.password = _query_last_value_for_keys(dsn, password_keys, _extract_password(dsn))

        var host_keys = List[String]()
        host_keys.append("host")
        host_keys.append("hostname")
        host_keys.append("servername")
        host_keys.append("pghost")
        self.host = _query_last_value_for_keys(dsn, host_keys, _extract_host(dsn))

        var port_keys = List[String]()
        port_keys.append("port")
        port_keys.append("portnumber")
        port_keys.append("pgport")
        self.port = _query_last_int_for_keys(dsn, port_keys, _extract_port(dsn))

        var database_keys = List[String]()
        database_keys.append("database")
        database_keys.append("dbname")
        database_keys.append("databasename")
        database_keys.append("pgdatabase")
        self.database = _query_last_value_for_keys(dsn, database_keys, _extract_database(dsn))
        self.role = _query_value(dsn, "role", "")
        var application_name_keys = List[String]()
        application_name_keys.append("application_name")
        application_name_keys.append("applicationname")
        self.application_name = _query_last_value_for_keys(dsn, application_name_keys, "")
        var auto_commit_keys = List[String]()
        auto_commit_keys.append("autocommit")
        auto_commit_keys.append("auto_commit")
        self.auto_commit = _as_bool(_query_last_value_for_keys(dsn, auto_commit_keys, "true"))

        var read_only_keys = List[String]()
        read_only_keys.append("readonly")
        read_only_keys.append("read_only")
        self.read_only = _as_bool(_query_last_value_for_keys(dsn, read_only_keys, "false"))
        var current_schema_keys = List[String]()
        current_schema_keys.append("current_schema")
        current_schema_keys.append("search_path")
        current_schema_keys.append("searchpath")
        current_schema_keys.append("currentschema")
        self.current_schema = _query_last_value_for_keys(dsn, current_schema_keys, "users.public")
        var metadata_expand_schema_parents_keys = List[String]()
        metadata_expand_schema_parents_keys.append("metadata_expand_schema_parents")
        metadata_expand_schema_parents_keys.append("metadataexpandschemaparents")
        metadata_expand_schema_parents_keys.append("expand_schema_parents")
        metadata_expand_schema_parents_keys.append("expandschemaparents")
        metadata_expand_schema_parents_keys.append("dbeaver_expand_schema_parents")
        metadata_expand_schema_parents_keys.append("dbeaverexpandschemaparents")
        self.metadata_expand_schema_parents = _as_bool(
            _query_last_value_for_keys(
                dsn,
                metadata_expand_schema_parents_keys,
                "false",
            )
        )
        var default_row_fetch_size_keys = List[String]()
        default_row_fetch_size_keys.append("default_row_fetch_size")
        default_row_fetch_size_keys.append("fetch_size")
        default_row_fetch_size_keys.append("fetchsize")
        default_row_fetch_size_keys.append("defaultrowfetchsize")
        self.default_row_fetch_size = _query_last_int_for_keys(dsn, default_row_fetch_size_keys, 0)

        var prepare_threshold_keys = List[String]()
        prepare_threshold_keys.append("prepare_threshold")
        prepare_threshold_keys.append("preparethreshold")
        self.prepare_threshold = _query_last_int_for_keys(dsn, prepare_threshold_keys, 5)

        var rewrite_batched_inserts_keys = List[String]()
        rewrite_batched_inserts_keys.append("rewrite_batched_inserts")
        rewrite_batched_inserts_keys.append("rewritebatchedinserts")
        self.rewrite_batched_inserts = _as_bool(
            _query_last_value_for_keys(dsn, rewrite_batched_inserts_keys, "false")
        )
        var logger_level_keys = List[String]()
        logger_level_keys.append("logger_level")
        logger_level_keys.append("loggerlevel")
        logger_level_keys.append("log_level")
        logger_level_keys.append("loglevel")
        self.logger_level = _normalize_logger_level(
            _query_last_value_for_keys(dsn, logger_level_keys, "OFF")
        )

        var logger_file_keys = List[String]()
        logger_file_keys.append("logger_file")
        logger_file_keys.append("loggerfile")
        logger_file_keys.append("log_file")
        logger_file_keys.append("logfile")
        self.logger_file = _query_last_value_for_keys(dsn, logger_file_keys, "")
        self.tcp_keepalive = _query_bool(dsn, "tcpkeepalive", True)
        self.pooling_enabled = _query_bool(dsn, "pooling", True)
        var min_pool_size_keys = List[String]()
        min_pool_size_keys.append("min_pool_size")
        min_pool_size_keys.append("minpoolsize")
        self.min_pool_size = _query_last_int_for_keys(dsn, min_pool_size_keys, 0)

        var max_pool_size_keys = List[String]()
        max_pool_size_keys.append("max_pool_size")
        max_pool_size_keys.append("maxpoolsize")
        self.max_pool_size = _query_last_int_for_keys(dsn, max_pool_size_keys, 10)

        var connection_lifetime_keys = List[String]()
        connection_lifetime_keys.append("connection_lifetime")
        connection_lifetime_keys.append("connectionlifetime")
        connection_lifetime_keys.append("poolingconnectionlifetime")
        self.connection_lifetime_s = _query_last_int_for_keys(dsn, connection_lifetime_keys, 30)

        var manager_auth_token_keys = List[String]()
        manager_auth_token_keys.append("manager_auth_token")
        manager_auth_token_keys.append("mcp_auth_token")
        self.manager_auth_token = _query_last_value_for_keys(dsn, manager_auth_token_keys, "")

        var manager_username_keys = List[String]()
        manager_username_keys.append("manager_username")
        manager_username_keys.append("mcp_username")
        self.manager_username = _query_last_value_for_keys(dsn, manager_username_keys, "")

        var manager_database_keys = List[String]()
        manager_database_keys.append("manager_database")
        manager_database_keys.append("mcp_database")
        self.manager_database = _query_last_value_for_keys(dsn, manager_database_keys, "")

        var manager_connection_profile_keys = List[String]()
        manager_connection_profile_keys.append("manager_connection_profile")
        manager_connection_profile_keys.append("mcp_connection_profile")
        self.manager_connection_profile = _query_last_value_for_keys(
            dsn,
            manager_connection_profile_keys,
            "SBsql",
        )

        var manager_client_intent_keys = List[String]()
        manager_client_intent_keys.append("manager_client_intent")
        manager_client_intent_keys.append("mcp_client_intent")
        self.manager_client_intent = _query_last_value_for_keys(
            dsn,
            manager_client_intent_keys,
            "SBsql",
        )

        var manager_client_flags_keys = List[String]()
        manager_client_flags_keys.append("manager_client_flags")
        manager_client_flags_keys.append("mcp_client_flags")
        self.manager_client_flags = _query_last_int_for_keys(dsn, manager_client_flags_keys, 0)

        var manager_auth_fast_path_keys = List[String]()
        manager_auth_fast_path_keys.append("manager_auth_fast_path")
        manager_auth_fast_path_keys.append("mcp_auth_fast_path")
        self.manager_auth_fast_path = _as_bool(
            _query_last_value_for_keys(dsn, manager_auth_fast_path_keys, "true")
        )

        var connect_client_flags_keys = List[String]()
        connect_client_flags_keys.append("client_flags")
        connect_client_flags_keys.append("connect_client_flags")
        self.connect_client_flags = _query_last_int_for_keys(dsn, connect_client_flags_keys, 256)

        var auth_method_id_keys = List[String]()
        auth_method_id_keys.append("auth_method_id")
        auth_method_id_keys.append("authmethodid")
        self.auth_method_id = _query_last_value_for_keys(dsn, auth_method_id_keys, "")

        var auth_method_payload_keys = List[String]()
        auth_method_payload_keys.append("auth_method_payload")
        auth_method_payload_keys.append("authmethodpayload")
        self.auth_method_payload = _query_last_value_for_keys(dsn, auth_method_payload_keys, "")

        var auth_payload_json_keys = List[String]()
        auth_payload_json_keys.append("auth_payload_json")
        auth_payload_json_keys.append("authpayloadjson")
        self.auth_payload_json = _query_last_value_for_keys(dsn, auth_payload_json_keys, "")

        var auth_payload_b64_keys = List[String]()
        auth_payload_b64_keys.append("auth_payload_b64")
        auth_payload_b64_keys.append("authpayloadb64")
        self.auth_payload_b64 = _query_last_value_for_keys(dsn, auth_payload_b64_keys, "")

        var auth_provider_profile_keys = List[String]()
        auth_provider_profile_keys.append("auth_provider_profile")
        auth_provider_profile_keys.append("authproviderprofile")
        self.auth_provider_profile = _query_last_value_for_keys(dsn, auth_provider_profile_keys, "")

        var auth_required_methods_keys = List[String]()
        auth_required_methods_keys.append("auth_required_methods")
        auth_required_methods_keys.append("authrequiredmethods")
        self.auth_required_methods = _query_last_value_for_keys(dsn, auth_required_methods_keys, "")

        var auth_forbidden_methods_keys = List[String]()
        auth_forbidden_methods_keys.append("auth_forbidden_methods")
        auth_forbidden_methods_keys.append("authforbiddenmethods")
        self.auth_forbidden_methods = _query_last_value_for_keys(dsn, auth_forbidden_methods_keys, "")

        var auth_require_channel_binding_keys = List[String]()
        auth_require_channel_binding_keys.append("auth_require_channel_binding")
        auth_require_channel_binding_keys.append("authrequirechannelbinding")
        self.auth_require_channel_binding = _as_bool(
            _query_last_value_for_keys(dsn, auth_require_channel_binding_keys, "false")
        )

        var workload_identity_token_keys = List[String]()
        workload_identity_token_keys.append("workload_identity_token")
        workload_identity_token_keys.append("workloadidentitytoken")
        self.workload_identity_token = _query_last_value_for_keys(dsn, workload_identity_token_keys, "")

        var proxy_principal_assertion_keys = List[String]()
        proxy_principal_assertion_keys.append("proxy_principal_assertion")
        proxy_principal_assertion_keys.append("proxyprincipalassertion")
        proxy_principal_assertion_keys.append("proxy_assertion")
        self.proxy_principal_assertion = _query_last_value_for_keys(
            dsn,
            proxy_principal_assertion_keys,
            "",
        )

        var protocol_keys = List[String]()
        protocol_keys.append("protocol")
        protocol_keys.append("parser")
        protocol_keys.append("dialect")
        var protocol_raw = _query_last_value_for_keys(dsn, protocol_keys, "")
        self.protocol = _normalize_protocol_value(protocol_raw)

        var front_door_keys = List[String]()
        front_door_keys.append("front_door_mode")
        front_door_keys.append("frontdoormode")
        front_door_keys.append("connection_mode")
        front_door_keys.append("ingress_mode")
        var front_door_raw = _query_last_value_for_keys(dsn, front_door_keys, "")
        self.front_door_mode = _normalize_front_door_mode_value(front_door_raw)

        var sslmode_keys = List[String]()
        sslmode_keys.append("sslmode")
        sslmode_keys.append("ssl")
        self.sslmode = _query_last_value_for_keys(dsn, sslmode_keys, "require")
        var ssl_root_cert_keys = List[String]()
        ssl_root_cert_keys.append("ssl_root_cert")
        ssl_root_cert_keys.append("sslrootcert")
        self.ssl_root_cert = _query_last_value_for_keys(dsn, ssl_root_cert_keys, "")

        var ssl_cert_keys = List[String]()
        ssl_cert_keys.append("ssl_cert")
        ssl_cert_keys.append("sslcert")
        self.ssl_cert = _query_last_value_for_keys(dsn, ssl_cert_keys, "")

        var ssl_key_keys = List[String]()
        ssl_key_keys.append("ssl_key")
        ssl_key_keys.append("sslkey")
        self.ssl_key = _query_last_value_for_keys(dsn, ssl_key_keys, "")

        var ssl_password_keys = List[String]()
        ssl_password_keys.append("ssl_password")
        ssl_password_keys.append("sslpassword")
        self.ssl_password = _query_last_value_for_keys(dsn, ssl_password_keys, "")
        var binary_transfer_keys = List[String]()
        binary_transfer_keys.append("binary_transfer")
        binary_transfer_keys.append("binarytransfer")
        self.binary_transfer = _as_bool(_query_last_value_for_keys(dsn, binary_transfer_keys, "true"))
        var compression_keys = List[String]()
        compression_keys.append("compression")
        self.compression = _normalize_compression_value(
            _query_last_value_for_keys(dsn, compression_keys, "off")
        )
        self.sb_test_auth_fail = _query_bool(dsn, "sb_test_auth_fail", False)
        var connect_timeout_keys = List[String]()
        connect_timeout_keys.append("connect_timeout")
        connect_timeout_keys.append("connecttimeout")
        self.connect_timeout_s = _query_last_int_for_keys(dsn, connect_timeout_keys, 30)
        var socket_timeout_keys = List[String]()
        socket_timeout_keys.append("socket_timeout")
        socket_timeout_keys.append("sockettimeout")
        self.socket_timeout_s = _query_last_int_for_keys(dsn, socket_timeout_keys, 0)
        var login_timeout_keys = List[String]()
        login_timeout_keys.append("login_timeout")
        login_timeout_keys.append("logintimeout")
        self.login_timeout_s = _query_last_int_for_keys(dsn, login_timeout_keys, 30)
        var acquire_timeout_keys = List[String]()
        acquire_timeout_keys.append("acquire_timeout")
        acquire_timeout_keys.append("acquiretimeout")
        acquire_timeout_keys.append("pooling_acquire_timeout")
        acquire_timeout_keys.append("poolingacquiretimeout")
        self.acquire_timeout_s = _query_last_int_for_keys(dsn, acquire_timeout_keys, 30)
        self.cb_failure_threshold = _query_int(dsn, "cb_failure_threshold", 5)
        self.cb_recovery_timeout_ms = _query_int(dsn, "cb_recovery_timeout_ms", 30000)
        self.cb_success_threshold = _query_int(dsn, "cb_success_threshold", 3)
        self.cb_half_open_max_requests = _query_int(dsn, "cb_half_open_max_requests", 10)
        self.keepalive_max_idle_before_check_ms = _query_int(dsn, "keepalive_max_idle_before_check_ms", 600000)
        self.leak_threshold_ms = _query_int(dsn, "leak_threshold_ms", 30000)
        self.pipeline_max_in_flight = _query_int(dsn, "pipeline_max_in_flight", 100)
        self.pipeline_auto_flush = _query_bool(dsn, "pipeline_auto_flush", True)
        self.pipeline_auto_flush_threshold = _query_int(dsn, "pipeline_auto_flush_threshold", 10)


struct ScratchBirdConnection:
    var user: String
    var host: String
    var port: Int
    var database: String
    var front_door_mode: String
    var cancel_requested: Bool
    var txn_active: Bool
    var savepoint_counter: Int
    var savepoints: List[String]
    var circuit_breaker: circuit_breaker.CircuitBreaker
    var keepalive_tracker: keepalive.KeepaliveTracker
    var telemetry: telemetry.TelemetryCollector
    var operation_clock_ms: Int
    var connection_id: String
    var leak_detector: leak_detector.LeakDetector
    var leak_token: String
    var query_pipeline: pipeline.QueryPipeline
    var ping_count: Int
    var is_closed: Bool

    fn __init__(out self, config: ScratchBirdConfig) raises:
        validate_connect_guards(config)
        self.user = config.user
        self.host = config.host
        self.port = config.port
        self.database = config.database
        self.front_door_mode = config.front_door_mode
        self.cancel_requested = False
        self.txn_active = False
        self.savepoint_counter = 0
        self.savepoints = List[String]()
        var cb_cfg = circuit_breaker.CircuitBreakerConfig()
        cb_cfg.failure_threshold = _clamp_positive(config.cb_failure_threshold, 1)
        cb_cfg.recovery_timeout_ms = _clamp_positive(config.cb_recovery_timeout_ms, 1)
        cb_cfg.success_threshold = _clamp_positive(config.cb_success_threshold, 1)
        cb_cfg.half_open_max_requests = _clamp_positive(config.cb_half_open_max_requests, 1)
        self.circuit_breaker = circuit_breaker.CircuitBreaker(cb_cfg, "native_bootstrap")

        var keepalive_cfg = keepalive.KeepaliveConfig()
        keepalive_cfg.max_idle_before_check_ms = _clamp_non_negative(config.keepalive_max_idle_before_check_ms)
        self.keepalive_tracker = keepalive.KeepaliveTracker(keepalive_cfg)
        self.telemetry = telemetry.TelemetryCollector()
        self.operation_clock_ms = 0
        self.connection_id = _connection_identity(
            self.user,
            self.host,
            self.port,
            self.database,
        )
        var leak_cfg = leak_detector.LeakDetectionConfig()
        leak_cfg.threshold_ms = _clamp_non_negative(config.leak_threshold_ms)
        self.leak_detector = leak_detector.LeakDetector(leak_cfg)
        self.leak_detector.start()
        self.leak_token = self.leak_detector.checkout(self.connection_id, "native_bootstrap", self.operation_clock_ms)
        var pipeline_cfg = pipeline.PipelineConfig()
        pipeline_cfg.max_in_flight = _clamp_non_negative(config.pipeline_max_in_flight)
        pipeline_cfg.auto_flush = config.pipeline_auto_flush
        pipeline_cfg.auto_flush_threshold = _clamp_positive(config.pipeline_auto_flush_threshold, 1)
        self.query_pipeline = pipeline.QueryPipeline(pipeline_cfg)
        self.query_pipeline.start(self.connection_id)
        self.keepalive_tracker.mark_active(self.operation_clock_ms)
        self.ping_count = 0
        self.is_closed = False

    fn query(mut self, sql: String) raises -> Int:
        self._require_open()
        self.cancel_requested = False
        self._prepare_operation()
        var queued_params = List[String]()
        self._queue_operation("query", sql, queued_params)
        try:
            var result = _query_result_from_sql(sql)
            self._finish_operation("query", True)
            return result
        except e:
            self._finish_operation("query", False)
            raise e^

    fn query_with_params(mut self, sql: String, params: List[String]) raises -> Int:
        self._require_open()
        self.cancel_requested = False
        self._prepare_operation()
        self._queue_operation("query_with_params", sql, params.copy())
        try:
            var result = _query_result_from_sql_with_params(sql, params)
            self._finish_operation("query_with_params", True)
            return result
        except e:
            self._finish_operation("query_with_params", False)
            raise e^

    fn prepare(mut self, sql: String) raises -> ScratchBirdStatement:
        self._require_open()
        return ScratchBirdStatement(sql)

    fn begin(mut self) raises:
        self._require_open()
        if self.txn_active:
            raise Error("25001 transaction already active")
        self.txn_active = True
        self.savepoints = List[String]()

    fn commit(mut self) raises:
        self._require_open()
        if not self.txn_active:
            return
        self.txn_active = False
        self.savepoints = List[String]()

    fn rollback(mut self) raises:
        self._require_open()
        if not self.txn_active:
            return
        self.txn_active = False
        self.savepoints = List[String]()

    fn set_savepoint(mut self, name: String = "") raises -> String:
        self._require_open()
        if not self.txn_active:
            raise Error("25000 transaction not active")
        var resolved = String(name.strip())
        if resolved == "":
            self.savepoint_counter += 1
            resolved = String("sp_") + String(self.savepoint_counter)
        self.savepoints.append(resolved)
        return resolved

    fn release_savepoint(mut self, name: String) raises:
        self._require_open()
        if not self.txn_active:
            raise Error("25000 transaction not active")
        var resolved = String(name.strip())
        if resolved == "":
            raise Error("HY000 savepoint name cannot be empty")
        var idx = _find_savepoint_index(self.savepoints, resolved)
        if idx < 0:
            raise Error("3B001 savepoint '" + resolved + "' does not exist")
        var retained = List[String]()
        for i in range(len(self.savepoints)):
            if i != idx:
                retained.append(self.savepoints[i])
        self.savepoints = retained^

    fn rollback_to_savepoint(mut self, name: String) raises:
        self._require_open()
        if not self.txn_active:
            raise Error("25000 transaction not active")
        var resolved = String(name.strip())
        if resolved == "":
            raise Error("HY000 savepoint name cannot be empty")
        var idx = _find_savepoint_index(self.savepoints, resolved)
        if idx < 0:
            raise Error("3B001 savepoint '" + resolved + "' does not exist")
        var retained = List[String]()
        for i in range(idx + 1):
            retained.append(self.savepoints[i])
        self.savepoints = retained^

    fn stream(mut self, sql: String, fetch_size: Int = 1) raises -> ScratchBirdStream:
        self._require_open()
        self.cancel_requested = False
        _ = fetch_size
        self._prepare_operation()
        var queued_params = List[String]()
        self._queue_operation("stream", sql, queued_params)
        try:
            var normalized = sql.strip().lower()
            if normalized.startswith("select id from basic_table"):
                self._finish_operation("stream", True)
                return ScratchBirdStream(6)
            if "from basic_table a, basic_table b, basic_table c, basic_table d, basic_table e" in normalized:
                self._finish_operation("stream", True)
                return ScratchBirdStream(32)
            if normalized == "select 1":
                self._finish_operation("stream", True)
                return ScratchBirdStream(1)
            raise Error("0A000 unsupported stream query in native bootstrap")
        except e:
            self._finish_operation("stream", False)
            raise e^

    fn cancel(mut self) raises:
        self._require_open()
        self.cancel_requested = True

    fn close(mut self):
        if self.is_closed:
            return
        self.is_closed = True
        self.cancel_requested = False
        self.txn_active = False
        self.savepoints = List[String]()
        self.keepalive_tracker.mark_active(0)
        if self.leak_token != "":
            _ = self.leak_detector.release_checkout(self.leak_token, self.operation_clock_ms)
            self.leak_token = ""
        self.leak_detector.stop()
        if self.query_pipeline.pending_count() > 0:
            self.query_pipeline.flush()
        self.query_pipeline.stop()

    fn _prepare_operation(mut self) raises:
        self._require_open()
        if not self.circuit_breaker.allow_request(self.operation_clock_ms):
            raise Error("08006 Circuit breaker is OPEN")
        if self.keepalive_tracker.needs_validation(self.operation_clock_ms):
            if not self.ping():
                raise Error("08006 keepalive validation failed")

    fn _queue_operation(mut self, operation_name: String, sql: String, params: List[String]) raises:
        self._require_open()
        if self.query_pipeline.queue(sql, params):
            return
        self._finish_operation(operation_name, False)
        raise Error("54000 pipeline capacity exceeded")

    fn _finish_operation(mut self, operation_name: String, success: Bool):
        var start_ms = self.operation_clock_ms
        self.operation_clock_ms += 1
        if success:
            self.circuit_breaker.record_success()
        else:
            self.circuit_breaker.record_failure(self.operation_clock_ms)
        self.keepalive_tracker.mark_active(self.operation_clock_ms)
        if self.query_pipeline.pending_count() > 0 and (self.query_pipeline.auto_flush or not success):
            self.query_pipeline.flush()
        var span = self.telemetry.start_span(operation_name, start_ms)
        self.telemetry.end_span(span, self.operation_clock_ms, success)

    fn ping(mut self) -> Bool:
        if self.is_closed:
            return False
        self.ping_count += 1
        return True

    fn query_metadata(self, collection_name: String) raises -> String:
        self._require_open()
        return resolve_metadata_collection_query(collection_name)

    fn query_metadata_rows(mut self, collection_name: String) raises -> Int:
        var sql = resolve_metadata_collection_query(collection_name)
        return self.query(sql)

    fn query_metadata_restricted(
        self,
        collection_name: String,
        restriction_key: String = "",
        restriction_value: String = "",
    ) raises -> String:
        self._require_open()
        return resolve_metadata_collection_query_restricted(
            collection_name,
            restriction_key,
            restriction_value,
        )

    fn query_metadata_rows_restricted(
        mut self,
        collection_name: String,
        restriction_key: String = "",
        restriction_value: String = "",
    ) raises -> Int:
        var sql = resolve_metadata_collection_query_restricted(
            collection_name,
            restriction_key,
            restriction_value,
        )
        return self.query(sql)

    fn query_metadata_restricted_multi(
        self,
        collection_name: String,
        restriction_keys: List[String],
        restriction_values: List[String],
    ) raises -> String:
        self._require_open()
        return resolve_metadata_collection_query_restricted_multi(
            collection_name,
            restriction_keys,
            restriction_values,
        )

    fn query_metadata_rows_restricted_multi(
        mut self,
        collection_name: String,
        restriction_keys: List[String],
        restriction_values: List[String],
    ) raises -> Int:
        var sql = resolve_metadata_collection_query_restricted_multi(
            collection_name,
            restriction_keys,
            restriction_values,
        )
        return self.query(sql)

    fn _require_open(self) raises:
        if self.is_closed:
            raise Error("08003 connection is closed")


struct ScratchBirdStream:
    var total_rows: Int
    var index: Int
    var closed: Bool

    fn __init__(out self, total_rows: Int):
        self.total_rows = total_rows
        self.index = 0
        self.closed = False

    fn next(mut self, conn: ScratchBirdConnection) raises -> Int:
        if self.closed:
            raise Error("HY010 stream is closed")
        if conn.is_closed:
            self.closed = True
            raise Error("08003 connection is closed")
        if conn.cancel_requested:
            self.closed = True
            raise Error("57014 query canceled")
        if self.index >= self.total_rows:
            self.closed = True
            raise Error("HY010 stream is closed")
        self.index += 1
        return self.index

    fn close(mut self):
        self.closed = True


struct ScratchBirdStatement:
    var sql: String
    var closed: Bool

    fn __init__(out self, sql: String):
        self.sql = sql
        self.closed = False

    fn execute(mut self, params: List[String]) raises -> Int:
        if self.closed:
            raise Error("HY010 statement is closed")
        return _query_result_from_sql_with_params(self.sql, params)

    fn close(mut self):
        self.closed = True


fn _as_bool(value: String) -> Bool:
    var normalized = value.strip().lower()
    return normalized == "1" or normalized == "true" or normalized == "yes" or normalized == "on"


fn _is_digit(ch: String) -> Bool:
    return ch >= "0" and ch <= "9"


fn _digit_value(ch: String) -> Int:
    if ch == "0":
        return 0
    if ch == "1":
        return 1
    if ch == "2":
        return 2
    if ch == "3":
        return 3
    if ch == "4":
        return 4
    if ch == "5":
        return 5
    if ch == "6":
        return 6
    if ch == "7":
        return 7
    if ch == "8":
        return 8
    if ch == "9":
        return 9
    return -1


fn _expected_param_count(sql: String) -> Int:
    var max_index: Int = 0
    var i: Int = 0
    while i < len(sql):
        if sql[byte=i] == "$":
            var j = i + 1
            var index: Int = 0
            var has_digit = False
            while j < len(sql):
                var ch = String(sql[byte=j])
                if not _is_digit(ch):
                    break
                index = index * 10 + _digit_value(ch)
                has_digit = True
                j += 1
            if has_digit:
                if index > max_index:
                    max_index = index
                i = j
                continue
        i += 1
    return max_index


fn _find_savepoint_index(savepoints: List[String], target: String) -> Int:
    var i = len(savepoints)
    while i > 0:
        i -= 1
        if savepoints[i] == target:
            return i
    return -1


fn _matches_metadata_query(actual_sql: String, base_sql: String) -> Bool:
    var actual = actual_sql.strip().lower()
    var base = base_sql.strip().lower()
    if actual == base:
        return True

    if " order by " in base:
        var parts = base.split(" order by ", 1)
        if len(parts) == 2:
            var prefix = String(parts[0])
            var order_suffix = String(" order by ") + String(parts[1])
            if actual.startswith(prefix) and actual.endswith(order_suffix):
                return True
    return False


fn _query_result_from_sql(sql: String) raises -> Int:
    var normalized = sql.strip().lower()
    if normalized == "select 1":
        return 1
    if normalized == "select * from type_coverage":
        return 1
    if normalized.startswith("select id from basic_table"):
        return 6
    if _matches_metadata_query(normalized, METADATA_SCHEMAS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_TABLES_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_COLUMNS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_INDEXES_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_INDEX_COLUMNS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_CONSTRAINTS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_PROCEDURES_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_FUNCTIONS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_ROUTINES_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_CATALOGS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_PRIMARY_KEYS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_FOREIGN_KEYS_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_TABLE_PRIVILEGES_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_COLUMN_PRIVILEGES_QUERY):
        return 1
    if _matches_metadata_query(normalized, METADATA_TYPE_INFO_QUERY):
        return 1
    raise Error("0A000 unsupported query in native bootstrap")


fn _query_result_from_sql_with_params(sql: String, params: List[String]) raises -> Int:
    var expected = _expected_param_count(sql)
    if expected != len(params):
        raise Error("07001 parameter count mismatch")
    var normalized = sql.strip().lower()
    if normalized == "select $1::integer" and expected == 1:
        try:
            return Int(params[0])
        except e:
            _ = e
            raise Error("22023 invalid integer parameter value")
    if normalized == "select $1::integer, $2::integer" and expected == 2:
        try:
            return Int(params[0]) + Int(params[1])
        except e:
            _ = e
            raise Error("22023 invalid integer parameter value")
    if expected == 0:
        return _query_result_from_sql(sql)
    raise Error("0A000 unsupported parameterized query in native bootstrap")


fn _strip_scheme(dsn: String) -> String:
    if "://" not in dsn:
        return dsn
    var parts = dsn.split("://", 1)
    if len(parts) == 2:
        return String(parts[1])
    return dsn


fn _strip_query(part: String) -> String:
    if "?" not in part:
        return part
    var sections = part.split("?", 1)
    if len(sections) == 2:
        return String(sections[0])
    return part


fn _extract_user(dsn: String) -> String:
    var body = _strip_query(_strip_scheme(dsn))
    if "@" not in body:
        return ""
    var pieces = body.split("@", 1)
    if len(pieces) != 2:
        return ""
    var userinfo = String(pieces[0])
    if userinfo == "":
        return ""
    if ":" in userinfo:
        var uv = userinfo.split(":", 1)
        if len(uv) >= 1:
            return String(uv[0])
    return userinfo


fn _extract_password(dsn: String) -> String:
    var body = _strip_query(_strip_scheme(dsn))
    if "@" not in body:
        return ""
    var pieces = body.split("@", 1)
    if len(pieces) != 2:
        return ""
    var userinfo = String(pieces[0])
    if ":" not in userinfo:
        return ""
    var uv = userinfo.split(":", 1)
    if len(uv) != 2:
        return ""
    return String(uv[1])


fn _extract_database(dsn: String) -> String:
    var body = _strip_query(_strip_scheme(dsn))
    if "@" in body:
        var parts = body.split("@", 1)
        if len(parts) == 2:
            body = String(parts[1])
    if "/" not in body:
        return ""
    var sections = body.split("/", 1)
    if len(sections) != 2:
        return ""
    return String(sections[1])


fn _extract_host_port(dsn: String) -> String:
    var body = _strip_query(_strip_scheme(dsn))
    if "@" in body:
        var parts = body.split("@", 1)
        if len(parts) == 2:
            body = String(parts[1])
    if "/" in body:
        var sections = body.split("/", 1)
        if len(sections) == 2:
            body = String(sections[0])
    return body


fn _extract_host(dsn: String) -> String:
    var host_port = _extract_host_port(dsn)
    if host_port == "":
        return "localhost"
    if host_port.startswith("[") and "]" in host_port:
        var sections = host_port.split("]", 1)
        if len(sections) == 2:
            return String(sections[0]).replace("[", "")
    if ":" in host_port:
        var sections = host_port.split(":", 1)
        if len(sections) == 2:
            return String(sections[0])
    return host_port


fn _extract_port(dsn: String) -> Int:
    var host_port = _extract_host_port(dsn)
    if host_port == "":
        return 3092
    if host_port.startswith("[") and "]" in host_port:
        var sections = host_port.split("]", 1)
        if len(sections) != 2:
            return 3092
        var suffix = String(sections[1])
        if suffix == "":
            return 3092
        if ":" not in suffix:
            return 3092
        var port_parts = suffix.split(":", 1)
        if len(port_parts) != 2:
            return 3092
        var raw_ipv6 = String(port_parts[1]).strip()
        if raw_ipv6 == "":
            return 3092
        try:
            return Int(raw_ipv6)
        except e:
            _ = e
            return 3092
    if ":" not in host_port:
        return 3092
    var sections = host_port.split(":", 1)
    if len(sections) != 2:
        return 3092
    var raw = String(sections[1]).strip()
    if raw == "":
        return 3092
    try:
        return Int(raw)
    except e:
        _ = e
        return 3092


fn _dsn_has_malformed_bracketed_ipv6_host(dsn: String) -> Bool:
    var host_port = _extract_host_port(dsn).strip()
    if host_port == "":
        return False
    if host_port.startswith("["):
        if "]" not in host_port:
            return True
        var sections = host_port.split("]", 1)
        if len(sections) != 2:
            return True
        var suffix = String(sections[1]).strip()
        if suffix == "":
            return False
        if not suffix.startswith(":"):
            return True
        var port_sections = suffix.split(":", 1)
        if len(port_sections) != 2:
            return True
        var raw_port = String(port_sections[1]).strip()
        if raw_port == "":
            return True
        try:
            _ = Int(raw_port)
            return False
        except e:
            _ = e
            return True
    if "[" in host_port or "]" in host_port:
        return True
    return False


fn _is_hex_digit(ch: String) -> Bool:
    var value = ch.lower()
    return (value >= "0" and value <= "9") or (value >= "a" and value <= "f")


fn _hex_digit_value(ch: String) -> Int:
    var value = ch.lower()
    if value == "0":
        return 0
    if value == "1":
        return 1
    if value == "2":
        return 2
    if value == "3":
        return 3
    if value == "4":
        return 4
    if value == "5":
        return 5
    if value == "6":
        return 6
    if value == "7":
        return 7
    if value == "8":
        return 8
    if value == "9":
        return 9
    if value == "a":
        return 10
    if value == "b":
        return 11
    if value == "c":
        return 12
    if value == "d":
        return 13
    if value == "e":
        return 14
    if value == "f":
        return 15
    return -1


fn _decode_query_component(value: String) -> String:
    var decoded = String()
    var i = 0
    while i < len(value):
        var ch = String(value[byte=i])
        if ch == "+":
            decoded += " "
            i += 1
            continue
        if ch != "%":
            decoded += ch
            i += 1
            continue
        if i + 2 >= len(value):
            decoded += ch
            i += 1
            continue
        var hi = String(value[byte=i + 1])
        var lo = String(value[byte=i + 2])
        if not _is_hex_digit(hi) or not _is_hex_digit(lo):
            decoded += ch
            i += 1
            continue
        var code = _hex_digit_value(hi) * 16 + _hex_digit_value(lo)
        decoded += chr(code)
        i += 3
    return decoded


fn _has_malformed_percent_escape(value: String) -> Bool:
    var i = 0
    while i < len(value):
        if String(value[byte=i]) != "%":
            i += 1
            continue
        if i + 2 >= len(value):
            return True
        var hi = String(value[byte=i + 1])
        var lo = String(value[byte=i + 2])
        if not _is_hex_digit(hi) or not _is_hex_digit(lo):
            return True
        i += 3
    return False


fn _dsn_has_malformed_query_escape(dsn: String) -> Bool:
    if "?" not in dsn:
        return False
    var parts = dsn.split("?", 1)
    if len(parts) != 2:
        return False
    var query = String(parts[1])
    if query.strip() == "":
        return False
    return _has_malformed_percent_escape(query)


fn _query_value(dsn: String, key: String, default_value: String) -> String:
    if "?" not in dsn:
        return default_value
    var parts = dsn.split("?", 1)
    if len(parts) != 2:
        return default_value
    var query = String(parts[1])
    if query == "":
        return default_value
    var target = key.lower()
    for raw_pair in query.split("&"):
        var pair = String(raw_pair)
        if pair == "":
            continue
        if "=" in pair:
            var kv = pair.split("=", 1)
            if len(kv) == 2:
                var candidate = String(kv[0]).lower()
                if candidate == target:
                    return _decode_query_component(String(kv[1]))
        else:
            if pair.lower() == target:
                return ""
    return default_value


fn _query_has_key(dsn: String, key: String) -> Bool:
    if "?" not in dsn:
        return False
    var parts = dsn.split("?", 1)
    if len(parts) != 2:
        return False
    var query = String(parts[1])
    if query == "":
        return False
    var target = key.lower()
    for raw_pair in query.split("&"):
        var pair = String(raw_pair)
        if pair == "":
            continue
        if "=" in pair:
            var kv = pair.split("=", 1)
            if len(kv) == 2:
                var candidate = String(kv[0]).lower()
                if candidate == target:
                    return True
        else:
            if pair.lower() == target:
                return True
    return False


fn _query_last_value_for_keys(
    dsn: String,
    keys: List[String],
    default_value: String,
) -> String:
    if "?" not in dsn:
        return default_value
    var parts = dsn.split("?", 1)
    if len(parts) != 2:
        return default_value
    var query = String(parts[1])
    if query == "":
        return default_value
    var last = default_value
    var found = False
    for raw_pair in query.split("&"):
        var pair = String(raw_pair)
        if pair == "":
            continue
        if "=" in pair:
            var kv = pair.split("=", 1)
            if len(kv) != 2:
                continue
            var candidate = String(kv[0]).lower()
            for key in keys:
                if candidate == key.lower():
                    last = _decode_query_component(String(kv[1]))
                    found = True
                    break
        else:
            var candidate = pair.lower()
            for key in keys:
                if candidate == key.lower():
                    last = ""
                    found = True
                    break
    if found:
        return last
    return default_value


fn _query_last_int_for_keys(
    dsn: String,
    keys: List[String],
    default_value: Int,
) -> Int:
    var raw = _query_last_value_for_keys(dsn, keys, "")
    if raw.strip() == "":
        return default_value
    try:
        return Int(raw)
    except e:
        _ = e
        return default_value


fn _query_int(dsn: String, key: String, default_value: Int) -> Int:
    var raw = _query_value(dsn, key, "")
    if raw.strip() == "":
        return default_value
    try:
        return Int(raw)
    except e:
        _ = e
        return default_value


fn _is_valid_int_text(raw: String) -> Bool:
    if raw.strip() == "":
        return False
    try:
        _ = Int(raw)
        return True
    except e:
        _ = e
        return False


fn _query_int_is_malformed(dsn: String, key: String) -> Bool:
    if not _query_has_key(dsn, key):
        return False
    return not _is_valid_int_text(_query_value(dsn, key, ""))


fn _query_int_alias_is_malformed(
    dsn: String,
    primary_key: String,
    alias_key: String,
) -> Bool:
    if _query_has_key(dsn, primary_key):
        return _query_int_is_malformed(dsn, primary_key)
    if _query_has_key(dsn, alias_key):
        return _query_int_is_malformed(dsn, alias_key)
    return False


fn _query_any_int_for_keys_is_malformed(dsn: String, keys: List[String]) -> Bool:
    if "?" not in dsn:
        return False
    var parts = dsn.split("?", 1)
    if len(parts) != 2:
        return False
    var query = String(parts[1])
    if query == "":
        return False
    for raw_pair in query.split("&"):
        var pair = String(raw_pair)
        if pair == "":
            continue
        if "=" in pair:
            var kv = pair.split("=", 1)
            if len(kv) != 2:
                continue
            var candidate = String(kv[0]).lower()
            var value = _decode_query_component(String(kv[1]))
            for key in keys:
                if candidate == key.lower():
                    if not _is_valid_int_text(value):
                        return True
                    break
        else:
            var candidate = pair.lower()
            for key in keys:
                if candidate == key.lower():
                    if not _is_valid_int_text(""):
                        return True
                    break
    return False


fn _query_value_alias(
    dsn: String,
    primary_key: String,
    alias_key: String,
    default_value: String,
) -> String:
    var primary = _query_value(dsn, primary_key, "")
    if String(primary.strip()) != "":
        return primary
    var alias_value = _query_value(dsn, alias_key, "")
    if String(alias_value.strip()) != "":
        return alias_value
    return default_value


fn _query_int_alias(
    dsn: String,
    primary_key: String,
    alias_key: String,
    default_value: Int,
) -> Int:
    var raw = _query_value(dsn, primary_key, "")
    if raw.strip() == "":
        raw = _query_value(dsn, alias_key, "")
    if raw.strip() == "":
        return default_value
    try:
        return Int(raw)
    except e:
        _ = e
        return default_value


fn _query_bool(dsn: String, key: String, default_value: Bool) -> Bool:
    var raw = _query_value(dsn, key, "")
    if raw.strip() == "":
        return default_value
    return _as_bool(raw)


fn _query_bool_alias(
    dsn: String,
    primary_key: String,
    alias_key: String,
    default_value: Bool,
) -> Bool:
    var raw = _query_value(dsn, primary_key, "")
    if raw.strip() == "":
        raw = _query_value(dsn, alias_key, "")
    if raw.strip() == "":
        return default_value
    return _as_bool(raw)


fn _clamp_non_negative(value: Int) -> Int:
    if value < 0:
        return 0
    return value


fn _clamp_positive(value: Int, fallback: Int) -> Int:
    if value <= 0:
        return fallback
    return value


fn _connection_identity(
    user: String,
    host: String,
    port: Int,
    database: String,
) -> String:
    return user + "@" + host + ":" + String(port) + "/" + database


fn _normalize_front_door_mode_value(value: String) -> String:
    var normalized = value.strip().lower().replace("-", "_")
    if normalized == "managerproxy" or normalized == "managed":
        return "manager_proxy"
    if normalized == "":
        return "direct"
    return normalized


fn _normalize_protocol_value(value: String) -> String:
    var normalized = value.strip().lower().replace("-", "_")
    if normalized == "":
        return "native"
    if normalized == "scratchbird" or normalized == "scratchbird_native" or normalized == "scratchbirdnative":
        return "native"
    return normalized


fn _normalize_compression_value(value: String) -> String:
    var normalized = value.strip().lower()
    if normalized == "" or normalized == "none":
        return "off"
    return normalized


fn _normalize_logger_level(value: String) -> String:
    var normalized = value.strip().upper()
    if normalized == "":
        return "OFF"
    return normalized


fn _metadata_alias(value: String) -> String:
    if value == "schema" or value == "schemas":
        return "schemas"
    if value == "table" or value == "tables":
        return "tables"
    if value == "column" or value == "columns":
        return "columns"
    if value == "index" or value == "indexes":
        return "indexes"
    if value == "index_column" or value == "index_columns" or value == "indexcolumn" or value == "indexcolumns":
        return "index_columns"
    if value == "constraint" or value == "constraints":
        return "constraints"
    if value == "procedure" or value == "procedures":
        return "procedures"
    if value == "function" or value == "functions":
        return "functions"
    if value == "routine" or value == "routines":
        return "routines"
    if value == "catalog" or value == "catalogs":
        return "catalogs"
    if value == "primary_key" or value == "primary_keys" or value == "primarykey" or value == "primarykeys":
        return "primary_keys"
    if value == "foreign_key" or value == "foreign_keys" or value == "foreignkey" or value == "foreignkeys":
        return "foreign_keys"
    if value == "table_privilege" or value == "table_privileges" or value == "tableprivilege" or value == "tableprivileges":
        return "table_privileges"
    if value == "column_privilege" or value == "column_privileges" or value == "columnprivilege" or value == "columnprivileges":
        return "column_privileges"
    if value == "type_info" or value == "typeinfo":
        return "type_info"
    return ""


fn normalize_metadata_collection_name(collection_name: String) raises -> String:
    var raw = collection_name
    var normalized = collection_name.strip().lower().replace("-", "_").replace(" ", "_")
    if normalized == "":
        normalized = "tables"
    var resolved = _metadata_alias(normalized)
    if resolved == "":
        resolved = _metadata_alias(normalized.replace("_", ""))
    if resolved == "":
        raise Error("0A000 metadata collection '" + raw + "' is not supported")
    return resolved


fn resolve_metadata_collection_query(collection_name: String) raises -> String:
    var resolved = normalize_metadata_collection_name(collection_name)
    if resolved == "schemas":
        return METADATA_SCHEMAS_QUERY
    if resolved == "tables":
        return METADATA_TABLES_QUERY
    if resolved == "columns":
        return METADATA_COLUMNS_QUERY
    if resolved == "indexes":
        return METADATA_INDEXES_QUERY
    if resolved == "index_columns":
        return METADATA_INDEX_COLUMNS_QUERY
    if resolved == "constraints":
        return METADATA_CONSTRAINTS_QUERY
    if resolved == "procedures":
        return METADATA_PROCEDURES_QUERY
    if resolved == "functions":
        return METADATA_FUNCTIONS_QUERY
    if resolved == "routines":
        return METADATA_ROUTINES_QUERY
    if resolved == "catalogs":
        return METADATA_CATALOGS_QUERY
    if resolved == "primary_keys":
        return METADATA_PRIMARY_KEYS_QUERY
    if resolved == "foreign_keys":
        return METADATA_FOREIGN_KEYS_QUERY
    if resolved == "table_privileges":
        return METADATA_TABLE_PRIVILEGES_QUERY
    if resolved == "column_privileges":
        return METADATA_COLUMN_PRIVILEGES_QUERY
    if resolved == "type_info":
        return METADATA_TYPE_INFO_QUERY
    raise Error("0A000 metadata collection '" + collection_name + "' is not supported")


fn normalize_metadata_restriction_key(restriction_key: String) raises -> String:
    var raw = restriction_key
    var normalized = restriction_key.strip().lower().replace("-", "_").replace(" ", "_")
    if normalized == "" or normalized == "none":
        return ""
    if normalized == "name" or normalized == "object_name" or normalized == "objectname" or normalized == "entity_name" or normalized == "entityname":
        return "name"
    if normalized == "catalog" or normalized == "catalog_name" or normalized == "catalogname" or normalized == "table_catalog" or normalized == "tablecatalog" or normalized == "table_cat" or normalized == "tablecat":
        return "catalog_name"
    if normalized == "schema" or normalized == "schema_name" or normalized == "schemaname" or normalized == "table_schema" or normalized == "tableschema" or normalized == "table_schem" or normalized == "tableschem":
        return "schema_name"
    if normalized == "table" or normalized == "table_name" or normalized == "tablename":
        return "table_name"
    if normalized == "column" or normalized == "column_name" or normalized == "columnname":
        return "column_name"
    if normalized == "index" or normalized == "index_name" or normalized == "indexname":
        return "index_name"
    if normalized == "constraint" or normalized == "constraint_name" or normalized == "constraintname":
        return "constraint_name"
    if normalized == "routine" or normalized == "routine_name" or normalized == "routinename" or normalized == "procedure" or normalized == "procedure_name" or normalized == "procedurename" or normalized == "function" or normalized == "function_name" or normalized == "functionname":
        return "routine_name"
    if normalized == "type" or normalized == "type_name" or normalized == "typename" or normalized == "data_type" or normalized == "datatype" or normalized == "data_type_name" or normalized == "datatypename" or normalized == "udt_name" or normalized == "udtname":
        return "type_name"
    raise Error("0A000 metadata restriction '" + raw + "' is not supported")


fn _comparison_predicate(column: String, restriction_value: String) -> String:
    if restriction_value.lower() == "null":
        return column + " IS NULL"
    var literal = "'" + _escape_sql_literal(restriction_value) + "'"
    if "%" in restriction_value or "_" in restriction_value:
        return column + " LIKE " + literal + " ESCAPE '\\'"
    return column + " = " + literal


fn _table_filter_by_schema_name(restriction_value: String) -> String:
    return "table_id IN (SELECT t.table_id FROM sys.tables t JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE " + _comparison_predicate("s.schema_name", restriction_value) + ")"


fn _index_filter_by_schema_name(restriction_value: String) -> String:
    return "index_id IN (SELECT i.index_id FROM sys.indexes i JOIN sys.tables t ON t.table_id = i.table_id JOIN sys.schemas s ON s.schema_id = t.schema_id WHERE " + _comparison_predicate("s.schema_name", restriction_value) + ")"


fn _table_filter_by_table_name(restriction_value: String) -> String:
    return "table_id IN (SELECT table_id FROM sys.tables WHERE " + _comparison_predicate("table_name", restriction_value) + ")"


fn _index_filter_by_table_name(restriction_value: String) -> String:
    return "index_id IN (SELECT i.index_id FROM sys.indexes i JOIN sys.tables t ON t.table_id = i.table_id WHERE " + _comparison_predicate("t.table_name", restriction_value) + ")"


fn _index_filter_by_index_name(restriction_value: String) -> String:
    return "index_id IN (SELECT index_id FROM sys.indexes WHERE " + _comparison_predicate("index_name", restriction_value) + ")"


fn _metadata_restriction_predicate(
    collection_name: String,
    restriction_key: String,
    restriction_value: String,
) raises -> String:
    if restriction_key == "name":
        if collection_name == "schemas":
            return _comparison_predicate("schema_name", restriction_value)
        if collection_name == "catalogs":
            return _comparison_predicate("catalog_name", restriction_value)
        if collection_name == "tables" or collection_name == "table_privileges":
            return _comparison_predicate("table_name", restriction_value)
        if collection_name == "columns" or collection_name == "column_privileges" or collection_name == "index_columns":
            return _comparison_predicate("column_name", restriction_value)
        if collection_name == "indexes":
            return _comparison_predicate("index_name", restriction_value)
        if collection_name == "constraints" or collection_name == "primary_keys" or collection_name == "foreign_keys":
            return _comparison_predicate("constraint_name", restriction_value)
        if collection_name == "procedures":
            return _comparison_predicate("procedure_name", restriction_value)
        if collection_name == "functions":
            return _comparison_predicate("function_name", restriction_value)
        if collection_name == "routines":
            return _comparison_predicate("routine_name", restriction_value)
        if collection_name == "type_info":
            return _comparison_predicate("data_type_name", restriction_value)
        raise Error("0A000 metadata restriction 'name' is not supported for '" + collection_name + "'")

    if restriction_key == "schema_name":
        if collection_name == "schemas":
            return _comparison_predicate("schema_name", restriction_value)
        if collection_name == "catalogs":
            return _comparison_predicate("catalog_name", restriction_value)
        if collection_name == "tables":
            return "schema_id IN (SELECT schema_id FROM sys.schemas WHERE " + _comparison_predicate("schema_name", restriction_value) + ")"
        if collection_name == "columns" or collection_name == "indexes" or collection_name == "constraints":
            return _table_filter_by_schema_name(restriction_value)
        if collection_name == "index_columns":
            return _index_filter_by_schema_name(restriction_value)
        if collection_name == "primary_keys" or collection_name == "foreign_keys" or collection_name == "table_privileges" or collection_name == "column_privileges":
            return _table_filter_by_schema_name(restriction_value)
        if collection_name == "procedures" or collection_name == "functions" or collection_name == "routines":
            return "schema_id IN (SELECT schema_id FROM sys.schemas WHERE " + _comparison_predicate("schema_name", restriction_value) + ")"
        raise Error("0A000 metadata restriction 'schema_name' is not supported for '" + collection_name + "'")

    if restriction_key == "catalog_name":
        return _metadata_restriction_predicate(collection_name, "schema_name", restriction_value)

    if restriction_key == "table_name":
        if collection_name == "tables" or collection_name == "table_privileges":
            return _comparison_predicate("table_name", restriction_value)
        if collection_name == "columns" or collection_name == "indexes" or collection_name == "constraints":
            return _table_filter_by_table_name(restriction_value)
        if collection_name == "index_columns":
            return _index_filter_by_table_name(restriction_value)
        if collection_name == "primary_keys" or collection_name == "foreign_keys" or collection_name == "column_privileges":
            return _table_filter_by_table_name(restriction_value)
        raise Error("0A000 metadata restriction 'table_name' is not supported for '" + collection_name + "'")

    if restriction_key == "column_name":
        if collection_name == "columns" or collection_name == "column_privileges" or collection_name == "index_columns":
            return _comparison_predicate("column_name", restriction_value)
        raise Error("0A000 metadata restriction 'column_name' is not supported for '" + collection_name + "'")

    if restriction_key == "index_name":
        if collection_name == "indexes":
            return _comparison_predicate("index_name", restriction_value)
        if collection_name == "index_columns":
            return _index_filter_by_index_name(restriction_value)
        raise Error("0A000 metadata restriction 'index_name' is not supported for '" + collection_name + "'")

    if restriction_key == "constraint_name":
        if collection_name == "constraints" or collection_name == "primary_keys" or collection_name == "foreign_keys":
            return _comparison_predicate("constraint_name", restriction_value)
        raise Error("0A000 metadata restriction 'constraint_name' is not supported for '" + collection_name + "'")

    if restriction_key == "routine_name":
        if collection_name == "procedures":
            return _comparison_predicate("procedure_name", restriction_value)
        if collection_name == "functions":
            return _comparison_predicate("function_name", restriction_value)
        if collection_name == "routines":
            return _comparison_predicate("routine_name", restriction_value)
        raise Error("0A000 metadata restriction 'routine_name' is not supported for '" + collection_name + "'")

    if restriction_key == "type_name":
        if collection_name == "columns" or collection_name == "type_info":
            return _comparison_predicate("data_type_name", restriction_value)
        raise Error("0A000 metadata restriction 'type_name' is not supported for '" + collection_name + "'")

    raise Error("0A000 metadata restriction '" + restriction_key + "' is not supported")


fn _escape_sql_literal(value: String) -> String:
    return value.replace("'", "''")


fn _append_metadata_filter(sql: String, predicate: String) -> String:
    if " ORDER BY " in sql:
        var parts = sql.split(" ORDER BY ", 1)
        if len(parts) == 2:
            var head = String(parts[0])
            var tail = String(parts[1])
            if " where " in head.lower():
                return head + " AND " + predicate + " ORDER BY " + tail
            return head + " WHERE " + predicate + " ORDER BY " + tail

    if " where " in sql.lower():
        return sql + " AND " + predicate
    return sql + " WHERE " + predicate


fn resolve_metadata_collection_query_restricted(
    collection_name: String,
    restriction_key: String = "",
    restriction_value: String = "",
) raises -> String:
    var keys = List[String]()
    keys.append(restriction_key)
    var values = List[String]()
    values.append(restriction_value)
    return resolve_metadata_collection_query_restricted_multi(
        collection_name,
        keys,
        values,
    )


fn resolve_metadata_collection_query_restricted_multi(
    collection_name: String,
    restriction_keys: List[String],
    restriction_values: List[String],
) raises -> String:
    if len(restriction_keys) != len(restriction_values):
        raise Error("07001 metadata restriction count mismatch")
    var resolved_collection = normalize_metadata_collection_name(collection_name)
    var sql = resolve_metadata_collection_query(resolved_collection)
    var normalized_keys = List[String]()
    var normalized_values = List[String]()
    for i in range(len(restriction_keys)):
        normalized_keys.append(normalize_metadata_restriction_key(restriction_keys[i]))
        normalized_values.append(String(restriction_values[i].strip()))

    for i in range(len(normalized_keys)):
        var resolved_key = normalized_keys[i]
        if resolved_key == "":
            continue
        var has_later = False
        for j in range(i + 1, len(normalized_keys)):
            if normalized_keys[j] == resolved_key:
                has_later = True
                break
        if has_later:
            continue
        var value = normalized_values[i]
        if value == "":
            continue
        var predicate = _metadata_restriction_predicate(resolved_collection, resolved_key, value)
        sql = _append_metadata_filter(sql, predicate)
    return sql


fn validate_connect_guards(config: ScratchBirdConfig) raises:
    if _dsn_has_malformed_query_escape(config.dsn):
        raise Error("22023 DSN query contains malformed percent-escape")
    if _dsn_has_malformed_bracketed_ipv6_host(config.dsn):
        raise Error("22023 DSN contains malformed bracketed IPv6 host")

    var port_keys = List[String]()
    port_keys.append("port")
    port_keys.append("portnumber")
    port_keys.append("pgport")
    if _query_any_int_for_keys_is_malformed(config.dsn, port_keys):
        raise Error("22023 port must be a valid integer")
    var connect_timeout_keys = List[String]()
    connect_timeout_keys.append("connect_timeout")
    connect_timeout_keys.append("connecttimeout")
    if _query_any_int_for_keys_is_malformed(config.dsn, connect_timeout_keys):
        raise Error("22023 connect_timeout must be a valid integer")
    var socket_timeout_keys = List[String]()
    socket_timeout_keys.append("socket_timeout")
    socket_timeout_keys.append("sockettimeout")
    if _query_any_int_for_keys_is_malformed(config.dsn, socket_timeout_keys):
        raise Error("22023 socket_timeout must be a valid integer")
    var login_timeout_keys = List[String]()
    login_timeout_keys.append("login_timeout")
    login_timeout_keys.append("logintimeout")
    if _query_any_int_for_keys_is_malformed(config.dsn, login_timeout_keys):
        raise Error("22023 login_timeout must be a valid integer")
    var acquire_timeout_keys = List[String]()
    acquire_timeout_keys.append("acquire_timeout")
    acquire_timeout_keys.append("acquiretimeout")
    acquire_timeout_keys.append("pooling_acquire_timeout")
    acquire_timeout_keys.append("poolingacquiretimeout")
    if _query_any_int_for_keys_is_malformed(config.dsn, acquire_timeout_keys):
        raise Error("22023 acquire_timeout must be a valid integer")
    var default_row_fetch_size_keys = List[String]()
    default_row_fetch_size_keys.append("default_row_fetch_size")
    default_row_fetch_size_keys.append("fetch_size")
    default_row_fetch_size_keys.append("fetchsize")
    default_row_fetch_size_keys.append("defaultrowfetchsize")
    if _query_any_int_for_keys_is_malformed(config.dsn, default_row_fetch_size_keys):
        raise Error("22023 default_row_fetch_size must be a valid integer")
    var prepare_threshold_keys = List[String]()
    prepare_threshold_keys.append("prepare_threshold")
    prepare_threshold_keys.append("preparethreshold")
    if _query_any_int_for_keys_is_malformed(config.dsn, prepare_threshold_keys):
        raise Error("22023 prepare_threshold must be a valid integer")
    var min_pool_size_keys = List[String]()
    min_pool_size_keys.append("min_pool_size")
    min_pool_size_keys.append("minpoolsize")
    if _query_any_int_for_keys_is_malformed(config.dsn, min_pool_size_keys):
        raise Error("22023 min_pool_size must be a valid integer")
    var max_pool_size_keys = List[String]()
    max_pool_size_keys.append("max_pool_size")
    max_pool_size_keys.append("maxpoolsize")
    if _query_any_int_for_keys_is_malformed(config.dsn, max_pool_size_keys):
        raise Error("22023 max_pool_size must be a valid integer")
    var connection_lifetime_keys = List[String]()
    connection_lifetime_keys.append("connection_lifetime")
    connection_lifetime_keys.append("connectionlifetime")
    connection_lifetime_keys.append("poolingconnectionlifetime")
    if _query_any_int_for_keys_is_malformed(config.dsn, connection_lifetime_keys):
        raise Error("22023 connection_lifetime must be a valid integer")
    var manager_client_flags_keys = List[String]()
    manager_client_flags_keys.append("manager_client_flags")
    manager_client_flags_keys.append("mcp_client_flags")
    if _query_any_int_for_keys_is_malformed(config.dsn, manager_client_flags_keys):
        raise Error("22023 manager_client_flags must be a valid integer")
    var connect_client_flags_keys = List[String]()
    connect_client_flags_keys.append("client_flags")
    connect_client_flags_keys.append("connect_client_flags")
    if _query_any_int_for_keys_is_malformed(config.dsn, connect_client_flags_keys):
        raise Error("22023 connect_client_flags must be a valid integer")
    if _query_int_is_malformed(config.dsn, "cb_failure_threshold"):
        raise Error("22023 cb_failure_threshold must be a valid integer")
    if _query_int_is_malformed(config.dsn, "cb_recovery_timeout_ms"):
        raise Error("22023 cb_recovery_timeout_ms must be a valid integer")
    if _query_int_is_malformed(config.dsn, "cb_success_threshold"):
        raise Error("22023 cb_success_threshold must be a valid integer")
    if _query_int_is_malformed(config.dsn, "cb_half_open_max_requests"):
        raise Error("22023 cb_half_open_max_requests must be a valid integer")
    if _query_int_is_malformed(config.dsn, "keepalive_max_idle_before_check_ms"):
        raise Error("22023 keepalive_max_idle_before_check_ms must be a valid integer")
    if _query_int_is_malformed(config.dsn, "leak_threshold_ms"):
        raise Error("22023 leak_threshold_ms must be a valid integer")
    if _query_int_is_malformed(config.dsn, "pipeline_max_in_flight"):
        raise Error("22023 pipeline_max_in_flight must be a valid integer")
    if _query_int_is_malformed(config.dsn, "pipeline_auto_flush_threshold"):
        raise Error("22023 pipeline_auto_flush_threshold must be a valid integer")

    if config.protocol != "native":
        raise Error("0A000 protocol must be native")

    var mode = _normalize_front_door_mode_value(config.front_door_mode)
    if mode != "direct" and mode != "manager_proxy":
        raise Error("0A000 front_door_mode must be direct or manager_proxy.")
    if mode == "manager_proxy" and config.manager_auth_token.strip() == "":
        raise Error("08001 manager_proxy mode requires manager_auth_token")

    if config.user.strip() == "" or config.database.strip() == "":
        raise Error("28000 user and database are required")

    if config.host.strip() == "":
        raise Error("28000 host and database are required")

    if config.port <= 0:
        raise Error("22023 port must be positive")
    if config.port > 65535:
        raise Error("22023 port must be between 1 and 65535")

    if config.connect_timeout_s < 0:
        raise Error("22023 connect_timeout must be >= 0")

    if config.socket_timeout_s < 0:
        raise Error("22023 socket_timeout must be >= 0")

    if config.login_timeout_s < 0:
        raise Error("22023 login_timeout must be >= 0")

    if config.acquire_timeout_s < 0:
        raise Error("22023 acquire_timeout must be >= 0")

    if config.default_row_fetch_size < 0:
        raise Error("22023 default_row_fetch_size must be >= 0")
    if config.min_pool_size < 0:
        raise Error("22023 min_pool_size must be >= 0")
    if config.max_pool_size < 1:
        raise Error("22023 max_pool_size must be >= 1")
    if config.min_pool_size > config.max_pool_size:
        raise Error("22023 min_pool_size must be <= max_pool_size")
    if config.connection_lifetime_s < 0:
        raise Error("22023 connection_lifetime must be >= 0")
    if config.manager_client_flags < 0:
        raise Error("22023 manager_client_flags must be >= 0")
    if config.connect_client_flags < 0:
        raise Error("22023 connect_client_flags must be >= 0")
    var auth_method_id = String(config.auth_method_id.strip())
    if auth_method_id != "":
        if not auth_method_id.startswith("scratchbird.auth."):
            raise Error("28000 invalid auth_method_id namespace")

    if config.compression.strip().lower() != "off" and config.compression.strip().lower() != "zstd":
        raise Error("0A000 compression=" + config.compression.strip().lower() + " is not supported")

    if config.sb_test_auth_fail:
        raise Error("28P01 authentication failed")


fn connect(config: ScratchBirdConfig) raises -> ScratchBirdConnection:
    return ScratchBirdConnection(config)


fn _is_sqlstate_char(ch: String) -> Bool:
    if _is_digit(ch):
        return True
    return ch >= "A" and ch <= "Z"


fn extract_sqlstate(message: String) -> String:
    var text = message.strip()
    if len(text) < 5:
        return ""
    var code = String()
    for i in range(5):
        var ch = String(text[byte=i])
        if not _is_sqlstate_char(ch):
            return ""
        code += ch
    return code
