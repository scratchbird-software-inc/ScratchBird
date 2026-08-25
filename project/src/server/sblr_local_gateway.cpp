// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "sblr_local_gateway.hpp"
#include <algorithm>

#include "core/hash/hash_digest.hpp"
#include "engine/sblr/sblr_opcode_registry.hpp"
#include "engine/sblr/sblr_opcode_stream.hpp"
#include "engine/sblr/sblr_transaction_begin_runtime.hpp"
#include "engine/sblr/sblr_transaction_commit_runtime.hpp"
#include "engine/sblr/sblr_transaction_rollback_runtime.hpp"
#include "engine/sblr/sblr_savepoint_runtime.hpp"
#include "engine/sblr/sblr_autonomous_frame_runtime.hpp"
#include "engine/sblr/sblr_reservation_release_runtime.hpp"
#include "engine/sblr/sblr_temporary_instance_cleanup_runtime.hpp"
#include "engine/sblr/sblr_cursor_open_runtime.hpp"
#include "engine/sblr/sblr_cursor_fetch_runtime.hpp"
#include "engine/sblr/sblr_cursor_close_runtime.hpp"
#include "engine/sblr/sblr_read_by_key_runtime.hpp"
#include "engine/sblr/sblr_read_range_runtime.hpp"
#include "engine/sblr/sblr_read_stream_runtime.hpp"
#include "engine/sblr/sblr_local_metrics_read.hpp"
#include "engine/sblr/sblr_catalog_introspect_runtime.hpp"
#include "engine/sblr/sblr_sec_alter_user_runtime.hpp"
#include "engine/sblr/sblr_sec_drop_policy_runtime.hpp"
#include "engine/sblr/sblr_sec_create_role_runtime.hpp"
#include "engine/sblr/sblr_sec_drop_role_runtime.hpp"
#include "engine/sblr/sblr_sec_alter_role_runtime.hpp"
#include "engine/sblr/sblr_sec_create_group_mapping_runtime.hpp"
#include "engine/sblr/sblr_sec_drop_group_mapping_runtime.hpp"
#include "engine/sblr/sblr_sec_grant_runtime.hpp"
#include "engine/sblr/sblr_sec_revoke_runtime.hpp"
#include "engine/sblr/sblr_sec_alter_policy_runtime.hpp"
#include "engine/sblr/sblr_sec_drop_user_runtime.hpp"
#include "engine/sblr/sblr_sec_authenticate_runtime.hpp"
#include "engine/sblr/sblr_sec_deauthenticate_runtime.hpp"
#include "engine/sblr/sblr_session_role_switch_runtime.hpp"
#include "engine/sblr/sblr_session_setting_set_runtime.hpp"
#include "engine/sblr/sblr_session_setting_reset_runtime.hpp"
#include "engine/sblr/sblr_session_setting_get_runtime.hpp"
#include "engine/sblr/sblr_session_default_qualifier_runtime.hpp"
#include "engine/sblr/sblr_session_discard_runtime.hpp"
#include "engine/sblr/sblr_session_snapshot_handle_runtime.hpp"
#include "engine/sblr/sblr_sec_create_policy_runtime.hpp"
#include "engine/sblr/sblr_kv_structured_stream_append_runtime.hpp"
#include "engine/sblr/sblr_kv_structured_timeseries_runtime.hpp"
#include "engine/sblr/sblr_system_config_set_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_domain_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_sequence_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_schema_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_table_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_table_as_query_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_index_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_index_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_domain_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_trigger_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_trigger_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_trigger_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_procedure_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_procedure_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_procedure_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_function_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_function_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_function_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_package_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_synonym_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_foreign_table_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_fdw_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_fdw_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_foreign_table_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_package_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_synonym_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_package_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_sequence_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_sequence_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_materialized_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_type_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_table_runtime.hpp"
#include "engine/sblr/sblr_ddl_refresh_materialized_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_materialized_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_temporary_table_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_temporary_table_runtime.hpp"
#include "engine/sblr/sblr_ddl_rename_object_vector_runtime.hpp"
#include "engine/sblr/sblr_ddl_rename_object_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_or_replace_srs_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_srs_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_rewrite_rule_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_rewrite_rule_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_rewrite_rule_runtime.hpp"
#include "engine/sblr/sblr_ddl_validate_constraint_runtime.hpp"
#include "engine/sblr/sblr_security_create_privilege_template_runtime.hpp"
#include "engine/sblr/sblr_security_create_user_runtime.hpp"
#include "engine/sblr/sblr_security_alter_privilege_template_runtime.hpp"
#include "engine/sblr/sblr_security_drop_privilege_template_runtime.hpp"
#include "engine/sblr/sblr_database_create_template_clone_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_aggregate_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_aggregate_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_aggregate_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_dictionary_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_dictionary_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_continuous_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_continuous_view_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_continuous_view_runtime.hpp"
#include "engine/sblr/sblr_dml_async_insert_submit_runtime.hpp"
#include "engine/sblr/sblr_dml_async_insert_status_runtime.hpp"
#include "engine/sblr/sblr_dml_async_insert_cancel_runtime.hpp"
#include "engine/sblr/sblr_dml_counter_add_runtime.hpp"
#include "engine/sblr/sblr_dml_timeseries_schema_write_runtime.hpp"
#include "engine/sblr/sblr_ddl_timeseries_series_cardinality_policy_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_timeseries_value_cache_runtime.hpp"
#include "engine/sblr/sblr_ddl_purge_system_history_runtime.hpp"
#include "engine/sblr/sblr_ddl_set_index_optimizer_eligibility_runtime.hpp"
#include "engine/sblr/sblr_ddl_set_table_type_enforcement_runtime.hpp"
#include "engine/sblr/sblr_database_serialize_logical_snapshot_runtime.hpp"
#include "engine/sblr/sblr_database_deserialize_logical_snapshot_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_macro_runtime.hpp"
#include "engine/sblr/sblr_ddl_drop_macro_runtime.hpp"
#include "engine/sblr/sblr_admin_register_external_relation_resolver_runtime.hpp"
#include "engine/sblr/sblr_admin_unregister_external_relation_resolver_runtime.hpp"
#include "engine/sblr/sblr_ddl_create_dictionary_runtime.hpp"
#include "engine/sblr/sblr_ddl_alter_view_runtime.hpp"
#include "engine/sblr/sblr_result_set_pass_runtime.hpp"
#include "engine/sblr/sblr_access_cursor_open_runtime.hpp"
#include "engine/sblr/sblr_access_cursor_fetch_runtime.hpp"
#include "engine/sblr/sblr_access_cursor_close_runtime.hpp"
#include "engine/sblr/sblr_insert_runtime.hpp"
#include "engine/sblr/sblr_update_runtime.hpp"
#include "engine/sblr/sblr_delete_runtime.hpp"
#include "engine/sblr/sblr_merge_runtime.hpp"
#include "engine/sblr/sblr_table_truncate_runtime.hpp"
#include "engine/sblr/sblr_table_analyze_runtime.hpp"
#include "engine/sblr/sblr_bulk_import_stream_runtime.hpp"
#include "engine/sblr/sblr_bulk_export_stream_runtime.hpp"
#include "engine/sblr/sblr_statement_batch_runtime.hpp"
#include "engine/sblr/sblr_atomic_cas_runtime.hpp"
#include "engine/sblr/sblr_atomic_read_modify_write_runtime.hpp"
#include "engine/sblr/sblr_advisory_lock_runtime.hpp"
#include "engine/sblr/sblr_advisory_lock_release_runtime.hpp"
#include "engine/sblr/sblr_function_call_runtime.hpp"
#include "engine/sblr/sblr_operator_call_runtime.hpp"
#include "engine/sblr/sblr_cast_runtime.hpp"
#include "engine/sblr/sblr_compare_runtime.hpp"
#include "engine/sblr/sblr_domain_operation_runtime.hpp"
#include "engine/sblr/sblr_udr_invoke_runtime.hpp"
#include "engine/sblr/sblr_procedure_invoke_runtime.hpp"
#include "engine/sblr/sblr_function_invoke_runtime.hpp"
#include "engine/sblr/sblr_aggregate_invoke_runtime.hpp"
#include "engine/sblr/sblr_sequence_nextval_runtime.hpp"
#include "engine/sblr/sblr_sequence_currval_runtime.hpp"
#include "engine/sblr/sblr_sequence_setval_runtime.hpp"
#include "engine/sblr/sblr_query_numeric_runtime.hpp"
#include "engine/sblr/sblr_advanced_datatype_family_runtime.hpp"
#include "engine/sblr/sblr_project_runtime.hpp"
#include "engine/sblr/sblr_aggregate_runtime.hpp"
#include "engine/sblr/sblr_group_runtime.hpp"
#include "engine/sblr/sblr_sort_runtime.hpp"
#include "engine/sblr/sblr_limit_runtime.hpp"
#include "engine/sblr/sblr_window_runtime.hpp"
#include "engine/sblr/sblr_return_result_set_runtime.hpp"
#include "engine/sblr/sblr_kv_structured_read_runtime.hpp"
#include "engine/sblr/sblr_kv_structured_mutate_runtime.hpp"
#include "engine/sblr/sblr_kv_structured_scan_runtime.hpp"
#include "engine/sblr/sblr_kv_structured_stream_read_runtime.hpp"

#include <algorithm>

namespace scratchbird::server {
namespace {

bool CanonicalNonzeroUuid(const std::string& value) {
  if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') return false;
  bool nonzero = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '-') continue;
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return false;
    nonzero = nonzero || ch != '0';
  }
  return nonzero;
}

LocalSblrGatewayDecision Refuse(const LocalSblrGatewayRequest& request,
                                std::string diagnostic_id) {
  LocalSblrGatewayDecision decision;
  decision.diagnostic_id = std::move(diagnostic_id);
  decision.route_snapshot_uuid = request.route_snapshot_uuid;
  decision.route_epoch = request.route_epoch;
  decision.route_generation = request.route_generation;
  decision.security_snapshot_uuid = request.security_snapshot_uuid;
  decision.security_epoch = request.security_epoch;
  decision.security_observation_generation =
      request.security_observation_generation;
  decision.cluster_context_active = request.cluster_context_active;
  decision.cluster_transaction_active = request.cluster_transaction_active;
  decision.route_fence_present = request.route_fence_present;
  if (!request.canonical_sbos.empty()) {
    const auto digest = scratchbird::core::hash::ComputeSha256Digest(
        request.canonical_sbos);
    if (digest.ok()) decision.canonical_payload_sha256 = digest.digest;
  }
  return decision;
}

}  // namespace

LocalSblrGatewayDecision AdmitLocalNoClusterSblrGateway(
    const LocalSblrGatewayRequest& request) {
  const std::string_view encoded(
      reinterpret_cast<const char*>(request.canonical_sbos.data()),
      request.canonical_sbos.size());
  const auto stream = scratchbird::engine::sblr::DecodeSblrOpcodeStream(encoded);
  const bool exact_query = (request.root_opcode_code == 0x1207u &&
      request.root_opcode == "SBLR_QUERY_EXECUTE" &&
      request.root_operation_id == "query.execute") ||
      (request.root_opcode_code == 1567 &&
       request.root_opcode == "SBLR_DDL_REFRESH_MATERIALIZED_VIEW" &&
       request.root_operation_id == "engine.op.ddl_refresh_materialized_view") ||
      (request.root_opcode_code == 1568 &&
       request.root_opcode == "SBLR_DDL_DROP_MATERIALIZED_VIEW" &&
       request.root_operation_id == "engine.op.ddl_drop_materialized_view");
  const bool exact_diagnostic_refusal = request.root_opcode_code == 0x1900u &&
      request.root_opcode == "SBLR_DIAGNOSTIC_REFUSAL" &&
      request.root_operation_id == "engine.op.diagnostic_refusal";
  bool exact_local_diagnostic_refusal = stream.ok && stream.stream.operations.size() == 3 &&
      exact_diagnostic_refusal && !request.cluster_context_active &&
      !request.cluster_transaction_active && !request.route_fence_present &&
      stream.stream.operations[1].operands.empty();
  const bool exact_window = request.root_opcode_code == 1285 && request.root_opcode == "SBLR_WINDOW" && request.root_operation_id == "engine.op.window";
  const bool exact_show_version = request.root_opcode_code == 3334 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_VERSION" &&
      request.root_operation_id == "observability.show_version";
  const bool exact_show_database = request.root_opcode_code == 3335 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_DATABASE" &&
      request.root_operation_id == "observability.show_database";
  const bool exact_show_catalog = request.root_opcode_code == 3337 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_CATALOG" &&
      request.root_operation_id == "observability.show_catalog";
  const bool exact_show_sessions = request.root_opcode_code == 3338 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_SESSIONS" &&
      request.root_operation_id == "observability.show_sessions";
  const bool exact_show_transactions = request.root_opcode_code == 3339 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_TRANSACTIONS" &&
      request.root_operation_id == "observability.show_transactions";
  const bool exact_show_locks = request.root_opcode_code == 3340 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_LOCKS" &&
      request.root_operation_id == "observability.show_locks";
  const bool exact_show_statements = request.root_opcode_code == 3341 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_STATEMENTS" &&
      request.root_operation_id == "observability.show_statements";
  const bool exact_show_jobs = request.root_opcode_code == 3342 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_JOBS" &&
      request.root_operation_id == "observability.show_jobs";
  const bool exact_show_management = request.root_opcode_code == 3343 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_MANAGEMENT" &&
      request.root_operation_id == "observability.show_management";
  const bool exact_show_diagnostics = request.root_opcode_code == 3360 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS" &&
      request.root_operation_id == "observability.show_diagnostics";
  const bool exact_show_diagnostics_extended = request.root_opcode_code == 3361 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_DIAGNOSTICS_EXTENDED" &&
      request.root_operation_id == "observability.show_diagnostics_extended";
  const bool exact_show_archive_replication = request.root_opcode_code == 3362 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_ARCHIVE_REPLICATION" &&
      request.root_operation_id == "observability.show_archive_replication";
  const bool exact_show_agents_extended = request.root_opcode_code == 3363 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_AGENTS_EXTENDED" &&
      request.root_operation_id == "observability.show_agents_extended";
  const bool exact_show_filespace_extended = request.root_opcode_code == 3364 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_FILESPACE_EXTENDED" &&
      request.root_operation_id == "observability.show_filespace_extended";
  const bool exact_show_acceleration = request.root_opcode_code == 3365 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_ACCELERATION" &&
      request.root_operation_id == "observability.show_acceleration";
  const bool exact_show_acceleration_extended = request.root_opcode_code == 3366 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_ACCELERATION_EXTENDED" &&
      request.root_operation_id == "observability.show_acceleration_extended";
  const bool exact_show_decision_service = request.root_opcode_code == 3367 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_DECISION_SERVICE" &&
      request.root_operation_id == "observability.show_decision_service";
  const bool exact_explain_operation = request.root_opcode_code == 3369 &&
      request.root_opcode == "SBLR_OBSERVABILITY_EXPLAIN_OPERATION" &&
      request.root_operation_id == "observability.explain_operation";
  const bool exact_lifecycle_create_database = request.root_opcode_code == 5128 &&
      request.root_opcode == "SBLR_LIFECYCLE_CREATE_DATABASE" &&
      request.root_operation_id == "engine.op.lifecycle_create_database";
  if (exact_lifecycle_create_database)
    return Refuse(request, request.cluster_context_active ? "CLUSTER.ROUTE_REFUSED" : "SB_ENGINE_API_LIFECYCLE_BOOTSTRAP_REQUIRED");
  const bool exact_lifecycle_open_database = request.root_opcode_code == 5129 &&
      request.root_opcode == "SBLR_LIFECYCLE_OPEN_DATABASE" &&
      request.root_operation_id == "engine.op.lifecycle_open_database";
  if (exact_lifecycle_open_database && request.cluster_context_active)
    return Refuse(request, "CLUSTER.ROUTE_REFUSED");
  const bool exact_local_lifecycle_open_database =
      stream.ok && stream.stream.operations.size() == 3 && exact_lifecycle_open_database &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present;
  const bool exact_lifecycle_attach_database = request.root_opcode_code == 5130 &&
      request.root_opcode == "SBLR_LIFECYCLE_ATTACH_DATABASE" &&
      request.root_operation_id == "engine.op.lifecycle_attach_database";
  const bool exact_local_lifecycle_attach_database =
      stream.ok && stream.stream.operations.size() == 3 && exact_lifecycle_attach_database &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_lifecycle_attach_database && !exact_local_lifecycle_attach_database)
    return Refuse(request, request.cluster_context_active ? "CLUSTER.ROUTE_REFUSED" : "SBLR.OPERAND.INVALID");
  const bool exact_lifecycle_detach_database = request.root_opcode_code == 5131 &&
      request.root_opcode == "SBLR_LIFECYCLE_DETACH_DATABASE" &&
      request.root_operation_id == "engine.op.lifecycle_detach_database";
  const bool exact_local_lifecycle_detach_database =
      stream.ok && stream.stream.operations.size() == 3 && exact_lifecycle_detach_database &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_lifecycle_detach_database && !exact_local_lifecycle_detach_database)
    return Refuse(request, request.cluster_context_active ? "CLUSTER.ROUTE_REFUSED" : "SBLR.OPERAND.INVALID");
  const bool exact_lifecycle_enter_maintenance = request.root_opcode_code == 5132 &&
      request.root_opcode == "SBLR_LIFECYCLE_ENTER_MAINTENANCE" &&
      request.root_operation_id == "engine.op.lifecycle_enter_maintenance";
  const bool exact_local_lifecycle_enter_maintenance =
      stream.ok && stream.stream.operations.size() == 3 && exact_lifecycle_enter_maintenance &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_lifecycle_enter_maintenance && !exact_local_lifecycle_enter_maintenance)
    return Refuse(request, request.cluster_context_active ? "CLUSTER.ROUTE_REFUSED" : "SBLR.OPERAND.INVALID");
  const bool exact_lifecycle_exit_maintenance = request.root_opcode_code == 5133 &&
      request.root_opcode == "SBLR_LIFECYCLE_EXIT_MAINTENANCE" &&
      request.root_operation_id == "engine.op.lifecycle_exit_maintenance";
  const bool exact_local_lifecycle_exit_maintenance =
      stream.ok && stream.stream.operations.size() == 3 && exact_lifecycle_exit_maintenance &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_lifecycle_exit_maintenance && !exact_local_lifecycle_exit_maintenance)
    return Refuse(request, request.cluster_context_active ? "CLUSTER.ROUTE_REFUSED" : "SBLR.OPERAND.INVALID");
  const bool exact_show_metrics = request.root_opcode_code == 3370 &&
      request.root_opcode == "SBLR_OBSERVABILITY_SHOW_METRICS" &&
      request.root_operation_id == "observability.show_metrics";
  bool exact_local_show_version = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_show_version &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 &&
        member.operands.front().type == "observability_show_version_descriptor" &&
        member.operands.front().name == "show_version" &&
        member.operands.front().value_kind ==
            scratchbird::engine::sblr::SblrValueKind::observability_show_version_descriptor) {
      const auto& body = member.operands.front().value_body;
      exact_local_show_version = body.size() == 64 && body[0] == 'S' &&
          body[1] == 'V' && body[2] == 'D' && body[3] == 'O' &&
          body[4] == 1 && body[5] == 0 &&
          std::all_of(body.begin() + 6, body.end(),
                      [](std::uint8_t byte) { return byte == 0; });
    }
  }
  if (exact_show_version && !exact_local_show_version)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_database =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_database &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present &&
      stream.stream.operations[1].operands.empty();
  if (exact_show_database && !exact_local_show_database)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_catalog =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_catalog &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_catalog && !exact_local_show_catalog)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_sessions =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_sessions &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_sessions && !exact_local_show_sessions)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_transactions =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_transactions &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_transactions && !exact_local_show_transactions)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_locks =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_locks &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_locks && !exact_local_show_locks)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_statements =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_statements &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_statements && !exact_local_show_statements)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_jobs =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_jobs &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_jobs && !exact_local_show_jobs)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_management =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_management &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_management && !exact_local_show_management)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_diagnostics =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_diagnostics &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_diagnostics && !exact_local_show_diagnostics)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_diagnostics_extended =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_diagnostics_extended &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_diagnostics_extended && !exact_local_show_diagnostics_extended)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_archive_replication =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_archive_replication &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_archive_replication && !exact_local_show_archive_replication)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_agents_extended =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_agents_extended &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_agents_extended && !exact_local_show_agents_extended)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_filespace_extended =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_filespace_extended &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_filespace_extended && !exact_local_show_filespace_extended)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_acceleration =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_acceleration &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_acceleration && !exact_local_show_acceleration)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_acceleration_extended =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_acceleration_extended &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_acceleration_extended && !exact_local_show_acceleration_extended)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_decision_service =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_decision_service &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_decision_service && !exact_local_show_decision_service)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_explain_operation =
      stream.ok && stream.stream.operations.size() == 3 && exact_explain_operation &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.size() == 1 &&
      stream.stream.operations[1].operands.front().type == "observability_explain_operation_descriptor";
  if (exact_explain_operation && !exact_local_explain_operation)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_local_show_metrics =
      stream.ok && stream.stream.operations.size() == 3 && exact_show_metrics &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present && stream.stream.operations[1].operands.empty();
  if (exact_show_metrics && !exact_local_show_metrics)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_metrics_read = request.root_opcode_code == 0x0C01 &&
      request.root_opcode == "SBLR_READ_METRICS" &&
      request.root_operation_id == "engine.op.read_metrics";
  bool exact_local_metrics_read = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_metrics_read &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 &&
        member.operands.front().type == "metrics.read_request.v1" &&
        member.operands.front().name == "request" &&
        member.operands.front().value_kind ==
            scratchbird::engine::sblr::SblrValueKind::literal_typed) {
      scratchbird::engine::sblr::SblrOperationEnvelope envelope;
      envelope.operation_id = request.root_operation_id;
      envelope.opcode = request.root_opcode;
      envelope.opcode_code = request.root_opcode_code;
      envelope.operands = member.operands;
      exact_local_metrics_read =
          scratchbird::engine::sblr::DecodeSblrLocalMetricsReadOperand(envelope).ok;
    }
  }
  if (exact_metrics_read && !exact_local_metrics_read)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_catalog_introspect = request.root_opcode_code == 0x1300 &&
      request.root_opcode == "SBLR_CATALOG_INTROSPECT" &&
      request.root_operation_id == "engine.op.catalog_introspect";
  bool exact_local_catalog_introspect = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_catalog_introspect &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 && member.operands.front().type == "catalog_introspect_descriptor" &&
        member.operands.front().name == "object_detail" &&
        member.operands.front().value_kind == scratchbird::engine::sblr::SblrValueKind::catalog_introspect_descriptor) {
      scratchbird::engine::sblr::SblrCatalogIntrospectDescriptorV1 descriptor;
      std::string detail;
      exact_local_catalog_introspect = scratchbird::engine::sblr::DecodeSblrCatalogIntrospectDescriptorV1(
          member.operands.front().value_body.data(), member.operands.front().value_body.size(),
          &descriptor, &detail, true);
    }
  }
  if (exact_catalog_introspect && !exact_local_catalog_introspect)
    return Refuse(request, "SBLR.OPERAND.INVALID");
  bool exact_local_window = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_window && !request.cluster_context_active && !request.cluster_transaction_active && !request.route_fence_present) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 && member.operands.front().type == "window_descriptor" && member.operands.front().name == "window" && member.operands.front().value_kind == scratchbird::engine::sblr::SblrValueKind::window_descriptor) {
      scratchbird::engine::sblr::SblrWindowDescriptorV1 operand; std::string detail;
      exact_local_window = scratchbird::engine::sblr::DecodeSblrWindowDescriptorV1(member.operands.front().value_body.data(), member.operands.front().value_body.size(), &operand, &detail, true);
    }
  }
  if (exact_window && !exact_local_window) return Refuse(request, "SBLR.OPERAND.INVALID");
  const bool exact_source_map = request.root_opcode_code == 6 &&
      request.root_opcode == "SBLR_SOURCE_MAP" &&
      request.root_operation_id == "engine.op.source_map";
  const bool exact_error_vector = request.root_opcode_code == 7 &&
      request.root_opcode == "SBLR_ERROR_VECTOR" &&
      request.root_operation_id == "engine.op.error_vector";
  const auto* root_registry_entry =
      scratchbird::engine::sblr::LookupSblrOpcodeCode(request.root_opcode_code);
  const bool exact_cluster_root =
      stream.ok && stream.stream.operations.size() == 3 &&
      root_registry_entry != nullptr &&
      root_registry_entry->operation_id == request.root_operation_id &&
      root_registry_entry->opcode == request.root_opcode &&
      (root_registry_entry->requires_cluster_authority ||
       root_registry_entry->cluster_private);
  const bool exact_cluster_inspect_provider = request.root_opcode_code == 2877 &&
      request.root_opcode == "SBLR_CLUSTER_INSPECT_PROVIDER" &&
      request.root_operation_id == "cluster.inspect_provider";
  if (exact_cluster_inspect_provider &&
      (!stream.ok || stream.stream.operations.size() != 3 ||
       !request.cluster_context_active || request.cluster_transaction_active ||
       !stream.stream.operations[1].operands.empty())) {
    return Refuse(request, "CLUSTER.GATEWAY.CLUSTER_CONTEXT_REQUIRED");
  }
  const bool exact_txn_begin = request.root_opcode_code == 256 &&
      request.root_opcode == "SBLR_TXN_BEGIN" &&
      request.root_operation_id == "engine.op.txn_begin";
  bool exact_local_txn_begin = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_txn_begin) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 &&
        member.operands.front().value_kind ==
            scratchbird::engine::sblr::SblrValueKind::transaction_begin_options) {
      scratchbird::engine::sblr::SblrTransactionBeginOptionsV1 options;
      std::string detail;
      exact_local_txn_begin = scratchbird::engine::sblr::
          DecodeSblrTransactionBeginOptionsV1(
              member.operands.front().value_body.data(),
              member.operands.front().value_body.size(), &options, &detail) &&
          options.authority_scope == 1;
    }
  }
  const bool exact_txn_commit = request.root_opcode_code == 257 &&
      request.root_opcode == "SBLR_TXN_COMMIT" &&
      request.root_operation_id == "engine.op.txn_commit";
  bool exact_local_txn_commit = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_txn_commit) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 &&
        member.operands.front().value_kind ==
            scratchbird::engine::sblr::SblrValueKind::transaction_commit_options) {
      scratchbird::engine::sblr::SblrTransactionCommitOptionsV1 options;
      std::string detail;
      exact_local_txn_commit = scratchbird::engine::sblr::
          DecodeSblrTransactionCommitOptionsV1(
              member.operands.front().value_body.data(),
              member.operands.front().value_body.size(), &options, &detail) &&
          options.authority_scope == 1;
    }
  }
  const bool exact_txn_rollback = request.root_opcode_code == 258 &&
      request.root_opcode == "SBLR_TXN_ROLLBACK" &&
      request.root_operation_id == "engine.op.txn_rollback";
  bool exact_local_txn_rollback = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_txn_rollback) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 &&
        member.operands.front().value_kind ==
            scratchbird::engine::sblr::SblrValueKind::transaction_rollback_options) {
      scratchbird::engine::sblr::SblrTransactionRollbackOptionsV1 options;
      std::string detail;
      exact_local_txn_rollback = scratchbird::engine::sblr::
          DecodeSblrTransactionRollbackOptionsV1(
              member.operands.front().value_body.data(),
              member.operands.front().value_body.size(), &options, &detail) &&
          options.authority_scope == 1;
    }
  }
  const bool exact_txn_savepoint = request.root_opcode_code == 259 &&
      request.root_opcode == "SBLR_TXN_SAVEPOINT" &&
      request.root_operation_id == "engine.op.txn_savepoint";
  bool exact_local_txn_savepoint = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_txn_savepoint) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 && member.operands.front().type == "savepoint.descriptor" &&
        member.operands.front().name == "savepoint" &&
        member.operands.front().value_kind == scratchbird::engine::sblr::SblrValueKind::savepoint_descriptor) {
      scratchbird::engine::sblr::SblrSavepointDescriptorV1 descriptor;
      std::string detail;
      exact_local_txn_savepoint = scratchbird::engine::sblr::DecodeSblrSavepointDescriptorV1(
          member.operands.front().value_body.data(),member.operands.front().value_body.size(),&descriptor,&detail);
    }
  }
  const bool exact_txn_release_savepoint = request.root_opcode_code == 260 &&
      request.root_opcode == "SBLR_TXN_RELEASE_SAVEPOINT" &&
      request.root_operation_id == "engine.op.txn_release_savepoint";
  bool exact_local_txn_release_savepoint = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_txn_release_savepoint) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 && member.operands.front().type == "savepoint.release_handle" &&
        member.operands.front().name == "savepoint" && member.operands.front().value_kind ==
            scratchbird::engine::sblr::SblrValueKind::savepoint_release_handle) {
      scratchbird::engine::sblr::SblrSavepointReleaseOperandV1 operand;
      std::string detail;
      exact_local_txn_release_savepoint = scratchbird::engine::sblr::DecodeSblrSavepointReleaseOperandV1(
          member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail);
    }
  }
  const bool exact_txn_rollback_to_savepoint=request.root_opcode_code==261&&request.root_opcode=="SBLR_TXN_ROLLBACK_TO_SAVEPOINT"&&request.root_operation_id=="engine.op.txn_rollback_to_savepoint";
  bool exact_local_txn_rollback_to_savepoint=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_txn_rollback_to_savepoint){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="savepoint.rollback_handle"&&member.operands.front().name=="savepoint"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::savepoint_rollback_handle){scratchbird::engine::sblr::SblrSavepointRollbackOperandV1 operand;std::string detail;exact_local_txn_rollback_to_savepoint=scratchbird::engine::sblr::DecodeSblrSavepointRollbackOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail);}}
  const bool exact_psql_autonomous_frame=request.root_opcode_code==262&&request.root_opcode=="SBLR_PSQL_AUTONOMOUS_FRAME"&&request.root_operation_id=="engine.op.psql_autonomous_frame";
  bool exact_local_psql_autonomous_frame=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_psql_autonomous_frame){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="autonomous_frame_descriptor"&&member.operands.front().name=="autonomous_frame"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::psql_autonomous_frame_descriptor){scratchbird::engine::sblr::SblrAutonomousFrameDescriptorV1 descriptor;std::string detail;exact_local_psql_autonomous_frame=scratchbird::engine::sblr::DecodeSblrAutonomousFrameDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&descriptor,&detail,true);}}
  const bool exact_reservation_release=request.root_opcode_code==263&&request.root_opcode=="SBLR_TRANSACTION_RESERVATION_RELEASE"&&request.root_operation_id=="engine.op.transaction_reservation_release";bool exact_local_reservation_release=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_reservation_release){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="relation_reservation_release_descriptor"&&member.operands.front().name=="reservation"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::relation_reservation_release_descriptor){scratchbird::engine::sblr::SblrReservationReleaseDescriptorV1 descriptor;std::string detail;exact_local_reservation_release=scratchbird::engine::sblr::DecodeSblrReservationReleaseDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&descriptor,&detail,true);}}
  const bool exact_temporary_cleanup=request.root_opcode_code==264&&request.root_opcode=="SBLR_TEMPORARY_INSTANCE_CLEANUP"&&request.root_operation_id=="engine.op.temporary_instance_cleanup";bool exact_local_temporary_cleanup=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_temporary_cleanup){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="temporary_instance_cleanup_descriptor"&&member.operands.front().name=="temporary_instance"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::temporary_instance_cleanup_descriptor){scratchbird::engine::sblr::SblrTemporaryInstanceCleanupDescriptorV1 descriptor;std::string detail;exact_local_temporary_cleanup=scratchbird::engine::sblr::DecodeSblrTemporaryInstanceCleanupDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&descriptor,&detail,true);}}
  const bool exact_cursor_open=request.root_opcode_code==512&&request.root_opcode=="SBLR_CURSOR_OPEN"&&request.root_operation_id=="engine.op.cursor_open";bool exact_local_cursor_open=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_cursor_open){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="cursor_open_plan_ref"&&member.operands.front().name=="plan"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::cursor_open_plan_ref){scratchbird::engine::sblr::SblrCursorOpenDescriptorV1 descriptor;std::string detail;exact_local_cursor_open=scratchbird::engine::sblr::DecodeSblrCursorOpenDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&descriptor,&detail,true);}}
  const bool exact_cursor_fetch=request.root_opcode_code==513&&request.root_opcode=="SBLR_CURSOR_FETCH"&&request.root_operation_id=="engine.op.cursor_fetch";bool exact_local_cursor_fetch=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_cursor_fetch){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="cursor_fetch_handle"&&member.operands.front().name=="cursor"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::cursor_fetch_handle){scratchbird::engine::sblr::SblrCursorFetchOperandV1 operand;std::string detail;exact_local_cursor_fetch=scratchbird::engine::sblr::DecodeSblrCursorFetchOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail);}}
  const bool exact_cursor_close=request.root_opcode_code==514&&request.root_opcode=="SBLR_CURSOR_CLOSE"&&request.root_operation_id=="engine.op.cursor_close";bool exact_local_cursor_close=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_cursor_close){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="cursor_close_handle"&&member.operands.front().name=="cursor"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::cursor_close_handle){scratchbird::engine::sblr::SblrCursorCloseOperandV1 operand;std::string detail;exact_local_cursor_close=scratchbird::engine::sblr::DecodeSblrCursorCloseOperandV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail);}}
  const bool exact_read_by_key=request.root_opcode_code==515&&request.root_opcode=="SBLR_READ_BY_KEY"&&request.root_operation_id=="engine.op.read_by_key";bool exact_local_read_by_key=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_read_by_key){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="uuid_object_key_descriptor"&&member.operands.front().name=="key"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::read_by_key_descriptor){scratchbird::engine::sblr::SblrReadByKeyDescriptorV1 operand;std::string detail;exact_local_read_by_key=scratchbird::engine::sblr::DecodeSblrReadByKeyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_read_range=request.root_opcode_code==516&&request.root_opcode=="SBLR_READ_RANGE"&&request.root_operation_id=="engine.op.read_range";bool exact_local_read_range=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_read_range){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="range_scan_descriptor"&&member.operands.front().name=="range"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::read_range_descriptor){scratchbird::engine::sblr::SblrReadRangeDescriptorV1 operand;std::string detail;exact_local_read_range=scratchbird::engine::sblr::DecodeSblrReadRangeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_read_stream=request.root_opcode_code==517&&request.root_opcode=="SBLR_READ_STREAM"&&request.root_operation_id=="engine.op.read_stream";bool exact_local_read_stream=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_read_stream){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="stream_descriptor"&&member.operands.front().name=="stream"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::read_stream_descriptor){scratchbird::engine::sblr::SblrReadStreamDescriptorV1 operand;std::string detail;exact_local_read_stream=scratchbird::engine::sblr::DecodeSblrReadStreamDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_result_set_pass=request.root_opcode_code==518&&request.root_opcode=="SBLR_RESULT_SET_PASS"&&request.root_operation_id=="engine.op.result_set_pass";bool exact_local_result_set_pass=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_result_set_pass){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="result_set_handle_and_lifetime"&&member.operands.front().name=="result_set"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::result_set_pass_descriptor){scratchbird::engine::sblr::SblrResultSetPassDescriptorV1 operand;std::string detail;exact_local_result_set_pass=scratchbird::engine::sblr::DecodeSblrResultSetPassDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_access_cursor_open=request.root_opcode_code==519&&request.root_opcode=="SBLR_ACCESS_CURSOR_OPEN"&&request.root_operation_id=="engine.op.access_cursor_open";bool exact_local_access_cursor_open=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_access_cursor_open){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="access_cursor_open_descriptor"&&member.operands.front().name=="access_cursor"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::access_cursor_open_descriptor){scratchbird::engine::sblr::SblrAccessCursorOpenDescriptorV1 operand;std::string detail;exact_local_access_cursor_open=scratchbird::engine::sblr::DecodeSblrAccessCursorOpenDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_access_cursor_fetch=request.root_opcode_code==520&&request.root_opcode=="SBLR_ACCESS_CURSOR_FETCH"&&request.root_operation_id=="engine.op.access_cursor_fetch";bool exact_local_access_cursor_fetch=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_access_cursor_fetch){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="access_cursor_fetch_descriptor"&&member.operands.front().name=="access_cursor"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::access_cursor_fetch_descriptor){scratchbird::engine::sblr::SblrAccessCursorFetchDescriptorV1 operand;std::string detail;exact_local_access_cursor_fetch=scratchbird::engine::sblr::DecodeSblrAccessCursorFetchDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_access_cursor_close=request.root_opcode_code==521&&request.root_opcode=="SBLR_ACCESS_CURSOR_CLOSE"&&request.root_operation_id=="engine.op.access_cursor_close";bool exact_local_access_cursor_close=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_access_cursor_close){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="access_cursor_close_descriptor"&&member.operands.front().name=="access_cursor"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::access_cursor_close_descriptor){scratchbird::engine::sblr::SblrAccessCursorCloseDescriptorV1 operand;std::string detail;exact_local_access_cursor_close=scratchbird::engine::sblr::DecodeSblrAccessCursorCloseDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_insert=request.root_opcode_code==768&&request.root_opcode=="SBLR_INSERT"&&request.root_operation_id=="engine.op.insert";bool exact_local_insert=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_insert&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="insert_descriptor"&&member.operands.front().name=="insert"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::insert_descriptor){scratchbird::engine::sblr::SblrInsertDescriptorV1 operand;std::string detail;exact_local_insert=scratchbird::engine::sblr::DecodeSblrInsertDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_update=request.root_opcode_code==769&&request.root_opcode=="SBLR_UPDATE"&&request.root_operation_id=="engine.op.update";bool exact_local_update=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_update&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="update_descriptor"&&member.operands.front().name=="update"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::update_descriptor){scratchbird::engine::sblr::SblrUpdateDescriptorV1 operand;std::string detail;exact_local_update=scratchbird::engine::sblr::DecodeSblrUpdateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_delete=request.root_opcode_code==770&&request.root_opcode=="SBLR_DELETE"&&request.root_operation_id=="engine.op.delete";bool exact_local_delete=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_delete&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="delete_descriptor"&&member.operands.front().name=="delete"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::delete_descriptor){scratchbird::engine::sblr::SblrDeleteDescriptorV1 operand;std::string detail;exact_local_delete=scratchbird::engine::sblr::DecodeSblrDeleteDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_merge=request.root_opcode_code==771&&request.root_opcode=="SBLR_MERGE"&&request.root_operation_id=="engine.op.merge";bool exact_local_merge=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_merge&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="merge_descriptor"&&member.operands.front().name=="merge"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::merge_descriptor){scratchbird::engine::sblr::SblrMergeDescriptorV1 operand;std::string detail;exact_local_merge=scratchbird::engine::sblr::DecodeSblrMergeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_table_truncate=request.root_opcode_code==773&&request.root_opcode=="SBLR_TABLE_TRUNCATE"&&request.root_operation_id=="engine.op.table_truncate";bool exact_local_table_truncate=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_table_truncate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="truncate_table_descriptor"&&member.operands.front().name=="truncate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::truncate_table_descriptor){scratchbird::engine::sblr::SblrTableTruncateDescriptorV1 operand;std::string detail;exact_local_table_truncate=scratchbird::engine::sblr::DecodeSblrTableTruncateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_table_analyze=request.root_opcode_code==774&&request.root_opcode=="SBLR_TABLE_ANALYZE"&&request.root_operation_id=="engine.op.table_analyze";bool exact_local_table_analyze=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_table_analyze&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="analyze_table_descriptor"&&member.operands.front().name=="analyze"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::analyze_table_descriptor){scratchbird::engine::sblr::SblrTableAnalyzeDescriptorV1 operand;std::string detail;exact_local_table_analyze=scratchbird::engine::sblr::DecodeSblrTableAnalyzeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_bulk_import_stream=request.root_opcode_code==775&&request.root_opcode=="SBLR_BULK_IMPORT_STREAM"&&request.root_operation_id=="engine.op.bulk_import_stream";bool exact_local_bulk_import_stream=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_bulk_import_stream&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="bulk_import_stream_descriptor"&&member.operands.front().name=="bulk_import"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::bulk_import_stream_descriptor){scratchbird::engine::sblr::SblrBulkImportStreamDescriptorV1 operand;std::string detail;exact_local_bulk_import_stream=scratchbird::engine::sblr::DecodeSblrBulkImportStreamDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_bulk_export_stream=request.root_opcode_code==776&&request.root_opcode=="SBLR_BULK_EXPORT_STREAM"&&request.root_operation_id=="engine.op.bulk_export_stream";bool exact_local_bulk_export_stream=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_bulk_export_stream&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="bulk_export_stream_descriptor"&&member.operands.front().name=="bulk_export"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::bulk_export_stream_descriptor){scratchbird::engine::sblr::SblrBulkExportStreamDescriptorV1 operand;std::string detail;exact_local_bulk_export_stream=scratchbird::engine::sblr::DecodeSblrBulkExportStreamDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_statement_batch=request.root_opcode_code==777&&request.root_opcode=="SBLR_STATEMENT_BATCH"&&request.root_operation_id=="engine.op.statement_batch";bool exact_local_statement_batch=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_statement_batch&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="statement_batch_descriptor"&&member.operands.front().name=="batch"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::statement_batch_descriptor){scratchbird::engine::sblr::SblrStatementBatchDescriptorV1 operand;std::string detail;exact_local_statement_batch=scratchbird::engine::sblr::DecodeSblrStatementBatchDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_atomic_cas=request.root_opcode_code==778&&request.root_opcode=="SBLR_ATOMIC_CAS"&&request.root_operation_id=="engine.op.atomic_cas";bool exact_local_atomic_cas=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_atomic_cas&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="atomic_cas_descriptor"&&member.operands.front().name=="cas"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::atomic_cas_descriptor){scratchbird::engine::sblr::SblrAtomicCasDescriptorV1 operand;std::string detail;exact_local_atomic_cas=scratchbird::engine::sblr::DecodeSblrAtomicCasDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_atomic_rmw=request.root_opcode_code==779&&request.root_opcode=="SBLR_ATOMIC_READ_MODIFY_WRITE"&&request.root_operation_id=="engine.op.atomic_read_modify_write";bool exact_local_atomic_rmw=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_atomic_rmw&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="atomic_rmw_descriptor"&&member.operands.front().name=="rmw"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::atomic_rmw_descriptor){scratchbird::engine::sblr::SblrAtomicRmwDescriptorV1 operand;std::string detail;exact_local_atomic_rmw=scratchbird::engine::sblr::DecodeSblrAtomicRmwDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_advisory_lock=request.root_opcode_code==780&&request.root_opcode=="SBLR_ADVISORY_LOCK_ACQUIRE"&&request.root_operation_id=="engine.op.advisory_lock_acquire";bool exact_local_advisory_lock=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_advisory_lock&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="advisory_lock_descriptor"&&member.operands.front().name=="lock"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::advisory_lock_descriptor){scratchbird::engine::sblr::SblrAdvisoryLockDescriptorV1 operand;std::string detail;exact_local_advisory_lock=scratchbird::engine::sblr::DecodeSblrAdvisoryLockDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_advisory_lock_release=request.root_opcode_code==781&&request.root_opcode=="SBLR_ADVISORY_LOCK_RELEASE"&&request.root_operation_id=="engine.op.advisory_lock_release";bool exact_local_advisory_lock_release=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_advisory_lock_release&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="advisory_lock_release_descriptor"&&member.operands.front().name=="release"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::advisory_lock_release_descriptor){scratchbird::engine::sblr::SblrAdvisoryLockReleaseDescriptorV1 operand;std::string detail;exact_local_advisory_lock_release=scratchbird::engine::sblr::DecodeSblrAdvisoryLockReleaseDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_function_call=request.root_opcode_code==1024&&request.root_opcode=="SBLR_FUNCTION_CALL"&&request.root_operation_id=="engine.op.function_call";bool exact_local_function_call=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_function_call&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="function_call_descriptor"&&member.operands.front().name=="call"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::function_call_descriptor){scratchbird::engine::sblr::SblrFunctionCallDescriptorV1 operand;std::string detail;exact_local_function_call=scratchbird::engine::sblr::DecodeSblrFunctionCallDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_operator_call=request.root_opcode_code==1025&&request.root_opcode=="SBLR_OPERATOR_CALL"&&request.root_operation_id=="engine.op.operator_call";bool exact_local_operator_call=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_operator_call&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="operator_call_descriptor"&&member.operands.front().name=="call"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::operator_call_descriptor){scratchbird::engine::sblr::SblrOperatorCallDescriptorV1 operand;std::string detail;exact_local_operator_call=scratchbird::engine::sblr::DecodeSblrOperatorCallDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_cast=request.root_opcode_code==1026&&request.root_opcode=="SBLR_CAST"&&request.root_operation_id=="engine.op.cast";bool exact_local_cast=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_cast&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="cast_descriptor"&&member.operands.front().name=="cast"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::cast_descriptor){scratchbird::engine::sblr::SblrCastDescriptorV1 operand;std::string detail;exact_local_cast=scratchbird::engine::sblr::DecodeSblrCastDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_compare=request.root_opcode_code==1027&&request.root_opcode=="SBLR_COMPARE"&&request.root_operation_id=="engine.op.compare";bool exact_local_compare=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_compare&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="comparison_descriptor"&&member.operands.front().name=="compare"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::comparison_descriptor){scratchbird::engine::sblr::SblrCompareDescriptorV1 operand;std::string detail;exact_local_compare=scratchbird::engine::sblr::DecodeSblrCompareDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_domain_operation=request.root_opcode_code==1028&&request.root_opcode=="SBLR_DOMAIN_OPERATION"&&request.root_operation_id=="engine.op.domain_operation";bool exact_local_domain_operation=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_domain_operation&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="domain_operation_descriptor"&&member.operands.front().name=="domain_operation"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::domain_operation_descriptor){scratchbird::engine::sblr::SblrDomainOperationDescriptorV1 operand;std::string detail;exact_local_domain_operation=scratchbird::engine::sblr::DecodeSblrDomainOperationDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_udr=request.root_opcode_code==1029&&request.root_opcode=="SBLR_UDR_INVOKE"&&request.root_operation_id=="engine.op.udr_invoke";bool exact_local_udr=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_udr&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="registered_cpp_udr_invocation"&&member.operands.front().name=="udr"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::registered_cpp_udr_invocation){scratchbird::engine::sblr::SblrUdrInvokeDescriptorV1 operand;std::string detail;exact_local_udr=scratchbird::engine::sblr::DecodeSblrUdrInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_procedure=request.root_opcode_code==1030&&request.root_opcode=="SBLR_PROCEDURE_INVOKE"&&request.root_operation_id=="engine.op.procedure_invoke";bool exact_local_procedure=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_procedure&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="procedure_invoke_descriptor"&&member.operands.front().name=="procedure"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::procedure_invoke_descriptor){scratchbird::engine::sblr::SblrProcedureInvokeDescriptorV1 operand;std::string detail;exact_local_procedure=scratchbird::engine::sblr::DecodeSblrProcedureInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_function_invoke=request.root_opcode_code==1031&&request.root_opcode=="SBLR_FUNCTION_INVOKE"&&request.root_operation_id=="engine.op.function_invoke";bool exact_local_function_invoke=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_function_invoke&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="function_invoke_descriptor"&&member.operands.front().name=="function"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::function_invoke_descriptor){scratchbird::engine::sblr::SblrFunctionInvokeDescriptorV1 operand;std::string detail;exact_local_function_invoke=scratchbird::engine::sblr::DecodeSblrFunctionInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_aggregate_invoke=request.root_opcode_code==1032&&request.root_opcode=="SBLR_AGGREGATE_INVOKE"&&request.root_operation_id=="engine.op.aggregate_invoke";bool exact_local_aggregate_invoke=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_aggregate_invoke&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="aggregate_invoke_descriptor"&&member.operands.front().name=="aggregate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::aggregate_invoke_descriptor){scratchbird::engine::sblr::SblrAggregateInvokeDescriptorV1 operand;std::string detail;exact_local_aggregate_invoke=scratchbird::engine::sblr::DecodeSblrAggregateInvokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_sequence_nextval=request.root_opcode_code==1033&&request.root_opcode=="SBLR_SEQUENCE_NEXTVAL"&&request.root_operation_id=="engine.op.sequence_nextval";bool exact_local_sequence_nextval=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_sequence_nextval&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="sequence_nextval_descriptor"&&member.operands.front().name=="sequence"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::sequence_nextval_descriptor){scratchbird::engine::sblr::SblrSequenceNextvalDescriptorV1 operand;std::string detail;exact_local_sequence_nextval=scratchbird::engine::sblr::DecodeSblrSequenceNextvalDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_sequence_currval=request.root_opcode_code==1034&&request.root_opcode=="SBLR_SEQUENCE_CURRVAL"&&request.root_operation_id=="engine.op.sequence_currval";bool exact_local_sequence_currval=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_sequence_currval&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="sequence_currval_descriptor"&&member.operands.front().name=="sequence"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::sequence_currval_descriptor){scratchbird::engine::sblr::SblrSequenceCurrvalDescriptorV1 operand;std::string detail;exact_local_sequence_currval=scratchbird::engine::sblr::DecodeSblrSequenceCurrvalDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_sequence_setval=request.root_opcode_code==1035&&request.root_opcode=="SBLR_SEQUENCE_SETVAL"&&request.root_operation_id=="engine.op.sequence_setval";bool exact_local_sequence_setval=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_sequence_setval&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="sequence_setval_descriptor"&&member.operands.front().name=="sequence"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::sequence_setval_descriptor){scratchbird::engine::sblr::SblrSequenceSetvalDescriptorV1 operand;std::string detail;exact_local_sequence_setval=scratchbird::engine::sblr::DecodeSblrSequenceSetvalDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_query_numeric=request.root_opcode_code==1036&&request.root_opcode=="SBLR_QUERY_APPLY_NUMERIC_OPERATION"&&request.root_operation_id=="engine.op.query_apply_numeric_operation";bool exact_local_query_numeric=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_query_numeric&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="numeric_descriptor_and_operand_values"&&member.operands.front().name=="numeric_operation"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::numeric_operation_descriptor){scratchbird::engine::sblr::SblrQueryNumericDescriptorV1 operand;std::string detail;exact_local_query_numeric=scratchbird::engine::sblr::DecodeSblrQueryNumericDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_advanced_datatype_family=request.root_opcode_code==1037&&request.root_opcode=="SBLR_QUERY_EVALUATE_ADVANCED_DATATYPE_FAMILY"&&request.root_operation_id=="engine.op.query_evaluate_advanced_datatype_family";bool exact_local_advanced_datatype_family=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_advanced_datatype_family&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="advanced_family_descriptor_operation_index_profile"&&member.operands.front().name=="datatype_family"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::advanced_datatype_family_descriptor){scratchbird::engine::sblr::SblrAdvancedDatatypeFamilyDescriptorV1 operand;std::string detail;exact_local_advanced_datatype_family=scratchbird::engine::sblr::DecodeSblrAdvancedDatatypeFamilyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_project=request.root_opcode_code==1280&&request.root_opcode=="SBLR_PROJECT"&&request.root_operation_id=="engine.op.project";bool exact_local_project=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_project&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="projection_descriptor"&&member.operands.front().name=="projection"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::projection_descriptor){scratchbird::engine::sblr::SblrProjectDescriptorV1 operand;std::string detail;exact_local_project=scratchbird::engine::sblr::DecodeSblrProjectDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_aggregate=request.root_opcode_code==1281&&request.root_opcode=="SBLR_AGGREGATE"&&request.root_operation_id=="engine.op.aggregate";bool exact_local_aggregate=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_aggregate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="aggregate_descriptor"&&member.operands.front().name=="aggregate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::aggregate_descriptor){scratchbird::engine::sblr::SblrAggregateDescriptorV1 operand;std::string detail;exact_local_aggregate=scratchbird::engine::sblr::DecodeSblrAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_group=request.root_opcode_code==1282&&request.root_opcode=="SBLR_GROUP"&&request.root_operation_id=="engine.op.group";bool exact_local_group=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_group&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="group_descriptor"&&member.operands.front().name=="group"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::group_descriptor){scratchbird::engine::sblr::SblrGroupDescriptorV1 operand;std::string detail;exact_local_group=scratchbird::engine::sblr::DecodeSblrGroupDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_limit=request.root_opcode_code==1284&&request.root_opcode=="SBLR_LIMIT"&&request.root_operation_id=="engine.op.limit";bool exact_local_limit=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_limit&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="limit_descriptor"&&member.operands.front().name=="limit"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::limit_descriptor){scratchbird::engine::sblr::SblrLimitDescriptorV1 operand;std::string detail;exact_local_limit=scratchbird::engine::sblr::DecodeSblrLimitDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}  const bool exact_sort=request.root_opcode_code==1283&&request.root_opcode=="SBLR_SORT"&&request.root_operation_id=="engine.op.sort";bool exact_local_sort=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_sort&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="sort_descriptor"&&member.operands.front().name=="sort"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::sort_descriptor){scratchbird::engine::sblr::SblrSortDescriptorV1 operand;std::string detail;exact_local_sort=scratchbird::engine::sblr::DecodeSblrSortDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_window) exact_local_limit = true;
  const bool exact_kv_structured_read=request.root_opcode_code==8192&&request.root_opcode=="SBLR_KV_STRUCTURED_READ"&&request.root_operation_id=="engine.op.kv_structured_read";bool exact_local_kv_structured_read=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_kv_structured_read&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="kv_structured_read_descriptor"&&member.operands.front().name=="kv_read"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::kv_structured_read_descriptor){scratchbird::engine::sblr::SblrKvStructuredReadDescriptorV1 operand;std::string detail;exact_local_kv_structured_read=scratchbird::engine::sblr::DecodeSblrKvStructuredReadDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_kv_structured_read && !exact_local_kv_structured_read) return Refuse(request,"SBLR.OPERAND.INVALID");
  const bool exact_return_result_set = request.root_opcode_code==1286&&request.root_opcode=="SBLR_RETURN_RESULT_SET"&&request.root_operation_id=="engine.op.return_result_set";
  const bool exact_kv_structured_mutate=request.root_opcode_code==8193&&request.root_opcode=="SBLR_KV_STRUCTURED_MUTATE"&&request.root_operation_id=="engine.op.kv_structured_mutate";bool exact_local_kv_structured_mutate=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_kv_structured_mutate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="kv_structured_mutate_descriptor"&&member.operands.front().name=="kv_mutate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::kv_structured_mutate_descriptor){scratchbird::engine::sblr::SblrKvStructuredMutateDescriptorV1 operand;std::string detail;exact_local_kv_structured_mutate=scratchbird::engine::sblr::DecodeSblrKvStructuredMutateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_kv_structured_scan=request.root_opcode_code==8194&&request.root_opcode=="SBLR_KV_STRUCTURED_SCAN"&&request.root_operation_id=="engine.op.kv_structured_scan";bool exact_local_kv_structured_scan=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_kv_structured_scan&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="kv_structured_scan_descriptor"&&member.operands.front().name=="kv_scan"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::kv_structured_scan_descriptor){scratchbird::engine::sblr::SblrKvStructuredScanDescriptorV1 operand;std::string detail;exact_local_kv_structured_scan=scratchbird::engine::sblr::DecodeSblrKvStructuredScanDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_kv_structured_stream_read=request.root_opcode_code==8195&&request.root_opcode=="SBLR_KV_STRUCTURED_STREAM_READ"&&request.root_operation_id=="engine.op.kv_structured_stream_read";bool exact_local_kv_structured_stream_read=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_kv_structured_stream_read&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="kv_structured_stream_read_descriptor"&&member.operands.front().name=="kv_stream_read"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::kv_structured_stream_read_descriptor){scratchbird::engine::sblr::SblrKvStructuredStreamReadDescriptorV1 operand;std::string detail;exact_local_kv_structured_stream_read=scratchbird::engine::sblr::DecodeSblrKvStructuredStreamReadDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_kv_structured_stream_append=request.root_opcode_code==8196&&request.root_opcode=="SBLR_KV_STRUCTURED_STREAM_APPEND"&&request.root_operation_id=="engine.op.kv_structured_stream_append";bool exact_local_kv_structured_stream_append=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_kv_structured_stream_append&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="kv_structured_stream_append_descriptor"&&member.operands.front().name=="kv_stream_append"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::kv_structured_stream_append_descriptor){scratchbird::engine::sblr::SblrKvStructuredStreamAppendDescriptorV1 operand;std::string detail;exact_local_kv_structured_stream_append=scratchbird::engine::sblr::DecodeSblrKvStructuredStreamAppendDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_kv_structured_timeseries=request.root_opcode_code==8197&&request.root_opcode=="SBLR_KV_STRUCTURED_TIMESERIES"&&request.root_operation_id=="engine.op.kv_structured_timeseries";bool exact_local_kv_structured_timeseries=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_kv_structured_timeseries&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="kv_timeseries_descriptor"&&member.operands.front().name=="kv_timeseries"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::kv_structured_timeseries_descriptor){scratchbird::engine::sblr::SblrKvStructuredTimeseriesDescriptorV1 operand;std::string detail;exact_local_kv_structured_timeseries=scratchbird::engine::sblr::DecodeSblrKvStructuredTimeseriesDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_system_config_set=request.root_opcode_code==5125&&request.root_opcode=="SBLR_SYSTEM_CONFIG_SET"&&request.root_operation_id=="engine.op.system_config_set";bool exact_local_system_config_set=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_system_config_set&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="system_config_set_descriptor"&&member.operands.front().name=="system_config"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::system_config_set_descriptor){scratchbird::engine::sblr::SblrSystemConfigSetDescriptorV1 operand;std::string detail;exact_local_system_config_set=scratchbird::engine::sblr::DecodeSblrSystemConfigSetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_ddl_create_domain=request.root_opcode_code==1542&&request.root_opcode=="SBLR_DDL_CREATE_DOMAIN"&&request.root_operation_id=="engine.op.ddl_create_domain";bool exact_local_ddl_create_domain=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_domain&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_domain_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_domain_descriptor){scratchbird::engine::sblr::SblrDdlCreateDomainDescriptorV1 operand;std::string detail;exact_local_ddl_create_domain=scratchbird::engine::sblr::DecodeSblrDdlCreateDomainDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_ddl_create_schema=request.root_opcode_code==1536&&request.root_opcode=="SBLR_DDL_CREATE_SCHEMA"&&request.root_operation_id=="engine.op.ddl_create_schema";bool exact_local_ddl_create_schema=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_schema&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_schema_descriptor"&&member.operands.front().name=="schema"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_schema_descriptor){scratchbird::engine::sblr::SblrDdlCreateSchemaDescriptorV1 operand;std::string detail;exact_local_ddl_create_schema=scratchbird::engine::sblr::DecodeSblrDdlCreateSchemaDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_ddl_alter_domain=request.root_opcode_code==1547&&request.root_opcode=="SBLR_DDL_ALTER_DOMAIN"&&request.root_operation_id=="engine.op.ddl_alter_domain";bool exact_local_ddl_alter_domain=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_domain&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_domain_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_domain_descriptor){scratchbird::engine::sblr::SblrDdlAlterDomainDescriptorV1 operand;std::string detail;exact_local_ddl_alter_domain=scratchbird::engine::sblr::DecodeSblrDdlAlterDomainDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}const bool exact_ddl_create_view=request.root_opcode_code==1548&&request.root_opcode=="SBLR_DDL_CREATE_VIEW"&&request.root_operation_id=="engine.op.ddl_create_view";bool exact_local_ddl_create_view=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_view_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_view_descriptor){scratchbird::engine::sblr::SblrDdlCreateViewDescriptorV1 operand;std::string detail;exact_local_ddl_create_view=scratchbird::engine::sblr::DecodeSblrDdlCreateViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}const bool exact_ddl_alter_view=request.root_opcode_code==1549&&request.root_opcode=="SBLR_DDL_ALTER_VIEW"&&request.root_operation_id=="engine.op.ddl_alter_view";bool exact_local_ddl_alter_view=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_view_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_view_descriptor){scratchbird::engine::sblr::SblrDdlAlterViewDescriptorV1 operand;std::string detail;exact_local_ddl_alter_view=scratchbird::engine::sblr::DecodeSblrDdlAlterViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}const bool exact_ddl_drop_view=request.root_opcode_code==1550&&request.root_opcode=="SBLR_DDL_DROP_VIEW"&&request.root_operation_id=="engine.op.ddl_drop_view";bool exact_local_ddl_drop_view=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_view_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_view_descriptor){scratchbird::engine::sblr::SblrDdlDropViewDescriptorV1 operand;std::string detail;exact_local_ddl_drop_view=scratchbird::engine::sblr::DecodeSblrDdlDropViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}


const bool exact_ddl_create_table=request.root_opcode_code==1537&&request.root_opcode=="SBLR_DDL_CREATE_TABLE"&&request.root_operation_id=="engine.op.ddl_create_table";bool exact_local_ddl_create_table=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_table&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_table_descriptor"&&member.operands.front().name=="table"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_table_descriptor){scratchbird::engine::sblr::SblrDdlCreateTableDescriptorV1 operand;std::string detail;exact_local_ddl_create_table=scratchbird::engine::sblr::DecodeSblrDdlCreateTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_ddl_create_index=request.root_opcode_code==1540&&request.root_opcode=="SBLR_DDL_CREATE_INDEX"&&request.root_operation_id=="engine.op.ddl_create_index";bool exact_local_ddl_create_index=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_index&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_index_descriptor"&&member.operands.front().name=="index"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_index_descriptor){scratchbird::engine::sblr::SblrDdlCreateIndexDescriptorV1 operand;std::string detail;exact_local_ddl_create_index=scratchbird::engine::sblr::DecodeSblrDdlCreateIndexDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
const bool exact_ddl_drop_index=request.root_opcode_code==1541&&request.root_opcode=="SBLR_DDL_DROP_INDEX"&&request.root_operation_id=="engine.op.ddl_drop_index";bool exact_local_ddl_drop_index=false;if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_index&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_index_descriptor"&&member.operands.front().name=="index"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_index_descriptor){scratchbird::engine::sblr::SblrDdlDropIndexDescriptorV1 operand;std::string detail;exact_local_ddl_drop_index=scratchbird::engine::sblr::DecodeSblrDdlDropIndexDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_ddl_create_trigger=request.root_opcode_code==1551&&request.root_opcode=="SBLR_DDL_CREATE_TRIGGER"&&request.root_operation_id=="engine.op.ddl_create_trigger";
  bool exact_local_ddl_create_trigger=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_trigger&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_trigger_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_trigger_descriptor){scratchbird::engine::sblr::SblrDdlCreateTriggerDescriptorV1 operand;std::string detail;exact_local_ddl_create_trigger=scratchbird::engine::sblr::DecodeSblrDdlCreateTriggerDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_create_trigger) exact_local_ddl_drop_view = true;
  const bool exact_ddl_alter_trigger=request.root_opcode_code==1552&&request.root_opcode=="SBLR_DDL_ALTER_TRIGGER"&&request.root_operation_id=="engine.op.ddl_alter_trigger"; bool exact_local_ddl_alter_trigger=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_trigger&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_trigger_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_trigger_descriptor){scratchbird::engine::sblr::SblrDdlAlterTriggerDescriptorV1 operand;std::string detail;exact_local_ddl_alter_trigger=scratchbird::engine::sblr::DecodeSblrDdlAlterTriggerDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_alter_trigger) { exact_local_ddl_create_trigger = true; exact_local_ddl_drop_view = true; }
  const bool exact_ddl_drop_trigger=request.root_opcode_code==1553&&request.root_opcode=="SBLR_DDL_DROP_TRIGGER"&&request.root_operation_id=="engine.op.ddl_drop_trigger"; bool exact_local_ddl_drop_trigger=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_trigger&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_trigger_descriptor"&&member.operands.front().name=="domain"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_trigger_descriptor){scratchbird::engine::sblr::SblrDdlDropTriggerDescriptorV1 operand;std::string detail;exact_local_ddl_drop_trigger=scratchbird::engine::sblr::DecodeSblrDdlDropTriggerDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_drop_trigger) { exact_local_ddl_create_trigger = true; exact_local_ddl_drop_view = true; }
  const bool exact_ddl_create_procedure=request.root_opcode_code==1554&&request.root_opcode=="SBLR_DDL_CREATE_PROCEDURE"&&request.root_operation_id=="engine.op.ddl_create_procedure"; bool exact_local_ddl_create_procedure=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_procedure&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_procedure_descriptor"&&member.operands.front().name=="procedure"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_procedure_descriptor){scratchbird::engine::sblr::SblrDdlCreateProcedureDescriptorV1 operand;std::string detail;exact_local_ddl_create_procedure=scratchbird::engine::sblr::DecodeSblrDdlCreateProcedureDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_create_procedure) exact_local_ddl_create_schema = true;
  const bool exact_ddl_alter_procedure=request.root_opcode_code==1555&&request.root_opcode=="SBLR_DDL_ALTER_PROCEDURE"&&request.root_operation_id=="engine.op.ddl_alter_procedure"; bool exact_local_ddl_alter_procedure=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_procedure&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_procedure_descriptor"&&member.operands.front().name=="procedure"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_procedure_descriptor){scratchbird::engine::sblr::SblrDdlAlterProcedureDescriptorV1 operand;std::string detail;exact_local_ddl_alter_procedure=scratchbird::engine::sblr::DecodeSblrDdlAlterProcedureDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_alter_procedure) exact_local_ddl_create_schema = true;
  const bool exact_ddl_drop_procedure=request.root_opcode_code==1556&&request.root_opcode=="SBLR_DDL_DROP_PROCEDURE"&&request.root_operation_id=="engine.op.ddl_drop_procedure"; bool exact_local_ddl_drop_procedure=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_procedure&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_procedure_descriptor"&&member.operands.front().name=="procedure"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_procedure_descriptor){scratchbird::engine::sblr::SblrDdlDropProcedureDescriptorV1 operand;std::string detail;exact_local_ddl_drop_procedure=scratchbird::engine::sblr::DecodeSblrDdlDropProcedureDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_drop_procedure) exact_local_ddl_create_schema = true;
  const bool exact_ddl_create_function=request.root_opcode_code==1557&&request.root_opcode=="SBLR_DDL_CREATE_FUNCTION"&&request.root_operation_id=="engine.op.ddl_create_function"; bool exact_local_ddl_create_function=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_function&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_function_descriptor"&&member.operands.front().name=="function"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_function_descriptor){scratchbird::engine::sblr::SblrDdlCreateFunctionDescriptorV1 operand;std::string detail;exact_local_ddl_create_function=scratchbird::engine::sblr::DecodeSblrDdlCreateFunctionDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_create_function) exact_local_ddl_create_schema = true;
  const bool exact_ddl_alter_function=request.root_opcode_code==1558&&request.root_opcode=="SBLR_DDL_ALTER_FUNCTION"&&request.root_operation_id=="engine.op.ddl_alter_function"; bool exact_local_ddl_alter_function=false;
  const bool exact_ddl_drop_function=request.root_opcode_code==1559&&request.root_opcode=="SBLR_DDL_DROP_FUNCTION"&&request.root_operation_id=="engine.op.ddl_drop_function"; bool exact_local_ddl_drop_function=false;
  const bool exact_ddl_create_package=request.root_opcode_code==1560&&request.root_opcode=="SBLR_DDL_CREATE_PACKAGE"&&request.root_operation_id=="engine.op.ddl_create_package"; bool exact_local_ddl_create_package=false;
  const bool exact_ddl_create_sequence=request.root_opcode_code==1671&&request.root_opcode=="SBLR_DDL_CREATE_SEQUENCE"&&request.root_operation_id=="engine.op.ddl_create_sequence"; bool exact_local_ddl_create_sequence=false;
  const bool exact_ddl_drop_package=request.root_opcode_code==1562&&request.root_opcode=="SBLR_DDL_DROP_PACKAGE"&&request.root_operation_id=="engine.op.ddl_drop_package"; bool exact_local_ddl_drop_package=false;
  const bool exact_ddl_create_synonym=request.root_opcode_code==1574&&request.root_opcode=="SBLR_DDL_CREATE_SYNONYM"&&request.root_operation_id=="engine.op.ddl_create_synonym"; bool exact_local_ddl_create_synonym=false;
  const bool exact_ddl_create_foreign_table=request.root_opcode_code==1576&&request.root_opcode=="SBLR_DDL_CREATE_FOREIGN_TABLE"&&request.root_operation_id=="engine.op.ddl_create_foreign_table"; bool exact_local_ddl_create_foreign_table=false;
  const bool exact_ddl_create_fdw=request.root_opcode_code==1578&&request.root_opcode=="SBLR_DDL_CREATE_FDW"&&request.root_operation_id=="engine.op.ddl_create_fdw"; bool exact_local_ddl_create_fdw=false;
  const bool exact_ddl_drop_fdw=request.root_opcode_code==1579&&request.root_opcode=="SBLR_DDL_DROP_FDW"&&request.root_operation_id=="engine.op.ddl_drop_fdw"; bool exact_local_ddl_drop_fdw=false;
  const bool exact_ddl_drop_synonym=request.root_opcode_code==1575&&request.root_opcode=="SBLR_DDL_DROP_SYNONYM"&&request.root_operation_id=="engine.op.ddl_drop_synonym"; bool exact_local_ddl_drop_synonym=false;
  const bool exact_ddl_drop_foreign_table=request.root_opcode_code==1577&&request.root_opcode=="SBLR_DDL_DROP_FOREIGN_TABLE"&&request.root_operation_id=="engine.op.ddl_drop_foreign_table"; bool exact_local_ddl_drop_foreign_table=false;
  const bool exact_ddl_alter_package=request.root_opcode_code==1561&&request.root_opcode=="SBLR_DDL_ALTER_PACKAGE"&&request.root_operation_id=="engine.op.ddl_alter_package"; bool exact_local_ddl_alter_package=false;
  const bool exact_ddl_alter_sequence=request.root_opcode_code==1564&&request.root_opcode=="SBLR_DDL_ALTER_SEQUENCE"&&request.root_operation_id=="engine.op.ddl_alter_sequence"; bool exact_local_ddl_alter_sequence=false;
  const bool exact_ddl_drop_sequence=request.root_opcode_code==1565&&request.root_opcode=="SBLR_DDL_DROP_SEQUENCE"&&request.root_operation_id=="engine.op.ddl_drop_sequence"; bool exact_local_ddl_drop_sequence=false;
  const bool exact_ddl_create_materialized_view=request.root_opcode_code==1566&&request.root_opcode=="SBLR_DDL_CREATE_MATERIALIZED_VIEW"&&request.root_operation_id=="engine.op.ddl_create_materialized_view"; bool exact_local_ddl_create_materialized_view=false;
  const bool exact_ddl_create_type=request.root_opcode_code==1569&&request.root_opcode=="SBLR_DDL_CREATE_TYPE"&&request.root_operation_id=="engine.op.ddl_create_type"; bool exact_local_ddl_create_type=false;
  const bool exact_ddl_alter_type=request.root_opcode_code==1570&&request.root_opcode=="SBLR_DDL_ALTER_TYPE"&&request.root_operation_id=="engine.op.ddl_alter_type"; bool exact_local_ddl_alter_type=false;
  const bool exact_ddl_drop_type=request.root_opcode_code==1571&&request.root_opcode=="SBLR_DDL_DROP_TYPE"&&request.root_operation_id=="engine.op.ddl_drop_type"; bool exact_local_ddl_drop_type=false;
  const bool exact_ddl_refresh_materialized_view=request.root_opcode_code==1567&&request.root_opcode=="SBLR_DDL_REFRESH_MATERIALIZED_VIEW"&&request.root_operation_id=="engine.op.ddl_refresh_materialized_view"; bool exact_local_ddl_refresh_materialized_view=false;
  const bool exact_ddl_drop_materialized_view=request.root_opcode_code==1568&&request.root_opcode=="SBLR_DDL_DROP_MATERIALIZED_VIEW"&&request.root_operation_id=="engine.op.ddl_drop_materialized_view"; bool exact_local_ddl_drop_materialized_view=false;
  const bool exact_ddl_create_temporary_table=request.root_opcode_code==1561&&request.root_opcode=="SBLR_DDL_CREATE_TEMPORARY_TABLE"&&request.root_operation_id=="engine.op.ddl_create_temporary_table"; bool exact_local_ddl_create_temporary_table=false;
  const bool exact_ddl_drop_temporary_table=request.root_opcode_code==1562&&request.root_opcode=="SBLR_DDL_DROP_TEMPORARY_TABLE"&&request.root_operation_id=="engine.op.ddl_drop_temporary_table"; bool exact_local_ddl_drop_temporary_table=false;
  const bool exact_ddl_rename_object_vector=request.root_opcode_code==1563&&request.root_opcode=="SBLR_DDL_RENAME_OBJECT_VECTOR"&&request.root_operation_id=="engine.op.ddl_rename_object_vector"; bool exact_local_ddl_rename_object_vector=false; const bool exact_ddl_rename_object=request.root_opcode_code==1572&&request.root_opcode=="SBLR_DDL_RENAME_OBJECT"&&request.root_operation_id=="engine.op.ddl_rename_object"; bool exact_local_ddl_rename_object=false;
  const bool exact_ddl_create_or_replace_srs=request.root_opcode_code==1615&&request.root_opcode=="SBLR_DDL_CREATE_OR_REPLACE_SRS"&&request.root_operation_id=="engine.op.ddl_create_or_replace_srs"; bool exact_local_ddl_create_or_replace_srs=false;
  const bool exact_ddl_drop_srs=request.root_opcode_code==1616&&request.root_opcode=="SBLR_DDL_DROP_SRS"&&request.root_operation_id=="engine.op.ddl_drop_srs"; bool exact_local_ddl_drop_srs=false;
  const bool exact_ddl_create_rewrite_rule=request.root_opcode_code==1617&&request.root_opcode=="SBLR_DDL_CREATE_REWRITE_RULE"&&request.root_operation_id=="engine.op.ddl_create_rewrite_rule"; bool exact_local_ddl_create_rewrite_rule=false;
  const bool exact_ddl_alter_rewrite_rule=request.root_opcode_code==1618&&request.root_opcode=="SBLR_DDL_ALTER_REWRITE_RULE"&&request.root_operation_id=="engine.op.ddl_alter_rewrite_rule"; bool exact_local_ddl_alter_rewrite_rule=false;
  const bool exact_ddl_drop_rewrite_rule=request.root_opcode_code==1619&&request.root_opcode=="SBLR_DDL_DROP_REWRITE_RULE"&&request.root_operation_id=="engine.op.ddl_drop_rewrite_rule"; bool exact_local_ddl_drop_rewrite_rule=false;
  const bool exact_ddl_validate_constraint=request.root_opcode_code==1620&&request.root_opcode=="SBLR_DDL_VALIDATE_CONSTRAINT"&&request.root_operation_id=="engine.op.ddl_validate_constraint"; bool exact_local_ddl_validate_constraint=false;
  const bool exact_security_create_privilege_template=request.root_opcode_code==1621&&request.root_opcode=="SBLR_SECURITY_CREATE_PRIVILEGE_TEMPLATE"&&request.root_operation_id=="engine.op.security_create_privilege_template"; bool exact_local_security_create_privilege_template=false;
  const bool exact_security_create_user=request.root_opcode_code==1792&&request.root_opcode=="SBLR_SEC_CREATE_USER"&&request.root_operation_id=="engine.op.security_create_user"; bool exact_local_security_create_user=false;
  const bool exact_security_alter_user=request.root_opcode_code==1793&&request.root_opcode=="SBLR_SEC_ALTER_USER"&&request.root_operation_id=="engine.op.sec_alter_user"; bool exact_local_security_alter_user=false;
  const bool exact_security_create_role=request.root_opcode_code==1794&&request.root_opcode=="SBLR_SEC_CREATE_ROLE"&&request.root_operation_id=="engine.op.sec_create_role"; bool exact_local_security_create_role=false;
  const bool exact_security_drop_role=request.root_opcode_code==1801&&request.root_opcode=="SBLR_SEC_DROP_ROLE"&&request.root_operation_id=="engine.op.sec_drop_role"; bool exact_local_security_drop_role=false;
  const bool exact_security_create_policy=request.root_opcode_code==1802&&request.root_opcode=="SBLR_SEC_CREATE_POLICY"&&request.root_operation_id=="engine.op.sec_create_policy"; bool exact_local_security_create_policy=false;
  const bool exact_security_drop_policy=request.root_opcode_code==1803&&request.root_opcode=="SBLR_SEC_DROP_POLICY"&&request.root_operation_id=="engine.op.sec_drop_policy"; bool exact_local_security_drop_policy=false;
  const bool exact_security_alter_role=request.root_opcode_code==1800&&request.root_opcode=="SBLR_SEC_ALTER_ROLE"&&request.root_operation_id=="engine.op.sec_alter_role"; bool exact_local_security_alter_role=false;
  const bool exact_security_create_group_mapping=request.root_opcode_code==1797&&request.root_opcode=="SBLR_SEC_CREATE_GROUP_MAPPING"&&request.root_operation_id=="engine.op.sec_create_group_mapping"; bool exact_local_security_create_group_mapping=false;
  const bool exact_security_drop_group_mapping=request.root_opcode_code==1806&&request.root_opcode=="SBLR_SEC_DROP_GROUP_MAPPING"&&request.root_operation_id=="engine.op.sec_drop_group_mapping"; bool exact_local_security_drop_group_mapping=false;
  const bool exact_security_grant=request.root_opcode_code==1795&&request.root_opcode=="SBLR_SEC_GRANT"&&request.root_operation_id=="engine.op.sec_grant"; bool exact_local_security_grant=false;
  const bool exact_security_revoke=request.root_opcode_code==1796&&request.root_opcode=="SBLR_SEC_REVOKE"&&request.root_operation_id=="engine.op.sec_revoke"; bool exact_local_security_revoke=false;
  const bool exact_security_alter_policy=request.root_opcode_code==1798&&request.root_opcode=="SBLR_SEC_ALTER_POLICY"&&request.root_operation_id=="engine.op.sec_alter_policy"; bool exact_local_security_alter_policy=false;
  const bool exact_security_drop_user=request.root_opcode_code==1799&&request.root_opcode=="SBLR_SEC_DROP_USER"&&request.root_operation_id=="engine.op.sec_drop_user"; bool exact_local_security_drop_user=false;
  const bool exact_security_authenticate=request.root_opcode_code==1804&&request.root_opcode=="SBLR_SEC_AUTHENTICATE"&&request.root_operation_id=="engine.op.sec_authenticate"; bool exact_local_security_authenticate=false;
  const bool exact_security_deauthenticate=request.root_opcode_code==1805&&request.root_opcode=="SBLR_SEC_DEAUTHENTICATE"&&request.root_operation_id=="engine.op.sec_deauthenticate"; bool exact_local_security_deauthenticate=false;
  const bool exact_session_role_switch=request.root_opcode_code==4359&&request.root_opcode=="SBLR_SESSION_ROLE_SWITCH"&&request.root_operation_id=="engine.op.session_role_switch"; bool exact_local_session_role_switch=false;
  const bool exact_session_setting_set=request.root_opcode_code==4355&&request.root_opcode=="SBLR_SESSION_SETTING_SET"&&request.root_operation_id=="engine.op.session_setting_set"; bool exact_local_session_setting_set=false;
  const bool exact_session_setting_reset=request.root_opcode_code==4357&&request.root_opcode=="SBLR_SESSION_SETTING_RESET"&&request.root_operation_id=="engine.op.session_setting_reset"; bool exact_local_session_setting_reset=false;
  const bool exact_session_setting_get=request.root_opcode_code==4356&&request.root_opcode=="SBLR_SESSION_SETTING_GET"&&request.root_operation_id=="engine.op.session_setting_get"; bool exact_local_session_setting_get=false;
  const bool exact_session_default_qualifier_set=request.root_opcode_code==4358&&request.root_opcode=="SBLR_SESSION_DEFAULT_QUALIFIER_SET"&&request.root_operation_id=="engine.op.session_default_qualifier_set"; bool exact_local_session_default_qualifier_set=false;
  const bool exact_session_discard=request.root_opcode_code==4360&&request.root_opcode=="SBLR_SESSION_DISCARD"&&request.root_operation_id=="engine.op.session_discard"; bool exact_local_session_discard=false;
  const bool exact_session_snapshot_handle=request.root_opcode_code==4361&&request.root_opcode=="SBLR_SESSION_SNAPSHOT_HANDLE"&&request.root_operation_id=="engine.op.session_snapshot_handle"; bool exact_local_session_snapshot_handle=false;
  const bool exact_security_alter_privilege_template=request.root_opcode_code==1622&&request.root_opcode=="SBLR_SECURITY_ALTER_PRIVILEGE_TEMPLATE"&&request.root_operation_id=="engine.op.security_alter_privilege_template"; bool exact_local_security_alter_privilege_template=false;
  const bool exact_security_drop_privilege_template=request.root_opcode_code==1623&&request.root_opcode=="SBLR_SECURITY_DROP_PRIVILEGE_TEMPLATE"&&request.root_operation_id=="engine.op.security_drop_privilege_template"; bool exact_local_security_drop_privilege_template=false;
  const bool exact_database_create_template_clone=request.root_opcode_code==1624&&request.root_opcode=="SBLR_DATABASE_CREATE_TEMPLATE_CLONE"&&request.root_operation_id=="engine.op.database_create_template_clone"; bool exact_local_database_create_template_clone=false;
  const bool exact_ddl_create_aggregate=request.root_opcode_code==1625&&request.root_opcode=="SBLR_DDL_CREATE_AGGREGATE"&&request.root_operation_id=="engine.op.ddl_create_aggregate"; bool exact_local_ddl_create_aggregate=false;
  const bool exact_ddl_alter_aggregate=request.root_opcode_code==1626&&request.root_opcode=="SBLR_DDL_ALTER_AGGREGATE"&&request.root_operation_id=="engine.op.ddl_alter_aggregate"; bool exact_local_ddl_alter_aggregate=false;
  const bool exact_ddl_drop_aggregate=request.root_opcode_code==1627&&request.root_opcode=="SBLR_DDL_DROP_AGGREGATE"&&request.root_operation_id=="engine.op.ddl_drop_aggregate"; bool exact_local_ddl_drop_aggregate=false;
  const bool exact_ddl_drop_dictionary=request.root_opcode_code==1638&&request.root_opcode=="SBLR_DDL_DROP_DICTIONARY"&&request.root_operation_id=="engine.op.ddl_drop_dictionary"; bool exact_local_ddl_drop_dictionary=false;
  const bool exact_ddl_alter_dictionary=request.root_opcode_code==1639&&request.root_opcode=="SBLR_DDL_ALTER_DICTIONARY"&&request.root_operation_id=="engine.op.ddl_alter_dictionary"; bool exact_local_ddl_alter_dictionary=false;
  const bool exact_ddl_create_continuous_view=request.root_opcode_code==1640&&request.root_opcode=="SBLR_DDL_CREATE_CONTINUOUS_VIEW"&&request.root_operation_id=="engine.op.ddl_create_continuous_view"; bool exact_local_ddl_create_continuous_view=false;
  const bool exact_ddl_alter_continuous_view=request.root_opcode_code==1641&&request.root_opcode=="SBLR_DDL_ALTER_CONTINUOUS_VIEW"&&request.root_operation_id=="engine.op.ddl_alter_continuous_view"; bool exact_local_ddl_alter_continuous_view=false;
  const bool exact_ddl_drop_continuous_view=request.root_opcode_code==1642&&request.root_opcode=="SBLR_DDL_DROP_CONTINUOUS_VIEW"&&request.root_operation_id=="engine.op.ddl_drop_continuous_view"; bool exact_local_ddl_drop_continuous_view=false;
  const bool exact_dml_async_insert_submit=request.root_opcode_code==1643&&request.root_opcode=="SBLR_DML_ASYNC_INSERT_SUBMIT"&&request.root_operation_id=="engine.op.dml_async_insert_submit"; bool exact_local_dml_async_insert_submit=false;
  const bool exact_dml_async_insert_status=request.root_opcode_code==1644&&request.root_opcode=="SBLR_DML_ASYNC_INSERT_STATUS"&&request.root_operation_id=="engine.op.dml_async_insert_status"; bool exact_local_dml_async_insert_status=false;
  const bool exact_dml_async_insert_cancel=request.root_opcode_code==1645&&request.root_opcode=="SBLR_DML_ASYNC_INSERT_CANCEL"&&request.root_operation_id=="engine.op.dml_async_insert_cancel"; bool exact_local_dml_async_insert_cancel=false;
  const bool exact_dml_counter_add=request.root_opcode_code==1647&&request.root_opcode=="SBLR_DML_COUNTER_ADD"&&request.root_operation_id=="engine.op.dml_counter_add"; bool exact_local_dml_counter_add=false;
  const bool exact_dml_timeseries_schema_write=request.root_opcode_code==1648&&request.root_opcode=="SBLR_DML_TIMESERIES_SCHEMA_WRITE"&&request.root_operation_id=="engine.op.dml_timeseries_schema_write"; bool exact_local_dml_timeseries_schema_write=false;
  const bool exact_ddl_timeseries_series_cardinality_policy=request.root_opcode_code==1649&&request.root_opcode=="SBLR_DDL_SET_TIMESERIES_SERIES_CARDINALITY_POLICY"&&request.root_operation_id=="engine.op.ddl_set_timeseries_series_cardinality_policy"; bool exact_local_ddl_timeseries_series_cardinality_policy=false;
  const bool exact_ddl_create_timeseries_value_cache=request.root_opcode_code==1650&&request.root_opcode=="SBLR_DDL_CREATE_TIMESERIES_VALUE_CACHE"&&request.root_operation_id=="engine.op.ddl_create_timeseries_value_cache"; bool exact_local_ddl_create_timeseries_value_cache=false;
  const bool exact_ddl_purge_system_history=request.root_opcode_code==1628&&request.root_opcode=="SBLR_DDL_PURGE_SYSTEM_HISTORY"&&request.root_operation_id=="engine.op.ddl_purge_system_history"; bool exact_local_ddl_purge_system_history=false;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_package&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_package_descriptor"&&member.operands.front().name=="package"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_package_descriptor){scratchbird::engine::sblr::SblrDdlCreatePackageDescriptorV1 operand;std::string detail;exact_local_ddl_create_package=scratchbird::engine::sblr::DecodeSblrDdlCreatePackageDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_sequence&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_sequence_descriptor"&&member.operands.front().name=="sequence"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_sequence_descriptor){scratchbird::engine::sblr::SblrDdlCreateSequenceDescriptorV1 operand;std::string detail;exact_local_ddl_create_sequence=scratchbird::engine::sblr::DecodeSblrDdlCreateSequenceDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_create_sequence) exact_local_ddl_create_schema = true;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_package&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_package_descriptor"&&member.operands.front().name=="package"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_package_descriptor){scratchbird::engine::sblr::SblrDdlDropPackageDescriptorV1 operand;std::string detail;exact_local_ddl_drop_package=scratchbird::engine::sblr::DecodeSblrDdlDropPackageDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_synonym&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_synonym_descriptor"&&member.operands.front().name=="synonym"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_synonym_descriptor){scratchbird::engine::sblr::SblrDdlCreateSynonymDescriptorV1 operand;std::string detail;exact_local_ddl_create_synonym=scratchbird::engine::sblr::DecodeSblrDdlCreateSynonymDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_foreign_table&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_foreign_table_descriptor"&&member.operands.front().name=="foreign_table"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_foreign_table_descriptor){scratchbird::engine::sblr::SblrDdlCreateForeignTableDescriptorV1 operand;std::string detail;exact_local_ddl_create_foreign_table=scratchbird::engine::sblr::DecodeSblrDdlCreateForeignTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_fdw&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_fdw_descriptor"&&member.operands.front().name=="fdw"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_fdw_descriptor){scratchbird::engine::sblr::SblrDdlCreateFdwDescriptorV1 operand;std::string detail;exact_local_ddl_create_fdw=scratchbird::engine::sblr::DecodeSblrDdlCreateFdwDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_fdw&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_fdw_descriptor"&&member.operands.front().name=="fdw"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_fdw_descriptor){scratchbird::engine::sblr::SblrDdlDropFdwDescriptorV1 operand;std::string detail;exact_local_ddl_drop_fdw=scratchbird::engine::sblr::DecodeSblrDdlDropFdwDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_ddl_drop_fdw && !request.cluster_context_active && !request.cluster_transaction_active && !request.route_fence_present) exact_local_ddl_drop_fdw = true;
  if (exact_local_ddl_drop_fdw) exact_local_ddl_drop_temporary_table = true;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_synonym&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_synonym_descriptor"&&member.operands.front().name=="synonym"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_package_descriptor){scratchbird::engine::sblr::SblrDdlDropSynonymDescriptorV1 operand;std::string detail;exact_local_ddl_drop_synonym=scratchbird::engine::sblr::DecodeSblrDdlDropSynonymDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_foreign_table&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_foreign_table_descriptor"&&member.operands.front().name=="foreign_table"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_foreign_table_descriptor){scratchbird::engine::sblr::SblrDdlDropForeignTableDescriptorV1 operand;std::string detail;exact_local_ddl_drop_foreign_table=scratchbird::engine::sblr::DecodeSblrDdlDropForeignTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_drop_synonym) exact_local_ddl_create_schema = true;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_package&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_package_descriptor"&&member.operands.front().name=="package"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_package_descriptor){scratchbird::engine::sblr::SblrDdlAlterPackageDescriptorV1 operand;std::string detail;exact_local_ddl_alter_package=scratchbird::engine::sblr::DecodeSblrDdlAlterPackageDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_sequence&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_sequence_descriptor"&&member.operands.front().name=="sequence"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_sequence_descriptor){scratchbird::engine::sblr::SblrDdlAlterSequenceDescriptorV1 operand;std::string detail;exact_local_ddl_alter_sequence=scratchbird::engine::sblr::DecodeSblrDdlAlterSequenceDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_sequence&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_sequence_descriptor"&&member.operands.front().name=="sequence"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_sequence_descriptor){scratchbird::engine::sblr::SblrDdlDropSequenceDescriptorV1 operand;std::string detail;exact_local_ddl_drop_sequence=scratchbird::engine::sblr::DecodeSblrDdlDropSequenceDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_materialized_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_materialized_view_descriptor"&&member.operands.front().name=="view"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_materialized_view_descriptor){scratchbird::engine::sblr::SblrDdlCreateMaterializedViewDescriptorV1 operand;std::string detail;exact_local_ddl_create_materialized_view=scratchbird::engine::sblr::DecodeSblrDdlCreateMaterializedViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_type&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_type_descriptor"&&member.operands.front().name=="type"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_type_descriptor){scratchbird::engine::sblr::SblrDdlCreateTypeDescriptorV1 operand;std::string detail;exact_local_ddl_create_type=scratchbird::engine::sblr::DecodeSblrDdlCreateTypeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_type&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_type_descriptor"&&member.operands.front().name=="type"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_type_descriptor){scratchbird::engine::sblr::SblrDdlAlterTypeDescriptorV1 operand;std::string detail;exact_local_ddl_alter_type=scratchbird::engine::sblr::DecodeSblrDdlAlterTypeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_type&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_type_descriptor"&&member.operands.front().name=="type"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_type_descriptor){scratchbird::engine::sblr::SblrDdlDropTypeDescriptorV1 operand;std::string detail;exact_local_ddl_drop_type=scratchbird::engine::sblr::DecodeSblrDdlDropTypeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_refresh_materialized_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="refresh_materialized_view_descriptor"&&member.operands.front().name=="view"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::refresh_materialized_view_descriptor){scratchbird::engine::sblr::SblrDdlRefreshMaterializedViewDescriptorV1 operand;std::string detail;exact_local_ddl_refresh_materialized_view=scratchbird::engine::sblr::DecodeSblrDdlRefreshMaterializedViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_dictionary&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="external_dictionary_alter_descriptor"&&member.operands.front().name=="dictionary"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::external_dictionary_alter_descriptor){scratchbird::engine::sblr::SblrDdlAlterDictionaryDescriptorV1 operand;std::string detail;exact_local_ddl_alter_dictionary=scratchbird::engine::sblr::DecodeSblrDdlAlterDictionaryDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_continuous_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="continuous_view_descriptor"&&member.operands.front().name=="view"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::continuous_view_descriptor){scratchbird::engine::sblr::SblrDdlCreateContinuousViewDescriptorV1 operand;std::string detail;exact_local_ddl_create_continuous_view=scratchbird::engine::sblr::DecodeSblrDdlCreateContinuousViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_continuous_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="continuous_view_alter_descriptor"&&member.operands.front().name=="view"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::continuous_view_alter_descriptor){scratchbird::engine::sblr::SblrDdlAlterContinuousViewDescriptorV1 operand;std::string detail;exact_local_ddl_alter_continuous_view=scratchbird::engine::sblr::DecodeSblrDdlAlterContinuousViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_continuous_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="continuous_view_drop_descriptor"&&member.operands.front().name=="view"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::continuous_view_drop_descriptor){scratchbird::engine::sblr::SblrDdlDropContinuousViewDescriptorV1 operand;std::string detail;exact_local_ddl_drop_continuous_view=scratchbird::engine::sblr::DecodeSblrDdlDropContinuousViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_dml_async_insert_submit&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="async_insert_submission_descriptor"&&member.operands.front().name=="submission"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::async_insert_submission_descriptor){scratchbird::engine::sblr::SblrDmlAsyncInsertSubmitDescriptorV1 operand;std::string detail;exact_local_dml_async_insert_submit=scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertSubmitDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_dml_async_insert_status&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="async_insert_status_descriptor"&&member.operands.front().name=="status"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::async_insert_status_descriptor){scratchbird::engine::sblr::SblrDmlAsyncInsertStatusDescriptorV1 operand;std::string detail;exact_local_dml_async_insert_status=scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertStatusDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_dml_async_insert_cancel&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="async_insert_cancel_descriptor"&&member.operands.front().name=="cancel"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::async_insert_cancel_descriptor){scratchbird::engine::sblr::SblrDmlAsyncInsertCancelDescriptorV1 operand;std::string detail;exact_local_dml_async_insert_cancel=scratchbird::engine::sblr::DecodeSblrDmlAsyncInsertCancelDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_dml_counter_add&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="counter_delta_descriptor"&&member.operands.front().name=="delta"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::counter_delta_descriptor){scratchbird::engine::sblr::SblrDmlCounterAddDescriptorV1 operand;std::string detail;exact_local_dml_counter_add=scratchbird::engine::sblr::DecodeSblrDmlCounterAddDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_dml_timeseries_schema_write&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="timeseries_schema_write_descriptor"&&member.operands.front().name=="write"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::timeseries_schema_write_descriptor){scratchbird::engine::sblr::SblrDmlTimeseriesSchemaWriteDescriptorV1 operand;std::string detail;exact_local_dml_timeseries_schema_write=scratchbird::engine::sblr::DecodeSblrDmlTimeseriesSchemaWriteDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_timeseries_series_cardinality_policy&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="timeseries_series_cardinality_policy_descriptor"&&member.operands.front().name=="policy"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::timeseries_series_cardinality_policy_descriptor){scratchbird::engine::sblr::SblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1 operand;std::string detail;exact_local_ddl_timeseries_series_cardinality_policy=scratchbird::engine::sblr::DecodeSblrDdlTimeseriesSeriesCardinalityPolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_timeseries_value_cache&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="timeseries_value_cache_descriptor"&&member.operands.front().name=="cache"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::timeseries_value_cache_descriptor){scratchbird::engine::sblr::SblrDdlCreateTimeseriesValueCacheDescriptorV1 operand;std::string detail;exact_local_ddl_create_timeseries_value_cache=scratchbird::engine::sblr::DecodeSblrDdlCreateTimeseriesValueCacheDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_temporary_table&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="create_temporary_table_descriptor"&&member.operands.front().name=="temporary_table"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_temporary_table_descriptor){scratchbird::engine::sblr::SblrDdlCreateTemporaryTableDescriptorV1 operand;std::string detail;exact_local_ddl_create_temporary_table=scratchbird::engine::sblr::DecodeSblrDdlCreateTemporaryTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_temporary_table&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_temporary_table_descriptor"&&member.operands.front().name=="temporary_table"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_temporary_table_descriptor){scratchbird::engine::sblr::SblrDdlDropTemporaryTableDescriptorV1 operand;std::string detail;exact_local_ddl_drop_temporary_table=scratchbird::engine::sblr::DecodeSblrDdlDropTemporaryTableDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_rename_object_vector&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="object_rename_vector_descriptor"&&member.operands.front().name=="rename_vector"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::object_rename_vector_descriptor){scratchbird::engine::sblr::SblrDdlRenameObjectVectorDescriptorV1 operand;std::string detail;exact_local_ddl_rename_object_vector=scratchbird::engine::sblr::DecodeSblrDdlRenameObjectVectorDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_rename_object&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="rename_object_descriptor"&&member.operands.front().name=="rename"&&(member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::rename_object_descriptor||member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::object_rename_vector_descriptor)){scratchbird::engine::sblr::SblrDdlRenameObjectDescriptorV1 operand;std::string detail;exact_local_ddl_rename_object=scratchbird::engine::sblr::DecodeSblrDdlRenameObjectDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_or_replace_srs&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="spatial_reference_system_descriptor"&&member.operands.front().name=="srs"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::spatial_reference_system_descriptor){scratchbird::engine::sblr::SblrDdlCreateOrReplaceSrsDescriptorV1 operand;std::string detail;exact_local_ddl_create_or_replace_srs=scratchbird::engine::sblr::DecodeSblrDdlCreateOrReplaceSrsDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_srs&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="spatial_reference_system_drop_descriptor"&&member.operands.front().name=="srs"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::spatial_reference_system_drop_descriptor){scratchbird::engine::sblr::SblrDdlDropSrsDescriptorV1 operand;std::string detail;exact_local_ddl_drop_srs=scratchbird::engine::sblr::DecodeSblrDdlDropSrsDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_rewrite_rule&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="rewrite_rule_descriptor"&&member.operands.front().name=="rewrite_rule"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::rewrite_rule_descriptor){scratchbird::engine::sblr::SblrDdlCreateRewriteRuleDescriptorV1 operand;std::string detail;exact_local_ddl_create_rewrite_rule=scratchbird::engine::sblr::DecodeSblrDdlCreateRewriteRuleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_rewrite_rule&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="rewrite_rule_alter_descriptor"&&member.operands.front().name=="rewrite_rule"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::rewrite_rule_alter_descriptor){scratchbird::engine::sblr::SblrDdlAlterRewriteRuleDescriptorV1 operand;std::string detail;exact_local_ddl_alter_rewrite_rule=scratchbird::engine::sblr::DecodeSblrDdlAlterRewriteRuleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_rewrite_rule&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="rewrite_rule_drop_descriptor"&&member.operands.front().name=="rewrite_rule"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::rewrite_rule_drop_descriptor){scratchbird::engine::sblr::SblrDdlDropRewriteRuleDescriptorV1 operand;std::string detail;exact_local_ddl_drop_rewrite_rule=scratchbird::engine::sblr::DecodeSblrDdlDropRewriteRuleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_validate_constraint&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="constraint_validation_descriptor"&&member.operands.front().name=="constraint"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::constraint_validation_descriptor){scratchbird::engine::sblr::SblrDdlValidateConstraintDescriptorV1 operand;std::string detail;exact_local_ddl_validate_constraint=scratchbird::engine::sblr::DecodeSblrDdlValidateConstraintDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_security_create_privilege_template&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="privilege_template_descriptor"&&member.operands.front().name=="privilege_template"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::privilege_template_descriptor){scratchbird::engine::sblr::SblrSecurityCreatePrivilegeTemplateDescriptorV1 operand;std::string detail;exact_local_security_create_privilege_template=scratchbird::engine::sblr::DecodeSblrSecurityCreatePrivilegeTemplateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_create_user&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="security_create_user_descriptor"&&member.operands.front().name=="user"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_create_user_descriptor){scratchbird::engine::sblr::SblrSecurityCreateUserDescriptorV1 operand;std::string detail;exact_local_security_create_user=scratchbird::engine::sblr::DecodeSblrSecurityCreateUserDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_alter_user&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_user_descriptor"&&member.operands.front().name=="user"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_user_descriptor){scratchbird::engine::sblr::SblrSecAlterUserDescriptorV1 operand;std::string detail;exact_local_security_alter_user=scratchbird::engine::sblr::DecodeSblrSecAlterUserDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_create_role&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="security_create_role_descriptor"&&member.operands.front().name=="role"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_create_role_descriptor){scratchbird::engine::sblr::SblrSecCreateRoleDescriptorV1 operand;std::string detail;exact_local_security_create_role=scratchbird::engine::sblr::DecodeSblrSecCreateRoleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_drop_role&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_role_descriptor"&&member.operands.front().name=="role"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_drop_role_descriptor){scratchbird::engine::sblr::SblrSecDropRoleDescriptorV1 operand;std::string detail;exact_local_security_drop_role=scratchbird::engine::sblr::DecodeSblrSecDropRoleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_create_policy&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="security_policy_descriptor"&&member.operands.front().name=="policy"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_create_policy_descriptor){scratchbird::engine::sblr::SblrSecCreatePolicyDescriptorV1 operand;std::string detail;exact_local_security_create_policy=scratchbird::engine::sblr::DecodeSblrSecCreatePolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_drop_policy&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_security_policy_descriptor"&&member.operands.front().name=="policy"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_drop_policy_descriptor){scratchbird::engine::sblr::SblrSecDropPolicyDescriptorV1 operand;std::string detail;exact_local_security_drop_policy=scratchbird::engine::sblr::DecodeSblrSecDropPolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_alter_role&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="security_alter_role_descriptor"&&member.operands.front().name=="role"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_alter_role_descriptor){scratchbird::engine::sblr::SblrSecAlterRoleDescriptorV1 operand;std::string detail;exact_local_security_alter_role=scratchbird::engine::sblr::DecodeSblrSecAlterRoleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_create_group_mapping&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="security_create_group_mapping_descriptor"&&member.operands.front().name=="group_mapping"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_create_group_mapping_descriptor){scratchbird::engine::sblr::SblrSecCreateGroupMappingDescriptorV1 operand;std::string detail;exact_local_security_create_group_mapping=scratchbird::engine::sblr::DecodeSblrSecCreateGroupMappingDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_drop_group_mapping&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_group_mapping_descriptor"&&member.operands.front().name=="group_mapping"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_drop_group_mapping_descriptor){scratchbird::engine::sblr::SblrSecDropGroupMappingDescriptorV1 operand;std::string detail;exact_local_security_drop_group_mapping=scratchbird::engine::sblr::DecodeSblrSecDropGroupMappingDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_grant&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="grant_descriptor"&&member.operands.front().name=="grant"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_grant_descriptor){scratchbird::engine::sblr::SblrSecGrantDescriptorV1 operand;std::string detail;exact_local_security_grant=scratchbird::engine::sblr::DecodeSblrSecGrantDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_revoke&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="revoke_descriptor"&&member.operands.front().name=="revoke"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_revoke_descriptor){scratchbird::engine::sblr::SblrSecRevokeDescriptorV1 operand;std::string detail;exact_local_security_revoke=scratchbird::engine::sblr::DecodeSblrSecRevokeDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_alter_policy&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="security_policy_descriptor"&&member.operands.front().name=="policy"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_alter_policy_descriptor){scratchbird::engine::sblr::SblrSecAlterPolicyDescriptorV1 operand;std::string detail;exact_local_security_alter_policy=scratchbird::engine::sblr::DecodeSblrSecAlterPolicyDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_drop_user&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="user_descriptor"&&member.operands.front().name=="user"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_drop_user_descriptor){scratchbird::engine::sblr::SblrSecDropUserDescriptorV1 operand;std::string detail;exact_local_security_drop_user=scratchbird::engine::sblr::DecodeSblrSecDropUserDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_authenticate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="authenticate_descriptor"&&member.operands.front().name=="authenticate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_authenticate_descriptor){scratchbird::engine::sblr::SblrSecAuthenticateDescriptorV1 operand;std::string detail;exact_local_security_authenticate=scratchbird::engine::sblr::DecodeSblrSecAuthenticateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_security_deauthenticate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="deauthenticate_descriptor"&&member.operands.front().name=="deauthenticate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::security_deauthenticate_descriptor){scratchbird::engine::sblr::SblrSecDeauthenticateDescriptorV1 operand;std::string detail;exact_local_security_deauthenticate=scratchbird::engine::sblr::DecodeSblrSecDeauthenticateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_role_switch&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="session_role_switch_descriptor"&&member.operands.front().name=="role_switch"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_role_switch_descriptor){scratchbird::engine::sblr::SblrSessionRoleSwitchDescriptorV1 operand;std::string detail;exact_local_session_role_switch=scratchbird::engine::sblr::DecodeSblrSessionRoleSwitchDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_setting_set&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="session_setting_set_descriptor"&&member.operands.front().name=="setting"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_setting_set_descriptor){scratchbird::engine::sblr::SblrSessionSettingSetDescriptorV1 operand;std::string detail;exact_local_session_setting_set=scratchbird::engine::sblr::DecodeSblrSessionSettingSetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_setting_reset&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="session_setting_reset_descriptor"&&member.operands.front().name=="setting"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_setting_reset_descriptor){scratchbird::engine::sblr::SblrSessionSettingResetDescriptorV1 operand;std::string detail;exact_local_session_setting_reset=scratchbird::engine::sblr::DecodeSblrSessionSettingResetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_setting_get&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="session_setting_get_descriptor"&&member.operands.front().name=="setting"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_setting_get_descriptor){scratchbird::engine::sblr::SblrSessionSettingGetDescriptorV1 operand;std::string detail;exact_local_session_setting_get=scratchbird::engine::sblr::DecodeSblrSessionSettingGetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_default_qualifier_set&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="default_qualifier_descriptor"&&member.operands.front().name=="qualifier"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_default_qualifier_set_descriptor){scratchbird::engine::sblr::SblrSessionDefaultQualifierSetDescriptorV1 operand;std::string detail;exact_local_session_default_qualifier_set=scratchbird::engine::sblr::DecodeSblrSessionDefaultQualifierSetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_discard&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="session_discard_descriptor"&&member.operands.front().name=="discard"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_discard_descriptor){scratchbird::engine::sblr::SblrSessionDiscardDescriptorV1 operand;std::string detail;exact_local_session_discard=scratchbird::engine::sblr::DecodeSblrSessionDiscardDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()>=2&&exact_session_snapshot_handle&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="snapshot_handle_descriptor"&&member.operands.front().name=="snapshot"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::session_snapshot_handle_descriptor){scratchbird::engine::sblr::SblrSessionSnapshotHandleDescriptorV1 operand;std::string detail;exact_local_session_snapshot_handle=scratchbird::engine::sblr::DecodeSblrSessionSnapshotHandleDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_security_alter_privilege_template&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="privilege_template_alter_descriptor"&&member.operands.front().name=="privilege_template"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::privilege_template_alter_descriptor){scratchbird::engine::sblr::SblrSecurityAlterPrivilegeTemplateDescriptorV1 operand;std::string detail;exact_local_security_alter_privilege_template=scratchbird::engine::sblr::DecodeSblrSecurityAlterPrivilegeTemplateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_security_drop_privilege_template&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="privilege_template_drop_descriptor"&&member.operands.front().name=="privilege_template"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::privilege_template_drop_descriptor){scratchbird::engine::sblr::SblrSecurityDropPrivilegeTemplateDescriptorV1 operand;std::string detail;exact_local_security_drop_privilege_template=scratchbird::engine::sblr::DecodeSblrSecurityDropPrivilegeTemplateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_database_create_template_clone&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="template_database_creation_descriptor"&&member.operands.front().name=="template_clone"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::template_database_creation_descriptor){scratchbird::engine::sblr::SblrDatabaseCreateTemplateCloneDescriptorV1 operand;std::string detail;exact_local_database_create_template_clone=scratchbird::engine::sblr::DecodeSblrDatabaseCreateTemplateCloneDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_aggregate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="aggregate_descriptor"&&member.operands.front().name=="aggregate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::ddl_create_aggregate_descriptor){scratchbird::engine::sblr::SblrDdlCreateAggregateDescriptorV1 operand;std::string detail;exact_local_ddl_create_aggregate=scratchbird::engine::sblr::DecodeSblrDdlCreateAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_aggregate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="aggregate_alter_descriptor"&&member.operands.front().name=="aggregate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::ddl_alter_aggregate_descriptor){scratchbird::engine::sblr::SblrDdlAlterAggregateDescriptorV1 operand;std::string detail;exact_local_ddl_alter_aggregate=scratchbird::engine::sblr::DecodeSblrDdlAlterAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_aggregate&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="aggregate_drop_descriptor"&&member.operands.front().name=="aggregate"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::ddl_drop_aggregate_descriptor){scratchbird::engine::sblr::SblrDdlDropAggregateDescriptorV1 operand;std::string detail;exact_local_ddl_drop_aggregate=scratchbird::engine::sblr::DecodeSblrDdlDropAggregateDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_dictionary&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="external_dictionary_drop_descriptor"&&member.operands.front().name=="dictionary"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::external_dictionary_drop_descriptor){scratchbird::engine::sblr::SblrDdlDropDictionaryDescriptorV1 operand;std::string detail;exact_local_ddl_drop_dictionary=scratchbird::engine::sblr::DecodeSblrDdlDropDictionaryDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_purge_system_history&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="system_history_purge_descriptor"&&member.operands.front().name=="history"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::system_history_purge_descriptor){scratchbird::engine::sblr::SblrDdlPurgeSystemHistoryDescriptorV1 operand;std::string detail;exact_local_ddl_purge_system_history=scratchbird::engine::sblr::DecodeSblrDdlPurgeSystemHistoryDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_ddl_set_index_optimizer_eligibility=request.root_opcode_code==1629&&request.root_opcode=="SBLR_DDL_SET_INDEX_OPTIMIZER_ELIGIBILITY"&&request.root_operation_id=="engine.op.ddl_set_index_optimizer_eligibility"; bool exact_local_ddl_set_index_optimizer_eligibility=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_set_index_optimizer_eligibility&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="index_optimizer_eligibility_descriptor"&&member.operands.front().name=="index"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::index_optimizer_eligibility_descriptor){scratchbird::engine::sblr::SblrDdlSetIndexOptimizerEligibilityDescriptorV1 operand;std::string detail;exact_local_ddl_set_index_optimizer_eligibility=scratchbird::engine::sblr::DecodeSblrDdlSetIndexOptimizerEligibilityDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_ddl_set_table_type_enforcement=request.root_opcode_code==1630&&request.root_opcode=="SBLR_DDL_SET_TABLE_TYPE_ENFORCEMENT"&&request.root_operation_id=="engine.op.ddl_set_table_type_enforcement"; bool exact_local_ddl_set_table_type_enforcement=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_set_table_type_enforcement&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="table_type_enforcement_descriptor"&&member.operands.front().name=="table"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::table_type_enforcement_descriptor){scratchbird::engine::sblr::SblrDdlSetTableTypeEnforcementDescriptorV1 operand;std::string detail;exact_local_ddl_set_table_type_enforcement=scratchbird::engine::sblr::DecodeSblrDdlSetTableTypeEnforcementDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_database_serialize_logical_snapshot=request.root_opcode_code==1631&&request.root_opcode=="SBLR_DATABASE_SERIALIZE_LOGICAL_SNAPSHOT"&&request.root_operation_id=="engine.op.database_serialize_logical_snapshot"; bool exact_local_database_serialize_logical_snapshot=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_database_serialize_logical_snapshot&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="logical_snapshot_serialization_descriptor"&&member.operands.front().name=="snapshot"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::logical_snapshot_serialization_descriptor){scratchbird::engine::sblr::SblrDatabaseSerializeLogicalSnapshotDescriptorV1 operand;std::string detail;exact_local_database_serialize_logical_snapshot=scratchbird::engine::sblr::DecodeSblrDatabaseSerializeLogicalSnapshotDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_database_deserialize_logical_snapshot=request.root_opcode_code==1632&&request.root_opcode=="SBLR_DATABASE_DESERIALIZE_LOGICAL_SNAPSHOT"&&request.root_operation_id=="engine.op.database_deserialize_logical_snapshot"; bool exact_local_database_deserialize_logical_snapshot=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_database_deserialize_logical_snapshot&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="logical_snapshot_deserialization_descriptor"&&member.operands.front().name=="snapshot"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::logical_snapshot_deserialization_descriptor){scratchbird::engine::sblr::SblrDatabaseDeserializeLogicalSnapshotDescriptorV1 operand;std::string detail;exact_local_database_deserialize_logical_snapshot=scratchbird::engine::sblr::DecodeSblrDatabaseDeserializeLogicalSnapshotDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_ddl_create_macro=request.root_opcode_code==1633&&request.root_opcode=="SBLR_DDL_CREATE_MACRO"&&request.root_operation_id=="engine.op.ddl_create_macro"; bool exact_local_ddl_create_macro=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_macro&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="macro_descriptor"&&member.operands.front().name=="macro"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::macro_descriptor){scratchbird::engine::sblr::SblrDdlCreateMacroDescriptorV1 operand;std::string detail;exact_local_ddl_create_macro=scratchbird::engine::sblr::DecodeSblrDdlCreateMacroDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_ddl_drop_macro=request.root_opcode_code==1634&&request.root_opcode=="SBLR_DDL_DROP_MACRO"&&request.root_operation_id=="engine.op.ddl_drop_macro"; bool exact_local_ddl_drop_macro=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_macro&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="macro_drop_descriptor"&&member.operands.front().name=="macro"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::macro_drop_descriptor){scratchbird::engine::sblr::SblrDdlDropMacroDescriptorV1 operand;std::string detail;exact_local_ddl_drop_macro=scratchbird::engine::sblr::DecodeSblrDdlDropMacroDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_admin_register_external_relation_resolver=request.root_opcode_code==1635&&request.root_opcode=="SBLR_ADMIN_REGISTER_EXTERNAL_RELATION_RESOLVER"&&request.root_operation_id=="engine.op.admin_register_external_relation_resolver"; bool exact_local_admin_register_external_relation_resolver=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_admin_register_external_relation_resolver&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="external_relation_resolver_registration_descriptor"&&member.operands.front().name=="resolver"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::external_relation_resolver_registration_descriptor){scratchbird::engine::sblr::SblrAdminRegisterExternalRelationResolverDescriptorV1 operand;std::string detail;exact_local_admin_register_external_relation_resolver=scratchbird::engine::sblr::DecodeSblrAdminRegisterExternalRelationResolverDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_admin_unregister_external_relation_resolver=request.root_opcode_code==1636&&request.root_opcode=="SBLR_ADMIN_UNREGISTER_EXTERNAL_RELATION_RESOLVER"&&request.root_operation_id=="engine.op.admin_unregister_external_relation_resolver"; bool exact_local_admin_unregister_external_relation_resolver=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_admin_unregister_external_relation_resolver&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="external_relation_resolver_unregistration_descriptor"&&member.operands.front().name=="resolver"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::external_relation_resolver_unregistration_descriptor){scratchbird::engine::sblr::SblrAdminUnregisterExternalRelationResolverDescriptorV1 operand;std::string detail;exact_local_admin_unregister_external_relation_resolver=scratchbird::engine::sblr::DecodeSblrAdminUnregisterExternalRelationResolverDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  const bool exact_ddl_create_dictionary=(request.root_opcode_code==1637||request.root_opcode_code==1608)&&request.root_opcode=="SBLR_DDL_CREATE_DICTIONARY"&&request.root_operation_id=="engine.op.ddl_create_dictionary"; bool exact_local_ddl_create_dictionary=false; if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_create_dictionary&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="external_dictionary_descriptor"&&member.operands.front().name=="dictionary"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::external_dictionary_descriptor){scratchbird::engine::sblr::SblrDdlCreateDictionaryDescriptorV1 operand;std::string detail;exact_local_ddl_create_dictionary=scratchbird::engine::sblr::DecodeSblrDdlCreateDictionaryDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_ddl_create_dictionary && !request.cluster_context_active && !request.cluster_transaction_active && !request.route_fence_present) exact_local_ddl_create_dictionary = true;
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_function&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_function_descriptor"&&member.operands.front().name=="function"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_function_descriptor){scratchbird::engine::sblr::SblrDdlDropFunctionDescriptorV1 operand;std::string detail;exact_local_ddl_drop_function=scratchbird::engine::sblr::DecodeSblrDdlDropFunctionDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_alter_function&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="alter_function_descriptor"&&member.operands.front().name=="function"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::alter_function_descriptor){scratchbird::engine::sblr::SblrDdlAlterFunctionDescriptorV1 operand;std::string detail;exact_local_ddl_alter_function=scratchbird::engine::sblr::DecodeSblrDdlAlterFunctionDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_local_ddl_alter_function) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_function) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_package) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_synonym) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_foreign_table) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_fdw) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_fdw) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_foreign_table) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_package) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_package) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_sequence || exact_local_ddl_drop_sequence) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_materialized_view) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_type) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_type) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_type) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_temporary_table) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_temporary_table) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_or_replace_srs) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_rename_object_vector) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_rename_object) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_rewrite_rule) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_rewrite_rule) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_rewrite_rule) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_validate_constraint) exact_local_ddl_create_schema = true;
  if (exact_local_security_create_privilege_template) exact_local_ddl_create_schema = true;
  if (exact_local_security_create_user) exact_local_ddl_create_schema = true;
  if (exact_local_security_alter_user) exact_local_ddl_create_schema = true;
  if (exact_local_security_alter_privilege_template) exact_local_ddl_create_schema = true;
  if (exact_local_security_drop_privilege_template) exact_local_ddl_create_schema = true;
  if (exact_local_database_create_template_clone) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_aggregate) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_aggregate) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_aggregate) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_dictionary) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_dictionary) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_purge_system_history) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_set_table_type_enforcement) exact_local_ddl_create_schema = true;
  if (exact_local_database_serialize_logical_snapshot) exact_local_ddl_create_schema = true;
  if (exact_local_database_deserialize_logical_snapshot) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_macro) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_macro) exact_local_ddl_create_schema = true;
  if (exact_local_admin_register_external_relation_resolver) exact_local_ddl_create_schema = true;
  if (exact_local_admin_unregister_external_relation_resolver) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_dictionary) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_continuous_view) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_alter_continuous_view) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_drop_continuous_view) exact_local_ddl_create_schema = true;
  if (exact_local_dml_async_insert_submit) exact_local_ddl_create_schema = true;
  if (exact_local_dml_async_insert_status) exact_local_ddl_create_schema = true;
  if (exact_local_dml_async_insert_cancel) exact_local_ddl_create_schema = true;
  if (exact_local_dml_counter_add) exact_local_ddl_create_schema = true;
  if (exact_local_dml_timeseries_schema_write) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_timeseries_series_cardinality_policy) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_create_timeseries_value_cache) exact_local_ddl_create_schema = true;
  if (exact_local_ddl_set_index_optimizer_eligibility) exact_local_ddl_create_schema = true;
  if (exact_kv_structured_mutate && !exact_local_kv_structured_mutate) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_kv_structured_stream_append && !exact_local_kv_structured_stream_append) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_kv_structured_timeseries && !exact_local_kv_structured_timeseries) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_system_config_set && !exact_local_system_config_set) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_domain && !exact_local_ddl_create_domain) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_domain && !exact_local_ddl_alter_domain) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_view && !exact_local_ddl_create_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_view && !exact_local_ddl_alter_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_view && !exact_local_ddl_drop_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_trigger && !exact_local_ddl_create_trigger) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_trigger && !exact_local_ddl_alter_trigger) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_trigger && !exact_local_ddl_drop_trigger) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_procedure && !exact_local_ddl_create_procedure) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_procedure && !exact_local_ddl_alter_procedure) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_procedure && !exact_local_ddl_drop_procedure) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_function && !exact_local_ddl_create_function) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_function && !exact_local_ddl_alter_function) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_function && !exact_local_ddl_drop_function) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_package && !exact_local_ddl_create_package) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_synonym && !exact_local_ddl_create_synonym) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_foreign_table && !exact_local_ddl_create_foreign_table) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_fdw && !exact_local_ddl_create_fdw) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_fdw && !exact_local_ddl_drop_fdw) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_package && !exact_local_ddl_drop_package) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_synonym && !exact_local_ddl_drop_synonym) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_foreign_table && !exact_local_ddl_drop_foreign_table) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_package && !exact_local_ddl_alter_package) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_sequence && !exact_local_ddl_alter_sequence) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_sequence && !exact_local_ddl_drop_sequence) return Refuse(request,"SBLR.OPERAND.INVALID");
  const bool exact_ddl_create_table_as_query_with_data=request.root_opcode_code==1669&&request.root_opcode=="SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_DATA"&&request.root_operation_id=="engine.op.ddl_create_table_as_query_with_data";
  const bool exact_ddl_create_table_as_query_with_no_data=request.root_opcode_code==1670&&request.root_opcode=="SBLR_DDL_CREATE_TABLE_AS_QUERY_WITH_NO_DATA"&&request.root_operation_id=="engine.op.ddl_create_table_as_query_with_no_data";
  const bool exact_local_ddl_create_table_as_query=(stream.ok&&stream.stream.operations.size()>=3&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present&&std::any_of(stream.stream.operations.begin(),stream.stream.operations.end(),[](const auto& op){return op.operands.size()==1&&(op.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_table_as_query_with_data_descriptor||op.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::create_table_as_query_with_no_data_descriptor);})) || (exact_ddl_create_table_as_query_with_data||exact_ddl_create_table_as_query_with_no_data);
  if ((exact_ddl_create_table_as_query_with_data||exact_ddl_create_table_as_query_with_no_data)&&!exact_local_ddl_create_table_as_query)return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_local_ddl_create_table_as_query) exact_local_ddl_create_schema = true;
  if (exact_ddl_create_materialized_view && !exact_local_ddl_create_materialized_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_type && !exact_local_ddl_create_type) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_sequence && !exact_local_ddl_create_sequence) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_type && !exact_local_ddl_alter_type) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_type && !exact_local_ddl_drop_type) return Refuse(request,"SBLR.OPERAND.INVALID");
  if(stream.ok&&stream.stream.operations.size()==3&&exact_ddl_drop_materialized_view&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto&member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="drop_materialized_view_descriptor"&&member.operands.front().name=="view"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::drop_materialized_view_descriptor){scratchbird::engine::sblr::SblrDdlDropMaterializedViewDescriptorV1 operand;std::string detail;exact_local_ddl_drop_materialized_view=scratchbird::engine::sblr::DecodeSblrDdlDropMaterializedViewDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_ddl_drop_materialized_view && !exact_local_ddl_drop_materialized_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_refresh_materialized_view && !exact_local_ddl_refresh_materialized_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_temporary_table && !exact_local_ddl_create_temporary_table) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_temporary_table && !exact_local_ddl_drop_temporary_table) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_or_replace_srs && !exact_local_ddl_create_or_replace_srs) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_srs && !exact_local_ddl_drop_srs) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_rename_object_vector && !exact_local_ddl_rename_object_vector) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_rename_object && !exact_local_ddl_rename_object) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_schema && !exact_local_ddl_create_schema) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_local_diagnostic_refusal) exact_local_ddl_create_schema = true;
  if (exact_ddl_create_table && !exact_local_ddl_create_table) return Refuse(request,"SBLR.OPERAND.INVALID");
  const bool exact_ddl_drop_table = request.root_opcode_code == 1539 && request.root_opcode == "SBLR_DDL_DROP_TABLE" && request.root_operation_id == "engine.op.ddl_drop_table";
  bool exact_local_ddl_drop_table = false;
  if (stream.ok && stream.stream.operations.size() == 3 && exact_ddl_drop_table &&
      !request.cluster_context_active && !request.cluster_transaction_active &&
      !request.route_fence_present) {
    const auto& member = stream.stream.operations[1];
    if (member.operands.size() == 1 && member.operands.front().type == "drop_table_descriptor" &&
        member.operands.front().name == "table" &&
        member.operands.front().value_kind == scratchbird::engine::sblr::SblrValueKind::drop_table_descriptor) {
      scratchbird::engine::sblr::SblrDdlDropTableDescriptorV1 operand;
      std::string detail;
      exact_local_ddl_drop_table = scratchbird::engine::sblr::DecodeSblrDdlDropTableDescriptorV1(
          member.operands.front().value_body.data(), member.operands.front().value_body.size(),
          &operand, &detail, true);
    }
  }
  if (exact_ddl_drop_table && !exact_local_ddl_drop_table) {
    return Refuse(request, request.cluster_context_active ? "CLUSTER.ROUTE_REFUSED" : "SBLR.OPERAND.INVALID");
  }
  if (exact_local_ddl_drop_table) exact_local_ddl_create_schema = true;
  if (exact_ddl_create_index && !exact_local_ddl_create_index) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_index && !exact_local_ddl_drop_index) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_create_privilege_template && !exact_local_security_create_privilege_template) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_create_user && !exact_local_security_create_user) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_alter_user && !exact_local_security_alter_user) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_create_role && !exact_local_security_create_role) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_drop_role && !exact_local_security_drop_role) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_create_policy && !exact_local_security_create_policy) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_drop_policy && !exact_local_security_drop_policy) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_alter_role && !exact_local_security_alter_role) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_create_group_mapping && !exact_local_security_create_group_mapping) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_drop_group_mapping && !exact_local_security_drop_group_mapping) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_grant && !exact_local_security_grant) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_revoke && !exact_local_security_revoke) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_alter_policy && !exact_local_security_alter_policy) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_drop_user && !exact_local_security_drop_user) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_authenticate && !exact_local_security_authenticate) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_deauthenticate && !exact_local_security_deauthenticate) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_role_switch && !exact_local_session_role_switch) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_setting_set && !exact_local_session_setting_set) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_setting_reset && !exact_local_session_setting_reset) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_setting_get && !exact_local_session_setting_get) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_default_qualifier_set && !exact_local_session_default_qualifier_set) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_discard && !exact_local_session_discard) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_session_snapshot_handle && !exact_local_session_snapshot_handle) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_local_security_create_role) exact_local_ddl_create_schema = true;
  if (exact_local_security_drop_role) exact_local_ddl_create_schema = true;
  if (exact_local_security_create_policy) exact_local_ddl_create_schema = true;
  if (exact_local_security_drop_policy) exact_local_ddl_create_schema = true;
  if (exact_local_security_alter_role) exact_local_ddl_create_schema = true;
  if (exact_local_security_create_group_mapping) exact_local_ddl_create_schema = true;
  if (exact_local_security_drop_group_mapping) exact_local_ddl_create_schema = true;
  if (exact_local_security_grant) exact_local_ddl_create_schema = true;
  if (exact_local_security_revoke) exact_local_ddl_create_schema = true;
  if (exact_local_security_alter_policy) exact_local_ddl_create_schema = true;
  if (exact_local_security_drop_user) exact_local_ddl_create_schema = true;
  if (exact_security_alter_privilege_template && !exact_local_security_alter_privilege_template) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_security_drop_privilege_template && !exact_local_security_drop_privilege_template) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_database_create_template_clone && !exact_local_database_create_template_clone) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_aggregate && !exact_local_ddl_create_aggregate) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_aggregate && !exact_local_ddl_alter_aggregate) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_aggregate && !exact_local_ddl_drop_aggregate) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_dictionary && !exact_local_ddl_drop_dictionary) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_dictionary && !exact_local_ddl_alter_dictionary) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_continuous_view && !exact_local_ddl_create_continuous_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_alter_continuous_view && !exact_local_ddl_alter_continuous_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_continuous_view && !exact_local_ddl_drop_continuous_view) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_dml_async_insert_submit && !exact_local_dml_async_insert_submit) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_dml_async_insert_status && !exact_local_dml_async_insert_status)
  if (exact_dml_async_insert_cancel && !exact_local_dml_async_insert_cancel) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_dml_counter_add && !exact_local_dml_counter_add) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_dml_timeseries_schema_write && !exact_local_dml_timeseries_schema_write) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_timeseries_series_cardinality_policy && !exact_local_ddl_timeseries_series_cardinality_policy) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_timeseries_value_cache && !exact_local_ddl_create_timeseries_value_cache) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_timeseries_series_cardinality_policy && !exact_local_ddl_timeseries_series_cardinality_policy) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_purge_system_history && !exact_local_ddl_purge_system_history) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_set_index_optimizer_eligibility && !exact_local_ddl_set_index_optimizer_eligibility) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_set_table_type_enforcement && !exact_local_ddl_set_table_type_enforcement) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_database_serialize_logical_snapshot && !exact_local_database_serialize_logical_snapshot) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_database_deserialize_logical_snapshot && !exact_local_database_deserialize_logical_snapshot) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_macro && !exact_local_ddl_create_macro) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_drop_macro && !exact_local_ddl_drop_macro) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_admin_register_external_relation_resolver && !exact_local_admin_register_external_relation_resolver) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_admin_unregister_external_relation_resolver && !exact_local_admin_unregister_external_relation_resolver) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_ddl_create_dictionary && !exact_local_ddl_create_dictionary) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (exact_kv_structured_scan && !exact_local_kv_structured_scan && !exact_local_kv_structured_stream_read) return Refuse(request,"SBLR.OPERAND.INVALID");
  bool exact_local_return_result_set = false;
  if (exact_kv_structured_read) exact_local_return_result_set = true;
  if (stream.ok&&stream.stream.operations.size()==3&&exact_return_result_set&&!request.cluster_context_active&&!request.cluster_transaction_active&&!request.route_fence_present){const auto& member=stream.stream.operations[1];if(member.operands.size()==1&&member.operands.front().type=="result_set_return_descriptor"&&member.operands.front().name=="result_set"&&member.operands.front().value_kind==scratchbird::engine::sblr::SblrValueKind::result_set_return_descriptor){scratchbird::engine::sblr::SblrReturnResultSetDescriptorV1 operand;std::string detail;exact_local_return_result_set=scratchbird::engine::sblr::DecodeSblrReturnResultSetDescriptorV1(member.operands.front().value_body.data(),member.operands.front().value_body.size(),&operand,&detail,true);}}
  if (exact_return_result_set && !exact_local_return_result_set) return Refuse(request,"SBLR.OPERAND.INVALID");
  if (!stream.ok || stream.stream.operations.size() != 3 ||
      (!exact_query && !exact_source_map && !exact_error_vector &&
       !exact_local_show_version && !exact_local_show_database && !exact_local_show_catalog && !exact_local_show_sessions && !exact_local_show_transactions && !exact_local_show_locks && !exact_local_show_statements && !exact_local_show_jobs && !exact_local_show_management && !exact_local_show_diagnostics && !exact_local_show_diagnostics_extended && !exact_local_show_archive_replication && !exact_local_show_agents_extended && !exact_local_show_filespace_extended && !exact_local_show_decision_service && !exact_local_show_metrics && !exact_local_show_acceleration && !exact_local_show_acceleration_extended && !exact_local_explain_operation && !exact_local_lifecycle_open_database && !exact_local_lifecycle_attach_database && !exact_local_lifecycle_detach_database && !exact_local_lifecycle_enter_maintenance && !exact_local_lifecycle_exit_maintenance && !exact_local_metrics_read && !exact_local_catalog_introspect && !exact_cluster_root &&
       !exact_local_security_authenticate && !exact_local_security_deauthenticate && !exact_local_session_role_switch && !exact_local_session_setting_set && !exact_local_session_setting_reset && !exact_local_session_setting_get && !exact_local_session_default_qualifier_set && !exact_local_session_discard && !exact_local_session_snapshot_handle && !exact_local_txn_begin && !exact_local_txn_commit &&
       !exact_local_txn_rollback && !exact_local_txn_savepoint &&
       !exact_local_txn_release_savepoint && !exact_local_txn_rollback_to_savepoint &&
       !exact_local_psql_autonomous_frame && !exact_local_reservation_release && !exact_local_temporary_cleanup && !exact_local_cursor_open && !exact_local_cursor_fetch && !exact_local_cursor_close && !exact_local_read_by_key && !exact_local_read_range && !exact_local_read_stream && !exact_local_result_set_pass && !exact_local_access_cursor_open && !exact_local_access_cursor_fetch && !exact_local_access_cursor_close && !exact_local_insert && !exact_local_update && !exact_local_delete && !exact_local_merge && !exact_local_table_truncate && !exact_local_table_analyze && !exact_local_bulk_import_stream && !exact_local_bulk_export_stream && !exact_local_statement_batch && !exact_local_atomic_cas && !exact_local_atomic_rmw && !exact_local_advisory_lock && !exact_local_advisory_lock_release && !exact_local_function_call && !exact_local_operator_call && !exact_local_cast && !exact_local_compare && !exact_local_domain_operation && !exact_local_udr && !exact_local_procedure && !exact_local_function_invoke && !exact_local_aggregate_invoke && !exact_local_sequence_nextval && !exact_local_sequence_currval && !exact_local_sequence_setval && !exact_local_query_numeric && !exact_local_advanced_datatype_family && !exact_local_project && !exact_local_aggregate && !exact_local_group && !exact_local_sort && !exact_local_limit && !exact_local_return_result_set && !exact_local_kv_structured_read && !exact_local_kv_structured_mutate && !exact_local_kv_structured_scan && !exact_local_kv_structured_stream_read && !exact_local_kv_structured_stream_append && !exact_local_kv_structured_timeseries && !exact_local_system_config_set && !exact_local_ddl_create_domain && !exact_local_ddl_alter_domain && !exact_local_ddl_create_view && !exact_local_ddl_alter_view && !exact_local_ddl_drop_view && !exact_local_ddl_create_schema && !exact_local_ddl_create_table && !exact_local_ddl_create_index && !exact_local_ddl_drop_index && !exact_local_ddl_create_procedure && !exact_local_ddl_alter_procedure && !exact_local_ddl_drop_procedure && !exact_local_ddl_create_function && !exact_local_ddl_alter_function && !exact_local_ddl_drop_function && !exact_local_ddl_drop_temporary_table && !exact_local_ddl_rename_object_vector && !exact_local_ddl_create_or_replace_srs && !exact_local_ddl_drop_srs) ||
      stream.stream.operations[1].opcode_code != request.root_opcode_code ||
      stream.stream.operations[1].opcode != request.root_opcode ||
      stream.stream.operations[1].operation_id != request.root_operation_id ||
      !request.route_snapshot_engine_owned ||
      !request.security_snapshot_engine_owned ||
      !CanonicalNonzeroUuid(request.route_snapshot_uuid) ||
      !CanonicalNonzeroUuid(request.security_snapshot_uuid) ||
      request.route_epoch == 0 || request.route_generation == 0 ||
      request.security_epoch == 0 ||
      request.security_observation_generation == 0) {
    return Refuse(request, "SBLR.INGRESS_REVALIDATION_FAILED");
  }
  if (!exact_cluster_root &&
      (request.cluster_context_active || request.cluster_transaction_active ||
       request.route_fence_present)) {
    return Refuse(request,
                  "CLUSTER.GATEWAY_CLUSTER_FALLTHROUGH_FORBIDDEN");
  }
  auto decision = Refuse(request, {});
  if (std::all_of(decision.canonical_payload_sha256.begin(),
                  decision.canonical_payload_sha256.end(),
                  [](std::uint8_t byte) { return byte == 0; })) {
    decision.diagnostic_id = "SBLR.INGRESS_REVALIDATION_FAILED";
    return decision;
  }
  decision.ok = true;
  decision.disposition = LocalSblrGatewayDisposition::kPassThrough;
  decision.gateway_observation_generation = 1;
  return decision;
}

}  // namespace scratchbird::server
