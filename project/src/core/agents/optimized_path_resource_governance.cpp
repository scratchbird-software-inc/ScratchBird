// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimized_path_resource_governance.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

std::string BoolText(bool value) { return value ? "true" : "false"; }

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

bool KnownSurface(OptimizedPathResourceSurface surface) {
  switch (surface) {
    case OptimizedPathResourceSurface::compression:
    case OptimizedPathResourceSurface::cache:
    case OptimizedPathResourceSurface::vector_maintenance:
    case OptimizedPathResourceSurface::nosql_provider:
    case OptimizedPathResourceSurface::streaming_cursor:
    case OptimizedPathResourceSurface::background_job:
      return true;
    case OptimizedPathResourceSurface::unknown:
      return false;
  }
  return false;
}

OptimizedPathResourceGovernanceResult BaseResult(
    const OptimizedPathResourceGovernanceRequest& request) {
  OptimizedPathResourceGovernanceResult result;
  Add(&result.evidence, "ORH_RESOURCE_GOVERNANCE_STARVATION_GATE");
  Add(&result.evidence,
      "optimized_resource.operation_id=" + request.operation_id);
  Add(&result.evidence,
      "optimized_resource.surface=" +
          std::string(OptimizedPathResourceSurfaceName(request.surface)));
  Add(&result.evidence,
      "optimized_resource.workload_quota_required=" +
          BoolText(request.workload_quota_required));
  Add(&result.evidence,
      "optimized_resource.foreground_protection_required=" +
          BoolText(request.foreground_protection_required));
  Add(&result.evidence,
      "optimized_resource.background_workload=" +
          BoolText(request.background_workload));
  Add(&result.evidence,
      "optimized_resource.foreground_pool_id=" +
          (request.foreground_pool_id.empty() ? std::string("none")
                                              : request.foreground_pool_id));
  Add(&result.evidence,
      "optimized_resource.foreground_capacity_reserved=" +
          BoolText(request.foreground_capacity_reserved));
  Add(&result.evidence,
      "optimized_resource.index_runtime_dependent=" +
          BoolText(request.index_runtime_dependent));
  Add(&result.evidence,
      "optimized_resource.index_runtime_correctness_proven=" +
          BoolText(request.index_runtime_correctness_proven));
  Add(&result.evidence,
      "optimized_resource.index_runtime_closure_claimed=false");
  Add(&result.evidence,
      "optimized_resource.mga_authority=engine_transaction_inventory");
  Add(&result.evidence,
      "optimized_resource.visibility_authority=engine_mga_snapshot");
  Add(&result.evidence,
      "optimized_resource.finality_authority=engine_mga_transaction_inventory");
  Add(&result.evidence,
      "optimized_resource.parser_or_reference_authority=false");
  Add(&result.evidence,
      "optimized_resource.recovery_authority=engine_mga_recovery");
  return result;
}

void FinishEvidence(OptimizedPathResourceGovernanceResult* result) {
  Add(&result->evidence, "optimized_resource.decision=" + result->decision);
  Add(&result->evidence,
      "optimized_resource.diagnostic_code=" + result->diagnostic_code);
  Add(&result->evidence,
      "optimized_resource.admitted=" + BoolText(result->admitted));
  Add(&result->evidence,
      "optimized_resource.throttled=" + BoolText(result->throttled));
  Add(&result->evidence,
      "optimized_resource.queued=" + BoolText(result->queued));
  Add(&result->evidence,
      "optimized_resource.rejected=" + BoolText(result->rejected));
  Add(&result->evidence,
      "optimized_resource.fail_closed=" + BoolText(result->fail_closed));
  Add(&result->evidence,
      "optimized_resource.slowdown=" + BoolText(result->slowdown));
  Add(&result->evidence,
      "optimized_resource.exact_fallback=" +
          BoolText(result->exact_fallback));
  Add(&result->evidence,
      "optimized_resource.cancelled=" + BoolText(result->cancelled));
  Add(&result->evidence,
      "optimized_resource.foreground_protected=" +
          BoolText(result->foreground_protected));
  Add(&result->evidence,
      "optimized_resource.reservation_created=" +
          BoolText(result->reservation_created));
  Add(&result->evidence,
      "optimized_resource.index_runtime_closure_claimed=" +
          BoolText(result->index_runtime_closure_claimed));
  for (const auto& item : result->resource_admission.evidence) {
    Add(&result->evidence, "resource_admission." + item);
  }
  if (!result->workload_admission.evidence.operation_uuid.empty()) {
    Add(&result->evidence,
        "workload_admission.decision=" +
            result->workload_admission.evidence.decision);
    Add(&result->evidence,
        "workload_admission.diagnostic_code=" +
            result->workload_admission.evidence.diagnostic_code);
    Add(&result->evidence,
        "workload_admission.reservation_created=" +
            BoolText(result->workload_admission.evidence.reservation_created));
    Add(&result->evidence,
        "workload_admission.workload_class=" +
            result->workload_admission.evidence.workload_class);
    Add(&result->evidence,
        "workload_admission.pool_id=" +
            result->workload_admission.evidence.pool_id);
  }
}

OptimizedPathResourceGovernanceResult Finish(
    OptimizedPathResourceGovernanceResult result,
    AgentRuntimeStatus status,
    std::string decision,
    std::string diagnostic_code) {
  result.status = std::move(status);
  result.decision = std::move(decision);
  result.diagnostic_code = std::move(diagnostic_code);
  FinishEvidence(&result);
  return result;
}

}  // namespace

const char* OptimizedPathResourceSurfaceName(
    OptimizedPathResourceSurface surface) {
  switch (surface) {
    case OptimizedPathResourceSurface::compression:
      return "compression";
    case OptimizedPathResourceSurface::cache:
      return "cache";
    case OptimizedPathResourceSurface::vector_maintenance:
      return "vector_maintenance";
    case OptimizedPathResourceSurface::nosql_provider:
      return "nosql_provider";
    case OptimizedPathResourceSurface::streaming_cursor:
      return "streaming_cursor";
    case OptimizedPathResourceSurface::background_job:
      return "background_job";
    case OptimizedPathResourceSurface::unknown:
      return "unknown";
  }
  return "unknown";
}

OptimizedPathResourceGovernanceResult GovernOptimizedPathResources(
    const OptimizedPathResourceGovernanceRequest& request) {
  auto result = BaseResult(request);
  if (request.operation_id.empty() || !KnownSurface(request.surface)) {
    result.fail_closed = true;
    return Finish(std::move(result),
                  AgentError("ORH_RESOURCE_GOVERNANCE.INVALID_REQUEST",
                             "operation_id_and_known_surface_required"),
                  "fail_closed",
                  "ORH_RESOURCE_GOVERNANCE.INVALID_REQUEST");
  }

  if (request.index_runtime_dependent &&
      !request.index_runtime_correctness_proven) {
    Add(&result.evidence,
        "optimized_resource.index_runtime_blocker=" +
            (request.exact_index_runtime_blocker.empty()
                 ? std::string("INDEX_RUNTIME_UNPROVEN")
                 : request.exact_index_runtime_blocker));
  }

  result.resource_admission =
      AdmitResourceGovernance(request.resource_admission);
  if (!result.resource_admission.ok) {
    switch (result.resource_admission.action) {
      case ResourceGovernanceAction::kSlowdownDegrade:
        result.throttled = true;
        result.slowdown = true;
        return Finish(std::move(result),
                      AgentOk(),
                      "throttled",
                      result.resource_admission.diagnostic_code);
      case ResourceGovernanceAction::kExactScalarFallback:
        result.exact_fallback = true;
        return Finish(std::move(result),
                      AgentOk(),
                      "fallback",
                      result.resource_admission.diagnostic_code);
      case ResourceGovernanceAction::kCancel:
        result.cancelled = true;
        result.rejected = true;
        return Finish(std::move(result),
                      AgentError(result.resource_admission.diagnostic_code,
                                 result.resource_admission.diagnostic_detail),
                      "rejected",
                      result.resource_admission.diagnostic_code);
      case ResourceGovernanceAction::kFailClosed:
      case ResourceGovernanceAction::kAdmit:
        result.fail_closed = result.resource_admission.fail_closed;
        result.rejected = !result.fail_closed;
        return Finish(std::move(result),
                      AgentError(result.resource_admission.diagnostic_code,
                                 result.resource_admission.diagnostic_detail),
                      result.fail_closed ? "fail_closed" : "rejected",
                      result.resource_admission.diagnostic_code);
    }
  }

  if (request.workload_quota_required) {
    if (request.workload_quota == nullptr) {
      result.fail_closed = true;
      return Finish(std::move(result),
                    AgentError("ORH_RESOURCE_GOVERNANCE.WORKLOAD_QUOTA_REQUIRED",
                               "workload_quota_controller_required"),
                    "fail_closed",
                    "ORH_RESOURCE_GOVERNANCE.WORKLOAD_QUOTA_REQUIRED");
    }
    result.workload_admission =
        request.workload_quota->Admit(request.workload_admission);
    result.reservation_created =
        result.workload_admission.reservation_created();

    if (request.foreground_protection_required &&
        request.background_workload) {
      const bool uses_foreground_pool =
          !request.foreground_pool_id.empty() &&
          request.workload_admission.pool_id == request.foreground_pool_id;
      result.foreground_protected =
          request.foreground_capacity_reserved && !uses_foreground_pool;
      if (!result.foreground_protected &&
          result.workload_admission.reservation_created()) {
        (void)request.workload_quota->Release(
            result.workload_admission.reservation.token_id,
            WorkloadReleaseReason::failure);
        result.reservation_created = false;
      }
      if (!result.foreground_protected) {
        result.fail_closed = true;
        return Finish(
            std::move(result),
            AgentError("ORH_RESOURCE_GOVERNANCE.FOREGROUND_PROTECTION_REFUSED",
                       "background_work_must_not_consume_foreground_capacity"),
            "fail_closed",
            "ORH_RESOURCE_GOVERNANCE.FOREGROUND_PROTECTION_REFUSED");
      }
    } else {
      result.foreground_protected =
          !request.foreground_protection_required ||
          request.foreground_capacity_reserved ||
          !request.background_workload;
    }

    switch (result.workload_admission.decision) {
      case WorkloadAdmissionDecisionClass::admitted:
        result.admitted = true;
        return Finish(std::move(result),
                      AgentOk(),
                      "admitted",
                      result.workload_admission.diagnostic.diagnostic_code);
      case WorkloadAdmissionDecisionClass::throttled:
        result.throttled = true;
        return Finish(std::move(result),
                      AgentOk(),
                      "throttled",
                      result.workload_admission.diagnostic.diagnostic_code);
      case WorkloadAdmissionDecisionClass::queued:
        result.queued = true;
        return Finish(std::move(result),
                      AgentOk(),
                      "queued",
                      result.workload_admission.diagnostic.diagnostic_code);
      case WorkloadAdmissionDecisionClass::rejected:
      case WorkloadAdmissionDecisionClass::drain_refused:
        result.rejected = true;
        return Finish(std::move(result),
                      result.workload_admission.status,
                      "rejected",
                      result.workload_admission.diagnostic.diagnostic_code);
      case WorkloadAdmissionDecisionClass::failed_closed:
        result.fail_closed = true;
        return Finish(std::move(result),
                      result.workload_admission.status,
                      "fail_closed",
                      result.workload_admission.diagnostic.diagnostic_code);
    }
  }

  result.admitted = true;
  result.foreground_protected =
      !request.foreground_protection_required ||
      request.foreground_capacity_reserved ||
      !request.background_workload;
  return Finish(std::move(result),
                AgentOk(),
                "admitted",
                result.resource_admission.diagnostic_code);
}

std::string SerializeOptimizedPathResourceGovernanceEvidence(
    const OptimizedPathResourceGovernanceResult& result) {
  std::ostringstream out;
  for (const auto& item : result.evidence) {
    out << item << '\n';
  }
  return out.str();
}

}  // namespace scratchbird::core::agents
