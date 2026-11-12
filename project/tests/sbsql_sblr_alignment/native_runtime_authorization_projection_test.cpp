// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Real native files, indexed/unindexed catalogs, admitted memory, and independent
// byte/I/O fault controls are deliberately reused, not replaced by mock grants.
#define SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
#include "native_runtime_security_observation_test.cpp"
#undef SB_NATIVE_RUNTIME_SECURITY_FIXTURE_WITHOUT_MAIN
#include "security/native_runtime_authorization_projection.hpp"

namespace {
namespace authorization=scratchbird::engine::internal_api;
using ProjectionError=authorization::NativeRuntimeAuthorizationError;
using ProjectionResult=authorization::NativeRuntimeAuthorizationResult;
using SubjectKind=authorization::NativeRuntimeAuthorizationSubjectKind;
constexpr authorization::NativeRuntimeAuthorizationLimits projection_limits{64,32,1000000};

ProjectionResult NoHeapProject(const SecurityResult& source,
    authorization::NativeRuntimeAuthorizationPreparedMemory& workspace) {
  allocation_budget=0;const auto result=authorization::ProjectNativeRuntimeAuthorization(source,workspace);
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining==0,"complete native graph projection/evaluation makes no ordinary heap allocation");
  return result;
}
void ProjectionEmpty(const ProjectionResult& result,ProjectionError error) {
  if(result.error!=error)std::cerr<<"projection error="<<int(result.error)<<" expected="<<int(error)
      <<" record="<<result.failing_record<<" work="<<result.work_performed<<'\n';
  Check(!result.ok()&&result.error==error&&!result.projection.present()&&result.projection.subjects().empty()&&
      result.projection.memberships().empty()&&result.projection.grants().empty()&&
      result.projection.engine_owned_bootstrap_role_uuid().is_nil()&&!result.projection.Allows(catalog::CatalogSecurityRight::CONNECT),
      "projection refusal exposes no usable partial closure/grants/bundle");
}

// In place, as the engine does it (W4b): the catalog root first (a committed
// transaction, then the post-commit advance to generation 2, security 4,
// schema 11, resource 17), then a real successor of the runtime binding born
// at that generation and epoch by a second committed transaction (never ahead
// of the root). The checkpoint-era fixture rewrote the binding's bytes under
// its existing version; a real successor takes that family's next generation
// (2, its definition_version), which the historical request therefore names.
// Security descriptors keep their original visibility generations.
void AdvanceNativeSelectedBindingEpochs(SecurityFixture& f) {
  f.SelectedCatalogEpochs(2,4,11,17);
  const auto roots=SecurityFixture::RootsOf(f.device);
  disk::FileDeviceCarefulWriteSink sink(f.device);
  page::TransactionInventoryWriter w(SecurityFixture::TxBinding(roots));Check(w.Restart(sink).ok(),"restart before the binding successor");
  page::TransactionHeaderRecord header;header.transaction_uuid=Id(15);
  const auto begun=w.Begin(sink,header);Check(begun.ok(),"in-place BEGIN of the binding change");
  const u64 n=*begun.transaction_order;
  const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(f.device,Id(1),f.receipt.relations[4]);
  Check(loaded.ok(),"actual configuration leaf before historical epoch fixture");
  const auto& leaf=*loaded.page;bool found=false;
  for(const auto& row:leaf.body.rows){auto metadata=loaded.metadata.at(row.version_uuid);
    if(metadata.record.header.object_uuid.value!=Id(180))continue;
    auto binding=catalog::DecodeCatalogRuntimeAuthorityBinding(metadata.record.payload);Check(binding.ok(),"actual persisted binding decoded for fixture");
    binding.record->generation=2;binding.record->security_epoch=4;binding.record->catalog_generation=2;
    const auto body=catalog::EncodeCatalogRuntimeAuthorityBinding(*binding.record);Check(body.ok(),"exact advanced selected epochs remain native u64");
    metadata.definition_version=2;metadata.security_epoch=4;metadata.catalog_generation=2;
    metadata.creator_transaction_uuid={platform::UuidKind::transaction,Id(15)};metadata.creator_local_transaction_id=n;
    metadata.record.payload.assign(body.bytes.begin(),body.bytes.end());
    // W3b-2b: the transaction's catalog.metadata_change event, which the successor cites.
    Check(scratchbird::tests::catalog_change_event::WriteEvent(sink,SecurityFixture::CvBinding(roots),n,Id(15),security_fixture::Id(9794),security_fixture::Id(9795),
        security_fixture::Id(9796),security_fixture::Id(9790),1789357073001000ULL).ok(),"the binding change's catalog change event");
    metadata.audit_uuid={platform::UuidKind::object,security_fixture::Id(9794)};
    db::CatalogVersionRequest r;r.role=5;r.transaction_order=n;r.transaction_uuid=Id(15);r.row_uuid=row.row_uuid.value;
    r.predecessor_version_uuid=row.version_uuid;r.version_uuid=Id(16);r.metadata=metadata;
    const auto written=db::WriteCatalogSuccessorVersion(sink,SecurityFixture::CvBinding(roots),r);
    if(!written.ok())std::cerr<<"binding successor="<<db::CatalogVersionWriteErrorName(written.error)<<'\n';
    Check(written.ok(),"the binding successor written in place at the advanced generation");found=true;break;
  }
  Check(found,"actual bound configuration identity changed, not synthesized observation");
  Check(w.Commit(sink,n,1789357074000000000ULL).ok(),"the binding change commits in place");
}
void RealNativeProjection(unsigned profile,bool indexed,bool historical=false) {
  SecurityFixture f(profile,0,indexed,historical);  // W3b-2b: audited where it writes
  if(historical)AdvanceNativeSelectedBindingEpochs(f);
  const db::NativeCatalogRelationMemoryLimits limits{Profile(profile),1,16,64,64,{4,16*static_cast<std::size_t>(sizes[profile])}};
  const auto amount=runtime_security::NativeRuntimeSecurityWorkspaceBytes(limits);
  const auto projection_bytes=authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits);
  Check(projection_bytes>0,"checked actual graph/vocabulary/schema workspace extent");
  LeafMemoryFixture memory(amount),source_memory(32*1024*1024),projection_memory(projection_bytes);
  auto prepared=runtime_security::PrepareNativeRuntimeSecurityRead(limits,memory.memory,memory.binding);
  auto graph=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,projection_memory.memory,projection_memory.binding);
  Check(prepared.ok()&&graph.ok(),"all actual native arenas prepared BEFORE retained source/device guards");
  const std::array<disk::NativeFilespaceDevice,1> devices{{{Id(2),Profile(profile),&f.device}}};
  auto source=db::AcquireNativeFixedRootMemoryLease(Id(1),devices,Id(2),{f.budget,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual reopened native fixed-root lease over the in-place inventory");
  auto request=f.Request();if(historical)request.runtime_binding_generation=2;
  allocation_budget=0;const auto observed=runtime_security::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease,request,prepared.workspace);
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining==0&&observed.ok(),"complete actual role4/role5 read with no ordinary heap");
  const auto before_write=stage_writes,before_sync=stage_syncs;
  const auto result=NoHeapProject(observed,graph.workspace);
  if(!result.ok())std::cerr<<"real projection error="<<int(result.error)<<" record="<<result.failing_record<<'\n';
  Check(result.ok()&&result.projection.subjects().size()==2&&result.projection.memberships().size()==1&&
      result.projection.grants().size()==2,"real native direct/inherited/denied cohort fully projected");
  const auto& p=result.projection;
  Check(p.binding().database_uuid==Id(1)&&p.binding().security_epoch==(historical?4:1)&&p.binding().policy_epoch==31&&p.binding().catalog_generation==(historical?2:1)&&
      p.principal().security_context_generation==9&&p.principal().security_generation==11&&p.principal().policy_generation==13,
      "all real configuration and independent service event generations preserved without clamping/equating");
  if(historical)Check(p.grants()[0].security_epoch==4&&p.grants()[0].source_security_epoch==1&&p.grants()[0].catalog_generation==1&&
      p.memberships()[0].security_epoch==4&&p.memberships()[0].source_security_epoch==1&&p.subjects()[0].catalog_generation==1,
      "real historical native versions remain effective under newer actual selected epochs with unchanged provenance");
  allocation_budget=0;
  // 3a-2: rights are coding-time constants; "not a right" is none or a value
  // outside the listed constants (was the unregistered text "NOT_A_RIGHT").
  using RT=catalog::CatalogSecurityRight;
  const bool allow=p.Allows(RT::CONNECT),target_deny=!p.Allows(RT::SELECT,Id(240)),other_deny=!p.Allows(RT::SELECT,Id(241)),
      unknown_deny=!p.Allows(RT::none)&&!p.Allows(static_cast<RT>(999));
  const auto evaluation_budget=allocation_budget;allocation_budget=-1;
  Check(evaluation_budget==0&&allow&&target_deny&&other_deny&&unknown_deny,"base decision is allocation-free and defaults deny");
  Check(p.engine_owned_bootstrap_role_uuid().is_nil(),"ordinary provisioned service is not elevated by name or absent canonical membership");
  Check(stage_writes==before_write&&stage_syncs==before_sync,"projection never publishes/syncs or mutates native storage");
  // A source failure retains its complete actual typed I/O/MGA failure and no
  // previously successful graph prefix, even when the workspace is reused.
  reads=0;read_fault=1;track_reads=true;
  const auto failed_source=runtime_security::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease,request,prepared.workspace);
  track_reads=false;Check(!read_fault&&!failed_source.ok(),"actual native source I/O fault consumed");
  const auto failed=NoHeapProject(failed_source,graph.workspace);ProjectionEmpty(failed,ProjectionError::source_failure);
  Check(failed.source_error==failed_source.error&&failed.catalog_error==failed_source.source_error&&
      failed.binding_error==failed_source.binding_error&&failed.relation_error==failed_source.relation_error&&
      failed.io_failure&&failed.io_failure->native_error==EIO&&failed.io_failure->native_attempted&&
      failed.io_failure_filespace_uuid==Id(2),"projection retains native failure dimensions, never invents a diagnostic");
  // No output references source credential/name payloads. Storage owner remains
  // retained explicitly until borrowed projection views are no longer used.
  source.lease.Reset();prepared.workspace={};graph.workspace={};
  memory.memory={};source_memory.memory={};projection_memory.memory={};
  memory.Empty();source_memory.Empty();projection_memory.Empty();
  Check(f.device.Close().ok(),"actual native projection closes after source/backing release");f.artifacts.Cleanup();
}

// Graph semantics are fed through the actual typed native codecs (3a-2) and
// complete common metadata, not caller-constructed preflattened effective
// grants. They remain explicitly DATA fixtures, never an authenticated
// service/admission proof.
//
// 3a-2: typed records. The graph is the attribute-bag graph unchanged:
// principal->group 202, group 202->role 206, role 206->group 207, group
// 207->role 208 and the group 202->role 208 short path, with grants to the
// groups. (An interim roles-only rebuild was reverted after the coordinator's
// 2026-11-12 correction: memberships into groups are writable.)
using SK=catalog::CatalogSecuritySubjectKind;
using RT=catalog::CatalogSecurityRight;
std::string_view RightTag(RT right){
  for(const auto& r:catalog::kCatalogSecurityRights)if(r.right==right)return r.tag;
  return {};
}
std::string KindText(SK k){return k==SK::principal?"principal":k==SK::role?"role":k==SK::group?"group":"invalid";}
struct GraphData {
  using Record=SecurityRow;
  std::vector<Record> rows;
  std::vector<Bytes> payloads;
  std::vector<authorization::NativeRuntimeSecurityRecordObservation> records;
  SecurityResult observation;
  static Record Principal(){auto r=SecurityRecord(catalog::CatalogRecordKind::user_account,200);
    AsPrincipal(r).principal_kind=catalog::CatalogSecurityPrincipalKind::service;AsPrincipal(r).bootstrap_principal=false;return r;}
  static Record Group(unsigned id){return SecurityRecord(catalog::CatalogRecordKind::group_account,id);}
  static Record Role(unsigned id){return SecurityRecord(catalog::CatalogRecordKind::role_account,id);}
  // As the attribute-bag fixture: a role parent is a role_membership, a
  // group parent a group_membership (the class is a legacy label; the kinds
  // decide the edge).
  static Record Member(unsigned id,unsigned member,SK kind,unsigned parent,SK parent_kind=SK::role){
    auto r=SecurityRecord(catalog::CatalogRecordKind::grant_record,id);auto& g=AsGrant(r);
    g.member_uuid=Id(member);g.parent_uuid=Id(parent);g.member_kind=kind;g.parent_kind=parent_kind;
    g.grant_class=parent_kind==SK::role?catalog::CatalogSecurityGrantClass::role_membership:catalog::CatalogSecurityGrantClass::group_membership;
    return r;
  }
  static Record Grant(unsigned id,unsigned subject,SK kind,RT right,bool deny=false,unsigned target=0){
    auto r=SecurityRecord(catalog::CatalogRecordKind::grant_record,id);auto& g=AsGrant(r);g.subject_uuid=Id(subject);
    if(target)g.target_uuid=Id(target);
    g.grant_class=catalog::CatalogSecurityGrantClass::privilege;g.subject_kind=kind;g.right=right;
    g.effect=deny?catalog::CatalogSecurityEffect::deny:catalog::CatalogSecurityEffect::allow;return r;
  }
  GraphData(){
    rows={Principal(),Group(202),Role(206),Group(207),Role(208),Role(209),
        Member(210,200,SK::principal,202,SK::group),Member(211,202,SK::group,206),
        Member(212,206,SK::role,207,SK::group),Member(213,207,SK::group,208),Member(214,202,SK::group,208),
        Grant(220,200,SK::principal,RT::CONNECT),Grant(221,208,SK::role,RT::SELECT,false,240),
        Grant(222,207,SK::group,RT::SELECT,true,241),Grant(223,202,SK::group,RT::UPDATE),Grant(224,206,SK::role,RT::UPDATE,true,240)};
    AsRole(rows[5]).active=false;
    Build();
  }
  void Build(){
    payloads.clear();records.clear();payloads.reserve(rows.size());records.reserve(rows.size());
    for(std::size_t n=0;n<rows.size();++n){
      auto metadata=SecurityMetadata(rows[n],n).metadata;
      Check(!catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(metadata)),"genuine graph common metadata framing");
      const auto encoded=EncodeSecurityRow(rows[n]);Check(encoded.ok(),"genuine graph native typed codec");payloads.push_back(encoded.bytes);
      const auto& bytes=payloads.back();const auto decoded=catalog::DecodeNativeSecurityRecord(rows[n].kind,
          {reinterpret_cast<const char*>(bytes.data()),bytes.size()});Check(decoded.ok(),"genuine graph native typed record");
      authorization::NativeRuntimeSecurityRecordObservation r;r.record=*decoded.record;
      r.object_uuid=metadata.record.header.object_uuid.value;r.row_uuid=metadata.record.header.row_uuid.value;
      r.version_uuid=security_fixture::Id(10000+n);r.origin_transaction_uuid=security_tx.value;r.creator_transaction_uuid=security_tx.value;
      r.origin_local_transaction_id=r.creator_local_transaction_id=1;r.definition_generation=1;
      r.catalog_generation=r.security_epoch=1;r.lifecycle=metadata.lifecycle;r.status=metadata.status;records.push_back(r);
    }
    catalog::CatalogRuntimeAuthorityBinding b;b.binding_uuid=Id(180);b.database_uuid=b.security_authority_uuid=Id(1);b.service_principal_uuid=Id(200);
    b.provider_uuid=Id(210);b.policy_uuid=Id(52);b.credential_reference_uuid=Id(211);b.generation=b.security_epoch=b.catalog_generation=1;
    b.policy_epoch=31;b.provider_generation=37;b.authority_mode=catalog::RuntimeAuthorityMode::database_local;
    b.origin_transaction_uuid=security_tx;b.origin_local_transaction_id=1;
    observation={};observation.error=SecurityError::none;observation.binding_error=authorization::RuntimeAuthorityBindingReadError::none;
    observation.source_error=db::NativePinnedCatalogReadError::none;observation.relation_error=db::NativeCatalogRelationLeaseError::none;
    observation.binding=b;observation.binding_version_uuid=Id(250);
    const auto selected=std::find_if(records.begin(),records.end(),[](const auto& r){return r.object_uuid==Id(200);});
    Check(selected!=records.end(),"graph owns selected actual service record");
    observation.principal=authorization::NativeRuntimeServicePrincipalObservation{Id(200),selected->version_uuid,1,9,11,13};
    observation.records=records;
  }
};
std::string KindName(SubjectKind k){return k==SubjectKind::principal?"principal":k==SubjectKind::role?"role":k==SubjectKind::group?"group":"invalid";}
// The existing owning (text-vocabulary) materializer is the parity oracle; the
// typed rows are rendered into its input here, in the test only.
authorization::DurableAuthorizationState LegacyGraph(const GraphData& graph){
  authorization::DurableAuthorizationState s;const auto& b=*graph.observation.binding;
  s.authority_uuid=b.security_authority_uuid;s.security_context_generation=graph.observation.principal->security_context_generation;
  s.security_epoch=b.security_epoch;s.policy_epoch=b.policy_epoch;s.catalog_generation_id=b.catalog_generation;
  for(std::size_t n=0;n<graph.rows.size();++n){const auto& row=graph.rows[n];const auto& native=graph.records[n];
    const bool flag=row.principal()?row.principal()->active:row.role()?row.role()->active:true;
    const bool active=!native.deleted&&native.lifecycle==catalog::CatalogObjectLifecycle::active&&native.status==catalog::CatalogObjectStatus::active&&flag;
    const auto id=row.primary();
    if(row.kind==catalog::CatalogRecordKind::user_account)s.principals.push_back({id,"principal",active,s.security_epoch});
    else if(row.kind==catalog::CatalogRecordKind::role_account){s.roles.push_back({id,active,s.security_epoch});
      const auto& role=*row.role();
      if(id==db::kCanonicalSysarchRoleIdentity&&role.engine_owned&&role.immutable&&role.authority_class==catalog::CatalogSecurityAuthorityClass::engine_owned_sysarch)
        s.engine_owned_sysarch_role_uuid=id;}
    else if(row.kind==catalog::CatalogRecordKind::group_account)s.groups.push_back({id,active,s.security_epoch});
    else if(const auto& g=*row.grant();g.grant_class==catalog::CatalogSecurityGrantClass::privilege)
      s.grants.push_back({id,*g.subject_uuid,KindText(*g.subject_kind),g.target_uuid.value_or(Uuid{}),std::string(RightTag(*g.right)),
          g.effect==catalog::CatalogSecurityEffect::deny,active,s.security_epoch});
    else s.memberships.push_back({*g.member_uuid,KindText(*g.member_kind),*g.parent_uuid,KindText(*g.parent_kind),active,s.security_epoch});
  }return s;
}
void Parity(const GraphData& data,const ProjectionResult& result){
  auto state=LegacyGraph(data);const auto legacy=authorization::MaterializeDurableAuthorizationContext(state,{Id(200),state.security_epoch,state.policy_epoch,state.catalog_generation_id});
  Check(legacy.ok&&result.ok(),"native and existing owning materializers both accept genuine graph");
  Check(legacy.context.effective_subjects.size()==result.projection.subjects().size()&&legacy.context.grants.size()==result.projection.grants().size(),"exact closure/grant cardinality parity");
  for(const auto& subject:result.projection.subjects())Check(std::any_of(legacy.context.effective_subjects.begin(),legacy.context.effective_subjects.end(),[&](const auto& old){return old.subject_uuid==subject.subject_uuid&&old.subject_kind==KindName(subject.kind);}),"exact binary typed subject parity");
  for(const auto& grant:result.projection.grants())Check(std::any_of(legacy.context.grants.begin(),legacy.context.grants.end(),[&](const auto& old){return old.grant_uuid==grant.grant_uuid&&old.subject_uuid==grant.subject_uuid&&old.subject_kind==KindName(grant.subject_kind)&&old.target_uuid==grant.target_uuid&&old.right==RightTag(grant.right)&&old.deny==grant.deny&&old.security_epoch==grant.security_epoch;}),"exact durable binary grant/effect/target parity");
  authorization::EngineRequestContext request;request.security_context_present=true;request.principal_uuid=Id(200);request.security_epoch=state.security_epoch;request.catalog_generation_id=state.catalog_generation_id;
  for(const auto& right:authorization::KnownSecurityRights())for(const auto target:{Uuid{},Id(240),Id(241),Id(242)}){
    const auto old=authorization::EvaluateMaterializedAuthorization(request,legacy.context,right,target);
    const auto constant=catalog::CatalogSecurityRightForTag(right);
    Check(constant.has_value(),"every registered right has its constant");
    Check(old.authorized==result.projection.Allows(*constant,target),"all registered rights/global/target explicit-deny parity");
  }
}
// The native creation translator writes a pack group's link to its role as a
// group_membership (group->role). In the mixed graph that edge (211, group
// 202 -> role 206) is reached through the principal's membership in group
// 202. 3a-2 (typed records, coordinator correction 2026-11-12: the kinds
// decide the edge): a principal parent, a deny effect and a missing parent
// cannot be encoded, and an observation carrying one anyway is refused by the
// projection's codec re-validation as invalid_record (was
// malformed_membership); a principal member of a group_membership is now a
// legal record whose member endpoint (202 is a group, not a principal) does
// not resolve, so the edge is dropped (was malformed_membership); a group
// parent that is really a role still refuses as subject_missing.
void ImportedGroupRoleMembership(){
  LeafMemoryFixture memory(authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits));
  auto prepared=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,memory.binding);
  Check(prepared.ok(),"imported group-to-role workspace admitted before projection");
  GraphData graph;
  const auto found=std::find_if(graph.rows.begin(),graph.rows.end(),[](const auto& row){
    return row.grant()&&row.grant()->member_kind==SK::group&&row.grant()->parent_kind==SK::role;
  });
  Check(found!=graph.rows.end(),"actual typed group-to-role edge in mixed graph");
  const auto at=static_cast<std::size_t>(found-graph.rows.begin());
  // Match the actual native creation translator's durable grant class.
  AsGrant(graph.rows[at]).grant_class=catalog::CatalogSecurityGrantClass::group_membership;graph.Build();
  auto projected=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,projected);
  Check(projected.ok()&&projected.projection.subjects().size()==5&&projected.projection.memberships().size()==5,
      "imported group membership retains its actual role parent and exact transitive closure");
  const auto original=graph.rows[at];
  for(unsigned mode=0;mode<5;++mode){
    const auto mutate=[&](catalog::NativeSecurityGrant& g){
      if(mode==0)g.parent_kind=SK::principal;
      if(mode==1)g.member_kind=SK::principal;
      if(mode==2)g.effect=catalog::CatalogSecurityEffect::deny;
      if(mode==3)g.parent_kind=SK::group;
      if(mode==4)g.parent_uuid.reset();
    };
    mutate(AsGrant(graph.rows[at]));
    const bool encodable=EncodeSecurityRow(graph.rows[at]).ok();
    Check(encodable==(mode==1||mode==3),"the typed codec refuses exactly the shapes it cannot represent");
    if(encodable)graph.Build();
    else{graph.rows[at]=original;graph.Build();mutate(std::get<catalog::NativeSecurityGrant>(graph.records[at].record.value));}
    const auto result=NoHeapProject(graph.observation,prepared.workspace);
    // Without edge 211, role 206 and group 207 are unreachable: 200 -> 202 ->
    // 208 remains (subjects 200, 202, 208; memberships 210, 214).
    if(mode==1)Check(result.ok()&&result.projection.memberships().size()==2&&result.projection.subjects().size()==3,
        "an unresolved member endpoint adds no edge");
    else ProjectionEmpty(result,mode==3?ProjectionError::subject_missing:ProjectionError::invalid_record);
    graph.rows[at]=original;graph.Build();
  }
  projected=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,projected);
  prepared.workspace={};memory.memory={};memory.Empty();
}
// Coordinator correction 2026-11-12: memberships into groups. A principal in
// the PUBLIC-style group 185, that group a member of group 186, group 186 a
// member of role 187 (fixture Id() holds one byte, so ids stay below 256). Grants to each group and the role reach the principal;
// the group-parent edges are exactly the projection's group-parent branch.
void GroupChains(){
  LeafMemoryFixture memory(authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits));
  auto prepared=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,memory.binding);
  Check(prepared.ok(),"group chain workspace admitted");
  GraphData graph;
  graph.rows={GraphData::Principal(),GraphData::Group(185),GraphData::Group(186),GraphData::Role(187),
      GraphData::Member(188,200,SK::principal,185,SK::group),GraphData::Member(189,185,SK::group,186,SK::group),
      GraphData::Member(190,186,SK::group,187),
      GraphData::Grant(191,185,SK::group,RT::CONNECT),GraphData::Grant(192,186,SK::group,RT::SELECT,false,240),
      GraphData::Grant(193,187,SK::role,RT::UPDATE),GraphData::Grant(194,186,SK::group,RT::DELETE,true,241)};
  graph.Build();
  auto result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(result.ok()&&result.projection.subjects().size()==4&&result.projection.memberships().size()==3&&
      result.projection.grants().size()==4&&result.projection.maximum_membership_depth()==4,
      "principal->group, group->group and group->role closure with every grant reaching the principal");
  Check(result.projection.Allows(RT::CONNECT)&&result.projection.Allows(RT::SELECT,Id(240))&&!result.projection.Allows(RT::SELECT,Id(241))&&
      result.projection.Allows(RT::UPDATE)&&!result.projection.Allows(RT::DELETE,Id(241))&&!result.projection.Allows(RT::DELETE),
      "a PUBLIC-style group grant, an inherited group grant and a group deny take effect for the member principal");
  // An inactive intermediate group parent refuses as a missing subject.
  graph.records[2].status=catalog::CatalogObjectStatus::disabled_by_policy;
  ProjectionEmpty(NoHeapProject(graph.observation,prepared.workspace),ProjectionError::subject_missing);graph.Build();
  // A group cycle through the group-parent branch is refused.
  graph.rows.push_back(GraphData::Member(195,186,SK::group,185,SK::group));graph.Build();
  ProjectionEmpty(NoHeapProject(graph.observation,prepared.workspace),ProjectionError::membership_cycle);
  prepared.workspace={};memory.memory={};memory.Empty();
}
void GraphSemantics(){
  const auto bytes=authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits);LeafMemoryFixture memory(bytes);
  auto prepared=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,memory.binding);Check(prepared.ok(),"governed graph preparation");
  GraphData graph;const auto original_payloads=graph.payloads;
  auto result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(graph.payloads==original_payloads,"native security/credential attribute payload bytes remain immutable during projection");
  Check(result.projection.subjects().size()==5&&result.projection.memberships().size()==5&&result.projection.grants().size()==5,
      "transitive mixed group-role diamond is deduplicated without discarding actual membership lineage");
  Check(result.projection.Allows(RT::CONNECT)&&result.projection.Allows(RT::SELECT,Id(240))&&!result.projection.Allows(RT::SELECT,Id(241))&&
      !result.projection.Allows(RT::SELECT,Id(242))&&result.projection.Allows(RT::UPDATE,Id(242))&&!result.projection.Allows(RT::UPDATE,Id(240)),
      "global grants, exact scoped grants and inherited denies have actual effects");
  graph.observation.binding->security_epoch=4;graph.observation.binding->catalog_generation=7;
  graph.records[2].security_epoch=2;graph.records[2].catalog_generation=3;
  result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(result.projection.grants()[0].security_epoch==4&&result.projection.grants()[0].source_security_epoch==1&&
      std::any_of(result.projection.subjects().begin(),result.projection.subjects().end(),[](const auto& s){return s.subject_uuid==Id(206)&&s.security_epoch==2&&s.catalog_generation==3;}),
      "mixed historical descriptors preserve provenance while selected context epochs remain exact");
  graph.Build();
  std::reverse(graph.rows.begin(),graph.rows.end());graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  // Duplicate identity must refuse regardless of physical row order.
  graph.rows.push_back(graph.rows.back());graph.Build();ProjectionEmpty(NoHeapProject(graph.observation,prepared.workspace),ProjectionError::duplicate_identity);
  graph=GraphData{};graph.Build();
  const auto reject=[&](ProjectionError expected){ProjectionEmpty(NoHeapProject(graph.observation,prepared.workspace),expected);};
  graph.rows.push_back(GraphData::Member(225,208,SK::role,202,SK::group));graph.Build();reject(ProjectionError::membership_cycle);
  auto legacy_cycle=authorization::MaterializeDurableAuthorizationContext(LegacyGraph(graph),{Id(200),1,31,1});Check(!legacy_cycle.ok,"existing materializer also rejects actual reachable cycle");
  // 3a-2: the typed codec refuses a self membership (member == parent), so the
  // unreachable inactive cycle is a two-role cycle through inactive role 209.
  graph.rows.pop_back();Check(!EncodeSecurityRow(GraphData::Member(225,209,SK::role,209)).ok(),"the typed codec refuses a self membership");
  graph.rows.push_back(GraphData::Role(226));graph.rows.push_back(GraphData::Member(225,209,SK::role,226));
  graph.rows.push_back(GraphData::Member(227,226,SK::role,209));graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(result.projection.subjects().size()==5,"unreachable inactive cycle grants nothing and does not deny unrelated principal");
  graph.rows.resize(graph.rows.size()-3);AsRole(graph.rows[2]).active=false;graph.Build();reject(ProjectionError::subject_missing);
  // 3a-2: a role's active flag is a required typed field, so "omitted" cannot
  // be written; the attribute-bag created_disabled flag has no typed field.
  AsRole(graph.rows[2]).active=true;graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Check(result.ok(),"an active role projects");
  for(unsigned mode=0;mode<3;++mode){auto& r=graph.records[2];const auto saved=r;
    if(mode==0)r.deleted=true;else if(mode==1){r.lifecycle=catalog::CatalogObjectLifecycle::archived;r.status=catalog::CatalogObjectStatus::retired;r.retired_transaction_uuid=security_tx.value;r.retired_local_transaction_id=1;}
    else r.status=catalog::CatalogObjectStatus::disabled_by_policy;
    reject(ProjectionError::subject_missing);r=saved;
  }
  graph.records[2].security_epoch=2;reject(ProjectionError::generation_mismatch);graph.records[2].security_epoch=1;
  graph.records[2].catalog_generation=2;reject(ProjectionError::generation_mismatch);graph.records[2].catalog_generation=1;
  graph.observation.principal->security_generation=12;reject(ProjectionError::principal_invalid);graph.observation.principal->security_generation=11;
  auto saved_binding=*graph.observation.binding;
  graph.observation.binding->database_uuid=Id(242);reject(ProjectionError::invalid_binding);graph.observation.binding=saved_binding;
  graph.observation.binding->authority_mode=catalog::RuntimeAuthorityMode::cluster;reject(ProjectionError::invalid_binding);graph.observation.binding=saved_binding;
  graph.observation.binding->provider_generation=0;reject(ProjectionError::invalid_binding);graph.observation.binding=saved_binding;
  {
    {auto& p=AsPrincipal(graph.rows[0]);p.security_context_generation=p.security_generation=p.policy_generation=std::numeric_limits<u64>::max();}
    graph.Build();graph.observation.principal->security_context_generation=graph.observation.principal->security_generation=
        graph.observation.principal->policy_generation=std::numeric_limits<u64>::max();
    graph.observation.binding->security_epoch=graph.observation.binding->policy_epoch=graph.observation.binding->provider_generation=
        graph.observation.binding->catalog_generation=std::numeric_limits<u64>::max();
    result=NoHeapProject(graph.observation,prepared.workspace);
    Check(result.ok()&&result.projection.principal().security_context_generation==std::numeric_limits<u64>::max()&&
        result.projection.grants()[0].security_epoch==std::numeric_limits<u64>::max()&&result.projection.grants()[0].source_security_epoch==1,
        "exact u64 generation maxima preserve domains without signed truncation/wrap");
    graph.rows[0]=GraphData::Principal();graph.Build();
  }
  graph.records[0].version_uuid={};reject(ProjectionError::invalid_record);graph.Build();
  graph.records[0].origin_local_transaction_id=2;reject(ProjectionError::invalid_record);graph.Build();
  // 3a-2: a deny effect on a membership, an unknown effect and an unknown right
  // cannot be encoded by the typed codec; an observation carrying one anyway
  // is refused by the projection's codec re-validation as invalid_record (was
  // malformed_membership, malformed_grant and unknown_right respectively).
  Check(!EncodeSecurityRow([&]{auto r=graph.rows[6];AsGrant(r).effect=catalog::CatalogSecurityEffect::deny;return r;}()).ok(),
      "the typed codec refuses a deny membership");
  std::get<catalog::NativeSecurityGrant>(graph.records[6].record.value).effect=catalog::CatalogSecurityEffect::deny;
  reject(ProjectionError::invalid_record);graph.Build();
  AsGrant(graph.rows[6]).principal_uuid=Id(202);graph.Build();reject(ProjectionError::malformed_membership);AsGrant(graph.rows[6]).principal_uuid.reset();graph.Build();
  std::get<catalog::NativeSecurityGrant>(graph.records[11].record.value).effect=static_cast<catalog::CatalogSecurityEffect>(9);
  reject(ProjectionError::invalid_record);graph.Build();
  Check(!catalog::CatalogSecurityRightForTag("MADE_UP_PERMISSION"),"an unknown right tag has no constant");
  std::get<catalog::NativeSecurityGrant>(graph.records[11].record.value).right=static_cast<RT>(999);
  reject(ProjectionError::invalid_record);graph.Build();
  // 3a-2: typed fields cannot carry malformed text (the attribute-bag checks of
  // an active flag spelled as text and of invalid UTF-8 are gone with the
  // text, as is the borrowed-text overlap check). A record whose kind tag
  // disagrees with its value, or whose primary identity is not its object, is
  // refused.
  graph.records[5].record.kind=catalog::CatalogRecordKind::group_account;reject(ProjectionError::invalid_record);graph.Build();
  std::get<catalog::NativeSecurityRole>(graph.records[5].record.value).role_uuid=Id(241);reject(ProjectionError::invalid_record);graph.Build();
  std::get<catalog::NativeSecurityPrincipal>(graph.records[0].record.value).principal_uuid=Id(201);reject(ProjectionError::invalid_record);graph.Build();
  graph.records[0].row_uuid=graph.records[0].object_uuid;reject(ProjectionError::invalid_record);graph.Build();
  // 3a-2: a typed grant has no active flag; an inactive grant is one whose
  // version is not active (here disabled by policy).
  graph.records[11].status=catalog::CatalogObjectStatus::disabled_by_policy;result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(!result.projection.Allows(RT::CONNECT),"retired/inactive grants cannot authorize");graph.Build();
  // Caller attributes on a different role identity never attach the
  // engine-owned bootstrap bundle (3a-2: there is no role_code or role_name to
  // forge; the role UUID alone decides).
  {auto& r=AsRole(graph.rows[4]);r.engine_owned=true;r.immutable=true;r.authority_class=catalog::CatalogSecurityAuthorityClass::engine_owned_sysarch;}
  graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Check(result.ok()&&result.projection.engine_owned_bootstrap_role_uuid().is_nil()&&!result.projection.Allows(RT::DELETE),"forged display bootstrap role cannot elevate");
  AsRole(graph.rows[4]).role_uuid=db::kCanonicalSysarchRoleIdentity;AsRole(graph.rows[4]).create_time_only=true;
  for(auto& r:graph.rows)if(r.grant()&&r.grant()->parent_uuid==Id(208))AsGrant(r).parent_uuid=db::kCanonicalSysarchRoleIdentity;
  graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(result.projection.engine_owned_bootstrap_role_uuid()==db::kCanonicalSysarchRoleIdentity&&result.projection.Allows(RT::DELETE)&&
      !result.projection.Allows(RT::UPDATE,Id(240)),"actual canonical immutable reachable role bundle still obeys explicit denial");
  Check(!result.projection.Allows(RT::DELETE,{},false)&&result.projection.Allows(RT::UPDATE,{},false)&&
      !result.projection.Allows(RT::UPDATE,Id(240),false),
      "a selected policy can disable the bootstrap bundle while retaining actual explicit grants and target denies");
  AsRole(graph.rows[4]).immutable=false;graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);
  ProjectionEmpty(result,ProjectionError::invalid_record);
  prepared.workspace={};memory.memory={};memory.Empty();
}

void ProjectionBounds(){
  const auto bytes=authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits);Check(bytes>1,"positive bounded arena query");
  LeafMemoryFixture memory(bytes);GraphData graph;
  graph.rows.erase(graph.rows.begin()+10);graph.Build(); // unambiguous five-frame chain, no diamond short path
  const auto prepare=[&](authorization::NativeRuntimeAuthorizationLimits l){return authorization::PrepareNativeRuntimeAuthorizationProjection(l,memory.memory,memory.binding);};
  {auto prepared=prepare(projection_limits);Check(prepared.ok(),"bounded prepared arena");
    auto result=NoHeapProject(graph.observation,prepared.workspace);Check(result.ok(),"bounded successful projection");
    Check(result.projection.source_record_count()==graph.records.size()&&
        result.projection.maximum_membership_depth()==5&&result.projection.work_performed()==result.work_performed,
        "private projection retains actual source size, longest membership path and evaluated work");
    const authorization::NativeRuntimeAuthorizationLimits measured{graph.records.size(),5,result.work_performed};
    Check(result.projection.WithinLimits(measured),"exact selected policy bounds admit the actual evaluated closure");
    auto short_limit=measured;--short_limit.maximum_records;
    Check(!result.projection.WithinLimits(short_limit),"selected record bound applies to the complete source, not just reachable subjects");
    short_limit=measured;--short_limit.maximum_depth;
    Check(!result.projection.WithinLimits(short_limit),"selected depth bound applies to the actual longest membership path");
    short_limit=measured;--short_limit.maximum_work;
    Check(!result.projection.WithinLimits(short_limit),"selected work bound cannot be replaced by bootstrap workspace capacity");
    const auto exact_work=result.work_performed;prepared.workspace={};
    auto limited=projection_limits;limited.maximum_work=exact_work;prepared=prepare(limited);
    Check(prepared.ok()&&NoHeapProject(graph.observation,prepared.workspace).ok(),"exact measured work boundary accepted without off-by-one");
    prepared.workspace={};limited.maximum_work=exact_work-1;prepared=prepare(limited);Check(prepared.ok(),"one short work preparation");
    ProjectionEmpty(NoHeapProject(graph.observation,prepared.workspace),ProjectionError::work_limit);
  }
  {
    // 3a-2: a right is a coding-time constant, so there is no vocabulary scan
    // and no text inspection to charge: the first and the last listed rights
    // cost the same work (was: every candidate comparison plus text bytes).
    const auto& rights=catalog::kCatalogSecurityRights;
    Check(rights.size()>1,"actual listed rights have distinct first/last positions");
    const auto first=rights.front().right,last=rights.back().right;
    // This is a separately measured full valid projection, not an internal
    // selector oracle. Only one genuine encoded grant's right changes.
    auto p=prepare(projection_limits);Check(p.ok(),"vocabulary accounting preparation");
    AsGrant(graph.rows[10]).right=first;graph.Build();
    const auto first_result=NoHeapProject(graph.observation,p.workspace);Check(first_result.ok(),"first actual registered right projects");
    AsGrant(graph.rows[10]).right=last;graph.Build();
    const auto last_result=NoHeapProject(graph.observation,p.workspace);Check(last_result.ok(),"last actual registered right projects");
    Check(last_result.work_performed==first_result.work_performed,
        "a constant right costs the same work whichever listed right it is");
    const auto exact=last_result.work_performed;p.workspace={};
    auto l=projection_limits;l.maximum_work=exact;p=prepare(l);
    Check(p.ok()&&NoHeapProject(graph.observation,p.workspace).ok(),"last vocabulary candidate exact total work bound succeeds");
    p.workspace={};--l.maximum_work;p=prepare(l);Check(p.ok(),"one-short complete vocabulary work preparation");
    ProjectionEmpty(NoHeapProject(graph.observation,p.workspace),ProjectionError::work_limit);
    AsGrant(graph.rows[10]).right=RT::CONNECT;graph.Build();
  }
  {auto l=projection_limits;l.maximum_records=graph.records.size()-1;l.maximum_depth=1;auto p=prepare(l);Check(p.ok(),"smaller record capacity");ProjectionEmpty(NoHeapProject(graph.observation,p.workspace),ProjectionError::resource_exhausted);}
  {auto l=projection_limits;l.maximum_depth=4;auto p=prepare(l);Check(p.ok(),"shallower DFS capacity");ProjectionEmpty(NoHeapProject(graph.observation,p.workspace),ProjectionError::depth_limit);}
  {auto l=projection_limits;l.maximum_depth=5;auto p=prepare(l);Check(p.ok()&&NoHeapProject(graph.observation,p.workspace).ok(),"exact mixed graph depth admitted");}
  {
    GraphData diamond;
    auto l=projection_limits;l.maximum_depth=4;auto p=prepare(l);
    Check(p.ok(),"diamond graph workspace admitted");
    ProjectionEmpty(NoHeapProject(diamond.observation,p.workspace),ProjectionError::depth_limit);
    p.workspace={};l.maximum_depth=5;p=prepare(l);
    const auto complete=NoHeapProject(diamond.observation,p.workspace);
    Check(complete.ok()&&complete.projection.maximum_membership_depth()==5,
        "a previously visited shared ancestor retains its depth on the longer branch");
  }
  for(unsigned mode=0;mode<4;++mode){auto l=projection_limits;if(mode==0)l.maximum_records=0;else if(mode==1)l.maximum_depth=0;else if(mode==2)l.maximum_work=0;else l.maximum_depth=l.maximum_records+1;
    Check(!authorization::NativeRuntimeAuthorizationWorkspaceBytes(l)&&!prepare(l).ok(),"zero/contradictory caller bounds refuse without invented defaults");}
  {auto l=projection_limits;l.maximum_records=std::numeric_limits<std::size_t>::max();Check(!authorization::NativeRuntimeAuthorizationWorkspaceBytes(l),"all array multiplication/total overflow refuses");}
  {auto foreign=memory.binding;foreign.owner_uuid=Id(240);auto p=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,foreign);
    Check(!p.ok()&&p.error==ProjectionError::backing_unavailable&&p.memory_error==db::NativeStorageMemoryError::invalid_binding,"complete actual binary grant ownership required");}
  memory.memory={};memory.Empty();
  {LeafMemoryFixture short_memory(bytes-1);auto p=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,short_memory.memory,short_memory.binding);
    Check(!p.ok()&&p.error==ProjectionError::backing_unavailable,"one-byte-short actual reservation cannot be silently supplemented");p.workspace={};short_memory.memory={};short_memory.Empty();}
  authorization::NativeRuntimeAuthorizationPreparedMemory absent;ProjectionEmpty(NoHeapProject(graph.observation,absent),ProjectionError::backing_unavailable);
}
void ProjectionPreparationFaults(){
  const auto bytes=authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits);
  GraphData graph;
  // Query/registry initialization and actual grant admission are OUTSIDE the
  // injection window. Each attempt owns a fresh real ledger/resource, retaining
  // both cold allocation sites and any post-commit optional reporting sites.
  const auto sites=[&]{LeafMemoryFixture memory(bytes);
    observed_allocations=0;count_allocations=true;
    auto prepared=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,memory.binding);
    count_allocations=false;const auto measured=observed_allocations;const auto actual=memory.manager.Snapshot();
    Check(prepared.ok()&&actual.current_bytes==bytes&&actual.active_allocation_count==1&&actual.reserved_capacity_bytes==0&&
        memory.ledger.Snapshot().current_bytes==bytes,"complete measured projection preparation owns its exact single governed backing");
    prepared.workspace={};const auto released=memory.manager.Snapshot();
    Check(!released.current_bytes&&!released.active_allocation_count&&released.reserved_capacity_bytes==bytes,
        "measurement releases every physical owner before original reserved capacity");
    memory.memory={};memory.Empty();return measured;}();
  Check(sites&&sites<static_cast<unsigned long>(std::numeric_limits<long>::max()),
      "actual projection preparation allocation count fits full checked fault range");
  bool completed=false;unsigned refused=0,reporting_only=0;
  for(unsigned long at=0;at<=sites&&!completed;++at){LeafMemoryFixture memory(bytes);
    const auto before=memory.manager.Snapshot();
    Check(!before.current_bytes&&!before.active_allocation_count&&before.reserved_capacity_bytes==bytes,
        "each full preparation fault starts with actual reserved capacity and no physical payload");
    allocation_budget=static_cast<long>(at);
    auto prepared=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,memory.binding);
    const auto remaining=allocation_budget;allocation_budget=-1;const auto after=memory.manager.Snapshot();
    if(!prepared.ok()){
      ++refused;
      if(remaining!=-1)std::cerr<<"projection preparation unconsumed refusal at="<<at<<" measured="<<sites<<" error="<<int(prepared.error)<<'\n';
      Check(remaining==-1&&!prepared.workspace&&
          (prepared.error==ProjectionError::resource_exhausted||prepared.error==ProjectionError::backing_unavailable),
          "every allocation refusal consumes the actual injected site and exposes no partial prepared owner");
    }else{
      Check(bool(prepared.workspace)&&after.current_bytes==bytes&&after.active_allocation_count==1&&after.reserved_capacity_bytes==0&&
          memory.ledger.Snapshot().current_bytes==bytes,"every successful faulted preparation retains complete exact backing/charges");
      Check(NoHeapProject(graph.observation,prepared.workspace).ok(),
          "successful faulted preparation contains usable complete schemas/vocabulary/graph capacity, not a partial marker");
      if(remaining==-1){
        ++reporting_only;
        Check(after.telemetry_truncation_count>before.telemetry_truncation_count,
            "post-commit report-only fault preserves real ownership and records actual telemetry loss");
      }else{completed=true;Check(remaining>=0,"terminal successful preparation has no consumed hidden allocation fault");}
    }
    prepared.workspace={};const auto released=memory.manager.Snapshot();
    Check(!released.current_bytes&&!released.active_allocation_count&&released.reserved_capacity_bytes==bytes&&
        memory.ledger.Snapshot().current_bytes==bytes,"every fault/success releases physical owner without losing original quota custody");
    memory.memory={};memory.Empty();
  }
  Check(completed&&refused&&reporting_only,"complete projection preparation sweep covers allocation/refusal/reporting sites through actual terminal success");
}
} // namespace
int main()try{
  ProjectionBounds();ProjectionPreparationFaults();ImportedGroupRoleMembership();GroupChains();GraphSemantics();
  for(unsigned profile=0;profile<5;++profile){RealNativeProjection(profile,false);RealNativeProjection(profile,true);}
  RealNativeProjection(0,false,true);
  std::cout<<"PASS native runtime authorization projection "<<checks<<" checks; projection is not authentication/activation/BEGIN\n";return 0;
}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
