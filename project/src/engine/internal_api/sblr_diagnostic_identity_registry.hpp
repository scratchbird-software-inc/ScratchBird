#pragma once
#include "api_types.hpp"
#include "wire/diagnostic_identity_projection_codec.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::internal_api {
using SblrDiagnosticIdentityUuidV1 = wire::DiagnosticRegistryUuidV1;
using SblrDiagnosticIdentitySha256V1 = wire::DiagnosticRegistrySha256V1;
struct SblrDiagnosticIdentityRowV1 : wire::DiagnosticIdentityProjectionV1 {
  // Private registration metadata, never serialized in the parser cohort.
  std::string canonical_code;
};
struct SblrDiagnosticIdentitySnapshotV1 {
  SblrDiagnosticIdentityUuidV1 snapshot_uuid{};
  SblrDiagnosticIdentityUuidV1 database_uuid{};
  std::uint64_t generation=0;
  std::vector<SblrDiagnosticIdentityRowV1> rows;
  SblrDiagnosticIdentitySha256V1 source_sha256{};
  SblrDiagnosticIdentitySha256V1 evidence_sha256{};
};
struct SblrDiagnosticIdentityResultV1 {bool ok=false;EngineApiDiagnostic diagnostic;SblrDiagnosticIdentitySnapshotV1 snapshot;SblrDiagnosticIdentityRowV1 row;};
// Bit N is the permission to project canonical severity N, not membership or
// field-disclosure authority. Used by the actual snapshot filter and its
// independent materialized-authorization tests. Internal bit 13 is always zero.
std::uint16_t SblrDiagnosticIdentityVisibilityMaskV1(const EngineRequestContext&);
SblrDiagnosticIdentityResultV1 LoadSblrDiagnosticIdentitySnapshotV1(const EngineRequestContext&);
SblrDiagnosticIdentityResultV1 LookupSblrDiagnosticIdentityV1(const EngineRequestContext&,const SblrDiagnosticIdentitySnapshotV1&,const SblrDiagnosticIdentityUuidV1&,std::uint64_t);
}
