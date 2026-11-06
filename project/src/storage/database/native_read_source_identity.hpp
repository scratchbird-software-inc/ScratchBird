// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "native_common_page_header.hpp"
#include <array>
#include <optional>

namespace scratchbird::storage::database {
using scratchbird::core::platform::Uuid;
using scratchbird::core::platform::byte;
using scratchbird::core::platform::u8;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

// Which kind of read source a set of startup observations came from.
enum class NativeReadSourceKind : u8 { none, fixed_root, selected_checkpoint };

// SB_TRANSITIONAL_SELECTION_KEY: the checkpoint selection's remaining identity
// fields. While a selected checkpoint still supplies the source, equality of
// two identities keeps the full strength of comparing the selections
// themselves. All empty for a fixed-root source. Deleted with the
// checkpoint/selector layer.
struct NativeTransitionalSelectionKey {
  Uuid selector_object_uuid, bootstrap_uuid, publication_uuid;
  disk::NativePageReference checkpoint;
  std::array<byte, 32> checkpoint_sha256{};
  u64 selected_root_set_generation = 0;
  Uuid timeline_uuid;
  u64 previous_selection_generation = 0;
  std::optional<disk::NativePageReference> previous_checkpoint;
  Uuid previous_checkpoint_object_uuid;
  std::array<byte, 32> previous_checkpoint_sha256{};
  bool operator==(const NativeTransitionalSelectionKey&) const = default;
};

// The identity of the source that startup observations were read from. Each
// stage copies it, and a later stage refuses an observation whose identity
// differs. It is a value only: holding one grants no read, custody or
// ownership of the source.
//
//   field                fixed root (page zero)       selected checkpoint
//   root_set_uuid        page zero UUID               checkpoint object UUID
//   source_generation    page zero page generation    selection generation
//   root_set_generation  page zero root-set gen.      checkpoint generation
//
// Create writes every one of these generations as 1.
struct NativeReadSourceIdentity {
  NativeReadSourceKind kind = NativeReadSourceKind::none;
  Uuid database_uuid, filespace_uuid;
  u32 page_size_bytes = 0;
  Uuid root_set_uuid;
  u64 source_generation = 0, root_set_generation = 0;
  NativeTransitionalSelectionKey transitional_selection_key;
  bool operator==(const NativeReadSourceIdentity&) const = default;
};

inline bool SameReadSource(const NativeReadSourceIdentity& a, const NativeReadSourceIdentity& b) noexcept {
  return a == b;
}
}  // namespace scratchbird::storage::database
