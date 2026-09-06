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
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace scratchbird::engine::internal_api {
namespace {
constexpr std::string_view kMagic = "SBPCR1";
constexpr std::string_view kDomain =
    "ScratchBird.SblrPreparedCoordinationRegistry.V1";
std::mutex g_mutex;
std::unordered_map<std::string, SblrPreparedCoordinationSnapshot> g_live;
std::unordered_map<std::string, std::uint64_t> g_high_water;
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
bool ValidUuid(std::string_view s, scratchbird::core::platform::UuidKind kind) {
  if (kind == scratchbird::core::platform::UuidKind::session)
    return scratchbird::core::uuid::ParseTypedUuid(kind, std::string(s)).ok();
  return scratchbird::core::uuid::ParseDurableEngineIdentityUuid(
      kind, std::string(s)).ok();
}
std::string NewUuid(std::uint64_t salt) {
  const auto now = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  const auto value = scratchbird::core::uuid::GenerateEngineIdentityV7(
      scratchbird::core::platform::UuidKind::object, now + salt);
  return value.ok() ? scratchbird::core::uuid::UuidToString(value.value.value)
                    : std::string{};
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
        return std::isalnum(c) || c == '.' || c == '_' || c == ':' || c == '-';
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
std::string Material(const SblrPreparedCoordinationSnapshot& s,
                     std::uint64_t prior, std::string_view reason) {
  std::ostringstream out;
  out << kDomain << '\n' << s.coordination_uuid << '\n' << s.operation_uuid
      << '\n' << s.database_uuid << '\n' << s.session_uuid << '\n'
      << s.provisional_prepared_uuid << '\n'
      << s.provisional_prepared_generation << '\n'
      << s.coordinator_generation << '\n' << prior << '\n'
      << static_cast<unsigned>(s.state) << '\n' << s.seal_evidence_sha256
      << '\n' << reason;
  return out.str();
}
std::string Record(std::string_view kind,
                   const SblrPreparedCoordinationSnapshot& s,
                   std::uint64_t prior, std::string_view reason) {
  std::ostringstream out;
  out << kMagic << '\t' << kind << '\t' << s.coordination_uuid << '\t'
      << s.operation_uuid << '\t' << s.database_uuid << '\t' << s.session_uuid
      << '\t' << s.provisional_prepared_uuid << '\t'
      << s.provisional_prepared_generation << '\t' << s.coordinator_generation
      << '\t' << prior << '\t' << static_cast<unsigned>(s.state) << '\t'
      << s.seal_evidence_sha256 << '\t' << reason << '\t'
      << s.decision_evidence_sha256;
  return out.str();
}
bool Append(const std::string& path, const std::string& line) {
  {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (!out) return false;
    out << line << '\n';
    out.flush();
    if (!out) return false;
  }
#if defined(_WIN32)
  HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const bool ok = FlushFileBuffers(h) != 0; CloseHandle(h); return ok;
#else
  const int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
  if (fd < 0) return false;
  const bool ok = ::fsync(fd) == 0; ::close(fd); return ok;
#endif
}
bool Publish(const EngineRequestContext& c,
             const SblrPreparedCoordinationSnapshot& s,
             std::uint64_t prior, std::string_view reason) {
  return Append(Path(c), Record("EVIDENCE", s, prior, reason)) &&
         Append(Path(c), Record("STATE", s, prior, reason));
}
std::vector<std::string> Split(std::string_view v, char separator) {
  std::vector<std::string> result; std::size_t begin = 0;
  while (begin <= v.size()) {
    const auto end = v.find(separator, begin);
    result.emplace_back(v.substr(begin, end == std::string_view::npos
        ? v.size() - begin : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1;
  }
  return result;
}
bool U64(std::string_view v, std::uint64_t* out) {
  if (v.empty() || out == nullptr) return false;
  std::uint64_t n = 0;
  for (char c : v) { if (c < '0' || c > '9' ||
      n > (std::numeric_limits<std::uint64_t>::max() - (c-'0')) / 10) return false;
    n = n * 10 + static_cast<unsigned>(c-'0'); }
  *out = n; return true;
}
bool Decode(const std::string& line, std::string_view kind,
            SblrPreparedCoordinationSnapshot* s, std::uint64_t* prior,
            std::string* reason) {
  const auto f = Split(line, '\t'); std::uint64_t state = 0;
  if (f.size()!=14 || f[0]!=kMagic || f[1]!=kind || !U64(f[7],&s->provisional_prepared_generation) ||
      !U64(f[8],&s->coordinator_generation) || !U64(f[9],prior) || !U64(f[10],&state) ||
      state<1 || state>4) return false;
  s->coordination_uuid=f[2]; s->operation_uuid=f[3]; s->database_uuid=f[4];
  s->session_uuid=f[5]; s->provisional_prepared_uuid=f[6];
  s->state=static_cast<SblrPreparedCoordinationState>(state);
  s->seal_evidence_sha256=f[11]; *reason=f[12];
  s->kind=KindFromReason(*reason);
  s->decision_evidence_sha256=f[13];
  return ValidUuid(s->coordination_uuid,scratchbird::core::platform::UuidKind::object) &&
      ValidUuid(s->operation_uuid,scratchbird::core::platform::UuidKind::object) &&
      ValidUuid(s->database_uuid,scratchbird::core::platform::UuidKind::database) &&
      ValidUuid(s->session_uuid,scratchbird::core::platform::UuidKind::session) &&
      ValidUuid(s->provisional_prepared_uuid,scratchbird::core::platform::UuidKind::object) &&
      s->provisional_prepared_generation!=0 && s->coordinator_generation!=0 && Reason(*reason) &&
      (s->seal_evidence_sha256.empty() || HashValue(s->seal_evidence_sha256)) &&
      s->decision_evidence_sha256==Hash(Material(*s,*prior,*reason));
}
bool Replay(const EngineRequestContext& c,
            std::unordered_map<std::string,SblrPreparedCoordinationSnapshot>* states,
            std::uint64_t* high) {
  states->clear(); *high=0; std::ifstream in(Path(c),std::ios::binary);
  if (!in) return !std::filesystem::exists(Path(c));
  std::vector<std::string> lines; std::string line;
  while(std::getline(in,line)) lines.push_back(line);
  if(!in.eof() || lines.size()%2) return false;
  for(std::size_t i=0;i<lines.size();i+=2) {
    SblrPreparedCoordinationSnapshot e,s; std::uint64_t ep=0,sp=0; std::string er,sr;
    if(!Decode(lines[i],"EVIDENCE",&e,&ep,&er)||!Decode(lines[i+1],"STATE",&s,&sp,&sr)||
       lines[i].substr(lines[i].find('\t',lines[i].find('\t')+1)) !=
       lines[i+1].substr(lines[i+1].find('\t',lines[i+1].find('\t')+1) ) ||
       ep!=sp || er!=sr || e.kind!=s.kind ||
       s.database_uuid!=c.database_uuid.canonical ||
       s.coordinator_generation<=*high) return false;
    const auto found=states->find(s.coordination_uuid);
    if((found==states->end() &&
        (ep!=0||s.state!=SblrPreparedCoordinationState::begun||
         s.kind==SblrPreparedCoordinationKind::unknown)) ||
       (found!=states->end() && (ep!=found->second.coordinator_generation ||
        s.kind!=SblrPreparedCoordinationKind::unknown ||
        found->second.provisional_prepared_uuid!=s.provisional_prepared_uuid ||
        found->second.provisional_prepared_generation!=s.provisional_prepared_generation ||
        found->second.operation_uuid!=s.operation_uuid || found->second.session_uuid!=s.session_uuid))) return false;
    if(found!=states->end()) s.kind=found->second.kind;
    (*states)[s.coordination_uuid]=s; *high=s.coordinator_generation;
  }
  return true;
}
std::uint64_t NextGeneration(const std::string& db) {
  auto& value=g_high_water[db]; if(value==std::numeric_limits<std::uint64_t>::max()) return 0;
  return ++value;
}
SblrPreparedCoordinationResult Denied() {
  SblrPreparedCoordinationResult r;
  r.diagnostic=Diag("SECURITY.ACCESS_DENIED","sblr.prepared_coordination.hidden",
                    "coordination reference is not visible"); return r;
}
SblrPreparedCoordinationResult Mutate(
    const EngineRequestContext& c, const std::string& coordination,
    const std::string& operation, std::uint64_t expected,
    SblrPreparedCoordinationState from, SblrPreparedCoordinationState to,
    std::string_view seal_hash, std::string_view reason) {
  if(!HasAuthority(c)) return Denied();
  const auto it=g_live.find(coordination);
  if(it==g_live.end() || it->second.database_uuid!=c.database_uuid.canonical ||
     it->second.session_uuid!=c.session_uuid.canonical || it->second.operation_uuid!=operation)
    return Denied();
  SblrPreparedCoordinationResult r;
  if(it->second.coordinator_generation!=expected || it->second.state!=from) {
    r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.compare_stale","coordination compare failed"); return r;
  }
  auto next=it->second; const auto prior=next.coordinator_generation;
  next.coordinator_generation=NextGeneration(next.database_uuid); next.state=to;
  next.seal_evidence_sha256=std::string(seal_hash);
  next.decision_evidence_sha256=Hash(Material(next,prior,reason));
  if(next.coordinator_generation==0 || !Publish(c,next,prior,reason)) {
    g_high_water[next.database_uuid]=prior;
    r.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.publish_failed","durable transition failed"); return r;
  }
  if(to==SblrPreparedCoordinationState::sealed || to==SblrPreparedCoordinationState::revoked) g_live.erase(it);
  else g_live[coordination]=next;
  r.ok=true; r.snapshot=next; r.diagnostic=Ok();
  r.evidence.push_back({"sblr.prepared_coordination.transition",next.decision_evidence_sha256}); return r;
}
}  // namespace

SblrPreparedCoordinationResult BeginSblrPreparedCoordination(
    const EngineRequestContext& c,const std::string& operation) {
  std::lock_guard lock(g_mutex); SblrPreparedCoordinationResult r;
  if(!HasAuthority(c)) return Denied();
  if(c.database_path.empty() || !ValidUuid(c.database_uuid.canonical,scratchbird::core::platform::UuidKind::database) ||
     !ValidUuid(c.session_uuid.canonical,scratchbird::core::platform::UuidKind::session) ||
     !ValidUuid(operation,scratchbird::core::platform::UuidKind::object)) {
    r.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.begin_invalid","exact prepared begin identity required"); return r;
  }
  auto& high=g_high_water[c.database_uuid.canonical];
  if(high==0 && std::filesystem::exists(Path(c))) {
    std::unordered_map<std::string,SblrPreparedCoordinationSnapshot> states;
    if(!Replay(c,&states,&high)) { r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.recovery_required","registry replay failed or recovery required"); return r; }
    for(const auto& [id,s]:states) if(s.state==SblrPreparedCoordinationState::begun||s.state==SblrPreparedCoordinationState::acquired) {
      r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.recovery_required","unfinished coordination requires startup recovery"); return r; }
  }
  SblrPreparedCoordinationSnapshot s; s.coordinator_generation=NextGeneration(c.database_uuid.canonical);
  s.provisional_prepared_generation=s.coordinator_generation;
  s.coordination_uuid=NewUuid(s.coordinator_generation*2); s.provisional_prepared_uuid=NewUuid(s.coordinator_generation*2+1);
  s.operation_uuid=operation; s.database_uuid=c.database_uuid.canonical; s.session_uuid=c.session_uuid.canonical;
  s.private_handle=g_handle.fetch_add(1,std::memory_order_relaxed); if(s.private_handle==0) s.private_handle=g_handle.fetch_add(1,std::memory_order_relaxed);
  s.kind=SblrPreparedCoordinationKind::preparation;
  s.state=SblrPreparedCoordinationState::begun; s.decision_evidence_sha256=Hash(Material(s,0,"prepared.begin"));
  if(s.coordinator_generation==0||s.coordination_uuid.empty()||s.provisional_prepared_uuid.empty()||!Publish(c,s,0,"prepared.begin")) {
    if(s.coordinator_generation) --g_high_water[c.database_uuid.canonical];
    r.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.begin_publish_failed","durable begin failed"); return r;
  }
  g_live[s.coordination_uuid]=s; r.ok=true;r.snapshot=s;r.diagnostic=Ok();
  r.evidence.push_back({"sblr.prepared_coordination.begin",s.decision_evidence_sha256}); return r;
}

SblrPreparedCoordinationResult BeginSblrPreparedExecutionCoordination(
    const EngineRequestContext& c, const std::string& operation,
    const std::string& prepared) {
  std::lock_guard lock(g_mutex); SblrPreparedCoordinationResult r;
  if(!HasAuthority(c)) return Denied();
  if(c.database_path.empty() ||
     !ValidUuid(c.database_uuid.canonical,scratchbird::core::platform::UuidKind::database) ||
     !ValidUuid(c.session_uuid.canonical,scratchbird::core::platform::UuidKind::session) ||
     !ValidUuid(operation,scratchbird::core::platform::UuidKind::object) ||
     !ValidUuid(prepared,scratchbird::core::platform::UuidKind::object)) {
    r.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.execution_begin_invalid","exact prepared execution identity required"); return r;
  }
  std::unordered_map<std::string,SblrPreparedCoordinationSnapshot> states;
  std::uint64_t durable_high=0;
  if(!Replay(c,&states,&durable_high)) {
    r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.execution_replay_stale","prepared registry replay failed"); return r;
  }
  g_high_water[c.database_uuid.canonical]=std::max(g_high_water[c.database_uuid.canonical],durable_high);
  const SblrPreparedCoordinationSnapshot* sealed=nullptr;
  for(const auto& [id,state]:states) {
    if(state.provisional_prepared_uuid==prepared &&
       state.kind==SblrPreparedCoordinationKind::preparation &&
       state.state==SblrPreparedCoordinationState::sealed &&
       state.database_uuid==c.database_uuid.canonical &&
       state.session_uuid==c.session_uuid.canonical) {
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
  s.coordinator_generation=NextGeneration(c.database_uuid.canonical);
  s.coordination_uuid=NewUuid(s.coordinator_generation*2);
  s.operation_uuid=operation; s.database_uuid=c.database_uuid.canonical;
  s.session_uuid=c.session_uuid.canonical;
  s.provisional_prepared_uuid=sealed->provisional_prepared_uuid;
  s.provisional_prepared_generation=sealed->provisional_prepared_generation;
  s.private_handle=g_handle.fetch_add(1,std::memory_order_relaxed);
  if(s.private_handle==0) s.private_handle=g_handle.fetch_add(1,std::memory_order_relaxed);
  s.kind=SblrPreparedCoordinationKind::execution;
  s.state=SblrPreparedCoordinationState::begun;
  s.seal_evidence_sha256=sealed->seal_evidence_sha256;
  s.decision_evidence_sha256=Hash(Material(s,0,"prepared.execution.begin"));
  if(s.coordinator_generation==0||s.coordination_uuid.empty()||s.private_handle==0||
     !Publish(c,s,0,"prepared.execution.begin")) {
    r.diagnostic=Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.execution_begin_publish_failed","durable execution begin failed"); return r;
  }
  g_live[s.coordination_uuid]=s; r.ok=true; r.snapshot=s; r.diagnostic=Ok();
  r.evidence.push_back({"sblr.prepared_coordination.execution_begin",s.decision_evidence_sha256}); return r;
}

SblrPreparedCoordinationResult AcquireSblrPreparedCoordination(
    const EngineRequestContext& c,const std::string& coordination,const std::string& operation,std::uint64_t expected) {
  std::lock_guard lock(g_mutex); return Mutate(c,coordination,operation,expected,SblrPreparedCoordinationState::begun,SblrPreparedCoordinationState::acquired,{},"prepared.acquire");
}
SblrPreparedCoordinationResult SealSblrPreparedCoordination(
    const EngineRequestContext& c,const std::string& coordination,const std::string& operation,std::uint64_t expected,
    const std::string& prepared,std::uint64_t prepared_generation,const std::string& evidence) {
  std::lock_guard lock(g_mutex); const auto it=g_live.find(coordination);
  if(!HasAuthority(c)||it==g_live.end()||it->second.database_uuid!=c.database_uuid.canonical||it->second.session_uuid!=c.session_uuid.canonical) return Denied();
  if(it->second.provisional_prepared_uuid!=prepared||it->second.provisional_prepared_generation!=prepared_generation||!HashValue(evidence)) {
    SblrPreparedCoordinationResult r;r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.provisional_stale","provisional prepared identity or evidence mismatch");return r; }
  return Mutate(c,coordination,operation,expected,SblrPreparedCoordinationState::acquired,SblrPreparedCoordinationState::sealed,evidence,"prepared.seal");
}
SblrPreparedCoordinationResult RevokeSblrPreparedCoordination(
    const EngineRequestContext& c,const std::string& coordination,const std::string& operation,std::uint64_t expected,const std::string& reason) {
  std::lock_guard lock(g_mutex); SblrPreparedCoordinationResult r;
  if(!Reason(reason)){r.diagnostic=Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.reason_invalid","canonical reason required");return r;}
  const auto it=g_live.find(coordination); if(it==g_live.end()) return Denied();
  const auto from=it->second.state; if(from!=SblrPreparedCoordinationState::begun&&from!=SblrPreparedCoordinationState::acquired){r.diagnostic=Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.terminal","coordination already terminal");return r;}
  return Mutate(c,coordination,operation,expected,from,SblrPreparedCoordinationState::revoked,{},reason);
}
EngineApiDiagnostic RecoverSblrPreparedCoordinationRegistry(const EngineRequestContext& c) {
  std::lock_guard lock(g_mutex); if(!HasRecoveryAuthority(c)) return Diag("SECURITY.ACCESS_DENIED","sblr.prepared_coordination.recovery_denied","startup recovery authority required");
  if(c.database_path.empty()||!ValidUuid(c.database_uuid.canonical,scratchbird::core::platform::UuidKind::database)) return Diag("SBLR.OPERAND_INVALID","sblr.prepared_coordination.recovery_invalid","database identity required");
  std::unordered_map<std::string,SblrPreparedCoordinationSnapshot> states; std::uint64_t high=0;
  if(!Replay(c,&states,&high)) return Diag("SBLR.PARAMETER.STALE","sblr.prepared_coordination.corrupt","contradictory or torn durable evidence");
  g_high_water[c.database_uuid.canonical]=high;
  for(auto& [id,s]:states) if(s.state==SblrPreparedCoordinationState::begun||s.state==SblrPreparedCoordinationState::acquired) {
    const auto prior=s.coordinator_generation; s.coordinator_generation=NextGeneration(c.database_uuid.canonical); s.state=SblrPreparedCoordinationState::revoked; s.private_handle=0; s.seal_evidence_sha256.clear(); s.decision_evidence_sha256=Hash(Material(s,prior,"recovery.revoke"));
    if(s.coordinator_generation==0||!Publish(c,s,prior,"recovery.revoke")) return Diag("SBLR.EXECUTION_FAILED","sblr.prepared_coordination.recovery_publish_failed","durable recovery revocation failed");
  }
  for(auto it=g_live.begin();it!=g_live.end();) if(it->second.database_uuid==c.database_uuid.canonical) it=g_live.erase(it); else ++it;
  return Ok();
}
}  // namespace scratchbird::engine::internal_api
