#include "sblr_temporary_instance_cleanup_coordinator.hpp"
#include "api_diagnostics.hpp"
#include "engine/sblr/sblr_temporary_instance_cleanup_runtime.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <map>
#include "mga_relation_store/mga_binary_fields.hpp"
namespace scratchbird::engine::internal_api {namespace {std::mutex m;std::map<EngineUuid,SblrTemporaryInstanceSnapshot> rows;std::uint64_t generation=0;EngineApiDiagnostic D(std::string c,std::string k){const bool error=c!="OK";return MakeEngineApiDiagnostic(std::move(c),std::move(k),{},error);}bool tag(const EngineRequestContext&c,const char*t){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),t)!=c.trace_tags.end();}EngineUuid id(){auto ms=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());auto u=scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object,ms+(++generation));return u.ok()?u.value.value:EngineUuid{};}std::string hash(const std::string&s){auto d=scratchbird::core::hash::ComputeSha256Digest(std::vector<std::uint8_t>(s.begin(),s.end())).digest;return "sha256:"+scratchbird::core::hash::HexLower(d);}bool journal(const EngineRequestContext& c,const SblrTemporaryInstanceSnapshot& s,char event) {
  std::string bytes("SBTINST2");AppendBinaryU8(&bytes,static_cast<std::uint8_t>(event));
  for(const auto& identity:{s.database_uuid,s.receipt_uuid,s.descriptor_uuid,s.definition_uuid,s.instance_uuid,s.owner_session_uuid}) {
    if(!scratchbird::core::uuid::IsEngineIdentityUuid(identity)) return false;
    bytes.append(reinterpret_cast<const char*>(identity.bytes.data()),16);
  }
  if(!s.owner_transaction_uuid.is_nil()&&!scratchbird::core::uuid::IsEngineIdentityUuid(s.owner_transaction_uuid))return false;
  bytes.append(reinterpret_cast<const char*>(s.owner_transaction_uuid.bytes.data()),16);
  for(auto value:{s.structural_occurrence_id,s.descriptor_generation,s.instance_generation,s.catalog_generation,
                 s.security_generation,s.policy_generation,s.availability_generation,s.cleanup_sequence,s.reclaimed_pages})
    AppendBinaryU64(&bytes,value);
  AppendBinaryU8(&bytes,s.retention);AppendBinaryU8(&bytes,s.trigger);AppendBinaryU8(&bytes,static_cast<std::uint8_t>(s.state));
  std::ofstream output(c.database_path+".sb.sblr_temporary_instance.v2",std::ios::app|std::ios::binary);
  if(!output)return false;
  output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));output.flush();return static_cast<bool>(output);
}
// Runtime instance bookkeeping only; MGA inventory retains transaction authority.
bool owns(const EngineRequestContext& c,const SblrTemporaryInstanceSnapshot& s) {
  return s.database_uuid==c.database_uuid&&s.owner_session_uuid==c.session_uuid&&
      (s.owner_transaction_uuid.is_nil()||s.owner_transaction_uuid==c.transaction_uuid);
}
std::string identity_hash(std::string_view domain,std::initializer_list<EngineUuid> identities,std::uint64_t sequence=0) {
  std::string material(domain);material.push_back('\0');
  for(const auto& identity:identities)material.append(reinterpret_cast<const char*>(identity.bytes.data()),16);
  AppendBinaryU64(&material,sequence);return hash(material);
}
}

SblrTemporaryInstanceResult PublishSblrTemporaryInstance(const EngineRequestContext&c,const EngineUuid&receipt,std::uint64_t occurrence,std::uint8_t retention){std::lock_guard l(m);SblrTemporaryInstanceResult o;if(!tag(c,"private_temporary_instance_compiler")||!c.statement_metadata_snapshot_engine_owned){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.temporary_instance.compiler_hidden");return o;}if(!scratchbird::core::uuid::IsEngineIdentityUuid(c.database_uuid)||!scratchbird::core::uuid::IsEngineIdentityUuid(receipt)||(retention==1&&!scratchbird::core::uuid::IsEngineIdentityUuid(c.transaction_uuid))||receipt!=c.statement_uuid||!occurrence||(retention!=1&&retention!=2)||c.session_uuid.is_nil()){o.diagnostic=D("SBLR.OPERAND_INVALID","sblr.temporary_instance.publish_invalid");return o;}SblrTemporaryInstanceSnapshot s;s.database_uuid=c.database_uuid;s.receipt_uuid=receipt;s.structural_occurrence_id=occurrence;s.descriptor_uuid=id();s.definition_uuid=id();s.instance_uuid=id();s.owner_session_uuid=c.session_uuid;s.owner_transaction_uuid=retention==1?c.transaction_uuid:EngineUuid{};s.descriptor_generation=++generation;s.instance_generation=++generation;s.catalog_generation=std::max<std::uint64_t>(1,c.statement_metadata_snapshot_visible_through_local_transaction_id);s.security_generation=std::max<std::uint64_t>(1,c.local_transaction_id);s.policy_generation=++generation;s.retention=retention;s.descriptor_evidence_sha256=identity_hash("ScratchBird.SblrTemporaryInstanceCleanupDescriptor.V2",{s.descriptor_uuid,s.instance_uuid});if(!journal(c,s,'P')){o.diagnostic=D("TEMP.TABLE.CLEANUP_FAILED","sblr.temporary_instance.publish_failed");return o;}rows[s.descriptor_uuid]=s;o.ok=true;o.snapshot=s;o.diagnostic=D("OK","ok");return o;}
SblrTemporaryInstanceResult CoordinateSblrTemporaryInstanceCleanup(const EngineRequestContext&c,const EngineUuid&receipt,std::uint64_t occurrence,std::uint8_t trigger,std::uint64_t availability){std::lock_guard l(m);SblrTemporaryInstanceResult o;if(!tag(c,"private_temporary_instance_cleanup")){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.temporary_instance.hidden");return o;}for(auto&[_,stored]:rows)if(stored.receipt_uuid==receipt&&stored.structural_occurrence_id==occurrence&&owns(c,stored)){auto s=stored;if(s.state!=SblrTemporaryInstanceState::active){o.diagnostic=D("TEMP.TABLE.INSTANCE_STALE","sblr.temporary_instance.stale");return o;}if((s.retention==1&&trigger!=1)||(s.retention==2&&trigger!=2)){o.diagnostic=D("TEMP.TABLE.INVALID_RETENTION_MODE","sblr.temporary_instance.retention_mismatch");return o;}s.trigger=trigger;s.availability_generation=availability;scratchbird::engine::sblr::SblrTemporaryInstanceCleanupDescriptorV1 d;d.descriptor=s.descriptor_uuid.bytes;d.definition=s.definition_uuid.bytes;d.instance=s.instance_uuid.bytes;d.owner_session=s.owner_session_uuid.bytes;d.owner_transaction=s.owner_transaction_uuid.bytes;d.descriptor_generation=s.descriptor_generation;d.instance_generation=s.instance_generation;d.retention=s.retention;d.trigger=s.trigger;d.state=1;d.catalog_generation=s.catalog_generation;d.security_generation=s.security_generation;d.policy_generation=s.policy_generation;d.availability_generation=s.availability_generation;const auto bytes=scratchbird::engine::sblr::EncodeSblrTemporaryInstanceCleanupDescriptorV1(d);if(bytes.size()!=184){o.diagnostic=D("TEMP.TABLE.CLEANUP_FAILED","sblr.temporary_instance.evidence_failed");return o;}s.descriptor_evidence_sha256="sha256:";static constexpr char hx[]="0123456789abcdef";for(size_t i=144;i<176;++i){s.descriptor_evidence_sha256.push_back(hx[bytes[i]>>4]);s.descriptor_evidence_sha256.push_back(hx[bytes[i]&15]);}stored=s;o.ok=true;o.snapshot=s;o.diagnostic=D("OK","ok");return o;}o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.temporary_instance.hidden");return o;}
SblrTemporaryInstanceResult CleanupSblrTemporaryInstance(const EngineRequestContext&c,const EngineUuid&descriptor,std::uint64_t generation_id,const std::string&evidence,std::uint64_t availability){std::lock_guard l(m);SblrTemporaryInstanceResult o;if(!tag(c,"private_temporary_instance_cleanup")){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.temporary_instance.hidden");return o;}auto it=rows.find(descriptor);if(it==rows.end()||!owns(c,it->second)){o.diagnostic=D("SECURITY.ACCESS_DENIED","sblr.temporary_instance.hidden");return o;}auto s=it->second;if(s.state!=SblrTemporaryInstanceState::active||s.descriptor_generation!=generation_id||s.descriptor_evidence_sha256!=evidence||s.availability_generation!=availability){o.diagnostic=D("TEMP.TABLE.INSTANCE_STALE","sblr.temporary_instance.stale");return o;}s.state=SblrTemporaryInstanceState::cleaned;s.cleanup_sequence=++generation;s.cleanup_evidence_sha256=identity_hash("ScratchBird.SblrTemporaryInstanceCleanup.V2",{s.instance_uuid},s.cleanup_sequence);if(!journal(c,s,'C')){o.diagnostic=D("TEMP.TABLE.CLEANUP_FAILED","sblr.temporary_instance.cleanup_failed");return o;}it->second=s;o.ok=true;o.snapshot=s;o.diagnostic=D("OK","ok");return o;}
EngineApiDiagnostic RecoverSblrTemporaryInstances(const EngineRequestContext& c) {
  std::lock_guard lock(m);
  if(!tag(c,"right:SBLR_TEMPORARY_INSTANCE_ADMIN")||!scratchbird::core::uuid::IsEngineIdentityUuid(c.database_uuid))
    return D("SECURITY.ACCESS_DENIED","sblr.temporary_instance.recovery_denied");
  std::vector<SblrTemporaryInstanceSnapshot> staged;
  for(const auto& [identity,snapshot]:rows) {
    if(snapshot.database_uuid!=c.database_uuid||snapshot.state!=SblrTemporaryInstanceState::active)continue;
    auto candidate=snapshot;candidate.state=SblrTemporaryInstanceState::revoked;candidate.cleanup_sequence=++generation;
    staged.push_back(std::move(candidate));
  }
  for(const auto& candidate:staged)if(!journal(c,candidate,'X'))return D("TEMP.TABLE.CLEANUP_FAILED","sblr.temporary_instance.recovery_failed");
  for(auto& candidate:staged)rows[candidate.descriptor_uuid]=std::move(candidate);
  return D("OK","ok");
}
}
