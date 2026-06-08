// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "index_optimizer_integration.hpp"
#include "optimizer_request.hpp"
#include "time_range_summary_pruning.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_EXPLAIN_EVIDENCE_MODEL
struct OptimizerExplainDocument {
  std::string schema_version = "optimizer_explain_v1";
  std::string request_uuid;
  std::string operation_id;
  std::string plan_id;
  // SEARCH_KEY: OPCH_RUNTIME_PLAN_PAYLOAD_EXPLAIN_PARITY
  // This hash is deterministic explain/evidence identity only. It is not row,
  // visibility, authorization, transaction-finality, or recovery authority.
  std::string plan_hash;
  std::string optimizer_profile;
  std::string selected_candidate_id;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::string statistics_snapshot_id;
  std::string metric_snapshot_id;
  std::vector<PlanCandidate> candidates;
  std::vector<OptimizerAuthorityFact> authority_facts;
  std::vector<std::string> invalidation_dependencies;
  std::vector<std::string> statistics_provenance;
  std::vector<std::string> candidate_refusals;
  std::vector<std::string> optimizer_controls;
  std::vector<std::string> join_search_telemetry;
  std::vector<std::string> adaptive_feedback_evidence;
  std::vector<std::string> runtime_actuals;
  std::vector<std::string> memory_metric_evidence;
  std::vector<std::string> route_evidence;
  std::vector<std::string> executor_capability_evidence;
  std::vector<std::string> diagnostics;
  std::vector<std::string> redactions;
};

OptimizerExplainDocument BuildOptimizerExplainDocument(const BoundOptimizerRequest& request,
                                                       const BoundOptimizerResult& result);
PlanSummaryPruneEvidence BuildPlanSummaryPruneEvidence(
    const scratchbird::core::index::PageExtentSummaryPrunePlan& plan);
PlanSummaryPruneEvidence BuildPlanSummaryPruneEvidence(
    const scratchbird::core::index::TimeRangeSummaryPrunePlan& plan);
std::string RenderOptimizerExplainJson(const OptimizerExplainDocument& document);

}  // namespace scratchbird::engine::optimizer
