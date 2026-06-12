// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "metric_registry.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api::observability {

// SEARCH_KEY: OEIC_OPTIMIZER_METRIC_RETENTION_REDACTION
// Optimizer metric support-bundle export is redacted evidence only. It cannot
// provide transaction finality, visibility, security, parser, reference, recovery,
// WAL, cluster, or benchmark authority.

struct OptimizerMetricSupportBundleAuthority {
  bool metric_registry_authoritative = false;
  bool optimizer_manifest_authoritative = false;
  bool support_bundle_request_authorized = false;
  bool redaction_policy_bound = false;
  bool retention_policy_bound = false;
  bool metrics_trusted = false;
  bool snapshot_fresh = false;
  bool engine_scope_bound = false;
  bool parser_or_reference_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool metric_visibility_or_finality_authority = false;
  bool metric_recovery_authority = false;
  bool wal_or_redo_authority = false;
  bool cluster_authority = false;
  bool benchmark_authority = false;
};

struct OptimizerMetricSupportBundleRequest {
  std::string scope_uuid;
  std::string support_bundle_id;
  std::string capture_generation;
  std::string evidence_digest;
  std::uint64_t min_source_generation = 1;
  std::uint64_t max_metric_values = 4096;
  bool benchmark_clean_export = false;
  bool allow_sensitive_labels = false;
  std::vector<scratchbird::core::metrics::MetricValue> metric_snapshot;
  OptimizerMetricSupportBundleAuthority authority;
};

struct OptimizerMetricSupportBundleRow {
  std::string registry_family;
  std::string metric_family;
  std::string producer_owner;
  std::string retention_class;
  std::string redaction_class;
  std::string support_bundle_class;
  std::string serialized_redacted_value;
};

struct OptimizerMetricSupportBundleResult {
  bool ok = false;
  std::string diagnostic_code;
  std::string detail;
  std::string tamper_digest;
  std::string support_bundle_json;
  bool redaction_applied = false;
  std::vector<std::string> evidence;
  std::vector<OptimizerMetricSupportBundleRow> rows;
};

OptimizerMetricSupportBundleResult BuildOptimizerMetricSupportBundle(
    const OptimizerMetricSupportBundleRequest& request);

}  // namespace scratchbird::engine::internal_api::observability
