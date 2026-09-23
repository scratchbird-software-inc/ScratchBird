#include "sblr_reservation_release_coordinator.hpp"
#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include "engine/sblr/sblr_reservation_release_runtime.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <map>
#include <atomic>
#include "mga_relation_store/mga_binary_fields.hpp"
namespace scratchbird::engine::internal_api {namespace {std::mutex m;std::map<EngineUuid,SblrRelationReservationSnapshot> rows;std::atomic<std::uint64_t> generation{0};EngineApiDiagnostic D(std::string c,std::string k){const bool error=c!="OK";return MakeEngineApiDiagnostic(std::move(c),std::move(k),{},error);}bool tag(const EngineRequestContext&c,const char*t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}EngineUuid id(){auto ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());auto u=scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object,ms+(++generation));return u.ok()?u.value.value:EngineUuid{};}std::string hash(const std::string&s){auto d=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(s.begin(),s.end())).digest;return "sha256:"+scratchbird::core::hash::HexLower(d);}bool publish(const EngineRequestContext& c,const SblrRelationReservationSnapshot& s,char event) {
  std::string bytes("SBRSRV02");
  AppendBinaryU8(&bytes, static_cast<std::uint8_t>(event));
  for (const auto& identity : {s.database_uuid,s.receipt_uuid,s.reservation_uuid,s.transaction_uuid,s.relation_uuid}) {
    if (!scratchbird::core::uuid::IsEngineIdentityUuid(identity)) return false;
    bytes.append(reinterpret_cast<const char*>(identity.bytes.data()),16);
  }
  for (auto value : {s.reservation_generation,s.local_transaction_id,s.catalog_generation,
                    s.policy_generation,s.structural_occurrence_id,s.availability_generation,s.release_sequence})
    AppendBinaryU64(&bytes,value);
  AppendBinaryU8(&bytes,s.mode);
  AppendBinaryU8(&bytes,static_cast<std::uint8_t>(s.state));
  std::ofstream output(c.database_path+".sb.sblr_relation_reservation.v2",std::ios::app|std::ios::binary);
  if (!output) return false;
  output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
  output.flush();
  return static_cast<bool>(output);
}
// These publication records describe runtime reservations, not MGA finality.
bool owns(const EngineRequestContext& c,const SblrRelationReservationSnapshot& s) {
  return s.database_uuid==c.database_uuid&&s.transaction_uuid==c.transaction_uuid;
}
}

SblrRelationReservationResult PublishSblrRelationReservation(const EngineRequestContext&c,const EngineUuid&receipt,const EngineUuid&relation,std::uint8_t mode,std::uint64_t catalog,std::uint64_t policy){std::lock_guard l(m);SblrRelationReservationResult o;if(!tag(c,"private_transaction_relation_reservation")||!c.statement_metadata_snapshot_engine_owned){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.reservation.hidden");return o;}if(!scratchbird::core::uuid::IsEngineIdentityUuid(c.database_uuid)||!scratchbird::core::uuid::IsEngineIdentityUuid(receipt)||receipt!=c.statement_uuid||c.transaction_uuid.is_nil()||relation.is_nil()||mode<1||mode>4||!catalog||!policy){o.diagnostic=D("SBLR.OPERAND_INVALID","sblr.reservation.publish_invalid");return o;}SblrRelationReservationSnapshot s;s.database_uuid=c.database_uuid;s.receipt_uuid=receipt;s.reservation_uuid=id();s.reservation_generation=++generation;s.transaction_uuid=c.transaction_uuid;s.local_transaction_id=c.local_transaction_id;s.relation_uuid=relation;s.mode=mode;s.catalog_generation=catalog;s.policy_generation=policy;s.state=SblrRelationReservationState::active;scratchbird::engine::sblr::SblrReservationReleaseDescriptorV1 wire;if(!scratchbird::core::uuid::IsEngineIdentityUuid(s.reservation_uuid)||!scratchbird::core::uuid::IsEngineIdentityUuid(s.transaction_uuid)||!scratchbird::core::uuid::IsEngineIdentityUuid(s.relation_uuid)){o.diagnostic=D("TX.RESERVATION.RELEASE_FAILED","sblr.reservation.identity_invalid");return o;}wire.reservation=s.reservation_uuid.bytes;wire.reservation_generation=s.reservation_generation;wire.transaction=s.transaction_uuid.bytes;wire.local_transaction_id=s.local_transaction_id;wire.relation=s.relation_uuid.bytes;wire.mode=s.mode;wire.catalog_generation=s.catalog_generation;wire.policy_generation=s.policy_generation;wire.availability_generation=1;const auto canonical=scratchbird::engine::sblr::EncodeSblrReservationReleaseDescriptorV1(wire);if(canonical.size()!=144){o.diagnostic=D("TX.RESERVATION.RELEASE_FAILED","sblr.reservation.evidence_failed");return o;}s.reservation_evidence_sha256="sha256:";static constexpr char hx[]="0123456789abcdef";for(size_t i=104;i<136;++i){s.reservation_evidence_sha256.push_back(hx[canonical[i]>>4]);s.reservation_evidence_sha256.push_back(hx[canonical[i]&15]);}if(s.reservation_uuid.is_nil()||!publish(c,s,'P')){o.diagnostic=D("TX.RESERVATION.RELEASE_FAILED","sblr.reservation.publish_failed");return o;}rows[s.reservation_uuid]=s;o.ok=true;o.snapshot=s;o.diagnostic=D("OK","ok");return o;}
SblrRelationReservationResult CompileAndPublishSblrRelationReservation(const EngineRequestContext&c,const EngineUuid&receipt,std::uint64_t occurrence){SblrRelationReservationResult o;if(!tag(c,"private_transaction_relation_reservation_compiler")||!c.statement_metadata_snapshot_engine_owned||receipt!=c.statement_uuid||!occurrence||c.transaction_uuid.is_nil()){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.reservation.compiler_hidden");return o;}auto compiler_context=c;compiler_context.trace_tags.push_back("private_transaction_relation_reservation");const auto relation=id();if(relation.is_nil()){o.diagnostic=D("TX.RESERVATION.RELEASE_FAILED","sblr.reservation.relation_identity_failed");return o;}o=PublishSblrRelationReservation(compiler_context,receipt,relation,1,std::max<std::uint64_t>(1,c.statement_metadata_snapshot_visible_through_local_transaction_id),std::max<std::uint64_t>(1,c.local_transaction_id));if(o.ok)o.snapshot.structural_occurrence_id=occurrence;return o;}
SblrRelationReservationResult CoordinateSblrReservationRelease(const EngineRequestContext&c,const EngineUuid&receipt,const EngineUuid&relation,std::uint64_t occurrence,std::uint64_t availability){std::lock_guard l(m);SblrRelationReservationResult o;if(!tag(c,"private_transaction_relation_reservation")){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.reservation.hidden");return o;}for(auto&[_,s]:rows)if(s.receipt_uuid==receipt&&owns(c,s)&&s.relation_uuid==relation){if(s.state!=SblrRelationReservationState::active){o.diagnostic=D("MGA.TRANSACTION.STALE","sblr.reservation.stale");return o;}s.structural_occurrence_id=occurrence;s.availability_generation=availability;o.ok=true;o.snapshot=s;o.diagnostic=D("OK","ok");return o;}o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.reservation.hidden");return o;}
SblrRelationReservationResult ReleaseSblrRelationReservation(const EngineRequestContext&c,const EngineUuid&idv,std::uint64_t gen,const EngineUuid&relation,const std::string&evidence,std::uint64_t availability){std::lock_guard l(m);SblrRelationReservationResult o;if(!tag(c,"private_transaction_relation_reservation")){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.reservation.hidden");return o;}auto it=rows.find(idv);if(it==rows.end()||!owns(c,it->second)||it->second.relation_uuid!=relation){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.reservation.hidden");return o;}auto s=it->second;if(s.state!=SblrRelationReservationState::active||s.reservation_generation!=gen||s.reservation_evidence_sha256!=evidence||s.availability_generation!=availability){o.diagnostic=D("MGA.TRANSACTION.STALE","sblr.reservation.stale");return o;}s.state=SblrRelationReservationState::released;s.release_sequence=++generation;s.release_evidence_sha256=hash("ScratchBird.SblrTransactionReservationRelease.V1"+s.reservation_evidence_sha256+std::to_string(s.release_sequence));if(!publish(c,s,'R')){o.diagnostic=D("TX.RESERVATION.RELEASE_FAILED","sblr.reservation.release_publish_failed");return o;}it->second=s;o.ok=true;o.snapshot=s;o.diagnostic=D("OK","ok");return o;}
EngineApiDiagnostic RecoverSblrRelationReservations(const EngineRequestContext& c) {
  std::lock_guard lock(m);
  if (!tag(c,"right:SBLR_TRANSACTION_RESERVATION_ADMIN") ||
      !scratchbird::core::uuid::IsEngineIdentityUuid(c.database_uuid))
    return D("SECURITY.ACCESS_DENIED","sblr.reservation.recovery_denied");
  std::vector<SblrRelationReservationSnapshot> staged;
  for (const auto& [identity, snapshot] : rows) {
    if (snapshot.database_uuid!=c.database_uuid || snapshot.state!=SblrRelationReservationState::active) continue;
    auto candidate=snapshot;
    candidate.state=SblrRelationReservationState::revoked;
    candidate.release_sequence=++generation;
    staged.push_back(std::move(candidate));
  }
  for (const auto& candidate : staged)
    if (!publish(c,candidate,'X')) return D("TX.RESERVATION.RELEASE_FAILED","sblr.reservation.recovery_failed");
  for (auto& candidate : staged) rows[candidate.reservation_uuid]=std::move(candidate);
  return D("OK","ok");
}
}
