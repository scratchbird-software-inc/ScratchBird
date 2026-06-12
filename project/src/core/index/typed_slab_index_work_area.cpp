// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-013 index adapter for typed slab hot work areas.
#include "typed_slab_index_work_area.hpp"

#include "runtime_platform.hpp"

#include <array>
#include <utility>

namespace scratchbird::core::index {
namespace {

using scratchbird::core::memory::TypedSlabPool;
using scratchbird::core::memory::TypedSlabPoolObjectKind;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityScope =
    "index.typed_slab.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_plan_or_index_finality_authority";

struct IndexCursor {
  u64 cursor_id = 0;
  u64 page_id = 0;
  u64 ordinal = 0;
};

struct CandidateChunk {
  u64 chunk_id = 0;
  u64 row_count = 0;
  std::array<u64, 12> row_ordinals{};
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

IndexTypedSlabWorkAreaResult Refuse(IndexTypedSlabWorkAreaRequest request,
                                    std::string code,
                                    std::string message,
                                    std::string reason) {
  IndexTypedSlabWorkAreaResult result;
  result.status = {StatusCode::memory_invalid_request, Severity::error, Subsystem::engine};
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(result.status.code,
                                     result.status.severity,
                                     result.status.subsystem,
                                     std::move(code),
                                     std::move(message),
                                     {{"route_label", request.route_label},
                                      {"reason", std::move(reason)}},
                                     {},
                                     "core.index.typed_slab_work_area",
                                     "index typed slabs require a reservation-backed CEIC-013 size-class allocator");
  result.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS");
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("index.typed_slab.fail_closed=true");
  return result;
}

}  // namespace

IndexTypedSlabWorkAreaResult BuildIndexTypedSlabWorkArea(
    IndexTypedSlabWorkAreaRequest request) {
  if (request.allocator == nullptr) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_INDEX_TYPED_SLAB.ALLOCATOR_REQUIRED",
                  "index.ceic_013.typed_slab.allocator_required",
                  "allocator_required");
  }
  if (request.route_label.empty() || request.cursor_count == 0) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_INDEX_TYPED_SLAB.ROUTE_REQUIRED",
                  "index.ceic_013.typed_slab.route_required",
                  "route_and_cursor_count_required");
  }
  if (!request.engine_mga_authoritative ||
      !request.exact_recheck_required ||
      request.parser_or_reference_authority ||
      request.memory_index_finality_authority) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_INDEX_TYPED_SLAB.UNSAFE_AUTHORITY",
                  "index.ceic_013.typed_slab.unsafe_authority",
                  "mga_exact_recheck_and_non_authority_evidence_required");
  }

  TypedSlabPool<IndexCursor> cursors(
      request.allocator,
      TypedSlabPoolObjectKind::index_cursor,
      "index_cursor");
  TypedSlabPool<CandidateChunk> chunks(
      request.allocator,
      TypedSlabPoolObjectKind::candidate_chunk,
      "index_candidate_chunk");

  IndexTypedSlabWorkAreaResult result;
  result.status = OkStatus();
  result.cursor_count = request.cursor_count;
  for (u64 i = 0; i < request.cursor_count; ++i) {
    auto cursor = cursors.Make(i, 900 + i, i * 4);
    if (!cursor.ok()) {
      result.status = cursor.status;
      result.fail_closed = true;
      result.diagnostic = cursor.diagnostic;
      result.evidence = cursor.evidence;
      return result;
    }
    ++result.typed_object_count;
  }
  auto chunk = chunks.Make(1, request.cursor_count);
  if (!chunk.ok()) {
    result.status = chunk.status;
    result.fail_closed = true;
    result.diagnostic = chunk.diagnostic;
    result.evidence = chunk.evidence;
    return result;
  }
  ++result.typed_object_count;

  result.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS");
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("index.typed_slab.route_label=" + request.route_label);
  result.evidence.push_back("index.typed_slab.index_cursor=true");
  result.evidence.push_back("index.typed_slab.candidate_chunk=true");
  result.evidence.push_back("index.typed_slab.exact_recheck_required=true");
  return result;
}

}  // namespace scratchbird::core::index
