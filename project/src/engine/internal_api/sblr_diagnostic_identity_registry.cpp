// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "sblr_diagnostic_identity_registry.hpp"
#include "api_diagnostics.hpp"
#include "core/diagnostics/canonical_diagnostic_catalog.hpp"
#include "hash_digest.hpp"
#include "security/security_model.hpp"
#include "storage/disk/disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::engine::internal_api {
EngineApiDiagnostic MakeEngineApiDiagnostic(std::string code,std::string message_key) {
  return MakeEngineApiDiagnostic(std::move(code),std::move(message_key),{});
}
namespace {
using Bytes=std::vector<std::uint8_t>;
using Row=SblrDiagnosticIdentityRowV1;
using Snapshot=SblrDiagnosticIdentitySnapshotV1;
using Uuid=SblrDiagnosticIdentityUuidV1;
using Sha=SblrDiagnosticIdentitySha256V1;
namespace catalog=core::diagnostics;
namespace disk=storage::disk;
constexpr std::size_t kHeader=160,kFooter=64,kMaximumFrame=4194304,kMaximumJournal=67108864;
std::mutex registry_mutex;
struct RegistryFailure {const char* code;const char* key;};
[[noreturn]] void Stale(const char* key) {throw RegistryFailure{"SBLR.ERROR_VECTOR.STALE",key};}
[[noreturn]] void IoFailure() {throw RegistryFailure{"SBLR.EXECUTION_FAILED","sblr.diagnostic_registry.io_failed"};}
[[noreturn]] void Limit() {throw RegistryFailure{"RESOURCE.BUDGET_EXCEEDED","sblr.diagnostic_registry.capacity"};}
[[noreturn]] void Hidden() {throw RegistryFailure{"SECURITY.ACCESS_DENIED","sblr.diagnostic_registry.hidden"};}
bool Valid(const Uuid& id) {return (id[6]&0xf0)==0x70 && (id[8]&0xc0)==0x80;}
template<class A> bool Zero(const A& a) {return std::all_of(a.begin(),a.end(),[](auto b){return b==0;});}
std::uint64_t Get(const std::uint8_t* p,std::size_t at,unsigned width) {
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(p[at+i])<<(8*i);return n;
}
template<class B> void Put(B& b,std::size_t at,std::uint64_t n,unsigned width) {
  for(unsigned i=0;i<width;++i)b[at+i]=static_cast<std::uint8_t>(n>>(8*i));
}
template<class B,class A> void Copy(B& b,std::size_t at,const A& a) {
  std::copy(a.begin(),a.end(),b.begin()+at);
}
template<class A> void Read(const std::uint8_t* p,A* a) {std::copy_n(p,a->size(),a->begin());}
Uuid ContextIdentity(const EngineUuid& source) {
  if(!Valid(source.bytes))Hidden();
  return source.bytes;
}
Uuid NewUuid() {
  const auto millis=std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if(millis<0 || static_cast<std::uint64_t>(millis)>0x0000ffffffffffffULL)IoFailure();
  const auto generated=core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::object,
      static_cast<std::uint64_t>(millis));
  if(!generated.ok() || !Valid(generated.value.value.bytes))IoFailure();
  return generated.value.value.bytes;
}
std::uint8_t Redaction(std::uint8_t severity) {
  switch(severity) {
    case 1:case 2:case 3:case 10:return 1;
    case 4:case 5:case 6:case 7:case 8:return 3;
    case 9:case 11:case 12:return 2;
    case 13:return 4;
    default:Stale("sblr.diagnostic_registry.severity_invalid");
  }
}
bool SameMetadata(const Row& a,const Row& b) {
  return a.precedence_ordinal==b.precedence_ordinal && a.severity_code==b.severity_code &&
    a.redaction_class==b.redaction_class && a.maximum_safe_field_count==b.maximum_safe_field_count;
}
bool Granted(const EngineRequestContext& c,const char* right) {
  if(!c.authorization_context.present)return false;
  const auto decision=EvaluateMaterializedAuthorization(c,c.authorization_context,right,c.database_uuid);
  return decision.authorized && !decision.denied && !decision.policy_recheck_required;
}
void Filter(const EngineRequestContext& c,Snapshot* s) {
  const auto mask=SblrDiagnosticIdentityVisibilityMaskV1(c);
  s->rows.erase(std::remove_if(s->rows.begin(),s->rows.end(),[&](const Row& row) {
    return (mask & (std::uint16_t(1)<<row.severity_code))==0;
  }),s->rows.end());
}
Sha FrameHash(const std::uint8_t* p,std::size_t size) {
  constexpr std::string_view domain="ScratchBird.DiagnosticIdentityRegistryFrame.V1";
  Bytes material;material.reserve(domain.size()+size-32);
  material.insert(material.end(),domain.begin(),domain.end());
  material.insert(material.end(),p,p+128);
  material.insert(material.end(),p+kHeader,p+size);
  const auto digest=core::hash::ComputeSha256Digest(material);
  if(!digest.ok() || digest.digest_bytes!=32)IoFailure();
  return digest.digest;
}
void ValidateRows(const Snapshot& s) {
  if(s.rows.empty() || s.rows.size()>4096 || s.snapshot_uuid==s.database_uuid)
    Stale("sblr.diagnostic_registry.row_count");
  std::string_view previous;std::set<Uuid> identities;
  identities.insert(s.snapshot_uuid);identities.insert(s.database_uuid);
  for(std::size_t i=0;i<s.rows.size();++i) {
    const auto& row=s.rows[i];
    if(row.canonical_code.empty() || row.canonical_code.size()>512 || previous>=row.canonical_code ||
       std::any_of(row.canonical_code.begin(),row.canonical_code.end(),[](unsigned char b){return b<33 || b>126;}) ||
       row.precedence_ordinal!=i+1 || row.redaction_class!=Redaction(row.severity_code) ||
       row.maximum_safe_field_count!=145 || !identities.insert(row.diagnostic_uuid).second)
      Stale("sblr.diagnostic_registry.row_invalid");
    previous=row.canonical_code;
  }
}
void ValidateTransition(const Snapshot* previous,const Snapshot& current) {
  if(!previous) {
    if(current.generation!=1)Stale("sblr.diagnostic_registry.initial_generation");
    for(const auto& row:current.rows)if(row.diagnostic_generation!=1)
      Stale("sblr.diagnostic_registry.initial_row_generation");
    return;
  }
  if(previous->generation==UINT64_MAX || current.generation!=previous->generation+1 ||
     current.snapshot_uuid==previous->snapshot_uuid || current.source_sha256==previous->source_sha256)
    Stale("sblr.diagnostic_registry.snapshot_transition");
  std::map<std::string,const Row*,std::less<>> prior;
  std::set<Uuid> previous_ids;
  for(const auto& row:previous->rows) {prior.emplace(row.canonical_code,&row);previous_ids.insert(row.diagnostic_uuid);}
  std::size_t retained=0;
  for(const auto& row:current.rows) {
    const auto found=prior.find(row.canonical_code);
    if(found==prior.end()) {
      if(row.diagnostic_generation!=1 || previous_ids.count(row.diagnostic_uuid))
        Stale("sblr.diagnostic_registry.identity_reuse");
      continue;
    }
    ++retained;const auto& old=*found->second;const bool changed=!SameMetadata(old,row);
    if(row.diagnostic_uuid!=old.diagnostic_uuid || (changed && old.diagnostic_generation==UINT64_MAX) ||
       row.diagnostic_generation!=old.diagnostic_generation+(changed?1:0))
      Stale("sblr.diagnostic_registry.row_transition");
  }
  if(retained!=previous->rows.size())Stale("sblr.diagnostic_registry.code_removed");
}
Snapshot Parse(const Bytes& bytes,const Uuid& database,std::set<Uuid>* history) {
  Snapshot previous;bool have_previous=false;std::size_t offset=0;auto& snapshot_ids=*history;
  if(bytes.empty())Stale("sblr.diagnostic_registry.empty_file");
  while(offset<bytes.size()) {
    if(bytes.size()-offset<kHeader+kFooter)Stale("sblr.diagnostic_registry.torn_frame");
    const auto* p=bytes.data()+offset;
    if(!std::equal(p,p+4,"DIDR") || Get(p,4,2)!=1 || Get(p,6,2)!=kHeader || Get(p,60,4)!=0)
      Stale("sblr.diagnostic_registry.format_invalid");
    const auto frame_size=Get(p,8,8);
    if(frame_size<kHeader || frame_size>kMaximumFrame || frame_size>bytes.size()-offset-kFooter)
      Stale("sblr.diagnostic_registry.frame_extent");
    Snapshot current;Read(p+16,&current.snapshot_uuid);current.generation=Get(p,32,8);
    Read(p+40,&current.database_uuid);const auto count=Get(p,56,4);
    Read(p+64,&current.source_sha256);Sha prior{};Read(p+96,&prior);Read(p+128,&current.evidence_sha256);
    if(current.database_uuid!=database)Hidden();
    if(!Valid(current.snapshot_uuid) || current.generation==0 || count==0 || count>4096 ||
       (!have_previous && !Zero(prior)) || (have_previous && prior!=previous.evidence_sha256) ||
       !snapshot_ids.insert(current.snapshot_uuid).second ||
       current.evidence_sha256!=FrameHash(p,static_cast<std::size_t>(frame_size)))
      Stale("sblr.diagnostic_registry.frame_identity");
    current.rows.reserve(static_cast<std::size_t>(count));std::size_t at=kHeader;
    for(std::uint64_t i=0;i<count;++i) {
      if(frame_size-at<76)Stale("sblr.diagnostic_registry.row_extent");
      Row row;
      if(!wire::DecodeDiagnosticIdentityProjectionV1(p+at,72,&row,true) || Get(p,at+74,2)!=0)
        Stale("sblr.diagnostic_registry.row_hash");
      const auto code_size=Get(p,at+72,2);
      const auto row_size=static_cast<std::size_t>((76+code_size+3)&~std::uint64_t(3));
      if(code_size==0 || code_size>512 || row_size>frame_size-at)
        Stale("sblr.diagnostic_registry.code_extent");
      row.canonical_code.assign(reinterpret_cast<const char*>(p+at+76),static_cast<std::size_t>(code_size));
      if(std::any_of(p+at+76+code_size,p+at+row_size,[](auto b){return b!=0;}) ||
         snapshot_ids.count(row.diagnostic_uuid))
        Stale("sblr.diagnostic_registry.row_padding_or_identity");
      current.rows.push_back(std::move(row));at+=row_size;
    }
    if(at!=frame_size)Stale("sblr.diagnostic_registry.trailing_rows");
    const auto* footer=p+frame_size;Uuid footer_uuid{};Read(footer+8,&footer_uuid);Sha footer_hash{};Read(footer+32,&footer_hash);
    if(!std::equal(footer,footer+4,"DICP") || Get(footer,4,2)!=1 || Get(footer,6,2)!=kFooter ||
       footer_uuid!=current.snapshot_uuid || Get(footer,24,8)!=current.generation || footer_hash!=current.evidence_sha256)
      Stale("sblr.diagnostic_registry.commit_invalid");
    ValidateRows(current);ValidateTransition(have_previous?&previous:nullptr,current);
    previous=std::move(current);have_previous=true;offset+=static_cast<std::size_t>(frame_size)+kFooter;
  }
  return previous;
}
bool MatchesCatalog(const Snapshot& s) {
  const auto definitions=catalog::CanonicalDiagnosticCodeCatalog();
  if(s.source_sha256!=catalog::CanonicalDiagnosticCodeSourceSha256() || definitions.size!=s.rows.size())return false;
  for(std::size_t i=0;i<definitions.size;++i) {
    const auto& row=s.rows[i];const auto& definition=definitions.data[i];
    if(row.canonical_code!=definition.code || row.severity_code!=static_cast<std::uint8_t>(definition.severity) ||
       row.redaction_class!=Redaction(row.severity_code) || row.maximum_safe_field_count!=145)return false;
  }
  return true;
}
Snapshot Normalize(const Uuid& database,const Snapshot* previous) {
  const auto definitions=catalog::CanonicalDiagnosticCodeCatalog();
  if(definitions.size==0 || definitions.size>4096)Limit();
  Snapshot s;s.database_uuid=database;s.snapshot_uuid=NewUuid();
  if(previous && previous->generation==UINT64_MAX)Stale("sblr.diagnostic_registry.generation_overflow");
  s.generation=previous?previous->generation+1:1;s.source_sha256=catalog::CanonicalDiagnosticCodeSourceSha256();
  std::map<std::string,const Row*,std::less<>> prior;
  if(previous)for(const auto& row:previous->rows)prior.emplace(row.canonical_code,&row);
  s.rows.reserve(definitions.size);
  for(std::size_t i=0;i<definitions.size;++i) {
    const auto& definition=definitions.data[i];Row row;row.canonical_code=definition.code;
    row.precedence_ordinal=static_cast<std::uint32_t>(i+1);row.severity_code=static_cast<std::uint8_t>(definition.severity);
    row.redaction_class=Redaction(row.severity_code);row.maximum_safe_field_count=145;
    const auto found=prior.find(definition.code);
    if(found==prior.end()) {row.diagnostic_uuid=NewUuid();row.diagnostic_generation=1;}
    else {
      const auto& old=*found->second;row.diagnostic_uuid=old.diagnostic_uuid;
      const bool changed=!SameMetadata(old,row);
      if(changed && old.diagnostic_generation==UINT64_MAX)Stale("sblr.diagnostic_registry.row_generation_overflow");
      row.diagnostic_generation=old.diagnostic_generation+(changed?1:0);
    }
    wire::DiagnosticIdentityProjectionBytesV1 bytes{};
    if(!wire::EncodeDiagnosticIdentityProjectionV1(&row,&bytes,true))IoFailure();
    s.rows.push_back(std::move(row));
  }
  ValidateRows(s);ValidateTransition(previous,s);return s;
}
Bytes Encode(Snapshot* s,const Sha& previous_hash) {
  Bytes bytes(kHeader,0);std::copy_n("DIDR",4,bytes.begin());Put(bytes,4,1,2);Put(bytes,6,kHeader,2);
  Copy(bytes,16,s->snapshot_uuid);Put(bytes,32,s->generation,8);Copy(bytes,40,s->database_uuid);
  Put(bytes,56,s->rows.size(),4);Copy(bytes,64,s->source_sha256);Copy(bytes,96,previous_hash);
  for(auto& row:s->rows) {
    wire::DiagnosticIdentityProjectionBytesV1 projection{};
    if(!wire::EncodeDiagnosticIdentityProjectionV1(&row,&projection,true))IoFailure();
    const std::size_t at=bytes.size(),extent=(76+row.canonical_code.size()+3)&~std::size_t(3);
    if(extent>kMaximumFrame-at)Limit();
    bytes.resize(at+extent,0);Copy(bytes,at,projection);
    Put(bytes,at+72,row.canonical_code.size(),2);
    std::copy(row.canonical_code.begin(),row.canonical_code.end(),bytes.begin()+at+76);
  }
  Put(bytes,8,bytes.size(),8);s->evidence_sha256=FrameHash(bytes.data(),bytes.size());
  Copy(bytes,128,s->evidence_sha256);return bytes;
}
Snapshot Load(const EngineRequestContext& c) {
  if(!c.security_context_present || !c.statement_metadata_snapshot_engine_owned || c.database_path.empty())Hidden();
  const auto database=ContextIdentity(c.database_uuid);
  (void)ContextIdentity(c.principal_uuid);(void)ContextIdentity(c.session_uuid);
  std::error_code error;
  if(!std::filesystem::is_regular_file(c.database_path,error) || error)Hidden();
  const auto path=c.database_path+".sb.sblr_diagnostic_identity_registry.v1";
  const auto state=std::filesystem::symlink_status(path,error);
  const bool missing=state.type()==std::filesystem::file_type::not_found;
  if(error && error!=std::errc::no_such_file_or_directory)IoFailure();
  if(!missing && !std::filesystem::is_regular_file(state))Stale("sblr.diagnostic_registry.file_kind");
  disk::FileDevice file;
  std::uint64_t size_bytes=0;
  std::set<Uuid> history;
  Snapshot current;bool have_current=false;
  if(!missing) {
    if(!file.Open(path,disk::FileOpenMode::open_existing).ok())IoFailure();
    const auto size=file.Size();if(!size.ok())IoFailure();if(size.size_bytes>kMaximumJournal)Limit();
    size_bytes=size.size_bytes;
    Bytes data(static_cast<std::size_t>(size_bytes));
    const auto read=file.ReadAt(0,data.data(),data.size());
    if(!read.ok() || read.bytes_transferred!=data.size())IoFailure();
    current=Parse(data,database,&history);have_current=true;
    if(current.source_sha256==catalog::CanonicalDiagnosticCodeSourceSha256() && !MatchesCatalog(current))
      Stale("sblr.diagnostic_registry.installed_metadata_mismatch");
  }
  if(!have_current || !MatchesCatalog(current)) {
    Snapshot next=Normalize(database,have_current?&current:nullptr);
    if(history.count(next.snapshot_uuid))Stale("sblr.diagnostic_registry.snapshot_reuse");
    for(const auto& row:next.rows)if(history.count(row.diagnostic_uuid))
      Stale("sblr.diagnostic_registry.snapshot_row_identity_reuse");
    const auto bytes=Encode(&next,have_current?current.evidence_sha256:Sha{});
    if(bytes.size()+kFooter>kMaximumJournal-size_bytes)Limit();
    // Do not create even an empty journal until all normalization, evidence
    // encoding and capacity checks have succeeded.
    if(missing && !file.Open(path,disk::FileOpenMode::create_new).ok())IoFailure();
    const auto evidence=file.WriteAt(size_bytes,bytes.data(),bytes.size());
    if(!evidence.ok() || evidence.bytes_transferred!=bytes.size() || !file.Sync().ok())IoFailure();
    std::array<std::uint8_t,kFooter> footer{};std::copy_n("DICP",4,footer.begin());
    Put(footer,4,1,2);Put(footer,6,kFooter,2);Copy(footer,8,next.snapshot_uuid);
    Put(footer,24,next.generation,8);Copy(footer,32,next.evidence_sha256);
    const auto committed=file.WriteAt(size_bytes+bytes.size(),footer.data(),footer.size());
    if(!committed.ok() || committed.bytes_transferred!=footer.size())IoFailure();
    current=std::move(next);
  }
  // Also establishes durability after a previous unacknowledged complete
  // footer. No cached parse success substitutes for a successful sync.
  if(!file.Sync().ok() || !disk::SyncParentDirectoryPath(path).ok())IoFailure();
  return current;
}
SblrDiagnosticIdentityResultV1 Failure(const char* code,const char* key) {
  SblrDiagnosticIdentityResultV1 out;out.diagnostic=MakeEngineApiDiagnostic(code,key);return out;
}
}
std::uint16_t SblrDiagnosticIdentityVisibilityMaskV1(const EngineRequestContext& c) {
  if(!c.security_context_present || !c.statement_metadata_snapshot_engine_owned)return 0;
  std::uint16_t mask=0x05fe; // Canonical 1..8 and notice10 only.
  if(Granted(c,"READ_DIAGNOSTIC_DETAIL")) {
    mask|=(std::uint16_t(1)<<9)|(std::uint16_t(1)<<12);
    if(Granted(c,"AUDIT_READ"))mask|=std::uint16_t(1)<<11;
  }
  return mask;
}
SblrDiagnosticIdentityResultV1 LoadSblrDiagnosticIdentitySnapshotV1(const EngineRequestContext& c) {
  try {
    try {
      std::lock_guard guard(registry_mutex);auto snapshot=Load(c);Filter(c,&snapshot);
      SblrDiagnosticIdentityResultV1 out;out.snapshot=std::move(snapshot);out.diagnostic.error=false;out.ok=true;return out;
    }catch(const RegistryFailure& failure) {return Failure(failure.code,failure.key);}
  }catch(const std::bad_alloc&) {return Failure("RESOURCE.BUDGET_EXCEEDED","sblr.diagnostic_registry.allocation_failed");}
   catch(const std::length_error&) {return Failure("RESOURCE.BUDGET_EXCEEDED","sblr.diagnostic_registry.extent_failed");}
}
SblrDiagnosticIdentityResultV1 LookupSblrDiagnosticIdentityV1(const EngineRequestContext& c,
    const Snapshot& frozen,const Uuid& id,std::uint64_t generation) {
  try {
    auto loaded=LoadSblrDiagnosticIdentitySnapshotV1(c);if(!loaded.ok)return loaded;
    if(loaded.snapshot.snapshot_uuid!=frozen.snapshot_uuid || loaded.snapshot.generation!=frozen.generation)
      return Failure("SBLR.ERROR_VECTOR.STALE","sblr.diagnostic_registry.stale");
    const auto found=std::find_if(loaded.snapshot.rows.begin(),loaded.snapshot.rows.end(),[&](const Row& row) {
      return row.diagnostic_uuid==id && row.diagnostic_generation==generation;
    });
    // Keep refusal construction inside the allocation-failure boundary too.
    if(found==loaded.snapshot.rows.end())
      return Failure("SECURITY.ACCESS_DENIED","sblr.diagnostic_registry.hidden");
    loaded.row=*found;return loaded;
  }catch(const std::bad_alloc&) {return Failure("RESOURCE.BUDGET_EXCEEDED","sblr.diagnostic_registry.allocation_failed");}
   catch(const std::length_error&) {return Failure("RESOURCE.BUDGET_EXCEEDED","sblr.diagnostic_registry.extent_failed");}
}
} // namespace scratchbird::engine::internal_api
