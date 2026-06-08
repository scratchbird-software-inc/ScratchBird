// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/management_api.hpp"

#include "behavior_support/api_behavior_store.hpp"

#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  for (const auto& option : request.option_envelopes) {
    if (option.rfind(std::string(prefix), 0) == 0) {
      return option.substr(prefix.size());
    }
  }
  return {};
}

bool MatchesTarget(const EngineApiRequest& request, std::string_view candidate) {
  const std::string target = OptionValue(request, "runtime_target_name:");
  return target.empty() || target == candidate;
}

bool StartsWith(std::string_view value, std::string_view prefix) {
  return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string OperationIdOr(const EngineApiRequest& request, const std::string& fallback) {
  return request.operation_id.empty() ? fallback : request.operation_id;
}

std::string ResultShapeContract(const EngineApiRequest& request, const std::string& fallback) {
  const auto result_shape = OptionValue(request, "result_shape_contract:");
  return result_shape.empty() ? fallback : result_shape;
}

void AddManagementOperationResult(EngineApiResult* result,
                                  const EngineApiRequest& request,
                                  const std::string& operation_id,
                                  const std::string& api_function,
                                  const std::string& route_kind,
                                  const std::string& result_shape) {
  AddApiBehaviorEvidence(result, "public_sbsql_operation", operation_id);
  AddApiBehaviorEvidence(result, "engine_api_function", api_function);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "result_shape_contract", result_shape);
  AddApiBehaviorRow(result,
                    {{"operation_id", operation_id},
                     {"result_shape", result_shape},
                     {"route_kind", route_kind},
                     {"target_ref_kind", OptionValue(request, "target_ref_kind:")},
                     {"target_ref_visible", OptionValue(request, "target_ref:").empty() ? "false" : "true"},
                     {"resize_count", OptionValue(request, "resize_count:")},
                     {"security_epoch", std::to_string(request.context.security_epoch)},
                     {"resource_epoch", std::to_string(request.context.resource_epoch)}});
  result->result_shape.result_kind = result_shape;
}

template <typename TResult>
void AddSbsfc080Evidence(TResult* result, const EngineApiRequest& request) {
  const std::string surface_id = OptionValue(request, "sbsfc080_surface_id:");
  if (surface_id.empty()) return;
  const std::string evidence_kind = OptionValue(request, "sbsfc080_runtime_evidence_kind:");
  const std::string evidence_id = OptionValue(request, "sbsfc080_runtime_evidence_id:");
  AddApiBehaviorEvidence(result, evidence_kind.empty() ? "management_descriptor_route" : evidence_kind,
                         evidence_id.empty() ? surface_id : evidence_id);
  AddApiBehaviorEvidence(result, "sbsfc080_surface", surface_id);
  AddApiBehaviorEvidence(result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(result, "wal_recovery_authority", "false");
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_MANAGEMENT_MANAGEMENT_API_BEHAVIOR
EngineInspectManagementRuntimeResult EngineInspectManagementRuntime(const EngineInspectManagementRuntimeRequest& request) {
  const std::string operation_id = OperationIdOr(request, "management.inspect_runtime");
  auto result = MakeApiBehaviorSuccess<EngineInspectManagementRuntimeResult>(request.context, operation_id);
  const std::string component = OptionValue(request, "runtime_component:");
  if (component.empty() || component == "listeners") {
    if (MatchesTarget(request, "native_listener")) {
      AddApiBehaviorRow(&result,
                        {{"runtime_component", "listeners"},
                         {"listener_name", "native_listener"},
                         {"listener_uuid", "019f0000-0000-7000-8000-00000000b101"},
                         {"state", "available"},
                         {"bind_profile", "local_ipc_or_native_wire"},
                         {"parser_pool", "sbsql"},
                         {"database_path", request.context.database_path},
                         {"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"}});
    }
  }
  if (component.empty() || component == "parsers") {
    if (MatchesTarget(request, "sbsql")) {
      AddApiBehaviorRow(&result,
                        {{"runtime_component", "parsers"},
                         {"parser_name", "sbsql"},
                         {"parser_package", "sbp_sbsql"},
                         {"parser_package_uuid", "019f0000-0000-7000-8000-00000000b201"},
                         {"state", "available"},
                         {"trust_boundary", "untrusted_per_connection"},
                         {"render_contract", "sbsql.management.runtime.v1"},
                         {"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"}});
    }
  }
  if (component.empty() || component == "parser_packages") {
    if (MatchesTarget(request, "sbp_sbsql")) {
      AddApiBehaviorRow(&result,
                        {{"runtime_component", "parser_packages"},
                         {"package_name", "sbp_sbsql"},
                         {"parser_name", "sbsql"},
                         {"parser_package_uuid", "019f0000-0000-7000-8000-00000000b201"},
                         {"api_contract", "sbps.parser.package.v1"},
                         {"state", "registered"},
                         {"trust_boundary", "untrusted_per_connection"},
                         {"engine_mutation_authority", "false"}});
    }
  }
  if (result.result_shape.rows.empty()) {
    AddApiBehaviorRow(&result,
                      {{"runtime_component", component.empty() ? "runtime" : component},
                       {"state", "not_visible_or_not_registered"},
                       {"database_path", request.context.database_path},
                       {"cluster_authority", request.context.cluster_authority_available ? "active" : "inactive"}});
  }
  AddApiBehaviorEvidence(&result, "management_runtime", "local_node");
  AddApiBehaviorEvidence(&result, "parser_listener_runtime", component.empty() ? "all" : component);
  AddApiBehaviorEvidence(&result, "authority_boundary", "engine_owned_no_parser_process_control");
  AddSbsfc080Evidence(&result, request);
  if (StartsWith(operation_id, "op.")) {
    AddManagementOperationResult(&result,
                                 request,
                                 operation_id,
                                 "EngineInspectManagementRuntime",
                                 "management_runtime_inspect",
                                 ResultShapeContract(request, "rs.management.config.v1"));
  }
  return result;
}

EngineControlManagementRuntimeResult EngineControlManagementRuntime(const EngineControlManagementRuntimeRequest& request) {
  const std::string operation_id = OperationIdOr(request, "management.control_runtime");
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineControlManagementRuntimeResult>(
        request.context,
        operation_id,
        MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  for (const auto& option : request.option_envelopes) {
    if (option.find("cluster") != std::string::npos) {
      return MakeApiBehaviorDiagnostic<EngineControlManagementRuntimeResult>(request.context, operation_id, MakeClusterAuthorityUnavailableDiagnostic(operation_id));
    }
  }
  auto result = PersistedRecordResult<EngineControlManagementRuntimeResult>(
      request, operation_id, "management_control", false, "applied");
  if (result.ok && StartsWith(operation_id, "op.")) {
    AddManagementOperationResult(&result,
                                 request,
                                 operation_id,
                                 "EngineControlManagementRuntime",
                                 "management_runtime_control",
                                 ResultShapeContract(request, "rs.acceleration.control.v1"));
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
