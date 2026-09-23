// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_variable_descriptor_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
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
constexpr std::string_view kMagic="SBVDR002";
constexpr std::string_view kRowDomain="ScratchBird.SblrVariableDescriptorRow.V2";
constexpr std::string_view kEvidenceDomain="ScratchBird.SblrVariableDescriptorRegistry.V2";
std::mutex g_mutex;
std::map<EngineUuid,SblrVariableDescriptorRow> g_live;
std::map<EngineUuid,std::uint64_t> g_high;

EngineApiDiagnostic Diag(std::string c,std::string k,std::string d){return MakeEngineApiDiagnostic(std::move(c),std::move(k),std::move(d));}
EngineApiDiagnostic Ok(){return MakeEngineApiDiagnostic("OK","ok",{},false);}
bool Authority(const EngineRequestContext& c){return c.security_context_present&&c.statement_metadata_snapshot_engine_owned&&std::find(c.trace_tags.begin(),c.trace_tags.end(),"private_variable_registry")!=c.trace_tags.end();}
bool Admin(const EngineRequestContext& c){return c.security_context_present&&std::find(c.trace_tags.begin(),c.trace_tags.end(),"right:SBLR_VARIABLE_REGISTRY_ADMIN")!=c.trace_tags.end();}
bool CanonicalAuthority(const EngineRequestContext& c){return Authority(c)&&std::find(c.trace_tags.begin(),c.trace_tags.end(),"canonical_datatype_value_validated")!=c.trace_tags.end();}
bool Uuid(const EngineUuid& s, scratchbird::core::platform::UuidKind){return core::uuid::IsEngineIdentityUuid(s);}
EngineUuid NewUuid(std::uint64_t salt){const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());const auto v=scratchbird::core::uuid::GenerateEngineIdentityV7(scratchbird::core::platform::UuidKind::object,now+salt);return v.ok()?v.value.value:EngineUuid{};}
std::string Hash(std::string_view v){const auto d=scratchbird::core::hash::ComputeSha256Digest(reinterpret_cast<const scratchbird::core::platform::byte*>(v.data()),v.size());return d.ok()?"sha256:"+scratchbird::core::hash::HexLower(d.digest):std::string{};}
bool Reason(std::string_view v){return !v.empty()&&v.size()<=128&&std::all_of(v.begin(),v.end(),[](unsigned char c){return std::isalnum(c)||c=='.'||c=='_'||c==':'||c=='-';});}
std::string Path(const EngineRequestContext& c){return c.database_path+".sb.sblr_variable_registry.v2";}
void Field(std::string* out,std::string_view v){AppendBinaryString(out,v);}
void Field(std::string* out,const EngineUuid& v){out->append(reinterpret_cast<const char*>(v.bytes.data()),16);}
std::string Immutable(const SblrVariableDescriptorRow& r){std::string v(kRowDomain);for(const auto& x:{r.variable_descriptor_uuid,r.statement_receipt_uuid,r.database_uuid,r.session_uuid,r.transaction_uuid,r.scope_uuid,r.frame_uuid,r.datatype_descriptor_uuid})Field(&v,x);for(auto n:{r.variable_descriptor_generation,r.scope_generation,r.frame_generation,static_cast<std::uint64_t>(r.variable_ordinal),r.datatype_descriptor_generation,static_cast<std::uint64_t>(r.nullable),static_cast<std::uint64_t>(r.mutability)})Field(&v,std::to_string(n));return v;}
std::string Material(const SblrVariableDescriptorRow& r,std::uint64_t prior,std::string_view reason){std::string v(kEvidenceDomain);Field(&v,r.row_identity_sha256);Field(&v,r.canonical_value_sha256);Field(&v,r.canonical_value_bytes);Field(&v,std::to_string(r.value_generation));Field(&v,std::to_string(r.registry_generation));Field(&v,std::to_string(prior));Field(&v,std::to_string(static_cast<unsigned>(r.value_state)));Field(&v,std::to_string(static_cast<unsigned>(r.lifecycle)));Field(&v,reason);return v;}
constexpr std::size_t kMaxRecordSize=64*1024*1024;
std::string Record(std::string_view kind,const SblrVariableDescriptorRow& r,std::uint64_t prior,std::string_view reason){
  std::string out(kMagic);AppendBinaryU8(&out,kind=="EVIDENCE"?1:2);
  if(!AppendBinaryEngineUuid(&out,r.variable_descriptor_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.statement_receipt_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.database_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.session_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.transaction_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.scope_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.frame_uuid))return {};
  if(!AppendBinaryEngineUuid(&out,r.datatype_descriptor_uuid))return {};
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.variable_descriptor_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.scope_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.frame_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.variable_ordinal));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.datatype_descriptor_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.nullable));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.mutability));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.value_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.value_state));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.registry_generation));
  AppendBinaryU64(&out,static_cast<std::uint64_t>(r.lifecycle));
  AppendBinaryU64(&out,prior);
  if(!AppendBinaryString(&out,r.canonical_value_bytes))return {};
  if(!AppendBinaryString(&out,r.canonical_value_sha256))return {};
  if(!AppendBinaryString(&out,r.row_identity_sha256))return {};
  if(!AppendBinaryString(&out,r.decision_evidence_sha256))return {};
  if(!AppendBinaryString(&out,reason)||out.size()>kMaxRecordSize)return {};
  std::string framed;AppendBinaryU32(&framed,static_cast<std::uint32_t>(out.size()));framed+=out;return framed;
}
bool Append(const std::string& p,const std::string& line){{std::ofstream o(p,std::ios::binary|std::ios::app);if(!o||line.empty())return false;o.write(line.data(),static_cast<std::streamsize>(line.size()));o.flush();if(!o)return false;}
#if defined(_WIN32)
HANDLE h=CreateFileA(p.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;const bool ok=FlushFileBuffers(h)!=0;CloseHandle(h);return ok;
#else
const int fd=::open(p.c_str(),O_WRONLY|O_CLOEXEC);if(fd<0)return false;const bool ok=::fsync(fd)==0;::close(fd);return ok;
#endif
}
bool Publish(const EngineRequestContext& c,const SblrVariableDescriptorRow& r,std::uint64_t prior,std::string_view reason){return Append(Path(c),Record("EVIDENCE",r,prior,reason))&&Append(Path(c),Record("SNAPSHOT",r,prior,reason));}
bool PublishBatch(const EngineRequestContext& c,const std::vector<SblrVariableDescriptorRow>& rows,const std::vector<std::uint64_t>& priors){
  if(rows.empty()||rows.size()!=priors.size())return false;
  std::string block;
  for(std::size_t i=0;i<rows.size();++i){
    const auto evidence=Record("EVIDENCE",rows[i],priors[i],"value.assign.batch");
    const auto snapshot=Record("SNAPSHOT",rows[i],priors[i],"value.assign.batch");
    if(evidence.empty()||snapshot.empty())return false;
    block+=evidence;block+=snapshot;
  }
  if(block.empty())return false;
  // Append performs one durable write barrier for the canonical batch. Replay
  // rejects a torn odd/incomplete record set before restoring any row.
  return Append(Path(c),block);
}
bool ReadRecord(std::istream& in,std::string* out){
  std::array<std::uint8_t,4> length{};
  if(!in.read(reinterpret_cast<char*>(length.data()),4))return false;
  std::size_t offset=0;std::uint32_t size=0;
  if(!ReadBinaryU32(length,&offset,&size)||size<9||size>kMaxRecordSize)return false;
  std::string candidate(size,'\0');
  if(!in.read(candidate.data(),size))return false;
  out->swap(candidate);return true;
}
bool Decode(const std::string& line,std::string_view kind,SblrVariableDescriptorRow* r,std::uint64_t* prior,std::string* reason){
  if(line.size()<9||!line.starts_with(kMagic)||static_cast<unsigned char>(line[8])!=(kind=="EVIDENCE"?1:2))return false;
  std::span<const std::uint8_t> bytes(reinterpret_cast<const std::uint8_t*>(line.data()),line.size());std::size_t offset=9;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->variable_descriptor_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->statement_receipt_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->database_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->session_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->transaction_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->scope_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->frame_uuid))return false;
  if(!ReadBinaryEngineUuid(bytes,&offset,&r->datatype_descriptor_uuid))return false;
  std::uint64_t variable_descriptor_generation=0;if(!ReadBinaryU64(bytes,&offset,&variable_descriptor_generation))return false;
  std::uint64_t scope_generation=0;if(!ReadBinaryU64(bytes,&offset,&scope_generation))return false;
  std::uint64_t frame_generation=0;if(!ReadBinaryU64(bytes,&offset,&frame_generation))return false;
  std::uint64_t variable_ordinal=0;if(!ReadBinaryU64(bytes,&offset,&variable_ordinal))return false;
  std::uint64_t datatype_descriptor_generation=0;if(!ReadBinaryU64(bytes,&offset,&datatype_descriptor_generation))return false;
  std::uint64_t nullable=0;if(!ReadBinaryU64(bytes,&offset,&nullable))return false;
  std::uint64_t mutability=0;if(!ReadBinaryU64(bytes,&offset,&mutability))return false;
  std::uint64_t value_generation=0;if(!ReadBinaryU64(bytes,&offset,&value_generation))return false;
  std::uint64_t value_state=0;if(!ReadBinaryU64(bytes,&offset,&value_state))return false;
  std::uint64_t registry_generation=0;if(!ReadBinaryU64(bytes,&offset,&registry_generation))return false;
  std::uint64_t lifecycle=0;if(!ReadBinaryU64(bytes,&offset,&lifecycle))return false;
  if(variable_ordinal>UINT32_MAX||nullable>1||mutability>1||value_state<1||value_state>3||lifecycle<1||lifecycle>2)return false;
  r->variable_descriptor_generation=static_cast<decltype(r->variable_descriptor_generation)>(variable_descriptor_generation);
  r->scope_generation=static_cast<decltype(r->scope_generation)>(scope_generation);
  r->frame_generation=static_cast<decltype(r->frame_generation)>(frame_generation);
  r->variable_ordinal=static_cast<decltype(r->variable_ordinal)>(variable_ordinal);
  r->datatype_descriptor_generation=static_cast<decltype(r->datatype_descriptor_generation)>(datatype_descriptor_generation);
  r->nullable=static_cast<decltype(r->nullable)>(nullable);
  r->mutability=static_cast<decltype(r->mutability)>(mutability);
  r->value_generation=static_cast<decltype(r->value_generation)>(value_generation);
  r->value_state=static_cast<decltype(r->value_state)>(value_state);
  r->registry_generation=static_cast<decltype(r->registry_generation)>(registry_generation);
  r->lifecycle=static_cast<decltype(r->lifecycle)>(lifecycle);
  if(!ReadBinaryU64(bytes,&offset,prior))return false;
  if(!ReadBinaryString(bytes,&offset,&r->canonical_value_bytes))return false;
  if(!ReadBinaryString(bytes,&offset,&r->canonical_value_sha256))return false;
  if(!ReadBinaryString(bytes,&offset,&r->row_identity_sha256))return false;
  if(!ReadBinaryString(bytes,&offset,&r->decision_evidence_sha256))return false;
  if(!ReadBinaryString(bytes,&offset,reason)||offset!=bytes.size())return false;
  return Uuid(r->variable_descriptor_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(r->statement_receipt_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(r->database_uuid,scratchbird::core::platform::UuidKind::database)&&Uuid(r->session_uuid,scratchbird::core::platform::UuidKind::session)&&Uuid(r->scope_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(r->frame_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(r->datatype_descriptor_uuid,scratchbird::core::platform::UuidKind::object)&&Uuid(r->transaction_uuid,scratchbird::core::platform::UuidKind::object)&&r->variable_descriptor_generation&&r->scope_generation&&r->frame_generation&&r->datatype_descriptor_generation&&r->value_generation&&r->registry_generation&&Reason(*reason)&&r->canonical_value_sha256==Hash(r->canonical_value_bytes)&&r->row_identity_sha256==Hash(Immutable(*r))&&r->decision_evidence_sha256==Hash(Material(*r,*prior,*reason))&&((r->value_state==SblrVariableValueState::null_value||r->value_state==SblrVariableValueState::uninitialized)?r->canonical_value_bytes.empty():true)&&!(r->value_state==SblrVariableValueState::null_value&&!r->nullable);}
bool SameImmutable(const SblrVariableDescriptorRow&a,const SblrVariableDescriptorRow&b){return a.row_identity_sha256==b.row_identity_sha256;}
bool Replay(const EngineRequestContext& c,std::map<EngineUuid,SblrVariableDescriptorRow>* rows,std::uint64_t* high){rows->clear();*high=0;std::ifstream in(Path(c),std::ios::binary);if(!in)return !std::filesystem::exists(Path(c))&&!std::filesystem::exists(c.database_path+".sb.sblr_variable_registry.v1");std::vector<std::string> lines;while(in.peek()!=std::char_traits<char>::eof()){std::string line;if(!ReadRecord(in,&line))return false;lines.push_back(std::move(line));}if(in.bad()||lines.size()%2)return false;for(std::size_t i=0;i<lines.size();i+=2){SblrVariableDescriptorRow e,s;std::uint64_t ep=0,sp=0;std::string er,sr;if(!Decode(lines[i],"EVIDENCE",&e,&ep,&er)||!Decode(lines[i+1],"SNAPSHOT",&s,&sp,&sr)||lines[i].substr(9)!=lines[i+1].substr(9)||ep!=sp||er!=sr||s.database_uuid!=c.database_uuid||s.registry_generation<=*high)return false;auto it=rows->find(s.variable_descriptor_uuid);if((it==rows->end()&&(sp!=0||s.variable_descriptor_generation!=1||s.value_generation!=1||s.lifecycle!=SblrVariableLifecycle::active))||(it!=rows->end()&&(sp!=it->second.registry_generation||!SameImmutable(it->second,s)||s.value_generation!=it->second.value_generation+1)))return false;(*rows)[s.variable_descriptor_uuid]=s;*high=s.registry_generation;}return true;}
std::uint64_t Next(const EngineUuid& db){auto& n=g_high[db];return n==UINT64_MAX?0:++n;}
bool Bound(const EngineRequestContext& c,const SblrVariableDescriptorRow&r,const EngineUuid& receipt,const EngineUuid& scope,std::uint64_t sg,const EngineUuid& frame,std::uint64_t fg){return r.database_uuid==c.database_uuid&&r.session_uuid==c.session_uuid&&r.transaction_uuid==c.transaction_uuid&&r.statement_receipt_uuid==receipt&&r.scope_uuid==scope&&r.scope_generation==sg&&r.frame_uuid==frame&&r.frame_generation==fg;}
SblrVariableRegistryResult Hidden(){SblrVariableRegistryResult r;r.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.variable.hidden","variable descriptor is not visible");return r;}
} // namespace

SblrVariableRegistryResult PublishSblrVariableFrame(const EngineRequestContext& c,const EngineUuid& receipt,const EngineUuid& scope,std::uint64_t sg,const EngineUuid& frame,std::uint64_t fg,const std::vector<SblrVariableDemand>& demands){std::lock_guard lock(g_mutex);SblrVariableRegistryResult out;if(!CanonicalAuthority(c))return Hidden();if(c.database_path.empty()||!Uuid(c.database_uuid,scratchbird::core::platform::UuidKind::database)||!Uuid(c.session_uuid,scratchbird::core::platform::UuidKind::session)||!Uuid(c.transaction_uuid,scratchbird::core::platform::UuidKind::object)||!Uuid(receipt,scratchbird::core::platform::UuidKind::object)||!Uuid(scope,scratchbird::core::platform::UuidKind::object)||!Uuid(frame,scratchbird::core::platform::UuidKind::object)||!sg||!fg||demands.empty()||demands.size()>4096){out.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.variable.frame_invalid","exact nonempty frame demands required");return out;}auto& high=g_high[c.database_uuid];if(high==0&&(std::filesystem::exists(Path(c))||std::filesystem::exists(c.database_path+".sb.sblr_variable_registry.v1"))){std::map<EngineUuid,SblrVariableDescriptorRow> rows;std::uint64_t recovered_high=0;if(!Replay(c,&rows,&recovered_high)){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable.recovery_required","registry recovery required");return out;}for(const auto&[id,r]:rows)if(r.lifecycle==SblrVariableLifecycle::active){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable.recovery_required","live recovered frame must be revoked");return out;}high=recovered_high;}
for(std::size_t i=0;i<demands.size();++i){const auto&d=demands[i];if(!Uuid(d.datatype_descriptor_uuid,scratchbird::core::platform::UuidKind::object)||!d.datatype_descriptor_generation||static_cast<unsigned>(d.mutability)>1||static_cast<unsigned>(d.initial_state)<1||static_cast<unsigned>(d.initial_state)>3||(d.initial_state==SblrVariableValueState::null_value&&!d.nullable)||((d.initial_state==SblrVariableValueState::null_value||d.initial_state==SblrVariableValueState::uninitialized)&&!d.canonical_value_bytes.empty())){out.diagnostic=Diag("DATATYPE.DESCRIPTOR.INVALID","sblr.variable.demand_invalid","canonical datatype value demand required");return out;}SblrVariableDescriptorRow r;r.variable_descriptor_uuid=NewUuid(high+i+1);r.variable_descriptor_generation=1;r.statement_receipt_uuid=receipt;r.database_uuid=c.database_uuid;r.session_uuid=c.session_uuid;r.transaction_uuid=c.transaction_uuid;r.scope_uuid=scope;r.scope_generation=sg;r.frame_uuid=frame;r.frame_generation=fg;r.variable_ordinal=static_cast<std::uint32_t>(i);r.datatype_descriptor_uuid=d.datatype_descriptor_uuid;r.datatype_descriptor_generation=d.datatype_descriptor_generation;r.nullable=d.nullable;r.mutability=d.mutability;r.value_generation=1;r.value_state=d.initial_state;r.canonical_value_bytes=d.canonical_value_bytes;r.canonical_value_sha256=Hash(r.canonical_value_bytes);r.row_identity_sha256=Hash(Immutable(r));r.registry_generation=Next(r.database_uuid);r.lifecycle=SblrVariableLifecycle::active;r.decision_evidence_sha256=Hash(Material(r,0,"frame.publish"));if(r.variable_descriptor_uuid.is_nil()||!r.registry_generation||!Publish(c,r,0,"frame.publish")){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable.publish_failed","durable descriptor publication failed");return out;}out.rows.push_back(r);}for(const auto&r:out.rows)g_live[r.variable_descriptor_uuid]=r;out.ok=true;out.diagnostic=Ok();for(const auto&r:out.rows)out.evidence.push_back({"sblr.variable.descriptor.publish",r.decision_evidence_sha256});return out;}

SblrVariableRegistryResult LookupSblrVariable(const EngineRequestContext& c,const EngineUuid& receipt,const EngineUuid& scope,std::uint64_t sg,const EngineUuid& frame,std::uint64_t fg,const EngineUuid& uuid,std::uint64_t dg,std::uint64_t vg){std::lock_guard lock(g_mutex);if(!Authority(c))return Hidden();auto it=g_live.find(uuid);if(it==g_live.end()||!Bound(c,it->second,receipt,scope,sg,frame,fg))return Hidden();SblrVariableRegistryResult out;if(it->second.variable_descriptor_generation!=dg||it->second.value_generation!=vg||it->second.lifecycle!=SblrVariableLifecycle::active){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable.generation_stale","descriptor or value generation changed");return out;}if(it->second.value_state==SblrVariableValueState::uninitialized){out.diagnostic=Diag("SBLR.VARIABLE.UNINITIALIZED","sblr.variable.uninitialized","variable has no value");return out;}out.ok=true;out.row=it->second;out.diagnostic=Ok();return out;}

SblrVariableRegistryResult AssignSblrVariable(const EngineRequestContext& c,const EngineUuid& receipt,const EngineUuid& scope,std::uint64_t sg,const EngineUuid& frame,std::uint64_t fg,const EngineUuid& uuid,std::uint64_t dg,std::uint64_t expected,SblrVariableValueState state,const std::string& bytes){std::lock_guard lock(g_mutex);if(!CanonicalAuthority(c))return Hidden();auto it=g_live.find(uuid);if(it==g_live.end()||!Bound(c,it->second,receipt,scope,sg,frame,fg))return Hidden();SblrVariableRegistryResult out;if(it->second.mutability!=SblrVariableMutability::mutable_value){out.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.variable.immutable","immutable variable cannot be assigned");return out;}if(it->second.variable_descriptor_generation!=dg||it->second.value_generation!=expected||it->second.lifecycle!=SblrVariableLifecycle::active){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable.assign_stale","assignment compare failed");return out;}if(static_cast<unsigned>(state)<1||static_cast<unsigned>(state)>3||(state==SblrVariableValueState::null_value&&!it->second.nullable)||((state==SblrVariableValueState::null_value||state==SblrVariableValueState::uninitialized)&&!bytes.empty())){out.diagnostic=Diag("DATATYPE.DESCRIPTOR.INVALID","sblr.variable.value_invalid","canonical value state invalid");return out;}auto r=it->second;const auto prior=r.registry_generation;++r.value_generation;r.registry_generation=Next(r.database_uuid);r.value_state=state;r.canonical_value_bytes=bytes;r.canonical_value_sha256=Hash(bytes);r.decision_evidence_sha256=Hash(Material(r,prior,"value.assign"));if(!r.registry_generation||!Publish(c,r,prior,"value.assign")){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable.assign_failed","durable assignment failed");return out;}g_live[uuid]=r;out.ok=true;out.row=r;out.diagnostic=Ok();out.evidence.push_back({"sblr.variable.value.assign",r.decision_evidence_sha256});return out;}

SblrVariableRegistryResult AssignSblrVariableBatch(const EngineRequestContext& c,const EngineUuid& receipt,const EngineUuid& scope,std::uint64_t sg,const EngineUuid& frame,std::uint64_t fg,const std::vector<SblrVariableAssignment>& assignments){
  std::lock_guard lock(g_mutex);if(!CanonicalAuthority(c))return Hidden();SblrVariableRegistryResult out;
  if(assignments.empty()||assignments.size()>4096){out.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.variable.assignment_batch_invalid","nonempty bounded assignment batch required");return out;}
  std::vector<SblrVariableDescriptorRow> rows;std::vector<std::uint64_t> priors;rows.reserve(assignments.size());priors.reserve(assignments.size());std::vector<EngineUuid> ids;ids.reserve(assignments.size());
  for(const auto&a:assignments){if(std::find(ids.begin(),ids.end(),a.variable_descriptor_uuid)!=ids.end()){out.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.variable.assignment_duplicate","descriptor assigned more than once");return out;}ids.push_back(a.variable_descriptor_uuid);}
  for(const auto&a:assignments){auto it=g_live.find(a.variable_descriptor_uuid);if(it==g_live.end()||!Bound(c,it->second,receipt,scope,sg,frame,fg))return Hidden();const auto& prior_row=it->second;if(prior_row.mutability!=SblrVariableMutability::mutable_value){out.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.variable.immutable","immutable variable cannot be assigned");return out;}if(prior_row.variable_descriptor_generation!=a.variable_descriptor_generation||prior_row.value_generation!=a.expected_value_generation||prior_row.lifecycle!=SblrVariableLifecycle::active){out.diagnostic=Diag("SBLR.VARIABLE.STALE","sblr.variable.assign_stale","assignment compare failed");return out;}if(static_cast<unsigned>(a.value_state)<1||static_cast<unsigned>(a.value_state)>3||(a.value_state==SblrVariableValueState::null_value&&!prior_row.nullable)||((a.value_state==SblrVariableValueState::null_value||a.value_state==SblrVariableValueState::uninitialized)&&!a.canonical_value_bytes.empty())){out.diagnostic=Diag("DATATYPE.DESCRIPTOR.INVALID","sblr.variable.value_invalid","canonical value state invalid");return out;}auto r=prior_row;priors.push_back(r.registry_generation);++r.value_generation;r.registry_generation=Next(r.database_uuid);r.value_state=a.value_state;r.canonical_value_bytes=a.canonical_value_bytes;r.canonical_value_sha256=Hash(r.canonical_value_bytes);r.decision_evidence_sha256=Hash(Material(r,priors.back(),"value.assign.batch"));if(!r.registry_generation){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable.assign_failed","registry generation exhausted");return out;}rows.push_back(std::move(r));}
  if(!PublishBatch(c,rows,priors)){out.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.variable.assign_failed","durable assignment batch failed");return out;}for(const auto&r:rows)g_live[r.variable_descriptor_uuid]=r;out.ok=true;out.rows=rows;out.row=rows.back();out.diagnostic=Ok();for(const auto&r:rows)out.evidence.push_back({"sblr.variable.value.assign",r.decision_evidence_sha256});return out;
}

EngineApiDiagnostic RevokeSblrVariableFrame(const EngineRequestContext& c,const EngineUuid& receipt,const EngineUuid& scope,std::uint64_t sg,const EngineUuid& frame,std::uint64_t fg,const std::string& reason){std::lock_guard lock(g_mutex);if(!Authority(c))return Diag("SECURITY.ACCESS_DENIED","sblr.variable.revoke_denied","variable frame authority required");if(!Reason(reason))return Diag("SBLR.OPERAND_INVALID","sblr.variable.reason_invalid","canonical reason required");std::vector<EngineUuid> ids;for(const auto&[id,r]:g_live)if(Bound(c,r,receipt,scope,sg,frame,fg))ids.push_back(id);if(ids.empty())return Diag("SECURITY.ACCESS_DENIED","sblr.variable.frame_hidden","variable frame is not visible");std::sort(ids.begin(),ids.end(),[](const auto&a,const auto&b){return g_live[a].variable_ordinal<g_live[b].variable_ordinal;});for(const auto&id:ids){auto r=g_live[id];const auto prior=r.registry_generation;++r.value_generation;r.registry_generation=Next(r.database_uuid);r.lifecycle=SblrVariableLifecycle::revoked;r.canonical_value_bytes.clear();r.canonical_value_sha256=Hash({});r.value_state=SblrVariableValueState::uninitialized;r.decision_evidence_sha256=Hash(Material(r,prior,reason));if(!r.registry_generation||!Publish(c,r,prior,reason))return Diag("SBLR.EXECUTION_FAILED","sblr.variable.revoke_failed","durable frame revocation failed");g_live.erase(id);}return Ok();}

EngineApiDiagnostic RecoverSblrVariableDescriptorRegistry(const EngineRequestContext& c){std::lock_guard lock(g_mutex);if(!Admin(c))return Diag("SECURITY.ACCESS_DENIED","sblr.variable.recovery_denied","startup registry authority required");if(c.database_path.empty()||!Uuid(c.database_uuid,scratchbird::core::platform::UuidKind::database))return Diag("SBLR.OPERAND_INVALID","sblr.variable.recovery_invalid","database identity required");std::map<EngineUuid,SblrVariableDescriptorRow> rows;std::uint64_t high=0;if(!Replay(c,&rows,&high))return Diag("SBLR.VARIABLE.STALE","sblr.variable.registry_corrupt","torn or contradictory registry evidence");g_high[c.database_uuid]=high;std::vector<EngineUuid> ids;for(const auto&[id,r]:rows)if(r.lifecycle==SblrVariableLifecycle::active)ids.push_back(id);std::sort(ids.begin(),ids.end(),[&](const auto&a,const auto&b){return rows[a].registry_generation<rows[b].registry_generation;});for(const auto&id:ids){auto r=rows[id];const auto prior=r.registry_generation;++r.value_generation;r.registry_generation=Next(r.database_uuid);r.lifecycle=SblrVariableLifecycle::revoked;r.value_state=SblrVariableValueState::uninitialized;r.canonical_value_bytes.clear();r.canonical_value_sha256=Hash({});r.decision_evidence_sha256=Hash(Material(r,prior,"recovery.revoke"));if(!r.registry_generation||!Publish(c,r,prior,"recovery.revoke"))return Diag("SBLR.EXECUTION_FAILED","sblr.variable.recovery_publish_failed","durable recovery revoke failed");}for(auto it=g_live.begin();it!=g_live.end();)if(it->second.database_uuid==c.database_uuid)it=g_live.erase(it);else++it;return Ok();}
} // namespace scratchbird::engine::internal_api
