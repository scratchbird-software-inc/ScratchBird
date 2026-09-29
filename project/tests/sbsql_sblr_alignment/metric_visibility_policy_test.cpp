// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Reuse independent byte-field helpers, not production encoders, as the oracle.
#define main MetricDescriptorOracleMain
#include "catalog_metric_descriptor_test.cpp"
#undef main
#include "catalog_metric_visibility_policy.hpp"
#include "security/metric_visibility_policy.hpp"
#include "security/security_model.hpp"

namespace api = scratchbird::engine::internal_api;
namespace {
std::string PolicyGolden(const c::CatalogMetricVisibilityPolicy& v) {
  std::string s(24,0);s.replace(0,4,"SBCV");Put(s,4,1,2);Put(s,6,24,2);
  Put(s,12,8,4);Put(s,16,65559,4);Put(s,20,1,2);
  Field(s,1,5,Uuid(v.policy_uuid));Field(s,2,1,Number(v.generation));
  Field(s,3,5,Uuid(v.database_uuid));Field(s,4,5,Uuid(v.metric_uuid));
  Field(s,5,3,v.read_right);Field(s,6,3,v.sensitive_read_right);
  Field(s,7,5,Uuid(v.origin_transaction_uuid.value));Field(s,8,1,Number(v.origin_local_transaction_id));
  Put(s,8,s.size(),4);return s;
}
struct VisibilityFixture {
  c::CatalogMetricDescriptor descriptor=Descriptor();
  c::CatalogMetricVisibilityPolicy policy;
  c::CatalogMetadataVersion dm,pm;
  api::EngineRequestContext context;
  VisibilityFixture() {
    descriptor.definition.security_family="OBS_METRICS_READ_FAMILY";
    policy.policy_uuid=descriptor.binding.visibility_policy_uuid;
    policy.generation=descriptor.binding.visibility_policy_generation;
    policy.database_uuid=Id(p::UuidKind::database,80).value;
    policy.metric_uuid=descriptor.binding.metric_uuid;
    policy.read_right=descriptor.definition.security_family;
    policy.sensitive_read_right="UNMASK";
    policy.origin_transaction_uuid=descriptor.origin_transaction_uuid;
    policy.origin_local_transaction_id=descriptor.origin_local_transaction_id;
    Sync();
    api::DurableAuthorizationState state;
    state.authority_uuid=policy.database_uuid;state.security_context_generation=3;
    state.security_epoch=7;state.policy_epoch=11;state.catalog_generation_id=13;
    context.database_uuid=policy.database_uuid;context.principal_uuid=Id(p::UuidKind::principal,81).value;
    context.security_context_present=true;context.security_epoch=7;context.catalog_generation_id=13;
    state.principals.push_back({context.principal_uuid,"principal",true,7});
    state.grants.push_back({Id(p::UuidKind::object,82).value,context.principal_uuid,"principal",
                           policy.metric_uuid,policy.read_right,false,true,7});
    api::DurableAuthorizationMaterializeRequest request;
    request.principal_uuid=context.principal_uuid;request.observed_security_epoch=7;
    request.observed_policy_epoch=11;request.observed_catalog_generation_id=13;
    auto auth=api::MaterializeDurableAuthorizationContext(state,request);
    Check(auth.ok,"real permission materializer rejected policy fixture");
    context.authorization_context=std::move(auth.context);
  }
  void Sync() {
    dm=Metadata(descriptor);pm=dm;
    pm.record.header.kind=c::CatalogRecordKind::policy;
    pm.record.header.object_uuid={p::UuidKind::object,policy.policy_uuid};
    pm.record.header.row_uuid=Id(p::UuidKind::row,83);
    pm.definition_version=policy.generation;pm.object_subtype="metric_visibility";
    pm.record.payload=PolicyGolden(policy);
  }
  api::LocalMetricVisibilityDecision Read() const {
    return api::EvaluateLocalMetricVisibility(context,dm,pm);
  }
};
void Refused(const VisibilityFixture& f) {
  const auto r=f.Read();
  Check(!r.read_allowed&&!r.sensitive_labels_allowed&&r.diagnostic=="METRIC.ACCESS_DENIED",
        "invalid or denied policy exposed access or existence detail");
}
void CodecChecks() {
  VisibilityFixture f;
  const auto golden=PolicyGolden(f.policy);
  const auto encoded=c::EncodeCatalogMetricVisibilityPolicy(f.policy);
  Check(encoded.ok()&&std::string(encoded.bytes.begin(),encoded.bytes.end())==golden,"visibility binary oracle");
  auto decoded=c::DecodeCatalogMetricVisibilityPolicy(golden);
  Check(decoded.ok()&&PolicyGolden(*decoded.record)==golden,"visibility exact decode");
  Check(c::EncodeCatalogMetadataVersion(f.pm).ok(),"visibility common metadata accepted");
  for(std::size_t n=0;n<golden.size();++n)
    Check(!c::DecodeCatalogMetricVisibilityPolicy(golden.substr(0,n)).ok(),"truncated policy accepted");
  Check(!c::DecodeCatalogMetricVisibilityPolicy(golden+"x").ok(),"trailing bytes accepted");
  for(unsigned id=1;id<=8;++id) {
    auto bytes=golden;const auto at=Offset(bytes,id);bytes.erase(at,8+Get(bytes,at+4,4));
    Put(bytes,8,bytes.size(),4);Put(bytes,12,7,4);
    Check(!c::DecodeCatalogMetricVisibilityPolicy(bytes).ok(),"missing field accepted");
    bytes=golden;Put(bytes,at,99,2);
    Check(!c::DecodeCatalogMetricVisibilityPolicy(bytes).ok(),"unknown field accepted");
    bytes=golden;bytes+=golden.substr(at,8+Get(golden,at+4,4));
    Put(bytes,8,bytes.size(),4);Put(bytes,12,9,4);
    Check(!c::DecodeCatalogMetricVisibilityPolicy(bytes).ok(),"duplicate field accepted");
  }
  auto wrong_schema=golden;Put(wrong_schema,20,2,2);
  Check(!c::DecodeCatalogMetricVisibilityPolicy(wrong_schema).ok(),"unknown schema version accepted");
  for(unsigned field:{1,3,4,7}) {
    Check(!c::DecodeCatalogMetricVisibilityPolicy(Replace(golden,field,std::string(36,'a'))).ok(),"text identity accepted");
    for(unsigned version=0;version<16;++version)if(version!=7) {
      auto bytes=golden;bytes[Offset(bytes,field)+8+6]=char(version<<4);
      Check(!c::DecodeCatalogMetricVisibilityPolicy(bytes).ok(),"non-v7 identity accepted");
    }
    auto bytes=golden;bytes[Offset(bytes,field)+8+8]=char(0x40);
    Check(!c::DecodeCatalogMetricVisibilityPolicy(bytes).ok(),"bad UUID variant accepted");
  }
  for(unsigned field:{2,8})Check(!c::DecodeCatalogMetricVisibilityPolicy(Replace(golden,field,Number(0))).ok(),"zero generation/origin");
  for(const auto& value:std::vector<std::string>{"", "lowercase", "A:B", " A", "1A", std::string(129,'A')})
    Check(!c::DecodeCatalogMetricVisibilityPolicy(Replace(golden,5,value)).ok(),"invalid right name accepted");
  Check(c::DecodeCatalogMetricVisibilityPolicy(Replace(golden,6,"")).ok(),"optional sensitive right refused");
  auto next=f.pm;next.definition_version++;auto successor=f.policy;successor.generation++;
  next.record.payload=PolicyGolden(successor);next.creator_local_transaction_id++;
  next.creator_transaction_uuid=Id(p::UuidKind::transaction,84);
  Check(c::CatalogMetadataPreservesFamilyOrigin(f.pm,next),"valid policy successor refused");
  successor.database_uuid=Id(p::UuidKind::database,85).value;next.record.payload=PolicyGolden(successor);
  Check(!c::CatalogMetadataPreservesFamilyOrigin(f.pm,next),"successor retargeted database");
  successor=f.policy;successor.generation++;successor.metric_uuid=Id(p::UuidKind::object,86).value;
  next.record.payload=PolicyGolden(successor);
  Check(!c::CatalogMetadataPreservesFamilyOrigin(f.pm,next),"successor retargeted metric");
  successor=f.policy;successor.generation++;successor.origin_transaction_uuid=next.creator_transaction_uuid;
  next.record.payload=PolicyGolden(successor);
  Check(!c::CatalogMetadataPreservesFamilyOrigin(f.pm,next),"successor rewrote origin");
  f.policy.generation=1;f.Sync();
  Check(c::EncodeCatalogMetadataVersion(f.pm).ok(),"first generation origin refused");
  f.pm.creator_transaction_uuid=Id(p::UuidKind::transaction,95);
  Check(!c::EncodeCatalogMetadataVersion(f.pm).ok(),"first generation origin substituted");
}
void PermissionChecks() {
  VisibilityFixture f;auto r=f.Read();
  Check(r.read_allowed&&!r.sensitive_labels_allowed&&r.diagnostic.empty(),"scoped read should redact sensitive labels");
  auto grant=f.context.authorization_context.grants.front();grant.grant_uuid=Id(p::UuidKind::object,87).value;grant.right="UNMASK";
  f.context.authorization_context.grants.push_back(grant);r=f.Read();
  Check(r.read_allowed&&r.sensitive_labels_allowed,"independent sensitive permission ignored");
  f.context.authorization_context.grants.back().deny=true;r=f.Read();
  Check(r.read_allowed&&!r.sensitive_labels_allowed,"sensitive deny must redact without refusing permitted read");
  f.context.authorization_context.grants.front().deny=true;Refused(f);
  f=VisibilityFixture();f.context.authorization_context.grants.clear();Refused(f);
  f=VisibilityFixture();f.context.authorization_context.grants.front().right="OBS_METRICS_READ_ALL";Refused(f);
  f=VisibilityFixture();f.context.authorization_context.grants.front().target_uuid=Id(p::UuidKind::object,88).value;Refused(f);
  f=VisibilityFixture();f.context.authorization_context.authority_uuid=Id(p::UuidKind::database,89).value;Refused(f);
  f=VisibilityFixture();f.context.security_epoch++;Refused(f);
  f=VisibilityFixture();f.context.catalog_generation_id++;Refused(f);
  f=VisibilityFixture();f.context.security_context_present=false;Refused(f);
  f=VisibilityFixture();f.context.authorization_context.present=false;Refused(f);
  f=VisibilityFixture();f.context.principal_uuid=Id(p::UuidKind::principal,90).value;Refused(f);
  f=VisibilityFixture();
  api::EngineMaterializedAuthorizationPolicy runtime;
  runtime.policy_uuid=Id(p::UuidKind::object,91).value;runtime.subject_uuid=f.context.principal_uuid;
  runtime.subject_kind="principal";runtime.target_uuid=f.policy.metric_uuid;runtime.right=f.policy.read_right;
  runtime.policy_epoch=11;runtime.requires_runtime_recheck=true;
  f.context.authorization_context.policies.push_back(runtime);Refused(f);
  f.context.authorization_context.policies.front().requires_runtime_recheck=false;
  f.context.authorization_context.policies.front().deny=true;Refused(f);
  f=VisibilityFixture();f.context.authorization_context.grants.push_back(grant);
  runtime.right="UNMASK";runtime.requires_runtime_recheck=true;
  f.context.authorization_context.policies.push_back(runtime);r=f.Read();
  Check(r.read_allowed&&!r.sensitive_labels_allowed,"pending sensitive recheck must redact");
  f=VisibilityFixture();f.policy.sensitive_read_right.clear();f.Sync();
  Check(f.Read().read_allowed&&!f.Read().sensitive_labels_allowed,"absent sensitive right not redacted");
  f=VisibilityFixture();f.policy.read_right="UNREGISTERED_RIGHT";f.descriptor.definition.security_family=f.policy.read_right;f.Sync();Refused(f);
  f=VisibilityFixture();f.policy.sensitive_read_right="UNREGISTERED_RIGHT";f.Sync();Refused(f);
  f=VisibilityFixture();f.policy.read_right="SELECT";f.Sync();Refused(f);
  f=VisibilityFixture();f.policy.database_uuid=Id(p::UuidKind::database,92).value;f.Sync();Refused(f);
  f=VisibilityFixture();f.policy.metric_uuid=Id(p::UuidKind::object,93).value;f.Sync();Refused(f);
  f=VisibilityFixture();f.policy.generation++;f.Sync();Refused(f);
  f=VisibilityFixture();f.policy.policy_uuid=Id(p::UuidKind::object,94).value;f.Sync();Refused(f);
  for(bool descriptor:{false,true}) {
    for(unsigned variant=0;variant<7;++variant) {
      f=VisibilityFixture();auto& row=descriptor?f.dm:f.pm;
      switch(variant) {
        case 0:row.record.header.deleted=true;break;
        case 1:row.authority_scope=c::CatalogAuthorityScope::cluster;break;
        case 2:row.record.header.row_uuid={};break;
        case 3:row.lifecycle=static_cast<c::CatalogObjectLifecycle>(0);break;
        case 4:row.status=static_cast<c::CatalogObjectStatus>(0);break;
        case 5:row.object_subtype="security_owner_fixture";row.record.payload=std::string(16,char(0xa5));break;
        case 6:row.definition_version++;break;
      }
      Refused(f);
    }
  }
}
void AllocationChecks() {
  VisibilityFixture f;bool complete=false;unsigned injected=0;
  for(long budget=0;budget<10000;++budget) {
    descriptor_allocation_fault::remaining=budget;descriptor_allocation_fault::fired=false;
    const auto r=f.Read();
    descriptor_allocation_fault::remaining=-1;
    if(descriptor_allocation_fault::fired){++injected;Check(!r.read_allowed&&!r.sensitive_labels_allowed,"allocation failure exposed partial permission");}
    else {Check(r.read_allowed&&!r.sensitive_labels_allowed,"permission failed after allocation retry");complete=true;break;}
  }
  Check(complete&&injected>10,"visibility allocation sweep incomplete");
}
}
int main(){CodecChecks();PermissionChecks();AllocationChecks();std::cout<<"metric visibility checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;}
