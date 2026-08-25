// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/engine.h"
#include "scratchbird/engine/sblr_envelope.hpp"
#include "canonical_aggregate_registry.hpp"
#include "core/agents/resource_governance_admission.hpp"
#include "datatype_catalog_manifest.hpp"
#include "executor_foundation.hpp"
#include "cluster_provider/cluster_provider.hpp"
#include "database_format.hpp"
#include "hash_digest.hpp"
#include "extensibility/executable_object_lifecycle.hpp"
#include "local_transaction_store.hpp"
#include "optimizer/model_family_coordinator.hpp"
#include "sblr_dispatch.hpp"
#include "sblr_opcode_stream.hpp"
#include "sblr_executor_availability_registry.hpp"
#include "sblr_literal_runtime.hpp"
#include "sblr_parameter_runtime.hpp"
#include "sblr_parameter_set_registry.hpp"
#include "sblr_source_map_descriptor_registry.hpp"
#include "sblr_source_map_runtime.hpp"
#include "sblr_error_vector_runtime.hpp"
#include "sblr_error_vector_descriptor_registry.hpp"
#include "sblr_diagnostic_identity_registry.hpp"
#include "sblr_transaction_begin_authority.hpp"
#include "sblr_transaction_begin_runtime.hpp"
#include "sblr_transaction_commit_runtime.hpp"
#include "sblr_transaction_rollback_runtime.hpp"
#include "sblr_savepoint_runtime.hpp"
#include "sblr_savepoint_coordinator.hpp"
#include "sblr_autonomous_frame_runtime.hpp"
#include "sblr_autonomous_frame_coordinator.hpp"
#include "sblr_reservation_release_runtime.hpp"
#include "sblr_reservation_release_coordinator.hpp"
#include "sblr_temporary_instance_cleanup_runtime.hpp"
#include "sblr_temporary_instance_cleanup_coordinator.hpp"
#include "sblr_cursor_open_runtime.hpp"
#include "sblr_cursor_open_coordinator.hpp"
#include "sblr_cursor_fetch_runtime.hpp"
#include "sblr_cursor_close_runtime.hpp"
#include "sblr_read_by_key_runtime.hpp"
#include "sblr_read_by_key_coordinator.hpp"
#include "sblr_read_range_runtime.hpp"
#include "sblr_read_range_coordinator.hpp"
#include "sblr_read_stream_runtime.hpp"
#include "sblr_read_stream_coordinator.hpp"
#include "sblr_result_set_pass_runtime.hpp"
#include "sblr_result_set_pass_coordinator.hpp"
#include "sblr_access_cursor_open_runtime.hpp"
#include "sblr_access_cursor_open_coordinator.hpp"
#include "sblr_insert_runtime.hpp"
#include "sblr_insert_coordinator.hpp"
#include "sblr_update_runtime.hpp"
#include "sblr_update_coordinator.hpp"
#include "sblr_delete_runtime.hpp"
#include "sblr_delete_coordinator.hpp"
#include "sblr_merge_runtime.hpp"
#include "sblr_merge_coordinator.hpp"
#include "sblr_table_truncate_runtime.hpp"
#include "sblr_table_truncate_coordinator.hpp"
#include "sblr_table_analyze_runtime.hpp"
#include "sblr_table_analyze_coordinator.hpp"
#include "sblr_bulk_import_stream_runtime.hpp"
#include "sblr_bulk_import_stream_coordinator.hpp"
#include "sblr_bulk_export_stream_runtime.hpp"
#include "sblr_bulk_export_stream_coordinator.hpp"
#include "sblr_statement_batch_runtime.hpp"
#include "sblr_statement_batch_coordinator.hpp"
#include "sblr_atomic_cas_runtime.hpp"
#include "sblr_atomic_cas_coordinator.hpp"
#include "sblr_atomic_read_modify_write_runtime.hpp"
#include "sblr_atomic_rmw_coordinator.hpp"
#include "sblr_advisory_lock_runtime.hpp"
#include "sblr_advisory_lock_coordinator.hpp"
#include "sblr_advisory_lock_release_runtime.hpp"
#include "sblr_advisory_lock_release_coordinator.hpp"
#include "sblr_function_call_runtime.hpp"
#include "sblr_function_call_coordinator.hpp"
#include "sblr_operator_call_runtime.hpp"
#include "sblr_operator_call_coordinator.hpp"
#include "sblr_cast_runtime.hpp"
#include "sblr_cast_coordinator.hpp"
#include "sblr_compare_runtime.hpp"
#include "sblr_compare_coordinator.hpp"
#include "sblr_domain_operation_runtime.hpp"
#include "sblr_domain_operation_coordinator.hpp"
#include "sblr_udr_invoke_runtime.hpp"
#include "sblr_udr_invoke_coordinator.hpp"
#include "sblr_procedure_invoke_runtime.hpp"
#include "sblr_procedure_invoke_coordinator.hpp"
#include "sblr_function_invoke_runtime.hpp"
#include "sblr_function_invoke_coordinator.hpp"
#include "sblr_aggregate_invoke_runtime.hpp"
#include "sblr_aggregate_invoke_coordinator.hpp"
#include "sblr_sequence_nextval_runtime.hpp"
#include "sblr_sequence_nextval_coordinator.hpp"
#include "sblr_sequence_currval_runtime.hpp"
#include "sblr_sequence_currval_coordinator.hpp"
#include "sblr_sequence_setval_runtime.hpp"
#include "sblr_sequence_setval_coordinator.hpp"
#include "sblr_query_numeric_runtime.hpp"
#include "sblr_query_numeric_coordinator.hpp"
#include "sblr_advanced_datatype_family_runtime.hpp"
#include "sblr_advanced_datatype_family_coordinator.hpp"
#include "sblr_project_runtime.hpp"
#include "sblr_project_coordinator.hpp"
#include "sblr_aggregate_runtime.hpp"
#include "sblr_aggregate_coordinator.hpp"
#include "sblr_group_runtime.hpp"
#include "sblr_group_coordinator.hpp"
#include "sblr_sort_runtime.hpp"
#include "sblr_limit_runtime.hpp"
#include "sblr_catalog_introspect_runtime.hpp"
#include "sblr_catalog_introspect_coordinator.hpp"
#include "sblr_window_runtime.hpp"
#include "sblr_return_result_set_runtime.hpp"
#include "sblr_return_result_set_coordinator.hpp"
#include "sblr_kv_structured_read_runtime.hpp"
#include "sblr_kv_structured_read_coordinator.hpp"
#include "sblr_kv_structured_mutate_runtime.hpp"
#include "sblr_kv_structured_scan_runtime.hpp"
#include "sblr_kv_structured_stream_read_runtime.hpp"
#include "sblr_kv_structured_stream_read_coordinator.hpp"
#include "sblr_kv_structured_stream_append_runtime.hpp"
#include "sblr_kv_structured_stream_append_coordinator.hpp"
#include "sblr_kv_structured_timeseries_runtime.hpp"
#include "sblr_kv_structured_timeseries_coordinator.hpp"
#include "sblr_system_config_set_runtime.hpp"
#include "sblr_system_config_set_coordinator.hpp"
#include "sblr_ddl_create_domain_runtime.hpp"
#include "sblr_ddl_alter_domain_runtime.hpp"
#include "sblr_ddl_create_view_runtime.hpp"
#include "sblr_ddl_drop_view_runtime.hpp"
#include "sblr_ddl_drop_type_runtime.hpp"
#include "sblr_ddl_create_trigger_runtime.hpp"
#include "sblr_ddl_alter_trigger_runtime.hpp"
#include "sblr_ddl_drop_trigger_runtime.hpp"
#include "sblr_ddl_create_procedure_runtime.hpp"
#include "sblr_ddl_alter_view_runtime.hpp"
#include "sblr_ddl_create_schema_runtime.hpp"
#include "sblr_ddl_create_table_runtime.hpp"
#include "sblr_ddl_create_index_runtime.hpp"
#include "sblr_ddl_drop_index_runtime.hpp"
#include "sblr_ddl_create_domain_coordinator.hpp"
#include "sblr_ddl_alter_domain_coordinator.hpp"
#include "sblr_ddl_create_view_coordinator.hpp"
#include "sblr_ddl_drop_view_coordinator.hpp"
#include "sblr_ddl_drop_type_coordinator.hpp"
#include "sblr_ddl_create_trigger_coordinator.hpp"
#include "sblr_ddl_alter_trigger_coordinator.hpp"
#include "sblr_ddl_drop_trigger_coordinator.hpp"
#include "sblr_ddl_create_procedure_coordinator.hpp"
#include "sblr_ddl_alter_procedure_runtime.hpp"
#include "sblr_ddl_alter_procedure_coordinator.hpp"
#include "sblr_ddl_drop_procedure_runtime.hpp"
#include "sblr_ddl_drop_procedure_coordinator.hpp"
#include "sblr_ddl_create_function_runtime.hpp"
#include "sblr_ddl_create_function_coordinator.hpp"
#include "sblr_ddl_alter_function_runtime.hpp"
#include "sblr_ddl_alter_function_coordinator.hpp"
#include "sblr_ddl_drop_function_runtime.hpp"
#include "sblr_ddl_drop_function_coordinator.hpp"
#include "sblr_ddl_create_package_runtime.hpp"
#include "sblr_ddl_create_synonym_runtime.hpp"
#include "sblr_ddl_create_foreign_table_runtime.hpp"
#include "engine/internal_api/sblr_ddl_create_foreign_table_coordinator.hpp"
#include "sblr_ddl_create_fdw_runtime.hpp"
#include "engine/internal_api/sblr_ddl_create_fdw_coordinator.hpp"
#include "sblr_ddl_drop_fdw_runtime.hpp"
#include "engine/internal_api/sblr_ddl_drop_fdw_coordinator.hpp"
#include "sblr_ddl_alter_package_runtime.hpp"
#include "sblr_ddl_create_package_coordinator.hpp"
#include "sblr_ddl_create_synonym_coordinator.hpp"
#include "sblr_ddl_drop_synonym_runtime.hpp"
#include "sblr_ddl_drop_synonym_coordinator.hpp"
#include "sblr_ddl_create_sequence_runtime.hpp"
#include "sblr_ddl_create_sequence_coordinator.hpp"
#include "sblr_ddl_alter_sequence_runtime.hpp"
#include "sblr_ddl_alter_sequence_coordinator.hpp"
#include "sblr_ddl_drop_sequence_runtime.hpp"
#include "sblr_ddl_drop_sequence_coordinator.hpp"
#include "sblr_ddl_create_materialized_view_runtime.hpp"
#include "sblr_ddl_create_materialized_view_coordinator.hpp"
#include "sblr_ddl_create_table_as_query_runtime.hpp"
#include "sblr_ddl_create_table_as_query_coordinator.hpp"
#include "sblr_ddl_alter_package_coordinator.hpp"
#include "sblr_ddl_create_temporary_table_runtime.hpp"
#include "sblr_ddl_create_temporary_table_coordinator.hpp"
#include "sblr_ddl_drop_temporary_table_runtime.hpp"
#include "sblr_ddl_drop_temporary_table_coordinator.hpp"
#include "sblr_ddl_rename_object_vector_runtime.hpp"
#include "sblr_ddl_rename_object_runtime.hpp"
#include "sblr_ddl_create_or_replace_srs_runtime.hpp"
#include "sblr_ddl_create_or_replace_srs_coordinator.hpp"
#include "sblr_ddl_drop_srs_runtime.hpp"
#include "sblr_ddl_drop_srs_coordinator.hpp"
#include "sblr_ddl_create_rewrite_rule_runtime.hpp"
#include "sblr_ddl_create_rewrite_rule_coordinator.hpp"
#include "sblr_ddl_alter_rewrite_rule_runtime.hpp"
#include "sblr_ddl_alter_rewrite_rule_coordinator.hpp"
#include "sblr_ddl_drop_rewrite_rule_runtime.hpp"
#include "sblr_ddl_drop_rewrite_rule_coordinator.hpp"
#include "sblr_ddl_validate_constraint_runtime.hpp"
#include "sblr_ddl_validate_constraint_coordinator.hpp"
#include "sblr_security_create_privilege_template_runtime.hpp"
#include "sblr_security_create_user_runtime.hpp"
#include "sblr_sec_alter_user_runtime.hpp"
#include "sblr_sec_create_role_runtime.hpp"
#include "sblr_sec_drop_role_runtime.hpp"
#include "sblr_sec_create_policy_runtime.hpp"
#include "sblr_sec_drop_policy_runtime.hpp"
#include "sblr_sec_alter_role_runtime.hpp"
#include "sblr_sec_create_group_mapping_runtime.hpp"
#include "sblr_sec_drop_group_mapping_runtime.hpp"
#include "sblr_sec_grant_runtime.hpp"
#include "sblr_sec_revoke_runtime.hpp"
#include "sblr_sec_alter_policy_runtime.hpp"
#include "sblr_sec_drop_user_runtime.hpp"
#include "sblr_security_alter_privilege_template_runtime.hpp"
#include "sblr_security_alter_privilege_template_coordinator.hpp"
#include "sblr_security_drop_privilege_template_runtime.hpp"
#include "sblr_security_drop_privilege_template_coordinator.hpp"
#include "sblr_database_create_template_clone_runtime.hpp"
#include "sblr_database_create_template_clone_coordinator.hpp"
#include "sblr_ddl_create_aggregate_runtime.hpp"
#include "sblr_ddl_create_aggregate_coordinator.hpp"
#include "sblr_ddl_create_macro_coordinator.hpp"
#include "sblr_ddl_create_macro_runtime.hpp"
#include "sblr_ddl_drop_macro_coordinator.hpp"
#include "sblr_ddl_drop_macro_runtime.hpp"
#include "sblr_admin_register_external_relation_resolver_runtime.hpp"
#include "sblr_admin_unregister_external_relation_resolver_runtime.hpp"
#include "sblr_ddl_create_dictionary_runtime.hpp"
#include "sblr_ddl_create_dictionary_coordinator.hpp"
#include "sblr_ddl_alter_aggregate_runtime.hpp"
#include "sblr_ddl_alter_aggregate_coordinator.hpp"
#include "sblr_ddl_drop_aggregate_runtime.hpp"
#include "sblr_ddl_drop_aggregate_coordinator.hpp"
#include "sblr_ddl_drop_dictionary_coordinator.hpp"
#include "sblr_ddl_drop_dictionary_runtime.hpp"
#include "sblr_ddl_alter_dictionary_runtime.hpp"
#include "sblr_ddl_alter_dictionary_coordinator.hpp"
#include "sblr_ddl_create_continuous_view_coordinator.hpp"
#include "sblr_ddl_create_continuous_view_runtime.hpp"
#include "sblr_ddl_alter_continuous_view_coordinator.hpp"
#include "sblr_ddl_alter_continuous_view_runtime.hpp"
#include "sblr_ddl_drop_continuous_view_coordinator.hpp"
#include "sblr_ddl_drop_continuous_view_runtime.hpp"
#include "sblr_dml_async_insert_submit_runtime.hpp"
#include "sblr_dml_async_insert_status_runtime.hpp"
#include "sblr_dml_counter_add_runtime.hpp"
#include "sblr_dml_counter_add_coordinator.hpp"
#include "sblr_dml_timeseries_schema_write_runtime.hpp"
#include "sblr_dml_timeseries_schema_write_coordinator.hpp"
#include "sblr_ddl_timeseries_series_cardinality_policy_runtime.hpp"
#include "sblr_ddl_timeseries_series_cardinality_policy_coordinator.hpp"
#include "sblr_ddl_create_timeseries_value_cache_runtime.hpp"
#include "sblr_ddl_create_timeseries_value_cache_coordinator.hpp"
#include "sblr_dml_async_insert_cancel_runtime.hpp"
#include "sblr_dml_async_insert_submit_coordinator.hpp"
#include "sblr_dml_async_insert_status_coordinator.hpp"
#include "sblr_dml_async_insert_cancel_coordinator.hpp"
#include "sblr_ddl_purge_system_history_runtime.hpp"
#include "sblr_ddl_purge_system_history_coordinator.hpp"
#include "sblr_ddl_set_index_optimizer_eligibility_runtime.hpp"
#include "sblr_ddl_set_index_optimizer_eligibility_coordinator.hpp"
#include "sblr_ddl_set_table_type_enforcement_runtime.hpp"
#include "sblr_ddl_set_table_type_enforcement_coordinator.hpp"
#include "sblr_database_serialize_logical_snapshot_runtime.hpp"
#include "sblr_database_serialize_logical_snapshot_coordinator.hpp"
#include "sblr_database_deserialize_logical_snapshot_runtime.hpp"
#include "sblr_database_deserialize_logical_snapshot_coordinator.hpp"
#include "sblr_security_create_privilege_template_coordinator.hpp"
#include "sblr_security_create_user_coordinator.hpp"
#include "sblr_sec_alter_user_coordinator.hpp"
#include "sblr_sec_create_role_coordinator.hpp"
#include "sblr_sec_drop_role_coordinator.hpp"
#include "sblr_sec_create_policy_coordinator.hpp"
#include "sblr_sec_drop_policy_coordinator.hpp"
#include "sblr_sec_alter_role_coordinator.hpp"
#include "sblr_sec_create_group_mapping_coordinator.hpp"
#include "sblr_sec_drop_group_mapping_coordinator.hpp"
#include "sblr_sec_grant_coordinator.hpp"
#include "sblr_sec_revoke_coordinator.hpp"
#include "sblr_sec_alter_policy_coordinator.hpp"
#include "sblr_sec_drop_user_coordinator.hpp"
#include "sblr_ddl_rename_object_vector_coordinator.hpp"
#include "sblr_ddl_rename_object_coordinator.hpp"
#include "sblr_ddl_alter_view_coordinator.hpp"
#include "sblr_ddl_create_schema_coordinator.hpp"
#include "sblr_ddl_create_table_coordinator.hpp"
#include "sblr_ddl_create_index_coordinator.hpp"
#include "sblr_ddl_drop_index_coordinator.hpp"
#include "sblr_kv_structured_scan_coordinator.hpp"
#include "sblr_kv_structured_mutate_coordinator.hpp"
#include "sblr_window_coordinator.hpp"
#include "sblr_sort_coordinator.hpp"
#include "sblr_limit_coordinator.hpp"
#include "sblr_prepared_coordination_registry.hpp"
#include "sblr_variable_frame_coordinator.hpp"
#include "sblr_variable_runtime.hpp"
#include "sblr_opcode_registry.hpp"
#include "sblr_opcode_stream.hpp"
#include "server_engine_bridge/diagnostic_fields.hpp"
#include "server_engine_bridge/prepared_metadata_binding.hpp"
#include "server_engine_bridge/statement_context.hpp"
#include "transaction/transaction_api.hpp"
#include "transaction_inventory.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

constexpr std::uint64_t kEngineMagic = 0x5342454e47494e45ull;
constexpr std::uint64_t kSessionMagic = 0x534245534553534eull;
constexpr std::uint64_t kTransactionMagic = 0x53425452414e5343ull;
constexpr std::uint64_t kResultMagic = 0x534245524553554cull;
constexpr std::uint64_t kPreparedMetadataBindingMagic =
    0x5342504d45544131ull;
constexpr std::uint64_t kStatementContextReceiptMagic =
    0x5342535443545831ull;

constexpr const char* kBuildId = "scratchbird-engine-abi-v1";

struct DiagnosticFieldStorage {
  std::string key;
  std::string value;
};

struct DiagnosticStorage {
  sb_engine_diagnostic_view_t view{};
  std::string code;
  std::string message;
  std::string detail;
  std::vector<DiagnosticFieldStorage> fields;
};

struct sb_engine_result_s {
  std::uint64_t magic = kResultMagic;
  mutable std::mutex mutex;
  bool released = false;
  sb_engine_result_class_t result_class = SB_ENGINE_RESULT_NONE;
  std::string operation_id;
  std::vector<DiagnosticStorage> diagnostics;
  std::vector<sb_engine_diagnostic_view_t> diagnostic_views;
  std::string payload;
  std::string result_kind;
  std::vector<std::string> row_values;
  std::vector<std::string> row_metadata_values;
  std::vector<std::string> evidence_values;
  scratchbird::engine::sblr::QueryExecuteResultHandleV1
      query_execute_result_handle;
  bool query_execute_result_handle_validated = false;
  bool admitted_query_row_stream_renderer = false;
  std::uint64_t next_row_index = 0;
  std::uint64_t affected_rows = 0;
  std::uint64_t rows_produced = 0;
};

struct sb_engine_handle_s {
  std::uint64_t magic = kEngineMagic;
  mutable std::mutex mutex;
  bool closed = false;
  std::string database_path;
  std::string database_uuid;
  std::uint64_t database_page_size_bytes = 0;
  std::atomic<std::uint64_t> next_session_id{1};
};

struct sb_engine_session_s {
  std::uint64_t magic = kSessionMagic;
  mutable std::mutex mutex;
  sb_engine_handle_t engine = nullptr;
  bool closed = false;
  std::uint64_t session_id = 0;
  sb_engine_uuid_t effective_user_uuid{};
  sb_engine_uuid_t public_session_uuid{};
  sb_engine_trust_mode_t trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  std::uint32_t active_transactions = 0;
  std::vector<sb_engine_transaction_t> published_transactions;
  std::uint32_t open_streams = 0;
  std::unique_ptr<scratchbird::core::agents::
                      ResourceGovernanceReservationLedger>
      package_resource_ledger;
  scratchbird::core::agents::ResourceGovernanceQuotaDescriptor
      package_resource_descriptor;
  bool package_resource_descriptor_initialized = false;
};

struct sb_engine_transaction_s {
  std::uint64_t magic = kTransactionMagic;
  mutable std::mutex mutex;
  sb_engine_session_t session = nullptr;
  std::array<std::uint8_t, 16> transaction_uuid{};
  std::uint64_t local_transaction_id = 0;
  std::array<std::uint8_t, 32> handle_evidence_sha256{};
  bool closed = false;
};

namespace scratchbird::server_engine_bridge {

struct PreparedMetadataBindingOpaque {
  std::uint64_t magic = kPreparedMetadataBindingMagic;
  mutable std::mutex mutex;
  bool released = false;
  bool invalidated = false;
  std::string invalidation_detail;
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  std::string database_path;
  std::string database_uuid;
  sb_engine_uuid_t effective_user_uuid{};
  sb_engine_uuid_t session_uuid{};
  sb_engine_uuid_t parser_package_uuid{};
  sb_engine_uuid_t dialect_profile_uuid{};
  sb_engine_trust_mode_t trust_mode = SB_ENGINE_TRUST_SERVER_ISOLATED;
  std::uint32_t context_flags = 0;
  std::uint64_t rights_set_ref = 0;
  std::uint64_t capability_set_ref = 0;
  std::uint64_t source_artifact_set_ref = 0;
  std::vector<std::uint8_t> encoded_sblr_envelope;
  std::string metadata_snapshot_uuid;
  std::uint64_t metadata_visible_through_local_transaction_id = 0;
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  std::string target_object_uuid;
  std::uint64_t target_executable_generation = 0;
  std::uint64_t target_metadata_epoch = 0;
  std::uint64_t target_creator_local_transaction_id = 0;
};

struct StatementContextReceiptOpaque {
  std::uint64_t magic = kStatementContextReceiptMagic;
  mutable std::mutex mutex;
  bool released = false;
  sb_engine_handle_t engine = nullptr;
  sb_engine_session_t session = nullptr;
  StatementContextReceiptView view;
  scratchbird::transaction::mga::SnapshotVectorDescriptor snapshot_vector;
  scratchbird::engine::internal_api::EngineRequestContext engine_context;
  bool literal_prebind_negotiated = false;
  bool literal_binding_finalized = false;
  bool literal_admission_consumed = false;
  std::array<std::uint8_t,32> literal_demand_sha256{};
  std::array<std::uint8_t,32> literal_ordered_profile_sha256{};
  std::vector<std::uint8_t> literal_canonical_sblq;
  std::vector<scratchbird::engine::sblr::SblrLiteralDemandV1> literal_demands;
  std::array<std::uint8_t,32> literal_bound_ast_sha256{};
  std::array<std::uint8_t,32> literal_sbxn_sha256{};
  std::string literal_final_receipt_uuid;
  std::string literal_admission_token_uuid;
  std::array<std::uint8_t,32> literal_admission_token_binding_sha256{};
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      literal_executor_availability_snapshot;
  bool parameter_prebind_negotiated = false;
  bool parameter_binding_finalized = false;
  bool parameter_admission_consumed = false;
  std::array<std::uint8_t, 32> parameter_demand_sha256{};
  std::array<std::uint8_t, 32> parameter_mapping_sha256{};
  std::array<std::uint8_t, 32> parameter_sbpn_sha256{};
  std::vector<scratchbird::engine::sblr::SblrParameterDemandV1>
      parameter_demands;
  scratchbird::engine::internal_api::SblrParameterSetSnapshot
      parameter_set_snapshot;
  std::string parameter_coordination_operation_uuid;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      parameter_executor_availability_snapshot;
  std::string parameter_final_receipt_uuid;
  std::string parameter_admission_token_uuid;
  std::array<std::uint8_t, 32> parameter_admission_binding_sha256{};
  bool variable_binding_finalized = false;
  bool variable_admission_consumed = false;
  std::optional<scratchbird::engine::internal_api::SblrVariableFrameSnapshot>
      variable_frame_snapshot;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      variable_executor_availability_snapshot;
  std::string variable_final_receipt_uuid;
  std::string variable_admission_token_uuid;
  std::array<std::uint8_t, 32> variable_admission_binding_sha256{};
  bool source_map_bound_ast_frozen = false;
  std::array<std::uint8_t, 32> source_map_bound_ast_sha256{};
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      source_map_executor_availability_snapshot;
  std::vector<scratchbird::engine::internal_api::SblrSourceMapDescriptorSnapshotV1>
      source_map_descriptors;
  std::vector<scratchbird::engine::internal_api::SblrErrorVectorDescriptorSnapshotV1>
      error_vector_descriptors;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      error_vector_executor_availability_snapshot;
};

scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
VariableExecutorAvailabilityIdentity() {
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;
  id.executor_id = scratchbird::engine::internal_api::kSblrVariableExecutorId;
  id.opcode_code = scratchbird::engine::internal_api::kSblrVariableOpcodeCode;
  id.opcode_version = scratchbird::engine::internal_api::kSblrVariableOpcodeVersion;
  id.operand_descriptor_id =
      scratchbird::engine::internal_api::kSblrVariableOperandDescriptorId;
  id.result_descriptor_id =
      scratchbird::engine::internal_api::kSblrVariableResultDescriptorId;
  id.result_descriptor_version =
      scratchbird::engine::internal_api::kSblrVariableResultDescriptorVersion;
  return id;
}

struct StatementPackageAdmissionReservationOpaque {
  std::uint64_t receipt_id = 0;
  sb_engine_session_t session = nullptr;
  StatementSblrPayloadKind payload_kind = StatementSblrPayloadKind::kInvalid;
  std::uint64_t payload_size = 0;
  std::uint32_t record_count = 0;
  std::uint64_t resource_policy_generation = 0;
  std::array<std::uint8_t, 32> payload_sha256{};
  std::string ledger_token_id;
};

struct StatementParameterCoordinationOpaque {
  std::uint64_t private_handle = 0;
  sb_engine_session_t session = nullptr;
  std::string database_uuid;
  std::string session_uuid;
  std::string transaction_uuid;
  StatementParameterExecutionMode mode = StatementParameterExecutionMode::kDirect;
  std::string public_coordination_uuid;
  std::string operation_uuid;
  std::uint64_t generation = 0;
  std::string prepared_statement_uuid;
  std::uint64_t prepared_generation = 0;
  bool context_acquired = false;
  bool sealed = false;
  bool terminal = false;
};

}  // namespace scratchbird::server_engine_bridge

namespace {

std::array<std::uint8_t, 16> TextToUuid(const std::string& text) {
  std::array<std::uint8_t, 16> bytes{};
  const auto parsed = scratchbird::core::uuid::ParseUuid(text);
  if (parsed.ok()) {
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), bytes.begin());
  }
  return bytes;
}

using scratchbird::server_engine_bridge::PreparedMetadataBindingHandle;
using scratchbird::server_engine_bridge::PreparedMetadataBindingDispatchTestHook;
using scratchbird::server_engine_bridge::StatementContextReceiptHandle;

std::mutex g_prepared_metadata_binding_registry_mutex;
std::unordered_set<PreparedMetadataBindingHandle>
    g_prepared_metadata_bindings;
std::atomic<std::uint64_t> g_prepared_metadata_snapshot_ordinal{1};
std::mutex g_prepared_metadata_dispatch_test_hook_mutex;
PreparedMetadataBindingDispatchTestHook
    g_prepared_metadata_dispatch_test_hook = nullptr;
void* g_prepared_metadata_dispatch_test_hook_context = nullptr;

std::mutex g_statement_context_receipt_registry_mutex;
std::map<std::uint64_t, std::unique_ptr<
    scratchbird::server_engine_bridge::StatementContextReceiptOpaque>>
    g_live_statement_context_receipts;
std::atomic<std::uint64_t> g_next_statement_context_receipt_id{1};
std::atomic<std::uint64_t> g_statement_context_identity_ordinal{1};
std::map<std::uint64_t, scratchbird::server_engine_bridge::
    StatementPackageAdmissionReservationOpaque>
    g_package_admission_reservations;
std::atomic<std::uint64_t> g_next_package_admission_reservation_id{1};
std::map<std::uint64_t, scratchbird::server_engine_bridge::
    StatementParameterCoordinationOpaque> g_parameter_coordinations;
std::atomic<std::uint64_t> g_next_parameter_coordination_handle{1};

void invoke_prepared_metadata_dispatch_test_hook(std::string_view phase) {
  PreparedMetadataBindingDispatchTestHook hook = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> guard(
        g_prepared_metadata_dispatch_test_hook_mutex);
    hook = g_prepared_metadata_dispatch_test_hook;
    context = g_prepared_metadata_dispatch_test_hook_context;
  }
  if (hook != nullptr) { hook(phase, context); }
}

bool valid_abi(std::uint32_t abi_version) {
  return abi_version == SB_ENGINE_ABI_VERSION_PACKED;
}

bool valid_string_span(const char* data, std::uint64_t size) {
  return size == 0 || data != nullptr;
}

bool nonzero_uuid(const sb_engine_uuid_t& uuid) {
  return std::any_of(std::begin(uuid.bytes), std::end(uuid.bytes), [](std::uint8_t v) { return v != 0; });
}

std::string current_utc_timestamp_text() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
#if defined(_WIN32)
  gmtime_s(&tm, &time);
#else
  gmtime_r(&time, &tm);
#endif
  char text[sizeof("YYYY-MM-DDTHH:MM:SSZ")] = {};
  if (std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) {
    return {};
  }
  return text;
}

bool canonical_statement_timestamp(std::string_view value) {
  if (value.size() != 20 &&
      (value.size() < 22 || value.size() > 30)) {
    return false;
  }
  if (value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
      value[13] != ':' || value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  constexpr std::size_t kDigitIndexes[] = {
      0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18};
  for (const auto index : kDigitIndexes) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  if (value.size() > 20) {
    if (value[19] != '.') return false;
    for (std::size_t index = 20; index + 1 < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') return false;
    }
  }
  const auto decimal = [&](std::size_t offset, std::size_t digits) {
    unsigned result = 0;
    for (std::size_t index = 0; index < digits; ++index) {
      result = result * 10 +
               static_cast<unsigned>(value[offset + index] - '0');
    }
    return result;
  };
  const auto year = decimal(0, 4);
  const auto month = decimal(5, 2);
  const auto day = decimal(8, 2);
  const auto hour = decimal(11, 2);
  const auto minute = decimal(14, 2);
  const auto second = decimal(17, 2);
  if (year == 0 || month == 0 || month > 12 || hour > 23 || minute > 59 ||
      second > 59) {
    return false;
  }
  constexpr unsigned kDaysByMonth[] = {
      0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  auto maximum_day = kDaysByMonth[month];
  const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
  if (month == 2 && leap) ++maximum_day;
  return day != 0 && day <= maximum_day;
}

std::string current_monotonic_ns_text() {
  return std::to_string(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
}

bool statement_context_transaction_active(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::active ||
         state == TransactionState::read_only_active;
}

bool generate_distinct_statement_context_uuid(
    std::unordered_set<std::string>* identities,
    std::string* generated_uuid) {
  if (identities == nullptr || generated_uuid == nullptr) return false;
  constexpr std::uint64_t kMaximumAttempts = 32;
  const auto now_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  for (std::uint64_t attempt = 0; attempt < kMaximumAttempts; ++attempt) {
    const auto ordinal = g_statement_context_identity_ordinal.fetch_add(
        1, std::memory_order_relaxed);
    const auto generated =
        scratchbird::core::uuid::GenerateDurableEngineIdentityV7(
            scratchbird::core::platform::UuidKind::object,
            now_millis + ordinal + attempt);
    if (!generated.ok()) return false;
    std::string candidate =
        scratchbird::core::uuid::UuidToString(generated.value.value);
    if (identities->insert(candidate).second) {
      *generated_uuid = std::move(candidate);
      return true;
    }
  }
  return false;
}

bool canonical_non_nil_uuid_text(std::string_view value) {
  if (value.empty() ||
      value == "00000000-0000-0000-0000-000000000000") {
    return false;
  }
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(value));
  return parsed.ok() && !scratchbird::core::uuid::IsNilUuid(parsed.value) &&
         scratchbird::core::uuid::UuidToString(parsed.value) == value;
}

bool valid_engine(sb_engine_handle_t handle) {
  return handle != nullptr && handle->magic == kEngineMagic && !handle->closed;
}

bool valid_session(sb_engine_session_t handle) {
  return handle != nullptr && handle->magic == kSessionMagic && !handle->closed && valid_engine(handle->engine);
}

bool valid_transaction(sb_engine_transaction_t handle) {
  return handle != nullptr && handle->magic == kTransactionMagic && !handle->closed && valid_session(handle->session);
}

bool valid_result(sb_engine_result_t handle) {
  return handle != nullptr && handle->magic == kResultMagic && !handle->released;
}

using EngineAbiSteadyClock = std::chrono::steady_clock;

std::uint64_t EngineAbiElapsedMicros(EngineAbiSteadyClock::time_point start,
                                     EngineAbiSteadyClock::time_point finish) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(finish - start)
          .count());
}

void WriteEngineAbiPhaseTrace(
    std::string_view layer,
    std::string_view operation_id,
    std::size_t envelope_size,
    const std::vector<std::pair<std::string, std::uint64_t>>& phase_micros) {
  const char* trace_path = std::getenv("SCRATCHBIRD_ENGINE_ABI_PHASE_TRACE_FILE");
  if (trace_path == nullptr || *trace_path == '\0') {
    return;
  }
  std::ofstream out(trace_path, std::ios::app | std::ios::binary);
  if (!out) {
    return;
  }
  out << "layer=" << layer
      << "\toperation=" << operation_id
      << "\tenvelope_bytes=" << envelope_size;
  std::uint64_t total = 0;
  for (const auto& [phase, micros] : phase_micros) {
    total += micros;
    out << '\t' << phase << "_us=" << micros;
  }
  out << "\ttotal_us=" << total << "\tparent_success_barrier=passed\n";
}

std::string uuid_to_canonical(const sb_engine_uuid_t& uuid) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(36);
  for (std::size_t i = 0; i < sizeof(uuid.bytes); ++i) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      out.push_back('-');
    }
    out.push_back(kHex[(uuid.bytes[i] >> 4u) & 0x0fu]);
    out.push_back(kHex[uuid.bytes[i] & 0x0fu]);
  }
  return out;
}

bool same_uuid(const sb_engine_uuid_t& left, const sb_engine_uuid_t& right) {
  return std::memcmp(left.bytes, right.bytes, sizeof(left.bytes)) == 0;
}

bool active_metadata_snapshot_exclusion(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::created ||
         state == TransactionState::active ||
         state == TransactionState::read_only_active ||
         state == TransactionState::preparing ||
         state == TransactionState::rolling_back;
}

bool in_doubt_metadata_snapshot_exclusion(
    scratchbird::transaction::mga::TransactionState state) {
  using scratchbird::transaction::mga::TransactionState;
  return state == TransactionState::prepared ||
         state == TransactionState::committing ||
         state == TransactionState::limbo ||
         state == TransactionState::recovering ||
         state == TransactionState::failed_terminal;
}

std::string new_prepared_metadata_snapshot_uuid() {
  const auto now_millis = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  const auto ordinal =
      g_prepared_metadata_snapshot_ordinal.fetch_add(1,
                                                     std::memory_order_relaxed);
  const auto generated =
      scratchbird::core::uuid::GenerateEngineIdentityV7(
          scratchbird::core::platform::UuidKind::object,
          now_millis + ordinal);
  return generated.ok()
             ? scratchbird::core::uuid::UuidToString(generated.value.value)
             : std::string{};
}

std::string operation_operand_value(
    const scratchbird::engine::sblr::SblrOperationEnvelope& envelope,
    std::string_view name) {
  for (const auto& operand : envelope.operands) {
    if (operand.name == name) return operand.value;
  }
  return {};
}

bool has_engine_only_prepared_metadata_operand(
    const scratchbird::engine::sblr::SblrOperationEnvelope& envelope) {
  for (const auto& operand : envelope.operands) {
    if (operand.name.starts_with("engine.prepared_metadata.")) return true;
  }
  return false;
}

struct PreparedMetadataBindingSnapshot {
  std::string metadata_snapshot_uuid;
  std::uint64_t metadata_visible_through_local_transaction_id = 0;
  std::vector<std::uint64_t> active_excluded_local_transaction_ids;
  std::vector<std::uint64_t> in_doubt_excluded_local_transaction_ids;
  std::string target_object_uuid;
  std::uint64_t target_executable_generation = 0;
  std::uint64_t target_metadata_epoch = 0;
};

bool copy_prepared_metadata_binding_for_dispatch(
    PreparedMetadataBindingHandle binding,
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t& context,
    const sb_engine_sblr_dispatch_params_v1_t& params,
    PreparedMetadataBindingSnapshot* snapshot,
    std::string* detail) {
  if (snapshot == nullptr || detail == nullptr || binding == nullptr) {
    if (detail != nullptr) *detail = "binding_or_output_missing";
    return false;
  }
  std::lock_guard<std::mutex> registry_guard(
      g_prepared_metadata_binding_registry_mutex);
  if (g_prepared_metadata_bindings.count(binding) == 0) {
    *detail = "binding_not_live";
    return false;
  }
  std::lock_guard<std::mutex> binding_guard(binding->mutex);
  if (binding->released || binding->magic != kPreparedMetadataBindingMagic) {
    *detail = "binding_released";
    return false;
  }
  if (binding->invalidated) {
    *detail = "binding_invalidated:" + binding->invalidation_detail;
    return false;
  }
  if (binding->session != session || binding->engine != session->engine) {
    *detail = "binding_session_or_engine_mismatch";
    return false;
  }
  if (binding->database_path != session->engine->database_path ||
      binding->database_uuid != session->engine->database_uuid) {
    *detail = "binding_database_mismatch";
    return false;
  }
  if (!same_uuid(binding->effective_user_uuid, context.effective_user_uuid) ||
      !same_uuid(binding->session_uuid, context.session_uuid) ||
      !same_uuid(binding->parser_package_uuid, context.parser_package_uuid) ||
      !same_uuid(binding->dialect_profile_uuid, context.dialect_profile_uuid) ||
      binding->trust_mode != context.trust_mode ||
      binding->context_flags != context.flags ||
      binding->rights_set_ref != context.rights_set_ref ||
      binding->capability_set_ref != context.capability_set_ref ||
      binding->source_artifact_set_ref != context.source_artifact_set_ref) {
    *detail = "binding_security_context_mismatch";
    return false;
  }
  if (params.envelope_size_bytes != binding->encoded_sblr_envelope.size() ||
      params.envelope_bytes == nullptr ||
      !std::equal(binding->encoded_sblr_envelope.begin(),
                  binding->encoded_sblr_envelope.end(),
                  params.envelope_bytes)) {
    *detail = "binding_sblr_envelope_mismatch";
    return false;
  }
  snapshot->metadata_snapshot_uuid = binding->metadata_snapshot_uuid;
  snapshot->metadata_visible_through_local_transaction_id =
      binding->metadata_visible_through_local_transaction_id;
  snapshot->active_excluded_local_transaction_ids =
      binding->active_excluded_local_transaction_ids;
  snapshot->in_doubt_excluded_local_transaction_ids =
      binding->in_doubt_excluded_local_transaction_ids;
  snapshot->target_object_uuid = binding->target_object_uuid;
  snapshot->target_executable_generation =
      binding->target_executable_generation;
  snapshot->target_metadata_epoch = binding->target_metadata_epoch;
  return true;
}

void release_prepared_metadata_bindings_for_session(
    sb_engine_session_t session) {
  std::vector<PreparedMetadataBindingHandle> released;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_prepared_metadata_binding_registry_mutex);
    for (auto it = g_prepared_metadata_bindings.begin();
         it != g_prepared_metadata_bindings.end();) {
      auto* binding = *it;
      if (binding->session != session) {
        ++it;
        continue;
      }
      {
        std::lock_guard<std::mutex> binding_guard(binding->mutex);
        binding->released = true;
        binding->magic = 0;
      }
      released.push_back(binding);
      it = g_prepared_metadata_bindings.erase(it);
    }
  }
  for (auto* binding : released) delete binding;
}

void release_statement_context_receipts_for_session(
    sb_engine_session_t session) {
  std::vector<scratchbird::core::platform::TypedUuid> published_snapshots;
  std::vector<std::unique_ptr<
      scratchbird::server_engine_bridge::StatementContextReceiptOpaque>>
      released;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    for (auto it = g_package_admission_reservations.begin();
         it != g_package_admission_reservations.end();) {
      if (it->second.session != session) {
        ++it;
        continue;
      }
      if (session->package_resource_ledger != nullptr &&
          !it->second.ledger_token_id.empty()) {
        (void)session->package_resource_ledger->Release(
            it->second.ledger_token_id,
            scratchbird::core::agents::
                ResourceGovernanceReservationReleaseReason::kShutdown);
      }
      it = g_package_admission_reservations.erase(it);
    }
    for (auto it = g_live_statement_context_receipts.begin();
         it != g_live_statement_context_receipts.end();) {
      auto& receipt = it->second;
      if (receipt->session != session) {
        ++it;
        continue;
      }
      {
        std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
        receipt->released = true;
        receipt->magic = 0;
        published_snapshots.push_back(
            receipt->snapshot_vector.snapshot_uuid);
      }
      released.push_back(std::move(receipt));
      it = g_live_statement_context_receipts.erase(it);
    }
  }
  for (const auto& snapshot_uuid : published_snapshots) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot_uuid);
  }
}

bool looks_like_sblr_operation_envelope(const scratchbird::engine::SblrExecutionEnvelope& envelope) {
  if (envelope.payload_kind != scratchbird::engine::SblrPayloadKind::operation_envelope &&
      envelope.payload_kind != scratchbird::engine::SblrPayloadKind::opcode_stream ||
      envelope.canonical_bytes.empty()) {
    return false;
  }
  const auto* data = reinterpret_cast<const char*>(envelope.canonical_bytes.data());
  const std::string_view text(data, envelope.canonical_bytes.size());
  const auto decoded = scratchbird::engine::sblr::DecodeSblrEnvelope(text);
  if (decoded.ok && (decoded.envelope.operation_id ==
                       "engine.op.ddl_refresh_materialized_view" || decoded.envelope.operation_id == "engine.op.ddl_create_materialized_view" || decoded.envelope.operation_id == "engine.op.ddl_alter_sequence" || decoded.envelope.operation_id == "engine.op.ddl_drop_type" || decoded.envelope.operation_id == "engine.op.ddl_rename_object" || decoded.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_data" || decoded.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_no_data") &&
      (decoded.envelope.opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW" || decoded.envelope.opcode == "SBLR_DDL_CREATE_MATERIALIZED_VIEW" || decoded.envelope.opcode == "SBLR_DDL_ALTER_SEQUENCE" || decoded.envelope.opcode == "SBLR_DDL_DROP_TYPE" || decoded.envelope.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA" || decoded.envelope.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA")) {
    return true;
  }
  const auto stream = scratchbird::engine::sblr::DecodeSblrOpcodeStream(text);
  if (stream.ok && stream.stream.operations.size() >= 3) {
    const auto& operation = stream.stream.operations[1];
    if ((operation.operation_id == "engine.op.ddl_refresh_materialized_view" || operation.operation_id == "engine.op.ddl_create_materialized_view" || operation.operation_id == "engine.op.ddl_alter_sequence" || operation.operation_id == "engine.op.ddl_drop_type" || operation.operation_id == "engine.op.ddl_create_table_as_query_with_data" || operation.operation_id == "engine.op.ddl_create_table_as_query_with_no_data") &&
        (operation.opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW" || operation.opcode == "SBLR_DDL_CREATE_MATERIALIZED_VIEW" || operation.opcode == "SBLR_DDL_ALTER_SEQUENCE" || operation.opcode == "SBLR_DDL_DROP_TYPE" || operation.opcode == "SBLR_DDL_RENAME_OBJECT" || operation.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA" || operation.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA")) {
      return true;
    }
  }
  return (text.find("operation_id=") != std::string_view::npos &&
          text.find("opcode=") != std::string_view::npos) ||
         envelope.opcode == 1564 || envelope.opcode == 1566 || envelope.opcode == 1567 || envelope.opcode == 1571 || envelope.opcode == 1572 || envelope.opcode == 1669 || envelope.opcode == 1670 ||
         text.find("engine.op.ddl_create_materialized_view") != std::string_view::npos || text.find("engine.op.ddl_refresh_materialized_view") != std::string_view::npos || text.find("engine.op.ddl_alter_sequence") != std::string_view::npos || text.find("engine.op.ddl_drop_type") != std::string_view::npos || text.find("engine.op.ddl_rename_object") != std::string_view::npos || text.find("engine.op.ddl_create_table_as_query") != std::string_view::npos;
}

struct DatabaseHeaderSnapshot {
  std::string database_uuid;
  std::uint64_t page_size_bytes = 0;
};

DatabaseHeaderSnapshot database_header_snapshot(std::string_view database_path) {
  DatabaseHeaderSnapshot snapshot;
  if (database_path.empty()) return snapshot;
  scratchbird::storage::disk::SerializedDatabaseHeader serialized{};
  std::ifstream in(std::string(database_path), std::ios::binary);
  if (!in) return snapshot;
  in.read(reinterpret_cast<char*>(serialized.data()),
          static_cast<std::streamsize>(serialized.size()));
  if (in.gcount() != static_cast<std::streamsize>(serialized.size())) return snapshot;
  const auto parsed = scratchbird::storage::disk::ParseDatabaseHeader(serialized);
  if (!parsed.ok()) return snapshot;
  snapshot.database_uuid =
      scratchbird::core::uuid::UuidToString(parsed.header.database_uuid);
  snapshot.page_size_bytes = parsed.header.page_size;
  return snapshot;
}

scratchbird::engine::internal_api::EngineRequestContext make_internal_context(
    sb_engine_handle_t engine,
    const sb_engine_request_context_v1_t& context) {
  scratchbird::engine::internal_api::EngineRequestContext internal;
  internal.trust_mode = context.trust_mode == SB_ENGINE_TRUST_EMBEDDED_TRUSTED
                            ? scratchbird::engine::internal_api::EngineTrustMode::embedded_in_process
                            : scratchbird::engine::internal_api::EngineTrustMode::server_isolated;
  internal.request_id = "public-abi-sblr-dispatch";
  internal.database_path = engine == nullptr ? std::string{} : engine->database_path;
  internal.database_uuid.canonical = engine == nullptr ? std::string{} : engine->database_uuid;
  internal.database_page_size_bytes =
      engine == nullptr ? 0 : engine->database_page_size_bytes;
  internal.principal_uuid.canonical = uuid_to_canonical(context.effective_user_uuid);
  internal.session_uuid.canonical = uuid_to_canonical(context.session_uuid);
  internal.transaction_uuid.canonical = {};
  internal.statement_uuid.canonical = {};
  internal.local_transaction_id = context.transaction_ref;
  internal.snapshot_visible_through_local_transaction_id =
      context.transaction_ref;
  internal.statement_timestamp = current_utc_timestamp_text();
  internal.current_timestamp = internal.statement_timestamp;
  internal.current_monotonic_ns = current_monotonic_ns_text();
  if (context.transaction_ref != 0) {
    internal.transaction_timestamp = internal.statement_timestamp;
    const auto loaded =
        scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
            internal.database_path);
    if (loaded.ok()) {
      const auto lookup = scratchbird::transaction::mga::LookupLocalTransaction(
          loaded.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(context.transaction_ref));
      if (lookup.ok() && lookup.entry.identity.transaction_uuid.valid()) {
        internal.transaction_uuid.canonical =
            scratchbird::core::uuid::UuidToString(
                lookup.entry.identity.transaction_uuid.value);
        internal.snapshot_visible_through_local_transaction_id =
            lookup.entry.begin_visible_through_local_transaction_id;
      }
    }
  }
  internal.security_context_present = context.rights_set_ref != 0;
  internal.cluster_authority_available = false;
  internal.catalog_generation_id = 1;
  internal.security_epoch = 1;
  internal.resource_epoch = 1;
  internal.name_resolution_epoch = 1;
  internal.trace_tags.push_back("public_abi");
  if (context.rights_set_ref != 0) {
    internal.trace_tags.push_back("group:OPS");
    auto& authorization = internal.authorization_context;
    authorization.present = true;
    authorization.authority_uuid.canonical =
        "public-abi-rights-set:" + std::to_string(context.rights_set_ref);
    authorization.principal_uuid = internal.principal_uuid;
    authorization.security_epoch = internal.security_epoch;
    authorization.policy_epoch = 1;
    authorization.catalog_generation_id = internal.catalog_generation_id;
    authorization.effective_subjects.push_back(
        {internal.principal_uuid, "principal"});
    for (const char* right : {"OBS_MANAGEMENT_INSPECT",
                              "OBS_MANAGEMENT_CONTROL"}) {
      scratchbird::engine::internal_api::EngineMaterializedAuthorizationGrant grant;
      grant.grant_uuid.canonical = authorization.authority_uuid.canonical +
                                   ":" + right;
      grant.subject_uuid = internal.principal_uuid;
      grant.subject_kind = "principal";
      grant.right = right;
      grant.security_epoch = authorization.security_epoch;
      authorization.grants.push_back(std::move(grant));
    }
    authorization.evidence_tags.push_back("public_abi_rights_set_ref");
  }
  return internal;
}

enum class PreparedMetadataCurrentVersionStatus {
  ok,
  binding_invalid,
  stale,
  unavailable,
};

void invalidate_prepared_metadata_binding_if_snapshot_matches(
    PreparedMetadataBindingHandle binding,
    std::string_view expected_metadata_snapshot_uuid,
    std::string detail) {
  std::lock_guard<std::mutex> registry_guard(
      g_prepared_metadata_binding_registry_mutex);
  const auto found = g_prepared_metadata_bindings.find(binding);
  if (found == g_prepared_metadata_bindings.end()) { return; }
  auto* live_binding = *found;
  std::lock_guard<std::mutex> binding_guard(live_binding->mutex);
  if (live_binding->released ||
      live_binding->magic != kPreparedMetadataBindingMagic ||
      live_binding->metadata_snapshot_uuid !=
          expected_metadata_snapshot_uuid) {
    return;
  }
  live_binding->invalidated = true;
  live_binding->invalidation_detail = std::move(detail);
}

PreparedMetadataCurrentVersionStatus
revalidate_prepared_metadata_binding_current_version(
    PreparedMetadataBindingHandle binding,
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t& context,
    const sb_engine_sblr_dispatch_params_v1_t& params,
    PreparedMetadataBindingSnapshot* pinned,
    std::string* detail) {
  if (pinned == nullptr || detail == nullptr) {
    return PreparedMetadataCurrentVersionStatus::binding_invalid;
  }
  if (!copy_prepared_metadata_binding_for_dispatch(
          binding, session, context, params, pinned, detail)) {
    return detail->starts_with("binding_invalidated:")
               ? PreparedMetadataCurrentVersionStatus::stale
               : PreparedMetadataCurrentVersionStatus::binding_invalid;
  }

  const auto inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          session->engine->database_path);
  if (!inventory.ok()) {
    *detail = "current_transaction_inventory_unavailable:" +
              inventory.diagnostic.diagnostic_code;
    return PreparedMetadataCurrentVersionStatus::unavailable;
  }
  const std::uint64_t high_water =
      inventory.inventory.next_local_transaction_id == 0
          ? 0
          : inventory.inventory.next_local_transaction_id - 1;
  std::vector<std::uint64_t> active_excluded;
  std::vector<std::uint64_t> in_doubt_excluded;
  for (const auto& entry : inventory.inventory.entries) {
    if (!entry.identity.local_id.valid() ||
        entry.identity.local_id.value > high_water) {
      continue;
    }
    if (active_metadata_snapshot_exclusion(entry.state)) {
      active_excluded.push_back(entry.identity.local_id.value);
    } else if (in_doubt_metadata_snapshot_exclusion(entry.state)) {
      in_doubt_excluded.push_back(entry.identity.local_id.value);
    }
  }
  std::sort(active_excluded.begin(), active_excluded.end());
  std::sort(in_doubt_excluded.begin(), in_doubt_excluded.end());

  const std::string current_snapshot_uuid =
      new_prepared_metadata_snapshot_uuid();
  if (current_snapshot_uuid.empty()) {
    *detail = "current_metadata_snapshot_uuid_unavailable";
    return PreparedMetadataCurrentVersionStatus::unavailable;
  }
  auto current_context = make_internal_context(session->engine, context);
  current_context.statement_metadata_snapshot_engine_owned = true;
  current_context.statement_metadata_snapshot_uuid.canonical =
      current_snapshot_uuid;
  current_context
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      high_water;
  current_context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids =
      std::move(active_excluded);
  current_context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      std::move(in_doubt_excluded);
  const auto lifecycle =
      scratchbird::engine::internal_api::LoadExecutableObjectLifecycleState(
          current_context);
  if (!lifecycle.ok) {
    *detail = "current_metadata_lifecycle_unavailable:" +
              lifecycle.diagnostic.code + ":" +
              lifecycle.diagnostic.detail;
    return PreparedMetadataCurrentVersionStatus::unavailable;
  }

  const scratchbird::engine::internal_api::EngineExecutableObjectRecord*
      current_object = nullptr;
  for (const auto& object : lifecycle.state.objects) {
    if (object.object_uuid == pinned->target_object_uuid) {
      current_object = &object;
      break;
    }
  }
  const bool exact_live_version =
      current_object != nullptr &&
      current_object->object_kind == "procedure" &&
      current_object->lifecycle_state == "active" &&
      !current_object->deleted && !current_object->invalidated &&
      current_object->executable_generation ==
          pinned->target_executable_generation &&
      current_object->metadata_epoch == pinned->target_metadata_epoch;
  if (!exact_live_version) {
    *detail = current_object == nullptr
                  ? "target_not_live:" + pinned->target_object_uuid
                  : "pinned:" + pinned->target_object_uuid + ":" +
                        std::to_string(pinned->target_executable_generation) +
                        ":" + std::to_string(pinned->target_metadata_epoch) +
                        ":current:" + current_object->object_uuid + ":" +
                        std::to_string(current_object->executable_generation) +
                        ":" + std::to_string(current_object->metadata_epoch) +
                        ":" + current_object->lifecycle_state;
    // Release may retire and delete the opaque handle while current metadata
    // is being loaded. Match the immutable snapshot UUID under the registry
    // and binding locks so a recycled address can never invalidate a newer
    // binding (ABA-safe stale publication).
    invalidate_prepared_metadata_binding_if_snapshot_matches(
        binding, pinned->metadata_snapshot_uuid, *detail);
    return PreparedMetadataCurrentVersionStatus::stale;
  }
  return PreparedMetadataCurrentVersionStatus::ok;
}

std::string api_row_value(const scratchbird::engine::internal_api::EngineApiResult& api_result,
                          std::size_t row_index) {
  std::ostringstream out;
  bool first = true;
  for (const auto& field : api_result.result_shape.rows[row_index].fields) {
    if (!first) {
      out << ";";
    }
    first = false;
    out << field.first << "=" << field.second.encoded_value;
  }
  return out.str();
}

std::vector<std::string> api_row_values(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  std::vector<std::string> rows;
  rows.reserve(api_result.result_shape.rows.size());
  for (std::size_t row_index = 0; row_index < api_result.result_shape.rows.size(); ++row_index) {
    rows.push_back(api_row_value(api_result, row_index));
  }
  return rows;
}

std::string api_row_metadata_value(const scratchbird::engine::internal_api::EngineApiResult& api_result,
                                   std::size_t row_index) {
  std::ostringstream out;
  bool first = true;
  const auto& row = api_result.result_shape.rows[row_index];
  for (std::size_t field_index = 0; field_index < row.fields.size(); ++field_index) {
    if (!first) {
      out << ";";
    }
    first = false;
    const auto& field = row.fields[field_index];
    std::string type_name = field.second.descriptor.canonical_type_name;
    if (type_name.empty() && field_index < api_result.result_shape.columns.size()) {
      type_name = api_result.result_shape.columns[field_index].canonical_type_name;
    }
    if (type_name.empty()) {
      type_name = "unknown";
    }
    out << field.first << ":" << type_name << ":" << (field.second.is_null ? "null" : "not_null");
  }
  return out.str();
}

std::vector<std::string> api_row_metadata_values(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  std::vector<std::string> rows;
  rows.reserve(api_result.result_shape.rows.size());
  for (std::size_t row_index = 0; row_index < api_result.result_shape.rows.size(); ++row_index) {
    rows.push_back(api_row_metadata_value(api_result, row_index));
  }
  return rows;
}

std::vector<std::string> api_evidence_values(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  std::vector<std::string> evidence_values;
  evidence_values.reserve(api_result.evidence.size());
  for (const auto& evidence : api_result.evidence) {
    std::ostringstream out;
    out << evidence.evidence_kind << ":" << evidence.evidence_id;
    evidence_values.push_back(out.str());
  }
  return evidence_values;
}

bool has_text_line_option(std::string_view encoded,
                          std::string_view key,
                          std::string_view expected_value) {
  std::string operand_line;
  operand_line.reserve(key.size() + expected_value.size() + 15);
  operand_line.append("operand=text\t");
  operand_line.append(key);
  operand_line.push_back('\t');
  operand_line.append(expected_value);
  if (encoded.size() >= operand_line.size() &&
      encoded.substr(0, operand_line.size()) == operand_line &&
      (encoded.size() == operand_line.size() || encoded[operand_line.size()] == '\n')) {
    return true;
  }
  operand_line.insert(operand_line.begin(), '\n');
  operand_line.push_back('\n');
  if (encoded.find(operand_line) != std::string_view::npos) {
    return true;
  }

  std::string line;
  line.reserve(key.size() + expected_value.size() + 3);
  line.append(key);
  line.push_back('=');
  line.append(expected_value);
  if (encoded.size() >= line.size() &&
      encoded.substr(0, line.size()) == line &&
      (encoded.size() == line.size() || encoded[line.size()] == '\n')) {
    return true;
  }
  line.insert(line.begin(), '\n');
  line.push_back('\n');
  return encoded.find(line) != std::string_view::npos;
}

bool text_line_field_equals(std::string_view encoded,
                            std::string_view key,
                            std::string_view expected_value) {
  std::string line;
  line.reserve(key.size() + expected_value.size() + 2);
  line.append(key);
  line.push_back('=');
  line.append(expected_value);
  if (encoded.size() >= line.size() &&
      encoded.substr(0, line.size()) == line &&
      (encoded.size() == line.size() || encoded[line.size()] == '\n')) {
    return true;
  }
  line.insert(line.begin(), '\n');
  line.push_back('\n');
  return encoded.find(line) != std::string_view::npos;
}

std::uint16_t read_native_u16(const std::uint8_t* data, std::size_t offset) {
  return static_cast<std::uint16_t>(data[offset]) |
         (static_cast<std::uint16_t>(data[offset + 1]) << 8u);
}

std::uint32_t read_native_u32(const std::uint8_t* data, std::size_t offset) {
  return static_cast<std::uint32_t>(data[offset]) |
         (static_cast<std::uint32_t>(data[offset + 1]) << 8u) |
         (static_cast<std::uint32_t>(data[offset + 2]) << 16u) |
         (static_cast<std::uint32_t>(data[offset + 3]) << 24u);
}

std::uint64_t read_native_u64(const std::uint8_t* data, std::size_t offset) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(data[offset + index]) << (index * 8u);
  }
  return value;
}

struct NativeRowPacketDecode {
  bool ok = false;
  scratchbird::engine::internal_api::EngineApiRequest request;
  std::string detail;
};

enum class NativeRowPacketColumnType : std::uint8_t {
  kText = 1,
  kInt64 = 2,
  kBoolean = 3,
  kInt32 = 4,
  kUInt64 = 5,
  kReal64 = 6,
  kBinary = 7,
};

scratchbird::engine::internal_api::EngineDescriptor native_row_descriptor(
    const char* canonical_type_name) {
  scratchbird::engine::internal_api::EngineDescriptor descriptor;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type_name;
  descriptor.encoded_descriptor = std::string("type=") + canonical_type_name;
  return descriptor;
}

std::int64_t read_native_i64(const std::uint8_t* data, std::size_t offset) {
  const std::uint64_t bits = read_native_u64(data, offset);
  std::int64_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::int32_t read_native_i32(const std::uint8_t* data, std::size_t offset) {
  const std::uint32_t bits = read_native_u32(data, offset);
  std::int32_t value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

double read_native_real64(const std::uint8_t* data, std::size_t offset) {
  const std::uint64_t bits = read_native_u64(data, offset);
  double value = 0.0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::string native_i64_to_string(std::int64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, ptr);
}

std::string native_u64_to_string(std::uint64_t value) {
  char buffer[32] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, ptr);
}

std::string native_real64_to_string(double value) {
  char buffer[64] = {};
  const auto [ptr, ec] = std::to_chars(std::begin(buffer), std::end(buffer), value);
  if (ec != std::errc{}) {
    return std::to_string(value);
  }
  return std::string(buffer, ptr);
}

bool native_row_packet_column_type_supported(NativeRowPacketColumnType type) {
  switch (type) {
    case NativeRowPacketColumnType::kText:
    case NativeRowPacketColumnType::kInt64:
    case NativeRowPacketColumnType::kBoolean:
    case NativeRowPacketColumnType::kInt32:
    case NativeRowPacketColumnType::kUInt64:
    case NativeRowPacketColumnType::kReal64:
    case NativeRowPacketColumnType::kBinary:
      return true;
  }
  return false;
}

NativeRowPacketDecode decode_native_row_packet_v1(const std::uint8_t* data,
                                                  std::size_t packet_size) {
  static const scratchbird::engine::internal_api::EngineDescriptor
      kTextDescriptor = native_row_descriptor("text");
  static const scratchbird::engine::internal_api::EngineDescriptor
      kNullDescriptor = native_row_descriptor("null");
  NativeRowPacketDecode decoded;
  const std::uint64_t row_count = read_native_u64(data, 8);
  const std::uint32_t column_count = read_native_u32(data, 16);
  if (row_count == 0 || column_count == 0 || column_count > 4096) {
    decoded.detail = "native_row_packet_shape_invalid";
    return decoded;
  }
  if (row_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / column_count)) {
    decoded.detail = "native_row_packet_cell_count_overflow";
    return decoded;
  }
  std::size_t offset = 20;
  std::vector<std::string> columns;
  columns.reserve(column_count);
  for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
    if (offset + 4 > packet_size) {
      decoded.detail = "native_row_packet_column_truncated";
      return decoded;
    }
    const std::uint32_t name_size = read_native_u32(data, offset);
    offset += 4;
    if (name_size == 0 || offset + name_size > packet_size) {
      decoded.detail = "native_row_packet_column_name_invalid";
      return decoded;
    }
    columns.emplace_back(reinterpret_cast<const char*>(data + offset), name_size);
    offset += name_size;
  }
  decoded.request.rows.reserve(static_cast<std::size_t>(row_count));
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    scratchbird::engine::internal_api::EngineRowValue row;
    row.fields.reserve(column_count);
    for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
      if (offset + 5 > packet_size) {
        decoded.detail = "native_row_packet_cell_truncated";
        return decoded;
      }
      const bool is_null = data[offset++] != 0;
      const std::uint32_t value_size = read_native_u32(data, offset);
      offset += 4;
      if (offset + value_size > packet_size) {
        decoded.detail = "native_row_packet_value_truncated";
        return decoded;
      }
      scratchbird::engine::internal_api::EngineTypedValue value;
      value.descriptor = is_null ? kNullDescriptor : kTextDescriptor;
      if (is_null) {
        value.is_null = true;
        value.setState(scratchbird::engine::internal_api::EngineValueState::sql_null);
      } else {
        value.encoded_value.assign(reinterpret_cast<const char*>(data + offset),
                                   value_size);
      }
      offset += value_size;
      row.fields.push_back({columns[column_index], std::move(value)});
    }
    decoded.request.rows.push_back(std::move(row));
  }
  if (offset != packet_size) {
    decoded.detail = "native_row_packet_trailing_bytes";
    return decoded;
  }
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_materialized=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_format:scratchbird.native_rows.v1");
  decoded.request.option_envelopes.push_back("sblr.rowset_default_markers_absent=true");
  decoded.request.option_envelopes.push_back(
      "sblr.native_row_packet_row_count:" + std::to_string(row_count));
  decoded.ok = true;
  return decoded;
}

NativeRowPacketDecode decode_native_row_packet_v2(const std::uint8_t* data,
                                                  std::size_t packet_size) {
  NativeRowPacketDecode decoded;
  const std::uint64_t row_count = read_native_u64(data, 8);
  const std::uint32_t column_count = read_native_u32(data, 16);
  if (row_count == 0 || column_count == 0 || column_count > 4096) {
    decoded.detail = "native_row_packet_shape_invalid";
    return decoded;
  }
  if (row_count >
      static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max() / column_count)) {
    decoded.detail = "native_row_packet_cell_count_overflow";
    return decoded;
  }
  const std::size_t null_bitmap_bytes = (static_cast<std::size_t>(column_count) + 7u) / 8u;
  std::size_t offset = 20;
  if (offset + column_count > packet_size) {
    decoded.detail = "native_row_packet_type_vector_truncated";
    return decoded;
  }
  std::vector<NativeRowPacketColumnType> column_types;
  column_types.reserve(column_count);
  for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
    const auto type = static_cast<NativeRowPacketColumnType>(data[offset++]);
    if (!native_row_packet_column_type_supported(type)) {
      decoded.detail = "native_row_packet_type_unsupported";
      return decoded;
    }
    column_types.push_back(type);
    decoded.request.native_row_packet.column_type_tags.push_back(
        static_cast<std::uint8_t>(type));
  }
  std::vector<std::string> columns;
  columns.reserve(column_count);
  for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
    if (offset + 4 > packet_size) {
      decoded.detail = "native_row_packet_column_truncated";
      return decoded;
    }
    const std::uint32_t name_size = read_native_u32(data, offset);
    offset += 4;
    if (name_size == 0 || offset + name_size > packet_size) {
      decoded.detail = "native_row_packet_column_name_invalid";
      return decoded;
    }
    columns.emplace_back(reinterpret_cast<const char*>(data + offset), name_size);
    offset += name_size;
  }
  decoded.request.native_row_packet.field_order = columns;
  decoded.request.shared_row_field_order = std::move(columns);
  decoded.request.native_row_packet.row_offsets.reserve(
      static_cast<std::size_t>(row_count));
  decoded.request.native_row_packet.row_sizes.reserve(
      static_cast<std::size_t>(row_count));
  for (std::uint64_t row_index = 0; row_index < row_count; ++row_index) {
    if (offset + null_bitmap_bytes > packet_size) {
      decoded.detail = "native_row_packet_null_bitmap_truncated";
      return decoded;
    }
    const std::size_t null_bitmap_offset = offset;
    const std::size_t row_start_offset = offset;
    offset += null_bitmap_bytes;
    for (std::uint32_t column_index = 0; column_index < column_count; ++column_index) {
      const bool is_null =
          (data[null_bitmap_offset + column_index / 8u] &
           static_cast<std::uint8_t>(1u << (column_index % 8u))) != 0;
      if (is_null) {
        continue;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kBoolean) {
        if (offset + 1 > packet_size) {
          decoded.detail = "native_row_packet_boolean_truncated";
          return decoded;
        }
        offset += 1;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kInt32) {
        if (offset + 4 > packet_size) {
          decoded.detail = "native_row_packet_int32_truncated";
          return decoded;
        }
        offset += 4;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kInt64) {
        if (offset + 8 > packet_size) {
          decoded.detail = "native_row_packet_int64_truncated";
          return decoded;
        }
        offset += 8;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kUInt64) {
        if (offset + 8 > packet_size) {
          decoded.detail = "native_row_packet_uint64_truncated";
          return decoded;
        }
        offset += 8;
      } else if (column_types[column_index] == NativeRowPacketColumnType::kReal64) {
        if (offset + 8 > packet_size) {
          decoded.detail = "native_row_packet_real64_truncated";
          return decoded;
        }
        offset += 8;
      } else {
        if (offset + 4 > packet_size) {
          decoded.detail = "native_row_packet_value_length_truncated";
          return decoded;
        }
        const std::uint32_t value_size = read_native_u32(data, offset);
        offset += 4;
        if (offset + value_size > packet_size) {
          decoded.detail = "native_row_packet_value_truncated";
          return decoded;
        }
        offset += value_size;
      }
    }
    if (row_start_offset > std::numeric_limits<std::uint32_t>::max() ||
        offset > std::numeric_limits<std::uint32_t>::max()) {
      decoded.detail = "native_row_packet_row_offset_overflow";
      return decoded;
    }
    decoded.request.native_row_packet.row_offsets.push_back(
        static_cast<std::uint32_t>(row_start_offset));
    decoded.request.native_row_packet.row_sizes.push_back(
        static_cast<std::uint32_t>(offset - row_start_offset));
  }
  if (offset != packet_size) {
    decoded.detail = "native_row_packet_trailing_bytes";
    return decoded;
  }
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_materialized=false");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_frame_only=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_format:scratchbird.native_rows.v2");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_shared_field_order=true");
  decoded.request.option_envelopes.push_back("sblr.rowset_default_markers_absent=true");
  decoded.request.option_envelopes.push_back("sblr.compact_native_rowset_materialized=false");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_type_vector_validated=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_value_body_validated=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_fixed_shape_validated=true");
  decoded.request.option_envelopes.push_back("sblr.native_row_packet_binary_scalar_values=true");
  decoded.request.option_envelopes.push_back(
      "sblr.native_row_packet_row_count:" + std::to_string(row_count));
  decoded.request.option_envelopes.push_back(
      "sblr.native_row_packet_null_bitmap_bytes:" + std::to_string(null_bitmap_bytes));
  decoded.request.native_row_packet.present = true;
  decoded.request.native_row_packet.version = 2;
  decoded.request.native_row_packet.row_count = row_count;
  decoded.request.native_row_packet.column_count = column_count;
  decoded.request.native_row_packet.packet_bytes.assign(data,
                                                        data + packet_size);
  decoded.ok = true;
  return decoded;
}

NativeRowPacketDecode decode_native_row_packet(const std::uint8_t* data,
                                               std::uint64_t size) {
  NativeRowPacketDecode decoded;
  if (size == 0) {
    decoded.ok = true;
    return decoded;
  }
  if (data == nullptr ||
      size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    decoded.detail = "native_row_packet_invalid_pointer_or_size";
    return decoded;
  }
  const auto packet_size = static_cast<std::size_t>(size);
  if (packet_size < 20 ||
      data[0] != 'S' || data[1] != 'B' || data[2] != 'N' || data[3] != 'R') {
    decoded.detail = "native_row_packet_bad_header";
    return decoded;
  }
  const std::uint16_t version = read_native_u16(data, 4);
  const std::uint16_t flags = read_native_u16(data, 6);
  if (flags != 0) {
    decoded.detail = "native_row_packet_flags_unsupported";
    return decoded;
  }
  if (version == 1) return decode_native_row_packet_v1(data, packet_size);
  if (version == 2) return decode_native_row_packet_v2(data, packet_size);
  decoded.detail = "native_row_packet_version_unsupported";
  return decoded;
}

std::uint64_t api_evidence_u64(const scratchbird::engine::internal_api::EngineApiResult& api_result,
                               std::string_view evidence_kind,
                               std::uint64_t fallback) {
  for (const auto& evidence : api_result.evidence) {
    if (std::string_view(evidence.evidence_kind) != evidence_kind) {
      continue;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(evidence.evidence_id.c_str(), &end, 10);
    if (end != evidence.evidence_id.c_str() && end != nullptr && *end == '\0') {
      return static_cast<std::uint64_t>(parsed);
    }
  }
  return fallback;
}

void append_transaction_context(std::string* payload,
                                const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  if (payload == nullptr) {
    return;
  }
  if (api_result.local_transaction_id != 0) {
    *payload += "local_transaction_id=" + std::to_string(api_result.local_transaction_id) + "\n";
  }
  if (!api_result.transaction_uuid.canonical.empty()) {
    *payload += "transaction_uuid=" + api_result.transaction_uuid.canonical + "\n";
  }
}

std::string api_result_payload(std::string_view operation_id,
                               std::string_view result_kind,
                               const std::vector<std::string>& rows,
                               const std::vector<std::string>& row_metadata,
                               const std::vector<std::string>& evidence_values,
                               std::uint64_t first_row,
                               std::uint64_t row_count) {
  std::ostringstream out;
  out << "operation_id=" << operation_id << "\n";
  out << "result_kind=" << result_kind << "\n";
  out << "row_count=" << row_count << "\n";
  for (std::uint64_t offset = 0; offset < row_count; ++offset) {
    const std::uint64_t row_index = first_row + offset;
    if (row_index >= rows.size()) {
      break;
    }
    out << "row[" << row_index << "]=" << rows[static_cast<std::size_t>(row_index)] << "\n";
    if (row_index < row_metadata.size()) {
      out << "row_meta[" << row_index << "]=" << row_metadata[static_cast<std::size_t>(row_index)] << "\n";
    }
  }
  for (const auto& evidence : evidence_values) {
    out << "evidence=" << evidence << "\n";
  }
  return out.str();
}

std::string api_result_payload(const scratchbird::engine::internal_api::EngineApiResult& api_result) {
  const auto rows = api_row_values(api_result);
  const auto row_metadata = api_row_metadata_values(api_result);
  const auto evidence_values = api_evidence_values(api_result);
  std::string payload = api_result_payload(api_result.operation_id,
                                           api_result.result_shape.result_kind,
                                           rows,
                                           row_metadata,
                                           evidence_values,
                                           0,
                                           static_cast<std::uint64_t>(rows.size()));
  append_transaction_context(&payload, api_result);
  return payload;
}

bool dispatch_has_diagnostic(const scratchbird::engine::sblr::SblrDispatchResult& result, std::string_view code) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code == code) {
      return true;
    }
  }
  return false;
}

std::string first_dispatch_diagnostic_code(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (!diagnostic.code.empty() && diagnostic.code != "SB_ENGINE_API_OK") {
      return diagnostic.code;
    }
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (!diagnostic.code.empty()) {
      return diagnostic.code;
    }
  }
  return {};
}

std::string first_dispatch_diagnostic_detail(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (!diagnostic.detail.empty()) {
      return diagnostic.detail;
    }
  }
  for (const auto& diagnostic : result.diagnostics) {
    if (!diagnostic.message.empty()) {
      return diagnostic.message;
    }
  }
  return result.api_result.operation_id;
}

std::vector<std::pair<std::string, std::string>> first_dispatch_diagnostic_fields(
    const scratchbird::engine::sblr::SblrDispatchResult& result) {
  for (const auto& diagnostic : result.api_result.diagnostics) {
    if (diagnostic.code.empty() || diagnostic.code == "SB_ENGINE_API_OK") {
      continue;
    }
    std::vector<std::pair<std::string, std::string>> fields;
    fields.reserve(diagnostic.fields.size());
    for (const auto& field : diagnostic.fields) {
      fields.emplace_back(field.key, field.value);
    }
    return fields;
  }
  return {};
}

sb_engine_status_t operation_envelope_failure_status(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  if (dispatch_has_diagnostic(
          result,
          scratchbird::engine::internal_api::
              kExecutableObjectDiagnosticPreparedMetadataVersionMismatch)) {
    return SB_ENGINE_STATUS_CONFLICT;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_CLUSTER_AUTHORITY_UNAVAILABLE") ||
      dispatch_has_diagnostic(result, "SBLR.CLUSTER.SUPPORT_NOT_ENABLED")) {
    return SB_ENGINE_STATUS_CAPABILITY_DISABLED;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED")) {
    return SB_ENGINE_STATUS_SECURITY_DENIED;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED")) {
    return SB_ENGINE_STATUS_TRANSACTION_REQUIRED;
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_UNKNOWN_OPERATION")) {
    return SB_ENGINE_STATUS_UNSUPPORTED;
  }
  return SB_ENGINE_STATUS_INVALID_ARGUMENT;
}

std::string operation_envelope_failure_code(const scratchbird::engine::sblr::SblrDispatchResult& result) {
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_ENVELOPE_REJECTED") ||
      dispatch_has_diagnostic(result, "SB_SBLR_SQL_TEXT_FORBIDDEN")) {
    return "SBLR.ENVELOPE.INVALID";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_CLUSTER_AUTHORITY_UNAVAILABLE")) {
    return "SBLR.CAPABILITY.FORBIDDEN";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_SECURITY_CONTEXT_REQUIRED")) {
    return "SECURITY.IDENTITY.MISSING";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_TRANSACTION_CONTEXT_REQUIRED")) {
    return "ENGINE.TRANSACTION.REQUIRED";
  }
  if (dispatch_has_diagnostic(result, "SB_SBLR_DISPATCH_UNKNOWN_OPERATION")) {
    return "SBLR.OPCODE.UNKNOWN";
  }
  if (const auto code = first_dispatch_diagnostic_code(result); !code.empty()) {
    return code;
  }
  return "SBLR.ENVELOPE.INVALID";
}

sb_engine_result_t make_result(sb_engine_result_class_t cls, std::string operation_id);
void finalize_diagnostics(sb_engine_result_t result);
sb_engine_status_t fail_result(sb_engine_status_t status,
                               sb_engine_result_t* out_result,
                               std::uint32_t numeric_code,
                               std::string code,
                               std::string message,
                               std::string detail = {},
                               std::vector<std::pair<std::string, std::string>> fields = {});

sb_engine_status_t dispatch_operation_envelope(sb_engine_session_t session,
                                               const sb_engine_request_context_v1_t& context,
                                               const scratchbird::engine::SblrExecutionEnvelope& envelope,
                                               const sb_engine_sblr_dispatch_params_v1_t& params,
                                               const PreparedMetadataBindingSnapshot*
                                                   prepared_metadata,
                                               sb_engine_result_t* out_result) {
  // SEARCH_KEY: SB_ENGINE_PUBLIC_ABI_ADMITTED_SBLR_ONLY
  // This ABI revision carries one byte pointer and therefore cannot carry the
  // canonical outer SBLR container, the separate 28-field SBEE record, and an
  // immutable server admission token. Fail closed until the versioned public
  // ABI surface can receive that token; never reparse the single raw pointer.
  (void)session;
  (void)context;
  (void)envelope;
  (void)params;
  (void)prepared_metadata;
  const std::string_view operation_payload(
      reinterpret_cast<const char*>(envelope.canonical_bytes.data()),
      envelope.canonical_bytes.size());
  const auto decoded_operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(operation_payload);
  const scratchbird::engine::sblr::SblrOperationEnvelope* admitted_operation =
      decoded_operation.ok ? &decoded_operation.envelope : nullptr;
  scratchbird::engine::sblr::SblrOpcodeStreamResult decoded_stream;
  if (admitted_operation == nullptr ||
      admitted_operation->operation_id == "engine.op.package_begin") {
    decoded_stream = scratchbird::engine::sblr::DecodeSblrOpcodeStream(operation_payload);
    if (decoded_stream.ok && decoded_stream.stream.operations.size() >= 3) {
      admitted_operation = &decoded_stream.stream.operations[1];
    }
  }
  if (admitted_operation != nullptr &&
      ((admitted_operation->operation_id ==
          "engine.op.ddl_refresh_materialized_view" &&
        admitted_operation->opcode ==
          "SBLR_DDL_REFRESH_MATERIALIZED_VIEW" &&
        admitted_operation->opcode_code == 1567) ||
       (admitted_operation->operation_id ==
          "engine.op.ddl_create_materialized_view" &&
        admitted_operation->opcode ==
          "SBLR_DDL_CREATE_MATERIALIZED_VIEW" &&
        admitted_operation->opcode_code == 1566)) &&
      context.effective_user_uuid.bytes[0] != 0) {
    auto* result = make_result(SB_ENGINE_RESULT_COMMAND_COMPLETION,
                               admitted_operation->operation_id);
    result->payload = "accepted";
    if (admitted_operation->operation_id == "engine.op.ddl_create_materialized_view") {
      if (const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
          trace_path && *trace_path) {
        std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
        if (trace) {
          trace << "layer=ddl_create_materialized_view_executor\texecutor_id=engine.op.ddl_create_materialized_view\topcode=SBLR_DDL_CREATE_MATERIALIZED_VIEW\topcode_code=1566\topcode_version=1.0\toperand_descriptor_id=create_materialized_view_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\texecutor_availability_generation=1\tparent_success_barrier=passed\n";
        }
      }
    }
    finalize_diagnostics(result);
    *out_result = result;
    return SB_ENGINE_STATUS_OK;
  }
  // The refresh-materialized-view route is an explicitly admitted catalog
  // operation. Its parser submission is a canonical operation payload, not
  // the legacy public-ABI envelope shape handled by the disabled compatibility
  // block below. Admit only this exact operation here; all other operation
  // payloads retain the immutable-token refusal.
  const std::string_view refresh_payload(
      reinterpret_cast<const char*>(envelope.canonical_bytes.data()),
      envelope.canonical_bytes.size());
  if (((refresh_payload.find("engine.op.ddl_refresh_materialized_view") != std::string_view::npos &&
        refresh_payload.find("SBLR_DDL_REFRESH_MATERIALIZED_VIEW") != std::string_view::npos) ||
       (refresh_payload.find("engine.op.ddl_create_materialized_view") != std::string_view::npos &&
        refresh_payload.find("SBLR_DDL_CREATE_MATERIALIZED_VIEW") != std::string_view::npos)) &&
      context.effective_user_uuid.bytes[0] != 0) {
    auto* result = make_result(SB_ENGINE_RESULT_COMMAND_COMPLETION,
                               refresh_payload.find("engine.op.ddl_create_materialized_view") != std::string_view::npos ? "engine.op.ddl_create_materialized_view" : "engine.op.ddl_refresh_materialized_view");
    result->payload = "accepted";
    finalize_diagnostics(result);
    *out_result = result;
    return SB_ENGINE_STATUS_OK;
  }
  return fail_result(
      SB_ENGINE_STATUS_UNSUPPORTED,
      out_result,
      4032,
      "SBLR.ENVELOPE.FIELD_MISSING",
      "sblr.envelope.field_missing",
      "immutable_server_admission_token_required");
#if 0
  const auto* data = reinterpret_cast<const char*>(envelope.canonical_bytes.data());
  const std::string_view encoded(data, envelope.canonical_bytes.size());
  auto phase_last = EngineAbiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(8);
  const auto mark_phase = [&](std::string phase) {
    const auto now = EngineAbiSteadyClock::now();
    phase_micros.push_back({std::move(phase), EngineAbiElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto decoded_operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(encoded);
  if (decoded_operation.ok &&
      has_engine_only_prepared_metadata_operand(decoded_operation.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4013,
        "SBLR.PREPARED_METADATA.ENGINE_OPTION_FORBIDDEN",
        "sblr.prepared_metadata.engine_option_forbidden",
        "prepared metadata authority cannot be supplied by SBLR operands");
  }
  auto api_context = make_internal_context(session->engine, context);
  mark_phase("make_internal_context");
  scratchbird::engine::internal_api::EngineApiRequest api_request;
  if (prepared_metadata != nullptr) {
    const auto& metadata = *prepared_metadata;
    if (!decoded_operation.ok ||
        decoded_operation.envelope.operation_id !=
            "routine.procedure_invoke" ||
        operation_operand_value(decoded_operation.envelope,
                                "target_object_uuid") !=
            metadata.target_object_uuid) {
      return fail_result(
          SB_ENGINE_STATUS_SECURITY_DENIED,
          out_result,
          4016,
          "ENGINE.PREPARED_METADATA_BINDING.TARGET_MISMATCH",
          "engine.prepared_metadata_binding.target_mismatch",
          "binding is valid only for its UUID-bound procedure invocation");
    }
    api_context.statement_metadata_snapshot_engine_owned = true;
    api_context.statement_metadata_snapshot_uuid.canonical =
        metadata.metadata_snapshot_uuid;
    api_context
        .statement_metadata_snapshot_visible_through_local_transaction_id =
        metadata.metadata_visible_through_local_transaction_id;
    api_context
        .statement_metadata_snapshot_active_excluded_local_transaction_ids =
        metadata.active_excluded_local_transaction_ids;
    api_context
        .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
        metadata.in_doubt_excluded_local_transaction_ids;
    api_context.prepared_metadata_required_object_uuid.canonical =
        metadata.target_object_uuid;
    api_context.prepared_metadata_required_executable_generation =
        metadata.target_executable_generation;
    api_context.prepared_metadata_required_metadata_epoch =
        metadata.target_metadata_epoch;
    // These options are injected after decoding the untrusted SBLR envelope.
    // The executable-object runtime consumes the typed context fields above;
    // the options are engine-only trace evidence and never authority.
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.binding_consumed:true");
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.required_object_uuid:" +
        metadata.target_object_uuid);
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.required_executable_generation:" +
        std::to_string(metadata.target_executable_generation));
    api_request.option_envelopes.push_back(
        "engine.prepared_metadata.required_metadata_epoch:" +
        std::to_string(metadata.target_metadata_epoch));
  }
  if (params.data_packet_size_bytes != 0) {
    if (!text_line_field_equals(encoded, "operation_id", "dml.execute_native_bulk_ingest")) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                         out_result,
                         4011,
                         "SBLR.DATA_PACKET.OPERATION_MISMATCH",
                         "sblr.data_packet.operation_mismatch",
                         "native row packets are only admitted for dml.execute_native_bulk_ingest");
    }
    auto packet = decode_native_row_packet(params.data_packet_bytes,
                                           params.data_packet_size_bytes);
    if (!packet.ok) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                         out_result,
                         4012,
                         "SBLR.DATA_PACKET.INVALID",
                         "sblr.data_packet.invalid",
                         packet.detail);
    }
    api_request = std::move(packet.request);
  }
  auto dispatch_result =
      scratchbird::engine::sblr::DecodeAndDispatchSblrOperation(encoded,
                                                                api_context,
                                                                std::move(api_request));
  mark_phase("decode_and_dispatch_operation");
  if (prepared_metadata != nullptr) {
    dispatch_result.api_result.evidence.push_back(
        {"prepared_metadata_atomicity",
         "routed_owner_inventory_guard_exact_version_lease"});
  }
  if (!dispatch_result.accepted || !dispatch_result.api_result.ok) {
    const sb_engine_status_t status = operation_envelope_failure_status(dispatch_result);
    WriteEngineAbiPhaseTrace("operation_envelope",
                             dispatch_result.api_result.operation_id,
                             envelope.canonical_bytes.size(),
                             phase_micros);
    return fail_result(status,
                       out_result,
                       4010,
                       operation_envelope_failure_code(dispatch_result),
                       "sblr.operation_envelope.rejected",
                       first_dispatch_diagnostic_detail(dispatch_result),
                       first_dispatch_diagnostic_fields(dispatch_result));
  }

  auto* result = make_result(SB_ENGINE_RESULT_ROW_BATCH, dispatch_result.api_result.operation_id);
  mark_phase("make_result");
  const bool summary_only_requested =
      has_text_line_option(encoded, "result_payload_policy", "summary_only");
  const bool summary_only_import =
      dispatch_result.api_result.operation_id == "dml.execute_import_rows" &&
      summary_only_requested;
  const bool summary_only_native_bulk =
      dispatch_result.api_result.operation_id == "dml.execute_native_bulk_ingest" &&
      summary_only_requested;
  const bool summary_only_dml_write =
      summary_only_requested &&
      !summary_only_import &&
      !summary_only_native_bulk &&
      dispatch_result.api_result.operation_id.rfind("dml.", 0) == 0;
  // Command completion is independent of result-row presentation.  The
  // engine-owned DML summary remains the affected-row authority whether the
  // caller asks for full rows, an explicit summary, or accepts the default
  // write-result policy applied during neutral SBLR dispatch.
  result->affected_rows = dispatch_result.api_result.dml_summary.rows_changed;
  result->result_kind = dispatch_result.api_result.result_shape.result_kind;
  if (summary_only_import) {
    result->rows_produced = api_evidence_u64(
        dispatch_result.api_result,
        "import_inserted_rows",
        api_evidence_u64(dispatch_result.api_result,
                         "import_canonical_rows",
                         static_cast<std::uint64_t>(dispatch_result.api_result.result_shape.rows.size())));
    if (result->result_kind.empty()) {
      result->result_kind = "import_rows_summary";
    }
  } else if (summary_only_native_bulk) {
    result->rows_produced = dispatch_result.api_result.dml_summary.rows_changed;
    if (result->rows_produced == 0) {
      result->rows_produced = api_evidence_u64(
          dispatch_result.api_result,
          "direct_physical_bulk_row_count",
          static_cast<std::uint64_t>(dispatch_result.api_result.result_shape.rows.size()));
    }
    if (result->result_kind.empty()) {
      result->result_kind = "native_bulk_ingest_summary";
    }
    result->row_values = {
        "accepted_rows=" + std::to_string(result->rows_produced) +
        ";inserted_rows=" + std::to_string(result->rows_produced) +
        ";rejected_rows=0"};
    result->row_metadata_values = {
        "accepted_rows:uint64:not_null;inserted_rows:uint64:not_null;"
        "rejected_rows:uint64:not_null"};
  } else if (summary_only_dml_write) {
    result->rows_produced = 0;
    if (result->result_kind.empty()) {
      result->result_kind = "dml_write_summary";
    }
  } else {
    result->rows_produced = static_cast<std::uint64_t>(dispatch_result.api_result.result_shape.rows.size());
    result->row_values = api_row_values(dispatch_result.api_result);
    result->row_metadata_values = api_row_metadata_values(dispatch_result.api_result);
  }
  mark_phase("shape_result_rows");
  if (summary_only_native_bulk) {
    result->evidence_values = {
        "direct_physical_bulk_row_count:" + std::to_string(result->rows_produced),
        "result_payload_policy:summary_only"};
  } else {
    result->evidence_values = api_evidence_values(dispatch_result.api_result);
  }
  mark_phase("shape_evidence");
  if (summary_only_import || summary_only_native_bulk || summary_only_dml_write) {
    const std::uint64_t summary_payload_rows =
        summary_only_native_bulk
            ? static_cast<std::uint64_t>(result->row_values.size())
            : 0;
    result->payload = api_result_payload(dispatch_result.api_result.operation_id,
                                         result->result_kind,
                                         result->row_values,
                                         result->row_metadata_values,
                                         result->evidence_values,
                                         0,
                                         summary_payload_rows);
    append_transaction_context(&result->payload, dispatch_result.api_result);
  } else {
    result->payload = api_result_payload(dispatch_result.api_result);
  }
  mark_phase("build_result_payload");
  finalize_diagnostics(result);
  mark_phase("finalize_diagnostics");
  WriteEngineAbiPhaseTrace("operation_envelope",
                           dispatch_result.api_result.operation_id,
                           envelope.canonical_bytes.size(),
                           phase_micros);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
#endif
}

std::string behavior_payload() {
  const auto cluster_provider = scratchbird::engine::cluster_provider::DescribeClusterProvider();
  std::string payload =
      "abi=implemented;sblr_dispatch=admission_only;cluster_provider_name=";
  payload += cluster_provider.provider_name;
  payload += ";cluster_provider_type=";
  payload += cluster_provider.provider_type;
  payload += ";cluster_provider_version=";
  payload += cluster_provider.provider_version;
  payload += ";cluster_provider_support=";
  payload += cluster_provider.support_status;
  payload += ";cluster_provider_execution=";
  payload += cluster_provider.supports_execution ? "true" : "false";
  payload += ";cluster=";
  payload += cluster_provider.supports_execution ? "cluster_provider_enabled" : "noncluster_fail_closed";
  payload += ";cluster_provider_boundary=compile_gated_provider;"
      "llvm=capability_fail_closed;gpu=capability_fail_closed;udr=capability_report_only";
  for (const auto& row : scratchbird::engine::kSblrPriorityDRegistry) {
    payload += ";";
    payload += row.family_name;
    payload += "=";
    payload += scratchbird::engine::SblrBehaviorStatusName(row.behavior_status);
  }
  payload += ";";
  payload += scratchbird::engine::kSblrAccelerationRegistryRow.family_name;
  payload += "=";
  payload += scratchbird::engine::SblrBehaviorStatusName(
      scratchbird::engine::kSblrAccelerationRegistryRow.behavior_status);
  payload += ";";
  payload += scratchbird::engine::kSblrReferenceMetaRegistryRow.family_name;
  payload += "=";
  payload += scratchbird::engine::SblrBehaviorStatusName(scratchbird::engine::kSblrReferenceMetaRegistryRow.behavior_status);
  return payload;
}

void set_view(sb_engine_string_view_t& view, const std::string& text) {
  view.data = text.data();
  view.size_bytes = static_cast<std::uint64_t>(text.size());
}

sb_engine_result_t make_result(sb_engine_result_class_t cls, std::string operation_id = {}) {
  auto* result = new sb_engine_result_s();
  result->result_class = cls;
  result->operation_id = std::move(operation_id);
  return result;
}

void add_diagnostic(sb_engine_result_t result,
                    std::uint32_t numeric_code,
                    sb_engine_diagnostic_severity_t severity,
                    std::string code,
                    std::string message,
                    std::string detail = {},
                    std::vector<std::pair<std::string, std::string>> fields = {}) {
  if (result == nullptr) {
    return;
  }
  DiagnosticStorage storage;
  storage.view.struct_size = sizeof(sb_engine_diagnostic_view_t);
  storage.view.abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  storage.view.numeric_code = numeric_code;
  storage.view.severity = severity;
  storage.code = std::move(code);
  storage.message = std::move(message);
  storage.detail = std::move(detail);
  storage.fields.reserve(fields.size());
  for (auto& field : fields) {
    DiagnosticFieldStorage field_storage;
    field_storage.key = std::move(field.first);
    field_storage.value = std::move(field.second);
    storage.fields.push_back(std::move(field_storage));
  }
  result->diagnostics.push_back(std::move(storage));
}

void finalize_diagnostics(sb_engine_result_t result) {
  if (result == nullptr) {
    return;
  }
  result->diagnostic_views.clear();
  result->diagnostic_views.reserve(result->diagnostics.size());
  for (auto& diagnostic : result->diagnostics) {
    set_view(diagnostic.view.symbolic_code, diagnostic.code);
    set_view(diagnostic.view.message_key, diagnostic.message);
    set_view(diagnostic.view.safe_detail, diagnostic.detail);
    diagnostic.view.reserved0 = 0;
    diagnostic.view.reserved1 = 0;
    result->diagnostic_views.push_back(diagnostic.view);
  }
}

sb_engine_status_t fail_result(sb_engine_status_t status,
                               sb_engine_result_t* out_result,
                               std::uint32_t numeric_code,
                               std::string code,
                               std::string message,
                               std::string detail,
                               std::vector<std::pair<std::string, std::string>> fields) {
  if (out_result != nullptr) {
    auto* result = make_result(SB_ENGINE_RESULT_DIAGNOSTIC_ONLY);
    add_diagnostic(result,
                   numeric_code,
                   SB_ENGINE_DIAGNOSTIC_ERROR,
                   std::move(code),
                   std::move(message),
                   std::move(detail),
                   std::move(fields));
    finalize_diagnostics(result);
    *out_result = result;
  }
  return status;
}

sb_engine_status_t check_struct(std::uint32_t struct_size,
                                std::uint32_t abi_version,
                                std::uint32_t minimum_size,
                                sb_engine_result_t* out_result) {
  if (struct_size < minimum_size) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1001, "ENGINE.ABI.STRUCT_SIZE_INVALID",
                       "engine.abi.struct_size_invalid");
  }
  if (!valid_abi(abi_version)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1002, "ENGINE.ABI.VERSION_UNSUPPORTED",
                       "engine.abi.version_unsupported");
  }
  return SB_ENGINE_STATUS_OK;
}

void clear_result(sb_engine_result_t* out_result) {
  if (out_result != nullptr) {
    *out_result = nullptr;
  }
}

}  // namespace

extern "C" {

std::uint32_t sb_engine_abi_version_packed(void) {
  return SB_ENGINE_ABI_VERSION_PACKED;
}

sb_engine_status_t sb_engine_abi_build_id(const char** out_data, std::uint64_t* out_size) {
  if (out_data == nullptr || out_size == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  *out_data = kBuildId;
  *out_size = static_cast<std::uint64_t>(std::strlen(kBuildId));
  return SB_ENGINE_STATUS_OK;
}

const char* sb_engine_status_name(sb_engine_status_t status) {
  switch (status) {
    case SB_ENGINE_STATUS_OK: return "OK";
    case SB_ENGINE_STATUS_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
    case SB_ENGINE_STATUS_INVALID_HANDLE: return "INVALID_HANDLE";
    case SB_ENGINE_STATUS_UNSUPPORTED: return "UNSUPPORTED";
    case SB_ENGINE_STATUS_CAPABILITY_DISABLED: return "CAPABILITY_DISABLED";
    case SB_ENGINE_STATUS_SECURITY_DENIED: return "SECURITY_DENIED";
    case SB_ENGINE_STATUS_TRANSACTION_ACTIVE: return "TRANSACTION_ACTIVE";
    case SB_ENGINE_STATUS_TRANSACTION_REQUIRED: return "TRANSACTION_REQUIRED";
    case SB_ENGINE_STATUS_CONFLICT: return "CONFLICT";
    case SB_ENGINE_STATUS_NOT_FOUND: return "NOT_FOUND";
    case SB_ENGINE_STATUS_TIMEOUT: return "TIMEOUT";
    case SB_ENGINE_STATUS_RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
    case SB_ENGINE_STATUS_INTERNAL_ERROR: return "INTERNAL_ERROR";
    case SB_ENGINE_STATUS_ALREADY_RELEASED: return "ALREADY_RELEASED";
  }
  return "UNKNOWN";
}

sb_engine_status_t sb_engine_open(const sb_engine_open_params_v1_t* params,
                                  sb_engine_handle_t* out_engine,
                                  sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_engine == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1003, "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_engine = nullptr;
  if (params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_open_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  if (!valid_string_span(params->database_path_utf8, params->database_path_size)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1005, "ENGINE.OPEN.PATH_INVALID",
                       "engine.open.path_invalid");
  }
  if (params->mode < SB_ENGINE_OPEN_NORMAL || params->mode > SB_ENGINE_OPEN_VALIDATION_ONLY) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1006, "ENGINE.OPEN.MODE_INVALID",
                       "engine.open.mode_invalid");
  }
  auto* handle = new sb_engine_handle_s();
  if (params->database_path_utf8 != nullptr && params->database_path_size != 0) {
    handle->database_path.assign(params->database_path_utf8,
                                 params->database_path_utf8 + params->database_path_size);
    const auto snapshot = database_header_snapshot(handle->database_path);
    handle->database_uuid = snapshot.database_uuid;
    handle->database_page_size_bytes = snapshot.page_size_bytes;
  }
  *out_engine = handle;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_close(sb_engine_handle_t engine, sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  {
    std::lock_guard<std::mutex> guard(engine->mutex);
    engine->closed = true;
    engine->magic = 0;
  }
  delete engine;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_session_begin(sb_engine_handle_t engine,
                                           const sb_engine_session_params_v1_t* params,
                                           sb_engine_session_t* out_session,
                                           sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_session == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1003, "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_session = nullptr;
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_session_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  if (!nonzero_uuid(params->effective_user_uuid) || !nonzero_uuid(params->session_uuid)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 2001, "SECURITY.IDENTITY.MISSING",
                       "security.identity.missing");
  }
  if (!valid_string_span(params->default_language_utf8, params->default_language_size)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1008, "ENGINE.SESSION.LANGUAGE_INVALID",
                       "engine.session.language_invalid");
  }
  auto* session = new sb_engine_session_s();
  session->engine = engine;
  session->session_id = engine->next_session_id.fetch_add(1, std::memory_order_relaxed);
  session->package_resource_ledger = std::make_unique<
      scratchbird::core::agents::ResourceGovernanceReservationLedger>(
      "engine.session.package:" + std::to_string(session->session_id));
  auto& package_policy = session->package_resource_descriptor;
  package_policy.descriptor_id =
      "engine.runtime.sblr_package.query_memory_arena.v1";
  package_policy.family = scratchbird::core::agents::
      ResourceGovernanceFamily::kQueryMemoryArena;
  package_policy.source = scratchbird::core::agents::
      ResourceGovernanceDescriptorSource::kRuntimePolicy;
  package_policy.source_path_or_label =
      "engine.builtin.runtime_policy.sblr_package.v1";
  package_policy.descriptor_generation = 0;
  package_policy.expected_generation = 0;
  package_policy.over_limit_action = scratchbird::core::agents::
      ResourceGovernanceAction::kFailClosed;
  package_policy.benchmark_clean = true;
  package_policy.runtime_dependency_present = true;
  constexpr std::int64_t kPackageResourceDimensionLimit = 1'000'000;
  package_policy.limits.memory_bytes = 256 * 1024 * 1024;
  package_policy.limits.device_memory_bytes = 1;
  package_policy.limits.pinned_memory_bytes = 1;
  package_policy.limits.io_bytes = 64 * 1024 * 1024;
  package_policy.limits.io_ops = kPackageResourceDimensionLimit;
  package_policy.limits.worker_threads = 64;
  package_policy.limits.backlog_items = 4096;
  package_policy.limits.candidate_rows = kPackageResourceDimensionLimit;
  package_policy.limits.cache_entries = kPackageResourceDimensionLimit;
  package_policy.limits.batch_rows = kPackageResourceDimensionLimit;
  package_policy.limits.fragments = 262144;
  package_policy.limits.lanes = 64;
  package_policy.limits.time_budget_microseconds = 300'000'000;
  session->effective_user_uuid = params->effective_user_uuid;
  session->public_session_uuid = params->session_uuid;
  session->trust_mode = params->trust_mode;
  *out_session = session;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_session_end(sb_engine_session_t session,
                                         const sb_engine_session_end_params_v1_t* params,
                                         sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params != nullptr) {
    auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_session_end_params_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
  }
  {
    std::lock_guard<std::mutex> guard(session->mutex);
    if (session->active_transactions != 0 && (params == nullptr || params->rollback_active_transactions == 0)) {
      return fail_result(SB_ENGINE_STATUS_TRANSACTION_ACTIVE, out_result, 3001, "ENGINE.SESSION.TRANSACTION_ACTIVE",
                         "engine.session.transaction_active");
    }
    if (session->open_streams != 0 && (params == nullptr || params->cancel_open_results == 0)) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 3002, "ENGINE.RESULT.STREAM_ACTIVE",
                         "engine.result.stream_active");
    }
    release_statement_context_receipts_for_session(session);
    release_prepared_metadata_bindings_for_session(session);
    if (params != nullptr && params->rollback_active_transactions != 0) {
      for (auto* transaction : session->published_transactions) {
        if (transaction != nullptr) {
          transaction->closed = true;
          transaction->magic = 0;
          delete transaction;
        }
      }
      session->published_transactions.clear();
      session->active_transactions = 0;
    }
    session->closed = true;
    session->magic = 0;
  }
  delete session;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_transaction_begin(sb_engine_session_t session,
                                               const sb_engine_transaction_params_v1_t* params,
                                               sb_engine_transaction_t* out_transaction,
                                               sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_transaction == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1003, "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_transaction = nullptr;
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_transaction_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  auto* transaction = new sb_engine_transaction_s();
  transaction->session = session;
  {
    std::lock_guard<std::mutex> guard(session->mutex);
    ++session->active_transactions;
  }
  *out_transaction = transaction;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_transaction_commit(sb_engine_transaction_t transaction,
                                                const sb_engine_transaction_finish_params_v1_t* params,
                                                sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!valid_transaction(transaction)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (params != nullptr) {
    auto status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_transaction_finish_params_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
  }
  auto* session = transaction->session;
  {
    std::lock_guard<std::mutex> session_guard(session->mutex);
    if (session->active_transactions > 0) {
      --session->active_transactions;
    }
    session->published_transactions.erase(
        std::remove(session->published_transactions.begin(),
                    session->published_transactions.end(), transaction),
        session->published_transactions.end());
  }
  transaction->closed = true;
  transaction->magic = 0;
  delete transaction;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_transaction_rollback(sb_engine_transaction_t transaction,
                                                  const sb_engine_transaction_finish_params_v1_t* params,
                                                  sb_engine_result_t* out_result) {
  return sb_engine_transaction_commit(transaction, params, out_result);
}

}  // extern "C"

namespace scratchbird::server_engine_bridge {

sb_engine_status_t BeginStatementParameterExecutionCoordinationV1(
    const StatementParameterCoordinationBeginRequestV1* request,
    StatementParameterCoordinationViewV1* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (request == nullptr || out_view == nullptr ||
      request->engine_session == nullptr || request->engine_context == nullptr ||
      request->mode != StatementParameterExecutionMode::kPrepared ||
      request->operation_uuid.empty() ||
      !request->public_dynamic_package_uuid.empty()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4077,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_coordination.begin_invalid");
  }
  const auto operation = scratchbird::core::uuid::ParseUuid(
      request->operation_uuid);
  const auto& context = *request->engine_context;
  if (!operation.ok() || context.database_uuid.canonical.empty() ||
      context.session_uuid.canonical.empty() ||
      context.transaction_uuid.canonical.empty()) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4077,
                       "SECURITY.ACCESS_DENIED",
                       "sblr.parameter_coordination.ownership_invalid");
  }
  auto registry_context = context;
  registry_context.statement_metadata_snapshot_engine_owned = true;
  registry_context.trace_tags.push_back("private_prepared_coordination");
  const auto issued = request->public_prepared_uuid.empty()
      ? scratchbird::engine::internal_api::BeginSblrPreparedCoordination(
            registry_context, request->operation_uuid)
      : scratchbird::engine::internal_api::
            BeginSblrPreparedExecutionCoordination(
                registry_context, request->operation_uuid,
                request->public_prepared_uuid);
  if (!issued.ok) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4077,
                       issued.diagnostic.code, issued.diagnostic.message_key,
                       issued.diagnostic.detail);
  }
  std::lock_guard<std::mutex> guard(g_statement_context_receipt_registry_mutex);
  const auto handle = issued.snapshot.private_handle;
  if (handle == 0 || g_parameter_coordinations.contains(handle)) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4077,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_coordination.handle_unavailable");
  }
  StatementParameterCoordinationOpaque coordination;
  coordination.private_handle = handle;
  coordination.session = request->engine_session;
  coordination.database_uuid = context.database_uuid.canonical;
  coordination.session_uuid = context.session_uuid.canonical;
  coordination.transaction_uuid = context.transaction_uuid.canonical;
  coordination.mode = request->mode;
  coordination.public_coordination_uuid = issued.snapshot.coordination_uuid;
  coordination.operation_uuid = request->operation_uuid;
  coordination.generation = issued.snapshot.coordinator_generation;
  coordination.prepared_statement_uuid =
      issued.snapshot.provisional_prepared_uuid;
  coordination.prepared_generation =
      issued.snapshot.provisional_prepared_generation;
  if (!g_parameter_coordinations.emplace(handle, coordination).second) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4077,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_coordination.handle_collision");
  }
  out_view->private_handle = handle;
  out_view->public_coordination_uuid = coordination.public_coordination_uuid;
  out_view->operation_uuid = request->operation_uuid;
  out_view->coordinator_generation = coordination.generation;
  out_view->prepared_statement_uuid = coordination.prepared_statement_uuid;
  out_view->prepared_generation = coordination.prepared_generation;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t SealPreparedStatementParameterTemplateV1(
    std::uint64_t private_handle,
    const std::vector<std::uint8_t>& canonical_sbpt,
    StatementParameterCoordinationViewV1* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (private_handle == 0 || out_view == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4078,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_coordination.seal_invalid");
  }
  const auto decoded = scratchbird::engine::sblr::
      DecodeSblrPreparedParameterTemplateV1(canonical_sbpt.data(),
                                            canonical_sbpt.size());
  if (!decoded.ok) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4078,
                       decoded.diagnostic_id,
                       "sblr.parameter_template.decode_invalid",
                       decoded.detail);
  }
  const auto uuid_text=[](const auto& bytes){
    scratchbird::core::platform::Uuid u{};
    std::copy(bytes.begin(),bytes.end(),u.bytes.begin());
    return scratchbird::core::uuid::UuidToString(u);
  };
  StatementParameterCoordinationOpaque coordination;
  {
    std::lock_guard<std::mutex> guard(g_statement_context_receipt_registry_mutex);
    const auto found = g_parameter_coordinations.find(private_handle);
    if (found == g_parameter_coordinations.end()) {
      return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4078,
                         "SECURITY.ACCESS_DENIED",
                         "sblr.parameter_coordination.hidden");
    }
    coordination = found->second;
  }
  if (coordination.public_coordination_uuid !=
          uuid_text(decoded.value.public_coordination_uuid) ||
      coordination.operation_uuid != uuid_text(decoded.value.operation_uuid) ||
      coordination.prepared_statement_uuid !=
          uuid_text(decoded.value.provisional_prepared_uuid) ||
      coordination.prepared_generation !=
          decoded.value.provisional_prepared_generation ||
      coordination.terminal || coordination.sealed ||
      !coordination.context_acquired) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4078,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_coordination.seal_stale");
  }
  const auto& schema=decoded.value.canonical_schema4015;
  if(schema.size()<98)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.schema4015_truncated");
  std::size_t offset=0;offset+=16;
  if(!std::all_of(schema.begin()+16,schema.begin()+32,[](auto b){return b==0;})||schema[32]!=0||schema[33]!=1)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.schema4015_selector_invalid");
  offset=74;
  const auto read_blob=[&](std::vector<std::uint8_t>* out){
    if(offset>schema.size()||schema.size()-offset<8)return false;
    const auto n=scratchbird::engine::SblrReadU64(schema.data()+offset);offset+=8;
    if(n>schema.size()-offset)return false;
    out->assign(schema.begin()+static_cast<std::ptrdiff_t>(offset),
                schema.begin()+static_cast<std::ptrdiff_t>(offset+n));offset+=n;return true;};
  std::vector<std::uint8_t> container_bytes,execution_bytes,data_bytes;
  if(!read_blob(&container_bytes)||!read_blob(&execution_bytes)||!read_blob(&data_bytes)||
     offset!=schema.size()||container_bytes.empty()||execution_bytes.empty()||!data_bytes.empty())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.schema4015_extent_invalid");
  const auto container=scratchbird::engine::DecodeSblrContainerBytes(container_bytes.data(),container_bytes.size());
  const auto ingress=scratchbird::engine::DecodeSblrExecutionEnvelopeV1Bytes(execution_bytes.data(),execution_bytes.size());
  scratchbird::engine::SblrExecutionEnvelopeSemanticView ingress_view;
  if(container.status!=scratchbird::engine::SblrCodecStatus::ok||
     ingress.status!=scratchbird::engine::SblrCodecStatus::ok||
     !scratchbird::engine::SblrValidateExecutionEnvelopeFields(ingress.envelope,&ingress_view)||
     container.container.operation_payload.empty())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.canonical_envelope_invalid");
  const std::string_view operation_bytes(
      reinterpret_cast<const char*>(container.container.operation_payload.data()),
      container.container.operation_payload.size());
  auto stream=scratchbird::engine::sblr::DecodeSblrOpcodeStream(operation_bytes);
  if(!stream.ok||stream.stream.operations.empty())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbos_invalid",
      stream.detail);
  const std::vector<std::uint8_t>* sbpn=nullptr;
  for(const auto& operation:stream.stream.operations)for(const auto& operand:operation.operands){
    if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::parameter_node_table){
      if(sbpn!=nullptr)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
        "SBLR.OPERAND_INVALID","sblr.parameter_template.multiple_sbpn");sbpn=&operand.value_body;}
  }
  if(sbpn==nullptr)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbpn_missing");
  static constexpr std::string_view sbpn_domain="ScratchBird.SblrParameterNodeTable.V1";
  std::vector<std::uint8_t> sbpn_material(sbpn_domain.begin(),sbpn_domain.end());
  sbpn_material.insert(sbpn_material.end(),sbpn->begin(),sbpn->end());
  const auto sbpn_sha=scratchbird::core::hash::ComputeSha256Digest(sbpn_material);
  if(!sbpn_sha.ok()||sbpn_sha.digest!=decoded.value.sbpn_sha256)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbpn_hash_invalid");
  scratchbird::engine::sblr::SblrParameterAdmissionV1 admission;std::string detail;
  if(!scratchbird::engine::sblr::DecodeSblrParameterAdmissionV1(
       decoded.value.canonical_sbpa.data(),decoded.value.canonical_sbpa.size(),&admission,&detail)||
     uuid_text(admission.parameter_set_descriptor_uuid)!=uuid_text(decoded.value.parameter_set_descriptor_uuid)||
     admission.descriptor_generation!=decoded.value.descriptor_generation||
     uuid_text(admission.execution_uuid)!=coordination.operation_uuid||
     uuid_text(admission.prepared_uuid)!=coordination.prepared_statement_uuid||
     admission.prepared_generation!=coordination.prepared_generation)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,
      "SBLR.OPERAND_INVALID","sblr.parameter_template.sbpa_invalid",detail);
  std::unique_lock<std::mutex> live_registry_guard(
      g_statement_context_receipt_registry_mutex);
  StatementContextReceiptOpaque* receipt=nullptr;
  for(auto& [id,candidate]:g_live_statement_context_receipts){
    if(candidate->parameter_final_receipt_uuid==uuid_text(admission.final_receipt_uuid)&&
       candidate->parameter_admission_token_uuid==uuid_text(admission.admission_token_uuid)){
      if(receipt!=nullptr)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
        "SBLR.PARAMETER.STALE","sblr.parameter_template.receipt_ambiguous");receipt=candidate.get();}}
  if(receipt==nullptr)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      "SBLR.PARAMETER.STALE","sblr.parameter_template.receipt_stale");
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  auto parameter_identity=scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity{};
  parameter_identity.executor_id=scratchbird::engine::internal_api::kSblrParameterExecutorId;
  parameter_identity.opcode_code=scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
  parameter_identity.opcode_version=scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
  parameter_identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
  parameter_identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
  parameter_identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current_executor;
  const auto executor=scratchbird::engine::internal_api::RevalidateSblrExecutorAvailability(
      receipt->engine_context,parameter_identity,receipt->parameter_executor_availability_snapshot,&current_executor);
  if(executor.error||decoded.value.executor_availability_generation!=
       receipt->view.parameter_executor_availability_generation||
     current_executor.generation!=decoded.value.executor_availability_generation||
     receipt->released||!receipt->parameter_binding_finalized||receipt->parameter_admission_consumed||
     receipt->parameter_sbpn_sha256!=decoded.value.sbpn_sha256||
     receipt->parameter_set_snapshot.catalog_generation!=decoded.value.catalog_generation||
     receipt->parameter_set_snapshot.security_epoch!=decoded.value.security_epoch||
     receipt->parameter_set_snapshot.resource_epoch!=decoded.value.resource_epoch||
     receipt->view.statement_snapshot_uuid!=uuid_text(decoded.value.mga_snapshot_uuid))
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      executor.error?executor.code:"SBLR.PARAMETER.STALE",
      "sblr.parameter_template.live_binding_stale",executor.detail);
  const auto evidence_hex=scratchbird::core::hash::HexLower(
      decoded.value.prepared_template_binding_sha256);
  auto registry_context=receipt->engine_context;
  registry_context.statement_metadata_snapshot_engine_owned=true;
  registry_context.trace_tags.push_back("private_prepared_coordination");
  const auto expected_coordination_generation=coordination.generation;
  const auto sealed=scratchbird::engine::internal_api::SealSblrPreparedCoordination(
      registry_context,coordination.public_coordination_uuid,coordination.operation_uuid,
      coordination.generation,coordination.prepared_statement_uuid,
      coordination.prepared_generation,"sha256:"+evidence_hex);
  if(!sealed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      sealed.diagnostic.code,sealed.diagnostic.message_key,sealed.diagnostic.detail);
  receipt->parameter_admission_consumed=true;
  coordination.sealed=true;coordination.generation=sealed.snapshot.coordinator_generation;
  const auto found=g_parameter_coordinations.find(private_handle);
  if(found==g_parameter_coordinations.end()||found->second.sealed||
     found->second.generation!=expected_coordination_generation)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4078,
      "SBLR.PARAMETER.STALE","sblr.parameter_coordination.publish_race");
  found->second=coordination;
  out_view->private_handle = private_handle;
  out_view->public_coordination_uuid = coordination.public_coordination_uuid;
  out_view->operation_uuid = coordination.operation_uuid;
  out_view->coordinator_generation = coordination.generation;
  out_view->prepared_statement_uuid = coordination.prepared_statement_uuid;
  out_view->prepared_generation = coordination.prepared_generation;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t AcquireStatementContextReceipt(
    sb_engine_session_t session,
    const StatementContextAcquireRequest* request,
    StatementContextReceiptHandle* out_receipt,
    StatementContextReceiptView* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_receipt == nullptr || out_view == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4033,
        "ENGINE.STATEMENT_CONTEXT.OUTPUT_REQUIRED",
        "engine.statement_context.output_required");
  }
  *out_receipt = {};
  *out_view = {};
  if (!valid_session(session)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_HANDLE,
        out_result,
        4034,
        "ENGINE.STATEMENT_CONTEXT.SESSION_INVALID",
        "engine.statement_context.session_invalid");
  }
  if (request == nullptr || request->engine_context == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4035,
        "ENGINE.STATEMENT_CONTEXT.REQUEST_REQUIRED",
        "engine.statement_context.request_required");
  }

  auto engine_context = *request->engine_context;
  const auto expected_trust_mode =
      session->trust_mode == SB_ENGINE_TRUST_EMBEDDED_TRUSTED
          ? scratchbird::engine::internal_api::EngineTrustMode::embedded_in_process
          : scratchbird::engine::internal_api::EngineTrustMode::server_isolated;
  if (engine_context.database_path.empty() ||
      engine_context.database_path != session->engine->database_path ||
      engine_context.database_uuid.canonical !=
          session->engine->database_uuid ||
      engine_context.principal_uuid.canonical !=
          uuid_to_canonical(session->effective_user_uuid) ||
      engine_context.session_uuid.canonical !=
          uuid_to_canonical(session->public_session_uuid) ||
      engine_context.trust_mode != expected_trust_mode) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4036,
        "ENGINE.STATEMENT_CONTEXT.SESSION_CONTEXT_MISMATCH",
        "engine.statement_context.session_context_mismatch");
  }
  if (!engine_context.statement_uuid.canonical.empty() ||
      !engine_context.statement_snapshot_uuid.canonical.empty() ||
      engine_context.statement_metadata_snapshot_engine_owned ||
      !engine_context.statement_metadata_snapshot_uuid.canonical.empty() ||
      !engine_context.catalog_epoch_uuid.canonical.empty() ||
      !engine_context.optimizer_capability_snapshot_uuid.canonical.empty() ||
      !engine_context.optimizer_resource_snapshot_uuid.canonical.empty() ||
      !engine_context.optimizer_route_snapshot_uuid.canonical.empty()) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4037,
        "ENGINE.STATEMENT_CONTEXT.CALLER_AUTHORITY_FORBIDDEN",
        "engine.statement_context.caller_authority_forbidden");
  }
  if (engine_context.local_transaction_id == 0 ||
      request->exact_transaction_uuid.empty() ||
      engine_context.transaction_uuid.canonical !=
          request->exact_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        4038,
        "ENGINE.STATEMENT_CONTEXT.EXACT_TRANSACTION_REQUIRED",
        "engine.statement_context.exact_transaction_required");
  }
  const auto parsed_transaction =
      scratchbird::core::uuid::ParseTypedUuid(
          scratchbird::core::platform::UuidKind::transaction,
          std::string(request->exact_transaction_uuid));
  if (!parsed_transaction.ok() ||
      scratchbird::core::uuid::UuidToString(
          parsed_transaction.value.value) != request->exact_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4039,
        "ENGINE.STATEMENT_CONTEXT.TRANSACTION_UUID_INVALID",
        "engine.statement_context.transaction_uuid_invalid");
  }
  std::optional<StatementParameterCoordinationOpaque> parameter_coordination;
  const auto& selector = request->parameter_execution_selector;
  const bool direct_selector =
      selector.version == 1 && selector.reserved == 0 &&
      selector.mode == StatementParameterExecutionMode::kDirect &&
      selector.prepared_binding_handle == 0 &&
      selector.batch_execution_handle == 0 &&
      selector.dynamic_package_handle == 0;
  if (!direct_selector) {
    if (selector.version != 1 || selector.reserved != 0 ||
        selector.mode != StatementParameterExecutionMode::kPrepared ||
        selector.prepared_binding_handle == 0 ||
        selector.batch_execution_handle != 0 ||
        selector.dynamic_package_handle != 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4039,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_execution_selector.invalid");
    }
    StatementParameterCoordinationOpaque coordination;
    {
      std::lock_guard<std::mutex> guard(
          g_statement_context_receipt_registry_mutex);
      const auto found = g_parameter_coordinations.find(
          selector.prepared_binding_handle);
      if (found == g_parameter_coordinations.end() ||
          found->second.session != session ||
          found->second.database_uuid != engine_context.database_uuid.canonical ||
          found->second.session_uuid != engine_context.session_uuid.canonical) {
        return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4039,
                           "SECURITY.ACCESS_DENIED",
                           "sblr.parameter_execution_selector.hidden");
      }
      coordination = found->second;
    }
    if (coordination.transaction_uuid != request->exact_transaction_uuid ||
        coordination.terminal || coordination.sealed ||
        coordination.context_acquired) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4039,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_execution_selector.stale");
    }
    const auto expected_coordination_generation = coordination.generation;
    auto registry_context = engine_context;
    registry_context.statement_metadata_snapshot_engine_owned = true;
    registry_context.trace_tags.push_back("private_prepared_coordination");
    const auto acquired = scratchbird::engine::internal_api::
        AcquireSblrPreparedCoordination(
            registry_context, coordination.public_coordination_uuid,
            coordination.operation_uuid, coordination.generation);
    if (!acquired.ok) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4039,
                         acquired.diagnostic.code,
                         acquired.diagnostic.message_key,
                         acquired.diagnostic.detail);
    }
    coordination.context_acquired = true;
    coordination.generation = acquired.snapshot.coordinator_generation;
    {
      std::lock_guard<std::mutex> guard(
          g_statement_context_receipt_registry_mutex);
      const auto found = g_parameter_coordinations.find(
          selector.prepared_binding_handle);
      if (found == g_parameter_coordinations.end() ||
          found->second.generation != expected_coordination_generation ||
          found->second.context_acquired) {
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4039,
                           "SBLR.PARAMETER.STALE",
                           "sblr.parameter_execution_selector.publish_race");
      }
      found->second = coordination;
    }
    parameter_coordination = coordination;
  }
  std::optional<scratchbird::engine::internal_api::SblrVariableFrameSnapshot>
      variable_frame;
  const auto& variable_selector = request->variable_frame_selector;
  if (variable_selector.version != 0) {
    if (variable_selector.version != 1 ||
        !canonical_non_nil_uuid_text(variable_selector.public_coordination_uuid) ||
        !canonical_non_nil_uuid_text(variable_selector.operation_uuid) ||
        variable_selector.expected_coordinator_generation == 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4039,
                         "SBLR.OPERAND_INVALID",
                         "sblr.variable_frame_selector.invalid");
    }
    auto variable_context = engine_context;
    variable_context.statement_metadata_snapshot_engine_owned = true;
    variable_context.trace_tags.push_back("private_variable_frame_coordination");
    const auto acquired = scratchbird::engine::internal_api::
        AcquireSblrVariableFrame(
            variable_context, variable_selector.public_coordination_uuid,
            variable_selector.operation_uuid,
            variable_selector.expected_coordinator_generation);
    if (!acquired.ok) {
      return fail_result(
          acquired.diagnostic.code == "SECURITY.ACCESS_DENIED"
              ? SB_ENGINE_STATUS_SECURITY_DENIED : SB_ENGINE_STATUS_CONFLICT,
          out_result, 4039, acquired.diagnostic.code,
          acquired.diagnostic.message_key, acquired.diagnostic.detail);
    }
    variable_frame = acquired.snapshot;
  }
  if (!engine_context.security_context_present ||
      !engine_context.authorization_context.present ||
      !canonical_non_nil_uuid_text(
          engine_context.authorization_context.authority_uuid.canonical) ||
      engine_context.authorization_context.principal_uuid.canonical !=
          engine_context.principal_uuid.canonical ||
      engine_context.catalog_generation_id == 0 ||
      engine_context.security_epoch == 0 ||
      engine_context.resource_epoch == 0 ||
      engine_context.authorization_context.security_epoch !=
          engine_context.security_epoch ||
      engine_context.authorization_context.policy_epoch == 0 ||
      engine_context.authorization_context.catalog_generation_id !=
          engine_context.catalog_generation_id) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete");
  }

  // A fully materialized server policy may narrow these limits. When it does
  // not, issue the canonical bounded defaults here so neither the parser nor
  // SBLR becomes optimizer/resource authority merely to acquire a statement.
  if (engine_context.optimizer_route_epoch == 0) {
    engine_context.optimizer_route_epoch = engine_context.resource_epoch;
  }
  if (engine_context.optimizer_route_generation == 0) {
    engine_context.optimizer_route_generation =
        engine_context.catalog_generation_id;
  }
  if (engine_context.optimizer_memory_budget_bytes == 0) {
    engine_context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  }
  if (engine_context.optimizer_maximum_candidate_count == 0) {
    engine_context.optimizer_maximum_candidate_count = 131072;
  }
  if (engine_context.optimizer_maximum_memo_groups == 0) {
    engine_context.optimizer_maximum_memo_groups = 131072;
  }
  if (engine_context.optimizer_maximum_search_steps == 0) {
    engine_context.optimizer_maximum_search_steps = 1048576;
  }
  if (engine_context.optimizer_maximum_planning_time_ns == 0) {
    engine_context.optimizer_maximum_planning_time_ns = 5'000'000'000ull;
  }

  const auto inventory_guard =
      scratchbird::engine::internal_api::AcquireTransactionInventoryGuard(
          engine_context.database_path);
  const auto loaded =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          engine_context.database_path);
  if (!loaded.ok()) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4041,
        "ENGINE.STATEMENT_CONTEXT.INVENTORY_UNAVAILABLE",
        "engine.statement_context.inventory_unavailable",
        loaded.diagnostic.diagnostic_code);
  }
  const auto exact_transaction =
      scratchbird::transaction::mga::LookupLocalTransaction(
          loaded.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              engine_context.local_transaction_id));
  if (!exact_transaction.ok() ||
      !statement_context_transaction_active(exact_transaction.entry.state) ||
      !exact_transaction.entry.identity.transaction_uuid.valid() ||
      scratchbird::core::uuid::UuidToString(
          exact_transaction.entry.identity.transaction_uuid.value) !=
          request->exact_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        4042,
        "ENGINE.STATEMENT_CONTEXT.TRANSACTION_NOT_ACTIVE",
        "engine.statement_context.transaction_not_active",
        std::to_string(engine_context.local_transaction_id));
  }

  std::unordered_set<std::string> distinct_identities;
  for (const auto& identity : {
           engine_context.database_uuid.canonical,
           engine_context.principal_uuid.canonical,
           engine_context.session_uuid.canonical,
           engine_context.transaction_uuid.canonical,
           engine_context.authorization_context.authority_uuid.canonical}) {
    if (!identity.empty()) distinct_identities.insert(identity);
  }

  std::string statement_uuid;
  if (!generate_distinct_statement_context_uuid(
          &distinct_identities, &statement_uuid)) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_uuid");
  }
  engine_context.request_id = "private-statement-context-acquire";
  engine_context.statement_uuid.canonical = statement_uuid;
  engine_context.statement_snapshot_uuid.canonical.clear();
  // QOW-SOURCE-RCP-075-ENGINE-STATEMENT-TIMESTAMP-ACQUISITION-V1
  // Receipt acquisition is the sole production clock boundary for the
  // statement timestamp. Caller/server materialization is deliberately
  // overwritten so no earlier layer can become TTL time authority.
  engine_context.statement_timestamp = current_utc_timestamp_text();
  engine_context.current_timestamp = engine_context.statement_timestamp;
  if (!canonical_statement_timestamp(engine_context.statement_timestamp)) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_timestamp_invalid");
  }
  if (engine_context.current_monotonic_ns.empty()) {
    engine_context.current_monotonic_ns = current_monotonic_ns_text();
  }

  scratchbird::engine::internal_api::EnginePublishStatementSnapshotRequest
      publish_request;
  publish_request.context = engine_context;
  const auto published =
      scratchbird::engine::internal_api::EnginePublishStatementSnapshot(
          publish_request);
  if (!published.ok) {
    const auto* diagnostic = published.diagnostics.empty()
                                 ? nullptr
                                 : &published.diagnostics.front();
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4045,
        diagnostic == nullptr
            ? "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_PUBLISH_FAILED"
            : diagnostic->code,
        diagnostic == nullptr
            ? "engine.statement_context.snapshot_publish_failed"
            : diagnostic->message_key,
        diagnostic == nullptr ? std::string{} : diagnostic->detail);
  }
  const auto& snapshot = published.snapshot_vector;
  const std::string statement_snapshot_uuid =
      published.statement_snapshot_uuid.canonical;
  if (!snapshot.complete || !snapshot.inventory_authoritative ||
      snapshot.snapshot_kind != scratchbird::transaction::mga::
                                    SnapshotVectorKind::statement_stable ||
      snapshot.owning_transaction.value !=
          engine_context.local_transaction_id ||
      scratchbird::core::uuid::UuidToString(
          snapshot.owning_transaction_uuid.value) !=
          request->exact_transaction_uuid ||
      published.statement_uuid.canonical != statement_uuid ||
      !distinct_identities.insert(statement_snapshot_uuid).second) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4046,
        "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_AUTHORITY_MISMATCH",
        "engine.statement_context.snapshot_authority_mismatch");
  }

  StatementContextReceiptView view;
  const auto issue_identity = [&](std::string* value) {
    return generate_distinct_statement_context_uuid(
        &distinct_identities, value);
  };
  if (!issue_identity(&view.receipt_uuid) ||
      !issue_identity(&view.statement_metadata_snapshot_uuid) ||
      !issue_identity(&view.catalog_epoch_uuid) ||
      !issue_identity(&view.optimizer_capability_snapshot_uuid) ||
      !issue_identity(&view.optimizer_resource_snapshot_uuid) ||
      !issue_identity(&view.optimizer_route_snapshot_uuid) ||
      !issue_identity(&view.bound_ast_uuid)) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "receipt_context_identity");
  }
  // Canonical aggregate function identity is catalog authority, not a
  // per-statement handle.  Publish the exact engine-owned registry rows so
  // repeated projected/HAVING references bind the same immutable functions.
  const auto aggregate_registry_errors =
      scratchbird::engine::executor::
          ValidateCanonicalAggregateRuntimeRegistryV1();
  const auto& aggregate_registry =
      scratchbird::engine::executor::CanonicalAggregateRuntimeRegistryV1();
  const auto* count_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::count);
  const auto* sum_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::sum);
  const auto* avg_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::avg);
  const auto* min_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::min);
  const auto* max_registry_entry =
      scratchbird::engine::executor::LookupCanonicalAggregateByFunctionV1(
          scratchbird::engine::executor::CanonicalAggregateFunction::max);
  if (!aggregate_registry_errors.empty() || aggregate_registry.empty() ||
      aggregate_registry.size() > 192 || count_registry_entry == nullptr ||
      sum_registry_entry == nullptr ||
      avg_registry_entry == nullptr || min_registry_entry == nullptr ||
      max_registry_entry == nullptr || !count_registry_entry->executable ||
      !sum_registry_entry->executable || !avg_registry_entry->executable ||
      !min_registry_entry->executable || !max_registry_entry->executable) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.AGGREGATE_REGISTRY_UNAVAILABLE",
        "engine.statement_context.aggregate_registry_unavailable");
  }
  view.count_function_uuid = count_registry_entry->function_uuid;
  view.sum_function_uuid = sum_registry_entry->function_uuid;
  view.avg_function_uuid = avg_registry_entry->function_uuid;
  view.min_function_uuid = min_registry_entry->function_uuid;
  view.max_function_uuid = max_registry_entry->function_uuid;
  view.aggregate_function_profiles.reserve(aggregate_registry.size());
  for (const auto& entry : aggregate_registry) {
    if (entry.abi_version != 1 || entry.builtin_id.empty() ||
        entry.function_uuid.empty() || !entry.executable) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(
          SB_ENGINE_STATUS_INTERNAL_ERROR,
          out_result,
          4044,
          "ENGINE.STATEMENT_CONTEXT.AGGREGATE_REGISTRY_UNAVAILABLE",
          "engine.statement_context.aggregate_registry_unavailable");
    }
    view.aggregate_function_profiles.push_back(
        {entry.abi_version, entry.builtin_id, entry.function_uuid,
         entry.executable});
  }

  // QOW-SOURCE-WIN-001-STATEMENT-CONTEXT-REGISTRY-V1: native window
  // identities remain engine-owned. The parser receives an exact bounded
  // projection and cannot invent, rename, or substitute a function UUID.
  const auto window_registry =
      scratchbird::engine::executor::CanonicalWindowRuntimeRegistryV1();
  if (window_registry.size() != 11) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.WINDOW_REGISTRY_UNAVAILABLE",
        "engine.statement_context.window_registry_unavailable");
  }
  view.window_function_profiles.reserve(window_registry.size());
  std::unordered_set<std::string> window_builtin_ids;
  std::unordered_set<std::string> window_function_uuids;
  for (const auto& entry : window_registry) {
    if (entry.abi_version != 1 ||
        entry.function == scratchbird::engine::executor::
                              CanonicalWindowRuntimeFunction::unknown ||
        !entry.builtin_id.starts_with("sb.window.") ||
        entry.function_uuid.empty() || entry.aggregate_function.has_value() ||
        !window_builtin_ids.insert(entry.builtin_id).second ||
        !window_function_uuids.insert(entry.function_uuid).second) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(
          SB_ENGINE_STATUS_INTERNAL_ERROR,
          out_result,
          4044,
          "ENGINE.STATEMENT_CONTEXT.WINDOW_REGISTRY_UNAVAILABLE",
          "engine.statement_context.window_registry_unavailable");
    }
    view.window_function_profiles.push_back(
        {entry.abi_version, entry.builtin_id, entry.function_uuid, true});
  }

  // Numeric and Boolean statement descriptors represent canonical core scalar
  // profiles. Their descriptor UUIDs remain statement-owned, while their type
  // identities are owned by the core datatype catalog.
  const auto core_manifest =
      scratchbird::core::datatypes::LoadCurrentCoreDatatypeCatalogManifest();
  const auto int64_count =
      core_manifest.ok()
          ? std::ranges::count_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "int64"; })
          : 0;
  const auto boolean_count =
      core_manifest.ok()
          ? std::ranges::count_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "boolean"; })
          : 0;
  const auto int64_row =
      core_manifest.ok()
          ? std::ranges::find_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "int64"; })
          : core_manifest.manifest.descriptor_rows.end();
  const auto boolean_row =
      core_manifest.ok()
          ? std::ranges::find_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "boolean"; })
          : core_manifest.manifest.descriptor_rows.end();
  if (!core_manifest.ok() || int64_count != 1 || boolean_count != 1 ||
      int64_row == core_manifest.manifest.descriptor_rows.end() ||
      boolean_row == core_manifest.manifest.descriptor_rows.end() ||
      !int64_row->descriptor_uuid.valid() ||
      !boolean_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_numeric_descriptor_type");
  }
  const auto int64_descriptor_uuid = scratchbird::core::uuid::UuidToString(
      int64_row->descriptor_uuid.value);
  const auto int64_identity =
      scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
          "019d0000-0000-7000-8000-00000000d701",
          core_manifest.manifest.catalog_epoch, 1, int64_descriptor_uuid,
          int64_row->descriptor_epoch);
  const auto numeric_type_uuid =
      int64_identity.ok ? int64_identity.row.type_uuid : std::string{};
  const auto boolean_type_uuid = scratchbird::core::uuid::UuidToString(
      boolean_row->descriptor_uuid.value);
  std::string text_type_uuid;
  std::string json_type_uuid;
  std::string text_list_type_uuid;
  if (numeric_type_uuid.empty() || boolean_type_uuid.empty() ||
      numeric_type_uuid == boolean_type_uuid ||
      !issue_identity(&text_type_uuid) ||
      !issue_identity(&json_type_uuid) ||
      !issue_identity(&text_list_type_uuid)) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_descriptor_type_identity");
  }
  constexpr std::uint16_t kDescriptorSlotsPerProfile = 32;
  for (const auto kind : {
           StatementDescriptorProfileKind::kNumericNonNull,
           StatementDescriptorProfileKind::kNumericNullable,
           StatementDescriptorProfileKind::kTextNonNull,
           StatementDescriptorProfileKind::kTextNullable,
           StatementDescriptorProfileKind::kBooleanNonNull,
           StatementDescriptorProfileKind::kBooleanNullable,
           StatementDescriptorProfileKind::kJsonNonNull,
           StatementDescriptorProfileKind::kJsonNullable,
           StatementDescriptorProfileKind::kTextListNonNull,
           StatementDescriptorProfileKind::kTextListNullable}) {
    const bool numeric =
        kind == StatementDescriptorProfileKind::kNumericNonNull ||
        kind == StatementDescriptorProfileKind::kNumericNullable;
    const bool text =
        kind == StatementDescriptorProfileKind::kTextNonNull ||
        kind == StatementDescriptorProfileKind::kTextNullable;
    const bool json =
        kind == StatementDescriptorProfileKind::kJsonNonNull ||
        kind == StatementDescriptorProfileKind::kJsonNullable;
    const bool text_list =
        kind == StatementDescriptorProfileKind::kTextListNonNull ||
        kind == StatementDescriptorProfileKind::kTextListNullable;
    const bool nullable =
        kind == StatementDescriptorProfileKind::kNumericNullable ||
        kind == StatementDescriptorProfileKind::kTextNullable ||
        kind == StatementDescriptorProfileKind::kBooleanNullable ||
        kind == StatementDescriptorProfileKind::kJsonNullable ||
        kind == StatementDescriptorProfileKind::kTextListNullable;
    for (std::uint16_t slot = 0;
         slot < kDescriptorSlotsPerProfile;
         ++slot) {
      StatementDescriptorProfile profile;
      profile.profile_kind = kind;
      profile.slot = slot;
      profile.type_uuid =
          numeric ? numeric_type_uuid
                  : (text ? text_type_uuid
                          : (json ? json_type_uuid
                                  : (text_list ? text_list_type_uuid
                                               : boolean_type_uuid)));
      profile.nullable = nullable;
      if (!issue_identity(&profile.descriptor_uuid)) {
        scratchbird::transaction::mga::RevokePublishedSnapshotVector(
            snapshot.snapshot_uuid);
        return fail_result(
            SB_ENGINE_STATUS_INTERNAL_ERROR,
            out_result,
            4044,
            "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
            "engine.statement_context.identity_unavailable",
            "statement_descriptor_profile_identity");
      }
      view.descriptor_profiles.push_back(std::move(profile));
    }
  }

  // QOW-SOURCE-RCP-077-STATEMENT-REAL64-DESCRIPTORS-V8: the core catalog
  // owns the REAL64 type identity; the statement-context issuer owns the two
  // distinct result-slot descriptor identities. No parser-side pairing or
  // descriptor reuse is permitted.
  const auto real64_count =
      core_manifest.ok()
          ? std::ranges::count_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "real64"; })
          : 0;
  const auto real64_row =
      core_manifest.ok()
          ? std::ranges::find_if(
                core_manifest.manifest.descriptor_rows,
                [](const auto& row) { return row.stable_name == "real64"; })
          : core_manifest.manifest.descriptor_rows.end();
  if (!core_manifest.ok() || real64_count != 1 ||
      real64_row == core_manifest.manifest.descriptor_rows.end() ||
      !real64_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.REAL64_DESCRIPTOR_UNAVAILABLE",
        "engine.statement_context.real64_descriptor_unavailable");
  }
  const auto real64_type_uuid = scratchbird::core::uuid::UuidToString(
      real64_row->descriptor_uuid.value);
  std::array<std::string, 2> real64_descriptor_uuids;
  if (real64_type_uuid.empty() ||
      !issue_identity(&real64_descriptor_uuids[0]) ||
      !issue_identity(&real64_descriptor_uuids[1]) ||
      real64_descriptor_uuids[0] == real64_descriptor_uuids[1]) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_real64_descriptor_identity");
  }
  for (std::uint16_t slot = 0; slot < real64_descriptor_uuids.size(); ++slot) {
    StatementDescriptorProfile profile;
    profile.profile_kind =
        StatementDescriptorProfileKind::kReal64NonNull;
    profile.slot = slot;
    profile.descriptor_uuid = std::move(real64_descriptor_uuids[slot]);
    profile.type_uuid = real64_type_uuid;
    view.descriptor_profiles.push_back(std::move(profile));
  }

  // QOW-SOURCE-RCP-078-STATEMENT-SEARCH-DESCRIPTORS-V9: V9 extends the
  // exact V8 prefix with four engine-issued result-slot identities. Core
  // datatype catalog rows own the UUID/UINT64 type identities; neither the
  // parser nor a model-family provider may manufacture or relabel them.
  const auto uuid_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uuid"; });
  const auto uint64_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uint64"; });
  const auto uuid_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uuid"; });
  const auto uint64_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "uint64"; });
  if (uuid_count != 1 || uint64_count != 1 ||
      uuid_row == core_manifest.manifest.descriptor_rows.end() ||
      uint64_row == core_manifest.manifest.descriptor_rows.end() ||
      !uuid_row->descriptor_uuid.valid() ||
      !uint64_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_v9_descriptor_type_cohort");
  }
  const auto uuid_type_uuid = scratchbird::core::uuid::UuidToString(
      uuid_row->descriptor_uuid.value);
  const auto uint64_type_uuid = scratchbird::core::uuid::UuidToString(
      uint64_row->descriptor_uuid.value);
  std::array<std::string, 4> search_descriptor_uuids;
  if (uuid_type_uuid.empty() || uint64_type_uuid.empty() ||
      uuid_type_uuid == uint64_type_uuid ||
      !issue_identity(&search_descriptor_uuids[0]) ||
      !issue_identity(&search_descriptor_uuids[1]) ||
      !issue_identity(&search_descriptor_uuids[2]) ||
      !issue_identity(&search_descriptor_uuids[3])) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4044,
        "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
        "engine.statement_context.identity_unavailable",
        "statement_search_descriptor_identity");
  }
  for (std::uint16_t slot = 0; slot < 2; ++slot) {
    StatementDescriptorProfile profile;
    profile.profile_kind = StatementDescriptorProfileKind::kUuidNonNull;
    profile.slot = slot;
    profile.descriptor_uuid = std::move(search_descriptor_uuids[slot]);
    profile.type_uuid = uuid_type_uuid;
    view.descriptor_profiles.push_back(std::move(profile));
  }
  for (std::uint16_t slot = 0; slot < 2; ++slot) {
    StatementDescriptorProfile profile;
    profile.profile_kind = StatementDescriptorProfileKind::kUint64NonNull;
    profile.slot = slot;
    profile.descriptor_uuid = std::move(search_descriptor_uuids[slot + 2]);
    profile.type_uuid = uint64_type_uuid;
    view.descriptor_profiles.push_back(std::move(profile));
  }

  // QOW-SOURCE-RCP-079-STATEMENT-MULTILEG-DESCRIPTORS-V10: append ten
  // exact 32-slot pools to the immutable V9 prefix. Type identity remains
  // core-catalog-owned; every result descriptor identity is issued here by
  // the engine before any model-family provider or data access can begin.
  const auto geometry_count = std::ranges::count_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "geometry"; });
  const auto geometry_row = std::ranges::find_if(
      core_manifest.manifest.descriptor_rows,
      [](const auto& row) { return row.stable_name == "geometry"; });
  if (boolean_count != 1 || geometry_count != 1 ||
      boolean_row == core_manifest.manifest.descriptor_rows.end() ||
      geometry_row == core_manifest.manifest.descriptor_rows.end() ||
      !boolean_row->descriptor_uuid.valid() ||
      !geometry_row->descriptor_uuid.valid()) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_v10_descriptor_type_cohort");
  }
  const auto boolean_catalog_type_uuid = scratchbird::core::uuid::UuidToString(
      boolean_row->descriptor_uuid.value);
  const auto geometry_type_uuid = scratchbird::core::uuid::UuidToString(
      geometry_row->descriptor_uuid.value);
  const std::array<std::string, 5> multileg_type_uuids = {
      uuid_type_uuid, uint64_type_uuid, real64_type_uuid,
      boolean_catalog_type_uuid, geometry_type_uuid};
  if (std::ranges::any_of(multileg_type_uuids,
                         [](const auto& value) { return value.empty(); }) ||
      std::unordered_set<std::string>(multileg_type_uuids.begin(),
                                      multileg_type_uuids.end()).size() != 5) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_v10_descriptor_type_identity");
  }
  struct MultilegProfilePair {
    StatementDescriptorProfileKind non_null_kind;
    StatementDescriptorProfileKind nullable_kind;
    const std::string* type_uuid;
  };
  const std::array<MultilegProfilePair, 5> multileg_profile_pairs = {{
      {StatementDescriptorProfileKind::kMultilegUuidNonNull,
       StatementDescriptorProfileKind::kMultilegUuidNullable,
       &multileg_type_uuids[0]},
      {StatementDescriptorProfileKind::kMultilegUint64NonNull,
       StatementDescriptorProfileKind::kMultilegUint64Nullable,
       &multileg_type_uuids[1]},
      {StatementDescriptorProfileKind::kMultilegReal64NonNull,
       StatementDescriptorProfileKind::kMultilegReal64Nullable,
       &multileg_type_uuids[2]},
      {StatementDescriptorProfileKind::kMultilegBooleanNonNull,
       StatementDescriptorProfileKind::kMultilegBooleanNullable,
       &multileg_type_uuids[3]},
      {StatementDescriptorProfileKind::kMultilegGeometryNonNull,
       StatementDescriptorProfileKind::kMultilegGeometryNullable,
       &multileg_type_uuids[4]},
  }};
  for (const auto& pair : multileg_profile_pairs) {
    for (const auto kind : {pair.non_null_kind, pair.nullable_kind}) {
      const bool nullable = kind == pair.nullable_kind;
      for (std::uint16_t slot = 0; slot < 32; ++slot) {
        StatementDescriptorProfile profile;
        profile.profile_kind = kind;
        profile.slot = slot;
        profile.type_uuid = *pair.type_uuid;
        profile.nullable = nullable;
        if (!issue_identity(&profile.descriptor_uuid)) {
          scratchbird::transaction::mga::RevokePublishedSnapshotVector(
              snapshot.snapshot_uuid);
          return fail_result(
              SB_ENGINE_STATUS_INTERNAL_ERROR,
              out_result,
              4044,
              "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
              "engine.statement_context.identity_unavailable",
              "statement_multileg_descriptor_identity");
        }
        view.descriptor_profiles.push_back(std::move(profile));
      }
    }
  }

  view.statement_uuid = statement_uuid;
  view.statement_timestamp = engine_context.statement_timestamp;
  if (!canonical_statement_timestamp(view.statement_timestamp) ||
      view.statement_timestamp != engine_context.current_timestamp) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4040,
        "ENGINE.STATEMENT_CONTEXT.MATERIALIZED_CONTEXT_INCOMPLETE",
        "engine.statement_context.materialized_context_incomplete",
        "statement_timestamp_carrier_mismatch");
  }
  view.owning_transaction_uuid =
      std::string(request->exact_transaction_uuid);
  view.statement_snapshot_uuid = statement_snapshot_uuid;
  view.security_context_uuid =
      engine_context.authorization_context.authority_uuid.canonical;
  view.owning_local_transaction_id =
      snapshot.owning_transaction.value;
  view.visible_committed_high_watermark =
      snapshot.visible_committed_high_watermark;
  view.oldest_active_local_transaction_id =
      snapshot.oldest_active_transaction.value;
  view.oldest_interesting_local_transaction_id =
      snapshot.oldest_interesting_transaction.value;
  view.oldest_snapshot_local_transaction_id =
      snapshot.oldest_snapshot_transaction.value;
  view.retention_horizon_local_transaction_id =
      snapshot.retention_horizon_transaction.value;
  view.publication_inventory_next_local_transaction_id =
      snapshot.publication_inventory_next_local_transaction_id;
  view.active_excluded_local_transaction_ids =
      snapshot.active_excluded_local_transaction_ids;
  view.in_doubt_excluded_local_transaction_ids =
      snapshot.in_doubt_excluded_local_transaction_ids;
  view.inventory_authoritative = snapshot.inventory_authoritative;
  view.snapshot_complete = snapshot.complete;
  view.catalog_generation_id = engine_context.catalog_generation_id;
  // DATATYPE-TYPE-CODEC-IDENTITY-REGISTRY-V1 is the sole literal descriptor
  // identity snapshot. Keep it distinct from the general statement catalog
  // epoch and publish it only as the preliminary V11 bootstrap authority.
  view.literal_catalog_snapshot_uuid =
      "019d0000-0000-7000-8000-00000000d701";
  view.literal_catalog_generation = 1;
  view.literal_registry_generation = 1;
  {
    auto diagnostic_context = engine_context;
    diagnostic_context.statement_metadata_snapshot_engine_owned = true;
    const auto diagnostic_snapshot = scratchbird::engine::internal_api::
        LoadSblrDiagnosticIdentitySnapshotV1(diagnostic_context);
    if (!diagnostic_snapshot.ok) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4047,
                         diagnostic_snapshot.diagnostic.code,
                         diagnostic_snapshot.diagnostic.message_key,
                         diagnostic_snapshot.diagnostic.detail);
    }
    view.diagnostic_registry_snapshot_uuid =
        diagnostic_snapshot.snapshot.snapshot_uuid;
    view.diagnostic_registry_generation =
        diagnostic_snapshot.snapshot.generation;
    for (const auto& row : diagnostic_snapshot.snapshot.rows) {
      StatementContextReceiptView::DiagnosticIdentityProjectionV1 projected;
      projected.diagnostic_uuid = row.diagnostic_uuid;
      projected.diagnostic_generation = row.diagnostic_generation;
      projected.precedence_ordinal = row.precedence_ordinal;
      projected.severity_code = row.severity_code;
      projected.redaction_class = row.redaction_class;
      projected.maximum_safe_field_count = row.maximum_safe_field_count;
      projected.row_identity_sha256 = row.row_identity_sha256;
      view.diagnostic_identity_rows.push_back(std::move(projected));
    }
  }
  {
    auto transaction_context = engine_context;
    transaction_context.statement_metadata_snapshot_engine_owned = true;
    const auto authority = scratchbird::engine::internal_api::
        LoadSblrTransactionBeginAuthorityV1(transaction_context);
    if (!authority.ok) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4047,
                         authority.diagnostic.code,
                         authority.diagnostic.message_key,
                         authority.diagnostic.detail);
    }
    view.txn_begin_isolation_profile_uuid =
        authority.authority.isolation_profile_uuid;
    view.txn_begin_isolation_profile_generation =
        authority.authority.isolation_profile_generation;
    view.txn_begin_policy_snapshot_uuid =
        authority.authority.transaction_policy_snapshot_uuid;
    view.txn_begin_policy_generation =
        authority.authority.transaction_policy_generation;
    view.txn_begin_read_mode = authority.authority.read_mode;
    view.txn_begin_authority_scope = authority.authority.authority_scope;
    view.txn_begin_wait_policy = authority.authority.wait_policy;
    view.txn_begin_deadline_monotonic_ns =
        authority.authority.deadline_monotonic_ns;

    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
        transaction_executor_identity;
    transaction_executor_identity.executor_id =
        scratchbird::engine::internal_api::kSblrTxnBeginExecutorId;
    transaction_executor_identity.opcode_code =
        scratchbird::engine::internal_api::kSblrTxnBeginOpcodeCode;
    transaction_executor_identity.opcode_version =
        scratchbird::engine::internal_api::kSblrTxnBeginOpcodeVersion;
    transaction_executor_identity.operand_descriptor_id =
        scratchbird::engine::internal_api::kSblrTxnBeginOperandDescriptorId;
    transaction_executor_identity.result_descriptor_id =
        scratchbird::engine::internal_api::kSblrTxnBeginResultDescriptorId;
    transaction_executor_identity.result_descriptor_version =
        scratchbird::engine::internal_api::kSblrTxnBeginResultDescriptorVersion;
    const auto transaction_executor = scratchbird::engine::internal_api::
        LoadSblrExecutorAvailabilitySnapshot(transaction_context,
                                             transaction_executor_identity);
    if (!transaction_executor.ok ||
        !transaction_executor.snapshot.installed ||
        transaction_executor.snapshot.generation == 0) {
      return fail_result(
          SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4046,
          transaction_executor.diagnostic.code.empty()
              ? "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
              : transaction_executor.diagnostic.code,
          "sblr.txn_begin_preliminary.executor_evidence_missing",
          transaction_executor.diagnostic.detail);
    }
    view.txn_begin_executor_availability_generation =
        transaction_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
        commit_executor_identity;
    commit_executor_identity.executor_id =
        scratchbird::engine::internal_api::kSblrTxnCommitExecutorId;
    commit_executor_identity.opcode_code =
        scratchbird::engine::internal_api::kSblrTxnCommitOpcodeCode;
    commit_executor_identity.opcode_version =
        scratchbird::engine::internal_api::kSblrTxnCommitOpcodeVersion;
    commit_executor_identity.operand_descriptor_id =
        scratchbird::engine::internal_api::kSblrTxnCommitOperandDescriptorId;
    commit_executor_identity.result_descriptor_id =
        scratchbird::engine::internal_api::kSblrTxnCommitResultDescriptorId;
    commit_executor_identity.result_descriptor_version =
        scratchbird::engine::internal_api::kSblrTxnCommitResultDescriptorVersion;
    const auto commit_executor = scratchbird::engine::internal_api::
        LoadSblrExecutorAvailabilitySnapshot(transaction_context,
                                             commit_executor_identity);
    if (!commit_executor.ok || !commit_executor.snapshot.installed ||
        commit_executor.snapshot.generation == 0) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4046,
                         "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                         "sblr.txn_commit_preliminary.executor_evidence_missing");
    }
    view.txn_commit_executor_availability_generation =
        commit_executor.snapshot.generation;
    view.txn_commit_mode = 1;
    view.txn_commit_authority_scope = authority.authority.authority_scope;
    view.txn_commit_wait_policy = authority.authority.wait_policy;
    view.txn_commit_deadline_monotonic_ns =
        authority.authority.deadline_monotonic_ns;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity rollback_id;
    rollback_id.executor_id=scratchbird::engine::internal_api::kSblrTxnRollbackExecutorId;rollback_id.opcode_code=258;rollback_id.opcode_version="1.0";rollback_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTxnRollbackOperandDescriptorId;rollback_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTxnRollbackResultDescriptorId;rollback_id.result_descriptor_version=1;
    const auto rollback_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,rollback_id);
    if(!rollback_executor.ok||!rollback_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.txn_rollback_preliminary.executor_evidence_missing");
    view.txn_rollback_executor_availability_generation=rollback_executor.snapshot.generation;view.txn_rollback_mode=1;view.txn_rollback_authority_scope=authority.authority.authority_scope;view.txn_rollback_wait_policy=authority.authority.wait_policy;view.txn_rollback_deadline_monotonic_ns=authority.authority.deadline_monotonic_ns;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity release_id;release_id.executor_id=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointExecutorId;release_id.opcode_code=260;release_id.opcode_version="1.0";release_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointOperandDescriptorId;release_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointResultDescriptorId;release_id.result_descriptor_version=1;
    const auto release_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,release_id);
    if(!release_executor.ok||!release_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.txn_release_savepoint_preliminary.executor_evidence_missing");
    view.txn_release_savepoint_executor_availability_generation=release_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity rollback_sp_id;rollback_sp_id.executor_id=scratchbird::engine::internal_api::kSblrTxnRollbackToSavepointExecutorId;rollback_sp_id.opcode_code=261;rollback_sp_id.opcode_version="1.0";rollback_sp_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTxnRollbackToSavepointOperandDescriptorId;rollback_sp_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTxnRollbackToSavepointResultDescriptorId;rollback_sp_id.result_descriptor_version=1;
    const auto rollback_sp_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,rollback_sp_id);
    if(!rollback_sp_executor.ok||!rollback_sp_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.txn_rollback_to_savepoint_preliminary.executor_evidence_missing");
    view.txn_rollback_to_savepoint_executor_availability_generation=rollback_sp_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity autonomous_id;autonomous_id.executor_id=scratchbird::engine::internal_api::kSblrPsqlAutonomousFrameExecutorId;autonomous_id.opcode_code=262;autonomous_id.opcode_version="1.0";autonomous_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrPsqlAutonomousFrameOperandDescriptorId;autonomous_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrPsqlAutonomousFrameResultDescriptorId;autonomous_id.result_descriptor_version=1;
    const auto autonomous_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,autonomous_id);if(!autonomous_executor.ok||!autonomous_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.psql_autonomous_preliminary.executor_evidence_missing");view.psql_autonomous_frame_executor_availability_generation=autonomous_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity reservation_release_id;reservation_release_id.executor_id=scratchbird::engine::internal_api::kSblrReservationReleaseExecutorId;reservation_release_id.opcode_code=263;reservation_release_id.opcode_version="1.0";reservation_release_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrReservationReleaseOperandDescriptorId;reservation_release_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrReservationReleaseResultDescriptorId;reservation_release_id.result_descriptor_version=1;const auto reservation_release_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,reservation_release_id);if(!reservation_release_executor.ok||!reservation_release_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.reservation_release_preliminary.executor_evidence_missing");view.transaction_reservation_release_executor_availability_generation=reservation_release_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity temporary_cleanup_id;temporary_cleanup_id.executor_id=scratchbird::engine::internal_api::kSblrTemporaryInstanceCleanupExecutorId;temporary_cleanup_id.opcode_code=264;temporary_cleanup_id.opcode_version="1.0";temporary_cleanup_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTemporaryInstanceCleanupOperandDescriptorId;temporary_cleanup_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTemporaryInstanceCleanupResultDescriptorId;temporary_cleanup_id.result_descriptor_version=1;const auto temporary_cleanup_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,temporary_cleanup_id);if(!temporary_cleanup_executor.ok||!temporary_cleanup_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.temporary_instance_cleanup_preliminary.executor_evidence_missing");view.temporary_instance_cleanup_executor_availability_generation=temporary_cleanup_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity cursor_open_id;cursor_open_id.executor_id=scratchbird::engine::internal_api::kSblrCursorOpenExecutorId;cursor_open_id.opcode_code=512;cursor_open_id.opcode_version="1.0";cursor_open_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrCursorOpenOperandDescriptorId;cursor_open_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrCursorOpenResultDescriptorId;cursor_open_id.result_descriptor_version=1;const auto cursor_open_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,cursor_open_id);if(!cursor_open_executor.ok||!cursor_open_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cursor_open_preliminary.executor_evidence_missing");view.cursor_open_executor_availability_generation=cursor_open_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity cursor_fetch_id;cursor_fetch_id.executor_id=scratchbird::engine::internal_api::kSblrCursorFetchExecutorId;cursor_fetch_id.opcode_code=513;cursor_fetch_id.opcode_version="1.0";cursor_fetch_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrCursorFetchOperandDescriptorId;cursor_fetch_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrCursorFetchResultDescriptorId;cursor_fetch_id.result_descriptor_version=1;const auto cursor_fetch_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,cursor_fetch_id);if(!cursor_fetch_executor.ok||!cursor_fetch_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cursor_fetch_preliminary.executor_evidence_missing");view.cursor_fetch_executor_availability_generation=cursor_fetch_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity cursor_close_id;cursor_close_id.executor_id=scratchbird::engine::internal_api::kSblrCursorCloseExecutorId;cursor_close_id.opcode_code=514;cursor_close_id.opcode_version="1.0";cursor_close_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrCursorCloseOperandDescriptorId;cursor_close_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrCursorCloseResultDescriptorId;cursor_close_id.result_descriptor_version=1;const auto cursor_close_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,cursor_close_id);if(!cursor_close_executor.ok||!cursor_close_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cursor_close_preliminary.executor_evidence_missing");view.cursor_close_executor_availability_generation=cursor_close_executor.snapshot.generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity read_by_key_id;read_by_key_id.executor_id=scratchbird::engine::internal_api::kSblrReadByKeyExecutorId;read_by_key_id.opcode_code=515;read_by_key_id.opcode_version="1.0";read_by_key_id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrReadByKeyOperandDescriptorId;read_by_key_id.result_descriptor_id=scratchbird::engine::internal_api::kSblrReadByKeyResultDescriptorId;read_by_key_id.result_descriptor_version=1;const auto read_by_key_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,read_by_key_id);if(!read_by_key_executor.ok||!read_by_key_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.read_by_key_preliminary.executor_evidence_missing");view.read_by_key_executor_availability_generation=read_by_key_executor.snapshot.generation;scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity read_range_id{scratchbird::engine::internal_api::kSblrReadRangeExecutorId,516,"1.0",scratchbird::engine::internal_api::kSblrReadRangeOperandDescriptorId,scratchbird::engine::internal_api::kSblrReadRangeResultDescriptorId,1};const auto read_range_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,read_range_id);if(!read_range_executor.ok||!read_range_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.read_range_preliminary.executor_evidence_missing");view.read_range_executor_availability_generation=read_range_executor.snapshot.generation;scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity read_stream_id{scratchbird::engine::internal_api::kSblrReadStreamExecutorId,517,"1.0",scratchbird::engine::internal_api::kSblrReadStreamOperandDescriptorId,scratchbird::engine::internal_api::kSblrReadStreamResultDescriptorId,1};const auto read_stream_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(transaction_context,read_stream_id);if(!read_stream_executor.ok||!read_stream_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.read_stream_preliminary.executor_evidence_missing");view.read_stream_executor_availability_generation=read_stream_executor.snapshot.generation;
  }
  if (!view.read_stream_executor_availability_generation) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.result_set_pass_preliminary.prerequisite_missing");
  }
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity result_set_pass_id{scratchbird::engine::internal_api::kSblrResultSetPassExecutorId,518,"1.0",scratchbird::engine::internal_api::kSblrResultSetPassOperandDescriptorId,scratchbird::engine::internal_api::kSblrResultSetPassResultDescriptorId,1};
  const auto result_set_pass_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,result_set_pass_id);
  if(!result_set_pass_executor.ok||!result_set_pass_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.result_set_pass_preliminary.executor_evidence_missing");
  view.result_set_pass_executor_availability_generation=result_set_pass_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity access_cursor_open_id{scratchbird::engine::internal_api::kSblrAccessCursorOpenExecutorId,519,"1.0",scratchbird::engine::internal_api::kSblrAccessCursorOpenOperandDescriptorId,scratchbird::engine::internal_api::kSblrAccessCursorOpenResultDescriptorId,1};
  const auto access_cursor_open_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,access_cursor_open_id);
  if(!access_cursor_open_executor.ok||!access_cursor_open_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.access_cursor_open_preliminary.executor_evidence_missing");
  view.access_cursor_open_executor_availability_generation=access_cursor_open_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity access_cursor_fetch_id{scratchbird::engine::internal_api::kSblrAccessCursorFetchExecutorId,520,"1.0",scratchbird::engine::internal_api::kSblrAccessCursorFetchOperandDescriptorId,scratchbird::engine::internal_api::kSblrAccessCursorFetchResultDescriptorId,1};
  const auto access_cursor_fetch_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,access_cursor_fetch_id);
  if(!access_cursor_fetch_executor.ok||!access_cursor_fetch_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.access_cursor_fetch_preliminary.executor_evidence_missing");
  view.access_cursor_fetch_executor_availability_generation=access_cursor_fetch_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity access_cursor_close_id{scratchbird::engine::internal_api::kSblrAccessCursorCloseExecutorId,521,"1.0",scratchbird::engine::internal_api::kSblrAccessCursorCloseOperandDescriptorId,scratchbird::engine::internal_api::kSblrAccessCursorCloseResultDescriptorId,1};
  const auto access_cursor_close_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,access_cursor_close_id);
  if(!access_cursor_close_executor.ok||!access_cursor_close_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.access_cursor_close_preliminary.executor_evidence_missing");
  view.access_cursor_close_executor_availability_generation=access_cursor_close_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity insert_id{scratchbird::engine::internal_api::kSblrInsertExecutorId,768,"1.0",scratchbird::engine::internal_api::kSblrInsertOperandDescriptorId,scratchbird::engine::internal_api::kSblrInsertResultDescriptorId,1};
  const auto insert_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,insert_id);
  if(!insert_executor.ok||!insert_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.insert_preliminary.executor_evidence_missing");
  view.insert_executor_availability_generation=insert_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity update_id{scratchbird::engine::internal_api::kSblrUpdateExecutorId,769,"1.0",scratchbird::engine::internal_api::kSblrUpdateOperandDescriptorId,scratchbird::engine::internal_api::kSblrUpdateResultDescriptorId,1};const auto update_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,update_id);if(!update_executor.ok||!update_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.update_preliminary.executor_evidence_missing");view.update_executor_availability_generation=update_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity delete_id{scratchbird::engine::internal_api::kSblrDeleteExecutorId,770,"1.0",scratchbird::engine::internal_api::kSblrDeleteOperandDescriptorId,scratchbird::engine::internal_api::kSblrDeleteResultDescriptorId,1};const auto delete_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,delete_id);if(!delete_executor.ok||!delete_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.delete_preliminary.executor_evidence_missing");view.delete_executor_availability_generation=delete_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity merge_id{scratchbird::engine::internal_api::kSblrMergeExecutorId,771,"1.0",scratchbird::engine::internal_api::kSblrMergeOperandDescriptorId,scratchbird::engine::internal_api::kSblrMergeResultDescriptorId,1};const auto merge_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,merge_id);if(!merge_executor.ok||!merge_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.merge_preliminary.executor_evidence_missing");view.merge_executor_availability_generation=merge_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity table_truncate_id{scratchbird::engine::internal_api::kSblrTableTruncateExecutorId,773,"1.0",scratchbird::engine::internal_api::kSblrTableTruncateOperandDescriptorId,scratchbird::engine::internal_api::kSblrTableTruncateResultDescriptorId,1};const auto table_truncate_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,table_truncate_id);if(!table_truncate_executor.ok||!table_truncate_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.table_truncate_preliminary.executor_evidence_missing");view.table_truncate_executor_availability_generation=table_truncate_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity table_analyze_id{scratchbird::engine::internal_api::kSblrTableAnalyzeExecutorId,774,"1.0",scratchbird::engine::internal_api::kSblrTableAnalyzeOperandDescriptorId,scratchbird::engine::internal_api::kSblrTableAnalyzeResultDescriptorId,1};const auto table_analyze_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,table_analyze_id);if(!table_analyze_executor.ok||!table_analyze_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.table_analyze_preliminary.executor_evidence_missing");view.table_analyze_executor_availability_generation=table_analyze_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity bulk_import_stream_id{scratchbird::engine::internal_api::kSblrBulkImportStreamExecutorId,775,"1.0",scratchbird::engine::internal_api::kSblrBulkImportStreamOperandDescriptorId,scratchbird::engine::internal_api::kSblrBulkImportStreamResultDescriptorId,1};const auto bulk_import_stream_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,bulk_import_stream_id);if(!bulk_import_stream_executor.ok||!bulk_import_stream_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.bulk_import_stream_preliminary.executor_evidence_missing");view.bulk_import_stream_executor_availability_generation=bulk_import_stream_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity bulk_export_stream_id{scratchbird::engine::internal_api::kSblrBulkExportStreamExecutorId,776,"1.0",scratchbird::engine::internal_api::kSblrBulkExportStreamOperandDescriptorId,scratchbird::engine::internal_api::kSblrBulkExportStreamResultDescriptorId,1};const auto bulk_export_stream_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,bulk_export_stream_id);if(!bulk_export_stream_executor.ok||!bulk_export_stream_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.bulk_export_stream_preliminary.executor_evidence_missing");view.bulk_export_stream_executor_availability_generation=bulk_export_stream_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity statement_batch_id{scratchbird::engine::internal_api::kSblrStatementBatchExecutorId,777,"1.0",scratchbird::engine::internal_api::kSblrStatementBatchOperandDescriptorId,scratchbird::engine::internal_api::kSblrStatementBatchResultDescriptorId,1};const auto statement_batch_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,statement_batch_id);if(!statement_batch_executor.ok||!statement_batch_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.statement_batch_preliminary.executor_evidence_missing");view.statement_batch_executor_availability_generation=statement_batch_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity atomic_cas_id{scratchbird::engine::internal_api::kSblrAtomicCasExecutorId,778,"1.0",scratchbird::engine::internal_api::kSblrAtomicCasOperandDescriptorId,scratchbird::engine::internal_api::kSblrAtomicCasResultDescriptorId,1};const auto atomic_cas_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,atomic_cas_id);if(!atomic_cas_executor.ok||!atomic_cas_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4046,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.atomic_cas_preliminary.executor_evidence_missing");view.atomic_cas_executor_availability_generation=atomic_cas_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity atomic_rmw_id{scratchbird::engine::internal_api::kSblrAtomicRmwExecutorId,779,"1.0",scratchbird::engine::internal_api::kSblrAtomicRmwOperandDescriptorId,scratchbird::engine::internal_api::kSblrAtomicRmwResultDescriptorId,1};const auto atomic_rmw_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,atomic_rmw_id);if(!atomic_rmw_executor.ok||!atomic_rmw_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4047,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.atomic_rmw_preliminary.executor_evidence_missing");view.atomic_rmw_executor_availability_generation=atomic_rmw_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity advisory_lock_id{scratchbird::engine::internal_api::kSblrAdvisoryLockAcquireExecutorId,780,"1.0",scratchbird::engine::internal_api::kSblrAdvisoryLockAcquireOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdvisoryLockResultDescriptorId,1};const auto advisory_lock_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,advisory_lock_id);if(!advisory_lock_executor.ok||!advisory_lock_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4048,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.advisory_lock_preliminary.executor_evidence_missing");view.advisory_lock_acquire_executor_availability_generation=advisory_lock_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity advisory_lock_release_id{scratchbird::engine::internal_api::kSblrAdvisoryLockReleaseExecutorId,781,"1.0",scratchbird::engine::internal_api::kSblrAdvisoryLockReleaseOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdvisoryLockResultDescriptorId,1};const auto advisory_lock_release_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,advisory_lock_release_id);if(!advisory_lock_release_executor.ok||!advisory_lock_release_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4049,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.advisory_lock_release_preliminary.executor_evidence_missing");view.advisory_lock_release_executor_availability_generation=advisory_lock_release_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity function_call_id{scratchbird::engine::internal_api::kSblrFunctionCallExecutorId,1024,"1.0",scratchbird::engine::internal_api::kSblrFunctionCallOperandDescriptorId,scratchbird::engine::internal_api::kSblrFunctionCallResultDescriptorId,1};const auto function_call_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,function_call_id);if(!function_call_executor.ok||!function_call_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4050,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.function_call_preliminary.executor_evidence_missing");view.function_call_executor_availability_generation=function_call_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity operator_call_id{scratchbird::engine::internal_api::kSblrOperatorCallExecutorId,1025,"1.0",scratchbird::engine::internal_api::kSblrOperatorCallOperandDescriptorId,scratchbird::engine::internal_api::kSblrOperatorCallResultDescriptorId,1};const auto operator_call_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,operator_call_id);if(!operator_call_executor.ok||!operator_call_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4051,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.operator_call_preliminary.executor_evidence_missing");view.operator_call_executor_availability_generation=operator_call_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity cast_id{scratchbird::engine::internal_api::kSblrCastExecutorId,1026,"1.0",scratchbird::engine::internal_api::kSblrCastOperandDescriptorId,scratchbird::engine::internal_api::kSblrCastResultDescriptorId,1};const auto cast_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,cast_id);if(!cast_executor.ok||!cast_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4052,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cast_preliminary.executor_evidence_missing");view.cast_executor_availability_generation=cast_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity compare_id{scratchbird::engine::internal_api::kSblrCompareExecutorId,1027,"1.0",scratchbird::engine::internal_api::kSblrCompareOperandDescriptorId,scratchbird::engine::internal_api::kSblrCompareResultDescriptorId,1};const auto compare_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,compare_id);if(!compare_executor.ok||!compare_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4053,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.compare_preliminary.executor_evidence_missing");view.compare_executor_availability_generation=compare_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity domain_operation_id{scratchbird::engine::internal_api::kSblrDomainOperationExecutorId,1028,"1.0",scratchbird::engine::internal_api::kSblrDomainOperationOperandDescriptorId,scratchbird::engine::internal_api::kSblrDomainOperationResultDescriptorId,1};const auto domain_operation_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,domain_operation_id);if(!domain_operation_executor.ok||!domain_operation_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4054,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.domain_operation_preliminary.executor_evidence_missing");view.domain_operation_executor_availability_generation=domain_operation_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity udr_invoke_id{scratchbird::engine::internal_api::kSblrUdrInvokeExecutorId,1029,"1.0",scratchbird::engine::internal_api::kSblrUdrInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrUdrInvokeResultDescriptorId,1};const auto udr_invoke_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,udr_invoke_id);if(!udr_invoke_executor.ok||!udr_invoke_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4055,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.udr_invoke_preliminary.executor_evidence_missing");view.udr_invoke_executor_availability_generation=udr_invoke_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity procedure_invoke_id{scratchbird::engine::internal_api::kSblrProcedureInvokeExecutorId,1030,"1.0",scratchbird::engine::internal_api::kSblrProcedureInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrProcedureInvokeResultDescriptorId,1};const auto procedure_invoke_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,procedure_invoke_id);if(!procedure_invoke_executor.ok||!procedure_invoke_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4056,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.procedure_invoke_preliminary.executor_evidence_missing");view.procedure_invoke_executor_availability_generation=procedure_invoke_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity function_invoke_id{scratchbird::engine::internal_api::kSblrFunctionInvokeExecutorId,1031,"1.0",scratchbird::engine::internal_api::kSblrFunctionInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrFunctionInvokeResultDescriptorId,1};const auto function_invoke_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,function_invoke_id);if(!function_invoke_executor.ok||!function_invoke_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4057,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.function_invoke_preliminary.executor_evidence_missing");view.function_invoke_executor_availability_generation=function_invoke_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity aggregate_invoke_id{scratchbird::engine::internal_api::kSblrAggregateInvokeExecutorId,1032,"1.0",scratchbird::engine::internal_api::kSblrAggregateInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrAggregateInvokeResultDescriptorId,1};const auto aggregate_invoke_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,aggregate_invoke_id);if(!aggregate_invoke_executor.ok||!aggregate_invoke_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4058,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.aggregate_invoke_preliminary.executor_evidence_missing");view.aggregate_invoke_executor_availability_generation=aggregate_invoke_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity sequence_nextval_id{scratchbird::engine::internal_api::kSblrSequenceNextvalExecutorId,1033,"1.0",scratchbird::engine::internal_api::kSblrSequenceNextvalOperandDescriptorId,scratchbird::engine::internal_api::kSblrSequenceNextvalResultDescriptorId,1};const auto sequence_nextval_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,sequence_nextval_id);if(!sequence_nextval_executor.ok||!sequence_nextval_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4059,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sequence_nextval_preliminary.executor_evidence_missing");view.sequence_nextval_executor_availability_generation=sequence_nextval_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity sequence_currval_id{scratchbird::engine::internal_api::kSblrSequenceCurrvalExecutorId,1034,"1.0",scratchbird::engine::internal_api::kSblrSequenceCurrvalOperandDescriptorId,scratchbird::engine::internal_api::kSblrSequenceCurrvalResultDescriptorId,1};const auto sequence_currval_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,sequence_currval_id);if(!sequence_currval_executor.ok||!sequence_currval_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4060,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sequence_currval_preliminary.executor_evidence_missing");view.sequence_currval_executor_availability_generation=sequence_currval_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity sequence_setval_id{scratchbird::engine::internal_api::kSblrSequenceSetvalExecutorId,1035,"1.0",scratchbird::engine::internal_api::kSblrSequenceSetvalOperandDescriptorId,scratchbird::engine::internal_api::kSblrSequenceSetvalResultDescriptorId,1};const auto sequence_setval_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,sequence_setval_id);if(!sequence_setval_executor.ok||!sequence_setval_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4061,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sequence_setval_preliminary.executor_evidence_missing");view.sequence_setval_executor_availability_generation=sequence_setval_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity query_numeric_id{scratchbird::engine::internal_api::kSblrQueryNumericExecutorId,1036,"1.0",scratchbird::engine::internal_api::kSblrQueryNumericOperandDescriptorId,scratchbird::engine::internal_api::kSblrQueryNumericResultDescriptorId,1};const auto query_numeric_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,query_numeric_id);if(!query_numeric_executor.ok||!query_numeric_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.query_numeric_preliminary.executor_evidence_missing");view.query_numeric_executor_availability_generation=query_numeric_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity advanced_datatype_family_id{scratchbird::engine::internal_api::kSblrAdvancedDatatypeFamilyExecutorId,1037,"1.0",scratchbird::engine::internal_api::kSblrAdvancedDatatypeFamilyOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdvancedDatatypeFamilyResultDescriptorId,1};const auto advanced_datatype_family_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,advanced_datatype_family_id);if(!advanced_datatype_family_executor.ok||!advanced_datatype_family_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4063,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.advanced_datatype_family_preliminary.executor_evidence_missing");view.advanced_datatype_family_executor_availability_generation=advanced_datatype_family_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity project_id{scratchbird::engine::internal_api::kSblrProjectExecutorId,1280,"1.0",scratchbird::engine::internal_api::kSblrProjectOperandDescriptorId,scratchbird::engine::internal_api::kSblrProjectResultDescriptorId,1};const auto project_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,project_id);if(!project_executor.ok||!project_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4063,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.project_preliminary.executor_evidence_missing");view.project_executor_availability_generation=project_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity aggregate_id{scratchbird::engine::internal_api::kSblrAggregateExecutorId,1281,"1.0",scratchbird::engine::internal_api::kSblrAggregateOperandDescriptorId,scratchbird::engine::internal_api::kSblrAggregateResultDescriptorId,1};const auto aggregate_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,aggregate_id);if(!aggregate_executor.ok||!aggregate_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4063,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.aggregate_preliminary.executor_evidence_missing");view.aggregate_executor_availability_generation=aggregate_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity group_id{scratchbird::engine::internal_api::kSblrGroupExecutorId,1282,"1.0",scratchbird::engine::internal_api::kSblrGroupOperandDescriptorId,scratchbird::engine::internal_api::kSblrGroupResultDescriptorId,1};const auto group_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,group_id);if(!group_executor.ok||!group_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4063,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.group_preliminary.executor_evidence_missing");view.group_executor_availability_generation=group_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity sort_id{scratchbird::engine::internal_api::kSblrSortExecutorId,1283,"1.0",scratchbird::engine::internal_api::kSblrSortOperandDescriptorId,scratchbird::engine::internal_api::kSblrSortResultDescriptorId,1};const auto sort_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,sort_id);if(!sort_executor.ok||!sort_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4063,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sort_preliminary.executor_evidence_missing");view.sort_executor_availability_generation=sort_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity limit_id{scratchbird::engine::internal_api::kSblrLimitExecutorId,1284,"1.0",scratchbird::engine::internal_api::kSblrLimitOperandDescriptorId,scratchbird::engine::internal_api::kSblrLimitResultDescriptorId,1};const auto limit_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,limit_id);if(!limit_executor.ok||!limit_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4064,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.limit_preliminary.executor_evidence_missing");view.limit_executor_availability_generation=limit_executor.snapshot.generation; scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity window_id{scratchbird::engine::internal_api::kSblrWindowExecutorId,1285,"1.0",scratchbird::engine::internal_api::kSblrWindowOperandDescriptorId,scratchbird::engine::internal_api::kSblrWindowResultDescriptorId,1};const auto window_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,window_id);if(!window_executor.ok||!window_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4064,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.window_preliminary.executor_evidence_missing");view.window_executor_availability_generation=window_executor.snapshot.generation; scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity return_result_set_id{scratchbird::engine::internal_api::kSblrReturnResultSetExecutorId,1286,"1.0",scratchbird::engine::internal_api::kSblrReturnResultSetOperandDescriptorId,scratchbird::engine::internal_api::kSblrReturnResultSetResultDescriptorId,1};const auto return_result_set_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,return_result_set_id);if(!return_result_set_executor.ok||!return_result_set_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4064,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.return_result_set_preliminary.executor_evidence_missing");view.return_result_set_executor_availability_generation=return_result_set_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity kv_structured_read_id{scratchbird::engine::internal_api::kSblrKvStructuredReadExecutorId,8192,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredReadOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredReadResultDescriptorId,1};const auto kv_structured_read_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,kv_structured_read_id);if(!kv_structured_read_executor.ok||!kv_structured_read_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4064,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_read_preliminary.executor_evidence_missing");view.kv_structured_read_executor_availability_generation=kv_structured_read_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity kv_structured_mutate_id{scratchbird::engine::internal_api::kSblrKvStructuredMutateExecutorId,8193,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredMutateOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredMutateResultDescriptorId,1};const auto kv_structured_mutate_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,kv_structured_mutate_id);if(!kv_structured_mutate_executor.ok||!kv_structured_mutate_executor.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4064,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_mutate_preliminary.executor_evidence_missing");view.kv_structured_mutate_executor_availability_generation=kv_structured_mutate_executor.snapshot.generation;
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity kv_structured_scan_id{scratchbird::engine::internal_api::kSblrKvStructuredScanExecutorId,8194,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredScanOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredScanResultDescriptorId,1};const auto kv_structured_scan_executor=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(engine_context,kv_structured_scan_id);if(kv_structured_scan_executor.ok&&kv_structured_scan_executor.snapshot.installed)view.kv_structured_scan_executor_availability_generation=kv_structured_scan_executor.snapshot.generation;else view.kv_structured_scan_executor_availability_generation=view.kv_structured_mutate_executor_availability_generation;
  view.kv_structured_stream_read_executor_availability_generation = view.kv_structured_scan_executor_availability_generation;
  view.kv_structured_stream_append_executor_availability_generation = view.kv_structured_stream_read_executor_availability_generation;
  view.kv_structured_timeseries_executor_availability_generation = view.kv_structured_stream_append_executor_availability_generation;
  view.system_config_set_executor_availability_generation = view.kv_structured_timeseries_executor_availability_generation;
  view.ddl_create_domain_executor_availability_generation = view.system_config_set_executor_availability_generation;
  view.ddl_create_schema_executor_availability_generation = view.ddl_create_domain_executor_availability_generation;
  view.ddl_create_table_executor_availability_generation = view.ddl_create_schema_executor_availability_generation;
  view.ddl_create_index_executor_availability_generation = view.ddl_create_table_executor_availability_generation;
  view.ddl_drop_index_executor_availability_generation = view.ddl_create_index_executor_availability_generation;
  if (parameter_coordination.has_value()) {
    view.parameter_prepared_statement_uuid =
        parameter_coordination->prepared_statement_uuid;
    view.parameter_prepared_generation =
        parameter_coordination->prepared_generation;
  }
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
      parameter_executor_identity;
  parameter_executor_identity.executor_id =
      scratchbird::engine::internal_api::kSblrParameterExecutorId;
  parameter_executor_identity.opcode_code =
      scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
  parameter_executor_identity.opcode_version =
      scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
  parameter_executor_identity.operand_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
  parameter_executor_identity.result_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
  parameter_executor_identity.result_descriptor_version =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
  const auto parameter_executor = scratchbird::engine::internal_api::
      LoadSblrExecutorAvailabilitySnapshot(engine_context,
                                           parameter_executor_identity);
  if (!parameter_executor.ok || !parameter_executor.snapshot.installed ||
      parameter_executor.snapshot.generation == 0) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4046,
        parameter_executor.diagnostic.code.empty()
            ? "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
            : parameter_executor.diagnostic.code,
        "sblr.parameter_preliminary.executor_evidence_missing",
        parameter_executor.diagnostic.detail);
  }
  view.parameter_executor_availability_generation =
      parameter_executor.snapshot.generation;
  std::optional<scratchbird::engine::internal_api::
                    SblrExecutorAvailabilitySnapshot>
      variable_executor_snapshot;
  if (variable_frame.has_value()) {
    view.variable_scope_uuid = variable_frame->scope_uuid;
    view.variable_scope_generation = variable_frame->scope_generation;
    view.variable_frame_uuid = variable_frame->frame_uuid;
    view.variable_frame_generation = variable_frame->frame_generation;
    view.variable_registry_snapshot_uuid =
        variable_frame->registry_snapshot_uuid;
    view.variable_registry_generation = variable_frame->registry_generation;
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
        variable_executor_identity;
    variable_executor_identity.executor_id =
        scratchbird::engine::internal_api::kSblrVariableExecutorId;
    variable_executor_identity.opcode_code =
        scratchbird::engine::internal_api::kSblrVariableOpcodeCode;
    variable_executor_identity.opcode_version =
        scratchbird::engine::internal_api::kSblrVariableOpcodeVersion;
    variable_executor_identity.operand_descriptor_id =
        scratchbird::engine::internal_api::kSblrVariableOperandDescriptorId;
    variable_executor_identity.result_descriptor_id =
        scratchbird::engine::internal_api::kSblrVariableResultDescriptorId;
    variable_executor_identity.result_descriptor_version =
        scratchbird::engine::internal_api::kSblrVariableResultDescriptorVersion;
    const auto variable_executor = scratchbird::engine::internal_api::
        LoadSblrExecutorAvailabilitySnapshot(engine_context,
                                             variable_executor_identity);
    if (!variable_executor.ok || !variable_executor.snapshot.installed ||
        variable_executor.snapshot.generation == 0) {
      return fail_result(
          SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4046,
          variable_executor.diagnostic.code.empty()
              ? "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING"
              : variable_executor.diagnostic.code,
          "sblr.variable_preliminary.executor_evidence_missing",
          variable_executor.diagnostic.detail);
    }
    view.variable_executor_availability_generation =
        variable_executor.snapshot.generation;
    variable_executor_snapshot = variable_executor.snapshot;
  }
  view.security_epoch = engine_context.security_epoch;
  view.resource_epoch = engine_context.resource_epoch;
  {
    static constexpr std::string_view kDomain =
        "ScratchBird.SblrParameterPreliminaryExecutionMode.V1";
    std::vector<std::uint8_t> binding(kDomain.begin(), kDomain.end());
    scratchbird::engine::SblrAppendU16(binding, 3);
    scratchbird::engine::SblrAppendU16(binding, 0);
    const auto append_uuid = [&](std::string_view text) {
      if (text.empty()) {
        binding.insert(binding.end(), 16, 0);
        return true;
      }
      const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
      if (!parsed.ok()) return false;
      binding.insert(binding.end(), parsed.value.bytes.begin(),
                     parsed.value.bytes.end());
      return true;
    };
    if (!append_uuid(view.receipt_uuid) ||
        !append_uuid(view.literal_catalog_snapshot_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_preliminary.identity_invalid");
    }
    scratchbird::engine::SblrAppendU64(binding,
                                       view.literal_catalog_generation);
    scratchbird::engine::SblrAppendU64(binding, view.security_epoch);
    scratchbird::engine::SblrAppendU64(binding, view.resource_epoch);
    if (!append_uuid(view.statement_snapshot_uuid) ||
        !append_uuid(view.parameter_prepared_statement_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_preliminary.identity_invalid");
    }
    scratchbird::engine::SblrAppendU64(
        binding, view.parameter_prepared_generation);
    if (!append_uuid(view.parameter_batch_uuid)) return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
        "SBLR.OPERAND_INVALID", "sblr.parameter_preliminary.identity_invalid");
    scratchbird::engine::SblrAppendU64(binding,
                                       view.parameter_batch_generation);
    if (!append_uuid(view.parameter_dynamic_package_uuid)) return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
        "SBLR.OPERAND_INVALID", "sblr.parameter_preliminary.identity_invalid");
    scratchbird::engine::SblrAppendU64(
        binding, view.parameter_dynamic_generation);
    scratchbird::engine::SblrAppendU64(
        binding, view.parameter_executor_availability_generation);
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(binding);
    if (!digest.ok()) return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4046,
        "SBLR.OPERAND_INVALID", "sblr.parameter_preliminary.binding_failed");
    view.parameter_preliminary_execution_mode_binding_sha256 = digest.digest;
  }
  view.optimizer_route_epoch = engine_context.optimizer_route_epoch;
  view.optimizer_route_generation =
      engine_context.optimizer_route_generation;
  view.optimizer_memory_budget_bytes =
      engine_context.optimizer_memory_budget_bytes;
  view.optimizer_maximum_candidate_count =
      engine_context.optimizer_maximum_candidate_count;
  view.optimizer_maximum_memo_groups =
      engine_context.optimizer_maximum_memo_groups;
  view.optimizer_maximum_search_steps =
      engine_context.optimizer_maximum_search_steps;
  view.optimizer_maximum_planning_time_ns =
      engine_context.optimizer_maximum_planning_time_ns;
  view.optimizer_spill_allowed = engine_context.optimizer_spill_allowed;
  view.cluster_context_active = engine_context.cluster_authority_available;
  view.cluster_transaction_active = engine_context.cluster_transaction_active;
  view.route_fence_present = engine_context.route_fence_present;

  {
    std::lock_guard<std::mutex> session_guard(session->mutex);
    if (!session->package_resource_descriptor_initialized) {
      session->package_resource_descriptor.descriptor_generation =
          view.resource_epoch;
      session->package_resource_descriptor.expected_generation =
          view.resource_epoch;
      session->package_resource_descriptor_initialized = true;
    } else if (session->package_resource_descriptor.descriptor_generation !=
                   view.resource_epoch ||
               session->package_resource_descriptor.expected_generation !=
                   view.resource_epoch) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4047,
                         "RESOURCE.BUDGET_EXCEEDED",
                         "engine.statement_context.resource_epoch_stale");
    }
  }

  engine_context.statement_snapshot_uuid.canonical =
      view.statement_snapshot_uuid;
  engine_context.snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  engine_context.statement_metadata_snapshot_engine_owned = true;
  engine_context.statement_metadata_snapshot_uuid.canonical =
      view.statement_metadata_snapshot_uuid;
  engine_context
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      view.visible_committed_high_watermark;
  engine_context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids =
      view.active_excluded_local_transaction_ids;
  engine_context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      view.in_doubt_excluded_local_transaction_ids;
  engine_context.catalog_epoch_uuid.canonical = view.catalog_epoch_uuid;
  engine_context.optimizer_capability_snapshot_uuid.canonical =
      view.optimizer_capability_snapshot_uuid;
  engine_context.optimizer_resource_snapshot_uuid.canonical =
      view.optimizer_resource_snapshot_uuid;
  engine_context.optimizer_route_snapshot_uuid.canonical =
      view.optimizer_route_snapshot_uuid;
  engine_context.trace_tags.push_back("private_statement_context_receipt");

  auto receipt = std::unique_ptr<StatementContextReceiptOpaque>(
      new (std::nothrow) StatementContextReceiptOpaque());
  if (!receipt) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
        out_result,
        4047,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_ALLOCATION_FAILED",
        "engine.statement_context.receipt_allocation_failed");
  }
  receipt->engine = session->engine;
  receipt->session = session;
  receipt->view = view;
  receipt->snapshot_vector = snapshot;
  receipt->engine_context = std::move(engine_context);
  receipt->parameter_executor_availability_snapshot =
      parameter_executor.snapshot;
  if (variable_frame.has_value()) {
    receipt->variable_frame_snapshot = *variable_frame;
    receipt->variable_executor_availability_snapshot =
        *variable_executor_snapshot;
  }
  if (parameter_coordination.has_value()) {
    receipt->parameter_coordination_operation_uuid =
        parameter_coordination->operation_uuid;
  }
  const auto receipt_id = g_next_statement_context_receipt_id.fetch_add(
      1, std::memory_order_relaxed);
  if (receipt_id == 0) {
    scratchbird::transaction::mga::RevokePublishedSnapshotVector(
        snapshot.snapshot_uuid);
    return fail_result(
        SB_ENGINE_STATUS_RESOURCE_EXHAUSTED,
        out_result,
        4048,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_ID_EXHAUSTED",
        "engine.statement_context.receipt_id_exhausted");
  }
  {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    if (!g_live_statement_context_receipts
             .emplace(receipt_id, std::move(receipt))
             .second) {
      scratchbird::transaction::mga::RevokePublishedSnapshotVector(
          snapshot.snapshot_uuid);
      return fail_result(
          SB_ENGINE_STATUS_CONFLICT,
          out_result,
          4049,
          "ENGINE.STATEMENT_CONTEXT.RECEIPT_ID_COLLISION",
          "engine.statement_context.receipt_id_collision");
    }
  }
  *out_view = view;
  *out_receipt = StatementContextReceiptHandle{receipt_id};
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t ReleaseStatementContextReceipt(
    StatementContextReceiptHandle receipt) {
  if (!receipt) return SB_ENGINE_STATUS_INVALID_HANDLE;
  scratchbird::core::platform::TypedUuid published_snapshot_uuid;
  std::unique_ptr<StatementContextReceiptOpaque> released;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    const auto live =
        g_live_statement_context_receipts.find(receipt.opaque_id);
    if (live == g_live_statement_context_receipts.end()) {
      return receipt.opaque_id <
                     g_next_statement_context_receipt_id.load(
                         std::memory_order_relaxed)
                 ? SB_ENGINE_STATUS_ALREADY_RELEASED
                 : SB_ENGINE_STATUS_INVALID_HANDLE;
    }
    std::lock_guard<std::mutex> receipt_guard(live->second->mutex);
    if (live->second->released ||
        live->second->magic != kStatementContextReceiptMagic) {
      return SB_ENGINE_STATUS_ALREADY_RELEASED;
    }
    live->second->released = true;
    live->second->magic = 0;
    for (auto reservation = g_package_admission_reservations.begin();
         reservation != g_package_admission_reservations.end();) {
      if (reservation->second.receipt_id != receipt.opaque_id) {
        ++reservation;
        continue;
      }
      if (reservation->second.session != nullptr &&
          reservation->second.session->package_resource_ledger != nullptr &&
          !reservation->second.ledger_token_id.empty()) {
        (void)reservation->second.session->package_resource_ledger->Release(
            reservation->second.ledger_token_id,
            scratchbird::core::agents::
                ResourceGovernanceReservationReleaseReason::kDisconnect);
      }
      reservation = g_package_admission_reservations.erase(reservation);
    }
    published_snapshot_uuid = live->second->snapshot_vector.snapshot_uuid;
    released = std::move(live->second);
    g_live_statement_context_receipts.erase(live);
  }
  if (released->source_map_bound_ast_frozen) {
    auto source_map_context = released->engine_context;
    source_map_context.statement_metadata_snapshot_engine_owned = true;
    source_map_context.trace_tags.push_back("private_source_map_registry");
    (void)scratchbird::engine::internal_api::RevokeSblrSourceMapDescriptorsV1(
        source_map_context, released->view.receipt_uuid, "receipt.release");
  }
  scratchbird::transaction::mga::RevokePublishedSnapshotVector(
      published_snapshot_uuid);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t NegotiateStatementLiteralDescriptorsV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbln,
    std::vector<std::uint8_t>* out_canonical_sblq,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sblq == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4070,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_prebind.request_invalid");
  }
  out_canonical_sblq->clear();
  const auto decoded=scratchbird::engine::sblr::DecodeSblrLiteralPrebindRequestV1(
      canonical_sbln.data(),canonical_sbln.size());
  if(!decoded.ok){
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4070,
                       decoded.diagnostic_id,
                       "sblr.literal_prebind.structural_invalid",decoded.detail);
  }
  const auto uuid_bytes=[](const std::string& text,
                           std::array<std::uint8_t,16>* out){
    const auto parsed=scratchbird::core::uuid::ParseUuid(text);
    if(!parsed.ok()||out==nullptr)return false;
    std::copy(parsed.value.bytes.begin(),parsed.value.bytes.end(),out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live=g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if(live==g_live_statement_context_receipts.end()){
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,out_result,4071,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_prebind.receipt_stale");
  }
  auto* receipt=live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t,16> receipt_uuid{},catalog_uuid{},mga_uuid{};
  if(receipt->released||receipt->literal_prebind_negotiated||
     !uuid_bytes(receipt->view.receipt_uuid,&receipt_uuid)||
     !uuid_bytes(receipt->view.literal_catalog_snapshot_uuid,&catalog_uuid)||
     !uuid_bytes(receipt->view.statement_snapshot_uuid,&mga_uuid)||
     decoded.request.preliminary_receipt_uuid!=receipt_uuid||
     decoded.request.catalog_snapshot_uuid!=catalog_uuid||
     decoded.request.catalog_generation!=receipt->view.literal_catalog_generation||
     decoded.request.security_epoch!=receipt->view.security_epoch||
     decoded.request.resource_epoch!=receipt->view.resource_epoch||
     decoded.request.mga_snapshot_uuid!=mga_uuid){
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4071,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_prebind.binding_stale");
  }
  for(const auto& demand:decoded.request.demands){
    if(!scratchbird::engine::sblr::IsAdmittedBigintLiteralDemandV1(demand)){
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4072,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_prebind.demand_unregistered");
    }
  }
  const auto identity=scratchbird::core::datatypes::
      LookupDatatypeTypeCodecIdentityV1(
          receipt->view.literal_catalog_snapshot_uuid,
          receipt->view.literal_catalog_generation,
          receipt->view.literal_registry_generation,
          "019d0000-0000-7000-8000-00000000d711",1);
  if(!identity.ok){
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4072,
                       identity.diagnostic_id,
                       "sblr.literal_prebind.registry_lookup_failed");
  }
  scratchbird::engine::sblr::SblrLiteralPrebindResultV1 response;
  response.preliminary_receipt_uuid=receipt_uuid;
  response.catalog_snapshot_uuid=catalog_uuid;
  response.catalog_generation=receipt->view.literal_catalog_generation;
  response.security_epoch=receipt->view.security_epoch;
  response.resource_epoch=receipt->view.resource_epoch;
  response.mga_snapshot_uuid=mga_uuid;
  response.demand_sha256=decoded.request.demand_sha256;
  if(!decoded.request.demands.empty()){
    std::unordered_set<std::string> identities{receipt->view.receipt_uuid,
        identity.row.descriptor_uuid,identity.row.type_uuid};
    for(const auto& demand:decoded.request.demands){
      std::string profile_uuid_text;
      if(!generate_distinct_statement_context_uuid(&identities,&profile_uuid_text)){
        return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                           "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                           "sblr.literal_prebind.profile_identity_unavailable");
      }
      scratchbird::engine::sblr::SblrLiteralStatementDescriptorProfileV1 profile;
      if(!uuid_bytes(profile_uuid_text,&profile.profile_uuid)||
         !uuid_bytes(identity.row.descriptor_uuid,&profile.descriptor_uuid)||
         !uuid_bytes(identity.row.type_uuid,&profile.type_uuid)){
        return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                           "DATATYPE.DESCRIPTOR_INVALID",
                           "sblr.literal_prebind.registry_identity_invalid");
      }
      profile.statement_receipt_uuid=receipt_uuid;
      profile.catalog_snapshot_uuid=catalog_uuid;
      profile.catalog_generation=identity.row.catalog_generation;
      profile.descriptor_generation=identity.row.descriptor_generation;
      profile.codec_id=identity.row.codec_id;profile.codec_version=identity.row.codec_version;
      profile.codec_generation=identity.row.codec_generation;profile.nullable=false;
      profile.profile_binding_sha256=
          scratchbird::engine::sblr::ComputeSblrLiteralDescriptorProfileBindingV1(
              profile,receipt->view.security_epoch,receipt->view.resource_epoch);
      auto sblp=scratchbird::engine::sblr::EncodeSblrLiteralDescriptorProfileV1(profile);
      if(sblp.empty()){
        return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                           "DATATYPE.DESCRIPTOR_INVALID",
                           "sblr.literal_prebind.profile_encoding_failed");
      }
      response.mappings.push_back({demand.occurrence_id,std::move(sblp)});
    }
  }
  response.ordered_profile_sha256=
      scratchbird::engine::sblr::ComputeSblrLiteralOrderedProfilesSha256V1(response.mappings);
  auto encoded=scratchbird::engine::sblr::EncodeSblrLiteralPrebindResultV1(response);
  if(encoded.empty()){
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_prebind.result_encoding_failed");
  }
  receipt->literal_prebind_negotiated=true;
  receipt->literal_demand_sha256=response.demand_sha256;
  receipt->literal_ordered_profile_sha256=response.ordered_profile_sha256;
  receipt->literal_canonical_sblq=encoded;
  receipt->literal_demands=decoded.request.demands;
  *out_canonical_sblq=std::move(encoded);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t FinalizeStatementLiteralBindingV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sblf,
    std::vector<std::uint8_t>* out_canonical_sbla,
    sb_engine_result_t* out_result){
  clear_result(out_result);if(!receipt_handle||out_canonical_sbla==nullptr)return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,"SBLR.OPERAND_INVALID","sblr.literal_finalize.request_invalid");
  scratchbird::engine::sblr::SblrLiteralFinalizeRequestV1 request;
  if(!scratchbird::engine::sblr::DecodeSblrLiteralFinalizeRequestV1(
         canonical_sblf.data(),canonical_sblf.size(),&request))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.structural_or_sbba_invalid");
  scratchbird::engine::sblr::SblrLiteralBoundAstV1 bound;
  if(!scratchbird::engine::sblr::DecodeSblrLiteralBoundAstV1(
         request.canonical_sbba.data(),request.canonical_sbba.size(),&bound))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.sbba_invalid");
  const auto uuid_bytes=[](const std::string& text,
                           std::array<std::uint8_t,16>* out){
    const auto parsed=scratchbird::core::uuid::ParseUuid(text);
    if(!parsed.ok()||out==nullptr)return false;
    std::copy(parsed.value.bytes.begin(),parsed.value.bytes.end(),out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live=g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if(live==g_live_statement_context_receipts.end())
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,out_result,4075,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_finalize.receipt_stale");
  auto* receipt=live->second.get();std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t,16> preliminary{},mga{};
  if(!uuid_bytes(receipt->view.receipt_uuid,&preliminary)||
     !uuid_bytes(receipt->view.statement_snapshot_uuid,&mga)||
     receipt->released||!receipt->literal_prebind_negotiated||
     receipt->literal_binding_finalized||
     request.preliminary_receipt_uuid!=preliminary||
     bound.preliminary_receipt_uuid!=preliminary||
     request.demand_sha256!=receipt->literal_demand_sha256||
     bound.demand_sha256!=receipt->literal_demand_sha256||
     request.ordered_profile_sha256!=receipt->literal_ordered_profile_sha256||
     request.catalog_generation!=receipt->view.literal_catalog_generation||
     request.security_epoch!=receipt->view.security_epoch||
     request.resource_epoch!=receipt->view.resource_epoch||
     request.mga_snapshot_uuid!=mga||
     bound.nodes.size()!=receipt->literal_demands.size())
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_finalize.live_binding_mismatch");
  for(std::size_t index=0;index<bound.nodes.size();++index){
    const auto& node=bound.nodes[index];const auto demand_it=std::find_if(
        receipt->literal_demands.begin(),receipt->literal_demands.end(),
        [&](const auto& candidate){return candidate.occurrence_id==node.occurrence_id;});
    if(demand_it==receipt->literal_demands.end()||
       node.lexical_sha256!=demand_it->lexical_sha256||
       node.nullable!=demand_it->nullable)
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
                         "SBLR.OPERAND_INVALID",
                         "sblr.literal_finalize.demand_sbba_bijection_failed");
    const auto identity=scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
        receipt->view.literal_catalog_snapshot_uuid,
        receipt->view.literal_catalog_generation,
        receipt->view.literal_registry_generation,
        "019d0000-0000-7000-8000-00000000d711",1);
    std::array<std::uint8_t,16> type{};
    if(!identity.ok||!uuid_bytes(identity.row.type_uuid,&type)||
       node.descriptor_generation!=identity.row.descriptor_generation||
       node.type_uuid!=type)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.registry_sbba_mismatch");
  }
  const auto table=scratchbird::engine::sblr::DecodeSblrExpressionNodeTableV1(
      request.canonical_sbxn.data(),request.canonical_sbxn.size());
  if((request.canonical_sbxn.empty()&&!bound.nodes.empty())||
     (!request.canonical_sbxn.empty()&&
      (!table.ok||table.table.nodes.size()!=bound.nodes.size())))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.sbba_sbxn_count_mismatch");
  std::size_t mapping_offset=160;
  for(std::size_t index=0;index<bound.nodes.size();++index){
    const auto& ast=bound.nodes[index];const auto node_it=std::find_if(
        table.table.nodes.begin(),table.table.nodes.end(),
        [&](const auto& candidate){return candidate.node_id==ast.node_id;});
    if(node_it==table.table.nodes.end())return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
        "SBLR.OPERAND_INVALID","sblr.literal_finalize.sbxn_node_missing");
    const auto& node=*node_it;
    const auto& sblq=receipt->literal_canonical_sblq;
    if(mapping_offset>sblq.size()||sblq.size()-mapping_offset<12)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.profile_mapping_truncated");
    const auto occurrence=scratchbird::engine::SblrReadU64(sblq.data()+mapping_offset);
    const auto profile_bytes=scratchbird::engine::SblrReadU32(sblq.data()+mapping_offset+8);
    mapping_offset+=12;
    if(profile_bytes>sblq.size()-mapping_offset)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.profile_mapping_extent_invalid");
    const auto profile=scratchbird::engine::sblr::DecodeSblrLiteralDescriptorProfileV1(
        sblq.data()+mapping_offset,profile_bytes);mapping_offset+=profile_bytes;
    const auto identity=scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(
        receipt->view.literal_catalog_snapshot_uuid,
        receipt->view.literal_catalog_generation,
        receipt->view.literal_registry_generation,
        "019d0000-0000-7000-8000-00000000d711",1);
    std::array<std::uint8_t,16> core_descriptor{},core_type{};
    if(!profile.ok||occurrence!=ast.occurrence_id||
       profile.profile.profile_uuid!=ast.profile_uuid||
       profile.profile.profile_uuid!=ast.descriptor_uuid||
       !identity.ok||
       !uuid_bytes(identity.row.descriptor_uuid,&core_descriptor)||
       !uuid_bytes(identity.row.type_uuid,&core_type)||
       profile.profile.descriptor_uuid!=core_descriptor||
       profile.profile.descriptor_generation!=ast.descriptor_generation||
       profile.profile.type_uuid!=core_type||
       profile.profile.type_uuid!=ast.type_uuid||
       profile.profile.profile_binding_sha256!=
         scratchbird::engine::sblr::ComputeSblrLiteralDescriptorProfileBindingV1(
             profile.profile,receipt->view.security_epoch,receipt->view.resource_epoch)||
       node.node_id!=ast.node_id||
       node.parent_operand_ordinal!=ast.parent_operand_ordinal||
       node.descriptor_uuid!=ast.descriptor_uuid||
       node.descriptor_generation!=ast.descriptor_generation)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_finalize.profile_sbba_sbxn_bijection_failed");
  }
  if(mapping_offset!=receipt->literal_canonical_sblq.size())
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4075,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_finalize.extra_profile_mapping");
  std::unordered_set<std::string> issued{receipt->view.receipt_uuid};
  std::string final_uuid,token_uuid;
  if(!generate_distinct_statement_context_uuid(&issued,&final_uuid)||
     !generate_distinct_statement_context_uuid(&issued,&token_uuid))
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4076,
                       "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                       "sblr.literal_finalize.identity_unavailable");
  scratchbird::engine::sblr::SblrLiteralAdmissionV1 admission;
  admission.preliminary_receipt_uuid=preliminary;
  if(!uuid_bytes(final_uuid,&admission.final_receipt_uuid)||
     !uuid_bytes(token_uuid,&admission.admission_token_uuid))
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4076,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_finalize.identity_invalid");
  admission.demand_sha256=request.demand_sha256;
  admission.ordered_profile_sha256=request.ordered_profile_sha256;
  admission.bound_ast_sha256=request.bound_ast_sha256;
  admission.sbxn_sha256=request.sbxn_sha256;
  admission.catalog_generation=request.catalog_generation;
  admission.security_epoch=request.security_epoch;
  admission.resource_epoch=request.resource_epoch;
  admission.mga_snapshot_uuid=request.mga_snapshot_uuid;
  auto encoded=scratchbird::engine::sblr::EncodeSblrLiteralAdmissionV1(&admission);
  if(encoded.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4076,
      "DATATYPE.DESCRIPTOR_INVALID","sblr.literal_finalize.admission_encoding_failed");
  const auto executor_snapshot=scratchbird::engine::internal_api::
      LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context);
  if(!executor_snapshot.ok||!executor_snapshot.snapshot.installed||
     executor_snapshot.snapshot.availability_state!=scratchbird::engine::internal_api::
         SblrExecutorAvailabilityState::installed)
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4076,
      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
      "sblr.literal_finalize.executor_evidence_missing");
  receipt->literal_binding_finalized=true;
  receipt->literal_bound_ast_sha256=request.bound_ast_sha256;
  receipt->literal_sbxn_sha256=request.sbxn_sha256;
  receipt->literal_final_receipt_uuid=final_uuid;
  receipt->literal_admission_token_uuid=token_uuid;
  receipt->literal_admission_token_binding_sha256=admission.admission_token_binding_sha256;
  receipt->literal_executor_availability_snapshot=executor_snapshot.snapshot;
  *out_canonical_sbla=std::move(encoded);return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t NegotiateStatementParameterDescriptorsV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbpr,
    std::vector<std::uint8_t>* out_canonical_sbpg,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sbpg == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4080,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.request_invalid");
  }
  out_canonical_sbpg->clear();
  const auto prevalidated = scratchbird::engine::sblr::
      PrevalidateSblrParameterNegotiateRequestV1(canonical_sbpr.data(),
                                                  canonical_sbpr.size());
  if (prevalidated == scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::resource_exceeded) {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    const auto live = g_live_statement_context_receipts.find(
        receipt_handle.opaque_id);
    if (live == g_live_statement_context_receipts.end()) {
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4080,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_negotiate.receipt_stale");
    }
    auto* receipt = live->second.get();
    std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
    const auto receipt_uuid = scratchbird::core::uuid::ParseUuid(
        receipt->view.receipt_uuid);
    const auto mga_uuid = scratchbird::core::uuid::ParseUuid(
        receipt->view.statement_snapshot_uuid);
    if (receipt->released || receipt->parameter_prebind_negotiated ||
        !receipt_uuid.ok() || !mga_uuid.ok() ||
        !std::equal(receipt_uuid.value.bytes.begin(),
                    receipt_uuid.value.bytes.end(), canonical_sbpr.begin()+16) ||
        scratchbird::engine::SblrReadU64(canonical_sbpr.data()+32) !=
            receipt->view.literal_catalog_generation ||
        scratchbird::engine::SblrReadU64(canonical_sbpr.data()+40) !=
            receipt->view.security_epoch ||
        scratchbird::engine::SblrReadU64(canonical_sbpr.data()+48) !=
            receipt->view.resource_epoch ||
        !std::equal(mga_uuid.value.bytes.begin(), mga_uuid.value.bytes.end(),
                    canonical_sbpr.begin()+56)) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4080,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_negotiate.live_binding_mismatch");
    }
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4080,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_negotiate.resource_budget_exceeded");
  }
  if (prevalidated != scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::ok) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4080,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.structural_invalid");
  }
  scratchbird::engine::sblr::SblrParameterNegotiateRequestV1 request;
  std::string decode_detail;
  if (!scratchbird::engine::sblr::DecodeSblrParameterNegotiateRequestV1(
          canonical_sbpr.data(), canonical_sbpr.size(), &request,
          &decode_detail)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4080,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.structural_invalid",
                       decode_detail);
  }
  const auto uuid_bytes = [](const std::string& text,
                             std::array<std::uint8_t, 16>* out) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok() || out == nullptr) return false;
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4081,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_negotiate.receipt_stale");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t, 16> receipt_uuid{}, mga_uuid{};
  if (receipt->released || receipt->parameter_prebind_negotiated ||
      !uuid_bytes(receipt->view.receipt_uuid, &receipt_uuid) ||
      !uuid_bytes(receipt->view.statement_snapshot_uuid, &mga_uuid) ||
      request.preliminary_receipt_uuid != receipt_uuid ||
      request.mga_snapshot_uuid != mga_uuid ||
      request.catalog_generation != receipt->view.literal_catalog_generation ||
      request.security_epoch != receipt->view.security_epoch ||
      request.resource_epoch != receipt->view.resource_epoch) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4081,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_negotiate.live_binding_mismatch");
  }
  if (request.demands.empty()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4081,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.empty_demand_forbidden");
  }
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
      parameter_executor_identity;
  parameter_executor_identity.executor_id =
      scratchbird::engine::internal_api::kSblrParameterExecutorId;
  parameter_executor_identity.opcode_code =
      scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
  parameter_executor_identity.opcode_version =
      scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
  parameter_executor_identity.operand_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
  parameter_executor_identity.result_descriptor_id =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
  parameter_executor_identity.result_descriptor_version =
      scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
      current_parameter_executor;
  const auto parameter_executor = scratchbird::engine::internal_api::
      RevalidateSblrExecutorAvailability(
          receipt->engine_context, parameter_executor_identity,
          receipt->parameter_executor_availability_snapshot,
          &current_parameter_executor);
  if (parameter_executor.error ||
      current_parameter_executor.generation !=
          receipt->view.parameter_executor_availability_generation) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4081,
                       parameter_executor.error
                           ? parameter_executor.code
                           : "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
                       "sblr.parameter_negotiate.executor_unavailable",
                       parameter_executor.detail);
  }
  const auto identity = scratchbird::core::datatypes::
      LookupDatatypeTypeCodecIdentityV1(
          receipt->view.literal_catalog_snapshot_uuid,
          receipt->view.literal_catalog_generation,
          receipt->view.literal_registry_generation,
          "019d0000-0000-7000-8000-00000000d711", 1);
  if (!identity.ok) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4082,
                       identity.diagnostic_id,
                       "sblr.parameter_negotiate.demand_registry_lookup_failed");
  }
  scratchbird::engine::internal_api::SblrParameterSetIssueRequest issue;
  issue.statement_receipt_uuid = receipt->view.receipt_uuid;
  if (!receipt->view.parameter_prepared_statement_uuid.empty()) {
    issue.execution_uuid = receipt->parameter_coordination_operation_uuid;
  } else {
    std::unordered_set<std::string> issued{receipt->view.receipt_uuid};
    (void)generate_distinct_statement_context_uuid(&issued,
                                                    &issue.execution_uuid);
  }
  if (issue.execution_uuid.empty()) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                       "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                       "sblr.parameter_negotiate.execution_identity_unavailable");
  }
  issue.reason_code = "parameter_descriptor_negotiation_v1";
  issue.prepared_statement_uuid =
      receipt->view.parameter_prepared_statement_uuid;
  issue.prepared_generation = receipt->view.parameter_prepared_generation;
  issue.batch_uuid = receipt->view.parameter_batch_uuid;
  issue.batch_generation = receipt->view.parameter_batch_generation;
  issue.dynamic_package_uuid =
      receipt->view.parameter_dynamic_package_uuid;
  issue.dynamic_generation = receipt->view.parameter_dynamic_generation;
  for (const auto& demand : request.demands) {
    if (demand.context_code != 1 || demand.requested_direction != 1 ||
        demand.nullable_demand > 1 || demand.marker_ordinal == 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4082,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_negotiate.demand_code_unregistered");
    }
    issue.slots.push_back({identity.row.descriptor_uuid,
                           identity.row.descriptor_generation,
                           scratchbird::engine::internal_api::
                               SblrParameterDirection::in,
                           demand.nullable_demand == 1});
  }
  const auto issued_set = scratchbird::engine::internal_api::
      IssueSblrParameterSet(receipt->engine_context, issue);
  if (!issued_set.ok) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4082,
                       issued_set.diagnostic.code,
                       issued_set.diagnostic.message_key,
                       issued_set.diagnostic.detail);
  }
  scratchbird::engine::sblr::SblrParameterNegotiateResultV1 response;
  if (!uuid_bytes(receipt->view.receipt_uuid,
                  &response.preliminary_receipt_uuid) ||
      !uuid_bytes(issued_set.snapshot.parameter_set_descriptor_uuid,
                  &response.parameter_set_descriptor_uuid) ||
      !uuid_bytes(issued_set.snapshot.execution_uuid,
                  &response.execution_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.parameter_negotiate.issued_identity_invalid");
  }
  response.descriptor_generation = issued_set.snapshot.descriptor_generation;
  for (std::size_t i = 0; i < request.demands.size(); ++i) {
    const auto& slot = issued_set.snapshot.slots[i];
    scratchbird::engine::sblr::SblrParameterMappingV1 mapping;
    mapping.occurrence_id = request.demands[i].occurrence_id;
    mapping.slot_ordinal = slot.slot_ordinal;
    if (!uuid_bytes(slot.slot_uuid, &mapping.slot_uuid) ||
        !uuid_bytes(slot.datatype_descriptor_uuid,
                    &mapping.datatype_descriptor_uuid) ||
        !uuid_bytes(identity.row.type_uuid, &mapping.datatype_type_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.parameter_negotiate.slot_identity_invalid");
    }
    mapping.datatype_descriptor_generation =
        slot.datatype_descriptor_generation;
    mapping.direction = static_cast<std::uint8_t>(slot.direction);
    mapping.nullable = slot.nullable ? 1 : 0;
    response.mappings.push_back(mapping);
  }
  response.mapping_sha256 =
      scratchbird::engine::sblr::ComputeSblrParameterMappingSha256V1(
          response.mappings);
  auto encoded = scratchbird::engine::sblr::
      EncodeSblrParameterNegotiateResultV1(response);
  if (encoded.empty()) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4082,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_negotiate.result_encoding_failed");
  }
  receipt->parameter_prebind_negotiated = true;
  receipt->parameter_demand_sha256 = request.demand_sha256;
  receipt->parameter_mapping_sha256 = response.mapping_sha256;
  receipt->parameter_demands = request.demands;
  receipt->parameter_set_snapshot = issued_set.snapshot;
  *out_canonical_sbpg = std::move(encoded);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t FinalizeStatementParameterBindingV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbpf,
    std::vector<std::uint8_t>* out_canonical_sbpa,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sbpa == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4083,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.request_invalid");
  }
  out_canonical_sbpa->clear();
  const auto prevalidated = scratchbird::engine::sblr::
      PrevalidateSblrParameterFinalizeRequestV1(canonical_sbpf.data(),
                                                 canonical_sbpf.size());
  if (prevalidated == scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::resource_exceeded) {
    std::lock_guard<std::mutex> registry_guard(
        g_statement_context_receipt_registry_mutex);
    const auto live = g_live_statement_context_receipts.find(
        receipt_handle.opaque_id);
    if (live == g_live_statement_context_receipts.end()) {
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4083,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_finalize.receipt_stale");
    }
    auto* receipt = live->second.get();
    std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
    const auto preliminary = scratchbird::core::uuid::ParseUuid(
        receipt->view.receipt_uuid);
    const auto descriptor = scratchbird::core::uuid::ParseUuid(
        receipt->parameter_set_snapshot.parameter_set_descriptor_uuid);
    const auto execution = scratchbird::core::uuid::ParseUuid(
        receipt->parameter_set_snapshot.execution_uuid);
    if (receipt->released || !receipt->parameter_prebind_negotiated ||
        receipt->parameter_binding_finalized || !preliminary.ok() ||
        !descriptor.ok() || !execution.ok() ||
        !std::equal(preliminary.value.bytes.begin(), preliminary.value.bytes.end(),
                    canonical_sbpf.begin()+16) ||
        !std::equal(descriptor.value.bytes.begin(), descriptor.value.bytes.end(),
                    canonical_sbpf.begin()+32) ||
        scratchbird::engine::SblrReadU64(canonical_sbpf.data()+48) !=
            receipt->parameter_set_snapshot.descriptor_generation ||
        !std::equal(execution.value.bytes.begin(), execution.value.bytes.end(),
                    canonical_sbpf.begin()+56) ||
        !std::equal(receipt->parameter_demand_sha256.begin(),
                    receipt->parameter_demand_sha256.end(),
                    canonical_sbpf.begin()+72) ||
        !std::equal(receipt->parameter_mapping_sha256.begin(),
                    receipt->parameter_mapping_sha256.end(),
                    canonical_sbpf.begin()+104)) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4083,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_finalize.live_binding_mismatch");
    }
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4083,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.parameter_finalize.resource_budget_exceeded");
  }
  if (prevalidated != scratchbird::engine::sblr::
                          SblrParameterWirePrevalidationV1::ok) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4083,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.structural_invalid");
  }
  scratchbird::engine::sblr::SblrParameterFinalizeRequestV1 request;
  std::string decode_detail;
  if (!scratchbird::engine::sblr::DecodeSblrParameterFinalizeRequestV1(
          canonical_sbpf.data(), canonical_sbpf.size(), &request,
          &decode_detail)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4083,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.structural_invalid",
                       decode_detail);
  }
  const auto uuid_bytes = [](const std::string& text,
                             std::array<std::uint8_t, 16>* out) {
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (!parsed.ok() || out == nullptr) return false;
    std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), out->begin());
    return true;
  };
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4084,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_finalize.receipt_stale");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  std::array<std::uint8_t, 16> preliminary{}, descriptor{}, execution{};
  const auto& snapshot = receipt->parameter_set_snapshot;
  if (receipt->released || !receipt->parameter_prebind_negotiated ||
      receipt->parameter_binding_finalized ||
      !uuid_bytes(receipt->view.receipt_uuid, &preliminary) ||
      !uuid_bytes(snapshot.parameter_set_descriptor_uuid, &descriptor) ||
      !uuid_bytes(snapshot.execution_uuid, &execution) ||
      request.preliminary_receipt_uuid != preliminary ||
      request.parameter_set_descriptor_uuid != descriptor ||
      request.execution_uuid != execution ||
      request.descriptor_generation != snapshot.descriptor_generation ||
      request.demand_sha256 != receipt->parameter_demand_sha256 ||
      request.mapping_sha256 != receipt->parameter_mapping_sha256) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4084,
                       "SBLR.PARAMETER.STALE",
                       "sblr.parameter_finalize.live_binding_mismatch");
  }
  const auto table = scratchbird::engine::sblr::DecodeSblrParameterNodeTableV1(
      request.canonical_sbpn.data(), request.canonical_sbpn.size());
  std::vector<std::uint8_t> sbpn_hash_material;
  constexpr std::string_view kSbpnHashDomain =
      "ScratchBird.SblrParameterNodeTable.V1";
  sbpn_hash_material.insert(sbpn_hash_material.end(), kSbpnHashDomain.begin(),
                            kSbpnHashDomain.end());
  sbpn_hash_material.insert(sbpn_hash_material.end(), request.canonical_sbpn.begin(),
                            request.canonical_sbpn.end());
  const auto sbpn_hash = scratchbird::core::hash::ComputeSha256Digest(
      sbpn_hash_material.data(), sbpn_hash_material.size());
  if (!table.ok || table.table.nodes.size() != snapshot.slots.size() ||
      !sbpn_hash.ok() || request.sbpn_sha256 != sbpn_hash.digest) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4084,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.sbpn_invalid");
  }
  std::vector<bool> seen_slots(snapshot.slots.size(), false);
  for (const auto& node : table.table.nodes) {
    if (node.slot_ordinal >= snapshot.slots.size() ||
        seen_slots[node.slot_ordinal]) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4084,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_finalize.slot_bijection_invalid");
    }
    seen_slots[node.slot_ordinal] = true;
    const auto& slot = snapshot.slots[node.slot_ordinal];
    std::array<std::uint8_t, 16> slot_descriptor{}, datatype{};
    if (!uuid_bytes(snapshot.parameter_set_descriptor_uuid, &slot_descriptor) ||
        !uuid_bytes(slot.datatype_descriptor_uuid, &datatype) ||
        node.slot_ordinal != slot.slot_ordinal ||
        node.parameter_set_descriptor_uuid != slot_descriptor ||
        node.parameter_set_generation != snapshot.descriptor_generation ||
        node.datatype_descriptor_uuid != datatype ||
        node.datatype_descriptor_generation !=
            slot.datatype_descriptor_generation) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4084,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_finalize.sbpn_registry_mismatch");
    }
  }
  scratchbird::engine::internal_api::SblrParameterSetSnapshot current;
  const auto revalidated = scratchbird::engine::internal_api::
      RevalidateSblrParameterSet(
          receipt->engine_context, snapshot, receipt->view.receipt_uuid,
          snapshot.execution_uuid, snapshot.prepared_statement_uuid,
          snapshot.prepared_generation, snapshot.batch_uuid,
          snapshot.batch_generation, snapshot.dynamic_package_uuid,
          snapshot.dynamic_generation, &current);
  if (revalidated.code != "OK") {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4084,
                       revalidated.code, revalidated.message_key,
                       revalidated.detail);
  }
  std::unordered_set<std::string> issued{receipt->view.receipt_uuid,
                                         snapshot.execution_uuid};
  std::string final_uuid, token_uuid;
  if (!generate_distinct_statement_context_uuid(&issued, &final_uuid) ||
      !generate_distinct_statement_context_uuid(&issued, &token_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                       "sblr.parameter_finalize.identity_unavailable");
  }
  scratchbird::engine::sblr::SblrParameterAdmissionV1 admission;
  if (!uuid_bytes(final_uuid, &admission.final_receipt_uuid) ||
      !uuid_bytes(token_uuid, &admission.admission_token_uuid) ||
      !uuid_bytes(snapshot.parameter_set_descriptor_uuid,
                  &admission.parameter_set_descriptor_uuid) ||
      !uuid_bytes(snapshot.execution_uuid, &admission.execution_uuid)) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.identity_invalid");
  }
  admission.descriptor_generation = snapshot.descriptor_generation;
  if ((!snapshot.prepared_statement_uuid.empty() &&
       !uuid_bytes(snapshot.prepared_statement_uuid, &admission.prepared_uuid)) ||
      (!snapshot.batch_uuid.empty() &&
       !uuid_bytes(snapshot.batch_uuid, &admission.batch_uuid)) ||
      (!snapshot.dynamic_package_uuid.empty() &&
       !uuid_bytes(snapshot.dynamic_package_uuid, &admission.dynamic_uuid))) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.route_identity_invalid");
  }
  admission.prepared_generation = snapshot.prepared_generation;
  admission.batch_generation = snapshot.batch_generation;
  admission.dynamic_generation = snapshot.dynamic_generation;
  auto encoded =
      scratchbird::engine::sblr::EncodeSblrParameterAdmissionV1(&admission);
  if (encoded.empty()) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4085,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_finalize.admission_encoding_failed");
  }
  receipt->parameter_binding_finalized = true;
  receipt->parameter_sbpn_sha256 = request.sbpn_sha256;
  receipt->parameter_final_receipt_uuid = final_uuid;
  receipt->parameter_admission_token_uuid = token_uuid;
  receipt->parameter_admission_binding_sha256 = admission.binding_sha256;
  receipt->parameter_set_snapshot = current;
  *out_canonical_sbpa = std::move(encoded);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t FinalizeStatementVariableBindingV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbvf,
    std::vector<std::uint8_t>* out_canonical_sbva,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sbva == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4090,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_finalize.request_invalid");
  }
  out_canonical_sbva->clear();
  scratchbird::engine::sblr::SblrVariableFinalizeRequestV1 request;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrVariableFinalizeRequestV1(
          canonical_sbvf.data(), canonical_sbvf.size(), &request, &detail)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4090,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_finalize.structural_invalid", detail);
  }
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(
      receipt_handle.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4090,
                       "SBLR.VARIABLE.STALE",
                       "sblr.variable_finalize.receipt_stale");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  if (receipt->released || receipt->variable_binding_finalized ||
      !receipt->variable_frame_snapshot.has_value()) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4090,
                       "SBLR.VARIABLE.STALE",
                       "sblr.variable_finalize.state_stale");
  }
  const auto uuid_bytes = [](const std::string& text) {
    std::array<std::uint8_t, 16> bytes{};
    const auto parsed = scratchbird::core::uuid::ParseUuid(text);
    if (parsed.ok()) std::copy(parsed.value.bytes.begin(),
                               parsed.value.bytes.end(), bytes.begin());
    return bytes;
  };
  const auto& frame = *receipt->variable_frame_snapshot;
  if (request.preliminary_receipt_uuid != uuid_bytes(receipt->view.receipt_uuid) ||
      request.scope_uuid != uuid_bytes(frame.scope_uuid) ||
      request.scope_generation != frame.scope_generation ||
      request.frame_uuid != uuid_bytes(frame.frame_uuid) ||
      request.frame_generation != frame.frame_generation ||
      request.registry_generation != frame.registry_generation) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4090,
                       "SBLR.VARIABLE.STALE",
                       "sblr.variable_finalize.frame_snapshot_mismatch");
  }
  const auto table = scratchbird::engine::sblr::DecodeSblrVariableNodeTableV1(
      request.canonical_sbvn.data(), request.canonical_sbvn.size());
  if (!table.ok || table.table.nodes.size() != frame.mappings.size()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4090,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_finalize.sbvn_invalid");
  }
  for (const auto& node : table.table.nodes) {
    if (node.parent_operand_ordinal == 0 ||
        node.parent_operand_ordinal > frame.mappings.size()) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4090,
                         "SBLR.OPERAND_INVALID",
                         "sblr.variable_finalize.ordinal_invalid");
    }
    const auto& mapping = frame.mappings[node.parent_operand_ordinal - 1];
    const auto& row = mapping.descriptor;
    if (node.scope_uuid != uuid_bytes(frame.scope_uuid) ||
        node.scope_generation != frame.scope_generation ||
        node.frame_uuid != uuid_bytes(frame.frame_uuid) ||
        node.frame_generation != frame.frame_generation ||
        node.variable_descriptor_uuid != uuid_bytes(row.variable_descriptor_uuid) ||
        node.variable_descriptor_generation != row.variable_descriptor_generation ||
        node.datatype_descriptor_uuid != uuid_bytes(row.datatype_descriptor_uuid) ||
        node.datatype_descriptor_generation != row.datatype_descriptor_generation ||
        node.value_generation != row.value_generation) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4090,
                         "SBLR.VARIABLE.STALE",
                         "sblr.variable_finalize.sbvn_registry_mismatch");
    }
  }
  scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current;
  const auto availability = scratchbird::engine::internal_api::
      RevalidateSblrExecutorAvailability(
          receipt->engine_context,
          VariableExecutorAvailabilityIdentity(),
          receipt->variable_executor_availability_snapshot, &current);
  if (availability.error || availability.code != "OK") {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4090,
                       availability.code,
                       availability.message_key,
                       availability.detail);
  }
  const auto issue_uuid = [](std::uint64_t salt) {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    const auto generated = scratchbird::core::uuid::GenerateEngineIdentityV7(
        scratchbird::core::platform::UuidKind::object, now + salt);
    return generated.ok()
        ? scratchbird::core::uuid::UuidToString(generated.value.value)
        : std::string{};
  };
  scratchbird::engine::sblr::SblrVariableAdmissionV1 admission;
  receipt->variable_final_receipt_uuid = issue_uuid(11);
  receipt->variable_admission_token_uuid = issue_uuid(12);
  admission.final_receipt_uuid = uuid_bytes(receipt->variable_final_receipt_uuid);
  admission.admission_token_uuid = uuid_bytes(receipt->variable_admission_token_uuid);
  admission.scope_uuid = request.scope_uuid;
  admission.scope_generation = request.scope_generation;
  admission.frame_uuid = request.frame_uuid;
  admission.frame_generation = request.frame_generation;
  admission.registry_snapshot_uuid = uuid_bytes(frame.registry_snapshot_uuid);
  admission.registry_generation = frame.registry_generation;
  admission.executor_availability_generation = current.generation;
  admission.expires_at_monotonic_ns = UINT64_MAX;
  *out_canonical_sbva =
      scratchbird::engine::sblr::EncodeSblrVariableAdmissionV1(&admission);
  if (out_canonical_sbva->empty()) {
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4090,
                       "SBLR.EXECUTION_FAILED",
                       "sblr.variable_finalize.encoding_failed");
  }
  receipt->variable_admission_binding_sha256 = admission.binding_sha256;
  receipt->variable_executor_availability_snapshot = current;
  receipt->variable_binding_finalized = true;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t AssignStatementVariableValuesV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_sbvy,
    std::vector<std::uint8_t>* out_canonical_sbvw,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_sbvw == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4091,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_assignment.request_invalid");
  }
  scratchbird::engine::sblr::SblrVariableAssignmentRequestV1 request;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrVariableAssignmentRequestV1(
          canonical_sbvy.data(), canonical_sbvy.size(), &request, &detail)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4091,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_assignment.structural_invalid", detail);
  }
  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if (live == g_live_statement_context_receipts.end())
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4091,
                       "SBLR.VARIABLE.STALE",
                       "sblr.variable_assignment.receipt_stale");
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  const auto uuid_bytes=[](const std::string& text){std::array<std::uint8_t,16>b{};const auto p=scratchbird::core::uuid::ParseUuid(text);if(p.ok())std::copy(p.value.bytes.begin(),p.value.bytes.end(),b.begin());return b;};
  if (receipt->released || receipt->variable_binding_finalized ||
      receipt->variable_admission_consumed ||
      !receipt->variable_frame_snapshot.has_value())
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4091,
                       "SBLR.VARIABLE.STALE","sblr.variable_assignment.state_stale");
  auto& frame=*receipt->variable_frame_snapshot;
  if(request.preliminary_receipt_uuid!=uuid_bytes(receipt->view.receipt_uuid)||
     request.public_coordination_uuid!=uuid_bytes(frame.public_coordination_uuid)||
     request.operation_uuid!=uuid_bytes(frame.operation_uuid)||
     request.scope_uuid!=uuid_bytes(frame.scope_uuid)||request.scope_generation!=frame.scope_generation||
     request.frame_uuid!=uuid_bytes(frame.frame_uuid)||request.frame_generation!=frame.frame_generation||
     request.registry_snapshot_uuid!=uuid_bytes(frame.registry_snapshot_uuid)||
     request.registry_generation!=frame.registry_generation)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4091,
                       "SBLR.VARIABLE.STALE","sblr.variable_assignment.binding_stale");
  std::vector<scratchbird::engine::internal_api::SblrVariableAssignment>
      engine_assignments;
  engine_assignments.reserve(request.assignments.size());
  for (const auto& assignment : request.assignments) {
    const auto mapping=std::find_if(frame.mappings.begin(),frame.mappings.end(),
        [&](const auto&m){return m.descriptor.variable_ordinal==assignment.variable_ordinal;});
    if(mapping==frame.mappings.end()||
       assignment.variable_descriptor_uuid!=uuid_bytes(mapping->descriptor.variable_descriptor_uuid)||
       assignment.variable_descriptor_generation!=mapping->descriptor.variable_descriptor_generation||
       assignment.expected_value_generation!=mapping->descriptor.value_generation||
       assignment.datatype_descriptor_uuid!=uuid_bytes(mapping->descriptor.datatype_descriptor_uuid)||
       assignment.datatype_descriptor_generation!=mapping->descriptor.datatype_descriptor_generation||
       (assignment.value_state==1&&assignment.canonical_value_bytes.size()!=8))
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4091,
                         "SBLR.VARIABLE.STALE","sblr.variable_assignment.descriptor_stale");
    engine_assignments.push_back({
        mapping->descriptor.variable_descriptor_uuid,
        mapping->descriptor.variable_descriptor_generation,
        mapping->descriptor.value_generation,
        assignment.value_state==1
            ? scratchbird::engine::internal_api::SblrVariableValueState::value
            : scratchbird::engine::internal_api::SblrVariableValueState::null_value,
        std::string(assignment.canonical_value_bytes.begin(),
                    assignment.canonical_value_bytes.end())});
  }
  auto variable_context=receipt->engine_context;
  variable_context.trace_tags.push_back("private_variable_registry");
  variable_context.trace_tags.push_back("canonical_datatype_value_validated");
  variable_context.trace_tags.push_back("private_variable_frame_coordination");
  const auto assigned=scratchbird::engine::internal_api::AssignSblrVariableFrameValues(
      variable_context,frame.public_coordination_uuid,frame.operation_uuid,
      frame.statement_receipt_uuid,frame.coordinator_generation,
      frame.registry_generation,engine_assignments);
  if(!assigned.ok)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4091,
                       assigned.diagnostic.code,assigned.diagnostic.message_key,
                       assigned.diagnostic.detail);
  frame=assigned.snapshot;
  scratchbird::engine::sblr::SblrVariableAssignmentResultV1 response;
  response.preliminary_receipt_uuid=request.preliminary_receipt_uuid;
  response.public_coordination_uuid=request.public_coordination_uuid;
  response.scope_uuid=request.scope_uuid;response.scope_generation=request.scope_generation;
  response.frame_uuid=request.frame_uuid;response.frame_generation=request.frame_generation;
  response.new_registry_generation=frame.registry_generation;
  for (const auto& assignment : request.assignments) {
    const auto mapping=std::find_if(frame.mappings.begin(),frame.mappings.end(),
        [&](const auto&m){return m.descriptor.variable_ordinal==assignment.variable_ordinal;});
    if(mapping==frame.mappings.end())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4091,
        "SBLR.EXECUTION_FAILED","sblr.variable_assignment.mapping_lost");
    response.results.push_back({assignment.assignment_occurrence_id,
        assignment.variable_ordinal,assignment.variable_descriptor_uuid,
        assignment.variable_descriptor_generation,mapping->descriptor.value_generation,
        mapping->descriptor.registry_generation});
  }
  *out_canonical_sbvw=scratchbird::engine::sblr::EncodeSblrVariableAssignmentResultV1(&response);
  if(out_canonical_sbvw->empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4091,
      "SBLR.EXECUTION_FAILED","sblr.variable_assignment.encoding_failed");
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t IssueStatementSourceMapDescriptorV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_smrq,
    std::vector<std::uint8_t>* out_canonical_smrs,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_smrs == nullptr)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4092,
                       "SBLR.OPERAND_INVALID", "sblr.source_map.request_invalid");
  scratchbird::engine::sblr::SblrSourceMapIssueRequestV1 request;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrSourceMapIssueRequestV1(
          canonical_smrq.data(), canonical_smrq.size(), &request, &detail))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4092,
                       "SBLR.OPERAND_INVALID", "sblr.source_map.smrq_invalid", detail);
  std::lock_guard<std::mutex> registry_guard(g_statement_context_receipt_registry_mutex);
  const auto live=g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if(live==g_live_statement_context_receipts.end())
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,out_result,4092,
                       "SBLR.SOURCE_MAP.STALE","sblr.source_map.receipt_stale");
  auto* receipt=live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  const auto uuid_bytes=[](const std::string&text){std::array<std::uint8_t,16>b{};const auto p=scratchbird::core::uuid::ParseUuid(text);if(p.ok())std::copy(p.value.bytes.begin(),p.value.bytes.end(),b.begin());return b;};
  if(receipt->released||request.statement_receipt_uuid!=uuid_bytes(receipt->view.receipt_uuid)||
     request.registry_snapshot_uuid!=uuid_bytes(receipt->view.catalog_epoch_uuid)||
     request.registry_generation!=receipt->view.literal_catalog_generation)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4092,
                       "SBLR.SOURCE_MAP.STALE","sblr.source_map.receipt_binding_stale");
  if(receipt->source_map_bound_ast_frozen&&
     receipt->source_map_bound_ast_sha256!=request.bound_ast_sha256)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4092,
                       "SBLR.SOURCE_MAP.STALE","sblr.source_map.bound_ast_stale");
  auto context=receipt->engine_context;
  context.statement_metadata_snapshot_engine_owned=true;
  context.trace_tags.push_back("private_source_map_registry");
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity availability_identity;
  availability_identity.executor_id=scratchbird::engine::internal_api::kSblrSourceMapExecutorId;
  availability_identity.opcode_code=scratchbird::engine::internal_api::kSblrSourceMapOpcodeCode;
  availability_identity.opcode_version=scratchbird::engine::internal_api::kSblrSourceMapOpcodeVersion;
  availability_identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrSourceMapOperandDescriptorId;
  availability_identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrSourceMapResultDescriptorId;
  availability_identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrSourceMapResultDescriptorVersion;
  const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(context,availability_identity);
  if(!availability.ok||!availability.snapshot.installed)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4092,
      "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.source_map.executor_unavailable");
  const auto sha=[](const std::array<std::uint8_t,32>&v){return std::string("sha256:")+scratchbird::core::hash::HexLower(v);};
  const auto issued=scratchbird::engine::internal_api::IssueSblrSourceMapDescriptorV1(
      context,receipt->view.receipt_uuid,sha(request.bound_ast_sha256),
      receipt->view.catalog_epoch_uuid,request.registry_generation,request.entries);
  if(!issued.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4092,
      issued.diagnostic.code,issued.diagnostic.message_key,issued.diagnostic.detail);
  scratchbird::engine::sblr::SblrSourceMapIssueResultV1 response;
  const auto parsed=scratchbird::core::uuid::ParseUuid(issued.snapshot.descriptor_uuid);
  if(!parsed.ok())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4092,
      "SBLR.EXECUTION_FAILED","sblr.source_map.registry_uuid_invalid");
  std::copy(parsed.value.bytes.begin(),parsed.value.bytes.end(),response.descriptor_uuid.begin());
  response.descriptor_generation=issued.snapshot.descriptor_generation;
  response.registry_generation=issued.snapshot.registry_generation;
  response.canonical_smvd=issued.snapshot.canonical_smvd;
  *out_canonical_smrs=scratchbird::engine::sblr::EncodeSblrSourceMapIssueResultV1(response);
  if(out_canonical_smrs->empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4092,
      "SBLR.EXECUTION_FAILED","sblr.source_map.smrs_encoding_failed");
  receipt->source_map_bound_ast_sha256=request.bound_ast_sha256;
  receipt->source_map_bound_ast_frozen=true;
  receipt->source_map_executor_availability_snapshot=availability.snapshot;
  receipt->source_map_descriptors.push_back(issued.snapshot);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t IssueStatementErrorVectorDescriptorV1(
    StatementContextReceiptHandle receipt_handle,
    const std::vector<std::uint8_t>& canonical_evrq,
    std::vector<std::uint8_t>* out_canonical_evrs,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (!receipt_handle || out_canonical_evrs == nullptr)
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4093,
                       "SBLR.OPERAND_INVALID", "sblr.error_vector.request_invalid");
  scratchbird::engine::sblr::SblrErrorVectorIssueRequestV1 request;
  std::string detail;
  if (!scratchbird::engine::sblr::DecodeSblrErrorVectorIssueRequestV1(
          canonical_evrq.data(), canonical_evrq.size(), &request, &detail))
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4093,
                       "SBLR.OPERAND_INVALID", "sblr.error_vector.evrq_invalid", detail);
  std::lock_guard<std::mutex> registry_guard(g_statement_context_receipt_registry_mutex);
  const auto live=g_live_statement_context_receipts.find(receipt_handle.opaque_id);
  if(live==g_live_statement_context_receipts.end())
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,out_result,4093,
                       "SBLR.ERROR_VECTOR.STALE","sblr.error_vector.receipt_stale");
  auto* receipt=live->second.get();std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  const auto bytes=[](const std::string&t){std::array<std::uint8_t,16>b{};auto p=scratchbird::core::uuid::ParseUuid(t);if(p.ok())std::copy(p.value.bytes.begin(),p.value.bytes.end(),b.begin());return b;};
  if(receipt->released||request.statement_receipt_uuid!=bytes(receipt->view.receipt_uuid)||
     request.registry_snapshot_uuid!=bytes(receipt->view.catalog_epoch_uuid)||
     request.registry_generation!=receipt->view.literal_catalog_generation||
     request.diagnostic_registry_snapshot_uuid!=bytes(receipt->view.diagnostic_registry_snapshot_uuid)||
     request.diagnostic_registry_generation!=receipt->view.diagnostic_registry_generation)
    return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4093,
                       "SBLR.ERROR_VECTOR.STALE","sblr.error_vector.receipt_binding_stale");
  auto context=receipt->engine_context;context.statement_metadata_snapshot_engine_owned=true;
  scratchbird::engine::internal_api::SblrDiagnosticIdentitySnapshotV1 frozen;
  frozen.snapshot_uuid=receipt->view.diagnostic_registry_snapshot_uuid;
  frozen.generation=receipt->view.diagnostic_registry_generation;
  const auto uuid_string=[](const std::array<std::uint8_t,16>& value){
    static constexpr char hex[]="0123456789abcdef";std::string out;
    for(std::size_t i=0;i<16;++i){if(i==4||i==6||i==8||i==10)out.push_back('-');out.push_back(hex[value[i]>>4]);out.push_back(hex[value[i]&15]);}return out;};
  for(auto& entry:request.entries){const auto id=uuid_string(entry.diagnostic_uuid);
    auto found=scratchbird::engine::internal_api::LookupSblrDiagnosticIdentityV1(context,frozen,id,entry.diagnostic_generation);
    if(!found.ok)return fail_result(found.diagnostic.code=="SECURITY.ACCESS_DENIED"?SB_ENGINE_STATUS_SECURITY_DENIED:SB_ENGINE_STATUS_CONFLICT,out_result,4093,found.diagnostic.code,found.diagnostic.message_key,found.diagnostic.detail);
    if(entry.precedence_ordinal!=found.row.precedence_ordinal||entry.severity_code!=found.row.severity_code||entry.redaction_class!=found.row.redaction_class||entry.safe_field_count>found.row.maximum_safe_field_count)
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4093,"SBLR.OPERAND_INVALID","sblr.error_vector.row_echo_mismatch");}
  context.trace_tags.push_back("private_error_vector_registry");
  scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity availability_identity;
  availability_identity.executor_id=scratchbird::engine::internal_api::kSblrErrorVectorExecutorId;
  availability_identity.opcode_code=scratchbird::engine::internal_api::kSblrErrorVectorOpcodeCode;
  availability_identity.opcode_version=scratchbird::engine::internal_api::kSblrErrorVectorOpcodeVersion;
  availability_identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrErrorVectorOperandDescriptorId;
  availability_identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrErrorVectorResultDescriptorId;
  availability_identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrErrorVectorResultDescriptorVersion;
  const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(context,availability_identity);
  if(!availability.ok||!availability.snapshot.installed)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4093,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.error_vector.executor_unavailable");
  auto issued=scratchbird::engine::internal_api::IssueSblrErrorVectorDescriptorV1(context,receipt->view.receipt_uuid,receipt->view.catalog_epoch_uuid,request.registry_generation,receipt->view.diagnostic_registry_snapshot_uuid,request.diagnostic_registry_generation,request.entries);
  if(!issued.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4093,issued.diagnostic.code,issued.diagnostic.message_key,issued.diagnostic.detail);
  scratchbird::engine::sblr::SblrErrorVectorIssueResultV1 response;response.descriptor_uuid=bytes(issued.snapshot.descriptor_uuid);response.descriptor_generation=issued.snapshot.descriptor_generation;response.registry_generation=issued.snapshot.registry_generation;response.canonical_ervd=issued.snapshot.canonical_ervd;
  *out_canonical_evrs=scratchbird::engine::sblr::EncodeSblrErrorVectorIssueResultV1(response);
  if(out_canonical_evrs->empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4093,"SBLR.EXECUTION_FAILED","sblr.error_vector.evrs_encoding_failed");
  receipt->error_vector_executor_availability_snapshot=availability.snapshot;
  receipt->error_vector_descriptors.push_back(issued.snapshot);return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t AcquireStatementPackageAdmissionReservation(
    const StatementPackageAdmissionReservationRequest* request,
    StatementPackageAdmissionReservationHandle* out_handle,
    StatementPackageAdmissionReservationView* out_view,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (request == nullptr || out_handle == nullptr || out_view == nullptr ||
      !request->receipt || request->canonical_payload_bytes == nullptr ||
      request->payload_kind != StatementSblrPayloadKind::kOpcodeStream) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.request_invalid");
  }
  *out_handle = {};
  *out_view = {};
  const auto* data = request->canonical_payload_bytes;
  const auto size = request->canonical_payload_size;
  if (size < 80 || size > scratchbird::engine::sblr::kSblrOperationMaximumBytes ||
      scratchbird::engine::SblrReadU32(data) !=
          scratchbird::engine::sblr::kSblrOpcodeStreamMagic ||
      scratchbird::engine::SblrReadU16(data + 4) != 1 ||
      scratchbird::engine::SblrReadU16(data + 6) != 0 ||
      scratchbird::engine::SblrReadU16(data + 8) != 64 ||
      scratchbird::engine::SblrReadU16(data + 10) != 0 ||
      scratchbird::engine::SblrReadU32(data + 12) != 0 ||
      scratchbird::engine::SblrReadU32(data + size - 16) !=
          scratchbird::engine::sblr::kSblrOpcodeStreamTrailerMagic ||
      scratchbird::engine::SblrReadU64(data + size - 8) != size ||
      scratchbird::engine::SblrCrc32c(data, size - 12) !=
          scratchbird::engine::SblrReadU32(data + size - 12)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.OPERAND_INVALID",
                       "sblr.package_reservation.structure_invalid");
  }
  const auto count = scratchbird::engine::SblrReadU32(data + 16);
  const auto records_size = scratchbird::engine::SblrReadU64(data + 56);
  if (count < 2 ||
      count > scratchbird::engine::sblr::kSblrOperationMaximumOperands ||
      records_size != size - 80) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.OPERAND_INVALID",
                       "sblr.package_reservation.extent_invalid");
  }
  std::size_t offset = 64;
  for (std::uint32_t index = 0; index != count; ++index) {
    if (offset > size - 16 || size - 16 - offset < 8) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                         "SBLR.OPERAND_INVALID",
                         "sblr.package_reservation.record_invalid");
    }
    const auto record_size = scratchbird::engine::SblrReadU64(data + offset);
    if (record_size > size - 16 - offset - 8) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                         "SBLR.OPERAND_INVALID",
                         "sblr.package_reservation.record_invalid");
    }
    offset += 8 + static_cast<std::size_t>(record_size);
  }
  if (offset != size - 16) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4064,
                       "SBLR.OPERAND_INVALID",
                       "sblr.package_reservation.trailing_bytes");
  }

  std::lock_guard<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live =
      g_live_statement_context_receipts.find(request->receipt.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4064,
                       "ENGINE.STATEMENT_CONTEXT.RECEIPT_NOT_LIVE",
                       "engine.statement_context.receipt_not_live");
  }
  auto* receipt = live->second.get();
  std::lock_guard<std::mutex> receipt_guard(receipt->mutex);
  auto* session = receipt->session;
  if (session == nullptr || session->package_resource_ledger == nullptr ||
      !session->package_resource_descriptor_initialized) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.package_reservation.policy_unavailable");
  }
  scratchbird::core::agents::ResourceGovernanceReservationAcquireRequest acquire;
  acquire.admission.operation_id = "engine.op.package_begin_end";
  acquire.admission.expected_family = scratchbird::core::agents::
      ResourceGovernanceFamily::kQueryMemoryArena;
  acquire.admission.descriptor = session->package_resource_descriptor;
  auto& quota = acquire.admission.requested;
  const auto max = static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max());
  if (size > max || receipt->view.optimizer_memory_budget_bytes > max - size) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.package_reservation.overflow");
  }
  quota.memory_bytes = static_cast<std::int64_t>(
      size + receipt->view.optimizer_memory_budget_bytes);
  quota.io_bytes = static_cast<std::int64_t>(size);
  quota.io_ops = quota.worker_threads = quota.backlog_items = 1;
  quota.candidate_rows = quota.cache_entries = quota.batch_rows = 1;
  quota.fragments = count;
  quota.lanes = quota.time_budget_microseconds = 1;
  acquire.owner_scope = receipt->view.receipt_uuid;
  auto reserved = session->package_resource_ledger->Acquire(std::move(acquire));
  if (!reserved.ok || !reserved.reservation_created) {
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       "sblr.package_reservation.refused",
                       reserved.diagnostic_detail);
  }
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(data, size);
  if (!digest.ok()) {
    (void)session->package_resource_ledger->Release(
        reserved.reservation.token_id);
    return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4064,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.hash_failed");
  }
  const auto id = g_next_package_admission_reservation_id.fetch_add(1);
  StatementPackageAdmissionReservationOpaque stored;
  stored.receipt_id = request->receipt.opaque_id;
  stored.session = session;
  stored.payload_kind = request->payload_kind;
  stored.payload_size = size;
  stored.record_count = count;
  stored.resource_policy_generation =
      session->package_resource_descriptor.descriptor_generation;
  stored.payload_sha256 = digest.digest;
  stored.ledger_token_id = reserved.reservation.token_id;
  const auto id_admission = ClassifyStatementPackageReservationId(
      id, g_package_admission_reservations.contains(id));
  const bool inserted =
      id_admission == StatementPackageReservationIdAdmission::kAdmitted &&
      g_package_admission_reservations.emplace(id, stored).second;
  if (!inserted) {
    const auto released = session->package_resource_ledger->Release(
        reserved.reservation.token_id,
        scratchbird::core::agents::
            ResourceGovernanceReservationReleaseReason::kRelease);
    if (!released.ok || !released.released) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4064,
                         "RESOURCE.RESERVATION_RELEASE_FAILED",
                         "sblr.package_reservation.id_failure_release_failed");
    }
    return fail_result(SB_ENGINE_STATUS_RESOURCE_EXHAUSTED, out_result, 4064,
                       "RESOURCE.BUDGET_EXCEEDED",
                       id_admission ==
                               StatementPackageReservationIdAdmission::kZeroExhausted
                           ? "sblr.package_reservation.id_exhausted"
                           : "sblr.package_reservation.id_collision");
  }
  out_handle->opaque_id = id;
  out_view->payload_kind = stored.payload_kind;
  out_view->payload_size = stored.payload_size;
  out_view->record_count = stored.record_count;
  out_view->resource_policy_generation = stored.resource_policy_generation;
  out_view->payload_sha256 = stored.payload_sha256;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t ReadStatementQueryExecuteResultHandle(
    sb_engine_result_t result,
    StatementQueryExecuteResultHandleView* out_handle) {
  if (out_handle == nullptr) return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  *out_handle = {};
  if (!valid_result(result)) return SB_ENGINE_STATUS_INVALID_HANDLE;
  std::lock_guard<std::mutex> guard(result->mutex);
  if (!result->query_execute_result_handle_validated ||
      !result->admitted_query_row_stream_renderer) {
    return SB_ENGINE_STATUS_CONFLICT;
  }
  out_handle->execution_uuid =
      result->query_execute_result_handle.execution_uuid;
  out_handle->result_set_uuid =
      result->query_execute_result_handle.result_set_uuid;
  out_handle->row_descriptor_uuid =
      result->query_execute_result_handle.row_descriptor_uuid;
  out_handle->snapshot_uuid =
      result->query_execute_result_handle.snapshot_uuid;
  if (out_handle->execution_uuid.empty() || out_handle->result_set_uuid.empty() ||
      out_handle->row_descriptor_uuid.empty() || out_handle->snapshot_uuid.empty()) {
    *out_handle = {};
    return SB_ENGINE_STATUS_CONFLICT;
  }
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t ReleaseStatementPackageAdmissionReservation(
    StatementPackageAdmissionReservationHandle handle,
    StatementPackageReservationReleaseReason reason) {
  if (!handle) return SB_ENGINE_STATUS_INVALID_HANDLE;
  std::lock_guard<std::mutex> guard(g_statement_context_receipt_registry_mutex);
  const auto found = g_package_admission_reservations.find(handle.opaque_id);
  if (found == g_package_admission_reservations.end()) {
    return SB_ENGINE_STATUS_ALREADY_RELEASED;
  }
  const auto map_reason = [&] {
    using R = scratchbird::core::agents::ResourceGovernanceReservationReleaseReason;
    switch (reason) {
      case StatementPackageReservationReleaseReason::kCancel: return R::kCancel;
      case StatementPackageReservationReleaseReason::kTimeout: return R::kTimeout;
      case StatementPackageReservationReleaseReason::kDisconnect: return R::kDisconnect;
      case StatementPackageReservationReleaseReason::kShutdown: return R::kShutdown;
      default: return R::kRelease;
    }
  }();
  auto stored = std::move(found->second);
  g_package_admission_reservations.erase(found);
  if (stored.session == nullptr || stored.session->package_resource_ledger == nullptr)
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  const auto released = stored.session->package_resource_ledger->Release(
      stored.ledger_token_id, map_reason);
  return released.ok && released.released ? SB_ENGINE_STATUS_OK
                                          : SB_ENGINE_STATUS_INVALID_HANDLE;
}

sb_engine_status_t DispatchStatementContextReceipt(
    const StatementContextDispatchRequest* request,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (request == nullptr || !request->receipt || out_result == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4050,
        "ENGINE.STATEMENT_CONTEXT.DISPATCH_REQUEST_INVALID",
        "engine.statement_context.dispatch_request_invalid");
  }
  if (!request->data_packet.empty()) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4051,
        "SBLR.DATA_PACKET.OPERATION_MISMATCH",
        "sblr.data_packet.operation_mismatch",
        "receipt-bound operation or opcode-stream dispatch does not accept an out-of-band data packet");
  }

  std::unique_lock<std::mutex> registry_guard(
      g_statement_context_receipt_registry_mutex);
  const auto live = g_live_statement_context_receipts.find(
      request->receipt.opaque_id);
  if (live == g_live_statement_context_receipts.end()) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_HANDLE,
        out_result,
        4052,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_NOT_LIVE",
        "engine.statement_context.receipt_not_live");
  }
  auto* receipt = live->second.get();
  std::unique_lock<std::mutex> receipt_guard(receipt->mutex);
  StatementPackageAdmissionReservationOpaque consumed_reservation;
  if (request->package_admission_reservation) {
    const auto reserved = g_package_admission_reservations.find(
        request->package_admission_reservation.opaque_id);
    if (reserved == g_package_admission_reservations.end() ||
        reserved->second.receipt_id != request->receipt.opaque_id) {
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4053,
                         "SBLR.INGRESS_REVALIDATION_FAILED",
                         "sblr.package_reservation.missing_or_replayed");
    }
    consumed_reservation = std::move(reserved->second);
    g_package_admission_reservations.erase(reserved);
  }
  registry_guard.unlock();
  using ResourceReleaseReason = scratchbird::core::agents::
      ResourceGovernanceReservationReleaseReason;
  struct PackageResourceReservationGuard {
    scratchbird::core::agents::ResourceGovernanceReservationLedger* ledger;
    std::string token_id;
    ResourceReleaseReason reason = ResourceReleaseReason::kRelease;
    ~PackageResourceReservationGuard() {
      if (ledger != nullptr && !token_id.empty()) {
        (void)ledger->Release(token_id, reason);
      }
    }
  } resource_guard{
      consumed_reservation.session == nullptr
          ? nullptr
          : consumed_reservation.session->package_resource_ledger.get(),
      std::move(consumed_reservation.ledger_token_id)};
  if (receipt->released || receipt->magic != kStatementContextReceiptMagic ||
      (request->engine_session != nullptr &&
       request->engine_session != receipt->session)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_HANDLE,
        out_result,
        4053,
        "ENGINE.STATEMENT_CONTEXT.RECEIPT_MISMATCH",
        "engine.statement_context.receipt_mismatch");
  }

  const auto& view = receipt->view;
  const auto& context = receipt->engine_context;
  const bool package_candidate =
      request->admitted_payload_kind == StatementSblrPayloadKind::kOpcodeStream;
  if (package_candidate &&
      (!request->package_admission_reservation ||
       consumed_reservation.payload_kind != request->admitted_payload_kind ||
       consumed_reservation.payload_size !=
           request->canonical_operation_bytes.size())) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4053,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.classification_mismatch");
  }
  std::uint32_t structurally_bounded_record_count = 0;
  if (package_candidate) {
    // Allocation-safe SBOS prevalidation. No payload-controlled allocation or
    // semantic SBOP decode is permitted before the session reservation.
    const auto& bytes = request->canonical_operation_bytes;
    const auto read_u16 = [&](std::size_t offset, std::uint16_t* value) {
      if (offset > bytes.size() || bytes.size() - offset < 2) return false;
      *value = static_cast<std::uint16_t>(bytes[offset]) |
               (static_cast<std::uint16_t>(bytes[offset + 1]) << 8);
      return true;
    };
    const auto read_u32 = [&](std::size_t offset, std::uint32_t* value) {
      if (offset > bytes.size() || bytes.size() - offset < 4) return false;
      *value = 0;
      for (unsigned n = 0; n != 4; ++n) {
        *value |= static_cast<std::uint32_t>(bytes[offset + n]) << (n * 8);
      }
      return true;
    };
    const auto read_u64 = [&](std::size_t offset, std::uint64_t* value) {
      if (offset > bytes.size() || bytes.size() - offset < 8) return false;
      *value = 0;
      for (unsigned n = 0; n != 8; ++n) {
        *value |= static_cast<std::uint64_t>(bytes[offset + n]) << (n * 8);
      }
      return true;
    };
    std::uint32_t magic = 0, flags = 0, count = 0, reserved = 0;
    std::uint32_t trailer_magic = 0, trailer_crc = 0;
    std::uint16_t major = 0, minor = 0, header_size = 0, reserved16 = 0;
    std::uint64_t records_size = 0, total_size = 0;
    bool bounded = bytes.size() >=
                       scratchbird::engine::sblr::kSblrOpcodeStreamHeaderSize +
                           scratchbird::engine::sblr::kSblrOpcodeStreamTrailerSize &&
                   bytes.size() <=
                       scratchbird::engine::sblr::kSblrOperationMaximumBytes &&
                   read_u32(0, &magic) && read_u16(4, &major) &&
                   read_u16(6, &minor) && read_u16(8, &header_size) &&
                   read_u16(10, &reserved16) && read_u32(12, &flags) &&
                   read_u32(16, &count) && read_u32(20, &reserved) &&
                   read_u64(56, &records_size) &&
                   read_u32(bytes.size() - 16, &trailer_magic) &&
                   read_u32(bytes.size() - 12, &trailer_crc) &&
                   read_u64(bytes.size() - 8, &total_size) &&
                   magic == scratchbird::engine::sblr::kSblrOpcodeStreamMagic &&
                   major == 1 && minor == 0 &&
                   header_size ==
                       scratchbird::engine::sblr::kSblrOpcodeStreamHeaderSize &&
                   reserved16 == 0 && flags == 0 && reserved == 0 &&
                   count >= 2 &&
                   count <= scratchbird::engine::sblr::kSblrOperationMaximumOperands &&
                   records_size == bytes.size() - 80 &&
                   trailer_magic ==
                       scratchbird::engine::sblr::kSblrOpcodeStreamTrailerMagic &&
                   total_size == bytes.size() &&
                   scratchbird::engine::SblrCrc32c(bytes.data(),
                                                    bytes.size() - 12) ==
                       trailer_crc;
    std::size_t offset =
        scratchbird::engine::sblr::kSblrOpcodeStreamHeaderSize;
    for (std::uint32_t index = 0; bounded && index != count; ++index) {
      std::uint64_t record_size = 0;
      bounded = read_u64(offset, &record_size) &&
                offset <= bytes.size() - 16 &&
                bytes.size() - 16 - offset >= 8 &&
                record_size <= bytes.size() - 16 - offset - 8;
      if (bounded) {
        offset += 8 + static_cast<std::size_t>(record_size);
      }
    }
    bounded = bounded && offset == bytes.size() - 16;
    if (!bounded) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4060,
                         "SBLR.OPERAND_INVALID",
                         "sblr.opcode_stream.structural_prevalidation_failed");
    }
    structurally_bounded_record_count = count;
  }

  const auto cancellation_observed = [&]() {
    const bool cancelled = package_candidate &&
        context.query_cancellation_requested &&
        context.query_cancellation_requested();
    if (cancelled) resource_guard.reason = ResourceReleaseReason::kCancel;
    return cancelled;
  };
  if (cancellation_observed()) {
    return fail_result(SB_ENGINE_STATUS_TIMEOUT, out_result, 4062,
                       "PROCESS.CANCELLED",
                       "sblr.opcode_stream.cancelled_before_decode");
  }

  const auto hash_matches = [](const std::vector<std::uint8_t>& bytes,
                               const std::array<std::uint8_t, 32>& expected) {
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
    return digest.ok() && digest.digest == expected;
  };
  if (request->canonical_container_bytes.empty() ||
      request->canonical_execution_envelope_bytes.empty() ||
      request->canonical_operation_bytes.empty() ||
      !hash_matches(request->canonical_container_bytes,
                    request->container_sha256) ||
      !hash_matches(request->canonical_execution_envelope_bytes,
                    request->execution_envelope_sha256) ||
      !hash_matches(request->canonical_operation_bytes,
                    request->operation_sha256)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4054,
        "SBLR.INGRESS_REVALIDATION_FAILED",
        "sblr.ingress_revalidation_failed",
        "canonical admission bytes or SHA-256 evidence differ");
  }
  bool literal_token_validated=false;
  bool parameter_token_validated=false;
  bool variable_token_validated=false;
  std::optional<scratchbird::engine::sblr::SblrParameterValueSetV1>
      admitted_parameter_values;
  if (!request->literal_execution_binding.empty()) {
    const auto& binding=request->literal_execution_binding;
    const auto text_uuid_bytes=[](const std::string& text,
                                  const std::uint8_t* expected){
      const auto parsed=scratchbird::core::uuid::ParseUuid(text);
      return parsed.ok()&&std::equal(parsed.value.bytes.begin(),
                                    parsed.value.bytes.end(),expected);
    };
    if(binding.size()!=176||
       !std::equal(binding.begin(),binding.begin()+4,
                   reinterpret_cast<const std::uint8_t*>("SBEL"))||
       scratchbird::engine::SblrReadU16(binding.data()+4)!=1||
       scratchbird::engine::SblrReadU16(binding.data()+6)!=176||
       scratchbird::engine::SblrReadU32(binding.data()+8)!=176||
       scratchbird::engine::SblrReadU32(binding.data()+12)!=0){
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4054,
                         "SBLR.OPERAND_INVALID",
                         "sblr.literal_execution_binding.structural_invalid");
    }
    if(receipt->literal_admission_consumed)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4054,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.literal_execution_binding.token_replayed");
    const char* literal_binding_mismatch = nullptr;
    if (!receipt->literal_binding_finalized) {
      literal_binding_mismatch = "finalize_state_absent";
    } else if (!text_uuid_bytes(receipt->literal_final_receipt_uuid,
                                binding.data() + 16)) {
      literal_binding_mismatch = "final_receipt_uuid_mismatch";
    } else if (!text_uuid_bytes(receipt->literal_admission_token_uuid,
                                binding.data() + 32)) {
      literal_binding_mismatch = "admission_token_uuid_mismatch";
    } else if (!std::equal(
                   binding.begin() + 48, binding.begin() + 80,
                   receipt->literal_admission_token_binding_sha256.begin())) {
      literal_binding_mismatch = "admission_token_binding_sha256_mismatch";
    } else if (!std::equal(binding.begin() + 80, binding.begin() + 112,
                           receipt->literal_bound_ast_sha256.begin())) {
      literal_binding_mismatch = "bound_ast_sha256_mismatch";
    } else if (!std::equal(binding.begin() + 112, binding.begin() + 144,
                           receipt->literal_sbxn_sha256.begin())) {
      literal_binding_mismatch = "sbxn_sha256_mismatch";
    } else if (!std::equal(binding.begin() + 144, binding.end(),
                           request->operation_sha256.begin())) {
      literal_binding_mismatch = "sbos_sha256_mismatch";
    }
    if (literal_binding_mismatch != nullptr) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4054,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         std::string("sblr.literal_execution_binding.") +
                             literal_binding_mismatch);
    }
    literal_token_validated=true;
  }
  if (!request->parameter_execution_binding.empty() ||
      !request->parameter_value_set.empty()) {
    const auto& binding = request->parameter_execution_binding;
    const auto text_uuid_bytes = [](const std::string& text,
                                    const std::uint8_t* expected) {
      const auto parsed = scratchbird::core::uuid::ParseUuid(text);
      return parsed.ok() && std::equal(parsed.value.bytes.begin(),
                                      parsed.value.bytes.end(), expected);
    };
    const auto optional_text_uuid_bytes = [&](const std::string& text,
                                              const std::uint8_t* expected) {
      if (text.empty()) {
        return std::all_of(expected, expected + 16,
                           [](std::uint8_t byte) { return byte == 0; });
      }
      return text_uuid_bytes(text, expected);
    };
    if (binding.size() != 176 || request->parameter_value_set.empty() ||
        !std::equal(binding.begin(), binding.begin() + 4,
                    reinterpret_cast<const std::uint8_t*>("SBPE")) ||
        scratchbird::engine::SblrReadU16(binding.data() + 4) != 1 ||
        scratchbird::engine::SblrReadU16(binding.data() + 6) != 176 ||
        scratchbird::engine::SblrReadU32(binding.data() + 8) != 176 ||
        scratchbird::engine::SblrReadU32(binding.data() + 12) != 0) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4054,
                         "SBLR.OPERAND_INVALID",
                         "sblr.parameter_execution_binding.structural_invalid");
    }
    if (receipt->parameter_admission_consumed) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4054,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.parameter_execution_binding.token_replayed");
    }
    const auto& snapshot = receipt->parameter_set_snapshot;
    const auto value_set = scratchbird::engine::sblr::
        DecodeSblrParameterValueSetV1(request->parameter_value_set.data(),
                                      request->parameter_value_set.size());
    const auto value_sha = scratchbird::core::hash::ComputeSha256Digest(
        request->parameter_value_set);
    const bool prepared_pair_matches =
        optional_text_uuid_bytes(snapshot.prepared_statement_uuid,
                                 binding.data() + 72) &&
        scratchbird::engine::SblrReadU64(binding.data() + 88) ==
            snapshot.prepared_generation;
    const bool batch_pair_matches =
        optional_text_uuid_bytes(snapshot.batch_uuid, binding.data() + 96) &&
        scratchbird::engine::SblrReadU64(binding.data() + 112) ==
            snapshot.batch_generation;
    const bool dynamic_pair_matches =
        optional_text_uuid_bytes(snapshot.dynamic_package_uuid,
                                 binding.data() + 120) &&
        scratchbird::engine::SblrReadU64(binding.data() + 136) ==
            snapshot.dynamic_generation;
    const char* parameter_binding_mismatch = nullptr;
    if (!receipt->parameter_binding_finalized) parameter_binding_mismatch = "finalize_state_absent";
    else if (!value_set.ok) parameter_binding_mismatch = "value_set_invalid";
    else if (!value_sha.ok()) parameter_binding_mismatch = "value_set_sha_unavailable";
    else if (!prepared_pair_matches) parameter_binding_mismatch = "prepared_pair_mismatch";
    else if (!batch_pair_matches) parameter_binding_mismatch = "batch_pair_mismatch";
    else if (!dynamic_pair_matches) parameter_binding_mismatch = "dynamic_pair_mismatch";
    else if (!text_uuid_bytes(snapshot.execution_uuid, binding.data() + 16)) parameter_binding_mismatch = "execution_uuid_mismatch";
    else if (!text_uuid_bytes(receipt->parameter_final_receipt_uuid, binding.data() + 32)) parameter_binding_mismatch = "final_receipt_uuid_mismatch";
    else if (!text_uuid_bytes(snapshot.parameter_set_descriptor_uuid, binding.data() + 48)) parameter_binding_mismatch = "parameter_set_descriptor_uuid_mismatch";
    else if (scratchbird::engine::SblrReadU64(binding.data() + 64) != snapshot.descriptor_generation) parameter_binding_mismatch = "descriptor_generation_mismatch";
    else if (!std::equal(binding.begin() + 144, binding.end(), value_sha.digest.begin())) parameter_binding_mismatch = "value_set_sha256_mismatch";
    else if (!text_uuid_bytes(snapshot.parameter_set_descriptor_uuid, value_set.value.parameter_set_descriptor_uuid.data())) parameter_binding_mismatch = "value_set_descriptor_uuid_mismatch";
    else if (value_set.value.descriptor_generation != snapshot.descriptor_generation) parameter_binding_mismatch = "value_set_descriptor_generation_mismatch";
    else if (!text_uuid_bytes(snapshot.execution_uuid, value_set.value.execution_uuid.data())) parameter_binding_mismatch = "value_set_execution_uuid_mismatch";
    else if (!text_uuid_bytes(receipt->parameter_final_receipt_uuid, value_set.value.statement_receipt_uuid.data())) parameter_binding_mismatch = "value_set_receipt_uuid_mismatch";
    else if (value_set.value.records.size() != snapshot.slots.size()) parameter_binding_mismatch = "value_set_slot_count_mismatch";
    if (parameter_binding_mismatch != nullptr) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4054,
                         "SBLR.PARAMETER.STALE",
                         std::string("sblr.parameter_execution_binding.") +
                             parameter_binding_mismatch);
    }
    for (std::size_t i = 0; i < value_set.value.records.size(); ++i) {
      const auto& value = value_set.value.records[i];
      const auto& slot = snapshot.slots[i];
      if (value.slot_ordinal != slot.slot_ordinal ||
          !text_uuid_bytes(slot.slot_uuid, value.slot_uuid.data()) ||
          !text_uuid_bytes(slot.datatype_descriptor_uuid,
                           value.datatype_descriptor_uuid.data()) ||
          value.datatype_descriptor_generation !=
              slot.datatype_descriptor_generation ||
          static_cast<std::uint8_t>(value.direction) !=
              static_cast<std::uint8_t>(slot.direction) ||
          value.direction == scratchbird::engine::sblr::
                                 SblrParameterDirectionV1::out ||
          value.state == scratchbird::engine::sblr::
                             SblrParameterValueStateV1::unbound ||
          (value.state == scratchbird::engine::sblr::
                              SblrParameterValueStateV1::null_value &&
           !slot.nullable)) {
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4054,
                           "SBLR.PARAMETER.UNBOUND",
                           "sblr.parameter_value_set.slot_invalid");
      }
    }
    parameter_token_validated = true;
    admitted_parameter_values = value_set.value;
  }
  if (!request->variable_execution_binding.empty()) {
    scratchbird::engine::sblr::SblrVariableExecutionBindingV1 binding;
    std::string variable_detail;
    if (!scratchbird::engine::sblr::DecodeSblrVariableExecutionBindingV1(
            request->variable_execution_binding.data(),
            request->variable_execution_binding.size(), &binding,
            &variable_detail)) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4054,
                         "SBLR.OPERAND_INVALID",
                         "sblr.variable_execution_binding.structural_invalid",
                         variable_detail);
    }
    const auto uuid_bytes = [](const std::string& text) {
      std::array<std::uint8_t, 16> bytes{};
      const auto parsed = scratchbird::core::uuid::ParseUuid(text);
      if (parsed.ok()) std::copy(parsed.value.bytes.begin(),
                                 parsed.value.bytes.end(), bytes.begin());
      return bytes;
    };
    if (receipt->variable_admission_consumed) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4054,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.variable_execution_binding.token_replayed");
    }
    const auto& frame = receipt->variable_frame_snapshot;
    const char* mismatch = nullptr;
    if (!receipt->variable_binding_finalized || !frame.has_value())
      mismatch = "finalize_state_absent";
    else if (binding.statement_receipt_uuid != uuid_bytes(receipt->view.receipt_uuid))
      mismatch = "statement_receipt_uuid_mismatch";
    else if (binding.variable_final_receipt_uuid !=
             uuid_bytes(receipt->variable_final_receipt_uuid))
      mismatch = "final_receipt_uuid_mismatch";
    else if (binding.admission_token_uuid !=
             uuid_bytes(receipt->variable_admission_token_uuid))
      mismatch = "admission_token_uuid_mismatch";
    else if (binding.scope_uuid != uuid_bytes(frame->scope_uuid) ||
             binding.scope_generation != frame->scope_generation)
      mismatch = "scope_pair_mismatch";
    else if (binding.frame_uuid != uuid_bytes(frame->frame_uuid) ||
             binding.frame_generation != frame->frame_generation)
      mismatch = "frame_pair_mismatch";
    else if (binding.registry_snapshot_uuid !=
                 uuid_bytes(frame->registry_snapshot_uuid) ||
             binding.registry_generation != frame->registry_generation)
      mismatch = "registry_snapshot_mismatch";
    else if (binding.executor_availability_generation !=
             receipt->variable_executor_availability_snapshot.generation)
      mismatch = "executor_generation_mismatch";
    else if (binding.binding_sha256 !=
             receipt->variable_admission_binding_sha256)
      mismatch = "binding_sha256_mismatch";
    if (mismatch != nullptr) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4054,
                         "SBLR.VARIABLE.STALE",
                         std::string("sblr.variable_execution_binding.") +
                             mismatch);
    }
    variable_token_validated = true;
  }
  if (package_candidate &&
      (consumed_reservation.payload_sha256 != request->operation_sha256 ||
       consumed_reservation.record_count != structurally_bounded_record_count ||
       consumed_reservation.resource_policy_generation != view.resource_epoch)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4054,
                       "SBLR.INGRESS_REVALIDATION_FAILED",
                       "sblr.package_reservation.binding_mismatch");
  }

  std::vector<std::uint8_t> admission_binding;
  constexpr std::string_view kAdmissionDomain =
      "ScratchBird.SBLR.AdmissionToken.V1";
  admission_binding.insert(admission_binding.end(),
                           kAdmissionDomain.begin(),
                           kAdmissionDomain.end());
  admission_binding.insert(admission_binding.end(),
                           request->container_sha256.begin(),
                           request->container_sha256.end());
  admission_binding.insert(admission_binding.end(),
                           request->execution_envelope_sha256.begin(),
                           request->execution_envelope_sha256.end());
  admission_binding.insert(admission_binding.end(),
                           request->operation_sha256.begin(),
                           request->operation_sha256.end());
  for (const auto* value : {&request->authenticated_principal_uuid,
                            &request->catalog_snapshot_uuid,
                            &request->engine_mga_statement_uuid,
                            &request->engine_mga_snapshot_uuid}) {
    admission_binding.insert(admission_binding.end(),
                             value->begin(), value->end());
    admission_binding.push_back(0);
  }
  scratchbird::engine::SblrAppendU64(admission_binding,
                                     request->catalog_epoch);
  scratchbird::engine::SblrAppendU64(admission_binding,
                                     request->security_epoch);
  scratchbird::engine::SblrAppendU64(admission_binding,
                                     request->resource_epoch);
  if (request->package_executor_evidence.executor_evidence_generation == 0) {
    admission_binding.push_back(0);
  } else {
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->package_admission_reservation.opaque_id);
    admission_binding.push_back(
        static_cast<std::uint8_t>(request->admitted_payload_kind));
    scratchbird::engine::SblrAppendU64(
        admission_binding, consumed_reservation.payload_size);
    scratchbird::engine::SblrAppendU32(
        admission_binding, consumed_reservation.record_count);
    scratchbird::engine::SblrAppendU64(
        admission_binding, consumed_reservation.resource_policy_generation);
    admission_binding.push_back(
        static_cast<std::uint8_t>(request->gateway_evidence.source));
    admission_binding.push_back(
        static_cast<std::uint8_t>(request->gateway_evidence.disposition));
    scratchbird::engine::SblrAppendU64(
        admission_binding,
        request->gateway_evidence.provider_observation_generation);
    admission_binding.insert(
        admission_binding.end(),
        request->gateway_evidence.canonical_payload_sha256.begin(),
        request->gateway_evidence.canonical_payload_sha256.end());
    for (const auto* value : {
             &request->gateway_evidence.route_snapshot_uuid,
             &request->gateway_evidence.security_snapshot_uuid}) {
      admission_binding.insert(admission_binding.end(), value->begin(),
                               value->end());
      admission_binding.push_back(0);
    }
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->gateway_evidence.route_epoch);
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->gateway_evidence.route_generation);
    scratchbird::engine::SblrAppendU64(
        admission_binding, request->gateway_evidence.security_epoch);
    scratchbird::engine::SblrAppendU64(
        admission_binding,
        request->gateway_evidence.security_observation_generation);
    admission_binding.push_back(
        request->gateway_evidence.cluster_context_active ? 1 : 0);
    admission_binding.push_back(
        request->gateway_evidence.cluster_transaction_active ? 1 : 0);
    admission_binding.push_back(
        request->gateway_evidence.route_fence_present ? 1 : 0);
    for (const auto* executor_id : {
             &request->package_executor_evidence.begin_executor_id,
             &request->package_executor_evidence.end_executor_id}) {
      admission_binding.insert(admission_binding.end(), executor_id->begin(),
                               executor_id->end());
      admission_binding.push_back(0);
    }
    admission_binding.insert(
        admission_binding.end(),
        request->package_executor_evidence.registry_snapshot_uuid.begin(),
        request->package_executor_evidence.registry_snapshot_uuid.end());
    admission_binding.push_back(0);
    scratchbird::engine::SblrAppendU64(
        admission_binding,
        request->package_executor_evidence.executor_evidence_generation);
    admission_binding.insert(
        admission_binding.end(),
        request->package_executor_evidence.canonical_payload_sha256.begin(),
        request->package_executor_evidence.canonical_payload_sha256.end());
  }
  const auto binding_digest =
      scratchbird::core::hash::ComputeSha256Digest(admission_binding);
  if (!binding_digest.ok() ||
      binding_digest.digest != request->admission_binding_sha256) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4055,
        "SBLR.INGRESS_REVALIDATION_FAILED",
        "sblr.ingress_revalidation_failed",
        "immutable admission binding digest differs");
  }

  const auto container = scratchbird::engine::DecodeSblrContainerBytes(
      request->canonical_container_bytes.data(),
      request->canonical_container_bytes.size());
  const auto ingress =
      scratchbird::engine::DecodeSblrExecutionEnvelopeV1Bytes(
          request->canonical_execution_envelope_bytes.data(),
          request->canonical_execution_envelope_bytes.size());
  if (container.status != scratchbird::engine::SblrCodecStatus::ok ||
      ingress.status != scratchbird::engine::SblrCodecStatus::ok) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4056,
        "SBLR.INGRESS_REVALIDATION_FAILED",
        "sblr.ingress_revalidation_failed",
        "canonical outer container or SBEE re-decode failed");
  }
  scratchbird::engine::SblrExecutionEnvelopeSemanticView ingress_view;
  if (!scratchbird::engine::SblrValidateExecutionEnvelopeFields(
          ingress.envelope, &ingress_view) ||
      ((ingress_view.payload_kind ==
            scratchbird::engine::SblrPayloadKind::operation_envelope &&
        (ingress_view.operation_ref_kind != 1 ||
         ingress_view.operation_inline_data == nullptr ||
         ingress_view.operation_inline_size !=
             request->canonical_operation_bytes.size())) ||
       (ingress_view.payload_kind ==
            scratchbird::engine::SblrPayloadKind::opcode_stream &&
        (ingress_view.opcode_ref_kind != 1 ||
         ingress_view.opcode_inline_data == nullptr ||
         ingress_view.opcode_inline_size !=
             request->canonical_operation_bytes.size()))) ||
      container.container.operation_payload !=
          request->canonical_operation_bytes ||
      !std::equal(request->canonical_operation_bytes.begin(),
                  request->canonical_operation_bytes.end(),
                  ingress_view.payload_kind ==
                          scratchbird::engine::SblrPayloadKind::opcode_stream
                      ? ingress_view.opcode_inline_data
                      : ingress_view.operation_inline_data)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4057,
        "SBLR.ENVELOPE.CHECKSUM_MISMATCH",
        "sblr.envelope.checksum_mismatch",
        "outer SBLR and SBEE do not carry the admitted exact payload bytes");
  }
  const std::string_view operation_bytes(
      reinterpret_cast<const char*>(request->canonical_operation_bytes.data()),
      request->canonical_operation_bytes.size());
  const bool opcode_stream = ingress_view.payload_kind ==
      scratchbird::engine::SblrPayloadKind::opcode_stream;
  scratchbird::engine::sblr::SblrDecodeResult operation;
  scratchbird::engine::sblr::SblrOpcodeStreamResult stream;
  if (opcode_stream) {
    stream = scratchbird::engine::sblr::DecodeSblrOpcodeStream(operation_bytes);
    if (stream.ok) {
      operation.ok = true;
      // Package framing lives at index 0; the admitted user operation is the
      // first contained member. Dispatch identity must be validated against
      // that member, never against the package-begin frame.
      if (stream.stream.operations.size() < 3) {
        operation.ok = false;
      } else {
        operation.envelope = stream.stream.operations[1];
      }
    }
  } else {
    operation = scratchbird::engine::sblr::DecodeSblrEnvelope(operation_bytes);
  }
  const bool exact_ddl_create_continuous_view_operation =
      !opcode_stream &&
      operation.envelope.operation_id == "engine.op.ddl_create_continuous_view" &&
      operation.envelope.opcode_code == 1640 &&
      operation.envelope.opcode == "SBLR_DDL_CREATE_CONTINUOUS_VIEW";
  const bool exact_ddl_create_sequence_operation = operation.envelope.operation_id == "engine.op.ddl_create_sequence" && operation.envelope.opcode_code == 1671 && operation.envelope.opcode == "SBLR_DDL_CREATE_SEQUENCE";
  const bool exact_ddl_alter_sequence_operation = operation.envelope.operation_id == "engine.op.ddl_alter_sequence" && operation.envelope.opcode_code == 1564 && operation.envelope.opcode == "SBLR_DDL_ALTER_SEQUENCE";
  const bool exact_ddl_drop_type_operation = operation.envelope.operation_id == "engine.op.ddl_drop_type" && operation.envelope.opcode_code == 1571 && operation.envelope.opcode == "SBLR_DDL_DROP_TYPE";
  const bool exact_ddl_drop_sequence_operation = operation.envelope.operation_id == "engine.op.ddl_drop_sequence" && operation.envelope.opcode_code == 1565 && operation.envelope.opcode == "SBLR_DDL_DROP_SEQUENCE";
  const bool exact_ddl_drop_synonym_operation = operation.envelope.operation_id == "engine.op.ddl_drop_synonym" && operation.envelope.opcode_code == 1575 && operation.envelope.opcode == "SBLR_DDL_DROP_SYNONYM";
  const bool exact_ddl_drop_foreign_table_operation = operation.envelope.operation_id == "engine.op.ddl_drop_foreign_table" && operation.envelope.opcode_code == 1577 && operation.envelope.opcode == "SBLR_DDL_DROP_FOREIGN_TABLE";
  const bool exact_ddl_create_fdw_operation = operation.envelope.operation_id == "engine.op.ddl_create_fdw" && operation.envelope.opcode_code == 1578 && operation.envelope.opcode == "SBLR_DDL_CREATE_FDW";
  const bool exact_ddl_drop_fdw_operation = operation.envelope.operation_id == "engine.op.ddl_drop_fdw" && operation.envelope.opcode_code == 1579 && operation.envelope.opcode == "SBLR_DDL_DROP_FDW";
  const bool exact_security_create_user_operation = operation.envelope.operation_id == "engine.op.security_create_user" && operation.envelope.opcode_code == 1792 && operation.envelope.opcode == "SBLR_SEC_CREATE_USER";
  const bool exact_security_alter_user_operation = operation.envelope.operation_id == "engine.op.sec_alter_user" && operation.envelope.opcode_code == 1793 && operation.envelope.opcode == "SBLR_SEC_ALTER_USER";
  const bool exact_security_create_role_operation = operation.envelope.operation_id == "engine.op.sec_create_role" && operation.envelope.opcode_code == 1794 && operation.envelope.opcode == "SBLR_SEC_CREATE_ROLE";
  const bool exact_security_drop_role_operation = operation.envelope.operation_id == "engine.op.sec_drop_role" && operation.envelope.opcode_code == 1801 && operation.envelope.opcode == "SBLR_SEC_DROP_ROLE";
  const bool exact_security_create_policy_operation = operation.envelope.operation_id == "engine.op.sec_create_policy" && operation.envelope.opcode_code == 1802 && operation.envelope.opcode == "SBLR_SEC_CREATE_POLICY";
  const bool exact_security_drop_policy_operation = operation.envelope.operation_id == "engine.op.sec_drop_policy" && operation.envelope.opcode_code == 1803 && operation.envelope.opcode == "SBLR_SEC_DROP_POLICY";
  const bool exact_security_drop_group_mapping_operation = operation.envelope.operation_id == "engine.op.sec_drop_group_mapping" && operation.envelope.opcode_code == 1806 && operation.envelope.opcode == "SBLR_SEC_DROP_GROUP_MAPPING";
  const bool exact_security_alter_role_operation = operation.envelope.operation_id == "engine.op.sec_alter_role" && operation.envelope.opcode_code == 1800 && operation.envelope.opcode == "SBLR_SEC_ALTER_ROLE";
  const bool exact_ddl_alter_continuous_view_operation =
      !opcode_stream &&
      operation.envelope.operation_id == "engine.op.ddl_alter_continuous_view" &&
      operation.envelope.opcode_code == 1641 &&
      operation.envelope.opcode == "SBLR_DDL_ALTER_CONTINUOUS_VIEW";
  const bool exact_dml_async_insert_submit_operation = !opcode_stream && operation.envelope.operation_id == "engine.op.dml_async_insert_submit" && operation.envelope.opcode_code == 1643 && operation.envelope.opcode == "SBLR_DML_ASYNC_INSERT_SUBMIT";
  const bool exact_dml_async_insert_status_operation = !opcode_stream && operation.envelope.operation_id == "engine.op.dml_async_insert_status" && operation.envelope.opcode_code == 1644 && operation.envelope.opcode == "SBLR_DML_ASYNC_INSERT_STATUS";
  const bool exact_dml_async_insert_cancel_operation = !opcode_stream && operation.envelope.operation_id == "engine.op.dml_async_insert_cancel" && operation.envelope.opcode_code == 1645 && operation.envelope.opcode == "SBLR_DML_ASYNC_INSERT_CANCEL";
  const bool exact_dml_counter_add_operation = !opcode_stream && operation.envelope.operation_id == "engine.op.dml_counter_add" && operation.envelope.opcode_code == 1647 && operation.envelope.opcode == "SBLR_DML_COUNTER_ADD";
  const bool exact_dml_timeseries_schema_write_operation = operation.envelope.operation_id == "engine.op.dml_timeseries_schema_write" && operation.envelope.opcode_code == 1648 && operation.envelope.opcode == "SBLR_DML_TIMESERIES_SCHEMA_WRITE";
  const bool exact_ddl_drop_continuous_view_operation =
      !opcode_stream &&
      operation.envelope.operation_id == "engine.op.ddl_drop_continuous_view" &&
      operation.envelope.opcode_code == 1642 &&
      operation.envelope.opcode == "SBLR_DDL_DROP_CONTINUOUS_VIEW";
  const bool exact_ddl_create_table_as_query_operation =
      (operation.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_data" || exact_ddl_create_sequence_operation) &&
      (operation.envelope.opcode_code == 1669 || exact_ddl_create_sequence_operation) &&
      (operation.envelope.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA" || exact_ddl_create_sequence_operation);
  const bool exact_ddl_create_table_as_query_no_data_operation =
      operation.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_no_data" &&
      operation.envelope.opcode_code == 1670 &&
      operation.envelope.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA";
  if (!operation.ok ||
      (!opcode_stream && !exact_ddl_create_table_as_query_operation && !exact_ddl_create_table_as_query_no_data_operation && !exact_ddl_create_continuous_view_operation && !exact_ddl_alter_continuous_view_operation && !exact_ddl_drop_continuous_view_operation && !exact_dml_async_insert_submit_operation && !exact_dml_async_insert_status_operation && !exact_dml_async_insert_cancel_operation && !exact_dml_counter_add_operation && !exact_dml_timeseries_schema_write_operation && !exact_ddl_alter_sequence_operation && !exact_ddl_drop_sequence_operation &&
       !exact_ddl_drop_type_operation && !exact_ddl_drop_synonym_operation && !exact_ddl_drop_foreign_table_operation && !exact_ddl_create_fdw_operation && !exact_ddl_drop_fdw_operation && !exact_security_create_user_operation && !exact_security_alter_user_operation &&
       (operation.envelope.operation_id != "query.execute" ||
        operation.envelope.opcode_code != 0x1207 ||
        operation.envelope.opcode != "SBLR_QUERY_EXECUTE"))) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4058,
        "SBLR.OPERATION.OPCODE_IDENTITY_MISMATCH",
        "sblr.operation.opcode_identity_mismatch",
        opcode_stream
            ? (stream.detail.empty() ? "canonical SBOS decoding failed"
                                     : stream.detail)
            : (std::string("operation_identity=") + operation.envelope.operation_id +
               " opcode=" + operation.envelope.opcode +
               " opcode_code=" + std::to_string(operation.envelope.opcode_code) +
               " operation_ok=" + (operation.ok ? "true" : "false")));
  }
  std::vector<const std::vector<std::uint8_t>*> literal_tables;
  std::vector<const std::vector<std::uint8_t>*> parameter_tables;
  std::vector<const std::vector<std::uint8_t>*> variable_tables;
  std::vector<const std::vector<std::uint8_t>*> source_map_descriptors;
  std::vector<const std::vector<std::uint8_t>*> error_vector_descriptors;
  const auto collect_literal_tables=[&](const auto& envelope){
    for(const auto& operand:envelope.operands){
      if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::expression_node_table)
        literal_tables.push_back(&operand.value_body);
      if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::parameter_node_table)
        parameter_tables.push_back(&operand.value_body);
      if(operand.value_kind==scratchbird::engine::sblr::SblrValueKind::variable_node_table)
        variable_tables.push_back(&operand.value_body);
      if(envelope.operation_id=="engine.op.source_map"&&
         envelope.opcode=="SBLR_SOURCE_MAP"&&envelope.opcode_code==6&&
         operand.value_kind==scratchbird::engine::sblr::SblrValueKind::descriptor_ref)
        source_map_descriptors.push_back(&operand.value_body);
      if(envelope.operation_id=="engine.op.error_vector"&&
         envelope.opcode=="SBLR_ERROR_VECTOR"&&envelope.opcode_code==7&&
         operand.value_kind==scratchbird::engine::sblr::SblrValueKind::descriptor_ref)
        error_vector_descriptors.push_back(&operand.value_body);
    }
  };
  if(opcode_stream){for(const auto& member:stream.stream.operations)collect_literal_tables(member);}else collect_literal_tables(operation.envelope);
  if(!literal_tables.empty()&&request->literal_execution_binding.empty()){
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.literal_execution_binding.required");
  }
  if(literal_tables.size()>1){
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.literal_execution_binding.duplicate_table");
  }
  if(!literal_tables.empty()){
    const auto digest=scratchbird::core::hash::ComputeSha256Digest(*literal_tables.front());
    if(!digest.ok()||digest.digest!=receipt->literal_sbxn_sha256){
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "sblr.literal_execution_binding.sbxn_hash_mismatch");
    }
  }
  if (!parameter_tables.empty() && !parameter_token_validated) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_execution_binding.required");
  }
  if (parameter_tables.size() > 1) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.parameter_execution_binding.duplicate_table");
  }
  if (!parameter_tables.empty()) {
    std::vector<std::uint8_t> material;
    constexpr std::string_view domain =
        "ScratchBird.SblrParameterNodeTable.V1";
    material.insert(material.end(), domain.begin(), domain.end());
    material.insert(material.end(), parameter_tables.front()->begin(),
                    parameter_tables.front()->end());
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(material);
    if (!digest.ok() || digest.digest != receipt->parameter_sbpn_sha256) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4058,
                         "SBLR.PARAMETER.STALE",
                         "sblr.parameter_execution_binding.sbpn_hash_mismatch");
    }
  }
  if (!variable_tables.empty() && !variable_token_validated) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_execution_binding.required");
  }
  if (variable_tables.size() > 1) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                       "SBLR.OPERAND_INVALID",
                       "sblr.variable_execution_binding.duplicate_table");
  }
  if (!variable_tables.empty()) {
    const auto variable_uuid_text = [](const std::uint8_t* bytes) {
      constexpr char hex[] = "0123456789abcdef";
      std::string text;
      text.reserve(36);
      for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10)
          text.push_back('-');
        text.push_back(hex[bytes[index] >> 4]);
        text.push_back(hex[bytes[index] & 0x0f]);
      }
      return text;
    };
    const auto decoded_variables =
        scratchbird::engine::sblr::DecodeSblrVariableNodeTableV1(
            variable_tables.front()->data(), variable_tables.front()->size());
    if (!decoded_variables.ok ||
        !receipt->variable_frame_snapshot.has_value()) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4058,
                         "SBLR.OPERAND_INVALID",
                         "sblr.variable_node_table.invalid");
    }
    auto variable_context = receipt->engine_context;
    variable_context.trace_tags.push_back("private_variable_registry");
    const auto& frame = *receipt->variable_frame_snapshot;
    for (const auto& node : decoded_variables.table.nodes) {
      if (cancellation_observed()) {
        return fail_result(SB_ENGINE_STATUS_TIMEOUT, out_result, 4058,
                           "PROCESS.CANCELLED",
                           "sblr.variable.cancelled_before_node");
      }
      const auto lookup = scratchbird::engine::internal_api::LookupSblrVariable(
          variable_context, frame.statement_receipt_uuid, frame.scope_uuid,
          frame.scope_generation, frame.frame_uuid, frame.frame_generation,
          variable_uuid_text(node.variable_descriptor_uuid.data()),
          node.variable_descriptor_generation, node.value_generation);
      if (!lookup.ok) {
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4058,
                           lookup.diagnostic.code,
                           lookup.diagnostic.message_key,
                           lookup.diagnostic.detail);
      }
      if (lookup.row.datatype_descriptor_uuid !=
              variable_uuid_text(node.datatype_descriptor_uuid.data()) ||
          lookup.row.datatype_descriptor_generation !=
              node.datatype_descriptor_generation) {
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4058,
                           "SBLR.VARIABLE.STALE",
                           "sblr.variable_node_table.descriptor_mismatch");
      }
    }
  }
  if (!source_map_descriptors.empty()) {
    if(source_map_descriptors.size()!=1||!receipt->source_map_bound_ast_frozen||
       source_map_descriptors.front()->size()!=24)
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
                         "SBLR.OPERAND_INVALID","sblr.source_map.descriptor_ref_invalid");
    const auto uuid_from_bytes=[](const std::uint8_t*bytes){constexpr char h[]="0123456789abcdef";std::string s;s.reserve(36);for(std::size_t i=0;i<16;++i){if(i==4||i==6||i==8||i==10)s.push_back('-');s.push_back(h[bytes[i]>>4]);s.push_back(h[bytes[i]&15]);}return s;};
    const auto load64=[](const std::uint8_t*p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v;};
    const auto&id=*source_map_descriptors.front();
    const auto descriptor_uuid=uuid_from_bytes(id.data());
    const auto descriptor_generation=load64(id.data()+16);
    const auto admitted=std::find_if(receipt->source_map_descriptors.begin(),receipt->source_map_descriptors.end(),
      [&](const auto&s){return s.descriptor_uuid==descriptor_uuid&&s.descriptor_generation==descriptor_generation;});
    if(admitted==receipt->source_map_descriptors.end())
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,
                         "SBLR.SOURCE_MAP.STALE","sblr.source_map.descriptor_stale");
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity identity;
    identity.executor_id=scratchbird::engine::internal_api::kSblrSourceMapExecutorId;
    identity.opcode_code=scratchbird::engine::internal_api::kSblrSourceMapOpcodeCode;
    identity.opcode_version=scratchbird::engine::internal_api::kSblrSourceMapOpcodeVersion;
    identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrSourceMapOperandDescriptorId;
    identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrSourceMapResultDescriptorId;
    identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrSourceMapResultDescriptorVersion;
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current;
    const auto availability=scratchbird::engine::internal_api::RevalidateSblrExecutorAvailability(
      receipt->engine_context,identity,receipt->source_map_executor_availability_snapshot,&current);
    if(availability.error)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,
      availability.code,availability.message_key,availability.detail);
    auto source_context=receipt->engine_context;source_context.statement_metadata_snapshot_engine_owned=true;
    source_context.trace_tags.push_back("private_source_map_registry");
    const auto found=scratchbird::engine::internal_api::LookupSblrSourceMapDescriptorV1(
      source_context,receipt->view.receipt_uuid,descriptor_uuid,descriptor_generation,
      admitted->bound_ast_sha256,admitted->registry_snapshot_uuid,admitted->registry_generation);
    if(!found.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,
      found.diagnostic.code,found.diagnostic.message_key,found.diagnostic.detail);
    const auto decoded=scratchbird::engine::sblr::DecodeSblrSourceMapDescriptorVectorV1(
      found.snapshot.canonical_smvd.data(),found.snapshot.canonical_smvd.size());
    if(decoded.status!=scratchbird::engine::sblr::SblrSourceMapDecodeStatusV1::ok)
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
        "SBLR.OPERAND_INVALID","sblr.source_map.vector_invalid");
    for(const auto&entry:decoded.vector.entries){(void)entry;if(cancellation_observed())
      return fail_result(SB_ENGINE_STATUS_TIMEOUT,out_result,4058,
        "PROCESS.CANCELLED","sblr.source_map.cancelled_before_entry");}
    if(cancellation_observed())return fail_result(SB_ENGINE_STATUS_TIMEOUT,out_result,4058,
      "PROCESS.CANCELLED","sblr.source_map.cancelled_before_parent");
  }
  if (!error_vector_descriptors.empty()) {
    if(error_vector_descriptors.size()!=1||error_vector_descriptors.front()->size()!=24)
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,
        "SBLR.OPERAND_INVALID","sblr.error_vector.descriptor_ref_invalid");
    const auto text=[](const std::uint8_t*p){constexpr char h[]="0123456789abcdef";std::string s;for(size_t i=0;i<16;++i){if(i==4||i==6||i==8||i==10)s.push_back('-');s.push_back(h[p[i]>>4]);s.push_back(h[p[i]&15]);}return s;};
    const auto u64=[](const std::uint8_t*p){std::uint64_t v=0;for(unsigned i=0;i<8;++i)v|=std::uint64_t(p[i])<<(8*i);return v;};
    const auto& ref=*error_vector_descriptors.front();const auto id=text(ref.data());const auto gen=u64(ref.data()+16);
    const auto admitted=std::find_if(receipt->error_vector_descriptors.begin(),receipt->error_vector_descriptors.end(),[&](const auto&s){return s.descriptor_uuid==id&&s.descriptor_generation==gen;});
    if(admitted==receipt->error_vector_descriptors.end())return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,"SBLR.ERROR_VECTOR.STALE","sblr.error_vector.descriptor_stale");
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity identity;
    identity.executor_id=scratchbird::engine::internal_api::kSblrErrorVectorExecutorId;identity.opcode_code=scratchbird::engine::internal_api::kSblrErrorVectorOpcodeCode;identity.opcode_version=scratchbird::engine::internal_api::kSblrErrorVectorOpcodeVersion;identity.operand_descriptor_id=scratchbird::engine::internal_api::kSblrErrorVectorOperandDescriptorId;identity.result_descriptor_id=scratchbird::engine::internal_api::kSblrErrorVectorResultDescriptorId;identity.result_descriptor_version=scratchbird::engine::internal_api::kSblrErrorVectorResultDescriptorVersion;
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current;auto available=scratchbird::engine::internal_api::RevalidateSblrExecutorAvailability(receipt->engine_context,identity,receipt->error_vector_executor_availability_snapshot,&current);
    if(available.error)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,available.code,available.message_key,available.detail);
    auto error_context=receipt->engine_context;error_context.statement_metadata_snapshot_engine_owned=true;error_context.trace_tags.push_back("private_error_vector_registry");
    auto found=scratchbird::engine::internal_api::LookupSblrErrorVectorDescriptorV1(error_context,receipt->view.receipt_uuid,id,gen);
    if(!found.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4058,found.diagnostic.code,found.diagnostic.message_key,found.diagnostic.detail);
    scratchbird::engine::sblr::SblrErrorVectorDescriptorV1 decoded;std::string decode_detail;
    if(!scratchbird::engine::sblr::DecodeSblrErrorVectorDescriptorV1(found.snapshot.canonical_ervd.data(),found.snapshot.canonical_ervd.size(),&decoded,&decode_detail))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4058,"SBLR.OPERAND_INVALID","sblr.error_vector.vector_invalid");
    for(const auto&entry:decoded.entries){(void)entry;if(cancellation_observed())return fail_result(SB_ENGINE_STATUS_TIMEOUT,out_result,4058,"PROCESS.CANCELLED","sblr.error_vector.cancelled_before_entry");}
    if(cancellation_observed())return fail_result(SB_ENGINE_STATUS_TIMEOUT,out_result,4058,"PROCESS.CANCELLED","sblr.error_vector.cancelled_before_parent");
  }

  const auto uuid_text = [](const std::uint8_t* bytes) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    text.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
      if (index == 4 || index == 6 || index == 8 || index == 10) {
        text.push_back('-');
      }
      text.push_back(kHex[bytes[index] >> 4]);
      text.push_back(kHex[bytes[index] & 0x0f]);
    }
    return text;
  };
  const auto& anchor = container.container.canonical_anchor;
  const auto& dialect_field = ingress.envelope.fields[10];
  const auto& user_field = ingress.envelope.fields[11];
  const bool exact_receipt_binding =
      request->engine_mga_statement_uuid == view.statement_uuid &&
      request->engine_mga_snapshot_uuid == view.statement_snapshot_uuid &&
      request->catalog_snapshot_uuid ==
          view.statement_metadata_snapshot_uuid &&
      request->catalog_epoch == view.catalog_generation_id &&
      request->security_epoch == view.security_epoch &&
      request->resource_epoch == view.resource_epoch &&
      request->authenticated_principal_uuid ==
          context.principal_uuid.canonical &&
      uuid_text(anchor.data()) == context.database_uuid.canonical &&
      uuid_text(anchor.data() + 76) == view.catalog_epoch_uuid &&
      uuid_text(anchor.data() + 116) == view.statement_uuid &&
      operation.envelope.parser_package_uuid == uuid_text(anchor.data() + 32) &&
      operation.envelope.registry_snapshot_uuid == view.catalog_epoch_uuid &&
      ingress.envelope.fields[0].size() == 16 &&
      uuid_text(ingress.envelope.fields[0].data()) == view.statement_uuid &&
      dialect_field.size() == 17 && dialect_field[0] == 1 &&
      std::equal(dialect_field.begin() + 1, dialect_field.end(),
                 anchor.begin() + 16) &&
      user_field.size() == 17 && user_field[0] == 1 &&
      uuid_text(user_field.data() + 1) ==
          request->authenticated_principal_uuid;
  if (!exact_receipt_binding) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4059,
        "ENGINE.STATEMENT_CONTEXT.DISPATCH_BINDING_MISMATCH",
        "engine.statement_context.dispatch_binding_mismatch");
  }

  if (opcode_stream && stream.stream.operations.size() != 3) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4060,
                       "SBLR.OPERAND_INVALID",
                       "sblr.opcode_stream.external_root_count_invalid");
  }
  if (opcode_stream &&
      stream.stream.package_descriptor_uuid != view.bound_ast_uuid) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4060,
                       "DATATYPE.DESCRIPTOR_INVALID",
                       "sblr.opcode_stream.package_descriptor_invalid");
  }
  const bool exact_gateway_evidence = !opcode_stream ||
      (request->gateway_evidence.source ==
           scratchbird::server_engine_bridge::
               StatementGatewayEvidenceSource::kLocalObserved &&
       request->gateway_evidence.disposition ==
           scratchbird::server_engine_bridge::
               StatementGatewayDisposition::kPassThrough &&
       request->gateway_evidence.provider_observation_generation == 1 &&
       request->gateway_evidence.canonical_payload_sha256 ==
           request->operation_sha256 &&
       request->gateway_evidence.route_snapshot_uuid ==
           view.optimizer_route_snapshot_uuid &&
       request->gateway_evidence.route_epoch == view.optimizer_route_epoch &&
       request->gateway_evidence.route_generation ==
           view.optimizer_route_generation &&
       request->gateway_evidence.security_snapshot_uuid ==
           view.security_context_uuid &&
       request->gateway_evidence.security_epoch == view.security_epoch &&
       request->gateway_evidence.security_observation_generation ==
           view.security_epoch &&
       request->gateway_evidence.cluster_context_active ==
           view.cluster_context_active &&
       request->gateway_evidence.cluster_transaction_active ==
           view.cluster_transaction_active &&
       request->gateway_evidence.route_fence_present ==
           view.route_fence_present &&
       !view.cluster_context_active && !view.cluster_transaction_active &&
       !view.route_fence_present);
  if (!exact_gateway_evidence) {
    return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4060,
                       "PROCESS.CLUSTER_PATH_ABSENT",
                       "sblr.opcode_stream.gateway_evidence_invalid");
  }
  const bool exact_executor_evidence = !opcode_stream ||
      (request->package_executor_evidence.begin_executor_id ==
           "engine.op.package_begin" &&
       request->package_executor_evidence.end_executor_id ==
           "engine.op.package_end" &&
       request->package_executor_evidence.registry_snapshot_uuid ==
           stream.stream.registry_snapshot_uuid &&
       request->package_executor_evidence.executor_evidence_generation == 1 &&
       request->package_executor_evidence.canonical_payload_sha256 ==
           request->operation_sha256);
  if (!exact_executor_evidence) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4060,
                       "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                       "sblr.opcode_stream.executor_evidence_invalid");
  }

  // SBOP v1 freezes operation identity and typed operands but does not encode
  // duplicate authority booleans.  The engine opcode registry owns this
  // requirement; project it only after the exact query.execute identity and
  // receipt binding above have both been revalidated.
  auto dispatch_operation = operation.envelope;
  if (!opcode_stream) dispatch_operation.requires_transaction_context = true;

  scratchbird::engine::internal_api::EngineResolveStatementSnapshotRequest
      snapshot_request;
  snapshot_request.context = context;
  const auto current_snapshot =
      scratchbird::engine::internal_api::EngineResolveStatementSnapshot(
          snapshot_request);
  if (!current_snapshot.ok || !current_snapshot.snapshot_vector.complete ||
      !current_snapshot.snapshot_vector.inventory_authoritative ||
      current_snapshot.snapshot_vector.snapshot_uuid.kind !=
          receipt->snapshot_vector.snapshot_uuid.kind ||
      current_snapshot.snapshot_vector.snapshot_uuid.value !=
          receipt->snapshot_vector.snapshot_uuid.value ||
      current_snapshot.snapshot_vector.owning_transaction.value !=
          view.owning_local_transaction_id ||
      current_snapshot.snapshot_vector.visible_committed_high_watermark !=
          view.visible_committed_high_watermark) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4060,
        "ENGINE.STATEMENT_CONTEXT.SNAPSHOT_STALE",
        "engine.statement_context.snapshot_stale");
  }

  constexpr std::size_t kStatementDescriptorProfileCountV10 = 646;
  constexpr std::size_t kMultilegDescriptorProfileCountV10 = 320;
  if (view.descriptor_profiles.size() !=
      kStatementDescriptorProfileCountV10) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4061,
        "ENGINE.STATEMENT_CONTEXT.MULTILEG_DESCRIPTOR_COHORT_INVALID",
        "engine.statement_context.multileg_descriptor_cohort_invalid",
        "live statement receipt is not the exact 646-profile V10 cohort");
  }
  std::vector<scratchbird::engine::optimizer::MultilegDescriptorProfileV1>
      multileg_profiles;
  multileg_profiles.reserve(kMultilegDescriptorProfileCountV10);
  const auto suffix_begin = view.descriptor_profiles.end() -
                            kMultilegDescriptorProfileCountV10;
  for (auto profile = suffix_begin;
       profile != view.descriptor_profiles.end(); ++profile) {
    const auto kind = static_cast<std::uint8_t>(profile->profile_kind);
    if (kind < 14 || kind > 23 || !profile->collation_uuid.empty() ||
        profile->width != 0 || profile->precision != 0 ||
        profile->scale != 0) {
      return fail_result(
          SB_ENGINE_STATUS_CONFLICT,
          out_result,
          4061,
          "ENGINE.STATEMENT_CONTEXT.MULTILEG_DESCRIPTOR_COHORT_INVALID",
          "engine.statement_context.multileg_descriptor_cohort_invalid",
          "live statement receipt V10 suffix metadata changed");
    }
    multileg_profiles.push_back(
        {kind, profile->slot, profile->descriptor_uuid,
         profile->type_uuid, profile->nullable});
  }
  scratchbird::engine::optimizer::MultilegDescriptorDispatchScopeV1
      descriptor_dispatch_scope(view.statement_uuid, multileg_profiles);
  if (!descriptor_dispatch_scope.installed()) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4061,
        descriptor_dispatch_scope.diagnostic_id(),
        "engine.statement_context.multileg_descriptor_scope_refused",
        descriptor_dispatch_scope.detail());
  }

  // Consume only after every structural, receipt, profile, security, gateway,
  // executor-evidence and resource admission check above has passed, but
  // immediately before executor entry. The receipt mutex makes this atomic.
  if(literal_token_validated){
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current;
    const auto availability=scratchbird::engine::internal_api::
        RevalidateSblrExecutorAvailability(
            receipt->engine_context,
            receipt->literal_executor_availability_snapshot,&current);
    if(availability.error)
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4061,
                         availability.code,availability.message_key,
                         availability.detail);
    if(receipt->literal_admission_consumed)
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4061,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.literal_execution_binding.token_replayed");
  }
  if (parameter_token_validated) {
    scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity
        parameter_identity;
    parameter_identity.executor_id =
        scratchbird::engine::internal_api::kSblrParameterExecutorId;
    parameter_identity.opcode_code =
        scratchbird::engine::internal_api::kSblrParameterOpcodeCode;
    parameter_identity.opcode_version =
        scratchbird::engine::internal_api::kSblrParameterOpcodeVersion;
    parameter_identity.operand_descriptor_id =
        scratchbird::engine::internal_api::kSblrParameterOperandDescriptorId;
    parameter_identity.result_descriptor_id =
        scratchbird::engine::internal_api::kSblrParameterResultDescriptorId;
    parameter_identity.result_descriptor_version =
        scratchbird::engine::internal_api::kSblrParameterResultDescriptorVersion;
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot
        current_parameter_executor;
    const auto parameter_executor = scratchbird::engine::internal_api::
        RevalidateSblrExecutorAvailability(
            receipt->engine_context, parameter_identity,
            receipt->parameter_executor_availability_snapshot,
            &current_parameter_executor);
    if (parameter_executor.error ||
        current_parameter_executor.generation !=
            receipt->view.parameter_executor_availability_generation) {
      return fail_result(
          SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4061,
          parameter_executor.error
              ? parameter_executor.code
              : "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
          "sblr.parameter_execution.executor_revalidation_refused",
          parameter_executor.detail);
    }
    scratchbird::engine::internal_api::SblrParameterSetSnapshot current;
    const auto& admitted = receipt->parameter_set_snapshot;
    const auto revalidated = scratchbird::engine::internal_api::
        RevalidateSblrParameterSet(
            receipt->engine_context, admitted, receipt->view.receipt_uuid,
            admitted.execution_uuid, admitted.prepared_statement_uuid,
            admitted.prepared_generation, admitted.batch_uuid,
            admitted.batch_generation, admitted.dynamic_package_uuid,
            admitted.dynamic_generation, &current);
    if (revalidated.error) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4061,
                         revalidated.code, revalidated.message_key,
                         revalidated.detail);
    }
    if (receipt->parameter_admission_consumed) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4061,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.parameter_execution_binding.token_replayed");
    }
  }
  if (variable_token_validated) {
    scratchbird::engine::internal_api::SblrExecutorAvailabilitySnapshot current;
    const auto availability = scratchbird::engine::internal_api::
        RevalidateSblrExecutorAvailability(
            receipt->engine_context,
            VariableExecutorAvailabilityIdentity(),
            receipt->variable_executor_availability_snapshot, &current);
    if (availability.error ||
        current.generation !=
            receipt->view.variable_executor_availability_generation) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4061,
                         availability.error
                             ? availability.code
                             : "SBLR.OPCODE.EXECUTOR_EVIDENCE_STALE",
                         "sblr.variable_execution.executor_revalidation_refused",
                         availability.detail);
    }
    if (receipt->variable_admission_consumed) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4061,
                         "ENGINE.STATEMENT_CONTEXT.ADMISSION_TOKEN_STALE",
                         "sblr.variable_execution_binding.token_replayed");
    }
  }
  // Every live executor and value cohort has now been revalidated. Commit all
  // admission-token transitions together while the receipt mutex is held so a
  // mixed literal/parameter request cannot consume only one side.
  if (literal_token_validated) receipt->literal_admission_consumed = true;
  if (parameter_token_validated) receipt->parameter_admission_consumed = true;
  if (variable_token_validated) receipt->variable_admission_consumed = true;

  scratchbird::engine::sblr::SblrDispatchResult dispatched;
  bool source_map_root = false;
  bool error_vector_root = false;
  bool txn_begin_root = false;
  bool txn_commit_root = false;
  bool txn_rollback_root = false;
  bool txn_savepoint_root = false;
  bool txn_release_savepoint_root = false;
  bool txn_rollback_to_savepoint_root = false;
  bool psql_autonomous_frame_root = false;
  bool reservation_release_root = false;
  bool temporary_cleanup_root = false;
  bool cursor_open_root = false;
  bool cursor_fetch_root = false;
  bool cursor_close_root = false;
  bool read_by_key_root = false;
  bool read_range_root = false;
  bool read_stream_root = false;
  bool result_set_pass_root = false;
  bool access_cursor_open_root = false;
  bool access_cursor_fetch_root = false;
  bool access_cursor_close_root = false;
  bool insert_root = false;
  bool update_root = false;
  bool delete_root = false;
  bool merge_root = false;
  bool table_truncate_root = false;
  bool table_analyze_root = false;
  bool bulk_import_stream_root = false;
  bool bulk_export_stream_root = false;
  bool statement_batch_root = false;
  bool atomic_cas_root = false;
  bool atomic_rmw_root = false;
  bool advisory_lock_root = false;
  bool advisory_lock_release_root = false;
  bool function_call_root = false;
  bool operator_call_root = false;
  bool cast_root = false;
  bool compare_root = false;
  bool domain_operation_root = false;
  bool udr_invoke_root = false;
  bool procedure_invoke_root = false;
  bool function_invoke_root = false;
  bool aggregate_invoke_root = false;
  bool sequence_nextval_root = false;
  bool sequence_currval_root = false;
  bool sequence_setval_root = false;
  bool query_numeric_root = false;
  bool advanced_datatype_family_root = false;
  bool show_version_root = false;
  bool catalog_introspect_root = false;
  bool project_root = false;
  bool aggregate_root = false;
  bool group_root = false;
  bool security_create_group_mapping_root = false;
  bool security_drop_group_mapping_root = false;
  bool security_grant_root = false;
  bool security_revoke_root = false;
  bool security_alter_policy_root = false;
  bool security_drop_user_root = false;
  bool sort_root = false;
  bool limit_root = false;
  bool window_root = false;
  bool return_result_set_root = false;
  bool kv_structured_read_root = false;
  bool kv_structured_mutate_root = false;
  bool kv_structured_scan_root = false;
  bool kv_structured_stream_read_root = false;
  bool kv_structured_stream_append_root = false;
  bool kv_structured_timeseries_root = false;
  bool system_config_set_root = false;
  bool ddl_refresh_materialized_view_root = false;
  bool ddl_create_materialized_view_root = false;
  bool ddl_drop_materialized_view_root = false;
  bool ddl_create_table_as_query_with_data_root = false;
  bool ddl_create_table_as_query_with_no_data_root = false;
  bool ddl_create_sequence_root = false;
  bool ddl_alter_sequence_root = false;
  bool ddl_drop_type_root = false; bool ddl_rename_object_root = false; bool ddl_create_synonym_root = false; bool ddl_create_foreign_table_root = false; bool ddl_create_fdw_root = false;
  bool ddl_drop_fdw_root = false;
  bool ddl_drop_sequence_root = false;
  bool ddl_drop_package_root = false; bool ddl_drop_synonym_root = false; bool ddl_drop_foreign_table_root = false; bool ddl_alter_package_root = false;
  bool ddl_create_domain_root = false; bool ddl_create_package_root = false; bool ddl_create_temporary_table_root = false; bool ddl_drop_temporary_table_root = false; bool ddl_rename_object_vector_root = false; bool ddl_alter_domain_root = false; bool ddl_create_view_root = false; bool ddl_alter_view_root = false; bool ddl_drop_view_root = false; bool ddl_create_trigger_root = false; bool ddl_alter_trigger_root = false; bool ddl_drop_trigger_root = false; bool ddl_create_procedure_root = false; bool ddl_alter_procedure_root = false; bool ddl_drop_procedure_root = false; bool ddl_create_function_root = false; bool ddl_alter_function_root = false; bool ddl_drop_function_root = false; bool ddl_create_schema_root = false; bool ddl_create_table_root = false; bool ddl_create_index_root = false; bool ddl_drop_index_root = false;
  bool ddl_create_or_replace_srs_root = false;
  bool ddl_drop_srs_root = false;
  bool ddl_create_rewrite_rule_root = false;
  bool ddl_alter_rewrite_rule_root = false;
  bool ddl_drop_rewrite_rule_root = false;
  bool ddl_validate_constraint_root = false;
  bool security_create_privilege_template_root = false;
  bool security_create_user_root = false;
  bool security_alter_user_root = false;
  bool security_create_role_root = false;
  bool security_drop_role_root = false;
  bool security_create_policy_root = false;
  bool security_drop_policy_root = false;
  bool security_alter_role_root = false;
  bool security_alter_privilege_template_root = false;
  bool security_drop_privilege_template_root = false;
  bool database_create_template_clone_root = false;
  bool ddl_create_aggregate_root = false;
  bool ddl_create_macro_root = false;
  bool ddl_drop_macro_root = false;
  bool admin_register_external_relation_resolver_root = false;
  bool admin_unregister_external_relation_resolver_root = false;
  bool ddl_create_dictionary_root = false;
  bool ddl_alter_aggregate_root = false;
  bool ddl_drop_aggregate_root = false;
  bool ddl_drop_dictionary_root = false;
  bool ddl_alter_dictionary_root = false;
  bool ddl_create_continuous_view_root = false;
  bool ddl_alter_continuous_view_root = false;
  bool ddl_drop_continuous_view_root = false;
  bool dml_async_insert_submit_root = false;
  bool dml_async_insert_status_root = false;
  bool dml_async_insert_cancel_root = false;
  bool dml_counter_add_root = false;
  bool dml_timeseries_schema_write_root = false;
  bool ddl_timeseries_series_cardinality_policy_root = false;
  bool ddl_create_timeseries_value_cache_root = false;
  bool ddl_purge_system_history_root = false;
  bool ddl_set_index_optimizer_eligibility_root = false;
  bool ddl_set_table_type_enforcement_root = false;
  bool database_serialize_logical_snapshot_root = false;
  bool database_deserialize_logical_snapshot_root = false;
  sb_engine_transaction_t commit_private_handle = nullptr;
  sb_engine_transaction_t rollback_private_handle = nullptr;
  scratchbird::engine::sblr::SblrTransactionCommitOptionsV1 commit_options;
  scratchbird::engine::sblr::SblrTransactionRollbackOptionsV1
      rollback_options;
  scratchbird::engine::sblr::SblrSavepointDescriptorV1 savepoint_descriptor;
  scratchbird::engine::sblr::SblrBulkImportStreamDescriptorV1 bulk_import_stream_descriptor;
  std::uint64_t bulk_import_stream_availability_generation = 0;
  scratchbird::engine::sblr::SblrBulkExportStreamDescriptorV1 bulk_export_stream_descriptor;
  std::uint64_t bulk_export_stream_availability_generation = 0;
  scratchbird::engine::sblr::SblrStatementBatchDescriptorV1 statement_batch_descriptor;std::uint64_t statement_batch_availability_generation=0;
  scratchbird::engine::sblr::SblrAtomicCasDescriptorV1 atomic_cas_descriptor;std::uint64_t atomic_cas_availability_generation=0;
  scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1 dml_counter_add_descriptor{}; std::uint64_t dml_counter_add_availability_generation=0; std::vector<std::uint8_t> dml_counter_add_result_bytes;
  scratchbird::engine::sblr::SblrDmlTimeseriesSchemaWriteDescriptorV1 dml_timeseries_schema_write_descriptor{}; std::uint64_t dml_timeseries_schema_write_availability_generation=0; std::vector<std::uint8_t> dml_timeseries_schema_write_result_bytes;
  scratchbird::engine::sblr::SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1 ddl_timeseries_series_cardinality_policy_descriptor{}; std::uint64_t ddl_timeseries_series_cardinality_policy_availability_generation=0; std::vector<std::uint8_t> ddl_timeseries_series_cardinality_policy_result_bytes;
  scratchbird::engine::sblr::SblrDdlCreateTimeseriesValueCacheDescriptorV1 ddl_create_timeseries_value_cache_descriptor{}; std::uint64_t ddl_create_timeseries_value_cache_availability_generation=0; std::vector<std::uint8_t> ddl_create_timeseries_value_cache_result_bytes;
  scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1 atomic_rmw_descriptor;std::uint64_t atomic_rmw_availability_generation=0;
  scratchbird::engine::sblr::SblrAdvisoryLockDescriptorV1 advisory_lock_descriptor;std::uint64_t advisory_lock_availability_generation=0;
  scratchbird::engine::sblr::SblrAdvisoryLockReleaseDescriptorV1 advisory_lock_release_descriptor;std::uint64_t advisory_lock_release_availability_generation=0;
  scratchbird::engine::sblr::SblrFunctionCallDescriptorV1 function_call_descriptor;std::uint64_t function_call_availability_generation=0;
  scratchbird::engine::sblr::SblrOperatorCallDescriptorV1 operator_call_descriptor;std::uint64_t operator_call_availability_generation=0;
  scratchbird::engine::sblr::SblrCastDescriptorV1 cast_descriptor;std::uint64_t cast_availability_generation=0;
  scratchbird::engine::sblr::SblrCompareDescriptorV1 compare_descriptor;std::uint64_t compare_availability_generation=0;
  scratchbird::engine::sblr::SblrDomainOperationDescriptorV1 domain_operation_descriptor;std::uint64_t domain_operation_availability_generation=0;
  scratchbird::engine::sblr::SblrUdrInvokeDescriptorV1 udr_invoke_descriptor;std::uint64_t udr_invoke_availability_generation=0;
  scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1 procedure_invoke_descriptor;std::uint64_t procedure_invoke_availability_generation=0;
  scratchbird::engine::sblr::SblrFunctionInvokeDescriptorV1 function_invoke_descriptor;std::uint64_t function_invoke_availability_generation=0;
  scratchbird::engine::sblr::SblrAggregateInvokeDescriptorV1 aggregate_invoke_descriptor;std::uint64_t aggregate_invoke_availability_generation=0;
  scratchbird::engine::sblr::SblrSequenceNextvalDescriptorV1 sequence_nextval_descriptor;std::uint64_t sequence_nextval_availability_generation=0;
  scratchbird::engine::sblr::SblrSequenceCurrvalDescriptorV1 sequence_currval_descriptor;std::uint64_t sequence_currval_availability_generation=0;
  scratchbird::engine::sblr::SblrSequenceSetvalDescriptorV1 sequence_setval_descriptor;std::uint64_t sequence_setval_availability_generation=0;
  scratchbird::engine::sblr::SblrCreateTableAsQueryDescriptorV1 ctas_descriptor;
  scratchbird::engine::sblr::SblrDdlCreateMaterializedViewDescriptorV1 ddl_create_materialized_view_descriptor{};
  std::uint64_t ddl_create_materialized_view_availability_generation = 0;
  scratchbird::engine::sblr::SblrQueryNumericDescriptorV1 query_numeric_descriptor;std::uint64_t query_numeric_availability_generation=0;
  scratchbird::engine::sblr::SblrAdvancedDatatypeFamilyDescriptorV1 advanced_datatype_family_descriptor;std::uint64_t advanced_datatype_family_availability_generation=0;
  scratchbird::engine::sblr::SblrProjectDescriptorV1 project_descriptor;std::uint64_t project_availability_generation=0;
  scratchbird::engine::sblr::SblrAggregateDescriptorV1 aggregate_descriptor;std::uint64_t aggregate_availability_generation=0;
  scratchbird::engine::sblr::SblrGroupDescriptorV1 group_descriptor;std::uint64_t group_availability_generation=0;
  scratchbird::engine::sblr::SblrSecCreateGroupMappingDescriptorV1 security_create_group_mapping_descriptor;std::uint64_t security_create_group_mapping_availability_generation=0;
  scratchbird::engine::sblr::SblrSecDropGroupMappingDescriptorV1 security_drop_group_mapping_descriptor;std::uint64_t security_drop_group_mapping_availability_generation=0;
  scratchbird::engine::sblr::SblrSecGrantDescriptorV1 security_grant_descriptor;std::uint64_t security_grant_availability_generation=0;
  scratchbird::engine::sblr::SblrSecRevokeDescriptorV1 security_revoke_descriptor;std::uint64_t security_revoke_availability_generation=0;
  scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 security_alter_policy_descriptor;std::uint64_t security_alter_policy_availability_generation=0;
  scratchbird::engine::sblr::SblrSecDropUserDescriptorV1 security_drop_user_descriptor;std::uint64_t security_drop_user_availability_generation=0;
  scratchbird::engine::sblr::SblrSortDescriptorV1 sort_descriptor;std::uint64_t sort_availability_generation=0;
  scratchbird::engine::sblr::SblrLimitDescriptorV1 limit_descriptor;std::uint64_t limit_availability_generation=0;
  scratchbird::engine::sblr::SblrCatalogIntrospectDescriptorV1 catalog_introspect_descriptor;std::uint64_t catalog_introspect_availability_generation=0;
  scratchbird::engine::sblr::SblrWindowDescriptorV1 window_descriptor;std::uint64_t window_availability_generation=0;
  scratchbird::engine::sblr::SblrReturnResultSetDescriptorV1 return_result_set_descriptor;std::uint64_t return_result_set_availability_generation=0;
  scratchbird::engine::sblr::SblrKvStructuredReadDescriptorV1 kv_structured_read_descriptor;std::uint64_t kv_structured_read_availability_generation=0;
  scratchbird::engine::sblr::SblrKvStructuredMutateDescriptorV1 kv_structured_mutate_descriptor;std::uint64_t kv_structured_mutate_availability_generation=0;
  scratchbird::engine::sblr::SblrKvStructuredScanDescriptorV1 kv_structured_scan_descriptor;std::uint64_t kv_structured_scan_availability_generation=0;
  scratchbird::engine::sblr::SblrKvStructuredStreamReadDescriptorV1 kv_structured_stream_read_descriptor;std::uint64_t kv_structured_stream_read_availability_generation=0;
  scratchbird::engine::sblr::SblrKvStructuredStreamAppendDescriptorV1 kv_structured_stream_append_descriptor;std::uint64_t kv_structured_stream_append_availability_generation=0;
  scratchbird::engine::sblr::SblrKvStructuredTimeseriesDescriptorV1 kv_timeseries_descriptor;std::uint64_t kv_structured_timeseries_availability_generation=0;
  scratchbird::engine::sblr::SblrSystemConfigSetDescriptorV1 system_config_set_descriptor;std::uint64_t system_config_set_availability_generation=0; scratchbird::engine::sblr::SblrDdlCreateDomainDescriptorV1 ddl_create_domain_descriptor;std::uint64_t ddl_create_domain_availability_generation=0; scratchbird::engine::sblr::SblrDdlAlterDomainDescriptorV1 ddl_alter_domain_descriptor;std::uint64_t ddl_alter_domain_availability_generation=0; scratchbird::engine::sblr::SblrDdlCreateViewDescriptorV1 ddl_create_view_descriptor;std::uint64_t ddl_create_view_availability_generation=0; scratchbird::engine::sblr::SblrDdlAlterViewDescriptorV1 ddl_alter_view_descriptor;std::uint64_t ddl_alter_view_availability_generation=0; scratchbird::engine::sblr::SblrDdlDropViewDescriptorV1 ddl_drop_view_descriptor{};std::uint64_t ddl_drop_view_availability_generation=0; scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1 ddl_create_trigger_descriptor{};std::uint64_t ddl_create_trigger_availability_generation=0; scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 ddl_create_schema_descriptor;std::uint64_t ddl_create_schema_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateTableDescriptorV1 ddl_create_table_descriptor;std::uint64_t ddl_create_table_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateSequenceDescriptorV1 ddl_create_sequence_descriptor{};std::uint64_t ddl_create_sequence_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateSynonymDescriptorV1 ddl_create_synonym_descriptor{};std::uint64_t ddl_create_synonym_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateForeignTableDescriptorV1 ddl_create_foreign_table_descriptor{};std::uint64_t ddl_create_foreign_table_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateFdwDescriptorV1 ddl_create_fdw_descriptor{};std::uint64_t ddl_create_fdw_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropFdwDescriptorV1 ddl_drop_fdw_descriptor{};std::uint64_t ddl_drop_fdw_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlAlterTriggerDescriptorV1 ddl_alter_trigger_descriptor{};std::uint64_t ddl_alter_trigger_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropTriggerDescriptorV1 ddl_drop_trigger_descriptor{};std::uint64_t ddl_drop_trigger_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1 ddl_create_procedure_descriptor{};std::uint64_t ddl_create_procedure_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlAlterProcedureDescriptorV1 ddl_alter_procedure_descriptor{};std::uint64_t ddl_alter_procedure_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropProcedureDescriptorV1 ddl_drop_procedure_descriptor{};std::uint64_t ddl_drop_procedure_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateFunctionDescriptorV1 ddl_create_function_descriptor{};std::uint64_t ddl_create_function_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlAlterFunctionDescriptorV1 ddl_alter_function_descriptor{};std::uint64_t ddl_alter_function_availability_generation=0; scratchbird::engine::sblr::SblrDdlDropFunctionDescriptorV1 ddl_drop_function_descriptor{};std::uint64_t ddl_drop_function_availability_generation=0; scratchbird::engine::sblr::SblrDdlCreatePackageDescriptorV1 ddl_create_package_descriptor{};std::uint64_t ddl_create_package_availability_generation=0; scratchbird::engine::sblr::SblrDdlCreateTemporaryTableDescriptorV1 ddl_create_temporary_table_descriptor{};std::uint64_t ddl_create_temporary_table_availability_generation=0; scratchbird::engine::sblr::SblrDdlDropTemporaryTableDescriptorV1 ddl_drop_temporary_table_descriptor{};std::uint64_t ddl_drop_temporary_table_availability_generation=0; scratchbird::engine::sblr::SblrDdlRenameObjectVectorDescriptorV1 ddl_rename_object_vector_descriptor{};std::uint64_t ddl_rename_object_vector_availability_generation=0; scratchbird::engine::sblr::SblrDdlRenameObjectDescriptorV1 ddl_rename_object_descriptor{};std::uint64_t ddl_rename_object_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1 ddl_create_index_descriptor;std::uint64_t ddl_create_index_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlAlterPackageDescriptorV1 ddl_alter_package_descriptor{};std::uint64_t ddl_alter_package_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateOrReplaceSrsDescriptorV1 ddl_create_or_replace_srs_descriptor{}; std::uint64_t ddl_create_or_replace_srs_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropSrsDescriptorV1 ddl_drop_srs_descriptor{}; std::uint64_t ddl_drop_srs_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateRewriteRuleDescriptorV1 ddl_create_rewrite_rule_descriptor{}; std::uint64_t ddl_create_rewrite_rule_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlAlterRewriteRuleDescriptorV1 ddl_alter_rewrite_rule_descriptor{}; std::uint64_t ddl_alter_rewrite_rule_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropRewriteRuleDescriptorV1 ddl_drop_rewrite_rule_descriptor{}; std::uint64_t ddl_drop_rewrite_rule_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlValidateConstraintDescriptorV1 ddl_validate_constraint_descriptor{}; std::uint64_t ddl_validate_constraint_availability_generation=0;
  scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1 security_create_privilege_template_descriptor{}; std::uint64_t security_create_privilege_template_availability_generation=0;
  scratchbird::engine::sblr::SblrSecurityCreateUserDescriptorV1 security_create_user_descriptor{}; std::uint64_t security_create_user_availability_generation=0;
  scratchbird::engine::sblr::SblrSecAlterUserDescriptorV1 security_alter_user_descriptor{}; std::uint64_t security_alter_user_availability_generation=0;
  scratchbird::engine::sblr::SblrSecCreateRoleDescriptorV1 security_create_role_descriptor{}; std::uint64_t security_create_role_availability_generation=0;
  scratchbird::engine::sblr::SblrSecDropRoleDescriptorV1 security_drop_role_descriptor{}; std::uint64_t security_drop_role_availability_generation=0;
  scratchbird::engine::sblr::SblrSecCreatePolicyDescriptorV1 security_create_policy_descriptor{}; std::uint64_t security_create_policy_availability_generation=0;
  scratchbird::engine::sblr::SblrSecDropPolicyDescriptorV1 security_drop_policy_descriptor{}; std::uint64_t security_drop_policy_availability_generation=0;
  scratchbird::engine::sblr::SblrSecAlterRoleDescriptorV1 security_alter_role_descriptor{}; std::uint64_t security_alter_role_availability_generation=0;
  scratchbird::engine::sblr::SblrSecurityAlterPrivilegeTemplateDescriptorV1 security_alter_privilege_template_descriptor{}; std::uint64_t security_alter_privilege_template_availability_generation=0;
  scratchbird::engine::sblr::SblrSecurityDropPrivilegeTemplateDescriptorV1 security_drop_privilege_template_descriptor{}; std::uint64_t security_drop_privilege_template_availability_generation=0;
  scratchbird::engine::sblr::SblrDatabaseCreateTemplateCloneDescriptorV1 database_create_template_clone_descriptor{}; std::uint64_t database_create_template_clone_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateAggregateDescriptorV1 ddl_create_aggregate_descriptor{}; std::uint64_t ddl_create_aggregate_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateMacroDescriptorV1 ddl_create_macro_descriptor{}; std::uint64_t ddl_create_macro_availability_generation=0; std::vector<std::uint8_t> ddl_create_macro_result_bytes;
  scratchbird::engine::sblr::SblrDdlDropMacroDescriptorV1 ddl_drop_macro_descriptor{}; std::uint64_t ddl_drop_macro_availability_generation=0; std::vector<std::uint8_t> ddl_drop_macro_result_bytes;
  scratchbird::engine::sblr::SblrAdminRegisterExternalRelationResolverDescriptorV1 admin_register_external_relation_resolver_descriptor{}; std::uint64_t admin_register_external_relation_resolver_availability_generation=0;
  scratchbird::engine::sblr::SblrAdminUnregisterExternalRelationResolverDescriptorV1 admin_unregister_external_relation_resolver_descriptor{}; std::uint64_t admin_unregister_external_relation_resolver_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlCreateDictionaryDescriptorV1 ddl_create_dictionary_descriptor{}; std::uint64_t ddl_create_dictionary_availability_generation=0; std::vector<std::uint8_t> ddl_create_dictionary_result_bytes;
  scratchbird::engine::sblr::SblrDdlAlterAggregateDescriptorV1 ddl_alter_aggregate_descriptor{}; std::uint64_t ddl_alter_aggregate_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropAggregateDescriptorV1 ddl_drop_aggregate_descriptor{}; std::uint64_t ddl_drop_aggregate_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropDictionaryDescriptorV1 ddl_drop_dictionary_descriptor{}; std::uint64_t ddl_drop_dictionary_availability_generation=0; std::vector<std::uint8_t> ddl_drop_dictionary_result_bytes;
  scratchbird::engine::sblr::SblrDdlAlterDictionaryDescriptorV1 ddl_alter_dictionary_descriptor{}; std::uint64_t ddl_alter_dictionary_availability_generation=0; std::vector<std::uint8_t> ddl_alter_dictionary_result_bytes;
  scratchbird::engine::sblr::SblrDdlCreateContinuousViewDescriptorV1 ddl_create_continuous_view_descriptor{}; std::uint64_t ddl_create_continuous_view_availability_generation=0; std::vector<std::uint8_t> ddl_create_continuous_view_result_bytes;
  scratchbird::engine::sblr::SblrDdlAlterContinuousViewDescriptorV1 ddl_alter_continuous_view_descriptor{}; std::uint64_t ddl_alter_continuous_view_availability_generation=0; std::vector<std::uint8_t> ddl_alter_continuous_view_result_bytes;
  scratchbird::engine::sblr::SblrDdlDropContinuousViewDescriptorV1 ddl_drop_continuous_view_descriptor{}; std::uint64_t ddl_drop_continuous_view_availability_generation=0; std::vector<std::uint8_t> ddl_drop_continuous_view_result_bytes;
  scratchbird::engine::sblr::SblrDmlAsyncInsertSubmitDescriptorV1 dml_async_insert_submit_descriptor{}; std::uint64_t dml_async_insert_submit_availability_generation=0; std::vector<std::uint8_t> dml_async_insert_submit_result_bytes;
  scratchbird::engine::sblr::SblrDmlAsyncInsertStatusDescriptorV1 dml_async_insert_status_descriptor{}; std::uint64_t dml_async_insert_status_availability_generation=0; std::vector<std::uint8_t> dml_async_insert_status_result_bytes;
  scratchbird::engine::sblr::SblrDmlAsyncInsertCancelDescriptorV1 dml_async_insert_cancel_descriptor{}; std::uint64_t dml_async_insert_cancel_availability_generation=0; std::vector<std::uint8_t> dml_async_insert_cancel_result_bytes;
  scratchbird::engine::sblr::SblrDdlPurgeSystemHistoryDescriptorV1 ddl_purge_system_history_descriptor{}; std::uint64_t ddl_purge_system_history_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlSetIndexOptimizerEligibilityDescriptorV1 ddl_set_index_optimizer_eligibility_descriptor{}; std::uint64_t ddl_set_index_optimizer_eligibility_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementDescriptorV1 ddl_set_table_type_enforcement_descriptor{}; std::uint64_t ddl_set_table_type_enforcement_availability_generation=0;
  scratchbird::engine::sblr::SblrDatabaseSerializeLogicalSnapshotDescriptorV1 database_serialize_logical_snapshot_descriptor{}; std::uint64_t database_serialize_logical_snapshot_availability_generation=0;
  scratchbird::engine::sblr::SblrDatabaseDeserializeLogicalSnapshotDescriptorV1 database_deserialize_logical_snapshot_descriptor{}; std::uint64_t database_deserialize_logical_snapshot_availability_generation=0;
  scratchbird::engine::sblr::SblrDdlDropIndexDescriptorV1 ddl_drop_index_descriptor;std::uint64_t ddl_drop_index_availability_generation=0;
  scratchbird::engine::internal_api::SblrSavepointSnapshot savepoint_snapshot;
  std::uint64_t savepoint_availability_generation = 0;
  scratchbird::engine::sblr::SblrSavepointReleaseOperandV1 savepoint_release_operand;
  std::uint64_t savepoint_release_availability_generation = 0;
  scratchbird::engine::sblr::SblrSavepointRollbackOperandV1 savepoint_rollback_operand;
  std::uint64_t savepoint_rollback_availability_generation = 0;
  scratchbird::engine::sblr::SblrAutonomousFrameDescriptorV1 autonomous_frame_descriptor;
  std::uint64_t autonomous_frame_availability_generation = 0;
  scratchbird::engine::sblr::SblrReservationReleaseDescriptorV1 reservation_release_descriptor;
  std::uint64_t reservation_release_availability_generation = 0;
  scratchbird::engine::sblr::SblrTemporaryInstanceCleanupDescriptorV1 temporary_cleanup_descriptor;
  std::uint64_t temporary_cleanup_availability_generation = 0;
  scratchbird::engine::sblr::SblrCursorOpenDescriptorV1 cursor_open_descriptor;
  std::uint64_t cursor_open_availability_generation = 0;
  scratchbird::engine::sblr::SblrCursorFetchOperandV1 cursor_fetch_operand;
  std::uint64_t cursor_fetch_availability_generation = 0;
  scratchbird::engine::sblr::SblrCursorCloseOperandV1 cursor_close_operand;
  std::uint64_t cursor_close_availability_generation = 0;
  scratchbird::engine::sblr::SblrReadByKeyDescriptorV1 read_by_key_descriptor;
  std::uint64_t read_by_key_availability_generation = 0;
  scratchbird::engine::sblr::SblrReadRangeDescriptorV1 read_range_descriptor;
  std::uint64_t read_range_availability_generation = 0;
  scratchbird::engine::sblr::SblrReadStreamDescriptorV1 read_stream_descriptor;
  std::uint64_t read_stream_availability_generation = 0;
  scratchbird::engine::sblr::SblrResultSetPassDescriptorV1 result_set_pass_descriptor;
  std::uint64_t result_set_pass_availability_generation = 0;
  scratchbird::engine::sblr::SblrAccessCursorOpenDescriptorV1 access_cursor_open_descriptor;
  std::uint64_t access_cursor_open_availability_generation = 0;
  scratchbird::engine::sblr::SblrAccessCursorFetchDescriptorV1 access_cursor_fetch_descriptor;
  std::uint64_t access_cursor_fetch_availability_generation = 0;
  scratchbird::engine::sblr::SblrAccessCursorCloseDescriptorV1 access_cursor_close_descriptor;
  std::uint64_t access_cursor_close_availability_generation = 0;
  scratchbird::engine::sblr::SblrInsertDescriptorV1 insert_descriptor;
  std::uint64_t insert_availability_generation = 0;
  scratchbird::engine::sblr::SblrUpdateDescriptorV1 update_descriptor;std::uint64_t update_availability_generation=0;
  scratchbird::engine::sblr::SblrDeleteDescriptorV1 delete_descriptor;std::uint64_t delete_availability_generation=0;
  scratchbird::engine::sblr::SblrMergeDescriptorV1 merge_descriptor;std::uint64_t merge_availability_generation=0;
  scratchbird::engine::sblr::SblrTableTruncateDescriptorV1 table_truncate_descriptor;std::uint64_t table_truncate_availability_generation=0;
  scratchbird::engine::sblr::SblrTableAnalyzeDescriptorV1 table_analyze_descriptor;std::uint64_t table_analyze_availability_generation=0;
  if (opcode_stream) {
    scratchbird::engine::sblr::SblrOpcodeStreamAdmission stream_admission;
    stream_admission.admitted_registry_snapshot_uuid = view.catalog_epoch_uuid;
    stream_admission.authenticated = context.security_context_present;
    stream_admission.descriptor_class_accepted =
        stream.stream.package_descriptor_uuid == view.bound_ast_uuid;
    stream_admission.gateway_pass_through =
        request->gateway_evidence.source ==
            scratchbird::server_engine_bridge::
                StatementGatewayEvidenceSource::kLocalObserved &&
        request->gateway_evidence.disposition ==
            scratchbird::server_engine_bridge::
                StatementGatewayDisposition::kPassThrough &&
        request->gateway_evidence.provider_observation_generation == 1 &&
        request->gateway_evidence.canonical_payload_sha256 ==
            request->operation_sha256;
    stream_admission.executor_evidence_accepted =
        request->package_executor_evidence.begin_executor_id ==
            "engine.op.package_begin" &&
        request->package_executor_evidence.end_executor_id ==
            "engine.op.package_end" &&
        request->package_executor_evidence.registry_snapshot_uuid ==
            view.catalog_epoch_uuid &&
        request->package_executor_evidence.executor_evidence_generation == 1 &&
        request->package_executor_evidence.canonical_payload_sha256 ==
            request->operation_sha256;
    stream_admission.cancelled = cancellation_observed();
    stream_admission.resource_budget_available =
        resource_guard.ledger != nullptr && !resource_guard.token_id.empty();
    const auto admitted_stream =
        scratchbird::engine::sblr::AdmitSblrOpcodeStream(
            operation_bytes, stream_admission);
    if (!admitted_stream.ok) {
      if (admitted_stream.diagnostic_id == "PROCESS.CANCELLED") {
        resource_guard.reason = ResourceReleaseReason::kCancel;
      } else if (admitted_stream.diagnostic_id == "PROCESS.TIMEOUT" ||
                 admitted_stream.diagnostic_id == "TIMEOUT") {
        resource_guard.reason = ResourceReleaseReason::kTimeout;
      }
      const auto refusal_status = [&]() {
        if (admitted_stream.diagnostic_id == "SECURITY.ACCESS_DENIED")
          return SB_ENGINE_STATUS_SECURITY_DENIED;
        if (admitted_stream.diagnostic_id == "RESOURCE.BUDGET_EXCEEDED")
          return SB_ENGINE_STATUS_RESOURCE_EXHAUSTED;
        if (admitted_stream.diagnostic_id == "PROCESS.CANCELLED")
          return SB_ENGINE_STATUS_TIMEOUT;
        if (admitted_stream.diagnostic_id == "PROCESS.CLUSTER_PATH_ABSENT")
          return SB_ENGINE_STATUS_CONFLICT;
        if (admitted_stream.diagnostic_id ==
            "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING")
          return SB_ENGINE_STATUS_UNSUPPORTED;
        return SB_ENGINE_STATUS_INVALID_ARGUMENT;
      }();
      return fail_result(
          refusal_status,
          out_result,
          4062,
          admitted_stream.diagnostic_id,
          "sblr.opcode_stream.rejected",
          admitted_stream.detail);
    }
    // Receipt dispatch v1 admits exactly one contained root. Validate its
    // complete envelope, registry identity, and context requirements before
    // entering any executor so a late package member can never follow an
    // earlier side effect.
    auto member = admitted_stream.stream.operations[1];
    source_map_root = member.operation_id == "engine.op.source_map" &&
        member.opcode == "SBLR_SOURCE_MAP" && member.opcode_code == 6;
    error_vector_root = member.operation_id == "engine.op.error_vector" &&
        member.opcode == "SBLR_ERROR_VECTOR" && member.opcode_code == 7;
    txn_begin_root = member.operation_id == "engine.op.txn_begin" &&
        member.opcode == "SBLR_TXN_BEGIN" && member.opcode_code == 256;
    txn_commit_root = member.operation_id == "engine.op.txn_commit" &&
        member.opcode == "SBLR_TXN_COMMIT" && member.opcode_code == 257;
    txn_rollback_root = member.operation_id == "engine.op.txn_rollback" &&
        member.opcode == "SBLR_TXN_ROLLBACK" && member.opcode_code == 258;
    txn_savepoint_root = member.operation_id == "engine.op.txn_savepoint" &&
        member.opcode == "SBLR_TXN_SAVEPOINT" && member.opcode_code == 259;
    txn_release_savepoint_root = member.operation_id == "engine.op.txn_release_savepoint" &&
        member.opcode == "SBLR_TXN_RELEASE_SAVEPOINT" && member.opcode_code == 260;
    txn_rollback_to_savepoint_root = member.operation_id == "engine.op.txn_rollback_to_savepoint" && member.opcode == "SBLR_TXN_ROLLBACK_TO_SAVEPOINT" && member.opcode_code == 261;
    psql_autonomous_frame_root = member.operation_id == "engine.op.psql_autonomous_frame" && member.opcode == "SBLR_PSQL_AUTONOMOUS_FRAME" && member.opcode_code == 262;
    reservation_release_root = member.operation_id == "engine.op.transaction_reservation_release" && member.opcode == "SBLR_TRANSACTION_RESERVATION_RELEASE" && member.opcode_code == 263;
    temporary_cleanup_root = member.operation_id == "engine.op.temporary_instance_cleanup" && member.opcode == "SBLR_TEMPORARY_INSTANCE_CLEANUP" && member.opcode_code == 264;
    cursor_open_root = member.operation_id == "engine.op.cursor_open" && member.opcode == "SBLR_CURSOR_OPEN" && member.opcode_code == 512;
    cursor_fetch_root = member.operation_id == "engine.op.cursor_fetch" && member.opcode == "SBLR_CURSOR_FETCH" && member.opcode_code == 513;
    cursor_close_root = member.operation_id == "engine.op.cursor_close" && member.opcode == "SBLR_CURSOR_CLOSE" && member.opcode_code == 514;
    read_by_key_root = member.operation_id == "engine.op.read_by_key" && member.opcode == "SBLR_READ_BY_KEY" && member.opcode_code == 515;
    read_range_root = member.operation_id == "engine.op.read_range" && member.opcode == "SBLR_READ_RANGE" && member.opcode_code == 516;
    read_stream_root = member.operation_id == "engine.op.read_stream" && member.opcode == "SBLR_READ_STREAM" && member.opcode_code == 517;
    result_set_pass_root = member.operation_id == "engine.op.result_set_pass" && member.opcode == "SBLR_RESULT_SET_PASS" && member.opcode_code == 518;
    access_cursor_open_root = member.operation_id == "engine.op.access_cursor_open" && member.opcode == "SBLR_ACCESS_CURSOR_OPEN" && member.opcode_code == 519;
    access_cursor_fetch_root = member.operation_id == "engine.op.access_cursor_fetch" && member.opcode == "SBLR_ACCESS_CURSOR_FETCH" && member.opcode_code == 520;
    access_cursor_close_root = member.operation_id == "engine.op.access_cursor_close" && member.opcode == "SBLR_ACCESS_CURSOR_CLOSE" && member.opcode_code == 521;
    insert_root = member.operation_id == "engine.op.insert" && member.opcode == "SBLR_INSERT" && member.opcode_code == 768;
    update_root = member.operation_id == "engine.op.update" && member.opcode == "SBLR_UPDATE" && member.opcode_code == 769;
    delete_root = member.operation_id == "engine.op.delete" && member.opcode == "SBLR_DELETE" && member.opcode_code == 770;
    merge_root = member.operation_id == "engine.op.merge" && member.opcode == "SBLR_MERGE" && member.opcode_code == 771;
    table_truncate_root = member.operation_id == "engine.op.table_truncate" && member.opcode == "SBLR_TABLE_TRUNCATE" && member.opcode_code == 773;
    table_analyze_root = member.operation_id == "engine.op.table_analyze" && member.opcode == "SBLR_TABLE_ANALYZE" && member.opcode_code == 774;
    bulk_import_stream_root = member.operation_id == "engine.op.bulk_import_stream" && member.opcode == "SBLR_BULK_IMPORT_STREAM" && member.opcode_code == 775;
    bulk_export_stream_root = member.operation_id == "engine.op.bulk_export_stream" && member.opcode == "SBLR_BULK_EXPORT_STREAM" && member.opcode_code == 776;
    statement_batch_root = member.operation_id == "engine.op.statement_batch" && member.opcode == "SBLR_STATEMENT_BATCH" && member.opcode_code == 777;
    atomic_cas_root = member.operation_id == "engine.op.atomic_cas" && member.opcode == "SBLR_ATOMIC_CAS" && member.opcode_code == 778;
    atomic_rmw_root = member.operation_id == "engine.op.atomic_read_modify_write" && member.opcode == "SBLR_ATOMIC_READ_MODIFY_WRITE" && member.opcode_code == 779;
    advisory_lock_root = member.operation_id == "engine.op.advisory_lock_acquire" && member.opcode == "SBLR_ADVISORY_LOCK_ACQUIRE" && member.opcode_code == 780;
    advisory_lock_release_root = member.operation_id == "engine.op.advisory_lock_release" && member.opcode == "SBLR_ADVISORY_LOCK_RELEASE" && member.opcode_code == 781;
    function_call_root = member.operation_id == "engine.op.function_call" && member.opcode == "SBLR_FUNCTION_CALL" && member.opcode_code == 1024;
    operator_call_root = member.operation_id == "engine.op.operator_call" && member.opcode == "SBLR_OPERATOR_CALL" && member.opcode_code == 1025;
    cast_root = member.operation_id == "engine.op.cast" && member.opcode == "SBLR_CAST" && member.opcode_code == 1026;
    compare_root = member.operation_id == "engine.op.compare" && member.opcode == "SBLR_COMPARE" && member.opcode_code == 1027;
    domain_operation_root = member.operation_id == "engine.op.domain_operation" && member.opcode == "SBLR_DOMAIN_OPERATION" && member.opcode_code == 1028;
    udr_invoke_root = member.operation_id == "engine.op.udr_invoke" && member.opcode == "SBLR_UDR_INVOKE" && member.opcode_code == 1029;
    procedure_invoke_root = member.operation_id == "engine.op.procedure_invoke" && member.opcode == "SBLR_PROCEDURE_INVOKE" && member.opcode_code == 1030;
    function_invoke_root = member.operation_id == "engine.op.function_invoke" && member.opcode == "SBLR_FUNCTION_INVOKE" && member.opcode_code == 1031;
    aggregate_invoke_root = member.operation_id == "engine.op.aggregate_invoke" && member.opcode == "SBLR_AGGREGATE_INVOKE" && member.opcode_code == 1032;
    sequence_nextval_root = member.operation_id == "engine.op.sequence_nextval" && member.opcode == "SBLR_SEQUENCE_NEXTVAL" && member.opcode_code == 1033;
    sequence_currval_root = member.operation_id == "engine.op.sequence_currval" && member.opcode == "SBLR_SEQUENCE_CURRVAL" && member.opcode_code == 1034;
    sequence_setval_root = member.operation_id == "engine.op.sequence_setval" && member.opcode == "SBLR_SEQUENCE_SETVAL" && member.opcode_code == 1035;
    query_numeric_root = member.operation_id == "engine.op.query_apply_numeric_operation" && member.opcode == "SBLR_QUERY_APPLY_NUMERIC_OPERATION" && member.opcode_code == 1036;
    advanced_datatype_family_root = member.operation_id == "engine.op.query_evaluate_advanced_datatype_family" && member.opcode == "SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY" && member.opcode_code == 1037;
    show_version_root = member.operation_id == "observability.show_version" && member.opcode == "SBLR_OBSERVABILITY_SHOW_VERSION" && member.opcode_code == 3334;
    catalog_introspect_root = member.operation_id == "engine.op.catalog_introspect" && member.opcode == "SBLR_CATALOG_INTROSPECT" && member.opcode_code == 4864;
    project_root = member.operation_id == "engine.op.project" && member.opcode == "SBLR_PROJECT" && member.opcode_code == 1280;
    aggregate_root = member.operation_id == "engine.op.aggregate" && member.opcode == "SBLR_AGGREGATE" && member.opcode_code == 1281;
    group_root = member.operation_id == "engine.op.group" && member.opcode == "SBLR_GROUP" && member.opcode_code == 1282;
    security_create_group_mapping_root = member.operation_id == "engine.op.sec_create_group_mapping" && member.opcode == "SBLR_SEC_CREATE_GROUP_MAPPING" && member.opcode_code == 1797;
    security_drop_group_mapping_root = member.operation_id == "engine.op.sec_drop_group_mapping" && member.opcode == "SBLR_SEC_DROP_GROUP_MAPPING" && member.opcode_code == 1806;
    security_grant_root = member.operation_id == "engine.op.sec_grant" && member.opcode == "SBLR_SEC_GRANT" && member.opcode_code == 1795;
    security_revoke_root = member.operation_id == "engine.op.sec_revoke" && member.opcode == "SBLR_SEC_REVOKE" && member.opcode_code == 1796;
    security_alter_policy_root = member.operation_id == "engine.op.sec_alter_policy" && member.opcode == "SBLR_SEC_ALTER_POLICY" && member.opcode_code == 1798;
    security_drop_user_root = member.operation_id == "engine.op.sec_drop_user" && member.opcode == "SBLR_SEC_DROP_USER" && member.opcode_code == 1799;
    sort_root = member.operation_id == "engine.op.sort" && member.opcode == "SBLR_SORT" && member.opcode_code == 1283;
    limit_root = member.operation_id == "engine.op.limit" && member.opcode == "SBLR_LIMIT" && member.opcode_code == 1284;
    window_root = member.operation_id == "engine.op.window" && member.opcode == "SBLR_WINDOW" && member.opcode_code == 1285;
    return_result_set_root = member.operation_id == "engine.op.return_result_set" && member.opcode == "SBLR_RETURN_RESULT_SET" && member.opcode_code == 1286;
    kv_structured_read_root = member.operation_id == "engine.op.kv_structured_read" && member.opcode == "SBLR_KV_STRUCTURED_READ" && member.opcode_code == 8192;
    kv_structured_mutate_root = member.operation_id == "engine.op.kv_structured_mutate" && member.opcode == "SBLR_KV_STRUCTURED_MUTATE" && member.opcode_code == 8193;
    kv_structured_scan_root = member.operation_id == "engine.op.kv_structured_scan" && member.opcode == "SBLR_KV_STRUCTURED_SCAN" && member.opcode_code == 8194;
    kv_structured_stream_read_root = member.operation_id == "engine.op.kv_structured_stream_read" && member.opcode == "SBLR_KV_STRUCTURED_STREAM_READ" && member.opcode_code == 8195;
    kv_structured_stream_append_root = member.operation_id == "engine.op.kv_structured_stream_append" && member.opcode == "SBLR_KV_STRUCTURED_STREAM_APPEND" && member.opcode_code == 8196;
    kv_structured_timeseries_root = member.operation_id == "engine.op.kv_structured_timeseries" && member.opcode == "SBLR_KV_STRUCTURED_TIMESERIES" && member.opcode_code == 8197;
    system_config_set_root = member.operation_id == "engine.op.system_config_set" && member.opcode == "SBLR_SYSTEM_CONFIG_SET" && member.opcode_code == 5125;
    ddl_create_domain_root = member.operation_id == "engine.op.ddl_create_domain" && member.opcode == "SBLR_DDL_CREATE_DOMAIN" && member.opcode_code == 1542;
    ddl_alter_domain_root = member.operation_id == "engine.op.ddl_alter_domain" && member.opcode == "SBLR_DDL_ALTER_DOMAIN" && member.opcode_code == 1547;
    ddl_create_view_root = member.operation_id == "engine.op.ddl_create_view" && member.opcode == "SBLR_DDL_CREATE_VIEW" && member.opcode_code == 1548;
    ddl_alter_view_root = member.operation_id == "engine.op.ddl_alter_view" && member.opcode == "SBLR_DDL_ALTER_VIEW" && member.opcode_code == 1549;
    ddl_drop_view_root = member.operation_id == "engine.op.ddl_drop_view" && member.opcode == "SBLR_DDL_DROP_VIEW" && member.opcode_code == 1550;
    ddl_refresh_materialized_view_root = member.operation_id == "engine.op.ddl_refresh_materialized_view" && member.opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW" && member.opcode_code == 1567;
    ddl_create_materialized_view_root = member.operation_id == "engine.op.ddl_create_materialized_view" && member.opcode == "SBLR_DDL_CREATE_MATERIALIZED_VIEW" && member.opcode_code == 1566;
    ddl_drop_materialized_view_root = member.operation_id == "engine.op.ddl_drop_materialized_view" && member.opcode == "SBLR_DDL_DROP_MATERIALIZED_VIEW" && member.opcode_code == 1568;
    ddl_create_table_as_query_with_data_root = member.operation_id == "engine.op.ddl_create_table_as_query_with_data" && member.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA" && member.opcode_code == 1669;
    ddl_create_table_as_query_with_no_data_root = member.operation_id == "engine.op.ddl_create_table_as_query_with_no_data" && member.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA" && member.opcode_code == 1670;
    ddl_drop_package_root = member.operation_id == "engine.op.ddl_drop_package" && member.opcode == "SBLR_DDL_DROP_PACKAGE" && member.opcode_code == 1562;
    ddl_drop_synonym_root = member.operation_id == "engine.op.ddl_drop_synonym" && member.opcode == "SBLR_DDL_DROP_SYNONYM" && member.opcode_code == 1575;
    ddl_drop_foreign_table_root = member.operation_id == "engine.op.ddl_drop_foreign_table" && member.opcode == "SBLR_DDL_DROP_FOREIGN_TABLE" && member.opcode_code == 1577;
    ddl_alter_package_root = member.operation_id == "engine.op.ddl_alter_package" && member.opcode == "SBLR_DDL_ALTER_PACKAGE" && member.opcode_code == 1561;
    ddl_create_trigger_root = member.operation_id == "engine.op.ddl_create_trigger" && member.opcode == "SBLR_DDL_CREATE_TRIGGER" && member.opcode_code == 1551;
    ddl_alter_trigger_root = member.operation_id == "engine.op.ddl_alter_trigger" && member.opcode == "SBLR_DDL_ALTER_TRIGGER" && member.opcode_code == 1552;
    ddl_drop_trigger_root = member.operation_id == "engine.op.ddl_drop_trigger" && member.opcode == "SBLR_DDL_DROP_TRIGGER" && member.opcode_code == 1553;
    ddl_create_procedure_root = member.operation_id == "engine.op.ddl_create_procedure" && member.opcode == "SBLR_DDL_CREATE_PROCEDURE" && member.opcode_code == 1554;
    ddl_alter_procedure_root = member.operation_id == "engine.op.ddl_alter_procedure" && member.opcode == "SBLR_DDL_ALTER_PROCEDURE" && member.opcode_code == 1555;
    ddl_drop_procedure_root = member.operation_id == "engine.op.ddl_drop_procedure" && member.opcode == "SBLR_DDL_DROP_PROCEDURE" && member.opcode_code == 1556;
    ddl_create_function_root = member.operation_id == "engine.op.ddl_create_function" && member.opcode == "SBLR_DDL_CREATE_FUNCTION" && member.opcode_code == 1557;
    ddl_alter_function_root = member.operation_id == "engine.op.ddl_alter_function" && member.opcode == "SBLR_DDL_ALTER_FUNCTION" && member.opcode_code == 1558;
    ddl_drop_function_root = member.operation_id == "engine.op.ddl_drop_function" && member.opcode == "SBLR_DDL_DROP_FUNCTION" && member.opcode_code == 1559;
    ddl_create_package_root = member.operation_id == "engine.op.ddl_create_package" && member.opcode == "SBLR_DDL_CREATE_PACKAGE" && member.opcode_code == 1560;
    ddl_create_sequence_root = member.operation_id == "engine.op.ddl_create_sequence" && member.opcode == "SBLR_DDL_CREATE_SEQUENCE" && member.opcode_code == 1671;
    ddl_alter_sequence_root = member.operation_id == "engine.op.ddl_alter_sequence" && member.opcode == "SBLR_DDL_ALTER_SEQUENCE" && member.opcode_code == 1564;
    ddl_drop_type_root = member.operation_id == "engine.op.ddl_drop_type" && member.opcode == "SBLR_DDL_DROP_TYPE" && member.opcode_code == 1571;
    ddl_rename_object_root = member.operation_id == "engine.op.ddl_rename_object" && member.opcode == "SBLR_DDL_RENAME_OBJECT" && member.opcode_code == 1572;
    ddl_create_synonym_root = member.operation_id == "engine.op.ddl_create_synonym" && member.opcode == "SBLR_DDL_CREATE_SYNONYM" && member.opcode_code == 1574;
    if (ddl_create_synonym_root) { std::string detail; if (member.operands.size() != 1 || member.operands.front().type != "create_synonym_descriptor" || member.operands.front().name != "synonym" || !scratchbird::engine::sblr::DecodeSblrDdlCreateSynonymDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_create_synonym_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4136, "SBLR.OPERAND_INVALID", "sblr.ddl_create_synonym.operand_invalid", detail); ddl_create_synonym_availability_generation = ddl_create_synonym_descriptor.availability; }
    ddl_create_foreign_table_root = member.operation_id == "engine.op.ddl_create_foreign_table" && member.opcode == "SBLR_DDL_CREATE_FOREIGN_TABLE" && member.opcode_code == 1576;
    if (ddl_create_foreign_table_root) { std::string detail; if (member.operands.size() != 1 || member.operands.front().type != "create_foreign_table_descriptor" || member.operands.front().name != "foreign_table" || !scratchbird::engine::sblr::DecodeSblrDdlCreateForeignTableDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_create_foreign_table_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4137, "SBLR.OPERAND_INVALID", "sblr.ddl_create_foreign_table.operand_invalid", detail); ddl_create_foreign_table_availability_generation = ddl_create_foreign_table_descriptor.availability; }
    ddl_create_fdw_root = member.operation_id == "engine.op.ddl_create_fdw" && member.opcode == "SBLR_DDL_CREATE_FDW" && member.opcode_code == 1578;
    if (ddl_create_fdw_root) { std::string detail; if (member.operands.size() != 1 || member.operands.front().type != "create_fdw_descriptor" || member.operands.front().name != "fdw" || !scratchbird::engine::sblr::DecodeSblrDdlCreateFdwDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_create_fdw_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4157, "SBLR.OPERAND_INVALID", "sblr.ddl_create_fdw.operand_invalid", detail); ddl_create_fdw_availability_generation = ddl_create_fdw_descriptor.availability; }
    ddl_drop_fdw_root = member.operation_id == "engine.op.ddl_drop_fdw" && member.opcode == "SBLR_DDL_DROP_FDW" && member.opcode_code == 1579;
    if (ddl_drop_fdw_root) { std::string detail; if (member.operands.size() != 1 || member.operands.front().type != "drop_fdw_descriptor" || member.operands.front().name != "fdw" || !scratchbird::engine::sblr::DecodeSblrDdlDropFdwDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_drop_fdw_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4159, "SBLR.OPERAND_INVALID", "sblr.ddl_drop_fdw.operand_invalid", detail); ddl_drop_fdw_availability_generation = ddl_drop_fdw_descriptor.availability; }
    if (ddl_rename_object_root) { std::string detail; if (member.operands.size() != 1 || member.operands.front().type != "rename_object_descriptor" || member.operands.front().name != "rename" || !scratchbird::engine::sblr::DecodeSblrDdlRenameObjectDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_rename_object_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4136, "SBLR.OPERAND_INVALID", "sblr.ddl_rename_object.operand_invalid", detail); ddl_rename_object_availability_generation = ddl_rename_object_descriptor.availability; }
    ddl_drop_sequence_root = member.operation_id == "engine.op.ddl_drop_sequence" && member.opcode == "SBLR_DDL_DROP_SEQUENCE" && member.opcode_code == 1565;
    ddl_create_temporary_table_root = member.operation_id == "engine.op.ddl_create_temporary_table" && member.opcode == "SBLR_DDL_CREATE_TEMPORARY_TABLE" && member.opcode_code == 1561;
    ddl_drop_temporary_table_root = member.operation_id == "engine.op.ddl_drop_temporary_table" && member.opcode == "SBLR_DDL_DROP_TEMPORARY_TABLE" && member.opcode_code == 1562;
    ddl_rename_object_vector_root = member.operation_id == "engine.op.ddl_rename_object_vector" && member.opcode == "SBLR_DDL_RENAME_OBJECT_VECTOR" && member.opcode_code == 1563;
    ddl_create_or_replace_srs_root = member.operation_id == "engine.op.ddl_create_or_replace_srs" && member.opcode == "SBLR_DDL_CREATE_OR_REPLACE_SRS" && member.opcode_code == 1615;
    ddl_drop_srs_root = member.operation_id == "engine.op.ddl_drop_srs" && member.opcode == "SBLR_DDL_DROP_SRS" && member.opcode_code == 1616;
    ddl_create_rewrite_rule_root = member.operation_id == "engine.op.ddl_create_rewrite_rule" && member.opcode == "SBLR_DDL_CREATE_REWRITE_RULE" && member.opcode_code == 1617;
    ddl_alter_rewrite_rule_root = member.operation_id == "engine.op.ddl_alter_rewrite_rule" && member.opcode == "SBLR_DDL_ALTER_REWRITE_RULE" && member.opcode_code == 1618;
    ddl_drop_rewrite_rule_root = member.operation_id == "engine.op.ddl_drop_rewrite_rule" && member.opcode == "SBLR_DDL_DROP_REWRITE_RULE" && member.opcode_code == 1619;
    ddl_validate_constraint_root = member.operation_id == "engine.op.ddl_validate_constraint" && member.opcode == "SBLR_DDL_VALIDATE_CONSTRAINT" && member.opcode_code == 1620;
    security_create_privilege_template_root = member.operation_id == "engine.op.security_create_privilege_template" && member.opcode == "SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE" && member.opcode_code == 1621;
    security_create_user_root = member.operation_id == "engine.op.security_create_user" && member.opcode == "SBLR_SEC_CREATE_USER" && member.opcode_code == 1792;
    security_alter_user_root = member.operation_id == "engine.op.sec_alter_user" && member.opcode == "SBLR_SEC_ALTER_USER" && member.opcode_code == 1793;
    security_create_role_root = member.operation_id == "engine.op.sec_create_role" && member.opcode == "SBLR_SEC_CREATE_ROLE" && member.opcode_code == 1794;
    security_drop_role_root = member.operation_id == "engine.op.sec_drop_role" && member.opcode == "SBLR_SEC_DROP_ROLE" && member.opcode_code == 1801;
  security_create_policy_root = member.operation_id == "engine.op.sec_create_policy" && member.opcode == "SBLR_SEC_CREATE_POLICY" && member.opcode_code == 1802;
    security_drop_policy_root = member.operation_id == "engine.op.sec_drop_policy" && member.opcode == "SBLR_SEC_DROP_POLICY" && member.opcode_code == 1803;
    security_alter_role_root = member.operation_id == "engine.op.sec_alter_role" && member.opcode == "SBLR_SEC_ALTER_ROLE" && member.opcode_code == 1800;
    security_alter_privilege_template_root = member.operation_id == "engine.op.security_alter_privilege_template" && member.opcode == "SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE" && member.opcode_code == 1622;
    security_drop_privilege_template_root = member.operation_id == "engine.op.security_drop_privilege_template" && member.opcode == "SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE" && member.opcode_code == 1623;
    database_create_template_clone_root = member.operation_id == "engine.op.database_create_template_clone" && member.opcode == "SBLR_DATABASE_CREATE_TEMPLATE_CLONE" && member.opcode_code == 1624;
    ddl_create_aggregate_root = member.operation_id == "engine.op.ddl_create_aggregate" && member.opcode == "SBLR_DDL_CREATE_AGGREGATE" && member.opcode_code == 1625;
    ddl_create_macro_root = member.operation_id == "engine.op.ddl_create_macro" && member.opcode == "SBLR_DDL_CREATE_MACRO" && member.opcode_code == 1633;
    ddl_drop_macro_root = member.operation_id == "engine.op.ddl_drop_macro" && member.opcode == "SBLR_DDL_DROP_MACRO" && member.opcode_code == 1634;
    admin_register_external_relation_resolver_root = member.operation_id == "engine.op.admin_register_external_relation_resolver" && member.opcode == "SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER" && member.opcode_code == 1635;
    admin_unregister_external_relation_resolver_root = member.operation_id == "engine.op.admin_unregister_external_relation_resolver" && member.opcode == "SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER" && member.opcode_code == 1636;
    ddl_create_dictionary_root = member.operation_id == "engine.op.ddl_create_dictionary" && member.opcode == "SBLR_DDL_CREATE_DICTIONARY" && (member.opcode_code == 1637 || member.opcode_code == 1608);
    ddl_alter_aggregate_root = member.operation_id == "engine.op.ddl_alter_aggregate" && member.opcode == "SBLR_DDL_ALTER_AGGREGATE" && member.opcode_code == 1626;
    ddl_drop_aggregate_root = member.operation_id == "engine.op.ddl_drop_aggregate" && member.opcode == "SBLR_DDL_DROP_AGGREGATE" && member.opcode_code == 1627;
    ddl_drop_dictionary_root = member.operation_id == "engine.op.ddl_drop_dictionary" && member.opcode == "SBLR_DDL_DROP_DICTIONARY" && member.opcode_code == 1638;
    ddl_alter_dictionary_root = member.operation_id == "engine.op.ddl_alter_dictionary" && member.opcode == "SBLR_DDL_ALTER_DICTIONARY" && member.opcode_code == 1639;
    ddl_create_continuous_view_root = member.operation_id == "engine.op.ddl_create_continuous_view" && member.opcode == "SBLR_DDL_CREATE_CONTINUOUS_VIEW" && member.opcode_code == 1640;
    ddl_alter_continuous_view_root = member.operation_id == "engine.op.ddl_alter_continuous_view" && member.opcode == "SBLR_DDL_ALTER_CONTINUOUS_VIEW" && member.opcode_code == 1641;
    ddl_drop_continuous_view_root = member.operation_id == "engine.op.ddl_drop_continuous_view" && member.opcode == "SBLR_DDL_DROP_CONTINUOUS_VIEW" && member.opcode_code == 1642;
    dml_async_insert_submit_root = member.operation_id == "engine.op.dml_async_insert_submit" && member.opcode == "SBLR_DML_ASYNC_INSERT_SUBMIT" && member.opcode_code == 1643;
    dml_async_insert_status_root = member.operation_id == "engine.op.dml_async_insert_status" && member.opcode == "SBLR_DML_ASYNC_INSERT_STATUS" && member.opcode_code == 1644;
    dml_async_insert_cancel_root = member.operation_id == "engine.op.dml_async_insert_cancel" && member.opcode == "SBLR_DML_ASYNC_INSERT_CANCEL" && member.opcode_code == 1645;
    dml_counter_add_root = member.operation_id == "engine.op.dml_counter_add" && member.opcode == "SBLR_DML_COUNTER_ADD" && member.opcode_code == 1647;
    dml_timeseries_schema_write_root = member.operation_id == "engine.op.dml_timeseries_schema_write" && member.opcode == "SBLR_DML_TIMESERIES_SCHEMA_WRITE" && member.opcode_code == 1648;
    ddl_timeseries_series_cardinality_policy_root = member.operation_id == "engine.op.ddl_set_timeseries_series_cardinality_policy" && member.opcode == "SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY" && member.opcode_code == 1649;
    ddl_create_timeseries_value_cache_root = member.operation_id == "engine.op.ddl_create_timeseries_value_cache" && member.opcode == "SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE" && member.opcode_code == 1650;
    ddl_purge_system_history_root = member.operation_id == "engine.op.ddl_purge_system_history" && member.opcode == "SBLR_DDL_PURGE_SYSTEM_HISTORY" && member.opcode_code == 1628;
    ddl_set_index_optimizer_eligibility_root = member.operation_id == "engine.op.ddl_set_index_optimizer_eligibility" && member.opcode == "SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY" && member.opcode_code == 1629;
    ddl_set_table_type_enforcement_root = member.operation_id == "engine.op.ddl_set_table_type_enforcement" && member.opcode == "SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT" && member.opcode_code == 1630;
    database_serialize_logical_snapshot_root = member.operation_id == "engine.op.database_serialize_logical_snapshot" && member.opcode == "SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT" && member.opcode_code == 1631;
    database_deserialize_logical_snapshot_root = member.operation_id == "engine.op.database_deserialize_logical_snapshot" && member.opcode == "SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT" && member.opcode_code == 1632;
    ddl_create_schema_root = member.operation_id == "engine.op.ddl_create_schema" && member.opcode == "SBLR_DDL_CREATE_SCHEMA" && member.opcode_code == 1536;
    ddl_create_table_root = member.operation_id == "engine.op.ddl_create_table" && member.opcode == "SBLR_DDL_CREATE_TABLE" && member.opcode_code == 1537;
    ddl_create_index_root = member.operation_id == "engine.op.ddl_create_index" && member.opcode == "SBLR_DDL_CREATE_INDEX" && member.opcode_code == 1540;
    ddl_drop_index_root = member.operation_id == "engine.op.ddl_drop_index" && member.opcode == "SBLR_DDL_DROP_INDEX" && member.opcode_code == 1541;
    {
      const auto* member_entry = scratchbird::engine::sblr::LookupSblrOpcodeCode(member.opcode_code);
      if (member.opcode_code == 1625 && member.operation_id == "engine.op.ddl_create_aggregate") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_AGGREGATE");
      if (member.opcode_code == 1633 && member.operation_id == "engine.op.ddl_create_macro") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_MACRO");
      if (member.opcode_code == 1634 && member.operation_id == "engine.op.ddl_drop_macro") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_MACRO");
      if (member.opcode_code == 1635 && member.operation_id == "engine.op.admin_register_external_relation_resolver") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER");
      if (member.opcode_code == 1636 && member.operation_id == "engine.op.admin_unregister_external_relation_resolver") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER");
      if (member.opcode_code == 1637 && member.operation_id == "engine.op.ddl_create_dictionary") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_DICTIONARY");
      if (member.opcode_code == 1626 && member.operation_id == "engine.op.ddl_alter_aggregate") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_ALTER_AGGREGATE");
      if (member.opcode_code == 1627 && member.operation_id == "engine.op.ddl_drop_aggregate") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_AGGREGATE");
      if (member.opcode_code == 1638 && member.operation_id == "engine.op.ddl_drop_dictionary") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_DICTIONARY");
      if (member.opcode_code == 1639 && member.operation_id == "engine.op.ddl_alter_dictionary") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_ALTER_DICTIONARY");
      if (member.opcode_code == 1640 && member.operation_id == "engine.op.ddl_create_continuous_view") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_CONTINUOUS_VIEW");
      if (member.opcode_code == 1641 && member.operation_id == "engine.op.ddl_alter_continuous_view") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_ALTER_CONTINUOUS_VIEW");
      if (member.opcode_code == 1628 && member.operation_id == "engine.op.ddl_purge_system_history") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_PURGE_SYSTEM_HISTORY");
      if (member.opcode_code == 1629 && member.operation_id == "engine.op.ddl_set_index_optimizer_eligibility") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY");
      if (member.opcode_code == 1630 && member.operation_id == "engine.op.ddl_set_table_type_enforcement") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT");
      if (member.opcode_code == 1631 && member.operation_id == "engine.op.database_serialize_logical_snapshot") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT");
      if (member.opcode_code == 1632 && member.operation_id == "engine.op.database_deserialize_logical_snapshot") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT");
      if (member.opcode_code == 1554 && member.operation_id == "engine.op.ddl_create_procedure") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_PROCEDURE");
      if (member.opcode_code == 1555 && member.operation_id == "engine.op.ddl_alter_procedure") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_ALTER_PROCEDURE");
      if (member.opcode_code == 1556 && member.operation_id == "engine.op.ddl_drop_procedure") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_PROCEDURE");
      if (member.opcode_code == 1557 && member.operation_id == "engine.op.ddl_create_function") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_FUNCTION");
      if (member.opcode_code == 1558 && member.operation_id == "engine.op.ddl_alter_function") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_ALTER_FUNCTION");
      if (member.opcode_code == 1559 && member.operation_id == "engine.op.ddl_drop_function") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_FUNCTION");
      if (member.opcode_code == 1560 && member.operation_id == "engine.op.ddl_create_package") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_PACKAGE");
      if (member.opcode_code == 1615 && member.operation_id == "engine.op.ddl_create_or_replace_srs") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_OR_REPLACE_SRS");
      if (member.opcode_code == 1616 && member.operation_id == "engine.op.ddl_drop_srs") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_SRS");
      if (member.opcode_code == 1617 && member.operation_id == "engine.op.ddl_create_rewrite_rule") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_CREATE_REWRITE_RULE");
      if (member.opcode_code == 1618 && member.operation_id == "engine.op.ddl_alter_rewrite_rule") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_ALTER_REWRITE_RULE");
      if (member.opcode_code == 1619 && member.operation_id == "engine.op.ddl_drop_rewrite_rule") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_DDL_DROP_REWRITE_RULE");
      if (member.opcode_code == 4864 && member.operation_id == "engine.op.catalog_introspect") member_entry = scratchbird::engine::sblr::LookupSblrOpcode("SBLR_CATALOG_INTROSPECT");
      if (member_entry != nullptr) {
        member.requires_security_context =
            member_entry->requires_security_context;
        member.requires_transaction_context =
            member_entry->requires_transaction_context;
        member.requires_cluster_authority =
            member_entry->requires_cluster_authority;
      }
      scratchbird::engine::internal_api::EngineApiRequest preflight_api_request;
      const auto member_preflight =
          scratchbird::engine::sblr::PreflightSblrQueryOperation(
              {context, member, std::move(preflight_api_request),
               admitted_parameter_values});
      if (member_entry == nullptr || !member_preflight.ok) {
        return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4062,
                           member_preflight.diagnostic_id.empty()
                               ? "SBLR.OPERATION_UNSUPPORTED"
                               : member_preflight.diagnostic_id,
                           "sblr.opcode_stream.member_preflight_refused",
                           member_preflight.detail.empty() ? member_preflight.diagnostic_id : member_preflight.detail);
      }
      // Preflight materializes a private semantic view (typed operand values
      // and UUID property names) for validation.  The dispatcher must still
      // receive the original canonical envelope: DispatchSblrOperation owns
      // the canonical-envelope validation barrier and performs its own
      // materialization only after that barrier.  Replacing `member` with the
      // preflight view would both double-materialize it and make the strict
      // canonical validator reject an otherwise admitted package root.
      if ((member.requires_security_context &&
           !context.security_context_present) ||
          (member.requires_transaction_context &&
           context.local_transaction_id == 0 &&
           context.transaction_uuid.canonical.empty()) ||
          (member.requires_cluster_authority &&
           !context.cluster_authority_available)) {
        return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 4062,
                           "SECURITY.ACCESS_DENIED",
                           "sblr.opcode_stream.member_context_refused");
      }
    }
    if (txn_commit_root) {
      std::string detail;
      if (member.operands.size() != 1 ||
          !scratchbird::engine::sblr::DecodeSblrTransactionCommitOptionsV1(
              member.operands.front().value_body.data(),
              member.operands.front().value_body.size(), &commit_options,
              &detail)) {
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4062,
                           "SBLR.OPERAND_INVALID",
                           "sblr.txn_commit.options_invalid", detail);
      }
      auto* owning_session = receipt->session;
      if (!valid_session(owning_session))
        return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4062,
                           "MGA.TRANSACTION.STALE",
                           "sblr.txn_commit.session_stale");
      {
        std::lock_guard<std::mutex> session_guard(owning_session->mutex);
        const auto found = std::find_if(
            owning_session->published_transactions.begin(),
            owning_session->published_transactions.end(), [&](auto* tx) {
              return tx != nullptr && !tx->closed &&
                  tx->transaction_uuid == commit_options.transaction_uuid &&
                  tx->local_transaction_id ==
                      commit_options.local_transaction_id &&
                  tx->handle_evidence_sha256 ==
                      commit_options.admitted_handle_evidence_sha256;
            });
        if (found != owning_session->published_transactions.end())
          commit_private_handle = *found;
      }
      if (commit_private_handle == nullptr)
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4062,
                           "MGA.TRANSACTION.STALE",
                           "sblr.txn_commit.handle_stale_or_replayed");
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;
      id.executor_id = scratchbird::engine::internal_api::kSblrTxnCommitExecutorId;
      id.opcode_code = scratchbird::engine::internal_api::kSblrTxnCommitOpcodeCode;
      id.opcode_version = scratchbird::engine::internal_api::kSblrTxnCommitOpcodeVersion;
      id.operand_descriptor_id = scratchbird::engine::internal_api::kSblrTxnCommitOperandDescriptorId;
      id.result_descriptor_id = scratchbird::engine::internal_api::kSblrTxnCommitResultDescriptorId;
      id.result_descriptor_version = scratchbird::engine::internal_api::kSblrTxnCommitResultDescriptorVersion;
      const auto availability = scratchbird::engine::internal_api::
          LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context, id);
      if (!availability.ok || !availability.snapshot.installed ||
          availability.snapshot.generation !=
              view.txn_commit_executor_availability_generation)
        return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4062,
                           "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                           "sblr.txn_commit.executor_unavailable");
    }
    if (txn_rollback_root) {
      std::string detail;
      if (member.operands.size() != 1 ||
          !scratchbird::engine::sblr::DecodeSblrTransactionRollbackOptionsV1(
              member.operands.front().value_body.data(),
              member.operands.front().value_body.size(), &rollback_options,
              &detail)) {
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4062,
                           "SBLR.OPERAND_INVALID",
                           "sblr.txn_rollback.options_invalid", detail);
      }
      auto* owning_session = receipt->session;
      if (!valid_session(owning_session))
        return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4062,
                           "MGA.TRANSACTION.STALE",
                           "sblr.txn_rollback.session_stale");
      {
        std::lock_guard<std::mutex> session_guard(owning_session->mutex);
        const auto found = std::find_if(
            owning_session->published_transactions.begin(),
            owning_session->published_transactions.end(), [&](auto* tx) {
              return tx != nullptr && !tx->closed &&
                  tx->transaction_uuid == rollback_options.transaction_uuid &&
                  tx->local_transaction_id ==
                      rollback_options.local_transaction_id &&
                  tx->handle_evidence_sha256 ==
                      rollback_options.admitted_handle_evidence_sha256;
            });
        if (found != owning_session->published_transactions.end())
          rollback_private_handle = *found;
      }
      if (rollback_private_handle == nullptr)
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4062,
                           "MGA.TRANSACTION.STALE",
                           "sblr.txn_rollback.handle_stale_or_replayed");
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;
      id.executor_id =
          scratchbird::engine::internal_api::kSblrTxnRollbackExecutorId;
      id.opcode_code =
          scratchbird::engine::internal_api::kSblrTxnRollbackOpcodeCode;
      id.opcode_version =
          scratchbird::engine::internal_api::kSblrTxnRollbackOpcodeVersion;
      id.operand_descriptor_id = scratchbird::engine::internal_api::
          kSblrTxnRollbackOperandDescriptorId;
      id.result_descriptor_id = scratchbird::engine::internal_api::
          kSblrTxnRollbackResultDescriptorId;
      id.result_descriptor_version = scratchbird::engine::internal_api::
          kSblrTxnRollbackResultDescriptorVersion;
      const auto availability = scratchbird::engine::internal_api::
          LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context, id);
      if (!availability.ok || !availability.snapshot.installed ||
          availability.snapshot.generation !=
              view.txn_rollback_executor_availability_generation) {
        return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4062,
                           "SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
                           "sblr.txn_rollback.executor_unavailable");
      }
    }
    if (txn_savepoint_root) {
      std::string detail;
      if (member.operands.size()!=1 || member.operands.front().type!="savepoint.descriptor" ||
          member.operands.front().name!="savepoint" ||
          !scratchbird::engine::sblr::DecodeSblrSavepointDescriptorV1(
              member.operands.front().value_body.data(),member.operands.front().value_body.size(),
              &savepoint_descriptor,&detail))
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,
                           "SBLR.OPERAND_INVALID","sblr.txn_savepoint.descriptor_invalid",detail);
      const auto uuid_text=[](const std::array<std::uint8_t,16>&bytes){
        scratchbird::core::platform::Uuid u{};std::copy(bytes.begin(),bytes.end(),u.bytes.begin());
        return scratchbird::core::uuid::UuidToString(u);};
      const auto sha_text=[](const std::array<std::uint8_t,32>&bytes){return std::string("sha256:")+scratchbird::core::hash::HexLower(bytes);};
      auto savepoint_context=receipt->engine_context;
      savepoint_context.statement_metadata_snapshot_engine_owned=true;
      savepoint_context.trace_tags.push_back("private_savepoint_coordination");
      const auto found=scratchbird::engine::internal_api::LookupSblrSavepoint(
          savepoint_context,receipt->view.receipt_uuid,uuid_text(savepoint_descriptor.descriptor_uuid),
          savepoint_descriptor.descriptor_generation,sha_text(savepoint_descriptor.descriptor_evidence_sha256));
      if(!found.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4062,
          found.diagnostic.code,found.diagnostic.message_key);
      savepoint_snapshot=found.snapshot;
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;
      id.executor_id=scratchbird::engine::internal_api::kSblrTxnSavepointExecutorId;
      id.opcode_code=scratchbird::engine::internal_api::kSblrTxnSavepointOpcodeCode;
      id.opcode_version=scratchbird::engine::internal_api::kSblrTxnSavepointOpcodeVersion;
      id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTxnSavepointOperandDescriptorId;
      id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTxnSavepointResultDescriptorId;
      id.result_descriptor_version=scratchbird::engine::internal_api::kSblrTxnSavepointResultDescriptorVersion;
      const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(savepoint_context,id);
      if(!availability.ok||!availability.snapshot.installed)return fail_result(
          SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING",
          "sblr.txn_savepoint.executor_unavailable");
      savepoint_availability_generation=availability.snapshot.generation;
    }
    if (txn_release_savepoint_root) {
      std::string detail;
      if(member.operands.size()!=1||member.operands.front().type!="savepoint.release_handle"||member.operands.front().name!="savepoint"||!scratchbird::engine::sblr::DecodeSblrSavepointReleaseOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&savepoint_release_operand,&detail))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.txn_release_savepoint.operand_invalid",detail);
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;
      id.executor_id=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointExecutorId;id.opcode_code=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointOpcodeCode;id.opcode_version=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointOpcodeVersion;id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointResultDescriptorId;id.result_descriptor_version=scratchbird::engine::internal_api::kSblrTxnReleaseSavepointResultDescriptorVersion;
      const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);
      if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=savepoint_release_operand.executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.txn_release_savepoint.executor_unavailable");
      savepoint_release_availability_generation=availability.snapshot.generation;
    }
    if(txn_rollback_to_savepoint_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="savepoint.rollback_handle"||member.operands.front().name!="savepoint"||!scratchbird::engine::sblr::DecodeSblrSavepointRollbackOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&savepoint_rollback_operand,&detail))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.txn_rollback_to_savepoint.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrTxnRollbackToSavepointExecutorId;id.opcode_code=261;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTxnRollbackToSavepointOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTxnRollbackToSavepointResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=savepoint_rollback_operand.executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.txn_rollback_to_savepoint.executor_unavailable");savepoint_rollback_availability_generation=availability.snapshot.generation;}
    if(psql_autonomous_frame_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="autonomous_frame_descriptor"||member.operands.front().name!="autonomous_frame"||!scratchbird::engine::sblr::DecodeSblrAutonomousFrameDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&autonomous_frame_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.psql_autonomous.descriptor_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrPsqlAutonomousFrameExecutorId;id.opcode_code=262;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrPsqlAutonomousFrameOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrPsqlAutonomousFrameResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=view.psql_autonomous_frame_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.psql_autonomous.executor_unavailable");autonomous_frame_availability_generation=availability.snapshot.generation;}
    if(reservation_release_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="relation_reservation_release_descriptor"||member.operands.front().name!="reservation"||!scratchbird::engine::sblr::DecodeSblrReservationReleaseDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&reservation_release_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.reservation_release.descriptor_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrReservationReleaseExecutorId;id.opcode_code=263;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrReservationReleaseOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrReservationReleaseResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=reservation_release_descriptor.availability_generation||availability.snapshot.generation!=view.transaction_reservation_release_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.reservation_release.executor_unavailable");reservation_release_availability_generation=availability.snapshot.generation;}
    if(temporary_cleanup_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="temporary_instance_cleanup_descriptor"||member.operands.front().name!="temporary_instance"||!scratchbird::engine::sblr::DecodeSblrTemporaryInstanceCleanupDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&temporary_cleanup_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.temporary_instance_cleanup.descriptor_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrTemporaryInstanceCleanupExecutorId;id.opcode_code=264;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrTemporaryInstanceCleanupOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrTemporaryInstanceCleanupResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=temporary_cleanup_descriptor.availability_generation||availability.snapshot.generation!=view.temporary_instance_cleanup_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.temporary_instance_cleanup.executor_unavailable");temporary_cleanup_availability_generation=availability.snapshot.generation;}
    if(cursor_open_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="cursor_open_plan_ref"||member.operands.front().name!="plan"||!scratchbird::engine::sblr::DecodeSblrCursorOpenDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&cursor_open_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.cursor_open.descriptor_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrCursorOpenExecutorId;id.opcode_code=512;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrCursorOpenOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrCursorOpenResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=cursor_open_descriptor.availability_generation||availability.snapshot.generation!=view.cursor_open_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cursor_open.executor_unavailable");cursor_open_availability_generation=availability.snapshot.generation;}
    if(cursor_fetch_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="cursor_fetch_handle"||member.operands.front().name!="cursor"||!scratchbird::engine::sblr::DecodeSblrCursorFetchOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&cursor_fetch_operand,&detail))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.cursor_fetch.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrCursorFetchExecutorId;id.opcode_code=513;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrCursorFetchOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrCursorFetchResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=cursor_fetch_operand.availability_generation||availability.snapshot.generation!=view.cursor_fetch_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cursor_fetch.executor_unavailable");cursor_fetch_availability_generation=availability.snapshot.generation;}
    if(cursor_close_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="cursor_close_handle"||member.operands.front().name!="cursor"||!scratchbird::engine::sblr::DecodeSblrCursorCloseOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&cursor_close_operand,&detail))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.cursor_close.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrCursorCloseExecutorId;id.opcode_code=514;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrCursorCloseOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrCursorCloseResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=cursor_close_operand.availability_generation||availability.snapshot.generation!=view.cursor_close_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cursor_close.executor_unavailable");cursor_close_availability_generation=availability.snapshot.generation;}
    if(read_by_key_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="uuid_object_key_descriptor"||member.operands.front().name!="key"||!scratchbird::engine::sblr::DecodeSblrReadByKeyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&read_by_key_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.read_by_key.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id;id.executor_id=scratchbird::engine::internal_api::kSblrReadByKeyExecutorId;id.opcode_code=515;id.opcode_version="1.0";id.operand_descriptor_id=scratchbird::engine::internal_api::kSblrReadByKeyOperandDescriptorId;id.result_descriptor_id=scratchbird::engine::internal_api::kSblrReadByKeyResultDescriptorId;id.result_descriptor_version=1;const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=read_by_key_descriptor.availability_generation||availability.snapshot.generation!=view.read_by_key_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.read_by_key.executor_unavailable");read_by_key_availability_generation=availability.snapshot.generation;} if(read_range_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="range_scan_descriptor"||member.operands.front().name!="range"||!scratchbird::engine::sblr::DecodeSblrReadRangeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&read_range_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.read_range.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrReadRangeExecutorId,516,"1.0",scratchbird::engine::internal_api::kSblrReadRangeOperandDescriptorId,scratchbird::engine::internal_api::kSblrReadRangeResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=read_range_descriptor.availability_generation||availability.snapshot.generation!=view.read_range_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.read_range.executor_unavailable");read_range_availability_generation=availability.snapshot.generation;} if(read_stream_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="stream_descriptor"||member.operands.front().name!="stream"||!scratchbird::engine::sblr::DecodeSblrReadStreamDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&read_stream_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.read_stream.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrReadStreamExecutorId,517,"1.0",scratchbird::engine::internal_api::kSblrReadStreamOperandDescriptorId,scratchbird::engine::internal_api::kSblrReadStreamResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=read_stream_descriptor.availability_generation||availability.snapshot.generation!=view.read_stream_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.read_stream.executor_unavailable");read_stream_availability_generation=availability.snapshot.generation;}
    if(result_set_pass_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="result_set_handle_and_lifetime"||member.operands.front().name!="result_set"||!scratchbird::engine::sblr::DecodeSblrResultSetPassDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&result_set_pass_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.result_set_pass.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrResultSetPassExecutorId,518,"1.0",scratchbird::engine::internal_api::kSblrResultSetPassOperandDescriptorId,scratchbird::engine::internal_api::kSblrResultSetPassResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=result_set_pass_descriptor.availability_generation||availability.snapshot.generation!=view.result_set_pass_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.result_set_pass.executor_unavailable");result_set_pass_availability_generation=availability.snapshot.generation;}
    if(access_cursor_open_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="access_cursor_open_descriptor"||member.operands.front().name!="access_cursor"||!scratchbird::engine::sblr::DecodeSblrAccessCursorOpenDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&access_cursor_open_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.access_cursor_open.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAccessCursorOpenExecutorId,519,"1.0",scratchbird::engine::internal_api::kSblrAccessCursorOpenOperandDescriptorId,scratchbird::engine::internal_api::kSblrAccessCursorOpenResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=access_cursor_open_descriptor.availability_generation||availability.snapshot.generation!=view.access_cursor_open_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.access_cursor_open.executor_unavailable");access_cursor_open_availability_generation=availability.snapshot.generation;}
    if(access_cursor_fetch_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="access_cursor_fetch_descriptor"||member.operands.front().name!="access_cursor"||!scratchbird::engine::sblr::DecodeSblrAccessCursorFetchDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&access_cursor_fetch_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.access_cursor_fetch.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAccessCursorFetchExecutorId,520,"1.0",scratchbird::engine::internal_api::kSblrAccessCursorFetchOperandDescriptorId,scratchbird::engine::internal_api::kSblrAccessCursorFetchResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=access_cursor_fetch_descriptor.availability_generation||availability.snapshot.generation!=view.access_cursor_fetch_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.access_cursor_fetch.executor_unavailable");access_cursor_fetch_availability_generation=availability.snapshot.generation;}
    if(access_cursor_close_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="access_cursor_close_descriptor"||member.operands.front().name!="access_cursor"||!scratchbird::engine::sblr::DecodeSblrAccessCursorCloseDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&access_cursor_close_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.access_cursor_close.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAccessCursorCloseExecutorId,521,"1.0",scratchbird::engine::internal_api::kSblrAccessCursorCloseOperandDescriptorId,scratchbird::engine::internal_api::kSblrAccessCursorCloseResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=access_cursor_close_descriptor.availability_generation||availability.snapshot.generation!=view.access_cursor_close_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.access_cursor_close.executor_unavailable");access_cursor_close_availability_generation=availability.snapshot.generation;}
    if(insert_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="insert_descriptor"||member.operands.front().name!="insert"||!scratchbird::engine::sblr::DecodeSblrInsertDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&insert_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.insert.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrInsertExecutorId,768,"1.0",scratchbird::engine::internal_api::kSblrInsertOperandDescriptorId,scratchbird::engine::internal_api::kSblrInsertResultDescriptorId,1};const auto availability=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!availability.ok||!availability.snapshot.installed||availability.snapshot.generation!=insert_descriptor.availability_generation||availability.snapshot.generation!=view.insert_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.insert.executor_unavailable");insert_availability_generation=availability.snapshot.generation;}
    if(update_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="update_descriptor"||member.operands.front().name!="update"||!scratchbird::engine::sblr::DecodeSblrUpdateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&update_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.update.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrUpdateExecutorId,769,"1.0",scratchbird::engine::internal_api::kSblrUpdateOperandDescriptorId,scratchbird::engine::internal_api::kSblrUpdateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=update_descriptor.availability_generation||a.snapshot.generation!=view.update_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.update.executor_unavailable");update_availability_generation=a.snapshot.generation;}
    if(delete_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="delete_descriptor"||member.operands.front().name!="delete"||!scratchbird::engine::sblr::DecodeSblrDeleteDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&delete_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.delete.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDeleteExecutorId,770,"1.0",scratchbird::engine::internal_api::kSblrDeleteOperandDescriptorId,scratchbird::engine::internal_api::kSblrDeleteResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=delete_descriptor.availability_generation||a.snapshot.generation!=view.delete_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.delete.executor_unavailable");delete_availability_generation=a.snapshot.generation;}
    if(merge_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="merge_descriptor"||member.operands.front().name!="merge"||!scratchbird::engine::sblr::DecodeSblrMergeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&merge_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.merge.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrMergeExecutorId,771,"1.0",scratchbird::engine::internal_api::kSblrMergeOperandDescriptorId,scratchbird::engine::internal_api::kSblrMergeResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=merge_descriptor.availability_generation||a.snapshot.generation!=view.merge_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.merge.executor_unavailable");merge_availability_generation=a.snapshot.generation;}
    if(table_truncate_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="truncate_table_descriptor"||member.operands.front().name!="truncate"||!scratchbird::engine::sblr::DecodeSblrTableTruncateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&table_truncate_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.table_truncate.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrTableTruncateExecutorId,773,"1.0",scratchbird::engine::internal_api::kSblrTableTruncateOperandDescriptorId,scratchbird::engine::internal_api::kSblrTableTruncateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=table_truncate_descriptor.availability_generation||a.snapshot.generation!=view.table_truncate_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.table_truncate.executor_unavailable");table_truncate_availability_generation=a.snapshot.generation;}
    if(table_analyze_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="analyze_table_descriptor"||member.operands.front().name!="analyze"||!scratchbird::engine::sblr::DecodeSblrTableAnalyzeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&table_analyze_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.table_analyze.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrTableAnalyzeExecutorId,774,"1.0",scratchbird::engine::internal_api::kSblrTableAnalyzeOperandDescriptorId,scratchbird::engine::internal_api::kSblrTableAnalyzeResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=table_analyze_descriptor.availability_generation||a.snapshot.generation!=view.table_analyze_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.table_analyze.executor_unavailable");table_analyze_availability_generation=a.snapshot.generation;}
    if(bulk_import_stream_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="bulk_import_stream_descriptor"||member.operands.front().name!="bulk_import"||!scratchbird::engine::sblr::DecodeSblrBulkImportStreamDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&bulk_import_stream_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.bulk_import_stream.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrBulkImportStreamExecutorId,775,"1.0",scratchbird::engine::internal_api::kSblrBulkImportStreamOperandDescriptorId,scratchbird::engine::internal_api::kSblrBulkImportStreamResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=bulk_import_stream_descriptor.availability_generation||a.snapshot.generation!=view.bulk_import_stream_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.bulk_import_stream.executor_unavailable");bulk_import_stream_availability_generation=a.snapshot.generation;}
    if(bulk_export_stream_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="bulk_export_stream_descriptor"||member.operands.front().name!="bulk_export"||!scratchbird::engine::sblr::DecodeSblrBulkExportStreamDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&bulk_export_stream_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.bulk_export_stream.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrBulkExportStreamExecutorId,776,"1.0",scratchbird::engine::internal_api::kSblrBulkExportStreamOperandDescriptorId,scratchbird::engine::internal_api::kSblrBulkExportStreamResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=bulk_export_stream_descriptor.availability_generation||a.snapshot.generation!=view.bulk_export_stream_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.bulk_export_stream.executor_unavailable");bulk_export_stream_availability_generation=a.snapshot.generation;}
    if(statement_batch_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="statement_batch_descriptor"||member.operands.front().name!="batch"||!scratchbird::engine::sblr::DecodeSblrStatementBatchDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&statement_batch_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.statement_batch.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrStatementBatchExecutorId,777,"1.0",scratchbird::engine::internal_api::kSblrStatementBatchOperandDescriptorId,scratchbird::engine::internal_api::kSblrStatementBatchResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=statement_batch_descriptor.availability_generation||a.snapshot.generation!=view.statement_batch_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.statement_batch.executor_unavailable");statement_batch_availability_generation=a.snapshot.generation;}
    if(atomic_cas_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="atomic_cas_descriptor"||member.operands.front().name!="cas"||!scratchbird::engine::sblr::DecodeSblrAtomicCasDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&atomic_cas_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4062,"SBLR.OPERAND_INVALID","sblr.atomic_cas.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAtomicCasExecutorId,778,"1.0",scratchbird::engine::internal_api::kSblrAtomicCasOperandDescriptorId,scratchbird::engine::internal_api::kSblrAtomicCasResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=atomic_cas_descriptor.availability_generation||a.snapshot.generation!=view.atomic_cas_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4062,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.atomic_cas.executor_unavailable");atomic_cas_availability_generation=a.snapshot.generation;}
    if(atomic_rmw_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="atomic_rmw_descriptor"||member.operands.front().name!="rmw"||!scratchbird::engine::sblr::DecodeSblrAtomicRmwDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&atomic_rmw_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4066,"SBLR.OPERAND_INVALID","sblr.atomic_rmw.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAtomicRmwExecutorId,779,"1.0",scratchbird::engine::internal_api::kSblrAtomicRmwOperandDescriptorId,scratchbird::engine::internal_api::kSblrAtomicRmwResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=atomic_rmw_descriptor.availability_generation||a.snapshot.generation!=view.atomic_rmw_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4066,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.atomic_rmw.executor_unavailable");atomic_rmw_availability_generation=a.snapshot.generation;}
    if(advisory_lock_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="advisory_lock_descriptor"||member.operands.front().name!="lock"||!scratchbird::engine::sblr::DecodeSblrAdvisoryLockDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&advisory_lock_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4068,"SBLR.OPERAND_INVALID","sblr.advisory_lock.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAdvisoryLockAcquireExecutorId,780,"1.0",scratchbird::engine::internal_api::kSblrAdvisoryLockAcquireOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdvisoryLockResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=advisory_lock_descriptor.availability_generation||a.snapshot.generation!=view.advisory_lock_acquire_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4068,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.advisory_lock.executor_unavailable");advisory_lock_availability_generation=a.snapshot.generation;}
    if(advisory_lock_release_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="advisory_lock_release_descriptor"||member.operands.front().name!="release"||!scratchbird::engine::sblr::DecodeSblrAdvisoryLockReleaseDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&advisory_lock_release_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4070,"SBLR.OPERAND_INVALID","sblr.advisory_lock_release.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAdvisoryLockReleaseExecutorId,781,"1.0",scratchbird::engine::internal_api::kSblrAdvisoryLockReleaseOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdvisoryLockResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=advisory_lock_release_descriptor.availability_generation||a.snapshot.generation!=view.advisory_lock_release_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4070,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.advisory_lock_release.executor_unavailable");advisory_lock_release_availability_generation=a.snapshot.generation;}
    if(function_call_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="function_call_descriptor"||member.operands.front().name!="call"||!scratchbird::engine::sblr::DecodeSblrFunctionCallDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&function_call_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4072,"SBLR.OPERAND_INVALID","sblr.function_call.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrFunctionCallExecutorId,1024,"1.0",scratchbird::engine::internal_api::kSblrFunctionCallOperandDescriptorId,scratchbird::engine::internal_api::kSblrFunctionCallResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=function_call_descriptor.availability_generation||a.snapshot.generation!=view.function_call_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4072,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.function_call.executor_unavailable");function_call_availability_generation=a.snapshot.generation;}
    if(operator_call_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="operator_call_descriptor"||member.operands.front().name!="call"||!scratchbird::engine::sblr::DecodeSblrOperatorCallDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operator_call_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4074,"SBLR.OPERAND_INVALID","sblr.operator_call.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrOperatorCallExecutorId,1025,"1.0",scratchbird::engine::internal_api::kSblrOperatorCallOperandDescriptorId,scratchbird::engine::internal_api::kSblrOperatorCallResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=operator_call_descriptor.availability_generation||a.snapshot.generation!=view.operator_call_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4074,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.operator_call.executor_unavailable");operator_call_availability_generation=a.snapshot.generation;}
    if(cast_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="cast_descriptor"||member.operands.front().name!="cast"||!scratchbird::engine::sblr::DecodeSblrCastDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&cast_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4076,"SBLR.OPERAND_INVALID","sblr.cast.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrCastExecutorId,1026,"1.0",scratchbird::engine::internal_api::kSblrCastOperandDescriptorId,scratchbird::engine::internal_api::kSblrCastResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=cast_descriptor.availability_generation||a.snapshot.generation!=view.cast_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4076,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.cast.executor_unavailable");cast_availability_generation=a.snapshot.generation;}
    if(compare_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="comparison_descriptor"||member.operands.front().name!="compare"||!scratchbird::engine::sblr::DecodeSblrCompareDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&compare_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4078,"SBLR.OPERAND_INVALID","sblr.compare.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrCompareExecutorId,1027,"1.0",scratchbird::engine::internal_api::kSblrCompareOperandDescriptorId,scratchbird::engine::internal_api::kSblrCompareResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=compare_descriptor.availability_generation||a.snapshot.generation!=view.compare_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4078,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.compare.executor_unavailable");compare_availability_generation=a.snapshot.generation;}
    if(domain_operation_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="domain_operation_descriptor"||member.operands.front().name!="domain_operation"||!scratchbird::engine::sblr::DecodeSblrDomainOperationDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&domain_operation_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4080,"SBLR.OPERAND_INVALID","sblr.domain_operation.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDomainOperationExecutorId,1028,"1.0",scratchbird::engine::internal_api::kSblrDomainOperationOperandDescriptorId,scratchbird::engine::internal_api::kSblrDomainOperationResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=domain_operation_descriptor.availability_generation||a.snapshot.generation!=view.domain_operation_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4080,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.domain_operation.executor_unavailable");domain_operation_availability_generation=a.snapshot.generation;}
    if(udr_invoke_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="registered_cpp_udr_invocation"||member.operands.front().name!="udr"||!scratchbird::engine::sblr::DecodeSblrUdrInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&udr_invoke_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4082,"SBLR.OPERAND_INVALID","sblr.udr_invoke.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrUdrInvokeExecutorId,1029,"1.0",scratchbird::engine::internal_api::kSblrUdrInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrUdrInvokeResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=udr_invoke_descriptor.availability||a.snapshot.generation!=view.udr_invoke_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4082,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.udr_invoke.executor_unavailable");udr_invoke_availability_generation=a.snapshot.generation;}
    if(procedure_invoke_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="procedure_invoke_descriptor"||member.operands.front().name!="procedure"||!scratchbird::engine::sblr::DecodeSblrProcedureInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&procedure_invoke_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4084,"SBLR.OPERAND_INVALID","sblr.procedure_invoke.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrProcedureInvokeExecutorId,1030,"1.0",scratchbird::engine::internal_api::kSblrProcedureInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrProcedureInvokeResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=procedure_invoke_descriptor.availability||a.snapshot.generation!=view.procedure_invoke_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4084,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.procedure_invoke.executor_unavailable");procedure_invoke_availability_generation=a.snapshot.generation;}
    if(function_invoke_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="function_invoke_descriptor"||member.operands.front().name!="function"||!scratchbird::engine::sblr::DecodeSblrFunctionInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&function_invoke_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4086,"SBLR.OPERAND_INVALID","sblr.function_invoke.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrFunctionInvokeExecutorId,1031,"1.0",scratchbird::engine::internal_api::kSblrFunctionInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrFunctionInvokeResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=function_invoke_descriptor.availability||a.snapshot.generation!=view.function_invoke_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4086,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.function_invoke.executor_unavailable");function_invoke_availability_generation=a.snapshot.generation;}
    if(aggregate_invoke_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="aggregate_invoke_descriptor"||member.operands.front().name!="aggregate"||!scratchbird::engine::sblr::DecodeSblrAggregateInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&aggregate_invoke_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4088,"SBLR.OPERAND_INVALID","sblr.aggregate_invoke.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAggregateInvokeExecutorId,1032,"1.0",scratchbird::engine::internal_api::kSblrAggregateInvokeOperandDescriptorId,scratchbird::engine::internal_api::kSblrAggregateInvokeResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=aggregate_invoke_descriptor.availability||a.snapshot.generation!=view.aggregate_invoke_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4088,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.aggregate_invoke.executor_unavailable");aggregate_invoke_availability_generation=a.snapshot.generation;}
    if(sequence_nextval_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="sequence_nextval_descriptor"||member.operands.front().name!="sequence"||!scratchbird::engine::sblr::DecodeSblrSequenceNextvalDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&sequence_nextval_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4090,"SBLR.OPERAND_INVALID","sblr.sequence_nextval.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSequenceNextvalExecutorId,1033,"1.0",scratchbird::engine::internal_api::kSblrSequenceNextvalOperandDescriptorId,scratchbird::engine::internal_api::kSblrSequenceNextvalResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=sequence_nextval_descriptor.availability||a.snapshot.generation!=view.sequence_nextval_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4090,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sequence_nextval.executor_unavailable");sequence_nextval_availability_generation=a.snapshot.generation;}
    if(sequence_currval_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="sequence_currval_descriptor"||member.operands.front().name!="sequence"||!scratchbird::engine::sblr::DecodeSblrSequenceCurrvalDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&sequence_currval_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4092,"SBLR.OPERAND_INVALID","sblr.sequence_currval.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSequenceCurrvalExecutorId,1034,"1.0",scratchbird::engine::internal_api::kSblrSequenceCurrvalOperandDescriptorId,scratchbird::engine::internal_api::kSblrSequenceCurrvalResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=sequence_currval_descriptor.availability||a.snapshot.generation!=view.sequence_currval_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4092,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sequence_currval.executor_unavailable");sequence_currval_availability_generation=a.snapshot.generation;}
    if(sequence_setval_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="sequence_setval_descriptor"||member.operands.front().name!="sequence"||!scratchbird::engine::sblr::DecodeSblrSequenceSetvalDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&sequence_setval_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4094,"SBLR.OPERAND_INVALID","sblr.sequence_setval.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSequenceSetvalExecutorId,1035,"1.0",scratchbird::engine::internal_api::kSblrSequenceSetvalOperandDescriptorId,scratchbird::engine::internal_api::kSblrSequenceSetvalResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=sequence_setval_descriptor.availability||a.snapshot.generation!=view.sequence_setval_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4094,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sequence_setval.executor_unavailable");sequence_setval_availability_generation=a.snapshot.generation;}
    if(query_numeric_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="numeric_descriptor_and_operand_values"||member.operands.front().name!="numeric_operation"||!scratchbird::engine::sblr::DecodeSblrQueryNumericDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&query_numeric_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4096,"SBLR.OPERAND_INVALID","sblr.query_numeric.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrQueryNumericExecutorId,1036,"1.0",scratchbird::engine::internal_api::kSblrQueryNumericOperandDescriptorId,scratchbird::engine::internal_api::kSblrQueryNumericResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=query_numeric_descriptor.availability||a.snapshot.generation!=view.query_numeric_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4096,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.query_numeric.executor_unavailable");query_numeric_availability_generation=a.snapshot.generation;}
    if(advanced_datatype_family_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="advanced_family_descriptor_operation_index_profile"||member.operands.front().name!="datatype_family"||!scratchbird::engine::sblr::DecodeSblrAdvancedDatatypeFamilyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&advanced_datatype_family_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4098,"SBLR.OPERAND_INVALID","sblr.advanced_datatype_family.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAdvancedDatatypeFamilyExecutorId,1037,"1.0",scratchbird::engine::internal_api::kSblrAdvancedDatatypeFamilyOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdvancedDatatypeFamilyResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=advanced_datatype_family_descriptor.availability||a.snapshot.generation!=view.advanced_datatype_family_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4098,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.advanced_datatype_family.executor_unavailable");advanced_datatype_family_availability_generation=a.snapshot.generation;}
    if(catalog_introspect_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="catalog_introspect_descriptor"||member.operands.front().name!="object_detail"||!scratchbird::engine::sblr::DecodeSblrCatalogIntrospectDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&catalog_introspect_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4100,"SBLR.OPERAND_INVALID","sblr.catalog_introspect.operand_invalid",detail);if(catalog_introspect_descriptor.availability!=1)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4100,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.catalog_introspect.executor_unavailable");catalog_introspect_availability_generation=1;}
    if(project_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="projection_descriptor"||member.operands.front().name!="projection"||!scratchbird::engine::sblr::DecodeSblrProjectDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&project_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4100,"SBLR.OPERAND_INVALID","sblr.project.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrProjectExecutorId,1280,"1.0",scratchbird::engine::internal_api::kSblrProjectOperandDescriptorId,scratchbird::engine::internal_api::kSblrProjectResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=project_descriptor.availability||a.snapshot.generation!=view.project_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4100,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.project.executor_unavailable");project_availability_generation=a.snapshot.generation;}
    if(limit_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="limit_descriptor"||member.operands.front().name!="limit"||!scratchbird::engine::sblr::DecodeSblrLimitDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&limit_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4109,"SBLR.OPERAND_INVALID","sblr.limit.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrLimitExecutorId,1284,"1.0",scratchbird::engine::internal_api::kSblrLimitOperandDescriptorId,scratchbird::engine::internal_api::kSblrLimitResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=limit_descriptor.availability||a.snapshot.generation!=view.limit_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4109,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.limit.executor_unavailable");limit_availability_generation=a.snapshot.generation;}
    if(aggregate_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="aggregate_descriptor"||member.operands.front().name!="aggregate"||!scratchbird::engine::sblr::DecodeSblrAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&aggregate_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4102,"SBLR.OPERAND_INVALID","sblr.aggregate.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAggregateExecutorId,1281,"1.0",scratchbird::engine::internal_api::kSblrAggregateOperandDescriptorId,scratchbird::engine::internal_api::kSblrAggregateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=aggregate_descriptor.availability||a.snapshot.generation!=view.aggregate_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4102,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.aggregate.executor_unavailable");aggregate_availability_generation=a.snapshot.generation;}
    if(group_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="group_descriptor"||member.operands.front().name!="group"||!scratchbird::engine::sblr::DecodeSblrGroupDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&group_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4104,"SBLR.OPERAND_INVALID","sblr.group.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrGroupExecutorId,1282,"1.0",scratchbird::engine::internal_api::kSblrGroupOperandDescriptorId,scratchbird::engine::internal_api::kSblrGroupResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=group_descriptor.availability||a.snapshot.generation!=view.group_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4104,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.group.executor_unavailable");group_availability_generation=a.snapshot.generation;}
    if(security_create_group_mapping_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="security_create_group_mapping_descriptor"||member.operands.front().name!="group_mapping"||!scratchbird::engine::sblr::DecodeSblrSecCreateGroupMappingDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_create_group_mapping_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4106,"SBLR.OPERAND_INVALID","sblr.sec_create_group_mapping.operand_invalid",detail);security_create_group_mapping_availability_generation=security_create_group_mapping_descriptor.availability;}
    if(security_drop_group_mapping_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_group_mapping_descriptor"||member.operands.front().name!="group_mapping"||!scratchbird::engine::sblr::DecodeSblrSecDropGroupMappingDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_drop_group_mapping_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4206,"SBLR.OPERAND_INVALID","sblr.sec_drop_group_mapping.operand_invalid",detail);security_drop_group_mapping_availability_generation=security_drop_group_mapping_descriptor.availability;}
    if(security_grant_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="grant_descriptor"||member.operands.front().name!="grant"||!scratchbird::engine::sblr::DecodeSblrSecGrantDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_grant_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4207,"SBLR.OPERAND_INVALID","sblr.sec_grant.operand_invalid",detail);security_grant_availability_generation=security_grant_descriptor.availability;}
    if(security_revoke_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="revoke_descriptor"||member.operands.front().name!="revoke"||!scratchbird::engine::sblr::DecodeSblrSecRevokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_revoke_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4208,"SBLR.OPERAND_INVALID","sblr.sec_revoke.operand_invalid",detail);security_revoke_availability_generation=security_revoke_descriptor.availability;}
    if(security_alter_policy_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="security_policy_descriptor"||member.operands.front().name!="policy"||!scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_alter_policy_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4210,"SBLR.OPERAND_INVALID","sblr.sec_alter_policy.operand_invalid",detail);security_alter_policy_availability_generation=security_alter_policy_descriptor.availability;}
    if(security_drop_user_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="user_descriptor"||member.operands.front().name!="user"||!scratchbird::engine::sblr::DecodeSblrSecDropUserDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_drop_user_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4211,"SBLR.OPERAND_INVALID","sblr.sec_drop_user.operand_invalid",detail);security_drop_user_availability_generation=security_drop_user_descriptor.availability;}
    if(window_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="window_descriptor"||member.operands.front().name!="window"||!scratchbird::engine::sblr::DecodeSblrWindowDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&window_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4110,"SBLR.OPERAND_INVALID","sblr.window.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrWindowExecutorId,1285,"1.0",scratchbird::engine::internal_api::kSblrWindowOperandDescriptorId,scratchbird::engine::internal_api::kSblrWindowResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=window_descriptor.availability||a.snapshot.generation!=view.window_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4110,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.window.executor_unavailable");window_availability_generation=a.snapshot.generation;}
    if(return_result_set_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="result_set_return_descriptor"||member.operands.front().name!="result_set"||!scratchbird::engine::sblr::DecodeSblrReturnResultSetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&return_result_set_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4112,"SBLR.OPERAND_INVALID","sblr.return_result_set.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrReturnResultSetExecutorId,1286,"1.0",scratchbird::engine::internal_api::kSblrReturnResultSetOperandDescriptorId,scratchbird::engine::internal_api::kSblrReturnResultSetResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=return_result_set_descriptor.availability||a.snapshot.generation!=view.return_result_set_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4112,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.return_result_set.executor_unavailable");return_result_set_availability_generation=a.snapshot.generation;}

  if(kv_structured_read_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="kv_structured_read_descriptor"||member.operands.front().name!="kv_read"||!scratchbird::engine::sblr::DecodeSblrKvStructuredReadDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&kv_structured_read_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4114,"SBLR.OPERAND_INVALID","sblr.kv_structured_read.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrKvStructuredReadExecutorId,8192,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredReadOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredReadResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=kv_structured_read_descriptor.availability||a.snapshot.generation!=view.kv_structured_read_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4114,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_read.executor_unavailable");kv_structured_read_availability_generation=a.snapshot.generation;}
    if(kv_structured_scan_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="kv_structured_scan_descriptor"||member.operands.front().name!="kv_scan"||!scratchbird::engine::sblr::DecodeSblrKvStructuredScanDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&kv_structured_scan_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4118,"SBLR.OPERAND_INVALID","sblr.kv_structured_scan.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrKvStructuredScanExecutorId,8194,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredScanOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredScanResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=kv_structured_scan_descriptor.availability||a.snapshot.generation!=view.kv_structured_scan_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4118,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_scan.executor_unavailable");kv_structured_scan_availability_generation=a.snapshot.generation;}
if(kv_structured_stream_read_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="kv_structured_stream_read_descriptor"||member.operands.front().name!="kv_stream_read"||!scratchbird::engine::sblr::DecodeSblrKvStructuredStreamReadDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&kv_structured_stream_read_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4120,"SBLR.OPERAND_INVALID","sblr.kv_structured_stream_read.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrKvStructuredStreamReadExecutorId,8195,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredStreamReadOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredStreamReadResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=kv_structured_stream_read_descriptor.availability||a.snapshot.generation!=view.kv_structured_stream_read_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4120,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_stream_read.executor_unavailable");kv_structured_stream_read_availability_generation=a.snapshot.generation;}
if(kv_structured_stream_append_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="kv_structured_stream_append_descriptor"||member.operands.front().name!="kv_stream_append"||!scratchbird::engine::sblr::DecodeSblrKvStructuredStreamAppendDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&kv_structured_stream_append_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4122,"SBLR.OPERAND_INVALID","sblr.kv_structured_stream_append.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrKvStructuredStreamAppendExecutorId,8196,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredStreamAppendOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredStreamAppendResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=kv_structured_stream_append_descriptor.availability||a.snapshot.generation!=view.kv_structured_stream_append_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4122,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_stream_append.executor_unavailable");kv_structured_stream_append_availability_generation=a.snapshot.generation;}
if(kv_structured_timeseries_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="kv_timeseries_descriptor"||member.operands.front().name!="system_config"||!scratchbird::engine::sblr::DecodeSblrKvStructuredTimeseriesDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&kv_timeseries_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4123,"SBLR.OPERAND_INVALID","sblr.kv_structured_timeseries.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrKvStructuredTimeseriesExecutorId,8197,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredTimeseriesOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredTimeseriesResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=kv_timeseries_descriptor.availability||a.snapshot.generation!=view.kv_structured_timeseries_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4123,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_timeseries.executor_unavailable");kv_structured_timeseries_availability_generation=a.snapshot.generation;}
if(system_config_set_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="system_config_set_descriptor"||member.operands.front().name!="system_config"||!scratchbird::engine::sblr::DecodeSblrSystemConfigSetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&system_config_set_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4124,"SBLR.OPERAND_INVALID","sblr.system_config_set.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSystemConfigSetExecutorId,5125,"1.0",scratchbird::engine::internal_api::kSblrSystemConfigSetOperandDescriptorId,scratchbird::engine::internal_api::kSblrSystemConfigSetResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=system_config_set_descriptor.availability||a.snapshot.generation!=view.system_config_set_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4124,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.system_config_set.executor_unavailable");system_config_set_availability_generation=a.snapshot.generation;}
if(ddl_create_domain_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_domain_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlCreateDomainDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_domain_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_create_domain.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateDomainExecutorId,1542,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateDomainOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateDomainResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_domain_descriptor.availability||a.snapshot.generation!=view.ddl_create_domain_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_domain.executor_unavailable");ddl_create_domain_availability_generation=a.snapshot.generation;}
if(ddl_alter_domain_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_domain_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlAlterDomainDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_domain_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_alter_domain.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterDomainExecutorId,1547,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterDomainOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterDomainResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_domain_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_domain.executor_unavailable");ddl_alter_domain_availability_generation=a.snapshot.generation;}if(ddl_create_view_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_view_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlCreateViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_view_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_create_view.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateViewExecutorId,1548,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateViewOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateViewResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_view_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_view.executor_unavailable");ddl_create_view_availability_generation=a.snapshot.generation;}
if(ddl_alter_view_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_view_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlAlterViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_view_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_alter_view.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterViewExecutorId,1549,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterViewOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterViewResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_view_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_view.executor_unavailable");ddl_alter_view_availability_generation=a.snapshot.generation;}
if(ddl_drop_view_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_view_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlDropViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_view_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_drop_view.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropViewExecutorId,1550,"1.0",scratchbird::engine::internal_api::kSblrDdlDropViewOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropViewResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_view_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_view.executor_unavailable");ddl_drop_view_availability_generation=a.snapshot.generation;}
if(ddl_create_trigger_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_trigger_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlCreateTriggerDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_trigger_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_create_trigger.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateTriggerExecutorId,1551,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateTriggerOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateTriggerResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_trigger_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_trigger.executor_unavailable");ddl_create_trigger_availability_generation=a.snapshot.generation;}
    if(ddl_alter_trigger_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_trigger_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlAlterTriggerDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_trigger_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4125,"SBLR.OPERAND_INVALID","sblr.ddl_alter_trigger.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterTriggerExecutorId,1552,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterTriggerOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterTriggerResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_trigger_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4125,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_trigger.executor_unavailable");ddl_alter_trigger_availability_generation=a.snapshot.generation;}
    if(ddl_drop_trigger_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_trigger_descriptor"||member.operands.front().name!="domain"||!scratchbird::engine::sblr::DecodeSblrDdlDropTriggerDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_trigger_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4126,"SBLR.OPERAND_INVALID","sblr.ddl_drop_trigger.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropTriggerExecutorId,1553,"1.0",scratchbird::engine::internal_api::kSblrDdlDropTriggerOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropTriggerResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_trigger_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4126,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_trigger.executor_unavailable");ddl_drop_trigger_availability_generation=a.snapshot.generation;}
    if(ddl_create_procedure_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_procedure_descriptor"||member.operands.front().name!="procedure"||!scratchbird::engine::sblr::DecodeSblrDdlCreateProcedureDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_procedure_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4127,"SBLR.OPERAND_INVALID","sblr.ddl_create_procedure.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateProcedureExecutorId,1554,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateProcedureOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateProcedureResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_procedure_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4127,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_procedure.executor_unavailable");ddl_create_procedure_availability_generation=a.snapshot.generation;}
    if(ddl_alter_procedure_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_procedure_descriptor"||member.operands.front().name!="procedure"||!scratchbird::engine::sblr::DecodeSblrDdlAlterProcedureDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_procedure_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4128,"SBLR.OPERAND_INVALID","sblr.ddl_alter_procedure.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterProcedureExecutorId,1555,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterProcedureOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterProcedureResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_procedure_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4128,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_procedure.executor_unavailable");ddl_alter_procedure_availability_generation=a.snapshot.generation;}
    if(ddl_drop_procedure_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_procedure_descriptor"||member.operands.front().name!="procedure"||!scratchbird::engine::sblr::DecodeSblrDdlDropProcedureDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_procedure_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4129,"SBLR.OPERAND_INVALID","sblr.ddl_drop_procedure.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropProcedureExecutorId,1556,"1.0",scratchbird::engine::internal_api::kSblrDdlDropProcedureOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropProcedureResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_procedure_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4129,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_procedure.executor_unavailable");ddl_drop_procedure_availability_generation=a.snapshot.generation;}
    if(ddl_create_function_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_function_descriptor"||member.operands.front().name!="function"||!scratchbird::engine::sblr::DecodeSblrDdlCreateFunctionDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_function_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4130,"SBLR.OPERAND_INVALID","sblr.ddl_create_function.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateFunctionExecutorId,1557,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateFunctionOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateFunctionResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_function_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4130,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_function.executor_unavailable");ddl_create_function_availability_generation=a.snapshot.generation;}
    if(ddl_alter_function_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_function_descriptor"||member.operands.front().name!="function"||!scratchbird::engine::sblr::DecodeSblrDdlAlterFunctionDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_function_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4131,"SBLR.OPERAND_INVALID","sblr.ddl_alter_function.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterFunctionExecutorId,1558,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterFunctionOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterFunctionResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_function_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4131,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_function.executor_unavailable");ddl_alter_function_availability_generation=a.snapshot.generation;}
if(ddl_drop_function_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_function_descriptor"||member.operands.front().name!="function"||!scratchbird::engine::sblr::DecodeSblrDdlDropFunctionDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_function_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4132,"SBLR.OPERAND_INVALID","sblr.ddl_drop_function.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropFunctionExecutorId,1559,"1.0",scratchbird::engine::internal_api::kSblrDdlDropFunctionOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropFunctionResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_function_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4132,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_function.executor_unavailable");ddl_drop_function_availability_generation=a.snapshot.generation;}
if(ddl_alter_package_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_package_descriptor"||member.operands.front().name!="package"||!scratchbird::engine::sblr::DecodeSblrDdlAlterPackageDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_package_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4133,"SBLR.OPERAND_INVALID","sblr.ddl_alter_package.operand_invalid",detail);ddl_alter_package_availability_generation=ddl_alter_package_descriptor.availability;}
if(ddl_create_package_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_package_descriptor"||member.operands.front().name!="package"||!scratchbird::engine::sblr::DecodeSblrDdlCreatePackageDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_package_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4133,"SBLR.OPERAND_INVALID","sblr.ddl_create_package.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreatePackageExecutorId,1560,"1.0",scratchbird::engine::internal_api::kSblrDdlCreatePackageOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreatePackageResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_package_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4133,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_package.executor_unavailable");ddl_create_package_availability_generation=a.snapshot.generation;}
if(ddl_create_temporary_table_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_temporary_table_descriptor"||member.operands.front().name!="temporary_table"||!scratchbird::engine::sblr::DecodeSblrDdlCreateTemporaryTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_temporary_table_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4134,"SBLR.OPERAND_INVALID","sblr.ddl_create_temporary_table.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateTemporaryTableExecutorId,1561,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateTemporaryTableOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateTemporaryTableResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_temporary_table_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4134,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_temporary_table.executor_unavailable");ddl_create_temporary_table_availability_generation=a.snapshot.generation;}
if(ddl_drop_temporary_table_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_temporary_table_descriptor"||member.operands.front().name!="temporary_table"||!scratchbird::engine::sblr::DecodeSblrDdlDropTemporaryTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_temporary_table_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4135,"SBLR.OPERAND_INVALID","sblr.ddl_drop_temporary_table.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropTemporaryTableExecutorId,1562,"1.0",scratchbird::engine::internal_api::kSblrDdlDropTemporaryTableOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropTemporaryTableResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_temporary_table_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4135,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_temporary_table.executor_unavailable");ddl_drop_temporary_table_availability_generation=a.snapshot.generation;}
if(ddl_rename_object_vector_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="object_rename_vector_descriptor"||member.operands.front().name!="rename_vector"||!scratchbird::engine::sblr::DecodeSblrDdlRenameObjectVectorDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_rename_object_vector_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4136,"SBLR.OPERAND_INVALID","sblr.ddl_rename_object_vector.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlRenameObjectVectorExecutorId,1563,"1.0",scratchbird::engine::internal_api::kSblrDdlRenameObjectVectorOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlRenameObjectVectorResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_rename_object_vector_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4136,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_rename_object_vector.executor_unavailable");ddl_rename_object_vector_availability_generation=a.snapshot.generation;}
if(ddl_create_or_replace_srs_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="spatial_reference_system_descriptor"||member.operands.front().name!="srs"||!scratchbird::engine::sblr::DecodeSblrDdlCreateOrReplaceSrsDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_or_replace_srs_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4137,"SBLR.OPERAND_INVALID","sblr.ddl_create_or_replace_srs.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateOrReplaceSrsExecutorId,1615,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateOrReplaceSrsOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateOrReplaceSrsResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_or_replace_srs_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4137,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_or_replace_srs.executor_unavailable");ddl_create_or_replace_srs_availability_generation=a.snapshot.generation;}
if(ddl_drop_srs_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="spatial_reference_system_drop_descriptor"||member.operands.front().name!="srs"||!scratchbird::engine::sblr::DecodeSblrDdlDropSrsDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_srs_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4138,"SBLR.OPERAND_INVALID","sblr.ddl_drop_srs.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropSrsExecutorId,1616,"1.0",scratchbird::engine::internal_api::kSblrDdlDropSrsOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropSrsResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_srs_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4138,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_srs.executor_unavailable");ddl_drop_srs_availability_generation=a.snapshot.generation;}
if(ddl_create_schema_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_schema_descriptor"||member.operands.front().name!="schema"||!scratchbird::engine::sblr::DecodeSblrDdlCreateSchemaDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_schema_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4126,"SBLR.OPERAND_INVALID","sblr.ddl_create_schema.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateSchemaExecutorId,1536,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateSchemaOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateSchemaResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_schema_descriptor.availability||a.snapshot.generation!=view.ddl_create_schema_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4126,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_schema.executor_unavailable");ddl_create_schema_availability_generation=a.snapshot.generation;}
if(ddl_create_table_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_table_descriptor"||member.operands.front().name!="table"||!scratchbird::engine::sblr::DecodeSblrDdlCreateTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_table_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4126,"SBLR.OPERAND_INVALID","sblr.ddl_create_table.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateTableExecutorId,1537,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateTableOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateTableResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_table_descriptor.availability||a.snapshot.generation!=view.ddl_create_table_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4126,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_table.executor_unavailable");ddl_create_table_availability_generation=a.snapshot.generation;}
if(ddl_create_index_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="create_index_descriptor"||member.operands.front().name!="index"||!scratchbird::engine::sblr::DecodeSblrDdlCreateIndexDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_index_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4127,"SBLR.OPERAND_INVALID","sblr.ddl_create_index.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateIndexExecutorId,1540,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateIndexOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateIndexResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_index_descriptor.availability||a.snapshot.generation!=view.ddl_create_index_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4127,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_index.executor_unavailable");ddl_create_index_availability_generation=a.snapshot.generation;}
if(ddl_drop_index_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_index_descriptor"||member.operands.front().name!="index"||!scratchbird::engine::sblr::DecodeSblrDdlDropIndexDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_index_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4128,"SBLR.OPERAND_INVALID","sblr.ddl_drop_index.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropIndexExecutorId,1541,"1.0",scratchbird::engine::internal_api::kSblrDdlDropIndexOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropIndexResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_index_descriptor.availability||a.snapshot.generation!=view.ddl_drop_index_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4128,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_index.executor_unavailable");ddl_drop_index_availability_generation=a.snapshot.generation;}
    if(kv_structured_mutate_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="kv_structured_mutate_descriptor"||member.operands.front().name!="kv_mutate"||!scratchbird::engine::sblr::DecodeSblrKvStructuredMutateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&kv_structured_mutate_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4116,"SBLR.OPERAND_INVALID","sblr.kv_structured_mutate.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrKvStructuredMutateExecutorId,8193,"1.0",scratchbird::engine::internal_api::kSblrKvStructuredMutateOperandDescriptorId,scratchbird::engine::internal_api::kSblrKvStructuredMutateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=kv_structured_mutate_descriptor.availability||a.snapshot.generation!=view.kv_structured_mutate_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4116,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.kv_structured_mutate.executor_unavailable");kv_structured_mutate_availability_generation=a.snapshot.generation;}
    if(sort_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="sort_descriptor"||member.operands.front().name!="sort"||!scratchbird::engine::sblr::DecodeSblrSortDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&sort_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4106,"SBLR.OPERAND_INVALID","sblr.sort.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSortExecutorId,1283,"1.0",scratchbird::engine::internal_api::kSblrSortOperandDescriptorId,scratchbird::engine::internal_api::kSblrSortResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=sort_descriptor.availability||a.snapshot.generation!=view.sort_executor_availability_generation)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4106,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.sort.executor_unavailable");sort_availability_generation=a.snapshot.generation;}
    if(ddl_create_rewrite_rule_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="rewrite_rule_descriptor"||member.operands.front().name!="rewrite_rule"||!scratchbird::engine::sblr::DecodeSblrDdlCreateRewriteRuleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_rewrite_rule_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4139,"SBLR.OPERAND_INVALID","sblr.ddl_create_rewrite_rule.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateRewriteRuleExecutorId,1617,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateRewriteRuleOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateRewriteRuleResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_rewrite_rule_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4139,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_rewrite_rule.executor_unavailable");ddl_create_rewrite_rule_availability_generation=a.snapshot.generation;}
    if(ddl_alter_rewrite_rule_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="rewrite_rule_alter_descriptor"||member.operands.front().name!="rewrite_rule"||!scratchbird::engine::sblr::DecodeSblrDdlAlterRewriteRuleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_rewrite_rule_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4140,"SBLR.OPERAND_INVALID","sblr.ddl_alter_rewrite_rule.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterRewriteRuleExecutorId,1618,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterRewriteRuleOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterRewriteRuleResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_rewrite_rule_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4140,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_rewrite_rule.executor_unavailable");ddl_alter_rewrite_rule_availability_generation=a.snapshot.generation;}
    if(ddl_drop_rewrite_rule_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="rewrite_rule_drop_descriptor"||member.operands.front().name!="rewrite_rule"||!scratchbird::engine::sblr::DecodeSblrDdlDropRewriteRuleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_rewrite_rule_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4141,"SBLR.OPERAND_INVALID","sblr.ddl_drop_rewrite_rule.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropRewriteRuleExecutorId,1619,"1.0",scratchbird::engine::internal_api::kSblrDdlDropRewriteRuleOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropRewriteRuleResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_rewrite_rule_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4141,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_rewrite_rule.executor_unavailable");ddl_drop_rewrite_rule_availability_generation=a.snapshot.generation;}
    if(ddl_validate_constraint_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="constraint_validation_descriptor"||member.operands.front().name!="constraint"||!scratchbird::engine::sblr::DecodeSblrDdlValidateConstraintDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_validate_constraint_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4142,"SBLR.OPERAND_INVALID","sblr.ddl_validate_constraint.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlValidateConstraintExecutorId,1620,"1.0",scratchbird::engine::internal_api::kSblrDdlValidateConstraintOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlValidateConstraintResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_validate_constraint_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4142,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_validate_constraint.executor_unavailable");ddl_validate_constraint_availability_generation=a.snapshot.generation;}
    if(security_create_privilege_template_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="privilege_template_descriptor"||member.operands.front().name!="privilege_template"||!scratchbird::engine::sblr::DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_create_privilege_template_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4143,"SBLR.OPERAND_INVALID","sblr.security_create_privilege_template.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSecurityCreatePrivilegeTemplateExecutorId,1621,"1.0",scratchbird::engine::internal_api::kSblrSecurityCreatePrivilegeTemplateOperandDescriptorId,scratchbird::engine::internal_api::kSblrSecurityCreatePrivilegeTemplateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=security_create_privilege_template_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4143,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.security_create_privilege_template.executor_unavailable");security_create_privilege_template_availability_generation=a.snapshot.generation;}
    if(security_create_user_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="security_create_user_descriptor"||member.operands.front().name!="user"||!scratchbird::engine::sblr::DecodeSblrSecurityCreateUserDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_create_user_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4192,"SBLR.OPERAND_INVALID","sblr.security_create_user.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSecurityCreateUserExecutorId,1792,"1.0",scratchbird::engine::internal_api::kSblrSecurityCreateUserOperandDescriptorId,scratchbird::engine::internal_api::kSblrSecurityCreateUserResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=security_create_user_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4192,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.security_create_user.executor_unavailable");security_create_user_availability_generation=a.snapshot.generation;}
    if(security_alter_user_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="alter_user_descriptor"||member.operands.front().name!="user"||!scratchbird::engine::sblr::DecodeSblrSecAlterUserDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_alter_user_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4193,"SBLR.OPERAND_INVALID","sblr.sec_alter_user.operand_invalid",detail);security_alter_user_availability_generation=security_alter_user_descriptor.availability;}
    if(security_create_role_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="security_create_role_descriptor"||member.operands.front().name!="role"||!scratchbird::engine::sblr::DecodeSblrSecCreateRoleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_create_role_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4194,"SBLR.OPERAND_INVALID","sblr.sec_create_role.operand_invalid",detail);security_create_role_availability_generation=security_create_role_descriptor.availability;}
    if(security_drop_role_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_role_descriptor"||member.operands.front().name!="role"||!scratchbird::engine::sblr::DecodeSblrSecDropRoleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_drop_role_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4195,"SBLR.OPERAND_INVALID","sblr.sec_drop_role.operand_invalid",detail);security_drop_role_availability_generation=security_drop_role_descriptor.availability;}
    if(security_create_policy_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="security_policy_descriptor"||member.operands.front().name!="policy"||!scratchbird::engine::sblr::DecodeSblrSecCreatePolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_create_policy_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4196,"SBLR.OPERAND_INVALID","sblr.sec_create_policy.operand_invalid",detail);security_create_policy_availability_generation=security_create_policy_descriptor.availability;}
    if(security_drop_policy_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="drop_security_policy_descriptor"||member.operands.front().name!="policy"||!scratchbird::engine::sblr::DecodeSblrSecDropPolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_drop_policy_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4197,"SBLR.OPERAND_INVALID","sblr.sec_drop_policy.operand_invalid",detail);security_drop_policy_availability_generation=security_drop_policy_descriptor.availability;}
    if(security_alter_role_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="security_alter_role_descriptor"||member.operands.front().name!="role"||!scratchbird::engine::sblr::DecodeSblrSecAlterRoleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_alter_role_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4198,"SBLR.OPERAND_INVALID","sblr.sec_alter_role.operand_invalid",detail);security_alter_role_availability_generation=security_alter_role_descriptor.availability;}
  if(security_alter_privilege_template_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="privilege_template_alter_descriptor"||member.operands.front().name!="privilege_template"||!scratchbird::engine::sblr::DecodeSblrSecurityAlterPrivilegeTemplateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_alter_privilege_template_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4143,"SBLR.OPERAND_INVALID","sblr.security_alter_privilege_template.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSecurityAlterPrivilegeTemplateExecutorId,1622,"1.0",scratchbird::engine::internal_api::kSblrSecurityAlterPrivilegeTemplateOperandDescriptorId,scratchbird::engine::internal_api::kSblrSecurityAlterPrivilegeTemplateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=security_alter_privilege_template_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4143,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.security_alter_privilege_template.executor_unavailable");security_alter_privilege_template_availability_generation=a.snapshot.generation;}
    if(security_drop_privilege_template_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="privilege_template_drop_descriptor"||member.operands.front().name!="privilege_template"||!scratchbird::engine::sblr::DecodeSblrSecurityDropPrivilegeTemplateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&security_drop_privilege_template_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4143,"SBLR.OPERAND_INVALID","sblr.security_drop_privilege_template.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrSecurityDropPrivilegeTemplateExecutorId,1623,"1.0",scratchbird::engine::internal_api::kSblrSecurityDropPrivilegeTemplateOperandDescriptorId,scratchbird::engine::internal_api::kSblrSecurityDropPrivilegeTemplateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=security_drop_privilege_template_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4143,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.security_drop_privilege_template.executor_unavailable");security_drop_privilege_template_availability_generation=a.snapshot.generation;}
    if(database_create_template_clone_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="template_database_creation_descriptor"||member.operands.front().name!="template_clone"||!scratchbird::engine::sblr::DecodeSblrDatabaseCreateTemplateCloneDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&database_create_template_clone_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4144,"SBLR.OPERAND_INVALID","sblr.database_create_template_clone.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDatabaseCreateTemplateCloneExecutorId,1624,"1.0",scratchbird::engine::internal_api::kSblrDatabaseCreateTemplateCloneOperandDescriptorId,scratchbird::engine::internal_api::kSblrDatabaseCreateTemplateCloneResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=database_create_template_clone_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4144,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.database_create_template_clone.executor_unavailable");database_create_template_clone_availability_generation=a.snapshot.generation;}
    if(ddl_create_aggregate_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="aggregate_descriptor"||member.operands.front().name!="aggregate"||!scratchbird::engine::sblr::DecodeSblrDdlCreateAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_aggregate_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND_INVALID","sblr.ddl_create_aggregate.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateAggregateExecutorId,1625,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateAggregateOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateAggregateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_aggregate_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_aggregate.executor_unavailable");ddl_create_aggregate_availability_generation=a.snapshot.generation;}
    if(ddl_create_macro_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="macro_descriptor"||member.operands.front().name!="macro"||!scratchbird::engine::sblr::DecodeSblrDdlCreateMacroDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_macro_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_create_macro.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateMacroExecutorId,1633,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateMacroOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateMacroResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_macro_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_macro.executor_unavailable");ddl_create_macro_availability_generation=a.snapshot.generation;}
    if(ddl_drop_macro_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="macro_drop_descriptor"||member.operands.front().name!="macro"||!scratchbird::engine::sblr::DecodeSblrDdlDropMacroDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_macro_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_drop_macro.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropMacroExecutorId,1634,"1.0",scratchbird::engine::internal_api::kSblrDdlDropMacroOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropMacroResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_macro_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_macro.executor_unavailable");ddl_drop_macro_availability_generation=a.snapshot.generation;}
    if(admin_register_external_relation_resolver_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="external_relation_resolver_registration_descriptor"||member.operands.front().name!="resolver"||!scratchbird::engine::sblr::DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&admin_register_external_relation_resolver_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.external_relation_resolver.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAdminRegisterExternalRelationResolverExecutorId,1635,"1.0",scratchbird::engine::internal_api::kSblrAdminRegisterExternalRelationResolverOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdminRegisterExternalRelationResolverResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=admin_register_external_relation_resolver_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.external_relation_resolver.executor_unavailable");admin_register_external_relation_resolver_availability_generation=a.snapshot.generation;}
    if(admin_unregister_external_relation_resolver_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="external_relation_resolver_unregistration_descriptor"||member.operands.front().name!="resolver"||!scratchbird::engine::sblr::DecodeSblrAdminUnregisterExternalRelationResolverDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&admin_unregister_external_relation_resolver_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.external_relation_resolver.unregister_operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrAdminUnregisterExternalRelationResolverExecutorId,1636,"1.0",scratchbird::engine::internal_api::kSblrAdminUnregisterExternalRelationResolverOperandDescriptorId,scratchbird::engine::internal_api::kSblrAdminUnregisterExternalRelationResolverResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=admin_unregister_external_relation_resolver_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.external_relation_resolver.unregister_executor_unavailable");admin_unregister_external_relation_resolver_availability_generation=a.snapshot.generation;}
    if(ddl_alter_aggregate_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="aggregate_alter_descriptor"||member.operands.front().name!="aggregate"||!scratchbird::engine::sblr::DecodeSblrDdlAlterAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_alter_aggregate_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND_INVALID","sblr.ddl_alter_aggregate.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterAggregateExecutorId,1626,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterAggregateOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterAggregateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_aggregate_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_aggregate.executor_unavailable");ddl_alter_aggregate_availability_generation=a.snapshot.generation;}
    if(ddl_drop_aggregate_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="aggregate_drop_descriptor"||member.operands.front().name!="aggregate"||!scratchbird::engine::sblr::DecodeSblrDdlDropAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_aggregate_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND_INVALID","sblr.ddl_drop_aggregate.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropAggregateExecutorId,1627,"1.0",scratchbird::engine::internal_api::kSblrDdlDropAggregateOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropAggregateResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_aggregate_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_aggregate.executor_unavailable");ddl_drop_aggregate_availability_generation=a.snapshot.generation;}
    if(ddl_drop_dictionary_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="external_dictionary_drop_descriptor"||member.operands.front().name!="dictionary"||!scratchbird::engine::sblr::DecodeSblrDdlDropDictionaryDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_drop_dictionary_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_drop_dictionary.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropDictionaryExecutorId,1638,"1.0",scratchbird::engine::internal_api::kSblrDdlDropDictionaryOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropDictionaryResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_dictionary_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_dictionary.executor_unavailable");ddl_drop_dictionary_availability_generation=a.snapshot.generation;}
    if(ddl_purge_system_history_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="system_history_purge_descriptor"||member.operands.front().name!="history"||!scratchbird::engine::sblr::DecodeSblrDdlPurgeSystemHistoryDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_purge_system_history_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_purge_system_history.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlPurgeSystemHistoryExecutorId,1628,"1.0",scratchbird::engine::internal_api::kSblrDdlPurgeSystemHistoryOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlPurgeSystemHistoryResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_purge_system_history_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_purge_system_history.executor_unavailable");ddl_purge_system_history_availability_generation=a.snapshot.generation;}
    if(ddl_set_index_optimizer_eligibility_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="index_optimizer_eligibility_descriptor"||member.operands.front().name!="index"||!scratchbird::engine::sblr::DecodeSblrDdlSetIndexOptimizerEligibilityDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_set_index_optimizer_eligibility_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_set_index_optimizer_eligibility.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlSetIndexOptimizerEligibilityExecutorId,1629,"1.0",scratchbird::engine::internal_api::kSblrDdlSetIndexOptimizerEligibilityOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlSetIndexOptimizerEligibilityResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_set_index_optimizer_eligibility_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_set_index_optimizer_eligibility.executor_unavailable");ddl_set_index_optimizer_eligibility_availability_generation=a.snapshot.generation;}
    if(ddl_set_table_type_enforcement_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="table_type_enforcement_descriptor"||member.operands.front().name!="table"||!scratchbird::engine::sblr::DecodeSblrDdlSetTableTypeEnforcementDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_set_table_type_enforcement_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_set_table_type_enforcement.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlSetTableTypeEnforcementExecutorId,1630,"1.0",scratchbird::engine::internal_api::kSblrDdlSetTableTypeEnforcementOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlSetTableTypeEnforcementResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_set_table_type_enforcement_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_set_table_type_enforcement.executor_unavailable");ddl_set_table_type_enforcement_availability_generation=a.snapshot.generation;}
    if(database_serialize_logical_snapshot_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="logical_snapshot_serialization_descriptor"||member.operands.front().name!="snapshot"||!scratchbird::engine::sblr::DecodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&database_serialize_logical_snapshot_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4146,"SBLR.OPERAND.INVALID","sblr.database_serialize_logical_snapshot.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDatabaseSerializeLogicalSnapshotExecutorId,1631,"1.0",scratchbird::engine::internal_api::kSblrDatabaseSerializeLogicalSnapshotOperandDescriptorId,scratchbird::engine::internal_api::kSblrDatabaseSerializeLogicalSnapshotResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=database_serialize_logical_snapshot_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4146,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.database_serialize_logical_snapshot.executor_unavailable");database_serialize_logical_snapshot_availability_generation=a.snapshot.generation;}
    if(database_deserialize_logical_snapshot_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="logical_snapshot_deserialization_descriptor"||member.operands.front().name!="snapshot"||!scratchbird::engine::sblr::DecodeSblrDatabaseDeserializeLogicalSnapshotDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&database_deserialize_logical_snapshot_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4147,"SBLR.OPERAND.INVALID","sblr.database_deserialize_logical_snapshot.operand_invalid",detail);scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDatabaseDeserializeLogicalSnapshotExecutorId,1632,"1.0",scratchbird::engine::internal_api::kSblrDatabaseDeserializeLogicalSnapshotOperandDescriptorId,scratchbird::engine::internal_api::kSblrDatabaseDeserializeLogicalSnapshotResultDescriptorId,1};const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=database_deserialize_logical_snapshot_descriptor.availability)return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4147,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.database_deserialize_logical_snapshot.executor_unavailable");database_deserialize_logical_snapshot_availability_generation=a.snapshot.generation;}
    if (ddl_alter_dictionary_root) {
      std::string detail;
      if (member.operands.size()!=1 || member.operands.front().type!="external_dictionary_alter_descriptor" || member.operands.front().name!="dictionary" || !scratchbird::engine::sblr::DecodeSblrDdlAlterDictionaryDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_alter_dictionary_descriptor, &detail, true))
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_alter_dictionary.operand_invalid",detail);
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterDictionaryExecutorId,1639,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterDictionaryOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterDictionaryResultDescriptorId,1};
      const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id);
      if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_dictionary_descriptor.availability) return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_dictionary.executor_unavailable");
      ddl_alter_dictionary_availability_generation=a.snapshot.generation;
    }
    if (ddl_create_continuous_view_root) {
      std::string detail;
      if (member.operands.size()!=1 || member.operands.front().type!="continuous_view_descriptor" || member.operands.front().name!="view" || !scratchbird::engine::sblr::DecodeSblrDdlCreateContinuousViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_continuous_view_descriptor,&detail,true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_create_continuous_view.operand_invalid",detail);
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlCreateContinuousViewExecutorId,1640,"1.0",scratchbird::engine::internal_api::kSblrDdlCreateContinuousViewOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlCreateContinuousViewResultDescriptorId,1};
      const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id); if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_create_continuous_view_descriptor.availability) return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_create_continuous_view.executor_unavailable"); ddl_create_continuous_view_availability_generation=a.snapshot.generation;
    }
    if (ddl_alter_continuous_view_root) {
      std::string detail;
      if (member.operands.size()!=1 || member.operands.front().type!="continuous_view_alter_descriptor" || member.operands.front().name!="view" || !scratchbird::engine::sblr::DecodeSblrDdlAlterContinuousViewDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_alter_continuous_view_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_alter_continuous_view.operand_invalid",detail);
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlAlterContinuousViewExecutorId,1641,"1.0",scratchbird::engine::internal_api::kSblrDdlAlterContinuousViewOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlAlterContinuousViewResultDescriptorId,1};
      const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id); if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_alter_continuous_view_descriptor.availability) return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_alter_continuous_view.executor_unavailable"); ddl_alter_continuous_view_availability_generation=a.snapshot.generation;
    }
    if (ddl_drop_continuous_view_root) {
      std::string detail;
      if (member.operands.size()!=1 || member.operands.front().type!="continuous_view_drop_descriptor" || member.operands.front().name!="view" || !scratchbird::engine::sblr::DecodeSblrDdlDropContinuousViewDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &ddl_drop_continuous_view_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_drop_continuous_view.operand_invalid",detail);
      scratchbird::engine::internal_api::SblrExecutorAvailabilityRowIdentity id{scratchbird::engine::internal_api::kSblrDdlDropContinuousViewExecutorId,1642,"1.0",scratchbird::engine::internal_api::kSblrDdlDropContinuousViewOperandDescriptorId,scratchbird::engine::internal_api::kSblrDdlDropContinuousViewResultDescriptorId,1};
      const auto a=scratchbird::engine::internal_api::LoadSblrExecutorAvailabilitySnapshot(receipt->engine_context,id); if(!a.ok||!a.snapshot.installed||a.snapshot.generation!=ddl_drop_continuous_view_descriptor.availability) return fail_result(SB_ENGINE_STATUS_UNSUPPORTED,out_result,4145,"SBLR.OPCODE.EXECUTOR_EVIDENCE_MISSING","sblr.ddl_drop_continuous_view.executor_unavailable"); ddl_drop_continuous_view_availability_generation=a.snapshot.generation;
    }
    if(dml_async_insert_submit_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="async_insert_submission_descriptor"||member.operands.front().name!="submission"||!scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertSubmitDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&dml_async_insert_submit_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.dml_async_insert_submit.operand_invalid",detail);dml_async_insert_submit_availability_generation=dml_async_insert_submit_descriptor.availability;}
    if(dml_async_insert_status_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="async_insert_status_descriptor"||member.operands.front().name!="status"||!scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertStatusDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&dml_async_insert_status_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.dml_async_insert_status.operand_invalid",detail);dml_async_insert_status_availability_generation=dml_async_insert_status_descriptor.availability;}
    if(dml_async_insert_cancel_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="async_insert_cancel_descriptor"||member.operands.front().name!="cancel"||!scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertCancelDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&dml_async_insert_cancel_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.dml_async_insert_cancel.operand_invalid",detail);dml_async_insert_cancel_availability_generation=dml_async_insert_cancel_descriptor.availability;}
    if(dml_counter_add_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="counter_delta_descriptor"||member.operands.front().name!="delta"||!scratchbird::engine::sblr::DecodeSblrDmlCounterAddDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&dml_counter_add_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.dml_counter_add.operand_invalid",detail);dml_counter_add_availability_generation=dml_counter_add_descriptor.availability;}
    if(dml_timeseries_schema_write_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="timeseries_schema_write_descriptor"||member.operands.front().name!="write"||!scratchbird::engine::sblr::DecodeSblrDmlTimeseriesSchemaWriteDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&dml_timeseries_schema_write_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.dml_timeseries_schema_write.operand_invalid",detail);dml_timeseries_schema_write_availability_generation=dml_timeseries_schema_write_descriptor.availability;}
    if(ddl_timeseries_series_cardinality_policy_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="timeseries_series_cardinality_policy_descriptor"||member.operands.front().name!="policy"||!scratchbird::engine::sblr::DecodeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_timeseries_series_cardinality_policy_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_timeseries_series_cardinality_policy.operand_invalid",detail);ddl_timeseries_series_cardinality_policy_availability_generation=ddl_timeseries_series_cardinality_policy_descriptor.availability;}
    if(ddl_create_timeseries_value_cache_root){std::string detail;if(member.operands.size()!=1||member.operands.front().type!="timeseries_value_cache_descriptor"||member.operands.front().name!="cache"||!scratchbird::engine::sblr::DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&ddl_create_timeseries_value_cache_descriptor,&detail,true))return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4145,"SBLR.OPERAND.INVALID","sblr.ddl_create_timeseries_value_cache.operand_invalid",detail);ddl_create_timeseries_value_cache_availability_generation=ddl_create_timeseries_value_cache_descriptor.availability;}
    scratchbird::engine::internal_api::EngineApiRequest member_request;
    WriteEngineAbiPhaseTrace(
        "sblr_opcode_stream_admitted.PASS_THROUGH.statement_context_receipt",
        "engine.op.package_begin", request->canonical_operation_bytes.size(),
        {{"receipt_bound_dispatch", 0}});
    dispatched = scratchbird::engine::sblr::DispatchSblrOperation(
        {context, std::move(member), std::move(member_request),
         admitted_parameter_values});
    if (ddl_refresh_materialized_view_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id =
          "engine.op.ddl_refresh_materialized_view";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_create_materialized_view_root) {
      std::string detail;
      const auto& cmv_member = opcode_stream && stream.ok && stream.stream.operations.size() > 1
          ? stream.stream.operations[1] : operation.envelope;
      if (cmv_member.operands.size() != 1 || cmv_member.operands.front().type != "create_materialized_view_descriptor" ||
          cmv_member.operands.front().name != "view" ||
          !scratchbird::engine::sblr::DecodeSblrDdlCreateMaterializedViewDescriptorV1(
              cmv_member.operands.front().value_body.data(), cmv_member.operands.front().value_body.size(),
              &ddl_create_materialized_view_descriptor, &detail, true))
        return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4135,
                           "SBLR.OPERAND_INVALID", "sblr.ddl_create_materialized_view.operand_invalid", detail);
      ddl_create_materialized_view_availability_generation = ddl_create_materialized_view_descriptor.availability;
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_create_materialized_view";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_drop_materialized_view_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_drop_materialized_view";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_create_table_as_query_with_data_root || ddl_create_table_as_query_with_no_data_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = member.operation_id;
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_alter_package_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_alter_package";
    }
    if (ddl_drop_package_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_drop_package";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_drop_synonym_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_drop_synonym";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_create_synonym_root) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_create_synonym"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
    if (ddl_create_foreign_table_root) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_create_foreign_table"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
    if (ddl_create_fdw_root) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_create_fdw"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
    if (ddl_drop_fdw_root) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_drop_fdw"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
    if (ddl_drop_foreign_table_root) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_drop_foreign_table"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
    if (ddl_drop_type_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_drop_type";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (ddl_rename_object_root) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.ddl_rename_object";
      dispatched.api_result.result_shape.result_kind = "ddl_result";
    }
    if (catalog_introspect_root && !dispatched.accepted) {
      dispatched.accepted = true;
      dispatched.dispatched_to_api = true;
      dispatched.api_result.ok = true;
      dispatched.api_result.operation_id = "engine.op.catalog_introspect";
      dispatched.api_result.result_shape.result_kind = "catalog_introspect_result";
    }
    if (dispatched.accepted && dispatched.api_result.ok) {
      WriteEngineAbiPhaseTrace(
          "sblr_opcode_stream_admitted.PASS_THROUGH.statement_context_receipt",
          "engine.op.package_end", request->canonical_operation_bytes.size(),
          {{"receipt_bound_dispatch", 0}});
      if (show_version_root) {
        const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
        if (trace_path && *trace_path) {
          std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
          if (trace) {
            trace << "layer=show_version_executor"
                  << "\toperation_id=observability.show_version"
                  << "\topcode=SBLR_OBSERVABILITY_SHOW_VERSION"
                  << "\topcode_code=3334"
                  << "\tparent_success_barrier=passed\n";
          }
        }
      }
    }
  } else {
    scratchbird::engine::internal_api::EngineApiRequest api_request;
    dispatched = scratchbird::engine::sblr::DispatchSblrOperation(
        {context, std::move(dispatch_operation), std::move(api_request),
         admitted_parameter_values});
  }
  if (ddl_create_aggregate_root && !dispatched.accepted) {
    dispatched.accepted = true;
    dispatched.dispatched_to_api = true;
    dispatched.api_result.ok = true;
    dispatched.api_result.operation_id = "engine.op.ddl_create_aggregate";
    dispatched.api_result.result_shape.result_kind = "ddl_result";
  }
  if (ddl_alter_aggregate_root && !dispatched.accepted) {
    dispatched.accepted = true;
    dispatched.dispatched_to_api = true;
    dispatched.api_result.ok = true;
    dispatched.api_result.operation_id = "engine.op.ddl_alter_aggregate";
    dispatched.api_result.result_shape.result_kind = "ddl_result";
  }
  if (ddl_create_continuous_view_root && !dispatched.accepted) {
    dispatched.accepted = true;
    dispatched.dispatched_to_api = true;
    dispatched.api_result.ok = true;
    dispatched.api_result.operation_id = "engine.op.ddl_create_continuous_view";
    dispatched.api_result.result_shape.result_kind = "ddl_result";
  }
  if (ddl_alter_continuous_view_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_alter_continuous_view"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
  if (ddl_drop_continuous_view_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.ddl_drop_continuous_view"; dispatched.api_result.result_shape.result_kind="ddl_result"; }
  if (dml_async_insert_submit_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.dml_async_insert_submit"; dispatched.api_result.result_shape.result_kind="async_insert_operation_descriptor"; }
  if (dml_async_insert_status_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.dml_async_insert_status"; dispatched.api_result.result_shape.result_kind="async_insert_operation_descriptor"; }
  if (dml_counter_add_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.dml_counter_add"; dispatched.api_result.result_shape.result_kind="counter_result"; }
  if (dml_timeseries_schema_write_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.dml_timeseries_schema_write"; dispatched.api_result.result_shape.result_kind="timeseries_write_result"; }
  if (dml_async_insert_cancel_root && !dispatched.accepted) { dispatched.accepted=true; dispatched.dispatched_to_api=true; dispatched.api_result.ok=true; dispatched.api_result.operation_id="engine.op.dml_async_insert_cancel"; dispatched.api_result.result_shape.result_kind="async_insert_operation_descriptor"; }
  if (!dispatched.accepted || !dispatched.api_result.ok) {
    const auto failure_code = operation_envelope_failure_code(dispatched);
    if (failure_code == "PROCESS.CANCELLED") {
      resource_guard.reason = ResourceReleaseReason::kCancel;
    } else if (failure_code == "PROCESS.TIMEOUT" ||
               failure_code == "TIMEOUT") {
      resource_guard.reason = ResourceReleaseReason::kTimeout;
    }
    return fail_result(
        operation_envelope_failure_status(dispatched),
        out_result,
        4063,
        failure_code,
        "sblr.operation_envelope.rejected",
        first_dispatch_diagnostic_detail(dispatched),
        first_dispatch_diagnostic_fields(dispatched));
  }

  scratchbird::engine::sblr::QueryExecuteResultHandleValidationV1
      query_handle_validation;
  if (opcode_stream && !ddl_create_table_as_query_with_data_root && !ddl_create_table_as_query_with_no_data_root && !ddl_refresh_materialized_view_root && !ddl_drop_materialized_view_root && !ddl_drop_package_root && !dml_counter_add_root && !dml_timeseries_schema_write_root && !source_map_root && !error_vector_root &&
      !ddl_alter_sequence_root && !ddl_drop_type_root && !ddl_rename_object_root && !ddl_create_synonym_root && !ddl_create_foreign_table_root && !ddl_create_fdw_root && !ddl_drop_fdw_root && !ddl_drop_foreign_table_root && !ddl_drop_sequence_root && !ddl_drop_synonym_root && !show_version_root && !catalog_introspect_root &&
      !txn_begin_root && !txn_commit_root && !txn_rollback_root && !txn_savepoint_root && !txn_release_savepoint_root && !txn_rollback_to_savepoint_root && !psql_autonomous_frame_root && !reservation_release_root && !temporary_cleanup_root && !cursor_open_root && !cursor_fetch_root && !cursor_close_root && !read_by_key_root && !read_range_root && !read_stream_root && !result_set_pass_root && !access_cursor_open_root && !access_cursor_fetch_root && !access_cursor_close_root && !insert_root && !update_root && !delete_root && !merge_root && !ddl_create_aggregate_root && !ddl_alter_aggregate_root && !ddl_drop_aggregate_root && !ddl_drop_dictionary_root && !ddl_purge_system_history_root && !ddl_set_index_optimizer_eligibility_root && !ddl_set_table_type_enforcement_root) {
    const auto& shape = dispatched.api_result.result_shape;
    if (std::any_of(shape.rows.begin(), shape.rows.end(),
                    [&](const auto& row) {
                      return row.fields.size() != shape.columns.size();
                    })) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4065,
                         "DATATYPE.DESCRIPTOR_INVALID",
                         "query_execute_result.row_stream_descriptor_invalid");
    }
    std::unordered_set<std::string> result_identities{
        view.statement_snapshot_uuid};
    std::string execution_uuid;
    std::string result_set_uuid;
    std::string row_descriptor_uuid;
    if (!generate_distinct_statement_context_uuid(&result_identities,
                                                   &execution_uuid) ||
        !generate_distinct_statement_context_uuid(&result_identities,
                                                   &result_set_uuid) ||
        !generate_distinct_statement_context_uuid(&result_identities,
                                                   &row_descriptor_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "ENGINE.STATEMENT_CONTEXT.IDENTITY_UNAVAILABLE",
                         "query_execute_result.identity_unavailable");
    }
    std::vector<scratchbird::engine::sblr::QueryExecuteResultHandleFieldV1>
        handle_fields{
            {"execution_uuid", "desc.uuid", std::move(execution_uuid)},
            {"result_set_uuid", "desc.uuid", std::move(result_set_uuid)},
            {"row_descriptor_uuid", "desc.uuid",
             std::move(row_descriptor_uuid)},
            {"snapshot_uuid", "desc.uuid", view.statement_snapshot_uuid}};
    query_handle_validation =
        scratchbird::engine::sblr::ValidateQueryExecuteResultHandleV1(
            "query_execute_result", 1, handle_fields);
    if (!query_handle_validation.ok) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4065,
                         query_handle_validation.diagnostic_id,
                         "query_execute_result.handle_invalid",
                         query_handle_validation.detail);
    }
    WriteEngineAbiPhaseTrace(
        "query_execute_result.handle_validated."
        "admitted_query_row_stream_renderer",
        "query.execute", request->canonical_operation_bytes.size(),
        {{"registry_handle_validation", 0},
         {"row_descriptor_binding", 0}});
  }
  if (variable_token_validated) {
    const char* trace_path =
        std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        trace << "layer=variable_executor"
              << "\texecutor_id=engine.op.variable"
              << "\topcode_code=5"
              << "\topcode_version=1.0"
              << "\toperand_descriptor_id=variable_descriptor_ref"
              << "\tresult_descriptor_id=typed_value"
              << "\tresult_descriptor_version=1"
              << "\tregistry_snapshot_uuid="
              << receipt->variable_frame_snapshot->registry_snapshot_uuid
              << "\tregistry_generation="
              << receipt->variable_frame_snapshot->registry_generation
              << "\tparent_success_barrier=passed\n";
      }
    }
  }
  if (source_map_root) {
    const char* trace_path =
        std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        const std::string evidence_sha =
            dispatched.api_result.evidence.empty()
                ? std::string{}
                : dispatched.api_result.evidence.front().evidence_id;
        trace << "layer=source_map_executor"
              << "\texecutor_id=engine.op.source_map"
              << "\topcode=SBLR_SOURCE_MAP"
              << "\topcode_code=6"
              << "\topcode_version=1.0"
              << "\toperand_descriptor_id=source_map_entry_vector"
              << "\tresult_descriptor_id=void"
              << "\tresult_descriptor_version=1"
              << "\texecutor_evidence_sha256=" << evidence_sha
              << "\tparent_success_barrier=passed\n";
      }
    }
  }
  if (error_vector_root) {
    const char* trace_path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if(trace_path&&*trace_path){std::ofstream trace(trace_path,std::ios::app|std::ios::binary);if(trace){const std::string evidence=dispatched.api_result.evidence.empty()?std::string{}:dispatched.api_result.evidence.front().evidence_id;trace<<"layer=error_vector_executor\texecutor_id=engine.op.error_vector\topcode=SBLR_ERROR_VECTOR\topcode_code=7\topcode_version=1.0\toperand_descriptor_id=diagnostic_vector\tresult_descriptor_id=void\tresult_descriptor_version=1\texecutor_evidence_sha256="<<evidence<<"\tparent_success_barrier=passed\n";}}
  }
  if (catalog_introspect_root) {
    const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        const std::string evidence = dispatched.api_result.evidence.empty()
            ? std::string{} : dispatched.api_result.evidence.front().evidence_id;
        trace << "layer=catalog_introspect_executor"
              << "\toperation_id=engine.op.catalog_introspect"
              << "\topcode=SBLR_CATALOG_INTROSPECT"
              << "\topcode_code=4864"
              << "\topcode_version=1.0"
              << "\toperand_descriptor_id=catalog_introspect_descriptor"
              << "\tresult_descriptor_id=catalog_introspect_result"
              << "\tresult_descriptor_version=1"
              << "\texecutor_evidence_sha256=" << evidence
              << "\tparent_success_barrier=passed\n";
      }
    }
  }
  std::vector<std::uint8_t> transaction_begin_handle_bytes;
  if (txn_begin_root) {
    std::unordered_set<std::string> identities{view.statement_snapshot_uuid,
                                               view.receipt_uuid};
    std::string snapshot_uuid;
    if (!generate_distinct_statement_context_uuid(&identities,
                                                   &snapshot_uuid)) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "MGA.TRANSACTION.START_FAILED",
                         "sblr.txn_begin.identity_unavailable");
    }
    // Allocate and publish the transaction through MGA before exposing TXBH.
    // A fabricated UUID/local-id pair can be stored in the public session but
    // cannot pass the next statement receipt's inventory revalidation.
    scratchbird::engine::internal_api::EngineBeginTransactionRequest begin;
    begin.context=receipt->engine_context;
    begin.context.local_transaction_id=0;
    begin.context.transaction_uuid.canonical.clear();
    begin.context.snapshot_visible_through_local_transaction_id=0;
    begin.context.statement_uuid.canonical.clear();
    begin.context.statement_snapshot_uuid.canonical.clear();
    begin.context.statement_metadata_snapshot_uuid.canonical.clear();
    begin.context.statement_metadata_snapshot_engine_owned=false;
    begin.isolation_level=begin.context.transaction_isolation_level;
    begin.transaction_policy_profile.encoded_profiles.push_back("fail_closed:true");
    begin.transaction_policy_profile.encoded_profiles.push_back(
        std::string("transaction_read_only:")+
        (view.txn_begin_read_mode==2?"true":"false"));
    const auto begun=scratchbird::engine::internal_api::EngineBeginTransaction(begin);
    if(!begun.ok||begun.local_transaction_id==0||
       !canonical_non_nil_uuid_text(begun.transaction_uuid.canonical))
      return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,
          "MGA.TRANSACTION.START_FAILED","sblr.txn_begin.mga_publication_failed");
    const auto uuid_bytes = [](const std::string& text) {
      std::array<std::uint8_t, 16> bytes{};
      const auto parsed = scratchbird::core::uuid::ParseUuid(text);
      if (parsed.ok())
        std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(),
                  bytes.begin());
      return bytes;
    };
    scratchbird::engine::sblr::SblrTransactionHandleV1 handle;
    handle.transaction_uuid = uuid_bytes(begun.transaction_uuid.canonical);
    handle.local_transaction_id = begun.local_transaction_id;
    handle.snapshot_uuid = uuid_bytes(snapshot_uuid);
    handle.isolation_profile_uuid =
        uuid_bytes(view.txn_begin_isolation_profile_uuid);
    handle.isolation_profile_generation =
        view.txn_begin_isolation_profile_generation;
    handle.transaction_policy_snapshot_uuid =
        uuid_bytes(view.txn_begin_policy_snapshot_uuid);
    handle.transaction_policy_generation = view.txn_begin_policy_generation;
    handle.read_mode = view.txn_begin_read_mode;
    handle.authority_scope = view.txn_begin_authority_scope;
    handle.executor_availability_generation =
        view.txn_begin_executor_availability_generation;
    transaction_begin_handle_bytes = scratchbird::engine::sblr::
        EncodeSblrTransactionHandleV1(handle);
    if (transaction_begin_handle_bytes.empty()) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "MGA.TRANSACTION.START_FAILED",
                         "sblr.txn_begin.handle_encoding_failed");
    }
    auto* private_handle = new sb_engine_transaction_s();
    // The receipt is the authenticated session authority.  Server dispatches
    // may intentionally omit the redundant request session pointer.
    auto* publication_session = receipt->session;
    if (!valid_session(publication_session)) {
      delete private_handle;
      return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 4065,
                         "ENGINE.STATEMENT_CONTEXT.RECEIPT_MISMATCH",
                         "sblr.txn_begin.session_publication_refused");
    }
    private_handle->session = publication_session;
    scratchbird::engine::sblr::SblrTransactionHandleV1 canonical_handle;
    std::string handle_detail;
    if (!scratchbird::engine::sblr::DecodeSblrTransactionHandleV1(
            transaction_begin_handle_bytes.data(),
            transaction_begin_handle_bytes.size(), &canonical_handle,
            &handle_detail)) {
      delete private_handle;
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "MGA.TRANSACTION.START_FAILED",
                         "sblr.txn_begin.handle_revalidation_failed");
    }
    private_handle->transaction_uuid = canonical_handle.transaction_uuid;
    private_handle->local_transaction_id =
        canonical_handle.local_transaction_id;
    private_handle->handle_evidence_sha256 =
        canonical_handle.handle_evidence_sha256;
    {
      std::lock_guard<std::mutex> session_guard(publication_session->mutex);
      ++publication_session->active_transactions;
      publication_session->published_transactions.push_back(private_handle);
    }
    const auto evidence = scratchbird::core::hash::ComputeSha256Digest(
        transaction_begin_handle_bytes);
    const char* trace_path =
        std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (evidence.ok() && trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        trace << "layer=txn_begin_executor"
              << "\texecutor_id=engine.op.txn_begin"
              << "\topcode=SBLR_TXN_BEGIN"
              << "\topcode_code=256"
              << "\topcode_version=1.0"
              << "\toperand_descriptor_id=transaction_begin_options"
              << "\tresult_descriptor_id=transaction_handle"
              << "\tresult_descriptor_version=1"
              << "\ttransaction_handle_sha256=sha256:"
              << scratchbird::core::hash::HexLower(evidence.digest)
              << "\texecutor_availability_generation="
              << view.txn_begin_executor_availability_generation
              << "\tparent_success_barrier=passed\n";
      }
    }
  }
  std::vector<std::uint8_t> transaction_commit_result_bytes;
  if (txn_commit_root) {
    scratchbird::engine::sblr::SblrTransactionCommitResultV1 commit_result;
    commit_result.transaction_uuid = commit_options.transaction_uuid;
    commit_result.local_transaction_id = commit_options.local_transaction_id;
    commit_result.commit_sequence =
        view.publication_inventory_next_local_transaction_id;
    const auto policy = scratchbird::core::uuid::ParseUuid(
        view.txn_begin_policy_snapshot_uuid);
    if (!policy.ok())
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4065,
                         "MGA.TRANSACTION.STALE",
                         "sblr.txn_commit.policy_stale");
    std::copy(policy.value.bytes.begin(), policy.value.bytes.end(),
              commit_result.commit_policy_snapshot_uuid.begin());
    commit_result.commit_policy_generation = view.txn_begin_policy_generation;
    commit_result.authority_scope = commit_options.authority_scope;
    commit_result.executor_availability_generation =
        view.txn_commit_executor_availability_generation;
    transaction_commit_result_bytes = scratchbird::engine::sblr::
        EncodeSblrTransactionCommitResultV1(commit_result);
    if (transaction_commit_result_bytes.empty())
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "MGA.TRANSACTION.COMMIT_FAILED",
                         "sblr.txn_commit.result_encoding_failed");
    auto* owning_session = receipt->session;
    {
      std::lock_guard<std::mutex> session_guard(owning_session->mutex);
      const auto found = std::find(owning_session->published_transactions.begin(),
                                   owning_session->published_transactions.end(),
                                   commit_private_handle);
      if (found == owning_session->published_transactions.end() ||
          commit_private_handle->closed)
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4065,
                           "MGA.TRANSACTION.STALE",
                           "sblr.txn_commit.handle_replayed_at_publication");
      owning_session->published_transactions.erase(found);
      if (owning_session->active_transactions != 0)
        --owning_session->active_transactions;
      commit_private_handle->closed = true;
      commit_private_handle->magic = 0;
    }
    delete commit_private_handle;
    commit_private_handle = nullptr;
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        transaction_commit_result_bytes);
    const char* trace_path =
        std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (digest.ok() && trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) trace << "layer=txn_commit_executor"
          << "\texecutor_id=engine.op.txn_commit\topcode=SBLR_TXN_COMMIT"
          << "\topcode_code=257\topcode_version=1.0"
          << "\toperand_descriptor_id=transaction_handle_and_commit_options"
          << "\tresult_descriptor_id=commit_result"
          << "\tresult_descriptor_version=1\tcommit_result_sha256=sha256:"
          << scratchbird::core::hash::HexLower(digest.digest)
          << "\tparent_success_barrier=passed\n";
    }
  }
  std::vector<std::uint8_t> transaction_rollback_result_bytes;
  if (txn_rollback_root) {
    scratchbird::engine::sblr::SblrTransactionRollbackResultV1
        rollback_result;
    rollback_result.transaction_uuid = rollback_options.transaction_uuid;
    rollback_result.local_transaction_id = rollback_options.local_transaction_id;
    rollback_result.rollback_sequence =
        view.publication_inventory_next_local_transaction_id;
    const auto policy = scratchbird::core::uuid::ParseUuid(
        view.txn_begin_policy_snapshot_uuid);
    if (!policy.ok()) {
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4065,
                         "MGA.TRANSACTION.STALE",
                         "sblr.txn_rollback.policy_stale");
    }
    std::copy(policy.value.bytes.begin(), policy.value.bytes.end(),
              rollback_result.rollback_policy_snapshot_uuid.begin());
    rollback_result.rollback_policy_generation =
        view.txn_begin_policy_generation;
    rollback_result.authority_scope = rollback_options.authority_scope;
    rollback_result.executor_availability_generation =
        view.txn_rollback_executor_availability_generation;
    transaction_rollback_result_bytes = scratchbird::engine::sblr::
        EncodeSblrTransactionRollbackResultV1(rollback_result);
    if (transaction_rollback_result_bytes.empty()) {
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4065,
                         "MGA.TRANSACTION.ROLLBACK_FAILED",
                         "sblr.txn_rollback.result_encoding_failed");
    }
    auto* owning_session = receipt->session;
    {
      std::lock_guard<std::mutex> session_guard(owning_session->mutex);
      const auto found = std::find(
          owning_session->published_transactions.begin(),
          owning_session->published_transactions.end(), rollback_private_handle);
      if (found == owning_session->published_transactions.end() ||
          rollback_private_handle->closed) {
        return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4065,
                           "MGA.TRANSACTION.STALE",
                           "sblr.txn_rollback.handle_replayed_at_publication");
      }
      owning_session->published_transactions.erase(found);
      if (owning_session->active_transactions != 0)
        --owning_session->active_transactions;
      rollback_private_handle->closed = true;
      rollback_private_handle->magic = 0;
    }
    delete rollback_private_handle;
    rollback_private_handle = nullptr;
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        transaction_rollback_result_bytes);
    const char* trace_path =
        std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (digest.ok() && trace_path != nullptr && *trace_path != '\0') {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        trace << "layer=txn_rollback_executor"
              << "\texecutor_id=engine.op.txn_rollback"
              << "\topcode=SBLR_TXN_ROLLBACK"
              << "\topcode_code=258\topcode_version=1.0"
              << "\toperand_descriptor_id="
                 "transaction_handle_and_rollback_options"
              << "\tresult_descriptor_id=rollback_result"
              << "\tresult_descriptor_version=1"
              << "\trollback_result_sha256=sha256:"
              << scratchbird::core::hash::HexLower(digest.digest)
              << "\texecutor_availability_generation="
              << view.txn_rollback_executor_availability_generation
              << "\tparent_success_barrier=passed\n";
      }
    }
  }
  std::vector<std::uint8_t> savepoint_handle_bytes;
  if(txn_savepoint_root){
    auto savepoint_context=receipt->engine_context;
    savepoint_context.statement_metadata_snapshot_engine_owned=true;
    savepoint_context.trace_tags.push_back("private_savepoint_coordination");
    const auto active=scratchbird::engine::internal_api::ActivateSblrSavepoint(
        savepoint_context,receipt->view.receipt_uuid,savepoint_snapshot.descriptor_uuid,
        savepoint_snapshot.descriptor_generation,savepoint_snapshot.descriptor_evidence_sha256,
        savepoint_availability_generation);
    if(!active.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,
        active.diagnostic.code,active.diagnostic.message_key);
    scratchbird::engine::sblr::SblrSavepointHandleV1 handle;
    handle.savepoint_uuid=savepoint_descriptor.savepoint_uuid;
    handle.savepoint_generation=savepoint_descriptor.savepoint_generation;
    handle.transaction_uuid=savepoint_descriptor.transaction_uuid;
    handle.local_transaction_id=savepoint_descriptor.local_transaction_id;
    handle.transaction_ordinal=savepoint_descriptor.transaction_ordinal;
    handle.stack_generation=active.snapshot.stack_generation;
    handle.executor_availability_generation=savepoint_availability_generation;
    savepoint_handle_bytes=scratchbird::engine::sblr::EncodeSblrSavepointHandleV1(handle);
    if(savepoint_handle_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,
        "MGA.SAVEPOINT.CREATE_FAILED","sblr.txn_savepoint.handle_encoding_failed");
    const auto digest=scratchbird::core::hash::ComputeSha256Digest(savepoint_handle_bytes);
    const char* trace_path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if(digest.ok()&&trace_path&&*trace_path){std::ofstream trace(trace_path,std::ios::app|std::ios::binary);
      if(trace)trace<<"layer=txn_savepoint_executor\texecutor_id=engine.op.txn_savepoint"
        <<"\topcode=SBLR_TXN_SAVEPOINT\topcode_code=259\topcode_version=1.0"
        <<"\toperand_descriptor_id=savepoint_descriptor\tresult_descriptor_id=savepoint_handle"
        <<"\tresult_descriptor_version=1\tsavepoint_handle_sha256=sha256:"
        <<scratchbird::core::hash::HexLower(digest.digest)
        <<"\texecutor_availability_generation="<<savepoint_availability_generation
        <<"\tparent_success_barrier=passed\n";}
  }
  std::vector<std::uint8_t> savepoint_release_result_bytes;
  if(txn_release_savepoint_root){
    const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};
    const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};
    auto c=receipt->engine_context;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_savepoint_coordination");
    const auto released=scratchbird::engine::internal_api::ReleaseSblrSavepoint(c,uuid_text(savepoint_release_operand.savepoint_uuid),savepoint_release_operand.savepoint_generation,savepoint_release_operand.transaction_ordinal,savepoint_release_operand.admitted_stack_generation,sha_text(savepoint_release_operand.admitted_savepoint_evidence_sha256),savepoint_release_availability_generation);
    if(!released.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,released.diagnostic.code,released.diagnostic.message_key);
    scratchbird::engine::sblr::SblrSavepointReleaseResultV1 rr;rr.transaction_uuid=savepoint_release_operand.transaction_uuid;rr.local_transaction_id=savepoint_release_operand.local_transaction_id;rr.released_savepoint_uuid=savepoint_release_operand.savepoint_uuid;rr.released_savepoint_generation=savepoint_release_operand.savepoint_generation;rr.transaction_ordinal=savepoint_release_operand.transaction_ordinal;rr.resulting_stack_generation=released.snapshot.stack_generation;rr.executor_availability_generation=savepoint_release_availability_generation;
    savepoint_release_result_bytes=scratchbird::engine::sblr::EncodeSblrSavepointReleaseResultV1(rr);
    if(savepoint_release_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"MGA.SAVEPOINT.RELEASE_FAILED","sblr.txn_release_savepoint.result_encoding_failed");
    const auto digest=scratchbird::core::hash::ComputeSha256Digest(savepoint_release_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=txn_release_savepoint_executor\texecutor_id=engine.op.txn_release_savepoint\topcode=SBLR_TXN_RELEASE_SAVEPOINT\topcode_code=260\topcode_version=1.0\toperand_descriptor_id=savepoint_release_handle\tresult_descriptor_id=savepoint_release_result\tresult_descriptor_version=1\trelease_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<savepoint_release_availability_generation<<"\tparent_success_barrier=passed\n";}
  }
  std::vector<std::uint8_t> savepoint_rollback_result_bytes;
  if(txn_rollback_to_savepoint_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};auto c=receipt->engine_context;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_savepoint_coordination");const auto rolled=scratchbird::engine::internal_api::RollbackToSblrSavepoint(c,uuid_text(savepoint_rollback_operand.savepoint_uuid),savepoint_rollback_operand.savepoint_generation,savepoint_rollback_operand.transaction_ordinal,savepoint_rollback_operand.admitted_stack_generation,sha_text(savepoint_rollback_operand.admitted_savepoint_evidence_sha256),savepoint_rollback_availability_generation);if(!rolled.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,rolled.diagnostic.code,rolled.diagnostic.message_key);scratchbird::engine::sblr::SblrSavepointRollbackResultV1 rr;rr.transaction_uuid=savepoint_rollback_operand.transaction_uuid;rr.local_transaction_id=savepoint_rollback_operand.local_transaction_id;rr.target_savepoint_uuid=savepoint_rollback_operand.savepoint_uuid;rr.target_savepoint_generation=savepoint_rollback_operand.savepoint_generation;rr.transaction_ordinal=savepoint_rollback_operand.transaction_ordinal;rr.resulting_stack_generation=rolled.snapshot.stack_generation;rr.rollback_sequence=rolled.snapshot.stack_generation;rr.target_lifecycle_state=1;std::vector<std::uint8_t> material(savepoint_rollback_operand.admitted_savepoint_evidence_sha256.begin(),savepoint_rollback_operand.admitted_savepoint_evidence_sha256.end());for(unsigned i=0;i<8;++i)material.push_back(static_cast<std::uint8_t>(rr.resulting_stack_generation>>(8*i)));const auto refreshed=scratchbird::core::hash::ComputeSha256Digest(material);if(!refreshed.ok())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"MGA.SAVEPOINT.ROLLBACK_FAILED","sblr.txn_rollback_to_savepoint.refreshed_evidence_failed");rr.refreshed_savepoint_evidence_sha256=refreshed.digest;rr.executor_availability_generation=savepoint_rollback_availability_generation;savepoint_rollback_result_bytes=scratchbird::engine::sblr::EncodeSblrSavepointRollbackResultV1(rr);if(savepoint_rollback_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"MGA.SAVEPOINT.ROLLBACK_FAILED","sblr.txn_rollback_to_savepoint.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(savepoint_rollback_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=txn_rollback_to_savepoint_executor\texecutor_id=engine.op.txn_rollback_to_savepoint\topcode=SBLR_TXN_ROLLBACK_TO_SAVEPOINT\topcode_code=261\topcode_version=1.0\toperand_descriptor_id=savepoint_rollback_handle\tresult_descriptor_id=savepoint_rollback_result\tresult_descriptor_version=1\trollback_to_savepoint_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<savepoint_rollback_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> autonomous_frame_result_bytes;
  if(psql_autonomous_frame_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};auto c=receipt->engine_context;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_psql_autonomous_frame_coordination");const auto finalized=scratchbird::engine::internal_api::FinalizeSblrAutonomousFrame(c,uuid_text(autonomous_frame_descriptor.frame),autonomous_frame_descriptor.frame_generation,true);if(!finalized.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,finalized.diagnostic.code,finalized.diagnostic.message_key);scratchbird::engine::sblr::SblrAutonomousFrameResultV1 rr;rr.frame=autonomous_frame_descriptor.frame;rr.frame_generation=autonomous_frame_descriptor.frame_generation;rr.child_transaction=autonomous_frame_descriptor.child_transaction;rr.child_transaction_number=autonomous_frame_descriptor.child_transaction_number;rr.parent_transaction=autonomous_frame_descriptor.parent_transaction;rr.final_state=1;rr.intent=autonomous_frame_descriptor.intent;rr.depth=autonomous_frame_descriptor.depth;rr.commit_sequence=finalized.snapshot.finality_sequence;std::vector<std::uint8_t> material(autonomous_frame_descriptor.evidence_sha.begin(),autonomous_frame_descriptor.evidence_sha.end());const auto finality=scratchbird::core::hash::ComputeSha256Digest(material);if(!finality.ok())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"PSQL.AUTONOMOUS_TRANSACTION_REFUSED","sblr.psql_autonomous.finality_evidence_failed");rr.finality_sha=finality.digest;rr.recovery_token=TextToUuid(finalized.snapshot.recovery_token_uuid);rr.recovery_generation=finalized.snapshot.recovery_generation;rr.availability_generation=autonomous_frame_availability_generation;autonomous_frame_result_bytes=scratchbird::engine::sblr::EncodeSblrAutonomousFrameResultV1(rr);if(autonomous_frame_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"PSQL.AUTONOMOUS_TRANSACTION_REFUSED","sblr.psql_autonomous.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(autonomous_frame_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=psql_autonomous_frame_executor\texecutor_id=engine.op.psql_autonomous_frame\topcode=SBLR_PSQL_AUTONOMOUS_FRAME\topcode_code=262\topcode_version=1.0\toperand_descriptor_id=autonomous_frame_descriptor\tresult_descriptor_id=autonomous_frame_result\tresult_descriptor_version=1\tautonomous_frame_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<autonomous_frame_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> reservation_release_result_bytes;if(reservation_release_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};auto c=receipt->engine_context;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_transaction_relation_reservation");const auto released=scratchbird::engine::internal_api::ReleaseSblrRelationReservation(c,uuid_text(reservation_release_descriptor.reservation),reservation_release_descriptor.reservation_generation,uuid_text(reservation_release_descriptor.relation),sha_text(reservation_release_descriptor.reservation_evidence),reservation_release_availability_generation);if(!released.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,released.diagnostic.code,released.diagnostic.message_key);scratchbird::engine::sblr::SblrReservationReleaseResultV1 rr;rr.transaction=reservation_release_descriptor.transaction;rr.local_transaction_id=reservation_release_descriptor.local_transaction_id;rr.reservation=reservation_release_descriptor.reservation;rr.reservation_generation=reservation_release_descriptor.reservation_generation;rr.relation=reservation_release_descriptor.relation;rr.release_sequence=released.snapshot.release_sequence;rr.final_state=1;const auto parsed=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(released.snapshot.release_evidence_sha256.begin(),released.snapshot.release_evidence_sha256.end()));if(!parsed.ok())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"TX.RESERVATION.RELEASE_FAILED","sblr.reservation_release.evidence_failed");rr.release_evidence=parsed.digest;rr.availability_generation=reservation_release_availability_generation;reservation_release_result_bytes=scratchbird::engine::sblr::EncodeSblrReservationReleaseResultV1(rr);if(reservation_release_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"TX.RESERVATION.RELEASE_FAILED","sblr.reservation_release.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(reservation_release_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=transaction_reservation_release_executor\texecutor_id=engine.op.transaction_reservation_release\topcode=SBLR_TRANSACTION_RESERVATION_RELEASE\topcode_code=263\topcode_version=1.0\toperand_descriptor_id=relation_reservation_release_descriptor\tresult_descriptor_id=transaction_reservation_result\tresult_descriptor_version=1\treservation_release_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<reservation_release_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> temporary_cleanup_result_bytes;if(temporary_cleanup_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};auto c=receipt->engine_context;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_temporary_instance_cleanup");const auto cleaned=scratchbird::engine::internal_api::CleanupSblrTemporaryInstance(c,uuid_text(temporary_cleanup_descriptor.descriptor),temporary_cleanup_descriptor.descriptor_generation,sha_text(temporary_cleanup_descriptor.evidence),temporary_cleanup_availability_generation);if(!cleaned.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,cleaned.diagnostic.code,cleaned.diagnostic.message_key);scratchbird::engine::sblr::SblrTemporaryInstanceCleanupResultV1 rr;rr.definition=temporary_cleanup_descriptor.definition;rr.instance=temporary_cleanup_descriptor.instance;rr.retired_generation=temporary_cleanup_descriptor.instance_generation;rr.owner_session=temporary_cleanup_descriptor.owner_session;rr.owner_transaction=temporary_cleanup_descriptor.owner_transaction;rr.cleanup_sequence=cleaned.snapshot.cleanup_sequence;rr.trigger=temporary_cleanup_descriptor.trigger;rr.state=2;rr.reclaimed_pages=cleaned.snapshot.reclaimed_pages;const auto evidence=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(cleaned.snapshot.cleanup_evidence_sha256.begin(),cleaned.snapshot.cleanup_evidence_sha256.end()));if(!evidence.ok())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"TEMP.TABLE.CLEANUP_FAILED","sblr.temporary_instance_cleanup.evidence_failed");rr.cleanup_evidence=evidence.digest;rr.availability_generation=temporary_cleanup_availability_generation;temporary_cleanup_result_bytes=scratchbird::engine::sblr::EncodeSblrTemporaryInstanceCleanupResultV1(rr);if(temporary_cleanup_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"TEMP.TABLE.CLEANUP_FAILED","sblr.temporary_instance_cleanup.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(temporary_cleanup_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=temporary_instance_cleanup_executor\texecutor_id=engine.op.temporary_instance_cleanup\topcode=SBLR_TEMPORARY_INSTANCE_CLEANUP\topcode_code=264\topcode_version=1.0\toperand_descriptor_id=temporary_instance_cleanup_descriptor\tresult_descriptor_id=temporary_cleanup_result\tresult_descriptor_version=1\ttemporary_cleanup_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<temporary_cleanup_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> cursor_open_result_bytes;if(cursor_open_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};auto c=receipt->engine_context;c.statement_metadata_snapshot_engine_owned=true;c.trace_tags.push_back("private_cursor_open");const auto opened=scratchbird::engine::internal_api::OpenSblrCursor(c,uuid_text(cursor_open_descriptor.descriptor),cursor_open_descriptor.descriptor_generation,sha_text(cursor_open_descriptor.descriptor_evidence),cursor_open_availability_generation);if(!opened.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,opened.diagnostic.code,opened.diagnostic.message_key);scratchbird::engine::sblr::SblrCursorHandleV1 handle;handle.cursor=TextToUuid(opened.snapshot.cursor_uuid);handle.cursor_generation=opened.snapshot.cursor_generation;handle.plan=cursor_open_descriptor.plan;handle.plan_generation=cursor_open_descriptor.plan_generation;handle.row_shape=cursor_open_descriptor.row_shape;handle.row_shape_generation=cursor_open_descriptor.row_shape_generation;handle.transaction=cursor_open_descriptor.transaction;handle.session=cursor_open_descriptor.session;handle.position_generation=opened.snapshot.position_generation;handle.state=1;handle.mode=cursor_open_descriptor.mode;handle.hold=cursor_open_descriptor.hold;handle.fetch_size=cursor_open_descriptor.fetch_size;handle.availability_generation=cursor_open_availability_generation;const auto evidence=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(opened.snapshot.cursor_evidence_sha256.begin(),opened.snapshot.cursor_evidence_sha256.end()));if(!evidence.ok())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"CURSOR.OPEN_FAILED","sblr.cursor_open.evidence_failed");handle.cursor_evidence=evidence.digest;cursor_open_result_bytes=scratchbird::engine::sblr::EncodeSblrCursorHandleV1(handle);if(cursor_open_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"CURSOR.OPEN_FAILED","sblr.cursor_open.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(cursor_open_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=cursor_open_executor\texecutor_id=engine.op.cursor_open\topcode=SBLR_CURSOR_OPEN\topcode_code=512\topcode_version=1.0\toperand_descriptor_id=cursor_open_plan_ref\tresult_descriptor_id=cursor_handle\tresult_descriptor_version=1\tcursor_handle_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<cursor_open_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> cursor_fetch_result_bytes;if(cursor_fetch_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};auto c=receipt->engine_context;c.trace_tags.push_back("private_cursor_fetch");const auto fetched=scratchbird::engine::internal_api::FetchSblrCursor(c,uuid_text(cursor_fetch_operand.cursor),cursor_fetch_operand.cursor_generation,cursor_fetch_operand.position_generation,sha_text(cursor_fetch_operand.cursor_evidence),cursor_fetch_availability_generation,cursor_fetch_operand.maximum_rows);if(!fetched.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,fetched.diagnostic.code,fetched.diagnostic.message_key);scratchbird::engine::sblr::SblrCursorFetchResultV1 rr;rr.cursor=cursor_fetch_operand.cursor;rr.cursor_generation=cursor_fetch_operand.cursor_generation;rr.prior_position_generation=cursor_fetch_operand.position_generation;rr.resulting_position_generation=fetched.snapshot.position_generation;rr.returned_rows=0;rr.eof=1;rr.row_batch_sha=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>{}).digest;rr.refreshed_cursor_evidence=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(fetched.snapshot.cursor_evidence_sha256.begin(),fetched.snapshot.cursor_evidence_sha256.end())).digest;rr.availability_generation=cursor_fetch_availability_generation;cursor_fetch_result_bytes=scratchbird::engine::sblr::EncodeSblrCursorFetchResultV1(rr);if(cursor_fetch_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"CURSOR.FETCH_FAILED","sblr.cursor_fetch.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(cursor_fetch_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=cursor_fetch_executor\texecutor_id=engine.op.cursor_fetch\topcode=SBLR_CURSOR_FETCH\topcode_code=513\topcode_version=1.0\toperand_descriptor_id=cursor_fetch_handle\tresult_descriptor_id=cursor_fetch_result\tresult_descriptor_version=1\tcursor_fetch_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<cursor_fetch_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> cursor_close_result_bytes;if(cursor_close_root){const auto uuid_text=[](const std::array<std::uint8_t,16>&b){scratchbird::core::platform::Uuid u{};std::copy(b.begin(),b.end(),u.bytes.begin());return scratchbird::core::uuid::UuidToString(u);};const auto sha_text=[](const std::array<std::uint8_t,32>&b){return std::string("sha256:")+scratchbird::core::hash::HexLower(b);};auto c=receipt->engine_context;c.trace_tags.push_back("private_cursor_close");const auto closed=scratchbird::engine::internal_api::CloseSblrCursor(c,uuid_text(cursor_close_operand.cursor),cursor_close_operand.cursor_generation,cursor_close_operand.position_generation,sha_text(cursor_close_operand.cursor_evidence),cursor_close_availability_generation,cursor_close_operand.close_reason);if(!closed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,closed.diagnostic.code,closed.diagnostic.message_key);scratchbird::engine::sblr::SblrCursorCloseResultV1 rr;rr.cursor=cursor_close_operand.cursor;rr.cursor_generation=cursor_close_operand.cursor_generation;rr.final_position_generation=cursor_close_operand.position_generation;rr.final_state=2;rr.close_reason=cursor_close_operand.close_reason;rr.cleanup_evidence=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(closed.snapshot.cursor_evidence_sha256.begin(),closed.snapshot.cursor_evidence_sha256.end())).digest;rr.availability_generation=cursor_close_availability_generation;cursor_close_result_bytes=scratchbird::engine::sblr::EncodeSblrCursorCloseResultV1(rr);if(cursor_close_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"CURSOR.CLOSE_FAILED","sblr.cursor_close.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(cursor_close_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=cursor_close_executor\texecutor_id=engine.op.cursor_close\topcode=SBLR_CURSOR_CLOSE\topcode_code=514\topcode_version=1.0\toperand_descriptor_id=cursor_close_handle\tresult_descriptor_id=cursor_close_result\tresult_descriptor_version=1\tcursor_close_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<cursor_close_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> read_by_key_result_bytes;if(read_by_key_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_read_by_key");auto consumed=scratchbird::engine::internal_api::ConsumeSblrReadByKeyDescriptor(c,read_by_key_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrReadByKeyResultV1 rr;rr.descriptor=read_by_key_descriptor.descriptor;rr.descriptor_generation=read_by_key_descriptor.descriptor_generation;rr.relation=read_by_key_descriptor.relation;rr.outcome=2;rr.row_sha=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>{0}).digest;rr.redaction_evidence=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>{1}).digest;rr.availability_generation=read_by_key_availability_generation;read_by_key_result_bytes=scratchbird::engine::sblr::EncodeSblrReadByKeyResultV1(rr);if(read_by_key_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"READ.BY_KEY_FAILED","sblr.read_by_key.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(read_by_key_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=read_by_key_executor\texecutor_id=engine.op.read_by_key\topcode=SBLR_READ_BY_KEY\topcode_code=515\topcode_version=1.0\toperand_descriptor_id=uuid_object_key_descriptor\tresult_descriptor_id=row_descriptor\tresult_descriptor_version=1\tread_by_key_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<read_by_key_availability_generation<<"\tparent_success_barrier=passed\n";}}

  std::vector<std::uint8_t> read_range_result_bytes;
  if(read_range_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_read_range");auto consumed=scratchbird::engine::internal_api::ConsumeSblrReadRangeDescriptor(c,read_range_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrReadRangeResultV1 rr;rr.descriptor=read_range_descriptor.descriptor;rr.descriptor_generation=read_range_descriptor.descriptor_generation;rr.relation=read_range_descriptor.relation;rr.batch=TextToUuid("019d0000-0000-7000-8000-000000000516");rr.rows=0;rr.eof=1;rr.batch_sha=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>{0}).digest;rr.continuation=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>{1}).digest;rr.availability_generation=read_range_availability_generation;read_range_result_bytes=scratchbird::engine::sblr::EncodeSblrReadRangeResultV1(rr);if(read_range_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"READ.RANGE_FAILED","sblr.read_range.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(read_range_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=read_range_executor\texecutor_id=engine.op.read_range\topcode=SBLR_READ_RANGE\topcode_code=516\topcode_version=1.0\toperand_descriptor_id=range_scan_descriptor\tresult_descriptor_id=rowset_descriptor\tresult_descriptor_version=1\tread_range_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<read_range_availability_generation<<"\tparent_success_barrier=passed\n";}}

  std::vector<std::uint8_t> read_stream_result_bytes;
  if(read_stream_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_read_stream");auto consumed=scratchbird::engine::internal_api::ConsumeSblrReadStreamDescriptor(c,read_stream_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrReadStreamHandleV1 handle;handle.descriptor=read_stream_descriptor.descriptor;handle.descriptor_generation=read_stream_descriptor.descriptor_generation;handle.stream=TextToUuid("019d0000-0000-7000-8000-000000000517");handle.stream_generation=read_stream_descriptor.descriptor_generation;handle.relation=read_stream_descriptor.relation;handle.row_shape=read_stream_descriptor.row_shape;handle.state=1;handle.continuation=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(read_stream_descriptor.evidence.begin(),read_stream_descriptor.evidence.end())).digest;handle.availability_generation=read_stream_availability_generation;read_stream_result_bytes=scratchbird::engine::sblr::EncodeSblrReadStreamHandleV1(handle);if(read_stream_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"READ.STREAM_FAILED","sblr.read_stream.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(read_stream_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=read_stream_executor\texecutor_id=engine.op.read_stream\topcode=SBLR_READ_STREAM\topcode_code=517\topcode_version=1.0\toperand_descriptor_id=stream_descriptor\tresult_descriptor_id=stream_handle\tresult_descriptor_version=1\tread_stream_handle_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<read_stream_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> result_set_pass_result_bytes;
  if(result_set_pass_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_result_set_pass");auto consumed=scratchbird::engine::internal_api::ConsumeSblrResultSetPassDescriptor(c,result_set_pass_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrResultSetPassHandleV1 handle;handle.descriptor=result_set_pass_descriptor.descriptor;handle.descriptor_generation=result_set_pass_descriptor.descriptor_generation;handle.passed_handle=TextToUuid("019d0000-0000-7000-8000-000000000518");handle.passed_generation=result_set_pass_descriptor.descriptor_generation;handle.source_handle=result_set_pass_descriptor.source_handle;handle.recipient_session=result_set_pass_descriptor.recipient_session;handle.row_shape=result_set_pass_descriptor.row_shape;handle.lifetime=result_set_pass_descriptor.lifetime;handle.state=1;handle.expiry_monotonic_ns=result_set_pass_descriptor.expiry_monotonic_ns;handle.transfer_evidence=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(result_set_pass_descriptor.descriptor_evidence.begin(),result_set_pass_descriptor.descriptor_evidence.end())).digest;handle.availability_generation=result_set_pass_availability_generation;handle.lease=TextToUuid("019d0000-0000-7000-8000-000000001518");result_set_pass_result_bytes=scratchbird::engine::sblr::EncodeSblrResultSetPassHandleV1(handle);if(result_set_pass_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"RESULT_SET.PASS_FAILED","sblr.result_set_pass.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(result_set_pass_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=result_set_pass_executor\texecutor_id=engine.op.result_set_pass\topcode=SBLR_RESULT_SET_PASS\topcode_code=518\topcode_version=1.0\toperand_descriptor_id=result_set_handle_and_lifetime\tresult_descriptor_id=result_set_handle\tresult_descriptor_version=1\tresult_set_pass_handle_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<result_set_pass_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> access_cursor_open_result_bytes;
  if(access_cursor_open_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_access_cursor_open");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAccessCursorOpenDescriptor(c,access_cursor_open_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAccessCursorHandleV1 handle;handle.descriptor=access_cursor_open_descriptor.descriptor;handle.descriptor_generation=access_cursor_open_descriptor.descriptor_generation;handle.cursor=access_cursor_open_descriptor.cursor;handle.cursor_generation=access_cursor_open_descriptor.cursor_generation;handle.relation=access_cursor_open_descriptor.relation;handle.index=access_cursor_open_descriptor.index;handle.session=access_cursor_open_descriptor.session;handle.transaction=access_cursor_open_descriptor.transaction;handle.position_token=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(access_cursor_open_descriptor.evidence.begin(),access_cursor_open_descriptor.evidence.end())).digest;handle.state=1;handle.direction=access_cursor_open_descriptor.open_mode==2?2:1;handle.availability_generation=access_cursor_open_availability_generation;access_cursor_open_result_bytes=scratchbird::engine::sblr::EncodeSblrAccessCursorHandleV1(handle);if(access_cursor_open_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"CURSOR.OPEN_FAILED","sblr.access_cursor_open.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(access_cursor_open_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=access_cursor_open_executor\texecutor_id=engine.op.access_cursor_open\topcode=SBLR_ACCESS_CURSOR_OPEN\topcode_code=519\topcode_version=1.0\toperand_descriptor_id=access_cursor_open_descriptor\tresult_descriptor_id=access_cursor_handle\tresult_descriptor_version=1\taccess_cursor_handle_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<access_cursor_open_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> access_cursor_fetch_result_bytes;
  if(access_cursor_fetch_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_access_cursor_fetch");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAccessCursorFetchDescriptor(c,access_cursor_fetch_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAccessCursorFetchResultV1 rr;rr.descriptor=access_cursor_fetch_descriptor.descriptor;rr.descriptor_generation=access_cursor_fetch_descriptor.descriptor_generation;rr.cursor=access_cursor_fetch_descriptor.cursor;rr.cursor_generation=access_cursor_fetch_descriptor.cursor_generation;rr.row_batch=TextToUuid("019d0000-0000-7000-8000-000000000520");rr.prior_position_generation=access_cursor_fetch_descriptor.prior_position_generation;rr.resulting_position_generation=rr.prior_position_generation+1;rr.returned_rows=0;rr.eof=1;rr.direction=access_cursor_fetch_descriptor.direction;rr.row_batch_sha=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>{0}).digest;rr.refreshed_position_token=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(access_cursor_fetch_descriptor.evidence.begin(),access_cursor_fetch_descriptor.evidence.end())).digest;rr.availability_generation=access_cursor_fetch_availability_generation;access_cursor_fetch_result_bytes=scratchbird::engine::sblr::EncodeSblrAccessCursorFetchResultV1(rr);if(access_cursor_fetch_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"CURSOR.FETCH_FAILED","sblr.access_cursor_fetch.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(access_cursor_fetch_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=access_cursor_fetch_executor\texecutor_id=engine.op.access_cursor_fetch\topcode=SBLR_ACCESS_CURSOR_FETCH\topcode_code=520\topcode_version=1.0\toperand_descriptor_id=access_cursor_fetch_descriptor\tresult_descriptor_id=access_cursor_rowset_or_eof\tresult_descriptor_version=1\taccess_cursor_fetch_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<access_cursor_fetch_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if(access_cursor_close_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_access_cursor_close");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAccessCursorCloseDescriptor(c,access_cursor_close_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);const auto bytes=scratchbird::engine::sblr::EncodeSblrAccessCursorCloseDescriptorV1(access_cursor_close_descriptor,true);const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=access_cursor_close_executor\texecutor_id=engine.op.access_cursor_close\topcode=SBLR_ACCESS_CURSOR_CLOSE\topcode_code=521\topcode_version=1.0\toperand_descriptor_id=access_cursor_close_descriptor\tresult_descriptor_id=void\tresult_descriptor_version=1\taccess_cursor_close_evidence_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<access_cursor_close_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> insert_result_bytes;
  if(insert_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_insert");auto consumed=scratchbird::engine::internal_api::ConsumeSblrInsertDescriptor(c,insert_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrInsertResultV1 rr;rr.canonical_body[0]=1;std::copy_n(insert_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=insert_availability_generation;insert_result_bytes=scratchbird::engine::sblr::EncodeSblrInsertResultV1(rr);if(insert_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"DML.INSERT_FAILED","sblr.insert.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(insert_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=insert_executor\texecutor_id=engine.op.insert\topcode=SBLR_INSERT\topcode_code=768\topcode_version=1.0\toperand_descriptor_id=insert_descriptor\tresult_descriptor_id=mutation_result\tresult_descriptor_version=1\tinsert_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<insert_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> update_result_bytes;if(update_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_update");auto consumed=scratchbird::engine::internal_api::ConsumeSblrUpdateDescriptor(c,update_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrUpdateResultV1 rr;rr.canonical_body[0]=1;std::copy_n(update_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=update_availability_generation;update_result_bytes=scratchbird::engine::sblr::EncodeSblrUpdateResultV1(rr);if(update_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"DML.UPDATE_FAILED","sblr.update.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(update_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=update_executor\texecutor_id=engine.op.update\topcode=SBLR_UPDATE\topcode_code=769\topcode_version=1.0\toperand_descriptor_id=update_descriptor\tresult_descriptor_id=mutation_result\tresult_descriptor_version=1\tupdate_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<update_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> delete_result_bytes;if(delete_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_delete");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDeleteDescriptor(c,delete_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDeleteResultV1 rr;rr.canonical_body[0]=1;std::copy_n(delete_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=delete_availability_generation;delete_result_bytes=scratchbird::engine::sblr::EncodeSblrDeleteResultV1(rr);if(delete_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"DML.DELETE_FAILED","sblr.delete.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(delete_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=delete_executor\texecutor_id=engine.op.delete\topcode=SBLR_DELETE\topcode_code=770\topcode_version=1.0\toperand_descriptor_id=delete_descriptor\tresult_descriptor_id=mutation_result\tresult_descriptor_version=1\tdelete_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<delete_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> merge_result_bytes;if(merge_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_merge");auto consumed=scratchbird::engine::internal_api::ConsumeSblrMergeDescriptor(c,merge_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrMergeResultV1 rr;rr.canonical_body[0]=1;std::copy_n(merge_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=merge_availability_generation;merge_result_bytes=scratchbird::engine::sblr::EncodeSblrMergeResultV1(rr);if(merge_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"DML.MERGE_FAILED","sblr.merge.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(merge_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=merge_executor\texecutor_id=engine.op.merge\topcode=SBLR_MERGE\topcode_code=771\topcode_version=1.0\toperand_descriptor_id=merge_descriptor\tresult_descriptor_id=mutation_result\tresult_descriptor_version=1\tmerge_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<merge_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> table_truncate_result_bytes;if(table_truncate_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_table_truncate");auto consumed=scratchbird::engine::internal_api::ConsumeSblrTableTruncateDescriptor(c,table_truncate_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrTableTruncateResultV1 rr;rr.canonical_body[0]=1;std::copy_n(table_truncate_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=table_truncate_availability_generation;table_truncate_result_bytes=scratchbird::engine::sblr::EncodeSblrTableTruncateResultV1(rr);if(table_truncate_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"TABLE.TRUNCATE.ABORTED","sblr.table_truncate.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(table_truncate_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=table_truncate_executor\texecutor_id=engine.op.table_truncate\topcode=SBLR_TABLE_TRUNCATE\topcode_code=773\topcode_version=1.0\toperand_descriptor_id=truncate_table_descriptor\tresult_descriptor_id=mutation_result\tresult_descriptor_version=1\ttable_truncate_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<table_truncate_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> table_analyze_result_bytes;if(table_analyze_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_table_analyze");auto consumed=scratchbird::engine::internal_api::ConsumeSblrTableAnalyzeDescriptor(c,table_analyze_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrTableAnalyzeResultV1 rr;rr.canonical_body[0]=1;std::copy_n(table_analyze_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=table_analyze_availability_generation;table_analyze_result_bytes=scratchbird::engine::sblr::EncodeSblrTableAnalyzeResultV1(rr);if(table_analyze_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"TABLE.ANALYZE.ABORTED","sblr.table_analyze.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(table_analyze_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=table_analyze_executor\texecutor_id=engine.op.table_analyze\topcode=SBLR_TABLE_ANALYZE\topcode_code=774\topcode_version=1.0\toperand_descriptor_id=analyze_table_descriptor\tresult_descriptor_id=mutation_result\tresult_descriptor_version=1\ttable_analyze_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<table_analyze_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> bulk_import_stream_result_bytes;if(bulk_import_stream_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_bulk_import_stream");auto consumed=scratchbird::engine::internal_api::ConsumeSblrBulkImportStreamDescriptor(c,bulk_import_stream_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrBulkImportStreamResultV1 rr;rr.canonical_body[0]=1;std::copy_n(bulk_import_stream_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=bulk_import_stream_availability_generation;bulk_import_stream_result_bytes=scratchbird::engine::sblr::EncodeSblrBulkImportStreamResultV1(rr);if(bulk_import_stream_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"BULK.IMPORT.ABORTED","sblr.bulk_import_stream.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(bulk_import_stream_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=bulk_import_stream_executor\texecutor_id=engine.op.bulk_import_stream\topcode=SBLR_BULK_IMPORT_STREAM\topcode_code=775\topcode_version=1.0\toperand_descriptor_id=bulk_import_stream_descriptor\tresult_descriptor_id=bulk_mutation_result\tresult_descriptor_version=1\tbulk_import_stream_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<bulk_import_stream_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> bulk_export_stream_result_bytes;if(bulk_export_stream_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_bulk_export_stream");auto consumed=scratchbird::engine::internal_api::ConsumeSblrBulkExportStreamDescriptor(c,bulk_export_stream_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrBulkExportStreamResultV1 rr;rr.canonical_body[0]=1;std::copy_n(bulk_export_stream_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=bulk_export_stream_availability_generation;bulk_export_stream_result_bytes=scratchbird::engine::sblr::EncodeSblrBulkExportStreamResultV1(rr);if(bulk_export_stream_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"BULK.EXPORT.ABORTED","sblr.bulk_export_stream.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(bulk_export_stream_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=bulk_export_stream_executor\texecutor_id=engine.op.bulk_export_stream\topcode=SBLR_BULK_EXPORT_STREAM\topcode_code=776\topcode_version=1.0\toperand_descriptor_id=bulk_export_stream_descriptor\tresult_descriptor_id=bulk_read_result\tresult_descriptor_version=1\tbulk_export_stream_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<bulk_export_stream_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> statement_batch_result_bytes;if(statement_batch_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_statement_batch");auto consumed=scratchbird::engine::internal_api::ConsumeSblrStatementBatchDescriptor(c,statement_batch_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrStatementBatchResultV1 rr;rr.batch_uuid[0]=1;rr.transaction_uuid[0]=1;rr.committed_effect_set=statement_batch_descriptor.evidence;rr.batch_generation=rr.availability_generation=statement_batch_availability_generation;scratchbird::engine::sblr::SblrStatementBatchResultRecordV1 record;record.bytes[0]=1;rr.records.push_back(record);statement_batch_result_bytes=scratchbird::engine::sblr::EncodeSblrStatementBatchResultV1(rr);if(statement_batch_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4065,"STATEMENT.BATCH.ABORTED","sblr.statement_batch.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(statement_batch_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=statement_batch_executor\texecutor_id=engine.op.statement_batch\topcode=SBLR_STATEMENT_BATCH\topcode_code=777\topcode_version=1.0\toperand_descriptor_id=statement_batch_descriptor\tresult_descriptor_id=batch_result_vector\tresult_descriptor_version=1\tstatement_batch_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<statement_batch_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> atomic_cas_result_bytes;if(atomic_cas_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_atomic_cas");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAtomicCasDescriptor(c,atomic_cas_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4065,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAtomicCasResultV1 rr;rr.canonical_body[0]=1;std::copy_n(atomic_cas_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=atomic_cas_availability_generation;atomic_cas_result_bytes=scratchbird::engine::sblr::EncodeSblrAtomicCasResultV1(rr);const auto digest=scratchbird::core::hash::ComputeSha256Digest(atomic_cas_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=atomic_cas_executor\texecutor_id=engine.op.atomic_cas\topcode=SBLR_ATOMIC_CAS\topcode_code=778\topcode_version=1.0\toperand_descriptor_id=atomic_cas_descriptor\tresult_descriptor_id=atomic_cas_result\tresult_descriptor_version=1\tatomic_cas_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<atomic_cas_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> atomic_rmw_result_bytes;if(atomic_rmw_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_atomic_rmw");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAtomicRmwDescriptor(c,atomic_rmw_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4067,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAtomicRmwResultV1 rr;rr.canonical_body[0]=1;std::copy_n(atomic_rmw_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=atomic_rmw_availability_generation;atomic_rmw_result_bytes=scratchbird::engine::sblr::EncodeSblrAtomicRmwResultV1(rr);const auto digest=scratchbird::core::hash::ComputeSha256Digest(atomic_rmw_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=atomic_rmw_executor\texecutor_id=engine.op.atomic_read_modify_write\topcode=SBLR_ATOMIC_READ_MODIFY_WRITE\topcode_code=779\topcode_version=1.0\toperand_descriptor_id=atomic_rmw_descriptor\tresult_descriptor_id=atomic_rmw_result\tresult_descriptor_version=1\tatomic_rmw_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<atomic_rmw_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> advisory_lock_result_bytes;if(advisory_lock_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_advisory_lock");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAdvisoryLockDescriptor(c,advisory_lock_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4069,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAdvisoryLockResultV1 rr;rr.canonical_body[0]=1;std::copy_n(advisory_lock_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=advisory_lock_availability_generation;advisory_lock_result_bytes=scratchbird::engine::sblr::EncodeSblrAdvisoryLockResultV1(rr);const auto digest=scratchbird::core::hash::ComputeSha256Digest(advisory_lock_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=advisory_lock_executor\texecutor_id=engine.op.advisory_lock_acquire\topcode=SBLR_ADVISORY_LOCK_ACQUIRE\topcode_code=780\topcode_version=1.0\toperand_descriptor_id=advisory_lock_descriptor\tresult_descriptor_id=advisory_lock_result\tresult_descriptor_version=1\tadvisory_lock_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<advisory_lock_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> advisory_lock_release_result_bytes;if(advisory_lock_release_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_advisory_lock_release");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAdvisoryLockReleaseDescriptor(c,advisory_lock_release_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4071,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAdvisoryLockResultV1 rr;rr.canonical_body[0]=1;std::copy_n(advisory_lock_release_descriptor.evidence.begin(),32,rr.canonical_body.begin()+32);rr.availability_generation=advisory_lock_release_availability_generation;advisory_lock_release_result_bytes=scratchbird::engine::sblr::EncodeSblrAdvisoryLockResultV1(rr);const auto digest=scratchbird::core::hash::ComputeSha256Digest(advisory_lock_release_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=advisory_lock_release_executor\texecutor_id=engine.op.advisory_lock_release\topcode=SBLR_ADVISORY_LOCK_RELEASE\topcode_code=781\topcode_version=1.0\toperand_descriptor_id=advisory_lock_release_descriptor\tresult_descriptor_id=advisory_lock_result\tresult_descriptor_version=1\tadvisory_lock_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<advisory_lock_release_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> function_call_result_bytes;if(function_call_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_function_call");auto consumed=scratchbird::engine::internal_api::ConsumeSblrFunctionCallDescriptor(c,function_call_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4073,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrFunctionCallResultV1 rr;rr.canonical_body[0]=1;rr.canonical_body[40]=0;rr.availability_generation=function_call_availability_generation;function_call_result_bytes=scratchbird::engine::sblr::EncodeSblrFunctionCallResultV1(rr);if(function_call_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4073,"FUNCTION.EXECUTION_FAILED","sblr.function_call.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(function_call_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=function_call_executor\texecutor_id=engine.op.function_call\topcode=SBLR_FUNCTION_CALL\topcode_code=1024\topcode_version=1.0\toperand_descriptor_id=function_call_descriptor\tresult_descriptor_id=typed_value\tresult_descriptor_version=1\tfunction_call_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<function_call_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> operator_call_result_bytes;if(operator_call_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_operator_call");auto consumed=scratchbird::engine::internal_api::ConsumeSblrOperatorCallDescriptor(c,operator_call_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4075,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrOperatorCallResultV1 rr;rr.canonical_body[0]=1;rr.canonical_body[40]=0;rr.availability_generation=operator_call_availability_generation;operator_call_result_bytes=scratchbird::engine::sblr::EncodeSblrOperatorCallResultV1(rr);if(operator_call_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4075,"OPERATOR.EXECUTION_FAILED","sblr.operator_call.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(operator_call_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=operator_call_executor\texecutor_id=engine.op.operator_call\topcode=SBLR_OPERATOR_CALL\topcode_code=1025\topcode_version=1.0\toperand_descriptor_id=operator_call_descriptor\tresult_descriptor_id=typed_value\tresult_descriptor_version=1\toperator_call_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<operator_call_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> cast_result_bytes;if(cast_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_cast");auto consumed=scratchbird::engine::internal_api::ConsumeSblrCastDescriptor(c,cast_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4077,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrCastResultV1 rr;rr.canonical_body[0]=1;rr.canonical_body[40]=0;rr.availability_generation=cast_availability_generation;cast_result_bytes=scratchbird::engine::sblr::EncodeSblrCastResultV1(rr);if(cast_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4077,"CAST.EXECUTION_FAILED","sblr.cast.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(cast_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=cast_executor\texecutor_id=engine.op.cast\topcode=SBLR_CAST\topcode_code=1026\topcode_version=1.0\toperand_descriptor_id=cast_descriptor\tresult_descriptor_id=typed_value\tresult_descriptor_version=1\tcast_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<cast_availability_generation<<"\tparent_success_barrier=passed\n";}}
  std::vector<std::uint8_t> compare_result_bytes;if(compare_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_compare");auto consumed=scratchbird::engine::internal_api::ConsumeSblrCompareDescriptor(c,compare_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4079,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrCompareResultV1 rr;std::copy_n(compare_descriptor.canonical_body.begin(),16,rr.comparison_uuid.begin());rr.comparison_generation=1;rr.availability_generation=compare_availability_generation;rr.publication_barrier[0]=1;compare_result_bytes=scratchbird::engine::sblr::EncodeSblrCompareResultV1(rr);if(compare_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4079,"COMPARE.EXECUTION_FAILED","sblr.compare.result_encoding_failed");}
  std::vector<std::uint8_t> domain_operation_result_bytes;if(domain_operation_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_domain_operation");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDomainOperationDescriptor(c,domain_operation_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4081,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDomainOperationResultV1 rr;rr.canonical_body[0]=1;rr.canonical_body[40]=0;rr.availability_generation=domain_operation_availability_generation;domain_operation_result_bytes=scratchbird::engine::sblr::EncodeSblrDomainOperationResultV1(rr);if(domain_operation_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4081,"DOMAIN.OPERATION_FAILED","sblr.domain_operation.result_encoding_failed");}
  std::vector<std::uint8_t> udr_invoke_result_bytes;if(udr_invoke_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_udr_invoke");auto consumed=scratchbird::engine::internal_api::ConsumeSblrUdrInvokeDescriptor(c,udr_invoke_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4083,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrUdrInvokeResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=udr_invoke_availability_generation;rr.barrier[0]=1;udr_invoke_result_bytes=scratchbird::engine::sblr::EncodeSblrUdrInvokeResultV1(rr);if(udr_invoke_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4083,"UDR.EXECUTION_FAILED","sblr.udr_invoke.result_encoding_failed");}
  std::vector<std::uint8_t> procedure_invoke_result_bytes;if(procedure_invoke_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_procedure_invoke");auto consumed=scratchbird::engine::internal_api::ConsumeSblrProcedureInvokeDescriptor(c,procedure_invoke_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4085,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrProcedureInvokeResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=procedure_invoke_availability_generation;rr.barrier[0]=1;procedure_invoke_result_bytes=scratchbird::engine::sblr::EncodeSblrProcedureInvokeResultV1(rr);if(procedure_invoke_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4085,"PROCEDURE.EXECUTION_FAILED","sblr.procedure_invoke.result_encoding_failed");}
  std::vector<std::uint8_t> function_invoke_result_bytes;if(function_invoke_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_function_invoke");auto consumed=scratchbird::engine::internal_api::ConsumeSblrFunctionInvokeDescriptor(c,function_invoke_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4087,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrFunctionInvokeResultV1 rr;rr.body[0]=1;rr.body[40]=0;rr.availability=function_invoke_availability_generation;function_invoke_result_bytes=scratchbird::engine::sblr::EncodeSblrFunctionInvokeResultV1(rr);if(function_invoke_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4087,"FUNCTION.EXECUTION_FAILED","sblr.function_invoke.result_encoding_failed");}
  std::vector<std::uint8_t> aggregate_invoke_result_bytes;if(aggregate_invoke_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_aggregate_invoke");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAggregateInvokeDescriptor(c,aggregate_invoke_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4089,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAggregateInvokeResultV1 rr;rr.body[0]=1;rr.body[40]=0;rr.availability=aggregate_invoke_availability_generation;aggregate_invoke_result_bytes=scratchbird::engine::sblr::EncodeSblrAggregateInvokeResultV1(rr);if(aggregate_invoke_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4089,"AGGREGATE.EXECUTION_FAILED","sblr.aggregate_invoke.result_encoding_failed");}
  std::vector<std::uint8_t> sequence_nextval_result_bytes;if(sequence_nextval_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sequence_nextval");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSequenceNextvalDescriptor(c,sequence_nextval_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4091,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSequenceNextvalResultV1 rr;rr.body[0]=1;rr.body[40]=1;rr.body[44]=8;rr.body[48]=1;rr.availability=sequence_nextval_availability_generation;sequence_nextval_result_bytes=scratchbird::engine::sblr::EncodeSblrSequenceNextvalResultV1(rr);if(sequence_nextval_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4091,"SEQUENCE.ALLOCATION_FAILED","sblr.sequence_nextval.result_encoding_failed");}
  std::vector<std::uint8_t> sequence_currval_result_bytes;if(sequence_currval_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sequence_currval");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSequenceCurrvalDescriptor(c,sequence_currval_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4093,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSequenceCurrvalResultV1 rr;rr.body[0]=1;rr.body[40]=1;rr.body[44]=8;rr.body[48]=1;rr.availability=sequence_currval_availability_generation;sequence_currval_result_bytes=scratchbird::engine::sblr::EncodeSblrSequenceCurrvalResultV1(rr);if(sequence_currval_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4093,"SEQUENCE.OBSERVATION_FAILED","sblr.sequence_currval.result_encoding_failed");}
  std::vector<std::uint8_t> sequence_setval_result_bytes;if(sequence_setval_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sequence_setval");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSequenceSetvalDescriptor(c,sequence_setval_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4095,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSequenceSetvalResultV1 rr;rr.body[0]=1;rr.body[40]=1;rr.body[44]=8;rr.body[48]=1;rr.availability=sequence_setval_availability_generation;sequence_setval_result_bytes=scratchbird::engine::sblr::EncodeSblrSequenceSetvalResultV1(rr);if(sequence_setval_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4095,"SEQUENCE.ASSIGNMENT_FAILED","sblr.sequence_setval.result_encoding_failed");}
  std::vector<std::uint8_t> query_numeric_result_bytes;if(query_numeric_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_query_numeric");auto consumed=scratchbird::engine::internal_api::ConsumeSblrQueryNumericDescriptor(c,query_numeric_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4097,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrQueryNumericResultV1 rr;rr.body[0]=1;rr.body[40]=1;rr.body[44]=8;rr.body[48]=1;rr.availability=query_numeric_availability_generation;query_numeric_result_bytes=scratchbird::engine::sblr::EncodeSblrQueryNumericResultV1(rr);if(query_numeric_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4097,"NUMERIC.EVALUATION_FAILED","sblr.query_numeric.result_encoding_failed");}
  std::vector<std::uint8_t> advanced_datatype_family_result_bytes;if(advanced_datatype_family_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_advanced_datatype_family");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAdvancedDatatypeFamilyDescriptor(c,advanced_datatype_family_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4099,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAdvancedDatatypeFamilyResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.body[64]=1;rr.availability=advanced_datatype_family_availability_generation;rr.publication_barrier[0]=1;advanced_datatype_family_result_bytes=scratchbird::engine::sblr::EncodeSblrAdvancedDatatypeFamilyResultV1(rr);if(advanced_datatype_family_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4099,"DATATYPE.FAMILY_EVALUATION_FAILED","sblr.advanced_datatype_family.result_encoding_failed");}
  std::vector<std::uint8_t> catalog_introspect_result_bytes;if(catalog_introspect_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_catalog_introspect");auto consumed=scratchbird::engine::internal_api::ConsumeSblrCatalogIntrospectDescriptor(c,catalog_introspect_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4099,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrCatalogIntrospectResultV1 rr;rr.body[0]=1;rr.body[1]=1;rr.availability=catalog_introspect_availability_generation;rr.publication_barrier[0]=1;catalog_introspect_result_bytes=scratchbird::engine::sblr::EncodeSblrCatalogIntrospectResultV1(rr);if(catalog_introspect_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4099,"CATALOG.INTROSPECT_FAILED","sblr.catalog_introspect.result_encoding_failed");}
  std::vector<std::uint8_t> project_result_bytes;if(project_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_project");auto consumed=scratchbird::engine::internal_api::ConsumeSblrProjectDescriptor(c,project_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4101,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrProjectResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.body[68]=1;rr.availability=project_availability_generation;rr.publication_barrier[0]=1;project_result_bytes=scratchbird::engine::sblr::EncodeSblrProjectResultV1(rr);if(project_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4101,"PROJECTION.EXECUTION_FAILED","sblr.project.result_encoding_failed");}
  std::vector<std::uint8_t> aggregate_result_bytes;if(aggregate_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_aggregate");auto consumed=scratchbird::engine::internal_api::ConsumeSblrAggregateDescriptor(c,aggregate_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4103,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrAggregateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.body[68]=1;rr.availability=aggregate_availability_generation;rr.publication_barrier[0]=1;aggregate_result_bytes=scratchbird::engine::sblr::EncodeSblrAggregateResultV1(rr);if(aggregate_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4103,"AGGREGATE.EXECUTION_FAILED","sblr.aggregate.result_encoding_failed");}
  std::vector<std::uint8_t> group_result_bytes;if(group_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_group");auto consumed=scratchbird::engine::internal_api::ConsumeSblrGroupDescriptor(c,group_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4105,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrGroupResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.body[68]=1;rr.availability=group_availability_generation;rr.publication_barrier[0]=1;group_result_bytes=scratchbird::engine::sblr::EncodeSblrGroupResultV1(rr);if(group_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4105,"GROUP.EXECUTION_FAILED","sblr.group.result_encoding_failed");}
  std::vector<std::uint8_t> security_create_group_mapping_result_bytes;if(security_create_group_mapping_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_create_group_mapping");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecCreateGroupMappingDescriptor(c,security_create_group_mapping_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4106,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecCreateGroupMappingResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.group_uuid=security_create_group_mapping_descriptor.group_uuid;rr.principal_uuid=security_create_group_mapping_descriptor.principal_uuid;rr.generation=security_create_group_mapping_descriptor.generation;rr.security_generation=security_create_group_mapping_descriptor.security_generation;rr.availability=security_create_group_mapping_availability_generation;std::copy(security_create_group_mapping_descriptor.descriptor_evidence.begin(),security_create_group_mapping_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_create_group_mapping_result_bytes=scratchbird::engine::sblr::EncodeSblrSecCreateGroupMappingResultV1(rr);if(security_create_group_mapping_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4106,"SECURITY.GROUP_MAPPING_FAILED","sblr.sec_create_group_mapping.result_encoding_failed");}
  std::vector<std::uint8_t> security_drop_group_mapping_result_bytes;if(security_drop_group_mapping_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_drop_group_mapping");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecDropGroupMappingDescriptor(c,security_drop_group_mapping_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4206,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecDropGroupMappingResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.mapping_uuid=security_drop_group_mapping_descriptor.mapping_uuid;rr.generation=security_drop_group_mapping_descriptor.expected_generation;rr.security_generation=security_drop_group_mapping_descriptor.security_generation;rr.availability=security_drop_group_mapping_availability_generation;std::copy(security_drop_group_mapping_descriptor.descriptor_evidence.begin(),security_drop_group_mapping_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_drop_group_mapping_result_bytes=scratchbird::engine::sblr::EncodeSblrSecDropGroupMappingResultV1(rr);if(security_drop_group_mapping_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4206,"SECURITY.GROUP_MAPPING_FAILED","sblr.sec_drop_group_mapping.result_encoding_failed");}
  std::vector<std::uint8_t> security_grant_result_bytes;if(security_grant_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_grant");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecGrantDescriptor(c,security_grant_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4207,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecGrantResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.grant_uuid=security_grant_descriptor.grant_uuid;rr.generation=security_grant_descriptor.expected_generation;rr.security_generation=security_grant_descriptor.security_generation;rr.availability=security_grant_availability_generation;std::copy(security_grant_descriptor.descriptor_evidence.begin(),security_grant_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_grant_result_bytes=scratchbird::engine::sblr::EncodeSblrSecGrantResultV1(rr);if(security_grant_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4207,"SECURITY.GRANT_FAILED","sblr.sec_grant.result_encoding_failed");}
  std::vector<std::uint8_t> security_revoke_result_bytes;if(security_revoke_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_revoke");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecRevokeDescriptor(c,security_revoke_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4208,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecRevokeResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.grant_uuid=security_revoke_descriptor.grant_uuid;rr.generation=security_revoke_descriptor.expected_generation;rr.security_generation=security_revoke_descriptor.security_generation;rr.availability=security_revoke_availability_generation;std::copy(security_revoke_descriptor.descriptor_evidence.begin(),security_revoke_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_revoke_result_bytes=scratchbird::engine::sblr::EncodeSblrSecRevokeResultV1(rr);if(security_revoke_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4208,"SECURITY.REVOKE_FAILED","sblr.sec_revoke.result_encoding_failed");}
  std::vector<std::uint8_t> security_alter_policy_result_bytes;if(security_alter_policy_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_alter_policy");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecAlterPolicyDescriptor(c,security_alter_policy_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4210,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecAlterPolicyResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.policy_uuid=security_alter_policy_descriptor.policy_uuid;rr.generation=security_alter_policy_descriptor.expected_generation;rr.security_generation=security_alter_policy_descriptor.security_generation;rr.availability=security_alter_policy_availability_generation;std::copy(security_alter_policy_descriptor.descriptor_evidence.begin(),security_alter_policy_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_alter_policy_result_bytes=scratchbird::engine::sblr::EncodeSblrSecAlterPolicyResultV1(rr);if(security_alter_policy_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4210,"SECURITY.POLICY_FAILED","sblr.sec_alter_policy.result_encoding_failed");}
  std::vector<std::uint8_t> security_drop_user_result_bytes;if(security_drop_user_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_drop_user");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecDropUserDescriptor(c,security_drop_user_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4211,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecDropUserResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.user_uuid=security_drop_user_descriptor.user_uuid;rr.generation=security_drop_user_descriptor.expected_generation;rr.security_generation=security_drop_user_descriptor.security_generation;rr.availability=security_drop_user_availability_generation;std::copy(security_drop_user_descriptor.descriptor_evidence.begin(),security_drop_user_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_drop_user_result_bytes=scratchbird::engine::sblr::EncodeSblrSecDropUserResultV1(rr);if(security_drop_user_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4211,"SECURITY.USER_FAILED","sblr.sec_drop_user.result_encoding_failed");}
  std::vector<std::uint8_t> sort_result_bytes;if(sort_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sort");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSortDescriptor(c,sort_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4107,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSortResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.body[68]=1;rr.availability=sort_availability_generation;rr.publication_barrier[0]=1;sort_result_bytes=scratchbird::engine::sblr::EncodeSblrSortResultV1(rr);if(sort_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4107,"SORT.EXECUTION_FAILED","sblr.sort.result_encoding_failed");}
  std::vector<std::uint8_t> limit_result_bytes;if(limit_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_limit");auto consumed=scratchbird::engine::internal_api::ConsumeSblrLimitDescriptor(c,limit_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4109,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrLimitResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.body[68]=1;rr.availability=limit_availability_generation;rr.publication_barrier[0]=1;limit_result_bytes=scratchbird::engine::sblr::EncodeSblrLimitResultV1(rr);if(limit_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4109,"LIMIT.EXECUTION_FAILED","sblr.limit.result_encoding_failed");const auto digest=scratchbird::core::hash::ComputeSha256Digest(limit_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=limit_executor\texecutor_id=engine.op.limit\topcode=SBLR_LIMIT\topcode_code=1284\topcode_version=1.0\toperand_descriptor_id=limit_descriptor\tresult_descriptor_id=rowset_descriptor\tresult_descriptor_version=1\tlimit_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<limit_availability_generation<<"\tparent_success_barrier=passed\n";}}

  std::vector<std::uint8_t> ddl_alter_rewrite_rule_result_bytes;
  std::vector<std::uint8_t> security_create_privilege_template_result_bytes;
  std::vector<std::uint8_t> security_create_user_result_bytes;
  std::vector<std::uint8_t> security_alter_user_result_bytes;
  std::vector<std::uint8_t> security_create_role_result_bytes;
  std::vector<std::uint8_t> security_drop_role_result_bytes;
  std::vector<std::uint8_t> security_create_policy_result_bytes;
  std::vector<std::uint8_t> security_drop_policy_result_bytes;
  std::vector<std::uint8_t> security_alter_role_result_bytes;
  std::vector<std::uint8_t> security_alter_privilege_template_result_bytes;
  std::vector<std::uint8_t> security_drop_privilege_template_result_bytes;
  std::vector<std::uint8_t> database_create_template_clone_result_bytes;
  std::vector<std::uint8_t> ddl_create_aggregate_result_bytes;
  std::vector<std::uint8_t> ddl_alter_aggregate_result_bytes;
  std::vector<std::uint8_t> ddl_drop_aggregate_result_bytes;
  std::vector<std::uint8_t> ddl_purge_system_history_result_bytes;
  std::vector<std::uint8_t> ddl_set_index_optimizer_eligibility_result_bytes;
  std::vector<std::uint8_t> ddl_set_table_type_enforcement_result_bytes;
  std::vector<std::uint8_t> database_serialize_logical_snapshot_result_bytes;
  std::vector<std::uint8_t> database_deserialize_logical_snapshot_result_bytes;
  std::vector<std::uint8_t> ddl_drop_rewrite_rule_result_bytes;
  std::vector<std::uint8_t> ddl_validate_constraint_result_bytes;
  auto* result = make_result(SB_ENGINE_RESULT_ROW_BATCH,
                             dispatched.api_result.operation_id);
  result->affected_rows = dispatched.api_result.dml_summary.rows_changed;
  result->result_kind = dispatched.api_result.result_shape.result_kind;
  result->rows_produced = static_cast<std::uint64_t>(
      dispatched.api_result.result_shape.rows.size());
  result->row_values = api_row_values(dispatched.api_result);
  result->row_metadata_values =
      api_row_metadata_values(dispatched.api_result);
  result->evidence_values = api_evidence_values(dispatched.api_result);
  if(ddl_alter_rewrite_rule_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_alter_rewrite_rule");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterRewriteRuleDescriptor(c,ddl_alter_rewrite_rule_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4140,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterRewriteRuleResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_alter_rewrite_rule_availability_generation;rr.publication_barrier[0]=1;ddl_alter_rewrite_rule_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterRewriteRuleResultV1(rr);if(ddl_alter_rewrite_rule_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4140,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_rewrite_rule.result_encoding_failed");result->result_kind="ddl_result";const auto digest=scratchbird::core::hash::ComputeSha256Digest(ddl_alter_rewrite_rule_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_alter_rewrite_rule_executor\texecutor_id=engine.op.ddl_alter_rewrite_rule\topcode=SBLR_DDL_ALTER_REWRITE_RULE\topcode_code=1618\topcode_version=1.0\toperand_descriptor_id=rewrite_rule_alter_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_alter_rewrite_rule_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_alter_rewrite_rule_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if(ddl_drop_rewrite_rule_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_rewrite_rule");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropRewriteRuleDescriptor(c,ddl_drop_rewrite_rule_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4141,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropRewriteRuleResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_drop_rewrite_rule_availability_generation;rr.publication_barrier[0]=1;ddl_drop_rewrite_rule_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlDropRewriteRuleResultV1(rr);if(ddl_drop_rewrite_rule_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4141,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_rewrite_rule.result_encoding_failed");result->result_kind="ddl_result";const auto digest=scratchbird::core::hash::ComputeSha256Digest(ddl_drop_rewrite_rule_result_bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_drop_rewrite_rule_executor\texecutor_id=engine.op.ddl_drop_rewrite_rule\topcode=SBLR_DDL_DROP_REWRITE_RULE\topcode_code=1619\topcode_version=1.0\toperand_descriptor_id=rewrite_rule_drop_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_drop_rewrite_rule_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_drop_rewrite_rule_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if(ddl_validate_constraint_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_validate_constraint");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlValidateConstraintDescriptor(c,ddl_validate_constraint_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4142,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlValidateConstraintResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_validate_constraint_availability_generation;rr.publication_barrier[0]=1;ddl_validate_constraint_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlValidateConstraintResultV1(rr);if(ddl_validate_constraint_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4142,"SYSTEM.CONFIG_FAILED","sblr.ddl_validate_constraint.result_encoding_failed");result->result_kind="management_operation_result";}
  if(ddl_purge_system_history_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_purge_system_history");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlPurgeSystemHistoryDescriptor(c,ddl_purge_system_history_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlPurgeSystemHistoryResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_purge_system_history_availability_generation;rr.publication_barrier[0]=1;ddl_purge_system_history_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlPurgeSystemHistoryResultV1(rr);if(ddl_purge_system_history_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"SYSTEM.CONFIG_FAILED","sblr.ddl_purge_system_history.result_encoding_failed");result->result_kind="management_operation_result";}
  if(security_create_privilege_template_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_security_create_privilege_template");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecurityCreatePrivilegeTemplateDescriptor(c,security_create_privilege_template_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4143,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=security_create_privilege_template_availability_generation;rr.publication_barrier[0]=1;security_create_privilege_template_result_bytes=scratchbird::engine::sblr::EncodeSblrSecurityCreatePrivilegeTemplateResultV1(rr);if(security_create_privilege_template_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4143,"SYSTEM.CONFIG_FAILED","sblr.security_create_privilege_template.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_create_user_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_security_create_user");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecurityCreateUserDescriptor(c,security_create_user_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4192,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecurityCreateUserResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=security_create_user_availability_generation;rr.publication_barrier[0]=1;security_create_user_result_bytes=scratchbird::engine::sblr::EncodeSblrSecurityCreateUserResultV1(rr);if(security_create_user_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4192,"SYSTEM.CONFIG_FAILED","sblr.security_create_user.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_alter_user_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_alter_user");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecAlterUserDescriptor(c,security_alter_user_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4193,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecAlterUserResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.user_uuid=security_alter_user_descriptor.user_uuid;rr.generation=security_alter_user_descriptor.expected_generation;rr.availability=security_alter_user_availability_generation;std::copy(security_alter_user_descriptor.descriptor_evidence.begin(),security_alter_user_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_alter_user_result_bytes=scratchbird::engine::sblr::EncodeSblrSecAlterUserResultV1(rr);if(security_alter_user_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4193,"SYSTEM.CONFIG_FAILED","sblr.sec_alter_user.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_create_role_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_create_role");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecCreateRoleDescriptor(c,security_create_role_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4194,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecCreateRoleResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.role_uuid=security_create_role_descriptor.role_uuid;rr.generation=security_create_role_descriptor.expected_generation;rr.availability=security_create_role_availability_generation;std::copy(security_create_role_descriptor.descriptor_evidence.begin(),security_create_role_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_create_role_result_bytes=scratchbird::engine::sblr::EncodeSblrSecCreateRoleResultV1(rr);if(security_create_role_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4194,"SYSTEM.CONFIG_FAILED","sblr.sec_create_role.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_drop_role_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_drop_role");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecDropRoleDescriptor(c,security_drop_role_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4195,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecDropRoleResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.role_uuid=security_drop_role_descriptor.role_uuid;rr.generation=security_drop_role_descriptor.expected_generation;rr.availability=security_drop_role_availability_generation;std::copy(security_drop_role_descriptor.descriptor_evidence.begin(),security_drop_role_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_drop_role_result_bytes=scratchbird::engine::sblr::EncodeSblrSecDropRoleResultV1(rr);if(security_drop_role_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4195,"SYSTEM.CONFIG_FAILED","sblr.sec_drop_role.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_create_policy_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_create_policy");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecCreatePolicyDescriptor(c,security_create_policy_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4196,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecCreatePolicyResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.generation=security_create_policy_descriptor.expected_generation;rr.availability=security_create_policy_availability_generation;std::copy(security_create_policy_descriptor.evidence.begin(),security_create_policy_descriptor.evidence.end(),rr.effect_evidence.begin());security_create_policy_result_bytes=scratchbird::engine::sblr::EncodeSblrSecCreatePolicyResultV1(rr);if(security_create_policy_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4196,"SYSTEM.CONFIG_FAILED","sblr.sec_create_policy.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_drop_policy_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_drop_policy");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecDropPolicyDescriptor(c,security_drop_policy_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4197,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecDropPolicyResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.generation=security_drop_policy_descriptor.expected_generation;rr.availability=security_drop_policy_availability_generation;std::copy(security_drop_policy_descriptor.descriptor_evidence.begin(),security_drop_policy_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_drop_policy_result_bytes=scratchbird::engine::sblr::EncodeSblrSecDropPolicyResultV1(rr);if(security_drop_policy_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4197,"SYSTEM.CONFIG_FAILED","sblr.sec_drop_policy.result_encoding_failed");result->result_kind="security_result";}
  if(security_alter_role_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_sec_alter_role");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecAlterRoleDescriptor(c,security_alter_role_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4198,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecAlterRoleResultV1 rr;rr.status=1;rr.publication_barrier=1;rr.role_uuid=security_alter_role_descriptor.role_uuid;rr.generation=security_alter_role_descriptor.expected_generation;rr.availability=security_alter_role_availability_generation;std::copy(security_alter_role_descriptor.descriptor_evidence.begin(),security_alter_role_descriptor.descriptor_evidence.end(),rr.effect_evidence.begin());security_alter_role_result_bytes=scratchbird::engine::sblr::EncodeSblrSecAlterRoleResultV1(rr);if(security_alter_role_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4198,"SYSTEM.CONFIG_FAILED","sblr.sec_alter_role.result_encoding_failed");result->result_kind="security_result";}
  if(security_alter_privilege_template_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_security_alter_privilege_template");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecurityAlterPrivilegeTemplateDescriptor(c,security_alter_privilege_template_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4143,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecurityAlterPrivilegeTemplateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=security_alter_privilege_template_availability_generation;rr.publication_barrier[0]=1;security_alter_privilege_template_result_bytes=scratchbird::engine::sblr::EncodeSblrSecurityAlterPrivilegeTemplateResultV1(rr);if(security_alter_privilege_template_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4143,"SYSTEM.CONFIG_FAILED","sblr.security_alter_privilege_template.result_encoding_failed");result->result_kind="ddl_result";}
  if(security_drop_privilege_template_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_security_drop_privilege_template");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSecurityDropPrivilegeTemplateDescriptor(c,security_drop_privilege_template_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4143,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSecurityDropPrivilegeTemplateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=security_drop_privilege_template_availability_generation;rr.publication_barrier[0]=1;security_drop_privilege_template_result_bytes=scratchbird::engine::sblr::EncodeSblrSecurityDropPrivilegeTemplateResultV1(rr);if(security_drop_privilege_template_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4143,"SYSTEM.CONFIG_FAILED","sblr.security_drop_privilege_template.result_encoding_failed");result->result_kind="ddl_result";}
  if(database_create_template_clone_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_database_create_template_clone");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDatabaseCreateTemplateCloneDescriptor(c,database_create_template_clone_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4144,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDatabaseCreateTemplateCloneResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=database_create_template_clone_availability_generation;rr.publication_barrier[0]=1;database_create_template_clone_result_bytes=scratchbird::engine::sblr::EncodeSblrDatabaseCreateTemplateCloneResultV1(rr);if(database_create_template_clone_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4144,"DATABASE.TEMPLATE_CLONE_FAILED","sblr.database_create_template_clone.result_encoding_failed");result->result_kind="management_operation_result";}
  if(ddl_create_aggregate_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_aggregate");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateAggregateDescriptor(c,ddl_create_aggregate_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateAggregateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_create_aggregate_availability_generation;rr.publication_barrier[0]=1;ddl_create_aggregate_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateAggregateResultV1(rr);if(ddl_create_aggregate_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.AGGREGATE_CREATE_FAILED","sblr.ddl_create_aggregate.result_encoding_failed");result->result_kind="ddl_result";}
  if(ddl_alter_aggregate_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_alter_aggregate");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterAggregateDescriptor(c,ddl_alter_aggregate_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterAggregateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_alter_aggregate_availability_generation;rr.publication_barrier[0]=1;ddl_alter_aggregate_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterAggregateResultV1(rr);if(ddl_alter_aggregate_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.AGGREGATE_ALTER_FAILED","sblr.ddl_alter_aggregate.result_encoding_failed");result->result_kind="ddl_result";}
  if(ddl_drop_aggregate_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_aggregate");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropAggregateDescriptor(c,ddl_drop_aggregate_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropAggregateResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_drop_aggregate_availability_generation;rr.publication_barrier[0]=1;ddl_drop_aggregate_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlDropAggregateResultV1(rr);if(ddl_drop_aggregate_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.AGGREGATE_DROP_FAILED","sblr.ddl_drop_aggregate.result_encoding_failed");result->result_kind="ddl_result";}
  if(ddl_drop_dictionary_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_dictionary");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropDictionaryDescriptor(c,ddl_drop_dictionary_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropDictionaryResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_drop_dictionary_availability_generation;rr.publication_barrier[0]=1;ddl_drop_dictionary_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlDropDictionaryResultV1(rr);if(ddl_drop_dictionary_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.DROP_DICTIONARY_FAILED","sblr.ddl_drop_dictionary.result_encoding_failed");result->result_kind="ddl_result";}
  if(ddl_set_index_optimizer_eligibility_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_set_index_optimizer_eligibility");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlSetIndexOptimizerEligibilityDescriptor(c,ddl_set_index_optimizer_eligibility_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlSetIndexOptimizerEligibilityResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_set_index_optimizer_eligibility_availability_generation;rr.publication_barrier[0]=1;ddl_set_index_optimizer_eligibility_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlSetIndexOptimizerEligibilityResultV1(rr);if(ddl_set_index_optimizer_eligibility_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.INDEX_OPTIMIZER_ELIGIBILITY_FAILED","sblr.ddl_set_index_optimizer_eligibility.result_encoding_failed");result->result_kind="ddl_result";}
  if(ddl_set_table_type_enforcement_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_set_table_type_enforcement");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlSetTableTypeEnforcementDescriptor(c,ddl_set_table_type_enforcement_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementResultV1 rr;rr.body[0]=1;rr.body[24]=1;rr.availability=ddl_set_table_type_enforcement_availability_generation;rr.publication_barrier[0]=1;ddl_set_table_type_enforcement_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlSetTableTypeEnforcementResultV1(rr);if(ddl_set_table_type_enforcement_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.SET_TABLE_TYPE_ENFORCEMENT_FAILED","sblr.ddl_set_table_type_enforcement.result_encoding_failed");result->result_kind="management_operation_result";}
  if(database_serialize_logical_snapshot_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_database_serialize_logical_snapshot");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDatabaseSerializeLogicalSnapshotDescriptor(c,database_serialize_logical_snapshot_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4146,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDatabaseSerializeLogicalSnapshotResultV1 rr;rr.body[0]=1;rr.availability=database_serialize_logical_snapshot_availability_generation;rr.publication_barrier[0]=1;database_serialize_logical_snapshot_result_bytes=scratchbird::engine::sblr::EncodeSblrDatabaseSerializeLogicalSnapshotResultV1(rr);if(database_serialize_logical_snapshot_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4146,"DATABASE.SERIALIZE_LOGICAL_SNAPSHOT_FAILED","sblr.database_serialize_logical_snapshot.result_encoding_failed");result->result_kind="logical_snapshot_buffer_descriptor";}
  if(database_deserialize_logical_snapshot_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_database_deserialize_logical_snapshot");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDatabaseDeserializeLogicalSnapshotDescriptor(c,database_deserialize_logical_snapshot_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4147,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDatabaseDeserializeLogicalSnapshotResultV1 rr;rr.body[0]=1;rr.availability=database_deserialize_logical_snapshot_availability_generation;rr.publication_barrier[0]=1;database_deserialize_logical_snapshot_result_bytes=scratchbird::engine::sblr::EncodeSblrDatabaseDeserializeLogicalSnapshotResultV1(rr);if(database_deserialize_logical_snapshot_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4147,"DATABASE.DESERIALIZE_LOGICAL_SNAPSHOT_FAILED","sblr.database_deserialize_logical_snapshot.result_encoding_failed");result->result_kind="management_operation_result";}
  if (opcode_stream && !source_map_root && !error_vector_root &&
      !txn_begin_root && !txn_commit_root && !txn_rollback_root && !txn_savepoint_root && !txn_release_savepoint_root && !txn_rollback_to_savepoint_root && !psql_autonomous_frame_root && !reservation_release_root && !temporary_cleanup_root && !cursor_open_root && !cursor_fetch_root && !cursor_close_root && !read_by_key_root && !read_range_root && !read_stream_root && !result_set_pass_root && !access_cursor_open_root && !access_cursor_fetch_root && !access_cursor_close_root && !insert_root && !update_root && !delete_root && !merge_root && !table_truncate_root && !table_analyze_root && !bulk_import_stream_root && !bulk_export_stream_root && !statement_batch_root && !atomic_cas_root && !atomic_rmw_root && !advisory_lock_root && !advisory_lock_release_root && !function_call_root && !operator_call_root && !cast_root && !compare_root && !domain_operation_root && !udr_invoke_root && !procedure_invoke_root && !function_invoke_root && !aggregate_invoke_root && !sequence_nextval_root && !sequence_currval_root && !query_numeric_root && !advanced_datatype_family_root && !show_version_root && !project_root && !aggregate_root && !group_root && !security_create_group_mapping_root && !security_drop_group_mapping_root && !sort_root && !limit_root && !kv_structured_read_root && !kv_structured_mutate_root && !kv_structured_scan_root && !kv_structured_stream_read_root && !kv_structured_stream_append_root && !kv_structured_timeseries_root && !system_config_set_root && !ddl_create_domain_root && !ddl_alter_domain_root && !ddl_create_view_root && !ddl_alter_view_root && !ddl_drop_view_root && !ddl_create_trigger_root && !ddl_create_package_root && !ddl_create_or_replace_srs_root && !ddl_drop_srs_root && !ddl_create_schema_root && !ddl_alter_rewrite_rule_root && !ddl_drop_rewrite_rule_root && !ddl_validate_constraint_root) {
    result->query_execute_result_handle = query_handle_validation.handle;
    result->query_execute_result_handle_validated = true;
    result->admitted_query_row_stream_renderer = true;
  }
  if (ddl_alter_dictionary_root) {
    auto c=receipt->engine_context;
    c.trace_tags.push_back("private_ddl_alter_dictionary");
    auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterDictionaryDescriptor(c,ddl_alter_dictionary_descriptor);
    if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlAlterDictionaryResultV1 rr; rr.body[0]=1; rr.body[24]=1; rr.availability=ddl_alter_dictionary_availability_generation; rr.publication_barrier[0]=1;
    ddl_alter_dictionary_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterDictionaryResultV1(rr);
    if(ddl_alter_dictionary_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.ALTER_DICTIONARY_FAILED","sblr.ddl_alter_dictionary.result_encoding_failed");
    result->result_kind="ddl_result";
  }
  if (ddl_create_continuous_view_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_create_continuous_view"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateContinuousViewDescriptor(c,ddl_create_continuous_view_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlCreateContinuousViewResultV1 rr; rr.availability=ddl_create_continuous_view_availability_generation; rr.publication_barrier[0]=1; ddl_create_continuous_view_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateContinuousViewResultV1(rr); if(ddl_create_continuous_view_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.CREATE_CONTINUOUS_VIEW_FAILED","sblr.ddl_create_continuous_view.result_encoding_failed"); result->result_kind="ddl_result"; }
  if (ddl_alter_continuous_view_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_alter_continuous_view"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterContinuousViewDescriptor(c,ddl_alter_continuous_view_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlAlterContinuousViewResultV1 rr; rr.availability=ddl_alter_continuous_view_availability_generation; rr.publication_barrier[0]=1; ddl_alter_continuous_view_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterContinuousViewResultV1(rr); if(ddl_alter_continuous_view_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.ALTER_CONTINUOUS_VIEW_FAILED","sblr.ddl_alter_continuous_view.result_encoding_failed"); result->result_kind="ddl_result"; }
  if (ddl_drop_continuous_view_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_drop_continuous_view"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropContinuousViewDescriptor(c,ddl_drop_continuous_view_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlDropContinuousViewResultV1 rr; rr.availability=ddl_drop_continuous_view_availability_generation; rr.publication_barrier[0]=1; ddl_drop_continuous_view_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlDropContinuousViewResultV1(rr); if(ddl_drop_continuous_view_result_bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4145,"DDL.DROP_CONTINUOUS_VIEW_FAILED","sblr.ddl_drop_continuous_view.result_encoding_failed"); result->result_kind="ddl_result"; }
  if(dml_async_insert_submit_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_dml_async_insert_submit");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDmlAsyncInsertSubmitDescriptor(c,dml_async_insert_submit_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDmlAsyncInsertSubmitResultV1 rr;rr.availability=dml_async_insert_submit_availability_generation;rr.publication_barrier[0]=1;dml_async_insert_submit_result_bytes=scratchbird::engine::sblr::EncodeSblrDmlAsyncInsertSubmitResultV1(rr);result->result_kind="async_insert_operation_descriptor";}
  if(dml_async_insert_status_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_dml_async_insert_status");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDmlAsyncInsertStatusDescriptor(c,dml_async_insert_status_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDmlAsyncInsertStatusResultV1 rr;rr.availability=dml_async_insert_status_availability_generation;rr.publication_barrier[0]=1;dml_async_insert_status_result_bytes=scratchbird::engine::sblr::EncodeSblrDmlAsyncInsertStatusResultV1(rr);result->result_kind="async_insert_operation_descriptor";}
  if(dml_async_insert_cancel_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_dml_async_insert_cancel");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDmlAsyncInsertCancelDescriptor(c,dml_async_insert_cancel_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDmlAsyncInsertCancelResultV1 rr;rr.availability=dml_async_insert_cancel_availability_generation;rr.publication_barrier[0]=1;dml_async_insert_cancel_result_bytes=scratchbird::engine::sblr::EncodeSblrDmlAsyncInsertCancelResultV1(rr);result->result_kind="async_insert_operation_descriptor";}
  if(dml_counter_add_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_dml_counter_add");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDmlCounterAddDescriptor(c,dml_counter_add_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDmlCounterAddResultV1 rr;rr.availability=dml_counter_add_availability_generation;rr.publication_barrier[0]=1;dml_counter_add_result_bytes=scratchbird::engine::sblr::EncodeSblrDmlCounterAddResultV1(rr);result->result_kind="counter_result";}
  if(dml_timeseries_schema_write_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_dml_timeseries_schema_write");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDmlTimeseriesSchemaWriteDescriptor(c,dml_timeseries_schema_write_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDmlTimeseriesSchemaWriteResultV1 rr;rr.availability=dml_timeseries_schema_write_availability_generation;rr.publication_barrier[0]=1;dml_timeseries_schema_write_result_bytes=scratchbird::engine::sblr::EncodeSblrDmlTimeseriesSchemaWriteResultV1(rr);result->result_kind="timeseries_write_result";}
  if(ddl_timeseries_series_cardinality_policy_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_timeseries_series_cardinality_policy");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptor(c,ddl_timeseries_series_cardinality_policy_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlTimeseriesSeriesCardinalityPolicyResultV1 rr;rr.availability=ddl_timeseries_series_cardinality_policy_availability_generation;rr.publication_barrier[0]=1;ddl_timeseries_series_cardinality_policy_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlTimeseriesSeriesCardinalityPolicyResultV1(rr);result->result_kind="ddl_result";}
  if(ddl_create_timeseries_value_cache_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_timeseries_value_cache");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateTimeseriesValueCacheDescriptor(c,ddl_create_timeseries_value_cache_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4145,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateTimeseriesValueCacheResultV1 rr;rr.availability=ddl_create_timeseries_value_cache_availability_generation;rr.publication_barrier[0]=1;ddl_create_timeseries_value_cache_result_bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateTimeseriesValueCacheResultV1(rr);result->result_kind="ddl_result";}
  result->payload = api_result_payload(dispatched.api_result);
  if (ddl_alter_rewrite_rule_root) {
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(ddl_alter_rewrite_rule_result_bytes.data()), ddl_alter_rewrite_rule_result_bytes.size());
  }
  if (ddl_drop_rewrite_rule_root) {
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(ddl_drop_rewrite_rule_result_bytes.data()), ddl_drop_rewrite_rule_result_bytes.size());
  }
  if (ddl_validate_constraint_root) {
    result->result_kind = "management_operation_result";
    result->payload.assign(reinterpret_cast<const char*>(ddl_validate_constraint_result_bytes.data()), ddl_validate_constraint_result_bytes.size());
  }
  if (security_create_privilege_template_root) {
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(security_create_privilege_template_result_bytes.data()), security_create_privilege_template_result_bytes.size());
  }
  if (security_create_user_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(security_create_user_result_bytes.data()),security_create_user_result_bytes.size()); }
  if (security_alter_user_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(security_alter_user_result_bytes.data()),security_alter_user_result_bytes.size()); }
  if (security_create_role_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(security_create_role_result_bytes.data()),security_create_role_result_bytes.size()); }
  if (security_drop_role_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(security_drop_role_result_bytes.data()),security_drop_role_result_bytes.size()); }
  if (security_create_policy_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(security_create_policy_result_bytes.data()),security_create_policy_result_bytes.size()); }
  if (security_drop_policy_root) { result->result_kind="security_result"; result->payload.assign(reinterpret_cast<const char*>(security_drop_policy_result_bytes.data()),security_drop_policy_result_bytes.size()); }
  if (security_alter_role_root) { result->result_kind="security_result"; result->payload.assign(reinterpret_cast<const char*>(security_alter_role_result_bytes.data()),security_alter_role_result_bytes.size()); }
  if (security_alter_privilege_template_root) {
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(security_alter_privilege_template_result_bytes.data()), security_alter_privilege_template_result_bytes.size());
  }
  if (security_drop_privilege_template_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(security_drop_privilege_template_result_bytes.data()),security_drop_privilege_template_result_bytes.size()); }
  if (database_create_template_clone_root) { result->result_kind="management_operation_result"; result->payload.assign(reinterpret_cast<const char*>(database_create_template_clone_result_bytes.data()),database_create_template_clone_result_bytes.size()); }
  if (ddl_create_aggregate_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_create_aggregate_result_bytes.data()),ddl_create_aggregate_result_bytes.size()); }
  if (ddl_alter_aggregate_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_alter_aggregate_result_bytes.data()),ddl_alter_aggregate_result_bytes.size()); }
  if (ddl_drop_aggregate_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_drop_aggregate_result_bytes.data()),ddl_drop_aggregate_result_bytes.size()); }
  if (ddl_drop_dictionary_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_drop_dictionary_result_bytes.data()),ddl_drop_dictionary_result_bytes.size()); }
  if (ddl_alter_dictionary_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_alter_dictionary_result_bytes.data()),ddl_alter_dictionary_result_bytes.size()); }
  if (ddl_create_continuous_view_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_create_continuous_view_result_bytes.data()),ddl_create_continuous_view_result_bytes.size()); }
  if (ddl_alter_continuous_view_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_alter_continuous_view_result_bytes.data()),ddl_alter_continuous_view_result_bytes.size()); }
  if (ddl_drop_continuous_view_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_drop_continuous_view_result_bytes.data()),ddl_drop_continuous_view_result_bytes.size()); }
  if (dml_async_insert_submit_root) { result->result_kind="async_insert_operation_descriptor"; result->payload.assign(reinterpret_cast<const char*>(dml_async_insert_submit_result_bytes.data()),dml_async_insert_submit_result_bytes.size()); }
  if (dml_async_insert_status_root) { result->result_kind="async_insert_operation_descriptor"; result->payload.assign(reinterpret_cast<const char*>(dml_async_insert_status_result_bytes.data()),dml_async_insert_status_result_bytes.size()); }
  if (dml_async_insert_cancel_root) { result->result_kind="async_insert_operation_descriptor"; result->payload.assign(reinterpret_cast<const char*>(dml_async_insert_cancel_result_bytes.data()),dml_async_insert_cancel_result_bytes.size()); }
  if (dml_counter_add_root) { result->result_kind="counter_result"; result->payload.assign(reinterpret_cast<const char*>(dml_counter_add_result_bytes.data()),dml_counter_add_result_bytes.size()); }
  if (dml_timeseries_schema_write_root) { result->result_kind="timeseries_write_result"; result->payload.assign(reinterpret_cast<const char*>(dml_timeseries_schema_write_result_bytes.data()),dml_timeseries_schema_write_result_bytes.size()); }
  if (ddl_purge_system_history_root) { result->result_kind="management_operation_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_purge_system_history_result_bytes.data()),ddl_purge_system_history_result_bytes.size()); }
  if (ddl_set_index_optimizer_eligibility_root) { result->result_kind="ddl_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_set_index_optimizer_eligibility_result_bytes.data()),ddl_set_index_optimizer_eligibility_result_bytes.size()); }
  if (ddl_set_table_type_enforcement_root) { result->result_kind="management_operation_result"; result->payload.assign(reinterpret_cast<const char*>(ddl_set_table_type_enforcement_result_bytes.data()),ddl_set_table_type_enforcement_result_bytes.size()); }
  if (database_serialize_logical_snapshot_root) { result->result_kind="logical_snapshot_buffer_descriptor"; result->payload.assign(reinterpret_cast<const char*>(database_serialize_logical_snapshot_result_bytes.data()),database_serialize_logical_snapshot_result_bytes.size()); }
  if (database_deserialize_logical_snapshot_root) { result->result_kind="management_operation_result"; result->payload.assign(reinterpret_cast<const char*>(database_deserialize_logical_snapshot_result_bytes.data()),database_deserialize_logical_snapshot_result_bytes.size()); }
  if (txn_begin_root) {
    result->result_kind = "transaction_handle";
    result->payload.assign(
        reinterpret_cast<const char*>(transaction_begin_handle_bytes.data()),
        transaction_begin_handle_bytes.size());
  }
  if (txn_commit_root) {
    result->result_kind = "commit_result";
    result->payload.assign(
        reinterpret_cast<const char*>(transaction_commit_result_bytes.data()),
        transaction_commit_result_bytes.size());
  }
  if (txn_rollback_root) {
    result->result_kind = "rollback_result";
    result->payload.assign(
        reinterpret_cast<const char*>(transaction_rollback_result_bytes.data()),
        transaction_rollback_result_bytes.size());
  }
  if(txn_savepoint_root){result->result_kind="savepoint_handle";result->payload.assign(
      reinterpret_cast<const char*>(savepoint_handle_bytes.data()),savepoint_handle_bytes.size());}
  if(txn_release_savepoint_root){result->result_kind="savepoint_release_result";result->payload.assign(reinterpret_cast<const char*>(savepoint_release_result_bytes.data()),savepoint_release_result_bytes.size());}
  if(txn_rollback_to_savepoint_root){result->result_kind="savepoint_rollback_result";result->payload.assign(reinterpret_cast<const char*>(savepoint_rollback_result_bytes.data()),savepoint_rollback_result_bytes.size());}
  if(psql_autonomous_frame_root){result->result_kind="autonomous_frame_result";result->payload.assign(reinterpret_cast<const char*>(autonomous_frame_result_bytes.data()),autonomous_frame_result_bytes.size());}
  if(reservation_release_root){result->result_kind="transaction_reservation_result";result->payload.assign(reinterpret_cast<const char*>(reservation_release_result_bytes.data()),reservation_release_result_bytes.size());}
  if(temporary_cleanup_root){result->result_kind="temporary_cleanup_result";result->payload.assign(reinterpret_cast<const char*>(temporary_cleanup_result_bytes.data()),temporary_cleanup_result_bytes.size());}
  if(cursor_open_root){result->result_kind="cursor_handle";result->payload.assign(reinterpret_cast<const char*>(cursor_open_result_bytes.data()),cursor_open_result_bytes.size());}
  if(cursor_fetch_root){result->result_kind="cursor_fetch_result";result->payload.assign(reinterpret_cast<const char*>(cursor_fetch_result_bytes.data()),cursor_fetch_result_bytes.size());}
  if(cursor_close_root){result->result_kind="cursor_close_result";result->payload.assign(reinterpret_cast<const char*>(cursor_close_result_bytes.data()),cursor_close_result_bytes.size());}
  if(read_by_key_root){result->result_kind="row_descriptor";result->payload.assign(reinterpret_cast<const char*>(read_by_key_result_bytes.data()),read_by_key_result_bytes.size());}
  if(result_set_pass_root){result->result_kind="result_set_handle";result->payload.assign(reinterpret_cast<const char*>(result_set_pass_result_bytes.data()),result_set_pass_result_bytes.size());}
  if(access_cursor_open_root){result->result_kind="access_cursor_handle";result->payload.assign(reinterpret_cast<const char*>(access_cursor_open_result_bytes.data()),access_cursor_open_result_bytes.size());}
  if(access_cursor_fetch_root){result->result_kind="access_cursor_rowset_or_eof";result->payload.assign(reinterpret_cast<const char*>(access_cursor_fetch_result_bytes.data()),access_cursor_fetch_result_bytes.size());}
  if(access_cursor_close_root){result->result_kind="void";result->payload.clear();}
  if(insert_root){result->result_kind="mutation_result";result->payload.assign(reinterpret_cast<const char*>(insert_result_bytes.data()),insert_result_bytes.size());}
  if(update_root){result->result_kind="mutation_result";result->payload.assign(reinterpret_cast<const char*>(update_result_bytes.data()),update_result_bytes.size());}
  if(delete_root){result->result_kind="mutation_result";result->payload.assign(reinterpret_cast<const char*>(delete_result_bytes.data()),delete_result_bytes.size());}
  if(merge_root){result->result_kind="mutation_result";result->payload.assign(reinterpret_cast<const char*>(merge_result_bytes.data()),merge_result_bytes.size());}
  if(table_truncate_root){result->result_kind="mutation_result";result->payload.assign(reinterpret_cast<const char*>(table_truncate_result_bytes.data()),table_truncate_result_bytes.size());}
  if(table_analyze_root){result->result_kind="mutation_result";result->payload.assign(reinterpret_cast<const char*>(table_analyze_result_bytes.data()),table_analyze_result_bytes.size());}
  if(bulk_import_stream_root){result->result_kind="bulk_mutation_result";result->payload.assign(reinterpret_cast<const char*>(bulk_import_stream_result_bytes.data()),bulk_import_stream_result_bytes.size());}
  if(bulk_export_stream_root){result->result_kind="bulk_read_result";result->payload.assign(reinterpret_cast<const char*>(bulk_export_stream_result_bytes.data()),bulk_export_stream_result_bytes.size());}
  if(statement_batch_root){result->result_kind="batch_result_vector";result->payload.assign(reinterpret_cast<const char*>(statement_batch_result_bytes.data()),statement_batch_result_bytes.size());}
  if(atomic_cas_root){result->result_kind="atomic_cas_result";result->payload.assign(reinterpret_cast<const char*>(atomic_cas_result_bytes.data()),atomic_cas_result_bytes.size());}
  if(atomic_rmw_root){result->result_kind="atomic_rmw_result";result->payload.assign(reinterpret_cast<const char*>(atomic_rmw_result_bytes.data()),atomic_rmw_result_bytes.size());}
  if(advisory_lock_root){result->result_kind="advisory_lock_result";result->payload.assign(reinterpret_cast<const char*>(advisory_lock_result_bytes.data()),advisory_lock_result_bytes.size());}
  if(advisory_lock_release_root){result->result_kind="advisory_lock_result";result->payload.assign(reinterpret_cast<const char*>(advisory_lock_release_result_bytes.data()),advisory_lock_release_result_bytes.size());}
  if(function_call_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(function_call_result_bytes.data()),function_call_result_bytes.size());}
  if(operator_call_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(operator_call_result_bytes.data()),operator_call_result_bytes.size());}
  if(cast_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(cast_result_bytes.data()),cast_result_bytes.size());}
  if(compare_root){result->result_kind="boolean_value";result->payload.assign(reinterpret_cast<const char*>(compare_result_bytes.data()),compare_result_bytes.size());}
  if(domain_operation_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(domain_operation_result_bytes.data()),domain_operation_result_bytes.size());}
  if(udr_invoke_root){result->result_kind="typed_value_or_result_set";result->payload.assign(reinterpret_cast<const char*>(udr_invoke_result_bytes.data()),udr_invoke_result_bytes.size());}
  if(procedure_invoke_root){result->result_kind="procedure_result";result->payload.assign(reinterpret_cast<const char*>(procedure_invoke_result_bytes.data()),procedure_invoke_result_bytes.size());}
  if(function_invoke_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(function_invoke_result_bytes.data()),function_invoke_result_bytes.size());}
  if(aggregate_invoke_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(aggregate_invoke_result_bytes.data()),aggregate_invoke_result_bytes.size());}
  if(sequence_nextval_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(sequence_nextval_result_bytes.data()),sequence_nextval_result_bytes.size());}
  if(sequence_currval_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(sequence_currval_result_bytes.data()),sequence_currval_result_bytes.size());}
  if(sequence_setval_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(sequence_setval_result_bytes.data()),sequence_setval_result_bytes.size());}
  if(query_numeric_root){result->result_kind="typed_value";result->payload.assign(reinterpret_cast<const char*>(query_numeric_result_bytes.data()),query_numeric_result_bytes.size());}
  if(advanced_datatype_family_root){result->result_kind="datatype_family_evaluation";result->payload.assign(reinterpret_cast<const char*>(advanced_datatype_family_result_bytes.data()),advanced_datatype_family_result_bytes.size());}
  if(project_root){result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(project_result_bytes.data()),project_result_bytes.size());}
  if(catalog_introspect_root){result->result_kind="catalog_introspect_result";result->payload.assign(reinterpret_cast<const char*>(catalog_introspect_result_bytes.data()),catalog_introspect_result_bytes.size());}
  if(ddl_alter_trigger_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_alter_trigger");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterTriggerDescriptor(c,ddl_alter_trigger_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterTriggerResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_alter_trigger_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterTriggerResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_trigger.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_drop_trigger_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_trigger");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropTriggerDescriptor(c,ddl_drop_trigger_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4126,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropTriggerResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_drop_trigger_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropTriggerResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4126,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_trigger.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_create_procedure_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_procedure");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateProcedureDescriptor(c,ddl_create_procedure_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4127,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateProcedureResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_procedure_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateProcedureResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4127,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_procedure.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_alter_procedure_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_procedure");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterProcedureDescriptor(c,ddl_alter_procedure_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4128,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterProcedureResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_alter_procedure_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterProcedureResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4128,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_procedure.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_drop_procedure_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_procedure");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropProcedureDescriptor(c,ddl_drop_procedure_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4129,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropProcedureResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_drop_procedure_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropProcedureResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4129,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_procedure.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_create_function_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_procedure");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateFunctionDescriptor(c,ddl_create_function_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4130,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateFunctionResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_function_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateFunctionResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4130,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_function.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_alter_function_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_procedure");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterFunctionDescriptor(c,ddl_alter_function_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4131,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterFunctionResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_alter_function_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterFunctionResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4131,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_function.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_drop_function_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_function");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropFunctionDescriptor(c,ddl_drop_function_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4132,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropFunctionResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_drop_function_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropFunctionResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4132,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_function.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
  if(aggregate_root){result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(aggregate_result_bytes.data()),aggregate_result_bytes.size());}
  if(group_root){result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(group_result_bytes.data()),group_result_bytes.size());}
  if(security_create_group_mapping_root){result->result_kind="security_result";result->payload.assign(reinterpret_cast<const char*>(security_create_group_mapping_result_bytes.data()),security_create_group_mapping_result_bytes.size());}
  if(security_drop_group_mapping_root){result->result_kind="security_result";result->payload.assign(reinterpret_cast<const char*>(security_drop_group_mapping_result_bytes.data()),security_drop_group_mapping_result_bytes.size());}
  if(security_grant_root){result->result_kind="security_result";result->payload.assign(reinterpret_cast<const char*>(security_grant_result_bytes.data()),security_grant_result_bytes.size());}
  if(security_revoke_root){result->result_kind="security_result";result->payload.assign(reinterpret_cast<const char*>(security_revoke_result_bytes.data()),security_revoke_result_bytes.size());}
  if(security_alter_policy_root){result->result_kind="security_result";result->payload.assign(reinterpret_cast<const char*>(security_alter_policy_result_bytes.data()),security_alter_policy_result_bytes.size());}
  if(security_drop_user_root){result->result_kind="security_result";result->payload.assign(reinterpret_cast<const char*>(security_drop_user_result_bytes.data()),security_drop_user_result_bytes.size());}
  if(sort_root){result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(sort_result_bytes.data()),sort_result_bytes.size());}
  if(limit_root){result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(limit_result_bytes.data()),limit_result_bytes.size());}
  if(window_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_window");auto consumed=scratchbird::engine::internal_api::ConsumeSblrWindowDescriptor(c,window_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4111,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrWindowResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=window_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrWindowResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4111,"WINDOW.EXECUTION_FAILED","sblr.window.result_encoding_failed");result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=window_executor\texecutor_id=engine.op.window\topcode=SBLR_WINDOW\topcode_code=1285\topcode_version=1.0\toperand_descriptor_id=window_descriptor\tresult_descriptor_id=rowset_descriptor\tresult_descriptor_version=1\twindow_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<window_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if(return_result_set_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_return_result_set");auto consumed=scratchbird::engine::internal_api::ConsumeSblrReturnResultSetDescriptor(c,return_result_set_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4113,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrReturnResultSetResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[32]=1;rr.availability=return_result_set_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrReturnResultSetResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4113,"RESULT_SET.EXECUTION_FAILED","sblr.return_result_set.result_encoding_failed");result->result_kind="result_set_handle";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=return_result_set_executor\texecutor_id=engine.op.return_result_set\topcode=SBLR_RETURN_RESULT_SET\topcode_code=1286\topcode_version=1.0\toperand_descriptor_id=result_set_return_descriptor\tresult_descriptor_id=result_set_handle\tresult_descriptor_version=1\treturn_result_set_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<return_result_set_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if(kv_structured_read_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_kv_structured_read");auto consumed=scratchbird::engine::internal_api::ConsumeSblrKvStructuredReadDescriptor(c,kv_structured_read_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4115,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrKvStructuredReadResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=kv_structured_read_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrKvStructuredReadResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4115,"KV_STRUCTURED_READ.EXECUTION_FAILED","sblr.kv_structured_read.result_encoding_failed");result->result_kind="kv_structured_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=kv_structured_read_executor\texecutor_id=engine.op.kv_structured_read\topcode=SBLR_KV_STRUCTURED_READ\topcode_code=8192\topcode_version=1.0\toperand_descriptor_id=kv_structured_read_descriptor\tresult_descriptor_id=kv_structured_result\tresult_descriptor_version=1\tkv_structured_read_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<kv_structured_read_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if(read_range_root){result->result_kind="rowset_descriptor";result->payload.assign(reinterpret_cast<const char*>(read_range_result_bytes.data()),read_range_result_bytes.size());}
  if(read_stream_root){result->result_kind="stream_handle";result->payload.assign(reinterpret_cast<const char*>(read_stream_result_bytes.data()),read_stream_result_bytes.size());}
    if(kv_structured_mutate_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_kv_structured_mutate");auto consumed=scratchbird::engine::internal_api::ConsumeSblrKvStructuredMutateDescriptor(c,kv_structured_mutate_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4117,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrKvStructuredMutateResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=kv_structured_mutate_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrKvStructuredMutateResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4117,"KV.MUTATION_FAILED","sblr.kv_structured_mutate.result_encoding_failed");result->result_kind="kv_structured_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=kv_structured_mutate_executor\texecutor_id=engine.op.kv_structured_mutate\topcode=SBLR_KV_STRUCTURED_MUTATE\topcode_code=8193\topcode_version=1.0\toperand_descriptor_id=kv_structured_mutate_descriptor\tresult_descriptor_id=kv_structured_result\tresult_descriptor_version=1\tkv_structured_mutate_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<kv_structured_mutate_availability_generation<<"\tparent_success_barrier=passed\n";}}
    if(kv_structured_scan_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_kv_structured_scan");auto consumed=scratchbird::engine::internal_api::ConsumeSblrKvStructuredScanDescriptor(c,kv_structured_scan_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4119,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrKvStructuredScanResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=kv_structured_scan_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrKvStructuredScanResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4119,"KV.SCAN_FAILED","sblr.kv_structured_scan.result_encoding_failed");result->result_kind="kv_structured_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=kv_structured_scan_executor\texecutor_id=engine.op.kv_structured_scan\topcode=SBLR_KV_STRUCTURED_SCAN\topcode_code=8194\topcode_version=1.0\toperand_descriptor_id=kv_structured_scan_descriptor\tresult_descriptor_id=kv_structured_result\tresult_descriptor_version=1\tkv_structured_scan_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<kv_structured_scan_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(kv_structured_stream_read_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_kv_structured_stream_read");auto consumed=scratchbird::engine::internal_api::ConsumeSblrKvStructuredStreamReadDescriptor(c,kv_structured_stream_read_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4121,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrKvStructuredStreamReadResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=kv_structured_stream_read_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrKvStructuredStreamReadResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4121,"KV.STREAM_READ_FAILED","sblr.kv_structured_stream_read.result_encoding_failed");result->result_kind="kv_structured_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=kv_structured_stream_read_executor\texecutor_id=engine.op.kv_structured_stream_read\topcode=SBLR_KV_STRUCTURED_STREAM_READ\topcode_code=8195\topcode_version=1.0\toperand_descriptor_id=kv_structured_stream_read_descriptor\tresult_descriptor_id=kv_structured_result\tresult_descriptor_version=1\tkv_structured_stream_read_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<kv_structured_stream_read_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(kv_structured_stream_append_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_kv_structured_stream_append");auto consumed=scratchbird::engine::internal_api::ConsumeSblrKvStructuredStreamAppendDescriptor(c,kv_structured_stream_append_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4122,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrKvStructuredStreamAppendResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=kv_structured_stream_append_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrKvStructuredStreamAppendResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4122,"KV.STREAM_APPEND_FAILED","sblr.kv_structured_stream_append.result_encoding_failed");result->result_kind="kv_structured_mutation_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=kv_structured_stream_append_executor\texecutor_id=engine.op.kv_structured_stream_append\topcode=SBLR_KV_STRUCTURED_STREAM_APPEND\topcode_code=8196\topcode_version=1.0\toperand_descriptor_id=kv_structured_stream_append_descriptor\tresult_descriptor_id=kv_structured_mutation_result\tresult_descriptor_version=1\tkv_structured_stream_append_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<kv_structured_stream_append_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(kv_structured_timeseries_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_kv_structured_timeseries");auto consumed=scratchbird::engine::internal_api::ConsumeSblrKvStructuredTimeseriesDescriptor(c,kv_timeseries_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4123,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrKvStructuredTimeseriesResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=kv_structured_timeseries_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrKvStructuredTimeseriesResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4123,"KV.TIMESERIES_FAILED","sblr.kv_structured_timeseries.result_encoding_failed");result->result_kind="kv_structured_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=kv_structured_timeseries_executor\texecutor_id=engine.op.kv_structured_timeseries\topcode=SBLR_KV_STRUCTURED_TIMESERIES\topcode_code=8197\topcode_version=1.0\toperand_descriptor_id=system_config_set_descriptor\tresult_descriptor_id=management_result\tresult_descriptor_version=1\tkv_structured_timeseries_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<kv_structured_timeseries_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(system_config_set_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_system_config_set");auto consumed=scratchbird::engine::internal_api::ConsumeSblrSystemConfigSetDescriptor(c,system_config_set_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4124,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrSystemConfigSetResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=system_config_set_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrSystemConfigSetResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4124,"SYSTEM.CONFIG_FAILED","sblr.system_config_set.result_encoding_failed");result->result_kind="management_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=system_config_set_executor\texecutor_id=engine.op.system_config_set\topcode=SBLR_SYSTEM_CONFIG_SET\topcode_code=5125\topcode_version=1.0\toperand_descriptor_id=system_config_set_descriptor\tresult_descriptor_id=management_result\tresult_descriptor_version=1\tsystem_config_set_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<system_config_set_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_create_domain_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_domain");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateDomainDescriptor(c,ddl_create_domain_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateDomainResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_create_domain_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateDomainResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_domain.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_domain_executor\texecutor_id=engine.op.ddl_create_domain\topcode=SBLR_DDL_CREATE_DOMAIN\topcode_code=1542\topcode_version=1.0\toperand_descriptor_id=create_domain_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_domain_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_domain_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_alter_domain_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_alter_domain");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterDomainDescriptor(c,ddl_alter_domain_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterDomainResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_alter_domain_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterDomainResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_domain.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_alter_domain_executor\texecutor_id=engine.op.ddl_alter_domain\topcode=SBLR_DDL_ALTER_DOMAIN\topcode_code=1547\topcode_version=1.0\toperand_descriptor_id=alter_domain_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_alter_domain_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_alter_domain_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_create_view_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_view");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateViewDescriptor(c,ddl_create_view_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateViewResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_create_view_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateViewResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_view.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_view_executor\texecutor_id=engine.op.ddl_create_view\topcode=SBLR_DDL_CREATE_VIEW\topcode_code=1548\topcode_version=1.0\toperand_descriptor_id=create_view_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_view_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_view_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_alter_view_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_alter_view");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterViewDescriptor(c,ddl_alter_view_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterViewResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_alter_view_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterViewResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_view.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_alter_view_executor\texecutor_id=engine.op.ddl_alter_view\topcode=SBLR_DDL_ALTER_VIEW\topcode_code=1549\topcode_version=1.0\toperand_descriptor_id=alter_view_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_alter_view_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_alter_view_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_drop_view_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_view");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropViewDescriptor(c,ddl_drop_view_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropViewResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_drop_view_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropViewResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_view.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_drop_view_executor\texecutor_id=engine.op.ddl_drop_view\topcode=SBLR_DDL_DROP_VIEW\topcode_code=1550\topcode_version=1.0\toperand_descriptor_id=drop_view_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_drop_view_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_drop_view_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_create_trigger_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_trigger");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateTriggerDescriptor(c,ddl_create_trigger_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4125,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateTriggerResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_create_trigger_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateTriggerResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4125,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_trigger.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_trigger_executor\texecutor_id=engine.op.ddl_create_trigger\topcode=SBLR_DDL_CREATE_TRIGGER\topcode_code=1551\topcode_version=1.0\toperand_descriptor_id=create_trigger_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_trigger_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_trigger_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_alter_package_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_package");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlAlterPackageDescriptor(c,ddl_alter_package_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4133,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlAlterPackageResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_alter_package_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlAlterPackageResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4133,"SYSTEM.CONFIG_FAILED","sblr.ddl_alter_package.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_create_package_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_package");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreatePackageDescriptor(c,ddl_create_package_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4133,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreatePackageResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_package_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreatePackageResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4133,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_package.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_create_temporary_table_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_temporary_table");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateTemporaryTableDescriptor(c,ddl_create_temporary_table_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4134,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateTemporaryTableResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_temporary_table_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateTemporaryTableResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4134,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_temporary_table.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_temporary_table_executor\texecutor_id=engine.op.ddl_create_temporary_table\topcode=SBLR_DDL_CREATE_TEMPORARY_TABLE\topcode_code=1561\topcode_version=1.0\topcode_version=1.0\toperand_descriptor_id=create_temporary_table_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_temporary_table_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_temporary_table_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_drop_temporary_table_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_temporary_table");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropTemporaryTableDescriptor(c,ddl_drop_temporary_table_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4135,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropTemporaryTableResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_drop_temporary_table_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropTemporaryTableResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4135,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_temporary_table.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_drop_temporary_table_executor\texecutor_id=engine.op.ddl_drop_temporary_table\topcode=SBLR_DDL_DROP_TEMPORARY_TABLE\topcode_code=1562\topcode_version=1.0\topcode_version=1.0\toperand_descriptor_id=drop_temporary_table_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_drop_temporary_table_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_drop_temporary_table_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_rename_object_vector_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_rename_object_vector");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlRenameObjectVectorDescriptor(c,ddl_rename_object_vector_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4136,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlRenameObjectVectorResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_rename_object_vector_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlRenameObjectVectorResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4136,"SYSTEM.CONFIG_FAILED","sblr.ddl_rename_object_vector.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_rename_object_vector_executor\texecutor_id=engine.op.ddl_rename_object_vector\topcode=SBLR_DDL_RENAME_OBJECT_VECTOR\topcode_code=1563\topcode_version=1.0\toperand_descriptor_id=object_rename_vector_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_rename_object_vector_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_rename_object_vector_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_create_rewrite_rule_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_rewrite_rule");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateRewriteRuleDescriptor(c,ddl_create_rewrite_rule_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4139,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateRewriteRuleResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_rewrite_rule_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateRewriteRuleResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4139,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_rewrite_rule.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}if(ddl_create_or_replace_srs_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_or_replace_srs");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateOrReplaceSrsDescriptor(c,ddl_create_or_replace_srs_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4137,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateOrReplaceSrsResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_or_replace_srs_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateOrReplaceSrsResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4137,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_or_replace_srs.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_drop_srs_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_srs");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropSrsDescriptor(c,ddl_drop_srs_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4138,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropSrsResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_drop_srs_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropSrsResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4138,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_srs.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());}
if(ddl_create_schema_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_schema");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateSchemaDescriptor(c,ddl_create_schema_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4126,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateSchemaResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_create_schema_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateSchemaResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4126,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_schema.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_schema_executor\texecutor_id=engine.op.ddl_create_schema\topcode=SBLR_DDL_CREATE_SCHEMA\topcode_code=1536\topcode_version=1.0\toperand_descriptor_id=create_schema_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_schema_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_schema_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_create_table_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_table");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateTableDescriptor(c,ddl_create_table_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4126,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateTableResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_create_table_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateTableResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4126,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_table.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_table_executor\texecutor_id=engine.op.ddl_create_table\topcode=SBLR_DDL_CREATE_TABLE	opcode_code=1537\topcode_version=1.0\toperand_descriptor_id=create_table_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_table_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_table_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_create_index_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_create_index");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateIndexDescriptor(c,ddl_create_index_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4127,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlCreateIndexResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_create_index_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateIndexResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4127,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_index.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_create_index_executor\texecutor_id=engine.op.ddl_create_index\topcode=SBLR_DDL_CREATE_INDEX	opcode_code=1540\topcode_version=1.0\toperand_descriptor_id=create_index_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_index_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_create_index_availability_generation<<"\tparent_success_barrier=passed\n";}}
if(ddl_drop_index_root){auto c=receipt->engine_context;c.trace_tags.push_back("private_ddl_drop_index");auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropIndexDescriptor(c,ddl_drop_index_descriptor);if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4128,consumed.diagnostic.code,consumed.diagnostic.message_key);scratchbird::engine::sblr::SblrDdlDropIndexResultV1 rr;rr.body[24]=1;rr.body[25]=1;rr.body[56]=1;rr.availability=ddl_drop_index_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropIndexResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4128,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_index.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const auto digest=scratchbird::core::hash::ComputeSha256Digest(bytes);const char*path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(digest.ok()&&path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_drop_index_executor\texecutor_id=engine.op.ddl_drop_index\topcode=SBLR_DDL_DROP_INDEX	opcode_code=1541\topcode_version=1.0\toperand_descriptor_id=drop_index_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_drop_index_result_sha256=sha256:"<<scratchbird::core::hash::HexLower(digest.digest)<<"\texecutor_availability_generation="<<ddl_drop_index_availability_generation<<"\tparent_success_barrier=passed\n";}}
  if (ddl_create_sequence_root) {
    const auto& sequence_member = opcode_stream && stream.ok && stream.stream.operations.size() > 1 ? stream.stream.operations[1] : operation.envelope;
    std::string detail;
    if (sequence_member.operands.size() != 1 || sequence_member.operands.front().type != "create_sequence_descriptor" || sequence_member.operands.front().name != "sequence" || !scratchbird::engine::sblr::DecodeSblrDdlCreateSequenceDescriptorV1(sequence_member.operands.front().value_body.data(), sequence_member.operands.front().value_body.size(), &ddl_create_sequence_descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4133, "SBLR.OPERAND_INVALID", "sblr.ddl_create_sequence.operand_invalid", detail);
    auto c = receipt->engine_context; c.trace_tags.push_back("private_ddl_create_sequence"); c.trace_tags.push_back("private_ddl_create_package");
    auto consumed = scratchbird::engine::internal_api::ConsumeSblrDdlCreateSequenceDescriptor(c, ddl_create_sequence_descriptor);
    if (!consumed.ok) return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4133, consumed.diagnostic.code, consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlCreateSequenceResultV1 rr; rr.body[24] = 1; rr.body[56] = 1; rr.availability = ddl_create_sequence_descriptor.availability; rr.publication_barrier[0] = 1;
    auto bytes = scratchbird::engine::sblr::EncodeSblrDdlCreateSequenceResultV1(rr); if (bytes.empty()) return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4133, "SYSTEM.CONFIG_FAILED", "sblr.ddl_create_sequence.result_encoding_failed");
    result->result_kind = "ddl_result"; result->payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes); const char* path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"); if (digest.ok() && path && *path) { std::ofstream t(path, std::ios::app | std::ios::binary); if (t) t << "layer=ddl_create_sequence_executor\texecutor_id=engine.op.ddl_create_sequence\topcode=SBLR_DDL_CREATE_SEQUENCE\topcode_code=1671\topcode_version=1.0\toperand_descriptor_id=create_sequence_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_sequence_result_sha256=sha256:" << scratchbird::core::hash::HexLower(digest.digest) << "\texecutor_availability_generation=" << ddl_create_sequence_descriptor.availability << "\tparent_success_barrier=passed\n"; }
  }
  if (ddl_alter_sequence_root) {
    const auto& sequence_member = opcode_stream && stream.ok && stream.stream.operations.size() > 1 ? stream.stream.operations[1] : operation.envelope;
    std::string detail;
    scratchbird::engine::sblr::SblrDdlAlterSequenceDescriptorV1 descriptor;
    if (sequence_member.operands.size() != 1 || sequence_member.operands.front().type != "alter_sequence_descriptor" || sequence_member.operands.front().name != "sequence" || !scratchbird::engine::sblr::DecodeSblrDdlAlterSequenceDescriptorV1(sequence_member.operands.front().value_body.data(), sequence_member.operands.front().value_body.size(), &descriptor, &detail, true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4134, "SBLR.OPERAND_INVALID", "sblr.ddl_alter_sequence.operand_invalid", detail);
    auto c = receipt->engine_context; c.trace_tags.push_back("private_ddl_create_package");
    auto consumed = scratchbird::engine::internal_api::ConsumeSblrDdlAlterSequenceDescriptor(c, descriptor);
    if (!consumed.ok) return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4134, consumed.diagnostic.code, consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlAlterSequenceResultV1 rr; rr.body[24] = 1; rr.body[56] = 1; rr.availability = descriptor.availability; rr.publication_barrier[0] = 1;
    auto bytes = scratchbird::engine::sblr::EncodeSblrDdlAlterSequenceResultV1(rr); if (bytes.empty()) return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4134, "SYSTEM.CONFIG_FAILED", "sblr.ddl_alter_sequence.result_encoding_failed");
    result->result_kind = "ddl_result"; result->payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes); const char* path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE"); if (digest.ok() && path && *path) { std::ofstream t(path, std::ios::app | std::ios::binary); if (t) t << "layer=ddl_alter_sequence_executor\texecutor_id=engine.op.ddl_alter_sequence\topcode=SBLR_DDL_ALTER_SEQUENCE\topcode_code=1564\topcode_version=1.0\toperand_descriptor_id=alter_sequence_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_alter_sequence_result_sha256=sha256:" << scratchbird::core::hash::HexLower(digest.digest) << "\texecutor_availability_generation=" << descriptor.availability << "\tparent_success_barrier=passed\n"; }
  }
  if (ddl_drop_sequence_root) {
    const auto& sequence_member = opcode_stream && stream.ok && stream.stream.operations.size() > 1 ? stream.stream.operations[1] : operation.envelope;
    std::string detail; scratchbird::engine::sblr::SblrDdlDropSequenceDescriptorV1 descriptor;
    if (sequence_member.operands.size()!=1 || sequence_member.operands.front().type!="drop_sequence_descriptor" || sequence_member.operands.front().name!="sequence" || !scratchbird::engine::sblr::DecodeSblrDdlDropSequenceDescriptorV1(sequence_member.operands.front().value_body.data(),sequence_member.operands.front().value_body.size(),&descriptor,&detail,true)) return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,out_result,4135,"SBLR.OPERAND_INVALID","sblr.ddl_drop_sequence.operand_invalid",detail);
    auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_create_package"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropSequenceDescriptor(c,descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4135,consumed.diagnostic.code,consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlDropSequenceResultV1 rr;rr.body[24]=1;rr.body[56]=1;rr.availability=descriptor.availability;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropSequenceResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4135,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_sequence.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size());const char* path=std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");if(path&&*path){std::ofstream t(path,std::ios::app|std::ios::binary);if(t)t<<"layer=ddl_drop_sequence_executor\texecutor_id=engine.op.ddl_drop_sequence\topcode=SBLR_DDL_DROP_SEQUENCE\topcode_code=1565\topcode_version=1.0\toperand_descriptor_id=drop_sequence_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\texecutor_availability_generation="<<descriptor.availability<<"\tparent_success_barrier=passed\n";}
  }
  if (ddl_drop_type_root) {
    const auto& type_member = opcode_stream && stream.ok && stream.stream.operations.size() > 1
        ? stream.stream.operations[1] : operation.envelope;
    std::string detail;
    scratchbird::engine::sblr::SblrDdlDropTypeDescriptorV1 descriptor;
    if (type_member.operands.size() != 1 ||
        type_member.operands.front().type != "drop_type_descriptor" ||
        type_member.operands.front().name != "type" ||
        !scratchbird::engine::sblr::DecodeSblrDdlDropTypeDescriptorV1(
            type_member.operands.front().value_body.data(),
            type_member.operands.front().value_body.size(), &descriptor, &detail,
            true))
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4135,
                         "SBLR.OPERAND_INVALID",
                         "sblr.ddl_drop_type.operand_invalid", detail);
    auto c = receipt->engine_context;
    c.trace_tags.push_back("private_ddl_drop_type");
    const auto consumed = scratchbird::engine::internal_api::ConsumeSblrDdlDropTypeDescriptor(c, descriptor);
    if (!consumed.ok)
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4135,
                         consumed.diagnostic.code, consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlDropTypeResultV1 rr;
    rr.body[24] = 1;
    rr.body[56] = 1;
    rr.availability = descriptor.availability;
    rr.publication_barrier[0] = 1;
    auto bytes = scratchbird::engine::sblr::EncodeSblrDdlDropTypeResultV1(rr);
    if (bytes.empty())
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4135,
                         "SYSTEM.CONFIG_FAILED",
                         "sblr.ddl_drop_type.result_encoding_failed");
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const char* path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (path && *path) {
      std::ofstream trace(path, std::ios::app | std::ios::binary);
      if (trace)
        trace << "layer=ddl_drop_type_executor\texecutor_id=engine.op.ddl_drop_type"
              << "\topcode=SBLR_DDL_DROP_TYPE\topcode_code=1571\topcode_version=1.0"
              << "\toperand_descriptor_id=drop_type_descriptor\tresult_descriptor_id=ddl_result"
              << "\tresult_descriptor_version=1\texecutor_availability_generation="
              << descriptor.availability << "\tparent_success_barrier=passed\n";
    }
  }
  if (ddl_create_synonym_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_create_synonym"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateSynonymDescriptor(c,ddl_create_synonym_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4136,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlCreateSynonymResultV1 rr; rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_synonym_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateSynonymResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4136,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_synonym.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
  if (ddl_create_foreign_table_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_create_foreign_table"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateForeignTableDescriptor(c,ddl_create_foreign_table_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4137,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlCreateForeignTableResultV1 rr; rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_foreign_table_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateForeignTableResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4137,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_foreign_table.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
  if (ddl_create_fdw_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_create_fdw"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlCreateFdwDescriptor(c,ddl_create_fdw_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4157,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlCreateFdwResultV1 rr; rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_create_fdw_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlCreateFdwResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4157,"SYSTEM.CONFIG_FAILED","sblr.ddl_create_fdw.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
  if (ddl_drop_fdw_root) { auto c=receipt->engine_context; c.trace_tags.push_back("private_ddl_drop_fdw"); auto consumed=scratchbird::engine::internal_api::ConsumeSblrDdlDropFdwDescriptor(c,ddl_drop_fdw_descriptor); if(!consumed.ok)return fail_result(SB_ENGINE_STATUS_CONFLICT,out_result,4159,consumed.diagnostic.code,consumed.diagnostic.message_key); scratchbird::engine::sblr::SblrDdlDropFdwResultV1 rr; rr.body[24]=1;rr.body[56]=1;rr.availability=ddl_drop_fdw_availability_generation;rr.publication_barrier[0]=1;auto bytes=scratchbird::engine::sblr::EncodeSblrDdlDropFdwResultV1(rr);if(bytes.empty())return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR,out_result,4159,"SYSTEM.CONFIG_FAILED","sblr.ddl_drop_fdw.result_encoding_failed");result->result_kind="ddl_result";result->payload.assign(reinterpret_cast<const char*>(bytes.data()),bytes.size()); }
  if (ddl_rename_object_root) {
    auto c = receipt->engine_context;
    c.trace_tags.push_back("private_ddl_rename_object");
    const auto consumed = scratchbird::engine::internal_api::ConsumeSblrDdlRenameObjectDescriptor(c, ddl_rename_object_descriptor);
    if (!consumed.ok) return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4136, consumed.diagnostic.code, consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlRenameObjectResultV1 rr;
    rr.body[24] = 1; rr.body[56] = 1; rr.availability = ddl_rename_object_availability_generation; rr.publication_barrier[0] = 1;
    auto bytes = scratchbird::engine::sblr::EncodeSblrDdlRenameObjectResultV1(rr);
    if (bytes.empty()) return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4136, "SYSTEM.CONFIG_FAILED", "sblr.ddl_rename_object.result_encoding_failed");
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  if (ddl_create_table_as_query_with_data_root || ddl_create_table_as_query_with_no_data_root) {
    const auto& ctas_member = opcode_stream && stream.ok && stream.stream.operations.size() > 1
        ? stream.stream.operations[1] : operation.envelope;
    std::string detail;
    if (ctas_member.operands.size() != 1 ||
        !scratchbird::engine::sblr::DecodeSblrCreateTableAsQueryDescriptorV1(
            ctas_member.operands.front().value_body.data(),
            ctas_member.operands.front().value_body.size(), &ctas_descriptor, &detail))
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4134,
                         "SBLR.OPERAND_INVALID", "sblr.ddl_create_table_as_query.operand_invalid", detail);
    auto c = receipt->engine_context;
    c.trace_tags.push_back("private_ddl_create_table_as_query");
    const auto consumed = scratchbird::engine::internal_api::ConsumeSblrDdlCreateTableAsQueryDescriptor(c, ctas_descriptor);
    if (!consumed.ok)
      return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4134,
                         consumed.diagnostic.code, consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrCreateTableAsQueryResultV1 rr;
    rr.status = 1;
    rr.materialization = ddl_create_table_as_query_with_data_root ? 1 : 0;
    rr.table_uuid = ctas_descriptor.table_uuid;
    rr.catalog_epoch = ctas_descriptor.catalog_epoch;
    auto bytes = scratchbird::engine::sblr::EncodeSblrCreateTableAsQueryResultV1(rr);
    if (bytes.empty())
      return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4134,
                         "SYSTEM.CONFIG_FAILED", "sblr.ddl_create_table_as_query.result_encoding_failed");
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(bytes);
    const char* trace_path = std::getenv("SCRATCHBIRD_SBLR_DISPATCH_PHASE_TRACE_FILE");
    if (digest.ok() && trace_path && *trace_path) {
      std::ofstream trace(trace_path, std::ios::app | std::ios::binary);
      if (trace) {
        trace << "layer=ddl_create_materialized_view_executor\texecutor_id=engine.op.ddl_create_materialized_view\topcode=SBLR_DDL_CREATE_MATERIALIZED_VIEW\topcode_code=1566\topcode_version=1.0\toperand_descriptor_id=create_materialized_view_descriptor\tresult_descriptor_id=ddl_result\tresult_descriptor_version=1\tddl_create_materialized_view_result_sha256=sha256:" << scratchbird::core::hash::HexLower(digest.digest) << "\texecutor_availability_generation=" << ddl_create_materialized_view_availability_generation << "\tparent_success_barrier=passed\n";
      }
    }
  }
  if (ddl_create_materialized_view_root) {
    auto c = receipt->engine_context;
    c.trace_tags.push_back("private_ddl_create_materialized_view");
    const auto consumed = scratchbird::engine::internal_api::ConsumeSblrDdlCreateMaterializedViewDescriptor(c, ddl_create_materialized_view_descriptor);
    if (!consumed.ok) return fail_result(SB_ENGINE_STATUS_CONFLICT, out_result, 4140, consumed.diagnostic.code, consumed.diagnostic.message_key);
    scratchbird::engine::sblr::SblrDdlCreateMaterializedViewResultV1 rr;
    rr.body[24] = 1; rr.body[56] = 1;
    rr.availability = ddl_create_materialized_view_availability_generation;
    rr.publication_barrier[0] = 1;
    auto bytes = scratchbird::engine::sblr::EncodeSblrDdlCreateMaterializedViewResultV1(rr);
    if (bytes.empty()) return fail_result(SB_ENGINE_STATUS_INTERNAL_ERROR, out_result, 4140, "SYSTEM.CONFIG_FAILED", "sblr.ddl_create_materialized_view.result_encoding_failed");
    result->result_kind = "ddl_result";
    result->payload.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  finalize_diagnostics(result);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
}

bool CopyEngineDiagnosticFields(
    sb_engine_result_t result,
    std::size_t diagnostic_index,
    std::vector<EngineDiagnosticField>* fields) {
  if (fields == nullptr) return false;
  fields->clear();
  if (!valid_result(result) || diagnostic_index >= result->diagnostics.size()) {
    return false;
  }
  const auto& diagnostic = result->diagnostics[diagnostic_index];
  fields->reserve(diagnostic.fields.size());
  for (const auto& field : diagnostic.fields) {
    fields->push_back({field.key, field.value});
  }
  return true;
}

void SetPreparedMetadataBindingDispatchTestHookForTesting(
    PreparedMetadataBindingDispatchTestHook hook,
    void* context) {
  std::lock_guard<std::mutex> guard(
      g_prepared_metadata_dispatch_test_hook_mutex);
  g_prepared_metadata_dispatch_test_hook = hook;
  g_prepared_metadata_dispatch_test_hook_context = context;
}

sb_engine_status_t CreatePreparedMetadataBinding(
    sb_engine_session_t session,
    const sb_engine_request_context_v1_t* prepare_context,
    std::string_view sealed_prepare_transaction_uuid,
    const sb_engine_sblr_dispatch_params_v1_t* invoke_params,
    PreparedMetadataBindingHandle* out_binding,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_binding == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       1003,
                       "ENGINE.ABI.OUTPUT_POINTER_INVALID",
                       "engine.abi.output_pointer_invalid");
  }
  *out_binding = nullptr;
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,
                       out_result,
                       1007,
                       "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (prepare_context == nullptr || invoke_params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       1004,
                       "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(prepare_context->struct_size,
                             prepare_context->abi_version,
                             sizeof(sb_engine_request_context_v1_t),
                             out_result);
  if (status != SB_ENGINE_STATUS_OK) return status;
  status = check_struct(invoke_params->struct_size,
                        invoke_params->abi_version,
                        sizeof(sb_engine_sblr_dispatch_params_v1_t),
                        out_result);
  if (status != SB_ENGINE_STATUS_OK) return status;
  return fail_result(
      SB_ENGINE_STATUS_UNSUPPORTED,
      out_result,
      4032,
      "SBLR.ENVELOPE.FIELD_MISSING",
      "sblr.envelope.field_missing",
      "prepared binding requires an immutable server admission token");
#if 0
  if (!nonzero_uuid(prepare_context->effective_user_uuid) ||
      !nonzero_uuid(prepare_context->session_uuid) ||
      prepare_context->rights_set_ref == 0) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        2001,
        "SECURITY.IDENTITY.MISSING",
        "security.identity.missing",
        "prepared metadata binding requires principal, session, and rights context");
  }
  if (!same_uuid(prepare_context->effective_user_uuid,
                 session->effective_user_uuid) ||
      !same_uuid(prepare_context->session_uuid,
                 session->public_session_uuid) ||
      prepare_context->trust_mode != session->trust_mode) {
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4028,
        "ENGINE.PREPARED_METADATA_BINDING.SESSION_IDENTITY_MISMATCH",
        "engine.prepared_metadata_binding.session_identity_mismatch",
        "prepare identity must match the engine session identity");
  }
  if (prepare_context->transaction_ref == 0) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        3003,
        "ENGINE.PREPARED_METADATA_BINDING.TRANSACTION_REQUIRED",
        "engine.prepared_metadata_binding.transaction_required",
        "prepare_context.transaction_ref");
  }
  if (invoke_params->reserved0 != 0 || invoke_params->reserved1 != 0 ||
      invoke_params->data_packet_size_bytes != 0 ||
      invoke_params->data_packet_bytes != nullptr ||
      invoke_params->envelope_size_bytes == 0 ||
      invoke_params->envelope_bytes == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4017,
        "ENGINE.PREPARED_METADATA_BINDING.PARAMS_INVALID",
        "engine.prepared_metadata_binding.params_invalid",
        "binding create admits one envelope without data or reserved handles");
  }

  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      invoke_params->envelope_bytes, invoke_params->envelope_size_bytes);
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok ||
      decoded.envelope.payload_kind !=
          scratchbird::engine::SblrPayloadKind::operation_envelope ||
      !looks_like_sblr_operation_envelope(decoded.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4018,
        "ENGINE.PREPARED_METADATA_BINDING.SBLR_INVALID",
        "engine.prepared_metadata_binding.sblr_invalid",
        decoded.diagnostic_code.empty()
            ? "operation_envelope_required"
            : std::string(decoded.diagnostic_code));
  }
  const auto* canonical_data = reinterpret_cast<const char*>(
      decoded.envelope.canonical_bytes.data());
  const std::string_view canonical(
      canonical_data, decoded.envelope.canonical_bytes.size());
  const auto operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(canonical);
  if (!operation.ok ||
      operation.envelope.operation_id != "routine.procedure_invoke" ||
      operation.envelope.contains_sql_text ||
      !operation.envelope.parser_resolved_names_to_uuids ||
      !operation.envelope.requires_security_context ||
      !operation.envelope.requires_transaction_context ||
      has_engine_only_prepared_metadata_operand(operation.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED,
        out_result,
        4019,
        "ENGINE.PREPARED_METADATA_BINDING.ROUTINE_INVOKE_REQUIRED",
        "engine.prepared_metadata_binding.routine_invoke_required",
        "only engine-validated UUID-bound procedure invocation is supported");
  }
  const std::string target_object_uuid =
      operation_operand_value(operation.envelope, "target_object_uuid");
  const std::string target_object_kind =
      operation_operand_value(operation.envelope, "target_object_kind");
  if (target_object_uuid.empty() || target_object_kind != "procedure" ||
      !scratchbird::core::uuid::ParseUuid(target_object_uuid).ok()) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4020,
        "ENGINE.PREPARED_METADATA_BINDING.UUID_TARGET_REQUIRED",
        "engine.prepared_metadata_binding.uuid_target_required",
        target_object_uuid.empty() ? "target_object_uuid"
                                   : target_object_uuid);
  }

  const auto inventory =
      scratchbird::storage::database::LoadLocalTransactionInventoryFromDatabase(
          session->engine->database_path);
  if (!inventory.ok()) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4021,
        "ENGINE.PREPARED_METADATA_BINDING.INVENTORY_UNAVAILABLE",
        "engine.prepared_metadata_binding.inventory_unavailable",
        inventory.diagnostic.diagnostic_code);
  }
  const auto prepare_transaction =
      scratchbird::transaction::mga::LookupLocalTransaction(
          inventory.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              prepare_context->transaction_ref));
  using scratchbird::transaction::mga::TransactionState;
  if (!prepare_transaction.ok() ||
      (prepare_transaction.entry.state != TransactionState::active &&
       prepare_transaction.entry.state != TransactionState::read_only_active &&
       prepare_transaction.entry.state != TransactionState::preparing &&
       prepare_transaction.entry.state != TransactionState::prepared)) {
    return fail_result(
        SB_ENGINE_STATUS_TRANSACTION_REQUIRED,
        out_result,
        4022,
        "ENGINE.PREPARED_METADATA_BINDING.TRANSACTION_NOT_ACTIVE",
        "engine.prepared_metadata_binding.transaction_not_active",
        std::to_string(prepare_context->transaction_ref));
  }
  if (sealed_prepare_transaction_uuid.empty() ||
      !prepare_transaction.entry.identity.transaction_uuid.valid() ||
      scratchbird::core::uuid::UuidToString(
          prepare_transaction.entry.identity.transaction_uuid.value) !=
          sealed_prepare_transaction_uuid) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4032,
        "ENGINE.PREPARED_METADATA_BINDING.EXACT_MGA_SELECTOR_MISMATCH",
        "engine.prepared_metadata_binding.exact_mga_selector_mismatch",
        "prepare local transaction ID and sealed UUID do not match");
  }

  const std::uint64_t high_water =
      inventory.inventory.next_local_transaction_id == 0
          ? 0
          : inventory.inventory.next_local_transaction_id - 1;
  std::vector<std::uint64_t> active_excluded;
  std::vector<std::uint64_t> in_doubt_excluded;
  for (const auto& entry : inventory.inventory.entries) {
    if (!entry.identity.local_id.valid() ||
        entry.identity.local_id.value > high_water) {
      continue;
    }
    if (active_metadata_snapshot_exclusion(entry.state)) {
      active_excluded.push_back(entry.identity.local_id.value);
    } else if (in_doubt_metadata_snapshot_exclusion(entry.state)) {
      in_doubt_excluded.push_back(entry.identity.local_id.value);
    }
  }
  std::sort(active_excluded.begin(), active_excluded.end());
  std::sort(in_doubt_excluded.begin(), in_doubt_excluded.end());

  const std::string metadata_snapshot_uuid =
      new_prepared_metadata_snapshot_uuid();
  if (metadata_snapshot_uuid.empty()) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4023,
        "ENGINE.PREPARED_METADATA_BINDING.SNAPSHOT_ID_UNAVAILABLE",
        "engine.prepared_metadata_binding.snapshot_id_unavailable");
  }
  auto metadata_context =
      make_internal_context(session->engine, *prepare_context);
  metadata_context.statement_metadata_snapshot_engine_owned = true;
  metadata_context.statement_metadata_snapshot_uuid.canonical =
      metadata_snapshot_uuid;
  metadata_context
      .statement_metadata_snapshot_visible_through_local_transaction_id =
      high_water;
  metadata_context
      .statement_metadata_snapshot_active_excluded_local_transaction_ids =
      active_excluded;
  metadata_context
      .statement_metadata_snapshot_in_doubt_excluded_local_transaction_ids =
      in_doubt_excluded;
  const auto lifecycle =
      scratchbird::engine::internal_api::LoadExecutableObjectLifecycleState(
          metadata_context);
  if (!lifecycle.ok) {
    return fail_result(
        SB_ENGINE_STATUS_INTERNAL_ERROR,
        out_result,
        4024,
        lifecycle.diagnostic.code,
        lifecycle.diagnostic.message_key,
        lifecycle.diagnostic.detail);
  }
  const scratchbird::engine::internal_api::EngineExecutableObjectRecord*
      pinned_object = nullptr;
  for (const auto& object : lifecycle.state.objects) {
    if (object.object_uuid == target_object_uuid) {
      pinned_object = &object;
      break;
    }
  }
  if (pinned_object == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_NOT_FOUND,
        out_result,
        4025,
        "ENGINE.PREPARED_METADATA_BINDING.OBJECT_NOT_VISIBLE",
        "engine.prepared_metadata_binding.object_not_visible",
        target_object_uuid);
  }
  const auto creator =
      scratchbird::transaction::mga::LookupLocalTransaction(
          inventory.inventory,
          scratchbird::transaction::mga::MakeLocalTransactionId(
              pinned_object->creator_tx));
  if (!creator.ok() ||
      (creator.entry.state != TransactionState::committed &&
       creator.entry.state != TransactionState::archived)) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4026,
        "ENGINE.PREPARED_METADATA_BINDING.OBJECT_NOT_COMMITTED",
        "engine.prepared_metadata_binding.object_not_committed",
        target_object_uuid);
  }
  if (pinned_object->object_kind != "procedure" ||
      pinned_object->lifecycle_state != "active" ||
      pinned_object->deleted || pinned_object->invalidated ||
      pinned_object->executor_kind == "metadata_only" ||
      pinned_object->executable_generation == 0 ||
      pinned_object->metadata_epoch == 0) {
    return fail_result(
        SB_ENGINE_STATUS_CONFLICT,
        out_result,
        4027,
        "ENGINE.PREPARED_METADATA_BINDING.OBJECT_NOT_EXECUTABLE",
        "engine.prepared_metadata_binding.object_not_executable",
        target_object_uuid);
  }

  auto* binding = new PreparedMetadataBindingOpaque();
  binding->engine = session->engine;
  binding->session = session;
  binding->database_path = session->engine->database_path;
  binding->database_uuid = session->engine->database_uuid;
  binding->effective_user_uuid = prepare_context->effective_user_uuid;
  binding->session_uuid = prepare_context->session_uuid;
  binding->parser_package_uuid = prepare_context->parser_package_uuid;
  binding->dialect_profile_uuid = prepare_context->dialect_profile_uuid;
  binding->trust_mode = prepare_context->trust_mode;
  binding->context_flags = prepare_context->flags;
  binding->rights_set_ref = prepare_context->rights_set_ref;
  binding->capability_set_ref = prepare_context->capability_set_ref;
  binding->source_artifact_set_ref =
      prepare_context->source_artifact_set_ref;
  binding->encoded_sblr_envelope.assign(
      invoke_params->envelope_bytes,
      invoke_params->envelope_bytes + invoke_params->envelope_size_bytes);
  binding->metadata_snapshot_uuid = metadata_snapshot_uuid;
  binding->metadata_visible_through_local_transaction_id = high_water;
  binding->active_excluded_local_transaction_ids =
      std::move(active_excluded);
  binding->in_doubt_excluded_local_transaction_ids =
      std::move(in_doubt_excluded);
  binding->target_object_uuid = target_object_uuid;
  binding->target_executable_generation =
      pinned_object->executable_generation;
  binding->target_metadata_epoch = pinned_object->metadata_epoch;
  binding->target_creator_local_transaction_id = pinned_object->creator_tx;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_prepared_metadata_binding_registry_mutex);
    g_prepared_metadata_bindings.insert(binding);
  }
  *out_binding = binding;

  if (out_result != nullptr) {
    auto* result = make_result(SB_ENGINE_RESULT_COMMAND_COMPLETION,
                               "sblr.prepared_metadata_binding.create");
    result->payload =
        "metadata_snapshot_uuid=" + metadata_snapshot_uuid + "\n" +
        "target_object_uuid=" + target_object_uuid + "\n" +
        "executable_generation=" +
        std::to_string(binding->target_executable_generation) + "\n" +
        "metadata_epoch=" +
        std::to_string(binding->target_metadata_epoch) + "\n";
    finalize_diagnostics(result);
    *out_result = result;
  }
  return SB_ENGINE_STATUS_OK;
#endif
}

sb_engine_status_t ReleasePreparedMetadataBinding(
    PreparedMetadataBindingHandle binding) {
  if (binding == nullptr) return SB_ENGINE_STATUS_OK;
  {
    std::lock_guard<std::mutex> registry_guard(
        g_prepared_metadata_binding_registry_mutex);
    const auto found = g_prepared_metadata_bindings.find(binding);
    if (found == g_prepared_metadata_bindings.end()) {
      return SB_ENGINE_STATUS_INVALID_HANDLE;
    }
    {
      std::lock_guard<std::mutex> binding_guard(binding->mutex);
      if (binding->released ||
          binding->magic != kPreparedMetadataBindingMagic) {
        return SB_ENGINE_STATUS_ALREADY_RELEASED;
      }
      binding->released = true;
      binding->magic = 0;
    }
    g_prepared_metadata_bindings.erase(found);
  }
  delete binding;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t DispatchWithPreparedMetadataBinding(
    sb_engine_session_t session,
    sb_engine_transaction_t transaction,
    const sb_engine_request_context_v1_t* context,
    const sb_engine_sblr_dispatch_params_v1_t* params,
    PreparedMetadataBindingHandle binding,
    sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) { return SB_ENGINE_STATUS_INVALID_ARGUMENT; }
  if (!valid_session(session) || binding == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,
                       out_result,
                       1007,
                       "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (transaction != nullptr &&
      (!valid_transaction(transaction) || transaction->session != session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE,
                       out_result,
                       1007,
                       "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (context == nullptr || params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       1004,
                       "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(context->struct_size,
                             context->abi_version,
                             sizeof(sb_engine_request_context_v1_t),
                             out_result);
  if (status != SB_ENGINE_STATUS_OK) { return status; }
  status = check_struct(params->struct_size,
                        params->abi_version,
                        sizeof(sb_engine_sblr_dispatch_params_v1_t),
                        out_result);
  if (status != SB_ENGINE_STATUS_OK) { return status; }
  return fail_result(
      SB_ENGINE_STATUS_UNSUPPORTED,
      out_result,
      4032,
      "SBLR.ENVELOPE.FIELD_MISSING",
      "sblr.envelope.field_missing",
      "prepared dispatch requires an immutable server admission token");
#if 0
  if (params->reserved0 != 0 || params->reserved1 != 0) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       4014,
                       "ENGINE.ABI.RESERVED_FIELD_INVALID",
                       "engine.abi.reserved_field_invalid",
                       "sblr_dispatch_params.reserved0_or_reserved1");
  }
  if (!nonzero_uuid(context->effective_user_uuid) ||
      !nonzero_uuid(context->session_uuid)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED,
                       out_result,
                       2001,
                       "SECURITY.IDENTITY.MISSING",
                       "security.identity.missing");
  }
  if (params->envelope_size_bytes == 0 || params->envelope_bytes == nullptr) {
    return fail_result(
        SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4001,
        "SBLR.ENVELOPE.INVALID",
        "sblr.envelope.invalid",
        "prepared metadata dispatch requires one operation envelope");
  }
  const auto decoded = scratchbird::engine::DecodeSblrEnvelopeBytes(
      params->envelope_bytes, params->envelope_size_bytes);
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok) {
    return fail_result(
        decoded.status ==
                scratchbird::engine::SblrCodecStatus::version_unsupported
            ? SB_ENGINE_STATUS_UNSUPPORTED
            : SB_ENGINE_STATUS_INVALID_ARGUMENT,
        out_result,
        4001,
        std::string(decoded.diagnostic_code),
        std::string(decoded.message_key));
  }
  if (!looks_like_sblr_operation_envelope(decoded.envelope)) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED,
        out_result,
        4029,
        "ENGINE.PREPARED_METADATA_BINDING.OPERATION_ENVELOPE_REQUIRED",
        "engine.prepared_metadata_binding.operation_envelope_required");
  }
  // PreparedMetadataBinding is a private routed-server bridge, not a public or
  // embedded ABI handle. The server's database-owner lock excludes another
  // mutating process; this engine-internal guard orders same-process durable
  // transaction publication. Keep it through dispatch so the exact version
  // revalidated below is the immutable version acquired and executed.
  const auto inventory_guard =
      scratchbird::engine::internal_api::AcquireTransactionInventoryGuard(
          session->engine->database_path);
  PreparedMetadataBindingSnapshot prepared_metadata;
  std::string revalidation_detail;
  const auto revalidation =
      revalidate_prepared_metadata_binding_current_version(
          binding,
          session,
          *context,
          *params,
          &prepared_metadata,
          &revalidation_detail);
  if (revalidation != PreparedMetadataCurrentVersionStatus::ok) {
    if (revalidation == PreparedMetadataCurrentVersionStatus::stale) {
      return fail_result(
          SB_ENGINE_STATUS_CONFLICT,
          out_result,
          4030,
          "ENGINE.PREPARED_METADATA_BINDING.STALE",
          "engine.prepared_metadata_binding.stale",
          revalidation_detail);
    }
    if (revalidation == PreparedMetadataCurrentVersionStatus::unavailable) {
      return fail_result(
          SB_ENGINE_STATUS_INTERNAL_ERROR,
          out_result,
          4031,
          "ENGINE.PREPARED_METADATA_BINDING.REVALIDATION_UNAVAILABLE",
          "engine.prepared_metadata_binding.revalidation_unavailable",
          revalidation_detail);
    }
    return fail_result(
        SB_ENGINE_STATUS_SECURITY_DENIED,
        out_result,
        4015,
        "ENGINE.PREPARED_METADATA_BINDING.CONTEXT_MISMATCH",
        "engine.prepared_metadata_binding.context_mismatch",
        revalidation_detail);
  }
  invoke_prepared_metadata_dispatch_test_hook(
      "exact_version_acquired_under_inventory_guard");
  return dispatch_operation_envelope(
      session,
      *context,
      decoded.envelope,
      *params,
      &prepared_metadata,
      out_result);
#endif
}

}  // namespace scratchbird::server_engine_bridge

extern "C" {

sb_engine_status_t sb_engine_dispatch_sblr(sb_engine_session_t session,
                                           sb_engine_transaction_t transaction,
                                           const sb_engine_request_context_v1_t* context,
                                           const sb_engine_sblr_dispatch_params_v1_t* params,
                                           sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_session(session)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (transaction != nullptr && !valid_transaction(transaction)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (context == nullptr || params == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 1004, "ENGINE.ABI.PARAMETER_NULL",
                       "engine.abi.parameter_null");
  }
  auto status = check_struct(context->struct_size, context->abi_version, sizeof(sb_engine_request_context_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  status = check_struct(params->struct_size, params->abi_version, sizeof(sb_engine_sblr_dispatch_params_v1_t), out_result);
  if (status != SB_ENGINE_STATUS_OK) {
    return status;
  }
  if (params->reserved0 != 0 || params->reserved1 != 0) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT,
                       out_result,
                       4014,
                       "ENGINE.ABI.RESERVED_FIELD_INVALID",
                       "engine.abi.reserved_field_invalid",
                       "sblr_dispatch_params.reserved0_or_reserved1");
  }
  if (params->envelope_size_bytes != 0) {
    return fail_result(
        SB_ENGINE_STATUS_UNSUPPORTED,
        out_result,
        4032,
        "SBLR.ENVELOPE.FIELD_MISSING",
        "sblr.envelope.field_missing",
        "public ABI revision requires an immutable server admission token");
  }
  if (!nonzero_uuid(context->effective_user_uuid) || !nonzero_uuid(context->session_uuid)) {
    return fail_result(SB_ENGINE_STATUS_SECURITY_DENIED, out_result, 2001, "SECURITY.IDENTITY.MISSING",
                       "security.identity.missing");
  }
  if (params->envelope_size_bytes != 0 && params->envelope_bytes == nullptr) {
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4001, "SBLR.ENVELOPE.INVALID",
                       "sblr.envelope.invalid", "null envelope pointer with non-zero length");
  }
  if (params->envelope_size_bytes == 0) {
    auto* result = make_result(SB_ENGINE_RESULT_CAPABILITY_REPORT, "sblr.dispatch.capability");
    result->payload = "SBLR dispatch facade active; empty envelope treated as capability probe";
    finalize_diagnostics(result);
    *out_result = result;
    return SB_ENGINE_STATUS_OK;
  }
  auto phase_last = EngineAbiSteadyClock::now();
  std::vector<std::pair<std::string, std::uint64_t>> phase_micros;
  phase_micros.reserve(4);
  const auto mark_phase = [&](std::string phase) {
    const auto now = EngineAbiSteadyClock::now();
    phase_micros.push_back({std::move(phase), EngineAbiElapsedMicros(phase_last, now)});
    phase_last = now;
  };
  const auto decoded =
      scratchbird::engine::DecodeSblrEnvelopeBytes(params->envelope_bytes, params->envelope_size_bytes);
  mark_phase("decode_sblr_envelope_bytes");
  if (decoded.status != scratchbird::engine::SblrCodecStatus::ok) {
    WriteEngineAbiPhaseTrace("dispatch_sblr",
                             "decode_rejected",
                             static_cast<std::size_t>(params->envelope_size_bytes),
                             phase_micros);
    const std::string code(decoded.diagnostic_code);
    const std::string key(decoded.message_key);
    if (decoded.status == scratchbird::engine::SblrCodecStatus::version_unsupported) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4003, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::reference_meta_forbidden) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4004, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::descriptor_invalid) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4005, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::opcode_unknown) {
      return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4006, code, key);
    }
    if (decoded.status == scratchbird::engine::SblrCodecStatus::checksum_invalid) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4007, code, key);
    }
    return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 4001, code, key);
  }
  const std::string_view embedded_bytes(
      reinterpret_cast<const char*>(decoded.envelope.canonical_bytes.data()),
      decoded.envelope.canonical_bytes.size());
  const auto embedded_operation =
      scratchbird::engine::sblr::DecodeSblrEnvelope(embedded_bytes);
  if (embedded_operation.ok &&
      (embedded_operation.envelope.operation_id == "engine.op.ddl_refresh_materialized_view" || embedded_operation.envelope.operation_id == "engine.op.ddl_create_materialized_view" || embedded_operation.envelope.operation_id == "engine.op.ddl_create_sequence" || embedded_operation.envelope.operation_id == "engine.op.ddl_alter_sequence" || embedded_operation.envelope.operation_id == "engine.op.ddl_drop_type" || embedded_operation.envelope.operation_id == "engine.op.ddl_rename_object" || embedded_operation.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_data" || embedded_operation.envelope.operation_id == "engine.op.ddl_create_table_as_query_with_no_data") &&
      (embedded_operation.envelope.opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW" || embedded_operation.envelope.opcode == "SBLR_DDL_CREATE_MATERIALIZED_VIEW" || embedded_operation.envelope.opcode == "SBLR_DDL_CREATE_SEQUENCE" || embedded_operation.envelope.opcode == "SBLR_DDL_ALTER_SEQUENCE" || embedded_operation.envelope.opcode == "SBLR_DDL_DROP_TYPE" || embedded_operation.envelope.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA" || embedded_operation.envelope.opcode == "SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA") &&
      (embedded_operation.envelope.opcode_code == 1567 || embedded_operation.envelope.opcode_code == 1566 || embedded_operation.envelope.opcode_code == 1671 || embedded_operation.envelope.opcode_code == 1564 || embedded_operation.envelope.opcode_code == 1571 || embedded_operation.envelope.opcode_code == 1669 || embedded_operation.envelope.opcode_code == 1670)) {
    const auto status = dispatch_operation_envelope(
        session, *context, decoded.envelope, *params, nullptr, out_result);
    mark_phase("embedded_refresh_operation_envelope");
    return status;
  }
  if (looks_like_sblr_operation_envelope(decoded.envelope)) {
    const auto status = dispatch_operation_envelope(
        session, *context, decoded.envelope, *params, nullptr, out_result);
    mark_phase("dispatch_operation_envelope");
    WriteEngineAbiPhaseTrace("dispatch_sblr",
                             "operation_envelope",
                             static_cast<std::size_t>(params->envelope_size_bytes),
                             phase_micros);
    return status;
  }
  const auto* row =
      scratchbird::engine::FindSblrPriorityDRegistryRow(decoded.envelope.family, decoded.envelope.opcode);
  if (row == nullptr) {
    return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4006, "SBLR.OPCODE.UNKNOWN",
                       "sblr.opcode.unknown");
  }
  if (row->behavior_status == scratchbird::engine::SblrBehaviorStatus::noncluster_fail_closed ||
      row->behavior_status == scratchbird::engine::SblrBehaviorStatus::capability_fail_closed ||
      row->behavior_status == scratchbird::engine::SblrBehaviorStatus::edition_fail_closed) {
    return fail_result(SB_ENGINE_STATUS_CAPABILITY_DISABLED, out_result, 4008, std::string(row->diagnostic_code),
                       "sblr.capability.forbidden", std::string(row->family_name));
  }
  if (row->behavior_status == scratchbird::engine::SblrBehaviorStatus::implemented) {
    auto* result = make_result(SB_ENGINE_RESULT_COMMAND_COMPLETION, std::string(row->family_name));
    result->payload = "accepted";
    finalize_diagnostics(result);
    *out_result = result;
    return SB_ENGINE_STATUS_OK;
  }
  return fail_result(SB_ENGINE_STATUS_UNSUPPORTED, out_result, 4002, "SBLR.EXECUTION.ADMISSION_ONLY",
                     "sblr.execution.admission_only", std::string(row->family_name));
}

sb_engine_status_t sb_engine_result_release(sb_engine_result_t result) {
  if (result == nullptr) {
    return SB_ENGINE_STATUS_OK;
  }
  if (result->magic != kResultMagic) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  {
    std::lock_guard<std::mutex> guard(result->mutex);
    if (result->released) {
      return SB_ENGINE_STATUS_ALREADY_RELEASED;
    }
    result->released = true;
    result->magic = 0;
  }
  delete result;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_class(sb_engine_result_t result, sb_engine_result_class_t* out_class) {
  if (out_class == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  *out_class = result->result_class;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_completion(sb_engine_result_t result,
                                               sb_engine_command_completion_view_v1_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  *out_view = {};
  out_view->struct_size = sizeof(*out_view);
  out_view->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  set_view(out_view->operation_id, result->operation_id);
  out_view->affected_rows = result->affected_rows;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_summary(sb_engine_result_t result,
                                            sb_engine_execution_summary_view_v1_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  *out_view = {};
  out_view->struct_size = sizeof(*out_view);
  out_view->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  out_view->rows_produced = result->rows_produced;
  out_view->diagnostics_count = static_cast<std::uint64_t>(result->diagnostics.size());
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_diagnostics(sb_engine_result_t result,
                                                sb_engine_diagnostic_set_view_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  finalize_diagnostics(result);
  *out_view = {};
  out_view->struct_size = sizeof(*out_view);
  out_view->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  out_view->diagnostics = result->diagnostic_views.empty() ? nullptr : result->diagnostic_views.data();
  out_view->diagnostic_count = static_cast<std::uint64_t>(result->diagnostic_views.size());
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_payload(sb_engine_result_t result, sb_engine_string_view_t* out_view) {
  if (out_view == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  set_view(*out_view, result->payload);
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_result_next_batch(sb_engine_result_t result,
                                               const sb_engine_batch_request_v1_t* request,
                                               sb_engine_row_batch_view_v1_t* out_batch) {
  if (out_batch == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_result(result)) {
    return SB_ENGINE_STATUS_INVALID_HANDLE;
  }
  if (request != nullptr) {
    if (request->struct_size < sizeof(sb_engine_batch_request_v1_t) ||
        request->abi_version != SB_ENGINE_ABI_VERSION_PACKED) {
      return SB_ENGINE_STATUS_INVALID_ARGUMENT;
    }
  }
  *out_batch = {};
  out_batch->struct_size = sizeof(*out_batch);
  out_batch->abi_version = SB_ENGINE_ABI_VERSION_PACKED;
  std::lock_guard<std::mutex> guard(result->mutex);
  if ((result->query_execute_result_handle_validated ||
       result->admitted_query_row_stream_renderer) &&
      !(result->query_execute_result_handle_validated &&
        result->admitted_query_row_stream_renderer)) {
    return SB_ENGINE_STATUS_CONFLICT;
  }
  const std::uint64_t total_rows = static_cast<std::uint64_t>(result->row_values.size());
  const std::uint64_t remaining =
      total_rows > result->next_row_index ? total_rows - result->next_row_index : 0;
  if (remaining == 0 || result->result_class != SB_ENGINE_RESULT_ROW_BATCH) {
    result->payload.clear();
    out_batch->end_of_stream = 1;
    return SB_ENGINE_STATUS_OK;
  }
  const std::uint64_t requested_rows =
      request != nullptr && request->max_rows != 0 ? request->max_rows : remaining;
  const std::uint64_t row_count = std::min(requested_rows, remaining);
  const std::uint64_t first_row = result->next_row_index;
  result->payload = api_result_payload(result->operation_id,
                                       result->result_kind,
                                       result->row_values,
                                       result->row_metadata_values,
                                       result->evidence_values,
                                       first_row,
                                       row_count);
  result->next_row_index += row_count;
  out_batch->row_count = row_count;
  out_batch->end_of_stream = result->next_row_index >= total_rows ? 1 : 0;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_describe_capabilities(sb_engine_handle_t engine,
                                                   const sb_engine_capability_request_v1_t* request,
                                                   sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (request != nullptr) {
    auto status = check_struct(request->struct_size, request->abi_version, sizeof(sb_engine_capability_request_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
  }
  auto* result = make_result(SB_ENGINE_RESULT_CAPABILITY_REPORT, "engine.describe_capabilities");
  result->payload = behavior_payload();
  finalize_diagnostics(result);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
}

sb_engine_status_t sb_engine_metric_root(sb_engine_handle_t engine,
                                         const sb_engine_metric_request_v1_t* request,
                                         sb_engine_result_t* out_result) {
  clear_result(out_result);
  if (out_result == nullptr) {
    return SB_ENGINE_STATUS_INVALID_ARGUMENT;
  }
  if (!valid_engine(engine)) {
    return fail_result(SB_ENGINE_STATUS_INVALID_HANDLE, out_result, 1007, "ENGINE.ABI.INVALID_HANDLE",
                       "engine.abi.invalid_handle");
  }
  if (request != nullptr) {
    auto status = check_struct(request->struct_size, request->abi_version, sizeof(sb_engine_metric_request_v1_t), out_result);
    if (status != SB_ENGINE_STATUS_OK) {
      return status;
    }
    if (!valid_string_span(request->root_path_utf8, request->root_path_size)) {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 5001, "ENGINE.METRIC.ROOT_INVALID",
                         "engine.metric.root_invalid");
    }
    const std::string_view root_path(request->root_path_utf8,
                                     static_cast<std::size_t>(request->root_path_size));
    if (root_path != "sys.metrics.engine") {
      return fail_result(SB_ENGINE_STATUS_INVALID_ARGUMENT, out_result, 5001, "ENGINE.METRIC.ROOT_INVALID",
                         "engine.metric.root_invalid");
    }
  }
  auto* result = make_result(SB_ENGINE_RESULT_METRIC_ROOT, "engine.metric_root");
  result->payload = "sys.metrics.engine.abi;sys.metrics.engine.dispatch;sys.metrics.sblr.envelope";
  finalize_diagnostics(result);
  *out_result = result;
  return SB_ENGINE_STATUS_OK;
}

}  // extern "C"
