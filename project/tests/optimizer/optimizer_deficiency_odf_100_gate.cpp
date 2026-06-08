// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_batch_controller.hpp"
#include "adaptive_batch_policy_evidence.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace opt = scratchbird::engine::optimizer;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(const std::vector<std::string>& values, std::string_view token) {
  for (const auto& value : values) {
    if (value.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

agents::AdaptiveBatchPolicyEvidence Evidence(std::string family,
                                             std::uint64_t backlog,
                                             std::uint64_t backlog_budget,
                                             std::uint64_t worker_pressure,
                                             std::uint64_t quota_pressure) {
  agents::AdaptiveBatchPolicyEvidenceRequest request;
  request.family_label = std::move(family);
  request.evidence_epoch = 100;
  request.required_epoch = 100;
  request.evidence_age_microseconds = 1000;
  request.backlog_units = backlog;
  request.backlog_budget_units = backlog_budget;
  request.worker_pressure_ppm = worker_pressure;
  request.quota_pressure_ppm = quota_pressure;
  return agents::BuildAdaptiveBatchPolicyEvidence(request);
}

opt::AdaptiveBatchControllerRequest BaseRequest(
    opt::AdaptiveBatchFamily family) {
  opt::AdaptiveBatchControllerRequest request;
  request.family = family;
  request.family_label = opt::AdaptiveBatchFamilyName(family);
  request.lower_bound = 8;
  request.upper_bound = 4096;
  request.current_batch_size = 256;
  request.latency_budget_microseconds = 10000;
  request.observed_latency_microseconds = 5000;
  request.memory_budget_bytes = 4 * 1024 * 1024;
  request.observed_memory_bytes = 1024 * 1024;
  request.history.success_count = 10;
  request.history.error_count = 0;
  request.history.consecutive_success_count = 4;
  request.agent_evidence =
      Evidence(request.family_label, 1200, 1000, 100, 100);
  request.benchmark_clean_input_evidence = true;
  return request;
}

std::vector<opt::AdaptiveBatchFamily> Families() {
  return {opt::AdaptiveBatchFamily::kInsert,
          opt::AdaptiveBatchFamily::kCopy,
          opt::AdaptiveBatchFamily::kSegmentVector,
          opt::AdaptiveBatchFamily::kBucketTimeSeries,
          opt::AdaptiveBatchFamily::kGraphFrontier,
          opt::AdaptiveBatchFamily::kIndexMerge};
}

void RequireEvidenceHygiene(const opt::AdaptiveBatchControllerResult& result) {
  const std::string serialized =
      opt::SerializeAdaptiveBatchControllerEvidence(result);
  for (const auto forbidden :
       {"docs/", "execution-plans", "findings", "contracts", "references",
        "parser_executes_sql=true",
        "provider_transaction_finality_authority=true",
        "provider_visibility_authority=true",
        "client_autocommit_authority=true",
        "wal_recovery_authority=true"}) {
    Require(serialized.find(forbidden) == std::string::npos,
            "ODF-100 leaked forbidden evidence or authority token");
  }
}

void CoversAllBatchFamiliesAndActions() {
  std::set<std::string> labels;
  for (const auto family : Families()) {
    auto request = BaseRequest(family);
    const auto result = opt::EvaluateAdaptiveBatchController(request);
    Require(result.ok, "ODF-100 family growth request failed");
    Require(!result.fail_closed, "ODF-100 healthy request failed closed");
    Require(result.benchmark_clean_evidence,
            "ODF-100 benchmark-clean evidence missing");
    Require(result.action == opt::AdaptiveBatchActionClass::kIncrease,
            "ODF-100 healthy backlog did not increase batch");
    Require(result.selected_batch_size > request.current_batch_size,
            "ODF-100 increase did not raise selected batch size");
    Require(result.selected_batch_size <= request.upper_bound,
            "ODF-100 selected batch exceeded upper bound");
    Require(Contains(result.evidence, "odf038_summary_counters_compatible=true"),
            "ODF-100 missing ODF-038 summary counter evidence");
    Require(Contains(result.evidence,
                     "odf079_backpressure_debt_compatible=true"),
            "ODF-100 missing ODF-079 backpressure/debt evidence");
    Require(Contains(result.evidence, "mga_recheck_evidence=required"),
            "ODF-100 missing MGA recheck evidence");
    Require(Contains(result.evidence, "security_recheck_evidence=required"),
            "ODF-100 missing security recheck evidence");
    labels.insert(opt::AdaptiveBatchFamilyName(family));
    RequireEvidenceHygiene(result);
  }
  Require(labels.size() == 6, "ODF-100 did not cover six batch families");

  auto decrease = BaseRequest(opt::AdaptiveBatchFamily::kInsert);
  decrease.observed_latency_microseconds = 20000;
  decrease.agent_evidence =
      Evidence(decrease.family_label, 900, 1000, 950, 100);
  auto result = opt::EvaluateAdaptiveBatchController(decrease);
  Require(result.ok, "ODF-100 pressure request failed unexpectedly");
  Require(result.action == opt::AdaptiveBatchActionClass::kDecrease,
          "ODF-100 latency/worker pressure did not decrease batch");
  Require(result.selected_batch_size < decrease.current_batch_size,
          "ODF-100 decrease did not lower selected batch size");
  Require(result.throttle_recommended,
          "ODF-100 pressure did not expose throttle diagnostic");

  auto hold = BaseRequest(opt::AdaptiveBatchFamily::kCopy);
  hold.agent_evidence = Evidence(hold.family_label, 100, 1000, 100, 100);
  hold.history.consecutive_success_count = 0;
  result = opt::EvaluateAdaptiveBatchController(hold);
  Require(result.ok, "ODF-100 hold request failed unexpectedly");
  Require(result.action == opt::AdaptiveBatchActionClass::kHold,
          "ODF-100 stable request did not hold batch size");
  Require(result.selected_batch_size == hold.current_batch_size,
          "ODF-100 hold changed selected batch size");
}

void FailClosedGuards() {
  auto request = BaseRequest(opt::AdaptiveBatchFamily::kInsert);
  request.family_label.clear();
  auto result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.MISSING_LABELS",
          "ODF-100 missing labels did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kCopy);
  request.lower_bound = 0;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.INVALID_BOUNDS",
          "ODF-100 invalid bounds did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kSegmentVector);
  request.current_batch_size = 99999;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.CURRENT_OUTSIDE_BOUNDS",
          "ODF-100 current outside bounds did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kBucketTimeSeries);
  request.agent_evidence.evidence_authoritative = false;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.STALE_AGENT_EVIDENCE",
          "ODF-100 non-authoritative evidence did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kGraphFrontier);
  request.agent_evidence.evidence_epoch = 99;
  request.agent_evidence.required_epoch = 100;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.STALE_AGENT_EVIDENCE",
          "ODF-100 stale evidence did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kIndexMerge);
  request.agent_evidence.mga_recheck_required = false;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNSAFE_AUTHORITY",
          "ODF-100 missing MGA recheck did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kInsert);
  request.agent_evidence.parser_or_donor_authority = true;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNSAFE_AUTHORITY",
          "ODF-100 parser/donor authority did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kCopy);
  request.agent_evidence.provider_transaction_finality_authority = true;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNSAFE_AUTHORITY",
          "ODF-100 provider finality authority did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kSegmentVector);
  request.agent_evidence.quota_pressure_ppm = 1001;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.QUOTA_OVERRUN",
          "ODF-100 quota overrun did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kBucketTimeSeries);
  request.agent_evidence.hard_backlog_pressure = true;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.HARD_BACKLOG_PRESSURE",
          "ODF-100 hard backlog pressure did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kInsert);
  request.family = opt::AdaptiveBatchFamily::kUnknown;
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNKNOWN_FAMILY",
          "ODF-100 unknown family did not fail closed");

  request = BaseRequest(opt::AdaptiveBatchFamily::kInsert);
  request.family = static_cast<opt::AdaptiveBatchFamily>(99);
  request.family_label = "unknown";
  request.agent_evidence.family_label = "unknown";
  result = opt::EvaluateAdaptiveBatchController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_OPTIMIZER_ADAPTIVE_BATCH.UNKNOWN_FAMILY",
          "ODF-100 corrupt family enum did not fail closed");
}

void AgentEvidenceAdaptorIsPolicyOnly() {
  const auto evidence = Evidence("index_merge", 20, 100, 300, 200);
  const auto fields = agents::SerializeAdaptiveBatchPolicyEvidence(evidence);
  bool saw_family = false;
  bool saw_policy = false;
  for (const auto& field : fields) {
    if (field.first == "family_label" && field.second == "index_merge") {
      saw_family = true;
    }
    if (field.first == "policy_allowed" && field.second == "true") {
      saw_policy = true;
    }
    Require(!(field.first == "parser_or_donor_authority" &&
              field.second == "true"),
            "ODF-100 agent evidence claimed parser/donor authority");
    Require(!(field.first == "provider_transaction_finality_authority" &&
              field.second == "true"),
            "ODF-100 agent evidence claimed provider finality authority");
    Require(!(field.first == "client_autocommit_authority" &&
              field.second == "true"),
            "ODF-100 agent evidence claimed client autocommit authority");
  }
  Require(saw_family && saw_policy,
          "ODF-100 agent adaptor did not provide workload policy evidence");
}

}  // namespace

int main() {
  CoversAllBatchFamiliesAndActions();
  FailClosedGuards();
  AgentEvidenceAdaptorIsPolicyOnly();
  return EXIT_SUCCESS;
}
