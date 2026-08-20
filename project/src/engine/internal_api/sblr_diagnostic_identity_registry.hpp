#pragma once
#include "api_types.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::internal_api {
struct SblrDiagnosticIdentityRowV1 {std::string diagnostic_uuid;std::uint64_t diagnostic_generation=0;std::uint32_t precedence_ordinal=0;std::uint8_t severity_code=0,redaction_class=0;std::uint32_t maximum_safe_field_count=0;std::string row_identity_sha256;};
struct SblrDiagnosticIdentitySnapshotV1 {std::string snapshot_uuid;std::uint64_t generation=0;std::vector<SblrDiagnosticIdentityRowV1> rows;std::string evidence_sha256;};
struct SblrDiagnosticIdentityResultV1 {bool ok=false;EngineApiDiagnostic diagnostic;SblrDiagnosticIdentitySnapshotV1 snapshot;SblrDiagnosticIdentityRowV1 row;};
SblrDiagnosticIdentityResultV1 LoadSblrDiagnosticIdentitySnapshotV1(const EngineRequestContext&);
SblrDiagnosticIdentityResultV1 LookupSblrDiagnosticIdentityV1(const EngineRequestContext&,const SblrDiagnosticIdentitySnapshotV1&,const std::string&,std::uint64_t);
}
