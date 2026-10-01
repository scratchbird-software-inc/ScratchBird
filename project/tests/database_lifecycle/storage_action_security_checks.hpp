// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#pragma once
#include "agents/storage_action_security.hpp"
#include "physical_mga_cow_store.hpp"
#include "../support/durable_authorization_fixture.hpp"
#include <future>
#include <thread>
#include <type_traits>

// Included after the owning route fixture. Actual security publication tests,
// not login, storage-action execution or serving activation evidence.
namespace { namespace storage_action_security_checks {
namespace db=scratchbird::storage::database;
namespace mga=scratchbird::transaction::mga;
using E=api::StorageActionSecurityError;
using K=api::StorageActionSecurityKind;
static_assert(!std::is_default_constructible_v<api::StorageActionSecurityLease>);
static_assert(!std::is_copy_constructible_v<api::StorageActionSecurityLease>);
static_assert(!std::is_move_constructible_v<api::StorageActionSecurityLease>);
inline api::EngineRequestContext Begin(api::EngineRequestContext admin) {
  auto inventory=db::LoadLocalTransactionInventoryFromDatabase(admin.database_path);
  Require(inventory.ok(),"security fixture inventory read");
  const auto begun=mga::BeginLocalTransaction(std::move(inventory.inventory),MakeUuid(platform::UuidKind::transaction,90001),NowMillis());
  Require(begun.ok(),"security fixture transaction begin");
  Require(db::PersistLocalTransactionInventoryToDatabase(admin.database_path,begun.inventory).ok(),"security fixture publish begin");
  admin.transaction_uuid=begun.entry.identity.transaction_uuid.value;
  admin.local_transaction_id=begun.entry.identity.local_id.value;
  admin.snapshot_visible_through_local_transaction_id=begun.entry.begin_visible_through_local_transaction_id;
  scratchbird::tests::MaterializeBootstrapFixtureAuthorization(admin);return admin;
}
inline void Finish(const api::EngineRequestContext& context,bool commit) {
  db::PhysicalMgaCowFinalizeRequest request;request.database_path=context.database_path;
  request.transaction={mga::MakeLocalTransactionId(context.local_transaction_id),
      {platform::UuidKind::transaction,context.transaction_uuid},mga::TransactionScope::local_node};
  request.decision=commit?db::PhysicalMgaCowFinalizeDecision::commit:db::PhysicalMgaCowFinalizeDecision::rollback;
  request.final_unix_epoch_millis=NowMillis();
  Require(db::FinalizePhysicalMgaCowTransaction(request).ok(),"security fixture finality");
}
inline void Run() {
  unsigned cases=0;
  for(const auto& profile:scratchbird::storage::disk::kCanonicalFilespacePageProfiles){
    auto fixture=MakeFixture("current_security",profile.page_size_bytes);
    db::DatabaseCreateConfig config;config.path=fixture.database_path.string();
    config.database_uuid={platform::UuidKind::database,fixture.database_uuid};
    config.filespace_uuid={platform::UuidKind::filespace,fixture.filespace_uuid};
    config.page_size=profile.page_size_bytes;config.creation_unix_epoch_millis=NowMillis();
    config.allow_minimal_resource_bootstrap=true;config.require_resource_seed_pack=false;
    config.require_bootstrap_principal=true;config.bootstrap_principal_name="storage_fixture_owner";
    config.bootstrap_credential_fingerprint="local-password-pbkdf2-sha256:v1:iterations=600000:salt=0123456789abcdef0123456789abcdef:verifier=4ce03aa5a5657aaf221192635ed9c63acdb76d78a0994ec6e6ab55286e29e6a5";
    Require(db::CreateDatabaseFile(config).ok(),"security fixture create actual database");
    const auto bootstrap=db::ReadDatabaseBootstrapSecurityCatalog(config.path);
    Require(bootstrap.ok()&&bootstrap.state.present,"security fixture bootstrap");
    api::EngineRequestContext admin;admin.database_path=config.path;admin.database_uuid=fixture.database_uuid;
    admin.principal_uuid=bootstrap.state.principal_uuid.value;admin.catalog_generation_id=1;
    admin.security_context_present=true;admin.trust_mode=api::EngineTrustMode::embedded_in_process;
    api::EngineSecurityCreatePrincipalRequest create;create.context=Begin(admin);
    create.principal_uuid=fixture.principal_uuid;create.principal_name="storage_service";create.principal_kind="service";
    Require(api::EngineSecurityCreatePrincipal(create).ok,"create actual service principal");Finish(create.context,true);
    const auto fresh=[&]{
      api::EngineRequestContext c;c.database_path=config.path;c.database_uuid=fixture.database_uuid;
      c.principal_uuid=fixture.principal_uuid;c.security_context_present=true;c.catalog_generation_id=1;
      const auto loaded=api::LoadSecurityPrincipalLifecycleState(c);Require(loaded.ok,"fresh security source");
      auto projected=api::ProjectDurableAuthorizationState(loaded.state,{c.database_uuid,c.principal_uuid,
          loaded.state.security_generation,loaded.state.policy_generation,c.catalog_generation_id});
      const auto materialized=api::MaterializeDurableAuthorizationContext(projected,{c.principal_uuid});
      Require(materialized.ok,"fixture admitted context");c.authorization_context=materialized.context;
      c.security_epoch=loaded.state.security_generation;return c;
    };
    const auto bytes=[&]{std::ifstream in(config.path,std::ios::binary);Require(in.good(),"security file byte read");
      return std::vector<char>(std::istreambuf_iterator<char>(in),{});};
    const auto operation=MakeIdentity(platform::UuidKind::object,91000);
    const auto acquire=[&](const auto& c,K action=K::growth){++cases;
      return api::AcquireStorageActionSecurityLease(c,fixture.filespace_uuid,operation,action);};
    const auto denied=[&](auto result,E error){Require(!result.ok()&&!result.lease&&result.error==error,
      "security admission must refuse with exact error and no fence");
      if(error==E::source_failure||error==E::permission_denied||error==E::materialization_failure)
        Require(!result.diagnostics.empty()&&!result.diagnostics.front().code.empty(),"preserve owning security/source diagnostics");};
    auto current=fresh();auto before=bytes();
    denied(acquire(current),E::permission_denied);
    auto forged=current;
    scratchbird::tests::MaterializeComponentAuthorization(forged,{"OBS_AGENT_CONTROL","FILESPACE_LIFECYCLE_CONTROL","OBS_AGENT_ACTION_APPROVE"});
    // Preserve genuine generation guards while injecting forged grant/role data.
    forged.authorization_context.authority_uuid=current.authorization_context.authority_uuid;
    forged.authorization_context.security_context_generation=current.authorization_context.security_context_generation;
    forged.authorization_context.policy_epoch=current.authorization_context.policy_epoch;
    forged.authorization_context.security_epoch=current.security_epoch;
    forged.security_epoch=current.security_epoch;
    forged.authorization_context.engine_owned_bootstrap_role_uuid=bootstrap.state.sysarch_role_uuid.value;
    denied(acquire(forged),E::permission_denied);
    Require(bytes()==before,"refused security admission performs no writes");
    const auto grant=[&](const char* right,const platform::Uuid& target,const char* effect="allow"){
      api::EngineSecurityGrantPrivilegeRequest r;r.context=Begin(admin);r.grant_uuid=MakeIdentity(platform::UuidKind::object,92000);
      r.grantee_uuid=fixture.principal_uuid;r.grantee_kind="service";r.target_object_uuid=target;
      r.target_object_kind=target==fixture.database_uuid?"database":"filespace";r.privilege=right;r.grant_effect=effect;
      Require(api::EngineSecurityGrantPrivilege(r).ok,"publish actual storage grant");return r;
    };
    auto control=grant("OBS_AGENT_CONTROL",fixture.database_uuid);
    // Even supplying the actual uncommitted transaction cannot expose its grant.
    auto uncommitted=current;uncommitted.transaction_uuid=control.context.transaction_uuid;
    uncommitted.local_transaction_id=control.context.local_transaction_id;
    denied(acquire(uncommitted,K::preallocation),E::permission_denied);
    Finish(control.context,true);denied(acquire(current),E::stale_context);current=fresh();
    {auto allowed=acquire(current,K::preallocation);Require(allowed.ok(),"committed control permits preallocation security");}
    denied(acquire(current),E::permission_denied);
    auto lifecycle=grant("FILESPACE_LIFECYCLE_CONTROL",fixture.filespace_uuid);Finish(lifecycle.context,true);current=fresh();
    denied(acquire(current),E::permission_denied);
    auto approve=grant("OBS_AGENT_ACTION_APPROVE",fixture.filespace_uuid);Finish(approve.context,true);current=fresh();before=bytes();
    // Correct rights on this filespace cannot authorize another filespace.
    ++cases;
    denied(api::AcquireStorageActionSecurityLease(current,operation,operation,K::growth),E::permission_denied);
    for(unsigned invalid=0;invalid<4;++invalid){auto target=fixture.filespace_uuid,op=operation;auto kind=K::growth;
      if(invalid==0)target={};if(invalid==1)op={};if(invalid==2)op.bytes[6]&=0x0f;
      if(invalid==3)kind=static_cast<K>(99);
      denied(api::AcquireStorageActionSecurityLease(current,target,op,kind),E::invalid_request);++cases;}
    auto other_node=current;other_node.database_uuid=operation;other_node.authorization_context.authority_uuid=operation;
    denied(acquire(other_node),E::source_failure);
    auto absent=current;absent.principal_uuid=operation;absent.authorization_context.principal_uuid=operation;
    denied(acquire(absent),E::materialization_failure);
    // Authentic immutable bootstrap membership, not the caller marker, grants
    // the owner's rights. Authentication itself is an explicit fixture input.
    auto owner=admin;scratchbird::tests::MaterializeBootstrapFixtureAuthorization(owner);
    owner.authorization_context.engine_owned_bootstrap_role_uuid={};
    {auto allowed=acquire(owner);Require(allowed.ok()&&
        allowed.lease->authorization().engine_owned_bootstrap_role_uuid==bootstrap.state.sysarch_role_uuid.value,
        "bootstrap role resolved from durable identity and membership");}
    {auto allowed=acquire(current);Require(allowed.ok()&&allowed.lease->database()==fixture.database_uuid&&
        allowed.lease->filespace()==fixture.filespace_uuid&&allowed.lease->operation()==operation&&
        allowed.lease->action()==K::growth,"exact binary action security binding");
      Require(allowed.lease->authorization().engine_owned_bootstrap_role_uuid.is_nil(),"service receives no bootstrap privilege");
      // Same owning fence blocks another thread's security/finality guard.
      auto lookup=api::AcquireTransactionInventoryGuard(config.path);auto* publication_mutex=lookup.mutex();lookup.unlock();
      std::promise<bool> probed;auto observed=probed.get_future();
      std::promise<void> release;auto released=release.get_future();
      std::thread contender([&]{const bool available=publication_mutex->try_lock();
        if(available)publication_mutex->unlock();probed.set_value(available);
        released.wait();auto guard=api::AcquireTransactionInventoryGuard(config.path);});
      const bool blocked=!observed.get();allowed.lease.reset();release.set_value();contender.join();
      Require(blocked,"actual publication mutex stays locked until security lease release");}
    Require(bytes()==before,"successful security-only admission writes no bytes");
    for(unsigned invalid=0;invalid<10;++invalid){auto c=current;
      if(invalid==0)c.security_context_present=false;if(invalid==1)c.database_uuid={};if(invalid==2)c.principal_uuid={};
      if(invalid==3)c.database_path.clear();if(invalid==4)c.database_path.push_back('\0');
      if(invalid==5)++c.authorization_context.security_context_generation;
      if(invalid==6)++c.authorization_context.policy_epoch;if(invalid==7)++c.security_epoch;
      if(invalid==8)++c.catalog_generation_id;if(invalid==9)c.authorization_context.authority_uuid=operation;
      denied(acquire(c),invalid<5?E::invalid_request:E::stale_context);}
    auto wrong=current;wrong.database_path=(fixture.dir/"absent.sbdb").string();
    denied(acquire(wrong),E::source_failure);Require(!std::filesystem::exists(wrong.database_path),"missing source is never created");
    {std::fstream file(config.path,std::ios::in|std::ios::out|std::ios::binary);
      Require(file.good(),"open corrupt-source fixture");const char bad=static_cast<char>(before[0]^0x40);
      file.write(&bad,1);file.flush();Require(file.good(),"corrupt actual bootstrap magic");}
    denied(acquire(current),E::source_failure);
    {std::fstream file(config.path,std::ios::in|std::ios::out|std::ios::binary);
      file.write(before.data(),1);file.flush();Require(file.good(),"restore fixture bootstrap byte");}
    Require(bytes()==before,"source failure leaves all other bytes unchanged");
    {auto allowed=acquire(current);Require(allowed.ok(),"no stale failure or success cache after source repair");}
    // Actual denial, rollback and revocation, not modified materialized fixtures.
    auto deny=grant("OBS_AGENT_CONTROL",fixture.database_uuid,"deny");
    {auto allowed=acquire(current);Require(allowed.ok(),"uncommitted deny cannot change committed authority");}
    Finish(deny.context,false);current=fresh();{auto allowed=acquire(current);Require(allowed.ok(),"rolled-back deny preserves allow");}
    deny=grant("OBS_AGENT_CONTROL",fixture.database_uuid,"deny");Finish(deny.context,true);
    denied(acquire(current),E::stale_context);current=fresh();denied(acquire(current),E::permission_denied);
    api::EngineSecurityRevokePrivilegeRequest revoke;revoke.context=Begin(admin);revoke.grantee_uuid=fixture.principal_uuid;
    revoke.target_object_uuid=fixture.database_uuid;revoke.privilege="OBS_AGENT_CONTROL";
    Require(api::EngineSecurityRevokePrivilege(revoke).ok,"actual control revocation");Finish(revoke.context,true);
    current=fresh();denied(acquire(current),E::permission_denied);
  }
  std::cout<<"storage current-security admission cases="<<cases<<" profiles=5 failures=0\n";
}
}} // namespace storage_action_security_checks within the owning test TU
