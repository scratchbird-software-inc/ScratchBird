// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "query_memory_arena.hpp"
#include "query_memory_arena_executor.hpp"
#include "resource_governance_admission.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

enum class AllocatorHotPathObjectKind {
  kRowBatch,
  kPlanNode,
  kEvidenceObject,
  kCursorFrame,
  kResultFrame,
  kDmlLocatorStream,
};

struct AllocatorHotPathObjectRequest {
  AllocatorHotPathObjectKind kind = AllocatorHotPathObjectKind::kRowBatch;
  std::uint64_t bytes = 0;
  std::string stable_id;
};

struct AllocatorHotPathAuthorityContext {
  bool engine_mga_snapshot_bound = false;
  bool transaction_inventory_authoritative = false;
  bool security_recheck_required = false;
  bool parser_client_or_donor_allocator_authority = false;
  bool allocator_visibility_or_finality_authority = false;
  bool allocator_recovery_authority = false;
  bool allocator_authorization_authority = false;
};

struct AllocatorHotPathProfilerEvidence {
  std::string source_label;
  bool measured = false;
  std::uint64_t sample_count = 0;
  std::uint64_t baseline_allocation_count = 0;
  std::uint64_t arena_allocation_count = 0;
  std::uint64_t baseline_allocation_bytes = 0;
  std::uint64_t arena_allocation_bytes = 0;
};

struct AllocatorHotPathRequest {
  std::string route_label;
  std::string result_contract_hash;
  std::string fallback_result_contract_hash;
  std::uint64_t arena_generation = 0;
  std::uint64_t expected_arena_generation = 0;
  std::uint64_t route_epoch = 0;
  std::uint64_t owner_route_epoch = 0;
  bool runtime_consumed = false;
  bool exact_fallback_available = false;
  bool cross_route_ownership_transfer = false;
  bool use_after_scope_observed = false;
  scratchbird::core::memory::QueryMemoryArena* arena = nullptr;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest
      resource_governance;
  std::vector<AllocatorHotPathObjectRequest> objects;
  AllocatorHotPathAuthorityContext authority;
  AllocatorHotPathProfilerEvidence profiler;
};

struct AllocatorHotPathResult {
  bool ok = false;
  bool benchmark_clean = false;
  bool fallback_used = false;
  bool fail_closed = false;
  std::string diagnostic_code;
  std::string detail;
  std::uint64_t allocation_count_saved = 0;
  std::uint64_t allocation_bytes_saved = 0;
  std::vector<std::string> grant_ids;
  std::vector<std::string> evidence;
};

const char* AllocatorHotPathObjectKindName(AllocatorHotPathObjectKind kind);

AllocatorHotPathResult ExecuteAllocatorHotPath(
    const AllocatorHotPathRequest& request);

}  // namespace scratchbird::engine::executor
