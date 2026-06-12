// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// CEIC-013: reservation-backed typed slab pools and size-class allocators.
#include "typed_slab_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace scratchbird::core::memory {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::MakeDiagnostic;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

constexpr std::uint64_t kSlotMagic = 0x5342434549433031ull;
constexpr std::uint64_t kTailCanaryXor = 0x9e3779b97f4a7c15ull;
constexpr std::uint32_t kSlotStateFree = 1;
constexpr std::uint32_t kSlotStateAllocated = 2;
constexpr std::uint32_t kSlotStateQuarantined = 3;
constexpr unsigned char kFreePoison = 0xdd;
constexpr unsigned char kAllocatedPoison = 0xa5;
constexpr usize kPayloadAlignment = alignof(std::max_align_t);
constexpr usize kDefaultSlotsPerSlab = 64;
constexpr usize kMaxLatencySamples = 2048;
constexpr const char* kAnchor = "CEIC-013_TYPED_SLAB_POOLS_SIZE_CLASS_ALLOCATORS";
constexpr const char* kAuthorityScope =
    "typed_slab_pool.authority_scope=evidence_only_not_transaction_finality_visibility_recovery_parser_reference_benchmark_cluster_optimizer_plan_index_finality_or_agent_authority";

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::memory};
}

Status ErrorStatus(StatusCode code) {
  return {code, Severity::error, Subsystem::memory};
}

bool Blank(const std::string& value) {
  return value.find_first_not_of(" \t\r\n") == std::string::npos;
}

usize AlignUp(usize value, usize alignment) {
  if (alignment == 0) {
    return value;
  }
  const usize remainder = value % alignment;
  if (remainder == 0) {
    return value;
  }
  return value + (alignment - remainder);
}

bool AddWouldOverflow(usize left, usize right) {
  return right > std::numeric_limits<usize>::max() - left;
}

bool MultiplyWouldOverflow(usize left, usize right) {
  return left != 0 && right > std::numeric_limits<usize>::max() / left;
}

std::string BoolText(bool value) {
  return value ? "true" : "false";
}

bool UnsafeAuthority(const SizeClassAllocatorRequest& request,
                     std::string* reason) {
  const auto& authority = request.authority;
  if (!authority.engine_mga_authoritative) {
    *reason = "engine_mga_authority_required";
    return true;
  }
  if (request.production_like && !authority.transaction_inventory_authoritative) {
    *reason = "transaction_inventory_authority_required";
    return true;
  }
  if (!authority.security_or_policy_checked) {
    *reason = "security_or_policy_check_required";
    return true;
  }
  if (authority.parser_or_reference_finality_authority ||
      authority.memory_visibility_or_finality_authority ||
      authority.memory_recovery_authority ||
      authority.memory_authorization_authority ||
      authority.benchmark_authority ||
      authority.cluster_authority ||
      authority.debug_or_relaxed_path ||
      authority.optimizer_plan_authority ||
      authority.index_finality_authority ||
      authority.agent_action_authority) {
    *reason = "unsafe_authority_claim_refused";
    return true;
  }
  return false;
}

DiagnosticRecord MakePoolDiagnostic(Status status,
                                    std::string diagnostic_code,
                                    std::string message_key,
                                    const SizeClassAllocatorRequest& request,
                                    std::vector<DiagnosticArgument> arguments = {}) {
  arguments.push_back({"slice", kAnchor});
  arguments.push_back({"authority_scope", kAuthorityScope});
  arguments.push_back({"owner_id", request.owner_id});
  arguments.push_back({"route_label", request.route_label});
  arguments.push_back({"operation_id", request.operation_id});
  arguments.push_back({"memory_class", request.memory_class});
  arguments.push_back({"category", MemoryCategoryName(request.category)});
  arguments.push_back({"object_kind", TypedSlabPoolObjectKindName(request.object_kind)});
  arguments.push_back({"reservation_bytes", std::to_string(request.reservation_bytes)});
  return MakeDiagnostic(status.code,
                        status.severity,
                        status.subsystem,
                        std::move(diagnostic_code),
                        std::move(message_key),
                        std::move(arguments),
                        {},
                        "core.memory.typed_slab_pool",
                        "Create typed slab pools only from CEIC-011/012 reservation-backed memory resources.");
}

SizeClassAllocatorAcquireResult RefuseAcquire(SizeClassAllocatorRequest request,
                                              std::string diagnostic_code,
                                              std::string message_key,
                                              std::string reason,
                                              StatusCode code = StatusCode::memory_invalid_request) {
  SizeClassAllocatorAcquireResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakePoolDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      request,
      {{"reason", std::move(reason)}});
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("typed_slab_pool.fail_closed=true");
  result.evidence.push_back("typed_slab_pool.reservation_created=false");
  return result;
}

u64 BasisPoints(u64 numerator, u64 denominator) {
  if (denominator == 0 || numerator == 0) {
    return 0;
  }
  if (numerator > std::numeric_limits<u64>::max() / 10000ull) {
    return 10000;
  }
  return std::min<u64>(10000, (numerator * 10000ull) / denominator);
}

u64 Percentile(std::vector<u64> values, unsigned percentile) {
  if (values.empty()) {
    return 0;
  }
  std::sort(values.begin(), values.end());
  const usize index = static_cast<usize>(
      ((static_cast<unsigned long long>(values.size() - 1) * percentile) + 99ull) / 100ull);
  return values[std::min(index, values.size() - 1)];
}

}  // namespace

const char* TypedSlabPoolObjectKindName(TypedSlabPoolObjectKind kind) {
  switch (kind) {
    case TypedSlabPoolObjectKind::planner_node: return "planner_node";
    case TypedSlabPoolObjectKind::plan_node: return "plan_node";
    case TypedSlabPoolObjectKind::expression_node: return "expression_node";
    case TypedSlabPoolObjectKind::predicate_node: return "predicate_node";
    case TypedSlabPoolObjectKind::executor_frame: return "executor_frame";
    case TypedSlabPoolObjectKind::row_batch: return "row_batch";
    case TypedSlabPoolObjectKind::row_locator: return "row_locator";
    case TypedSlabPoolObjectKind::index_cursor: return "index_cursor";
    case TypedSlabPoolObjectKind::candidate_chunk: return "candidate_chunk";
    case TypedSlabPoolObjectKind::candidate_set: return "candidate_set";
    case TypedSlabPoolObjectKind::posting_list_chunk: return "posting_list_chunk";
    case TypedSlabPoolObjectKind::hash_bucket: return "hash_bucket";
    case TypedSlabPoolObjectKind::sort_descriptor: return "sort_descriptor";
    case TypedSlabPoolObjectKind::vector_scratch: return "vector_scratch";
    case TypedSlabPoolObjectKind::diagnostic_record: return "diagnostic_record";
    case TypedSlabPoolObjectKind::metric_label: return "metric_label";
    case TypedSlabPoolObjectKind::page_cache_metadata:
      return "page_cache_metadata";
  }
  return "unknown";
}

MemoryCategory TypedSlabPoolDefaultCategory(TypedSlabPoolObjectKind kind) {
  switch (kind) {
    case TypedSlabPoolObjectKind::diagnostic_record:
    case TypedSlabPoolObjectKind::metric_label:
      return MemoryCategory::diagnostics;
    case TypedSlabPoolObjectKind::planner_node:
    case TypedSlabPoolObjectKind::plan_node:
    case TypedSlabPoolObjectKind::expression_node:
    case TypedSlabPoolObjectKind::predicate_node:
    case TypedSlabPoolObjectKind::executor_frame:
    case TypedSlabPoolObjectKind::row_batch:
    case TypedSlabPoolObjectKind::row_locator:
    case TypedSlabPoolObjectKind::index_cursor:
    case TypedSlabPoolObjectKind::candidate_chunk:
    case TypedSlabPoolObjectKind::candidate_set:
    case TypedSlabPoolObjectKind::posting_list_chunk:
    case TypedSlabPoolObjectKind::hash_bucket:
    case TypedSlabPoolObjectKind::sort_descriptor:
    case TypedSlabPoolObjectKind::vector_scratch:
      return MemoryCategory::executor_query_reserved;
    case TypedSlabPoolObjectKind::page_cache_metadata:
      return MemoryCategory::page_buffer;
  }
  return MemoryCategory::executor_query_reserved;
}

ReservationBackedMemoryConsumerKind TypedSlabPoolConsumerKind(
    TypedSlabPoolObjectKind kind) {
  switch (kind) {
    case TypedSlabPoolObjectKind::planner_node:
    case TypedSlabPoolObjectKind::plan_node:
    case TypedSlabPoolObjectKind::expression_node:
    case TypedSlabPoolObjectKind::predicate_node:
      return ReservationBackedMemoryConsumerKind::planner_temporary;
    case TypedSlabPoolObjectKind::diagnostic_record:
    case TypedSlabPoolObjectKind::metric_label:
      return ReservationBackedMemoryConsumerKind::background_maintenance;
    case TypedSlabPoolObjectKind::page_cache_metadata:
      return ReservationBackedMemoryConsumerKind::background_maintenance;
    case TypedSlabPoolObjectKind::executor_frame:
    case TypedSlabPoolObjectKind::row_batch:
    case TypedSlabPoolObjectKind::row_locator:
    case TypedSlabPoolObjectKind::index_cursor:
    case TypedSlabPoolObjectKind::candidate_chunk:
    case TypedSlabPoolObjectKind::candidate_set:
    case TypedSlabPoolObjectKind::posting_list_chunk:
    case TypedSlabPoolObjectKind::hash_bucket:
    case TypedSlabPoolObjectKind::sort_descriptor:
    case TypedSlabPoolObjectKind::vector_scratch:
      return ReservationBackedMemoryConsumerKind::executor_operator;
  }
  return ReservationBackedMemoryConsumerKind::executor_operator;
}

std::vector<SizeClassConfig> DefaultTypedSlabSizeClasses() {
  return {{64, kDefaultSlotsPerSlab},
          {128, kDefaultSlotsPerSlab},
          {256, 32},
          {512, 16},
          {1024, 8}};
}

SizeClassAllocator::SizeClassAllocator(
    SizeClassAllocatorRequest request,
    std::unique_ptr<ReservationBackedMemoryResource> resource)
    : request_(std::move(request)), resource_(std::move(resource)) {
  if (request_.category == MemoryCategory::unknown) {
    request_.category = TypedSlabPoolDefaultCategory(request_.object_kind);
  }
  classes_.reserve(request_.size_classes.size());
  for (const auto& config : request_.size_classes) {
    SizeClassState state;
    state.config = config;
    const usize payload_offset = AlignUp(sizeof(SlotHeader), kPayloadAlignment);
    const usize tail_offset = payload_offset + config.payload_bytes;
    state.slot_stride_bytes =
        AlignUp(tail_offset + sizeof(std::uint64_t), kPayloadAlignment);
    classes_.push_back(std::move(state));
  }
}

SizeClassAllocator::~SizeClassAllocator() {
  (void)Release();
}

void SizeClassAllocator::AppendBaseEvidence(std::vector<std::string>* evidence) const {
  evidence->push_back(kAnchor);
  evidence->push_back(kAuthorityScope);
  evidence->push_back("typed_slab_pool.owner_id=" + request_.owner_id);
  evidence->push_back("typed_slab_pool.route_label=" + request_.route_label);
  evidence->push_back("typed_slab_pool.operation_id=" + request_.operation_id);
  evidence->push_back("typed_slab_pool.memory_class=" + request_.memory_class);
  evidence->push_back("typed_slab_pool.category=" +
                      std::string(MemoryCategoryName(request_.category)));
  evidence->push_back("typed_slab_pool.object_kind=" +
                      std::string(TypedSlabPoolObjectKindName(request_.object_kind)));
}

SizeClassAllocationResult SizeClassAllocator::RefuseAllocation(
    SizeClassAllocationRequest request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code) {
  SizeClassAllocationResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.requested_bytes = request.bytes;
  result.diagnostic = MakePoolDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      request_,
      {{"reason", std::move(reason)},
       {"requested_bytes", std::to_string(request.bytes)},
       {"alignment", std::to_string(request.alignment)}});
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("typed_slab_pool.allocation.fail_closed=true");
  return result;
}

SizeClassFreeResult SizeClassAllocator::RefuseFree(
    std::string diagnostic_code,
    std::string message_key,
    std::string reason,
    StatusCode code) const {
  SizeClassFreeResult result;
  result.status = ErrorStatus(code);
  result.fail_closed = true;
  result.diagnostic = MakePoolDiagnostic(
      result.status,
      std::move(diagnostic_code),
      std::move(message_key),
      request_,
      {{"reason", std::move(reason)}});
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("typed_slab_pool.free.fail_closed=true");
  return result;
}

usize SizeClassAllocator::FindSizeClass(usize bytes) const {
  for (usize i = 0; i < classes_.size(); ++i) {
    if (bytes <= classes_[i].config.payload_bytes) {
      return i;
    }
  }
  return classes_.size();
}

bool SizeClassAllocator::EnsureSlab(usize class_index,
                                    SizeClassAllocationResult* result) {
  SizeClassState& klass = classes_[class_index];
  for (const auto& slab : klass.slabs) {
    if (!slab.free_slots.empty()) {
      return true;
    }
  }

  if (klass.config.slots_per_slab == 0 ||
      MultiplyWouldOverflow(klass.slot_stride_bytes, klass.config.slots_per_slab)) {
    ++klass.allocation_refusal_count;
    *result = RefuseAllocation(
        {klass.config.payload_bytes, kPayloadAlignment, "slab"},
        "SB_CEIC_013_TYPED_SLAB_POOL.SLAB_SIZE_INVALID",
        "memory.ceic_013.typed_slab_pool.slab_size_invalid",
        "slot_stride_or_slots_per_slab_invalid",
        StatusCode::memory_invalid_request);
    return false;
  }

  const usize slab_bytes = klass.slot_stride_bytes * klass.config.slots_per_slab;
  const auto snapshot = SnapshotLocked();
  if (request_.reservation_bytes != 0 &&
      (slab_bytes > request_.reservation_bytes ||
       snapshot.retained_bytes > request_.reservation_bytes - slab_bytes)) {
    ++klass.allocation_refusal_count;
    *result = RefuseAllocation(
        {klass.config.payload_bytes, kPayloadAlignment, "slab"},
        "SB_CEIC_013_TYPED_SLAB_POOL.RESERVATION_EXHAUSTED",
        "memory.ceic_013.typed_slab_pool.reservation_exhausted",
        "pool_retained_bytes_would_exceed_reservation",
        StatusCode::memory_limit_exceeded);
    return false;
  }

  ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = slab_bytes;
  allocation.alignment = kPayloadAlignment;
  allocation.purpose = request_.purpose + ".slab." +
                       std::to_string(klass.config.payload_bytes);
  auto allocated = resource_->Allocate(std::move(allocation));
  if (!allocated.ok()) {
    ++klass.allocation_refusal_count;
    result->status = allocated.status;
    result->fail_closed = true;
    result->diagnostic = allocated.diagnostic;
    AppendBaseEvidence(&result->evidence);
    result->evidence.push_back("typed_slab_pool.slab_allocation_refused=true");
    return false;
  }

  Slab slab;
  slab.pointer = allocated.pointer;
  slab.bytes = allocated.bytes;
  slab.class_index = class_index;
  slab.slot_count = klass.config.slots_per_slab;
  slab.free_slots.reserve(slab.slot_count);
  const usize slab_index = klass.slabs.size();
  klass.slabs.push_back(std::move(slab));
  InitializeSlabSlots(&klass.slabs.back());
  ++klass.slab_allocation_count;
  return true;
}

void SizeClassAllocator::InitializeSlabSlots(Slab* slab) {
  SizeClassState& klass = classes_[slab->class_index];
  slab->free_slots.clear();
  slab->active_slots = 0;
  slab->quarantined_slots = 0;
  for (usize slot = 0; slot < slab->slot_count; ++slot) {
    SlotRef ref{slab->class_index, 0, slot};
    ref.slab_index = klass.slabs.empty() ? 0 : klass.slabs.size() - 1;
    auto* header = HeaderFor(ref);
    auto* payload = PayloadFor(ref);
    header->magic = kSlotMagic;
    header->class_index = static_cast<std::uint32_t>(slab->class_index);
    header->slab_index = static_cast<std::uint32_t>(ref.slab_index);
    header->slot_index = static_cast<std::uint32_t>(slot);
    header->state = kSlotStateFree;
    header->requested_bytes = 0;
    header->payload_bytes = klass.config.payload_bytes;
    header->allocation_id = 0;
    header->canary = SlotCanary(*header, payload);
    WriteTailCanary(ref, header->canary ^ kTailCanaryXor);
    std::memset(payload, kFreePoison, klass.config.payload_bytes);
    slab->free_slots.push_back(slot);
  }
}

SizeClassAllocationResult SizeClassAllocator::Allocate(
    SizeClassAllocationRequest request) {
  const auto start = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  if (released_ || resource_ == nullptr || !resource_->active()) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_013_TYPED_SLAB_POOL.RELEASED",
                            "memory.ceic_013.typed_slab_pool.released",
                            "pool_or_reservation_resource_released",
                            StatusCode::memory_invalid_request);
  }
  if (request.bytes == 0) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_013_TYPED_SLAB_POOL.ZERO_SIZE",
                            "memory.ceic_013.typed_slab_pool.zero_size",
                            "requested_bytes_required",
                            StatusCode::memory_invalid_request);
  }
  if (request.alignment == 0) {
    request.alignment = kPayloadAlignment;
  }
  if (request.alignment > kPayloadAlignment || !IsPowerOfTwo(request.alignment)) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_013_TYPED_SLAB_POOL.ALIGNMENT_UNSUPPORTED",
                            "memory.ceic_013.typed_slab_pool.alignment_unsupported",
                            "alignment_exceeds_size_class_payload_alignment",
                            StatusCode::memory_invalid_request);
  }

  const usize class_index = FindSizeClass(request.bytes);
  if (class_index == classes_.size()) {
    return RefuseAllocation(std::move(request),
                            "SB_CEIC_013_TYPED_SLAB_POOL.SIZE_CLASS_UNAVAILABLE",
                            "memory.ceic_013.typed_slab_pool.size_class_unavailable",
                            "no_configured_size_class_can_hold_request",
                            StatusCode::memory_limit_exceeded);
  }
  SizeClassAllocationResult result;
  if (!EnsureSlab(class_index, &result)) {
    return result;
  }
  result = AllocateFromClass(class_index, std::move(request));
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count();
  RecordLatency(static_cast<u64>(std::max<long long>(0, elapsed)));
  return result;
}

SizeClassAllocationResult SizeClassAllocator::AllocateFromClass(
    usize class_index,
    SizeClassAllocationRequest request) {
  SizeClassState& klass = classes_[class_index];
  for (usize slab_index = 0; slab_index < klass.slabs.size(); ++slab_index) {
    Slab& slab = klass.slabs[slab_index];
    if (slab.free_slots.empty()) {
      continue;
    }
    const usize slot_index = slab.free_slots.back();
    slab.free_slots.pop_back();
    SlotRef ref{class_index, slab_index, slot_index};
    auto* header = HeaderFor(ref);
    auto* payload = PayloadFor(ref);
    std::string reason;
    if (header->magic != kSlotMagic ||
        header->state != kSlotStateFree ||
        !ValidateSlotCanaries(ref, &reason)) {
      ++klass.corruption_refusal_count;
      ++slab.quarantined_slots;
      header->state = kSlotStateQuarantined;
      return RefuseAllocation(
          std::move(request),
          "SB_CEIC_013_TYPED_SLAB_POOL.FREE_SLOT_CORRUPT",
          "memory.ceic_013.typed_slab_pool.free_slot_corrupt",
          reason.empty() ? "free_slot_canary_or_state_corrupt" : reason,
          StatusCode::memory_unknown_pointer);
    }

    const bool reused = header->generation != 0;
    header->state = kSlotStateAllocated;
    header->requested_bytes = request.bytes;
    header->payload_bytes = klass.config.payload_bytes;
    header->allocation_id = ++allocation_sequence_;
    ++header->generation;
    header->canary = SlotCanary(*header, payload);
    WriteTailCanary(ref, header->canary ^ kTailCanaryXor);
    std::memset(payload, kAllocatedPoison, klass.config.payload_bytes);

    active_[payload] = ref;
    ++slab.active_slots;
    ++klass.allocation_count;
    if (reused) {
      ++klass.reuse_count;
    }
    klass.active_requested_bytes += request.bytes;
    klass.active_size_class_bytes += klass.config.payload_bytes;
    klass.internal_fragmentation_bytes += klass.config.payload_bytes - request.bytes;
    u64 class_active_slots = 0;
    for (const auto& observed_slab : klass.slabs) {
      class_active_slots += observed_slab.active_slots;
    }
    klass.high_watermark_active_slots =
        std::max(klass.high_watermark_active_slots, class_active_slots);

    SizeClassAllocationResult result;
    result.status = OkStatus();
    result.pointer = payload;
    result.requested_bytes = request.bytes;
    result.size_class_bytes = klass.config.payload_bytes;
    result.allocation_id = header->allocation_id;
    result.reused = reused;
    AppendBaseEvidence(&result.evidence);
    result.evidence.push_back("typed_slab_pool.allocation.ok=true");
    result.evidence.push_back("typed_slab_pool.allocation_id=" +
                              std::to_string(result.allocation_id));
    result.evidence.push_back("typed_slab_pool.requested_bytes=" +
                              std::to_string(result.requested_bytes));
    result.evidence.push_back("typed_slab_pool.size_class_bytes=" +
                              std::to_string(result.size_class_bytes));
    result.evidence.push_back("typed_slab_pool.reused=" + BoolText(result.reused));
    return result;
  }
  ++klass.allocation_refusal_count;
  return RefuseAllocation(std::move(request),
                          "SB_CEIC_013_TYPED_SLAB_POOL.NO_FREE_SLOT",
                          "memory.ceic_013.typed_slab_pool.no_free_slot",
                          "slab_created_without_free_slot",
                          StatusCode::memory_allocation_failed);
}

SizeClassFreeResult SizeClassAllocator::Free(void* pointer) {
  if (pointer == nullptr) {
    SizeClassFreeResult result;
    result.status = OkStatus();
    result.freed = true;
    return result;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (released_) {
    return RefuseFree("SB_CEIC_013_TYPED_SLAB_POOL.FREE_AFTER_RELEASE",
                      "memory.ceic_013.typed_slab_pool.free_after_release",
                      "free_after_release_refused",
                      StatusCode::memory_unknown_pointer);
  }
  auto it = active_.find(pointer);
  if (it == active_.end()) {
    return RefuseFree("SB_CEIC_013_TYPED_SLAB_POOL.UNKNOWN_POINTER",
                      "memory.ceic_013.typed_slab_pool.unknown_pointer",
                      "pointer_not_owned_by_pool",
                      StatusCode::memory_unknown_pointer);
  }
  SlotRef ref = it->second;
  active_.erase(it);
  return FreeValidated(pointer, ref);
}

SizeClassFreeResult SizeClassAllocator::FreeValidated(void* pointer, SlotRef ref) {
  SizeClassState& klass = classes_[ref.class_index];
  Slab& slab = klass.slabs[ref.slab_index];
  auto* header = HeaderFor(ref);
  auto* payload = PayloadFor(ref);
  std::string reason;
  const bool corrupt =
      pointer != payload ||
      header->magic != kSlotMagic ||
      header->state != kSlotStateAllocated ||
      !ValidateSlotCanaries(ref, &reason);

  if (slab.active_slots != 0) {
    --slab.active_slots;
  }
  if (klass.active_requested_bytes >= header->requested_bytes) {
    klass.active_requested_bytes -= header->requested_bytes;
  } else {
    klass.active_requested_bytes = 0;
  }
  if (klass.active_size_class_bytes >= klass.config.payload_bytes) {
    klass.active_size_class_bytes -= klass.config.payload_bytes;
  } else {
    klass.active_size_class_bytes = 0;
  }
  const u64 fragmentation = klass.config.payload_bytes >= header->requested_bytes
                                ? klass.config.payload_bytes - header->requested_bytes
                                : 0;
  if (klass.internal_fragmentation_bytes >= fragmentation) {
    klass.internal_fragmentation_bytes -= fragmentation;
  } else {
    klass.internal_fragmentation_bytes = 0;
  }

  if (corrupt) {
    header->state = kSlotStateQuarantined;
    ++klass.corruption_refusal_count;
    ++slab.quarantined_slots;
    SizeClassFreeResult result = RefuseFree(
        "SB_CEIC_013_TYPED_SLAB_POOL.CORRUPTION_REFUSED",
        "memory.ceic_013.typed_slab_pool.corruption_refused",
        reason.empty() ? "slot_canary_or_header_corrupt" : reason,
        StatusCode::memory_unknown_pointer);
    result.corruption_refused = true;
    result.evidence.push_back("typed_slab_pool.slot_quarantined=true");
    return result;
  }

  std::memset(payload, kFreePoison, klass.config.payload_bytes);
  header->state = kSlotStateFree;
  header->requested_bytes = 0;
  header->allocation_id = 0;
  header->canary = SlotCanary(*header, payload);
  WriteTailCanary(ref, header->canary ^ kTailCanaryXor);
  slab.free_slots.push_back(ref.slot_index);
  ++klass.deallocation_count;

  SizeClassFreeResult result;
  result.status = OkStatus();
  result.freed = true;
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("typed_slab_pool.free.ok=true");
  result.evidence.push_back("typed_slab_pool.slot_reusable=true");
  return result;
}

SizeClassFreeResult SizeClassAllocator::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (released_) {
    SizeClassFreeResult result;
    result.status = OkStatus();
    result.freed = true;
    return result;
  }
  active_.clear();
  for (usize class_index = 0; class_index < classes_.size(); ++class_index) {
    SizeClassState& klass = classes_[class_index];
    klass.active_requested_bytes = 0;
    klass.active_size_class_bytes = 0;
    klass.internal_fragmentation_bytes = 0;
    for (usize slab_index = 0; slab_index < klass.slabs.size(); ++slab_index) {
      Slab& slab = klass.slabs[slab_index];
      slab.free_slots.clear();
      slab.active_slots = 0;
      slab.quarantined_slots = 0;
      for (usize slot = 0; slot < slab.slot_count; ++slot) {
        SlotRef ref{class_index, slab_index, slot};
        auto* header = HeaderFor(ref);
        auto* payload = PayloadFor(ref);
        header->magic = kSlotMagic;
        header->class_index = static_cast<std::uint32_t>(class_index);
        header->slab_index = static_cast<std::uint32_t>(slab_index);
        header->slot_index = static_cast<std::uint32_t>(slot);
        header->state = kSlotStateFree;
        header->requested_bytes = 0;
        header->payload_bytes = klass.config.payload_bytes;
        header->allocation_id = 0;
        header->canary = SlotCanary(*header, payload);
        WriteTailCanary(ref, header->canary ^ kTailCanaryXor);
        std::memset(payload, kFreePoison, klass.config.payload_bytes);
        slab.free_slots.push_back(slot);
      }
    }
  }
  ++reset_count_;

  SizeClassFreeResult result;
  result.status = OkStatus();
  result.freed = true;
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("typed_slab_pool.reset.ok=true");
  return result;
}

SizeClassFreeResult SizeClassAllocator::Release() {
  std::unique_ptr<ReservationBackedMemoryResource> resource;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (released_) {
      SizeClassFreeResult result;
      result.status = OkStatus();
      result.freed = true;
      AppendBaseEvidence(&result.evidence);
      result.evidence.push_back("typed_slab_pool.release.already_released=true");
      return result;
    }
    active_.clear();
    for (auto& klass : classes_) {
      klass.slabs.clear();
      klass.active_requested_bytes = 0;
      klass.active_size_class_bytes = 0;
      klass.internal_fragmentation_bytes = 0;
    }
    released_ = true;
    ++release_count_;
    resource = std::move(resource_);
  }

  SizeClassFreeResult result;
  result.status = OkStatus();
  result.freed = true;
  AppendBaseEvidence(&result.evidence);
  result.evidence.push_back("typed_slab_pool.release.routed=true");
  if (resource != nullptr) {
    auto released = resource->Release();
    result.status = released.status;
    result.fail_closed = released.fail_closed;
    result.freed = released.released;
    result.diagnostic = released.diagnostic;
    result.evidence.insert(result.evidence.end(),
                           released.evidence.begin(),
                           released.evidence.end());
  }
  return result;
}

bool SizeClassAllocator::ValidateSlotCanaries(const SlotRef& ref,
                                              std::string* reason) const {
  const auto* header = HeaderFor(ref);
  const auto* payload = PayloadFor(ref);
  if (header->class_index != ref.class_index ||
      header->slab_index != ref.slab_index ||
      header->slot_index != ref.slot_index ||
      header->payload_bytes != classes_[ref.class_index].config.payload_bytes) {
    *reason = "slot_header_identity_mismatch";
    return false;
  }
  const std::uint64_t expected = SlotCanary(*header, payload);
  if (header->canary != expected) {
    *reason = "slot_header_canary_mismatch";
    return false;
  }
  if (ReadTailCanary(ref) != (expected ^ kTailCanaryXor)) {
    *reason = "slot_tail_canary_mismatch";
    return false;
  }
  return true;
}

SizeClassAllocator::SlotHeader* SizeClassAllocator::HeaderFor(
    const SlotRef& ref) const {
  const SizeClassState& klass = classes_[ref.class_index];
  const Slab& slab = klass.slabs[ref.slab_index];
  return reinterpret_cast<SlotHeader*>(
      static_cast<unsigned char*>(slab.pointer) +
      (ref.slot_index * klass.slot_stride_bytes));
}

unsigned char* SizeClassAllocator::PayloadFor(const SlotRef& ref) const {
  auto* header = HeaderFor(ref);
  return reinterpret_cast<unsigned char*>(header) +
         AlignUp(sizeof(SlotHeader), kPayloadAlignment);
}

unsigned char* SizeClassAllocator::TailCanaryFor(const SlotRef& ref) const {
  return PayloadFor(ref) + classes_[ref.class_index].config.payload_bytes;
}

std::uint64_t SizeClassAllocator::ReadTailCanary(const SlotRef& ref) const {
  std::uint64_t value = 0;
  std::memcpy(&value, TailCanaryFor(ref), sizeof(value));
  return value;
}

void SizeClassAllocator::WriteTailCanary(const SlotRef& ref, std::uint64_t value) {
  std::memcpy(TailCanaryFor(ref), &value, sizeof(value));
}

std::uint64_t SizeClassAllocator::SlotCanary(const SlotHeader& header,
                                             const void* payload) const {
  return kSlotMagic ^
         (static_cast<std::uint64_t>(header.class_index) << 48u) ^
         (static_cast<std::uint64_t>(header.slab_index) << 32u) ^
         (static_cast<std::uint64_t>(header.slot_index) << 16u) ^
         header.generation ^
         reinterpret_cast<std::uintptr_t>(payload);
}

void SizeClassAllocator::RecordLatency(u64 latency_ns) {
  if (allocation_latencies_ns_.size() < kMaxLatencySamples) {
    allocation_latencies_ns_.push_back(latency_ns);
    return;
  }
  allocation_latencies_ns_[allocation_sequence_ % kMaxLatencySamples] = latency_ns;
}

SizeClassPoolSnapshot SizeClassAllocator::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

SizeClassPoolSnapshot SizeClassAllocator::SnapshotLocked() const {
  SizeClassPoolSnapshot snapshot;
  snapshot.owner_id = request_.owner_id;
  snapshot.route_label = request_.route_label;
  snapshot.operation_id = request_.operation_id;
  snapshot.memory_class = request_.memory_class;
  snapshot.category = request_.category;
  snapshot.object_kind = request_.object_kind;
  snapshot.reservation_bytes = request_.reservation_bytes;
  snapshot.reset_count = reset_count_;
  snapshot.release_count = release_count_;
  snapshot.active = !released_ && resource_ != nullptr && resource_->active();
  snapshot.authority_scope = kAuthorityScope;
  snapshot.allocation_latency_p50_ns = Percentile(allocation_latencies_ns_, 50);
  snapshot.allocation_latency_p95_ns = Percentile(allocation_latencies_ns_, 95);
  snapshot.allocation_latency_p99_ns = Percentile(allocation_latencies_ns_, 99);

  for (const auto& klass : classes_) {
    SizeClassSnapshot class_snapshot;
    class_snapshot.payload_bytes = klass.config.payload_bytes;
    class_snapshot.slot_stride_bytes = klass.slot_stride_bytes;
    class_snapshot.slots_per_slab = klass.config.slots_per_slab;
    class_snapshot.allocation_count = klass.allocation_count;
    class_snapshot.deallocation_count = klass.deallocation_count;
    class_snapshot.reuse_count = klass.reuse_count;
    class_snapshot.allocation_refusal_count = klass.allocation_refusal_count;
    class_snapshot.corruption_refusal_count = klass.corruption_refusal_count;
    class_snapshot.slab_allocation_count = klass.slab_allocation_count;
    class_snapshot.high_watermark_active_slots = klass.high_watermark_active_slots;
    class_snapshot.active_requested_bytes = klass.active_requested_bytes;
    class_snapshot.active_size_class_bytes = klass.active_size_class_bytes;
    class_snapshot.internal_fragmentation_bytes = klass.internal_fragmentation_bytes;
    for (const auto& slab : klass.slabs) {
      ++class_snapshot.slab_count;
      class_snapshot.retained_bytes += slab.bytes;
      class_snapshot.total_slots += slab.slot_count;
      class_snapshot.active_slots += slab.active_slots;
      class_snapshot.free_slots += slab.free_slots.size();
      class_snapshot.quarantined_slots += slab.quarantined_slots;
    }
    class_snapshot.reusable_payload_bytes =
        class_snapshot.free_slots * klass.config.payload_bytes;
    const u64 retained_payload_bytes =
        class_snapshot.total_slots * klass.config.payload_bytes;
    class_snapshot.occupancy_basis_points =
        BasisPoints(class_snapshot.active_slots, class_snapshot.total_slots);
    class_snapshot.fragmentation_basis_points = BasisPoints(
        class_snapshot.reusable_payload_bytes +
            class_snapshot.internal_fragmentation_bytes,
        retained_payload_bytes);

    snapshot.retained_bytes += class_snapshot.retained_bytes;
    snapshot.active_requested_bytes += class_snapshot.active_requested_bytes;
    snapshot.active_size_class_bytes += class_snapshot.active_size_class_bytes;
    snapshot.reusable_payload_bytes += class_snapshot.reusable_payload_bytes;
    snapshot.internal_fragmentation_bytes += class_snapshot.internal_fragmentation_bytes;
    snapshot.allocation_count += class_snapshot.allocation_count;
    snapshot.deallocation_count += class_snapshot.deallocation_count;
    snapshot.reuse_count += class_snapshot.reuse_count;
    snapshot.allocation_refusal_count += class_snapshot.allocation_refusal_count;
    snapshot.corruption_refusal_count += class_snapshot.corruption_refusal_count;
    snapshot.active_slots += class_snapshot.active_slots;
    snapshot.total_slots += class_snapshot.total_slots;
    snapshot.free_slots += class_snapshot.free_slots;
    snapshot.quarantined_slots += class_snapshot.quarantined_slots;
    snapshot.classes.push_back(class_snapshot);
  }
  u64 retained_payload_bytes = 0;
  for (const auto& class_snapshot : snapshot.classes) {
    retained_payload_bytes +=
        class_snapshot.total_slots * class_snapshot.payload_bytes;
  }
  snapshot.occupancy_basis_points =
      BasisPoints(snapshot.active_slots, snapshot.total_slots);
  snapshot.fragmentation_basis_points = BasisPoints(
      snapshot.reusable_payload_bytes + snapshot.internal_fragmentation_bytes,
      retained_payload_bytes);
  return snapshot;
}

SizeClassAllocatorAcquireResult CreateSizeClassAllocator(
    SizeClassAllocatorRequest request) {
  if (request.reservation_ledger == nullptr) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_013_TYPED_SLAB_POOL.RESERVATION_LEDGER_REQUIRED",
                         "memory.ceic_013.typed_slab_pool.reservation_ledger_required",
                         "hierarchical_memory_budget_ledger_required");
  }
  if (request.memory_manager == nullptr) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_013_TYPED_SLAB_POOL.MEMORY_MANAGER_REQUIRED",
                         "memory.ceic_013.typed_slab_pool.memory_manager_required",
                         "memory_manager_required");
  }
  if (request.scope_chain.empty()) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_013_TYPED_SLAB_POOL.SCOPE_CHAIN_REQUIRED",
                         "memory.ceic_013.typed_slab_pool.scope_chain_required",
                         "scope_chain_required");
  }
  if (Blank(request.owner_id) || Blank(request.route_label) ||
      Blank(request.operation_id)) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_013_TYPED_SLAB_POOL.IDENTITY_REQUIRED",
                         "memory.ceic_013.typed_slab_pool.identity_required",
                         "owner_route_and_operation_required");
  }
  if (request.reservation_bytes == 0) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_013_TYPED_SLAB_POOL.ZERO_RESERVATION",
                         "memory.ceic_013.typed_slab_pool.zero_reservation",
                         "reservation_bytes_required");
  }
  if (request.size_classes.empty()) {
    request.size_classes = DefaultTypedSlabSizeClasses();
  }
  std::sort(request.size_classes.begin(),
            request.size_classes.end(),
            [](const SizeClassConfig& left, const SizeClassConfig& right) {
              return left.payload_bytes < right.payload_bytes;
            });
  usize previous = 0;
  for (auto& config : request.size_classes) {
    if (config.payload_bytes == 0 || config.payload_bytes <= previous) {
      return RefuseAcquire(std::move(request),
                           "SB_CEIC_013_TYPED_SLAB_POOL.SIZE_CLASS_INVALID",
                           "memory.ceic_013.typed_slab_pool.size_class_invalid",
                           "size_classes_must_be_positive_and_strictly_increasing");
    }
    if (config.payload_bytes > std::numeric_limits<usize>::max() - sizeof(SizeClassAllocator::SlotHeader) ||
        AddWouldOverflow(config.payload_bytes, sizeof(std::uint64_t))) {
      return RefuseAcquire(std::move(request),
                           "SB_CEIC_013_TYPED_SLAB_POOL.SIZE_CLASS_OVERFLOW",
                           "memory.ceic_013.typed_slab_pool.size_class_overflow",
                           "size_class_stride_overflow");
    }
    if (config.slots_per_slab == 0) {
      config.slots_per_slab = kDefaultSlotsPerSlab;
    }
    previous = config.payload_bytes;
  }
  std::string unsafe_reason;
  if (UnsafeAuthority(request, &unsafe_reason)) {
    return RefuseAcquire(std::move(request),
                         "SB_CEIC_013_TYPED_SLAB_POOL.UNSAFE_AUTHORITY",
                         "memory.ceic_013.typed_slab_pool.unsafe_authority",
                         std::move(unsafe_reason));
  }
  if (request.category == MemoryCategory::unknown) {
    request.category = TypedSlabPoolDefaultCategory(request.object_kind);
  }
  if (request.memory_class.empty()) {
    request.memory_class =
        std::string("ceic_013.") + TypedSlabPoolObjectKindName(request.object_kind);
  }
  if (request.purpose.empty()) {
    request.purpose =
        std::string("ceic_013.") + TypedSlabPoolObjectKindName(request.object_kind);
  }

  ReservationBackedMemoryResourceRequest resource_request;
  resource_request.consumer_kind = TypedSlabPoolConsumerKind(request.object_kind);
  resource_request.reservation_ledger = request.reservation_ledger;
  resource_request.memory_manager = request.memory_manager;
  resource_request.scope_chain = request.scope_chain;
  resource_request.category = request.category;
  resource_request.memory_class = request.memory_class;
  resource_request.requested_bytes = request.reservation_bytes;
  resource_request.owner_id = request.owner_id;
  resource_request.route_label = request.route_label;
  resource_request.operation_id = request.operation_id;
  resource_request.purpose = request.purpose;
  resource_request.spillable = request.spillable;
  resource_request.cancelable = request.cancelable;
  resource_request.priority = request.priority;
  resource_request.weight = request.weight;
  resource_request.lease_expires_at_ms = request.lease_expires_at_ms;
  resource_request.production_like = request.production_like;
  resource_request.provenance = request.provenance;
  resource_request.authority.engine_mga_authoritative =
      request.authority.engine_mga_authoritative;
  resource_request.authority.transaction_inventory_authoritative =
      request.authority.transaction_inventory_authoritative;
  resource_request.authority.security_or_policy_checked =
      request.authority.security_or_policy_checked;
  resource_request.authority.parser_or_reference_finality_authority =
      request.authority.parser_or_reference_finality_authority;
  resource_request.authority.memory_visibility_or_finality_authority =
      request.authority.memory_visibility_or_finality_authority;
  resource_request.authority.memory_recovery_authority =
      request.authority.memory_recovery_authority;
  resource_request.authority.memory_authorization_authority =
      request.authority.memory_authorization_authority;
  resource_request.authority.benchmark_authority = request.authority.benchmark_authority;
  resource_request.authority.cluster_authority = request.authority.cluster_authority;
  resource_request.authority.debug_or_relaxed_path = request.authority.debug_or_relaxed_path;
  resource_request.authority.optimizer_plan_authority =
      request.authority.optimizer_plan_authority;
  resource_request.authority.index_finality_authority =
      request.authority.index_finality_authority;
  resource_request.authority.agent_action_authority = request.authority.agent_action_authority;

  auto resource = AcquireReservationBackedMemoryResource(std::move(resource_request));
  if (!resource.ok()) {
    SizeClassAllocatorAcquireResult result;
    result.status = resource.status;
    result.fail_closed = true;
    result.diagnostic = resource.diagnostic;
    result.evidence = resource.evidence;
    result.evidence.push_back(kAnchor);
    result.evidence.push_back("typed_slab_pool.reservation_created=false");
    return result;
  }

  SizeClassAllocatorAcquireResult result;
  result.status = OkStatus();
  result.evidence = resource.evidence;
  result.evidence.push_back(kAnchor);
  result.evidence.push_back(kAuthorityScope);
  result.evidence.push_back("typed_slab_pool.reservation_first=true");
  result.evidence.push_back("typed_slab_pool.size_class_count=" +
                            std::to_string(request.size_classes.size()));
  result.diagnostic = MakePoolDiagnostic(
      result.status,
      "SB_CEIC_013_TYPED_SLAB_POOL.OK",
      "memory.ceic_013.typed_slab_pool.ok",
      request,
      {{"size_class_count", std::to_string(request.size_classes.size())}});
  result.allocator.reset(new SizeClassAllocator(std::move(request),
                                                std::move(resource.resource)));
  return result;
}

}  // namespace scratchbird::core::memory
