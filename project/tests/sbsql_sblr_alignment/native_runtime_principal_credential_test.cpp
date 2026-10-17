// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse the qualified real native-file byte and fault fixtures, not their main.
#define SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
#include "native_runtime_security_observation_test.cpp"
#undef SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
#include "../../src/server/native_runtime_principal_credential.hpp"
#include "runtime_crypto_pool_owner.hpp"
#include <sys/mman.h>
#include <sys/wait.h>
#include <fcntl.h>

// Unlike the imported byte fixture's aborting dispatcher, this composition's
// assertions unwind its actual device/source/protected owners before terminal
// crypto cleanup. No throwing assertion is evaluated inside a noexcept API.
#define Check CheckCredentialTest
namespace {
namespace server=scratchbird::server;
namespace mem=scratchbird::core::memory;
namespace hash=scratchbird::core::hash;
namespace runtime=scratchbird::core::runtime;
using CredentialError=server::NativeRuntimePrincipalCredentialError;
using CredentialResult=server::NativeRuntimePrincipalCredentialResult;
constexpr std::string_view password="DBLC013G-fixture-password";
constexpr std::string_view verifier=
    "local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:"
    "verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
bool password_provider_fault=false;
unsigned password_calls=0;
std::stop_source* password_cancel=nullptr;
void CheckCredentialTest(bool value,const char* label){++checks;if(value)return;
  allocation_budget=-1;read_fault=0;track_reads=false;password_provider_fault=false;password_cancel=nullptr;
  throw std::runtime_error(label);}

// The parent, not an intentionally terminated child, owns ALL child files.
// Every target is a fresh mkdtemp descendant, never a shared/pre-existing path.
struct ParentOwnedArtifacts {
  scratchbird::tests::OwnedTempDirectory directory;
  std::optional<std::string> previous;
  ParentOwnedArtifacts(){if(const auto* p=std::getenv("TMPDIR"))previous=p;
    Check(::setenv("TMPDIR",directory.path().c_str(),1)==0,"private parent-owned test TMPDIR");}
  ~ParentOwnedArtifacts(){Restore();}
  void Restore()noexcept {if(previous)(void)::setenv("TMPDIR",previous->c_str(),1);else(void)::unsetenv("TMPDIR");}
  void Cleanup(){Check(previous ? ::setenv("TMPDIR",previous->c_str(),1)==0 : ::unsetenv("TMPDIR")==0,"restore original test temporary namespace");
    directory.Cleanup();}
};

// All provider backing is actually admitted, protected and retained until the
// final OpenSSL thread/provider drain. These are deliberately small bounded
// component pools, not a production default selection.
struct CredentialCryptoOwner {
  static mem::AllocationPolicy Policy(){auto p=mem::DefaultLocalEngineMemoryPolicy();
    p.hard_limit_bytes=p.per_context_limit_bytes=p.page_buffer_pool_limit_bytes=16*1024*1024;
    p.soft_limit_bytes=0;return p;}
  mem::MemoryManager manager{Policy()};mem::HierarchicalMemoryBudgetLedger ledger;
  hash::CryptoMemoryBinding binding;
  runtime::RuntimeCryptoPoolOwner owner;
  CredentialCryptoOwner(unsigned database,unsigned seed,std::size_t bytes):binding{database?Id(database):platform::Uuid{},Id(seed),Id(seed+1),Id(seed+2)} {
    try {
    if(!database)binding.process=Id(230);
    mem::ReservationBackedMemoryResourceRequest q;q.memory_manager=&manager;q.reservation_ledger=&ledger;
    q.requested_bytes=bytes;q.category=mem::MemoryCategory::core_runtime;q.memory_class="crypto_provider";
    q.consumer_kind=mem::ReservationBackedMemoryConsumerKind::background_maintenance;
    q.route_label="native credential component";q.purpose="actual bounded provider backing";
    q.binary_operation_uuid=binding.operation.bytes;
    q.binary_ownership[mem::MemoryBinaryScopeKind::database]=binding.database.bytes;
    q.binary_ownership[mem::MemoryBinaryScopeKind::process]=binding.process.bytes;
    q.binary_ownership[mem::MemoryBinaryScopeKind::owner]=binding.owner.bytes;
    q.binary_ownership[mem::MemoryBinaryScopeKind::context]=binding.context.bytes;
    q.scope_chain={{mem::HierarchicalMemoryScopeKind::process,{},Id(230).bytes}};
    if(database)q.scope_chain.push_back({mem::HierarchicalMemoryScopeKind::database,{},Id(database).bytes});
    q.provenance.source=mem::HierarchicalMemoryBudgetProvenanceSource::server_runtime_api;
    q.provenance.source_label="actual native credential component";
    for(const auto& scope:q.scope_chain){mem::HierarchicalMemoryBudget budget;budget.scope=scope;
      budget.hard_limit_bytes=bytes;budget.provenance=q.provenance;Check(ledger.SetBudget(budget).ok(),"actual crypto parent budget");}
    auto grant=mem::AcquireReservationBackedMemoryResource(q);Check(grant.ok(),"actual binary crypto capacity admission");
    Check(owner.Adopt(binding,grant.resource,bytes,mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump).ok(),
        "actual strict protected crypto custody");
    }catch(...){if(!owner.Close().ok())std::terminate();throw;}
  }
  template<class F>void With(F&& f){Check(owner.WithMemoryScope(binding,std::forward<F>(f)).ok(),"actual lexical provider scope");}
  void Close(){Check(owner.Close().ok(),"provider drained before actual protected grant release");
    Check(manager.Snapshot().current_bytes==0&&manager.Snapshot().reserved_capacity_bytes==0&&ledger.Snapshot().current_bytes==0,
        "actual provider charge fully released");}
};

enum class CredentialVariant { valid,malformed_verifier,inactive_principal,retired_principal,retired_binding,external_binding,
    absent_provider,stale_provider,disabled_provider,wrong_provider_class,retired_provider };
struct CredentialNativeFixture {
  scratchbird::tests::OwnedTempDirectory artifacts;disk::FileDevice device;unsigned profile;u64 budget;
  db::NativeCreationWorkspaceReceipt receipt;
  CredentialNativeFixture(unsigned p,bool indexed=false,CredentialVariant variant=CredentialVariant::valid):profile(p),budget(512*u64{sizes[p]}) {
    using K=catalog::CatalogRecordKind;db::NativeCreationCatalogSeed seed;db::NativeCreationCatalogIndexes indexes;
    auto principal=SecurityRecord(K::user_account,200);principal.attributes.emplace("kind","service");
    principal.attributes.emplace("principal_kind","service");principal.attributes.emplace("bootstrap_principal",false);
    principal.attributes.emplace("principal_name","actual-native-service");
    principal.attributes.emplace("credential_fingerprint",std::string(variant==CredentialVariant::malformed_verifier?"not-a-verifier":verifier));
    if(variant==CredentialVariant::inactive_principal)principal.attributes["active"]=false;
    auto principal_row=SecurityMetadata(principal,0);
    if(variant==CredentialVariant::retired_principal){
      principal_row.metadata.lifecycle=catalog::CatalogObjectLifecycle::archived;principal_row.metadata.status=catalog::CatalogObjectStatus::retired;
      principal_row.metadata.retired_transaction_uuid=security_tx;}
    seed[3].push_back(std::move(principal_row));
    auto grant=SecurityRecord(K::grant_record,201);grant.identities.emplace("subject_uuid",Id(200));
    grant.attributes.emplace("grant_class","privilege");grant.attributes.emplace("subject_kind","principal");
    grant.attributes.emplace("right","CONNECT");grant.attributes.emplace("effect","allow");seed[3].push_back(SecurityMetadata(grant,1));
    catalog::CatalogRuntimeAuthorityBinding binding;binding.binding_uuid=Id(180);binding.database_uuid=binding.security_authority_uuid=Id(1);
    binding.service_principal_uuid=Id(200);binding.provider_uuid=Id(210);binding.policy_uuid=Id(52);binding.credential_reference_uuid=Id(211);
    binding.generation=binding.security_epoch=binding.catalog_generation=binding.provider_generation=1;binding.policy_epoch=31;
    if(variant==CredentialVariant::stale_provider)binding.provider_generation=2;
    binding.authority_mode=variant==CredentialVariant::external_binding?catalog::RuntimeAuthorityMode::security_database:catalog::RuntimeAuthorityMode::database_local;
    binding.origin_transaction_uuid=security_tx;binding.origin_local_transaction_id=1;
    auto encoded=catalog::EncodeCatalogRuntimeAuthorityBinding(binding);Check(encoded.ok(),"exact actual native runtime binding payload");
    auto metadata=security_fixture::Metadata(0,security_tx,Id(61),{});metadata.record.header.kind=K::config_profile;
    metadata.record.header.object_uuid={platform::UuidKind::object,Id(180)};metadata.record.header.row_uuid={platform::UuidKind::row,security_fixture::Id(3100)};
    metadata.security_policy_uuid={platform::UuidKind::object,binding.policy_uuid};
    metadata.object_subtype="agent_runtime_authority";metadata.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    if(variant==CredentialVariant::retired_binding){metadata.lifecycle=catalog::CatalogObjectLifecycle::archived;
      metadata.status=catalog::CatalogObjectStatus::retired;metadata.retired_transaction_uuid=security_tx;}
    seed[4].push_back({metadata,{}});
    if(variant!=CredentialVariant::absent_provider){security_fixture::Fixture policy(security_tx);policy.provider.provider_uuid=Id(210);
      if(variant==CredentialVariant::disabled_provider)policy.provider.enabled_by_default=false;
      auto provider_meta=security_fixture::Metadata(1,security_tx,Id(61),security_fixture::EncodeGolden(policy.provider));
      provider_meta.record.header.object_uuid={platform::UuidKind::object,Id(210)};
      provider_meta.record.header.row_uuid={platform::UuidKind::row,security_fixture::Id(3101)};
      if(variant==CredentialVariant::wrong_provider_class){
        // A valid other family at the selected identity, not corrupt bytes
        // that the native leaf codec must refuse before family selection.
        auto other=binding;other.binding_uuid=Id(210);const auto payload=catalog::EncodeCatalogRuntimeAuthorityBinding(other);
        Check(payload.ok(),"valid foreign family for exact provider identity");
        provider_meta.record.header.kind=K::config_profile;provider_meta.object_subtype="agent_runtime_authority";
        provider_meta.security_policy_uuid={platform::UuidKind::object,other.policy_uuid};
        provider_meta.record.payload.assign(payload.bytes.begin(),payload.bytes.end());}
      if(variant==CredentialVariant::retired_provider){provider_meta.lifecycle=catalog::CatalogObjectLifecycle::archived;
        provider_meta.status=catalog::CatalogObjectStatus::retired;provider_meta.retired_transaction_uuid=security_tx;}
      seed[4].push_back({provider_meta,{}});}
    if(indexed)for(unsigned role:{3u,4u}){page::NativeBtreeDependencies deps;deps.index_uuid=security_fixture::Id(8000+role);
      deps.descriptor_generation=deps.storage_generation=1;deps.key_profile_uuid=security_fixture::Id(8100+role);
      deps.visibility_profile_uuid=security_fixture::Id(8200+role);deps.dependency_map_uuid=security_fixture::Id(8300+role);
      deps.dependency_map_sha256.fill(role+1);indexes[role]=db::NativeCreationCatalogIndexBinding{security_fixture::Id(8400+role),deps};}
    db::NativeFilespaceInitializationRequest request;request.bootstrap={Id(1),Id(2),Profile(p),disk::kNativeBootstrapIntegrityProfile,{},sizes[p],1,0,1,7};
    request.operation_uuid=Id(3);request.writer_uuid=Id(4);request.policy_snapshot_uuid=Id(10);request.creator.transaction_uuid=security_tx;
    request.creator.local_id=mga::MakeLocalTransactionId(1);request.creator.scope=mga::TransactionScope::local_node;
    request.total_pages=256;request.creation_utc_millis=1789357072000ULL;
    scratchbird::core::uuid::StandaloneUuidV7Issuer issuer({Id(1),Id(10)},{{},0,1000});
    for(unsigned role=0;role<seed.size();++role)for(const auto& row:seed[role]){
      const auto invalid=catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(row.metadata));
      if(invalid)std::cerr<<"credential metadata role="<<role+1<<" variant="<<int(variant)<<" detail="<<invalid->detail<<'\n';
      Check(!invalid,"complete native credential family/common metadata before physical construction");}
    const auto path=(artifacts.path()/"native-credential").string();Check(device.Open(path,disk::FileOpenMode::create_new).ok(),"create real native credential fixture");
    const auto made=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(device,request,seed,indexes,budget,issuer);
    if(!made.ok())std::cerr<<"credential fixture creation="<<int(made.error)<<" variant="<<int(variant)<<" profile="<<p
      <<" checkpoint="<<int(made.checkpoint_error)<<" selection="<<int(made.selection_error)
      <<" catalog="<<(made.catalog_read_error?int(*made.catalog_read_error):-1)<<" relation="<<(made.catalog_relation_error?int(*made.catalog_relation_error):-1)<<'\n';
    Check(made.ok(),"actual security/provider definitions persisted in roles4/5");receipt=*made.receipt;
    Check(device.Close().ok()&&device.Open(path,disk::FileOpenMode::open_existing).ok(),"independent native credential physical reopen");
  }
  auto Request()const {server::NativeRuntimePrincipalCredentialRequest r;r.security.database_uuid=Id(1);r.security.runtime_binding_uuid=Id(180);
    r.security.runtime_binding_generation=1;r.security.maximum_retained_image_bytes=budget;
    r.security.configuration_relation={receipt.relation_uuids[4],receipt.index_dependencies[4]};
    r.security.security_relation={receipt.relation_uuids[3],receipt.index_dependencies[3]};return r;}
};

// Password framing is written directly from protected staging into the sealed
// source. The runtime's protected import and held generation lock are real.
struct CredentialCustody {
  mem::MemoryManager manager{CredentialCryptoOwner::Policy()};mem::MemoryTag tag;
  server::StartupCredentialSource source{Id(231),4};
  std::optional<server::StartupCredentialSource::Lease> lease;mem::ScopedProtectedBuffer derived;
  server::StartupCredentialTransportBinding binding{Id(1),Id(200),Id(210),Id(211),7,static_cast<std::uint32_t>(::geteuid())};
  CredentialCustody(unsigned mismatch=0,bool wrong_password=false,std::size_t derived_bytes=32,unsigned derived_database=1) {
    tag.category=mem::MemoryCategory::core_runtime;tag.binary_ownership[mem::MemoryBinaryScopeKind::database]=Id(1).bytes;
    tag.binary_ownership[mem::MemoryBinaryScopeKind::owner]=Id(200).bytes;tag.binary_ownership[mem::MemoryBinaryScopeKind::context]=Id(222).bytes;
    if(mismatch==1)binding.principal=Id(249);if(mismatch==2)binding.provider=Id(249);if(mismatch==3)binding.credential_reference=Id(249);
    const std::string_view secret=wrong_password?"wrong-password":password;
    mem::ProtectedMemoryRequest request;request.bytes=server::kStartupCredentialHeaderBytes+secret.size();request.tag=tag;
    request.material_class="credential protected staging";request.platform_policy=mem::ProtectedMemoryPlatformPolicy::require_lock_and_no_dump;
    auto staging=manager.AllocateProtected(request);Check(staging.ok(),"actual protected credential frame staging");
    Check(server::EncodeStartupCredentialFrame(binding,{reinterpret_cast<const std::uint8_t*>(secret.data()),secret.size()},
        {static_cast<std::uint8_t*>(staging.buffer.data()),staging.buffer.size()})==server::StartupCredentialFrameError::none,"exact binary protected frame");
    const int fd=::memfd_create("native-credential-test",MFD_CLOEXEC|MFD_ALLOW_SEALING);Check(fd>=0,"actual anonymous credential descriptor");
    Check(::write(fd,staging.buffer.data(),staging.buffer.size())==static_cast<ssize_t>(staging.buffer.size())&&::fchmod(fd,0400)==0&&
        ::fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)==0,"actual sealed protected source");
    const server::StartupCredentialSource::Request publication{Id(231),Id(232),0,{}, {}};
    Check(source.Publish(publication,fd,binding,manager,tag).ok(),"actual source generation publication");Check(::close(fd)==0,"release caller descriptor");
    staging.buffer={};auto acquired=source.Acquire({Id(231),Id(233),7,{},{}},manager,tag);
    Check(acquired.outcome.ok()&&acquired.lease,"actual current generation credential lease");lease.emplace(std::move(*acquired.lease));
    request.bytes=derived_bytes;request.tag.binary_ownership[mem::MemoryBinaryScopeKind::database]=Id(derived_database).bytes;
    request.material_class="local password derivation";auto scratch=manager.AllocateProtected(request);
    Check(scratch.ok(),"actual private protected derivation workspace");derived=std::move(scratch.buffer);
  }
};
void Refused(const CredentialResult& result,CredentialError expected){
  if(result.error!=expected)std::cerr<<"credential error="<<int(result.error)<<" expected="<<int(expected)<<" security="<<int(result.security_error)
    <<" source="<<int(result.source_error)<<" record="<<int(result.record_error)<<'\n';
  Check(result.error==expected&&!result.lease&&!result.ok(),"refusal exposes no private-issued credential proof");
}
bool Zero(const mem::ScopedProtectedBuffer& buffer){return std::all_of(static_cast<const unsigned char*>(buffer.data()),
    static_cast<const unsigned char*>(buffer.data())+buffer.size(),[](auto b){return b==0;});}

template<class Work>void OnActualSource(CredentialNativeFixture& fixture,Work&& work,unsigned custody_mismatch=0,bool wrong_password=false,
    std::size_t derived_bytes=32,unsigned derived_database=1) {
  const db::NativeCatalogRelationMemoryLimits limits{Profile(fixture.profile),1,16,64,64,{4,16*static_cast<std::size_t>(sizes[fixture.profile])}};
  const auto bytes=server::NativeRuntimePrincipalCredentialWorkspaceBytes(limits);Check(bytes,"checked full credential observation workspace");
  LeafMemoryFixture memory(bytes),source_memory(32*1024*1024);
  auto prepared=server::PrepareNativeRuntimePrincipalCredential(limits,memory.memory,memory.binding);Check(prepared.ok(),"both readers admitted before guards");
  CredentialCustody custody(custody_mismatch,wrong_password,derived_bytes,derived_database);
  const std::array<disk::NativeFilespaceDevice,1> devices{{{Id(2),Profile(fixture.profile),&fixture.device}}};
  auto selected=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),devices,Id(2),{fixture.budget,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(selected.ok(),"actual selected-native checkpoint/inventory lease");
  auto request=fixture.Request();
  work(request,selected.lease,prepared.workspace,*custody.lease,custody.derived);
  selected.lease.Reset();custody.lease.reset();prepared.workspace={};
  memory.memory={};source_memory.memory={};memory.Empty();source_memory.Empty();
}
auto Verify(const server::NativeRuntimePrincipalCredentialRequest& request,db::NativeSelectedCheckpointMemoryHandle& source,
    server::NativeRuntimePrincipalCredentialPreparedMemory& workspace,server::StartupCredentialSource::Lease& credential,
    const mem::ScopedProtectedBuffer& derived) {
  allocation_budget=0;auto result=server::VerifyNativeRuntimePrincipalCredential(request,source,workspace,credential,derived);
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining==0,"native credential verification allocates no ordinary heap under device guards");return result;
}
void Success(unsigned profile,bool indexed,int death_mode=0) {
  CredentialNativeFixture fixture(profile,indexed);const auto writes=stage_writes,syncs=stage_syncs;
  OnActualSource(fixture,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    const auto calls=password_calls;auto result=Verify(request,source,workspace,credential,derived);
    if(!result.ok())std::cerr<<"positive credential="<<int(result.error)<<" verifier="<<int(result.verifier_error)<<" source="<<int(result.source_error)<<'\n';
    Check(result.ok()&&password_calls==calls+1,"real PBKDF2 against actual committed native verifier");
    Check(!source&&!workspace&&!credential.binding()&&Zero(derived),"success transfers all fences and erases actual derived secret");
    Check(result.lease->binding().binding_uuid==Id(180)&&result.lease->principal().principal_uuid==Id(200)&&
        result.lease->principal().security_context_generation==9&&result.lease->principal().security_generation==11&&
        result.lease->provider().provider_uuid==Id(210)&&result.lease->provider().generation==1&&
        result.lease->credential_binding().generation==7&&result.lease->security_records().size()==2,
        "source/provider/credential generations stay distinct and exact real grants retained");
    const auto row=result.lease->security_records().front().object_uuid;
    auto moved=std::move(*result.lease);Check(!result.ok()&&!result.lease->valid()&&moved.valid()&&moved.security_records().front().object_uuid==row,
        "proof move transfers original backing and invalidates old proof without copying secret");
    if(death_mode){std::set_terminate([]{::_exit(73);});
      if(death_mode==1){std::thread other([&]{(void)moved.binding();});other.join();}
      if(death_mode==2){std::thread other([&]{auto foreign=std::move(moved);(void)foreign;});other.join();}
      if(death_mode==3)(void)result.lease->binding();
      ::_exit(74);}
  });
  Check(stage_writes==writes&&stage_syncs==syncs,"observation/authentication never creates or publishes BEGIN/storage effects");
}
void Refusals(CredentialCryptoOwner& foreign) {
  CredentialNativeFixture fixture(0);
  OnActualSource(fixture,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    const auto retained=credential.binding();const auto password_pointer=credential.credential().data();
    const auto retained_owner=[&]{Check(bool(source)&&bool(workspace)&&credential.binding()&&credential.credential().data()==password_pointer&&
        credential.binding()->generation==retained->generation,"failed verification leaves actual source/workspace/protected credential custody intact");};
    auto bad=request;bad.security.runtime_binding_generation=2;auto stale=Verify(bad,source,workspace,credential,derived);
    Refused(stale,CredentialError::security_source_failure);Check(stale.security_error==SecurityError::binding_generation_mismatch,"stale exact runtime generation cause");retained_owner();
    bad=request;bad.security.database_uuid=Id(250);Refused(Verify(bad,source,workspace,credential,derived),CredentialError::protected_workspace_required);retained_owner();
    bad=request;bad.security.maximum_retained_image_bytes=1;auto bounded=Verify(bad,source,workspace,credential,derived);
    Refused(bounded,CredentialError::security_source_failure);retained_owner();
    std::stop_source stop;stop.request_stop();bad=request;bad.stop=stop.get_token();const auto calls=password_calls;
    Refused(Verify(bad,source,workspace,credential,derived),CredentialError::cancelled);Check(password_calls==calls,"pre-cancellation performs no password provider operation");retained_owner();
    password_provider_fault=true;auto failed=Verify(request,source,workspace,credential,derived);Refused(failed,CredentialError::provider_failure);
    Check(failed.verifier_error==runtime_security::LocalPasswordCredentialError::provider_failure&&Zero(derived),"partial provider output erased with exact typed cause");retained_owner();
    std::stop_source after;bad=request;bad.stop=after.get_token();password_cancel=&after;
    Refused(Verify(bad,source,workspace,credential,derived),CredentialError::cancelled);password_cancel=nullptr;
    Check(Zero(derived),"post-provider cancellation erases secret and publishes no proof");retained_owner();
    reads=0;track_reads=true;read_fault=1;auto io=Verify(request,source,workspace,credential,derived);read_fault=0;track_reads=false;
    Refused(io,CredentialError::security_source_failure);Check(io.io_failure&&!io.io_failure->ok()&&io.io_failure_filespace_uuid==Id(2),"actual I/O failure retains physical cause");retained_owner();
  });
  // Owner Check/Snapshot and scope admission happen before acquiring ANY
  // device guard. Public source hashing here is not credential admission; the
  // credential gate must reject this actual foreign database crypto scope.
  foreign.With([&]{OnActualSource(fixture,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    auto wrong=Verify(request,source,workspace,credential,derived);Refused(wrong,CredentialError::crypto_scope_unavailable);
    Check(wrong.crypto_error==hash::CryptoMemoryError::invalid_binding&&source&&workspace&&credential.binding()&&Zero(derived),
        "actual foreign database crypto scope rejected without consuming input owners");});});
  for(unsigned mismatch=1;mismatch<=3;++mismatch)OnActualSource(fixture,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    Refused(Verify(request,source,workspace,credential,derived),CredentialError::credential_binding_mismatch);
    Check(source&&workspace&&credential.binding()&&Zero(derived),"foreign principal/provider/reference refuses without consuming owners");},mismatch);
  OnActualSource(fixture,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    auto result=Verify(request,source,workspace,credential,derived);Refused(result,CredentialError::credential_rejected);
    Check(result.verifier_error==runtime_security::LocalPasswordCredentialError::mismatch&&Zero(derived),"wrong actual protected password is mismatch not malformed verifier");},0,true);
  for(unsigned kind=0;kind<2;++kind)OnActualSource(fixture,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    Refused(Verify(request,source,workspace,credential,derived),CredentialError::protected_workspace_required);
    Check(source&&workspace&&credential.binding(),"invalid protected workspace retains all input custody");},0,false,kind?32:31,kind?250:1);
  const std::array variants{CredentialVariant::malformed_verifier,CredentialVariant::inactive_principal,CredentialVariant::retired_principal,
    CredentialVariant::retired_binding,CredentialVariant::external_binding,
    CredentialVariant::absent_provider,CredentialVariant::stale_provider,CredentialVariant::wrong_provider_class,CredentialVariant::retired_provider};
  for(auto variant:variants){CredentialNativeFixture invalid(0,false,variant);
    OnActualSource(invalid,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
      auto result=Verify(request,source,workspace,credential,derived);
      const auto expected=variant==CredentialVariant::malformed_verifier?CredentialError::credential_rejected:
        variant==CredentialVariant::inactive_principal||variant==CredentialVariant::retired_principal||
        variant==CredentialVariant::retired_binding||variant==CredentialVariant::external_binding?CredentialError::security_source_failure:
        variant==CredentialVariant::absent_provider?CredentialError::provider_absent:variant==CredentialVariant::stale_provider?CredentialError::provider_generation_mismatch:
        variant==CredentialVariant::retired_provider?CredentialError::provider_inactive:CredentialError::provider_invalid;
      Refused(result,expected);Check(source&&workspace&&credential.binding()&&Zero(derived),"actual malformed/stale/retired native declarations leave no partial proof");
      if(variant==CredentialVariant::malformed_verifier)Check(result.verifier_error==runtime_security::LocalPasswordCredentialError::invalid_verifier,
          "native stored malformed verifier cause distinct from wrong password");
    });}
  CredentialNativeFixture default_disabled(0,false,CredentialVariant::disabled_provider);
  const auto before_writes=stage_writes,before_syncs=stage_syncs;
  OnActualSource(default_disabled,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    auto observed=Verify(request,source,workspace,credential,derived);
    Check(observed.ok()&&!observed.lease->provider().enabled_by_default&&Zero(derived),
        "real comparison preserves default-disabled declaration without claiming current provider activation");
  });
  Check(stage_writes==before_writes&&stage_syncs==before_syncs,
      "default-disabled credential comparison publishes no activation or other storage effects");
}
void PreparationFaults(){const db::NativeCatalogRelationMemoryLimits limits{Profile(0),1,16,64,64,{4,16*static_cast<std::size_t>(sizes[0])}};
  const auto bytes=server::NativeRuntimePrincipalCredentialWorkspaceBytes(limits);
  // Each visibility reader has an observation arena and a relation's overhead,
  // root, Btree and one arena per maximum base leaf; security adds its records.
  const auto physical_allocations=1+2*(4+limits.maximum_base_pages);
  // Measure THIS complete successful prepare, excluding admission and explicit
  // allocating snapshots. Never guess a fixed allocation-site ceiling: every
  // real retained base-leaf workspace has its own allocator/bookkeeping work.
  const auto allocation_sites=[&]{LeafMemoryFixture memory(bytes);
    observed_allocations=0;count_allocations=true;
    auto prepared=server::PrepareNativeRuntimePrincipalCredential(limits,memory.memory,memory.binding);
    count_allocations=false;const auto sites=observed_allocations;
    const auto actual=memory.manager.Snapshot();
    Check(prepared.ok()&&actual.current_bytes==bytes&&actual.active_allocation_count==physical_allocations&&
        actual.reserved_capacity_bytes==0&&memory.ledger.Snapshot().current_bytes==bytes,
        "measured baseline owns every actual admitted preparation backing");
    prepared.workspace={};const auto released=memory.manager.Snapshot();
    Check(!released.current_bytes&&!released.active_allocation_count&&released.reserved_capacity_bytes==bytes,
        "measured baseline releases every physical owner before its capacity");
    memory.memory={};memory.Empty();return sites;}();
  Check(allocation_sites&&allocation_sites<static_cast<unsigned long>(std::numeric_limits<long>::max()),
      "actual preparation allocation count fits complete checked fault-position range");
  bool completed=false;unsigned refused=0,reporting_only=0;
  for(unsigned long fault=0;fault<=allocation_sites&&!completed;++fault){LeafMemoryFixture memory(bytes);
    const auto before=memory.manager.Snapshot();
    Check(!before.current_bytes&&!before.active_allocation_count&&before.reserved_capacity_bytes==bytes,
        "preparation fault attempt starts with real reserved credit and no physical payload");
    allocation_budget=fault;
    auto prepared=server::PrepareNativeRuntimePrincipalCredential(limits,memory.memory,memory.binding);
    const auto remaining=allocation_budget;allocation_budget=-1;
    const auto after=memory.manager.Snapshot();
    if(!prepared.ok()){++refused;Check(remaining==-1&&!prepared.workspace,
        "each consumed allocation refusal publishes neither partial security nor partial provider reader");}
    else {
      Check(bool(prepared.workspace)&&after.current_bytes==bytes&&after.active_allocation_count==physical_allocations&&
          after.reserved_capacity_bytes==0&&memory.ledger.Snapshot().current_bytes==bytes,
          "successful preparation retains every real backing and exact physical/hierarchical charges");
      if(remaining==-1){
        // Core memory67–71: failure of optional reporting AFTER physical commit
        // must not turn real owned backing into a false allocation refusal.
        // Classify it explicitly and keep sweeping: it is NOT terminal proof.
        ++reporting_only;Check(after.telemetry_truncation_count>before.telemetry_truncation_count,
            "consumed reporting-only fault preserves complete backing and records actual telemetry loss");
      }else {completed=true;Check(remaining>=0,"terminal successful preparation has no consumed hidden allocation fault");}
    }
    prepared.workspace={};const auto released=memory.manager.Snapshot();
    Check(!released.current_bytes&&!released.active_allocation_count&&released.reserved_capacity_bytes==bytes&&
        memory.ledger.Snapshot().current_bytes==bytes,"all successful/refused preparation payloads released before original capacity");
    memory.memory={};memory.Empty();}
  Check(completed&&refused&&reporting_only,"complete backing/refusal/reporting fault sweep reaches genuine unconsumed terminal success");
}
}
extern "C" int __real_PKCS5_PBKDF2_HMAC(const char*,int,const unsigned char*,int,int,const EVP_MD*,int,unsigned char*);
extern "C" int __wrap_PKCS5_PBKDF2_HMAC(const char* p,int pn,const unsigned char* s,int sn,int iterations,const EVP_MD* md,int n,unsigned char* out){
  ++password_calls;if(password_provider_fault){password_provider_fault=false;if(n>0)out[0]=0xa5;return 0;}
  const auto result=__real_PKCS5_PBKDF2_HMAC(p,pn,s,sn,iterations,md,n,out);
  if(password_cancel)password_cancel->request_stop();return result;
}
int CredentialMain(int argc,char** argv){
  ParentOwnedArtifacts artifacts;
  const int death=argc==2?std::atoi(argv[1]):0;
  if(!death)for(int mode=1;mode<=3;++mode){const auto pid=::fork();Check(pid>=0,"owned credential thread-affinity child");
    if(!pid){const std::string option=std::to_string(mode);::execl(argv[0],argv[0],option.c_str(),nullptr);::_exit(72);}
    int status=0;Check(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==73,"foreign access/move and moved-from access terminate before custody access");}
  std::optional<CredentialCryptoOwner> process,database,foreign;bool installed=false;int outcome=0;
  try {
  process.emplace(0,240,4*1024*1024);database.emplace(1,244,2*1024*1024);foreign.emplace(250,248,128*1024);
  Check(process->owner.InstallProcessAdapter().ok(),"actual process allocator installed before first OpenSSL allocation");installed=true;
  database->With([&]{if(death){Success(0,false,death);return;}
    for(unsigned profile=0;profile<5;++profile)for(bool indexed:{false,true})Success(profile,indexed);
    Refusals(*foreign);PreparationFaults();});
  // No implicit process fallback: construct/read selected source under a real
  // database scope, then explicitly execute the gate without an active scope.
  std::optional<CredentialNativeFixture> no_scope;
  database->With([&]{no_scope.emplace(0);});
  OnActualSource(*no_scope,[&](auto& request,auto& source,auto& workspace,auto& credential,const auto& derived){
    auto result=Verify(request,source,workspace,credential,derived);Refused(result,CredentialError::crypto_scope_unavailable);
    Check(result.crypto_error==hash::CryptoMemoryError::no_explicit_scope&&source&&workspace&&credential.binding(),"process pool never substitutes an explicit database scope");});
  no_scope.reset();
  }catch(const std::exception& e){allocation_budget=-1;read_fault=0;track_reads=false;password_cancel=nullptr;
    std::cerr<<"native runtime principal credential FAIL "<<e.what()<<" checks="<<checks<<'\n';outcome=1;}
  // The try block has unwound every native guard/credential use lease and every
  // lexical scope before provider cleanup; do not try to drain active users.
  if(installed){OPENSSL_thread_stop();OPENSSL_cleanup();
    Check(hash::StopCryptoMemoryAdapter()==hash::CryptoMemoryError::none,"terminal provider allocator drained");}
  if(foreign)foreign->Close();if(database)database->Close();if(process)process->Close();
  artifacts.Cleanup();if(!outcome)std::cout<<"native runtime principal credential PASS "<<checks<<" checks\n";return outcome;
}
int main(int argc,char** argv){try{return CredentialMain(argc,argv);}catch(const std::exception& e){std::cerr<<"native credential owner cleanup FAIL "<<e.what()<<'\n';return 1;}}
#undef Check
