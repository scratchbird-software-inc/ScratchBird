// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PLAN-AWARE-PREFETCH-CONTRACT-ANCHOR
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

enum class PlanAwarePrefetchFamily {
  kHeapPage,
  kLargePayload,
  kVectorPayloadPage,
  kGraphAdjacencyPage,
  kTimeSeriesBucket,
  kExtentMetadata,
  kUnsupported
};

struct PlanAwarePrefetchBudget {
  u64 max_bytes = static_cast<u64>(~0ull);
  u64 max_pages = static_cast<u64>(~0ull);
  u64 max_items = static_cast<u64>(~0ull);
  u64 max_outstanding = static_cast<u64>(~0ull);
};

struct PlanAwarePrefetchCancellation {
  bool cancel_before_scheduling = false;
  u64 cancel_after_considered_items = 0;
};

struct PlanAwarePrefetchDescriptor {
  PlanAwarePrefetchFamily family = PlanAwarePrefetchFamily::kUnsupported;
  std::string item_id;
  std::string physical_plan_node_id;
  std::string physical_plan_descriptor_digest;
  u64 physical_plan_generation = 0;
  u64 descriptor_generation = 0;
  std::string late_materialization_source;
  bool late_materialization_proof_present = false;
  bool full_payload_prefetch = false;
  u64 byte_cost = 0;
  u64 page_cost = 0;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool write_ahead_log_recovery_or_finality_authority = false;
  bool prefetch_evidence_used_for_mga_or_security_authority = false;
  bool diagnostic_only_authority = true;
};

struct PlanAwarePrefetchRequest {
  std::string physical_plan_id;
  u64 physical_plan_generation = 0;
  std::vector<PlanAwarePrefetchDescriptor> descriptors;
  PlanAwarePrefetchBudget budget;
  PlanAwarePrefetchCancellation cancellation;
};

struct PlanAwarePrefetchCounters {
  u64 considered_items = 0;
  u64 scheduled_items = 0;
  u64 scheduled_heap_pages = 0;
  u64 scheduled_large_payloads = 0;
  u64 scheduled_vector_payload_pages = 0;
  u64 scheduled_graph_adjacency_pages = 0;
  u64 scheduled_time_series_buckets = 0;
  u64 scheduled_extent_metadata = 0;
  u64 skipped_items = 0;
  u64 refused_items = 0;
  u64 used_bytes = 0;
  u64 used_pages = 0;
  u64 max_outstanding_observed = 0;
  bool budget_exhausted = false;
  bool cancelled = false;
  bool cancellation_before_scheduling = false;
  bool cancellation_while_scheduling = false;
  bool late_materialization_source_present = false;
  bool physical_plan_source_present = false;
  bool diagnostic_only_authority = true;
};

struct PlanAwarePrefetchResult {
  Status status;
  bool fail_closed = false;
  PlanAwarePrefetchCounters counters;
  std::vector<std::string> scheduled_item_ids;
  std::vector<std::string> skipped_item_ids;
  std::vector<std::string> refused_item_ids;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* PlanAwarePrefetchFamilyName(PlanAwarePrefetchFamily family);

PlanAwarePrefetchResult ExecutePlanAwarePrefetch(
    const PlanAwarePrefetchRequest& request);

DiagnosticRecord MakePlanAwarePrefetchDiagnostic(Status status,
                                                 std::string diagnostic_code,
                                                 std::string message_key,
                                                 std::string detail = {});

}  // namespace scratchbird::storage::page
