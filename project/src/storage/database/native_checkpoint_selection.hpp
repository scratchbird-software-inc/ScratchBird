// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include <array>
#include <optional>
#include <vector>

namespace scratchbird::storage::database {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u64;
struct NativeCheckpointSelection {
  disk::NativeCommonPageHeader header;
  Uuid object_uuid, bootstrap_uuid, publication_uuid;
  u64 selection_generation=0;
  disk::NativePageReference checkpoint;
  Uuid checkpoint_object_uuid;
  std::array<byte,32> checkpoint_sha256{};
  u64 checkpoint_generation=0, root_set_generation=0;
  Uuid timeline_uuid;
  u64 previous_selection_generation=0;
  std::optional<disk::NativePageReference> previous_checkpoint;
  Uuid previous_checkpoint_object_uuid;
  std::array<byte,32> previous_checkpoint_sha256{};
};
enum class NativeCheckpointSelectionError {
  none, invalid_header, invalid_family, invalid_identity, invalid_reference,
  invalid_integrity, hash_failure, resource_exhausted, invalid_pair, repair_required
};
struct NativeCheckpointSelectionImage {
  NativeCheckpointSelectionError error=NativeCheckpointSelectionError::invalid_family;
  std::optional<NativeCheckpointSelection> selection;
  std::vector<byte> bytes;
  bool ok() const noexcept {return error==NativeCheckpointSelectionError::none&&selection.has_value();}
};
NativeCheckpointSelectionImage EncodeNativeCheckpointSelection(const NativeCheckpointSelection&) noexcept;
NativeCheckpointSelectionImage DecodeNativeCheckpointSelection(const std::vector<byte>&) noexcept;
// Image-level classification only. A stable pair does not prove allocation,
// bootstrap binding, actual checkpoint inventory, publication or serving.
struct NativeCheckpointSelectionPair {
  NativeCheckpointSelectionError error=NativeCheckpointSelectionError::invalid_pair;
  // Present only for a stable, internally agreeing pair; never a damaged prefix.
  std::optional<NativeCheckpointSelection> selection;
  bool ok() const noexcept {return error==NativeCheckpointSelectionError::none&&selection.has_value();}
};
NativeCheckpointSelectionPair ClassifyNativeCheckpointSelectionPair(
    const std::vector<byte>& first,const std::vector<byte>& second) noexcept;
}  // namespace scratchbird::storage::database
