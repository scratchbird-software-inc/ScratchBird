// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "observability/show_api.hpp"

#include "agent_runtime.hpp"
#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "crud_support/crud_store.hpp"
#include "observability/performance_optimization_surface.hpp"

namespace scratchbird::engine::internal_api {

namespace {

std::string OptionValue(const EngineApiRequest& request, const std::string& prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(prefix, 0) == 0) return option.substr(prefix.size());
  }
  return {};
}

std::string ResultShapeContract(const EngineApiRequest& request, const std::string& fallback) {
  const auto result_shape = OptionValue(request, "result_shape_contract:");
  return result_shape.empty() ? fallback : result_shape;
}

template <typename TResult>
void AddSbsfc080Evidence(TResult* result, const EngineApiRequest& request) {
  const std::string surface_id = OptionValue(request, "sbsfc080_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc080_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc080_runtime_evidence_id:");
  AddApiBehaviorEvidence(result, evidence_kind.empty() ? "operational_observability_route" : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc080_surface", surface_id);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

template <typename TResult>
void AddPublicExactShowEvidence(TResult* result,
                                const EngineApiRequest& request,
                                const std::string& operation_id,
                                const std::string& fallback_result_shape) {
  const std::string result_shape = ResultShapeContract(request, fallback_result_shape);
  if (OptionValue(request, "result_shape_contract:").empty()) return;
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", "EngineInspectShowOperation");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  result->result_shape.result_kind = result_shape;
}

template <typename TResult>
TResult ShowBase(const EngineApiRequest& request,
                 const std::string& operation_id,
                 std::vector<std::pair<std::string, std::string>> fields,
                 const std::string& fallback_result_shape) {
  auto result = MakeApiBehaviorSuccess<TResult>(request.context, operation_id);
  AddApiBehaviorEvidence(&result, "observability", operation_id);
  AddApiBehaviorRow(&result, std::move(fields));
  AddSbsfc080Evidence(&result, request);
  AddPublicExactShowEvidence(&result, request, operation_id, fallback_result_shape);
  return result;
}

std::string OperationIdOr(const EngineApiRequest& request, const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

void AddShowOperationResult(EngineApiResult* result,
                            const EngineApiRequest& request,
                            const std::string& operation_id,
                            const std::string& result_shape) {
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", "EngineInspectShowOperation");
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  AddApiBehaviorRow(result,
                    {{"operation_id", operation_id},
                     {"result_shape", result_shape},
                     {"route_kind", "observability_show_inspect"},
                     {"target_ref_kind", OptionValue(request, "target_ref_kind:")},
                     {"target_ref_visible", OptionValue(request, "target_ref:").empty() ? "false" : "true"},
                     {"catalog_generation_id", std::to_string(request.context.catalog_generation_id)},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)}});
  result->result_shape.result_kind = result_shape;
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_OBSERVABILITY_SHOW_API_BEHAVIOR
EngineShowVersionResult EngineShowVersion(const EngineShowVersionRequest& request) {
  return ShowBase<EngineShowVersionResult>(
      request,
      "observability.show_version",
      {{"product", "ScratchBird"}, {"component", "sb_engine"}, {"api", "1.0"}},
      "rs.show.version.v1");
}

EngineShowDatabaseResult EngineShowDatabase(const EngineShowDatabaseRequest& request) {
  return ShowBase<EngineShowDatabaseResult>(
      request,
      "observability.show_database",
      {{"database_path", request.context.database_path},
       {"database_uuid", request.context.database_uuid.canonical}},
      "rs.show.database.v1");
}

EngineShowSystemResult EngineShowSystem(const EngineShowSystemRequest& request) {
  return ShowBase<EngineShowSystemResult>(
      request,
      "observability.show_system",
      {{"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"},
       {"trust_mode", request.context.trust_mode == EngineTrustMode::embedded_in_process
                          ? "embedded"
                          : "server_isolated"}},
      "rs.show.system.v1");
}

EngineShowCatalogResult EngineShowCatalog(const EngineShowCatalogRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowCatalogResult>(request.context, "observability.show_catalog");
  const auto crud = LoadCrudState(request.context);
  if (crud.ok) {
    for (const auto& table : crud.state.tables) {
      if (CrudCreatorVisible(crud.state, table.creator_tx, table.event_sequence, request.context.local_transaction_id)) {
        AddApiBehaviorRow(&result, {{"object_uuid", table.table_uuid}, {"object_kind", "table"}, {"name", table.default_name}});
      }
    }
  }
  for (const auto& record : VisibleApiBehaviorRecords(request.context, {}, request.context.local_transaction_id)) {
    AddApiBehaviorRow(&result, {{"object_uuid", record.object_uuid}, {"object_kind", record.object_kind}, {"name", record.default_name}});
  }
  AddApiBehaviorEvidence(&result, "catalog_rows", std::to_string(result.result_shape.rows.size()));
  return result;
}

EngineShowSessionsResult EngineShowSessions(const EngineShowSessionsRequest& request) {
  return ShowBase<EngineShowSessionsResult>(
      request,
      "observability.show_sessions",
      {{"session_uuid", request.context.session_uuid.canonical},
       {"scope", request.context.security_context_present ? "all_or_self" : "self"}},
      "rs.show.sessions.v1");
}

EngineShowTransactionsResult EngineShowTransactions(const EngineShowTransactionsRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowTransactionsResult>(request.context, "observability.show_transactions");
  const auto crud = LoadCrudState(request.context);
  if (crud.ok) {
    for (const auto& [id, state] : crud.state.transactions) { AddApiBehaviorRow(&result, {{"local_transaction_id", std::to_string(id)}, {"state", state}}); }
  }
  AddApiBehaviorEvidence(&result, "transaction_rows", std::to_string(result.result_shape.rows.size()));
  AddPublicExactShowEvidence(&result, request, "observability.show_transactions", "rs.show.transactions.v1");
  return result;
}

EngineShowLocksResult EngineShowLocks(const EngineShowLocksRequest& request) {
  return ShowBase<EngineShowLocksResult>(
      request,
      "observability.show_locks",
      {{"lock_count", "0"}, {"scope", "local_node"}},
      "rs.show.locks.v1");
}

EngineShowStatementsResult EngineShowStatements(const EngineShowStatementsRequest& request) {
  return ShowBase<EngineShowStatementsResult>(
      request,
      "observability.show_statements",
      {{"statement_count", "0"}, {"scope", "local_node"}},
      "rs.show.statements.v1");
}

EngineShowJobsResult EngineShowJobs(const EngineShowJobsRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowJobsResult>(request.context, "observability.show_jobs");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_jobs");
  AddApiBehaviorEvidence(&result, "jobs_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"job_count", "0"},
                     {"scheduler_scope", "local_node"},
                     {"agent_visibility", request.context.security_context_present ? "authorized" : "self"}});
  AddPublicExactShowEvidence(&result, request, "observability.show_jobs", "rs.show.jobs.v1");
  return result;
}

EngineShowManagementResult EngineShowManagement(const EngineShowManagementRequest& request) {
  const auto surface = request.performance_optimization_snapshot_present
                           ? request.performance_optimization_snapshot
                           : DefaultPerformanceOptimizationSurfaceSnapshot();
  const auto validation =
      ValidatePerformanceOptimizationSurfaceSnapshot(surface);
  if (!validation.ok) {
    return MakeApiBehaviorDiagnostic<EngineShowManagementResult>(
        request.context,
        "observability.show_management",
        MakeEngineApiDiagnostic(validation.diagnostic_code,
                                "observability.performance_optimization.invalid_snapshot",
                                validation.detail,
                                true));
  }
  auto result =
      MakeApiBehaviorSuccess<EngineShowManagementResult>(request.context, "observability.show_management");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_management");
  AddApiBehaviorEvidence(&result, "management_rows", "2");
  AddApiBehaviorEvidence(&result,
                         "management_performance_optimization_surface",
                         PerformanceOptimizationSurfaceSchemaId());
  AddApiBehaviorRow(&result,
                    {{"read_only_mode", request.context.read_only_mode ? "true" : "false"},
                     {"catalog_generation_id", std::to_string(request.context.catalog_generation_id)},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)},
                     {"performance_optimization_surface", PerformanceOptimizationSurfaceSchemaId()},
                     {"optimization_profile", surface.optimization_profile},
                     {"copy_append_batching_enabled",
                      surface.copy_append_batching_enabled ? "true" : "false"},
                     {"plan_cache_enabled", surface.plan_cache_enabled ? "true" : "false"},
                     {"descriptor_metadata_cache_enabled",
                      surface.descriptor_metadata_cache_enabled ? "true" : "false"},
                     {"statistics_epoch", std::to_string(surface.statistics_epoch)},
                     {"cleanup_horizon_authority_status",
                      surface.cleanup_horizon_authority_status},
                     {"oldest_interesting_transaction_id",
                      std::to_string(surface.oldest_interesting_transaction_id)},
                     {"oldest_active_transaction_id",
                      std::to_string(surface.oldest_active_transaction_id)},
                     {"oldest_snapshot_transaction_id",
                      std::to_string(surface.oldest_snapshot_transaction_id)},
                     {"storage_row_version_backlog_count",
                      std::to_string(surface.storage_row_version_backlog_count)},
                     {"index_delta_backlog_count",
                      std::to_string(surface.index_delta_backlog_count)},
                     {"index_garbage_backlog_count",
                      std::to_string(surface.index_garbage_backlog_count)},
                     {"agent_worker_status", surface.agent_worker_status},
                     {"last_agent_decision", surface.last_agent_decision},
                     {"secondary_index_state", surface.secondary_index_state},
                     {"shadow_index_state", surface.shadow_index_state},
                     {"summary_index_state", surface.summary_index_state},
                     {"specialized_index_state", surface.specialized_index_state},
                     {"exact_refusal_diagnostic_code",
                      surface.exact_refusal_diagnostic_code},
                     {"exact_refusal_message_vector",
                      surface.exact_refusal_message_vector},
                     {"support_bundle_completeness_state",
                      surface.support_bundle_completeness_state},
                     {"resource_governor_state", surface.resource_governor_state},
                     {"odf108_surface_ready", "true"},
                     {"odf108_selected_path_count",
                      std::to_string(surface.odf108_selected_paths.size())},
                     {"odf108_feature_gate_count",
                      std::to_string(surface.odf108_feature_gates.size())},
                     {"odf108_fallback_reason_count",
                      std::to_string(surface.odf108_fallbacks.size())},
                     {"odf108_quota_state_count",
                      std::to_string(surface.odf108_quotas.size())},
                     {"odf108_runtime_compatibility_count",
                      std::to_string(
                          surface.odf108_runtime_compatibility.size())},
                     {"odf108_rebuild_state_count",
                      std::to_string(surface.odf108_rebuild_states.size())},
                     {"odf108_exact_refusal_count",
                      std::to_string(surface.odf108_exact_refusals.size())},
                     {"parser_finality_authority",
                      surface.parser_finality_authority ? "true" : "false"},
                     {"donor_finality_authority",
                      surface.donor_finality_authority ? "true" : "false"},
                     {"wal_recovery_authority",
                      surface.wal_recovery_authority ? "true" : "false"}});
  AddPerformanceOptimizationSurfaceRow(&result, surface);
  return result;
}

EngineShowDiagnosticsResult EngineShowDiagnostics(const EngineShowDiagnosticsRequest& request) {
  auto result =
      MakeApiBehaviorSuccess<EngineShowDiagnosticsResult>(request.context, "observability.show_diagnostics");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_diagnostics");
  AddApiBehaviorEvidence(&result, "diagnostic_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"current_sqlstate", request.context.current_sqlstate},
                     {"diagnostic_uuid", request.context.current_diagnostic_uuid.canonical},
                     {"statement_uuid", request.context.statement_uuid.canonical}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineShowDiagnosticsExtendedResult EngineShowDiagnosticsExtended(
    const EngineShowDiagnosticsExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowDiagnosticsExtendedResult>(
      request.context, "observability.show_diagnostics_extended");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_diagnostics_extended");
  AddApiBehaviorEvidence(&result, "diagnostic_extended_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"current_sqlstate", request.context.current_sqlstate},
                     {"diagnostic_uuid", request.context.current_diagnostic_uuid.canonical},
                     {"statement_uuid", request.context.statement_uuid.canonical},
                     {"request_id", request.context.request_id},
                     {"trace_tag_count", std::to_string(request.context.trace_tags.size())}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineShowArchiveReplicationResult EngineShowArchiveReplication(
    const EngineShowArchiveReplicationRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowArchiveReplicationResult>(
      request.context, "observability.show_archive_replication");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_archive_replication");
  AddApiBehaviorEvidence(&result, "archive_replication_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"database_uuid", request.context.database_uuid.canonical},
                     {"archive_mode", "local_mga_inventory"},
                     {"replication_channels", "0"},
                     {"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"}});
  return result;
}

EngineShowAgentsExtendedResult EngineShowAgentsExtended(
    const EngineShowAgentsExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowAgentsExtendedResult>(
      request.context, "observability.show_agents_extended");
  const auto& registry = scratchbird::core::agents::CanonicalAgentRegistry();
  AddApiBehaviorEvidence(&result, "observability", "observability.show_agents_extended");
  AddApiBehaviorEvidence(&result, "agent_registry", "canonical");
  AddApiBehaviorEvidence(&result, "agent_count", std::to_string(registry.size()));
  for (const auto& agent : registry) {
    AddApiBehaviorRow(&result,
                      {{"agent_type", agent.type_id},
                       {"deployment", scratchbird::core::agents::AgentDeploymentName(agent.deployment)},
                       {"scope", agent.scope},
                       {"authority", scratchbird::core::agents::AgentAuthorityClassName(agent.authority)},
                       {"cluster_only", agent.cluster_only ? "true" : "false"},
                       {"cluster_authority",
                        request.context.cluster_authority_available ? "active" : "inactive"}});
  }
  if (registry.empty()) {
    AddApiBehaviorRow(&result,
                      {{"agent_type", "none"},
                       {"deployment", "local"},
                       {"scope", "local_node"},
                       {"authority", "engine"},
                       {"cluster_only", "false"},
                       {"cluster_authority",
                        request.context.cluster_authority_available ? "active" : "inactive"}});
  }
  return result;
}

EngineShowFilespaceExtendedResult EngineShowFilespaceExtended(
    const EngineShowFilespaceExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowFilespaceExtendedResult>(
      request.context, "observability.show_filespace_extended");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_filespace_extended");
  AddApiBehaviorEvidence(&result, "filespace_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"database_uuid", request.context.database_uuid.canonical},
                     {"database_path", request.context.database_path},
                     {"filespace_scope", "primary_database"},
                     {"mga_finality_authority", "local_transaction_inventory"},
                     {"storage_visibility", "engine_owned"},
                     {"cluster_authority",
                      request.context.cluster_authority_available ? "active" : "inactive"}});
  return result;
}

EngineShowDecisionServiceResult EngineShowDecisionService(
    const EngineShowDecisionServiceRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowDecisionServiceResult>(
      request.context, "observability.show_decision_service");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_decision_service");
  AddApiBehaviorEvidence(&result, "decision_service_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"database_uuid", request.context.database_uuid.canonical},
                     {"cluster_uuid", request.context.cluster_uuid.canonical},
                     {"decision_service_scope",
                      request.context.cluster_authority_available ? "cluster_provider" : "local_node"},
                     {"decision_service_state",
                      request.context.cluster_authority_available ? "provider_available" : "not_enabled"},
                     {"provider_boundary", "compile_gated_cluster_provider"},
                     {"engine_mode",
                      request.context.cluster_authority_available ? "cluster_enabled" : "standalone"}});
  return result;
}

EngineShowAccelerationResult EngineShowAcceleration(const EngineShowAccelerationRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowAccelerationResult>(
      request.context, "observability.show_acceleration");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_acceleration");
  AddApiBehaviorEvidence(&result, "acceleration_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"provider_count", "0"},
                     {"runtime_mode", "interpreted_sblr"},
                     {"node_uuid", request.context.node_uuid.canonical}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineShowAccelerationExtendedResult EngineShowAccelerationExtended(
    const EngineShowAccelerationExtendedRequest& request) {
  auto result = MakeApiBehaviorSuccess<EngineShowAccelerationExtendedResult>(
      request.context, "observability.show_acceleration_extended");
  AddApiBehaviorEvidence(&result, "observability", "observability.show_acceleration_extended");
  AddApiBehaviorEvidence(&result, "acceleration_extended_rows", "1");
  AddApiBehaviorRow(&result,
                    {{"provider_count", "0"},
                     {"runtime_mode", "interpreted_sblr"},
                     {"llvm_module_count", "0"},
                     {"gpu_queue_count", "0"},
                     {"node_uuid", request.context.node_uuid.canonical}});
  AddSbsfc080Evidence(&result, request);
  return result;
}

EngineInspectShowOperationResult EngineInspectShowOperation(
    const EngineInspectShowOperationRequest& request) {
  const std::string operation_id = OperationIdOr(request, "observability.show_operation");
  auto result =
      MakeApiBehaviorSuccess<EngineInspectShowOperationResult>(request.context, operation_id);
  AddShowOperationResult(&result,
                         request,
                         operation_id,
                         ResultShapeContract(request, "rs.show.context.v1"));
  return result;
}

}  // namespace scratchbird::engine::internal_api
