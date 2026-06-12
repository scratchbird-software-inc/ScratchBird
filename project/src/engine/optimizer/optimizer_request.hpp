// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"
#include "access_path_full.hpp"
#include "statistics_catalog.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_REQUEST_RESULT_ABI
// Bound optimizer requests are engine-owned. SQL text, reference syntax, parser
// claims, and durable object names are not accepted as optimizer authority.
enum class OptimizerAuthorityStatus {
  kPresent,
  kMissing,
  kRejected,
  kRedacted,
};

struct OptimizerAuthorityFact {
  std::string fact_name;
  OptimizerAuthorityStatus status = OptimizerAuthorityStatus::kMissing;
  bool required = true;
  std::string detail;
};

struct OptimizerRequestContext {
  std::string request_uuid;
  std::string operation_id;
  std::string sblr_digest;
  std::string descriptor_set_digest;
  std::string statistics_snapshot_id;
  std::string metric_snapshot_id;
  std::string executor_capability_set_id;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t stats_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t redaction_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t resource_epoch = 0;
  std::uint64_t name_resolution_epoch = 0;
  std::uint64_t memory_policy_epoch = 0;
  std::uint64_t memory_feedback_generation = 0;
  std::uint64_t route_epoch = 0;
  bool security_context_present = false;
  bool transaction_context_present = false;
  bool parser_owned_claims_present = false;
  bool name_authority_present = false;
  bool cluster_build_enabled = false;
};

struct BoundOptimizerRequest {
  OptimizerRequestContext context;
  scratchbird::engine::planner::LogicalPlan logical_plan;
  OptimizerStatisticsCatalog statistics;
  std::optional<AccessPathPlanningRequest> catalog_access_path_request;
  std::vector<OptimizerAuthorityFact> authority_facts;
};

struct OptimizerRequestValidation {
  bool ok = false;
  std::vector<std::string> diagnostics;
  std::vector<OptimizerAuthorityFact> authority_facts;
};

struct BoundOptimizerResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string plan_id;
  std::string optimizer_profile = "deterministic_first_cost_v1";
  std::vector<PlanCandidate> candidates;
  std::vector<std::string> diagnostics;
};

const char* OptimizerAuthorityStatusName(OptimizerAuthorityStatus status);
OptimizerAuthorityFact MakeAuthorityFact(std::string fact_name,
                                         OptimizerAuthorityStatus status,
                                         bool required,
                                         std::string detail = {});
OptimizerRequestValidation ValidateBoundOptimizerRequest(const BoundOptimizerRequest& request);
BoundOptimizerResult MakeRefusedOptimizerResult(const BoundOptimizerRequest& request,
                                                std::string diagnostic_code,
                                                std::vector<std::string> diagnostics = {});
BoundOptimizerResult OptimizeBoundRequest(const BoundOptimizerRequest& request);

}  // namespace scratchbird::engine::optimizer
