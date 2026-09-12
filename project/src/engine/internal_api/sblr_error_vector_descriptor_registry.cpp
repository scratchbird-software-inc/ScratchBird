#include "sblr_error_vector_descriptor_registry.hpp"
#include "api_diagnostics.hpp"
#include "hash_digest.hpp"
#include "storage/disk/disk_device.hpp"
#include "uuid.hpp"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {
using Uuid=SblrErrorVectorUuidV1;
using Sha=SblrErrorVectorSha256V1;
using Snapshot=SblrErrorVectorDescriptorSnapshotV1;
using Result=SblrErrorVectorRegistryResultV1;
using Bytes=std::vector<std::uint8_t>;
namespace disk=storage::disk;
constexpr std::size_t kHeader=280, kFooter=64, kMaximumFrame=524720;
std::mutex registry_mutex; // No node/receipt data lives in a process-global map.
struct Failure { const char* code; const char* key; };
[[noreturn]] void Hidden() {throw Failure{"SECURITY.ACCESS_DENIED","sblr.error_vector.hidden"};}
[[noreturn]] void Stale() {throw Failure{"SBLR.ERROR_VECTOR.STALE","sblr.error_vector.registry_corrupt"};}
[[noreturn]] void Io() {throw Failure{"SBLR.EXECUTION_FAILED","sblr.error_vector.journal_io_failed"};}
[[noreturn]] void Invalid() {throw Failure{"SBLR.OPERAND_INVALID","sblr.error_vector.binding_invalid"};}
bool Valid(const Uuid& u) {return (u[6]&0xf0)==0x70 && (u[8]&0xc0)==0x80;}
Uuid ContextUuid(const EngineUuid& id) {
  if(!Valid(id.bytes))Hidden();
  return id.bytes;
}
Uuid NewUuid() {
  const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  if(now<0 || static_cast<std::uint64_t>(now)>0x0000ffffffffffffULL)Io();
  const auto id=core::uuid::GenerateEngineIdentityV7(core::platform::UuidKind::object,
                                                  static_cast<std::uint64_t>(now));
  if(!id.ok() || !Valid(id.value.value.bytes))Io();
  return id.value.value.bytes;
}
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
Sha Hash(const Bytes& b) {
  constexpr std::string_view domain="ScratchBird.SblrErrorVectorRegistryFrame.V1";
  Bytes input;input.reserve(domain.size()+b.size()-32);
  input.insert(input.end(),domain.begin(),domain.end());
  input.insert(input.end(),b.begin(),b.begin()+248);
  input.insert(input.end(),b.begin()+kHeader,b.end());
  const auto result=core::hash::ComputeSha256Digest(input);
  if(!result.ok() || result.digest_bytes!=32)Io();
  return result.digest;
}
bool SameBinding(const Snapshot& a,const Snapshot& b) {
  return a.descriptor_uuid==b.descriptor_uuid &&
      a.descriptor_generation==b.descriptor_generation &&
      a.database_uuid==b.database_uuid && a.principal_uuid==b.principal_uuid &&
      a.session_uuid==b.session_uuid && a.statement_receipt_uuid==b.statement_receipt_uuid &&
      a.registry_snapshot_uuid==b.registry_snapshot_uuid &&
      a.registry_generation==b.registry_generation &&
      a.diagnostic_registry_snapshot_uuid==b.diagnostic_registry_snapshot_uuid &&
      a.diagnostic_registry_generation==b.diagnostic_registry_generation &&
      a.vector_sha256==b.vector_sha256;
}
void Owner(const EngineRequestContext& c,const Uuid& receipt,const Snapshot& s) {
  if(s.database_uuid!=ContextUuid(c.database_uuid) ||
     s.principal_uuid!=ContextUuid(c.principal_uuid) ||
     s.session_uuid!=ContextUuid(c.session_uuid) || s.statement_receipt_uuid!=receipt)
    Hidden();
}
void Receipt(const EngineRequestContext& c,const Uuid& receipt) {
  if(!c.security_context_present || !c.statement_metadata_snapshot_engine_owned ||
     !Valid(receipt) || ContextUuid(c.statement_receipt_uuid)!=receipt)Hidden();
  (void)ContextUuid(c.principal_uuid);(void)ContextUuid(c.session_uuid);
}
EngineApiDiagnostic Success() {EngineApiDiagnostic out;out.error=false;return out;}
Result Error(const char* code,const char* key) {
  Result out;out.diagnostic=MakeEngineApiDiagnostic(code,key,{});return out;
}
template<class F> Result Run(F&& work) {
  try {
    try {std::lock_guard lock(registry_mutex);return work();}
    catch(const Failure& failure) {return Error(failure.code,failure.key);}
  } catch(const std::bad_alloc&) {
    return Error("RESOURCE.BUDGET_EXCEEDED","sblr.error_vector.allocation_failed");
  } catch(const std::length_error&) {
    return Error("RESOURCE.BUDGET_EXCEEDED","sblr.error_vector.extent_failed");
  } catch(const std::filesystem::filesystem_error&) {
    return Error("SBLR.EXECUTION_FAILED","sblr.error_vector.filesystem_failed");
  }
}
class Journal {
 public:
  std::map<Uuid,Snapshot> states;
  explicit Journal(const EngineRequestContext& c) {
    if(!c.security_context_present || !c.statement_metadata_snapshot_engine_owned ||
       c.database_path.empty())Hidden();
    database_=ContextUuid(c.database_uuid);
    std::error_code error;
    if(!std::filesystem::is_regular_file(c.database_path,error) || error)Hidden();
    path_=c.database_path+".sb.sblr_error_vector_registry.v1";
    const auto status=std::filesystem::symlink_status(path_,error);
    missing_=status.type()==std::filesystem::file_type::not_found;
    if(error && error!=std::errc::no_such_file_or_directory)Io();
    if(missing_)return;
    if(!std::filesystem::is_regular_file(status))Stale();
    if(!file_.Open(path_,disk::FileOpenMode::open_existing).ok())Io();
    const auto size=file_.Size();if(!size.ok())Io();size_=size.size_bytes;
    if(size_==0)Stale();
    std::uint64_t offset=0;
    while(offset<size_) {
      if(size_-offset<kHeader+kFooter)Stale();
      Bytes frame(kHeader);ReadExact(offset,frame.data(),frame.size());
      if(!std::equal(frame.begin(),frame.begin()+4,"EVDE") ||
         Get(frame.data(),4,2)!=1 || Get(frame.data(),6,2)!=kHeader)Stale();
      const auto extent=Get(frame.data(),8,8);
      if(extent<kHeader || extent>kMaximumFrame || extent>size_-offset-kFooter)Stale();
      frame.resize(static_cast<std::size_t>(extent));
      if(extent>kHeader)ReadExact(offset+kHeader,frame.data()+kHeader,extent-kHeader);
      std::array<std::uint8_t,kFooter> footer{};
      ReadExact(offset+extent,footer.data(),footer.size());
      Parse(frame,footer);offset+=extent+kFooter;
    }
    Sync();
  }
  void Append(Snapshot* s,std::uint8_t reason) {
    if(sequence_==UINT64_MAX)Stale();
    s->journal_sequence=sequence_+1;s->record_uuid=NewUuid();
    if(records_.count(s->record_uuid) || states.count(s->record_uuid) ||
       s->record_uuid==s->descriptor_uuid)Stale();
    const bool active=s->lifecycle==SblrErrorVectorLifecycleV1::active;
    if(active && records_.count(s->descriptor_uuid))Stale();
    Bytes frame(kHeader,0);
    if(active)frame.insert(frame.end(),s->canonical_ervd.begin(),s->canonical_ervd.end());
    if(frame.size()>kMaximumFrame || size_>UINT64_MAX-frame.size()-kFooter)Stale();
    std::copy_n("EVDE",4,frame.begin());Put(frame,4,1,2);Put(frame,6,kHeader,2);
    Put(frame,8,frame.size(),8);Put(frame,16,s->journal_sequence,8);
    frame[24]=static_cast<std::uint8_t>(s->lifecycle);frame[25]=reason;
    Copy(frame,32,s->database_uuid);Copy(frame,48,s->session_uuid);
    Copy(frame,64,s->principal_uuid);Copy(frame,80,s->statement_receipt_uuid);
    Copy(frame,96,s->descriptor_uuid);Copy(frame,112,s->record_uuid);
    Put(frame,128,s->descriptor_generation,8);Put(frame,136,s->registry_generation,8);
    Put(frame,144,s->diagnostic_registry_generation,8);
    Copy(frame,152,s->registry_snapshot_uuid);Copy(frame,168,s->diagnostic_registry_snapshot_uuid);
    Copy(frame,184,s->vector_sha256);Copy(frame,216,previous_);
    s->evidence_sha256=Hash(frame);Copy(frame,248,s->evidence_sha256);
    std::array<std::uint8_t,kFooter> footer{};std::copy_n("EVDC",4,footer.begin());
    Put(footer,4,1,2);Put(footer,6,kFooter,2);Put(footer,8,s->journal_sequence,8);
    Copy(footer,16,s->descriptor_uuid);Copy(footer,32,s->evidence_sha256);
    // All encoding and identity bookkeeping allocations precede durable mutation.
    if(!records_.insert(s->record_uuid).second)Stale();
    if(missing_) {
      if(!file_.Open(path_,disk::FileOpenMode::create_new).ok())Io();
      missing_=false;
    }
    WriteExact(size_,frame.data(),frame.size());
    if(!file_.Sync().ok())Io();
    WriteExact(size_+frame.size(),footer.data(),footer.size());Sync();
    size_+=frame.size()+footer.size();sequence_=s->journal_sequence;previous_=s->evidence_sha256;
  }
 private:
  disk::FileDevice file_;
  std::string path_;
  Uuid database_{};
  Sha previous_{};
  std::set<Uuid> records_;
  std::uint64_t size_=0,sequence_=0;
  bool missing_=false;
  // Replay remains complete and scoped to this exclusively opened journal.
  // Read ahead amortizes physical I/O and its real metrics publication without
  // retaining a node/global cache or weakening per-frame validation.
  std::array<std::uint8_t,65536> read_ahead_{};
  std::uint64_t read_ahead_offset_=0;
  std::size_t read_ahead_bytes_=0;
  void ReadExact(std::uint64_t at,void* bytes,std::uint64_t count) {
    auto* destination=static_cast<std::uint8_t*>(bytes);
    while(count) {
      if(at<read_ahead_offset_ || at-read_ahead_offset_>=read_ahead_bytes_) {
        if(at>=size_)Io();
        const auto wanted=static_cast<std::size_t>(
            std::min<std::uint64_t>(read_ahead_.size(),size_-at));
        const auto result=file_.ReadAt(at,read_ahead_.data(),wanted);
        if(!result.ok() || result.bytes_transferred!=wanted)Io();
        read_ahead_offset_=at;read_ahead_bytes_=wanted;
      }
      const auto offset=static_cast<std::size_t>(at-read_ahead_offset_);
      const auto copied=static_cast<std::size_t>(
          std::min<std::uint64_t>(count,read_ahead_bytes_-offset));
      std::copy_n(read_ahead_.data()+offset,copied,destination);
      destination+=copied;at+=copied;count-=copied;
    }
  }
  void WriteExact(std::uint64_t at,const void* bytes,std::uint64_t count) {
    const auto result=file_.WriteAt(at,bytes,count);
    if(!result.ok() || result.bytes_transferred!=count)Io();
  }
  void Sync() {
    if(!file_.Sync().ok() || !disk::SyncParentDirectoryPath(path_).ok())Io();
  }
  void Parse(const Bytes& frame,const std::array<std::uint8_t,kFooter>& footer) {
    const auto* p=frame.data();Snapshot s;Sha previous{};
    s.journal_sequence=Get(p,16,8);
    if(sequence_==UINT64_MAX || s.journal_sequence!=sequence_+1 ||
       (p[24]!=1 && p[24]!=2) || (p[24]==1 ? p[25]!=0 : (p[25]!=1 && p[25]!=2)) ||
       std::any_of(p+26,p+32,[](auto b){return b!=0;}))Stale();
    s.lifecycle=static_cast<SblrErrorVectorLifecycleV1>(p[24]);
    Read(p+32,&s.database_uuid);Read(p+48,&s.session_uuid);Read(p+64,&s.principal_uuid);
    Read(p+80,&s.statement_receipt_uuid);Read(p+96,&s.descriptor_uuid);Read(p+112,&s.record_uuid);
    s.descriptor_generation=Get(p,128,8);s.registry_generation=Get(p,136,8);
    s.diagnostic_registry_generation=Get(p,144,8);
    Read(p+152,&s.registry_snapshot_uuid);Read(p+168,&s.diagnostic_registry_snapshot_uuid);
    Read(p+184,&s.vector_sha256);Read(p+216,&previous);Read(p+248,&s.evidence_sha256);
    if(s.database_uuid!=database_)Hidden();
    for(const auto& id:{s.database_uuid,s.session_uuid,s.principal_uuid,s.statement_receipt_uuid,
                       s.descriptor_uuid,s.record_uuid,s.registry_snapshot_uuid,s.diagnostic_registry_snapshot_uuid})
      if(!Valid(id))Stale();
    if(s.descriptor_generation!=1 || !s.registry_generation || !s.diagnostic_registry_generation ||
       previous!=previous_ || s.evidence_sha256!=Hash(frame) ||
       s.record_uuid==s.descriptor_uuid || records_.count(s.descriptor_uuid) ||
       states.count(s.record_uuid) || !records_.insert(s.record_uuid).second)Stale();
    if(!std::equal(footer.begin(),footer.begin()+4,"EVDC") ||
       Get(footer.data(),4,2)!=1 || Get(footer.data(),6,2)!=kFooter ||
       Get(footer.data(),8,8)!=s.journal_sequence ||
       !std::equal(s.descriptor_uuid.begin(),s.descriptor_uuid.end(),footer.begin()+16) ||
       !std::equal(s.evidence_sha256.begin(),s.evidence_sha256.end(),footer.begin()+32))Stale();
    const auto found=states.find(s.descriptor_uuid);
    if(s.lifecycle==SblrErrorVectorLifecycleV1::active) {
      if(found!=states.end())Stale();
      s.canonical_ervd.assign(frame.begin()+kHeader,frame.end());
      engine::sblr::SblrErrorVectorDescriptorV1 decoded;std::string detail;
      if(!engine::sblr::DecodeSblrErrorVectorDescriptorV1(s.canonical_ervd.data(),s.canonical_ervd.size(),&decoded,&detail) ||
         decoded.descriptor_uuid!=s.descriptor_uuid || decoded.descriptor_generation!=s.descriptor_generation ||
         decoded.registry_snapshot_uuid!=s.registry_snapshot_uuid || decoded.registry_generation!=s.registry_generation ||
         decoded.statement_receipt_uuid!=s.statement_receipt_uuid ||
         decoded.diagnostic_registry_snapshot_uuid!=s.diagnostic_registry_snapshot_uuid ||
         decoded.diagnostic_registry_generation!=s.diagnostic_registry_generation ||
         decoded.vector_sha256!=s.vector_sha256)Stale();
      states.emplace(s.descriptor_uuid,std::move(s));
    } else {
      if(frame.size()!=kHeader || found==states.end() ||
         found->second.lifecycle!=SblrErrorVectorLifecycleV1::active || !SameBinding(found->second,s))Stale();
      found->second.lifecycle=s.lifecycle;found->second.journal_sequence=s.journal_sequence;
      found->second.record_uuid=s.record_uuid;found->second.evidence_sha256=s.evidence_sha256;
    }
    sequence_=Get(p,16,8);Read(p+248,&previous_);
  }
};
} // namespace
Result IssueSblrErrorVectorDescriptorV1(const EngineRequestContext& c,const Uuid& receipt,
    const Uuid& registry,std::uint64_t generation,const Uuid& diagnostics,std::uint64_t diagnostic_generation,
    std::vector<engine::sblr::SblrErrorVectorEntryV1> entries) {
  return Run([&] {
    Receipt(c,receipt);
    if(!Valid(registry) || !Valid(diagnostics) || !generation || !diagnostic_generation || entries.empty())Invalid();
    Journal journal(c);Result out;auto& s=out.snapshot;
    s.database_uuid=ContextUuid(c.database_uuid);s.principal_uuid=ContextUuid(c.principal_uuid);
    s.session_uuid=ContextUuid(c.session_uuid);s.statement_receipt_uuid=receipt;
    s.descriptor_uuid=NewUuid();s.descriptor_generation=1;
    if(journal.states.count(s.descriptor_uuid))Stale();
    s.registry_snapshot_uuid=registry;s.registry_generation=generation;
    s.diagnostic_registry_snapshot_uuid=diagnostics;s.diagnostic_registry_generation=diagnostic_generation;
    engine::sblr::SblrErrorVectorDescriptorV1 v;
    v.descriptor_uuid=s.descriptor_uuid;v.descriptor_generation=1;
    v.registry_snapshot_uuid=registry;v.registry_generation=generation;
    v.statement_receipt_uuid=receipt;v.diagnostic_registry_snapshot_uuid=diagnostics;
    v.diagnostic_registry_generation=diagnostic_generation;v.entries=std::move(entries);
    s.canonical_ervd=engine::sblr::EncodeSblrErrorVectorDescriptorV1(&v);
    if(s.canonical_ervd.empty())Invalid();
    s.vector_sha256=v.vector_sha256;journal.Append(&s,0);
    out.diagnostic=Success();out.ok=true;return out;
  });
}
Result LookupSblrErrorVectorDescriptorV1(const EngineRequestContext& c,const Uuid& receipt,
    const Uuid& id,std::uint64_t generation) {
  return Run([&] {
    Receipt(c,receipt);if(!Valid(id) || !generation)Invalid();
    Journal journal(c);const auto found=journal.states.find(id);
    if(found==journal.states.end())Hidden();
    Owner(c,receipt,found->second);
    if(found->second.descriptor_generation!=generation ||
       found->second.lifecycle!=SblrErrorVectorLifecycleV1::active)Stale();
    Result out;out.snapshot=found->second;out.diagnostic=Success();out.ok=true;return out;
  });
}
EngineApiDiagnostic RevokeSblrErrorVectorDescriptorsV1(const EngineRequestContext& c,const Uuid& receipt) {
  return Run([&] {
    Receipt(c,receipt);Journal journal(c);
    // Verify every matching owner before the first durable mutation.
    for(const auto& [id,s]:journal.states) {
      (void)id;if(s.statement_receipt_uuid==receipt)Owner(c,receipt,s);
    }
    for(auto& [id,s]:journal.states) {
      (void)id;if(s.statement_receipt_uuid==receipt && s.lifecycle==SblrErrorVectorLifecycleV1::active) {
        s.lifecycle=SblrErrorVectorLifecycleV1::revoked;journal.Append(&s,1);
      }
    }
    Result out;out.ok=true;out.diagnostic=Success();return out;
  }).diagnostic;
}
EngineApiDiagnostic RecoverSblrErrorVectorDescriptorRegistryV1(const EngineRequestContext& c) {
  return Run([&] {
    Journal journal(c);
    for(auto& [id,s]:journal.states) {
      (void)id;if(s.lifecycle==SblrErrorVectorLifecycleV1::active) {
        s.lifecycle=SblrErrorVectorLifecycleV1::revoked;journal.Append(&s,2);
      }
    }
    Result out;out.ok=true;out.diagnostic=Success();return out;
  }).diagnostic;
}
} // namespace scratchbird::engine::internal_api
