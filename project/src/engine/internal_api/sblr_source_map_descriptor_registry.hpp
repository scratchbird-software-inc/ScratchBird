#pragma once
#include "api_types.hpp"
#include "engine/sblr/sblr_source_map_runtime.hpp"
#include <cstdint>
#include <string>
#include <vector>
namespace scratchbird::engine::internal_api {
enum class SblrSourceMapLifecycleV1:std::uint8_t{active=1,revoked=2};
struct SblrSourceMapDescriptorSnapshotV1{std::string descriptor_uuid,registry_snapshot_uuid,statement_receipt_uuid,database_uuid,session_uuid,transaction_uuid,bound_ast_sha256,vector_sha256,decision_evidence_sha256;std::uint64_t descriptor_generation=0,registry_generation=0;SblrSourceMapLifecycleV1 lifecycle=SblrSourceMapLifecycleV1::revoked;std::vector<std::uint8_t> canonical_smvd;};
struct SblrSourceMapRegistryResultV1{bool ok=false;EngineApiDiagnostic diagnostic;SblrSourceMapDescriptorSnapshotV1 snapshot;std::vector<EngineEvidenceReference> evidence;};
SblrSourceMapRegistryResultV1 IssueSblrSourceMapDescriptorV1(const EngineRequestContext&,const std::string& statement_receipt_uuid,const std::string& bound_ast_sha256,const std::string& registry_snapshot_uuid,std::uint64_t registry_generation,std::vector<scratchbird::engine::sblr::SblrSourceMapEntryV1> entries);
SblrSourceMapRegistryResultV1 LookupSblrSourceMapDescriptorV1(const EngineRequestContext&,const std::string& statement_receipt_uuid,const std::string& descriptor_uuid,std::uint64_t descriptor_generation,const std::string& bound_ast_sha256,const std::string& registry_snapshot_uuid,std::uint64_t registry_generation);
EngineApiDiagnostic RevokeSblrSourceMapDescriptorsV1(const EngineRequestContext&,const std::string& statement_receipt_uuid,const std::string& reason_code);
EngineApiDiagnostic RecoverSblrSourceMapDescriptorRegistryV1(const EngineRequestContext&);
}
