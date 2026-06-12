// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "adaptive_tuning_metrics_evidence.hpp"

#include <utility>

namespace scratchbird::core::metrics {
namespace {

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

void Add(std::vector<std::pair<std::string, std::string>>* fields,
         std::string key,
         std::string value) {
  fields->push_back({std::move(key), std::move(value)});
}

}  // namespace

AdaptiveTuningMetricEvidence BuildAdaptiveTuningMetricEvidence(
    const AdaptiveTuningMetricEvidenceRequest& request) {
  AdaptiveTuningMetricEvidence evidence;
  evidence.knob_label = request.knob_label;
  evidence.metric_scope_label = request.metric_scope_label;
  evidence.evidence_id = request.evidence_id;
  evidence.evidence_epoch = request.evidence_epoch;
  evidence.required_epoch = request.required_epoch;
  evidence.evidence_age_microseconds = request.evidence_age_microseconds;
  evidence.max_evidence_age_microseconds =
      request.max_evidence_age_microseconds;
  evidence.observed_latency_microseconds =
      request.observed_latency_microseconds;
  evidence.latency_budget_microseconds = request.latency_budget_microseconds;
  evidence.observed_memory_bytes = request.observed_memory_bytes;
  evidence.memory_budget_bytes = request.memory_budget_bytes;
  evidence.backlog_units = request.backlog_units;
  evidence.backlog_budget_units = request.backlog_budget_units;
  evidence.error_count = request.error_count;
  evidence.throughput_units_per_second =
      request.throughput_units_per_second;
  evidence.quota_pressure_ppm = request.quota_pressure_ppm;
  evidence.evidence_present = !request.knob_label.empty();
  evidence.labels_present =
      !request.knob_label.empty() && !request.metric_scope_label.empty();
  evidence.evidence_authoritative = request.evidence_authoritative;
  evidence.benchmark_clean = request.benchmark_clean;
  evidence.hard_backlog_refusal = request.hard_backlog_refusal;
  evidence.parser_or_reference_authority = request.parser_or_reference_authority;
  evidence.provider_transaction_finality_authority =
      request.provider_transaction_finality_authority;
  evidence.provider_visibility_authority =
      request.provider_visibility_authority;
  evidence.client_autocommit_authority = request.client_autocommit_authority;
  evidence.wal_recovery_authority = request.wal_recovery_authority;
  return evidence;
}

bool AdaptiveTuningMetricEvidenceFresh(
    const AdaptiveTuningMetricEvidence& evidence) {
  if (evidence.required_epoch == 0 || evidence.evidence_epoch == 0 ||
      evidence.evidence_epoch < evidence.required_epoch) {
    return false;
  }
  return evidence.max_evidence_age_microseconds == 0 ||
         evidence.evidence_age_microseconds <=
             evidence.max_evidence_age_microseconds;
}

std::vector<std::pair<std::string, std::string>>
SerializeAdaptiveTuningMetricEvidence(
    const AdaptiveTuningMetricEvidence& evidence) {
  std::vector<std::pair<std::string, std::string>> fields;
  Add(&fields, "knob_label", evidence.knob_label);
  Add(&fields, "metric_scope_label", evidence.metric_scope_label);
  Add(&fields, "evidence_id", evidence.evidence_id);
  Add(&fields, "evidence_epoch", std::to_string(evidence.evidence_epoch));
  Add(&fields, "required_epoch", std::to_string(evidence.required_epoch));
  Add(&fields,
      "evidence_age_microseconds",
      std::to_string(evidence.evidence_age_microseconds));
  Add(&fields,
      "max_evidence_age_microseconds",
      std::to_string(evidence.max_evidence_age_microseconds));
  Add(&fields,
      "observed_latency_microseconds",
      std::to_string(evidence.observed_latency_microseconds));
  Add(&fields,
      "latency_budget_microseconds",
      std::to_string(evidence.latency_budget_microseconds));
  Add(&fields,
      "observed_memory_bytes",
      std::to_string(evidence.observed_memory_bytes));
  Add(&fields,
      "memory_budget_bytes",
      std::to_string(evidence.memory_budget_bytes));
  Add(&fields, "backlog_units", std::to_string(evidence.backlog_units));
  Add(&fields,
      "backlog_budget_units",
      std::to_string(evidence.backlog_budget_units));
  Add(&fields, "error_count", std::to_string(evidence.error_count));
  Add(&fields,
      "throughput_units_per_second",
      std::to_string(evidence.throughput_units_per_second));
  Add(&fields,
      "quota_pressure_ppm",
      std::to_string(evidence.quota_pressure_ppm));
  Add(&fields, "evidence_present", BoolText(evidence.evidence_present));
  Add(&fields, "labels_present", BoolText(evidence.labels_present));
  Add(&fields,
      "evidence_authoritative",
      BoolText(evidence.evidence_authoritative));
  Add(&fields, "benchmark_clean", BoolText(evidence.benchmark_clean));
  Add(&fields,
      "hard_backlog_refusal",
      BoolText(evidence.hard_backlog_refusal));
  Add(&fields,
      "parser_or_reference_authority",
      BoolText(evidence.parser_or_reference_authority));
  Add(&fields,
      "provider_transaction_finality_authority",
      BoolText(evidence.provider_transaction_finality_authority));
  Add(&fields,
      "provider_visibility_authority",
      BoolText(evidence.provider_visibility_authority));
  Add(&fields,
      "client_autocommit_authority",
      BoolText(evidence.client_autocommit_authority));
  Add(&fields,
      "wal_recovery_authority",
      BoolText(evidence.wal_recovery_authority));
  return fields;
}

}  // namespace scratchbird::core::metrics
