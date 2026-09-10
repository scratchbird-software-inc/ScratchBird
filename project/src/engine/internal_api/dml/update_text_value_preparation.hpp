// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "dml/update_text_target_authority_provider.hpp"
#include <span>

namespace scratchbird::engine::internal_api {

struct EngineDmlUpdateTextPreparationResultV2;

// Engine-private canonical value preparation. Owns bounded, tracked payload
// memory and exact target authority, but is NOT a DUBR governor grant,
// cancellation token, UPDATE permission, mutation or recovery admission.
class EngineDmlUpdatePreparedTextValueV2 final {
 public:
  struct Storage;
  bool valid() const noexcept { return storage_ != nullptr; }
  EngineValueState state() const noexcept;
  std::uint64_t scalar_count() const noexcept;
  // Borrowed immutable bytes; callers must retain this handle and revalidate
  // authority at consumption. Inspecting bytes does not authorize their use.
  std::span<const std::uint8_t> bytes() const noexcept;

 private:
  std::shared_ptr<const Storage> storage_;
  friend EngineDmlUpdateTextPreparationResultV2 PrepareDmlUpdateTextValueV2(
      const EngineRequestContext&, const EngineDmlUpdateTextTargetHandleV2&,
      EngineValueState, std::span<const std::uint8_t>);
  friend EngineApiDiagnostic RevalidateDmlUpdatePreparedTextValueV2(
      const EngineRequestContext&, const EngineDmlUpdatePreparedTextValueV2&);
};

struct EngineDmlUpdateTextPreparationResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlUpdatePreparedTextValueV2 value;
};

// Input is already-decoded canonical bytes, never SQL tokens or client UUID
// spellings. No truncation, padding, normalization, default or missing state.
EngineDmlUpdateTextPreparationResultV2 PrepareDmlUpdateTextValueV2(
    const EngineRequestContext& context,
    const EngineDmlUpdateTextTargetHandleV2& target,
    EngineValueState state, std::span<const std::uint8_t> bytes);
EngineApiDiagnostic RevalidateDmlUpdatePreparedTextValueV2(
    const EngineRequestContext& context,
    const EngineDmlUpdatePreparedTextValueV2& value);

}  // namespace scratchbird::engine::internal_api
