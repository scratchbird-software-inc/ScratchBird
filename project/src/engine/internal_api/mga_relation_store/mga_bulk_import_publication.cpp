// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_bulk_import_publication.hpp"
#include "mga_relation_store/mga_bulk_import_codec.hpp"
#include "api_diagnostics.hpp"
#include "storage/disk/disk_device.hpp"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <type_traits>

namespace scratchbird::engine::internal_api {
namespace {
// SEARCH_KEY: SB_ENGINE_MGA_BULK_IMPORT_PUBLICATION_IMPLEMENTATION_AUTHORITY
// Owns the binary bulk-import companion publication records. Transaction visibility and finality remain with MGA.
namespace binary=bulk_import_binary;
namespace disk=storage::disk;
using binary::Bytes;using binary::Sha;using binary::Valid;using binary::Nonzero;
using Publication=MgaBulkImportPublicationRecordV1;
using Event=MgaBulkImportImportedRowEventV1;
using PubResult=MgaBulkImportPublicationResultV1;
using RowResult=MgaBulkImportImportedRowEventResultV1;
using Life=MgaBulkImportPublicationLifecycleV1;
constexpr std::size_t kFileHeader=32,kHeader=208,kFooter=64;
constexpr std::string_view kFrameMagic{"BICF2\0\0\0",8},kFooterMagic{"BICC2\0\0\0",8};
constexpr std::string_view kHeaderDomain="ScratchBird.MgaBulkImportCompanionHeader.V2";
constexpr std::string_view kFrameDomain="ScratchBird.MgaBulkImportCompanionFrame.V2";
std::mutex journal_mutex; // Synchronization only; no cached node/receipt state.
std::atomic<std::uint64_t> temporary_ordinal{0};
struct Failure { const char* code;const char* key; };
[[noreturn]] void Conflict() {throw Failure{"BULK.IMPORT.RECOVERY_CONFLICT","mga.bulk_import.companion_invalid"};}
[[noreturn]] void Io() {throw Failure{"BULK.IMPORT.RECOVERY_CONFLICT","mga.bulk_import.companion_io_unconfirmed"};}
[[noreturn]] void Hidden() {throw Failure{"SECURITY.ACCESS_DENIED","mga.bulk_import.companion_hidden"};}
[[noreturn]] void Invalid() {throw Failure{"SBLR.OPERAND_INVALID","mga.bulk_import.companion_request_invalid"};}
template<class R> R Success() {
  R r;r.diagnostic=MakeEngineApiDiagnostic("OK","ok",{},false);r.ok=true;return r;
}
template<class R> R Error(const char* code,const char* key) {
  R r;r.diagnostic=MakeEngineApiDiagnostic(code,key,{});return r;
}
template<class R,class F> R Run(F&& f) {
  try {
    try {std::lock_guard lock(journal_mutex);return f();}
    catch(const Failure& e){return Error<R>(e.code,e.key);}
  } catch(const std::bad_alloc&) {return Error<R>("RESOURCE.BUDGET_EXCEEDED","mga.bulk_import.companion_allocation_failed");}
    catch(const std::length_error&) {return Error<R>("RESOURCE.BUDGET_EXCEEDED","mga.bulk_import.companion_extent_failed");}
    catch(const std::filesystem::filesystem_error&) {return Error<R>("BULK.IMPORT.RECOVERY_CONFLICT","mga.bulk_import.companion_filesystem_failed");}
}
void Context(const EngineRequestContext& c) {
  if(!c.security_context_present || !Valid(c.database_uuid) || !Valid(c.principal_uuid) ||
     !Valid(c.session_uuid) || !Valid(c.statement_receipt_uuid) || !Valid(c.transaction_uuid) ||
     !Valid(c.statement_uuid) || !c.local_transaction_id || c.database_path.empty())Hidden();
  std::error_code e;
  if(!std::filesystem::is_regular_file(c.database_path,e) || e)Hidden();
}
struct Entry {
  EngineUuid principal,session,receipt;
  Publication publication;
  std::vector<Event> events;
};
template<class R> void BodyOwner(const EngineRequestContext& c,const R& r) {
  if(r.owning_transaction_uuid!=c.transaction_uuid ||
     r.owning_local_transaction_id!=c.local_transaction_id ||
     r.statement_uuid!=c.statement_uuid)Hidden();
}
void Owner(const EngineRequestContext& c,const Entry& e,unsigned kind) {
  if(e.principal!=c.principal_uuid || e.session!=c.session_uuid ||
     e.receipt!=c.statement_receipt_uuid)Hidden();
  if(kind==1)BodyOwner(c,e.publication);
  else {if(e.events.empty())Conflict();BodyOwner(c,e.events.front());}
}
bool SameAuthority(Publication a,Publication b) {
  a.lifecycle=b.lifecycle=Life::prepared;return a==b;
}
bool SameOwner(const Entry& a,const Entry& b) {
  return a.principal==b.principal && a.session==b.session && a.receipt==b.receipt;
}
void Cohort(const std::vector<Event>& events) {
  if(events.empty())Invalid();
  const auto& first=events.front();
  std::set<std::array<std::uint8_t,16>> ids;
  for(std::size_t i=0;i<events.size();++i) {
    const auto& e=events[i];
    if(!binary::Shape(e) || e.import_ordinal!=i+1 ||
       e.durable_publication_uuid!=first.durable_publication_uuid ||
       e.recovery_idempotency_key!=first.recovery_idempotency_key ||
       e.mutation_uuid!=first.mutation_uuid || e.bulk_batch_uuid!=first.bulk_batch_uuid ||
       e.owning_transaction_uuid!=first.owning_transaction_uuid ||
       e.owning_local_transaction_id!=first.owning_local_transaction_id ||
       e.statement_uuid!=first.statement_uuid || e.savepoint_ordinal!=first.savepoint_ordinal ||
       e.target_relation_uuid!=first.target_relation_uuid ||
       e.target_relation_generation!=first.target_relation_generation)Invalid();
    for(const auto& id:{e.row_uuid,e.row_version_uuid,e.row_image_uuid}) {
      if(id==first.durable_publication_uuid || id==first.mutation_uuid ||
         id==first.bulk_batch_uuid || !ids.insert(id.bytes).second)Conflict();
    }
  }
}
Sha Postcondition(const std::vector<Event>& events) {
  if(events.empty())Conflict();
  Bytes bytes;binary::Append(bytes,events.front().durable_publication_uuid.bytes);
  binary::Number(bytes,events.size(),8);
  for(const auto& e:events) {binary::Number(bytes,e.import_ordinal,8);binary::Append(bytes,e.event_evidence_sha256);}
  return binary::Hash("ScratchBird.BulkImportStreamImportedRows.V1",bytes);
}
void MatchRows(const Publication& p,const Entry& e) {
  Cohort(e.events);const auto& r=e.events.front();
  if(e.receipt!=p.authenticated_receipt_uuid ||
     r.durable_publication_uuid!=p.durable_publication_uuid ||
     r.recovery_idempotency_key!=p.recovery_idempotency_key ||
     r.mutation_uuid!=p.mutation_uuid || r.bulk_batch_uuid!=p.bulk_batch_uuid ||
     r.owning_transaction_uuid!=p.owning_transaction_uuid ||
     r.owning_local_transaction_id!=p.owning_local_transaction_id ||
     r.statement_uuid!=p.statement_uuid || r.savepoint_ordinal!=p.savepoint_ordinal ||
     r.target_relation_uuid!=p.target_relation_uuid ||
     r.target_relation_generation!=p.target_relation_generation ||
     e.events.size()!=p.imported_row_postcondition_count ||
     Postcondition(e.events)!=p.imported_row_postcondition_sha256)Conflict();
  for(const auto& row:e.events)
    if(row.column_descriptor_set_sha256!=p.column_descriptor_set_sha256)Conflict();
}
void Put(Bytes& b,std::size_t at,std::uint64_t v,unsigned width) {
  for(unsigned i=0;i<width;++i)b[at+i]=static_cast<std::uint8_t>(v>>(i*8));
}
template<class A> void Copy(Bytes& b,std::size_t at,const A& a) {
  std::copy(a.begin(),a.end(),b.begin()+at);
}
bool Magic(std::span<const std::uint8_t> bytes,std::string_view expected) {
  return bytes.size()>=expected.size() && std::equal(expected.begin(),expected.end(),bytes.begin());
}
class Journal {
 public:
  std::map<Sha,Entry> entries;
  Journal(const EngineRequestContext& c,unsigned kind,const Sha* recovery_key=nullptr)
      : context_(c),kind_(kind),path_(c.database_path+(kind==1?
          ".sb.mga_bulk_import_publication.v1":".sb.mga_bulk_import_imported_rows.v1")) {
    Context(c);
    std::error_code ec;const auto status=std::filesystem::symlink_status(path_,ec);
    if(status.type()==std::filesystem::file_type::not_found &&
       (!ec || ec==std::errc::no_such_file_or_directory)){missing_=true;return;}
    if(ec)Io();
    if(!std::filesystem::is_regular_file(status))Conflict();
    if(!file_.Open(path_,disk::FileOpenMode::open_existing).ok())Io();
    const auto size=file_.Size();if(!size.ok())Io();size_=size.size_bytes;
    if(size_<kFileHeader)Conflict();
    Bytes header(kFileHeader);Read(0,header);
    EngineUuid database;binary::Read(header,16,database.bytes);
    if(!Magic(header,FileMagic()) || binary::ReadNumber(header,8,2)!=2 ||
       binary::ReadNumber(header,10,2)!=kFileHeader ||
       binary::ReadNumber(header,12,4)!=kind_)Conflict();
    if(database!=c.database_uuid)Hidden();
    std::uint64_t offset=kFileHeader;
    while(offset<size_) {
      if(size_-offset<kHeader) {
        Bytes partial(static_cast<std::size_t>(size_-offset));Read(offset,partial);
        const auto checked=std::min(partial.size(),kFrameMagic.size());
        if(!std::equal(partial.begin(),partial.begin()+checked,kFrameMagic.begin()))Conflict();
        // There is no authenticated frame/key authority in an incomplete
        // header. Never erase it on the strength of a caller-selected key.
        Conflict();
      }
      Bytes h(kHeader);Read(offset,h);
      if(!Magic(h,kFrameMagic) || h[24]!=kind_ ||
         std::any_of(h.begin()+26,h.begin()+32,[](auto b){return b!=0;}))Conflict();
      Sha digest,previous,key;binary::Read(h,176,digest);binary::Read(h,144,previous);binary::Read(h,96,key);
      if(digest!=binary::Hash(kHeaderDomain,{h.data(),176}) || !Nonzero(key))Conflict();
      const auto sequence=binary::ReadNumber(h,16,8);
      if(sequence_==UINT64_MAX || sequence!=sequence_+1 || previous!=previous_)Conflict();
      EngineUuid db;binary::Read(h,32,db.bytes);if(db!=context_.database_uuid)Hidden();
      Entry e;binary::Read(h,48,e.principal.bytes);binary::Read(h,64,e.session.bytes);binary::Read(h,80,e.receipt.bytes);
      if(!Valid(e.principal) || !Valid(e.session) || !Valid(e.receipt))Conflict();
      const auto count=binary::ReadNumber(h,128,8),payload=binary::ReadNumber(h,136,8);
      const auto total=binary::ReadNumber(h,8,8);
      const auto unit=kind_==1?binary::kPublicationBytes:binary::kEventBytes;
      if(!count || (kind_==1 && count!=1) || count>UINT64_MAX/unit ||
         payload!=count*unit || payload>UINT64_MAX-kHeader-kFooter ||
         total!=kHeader+payload+kFooter ||
         (kind_==1?(h[25]<1 || h[25]>3):h[25]!=0))Conflict();
      if(total>size_-offset) {
        if(!recovery_key || key!=*recovery_key)Conflict();
        if(e.principal!=c.principal_uuid || e.session!=c.session_uuid ||
           e.receipt!=c.statement_receipt_uuid)Hidden();
        // A complete canonical body or an already validated immutable record
        // must establish transaction/statement ownership before tail repair.
        const auto prior=entries.find(key);
        if(prior!=entries.end()) {
          Owner(c,prior->second,kind_);
          if(kind_!=1 || prior->second.publication.lifecycle!=Life::prepared ||
             h[25]==static_cast<std::uint8_t>(Life::prepared))Conflict();
        } else if(kind_==1 && h[25]!=static_cast<std::uint8_t>(Life::prepared))Conflict();
        if(payload<=size_-offset-kHeader) {
          if(payload>Bytes{}.max_size())throw std::length_error("bulk companion tail");
          Bytes body(static_cast<std::size_t>(payload));Read(offset+kHeader,body);
          if(kind_==1) {
            if(!binary::Decode(body,&e.publication) ||
               e.publication.recovery_idempotency_key!=key ||
               e.publication.authenticated_receipt_uuid!=e.receipt)Conflict();
            Owner(c,e,kind_);
            if(prior!=entries.end() &&
               !SameAuthority(prior->second.publication,e.publication))Conflict();
          } else {
            if(count>e.events.max_size())throw std::length_error("bulk companion tail cohort");
            e.events.reserve(static_cast<std::size_t>(count));
            for(std::uint64_t i=0;i<count;++i) {
              Event row;
              if(!binary::Decode({body.data()+i*unit,unit},&row) ||
                 row.recovery_idempotency_key!=key)Conflict();
              BodyOwner(c,row);
              e.events.push_back(row);
            }
            Cohort(e.events);
          }
          // Even an incomplete footer must agree with the commit bytes that
          // would seal this exact frame. Corrupt visible bytes are not a tear.
          Bytes expected(kFooter,0);Copy(expected,0,kFooterMagic);
          Put(expected,8,total,8);Put(expected,16,sequence,8);
          Bytes material=h;binary::Append(material,body);
          material.insert(material.end(),expected.begin(),expected.begin()+32);
          const auto seal=binary::Hash(kFrameDomain,material);
          if(!Nonzero(seal))Io();Copy(expected,32,seal);
          Bytes visible(static_cast<std::size_t>(size_-offset-kHeader-payload));
          Read(offset+kHeader+payload,visible);
          if(!std::equal(visible.begin(),visible.end(),expected.begin()))Conflict();
          pending_entry_=std::move(e);
        } else {
          if(prior==entries.end())Conflict();
          auto immutable=prior->second.publication;
          const auto expected=binary::Encode(immutable);
          Bytes visible(static_cast<std::size_t>(size_-offset-kHeader));Read(offset+kHeader,visible);
          if(expected.size()!=payload ||
             !std::equal(visible.begin(),visible.end(),expected.begin()))Conflict();
        }
        // Stage the repair; the selected committed record is checked before
        // any mutation, and outputs are constructed before the truncate.
        repair_offset_=offset;break;
      }
      if(payload>Bytes{}.max_size())throw std::length_error("bulk companion payload");
      Bytes body(static_cast<std::size_t>(payload));Read(offset+kHeader,body);
      Bytes footer(kFooter);Read(offset+kHeader+payload,footer);
      if(!Magic(footer,kFooterMagic) || binary::ReadNumber(footer,8,8)!=total ||
         binary::ReadNumber(footer,16,8)!=sequence ||
         binary::ReadNumber(footer,24,8)!=0)Conflict();
      Bytes frame=h;binary::Append(frame,body);
      frame.insert(frame.end(),footer.begin(),footer.begin()+32);
      Sha seal;binary::Read(footer,32,seal);
      if(!Nonzero(seal) || seal!=binary::Hash(kFrameDomain,frame))Conflict();
      if(kind_==1) {
        if(!binary::Decode(body,&e.publication) ||
           e.publication.recovery_idempotency_key!=key ||
           e.publication.authenticated_receipt_uuid!=e.receipt)Conflict();
        e.publication.lifecycle=static_cast<Life>(h[25]);
      } else {
        if(count>e.events.max_size())throw std::length_error("bulk companion events");
        e.events.reserve(static_cast<std::size_t>(count));
        for(std::uint64_t i=0;i<count;++i) {
          Event row;
          if(!binary::Decode({body.data()+i*unit,unit},&row) || row.recovery_idempotency_key!=key)Conflict();
          e.events.push_back(row);
        }
        Cohort(e.events);
      }
      const auto found=entries.find(key);
      if(kind_==1) {
        if(found==entries.end()) {if(e.publication.lifecycle!=Life::prepared)Conflict();}
        else if(!SameOwner(found->second,e) || !SameAuthority(found->second.publication,e.publication) ||
                found->second.publication.lifecycle!=Life::prepared ||
                e.publication.lifecycle==Life::prepared)Conflict();
      } else if(found!=entries.end())Conflict();
      entries.insert_or_assign(key,std::move(e));sequence_=sequence;previous_=seal;offset+=total;
    }
    // Unknown successful footer writes are not exposed until durability has
    // been re-established. These barriers are also required after tail repair.
    if(!file_.Sync().ok() || !disk::SyncParentDirectoryPath(path_).ok())Io();
  }
  Entry* Find(const Sha& key) {
    if(!Nonzero(key))Invalid();
    const auto it=entries.find(key);if(it==entries.end())return nullptr;
    Owner(context_,it->second,kind_);return &it->second;
  }
  void FinishRecovery() {
    if(repair_offset_)Repair(repair_offset_);
  }
  const Entry* PendingEntry() const {
    return pending_entry_?&*pending_entry_:nullptr;
  }
  void AppendFrame(Entry entry) {
    if(sequence_==UINT64_MAX)Conflict();
    Bytes body;
    Sha key;
    std::uint64_t count;
    if(kind_==1) {
      body=binary::Encode(entry.publication);key=entry.publication.recovery_idempotency_key;count=1;
      if(body.empty())Invalid();
    } else {
      Cohort(entry.events);count=entry.events.size();key=entry.events.front().recovery_idempotency_key;
      if(count>body.max_size()/binary::kEventBytes)throw std::length_error("bulk companion cohort");
      body.reserve(count*binary::kEventBytes);
      for(auto& event:entry.events){const auto bytes=binary::Encode(event);if(bytes.empty())Invalid();binary::Append(body,bytes);}
    }
    if(body.size()>UINT64_MAX-kHeader-kFooter || size_>UINT64_MAX-kHeader-kFooter-body.size())
      throw std::length_error("bulk companion journal");
    Bytes frame(kHeader,0);Copy(frame,0,kFrameMagic);
    const auto total=kHeader+body.size()+kFooter,sequence=sequence_+1;
    Put(frame,8,total,8);Put(frame,16,sequence,8);frame[24]=kind_;
    frame[25]=kind_==1?static_cast<std::uint8_t>(entry.publication.lifecycle):0;
    Copy(frame,32,context_.database_uuid.bytes);Copy(frame,48,entry.principal.bytes);
    Copy(frame,64,entry.session.bytes);Copy(frame,80,entry.receipt.bytes);Copy(frame,96,key);
    Put(frame,128,count,8);Put(frame,136,body.size(),8);Copy(frame,144,previous_);
    const auto header_hash=binary::Hash(kHeaderDomain,{frame.data(),176});
    if(!Nonzero(header_hash))Io();Copy(frame,176,header_hash);binary::Append(frame,body);
    Bytes footer(kFooter,0);Copy(footer,0,kFooterMagic);Put(footer,8,total,8);Put(footer,16,sequence,8);
    Bytes material=frame;material.insert(material.end(),footer.begin(),footer.begin()+32);
    const auto hash=binary::Hash(kFrameDomain,material);if(!Nonzero(hash))Io();Copy(footer,32,hash);
    if(missing_) {Install(frame,footer);return;}
    Write(file_,size_,frame);
    if(!file_.Sync().ok())Io();
    Write(file_,size_+frame.size(),footer);
    if(!file_.Sync().ok() || !disk::SyncParentDirectoryPath(path_).ok())Io();
  }
 private:
  const EngineRequestContext& context_;
  unsigned kind_;
  std::string path_;
  disk::FileDevice file_;
  bool missing_=false;
  std::uint64_t size_=kFileHeader,sequence_=0;
  std::uint64_t repair_offset_=0;
  Sha previous_{};
  std::optional<Entry> pending_entry_;
  std::string_view FileMagic() const {
    return kind_==1?std::string_view{"BIPJ2\0\0\0",8}:std::string_view{"BIRJ2\0\0\0",8};
  }
  void Read(std::uint64_t offset,Bytes& out) {
    const auto io=file_.ReadAt(offset,out.data(),out.size());
    if(!io.ok() || io.bytes_transferred!=out.size())Io();
  }
  static void Write(disk::FileDevice& file,std::uint64_t offset,const Bytes& bytes) {
    const auto io=file.WriteAt(offset,bytes.data(),bytes.size());
    if(!io.ok() || io.bytes_transferred!=bytes.size())Io();
  }
  void Repair(std::uint64_t offset) {
    std::error_code ec;std::filesystem::resize_file(path_,offset,ec);
    if(ec || !file_.Sync().ok() || !disk::SyncParentDirectoryPath(path_).ok())Io();
    size_=offset;
  }
  void Install(const Bytes& frame,const Bytes& footer) {
    Bytes header(kFileHeader,0);Copy(header,0,FileMagic());Put(header,8,2,2);
    Put(header,10,kFileHeader,2);Put(header,12,kind_,4);Copy(header,16,context_.database_uuid.bytes);
    const auto ordinal=temporary_ordinal.fetch_add(1,std::memory_order_relaxed);
    const auto tick=std::chrono::steady_clock::now().time_since_epoch().count();
    const auto name=path_+".tmp."+std::to_string(tick)+"."+std::to_string(ordinal);
    const std::filesystem::path temporary(name),lock(name+".sb.owner.lock");
    struct Cleanup {
      const std::filesystem::path& file;const std::filesystem::path& lock;
      bool owned=false,lock_owned=false;
      ~Cleanup() {try {std::error_code e;if(owned)std::filesystem::remove(file,e);
                       if(lock_owned)std::filesystem::remove(lock,e);}catch(...){}}
    } cleanup{temporary,lock};
    {
      disk::FileDevice file;if(!file.Open(name,disk::FileOpenMode::create_new).ok())Io();
      cleanup.owned=cleanup.lock_owned=true;
      Write(file,0,header);Write(file,kFileHeader,frame);
      if(!file.Sync().ok())Io();Write(file,kFileHeader+frame.size(),footer);
      if(!file.Sync().ok() || !file.Close().ok())Io();
    }
    std::error_code ec;std::filesystem::rename(temporary,path_,ec);if(ec)Io();cleanup.owned=false;
    if(!disk::SyncParentDirectoryPath(path_).ok())Io();
  }
};
Entry Owned(const EngineRequestContext& c) {
  Entry e;e.principal=c.principal_uuid;e.session=c.session_uuid;e.receipt=c.statement_receipt_uuid;return e;
}
PubResult Select(Journal& journal,const Sha& key) {
  auto out=Success<PubResult>();
  if(const auto* entry=journal.Find(key)){out.found=true;out.record=entry->publication;}
  return out;
}
PubResult Transition(const EngineRequestContext& c,const Publication& requested,Life target) {
  Context(c);Journal journal(c,1);auto* existing=journal.Find(requested.recovery_idempotency_key);
  if(!existing) {
    if(target==Life::aborted)return Success<PubResult>();
    Conflict();
  }
  auto canonical=requested;
  if(binary::Encode(canonical).empty() || !SameAuthority(existing->publication,canonical))Conflict();
  if(existing->publication.lifecycle!=target &&
     existing->publication.lifecycle!=Life::prepared)Conflict();
  if(target==Life::published_uncommitted) {
    Journal rows(c,2);const auto* cohort=rows.Find(canonical.recovery_idempotency_key);
    if(!cohort || !SameOwner(*existing,*cohort))Conflict();
    MatchRows(existing->publication,*cohort);
  }
  if(existing->publication.lifecycle==target) {auto out=Select(journal,canonical.recovery_idempotency_key);out.replayed=true;return out;}
  auto next=*existing;next.publication.lifecycle=target;
  auto out=Success<PubResult>();out.found=true;out.record=next.publication;
  journal.AppendFrame(std::move(next));return out;
}
static_assert(std::is_nothrow_move_constructible_v<PubResult>);
static_assert(std::is_nothrow_move_constructible_v<RowResult>);
} // namespace

PubResult PrepareMgaBulkImportPublicationV1(const EngineRequestContext& c,const Publication& requested) {
  return Run<PubResult>([&] {
    Context(c);auto entry=Owned(c);entry.publication=requested;entry.publication.lifecycle=Life::prepared;
    if(binary::Encode(entry.publication).empty())Invalid();
    if(entry.publication.authenticated_receipt_uuid!=c.statement_receipt_uuid)Hidden();
    BodyOwner(c,entry.publication);Journal journal(c,1);
    if(const auto* existing=journal.Find(entry.publication.recovery_idempotency_key)) {
      if(!SameAuthority(existing->publication,entry.publication))Conflict();
      auto out=Select(journal,entry.publication.recovery_idempotency_key);out.replayed=true;return out;
    }
    auto out=Success<PubResult>();out.found=true;out.record=entry.publication;
    journal.AppendFrame(std::move(entry));return out;
  });
}
PubResult PublishMgaBulkImportPublicationV1(const EngineRequestContext& c,const Publication& requested) {
  return Run<PubResult>([&]{return Transition(c,requested,Life::published_uncommitted);});
}
PubResult AbortMgaBulkImportPublicationV1(const EngineRequestContext& c,const Publication& requested) {
  return Run<PubResult>([&]{return Transition(c,requested,Life::aborted);});
}
PubResult RecoverMgaBulkImportPublicationV1(const EngineRequestContext& c,const Sha& key) {
  return Run<PubResult>([&]{Context(c);if(!Nonzero(key))Invalid();Journal journal(c,1,&key);
    auto out=Select(journal,key);
    if(out.found && out.record.lifecycle==Life::published_uncommitted) {
      Journal rows(c,2);const auto* cohort=rows.Find(key);
      if(!cohort)Conflict();MatchRows(out.record,*cohort);
    }
    journal.FinishRecovery();return out;});
}
RowResult StoreMgaBulkImportImportedRowEventsV1(const EngineRequestContext& c,const std::vector<Event>& requested) {
  return Run<RowResult>([&] {
    Context(c);auto entry=Owned(c);entry.events=requested;Cohort(entry.events);
    for(auto& row:entry.events){BodyOwner(c,row);if(binary::Encode(row).empty())Invalid();}
    const auto key=entry.events.front().recovery_idempotency_key;
    Journal publications(c,1);const auto* publication=publications.Find(key);
    if(!publication || !SameOwner(*publication,entry) || publication->publication.lifecycle==Life::aborted)Conflict();
    MatchRows(publication->publication,entry);
    Journal journal(c,2);
    if(const auto* existing=journal.Find(key)) {
      if(!SameOwner(*existing,entry) || existing->events!=entry.events)Conflict();
      auto out=Success<RowResult>();out.events=existing->events;out.replayed=true;return out;
    }
    auto out=Success<RowResult>();out.events=entry.events;
    journal.AppendFrame(std::move(entry));return out;
  });
}
RowResult RecoverMgaBulkImportImportedRowEventsV1(const EngineRequestContext& c,const Sha& key) {
  return Run<RowResult>([&] {
    Context(c);if(!Nonzero(key))Invalid();Journal journal(c,2,&key);
    auto out=Success<RowResult>();
    const auto* existing=journal.Find(key);
    if(const auto* evidence=existing?existing:journal.PendingEntry()) {
      Journal publications(c,1);const auto* publication=publications.Find(key);
      if(!publication || !SameOwner(*publication,*evidence))Conflict();
      MatchRows(publication->publication,*evidence);
      if(existing)out.events=existing->events;
    }
    journal.FinishRecovery();return out;
  });
}
} // namespace scratchbird::engine::internal_api
