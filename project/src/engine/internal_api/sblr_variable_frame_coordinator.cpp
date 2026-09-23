// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_variable_frame_coordinator.hpp"

#include "api_diagnostics.hpp"
#include "datatype_catalog_manifest.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string_view>
#include <map>
#include "mga_relation_store/mga_binary_identity_codec.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::string_view kMagic="SBVFC002";
constexpr std::string_view kDomain="ScratchBird.SblrVariableFrameCoordinator.V2";
constexpr std::string_view kMappingDomain="ScratchBird.SblrVariableFrameMappings.V2";
constexpr EngineUuid kBigintDescriptor{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd7,0x11}};
std::mutex g_mutex;std::map<EngineUuid,SblrVariableFrameSnapshot> g_live;
std::map<EngineUuid,std::uint64_t> g_high;std::atomic<std::uint64_t> g_handles{1};
EngineApiDiagnostic Diag(std::string c,std::string k,std::string d){return MakeEngineApiDiagnostic(std::move(c),std::move(k),std::move(d));}
EngineApiDiagnostic Ok(){return MakeEngineApiDiagnostic("OK","ok",{},false);}
bool Authority(const EngineRequestContext& c){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),"private_variable_frame_coordination")!=c.trace_tags.end();}
bool Admin(const EngineRequestContext& c){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),"right:SBLR_VARIABLE_FRAME_ADMIN")!=c.trace_tags.end();}
bool Uuid(const EngineUuid& s,scratchbird::core::platform::UuidKind){return core::uuid::IsEngineIdentityUuid(s);}
EngineUuid NewUuid(std::uint64_t salt){const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());auto v=scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object,now+salt);return v.ok()?v.value.value:EngineUuid{};}
std::string Hash(std::string_view v){auto d=scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const scratchbird::core::platform::byte*>(v.data()),v.size());return d.ok()?"sha256:"+scratchbird::core::hash::HexLower(d.digest):std::string{};}
bool HashValue(std::string_view v){return v.size()==71&&v.substr(0,7)=="sha256:"&&std::all_of(v.begin()+7,v.end(),[](char c){return std::isdigit(static_cast<unsigned char>(c))||(c>='a'&&c<='f');});}
bool Reason(std::string_view v){return !v.empty()&&v.size()<=128&&std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isalnum(c)||c=='.'||c=='_'||c==':'||c=='-';});}
void Field(std::string* o,std::string_view v){AppendBinaryString(o,v);}
void Field(std::string* o,const EngineUuid& v){o->append(reinterpret_cast<const char*>(v.bytes.data()),16);}
std::string MappingMaterial(const std::vector<SblrVariableFrameMapping>& m){std::string o(kMappingDomain);for(const auto& x:m){Field(&o,std::to_string(x.declaration_occurrence_id));Field(&o,std::to_string(x.descriptor.variable_ordinal));Field(&o,x.descriptor.variable_descriptor_uuid);Field(&o,std::to_string(x.descriptor.variable_descriptor_generation));Field(&o,x.descriptor.datatype_descriptor_uuid);Field(&o,std::to_string(x.descriptor.datatype_descriptor_generation));Field(&o,x.datatype_type_uuid);Field(&o,std::to_string(x.descriptor.value_generation));}return o;}
std::string Material(const SblrVariableFrameSnapshot&s,std::uint64_t prior,std::string_view reason){std::string o(kDomain);for(const auto&v:{s.public_coordination_uuid,s.operation_uuid,s.database_uuid,s.session_uuid,s.transaction_uuid,s.statement_receipt_uuid,s.scope_uuid,s.frame_uuid,s.registry_snapshot_uuid})Field(&o,v);Field(&o,s.mapping_sha256);for(auto n:{s.scope_generation,s.frame_generation,s.coordinator_generation,s.registry_generation,prior,static_cast<std::uint64_t>(s.state)})Field(&o,std::to_string(n));Field(&o,reason);return o;}
std::string Path(const EngineRequestContext&c){return c.database_path+".sb.sblr_variable_frame_coordinator.v2";}
constexpr std::size_t kMaxRecordSize=4096;
std::string Record(std::string_view kind,const SblrVariableFrameSnapshot& s,std::uint64_t prior,std::string_view reason){
  std::string out(kMagic);AppendBinaryU8(&out,kind=="EVIDENCE"?1:2);
  if(!AppendBinaryEngineUuid(&out,s.public_coordination_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.operation_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.database_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.session_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.transaction_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.statement_receipt_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.scope_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.frame_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,s.registry_snapshot_uuid))return {};
  AppendBinaryU64(&out,static_cast<std::uint64_t>(s.scope_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(s.frame_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(s.coordinator_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(s.registry_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(s.state));
  AppendBinaryU64(&out,prior);
  if(!AppendBinaryString(&out,s.mapping_sha256))return {};
  if(!AppendBinaryString(&out,s.decision_evidence_sha256))return {};
  if(!AppendBinaryString(&out,reason)||out.size()>kMaxRecordSize)return {};
  std::string framed;AppendBinaryU32(&framed,static_cast<std::uint32_t>(out.size()));framed+=out;return framed;
}
bool Append(const std::string&p,const std::string&line){{std::ofstream o(p,std::ios::binary|std::ios::app);if(!o||line.empty())return false;o.write(line.data(),static_cast<std::streamsize>(line.size()));o.flush();if(!o)return false;}
#if defined(_WIN32)
HANDLE h=CreateFileA(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;bool ok=FlushFileBuffers(h)!=0;CloseHandle(h);return ok;
#else
int fd=::open(p.c_str(),O_WRONLY|O_CLOEXEC);if(fd<0)return false;bool ok=::fsync(fd)==0;::close(fd);return ok;
#endif
}
bool Publish(const EngineRequestContext&c,const SblrVariableFrameSnapshot&s,std::uint64_t prior,std::string_view reason){return Append(Path(c),Record("EVIDENCE",s,prior,reason))&&Append(Path(c),Record("SNAPSHOT",s,prior,reason));}
bool ReadRecord(std::istream& in,std::string* out){
  std::array<std::uint8_t,4> length{};
  if(!in.read(reinterpret_cast<char*>(length.data()),4))return false;
  std::size_t offset=0;std::uint32_t size=0;
  if(!ReadBinaryU32(length,&offset,&size)||size<9||size>kMaxRecordSize)return false;
  std::string candidate(size,'\0');
  if(!in.read(candidate.data(),size))return false;
  out->swap(candidate);return true;
}
bool Decode(const std::string& line,std::string_view kind,SblrVariableFrameSnapshot* s,std::uint64_t* prior,std::string* reason){
  if(line.size()<9||!line.starts_with(kMagic)||static_cast<unsigned char>(line[8])!=(kind=="EVIDENCE"?1:2))return false;
  std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(line.data()),line.size());std::size_t offset=9;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->public_coordination_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->operation_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->database_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->session_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->transaction_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->statement_receipt_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->scope_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->frame_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&s->registry_snapshot_uuid))return false;
  std::uint64_t scope_generation=0;if(!ReadBinaryU64(bytes,&offset,&scope_generation))return false;
  std::uint64_t frame_generation=0;if(!ReadBinaryU64(bytes,&offset,&frame_generation))return false;
  std::uint64_t coordinator_generation=0;if(!ReadBinaryU64(bytes,&offset,&coordinator_generation))return false;
  std::uint64_t registry_generation=0;if(!ReadBinaryU64(bytes,&offset,&registry_generation))return false;
  std::uint64_t state=0;if(!ReadBinaryU64(bytes,&offset,&state))return false;
  if(state<1||state>3)return false;
  s->scope_generation=static_cast<decltype(s->scope_generation)>(scope_generation);
  s->frame_generation=static_cast<decltype(s->frame_generation)>(frame_generation);
  s->coordinator_generation=static_cast<decltype(s->coordinator_generation)>(coordinator_generation);
  s->registry_generation=static_cast<decltype(s->registry_generation)>(registry_generation);
  s->state=static_cast<decltype(s->state)>(state);
  if(!ReadBinaryU64(bytes,&offset,prior))return false;
  if(!ReadBinaryString(bytes,&offset,&s->mapping_sha256))return false;
  if(!ReadBinaryString(bytes,&offset,&s->decision_evidence_sha256))return false;
  if(!ReadBinaryString(bytes,&offset,reason)||offset!=bytes.size())return false;
  return Uuid(s->public_coordination_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(s->operation_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(s->database_uuid,scratchbird::core::platform::UuidKind::database)&&Uuid(s->session_uuid,scratchbird::core::platform::UuidKind::session)&&Uuid(s->transaction_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(s->statement_receipt_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(s->scope_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(s->frame_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(s->registry_snapshot_uuid,scratchbird::core::platform::UuidKind::object)&&s->scope_generation&&s->frame_generation&&s->coordinator_generation&&s->registry_generation&&HashValue(s->mapping_sha256)&&Reason(*reason)&&s->decision_evidence_sha256==Hash(Material(*s,*prior,*reason));}
bool Replay(const EngineRequestContext&c,std::map<EngineUuid,SblrVariableFrameSnapshot>*states,std::uint64_t*high){states->clear();*high=0;std::ifstream in(Path(c),std::ios::binary);if(!in)return !std::filesystem::exists(Path(c))&&!std::filesystem::exists(c.database_path+".sb.sblr_variable_frame_coordinator.v1");std::vector<std::string>lines;while(in.peek()!=std::char_traits<char>::eof()){std::string line;if(!ReadRecord(in,&line))return false;lines.push_back(std::move(line));}if(in.bad()||lines.size()%2)return false;for(std::size_t i=0;i<lines.size();i+=2){SblrVariableFrameSnapshot e,s;std::uint64_t ep=0,sp=0;std::string er,sr;if(!Decode(lines[i],"EVIDENCE",&e,&ep,&er)||!Decode(lines[i+1],"SNAPSHOT",&s,&sp,&sr)||lines[i].substr(9)!=lines[i+1].substr(9)||ep!=sp||er!=sr||s.database_uuid!=c.database_uuid||s.coordinator_generation<=*high)return false;auto it=states->find(s.public_coordination_uuid);if((it==states->end()&&(sp!=0||s.state!=SblrVariableFrameState::active))||(it!=states->end()&&(sp!=it->second.coordinator_generation||s.operation_uuid!=it->second.operation_uuid||s.scope_uuid!=it->second.scope_uuid||s.frame_uuid!=it->second.frame_uuid||s.registry_snapshot_uuid!=it->second.registry_snapshot_uuid)))return false;(*states)[s.public_coordination_uuid]=s;*high=s.coordinator_generation;}return true;}
std::uint64_t Next(const EngineUuid&db){auto&n=g_high[db];return n==UINT64_MAX?0:++n;}
bool Bound(const EngineRequestContext&c,const SblrVariableFrameSnapshot&s,const EngineUuid&op){return s.database_uuid==c.database_uuid&&s.session_uuid==c.session_uuid&&s.transaction_uuid==c.transaction_uuid&&s.operation_uuid==op;}
SblrVariableFrameResult Hidden(){SblrVariableFrameResult r;r.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.variable_frame.hidden","variable frame coordination is not visible");return r;}
} // namespace

SblrVariableFrameResult BeginSblrVariableFrame(const EngineRequestContext&c,const EngineUuid&operation,std::uint64_t expires,const std::vector<SblrVariableFrameDemand>&demands){std::lock_guard lock(g_mutex);SblrVariableFrameResult out;if(!Authority(c))return Hidden();if(c.database_path.empty()||!Uuid(operation,scratchbird::core::platform::UuidKind::object)||!Uuid(c.statement_uuid,scratchbird::core::platform::UuidKind::object)||!Uuid(c.transaction_uuid,scratchbird::core::platform::UuidKind::object)||!expires||demands.empty()||demands.size()>4096){out.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.variable_frame.begin_invalid","exact structural begin demands required");return out;}auto lookup=scratchbird::core::datatypes::LookupDatatypeTypeCodecIdentityV1(c.catalog_epoch_uuid,c.catalog_generation_id,1,kBigintDescriptor,1);if(!lookup.ok){out.diagnostic=Diag("DATATYPE.DESCRIPTOR.INVALID","sblr.variable_frame.bigint_registry_invalid","exact live context code 1 bigint row required");return out;}auto&high=g_high[c.database_uuid];if(high==0&&(std::filesystem::exists(Path(c))||std::filesystem::exists(c.database_path+".sb.sblr_variable_frame_coordinator.v1"))){std::map<EngineUuid,SblrVariableFrameSnapshot>states;std::uint64_t recovered_high=0;if(!Replay(c,&states,&recovered_high)){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable_frame.recovery_required","coordinator recovery required");return out;}for(const auto&[id,s]:states)if(s.state!=SblrVariableFrameState::revoked){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable_frame.recovery_required","live recovered frame must be revoked");return out;}high=recovered_high;}
std::vector<SblrVariableDemand> registry_demands;for(const auto&d:demands){if(!d.declaration_occurrence_id||d.datatype_context_code!=1||!HashValue(d.declaration_token_sha256)||(d.initial_state!=SblrVariableValueState::null_value&&d.initial_state!=SblrVariableValueState::uninitialized)||(d.initial_state==SblrVariableValueState::null_value&&!d.nullable)){out.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.variable_frame.demand_invalid","canonical structural demand required");return out;}registry_demands.push_back({lookup.row.descriptor_uuid,lookup.row.descriptor_generation,d.nullable,d.mutability,d.initial_state,{}});}SblrVariableFrameSnapshot s;s.public_coordination_uuid=NewUuid(high+1);s.operation_uuid=operation;s.database_uuid=c.database_uuid;s.session_uuid=c.session_uuid;s.transaction_uuid=c.transaction_uuid;s.statement_receipt_uuid=c.statement_uuid;s.scope_uuid=NewUuid(high+2);s.scope_generation=Next(s.database_uuid);s.frame_uuid=NewUuid(high+3);s.frame_generation=Next(s.database_uuid);s.registry_snapshot_uuid=NewUuid(high+4);s.coordinator_generation=Next(s.database_uuid);s.private_handle=g_handles.fetch_add(1);s.state=SblrVariableFrameState::active;
auto registry_context=c;registry_context.trace_tags={"private_variable_registry","canonical_datatype_value_validated"};auto published=PublishSblrVariableFrame(registry_context,s.statement_receipt_uuid,s.scope_uuid,s.scope_generation,s.frame_uuid,s.frame_generation,registry_demands);if(!published.ok){out.diagnostic=published.diagnostic;return out;}for(std::size_t i=0;i<published.rows.size();++i)s.mappings.push_back({demands[i].declaration_occurrence_id,published.rows[i],lookup.row.type_uuid});s.registry_generation=published.rows.back().registry_generation;s.mapping_sha256=Hash(MappingMaterial(s.mappings));s.decision_evidence_sha256=Hash(Material(s,0,"frame.begin"));if(s.public_coordination_uuid.is_nil()||s.scope_uuid.is_nil()||s.frame_uuid.is_nil()||s.registry_snapshot_uuid.is_nil()||!Publish(c,s,0,"frame.begin")){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable_frame.publish_failed","durable coordinator publication failed");return out;}g_live[s.public_coordination_uuid]=s;out.ok=true;out.snapshot=s;out.diagnostic=Ok();out.evidence.push_back({"sblr.variable_frame.begin",s.decision_evidence_sha256});return out;}

SblrVariableFrameResult AcquireSblrVariableFrame(const EngineRequestContext&c,const EngineUuid&id,const EngineUuid&operation,std::uint64_t expected){std::lock_guard lock(g_mutex);if(!Authority(c))return Hidden();auto it=g_live.find(id);if(it==g_live.end()||!Bound(c,it->second,operation))return Hidden();SblrVariableFrameResult out;if(it->second.state!=SblrVariableFrameState::active||it->second.coordinator_generation!=expected){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable_frame.acquire_stale","coordinator compare failed");return out;}auto s=it->second;auto prior=s.coordinator_generation;s.coordinator_generation=Next(s.database_uuid);s.state=SblrVariableFrameState::acquired;s.decision_evidence_sha256=Hash(Material(s,prior,"frame.acquire"));if(!s.coordinator_generation||!Publish(c,s,prior,"frame.acquire")){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable_frame.acquire_failed","durable acquire failed");return out;}g_live[id]=s;out.ok=true;out.snapshot=s;out.diagnostic=Ok();out.evidence.push_back({"sblr.variable_frame.acquire",s.decision_evidence_sha256});return out;}

SblrVariableFrameResult AssignSblrVariableFrameValues(const EngineRequestContext&c,const EngineUuid&id,const EngineUuid&operation,const EngineUuid&receipt,std::uint64_t expected_coordinator,std::uint64_t expected_registry,const std::vector<SblrVariableAssignment>& assignments){
  std::lock_guard lock(g_mutex);if(!Authority(c))return Hidden();auto it=g_live.find(id);if(it==g_live.end()||!Bound(c,it->second,operation))return Hidden();SblrVariableFrameResult out;
  if(it->second.state!=SblrVariableFrameState::acquired||it->second.statement_receipt_uuid!=receipt||it->second.coordinator_generation!=expected_coordinator||it->second.registry_generation!=expected_registry){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable_assignment.frame_stale","acquired frame snapshot changed");return out;}
  auto registry_context=c;registry_context.trace_tags={"private_variable_registry","canonical_datatype_value_validated"};const auto assigned=AssignSblrVariableBatch(registry_context,receipt,it->second.scope_uuid,it->second.scope_generation,it->second.frame_uuid,it->second.frame_generation,assignments);if(!assigned.ok){out.diagnostic=assigned.diagnostic;return out;}
  auto s=it->second;for(const auto&row:assigned.rows){auto m=std::find_if(s.mappings.begin(),s.mappings.end(),[&](const auto&x){return x.descriptor.variable_descriptor_uuid==row.variable_descriptor_uuid;});if(m==s.mappings.end()){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable_assignment.mapping_stale","assigned descriptor is absent from frame mapping");return out;}m->descriptor=row;}const auto prior=s.coordinator_generation;s.registry_generation=assigned.rows.back().registry_generation;s.coordinator_generation=Next(s.database_uuid);s.mapping_sha256=Hash(MappingMaterial(s.mappings));s.decision_evidence_sha256=Hash(Material(s,prior,"frame.assign"));if(!s.coordinator_generation||!Publish(c,s,prior,"frame.assign")){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable_assignment.coordinator_publish_failed","durable coordinator assignment publication failed");return out;}g_live[id]=s;out.ok=true;out.snapshot=s;out.diagnostic=Ok();out.evidence=assigned.evidence;out.evidence.push_back({"sblr.variable_frame.assign",s.decision_evidence_sha256});return out;
}

SblrVariableFrameResult CloseSblrVariableFrame(const EngineRequestContext&c,const EngineUuid&id,const EngineUuid&operation,std::uint64_t expected_frame,const std::string&reason){std::lock_guard lock(g_mutex);if(!Authority(c))return Hidden();auto it=g_live.find(id);if(it==g_live.end()||!Bound(c,it->second,operation))return Hidden();SblrVariableFrameResult out;if(it->second.frame_generation!=expected_frame||it->second.state==SblrVariableFrameState::revoked||!Reason(reason)){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable_frame.close_stale","frame compare failed");return out;}auto s=it->second;auto registry_context=c;registry_context.trace_tags={"private_variable_registry","canonical_datatype_value_validated"};auto revoked=RevokeSblrVariableFrame(registry_context,s.statement_receipt_uuid,s.scope_uuid,s.scope_generation,s.frame_uuid,s.frame_generation,reason);if(revoked.code!="OK"){out.diagnostic=revoked;return out;}auto prior=s.coordinator_generation;s.frame_generation=Next(s.database_uuid);s.coordinator_generation=Next(s.database_uuid);s.state=SblrVariableFrameState::revoked;s.private_handle=0;s.decision_evidence_sha256=Hash(Material(s,prior,reason));if(!Publish(c,s,prior,reason)){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable_frame.close_failed","durable close failed");return out;}g_live.erase(it);out.ok=true;out.snapshot=s;out.diagnostic=Ok();out.evidence.push_back({"sblr.variable_frame.close",s.decision_evidence_sha256});return out;}

EngineApiDiagnostic RecoverSblrVariableFrameCoordinator(const EngineRequestContext&c){std::lock_guard lock(g_mutex);if(!Admin(c))return Diag("SECURITY.ACCESS_DENIED","sblr.variable_frame.recovery_denied","startup frame authority required");std::map<EngineUuid,SblrVariableFrameSnapshot>states;std::uint64_t high=0;if(!Replay(c,&states,&high))return Diag("SBLR.VARIABLE.STALE","sblr.variable_frame.corrupt","torn or contradictory coordinator evidence");g_high[c.database_uuid]=high;auto registry_context=c;registry_context.trace_tags={"right:SBLR_VARIABLE_REGISTRY_ADMIN"};auto registry_recovery=RecoverSblrVariableDescriptorRegistry(registry_context);if(registry_recovery.code!="OK")return registry_recovery;std::vector<EngineUuid>ids;for(const auto&[id,s]:states)if(s.state!=SblrVariableFrameState::revoked)ids.push_back(id);std::sort(ids.begin(),ids.end(),[&](const auto&a,const auto&b){return states[a].coordinator_generation<states[b].coordinator_generation;});for(const auto&id:ids){auto s=states[id];auto prior=s.coordinator_generation;s.frame_generation=Next(s.database_uuid);s.coordinator_generation=Next(s.database_uuid);s.state=SblrVariableFrameState::revoked;s.private_handle=0;s.decision_evidence_sha256=Hash(Material(s,prior,"recovery.revoke"));if(!Publish(c,s,prior,"recovery.revoke"))return Diag("SBLR.EXECUTION_FAILED","sblr.variable_frame.recovery_publish_failed","durable recovery revoke failed");}for(auto it=g_live.begin();it!=g_live.end();)if(it->second.database_uuid==c.database_uuid)it=g_live.erase(it);else++it;return Ok();}
} // namespace scratchbird::engine::internal_api
