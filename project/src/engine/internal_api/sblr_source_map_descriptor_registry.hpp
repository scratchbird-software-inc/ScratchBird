// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_source_map_runtime.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {
using SblrSourceMapHashV2 = std::array<std::uint8_t, 32>;
enum class SblrSourceMapLifecycleV1 : std::uint8_t { active=1, revoked=2 };
struct SblrSourceMapDescriptorSnapshotV1 {
  EngineUuid descriptor_uuid, registry_snapshot_uuid, statement_receipt_uuid,
      database_uuid, session_uuid, transaction_uuid, principal_uuid,
      publication_uuid;
  SblrSourceMapHashV2 bound_ast_sha256{}, vector_sha256{}, decision_evidence_sha256{};
  std::uint64_t descriptor_generation=0, registry_generation=0,
      publication_generation=0;
  SblrSourceMapLifecycleV1 lifecycle=SblrSourceMapLifecycleV1::revoked;
  std::vector<std::uint8_t> canonical_smvd;
};
struct SblrSourceMapRegistryResultV1 {
  bool ok=false;
  EngineApiDiagnostic diagnostic;
  SblrSourceMapDescriptorSnapshotV1 snapshot;
};
SblrSourceMapRegistryResultV1 IssueSblrSourceMapDescriptorV1(
    const EngineRequestContext&, const EngineUuid& statement_receipt_uuid,
    const SblrSourceMapHashV2& bound_ast_sha256,
    const EngineUuid& registry_snapshot_uuid, std::uint64_t registry_generation,
    std::vector<scratchbird::engine::sblr::SblrSourceMapEntryV1> entries);
SblrSourceMapRegistryResultV1 LookupSblrSourceMapDescriptorV1(
    const EngineRequestContext&, const EngineUuid& statement_receipt_uuid,
    const EngineUuid& descriptor_uuid, std::uint64_t descriptor_generation,
    const SblrSourceMapHashV2& bound_ast_sha256,
    const EngineUuid& registry_snapshot_uuid, std::uint64_t registry_generation);
EngineApiDiagnostic RevokeSblrSourceMapDescriptorsV1(
    const EngineRequestContext&, const EngineUuid& statement_receipt_uuid);
// Private engine node-startup operation; parser trace tags cannot grant it.
EngineApiDiagnostic RecoverSblrSourceMapDescriptorRegistryV1(const EngineRequestContext&);
}  // namespace scratchbird::engine::internal_api
