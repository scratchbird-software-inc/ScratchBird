// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "catalog/name_resolution_api.hpp"
#include "mga_relation_store/mga_text_target_selection.hpp"
#include <memory>

namespace scratchbird::engine::internal_api {

struct EngineDmlUpdateTextTargetSnapshotV2 {
  MgaTextColumnKeyV2 key;
  std::uint64_t column_generation = 0;
  bool nullable = false;
  bool contextual = false;
  std::uint64_t byte_limit = 0;
  std::uint64_t character_limit = 0;
  std::string exact_persisted_value_descriptor;
  sblr::ContextualTextDescriptorV2 descriptor;
  std::vector<std::uint8_t> exact_relation_projection;
  EngineResolvedResourceDescriptor charset;
  EngineResolvedResourceDescriptor collation;
};

struct EngineDmlUpdateTextTargetCaptureResultV2;

// Engine-private read-only target/locale authority. It is NOT a memory grant,
// literal binding, UPDATE privilege, index capability or mutation admission.
class EngineDmlUpdateTextTargetHandleV2 final {
 public:
  struct Authority;
  bool valid() const noexcept { return authority_ != nullptr; }
  const EngineDmlUpdateTextTargetSnapshotV2* snapshot() const noexcept;

 private:
  std::shared_ptr<const Authority> authority_;
  friend EngineDmlUpdateTextTargetCaptureResultV2 CaptureDmlUpdateTextTargetV2(
      const EngineRequestContext&, const MgaTextColumnKeyV2&, std::uint64_t);
  friend EngineApiDiagnostic RevalidateDmlUpdateTextTargetV2(
      const EngineRequestContext&, const EngineDmlUpdateTextTargetHandleV2&);
};

struct EngineDmlUpdateTextTargetCaptureResultV2 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlUpdateTextTargetHandleV2 handle;
};

EngineDmlUpdateTextTargetCaptureResultV2 CaptureDmlUpdateTextTargetV2(
    const EngineRequestContext& context, const MgaTextColumnKeyV2& key,
    std::uint64_t expected_column_generation);
EngineApiDiagnostic RevalidateDmlUpdateTextTargetV2(
    const EngineRequestContext& context,
    const EngineDmlUpdateTextTargetHandleV2& handle);

}  // namespace scratchbird::engine::internal_api
