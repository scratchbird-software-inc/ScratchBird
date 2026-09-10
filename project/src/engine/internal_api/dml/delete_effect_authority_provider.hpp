// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "typed_delete_carrier_codec.hpp"
#include <memory>

namespace scratchbird::engine::internal_api {
struct EngineDmlDeleteEffectAuthorityResultV1;
class EngineDmlDeleteEffectAuthorityHandleV1 final {
 public:
  bool valid() const noexcept { return authority_ != nullptr; }
 private:
  struct Authority;
  std::shared_ptr<const Authority> authority_;
  friend EngineDmlDeleteEffectAuthorityResultV1 CaptureDmlDeleteEffectAuthorityV1(
      const EngineRequestContext&, const std::string&);
  friend EngineApiDiagnostic RevalidateDmlDeleteEffectAuthorityV1(
      const EngineRequestContext&, const EngineDmlDeleteEffectAuthorityResultV1&);
};
struct EngineDmlDeleteEffectSnapshotV1 {
  wire::TypedUpdateUuid snapshot_uuid{};
  wire::TypedUpdateUuid constraint_set_uuid{};
  wire::TypedUpdateUuid trigger_set_uuid{};
  std::uint64_t generation = 0;
  wire::TypedUpdateUuid target_relation_uuid{};
  std::uint64_t target_relation_generation = 0;
  wire::TypedUpdateUuid relation_descriptor_uuid{};
  std::uint64_t relation_descriptor_generation = 0;
  wire::TypedUpdateHash relation_shape_sha256{};
  wire::TypedUpdateHash index_set_sha256{};
  wire::TypedUpdateHash constraint_set_sha256{};
  wire::TypedUpdateHash trigger_set_sha256{};
  std::uint32_t index_count = 0;
  // The initial profile admits no inbound FK, cascade, or trigger body.
  // Absence is resolved from the actual managers, never an empty caller list.
  std::uint32_t constraint_count = 0;
  std::uint32_t trigger_count = 0;
  bool operator==(const EngineDmlDeleteEffectSnapshotV1&) const = default;
};
struct EngineDmlDeleteEffectAuthorityResultV1 {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  EngineDmlDeleteEffectSnapshotV1 snapshot;
  EngineDmlDeleteEffectAuthorityHandleV1 handle;
};

// Metadata-only, engine-private provider. Captures admitted index/overflow
// shape and proves the initial DELETE-specific constraint/trigger profile.
// This handle does not supply DELETE privilege, resources, execution, replay,
// or transaction finality, and cannot be reconstructed from a supplied hash.
EngineDmlDeleteEffectAuthorityResultV1 CaptureDmlDeleteEffectAuthorityV1(
    const EngineRequestContext&, const std::string& target_relation_uuid);
EngineApiDiagnostic RevalidateDmlDeleteEffectAuthorityV1(
    const EngineRequestContext&, const EngineDmlDeleteEffectAuthorityResultV1&);
// Read-only manager comparison, never a factory for live effect authority.
EngineApiDiagnostic RevalidateRecoveredDmlDeleteEffectProjectionV1(
    const EngineRequestContext&, const EngineDmlDeleteEffectSnapshotV1&);
}  // namespace scratchbird::engine::internal_api
