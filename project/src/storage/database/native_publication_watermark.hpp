// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once

#include "native_common_page_header.hpp"
#include "filespace_page_zero.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::database {
using core::platform::Uuid;
using core::platform::byte;
using core::platform::u64;

// MGA-NATIVE-PUBLICATION-WATERMARK-IMAGE-001.
// Durable operation-state IMAGE only. No live allocator/transaction authority.
struct NativePublicationWatermark {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid, bootstrap_uuid, timeline_uuid, operation_uuid;
  u64 watermark = 0, base_checkpoint_generation = 0, base_root_set_generation = 0;
  u64 previous_watermark = 0;
  disk::NativePageReference base_checkpoint;
  Uuid base_checkpoint_object_uuid;
  std::array<byte, 32> base_checkpoint_sha256{}, previous_state_sha256{};
};
enum class NativePublicationWatermarkError {
  none, invalid_header, invalid_family, invalid_identity, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_pair, repair_required
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
NativePublicationWatermarkImage EncodeNativePublicationWatermark(
    const NativePublicationWatermark&) noexcept;
NativePublicationWatermarkImage DecodeNativePublicationWatermark(
    const std::vector<byte>&) noexcept;
// A stable image pair still needs actual allocation/bootstrap/checkpoint,
// operation, retained-device and recovery authority before any use.
NativePublicationWatermarkImage ClassifyNativePublicationWatermarkPair(
    const std::vector<byte>& first, const std::vector<byte>& second) noexcept;
}  // namespace scratchbird::storage::database
