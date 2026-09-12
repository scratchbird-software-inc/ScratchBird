#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_error_vector_runtime.hpp"
#include <cstdint>
#include <vector>
namespace scratchbird::engine::internal_api {
using SblrErrorVectorUuidV1 = scratchbird::engine::sblr::SblrErrorUuidV1;
using SblrErrorVectorSha256V1 = scratchbird::engine::sblr::SblrErrorSha256V1;
enum class SblrErrorVectorLifecycleV1 : std::uint8_t { active=1, revoked=2 };
struct SblrErrorVectorDescriptorSnapshotV1 {
  SblrErrorVectorUuidV1 descriptor_uuid{}, registry_snapshot_uuid{},
      statement_receipt_uuid{}, diagnostic_registry_snapshot_uuid{},
      database_uuid{}, session_uuid{}, principal_uuid{}, record_uuid{};
  SblrErrorVectorSha256V1 vector_sha256{}, evidence_sha256{};
  std::uint64_t descriptor_generation=0, registry_generation=0,
      diagnostic_registry_generation=0, journal_sequence=0;
  SblrErrorVectorLifecycleV1 lifecycle=SblrErrorVectorLifecycleV1::active;
  std::vector<std::uint8_t> canonical_ervd;
};
struct SblrErrorVectorRegistryResultV1 {
  bool ok=false;
  EngineApiDiagnostic diagnostic;
  SblrErrorVectorDescriptorSnapshotV1 snapshot;
};
SblrErrorVectorRegistryResultV1 IssueSblrErrorVectorDescriptorV1(
    const EngineRequestContext&, const SblrErrorVectorUuidV1& receipt,
    const SblrErrorVectorUuidV1& registry_snapshot, std::uint64_t registry_generation,
    const SblrErrorVectorUuidV1& diagnostic_snapshot, std::uint64_t diagnostic_generation,
    std::vector<scratchbird::engine::sblr::SblrErrorVectorEntryV1>);
SblrErrorVectorRegistryResultV1 LookupSblrErrorVectorDescriptorV1(
    const EngineRequestContext&, const SblrErrorVectorUuidV1& receipt,
    const SblrErrorVectorUuidV1& descriptor, std::uint64_t descriptor_generation);
EngineApiDiagnostic RevokeSblrErrorVectorDescriptorsV1(
    const EngineRequestContext&, const SblrErrorVectorUuidV1& receipt);
// Private engine node-startup operation, never a parser-supplied right or trace.
EngineApiDiagnostic RecoverSblrErrorVectorDescriptorRegistryV1(const EngineRequestContext&);
}
