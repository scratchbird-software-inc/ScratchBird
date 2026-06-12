// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "plan_aware_prefetch.hpp"
#include "runtime_platform.hpp"

#include <string>
#include <vector>

namespace scratchbird::storage::page {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

enum class BufferPrefetchRouteKind {
  kSequentialScan,
  kJoin,
  kBulkIngest,
  kAggregate,
};

struct BufferPrefetchMeasurement {
  u64 cache_lookups = 0;
  u64 cache_hits = 0;
  u64 io_read_ops = 0;
  u64 wait_time_us = 0;
};

struct BufferPrefetchAuthorityContext {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_required = false;
  bool parser_client_or_reference_authority = false;
  bool prefetch_visibility_or_finality_authority = false;
  bool prefetch_recovery_authority = false;
  bool prefetch_security_authority = false;
};

struct HotPagePinningEvidence {
  u64 expected_page_generation = 0;
  u64 observed_page_generation = 0;
  u64 expected_epoch = 0;
  u64 observed_epoch = 0;
  u64 hot_pages_requested = 0;
  u64 hot_pages_pinned = 0;
  bool pinning_runtime_consumed = false;
  bool unsafe_generation_or_epoch = false;
};

struct BufferPrefetchReadaheadRequest {
  BufferPrefetchRouteKind route_kind = BufferPrefetchRouteKind::kSequentialScan;
  std::string route_label;
  bool runtime_consumed = false;
  bool contract_only = false;
  bool resource_pressure = false;
  bool cancellation_requested = false;
  PlanAwarePrefetchRequest prefetch;
  HotPagePinningEvidence hot_page_pinning;
  BufferPrefetchMeasurement without_prefetch;
  BufferPrefetchMeasurement with_prefetch;
  BufferPrefetchAuthorityContext authority;
};

struct BufferPrefetchImprovement {
  u64 cache_hit_delta = 0;
  u64 io_read_ops_saved = 0;
  u64 wait_time_us_saved = 0;
  bool cache_hit_improved = false;
  bool io_improved = false;
  bool wait_improved = false;
};

struct BufferPrefetchReadaheadResult {
  Status status;
  bool accepted = false;
  bool benchmark_clean = false;
  bool fallback_used = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  DiagnosticRecord diagnostic;
  PlanAwarePrefetchResult prefetch_result;
  BufferPrefetchImprovement improvement;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && accepted && benchmark_clean; }
};

const char* BufferPrefetchRouteKindName(BufferPrefetchRouteKind route_kind);

BufferPrefetchReadaheadResult EvaluateBufferPrefetchReadaheadRoute(
    const BufferPrefetchReadaheadRequest& request);

}  // namespace scratchbird::storage::page
