// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <span>
#include <vector>

namespace scratchbird::storage::database {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u64;

// MGA-NATIVE-PUBLICATION-WATERMARK-IMAGE-001.
// Durable operation-state IMAGE only. No live allocator/transaction authority.
// MGA-NATIVE-STARTUP-PUBLICATION-BINDING-001. These are binary identities,
// not authorization, allocated-inventory proof or a restart/recovery receipt.
struct NativeStartupBinding {
  Uuid operation_uuid, session_uuid, transaction_uuid;
  u64 local_transaction_id=0, fence_generation=0;
  bool operator==(const NativeStartupBinding&) const = default;
};
struct NativePublicationIntent {
  Uuid initiator_uuid, request_context_uuid, policy_snapshot_uuid;
  std::array<byte,32> normalized_request_sha256{};
  core::platform::u16 initiator_kind=0;
  // 0: unprofiled legacy; 1: metadata only; 2: inventory/control publication.
  // 3: native filespace preallocation/control publication (no abandonment).
  // 4: native physical growth/control publication (no abandonment).
  // A durable restriction, never authentication or user-effect authority.
  core::platform::u16 recovery_profile=0;
  std::optional<NativeStartupBinding> startup_binding;
  bool operator==(const NativePublicationIntent&) const = default;
};
struct NativePublicationWatermark {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid, bootstrap_uuid, timeline_uuid, operation_uuid;
  u64 watermark = 0, base_checkpoint_generation = 0, base_root_set_generation = 0;
  u64 previous_watermark = 0;
  disk::NativePageReference base_checkpoint;
  Uuid base_checkpoint_object_uuid;
  std::array<byte, 32> base_checkpoint_sha256{}, previous_state_sha256{};
  std::optional<NativePublicationIntent> intent;
  struct PlanAnchor {
    disk::NativePageReference page;
    Uuid object_uuid;
    std::array<byte,32> sha256{}, reservation_state_sha256{};
    bool operator==(const PlanAnchor&) const = default;
  };
  std::optional<PlanAnchor> publication_plan;
  struct Abandonment {
    Uuid resolution_uuid;
    std::array<byte,32> pending_state_sha256{};
    bool operator==(const Abandonment&) const = default;
  };
  std::optional<Abandonment> abandonment;
};
enum class NativePublicationWatermarkError {
  none, invalid_header, invalid_family, invalid_identity, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_pair, repair_required,
  invalid_backing
};
struct NativePublicationWatermarkImage {
  NativePublicationWatermarkError error = NativePublicationWatermarkError::invalid_family;
  std::optional<NativePublicationWatermark> state;
  std::array<byte, 32> state_sha256{};
  std::vector<byte> bytes;
  bool ok() const noexcept {
    return error == NativePublicationWatermarkError::none && state.has_value();
  }
};
// Fixed fields only. No retained page copy or source address. These values are
// image-level evidence, never a generation reservation or publication lease.
struct NativePublicationWatermarkValue {
  NativePublicationWatermarkError error=NativePublicationWatermarkError::invalid_family;
  std::optional<NativePublicationWatermark> state;
  std::array<byte,32> state_sha256{};
  bool ok() const noexcept {return error==NativePublicationWatermarkError::none&&state.has_value();}
};
struct NativePublicationWatermarkViewImage {
  NativePublicationWatermarkError error=NativePublicationWatermarkError::invalid_family;
  std::optional<NativePublicationWatermark> state;
  std::array<byte,32> state_sha256{};
  std::span<const byte> bytes;
  bool ok() const noexcept {return error==NativePublicationWatermarkError::none&&state.has_value()&&!bytes.empty();}
};
// The complete output must exclude the input value; only one physical-page
// prefix is initialized. A failure returns no usable prefix, though late hash
// failure may leave staging bytes. Memory admission precedes device guards.
NativePublicationWatermarkViewImage EncodeNativePublicationWatermarkInto(
    const NativePublicationWatermark&,std::span<byte>) noexcept;
NativePublicationWatermarkValue DecodeNativePublicationWatermarkValue(std::span<const byte>) noexcept;
NativePublicationWatermarkValue ClassifyNativePublicationWatermarkPairValue(
    std::span<const byte>,std::span<const byte>) noexcept;
NativePublicationWatermarkImage EncodeNativePublicationWatermark(
    const NativePublicationWatermark&) noexcept;
NativePublicationWatermarkImage DecodeNativePublicationWatermark(
    const std::vector<byte>&) noexcept;
// A stable image pair still needs actual allocation/bootstrap/checkpoint,
// operation, retained-device and recovery authority before any use.
NativePublicationWatermarkImage ClassifyNativePublicationWatermarkPair(
    const std::vector<byte>& first, const std::vector<byte>& second) noexcept;
}  // namespace scratchbird::storage::database
