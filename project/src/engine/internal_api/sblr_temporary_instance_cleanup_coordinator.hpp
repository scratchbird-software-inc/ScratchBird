#pragma once
#include "api_types.hpp"
#include <cstdint>
#include <string>
namespace scratchbird::engine::internal_api {
enum class SblrTemporaryInstanceState:std::uint8_t{active=1,cleaned=2,revoked=3};
struct SblrTemporaryInstanceSnapshot{std::string receipt_uuid,descriptor_uuid,definition_uuid,instance_uuid,owner_session_uuid,owner_transaction_uuid,descriptor_evidence_sha256,cleanup_evidence_sha256;std::uint64_t structural_occurrence_id=0,descriptor_generation=0,instance_generation=0,catalog_generation=0,security_generation=0,policy_generation=0,availability_generation=0,cleanup_sequence=0,reclaimed_pages=0;std::uint8_t retention=0,trigger=0;SblrTemporaryInstanceState state=SblrTemporaryInstanceState::active;};
struct SblrTemporaryInstanceResult{bool ok=false;SblrTemporaryInstanceSnapshot snapshot;EngineApiDiagnostic diagnostic;};
SblrTemporaryInstanceResult PublishSblrTemporaryInstance(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint8_t);
SblrTemporaryInstanceResult CoordinateSblrTemporaryInstanceCleanup(const EngineRequestContext&,const std::string&,std::uint64_t,std::uint8_t,std::uint64_t);
SblrTemporaryInstanceResult CleanupSblrTemporaryInstance(const EngineRequestContext&,const std::string&,std::uint64_t,const std::string&,std::uint64_t);
EngineApiDiagnostic RecoverSblrTemporaryInstances(const EngineRequestContext&);
}
