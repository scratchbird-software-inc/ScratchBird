// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-013 executor adapter for typed slab hot work areas.
#include "typed_slab_executor_work_area.hpp"

#include "runtime_platform.hpp"

#include <array>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::memory::TypedSlabPool;
using scratchbird::core::memory::TypedSlabPoolObjectKind;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr const char* kAuthorityScope =
    "executor.typed_slab.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_donor_benchmark_cluster_optimizer_plan_or_index_finality_authority";

struct ExecutorFrame {
  u64 frame_id = 0;
  u64 operator_id = 0;
  std::array<u64, 4> registers{};
};

struct RowBatch {
  u64 batch_id = 0;
  u64 row_count = 0;
  std::array<u64, 12> row_offsets{};
};

struct RowLocator {
  u64 page_id = 0;
  u64 slot = 0;
};

struct VectorScratch {
  std::array<double, 16> lanes{};
};

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

ExecutorTypedSlabWorkAreaResult Refuse(ExecutorTypedSlabWorkAreaRequest request,
                                       std::string code,
                                       std::string message,
                                       std::string reason) {
  ExecutorTypedSlabWorkAreaResult result;
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
                                     "engine.executor.typed_slab_work_area",
                                     "executor typed slabs require a reservation-backed CEIC-013 size-class allocator");
  result.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS");
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("executor.typed_slab.fail_closed=true");
  return result;
}

bool UnsafeAuthority(const ExecutorTypedSlabWorkAreaRequest& request) {
  return !request.engine_mga_snapshot_bound ||
         !request.transaction_inventory_authoritative ||
         request.parser_or_donor_authority ||
         request.memory_finality_or_visibility_authority;
}

}  // namespace

ExecutorTypedSlabWorkAreaResult BuildExecutorTypedSlabWorkArea(
    ExecutorTypedSlabWorkAreaRequest request) {
  if (request.allocator == nullptr) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_EXECUTOR_TYPED_SLAB.ALLOCATOR_REQUIRED",
                  "executor.ceic_013.typed_slab.allocator_required",
                  "allocator_required");
  }
  if (request.route_label.empty() || request.frame_count == 0) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_EXECUTOR_TYPED_SLAB.ROUTE_REQUIRED",
                  "executor.ceic_013.typed_slab.route_required",
                  "route_and_frame_count_required");
  }
  if (UnsafeAuthority(request)) {
    return Refuse(std::move(request),
                  "SB_CEIC_013_EXECUTOR_TYPED_SLAB.UNSAFE_AUTHORITY",
                  "executor.ceic_013.typed_slab.unsafe_authority",
                  "mga_inventory_and_non_authority_evidence_required");
  }

  TypedSlabPool<ExecutorFrame> frames(
      request.allocator,
      TypedSlabPoolObjectKind::executor_frame,
      "executor_frame");
  TypedSlabPool<RowBatch> batches(
      request.allocator,
      TypedSlabPoolObjectKind::row_batch,
      "executor_row_batch");
  TypedSlabPool<RowLocator> locators(
      request.allocator,
      TypedSlabPoolObjectKind::row_locator,
      "executor_row_locator");
  TypedSlabPool<VectorScratch> vectors(
      request.allocator,
      TypedSlabPoolObjectKind::vector_scratch,
      "executor_vector_scratch");

  ExecutorTypedSlabWorkAreaResult result;
  result.status = OkStatus();
  result.frame_count = request.frame_count;
  for (u64 i = 0; i < request.frame_count; ++i) {
    auto frame = frames.Make(i, i + 100);
    if (!frame.ok()) {
      result.status = frame.status;
      result.fail_closed = true;
      result.diagnostic = frame.diagnostic;
      result.evidence = frame.evidence;
      return result;
    }
    ++result.typed_object_count;
  }
  auto batch = batches.Make(7, request.frame_count);
  auto locator = locators.Make(42, 3);
  auto vector = vectors.Make();
  if (!batch.ok()) {
    result.status = batch.status;
    result.fail_closed = true;
    result.diagnostic = batch.diagnostic;
    result.evidence = batch.evidence;
    return result;
  }
  if (!locator.ok()) {
    result.status = locator.status;
    result.fail_closed = true;
    result.diagnostic = locator.diagnostic;
    result.evidence = locator.evidence;
    return result;
  }
  if (!vector.ok()) {
    result.status = vector.status;
    result.fail_closed = true;
    result.diagnostic = vector.diagnostic;
    result.evidence = vector.evidence;
    return result;
  }
  result.typed_object_count += 3;

  auto before_reuse = request.allocator->Snapshot().reuse_count;
  (void)locators.Free(locator.pointer);
  auto reused_locator = locators.Make(43, 4);
  if (!reused_locator.ok()) {
    result.status = reused_locator.status;
    result.fail_closed = true;
    result.diagnostic = reused_locator.diagnostic;
    result.evidence = reused_locator.evidence;
    return result;
  }
  result.reuse_count = request.allocator->Snapshot().reuse_count - before_reuse;

  result.evidence.push_back("CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS");
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("executor.typed_slab.route_label=" + request.route_label);
  result.evidence.push_back("executor.typed_slab.executor_frame=true");
  result.evidence.push_back("executor.typed_slab.row_batch=true");
  result.evidence.push_back("executor.typed_slab.row_locator=true");
  result.evidence.push_back("executor.typed_slab.vector_scratch=true");
  result.evidence.push_back("executor.typed_slab.reuse_count=" +
                            std::to_string(result.reuse_count));
  return result;
}

}  // namespace scratchbird::engine::executor
