// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_prepared_coordination_registry.hpp"

#include "sblr_prepared_statement_registry.hpp"

#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <type_traits>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::size_t kHeaderBytes = 200;
constexpr std::uint64_t kJournalLimit = 256ULL * 1024ULL * 1024ULL;
constexpr std::string_view kDomain =
    "ScratchBird.SblrPreparedCoordinationRegistry.V2";
std::mutex g_mutex;
using States = std::unordered_map<EngineUuid, SblrPreparedCoordinationSnapshot, EngineUuidHash>;
States g_live;
std::unordered_map<EngineUuid, std::uint64_t, EngineUuidHash> g_high_water;
std::atomic<std::uint64_t> g_handle{1};

EngineApiDiagnostic Diag(std::string code, std::string key, std::string detail) {
  return MakeEngineApiDiagnostic(std::move(code), std::move(key),
                                 std::move(detail));
}
EngineApiDiagnostic Ok() {
  return MakeEngineApiDiagnostic("OK", "ok", {}, false);
}
bool HasAuthority(const EngineRequestContext& c) {
  return c.security_context_present &&
      c.statement_metadata_snapshot_engine_owned &&
      std::find(c.trace_tags.begin(), c.trace_tags.end(),
                "private_prepared_coordination") != c.trace_tags.end();
}
bool HasRecoveryAuthority(const EngineRequestContext& c) {
  return c.security_context_present &&
      std::find(c.trace_tags.begin(), c.trace_tags.end(),
                "right:SBLR_PREPARED_COORDINATION_ADMIN") != c.trace_tags.end();
}
bool ValidUuid(const EngineUuid& id, scratchbird::core::platform::UuidKind kind) {
  return scratchbird::core::uuid::MakeTypedUuid(kind, id).ok() &&
         scratchbird::core::uuid::IsEngineIdentityUuid(id);
}
EngineUuid NewUuid() {
  return scratchbird::core::uuid::IssueRuntimeIdentityV7().value_or(EngineUuid{});
}
std::string Hash(std::string_view bytes) {
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(
      reinterpret_cast<const scratchbird::core::platform::byte*>(bytes.data()),
      bytes.size());
  return digest.ok() ? "sha256:" + scratchbird::core::hash::HexLower(digest.digest)
                     : std::string{};
}
bool HashValue(std::string_view value) {
  return value.size() == 71 && value.substr(0, 7) == "sha256:" &&
      std::all_of(value.begin() + 7, value.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
}
bool Reason(std::string_view value) {
  return !value.empty() && value.size() <= 128 &&
      std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == ':' || c == '-';
      });
}
SblrPreparedCoordinationKind KindFromReason(std::string_view reason) {
  if (reason == "prepared.begin") {
    return SblrPreparedCoordinationKind::preparation;
  }
  if (reason == "prepared.execution.begin") {
    return SblrPreparedCoordinationKind::execution;
  }
  return SblrPreparedCoordinationKind::unknown;
}
std::string Path(const EngineRequestContext& c) {
  return c.database_path + ".sb.sblr_prepared_coordination.v1";
}

void SetLe(std::string& bytes, std::size_t at, std::uint64_t value, std::size_t width) {
  for (std::size_t i=0;i<width;++i) bytes[at+i]=static_cast<char>(value>>(8*i));
}
std::uint64_t GetLe(std::string_view bytes, std::size_t at, std::size_t width) {
  std::uint64_t value=0;
  for (std::size_t i=0;i<width;++i) value|=std::uint64_t(static_cast<unsigned char>(bytes[at+i]))<<(8*i);
  return value;
}
bool Zero(std::string_view bytes) {
  return std::all_of(bytes.begin(),bytes.end(),[](char c){return c==0;});
}
bool PutHash(std::string& bytes, std::size_t at, std::string_view value) {
  if(value.empty()) return true;
  if(!HashValue(value)) return false;
  const auto nibble=[](char c){return c<='9'?c-'0':c-'a'+10;};
  for(std::size_t i=0;i<32;++i)
    bytes[at+i]=static_cast<char>((nibble(value[7+2*i])<<4)|nibble(value[8+2*i]));
  return true;
}
std::string GetHash(std::string_view bytes,std::size_t at) {
  std::array<std::uint8_t,32> value{};
  for(std::size_t i=0;i<32;++i)value[i]=static_cast<unsigned char>(bytes[at+i]);
  return "sha256:"+core::hash::HexLower(value);
}
std::string Record(std::uint8_t phase,const SblrPreparedCoordinationSnapshot& s,
                   std::uint64_t prior,std::string_view reason) {
  if(!Reason(reason)||phase>2||!ValidUuid(s.coordination_uuid,core::platform::UuidKind::object)||
     !ValidUuid(s.operation_uuid,core::platform::UuidKind::object)||
     !ValidUuid(s.database_uuid,core::platform::UuidKind::database)||
     !ValidUuid(s.session_uuid,core::platform::UuidKind::session)||
     !ValidUuid(s.provisional_prepared_uuid,core::platform::UuidKind::object)||
     s.provisional_prepared_generation==0||s.coordinator_generation==0||
     (s.kind!=SblrPreparedCoordinationKind::preparation&&s.kind!=SblrPreparedCoordinationKind::execution)||
     s.state<SblrPreparedCoordinationState::begun||s.state>SblrPreparedCoordinationState::revoked)
    return {};
  std::string bytes(kHeaderBytes+reason.size(),'\0');
  bytes.replace(0,4,"SBPC");SetLe(bytes,4,2,2);bytes[6]=phase;
  bytes[7]=static_cast<char>(s.state);SetLe(bytes,8,bytes.size(),4);bytes[12]=static_cast<char>(s.kind);
  const std::array<const EngineUuid*,5> ids{&s.coordination_uuid,&s.operation_uuid,
      &s.database_uuid,&s.session_uuid,&s.provisional_prepared_uuid};
  for(std::size_t i=0;i<ids.size();++i)
    std::copy(ids[i]->bytes.begin(),ids[i]->bytes.end(),bytes.begin()+16+16*i);
  SetLe(bytes,96,s.provisional_prepared_generation,8);SetLe(bytes,104,s.coordinator_generation,8);
  SetLe(bytes,112,prior,8);bytes[120]=s.seal_evidence_sha256.empty()?0:1;
  if(!PutHash(bytes,128,s.seal_evidence_sha256)||
     (phase!=0&&!PutHash(bytes,168,s.decision_evidence_sha256)))return {};
  SetLe(bytes,160,reason.size(),2);bytes.replace(kHeaderBytes,reason.size(),reason);
  return bytes;
}
std::string Material(const SblrPreparedCoordinationSnapshot& s,
                     std::uint64_t prior,std::string_view reason) {
  auto bytes=Record(0,s,prior,reason);
  if(bytes.empty())return {};
  return std::string(kDomain)+bytes;
}
#if !defined(_WIN32)
struct Descriptor {
  int fd=-1;
  ~Descriptor(){if(fd>=0)::close(fd);}
};
#endif
bool Append(const std::string& path,const std::string& bytes) {
#if defined(_WIN32)
  {
    std::ofstream output(path,std::ios::binary|std::ios::app);
    output.write(bytes.data(),static_cast<std::streamsize>(bytes.size()));
    output.flush();if(!output)return false;
  }
  HANDLE h=CreateFileA(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,
                      OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(h==INVALID_HANDLE_VALUE)return false;
  const bool ok=FlushFileBuffers(h)!=0;CloseHandle(h);return ok;
#else
  auto parent=std::filesystem::path(path).parent_path();if(parent.empty())parent=".";
  Descriptor file{::open(path.c_str(),O_WRONLY|O_APPEND|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600)};
  if(file.fd<0)return false;
  struct stat metadata{};
  if(::fstat(file.fd,&metadata)!=0||!S_ISREG(metadata.st_mode)||metadata.st_size<0||
     static_cast<std::uint64_t>(metadata.st_size)>kJournalLimit-bytes.size())return false;
  std::size_t offset=0;
  while(offset<bytes.size()) {
    const auto count=::write(file.fd,bytes.data()+offset,bytes.size()-offset);
    if(count<0&&errno==EINTR)continue;
    if(count<=0)return false;offset+=static_cast<std::size_t>(count);
  }
  if(::fsync(file.fd)!=0)return false;
  Descriptor directory{::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC)};
  return directory.fd>=0&&::fsync(directory.fd)==0;
#endif
}
bool Publish(const EngineRequestContext& c,const SblrPreparedCoordinationSnapshot& s,
             std::uint64_t prior,std::string_view reason) {
  const auto evidence=Record(1,s,prior,reason),state=Record(2,s,prior,reason);
  if(evidence.empty()||state.empty()||!HashValue(s.decision_evidence_sha256)||
     s.decision_evidence_sha256!=Hash(Material(s,prior,reason)))return false;
  const auto path=Path(c);
  std::error_code error;
  const auto existing=std::filesystem::exists(path,error);
  if(error)return false;
  const auto size=existing?std::filesystem::file_size(path,error):0;
  if(error||size>kJournalLimit-evidence.size()-state.size())return false;
  return Append(path,evidence)&&Append(path,state);
}
bool Decode(std::string_view bytes,std::uint8_t phase,
            SblrPreparedCoordinationSnapshot* output,std::uint64_t* prior,std::string* reason) {
  if(bytes.size()<kHeaderBytes||bytes.substr(0,4)!="SBPC"||GetLe(bytes,4,2)!=2||
     GetLe(bytes,6,1)!=phase||GetLe(bytes,8,4)!=bytes.size()||
     !Zero(bytes.substr(13,3))||!Zero(bytes.substr(121,7))||!Zero(bytes.substr(162,6))||
     GetLe(bytes,120,1)>1||GetLe(bytes,160,2)!=bytes.size()-kHeaderBytes||
     (bytes[120]==0&&!Zero(bytes.substr(128,32))))return false;
  SblrPreparedCoordinationSnapshot s;
  s.state=static_cast<SblrPreparedCoordinationState>(GetLe(bytes,7,1));
  s.kind=static_cast<SblrPreparedCoordinationKind>(GetLe(bytes,12,1));
  const std::array<EngineUuid*,5> ids{&s.coordination_uuid,&s.operation_uuid,
      &s.database_uuid,&s.session_uuid,&s.provisional_prepared_uuid};
  for(std::size_t i=0;i<ids.size();++i)
    std::copy_n(bytes.begin()+16+16*i,16,ids[i]->bytes.begin());
  s.provisional_prepared_generation=GetLe(bytes,96,8);s.coordinator_generation=GetLe(bytes,104,8);
  *prior=GetLe(bytes,112,8);*reason=std::string(bytes.substr(kHeaderBytes));
  if(bytes[120])s.seal_evidence_sha256=GetHash(bytes,128);
  s.decision_evidence_sha256=GetHash(bytes,168);
  if(Record(phase,s,*prior,*reason)!=bytes||
     s.decision_evidence_sha256!=Hash(Material(s,*prior,*reason)))return false;
  *output=std::move(s);return true;
}
bool ReadJournal(const std::string& path,std::string* bytes) {
#if defined(_WIN32)
  std::ifstream input(path,std::ios::binary|std::ios::ate);
  if(!input)return !std::filesystem::exists(path);
  const auto size=input.tellg();if(size<=0||std::uint64_t(size)>kJournalLimit)return false;
  bytes->resize(static_cast<std::size_t>(size));input.seekg(0);input.read(bytes->data(),size);
  return bool(input);
#else
  Descriptor file{::open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW)};
  if(file.fd<0)return errno==ENOENT;
  struct stat metadata{};
  if(::fstat(file.fd,&metadata)!=0||!S_ISREG(metadata.st_mode)||metadata.st_size<=0||
     std::uint64_t(metadata.st_size)>kJournalLimit)return false;
  bytes->resize(static_cast<std::size_t>(metadata.st_size));std::size_t offset=0;
  while(offset<bytes->size()) {
    const auto count=::read(file.fd,bytes->data()+offset,bytes->size()-offset);
    if(count<0&&errno==EINTR)continue;
    if(count<=0)return false;offset+=static_cast<std::size_t>(count);
  }
  return true;
#endif
}
bool LegalTransition(const SblrPreparedCoordinationSnapshot& s,
                     const SblrPreparedCoordinationSnapshot* previous,
                     std::uint64_t prior,std::string_view reason) {
  if(!previous) {
    return prior==0&&s.state==SblrPreparedCoordinationState::begun&&
      KindFromReason(reason)==s.kind&&
      (s.kind==SblrPreparedCoordinationKind::preparation
       ?s.seal_evidence_sha256.empty()&&s.provisional_prepared_generation==s.coordinator_generation
       :!s.seal_evidence_sha256.empty());
  }
  if(prior!=previous->coordinator_generation||s.kind!=previous->kind||
     s.provisional_prepared_uuid!=previous->provisional_prepared_uuid||
     s.provisional_prepared_generation!=previous->provisional_prepared_generation||
     s.operation_uuid!=previous->operation_uuid||s.database_uuid!=previous->database_uuid||
     s.session_uuid!=previous->session_uuid)return false;
  if(s.state==SblrPreparedCoordinationState::acquired)
    return previous->state==SblrPreparedCoordinationState::begun&&reason=="prepared.acquire"&&s.seal_evidence_sha256.empty();
  if(s.state==SblrPreparedCoordinationState::sealed)
    return previous->state==SblrPreparedCoordinationState::acquired&&reason=="prepared.seal"&&!s.seal_evidence_sha256.empty();
  return s.state==SblrPreparedCoordinationState::revoked&&
    (previous->state==SblrPreparedCoordinationState::begun||previous->state==SblrPreparedCoordinationState::acquired)&&
    KindFromReason(reason)==SblrPreparedCoordinationKind::unknown&&reason!="prepared.acquire"&&reason!="prepared.seal"&&s.seal_evidence_sha256.empty();
}
bool Replay(const EngineRequestContext& c,States* states,std::uint64_t* high) {
  std::string bytes;if(!ReadJournal(Path(c),&bytes))return false;
  States staged;std::uint64_t highest=0;std::size_t offset=0;
  while(offset<bytes.size()) {
    const auto read=[&](std::uint8_t phase,SblrPreparedCoordinationSnapshot* s,
                       std::uint64_t* prior,std::string* reason,std::string_view* raw) {
      if(bytes.size()-offset<kHeaderBytes)return false;
      const auto size=GetLe(bytes,offset+8,4);
      if(size<kHeaderBytes||size>kHeaderBytes+128||size>bytes.size()-offset)return false;
      *raw=std::string_view(bytes).substr(offset,size);offset+=size;
      return Decode(*raw,phase,s,prior,reason);
    };
    SblrPreparedCoordinationSnapshot e,s;std::uint64_t ep=0,sp=0;std::string er,sr;std::string_view eb,sb;
    if(!read(1,&e,&ep,&er,&eb)||!read(2,&s,&sp,&sr,&sb))return false;
    auto normalized=std::string(eb);normalized[6]=2;
    if(normalized!=sb||ep!=sp||er!=sr||s.database_uuid!=c.database_uuid||s.coordinator_generation<=highest)return false;
    const auto found=staged.find(s.coordination_uuid);
    if(!LegalTransition(s,found==staged.end()?nullptr:&found->second,sp,sr))return false;
    highest=s.coordinator_generation;staged[s.coordination_uuid]=std::move(s);
  }
  states->swap(staged);*high=highest;return true;
}
bool LiveMatchesDurable(const EngineUuid& node,const States& states) {
  for(const auto& [id,state]:states) {
    if(state.state!=SblrPreparedCoordinationState::begun&&
       state.state!=SblrPreparedCoordinationState::acquired)continue;
    const auto live=g_live.find(id);
    if(live==g_live.end()||live->second.database_uuid!=node||
       live->second.decision_evidence_sha256!=state.decision_evidence_sha256)return false;
  }
  for(const auto& [id,live]:g_live) {
    if(live.database_uuid!=node)continue;
    const auto durable=states.find(id);
    if(durable==states.end()||
       durable->second.decision_evidence_sha256!=live.decision_evidence_sha256)return false;
  }
  return true;
}
std::uint64_t NewHandle() {
  auto value=g_handle.load(std::memory_order_relaxed);
  while(value!=0&&value!=std::numeric_limits<std::uint64_t>::max()) {
    if(g_handle.compare_exchange_weak(value,value+1,std::memory_order_relaxed))return value;
  }
  return 0;
}
std::uint64_t NextGeneration(const EngineUuid& db) {
  auto& value=g_high_water[db]; if(value==std::numeric_limits<std::uint64_t>::max()) return 0;
  return ++value;
}
SblrPreparedCoordinationResult Denied() {
  SblrPreparedCoordinationResult r;
  r.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.prepared_coordination.hidden",
                    "coordination reference is not visible"); return r;
}
SblrPreparedCoordinationResult Mutate(
    const EngineRequestContext& c, const EngineUuid& coordination,
    const EngineUuid& operation, std::uint64_t expected,
    SblrPreparedCoordinationState from, SblrPreparedCoordinationState to,
    std::string_view seal_hash, std::string_view reason) {
  if(!HasAuthority(c)) return Denied();
  const auto it=g_live.find(coordination);
  if(it==g_live.end() || it->second.database_uuid!=c.database_uuid ||
     it->second.session_uuid!=c.session_uuid || it->second.operation_uuid!=operation)
    return Denied();
  SblrPreparedCoordinationResult r;
  if(it->second.coordinator_generation!=expected || it->second.state!=from) {
    r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.compare_stale","coordination compare failed"); return r;
  }
  States durable;std::uint64_t high=0;
  if(!Replay(c,&durable,&high)||!LiveMatchesDurable(c.database_uuid,durable)) {
    r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.recovery_required",
                      "durable coordination does not match live ownership");return r;
  }
  g_high_water[c.database_uuid]=std::max(g_high_water[c.database_uuid],high);
  auto next=it->second; const auto prior=next.coordinator_generation;
  next.coordinator_generation=NextGeneration(next.database_uuid); next.state=to;
  next.seal_evidence_sha256=std::string(seal_hash);
  next.decision_evidence_sha256=Hash(Material(next,prior,reason));
  if(next.coordinator_generation==0 || !Publish(c,next,prior,reason)) {
    r.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.publish_failed","durable transition failed"); return r;
  }
  if(to==SblrPreparedCoordinationState::sealed || to==SblrPreparedCoordinationState::revoked) g_live.erase(it);
  else g_live[coordination]=next;
  r.ok=true; r.snapshot=next; r.diagnostic=Ok();
  r.evidence.push_back({"sblr.prepared_coordination.transition",next.decision_evidence_sha256}); return r;
}
}  // namespace

SblrPreparedCoordinationResult BeginSblrPreparedCoordination(
    const EngineRequestContext& c,const EngineUuid& operation) {
  std::lock_guard lock(g_mutex); SblrPreparedCoordinationResult r;
  if(!HasAuthority(c)) return Denied();
  if(c.database_path.empty() || !ValidUuid(c.database_uuid,scratchbird::core::platform::UuidKind::database) ||
     !ValidUuid(c.session_uuid,scratchbird::core::platform::UuidKind::session) ||
     !ValidUuid(operation,scratchbird::core::platform::UuidKind::object)) {
    r.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.begin_invalid","exact prepared begin identity required"); return r;
  }
  std::uint64_t durable_high=0;States states;
  if(!Replay(c,&states,&durable_high)||!LiveMatchesDurable(c.database_uuid,states)) {
    r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.recovery_required",
                      "registry replay failed or unfinished coordination requires recovery");return r;
  }
  auto& high=g_high_water[c.database_uuid];high=std::max(high,durable_high);
  SblrPreparedCoordinationSnapshot s; s.coordinator_generation=NextGeneration(c.database_uuid);
  s.provisional_prepared_generation=s.coordinator_generation;
  s.coordination_uuid=NewUuid(); s.provisional_prepared_uuid=NewUuid();
  s.operation_uuid=operation; s.database_uuid=c.database_uuid; s.session_uuid=c.session_uuid;
  s.private_handle=NewHandle();
  s.kind=SblrPreparedCoordinationKind::preparation;
  s.state=SblrPreparedCoordinationState::begun; s.decision_evidence_sha256=Hash(Material(s,0,"prepared.begin"));
  if(s.coordinator_generation==0||s.private_handle==0||s.coordination_uuid.is_nil()||s.provisional_prepared_uuid.is_nil()||!Publish(c,s,0,"prepared.begin")) {
    r.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.begin_publish_failed","durable begin failed"); return r;
  }
  g_live[s.coordination_uuid]=s; r.ok=true;r.snapshot=s;r.diagnostic=Ok();
  r.evidence.push_back({"sblr.prepared_coordination.begin",s.decision_evidence_sha256}); return r;
}

SblrPreparedCoordinationResult BeginSblrPreparedExecutionCoordination(
    const EngineRequestContext& c, const EngineUuid& operation,
    const EngineUuid& prepared) {
  std::lock_guard lock(g_mutex); SblrPreparedCoordinationResult r;
  if(!HasAuthority(c)) return Denied();
  if(c.database_path.empty() ||
     !ValidUuid(c.database_uuid,scratchbird::core::platform::UuidKind::database) ||
     !ValidUuid(c.session_uuid,scratchbird::core::platform::UuidKind::session) ||
     !ValidUuid(operation,scratchbird::core::platform::UuidKind::object) ||
     !ValidUuid(prepared,scratchbird::core::platform::UuidKind::object)) {
    r.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.execution_begin_invalid","exact prepared execution identity required"); return r;
  }
  States states;
  std::uint64_t durable_high=0;
  if(!Replay(c,&states,&durable_high)||!LiveMatchesDurable(c.database_uuid,states)) {
    r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.execution_replay_stale","prepared registry replay failed"); return r;
  }
  g_high_water[c.database_uuid]=std::max(g_high_water[c.database_uuid],durable_high);
  const SblrPreparedCoordinationSnapshot* sealed=nullptr;
  for(const auto& [id,state]:states) {
    if(state.provisional_prepared_uuid==prepared &&
       state.kind==SblrPreparedCoordinationKind::preparation &&
       state.state==SblrPreparedCoordinationState::sealed &&
       state.database_uuid==c.database_uuid &&
       state.session_uuid==c.session_uuid) {
      if(sealed!=nullptr) return Denied();
      sealed=&state;
    }
  }
  if(sealed==nullptr) return Denied();
  auto capability_context = c;
  capability_context.trace_tags.push_back(
      "private_prepared_statement_capability_check");
  const auto capability = ResolveActiveSblrPreparedStatementCapabilityV1(
      capability_context, prepared,
      sealed->provisional_prepared_generation);
  if (!capability.ok) {
    r.diagnostic = capability.diagnostic;
    return r;
  }
  SblrPreparedCoordinationSnapshot s;
  s.coordinator_generation=NextGeneration(c.database_uuid);
  s.coordination_uuid=NewUuid();
  s.operation_uuid=operation; s.database_uuid=c.database_uuid;
  s.session_uuid=c.session_uuid;
  s.provisional_prepared_uuid=sealed->provisional_prepared_uuid;
  s.provisional_prepared_generation=sealed->provisional_prepared_generation;
  s.private_handle=NewHandle();
  s.kind=SblrPreparedCoordinationKind::execution;
  s.state=SblrPreparedCoordinationState::begun;
  s.seal_evidence_sha256=sealed->seal_evidence_sha256;
  s.decision_evidence_sha256=Hash(Material(s,0,"prepared.execution.begin"));
  if(s.coordinator_generation==0||s.coordination_uuid.is_nil()||s.private_handle==0||
     !Publish(c,s,0,"prepared.execution.begin")) {
    r.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.execution_begin_publish_failed","durable execution begin failed"); return r;
  }
  g_live[s.coordination_uuid]=s; r.ok=true; r.snapshot=s; r.diagnostic=Ok();
  r.evidence.push_back({"sblr.prepared_coordination.execution_begin",s.decision_evidence_sha256}); return r;
}

SblrPreparedCoordinationResult AcquireSblrPreparedCoordination(
    const EngineRequestContext& c,const EngineUuid& coordination,const EngineUuid& operation,std::uint64_t expected) {
  std::lock_guard lock(g_mutex); return Mutate(c,coordination,operation,expected,SblrPreparedCoordinationState::begun,SblrPreparedCoordinationState::acquired,{},"prepared.acquire");
}
SblrPreparedCoordinationResult SealSblrPreparedCoordination(
    const EngineRequestContext& c,const EngineUuid& coordination,const EngineUuid& operation,std::uint64_t expected,
    const EngineUuid& prepared,std::uint64_t prepared_generation,const std::string& evidence) {
  std::lock_guard lock(g_mutex); const auto it=g_live.find(coordination);
  if(!HasAuthority(c)||it==g_live.end()||it->second.database_uuid!=c.database_uuid||it->second.session_uuid!=c.session_uuid) return Denied();
  if(it->second.provisional_prepared_uuid!=prepared||it->second.provisional_prepared_generation!=prepared_generation||!HashValue(evidence)) {
    SblrPreparedCoordinationResult r;r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.provisional_stale","provisional prepared identity or evidence mismatch");return r; }
  return Mutate(c,coordination,operation,expected,SblrPreparedCoordinationState::acquired,SblrPreparedCoordinationState::sealed,evidence,"prepared.seal");
}
SblrPreparedCoordinationResult RevokeSblrPreparedCoordination(
    const EngineRequestContext& c,const EngineUuid& coordination,const EngineUuid& operation,std::uint64_t expected,const std::string& reason) {
  std::lock_guard lock(g_mutex); SblrPreparedCoordinationResult r;
  if(!Reason(reason)||KindFromReason(reason)!=SblrPreparedCoordinationKind::unknown||reason=="prepared.acquire"||reason=="prepared.seal"){r.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.reason_invalid","canonical reason required");return r;}
  const auto it=g_live.find(coordination); if(it==g_live.end()) return Denied();
  const auto from=it->second.state; if(from!=SblrPreparedCoordinationState::begun&&from!=SblrPreparedCoordinationState::acquired){r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.terminal","coordination already terminal");return r;}
  return Mutate(c,coordination,operation,expected,from,SblrPreparedCoordinationState::revoked,{},reason);
}
EngineApiDiagnostic RecoverSblrPreparedCoordinationRegistry(const EngineRequestContext& c) {
  std::lock_guard lock(g_mutex); if(!HasRecoveryAuthority(c)) return Diag("SECURITY.ACCESS_DENIED","sblr.prepared_coordination.recovery_denied","startup recovery authority required");
  if(c.database_path.empty()||!ValidUuid(c.database_uuid,scratchbird::core::platform::UuidKind::database)) return Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.recovery_invalid","database identity required");
  States states; std::uint64_t high=0;
  if(!Replay(c,&states,&high)) return Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.corrupt","contradictory or torn durable evidence");
  g_high_water[c.database_uuid]=high;
  for(auto& [id,s]:states) if(s.state==SblrPreparedCoordinationState::begun||s.state==SblrPreparedCoordinationState::acquired) {
    const auto prior=s.coordinator_generation; s.coordinator_generation=NextGeneration(c.database_uuid); s.state=SblrPreparedCoordinationState::revoked; s.private_handle=0; s.seal_evidence_sha256.clear(); s.decision_evidence_sha256=Hash(Material(s,prior,"recovery.revoke"));
    if(s.coordinator_generation==0||!Publish(c,s,prior,"recovery.revoke")) return Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.recovery_publish_failed","durable recovery revocation failed");
  }
  for(auto it=g_live.begin();it!=g_live.end();) if(it->second.database_uuid==c.database_uuid) it=g_live.erase(it); else ++it;
  return Ok();
}
}  // namespace scratchbird::engine::internal_api
