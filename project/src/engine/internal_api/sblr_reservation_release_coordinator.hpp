#pragma once
#include "api_types.hpp"
#include <cstdint>
#include <string>
namespace scratchbird::engine::internal_api {
enum class SblrRelationReservationState:std::uint8_t{active=1,released=2,revoked=3};
struct SblrRelationReservationSnapshot{std::string receipt_uuid,reservation_uuid,transaction_uuid,relation_uuid,reservation_evidence_sha256,release_evidence_sha256;std::uint64_t reservation_generation=0,local_transaction_id=0,catalog_generation=0,policy_generation=0,structural_occurrence_id=0,availability_generation=0,release_sequence=0;std::uint8_t mode=0;SblrRelationReservationState state=SblrRelationReservationState::revoked;};
struct SblrRelationReservationResult{bool ok=false;EngineApiDiagnostic diagnostic;SblrRelationReservationSnapshot snapshot;};
SblrRelationReservationResult PublishSblrRelationReservation(const EngineRequestContext&,const std::string&receipt,const std::string&relation,std::uint8_t mode,std::uint64_t catalog_generation,std::uint64_t policy_generation);
SblrRelationReservationResult CompileAndPublishSblrRelationReservation(const EngineRequestContext&,const std::string&receipt,std::uint64_t occurrence);
SblrRelationReservationResult CoordinateSblrReservationRelease(const EngineRequestContext&,const std::string&receipt,const std::string&relation,std::uint64_t occurrence,std::uint64_t availability_generation);
SblrRelationReservationResult ReleaseSblrRelationReservation(const EngineRequestContext&,const std::string&reservation,std::uint64_t generation,const std::string&relation,const std::string&evidence,std::uint64_t availability_generation);
EngineApiDiagnostic RecoverSblrRelationReservations(const EngineRequestContext&);
}
