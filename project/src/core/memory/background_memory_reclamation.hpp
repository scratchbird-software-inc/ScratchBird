// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "runtime_platform.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::core::memory {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u64;

// MMCH_BACKGROUND_MEMORY_RECLAMATION
enum class BackgroundMemoryReclamationWorkKind {
  idle_arena,
  clean_page_cache_frame,
  spill_record,
  old_diagnostic,
  completed_query_state
};

struct BackgroundMemoryReclamationPolicy {
  u64 max_items_per_run = 64;
  u64 max_reclaim_bytes_per_run = 64ull * 1024ull * 1024ull;
  bool require_route_label = true;
  bool require_engine_mga_authority = true;
  bool cancellation_requested = false;
};

using BackgroundMemoryReclamationCallback =
    std::function<Status(std::vector<std::string>* evidence)>;

class ReservationBackedMemoryResource;

struct BackgroundMemoryReclamationWorkItem {
  BackgroundMemoryReclamationWorkKind kind = BackgroundMemoryReclamationWorkKind::idle_arena;
  std::string label;
  u64 estimated_reclaim_bytes = 0;
  bool eligible = true;
  bool cancellation_safe = true;
  bool parser_or_donor_authority = false;
  bool client_authority = false;
  bool provider_authority = false;
  bool wal_authority = false;
  BackgroundMemoryReclamationCallback reclaim;
};

struct BackgroundMemoryReclamationRequest {
  std::string route_label;
  std::string operation_id;
  bool engine_mga_authoritative = true;
  std::vector<BackgroundMemoryReclamationWorkItem> work_items;
};

struct BackgroundMemoryReclamationCounters {
  u64 scanned_count = 0;
  u64 reclaimed_count = 0;
  u64 retained_count = 0;
  u64 failed_count = 0;
  u64 cancelled_count = 0;
  u64 reclaimed_bytes = 0;
};

struct BackgroundMemoryReclamationResult {
  Status status;
  bool fail_closed = false;
  bool cancelled = false;
  BackgroundMemoryReclamationCounters counters;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool ok() const {
    return status.ok() && !fail_closed && counters.failed_count == 0;
  }
};

const char* BackgroundMemoryReclamationWorkKindName(
    BackgroundMemoryReclamationWorkKind kind);
BackgroundMemoryReclamationResult RunBackgroundMemoryReclamation(
    const BackgroundMemoryReclamationPolicy& policy,
    const BackgroundMemoryReclamationRequest& request);
BackgroundMemoryReclamationResult RunBackgroundMemoryReclamationWithReservedResource(
    const BackgroundMemoryReclamationPolicy& policy,
    const BackgroundMemoryReclamationRequest& request,
    ReservationBackedMemoryResource* resource,
    u64 scratch_bytes);

}  // namespace scratchbird::core::memory
