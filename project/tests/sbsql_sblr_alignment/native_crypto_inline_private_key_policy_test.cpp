// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "native_crypto_inline_private_key_policy_lookup.hpp"
#include "native_creation_workspace.hpp"
#include "native_creation_catalog_input.hpp"
#include "native_catalog_definition_generation.hpp"
#include "bootstrap_policy_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>

thread_local long allocation_budget=-1;
thread_local unsigned allocations=0;
void* operator new(std::size_t n){++allocations;if(allocation_budget>=0&&allocation_budget--==0)throw std::bad_alloc();
  if(auto* p=std::malloc(n?n:1))return p;throw std::bad_alloc();}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete(void* p)noexcept{std::free(p);}void operator delete[](void* p)noexcept{std::free(p);}
void operator delete(void* p,std::size_t)noexcept{std::free(p);}void operator delete[](void* p,std::size_t)noexcept{std::free(p);}

namespace c=scratchbird::core::catalog;namespace p=scratchbird::core::platform;
namespace f=scratchbird::tests::bootstrap_policy;namespace db=scratchbird::storage::database;
namespace d=scratchbird::storage::disk;namespace u=scratchbird::core::uuid;
namespace mga=scratchbird::transaction::mga;
namespace {
unsigned checks=0,reads=0,read_fault=0;bool track_reads=false;
void Check(bool ok,const char* why){++checks;if(!ok){allocation_budget=-1;throw std::runtime_error(why);}}
const p::TypedUuid origin{p::UuidKind::transaction,f::Id(5)};
c::CatalogCryptoInlinePrivateKeyPolicy Body(bool allow){
  c::CatalogCryptoInlinePrivateKeyPolicy r;r.body_uuid=f::Id(100);r.database_uuid=f::Id(1);r.generation=1;
  r.origin_transaction_uuid=origin;r.origin_local_transaction_id=1;r.inline_private_key_use_allowed=allow;return r;
}
std::string Golden(const c::CatalogCryptoInlinePrivateKeyPolicy& r){
  f::Golden g(65620);g.Identity(1,r.body_uuid);g.Integer(2,r.generation);g.Identity(3,r.database_uuid);
  g.Integer(4,static_cast<p::u64>(r.purpose));g.Boolean(5,r.inline_private_key_use_allowed);
  g.Identity(6,r.origin_transaction_uuid.value);g.Integer(7,r.origin_local_transaction_id);return g.Finish();
}
c::CatalogPolicyDescriptor Descriptor(const c::CatalogCryptoInlinePrivateKeyPolicy& body){
  c::CatalogPolicyDescriptor r;r.policy_uuid=f::Id(200);r.policy_version_uuid=f::Id(201);r.database_uuid=body.database_uuid;
  r.body_uuid=body.body_uuid;r.body_generation=body.generation;r.generation=1;r.policy_class=c::CatalogPolicyClass::authorization;
  r.creating_transaction_number=r.origin_local_transaction_id=1;r.creating_transaction_uuid=r.origin_transaction_uuid=origin;
  r.scope=c::CatalogPolicyScope::database;r.scope_uuid=r.database_uuid;r.authority_owner_uuid=f::Id(50);
  r.precedence_rank=1;r.enforcement_points=c::kCatalogPolicyEnforcementMask;return r;
}
c::CatalogMetadataVersion Metadata(const p::Uuid& object,unsigned row,std::string subtype,std::string payload){
  auto r=f::Metadata(0,origin,f::Id(60),std::move(payload));r.record.header.kind=c::CatalogRecordKind::policy;
  r.record.header.object_uuid={p::UuidKind::object,object};r.record.header.row_uuid={p::UuidKind::row,f::Id(row)};
  r.object_subtype=std::move(subtype);r.default_name_uuid={p::UuidKind::object,f::Id(row+1000)};
  r.name_vector_uuid={p::UuidKind::object,f::Id(row+2000)};return r;
}
auto BodyMetadata(const c::CatalogCryptoInlinePrivateKeyPolicy& b){return Metadata(b.body_uuid,101,"crypto_inline_private_key",Golden(b));}
auto DescriptorMetadata(const c::CatalogPolicyDescriptor& v){
  std::array<p::byte,c::kCatalogPolicyDescriptorMaxBytes> buffer;
  const auto encoded=c::EncodeCatalogPolicyDescriptorInto(v,buffer);Check(encoded.ok(),"encode actual exact descriptor");
  return Metadata(v.policy_uuid,202,"policy_descriptor",std::string(reinterpret_cast<const char*>(encoded.bytes.data()),encoded.bytes.size()));
}
void Empty(const auto& result){
  if(result.ok()||result.descriptor||result.body)std::cerr<<"unexpected observation at check="<<checks<<" error="<<int(result.error)
      <<" allocation_budget="<<allocation_budget<<" read_fault="<<read_fault<<" reads="<<reads<<'\n';
  Check(!result.ok()&&!result.descriptor&&!result.body,"no typed permission observation after failed read");
}
void Generations(){
  auto body=Body(false);auto before=BodyMetadata(body);auto after=before;++after.security_epoch;
  Check(db::NativeCatalogDefinitionGenerationFollows(before,after),"unchanged exact policy definition admits a common-metadata successor");
  body.inline_private_key_use_allowed=true;after.record.payload=Golden(body);
  Check(!db::NativeCatalogDefinitionGenerationFollows(before,after),"same generation cannot change a denied permission into allow");
  body.generation=2;after.definition_version=2;after.record.payload=Golden(body);
  Check(db::NativeCatalogDefinitionGenerationFollows(before,after)&&c::CatalogMetadataPreservesFamilyOrigin(before,after),"next semantic definition may explicitly change permission");
  after.definition_version=3;Check(!db::NativeCatalogDefinitionGenerationFollows(before,after),"ordinary catalog sequencing does not skip semantic generations");
  body.generation=UINT64_MAX;before=BodyMetadata(body);before.definition_version=body.generation;after=before;
  Check(db::NativeCatalogDefinitionGenerationFollows(before,after),"identical metadata-only policy successor does not overflow maximum semantic generation");
  after.definition_version=0;Check(!db::NativeCatalogDefinitionGenerationFollows(before,after),"semantic generation never wraps");
  before.object_subtype="unrelated";before.definition_version=1;after=before;
  Check(!db::NativeCatalogDefinitionGenerationFollows(before,after),"same-generation exception never relaxes unrelated family rules");
  auto descriptor=Descriptor(Body(false));before=DescriptorMetadata(descriptor);after=before;++after.security_epoch;
  Check(db::NativeCatalogDefinitionGenerationFollows(before,after),"descriptor metadata successor retains exact semantic definition");
  descriptor.default_decision=c::CatalogPolicyDefault::allow;after.record.payload=DescriptorMetadata(descriptor).record.payload;
  Check(!db::NativeCatalogDefinitionGenerationFollows(before,after),"descriptor same-generation exception cannot change its default decision");
}
// Isolated resolver history vectors, deliberately NOT persistence, native
// source admission or policy-activation evidence. The separate Physical path
// below exercises actual files and the full common reader without alteration.
void SelectionHistory(const db::NativeCryptoInlinePrivateKeyPolicyLookupResult& actual,
    const db::NativeCryptoInlinePrivateKeyPolicyLookupBinding& binding){
  auto source=actual.source;
  const auto original=std::find_if(source.source.row_creators.begin(),source.source.row_creators.end(),[&](const auto& loc){
    const auto& leaf=source.source.relation.catalogs[loc.catalog_page_index];
    const auto& row=leaf.page->body.rows[loc.catalog_row_index];return leaf.metadata.at(row.version_uuid).record.header.object_uuid.value==actual.body->body_uuid;
  });
  Check(original!=source.source.row_creators.end(),"history vector starts at actual retained body");
  auto loc=*original;auto& leaf=source.source.relation.catalogs[loc.catalog_page_index];
  auto successor=leaf.page->body.rows[loc.catalog_row_index];const auto predecessor=successor.version_uuid;
  successor.version_uuid=f::Id(701);successor.previous_version_uuid=predecessor;successor.previous_row_version=successor.row_version;++successor.row_version;
  leaf.page->body.rows[loc.catalog_row_index].next_version_uuid=successor.version_uuid;
  leaf.page->body.rows[loc.catalog_row_index].next_row_version=successor.row_version;
  auto metadata=leaf.metadata.at(predecessor);++metadata.security_epoch;
  leaf.metadata.emplace(successor.version_uuid,metadata);loc.catalog_row_index=leaf.page->body.rows.size();
  leaf.page->body.rows.push_back(successor);source.source.row_creators.push_back(loc);
  auto current=std::find_if(source.rows.begin(),source.rows.end(),[&](const auto& row){return row.metadata.record.header.object_uuid.value==actual.body->body_uuid;});
  Check(current!=source.rows.end(),"history vector has an actual current-row slot");
  current->metadata=metadata;current->version_uuid=successor.version_uuid;current->previous_version_uuid=predecessor;
  mga::VisibilitySnapshotView snapshot;snapshot.allow_reader_own_uncommitted=false;
  snapshot.visible_through_local_transaction_id=1;snapshot.visible_through_local_transaction_id_is_boundary=true;
  snapshot.visible_through_commit_sequence=1;snapshot.visible_through_commit_sequence_is_boundary=true;
  const auto select=[&](auto candidate){return db::native_crypto_inline_private_key_policy_detail::Select(f::Id(1),std::move(candidate),binding,snapshot);};
  auto selected=select(source);Check(selected.ok()&&selected.body_physical_version_uuid==successor.version_uuid,
      "same semantic bytes choose the newest actual visible physical sequence");
  std::reverse(source.source.row_creators.begin(),source.source.row_creators.end());selected=select(source);
  Check(selected.ok()&&selected.body_physical_version_uuid==successor.version_uuid,"history selection is independent of retained image enumeration order");
  auto conflicting=source;auto false_body=*actual.body;false_body.inline_private_key_use_allowed=false;
  conflicting.source.relation.catalogs[loc.catalog_page_index].metadata.at(successor.version_uuid).record.payload=Golden(false_body);
  Empty(select(std::move(conflicting)));
  auto forked=source;auto& fork=forked.source.relation.catalogs[loc.catalog_page_index].page->body.rows[loc.catalog_row_index];
  fork.previous_version_uuid=f::Id(702);Empty(select(std::move(forked)));
  auto retired=source;for(auto& row:retired.rows)if(row.metadata.record.header.object_uuid.value==actual.body->body_uuid){
    row.metadata.record.header.deleted=true;row.metadata.lifecycle=c::CatalogObjectLifecycle::dropped;row.metadata.status=c::CatalogObjectStatus::retired;
  }Empty(select(std::move(retired)));
  // An exact historical true observation is intentionally not a present-day
  // permission: the private runtime must admit/revalidate activated selection.
  false_body.generation=2;auto denied=source;
  auto& latest=denied.source.relation.catalogs[loc.catalog_page_index].metadata.at(successor.version_uuid);
  latest.record.payload=Golden(false_body);latest.definition_version=2;
  for(auto& row:denied.rows)if(row.metadata.record.header.object_uuid.value==actual.body->body_uuid)row.metadata=latest;
  selected=select(std::move(denied));Check(selected.ok()&&selected.body->generation==1&&selected.body->inline_private_key_use_allowed&&
      selected.body_physical_version_uuid==predecessor,"explicit historical lookup never claims current activation or authorization");
  for(unsigned length:{4u,16u,64u}){
    auto longer=source;auto next_loc=loc;auto last_version=successor.version_uuid;
    auto& image=longer.source.relation.catalogs[loc.catalog_page_index];
    for(unsigned sequence=3;sequence<=length;++sequence){
      auto row=image.page->body.rows[next_loc.catalog_row_index];auto prior_metadata=image.metadata.at(last_version);
      image.page->body.rows[next_loc.catalog_row_index].next_row_version=sequence;
      image.page->body.rows[next_loc.catalog_row_index].next_version_uuid=f::Id(10000+sequence);
      row.previous_row_version=sequence-1;row.previous_version_uuid=last_version;row.row_version=sequence;row.version_uuid=f::Id(10000+sequence);
      row.next_row_version=0;row.next_version_uuid={};++prior_metadata.security_epoch;
      next_loc.catalog_row_index=image.page->body.rows.size();image.page->body.rows.push_back(row);
      image.metadata.emplace(row.version_uuid,prior_metadata);longer.source.row_creators.push_back(next_loc);last_version=row.version_uuid;
      for(auto& current_row:longer.rows)if(current_row.metadata.record.header.object_uuid.value==actual.body->body_uuid){
        current_row.metadata=prior_metadata;current_row.previous_version_uuid=row.previous_version_uuid;current_row.version_uuid=row.version_uuid;
      }
    }
    const auto first=select(longer);Check(first.ok()&&first.body_physical_version_uuid==last_version,"long same-generation history selects newest complete exact bytes");
    std::reverse(longer.source.row_creators.begin(),longer.source.row_creators.end());const auto reversed=select(std::move(longer));
    Check(reversed.ok()&&reversed.body_physical_version_uuid==last_version,"long history enumeration order never changes semantic selection");
  }
}
void Physical(){
  scratchbird::tests::OwnedTempDirectory artifacts;
  for(const auto& profile:d::kCanonicalFilespacePageProfiles)for(unsigned mode=0;mode!=4;++mode){
    const auto body=Body(mode==2);auto descriptor=Descriptor(body);
    if(mode==3)descriptor.default_decision=c::CatalogPolicyDefault::allow;
    db::NativeCreationCatalogSeed seed;
    if(mode){seed[3].push_back({BodyMetadata(body),{}});seed[3].push_back({DescriptorMetadata(descriptor),{}});}
    db::NativeFilespaceInitializationRequest request;
    request.bootstrap={f::Id(1),f::Id(2),profile.uuid,d::kNativeBootstrapIntegrityProfile,{},profile.page_size_bytes,1,0,1,7};
    request.operation_uuid=f::Id(3);request.writer_uuid=f::Id(4);request.policy_snapshot_uuid=f::Id(10);
    request.creator.transaction_uuid=origin;request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
    request.total_pages=64;request.creation_utc_millis=1789357072000ULL;
    const auto budget=128*p::u64{profile.page_size_bytes};u::StandaloneUuidV7Issuer issuer({f::Id(1),f::Id(10)},{{},0,1000});
    const auto path=(artifacts.path()/(std::to_string(profile.page_size_bytes)+"-"+std::to_string(mode))).string();
    d::FileDevice device;Check(device.Open(path,d::FileOpenMode::create_new).ok(),"real private native security-policy file");
    if(mode){
      auto foreign=body;foreign.database_uuid=f::Id(9);seed[3][0].metadata=BodyMetadata(foreign);
      const auto refused=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
      const auto extent=device.Size();Check(!refused.ok()&&extent.ok()&&!extent.size_bytes,"foreign database body refuses before physical effects");
      seed[3][0].metadata=BodyMetadata(body);
      for(unsigned wrong_role:{0u,2u,4u,5u}){
        auto wrong=seed;wrong[wrong_role].push_back(std::move(wrong[3][0]));wrong[3].erase(wrong[3].begin());
        const auto rejected=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,wrong,budget,issuer);
        const auto unchanged=device.Size();Check(!rejected.ok()&&unchanged.ok()&&!unchanged.size_bytes,"policy body requires authoritative security role before physical effects");
      }
    }
    const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,issuer);
    Check(created.ok(),"actual native security body and exact descriptor persist");
    Check(device.Close().ok()&&device.Open(path,d::FileOpenMode::open_existing_read_only).ok(),"independent read-only reopen");
    const std::vector<d::NativeFilespaceDevice> devices{{f::Id(2),profile.uuid,&device}};
    const db::NativeCatalogRelationBinding relation{created.receipt->relations[3].object_uuid,{}};
    if(mode){
      db::NativeCatalogVersionMutation mutation;mutation.transaction=request.creator;mutation.metadata=BodyMetadata(body);
      mutation.relation_uuid={p::UuidKind::object,relation.relation_uuid};mutation.page_number=63;
      db::NativeCatalogLeafPage destination;destination.header.database_uuid=f::Id(1);destination.header.filespace_uuid=f::Id(2);
      destination.header.page_uuid=f::Id(703);destination.header.page_number=63;destination.header.page_generation=1;
      destination.header.page_size_profile_uuid=profile.uuid;destination.header.page_size_bytes=profile.page_size_bytes;destination.header.page_type=6;
      destination.body.relation_uuid=mutation.relation_uuid;
      mga::PublishedSnapshotPin absent_pin;
      for(unsigned role:{1u,2u,3u,5u,6u}){
        reads=0;track_reads=true;
        const auto rejected=db::StageNativeCatalogVersionFromOpenDevices(devices,created.receipt->checkpoint,2,role,relation,absent_pin,
            mutation,destination,budget,f::Id(10),issuer);track_reads=false;
        Check(rejected.error==db::NativeCatalogVersionStageError::invalid_metadata&&!rejected.retry&&rejected.issued_version_uuid.is_nil()&&!reads,
            "ordinary typed staging rejects non-security placement before reads or identity issuance");
      }
      auto foreign=body;foreign.database_uuid=f::Id(9);mutation.metadata=BodyMetadata(foreign);reads=0;track_reads=true;
      const auto rejected=db::StageNativeCatalogVersionFromOpenDevices(devices,created.receipt->checkpoint,2,4,relation,absent_pin,
          mutation,destination,budget,f::Id(10),issuer);track_reads=false;
      Check(rejected.error==db::NativeCatalogVersionStageError::invalid_metadata&&!rejected.retry&&rejected.issued_version_uuid.is_nil()&&!reads,
          "ordinary typed staging rejects foreign database before reads or identity issuance");
    }
    const db::NativeCryptoInlinePrivateKeyPolicyLookupBinding binding{descriptor.policy_uuid,descriptor.policy_version_uuid,descriptor.generation};
    const auto read=[&](const auto& guard,p::u64 allowance=0){
      return db::ReadNativeCommittedCryptoInlinePrivateKeyPolicyFromOpenDevices(f::Id(1),devices,created.receipt->checkpoint,relation,guard,allowance?allowance:budget);
    };
    reads=0;track_reads=true;auto observed=read(binding);track_reads=false;const auto count=reads;
    if(!mode){Empty(observed);continue;}
    if(!observed.ok())std::cerr<<"lookup error="<<int(observed.error)<<'\n';
    Check(observed.ok()&&observed.body->inline_private_key_use_allowed==(mode==2)&&Golden(*observed.body)==Golden(body),"exact false/true purpose permission survives real native persistence and reopen");
    Check(observed.descriptor->policy_uuid==descriptor.policy_uuid&&observed.descriptor->body_uuid==body.body_uuid&&
        observed.descriptor->body_generation==body.generation&&!observed.descriptor_physical_version_uuid.is_nil()&&!observed.body_physical_version_uuid.is_nil()&&
        observed.descriptor_physical_version_uuid!=descriptor.policy_uuid&&observed.body_physical_version_uuid!=body.body_uuid,
        "observed exact descriptor and actual storage-issued MGA versions never become authorization");
    const auto zero=d::ReadFilespacePageZeroFromOpenDevice(device);Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"stored permission is not serving activation or an EXECUTE grant");
    for(unsigned n=0;n!=4;++n){auto bad=binding;if(n==0)bad.policy_uuid=f::Id(250);if(n==1)bad.policy_version_uuid=f::Id(251);
      if(n==2)bad.policy_generation=2;if(n==3)bad.policy_generation=0;Empty(read(bad));}
    Empty(read(binding,1));
    if(profile.page_size_bytes==d::kCanonicalFilespacePageProfiles[0].page_size_bytes&&mode==2)SelectionHistory(observed,binding);
    if(profile.page_size_bytes==d::kCanonicalFilespacePageProfiles[0].page_size_bytes&&mode==1){
      for(unsigned n=1;n<=count;++n){reads=0;read_fault=n;track_reads=true;const auto failed=read(binding);track_reads=false;
        Check(!read_fault,"actual native read fault consumed");Empty(failed);}
      allocations=0;const auto measured=read(binding);const auto sites=allocations;Check(measured.ok()&&sites,"measure real retained lookup allocations");
      unsigned optional_telemetry=0,operational_refusals=0;
      for(unsigned n=0;n<sites;++n){
        const auto telemetry_before=device.failed_io_latency_observations()+device.rejected_io_latency_observations();
        allocation_budget=n;const auto failed=read(binding);const auto left=allocation_budget;allocation_budget=-1;
        Check(left<0,"each measured actual allocation fault is consumed");
        if(!failed.ok()){Empty(failed);++operational_refusals;continue;}
        // I/O telemetry is explicitly non-authoritative and noexcept. An
        // actually recorded telemetry allocation failure must not invalidate
        // complete storage observations. Never tolerate an unclassified
        // successful fault or just ignore an incomplete result.
        Check(device.failed_io_latency_observations()+device.rejected_io_latency_observations()>telemetry_before,
            "successful allocation fallback is an actual recorded telemetry failure");
        Check(Golden(*failed.body)==Golden(body)&&DescriptorMetadata(*failed.descriptor).record.payload==DescriptorMetadata(descriptor).record.payload&&
            failed.descriptor_physical_version_uuid==measured.descriptor_physical_version_uuid&&
            failed.body_physical_version_uuid==measured.body_physical_version_uuid&&failed.source.rows.size()==measured.source.rows.size(),
            "telemetry-only failure preserves complete exact native observation");
        ++optional_telemetry;
      }
      Check(operational_refusals&&optional_telemetry&&operational_refusals+optional_telemetry==sites,"all allocation sites have actual classified outcomes");
      std::cout<<"native lookup allocation faults="<<sites<<" operational_refusals="<<operational_refusals<<" telemetry_only="<<optional_telemetry<<'\n';
      Check(read(binding).ok(),"native lookup recovers after every allocation and I/O fault");
    }
    Check(device.Close().ok(),"release own security-policy file");
  }
}
}
#ifdef SB_NATIVE_CRYPTO_POLICY_IO_WRAP
extern "C" ssize_t __real_pread(int,void*,size_t,off_t);
extern "C" ssize_t __wrap_pread(int fd,void* bytes,size_t size,off_t offset){
  if(track_reads&&++reads==read_fault){read_fault=0;errno=EIO;return -1;}return __real_pread(fd,bytes,size,offset);
}
#endif
int main()try{Generations();Physical();std::cout<<"PASS native inline private-key policy storage "<<checks<<" checks; not runtime admission\n";return 0;}
catch(const std::exception& e){allocation_budget=-1;track_reads=false;std::cerr<<e.what()<<'\n';return 1;}
