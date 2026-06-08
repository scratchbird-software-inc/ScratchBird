// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "foreign_memory_reservation.hpp"
#include "memory.hpp"
#include "metric_registry.hpp"
#include "runtime_platform.hpp"

#include <array>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::metrics::MetricValue;
using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// CEIC-023_MEMORY_SUPPORT_BUNDLE_LOW_MEMORY
enum class MemorySupportBundleMode {
  standard,
  low_memory,
  emergency_summary
};

struct MemorySupportBundleLimits {
  u64 max_rows = 256;
  u64 max_output_bytes = 32ull * 1024ull;
  u64 max_key_bytes = 96;
  u64 max_value_bytes = 256;
  u64 max_diagnostic_arguments = 16;
  u64 max_metrics = 64;
  u64 max_pressure_transitions = 16;
  u64 max_allocation_classes = 16;
};

struct MemorySupportBundlePressureTransitionRow {
  std::string previous_state;
  std::string new_state;
  std::string trigger;
  u64 current_bytes = 0;
  u64 limit_bytes = 0;
  bool emergency = false;
};

struct MemorySupportBundleAllocationClassSnapshot {
  std::string memory_class;
  u64 current_bytes = 0;
  u64 peak_bytes = 0;
  u64 allocation_count = 0;
  u64 release_count = 0;
  u64 failure_count = 0;
};

struct ProtectedSecretRouteEvidence {
  std::string source_kind;
  std::string route_id;
  bool routed_through_protected_buffer = false;
  bool protected_reference_only = true;
  bool plaintext_material_observed = false;
};

// CEIC-023_PROTECTED_MEMORY_SECURITY_REVIEW
struct ProtectedMemorySecurityReview {
  bool enabled = false;
  bool diagnostics_log_exception_scan_complete = false;
  bool support_bundle_scan_complete = false;
  bool zeroization_not_optimized_away = false;
  bool protected_buffer_zero_on_release = true;
  bool require_platform_protection_attempts = true;
  bool hsm_kms_plugin_routes_use_protected_buffers = false;
  bool require_protected_material_exclusion = true;
  std::vector<ProtectedMemoryEvidence> protected_buffer_evidence;
  std::vector<ProtectedSecretRouteEvidence> secret_routes;
};

struct MemorySupportBundleEmergencyRow {
  std::array<char, 64> key{};
  std::array<char, 96> value{};
  bool redacted = false;
};

struct MemorySupportBundleEmergencySummary {
  bool emitted = false;
  bool allocation_free_model = true;
  bool bounded = true;
  bool redaction_before_buffering = true;
  bool protected_material_excluded = true;
  u64 max_rows = 16;
  u64 row_count = 0;
  u64 dropped_row_count = 0;
  std::array<char, 256> authority_scope{};
  std::array<MemorySupportBundleEmergencyRow, 16> rows{};
};

// MMCH_MEMORY_SUPPORT_BUNDLE_EVIDENCE
struct MemorySupportBundleRow {
  std::string key;
  std::string value;
  std::string redaction_class = "public";
  bool redacted = false;
  std::string tamper_evidence_digest;
};

struct MemorySupportBundleRequest {
  std::string bundle_profile = "memory.support_bundle.default.v1";
  std::string redaction_profile = "memory.support_bundle.redacted.v1";
  MemorySupportBundleMode mode = MemorySupportBundleMode::standard;
  MemorySupportBundleLimits limits;
  MemoryAccountingSnapshot snapshot;
  std::vector<DiagnosticRecord> diagnostics;
  std::vector<MetricValue> metrics;
  ForeignMemoryReservationSnapshot foreign_memory_snapshot;
  std::vector<MemorySupportBundlePressureTransitionRow> pressure_transitions;
  std::vector<MemorySupportBundleAllocationClassSnapshot> allocation_classes;
  std::vector<MemorySupportBundleRow> memory_fragmentation_rows;
  std::vector<MemorySupportBundleRow> memory_working_set_locality_rows;
  ProtectedMemorySecurityReview protected_memory_review;
  u64 max_top_contexts = 8;
  u64 max_top_categories = 8;
  u64 max_memory_fragmentation_rows = 16;
  u64 max_memory_working_set_locality_rows = 16;
  u64 max_foreign_sources = 16;
  u64 max_foreign_owning_scopes = 16;
  bool include_failure_reasons = true;
  bool include_metrics = true;
  bool include_foreign_memory = false;
  bool allow_protected_material = false;
  bool redaction_before_buffering = true;
  bool exclude_protected_material = true;
  bool include_self_accounting = true;
};

struct MemorySupportBundleResult {
  Status status;
  std::vector<MemorySupportBundleRow> rows;
  std::vector<std::string> evidence;
  u64 redacted_row_count = 0;
  u64 top_context_count = 0;
  u64 top_category_count = 0;
  u64 foreign_source_count = 0;
  u64 foreign_owning_scope_count = 0;
  u64 failure_reason_count = 0;
  u64 metric_count = 0;
  u64 pressure_transition_count = 0;
  u64 allocation_class_count = 0;
  u64 memory_fragmentation_row_count = 0;
  u64 memory_working_set_locality_row_count = 0;
  u64 protected_buffer_evidence_count = 0;
  u64 protected_secret_route_count = 0;
  u64 dropped_row_count = 0;
  u64 output_bytes = 0;
  u64 output_byte_limit = 0;
  u64 row_limit = 0;
  bool low_memory_mode = false;
  bool emergency_summary_mode = false;
  bool redaction_before_buffering = false;
  bool protected_material_excluded = false;
  bool protected_memory_review_passed = false;
  MemorySupportBundleEmergencySummary emergency_summary;

  bool ok() const {
    return status.ok();
  }
};

const char* MemorySupportBundleModeName(MemorySupportBundleMode mode);
MemorySupportBundleResult BuildMemorySupportBundleEvidence(
    MemorySupportBundleRequest request);
MemorySupportBundleEmergencySummary BuildMemorySupportBundleEmergencySummary(
    const MemorySupportBundleRequest& request);
std::string RedactMemorySupportBundleValue(std::string value,
                                           bool allow_protected_material);

}  // namespace scratchbird::core::memory
