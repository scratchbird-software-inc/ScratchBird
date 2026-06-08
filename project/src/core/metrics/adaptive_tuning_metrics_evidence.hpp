// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: SB_METRICS_ADAPTIVE_TUNING_EVIDENCE_ODF_101
// Benchmark-clean observability evidence for adaptive resource tuning. This
// model is serialized for diagnostics only and does not change persisted metric
// formats or grant transaction, parser, donor, provider, client, or recovery
// authority.

#include "metric_registry.hpp"

#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::metrics {

struct AdaptiveTuningMetricEvidence {
  std::string knob_label;
  std::string metric_scope_label;
  std::string evidence_id = "adaptive_tuning_metrics_v1";
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 evidence_age_microseconds = 0;
  u64 max_evidence_age_microseconds = 60000000;
  u64 observed_latency_microseconds = 0;
  u64 latency_budget_microseconds = 0;
  u64 observed_memory_bytes = 0;
  u64 memory_budget_bytes = 0;
  u64 backlog_units = 0;
  u64 backlog_budget_units = 0;
  u64 error_count = 0;
  u64 throughput_units_per_second = 0;
  u64 quota_pressure_ppm = 0;
  bool evidence_present = false;
  bool labels_present = false;
  bool evidence_authoritative = false;
  bool benchmark_clean = false;
  bool hard_backlog_refusal = false;
  bool parser_or_donor_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool client_autocommit_authority = false;
  bool wal_recovery_authority = false;
};

struct AdaptiveTuningMetricEvidenceRequest {
  std::string knob_label;
  std::string metric_scope_label = "local_resource_governance";
  std::string evidence_id = "adaptive_tuning_metrics_v1";
  u64 evidence_epoch = 0;
  u64 required_epoch = 0;
  u64 evidence_age_microseconds = 0;
  u64 max_evidence_age_microseconds = 60000000;
  u64 observed_latency_microseconds = 0;
  u64 latency_budget_microseconds = 0;
  u64 observed_memory_bytes = 0;
  u64 memory_budget_bytes = 0;
  u64 backlog_units = 0;
  u64 backlog_budget_units = 0;
  u64 error_count = 0;
  u64 throughput_units_per_second = 0;
  u64 quota_pressure_ppm = 0;
  bool evidence_authoritative = true;
  bool benchmark_clean = true;
  bool hard_backlog_refusal = false;
  bool parser_or_donor_authority = false;
  bool provider_transaction_finality_authority = false;
  bool provider_visibility_authority = false;
  bool client_autocommit_authority = false;
  bool wal_recovery_authority = false;
};

AdaptiveTuningMetricEvidence BuildAdaptiveTuningMetricEvidence(
    const AdaptiveTuningMetricEvidenceRequest& request);

bool AdaptiveTuningMetricEvidenceFresh(
    const AdaptiveTuningMetricEvidence& evidence);

std::vector<std::pair<std::string, std::string>>
SerializeAdaptiveTuningMetricEvidence(
    const AdaptiveTuningMetricEvidence& evidence);

}  // namespace scratchbird::core::metrics
