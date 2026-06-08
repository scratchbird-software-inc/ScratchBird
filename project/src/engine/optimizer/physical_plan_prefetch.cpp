// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "physical_plan_prefetch.hpp"

#include <string>

namespace scratchbird::engine::optimizer {
namespace page = scratchbird::storage::page;
namespace platform = scratchbird::core::platform;

namespace {

bool FindPlanNodeDigest(const PhysicalPlanNode& node,
                        const std::string& node_id,
                        std::string* digest) {
  if (node.node_id == node_id) {
    *digest = node.descriptor_digest;
    return true;
  }
  for (const auto& child : node.children) {
    if (FindPlanNodeDigest(child, node_id, digest)) {
      return true;
    }
  }
  return false;
}

page::PlanAwarePrefetchResult Refuse(std::string diagnostic_code,
                                     std::string message_key,
                                     std::string detail) {
  page::PlanAwarePrefetchResult result;
  result.status = {platform::StatusCode::platform_required_feature_missing,
                   platform::Severity::error, platform::Subsystem::engine};
  result.fail_closed = true;
  result.evidence.push_back("plan_aware_prefetch.fail_closed=true");
  result.evidence.push_back("plan_aware_prefetch.refused=" + diagnostic_code);
  result.evidence.push_back(
      "plan_aware_prefetch.optimizer_physical_plan_driven=true");
  result.evidence.push_back(
      "plan_aware_prefetch.diagnostic_only_authority=true");
  result.evidence.push_back("plan_aware_prefetch.finality_authority=false");
  result.evidence.push_back("plan_aware_prefetch.visibility_authority=false");
  result.evidence.push_back("plan_aware_prefetch.security_authority=false");
  result.diagnostic = page::MakePlanAwarePrefetchDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      std::move(detail));
  return result;
}

}  // namespace

page::PlanAwarePrefetchResult ExecutePhysicalPlanDrivenPrefetch(
    const PhysicalPlanNode& physical_plan_root,
    const PhysicalPlanPrefetchInput& input) {
  if (physical_plan_root.node_id.empty() ||
      physical_plan_root.descriptor_digest.empty() ||
      input.physical_plan_generation == 0) {
    return Refuse(
        "plan_prefetch_physical_plan_identity_required",
        "engine.optimizer.plan_aware_prefetch.physical_plan_identity_required",
        "physical plan root identity, descriptor digest, and generation are required");
  }

  const auto validation = ValidatePhysicalPlanNode(physical_plan_root);
  if (!validation.ok) {
    return Refuse("plan_prefetch_physical_plan_validation_failed",
                  "engine.optimizer.plan_aware_prefetch.physical_plan_validation_failed",
                  validation.diagnostics.empty()
                      ? "physical plan validation failed"
                      : validation.diagnostics.front());
  }

  for (const auto& descriptor : input.descriptors) {
    std::string plan_digest;
    if (!FindPlanNodeDigest(physical_plan_root,
                            descriptor.physical_plan_node_id,
                            &plan_digest)) {
      return Refuse("plan_prefetch_physical_plan_node_missing",
                    "engine.optimizer.plan_aware_prefetch.physical_plan_node_missing",
                    "prefetch descriptor does not reference a selected physical plan node");
    }
    if (plan_digest != descriptor.physical_plan_descriptor_digest) {
      return Refuse("plan_prefetch_physical_descriptor_mismatch",
                    "engine.optimizer.plan_aware_prefetch.physical_descriptor_mismatch",
                    "prefetch descriptor digest does not match physical plan node");
    }
  }

  page::PlanAwarePrefetchRequest request;
  request.physical_plan_id = physical_plan_root.node_id;
  request.physical_plan_generation = input.physical_plan_generation;
  request.descriptors = input.descriptors;
  request.budget = input.budget;
  request.cancellation = input.cancellation;
  auto result = page::ExecutePlanAwarePrefetch(request);
  result.evidence.push_back(
      "plan_aware_prefetch.optimizer_physical_plan_driven=true");
  return result;
}

}  // namespace scratchbird::engine::optimizer
