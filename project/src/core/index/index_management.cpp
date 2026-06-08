// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "index_management.hpp"

#include "policy_blocked_index_admission.hpp"

#include <utility>

namespace scratchbird::core::index {
namespace {
using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() { return {StatusCode::ok, Severity::info, Subsystem::engine}; }
Status RefuseStatus() { return {StatusCode::platform_required_feature_missing, Severity::error, Subsystem::engine}; }

bool Mutating(IndexManagementOperation operation) {
  return operation != IndexManagementOperation::inspect && operation != IndexManagementOperation::support_bundle &&
         operation != IndexManagementOperation::verify;
}

IndexMaintenanceOperation MaintenanceFor(IndexManagementOperation operation) {
  switch (operation) {
    case IndexManagementOperation::rebuild: return IndexMaintenanceOperation::rebuild;
    case IndexManagementOperation::repair: return IndexMaintenanceOperation::rebuild;
    case IndexManagementOperation::discard_unpublished: return IndexMaintenanceOperation::refresh;
    case IndexManagementOperation::rebalance: return IndexMaintenanceOperation::rebalance;
    case IndexManagementOperation::refresh: return IndexMaintenanceOperation::refresh;
    case IndexManagementOperation::move: return IndexMaintenanceOperation::relocate;
    case IndexManagementOperation::verify:
    case IndexManagementOperation::inspect:
    case IndexManagementOperation::support_bundle:
    case IndexManagementOperation::create:
    case IndexManagementOperation::alter:
    case IndexManagementOperation::drop: return IndexMaintenanceOperation::verify;
  }
  return IndexMaintenanceOperation::verify;
}
}  // namespace

IndexManagementPlan PlanIndexManagementOperation(const IndexManagementRequest& request) {
  IndexManagementPlan plan;
  const auto ownership = EvaluateIndexOwnership("management_request", request.caller);
  if (!ownership.allowed) {
    plan.status = ownership.status;
    plan.diagnostic = ownership.diagnostic;
    return plan;
  }
  if (!request.index_uuid.valid() || request.family == IndexFamily::unknown) {
    plan.status = RefuseStatus();
    plan.diagnostic = MakeIndexManagementDiagnostic(plan.status,
                                                    "SB-INDEX-MANAGEMENT-INVALID-REQUEST",
                                                    "index.management.invalid_request");
    return plan;
  }
  if (IsPolicyBlockedIndexFamily(request.family)) {
    auto policy_request = MakePolicyBlockedIndexRouteRequest(
        request.family,
        "",
        "index.management",
        "management_policy_blocked_metadata",
        true);
    if (request.operation == IndexManagementOperation::inspect ||
        request.operation == IndexManagementOperation::support_bundle) {
      const auto metadata =
          ProjectPolicyBlockedIndexMetadata(policy_request);
      plan.status = metadata.status;
      plan.admitted = metadata.ok();
      plan.redaction_required = !request.allow_sensitive_support_data;
      plan.diagnostic = metadata.diagnostic;
      plan.steps.push_back("render_policy_blocked_management_metadata");
      plan.steps.insert(plan.steps.end(), metadata.evidence.begin(),
                        metadata.evidence.end());
      return plan;
    }
    const auto policy_blocked =
        EvaluatePolicyBlockedIndexAdmission(policy_request);
    plan.status = policy_blocked.status;
    plan.diagnostic = policy_blocked.diagnostic;
    plan.steps.push_back("policy_blocked_fail_closed_before_management_mutation");
    return plan;
  }
  plan.mutating = Mutating(request.operation);
  if (plan.mutating && (request.read_only_database || !request.policy_allows_mutation)) {
    plan.status = RefuseStatus();
    plan.diagnostic = MakeIndexManagementDiagnostic(plan.status,
                                                    "SB-INDEX-MANAGEMENT-MUTATION-REFUSED",
                                                    "index.management.mutation_refused");
    return plan;
  }
  plan.status = OkStatus();
  plan.admitted = true;
  plan.redaction_required = !request.allow_sensitive_support_data;
  plan.steps.push_back("resolve_index_uuid_authority");
  plan.steps.push_back("evaluate_security_policy");
  if (request.operation == IndexManagementOperation::support_bundle) {
    plan.steps.push_back(plan.redaction_required ? "render_redacted_support_bundle" : "render_sensitive_support_bundle");
  } else if (request.operation == IndexManagementOperation::create ||
             request.operation == IndexManagementOperation::alter ||
             request.operation == IndexManagementOperation::drop) {
    plan.steps.push_back("write_catalog_evidence_before_success");
    plan.steps.push_back("publish_index_resource_epoch");
  } else {
    IndexMaintenanceRequest maintenance;
    maintenance.index_uuid = request.index_uuid;
    maintenance.family = request.family;
    maintenance.operation = MaintenanceFor(request.operation);
    maintenance.page_budget = 1;
    maintenance.policy_allows_mutation = request.policy_allows_mutation;
    maintenance.read_only_database = request.read_only_database;
    const auto maintenance_plan = PlanIndexMaintenance(maintenance);
    if (!maintenance_plan.status.ok()) {
      plan.status = maintenance_plan.status;
      plan.admitted = false;
      plan.diagnostic = maintenance_plan.diagnostic;
      plan.steps.clear();
      return plan;
    }
    plan.steps.insert(plan.steps.end(), maintenance_plan.steps.begin(), maintenance_plan.steps.end());
  }
  return plan;
}

IndexFamilyManagementSurface BuildIndexFamilyManagementCatalogView(
    const IndexFamilyManagementSurfaceRequest& request) {
  return BuildIndexFamilyManagementSurface(request);
}

IndexFamilyManagementSurface BuildIndexFamilyManagementSupportBundle(
    const IndexFamilyManagementSurfaceRequest& request) {
  auto effective = request;
  effective.redact_diagnostic_details = true;
  effective.include_support_bundle_rows = true;
  return BuildIndexFamilyManagementSurface(effective);
}

IndexValidationRepairResult PlanIndexManagementValidationRepairOperation(
    IndexValidationRepairRequest request,
    IndexSubsystemOwner caller) {
  const auto ownership = EvaluateIndexOwnership("management_request", caller);
  if (!ownership.allowed) {
    IndexValidationRepairResult result;
    result.status = ownership.status;
    result.diagnostic = ownership.diagnostic;
    result.classification = IndexValidationRepairClass::refused;
    result.fail_closed = true;
    result.actions.push_back("management_route_refused_by_ownership_policy");
    result.support_evidence.push_back({"message_vector_key", result.diagnostic.message_key, false});
    return result;
  }
  request.target.catalog_resolution_proven =
      request.target.catalog_resolution_proven &&
      request.target.names_resolved_to_uuids &&
      !request.target.contains_sql_text;
  auto result = ExecuteIndexValidationRepairOperation(request);
  result.actions.insert(result.actions.begin(),
                        "management_route_bound_to_engine_index_validation_repair");
  result.support_evidence.push_back({"route_surface", "management_api", false});
  return result;
}

DiagnosticRecord MakeIndexManagementDiagnostic(Status status,
                                               std::string diagnostic_code,
                                               std::string message_key,
                                               std::string detail) {
  std::vector<DiagnosticArgument> arguments;
  if (!detail.empty()) {
    arguments.push_back({"detail", std::move(detail)});
  }
  return MakeDiagnostic(status.code, status.severity, status.subsystem,
                        std::move(diagnostic_code), std::move(message_key),
                        std::move(arguments), {}, "core.index.management");
}

}  // namespace scratchbird::core::index
