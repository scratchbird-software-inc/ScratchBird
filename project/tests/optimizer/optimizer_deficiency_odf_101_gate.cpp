// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_tuning_controller.hpp"
#include "adaptive_tuning_metrics_evidence.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace metrics = scratchbird::core::metrics;

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

std::vector<agents::AdaptiveTuningKnob> Knobs() {
  return {agents::AdaptiveTuningKnob::kPrefetchDepth,
          agents::AdaptiveTuningKnob::kMergeWorkers,
          agents::AdaptiveTuningKnob::kRefreshInterval,
          agents::AdaptiveTuningKnob::kCandidateBudget,
          agents::AdaptiveTuningKnob::kCachePartition,
          agents::AdaptiveTuningKnob::kEvidenceSampleRate};
}

agents::AdaptiveTuningSafetyPolicy Safety() {
  agents::AdaptiveTuningSafetyPolicy safety;
  safety.policy_allowed = true;
  safety.semantics_neutral_proof = true;
  safety.advisory_resource_governance_only = true;
  safety.engine_mga_authoritative = true;
  safety.security_snapshot_bound = true;
  safety.grants_proven = true;
  return safety;
}

metrics::AdaptiveTuningMetricEvidence MetricEvidence(
    std::string knob_label,
    std::uint64_t latency,
    std::uint64_t memory,
    std::uint64_t backlog,
    std::uint64_t backlog_budget,
    std::uint64_t errors,
    std::uint64_t quota) {
  metrics::AdaptiveTuningMetricEvidenceRequest request;
  request.knob_label = std::move(knob_label);
  request.evidence_epoch = 200;
  request.required_epoch = 200;
  request.evidence_age_microseconds = 1000;
  request.observed_latency_microseconds = latency;
  request.latency_budget_microseconds = 10000;
  request.observed_memory_bytes = memory;
  request.memory_budget_bytes = 4 * 1024 * 1024;
  request.backlog_units = backlog;
  request.backlog_budget_units = backlog_budget;
  request.error_count = errors;
  request.throughput_units_per_second = 1000;
  request.quota_pressure_ppm = quota;
  return metrics::BuildAdaptiveTuningMetricEvidence(request);
}

agents::AdaptiveTuningControllerRequest BaseRequest(
    agents::AdaptiveTuningKnob knob) {
  const std::string label = agents::AdaptiveTuningKnobName(knob);
  auto request = agents::BuildAdaptiveTuningAgentRequest(
      knob,
      1,
      1024,
      64,
      128,
      Safety(),
      MetricEvidence(label, 5000, 1024 * 1024, 1200, 1000, 0, 100));
  request.resource_governance.operation_id =
      "odf101.adaptive_tuning." + label;
  request.resource_governance.descriptor.descriptor_id =
      "odf106.adaptive_tuning.runtime_quota";
  request.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kAdaptiveTuningKnob;
  request.resource_governance.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.resource_governance.descriptor.source_path_or_label =
      "runtime.policy.odf106.adaptive_tuning";
  request.resource_governance.descriptor.descriptor_generation = 101;
  request.resource_governance.descriptor.expected_generation = 101;
  request.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kSlowdownDegrade;
  request.resource_governance.descriptor.benchmark_clean = true;
  request.resource_governance.descriptor.runtime_dependency_present = true;
  request.resource_governance.descriptor.limits = {
      4194304, 1, 1, 1024, 4, 4, 2000, 2048, 64, 2048, 64, 4, 1000000};
  request.resource_governance.requested = {
      1024 * 1024, 0, 0, 0, 0, 1, 1200, 128, 1, 128, 1, 1, 1000};
  return request;
}

void RequireEvidenceHygiene(
    const agents::AdaptiveTuningControllerResult& result) {
  const std::string serialized =
      agents::SerializeAdaptiveTuningControllerEvidence(result);
  for (const auto forbidden :
       {"docs/", "execution-plans", "findings", "contracts", "references",
        "parser_executes_sql=true",
        "provider_transaction_finality_authority=true",
        "provider_visibility_authority=true",
        "client_autocommit_authority=true",
        "wal_recovery_authority=true"}) {
    Require(serialized.find(forbidden) == std::string::npos,
            "ODF-101 leaked forbidden evidence or authority token");
  }
}

void CoversAllKnobsAndActions() {
  std::set<std::string> labels;
  for (const auto knob : Knobs()) {
    auto request = BaseRequest(knob);
    const auto result = agents::EvaluateAdaptiveTuningController(request);
    Require(result.ok, "ODF-101 healthy request failed");
    Require(!result.fail_closed, "ODF-101 healthy request failed closed");
    Require(result.benchmark_clean_evidence,
            "ODF-101 missing benchmark-clean evidence");
    Require(result.semantics_neutral,
            "ODF-101 result did not carry semantics-neutral proof");
    Require(Contains(result.evidence, "mga_recheck_evidence=required"),
            "ODF-101 missing MGA recheck evidence");
    Require(Contains(result.evidence, "security_recheck_evidence=required"),
            "ODF-101 missing security recheck evidence");
    Require(Contains(result.metrics_evidence, "benchmark_clean=true"),
            "ODF-101 missing serialized metrics evidence");
    labels.insert(agents::AdaptiveTuningKnobName(knob));
    RequireEvidenceHygiene(result);
  }
  Require(labels.size() == 6, "ODF-101 did not cover six safe knobs");

  auto request = BaseRequest(agents::AdaptiveTuningKnob::kPrefetchDepth);
  auto result = agents::EvaluateAdaptiveTuningController(request);
  Require(result.action == agents::AdaptiveTuningActionClass::kIncrease,
          "ODF-101 did not increase prefetch depth on healthy backlog");
  Require(result.selected_value > request.current_value,
          "ODF-101 increase did not raise selected value");
  Require(result.selected_value <= request.max_value,
          "ODF-101 increase exceeded upper bound");

  request = BaseRequest(agents::AdaptiveTuningKnob::kMergeWorkers);
  request.metrics = MetricEvidence(request.knob_label,
                                   20000,
                                   1024 * 1024,
                                   100,
                                   1000,
                                   0,
                                   100);
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(result.action == agents::AdaptiveTuningActionClass::kDecrease,
          "ODF-101 did not decrease worker knob under latency pressure");
  Require(result.selected_value < request.current_value,
          "ODF-101 decrease did not lower selected value");

  request = BaseRequest(agents::AdaptiveTuningKnob::kRefreshInterval);
  request.metrics = MetricEvidence(request.knob_label,
                                   20000,
                                   1024 * 1024,
                                   100,
                                   1000,
                                   0,
                                   100);
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(result.action == agents::AdaptiveTuningActionClass::kIncrease,
          "ODF-101 did not increase refresh interval under pressure");

  request = BaseRequest(agents::AdaptiveTuningKnob::kCandidateBudget);
  request.metrics = MetricEvidence(request.knob_label,
                                   8000,
                                   1024 * 1024,
                                   100,
                                   1000,
                                   0,
                                   100);
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(result.action == agents::AdaptiveTuningActionClass::kHold,
          "ODF-101 stable request did not hold");
  Require(result.selected_value == request.current_value,
          "ODF-101 hold changed selected value");

  request = BaseRequest(agents::AdaptiveTuningKnob::kCachePartition);
  request.safety.reset_to_default_requested = true;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(result.action == agents::AdaptiveTuningActionClass::kReset,
          "ODF-101 reset request did not reset");
  Require(result.selected_value == request.default_value,
          "ODF-101 reset did not select default value");

  request = BaseRequest(agents::AdaptiveTuningKnob::kEvidenceSampleRate);
  request.safety.use_default_requested = true;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(result.action == agents::AdaptiveTuningActionClass::kDefault,
          "ODF-101 default request did not select default action");
  Require(result.selected_value == request.default_value,
          "ODF-101 default did not select default value");
}

void FailClosedGuards() {
  auto request = BaseRequest(agents::AdaptiveTuningKnob::kPrefetchDepth);
  request.knob_label.clear();
  auto result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.MISSING_LABELS",
          "ODF-101 missing labels did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kMergeWorkers);
  request.min_value = 0;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.INVALID_BOUNDS",
          "ODF-101 invalid bounds did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kRefreshInterval);
  request.default_value = 4096;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.INVALID_DEFAULT",
          "ODF-101 invalid default did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kCandidateBudget);
  request.current_value = 4096;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.CURRENT_OUTSIDE_BOUNDS",
          "ODF-101 current outside bounds did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kCachePartition);
  request.metrics.evidence_authoritative = false;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.STALE_METRICS_EVIDENCE",
          "ODF-101 non-authoritative metrics did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kEvidenceSampleRate);
  request.metrics.evidence_epoch = 199;
  request.metrics.required_epoch = 200;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.STALE_METRICS_EVIDENCE",
          "ODF-101 stale metrics did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kPrefetchDepth);
  request.safety.semantics_neutral_proof = false;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.UNSAFE_POLICY",
          "ODF-101 missing semantics-neutral proof did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kMergeWorkers);
  request.safety.mga_recheck_required = false;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.UNSAFE_AUTHORITY",
          "ODF-101 missing MGA recheck did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kRefreshInterval);
  request.safety.parser_or_donor_authority = true;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.UNSAFE_AUTHORITY",
          "ODF-101 parser/donor authority did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kCandidateBudget);
  request.metrics.provider_transaction_finality_authority = true;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.UNSAFE_AUTHORITY",
          "ODF-101 provider authority did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kCachePartition);
  request.metrics.quota_pressure_ppm = 1001;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.QUOTA_OVERRUN",
          "ODF-101 quota overrun did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kEvidenceSampleRate);
  request.metrics.hard_backlog_refusal = true;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.HARD_BACKLOG_PRESSURE",
          "ODF-101 hard backlog refusal did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kPrefetchDepth);
  request.knob = agents::AdaptiveTuningKnob::kUnknown;
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.UNKNOWN_KNOB",
          "ODF-101 unknown knob did not fail closed");

  request = BaseRequest(agents::AdaptiveTuningKnob::kPrefetchDepth);
  request.knob = static_cast<agents::AdaptiveTuningKnob>(99);
  request.knob_label = "unknown";
  request.metrics.knob_label = "unknown";
  result = agents::EvaluateAdaptiveTuningController(request);
  Require(!result.ok &&
              result.diagnostic_code ==
                  "SB_AGENT_ADAPTIVE_TUNING.UNKNOWN_KNOB",
          "ODF-101 corrupt knob enum did not fail closed");
}

void MetricsEvidenceSerializerIsBenchmarkClean() {
  const auto evidence =
      MetricEvidence("prefetch_depth", 5000, 1024 * 1024, 10, 100, 0, 50);
  const auto fields = metrics::SerializeAdaptiveTuningMetricEvidence(evidence);
  bool saw_knob = false;
  bool saw_clean = false;
  for (const auto& field : fields) {
    if (field.first == "knob_label" && field.second == "prefetch_depth") {
      saw_knob = true;
    }
    if (field.first == "benchmark_clean" && field.second == "true") {
      saw_clean = true;
    }
    Require(!(field.first == "parser_or_donor_authority" &&
              field.second == "true"),
            "ODF-101 metrics evidence claimed parser/donor authority");
    Require(!(field.first == "provider_transaction_finality_authority" &&
              field.second == "true"),
            "ODF-101 metrics evidence claimed provider finality authority");
    Require(!(field.first == "client_autocommit_authority" &&
              field.second == "true"),
            "ODF-101 metrics evidence claimed client autocommit authority");
  }
  Require(saw_knob && saw_clean,
          "ODF-101 metrics serializer missed knob or benchmark evidence");
}

}  // namespace

int main() {
  CoversAllKnobsAndActions();
  FailClosedGuards();
  MetricsEvidenceSerializerIsBenchmarkClean();
  return EXIT_SUCCESS;
}
