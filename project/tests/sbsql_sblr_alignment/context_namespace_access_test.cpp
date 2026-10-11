// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#define main ContextNamespaceCodecMain
#include "catalog_context_namespace_test.cpp"
#undef main
#include "security/context_namespace_access.hpp"
#include "security/security_model.hpp"

namespace api = scratchbird::engine::internal_api;
namespace {
struct AccessFixture {
  c::CatalogContextNamespace definition=Definition();
  c::CatalogMetadataVersion metadata=Metadata(definition);
  api::EngineRequestContext context;
  api::DurableAuthorizationState state;
  AccessFixture() {
    context.database_uuid=definition.database_uuid;context.principal_uuid=Id(31);
    context.security_context_present=true;context.security_epoch=9;context.catalog_generation_id=11;
    state.authority_uuid=context.database_uuid;state.security_context_generation=3;
    state.security_epoch=9;state.policy_epoch=7;state.catalog_generation_id=11;
    state.principals.push_back({context.principal_uuid,"principal",true,9});
    state.grants.push_back({Id(32),context.principal_uuid,"principal",definition.namespace_uuid,"CONTEXT_READ",false,true,9});
    Refresh();
  }
  void Refresh() {
    const auto materialized=api::MaterializeDurableAuthorizationContext(state,
        {context.principal_uuid,state.security_epoch,state.policy_epoch,state.catalog_generation_id});
    Check(materialized.ok,"context grant materialization failed");
    context.authorization_context=materialized.context;
  }
  bool Access(api::ContextNamespaceAccess access=api::ContextNamespaceAccess::read) const {
    return api::EvaluateContextNamespaceAccess(context,c::BorrowCatalogMetadataVersion(metadata),
        definition.namespace_uuid,definition.generation,access);
  }
};
}
int main() {
  Check(ContextNamespaceCodecMain()==0,"codec controls");
  AccessFixture valid;
  Check(valid.Access(),"exact read grant refused");
  Check(!valid.Access(api::ContextNamespaceAccess::write),"read grant supplied write authority");
  valid.state.grants.push_back({Id(33),valid.context.principal_uuid,"principal",valid.definition.namespace_uuid,"CONTEXT_WRITE",false,true,9});
  valid.Refresh();Check(valid.Access(api::ContextNamespaceAccess::write),"exact write grant refused");
  for (unsigned which=0;which<18;++which) {
    AccessFixture f;
    switch(which) {
      case 0:f.context.security_context_present=false;break;
      case 1:f.context.authorization_context.present=false;break;
      case 2:f.context.authorization_context.authority_uuid=Id(40);break;
      case 3:f.context.database_uuid=Id(40);break;
      case 4:f.context.principal_uuid=Id(40);break;
      case 5:++f.context.security_epoch;break;
      case 6:++f.context.catalog_generation_id;break;
      case 7:f.state.grants.clear();f.Refresh();
        f.context.trace_tags={"right:CONTEXT_READ","security.fixture_trace_authority"};break;
      case 8:f.state.grants[0].target_uuid=Id(40);f.Refresh();break;
      case 9:f.state.grants.push_back({Id(41),f.context.principal_uuid,"principal",f.definition.namespace_uuid,"CONTEXT_READ",true,true,9});f.Refresh();break;
      case 10:f.metadata.record.header.deleted=true;break;
      case 11:f.metadata.lifecycle=c::CatalogObjectLifecycle::creating;break;
      case 12:f.metadata.status=c::CatalogObjectStatus::proposed;break;
      case 13:f.metadata.definition_version=2;break;
      case 14:f.metadata.catalog_generation=12;break;
      case 15:f.metadata.security_epoch=10;break;
      case 16:f.definition.namespace_uuid=Id(40);break;
      case 17:f.definition.generation=2;break;
    }
    Check(!f.Access(),"absent/stale/cross-owner/denied namespace granted");
  }
  AccessFixture policy;
  api::DurableAuthorizationPolicyRecord check;
  check.policy_uuid=Id(42);check.subject_uuid=policy.context.principal_uuid;
  check.subject_kind="principal";check.target_uuid=policy.definition.namespace_uuid;
  check.right="CONTEXT_READ";check.policy_kind="application_context";check.policy_epoch=7;
  check.active=true;check.requires_runtime_recheck=true;
  policy.state.policies.push_back(check);policy.Refresh();
  Check(!policy.Access(),"unperformed policy recheck granted access");
  policy.state.policies[0].requires_runtime_recheck=false;policy.state.policies[0].deny=true;
  policy.Refresh();Check(!policy.Access(),"policy denial ignored");
  Check(!api::EvaluateContextNamespaceAccess(valid.context,c::BorrowCatalogMetadataVersion(valid.metadata),
      valid.definition.namespace_uuid,1,static_cast<api::ContextNamespaceAccess>(99)),"unknown access admitted");
  // The warmed exact grant path requires no allocation. A denial constructs
  // diagnostics; prove its fault actually fires and stays contained.
  allocation_fault_fired=false;deny_heap=true;
  const bool allocation_free=valid.Access();
  deny_heap=false;
  Check(allocation_free&&!allocation_fault_fired,"exact granted path allocated or failed");
  allocation_fault_fired=false;deny_heap=true;
  const bool exhausted=policy.Access();
  deny_heap=false;
  Check(allocation_fault_fired&&!exhausted,"unexercised allocation fault or namespace access granted");
  Check(!policy.Access(),"retry lost original policy denial");
  std::cout<<"context_namespace_access checks="<<checks<<" PASS\n";
  return 0;
}
