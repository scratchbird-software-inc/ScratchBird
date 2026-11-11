// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
//
// The runtime authority binding readers (management/runtime_authority_binding.hpp)
// on a database written by the production create primitive (fixed roots) and
// changed in place as the engine changes it: the transaction inventory writer
// (W1) and the catalog version writer (W3a). W4d (2026-11-08) moved this gate
// off its hand-forged checkpoint fixture, which was deleted with that layer.
//   - Governed: the read on the caller's retained fixed-root lease copies the
//     exact committed definition; every request refusal exposes nothing; no
//     heap allocation under the device guards; every physical read is failed
//     in turn and typed with its extent and filespace; the read recovers and
//     writes nothing.
//   - Committed: the read through page zero, the same definition and
//     refusals; invalid requests refused before any read; every read and
//     allocation fault refused with nothing usable.
//   - Pinned: the read under a real published snapshot of a real reader
//     transaction; every read and allocation fault; a revoked pin refused.
//   - Visibility: a successor written in place by transaction 2 is invisible
//     while it is active (and refused to its own reader as not committed),
//     invisible after ROLLBACK, and the committed definition after COMMIT.
//   - FORGED (no writer produces them): a definition naming a foreign
//     database, refused; the construction transaction's state word set back
//     to active, so the catalog itself has no committed creator, refused.
//   - Cold: a fresh executable (fork + exec) reads the persisted binding.
// Not covered here any more: the owned-source read (its API was deleted with
// the selected-checkpoint source; hosted reads of the binding run in the
// principal credential test).
#define SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
#include "../sbsql_sblr_alignment/native_runtime_security_observation_test.cpp"
#undef SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
#include "management/runtime_authority_binding.hpp"
#include "transaction_inventory_reader.hpp"
#include <sys/wait.h>
#include <unistd.h>

namespace {
namespace runtime_api=scratchbird::engine::internal_api;
using BindingError=runtime_api::RuntimeAuthorityBindingReadError;
template<class T> concept HasSnapshotIdentity=requires(T value){value.snapshot_uuid;};
static_assert(!HasSnapshotIdentity<runtime_api::RuntimeAuthorityBindingCommittedReadResult>);
static_assert(!HasSnapshotIdentity<runtime_api::RuntimeAuthorityBindingLeaseReadResult>);

// The fixture's binding (SecurityFixture): Id(180) for database Id(1), service
// principal Id(200), provider Id(210), policy Id(52), credential reference
// Id(211), generation 1, policy epoch 31, provider generation 37. It lives in
// the configuration relation (catalog selector 2, role 5).
db::NativeCatalogRelationBinding Relation(const SecurityFixture& f) {
  return {f.receipt.relation_uuids[4],f.receipt.index_dependencies[4]};
}
std::string FixturePath(const SecurityFixture& f){return (f.artifacts.path()/"native-security").string();}
Uuid BaseVersion(SecurityFixture& f) {
  const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(f.device,Id(1),f.receipt.relations[4]);
  Check(loaded.ok(),"the configuration leaf the create wrote");
  for(const auto& row:loaded.page->body.rows)
    if(loaded.metadata.at(row.version_uuid).record.header.object_uuid.value==Id(180))return row.version_uuid;
  Check(false,"the create wrote the runtime binding");return {};
}
template<class Result> void Exact(const Result& r,const Uuid& version,u64 generation,const char* why) {
  if(!r.ok())std::cerr<<"binding error="<<int(r.error)<<" source="<<int(r.source_error)<<'\n';
  Check(r.ok()&&r.binding->binding_uuid==Id(180)&&r.binding->database_uuid==Id(1)&&
      r.binding->service_principal_uuid==Id(200)&&r.binding->credential_reference_uuid==Id(211)&&
      r.binding->provider_uuid==Id(210)&&r.binding->policy_uuid==Id(52)&&r.binding->generation==generation&&
      r.binding->policy_epoch==31&&r.binding->provider_generation==37&&r.native_version_uuid==version,why);
}
template<class Result> void Refused(const Result& r,BindingError error,const char* why) {
  if(r.error!=error)std::cerr<<"binding error="<<int(r.error)<<" expected="<<int(error)<<'\n';
  Check(r.error==error&&!r.binding&&r.native_version_uuid.is_nil(),why);
}
std::string Hex(const Uuid& id){static constexpr char digits[]="0123456789abcdef";std::string s;
  for(const auto b:id.bytes){s+=digits[b>>4];s+=digits[b&15];}return s;}
Uuid FromHex(std::string_view s){Uuid id;if(s.size()!=32)throw std::runtime_error("hex identity");
  for(unsigned i=0;i<16;++i)id.bytes[i]=static_cast<byte>(std::stoul(std::string(s.substr(2*i,2)),nullptr,16));return id;}

// The device list is built by the caller, once: an allocation fault injected
// around a read must land in the reader, not in the test's own setup.
using Devices=std::vector<disk::NativeFilespaceDevice>;
Devices DevicesOf(disk::FileDevice& device,unsigned profile){return {{Id(2),Profile(profile),&device}};}
runtime_api::RuntimeAuthorityBindingCommittedReadResult ReadCommitted(const Devices& devices,
    const db::NativeCatalogRelationBinding& relation,u64 generation,Uuid database,Uuid binding,u64 budget) {
  return runtime_api::ReadCommittedRuntimeAuthorityBindingFromOpenDevices(database,devices,
      db::NativeCatalogSource::PageZero(Id(2)),2,5,relation,binding,generation,budget);
}

// ---- Governed: the caller's retained fixed-root lease --------------------------------
void Governed(unsigned profile) {
  SecurityFixture f(profile);
  const auto base=BaseVersion(f);
  const u64 budget=f.budget;
  const db::NativeCatalogRelationMemoryLimits limits{Profile(profile),1,1,4,4,{1,sizes[profile]}};
  LeafMemoryFixture catalog_memory(db::NativeCatalogVisibilityWorkspaceBytes(limits));
  LeafMemoryFixture source_memory(32*1024*1024);
  auto prepared=db::PrepareNativeCatalogVisibilityRead(limits,catalog_memory.memory,catalog_memory.binding);
  Check(prepared.ok(),"prepare the governed configuration backing before the source guards");
  const std::array<disk::NativeFilespaceDevice,1> devices{{{Id(2),Profile(profile),&f.device}}};
  auto source=db::AcquireNativeFixedRootMemoryLease(Id(1),devices,Id(2),{budget,32*1024*1024},
      source_memory.memory,source_memory.binding);
  Check(source.ok(),"retain the real fixed-root source for the governed binding read");
  const auto writes_before=stage_writes,syncs_before=stage_syncs;
  const auto relation=Relation(f);
  const auto read=[&](Uuid database=Id(1),Uuid binding=Id(180),u64 generation=1,u64 ceiling=0) {
    allocation_budget=0;
    auto result=runtime_api::ReadCommittedRuntimeAuthorityBindingOnNativeLease(
        *source.lease,database,2,5,relation,binding,generation,ceiling?ceiling:budget,prepared.workspace);
    const auto remaining=allocation_budget;allocation_budget=-1;
    if(remaining!=0)std::cerr<<"governed forbidden allocation profile="<<profile<<" reads="<<reads
        <<" fault="<<read_fault<<" result="<<int(result.error)<<" source="<<int(result.source_error)
        <<" relation="<<int(result.relation_error)<<'\n';
    Check(remaining==0,"governed runtime binding performs no heap allocation under native device guards");
    return result;
  };
  reads=0;track_reads=true;const auto valid=read();track_reads=false;const auto read_count=reads;
  Exact(valid,base,1,"governed reader copies the exact committed binary definition without a fabricated snapshot");
  Check(valid.physical_bytes_read>0&&read_count>0,"the governed read reads the file");
  Refused(read(Id(1),Id(180),2),BindingError::generation_mismatch,"another generation is refused");
  Refused(read(Id(1),Id(250)),BindingError::absent,"an unknown binding is absent");
  Refused(read(Id(250)),BindingError::invalid_request,"a foreign database is refused");
  Refused(read({},Id(180)),BindingError::invalid_request,"a nil database is refused");
  Refused(read(Id(1),{}),BindingError::invalid_request,"a nil binding is refused");
  Refused(read(Id(1),Id(180),0),BindingError::invalid_request,"generation zero is refused");
  Refused(read(Id(1),Id(180),1,1),BindingError::source_failure,"a one-byte image ceiling is refused");
  auto moved=std::move(prepared.workspace);
  const auto missing=read();Refused(missing,BindingError::source_failure,"a missing workspace is refused");
  Check(!missing.physical_bytes_read,"missing pre-admitted backing refuses before configuration I/O");
  prepared.workspace=std::move(moved);
  Exact(read(),base,1,"governed binding succeeds again after workspace restoration");
  for(unsigned fault=1;fault<=read_count;++fault) {
    reads=0;read_fault=fault;track_reads=true;const auto failed=read();track_reads=false;
    Check(read_fault==0,"governed binding exact physical read failure injected");
    Refused(failed,BindingError::source_failure,"a failed read is refused");
    Check(failed.io_failure&&failed.io_failure->error==disk::BoundedIoError::native_failure&&
        failed.io_failure->operation==disk::BoundedIoOperation::read&&
        failed.io_failure->native_attempted&&failed.io_failure->native_error==EIO&&
        failed.io_failure->requested_bytes>0&&failed.io_failure->bytes_transferred==0&&
        failed.io_failure_filespace_uuid==Id(2),
        "governed read refusal preserves the fixed native I/O error and exact requested extent");
  }
  Exact(read(),base,1,"governed binding recovers its scratch after each read refusal");
  Check(valid.binding->binding_uuid==Id(180)&&valid.native_version_uuid==base,
        "workspace reuse does not invalidate the copied fixed-size authority tuple");
  Check(stage_writes==writes_before&&stage_syncs==syncs_before,"governed configuration read is physically read-only");
  source.lease.Reset();prepared.workspace={};
  source_memory.memory={};catalog_memory.memory={};source_memory.Empty();catalog_memory.Empty();
}

// ---- Committed: through page zero ----------------------------------------------------
void Committed(unsigned profile) {
  SecurityFixture f(profile);
  const auto base=BaseVersion(f);const auto relation=Relation(f);const auto devices=DevicesOf(f.device,profile);
  const auto read_committed=[&](u64 generation=1,Uuid binding=Id(180),u64 budget=0){
    return ReadCommitted(devices,relation,generation,Id(1),binding,budget?budget:f.budget);};
  stage_writes=stage_syncs=0;
  Exact(read_committed(),base,1,"committed source preserves the exact binary definition and version");
  Refused(read_committed(2),BindingError::generation_mismatch,"committed read refuses another generation");
  Refused(read_committed(1,Id(250)),BindingError::absent,"committed read finds no unknown binding");
  Check(!stage_writes&&!stage_syncs,"committed configuration observation performs no writes or syncs");
  if(profile!=0)return;
  for(unsigned invalid=0;invalid<5;++invalid) {
    auto database=Id(1),binding=Id(180);u64 generation=1,budget=f.budget;
    if(invalid==0)database={};if(invalid==1)binding={};if(invalid==2)generation=0;
    if(invalid==3)budget=0;if(invalid==4)binding.bytes[6]=0x40;
    reads=0;track_reads=true;
    const auto bad=ReadCommitted(devices,relation,generation,database,binding,budget);
    track_reads=false;Refused(bad,BindingError::invalid_request,"an invalid committed request is refused");
    Check(!reads,"invalid committed request performs no native reads");
  }
  Refused(read_committed(1,Id(180),1),BindingError::source_failure,"a one-byte budget is refused");
  reads=0;track_reads=true;auto committed=read_committed();track_reads=false;const auto committed_reads=reads;
  Exact(committed,base,1,"committed source reads the binding");Check(committed_reads>0,"committed source actually reads files");
  for(unsigned fault=1;fault<=committed_reads;++fault) {
    reads=0;read_fault=fault;track_reads=true;committed=read_committed();track_reads=false;
    Check(read_fault==0,"committed source read fault consumed");
    Refused(committed,BindingError::source_failure,"a committed read fault is refused");
  }
  for(long fault:{0l,1l,5l,20l,50l}) {
    const auto loss=f.device.failed_io_latency_observations();
    allocation_budget=fault;committed=read_committed();const bool consumed=allocation_budget==-1;allocation_budget=-1;
    Check(consumed,"committed source allocation fault consumed");
    if(committed.ok()) {
      Check(f.device.failed_io_latency_observations()==loss+1,"only recorded telemetry loss preserves committed source success");
      Exact(committed,base,1,"a committed read that survives an allocation fault is exact");
    }else {
      Check(!committed.binding&&committed.native_version_uuid.is_nil()&&
          (committed.error==BindingError::source_failure||committed.error==BindingError::resource_exhausted),
          "committed allocation failure retains no usable result");
    }
  }
  Exact(read_committed(),base,1,"committed source recovers after the faults");
}

// ---- Pinned: a real reader's published snapshot --------------------------------------
struct Reader {
  mga::LocalTransactionInventory inventory;
  mga::TransactionIdentity identity;
  u64 order=0;
};
// The local inventory as persisted, read through the sink without a writer
// (a writer's one-shot restart would mark live transactions dead).
mga::LocalTransactionInventory InventoryOf(disk::CarefulWriteSink& sink,const SecurityFixture::FixedRoots& roots) {
  const auto chains=page::ReadTransactionChainsThroughSink(sink,SecurityFixture::TxBinding(roots));
  Check(chains.ok(),"read the transaction inventory back");
  auto built=page::BuildLocalTransactionInventoryFromImages(chains.page_size,chains.state_images,chains.header_images);
  Check(built.ok(),"the local inventory built from the persisted chains");
  return std::move(built.inventory);
}
Reader ReaderOf(mga::LocalTransactionInventory inventory,u64 order) {
  Reader r;r.inventory=std::move(inventory);r.order=order;
  const auto found=std::find_if(r.inventory.entries.begin(),r.inventory.entries.end(),
      [&](const auto& e){return e.identity.local_id.value==order;});
  Check(found!=r.inventory.entries.end()&&found->state==mga::TransactionState::active,"the reader is active in the inventory");
  r.identity=found->identity;return r;
}
Reader BeginReader(SecurityFixture& f,const Uuid& transaction) {
  const auto roots=SecurityFixture::RootsOf(f.device);
  disk::FileDeviceCarefulWriteSink sink(f.device);
  page::TransactionInventoryWriter w(SecurityFixture::TxBinding(roots));Check(w.Restart(sink).ok(),"restart before the reader BEGIN");
  page::TransactionHeaderRecord header;header.transaction_uuid=transaction;
  const auto begun=w.Begin(sink,header);Check(begun.ok(),"the reader transaction begins in place");
  return ReaderOf(InventoryOf(sink,roots),*begun.transaction_order);
}
// One writer (one per database): transaction 2 writes the binding's successor
// (generation 2, version Id(13)) in place and stays active; transaction 3
// then begins, as a reader, and stays active too.
struct ActiveSuccessor {Reader creator,later;};
ActiveSuccessor ActiveSuccessorAndReader(SecurityFixture& f) {
  const auto roots=SecurityFixture::RootsOf(f.device);
  disk::FileDeviceCarefulWriteSink sink(f.device);
  page::TransactionInventoryWriter w(SecurityFixture::TxBinding(roots));Check(w.Restart(sink).ok(),"restart before transaction 2");
  page::TransactionHeaderRecord h2;h2.transaction_uuid=Id(12);
  const auto begun=w.Begin(sink,h2);Check(begun.ok()&&begun.transaction_order==2u,"transaction 2 begins in place");
  const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(f.device,Id(1),f.receipt.relations[4]);
  Check(loaded.ok(),"the configuration leaf before the successor");
  const auto original=std::find_if(loaded.page->body.rows.begin(),loaded.page->body.rows.end(),[&](const auto& row){
    return loaded.metadata.at(row.version_uuid).record.header.object_uuid.value==Id(180);});
  Check(original!=loaded.page->body.rows.end(),"the persisted binding row");
  auto metadata=loaded.metadata.at(original->version_uuid);
  metadata.creator_transaction_uuid={platform::UuidKind::transaction,Id(12)};metadata.creator_local_transaction_id=2;
  metadata.definition_version=2;
  auto binding=catalog::DecodeCatalogRuntimeAuthorityBinding(metadata.record.payload);Check(binding.ok(),"decode the binding");
  binding.record->generation=2;
  const auto payload=catalog::EncodeCatalogRuntimeAuthorityBinding(*binding.record);Check(payload.ok(),"the successor's payload");
  metadata.record.payload.assign(payload.bytes.begin(),payload.bytes.end());
  // W3b-2b: transaction 2's catalog.metadata_change event, which the successor cites.
  Check(scratchbird::tests::catalog_change_event::WriteEvent(sink,SecurityFixture::CvBinding(roots),2,Id(12),security_fixture::Id(9791),security_fixture::Id(9792),
      security_fixture::Id(9793),security_fixture::Id(9790),1789357072001000ULL).ok(),"transaction 2's catalog change event");
  metadata.audit_uuid={platform::UuidKind::object,security_fixture::Id(9791)};
  db::CatalogVersionRequest r;r.role=5;r.transaction_order=2;r.transaction_uuid=Id(12);r.row_uuid=original->row_uuid.value;
  r.predecessor_version_uuid=original->version_uuid;r.version_uuid=Id(13);r.metadata=metadata;
  const auto written=db::WriteCatalogSuccessorVersion(sink,SecurityFixture::CvBinding(roots),r);
  if(!written.ok())std::cerr<<"successor="<<db::CatalogVersionWriteErrorName(written.error)<<'\n';
  Check(written.ok(),"the binding successor written in place by transaction 2");
  page::TransactionHeaderRecord h3;h3.transaction_uuid=Id(21);
  const auto later=w.Begin(sink,h3);Check(later.ok()&&later.transaction_order==3u,"transaction 3 begins after it");
  auto inventory=InventoryOf(sink,roots);
  return {ReaderOf(inventory,2),ReaderOf(std::move(inventory),3)};
}
runtime_api::RuntimeAuthorityBindingReadResult ReadPinned(SecurityFixture& f,const Devices& devices,const Reader& reader,
    const mga::PublishedSnapshotPin& pin,u64 generation=1,Uuid binding=Id(180),u64 budget=0) {
  return runtime_api::ReadRuntimeAuthorityBindingFromOpenDevices(Id(1),devices,
      db::NativeCatalogSource::PageZero(Id(2)),2,5,Relation(f),reader.identity,pin,binding,generation,
      budget?budget:f.budget);
}
void Pinned(unsigned profile) {
  SecurityFixture f(profile);
  const auto devices=DevicesOf(f.device,profile);
  const auto base=BaseVersion(f);
  const auto reader=BeginReader(f,Id(20));
  const auto verify=[&](const auto& result,const char* why){
    Exact(result,base,1,why);Check(!result.snapshot_uuid.is_nil(),"the pinned read names its snapshot");};
  const auto empty=[&](const auto& result,BindingError error,const char* why){
    Refused(result,error,why);Check(result.snapshot_uuid.is_nil(),"a refusal names no snapshot");};
  {
    CatalogTestPin pin(reader.inventory,reader.order);
    verify(ReadPinned(f,devices,reader,pin.pin),"the actual committed binary binding and exact native version selected");
    empty(ReadPinned(f,devices,reader,pin.pin,2),BindingError::generation_mismatch,"pinned read refuses another generation");
    empty(ReadPinned(f,devices,reader,pin.pin,1,Id(250)),BindingError::absent,"pinned read finds no unknown binding");
    if(profile==0) {
      empty(ReadPinned(f,devices,reader,pin.pin,1,Id(180),1),BindingError::source_failure,"a one-byte budget is refused");
      reads=0;track_reads=true;auto result=ReadPinned(f,devices,reader,pin.pin);track_reads=false;const auto read_count=reads;
      verify(result,"measure the pinned read");Check(read_count>0,"measure actual source reads");
      for(unsigned fault=1;fault<=read_count;++fault) {
        reads=0;read_fault=fault;track_reads=true;result=ReadPinned(f,devices,reader,pin.pin);track_reads=false;
        Check(read_fault==0,"native read fault consumed");empty(result,BindingError::source_failure,"a pinned read fault is refused");
      }
      for(long fault:{0l,1l,5l,20l,50l}) {
        const auto failed_metrics=f.device.failed_io_latency_observations();
        allocation_budget=fault;result=ReadPinned(f,devices,reader,pin.pin);const bool consumed=allocation_budget==-1;allocation_budget=-1;
        Check(consumed,"pinned allocation fault consumed");
        if(result.ok()) {
          // FileDevice intentionally contains a latency-recorder exception.
          // Only this independently observed failure can preserve a good read.
          Check(f.device.failed_io_latency_observations()==failed_metrics+1,
              "successful read after allocation fault requires actual isolated metric failure");
          verify(result,"a pinned read that survives an allocation fault is exact");continue;
        }
        Check(!result.binding&&result.native_version_uuid.is_nil()&&result.snapshot_uuid.is_nil(),
            "allocation failure retains no usable binding");
        Check(result.error==BindingError::source_failure||result.error==BindingError::resource_exhausted,
            "typed allocation/source refusal");
      }
      verify(ReadPinned(f,devices,reader,pin.pin),"the pinned read recovers after the faults");
    }
  }
  CatalogTestPin revoked(reader.inventory,reader.order);
  mga::RevokePublishedSnapshotVector(revoked.published.descriptor.snapshot_uuid);
  empty(ReadPinned(f,devices,reader,revoked.pin),BindingError::source_failure,"a revoked snapshot pin is refused");
}

// ---- Visibility of a successor written in place by transaction 2 ---------------------
void Visibility(unsigned profile) {
  {  // 1: transaction 2 active (and transaction 3, a later reader, active).
    SecurityFixture f(profile,0,false,true);  // W3b-2b: audited, it writes
    const auto devices=DevicesOf(f.device,profile);
    const auto base=BaseVersion(f);
    const auto active=ActiveSuccessorAndReader(f);
    const auto relation=Relation(f);
    Exact(ReadCommitted(devices,relation,1,Id(1),Id(180),f.budget),base,1,
        "an active transaction's successor is invisible to the committed read");
    Refused(ReadCommitted(devices,relation,2,Id(1),Id(180),f.budget),BindingError::generation_mismatch,
        "the active successor's generation is not the committed one");
    {
      CatalogTestPin pin(active.later.inventory,active.later.order);
      Exact(ReadPinned(f,devices,active.later,pin.pin),base,1,"another reader's snapshot excludes the active successor");
    }
    CatalogTestPin pin(active.creator.inventory,active.creator.order);
    Refused(ReadPinned(f,devices,active.creator,pin.pin,2),BindingError::not_committed_active,
        "the creator's own uncommitted successor is not a committed active binding");
  }
  {  // 3: rolled back.
    SecurityFixture f(profile,0,false,true);  // W3b-2b: audited, it writes
    const auto devices=DevicesOf(f.device,profile);
    const auto base=BaseVersion(f);
    f.RetainedSuccessor(3,true);
    Exact(ReadCommitted(devices,Relation(f),1,Id(1),Id(180),f.budget),base,1,
        "a rolled-back successor is invisible: the committed definition stands");
  }
  {  // 4: committed.
    SecurityFixture f(profile,0,false,true);  // W3b-2b: audited, it writes
    const auto devices=DevicesOf(f.device,profile);
    f.RetainedSuccessor(4,true);
    Exact(ReadCommitted(devices,Relation(f),2,Id(1),Id(180),f.budget),Id(13),2,
        "a committed successor is the committed definition, at its generation");
    Refused(ReadCommitted(devices,Relation(f),1,Id(1),Id(180),f.budget),BindingError::generation_mismatch,
        "the superseded generation is no longer the committed one");
  }
}

// ---- FORGED states no writer produces -----------------------------------------------
// FORGED (production leaf codec, independent byte oracle): the binding's
// definition names a foreign database. The catalog version writer refuses such
// a successor, so only a forged leaf can hold it; every reader must refuse it.
void ForeignDefinition(unsigned profile) {
  SecurityFixture f(profile);
  const auto devices=DevicesOf(f.device,profile);
  const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(f.device,Id(1),f.receipt.relations[4]);
  Check(loaded.ok(),"the configuration leaf before the foreign-definition forgery");
  auto leaf=*loaded.page;bool found=false;
  for(auto& row:leaf.body.rows){auto metadata=loaded.metadata.at(row.version_uuid);
    if(metadata.record.header.object_uuid.value!=Id(180))continue;
    auto binding=catalog::DecodeCatalogRuntimeAuthorityBinding(metadata.record.payload);Check(binding.ok(),"decode the persisted binding");
    binding.record->database_uuid=binding.record->security_authority_uuid=Id(250);
    const auto foreign=catalog::EncodeCatalogRuntimeAuthorityBinding(*binding.record);Check(foreign.ok(),"a well-formed foreign definition");
    metadata.record.payload.assign(foreign.bytes.begin(),foreign.bytes.end());
    const auto outer=catalog::EncodeCatalogMetadataVersion(metadata);Check(outer.ok(),"well-formed foreign metadata");
    row.cells.front().value.payload=outer.bytes;found=true;
  }
  Check(found,"the persisted binding row was forged");
  const auto canonical=db::EncodeNativeCatalogLeaf(leaf);Check(canonical.ok(),"the forged leaf framing");
  const auto bytes=LeafOracle(leaf);Check(bytes==canonical.bytes,"the forged leaf's independent byte oracle");
  const auto io=f.device.WriteAt(leaf.header.page_number*u64{sizes[profile]},bytes.data(),bytes.size());
  Check(io.ok()&&io.bytes_transferred==bytes.size()&&f.device.Sync().ok(),"the forged leaf persists");
  Refused(ReadCommitted(devices,Relation(f),1,Id(1),Id(180),f.budget),BindingError::invalid_definition,
      "a committed definition naming a foreign database is refused");
  const auto reader=BeginReader(f,Id(20));
  CatalogTestPin pin(reader.inventory,reader.order);
  Refused(ReadPinned(f,devices,reader,pin.pin),BindingError::invalid_definition,"the pinned read refuses it too");
}
// FORGED (production state-word codec): the construction transaction's state
// word set back to active. Its catalog then has no committed creator at all;
// the committed read must refuse the source before selecting any row.
void UncommittedCatalog(unsigned profile) {
  SecurityFixture f(profile);
  const auto devices=DevicesOf(f.device,profile);
  SecurityFixture::ForgeStateWord(f.device,profile,SecurityFixture::RootsOf(f.device),1,page::TipStateCode::active);
  const auto read=ReadCommitted(devices,Relation(f),1,Id(1),Id(180),f.budget);
  Refused(read,BindingError::source_failure,"a catalog with no committed creator is refused");
  Check(read.source_error==db::NativeCommittedCatalogReadError::source_failure,
      "the refusal is the committed source's, before row selection");
}

// ---- Cold: a fresh executable reads the persisted binding ----------------------------
void Cold(unsigned profile) {
  SecurityFixture f(profile);
  const auto base=BaseVersion(f);
  const auto relation=Relation(f);
  Check(!relation.index_dependencies,"the configuration relation is a direct leaf");
  const auto path=FixturePath(f);
  // A new process must read the persisted catalog and inventory; it cannot use
  // a retained source result or a process-local configuration registry.
  Check(f.device.Close().ok(),"close before the independent reopen");
  const auto child=fork();Check(child>=0,"spawn the cold binding reader");
  const auto argument=std::to_string(profile),relation_hex=Hex(relation.relation_uuid),version_hex=Hex(base);
  if(child==0){execl("/proc/self/exe","runtime-binding-native","--read-binding",path.c_str(),argument.c_str(),
      relation_hex.c_str(),version_hex.c_str(),nullptr);_exit(125);}
  int status=0;
  Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
      "a fresh executable reads the actual committed binding");
  Check(f.device.Open(path,disk::FileOpenMode::open_existing).ok(),"reopen after the cold reader");
}
int ColdRead(const char* path,const char* profile_text,const char* relation_hex,const char* version_hex) {
  const auto profile=static_cast<unsigned>(std::stoul(profile_text));if(profile>=5)return 2;
  disk::FileDevice device;if(!device.Open(path,disk::FileOpenMode::open_existing_read_only).ok())return 3;
  const db::NativeCatalogRelationBinding relation{FromHex(relation_hex),{}};
  const auto devices=DevicesOf(device,profile);
  const auto read=ReadCommitted(devices,relation,1,Id(1),Id(180),512*u64{sizes[profile]});
  Exact(read,FromHex(version_hex),1,"the cold reader's committed binding");
  return device.Close().ok()?0:4;
}
} // namespace

int main(int argc,char** argv) {
  try {
    if(argc==6&&std::string_view(argv[1])=="--read-binding")return ColdRead(argv[2],argv[3],argv[4],argv[5]);
    if(argc!=1){std::cerr<<"FAIL unknown mode\n";return 2;}
    for(unsigned profile=0;profile<5;++profile){
      Governed(profile);Committed(profile);Pinned(profile);Visibility(profile);
      ForeignDefinition(profile);UncommittedCatalog(profile);Cold(profile);
    }
    std::cout<<"native runtime binding checks="<<checks<<" failures=0\n";return 0;
  }catch(const std::exception& error){allocation_budget=-1;std::cerr<<"FAIL "<<error.what()<<" checks="<<checks<<'\n';return 1;}
}
