// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "management/index_management_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "index_family_registry.hpp"
#include "index_management.hpp"
#include "security/authorization_api.hpp"
#include "security/security_model.hpp"
#include "uuid.hpp"

#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::engine::internal_api {
namespace {

namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

std::string OptionValue(const EngineApiRequest& request, std::string_view prefix) {
  return SecurityOptionValue(request, std::string(prefix));
}

bool OptionBool(const EngineApiRequest& request,
                std::string_view prefix,
                bool fallback = false) {
  return SecurityOptionBool(request, std::string(prefix), fallback);
}

std::string FirstNonEmpty(std::initializer_list<std::string> values) {
  for (const auto& value : values) {
    if (!value.empty()) return value;
  }
  return {};
}

platform::TypedUuid ParseTyped(platform::UuidKind kind, const std::string& text) {
  if (text.empty()) return {};
  const auto parsed = uuid::ParseTypedUuid(kind, text);
  return parsed.ok() ? parsed.value : platform::TypedUuid{};
}

idx::IndexFamily IndexFamilyForRequest(const EngineApiRequest& request) {
  const std::string family = FirstNonEmpty({
      OptionValue(request, "index_family:"),
      OptionValue(request, "index_physical_family:"),
      OptionValue(request, "physical_family:"),
      OptionValue(request, "family:")});
  if (family.empty()) return idx::IndexFamily::btree;
  const auto found = idx::FindBuiltinIndexFamilyById(family);
  return found.ok() ? found.descriptor->family : idx::IndexFamily::unknown;
}

idx::IndexValidationRepairFamily ValidationFamilyForRequest(
    const EngineApiRequest& request) {
  const std::string family = FirstNonEmpty({
      OptionValue(request, "validation_family:"),
      OptionValue(request, "index_validation_family:"),
      OptionValue(request, "index_management_family:")});
  if (family.empty() || family == "ordered_table_candidate_set") {
    return idx::IndexValidationRepairFamily::ordered_table_candidate_set;
  }
  if (family == "secondary_delta_ledger") {
    return idx::IndexValidationRepairFamily::secondary_delta_ledger;
  }
  if (family == "page_extent_summary") {
    return idx::IndexValidationRepairFamily::page_extent_summary;
  }
  if (family == "time_range_summary") {
    return idx::IndexValidationRepairFamily::time_range_summary;
  }
  if (family == "shadow_index_build_state") {
    return idx::IndexValidationRepairFamily::shadow_index_build_state;
  }
  if (family == "inverted_search_segment_state") {
    return idx::IndexValidationRepairFamily::inverted_search_segment_state;
  }
  if (family == "vector_generation_state") {
    return idx::IndexValidationRepairFamily::vector_generation_state;
  }
  return idx::IndexValidationRepairFamily::ordered_table_candidate_set;
}

idx::IndexValidationRepairOperation ValidationOperationFor(
    std::string_view operation_id) {
  if (operation_id == "index.repair") {
    return idx::IndexValidationRepairOperation::repair;
  }
  if (operation_id == "index.rebuild") {
    return idx::IndexValidationRepairOperation::rebuild;
  }
  if (operation_id == "index.discard_unpublished") {
    return idx::IndexValidationRepairOperation::discard_unpublished;
  }
  return idx::IndexValidationRepairOperation::validate;
}

bool IsValidationRepairOperation(std::string_view operation_id) {
  return operation_id == "index.validate" ||
         operation_id == "index.verify" ||
         operation_id == "index.repair" ||
         operation_id == "index.rebuild" ||
         operation_id == "index.discard_unpublished";
}

idx::IndexManagementOperation ManagementOperationFor(std::string_view operation_id) {
  if (operation_id == "index.inspect") return idx::IndexManagementOperation::inspect;
  if (operation_id == "index.support_bundle") return idx::IndexManagementOperation::support_bundle;
  if (operation_id == "index.rebalance") return idx::IndexManagementOperation::rebalance;
  if (operation_id == "index.analyze") return idx::IndexManagementOperation::verify;
  if (operation_id == "index.gather_statistics") return idx::IndexManagementOperation::verify;
  if (operation_id == "index.cleanup_backlog" ||
      operation_id == "index.backlog" ||
      operation_id == "index.backlog_visibility") {
    return idx::IndexManagementOperation::inspect;
  }
  if (operation_id == "index.cleanup_mga_versions") return idx::IndexManagementOperation::refresh;
  if (operation_id == "index.force_cleanup") return idx::IndexManagementOperation::refresh;
  if (operation_id == "index.optimization_control" ||
      operation_id == "index.set_optimization_flag" ||
      operation_id == "index.enable_optimization" ||
      operation_id == "index.disable_optimization") {
    return idx::IndexManagementOperation::rebalance;
  }
  if (operation_id == "index.rebuild") return idx::IndexManagementOperation::rebuild;
  if (operation_id == "index.repair") return idx::IndexManagementOperation::repair;
  if (operation_id == "index.discard_unpublished") return idx::IndexManagementOperation::discard_unpublished;
  return idx::IndexManagementOperation::verify;
}

struct IndexAuthorizationPolicy {
  std::vector<std::string> rights;
  std::string operation_class;
  bool mutating = false;
};

struct IndexAuthorizationDecision {
  bool allowed = false;
  std::string allowed_right;
  IndexAuthorizationPolicy policy;
  EngineApiDiagnostic diagnostic;
};

std::string JoinRights(const std::vector<std::string>& rights) {
  std::ostringstream out;
  for (std::size_t i = 0; i < rights.size(); ++i) {
    if (i != 0) out << '|';
    out << rights[i];
  }
  return out.str();
}

IndexAuthorizationPolicy AuthorizationPolicyFor(std::string_view operation_id) {
  if (operation_id == "index.cleanup_mga_versions" ||
      operation_id == "index.force_cleanup") {
    return {{"MGA_CLEANUP_CONTROL"}, "cleanup_control", true};
  }
  if (operation_id == "index.cleanup_backlog" ||
      operation_id == "index.backlog" ||
      operation_id == "index.backlog_visibility") {
    return {{"MGA_CLEANUP_INSPECT", "OBS_MANAGEMENT_INSPECT"},
            "cleanup_backlog_inspect",
            false};
  }
  if (operation_id == "index.repair" ||
      operation_id == "index.rebuild" ||
      operation_id == "index.discard_unpublished" ||
      operation_id == "index.rebalance") {
    return {{"OBS_MANAGEMENT_CONTROL"}, "index_management_control", true};
  }
  if (operation_id == "index.optimization_control" ||
      operation_id == "index.set_optimization_flag" ||
      operation_id == "index.enable_optimization" ||
      operation_id == "index.disable_optimization") {
    return {{"OBS_CONFIG_CONTROL", "OBS_MANAGEMENT_CONTROL"},
            "optimization_control",
            true};
  }
  if (operation_id == "index.support_bundle") {
    return {{"SUPPORT_EXPORT", "OBS_MANAGEMENT_INSPECT"},
            "support_export",
            false};
  }
  if (operation_id == "index.validate" ||
      operation_id == "index.verify" ||
      operation_id == "index.analyze" ||
      operation_id == "index.gather_statistics" ||
      operation_id == "index.inspect") {
    return {{"OBS_INDEX_PROFILE_READ", "OBS_MANAGEMENT_INSPECT"},
            "index_profile_inspect",
            false};
  }
  return {{"OBS_MANAGEMENT_CONTROL"}, "unknown_index_management_control", true};
}

std::string AuthorizationTargetUuid(const EngineIndexManagementRequest& request) {
  return FirstNonEmpty({OptionValue(request, "index_uuid:"),
                        OptionValue(request, "index_object_uuid:"),
                        OptionValue(request, "target_object_uuid:"),
                        request.target_object.uuid.canonical});
}

IndexAuthorizationDecision AuthorizeIndexOperation(
    const EngineIndexManagementRequest& request,
    std::string_view operation_id) {
  IndexAuthorizationDecision decision;
  decision.policy = AuthorizationPolicyFor(operation_id);
  const std::string target_uuid = AuthorizationTargetUuid(request);
  for (const auto& right : decision.policy.rights) {
    EngineAuthorizeRequest authorize;
    EngineApiRequest& authorize_base = authorize;
    authorize_base = request;
    authorize.operation_id = "security.authorize";
    authorize.required_right = right;
    authorize.target_object.uuid.canonical = target_uuid;
    authorize.target_object.object_kind = "index";
    const auto authorized = EngineAuthorize(authorize);
    if (authorized.ok && authorized.authorized) {
      decision.allowed = true;
      decision.allowed_right = right;
      return decision;
    }
    if (!authorized.diagnostics.empty() &&
        authorized.diagnostics.front().code == "SECURITY.CONTEXT.EXPIRED") {
      decision.diagnostic = authorized.diagnostics.front();
      return decision;
    }
  }
  decision.diagnostic = MakeSecurityDiagnostic(
      "SECURITY.AUTHORIZATION.DENIED",
      JoinRights(decision.policy.rights));
  return decision;
}

void AddIndexAuthorizationEvidence(EngineIndexManagementResult* result,
                                   const IndexAuthorizationDecision& decision) {
  AddApiBehaviorEvidence(result, "engine_authorization_authority", "EngineAuthorize");
  AddApiBehaviorEvidence(result, "authorization_operation_class",
                         decision.policy.operation_class);
  AddApiBehaviorEvidence(result, "authorization_required_rights",
                         JoinRights(decision.policy.rights));
  AddApiBehaviorEvidence(result, "authorization_decision",
                         decision.allowed ? "allow:" + decision.allowed_right
                                          : "deny:" + JoinRights(decision.policy.rights));
  AddApiBehaviorRow(result,
                    {{"record_kind", "engine_authorization_decision"},
                     {"authorization_authority", "engine_internal_api"},
                     {"decision", decision.allowed ? "allow" : "deny"},
                     {"operation_class", decision.policy.operation_class},
                     {"required_rights", JoinRights(decision.policy.rights)},
                     {"granted_right", decision.allowed ? decision.allowed_right : ""},
                     {"mutating", decision.policy.mutating ? "true" : "false"},
                     {"message_vector",
                      decision.allowed ? "SECURITY.AUTHORIZATION.ALLOW"
                                       : decision.diagnostic.code + "|" +
                                             JoinRights(decision.policy.rights)}});
}

EngineApiDiagnostic ConvertDiagnostic(const platform::DiagnosticRecord& diagnostic,
                                      std::string_view fallback_code) {
  std::string detail;
  for (const auto& argument : diagnostic.arguments) {
    if (argument.key == "detail") {
      detail = argument.value;
      break;
    }
  }
  return MakeEngineApiDiagnostic(
      diagnostic.diagnostic_code.empty() ? std::string(fallback_code)
                                         : diagnostic.diagnostic_code,
      diagnostic.message_key.empty() ? "index.management.route"
                                     : diagnostic.message_key,
      std::move(detail),
      true);
}

idx::IndexValidationRepairRequest ValidationRequestFrom(
    const EngineIndexManagementRequest& request,
    std::string_view operation_id) {
  idx::IndexValidationRepairRequest out;
  out.operation = ValidationOperationFor(operation_id);
  out.validation_family = ValidationFamilyForRequest(request);
  out.target.database_uuid = ParseTyped(
      platform::UuidKind::database,
      FirstNonEmpty({OptionValue(request, "database_uuid:"),
                     request.target_database.uuid.canonical,
                     request.context.database_uuid.canonical}));
  out.target.table_uuid = ParseTyped(
      platform::UuidKind::object,
      FirstNonEmpty({OptionValue(request, "table_uuid:"),
                     OptionValue(request, "target_table_uuid:")}));
  out.target.index_uuid = ParseTyped(
      platform::UuidKind::object,
      FirstNonEmpty({OptionValue(request, "index_uuid:"),
                     OptionValue(request, "index_object_uuid:"),
                     OptionValue(request, "target_object_uuid:"),
                     request.target_object.uuid.canonical}));
  out.target.generation_uuid = ParseTyped(
      platform::UuidKind::object,
      FirstNonEmpty({OptionValue(request, "generation_uuid:"),
                     OptionValue(request, "index_generation_uuid:")}));
  out.target.physical_family = IndexFamilyForRequest(request);
  out.target.names_resolved_to_uuids =
      OptionBool(request, "names_resolved_to_uuids:",
                 OptionBool(request, "parser_resolved_names_to_uuids:", true));
  out.target.catalog_resolution_proven =
      OptionBool(request, "catalog_resolution_proven:", true);
  out.target.contains_sql_text =
      OptionBool(request, "contains_sql_text:", false);
  out.policy_allows_mutation =
      OptionBool(request, "policy_allows_mutation:",
                 OptionBool(request, "mutation_policy_grant:", false));
  out.read_only_database =
      request.context.read_only_mode ||
      OptionBool(request, "read_only_database:", false);
  out.allow_sensitive_support_data =
      OptionBool(request, "allow_sensitive_support_data:", false);
  return out;
}

EngineIndexManagementResult ResultFromValidation(
    const EngineIndexManagementRequest& request,
    std::string operation_id,
    const idx::IndexValidationRepairResult& validation) {
  if (!validation.ok()) {
    return MakeApiBehaviorDiagnostic<EngineIndexManagementResult>(
        request.context,
        operation_id,
        ConvertDiagnostic(validation.diagnostic,
                          "INDEX.MANAGEMENT.ROUTE.REFUSED"));
  }
  auto result = MakeApiBehaviorSuccess<EngineIndexManagementResult>(
      request.context, operation_id);
  result.result_shape.result_kind = "index.management.route_surface.v1";
  result.primary_object.uuid.canonical = OptionValue(request, "index_uuid:");
  result.primary_object.object_kind = "index";
  AddApiBehaviorEvidence(&result, "route_surface", "sblr");
  AddApiBehaviorEvidence(&result, "engine_api_function",
                         "EngineIndexManagementOperation");
  AddApiBehaviorEvidence(&result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(&result, "transaction_authority",
                         "engine_mga");
  AddApiBehaviorEvidence(&result, "driver_visible_classification",
                         "capability_or_admin_route_not_driver_speed_benchmark");
  for (const auto& evidence : validation.support_evidence) {
    if (!evidence.sensitive) {
      AddApiBehaviorEvidence(&result, "index_support", evidence.key + "=" + evidence.value);
    }
  }
  AddApiBehaviorRow(&result,
                    {{"operation_id", operation_id},
                     {"validation_family", idx::IndexValidationRepairFamilyName(
                                             ValidationFamilyForRequest(request))},
                     {"classification", idx::IndexValidationRepairClassName(validation.classification)},
                     {"mutating", validation.mutating ? "true" : "false"},
                     {"mutation_applied", validation.mutation_applied ? "true" : "false"},
                     {"planner_visible", validation.planner_visible ? "true" : "false"},
                     {"diagnostic_code", validation.diagnostic.diagnostic_code}});
  return result;
}

EngineIndexManagementResult ResultFromManagementPlan(
    const EngineIndexManagementRequest& request,
    std::string operation_id,
    const idx::IndexManagementPlan& plan) {
  if (!plan.ok()) {
    return MakeApiBehaviorDiagnostic<EngineIndexManagementResult>(
        request.context,
        operation_id,
        ConvertDiagnostic(plan.diagnostic, "INDEX.MANAGEMENT.ROUTE.REFUSED"));
  }
  auto result = MakeApiBehaviorSuccess<EngineIndexManagementResult>(
      request.context, operation_id);
  result.result_shape.result_kind = "index.management.route_surface.v1";
  result.primary_object.uuid.canonical = OptionValue(request, "index_uuid:");
  result.primary_object.object_kind = "index";
  AddApiBehaviorEvidence(&result, "route_surface", "sblr");
  AddApiBehaviorEvidence(&result, "engine_api_function",
                         "EngineIndexManagementOperation");
  AddApiBehaviorEvidence(&result, "parser_executes_sql", "false");
  AddApiBehaviorEvidence(&result, "transaction_authority",
                         "engine_mga");
  AddApiBehaviorEvidence(&result, "driver_visible_classification",
                         "capability_or_admin_route_not_driver_speed_benchmark");
  for (const auto& step : plan.steps) {
    AddApiBehaviorEvidence(&result, "index_management_step", step);
  }
  AddApiBehaviorRow(&result,
                    {{"operation_id", operation_id},
                     {"validation_family", "not_applicable"},
                     {"classification", "management_plan"},
                     {"mutating", plan.mutating ? "true" : "false"},
                     {"mutation_applied", "false"},
                     {"planner_visible", "false"},
                     {"diagnostic_code", plan.diagnostic.diagnostic_code}});
  return result;
}

}  // namespace

EngineIndexManagementResult EngineIndexManagementOperation(
    const EngineIndexManagementRequest& request) {
  const std::string operation_id =
      request.operation_id.empty() ? "index.validate" : request.operation_id;
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EngineIndexManagementResult>(
        request.context,
        operation_id,
        MakeSecurityContextRequiredDiagnostic(operation_id));
  }
  const auto authorization = AuthorizeIndexOperation(request, operation_id);
  if (!authorization.allowed) {
    auto result = MakeApiBehaviorDiagnostic<EngineIndexManagementResult>(
        request.context,
        operation_id,
        authorization.diagnostic);
    AddIndexAuthorizationEvidence(&result, authorization);
    return result;
  }
  if (IsValidationRepairOperation(operation_id)) {
    auto validation_request = ValidationRequestFrom(request, operation_id);
    auto validation = idx::PlanIndexManagementValidationRepairOperation(
        std::move(validation_request),
        idx::IndexSubsystemOwner::management_api);
    auto result = ResultFromValidation(request, operation_id, validation);
    AddIndexAuthorizationEvidence(&result, authorization);
    return result;
  }

  idx::IndexManagementRequest management_request;
  management_request.operation = ManagementOperationFor(operation_id);
  management_request.index_uuid = ParseTyped(
      platform::UuidKind::object,
      FirstNonEmpty({OptionValue(request, "index_uuid:"),
                     OptionValue(request, "index_object_uuid:"),
                     OptionValue(request, "target_object_uuid:"),
                     request.target_object.uuid.canonical}));
  management_request.family = IndexFamilyForRequest(request);
  management_request.caller = idx::IndexSubsystemOwner::management_api;
  management_request.policy_allows_mutation =
      OptionBool(request, "policy_allows_mutation:",
                 OptionBool(request, "mutation_policy_grant:", false));
  management_request.read_only_database =
      request.context.read_only_mode ||
      OptionBool(request, "read_only_database:", false);
  management_request.allow_sensitive_support_data =
      OptionBool(request, "allow_sensitive_support_data:", false);
  auto result = ResultFromManagementPlan(
      request,
      operation_id,
      idx::PlanIndexManagementOperation(management_request));
  AddIndexAuthorizationEvidence(&result, authorization);
  return result;
}

}  // namespace scratchbird::engine::internal_api
