# ScratchBird Driver Baseline Requirement Mapping (S0)

## Scope

- Lane-local S0 artifact only for `lanes/active/drivers/ruby`.
- Baseline reflects currently present lane code/tests only.
- Mapping is grouped by JDBCBL capability groups: `CONN`, `TXN`, `EXEC`, `META`, `TYPE`, `ERR`, `RES`.

## MGA Recovery Contract

- This lane follows ScratchBird's MGA/state-based engine recovery model.
- Reconnect or reopen only repairs transport and session state.
- Reconnect never resurrects abandoned in-flight transactions or replay lost statements.
- Transaction recovery in the lane means reset, rollback, reopen, or retry against engine truth.
- Result resume is valid only for explicit suspended protocol states.
- `Scratchbird::Client#resume_portal` now fails closed with `55000` unless the
  server first reported `MSG_PORTAL_SUSPENDED`.
- `Scratchbird::Connection#prepare_transaction`, `#commit_prepared`, and
  `#rollback_prepared` expose explicit prepared/limbo control through
  canonical transaction-control SQL.
- `Scratchbird::Connection#supports_dormant_reattach?` is explicit and false,
  and `#detach_to_dormant` / `#reattach_dormant` fail closed until a public
  dormant front-door exists.
- `Scratchbird::Client#begin_transaction(options)` now exposes the canonical
  `READ COMMITTED` sub-mode selector directly through
  `:read_committed_mode`, including `READ COMMITTED READ CONSISTENCY`.
- `Scratchbird::Protocol.canonical_read_committed_mode_label(...)` keeps that
  selector source-visible for auditors and lane tests.
- Native `READY` status plus `current_txn_id` are authoritative for
  transaction activity in this lane. ScratchBird sessions stay always in a
  transaction, so autocommit-off statements execute against the server-owned
  session boundary instead of injecting a synthetic client-side `BEGIN`.
- `Scratchbird::ErrorMapper.retry_scope_for_sqlstate(...)` makes the retry
  boundary explicit: `40001`/`40P01` => statement only, `08xxx` => reconnect
  only, all other SQLSTATEs => no automatic replay.
- See `../../../../public_audit_summary`.

## CONN (JDBCBL: `CONN`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/config.rb:48` (`Config.parse`, URI and key-value handling)
  - `lib/scratchbird/config.rb:91` (`normalize_native_protocol`, `normalize_front_door_mode`)
  - `lib/scratchbird/config.rb:17` (`Config` auth/bootstrap fields, including `auth_token`)
  - `lib/scratchbird/connection.rb:19` (`Connection#initialize`, `#close`, `#closed?`)
  - `lib/scratchbird/connection.rb:175` (`Connection#resolved_auth_context`)
  - `lib/scratchbird.rb:10` (`Scratchbird.probe_auth_surface`)
  - `lib/scratchbird/client.rb:68` (`Client#connect`, staged bootstrap/auth, resolved auth reporting, connect/auth failure cleanup)
  - `lib/scratchbird/client.rb:112` (`Client#get_resolved_auth_context`, `#probe_auth_surface`)
  - `lib/scratchbird/client.rb:400` (`#connect_tcp`) and `lib/scratchbird/client.rb:407` (`#wrap_tls`)
  - `lib/scratchbird/client.rb:469` (`#perform_manager_connect`)
  - `lib/scratchbird/client.rb:525` (`#probe_direct_auth_surface`, `#probe_manager_auth_surface`)
  - `lib/scratchbird/protocol.rb:185` (`parse_auth_request`) and `lib/scratchbird/protocol.rb:192` (`parse_auth_continue`)
  - `lib/scratchbird/protocol.rb:30` (shared auth enum alignment including `SCRAM_SHA_512`, `TOKEN`, `PEER`, `REATTACH`)
  - `lib/scratchbird/scram.rb:8` (`SCRAM` digest-generalized client for SHA-256 and SHA-512)
- Lane-local test anchors:
  - `test/test_config.rb:11` (`test_parse_uri`)
  - `test/test_config.rb:27` (`test_parse_key_value`)
  - `test/test_config.rb:40` (`test_parse_manager_proxy_params`)
  - `test/test_config.rb:54` (`test_parse_auth_plugin_and_pinning_params`)
  - `test/test_config.rb:49` (`test_invalid_front_door_mode_raises`)
  - `test/test_conn_auth_protocol.rb:17` (`test_connect_requires_user_and_database`)
  - `test/test_conn_auth_protocol.rb:27` (`test_connect_allows_binary_transfer_false`)
  - `test/test_conn_auth_protocol.rb:36` (`test_connect_allows_zstd_compression`)
  - `test/test_conn_auth_protocol.rb:45` (`test_connect_rejects_non_native_protocol`)
  - `test/test_conn_auth_protocol.rb:54` (`test_connect_rejects_invalid_front_door_mode`)
  - `test/test_conn_auth_protocol.rb:63` (`test_wrap_tls_rejects_sslmode_disable`)
  - `test/test_conn_auth_protocol.rb:72` (`test_connect_closes_socket_when_manager_proxy_auth_token_missing`)
  - `test/test_conn_auth_protocol.rb:88` (`test_manager_proxy_auth_failure_raises_auth_error`)
  - `test/test_conn_auth_protocol.rb:116` (`test_manager_proxy_connect_success`)
  - `test/test_conn_auth_protocol.rb:145` (`test_probe_auth_surface_direct_reports_scram_sha512`)
  - `test/test_conn_auth_protocol.rb:178` (`test_probe_auth_surface_manager_proxy_reports_token`)
  - `test/test_conn_auth_protocol.rb:144` (`test_handshake_scram_success_with_server_verifier`)
  - `test/test_conn_auth_protocol.rb:211` (`test_handshake_scram_rejects_bad_server_verifier`)
  - `test/test_conn_auth_protocol.rb:244` (`test_handshake_scram_sha512_success`)
  - `test/test_conn_auth_protocol.rb:306` (`test_handshake_token_auth_uses_auth_token`)
  - `test/test_conn_auth_protocol.rb:351` (`test_handshake_peer_fails_closed`)
  - `test/test_conn_auth_protocol.rb:392` (`test_protocol_parse_auth_continue_round_trip`)
  - `test/test_conn_auth_protocol.rb:403` (`test_protocol_parse_auth_continue_rejects_truncated_payload`)
  - `test/test_integration.rb:11` (`test_select`, env-gated)
  - `test/test_integration.rb:50` (`test_manager_proxy_select`, env-gated)
  - `test/test_integration.rb:62` (`test_tls_verify_ca_select`, env-gated)
  - `test/test_integration.rb:74` (`test_tls_verify_full_select`, env-gated)
- Gaps / next actions:
  - None in lane-local baseline scope; live MCP/TLS matrices are env-gated in `test/test_integration.rb`.

## TXN (JDBCBL: `TXN`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/connection.rb:37` (`Connection#begin_transaction`, `#commit`, `#rollback`)
  - `lib/scratchbird/connection.rb:50` (`Connection#savepoint`, `#rollback_to_savepoint`, `#release_savepoint`)
  - `lib/scratchbird/connection.rb:52` (`Connection#in_transaction?`, transaction-state gate delegated to client)
  - `lib/scratchbird/connection.rb:57` (`Connection#execute`/`#stream` autocommit gate; autocommit-off statements rely on the server-owned session transaction instead of a synthetic local BEGIN`)
  - `lib/scratchbird/connection.rb:78` (`Connection#execute_prepared`, `#stream_prepared` uses the same transaction gate as direct execution)
  - `lib/scratchbird/client.rb:42` (`Client#txn_id` reader + `#in_transaction?`)
  - `lib/scratchbird/client.rb:125` (`Client#begin_transaction`, `#commit`, `#rollback`, READY-driven transaction activity including active-with-zero-txn-id native sessions)
  - `lib/scratchbird/client.rb:146` (`#savepoint`, `#release_savepoint`, `#rollback_to_savepoint`)
  - `lib/scratchbird/protocol.rb:300` (transaction payload builders and `canonical_read_committed_mode_label(...)`)
- Lane-local test anchors:
  - `test/test_txn_exec_parity.rb:67` (`test_execute_starts_transaction_once_when_autocommit_disabled`)
  - `test/test_txn_exec_parity.rb:79` (`test_commit_and_rollback_reset_transaction_gate`)
  - `test/test_txn_exec_parity.rb:94` (`test_begin_transaction_forwards_mga_options`)
  - `test/test_txn_exec_parity.rb:106` (`test_statement_execute_and_stream_use_connection_transaction_gate`)
  - `test/test_txn_exec_parity.rb:129` (`test_connection_savepoint_api_forwards_to_client`)
  - `test/test_wire_txn_exec.rb:98` (`test_txn_id_transitions_follow_ready_frames`)
  - `test/test_wire_txn_exec.rb:121` (`test_begin_transaction_encodes_read_committed_mode`)
  - `test/test_wire_txn_exec.rb:140` (`test_begin_transaction_rejects_read_committed_mode_with_snapshot_alias`)
  - `test/test_wire_txn_exec.rb:130` (`test_commit_raises_mapped_error_but_applies_ready_state_after_abort`)
  - `test/test_integration.rb:113` (`test_txn_id_transitions_follow_runtime_ready_frames`, env-gated)
  - `test/test_integration.rb:136` (`test_commit_and_rollback_behavior_after_server_abort`, env-gated)
- Gaps / next actions:
  - Public begin now exposes `read_committed_mode`, but richer non-READ-COMMITTED transaction-option parity remains open under `DMRW-005`.

## EXEC (JDBCBL: `EXEC`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/sql.rb:12` (`Sql.normalize`, positional/named rewrite) and `lib/scratchbird/sql.rb:26` / `:31` (`normalize_callable`, JDBC escape callable translation)
  - `lib/scratchbird/connection.rb:73` (`#execute`, `#query`, `#stream` with shared transaction gate)
  - `lib/scratchbird/connection.rb:89` (`#native_sql`, `#native_callable_sql`, `#call`, `#query_multi`, `#execute_batch`, `#execute_with_generated_keys`)
  - `lib/scratchbird/connection.rb:155` (`#prepare`) and `lib/scratchbird/connection.rb:160` (`#execute_prepared`, `#stream_prepared`)
  - `lib/scratchbird/statement.rb:21` (`Statement#execute`, `#stream` delegates through connection gate)
  - `lib/scratchbird/client.rb:260` (`Client#query`, `#stream`, core execute paths)
  - `lib/scratchbird/client.rb:272` (`#native_sql`, `#native_callable_sql`, `#call`, `#query_multi`, `#execute_batch`, `#execute_with_generated_keys`)
  - `lib/scratchbird/client.rb:374` (`#prepare`, `#execute`, `#execute_stream`)
  - `lib/scratchbird/client.rb:406` (`#deallocate`, explicit prepared-statement close protocol flow) and `lib/scratchbird/client.rb:418` (`#cancel`)
  - `lib/scratchbird/client.rb:842` (`execute_query_loop`, command tag/rowcount/generated key parsing), `lib/scratchbird/client.rb:994` (`summarize_result`), `lib/scratchbird/client.rb:1012` (`split_sql_statements`), and `lib/scratchbird/client.rb:1061` (`ResultStream`)
  - `lib/scratchbird/protocol.rb:388` (`parse_row_description`), `lib/scratchbird/protocol.rb:420` (`parse_data_row`), `lib/scratchbird/protocol.rb:451` (`parse_command_complete`)
- Lane-local test anchors:
  - `test/test_sql.rb:11` (`test_normalize_positional`)
  - `test/test_sql.rb:18` (`test_normalize_named`)
  - `test/test_sql.rb:25` (`test_normalize_binary`)
  - `test/test_sql.rb:32` (`test_normalize_callable_escape_syntax`) and `test/test_sql.rb:38` (`test_normalize_callable_sql_passthrough`)
  - `test/test_txn_exec_parity.rb:144` (`test_query_and_stream_forward_options`)
  - `test/test_txn_exec_parity.rb:156` (`test_statement_execute_and_stream_use_connection_transaction_gate`)
  - `test/test_txn_exec_parity.rb:206` (`test_native_sql_and_native_callable_sql_forward_to_client`)
  - `test/test_txn_exec_parity.rb:219` (`test_exec_parity_surfaces_use_transaction_gate_and_forward`)
  - `test/test_result_stream.rb:60` (`test_stream_each_hash_tracks_command_summary`) and `test/test_result_stream.rb:88` (`test_stream_rejects_second_consumption`)
  - `test/test_integration.rb:24` (`test_prepare_bind`, env-gated parameter execution)
  - `test/test_integration.rb:50` (`test_cancel`, env-gated)
  - `test/test_integration.rb:72` (`test_query_multi`, env-gated)
  - `test/test_integration.rb:90` (`test_execute_batch`, env-gated)
  - `test/test_integration.rb:108` (`test_call_callable_escape_syntax`, env-gated)
  - `test/test_integration.rb:125` (`test_execute_with_generated_keys`, env-gated)
  - `test/test_result_stream.rb:102` (`test_stream_resumes_after_portal_suspended`)
  - `test/test_wire_txn_exec.rb:11` (`test_query_resumes_portal_and_continues_rows`)
  - `test/test_wire_txn_exec.rb:40` (`test_query_multi_handles_single_request_multi_result_framing`)
  - `test/test_wire_txn_exec.rb:80` (`test_deallocate_waits_for_close_complete_then_ready`)
  - `test/test_integration.rb:170` (`test_prepared_close_sequence_roundtrip`, env-gated)
- Gaps / next actions:
  - None in lane-local baseline scope; retry-boundary helpers are now explicit in `lib/scratchbird/errors.rb`.

## META (JDBCBL: `META`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/connection.rb:131` (`Connection#query_metadata_with_restrictions`, `#get_schema_with_restrictions`, `#get_schema_tree` entry points)
  - `lib/scratchbird/client.rb:358` (`Client#query_metadata_with_restrictions`, `#get_schema_with_restrictions`, `#get_schema_tree` metadata execution/routing)
  - `lib/scratchbird/metadata.rb:11` (`Metadata` query constants and collection catalogs for schemas/tables/columns/indexes/constraints/procedures/functions/catalogs/types/key+privilege families/ddl_fields)
  - `lib/scratchbird/metadata.rb:121` (`normalize_collection_name` / `resolve_collection_query`, collection alias normalization and query resolution)
  - `lib/scratchbird/metadata.rb:139` (`normalize_restrictions` / `filter_rows_by_restrictions`, first-class metadata restriction filtering)
  - `lib/scratchbird/metadata.rb:116` (`schema_paths_for_navigation`, schema path normalization/de-duplication with optional parent expansion mode)
  - `lib/scratchbird/metadata.rb:141` (`build_schema_tree`, recursive schema tree shaping with per-parent uniqueness and terminal-node tracking)
  - `lib/scratchbird/metadata.rb:173` (`expand_schema_metadata_rows`, metadata-row parent expansion with synthetic ancestor rows)
  - `lib/scratchbird/metadata.rb:202` (`build_database_default_metadata_rows`, database->default branch-style metadata row shaping)
  - `lib/scratchbird.rb:18` (exports metadata module via top-level require)
- Lane-local test anchors:
  - `test/test_metadata_execution.rb:52` (`test_query_metadata_resolves_collection_alias`)
  - `test/test_metadata_execution.rb:64` (`test_query_metadata_with_restrictions_filters_rows`)
  - `test/test_metadata_execution.rb:78` (`test_query_metadata_with_restrictions_supports_null_and_ignores_unknown_keys`)
  - `test/test_metadata_execution.rb:65` (`test_get_schema_expands_parent_rows_from_config`)
  - `test/test_metadata_execution.rb:104` (`test_get_schema_with_restrictions_filters_then_expands_parents`)
  - `test/test_metadata_execution.rb:76` (`test_get_schema_tree_returns_recursive_nodes`)
  - `test/test_metadata_execution.rb:121` (`test_connection_get_schema_tree_shapes_database_default_rows`)
  - `test/test_metadata_execution.rb:146` (`test_connection_get_schema_with_restrictions_forwards_to_client`)
  - `test/test_metadata_execution.rb:62` (`test_query_metadata_resolves_extended_collection_aliases`)
  - `test/test_metadata_execution.rb:108` (`test_query_metadata_with_restrictions_applies_collection_specific_aliases`)
  - `test/test_metadata_execution.rb:120` (`test_query_metadata_with_restrictions_ignores_non_family_filters`)
  - `test/test_integration.rb:159` (`test_metadata_collections_and_restrictions_fixture_shape`, env-gated)
  - `test/test_metadata_recursive_schema.rb:11` (`test_database_default_branch_style_metadata_rows`)
  - `test/test_metadata_recursive_schema.rb:40` (`test_dotted_schema_parent_expansion`)
  - `test/test_metadata_recursive_schema.rb:64` (`test_tree_uniqueness_within_parent`)
  - `test/test_metadata_recursive_schema.rb:80` (`test_same_object_name_under_different_parents_is_preserved`)
- Gaps / next actions:
  - None in lane-local baseline scope.

## TYPE (JDBCBL: `TYPE`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/types.rb:67` (`Types` OID map and format constants)
  - `lib/scratchbird/types.rb:128` (`Types.encode_param`)
  - `lib/scratchbird/types.rb:206` (`Types.decode`)
  - `lib/scratchbird/types.rb:260` (`Types.decode_binary_value`)
  - `lib/scratchbird/client.rb:314` (`Client#decode_row` delegates to `Types.decode`)
  - `lib/scratchbird/client.rb:758` (`send_extended_query` uses `Types.encode_param`)
- Lane-local test anchors:
  - `test/test_types.rb:11` (`test_decode_uuid`)
  - `test/test_types.rb:17` (`test_decode_array`)
  - `test/test_types.rb:22` (`test_encode_decode_integer_float_and_text_roundtrip`)
  - `test/test_types.rb:34` (`test_encode_decode_date_and_timestamp_roundtrip`)
  - `test/test_types.rb:46` (`test_encode_decode_numeric_json_and_jsonb_roundtrip`)
  - `test/test_types.rb:61` (`test_encode_decode_range_and_composite`)
  - `test/test_types.rb:98` (`test_encode_decode_vector_and_null`)
  - `test/test_types.rb:112` (`test_decode_unknown_oid_text_and_binary_paths`)
  - `test/test_integration.rb:37` (`test_types_fixture`, env-gated)
- Gaps / next actions:
  - None in lane-local baseline scope.

## ERR (JDBCBL: `ERR`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/errors.rb:9` (driver error classes)
  - `lib/scratchbird/errors.rb:35` (`ErrorMapper.from_sqlstate`)
  - `lib/scratchbird/client.rb:449` (`Client#handle_query_error`, wire error parse + typed SQLSTATE mapping with preserved class propagation)
  - `lib/scratchbird/protocol.rb:510` (`parse_error_message`)
- Lane-local test anchors:
  - `test/test_errors.rb:11` (`test_sqlstate_mappings_cover_core_error_families`)
  - `test/test_errors.rb:38` (`test_unknown_sqlstate_falls_back_to_base_error`)
  - `test/test_errors.rb:45` (`test_client_handle_query_error_preserves_typed_sqlstate_mapping`)
  - `test/test_integration.rb:86` (`test_cancel`) asserts runtime error class/SQLSTATE mapping under live cancel.
  - `test/test_integration.rb:190` (`test_constraint_violation_maps_to_integrity_error`, env-gated)
  - `test/test_integration.rb:217` (`test_auth_failure_maps_to_auth_error`, env-gated)
- Gaps / next actions:
  - None in lane-local baseline scope.

## RES (JDBCBL: `RES`)

- Current status: `Implemented`
- Lane-local source anchors:
  - `lib/scratchbird/connection.rb:28` (`Connection#close`, `#closed?`, close-state finalize even when disconnect raises)
  - `lib/scratchbird/connection.rb:85` (`Connection#close_prepared`)
  - `lib/scratchbird/statement.rb:31` (`Statement#close`, `#closed?`, best-effort prepared deallocation)
  - `lib/scratchbird/result.rb:11` (`Result` container/enumeration helpers)
  - `lib/scratchbird/client.rb:86` (`Client#connect`, same-client reconnect now clears abandoned prepared/session caches before replacement handshake)
  - `lib/scratchbird/client.rb:112` (`Client#close` idempotent cleanup path: socket, keepalive, leak guard, detector teardown)
  - `lib/scratchbird/client.rb:870` (`with_resilience` wrapper for circuit breaker + telemetry + keepalive validation)
  - `lib/scratchbird/client.rb:1150` (`ResultStream` single-consumption iterator and rowcount finalization)
  - `lib/scratchbird/circuit_breaker.rb:4`, `lib/scratchbird/keepalive.rb:7`, `lib/scratchbird/telemetry.rb:6`, `lib/scratchbird/leak_detector.rb:4` (resilience helper implementations)
- Lane-local test anchors:
  - `test/test_integration.rb:11` / `:24` / `:37` (explicit `conn.close` in `ensure`; env-gated)
  - `test/test_result_stream.rb:44` (`test_result_supports_each_hash_and_generated_key`)
  - `test/test_result_stream.rb:60` (`test_stream_each_hash_tracks_command_summary`)
  - `test/test_result_stream.rb:88` (`test_stream_rejects_second_consumption`)
  - `test/test_resource_resilience.rb:176` (`test_connection_close_marks_closed_when_disconnect_raises_once`)
  - `test/test_resource_resilience.rb:192` (`test_statement_close_is_idempotent_when_close_prepared_raises`)
  - `test/test_resource_resilience.rb:214` (`test_client_close_cleans_resilience_helpers_when_socket_absent`)
  - `test/test_resource_resilience.rb:239` (`test_client_close_is_idempotent_when_socket_close_raises`)
  - `test/test_resource_resilience.rb:289` (`test_client_connect_clears_abandoned_session_state_on_same_instance_reuse`)
  - `test/test_resource_resilience.rb:264` (`test_with_resilience_success_records_telemetry_and_circuit_success`)
  - `test/test_resource_resilience.rb:287` (`test_with_resilience_failure_records_telemetry_and_circuit_failure`)
  - `test/test_resource_resilience.rb:311` (`test_with_resilience_runs_ping_when_keepalive_validation_required`)
  - `test/test_resource_resilience.rb:333` (`test_with_resilience_raises_when_circuit_is_open`)
  - `test/test_integration.rb:224` (`test_socket_drop_releases_keepalive_and_leak_tracking_on_close`, env-gated)
- Gaps / next actions:
  - None in lane-local baseline scope.
