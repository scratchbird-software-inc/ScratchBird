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
      result.projection.engine_owned_bootstrap_role_uuid().is_nil()&&!result.projection.Allows("CONNECT"),
      "projection refusal exposes no usable partial closure/grants/bundle");
}

void AdvanceNativeSelectedBindingEpochs(SecurityFixture& f) {
  // Independent provisioned-fixture bytes, NOT a production actuator or claim
  // of configuration activation. The actual reopened reader must observe the
  // new persisted selection while unchanged security descriptors retain their
  // original visibility generations.
  const auto loaded=db::ReadNativeCatalogLeafFromOpenDevice(f.device,Id(1),f.receipt.relations[4]);
  Check(loaded.ok(),"actual configuration leaf before historical epoch fixture");
  auto leaf=*loaded.page;bool found=false;
  for(auto& row:leaf.body.rows){auto metadata=loaded.metadata.at(row.version_uuid);
    if(metadata.record.header.object_uuid.value!=Id(180))continue;
    auto binding=catalog::DecodeCatalogRuntimeAuthorityBinding(metadata.record.payload);Check(binding.ok(),"actual persisted binding decoded for fixture");
    binding.record->security_epoch=4;binding.record->catalog_generation=7;
    const auto body=catalog::EncodeCatalogRuntimeAuthorityBinding(*binding.record);Check(body.ok(),"exact advanced selected epochs remain native u64");
    metadata.security_epoch=4;metadata.catalog_generation=7;metadata.record.payload.assign(body.bytes.begin(),body.bytes.end());
    const auto outer=catalog::EncodeCatalogMetadataVersion(metadata);Check(outer.ok(),"complete advanced configuration common metadata");
    row.cells.front().value.payload=outer.bytes;found=true;
  }
  Check(found,"actual bound configuration identity changed, not synthesized observation");
  const auto canonical=db::EncodeNativeCatalogLeaf(leaf);Check(canonical.ok(),"advanced native binding leaf framing");
  const auto independent=LeafOracle(leaf);Check(independent==canonical.bytes,"independent advanced binding fixture byte oracle");
  const auto io=f.device.WriteAt(leaf.header.page_number*u64{sizes[f.profile]},independent.data(),independent.size());
  Check(io.ok()&&io.bytes_transferred==independent.size()&&f.device.Sync().ok(),"advanced native bound selection physically persisted");
}
void RealNativeProjection(unsigned profile,bool indexed,bool historical=false) {
  SecurityFixture f(profile,0,indexed);
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
  auto source=db::AcquireNativeSelectedCheckpointMemoryLease(Id(1),devices,Id(2),{f.budget,32*1024*1024},source_memory.memory,source_memory.binding);
  Check(source.ok(),"actual reopened native selected checkpoint/inventory lease");
  allocation_budget=0;const auto observed=runtime_security::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease,f.Request(),prepared.workspace);
  const auto remaining=allocation_budget;allocation_budget=-1;
  Check(remaining==0&&observed.ok(),"complete actual role4/role5 read with no ordinary heap");
  const auto before_write=stage_writes,before_sync=stage_syncs;
  const auto result=NoHeapProject(observed,graph.workspace);
  if(!result.ok())std::cerr<<"real projection error="<<int(result.error)<<" record="<<result.failing_record<<'\n';
  Check(result.ok()&&result.projection.subjects().size()==2&&result.projection.memberships().size()==1&&
      result.projection.grants().size()==2,"real native direct/inherited/denied cohort fully projected");
  const auto& p=result.projection;
  Check(p.binding().database_uuid==Id(1)&&p.binding().security_epoch==(historical?4:1)&&p.binding().policy_epoch==31&&p.binding().catalog_generation==(historical?7:1)&&
      p.principal().security_context_generation==9&&p.principal().security_generation==11&&p.principal().policy_generation==13,
      "all real configuration and independent service event generations preserved without clamping/equating");
  if(historical)Check(p.grants()[0].security_epoch==4&&p.grants()[0].source_security_epoch==1&&p.grants()[0].catalog_generation==1&&
      p.memberships()[0].security_epoch==4&&p.memberships()[0].source_security_epoch==1&&p.subjects()[0].catalog_generation==1,
      "real historical native versions remain effective under newer actual selected epochs with unchanged provenance");
  allocation_budget=0;
  const bool allow=p.Allows("CONNECT"),target_deny=!p.Allows("SELECT",Id(240)),other_deny=!p.Allows("SELECT",Id(241)),unknown_deny=!p.Allows("NOT_A_RIGHT");
  const auto evaluation_budget=allocation_budget;allocation_budget=-1;
  Check(evaluation_budget==0&&allow&&target_deny&&other_deny&&unknown_deny,"base decision is allocation-free and defaults deny");
  Check(p.engine_owned_bootstrap_role_uuid().is_nil(),"ordinary provisioned service is not elevated by name or absent canonical membership");
  Check(stage_writes==before_write&&stage_syncs==before_sync,"projection never publishes/syncs or mutates native storage");
  // A source failure retains its complete actual typed I/O/MGA failure and no
  // previously successful graph prefix, even when the workspace is reused.
  reads=0;read_fault=1;track_reads=true;
  const auto failed_source=runtime_security::ReadCommittedNativeRuntimeSecurityOnLease(*source.lease,f.Request(),prepared.workspace);
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

// Graph semantics are fed through actual native v2 codecs and complete common
// metadata, not caller-constructed preflattened effective grants. They remain
// explicitly DATA fixtures, never an authenticated service/admission proof.
struct GraphData {
  using Record=catalog::CatalogSecurityRecord;
  std::vector<Record> rows;
  std::vector<Bytes> payloads;
  std::vector<authorization::NativeRuntimeSecurityRecordObservation> records;
  SecurityResult observation;
  static Record Principal(){auto r=SecurityRecord(catalog::CatalogRecordKind::user_account,200);
    r.attributes.emplace("kind","service");r.attributes.emplace("principal_kind","service");r.attributes.emplace("bootstrap_principal",false);return r;}
  static Record Group(unsigned id){return SecurityRecord(catalog::CatalogRecordKind::group_account,id);}
  static Record Role(unsigned id){return SecurityRecord(catalog::CatalogRecordKind::role_account,id);}
  static Record Member(unsigned id,unsigned member,std::string kind,unsigned parent,std::string parent_kind){
    auto r=SecurityRecord(catalog::CatalogRecordKind::grant_record,id);
    r.identities.emplace("membership_uuid",Id(id));r.identities.emplace("member_uuid",Id(member));r.identities.emplace("parent_uuid",Id(parent));
    r.attributes.emplace("grant_class",parent_kind=="role"?"role_membership":"group_membership");
    r.attributes.emplace("member_kind",std::move(kind));r.attributes.emplace("parent_kind",std::move(parent_kind));return r;
  }
  static Record Grant(unsigned id,unsigned subject,std::string kind,std::string right,bool deny=false,unsigned target=0){
    auto r=SecurityRecord(catalog::CatalogRecordKind::grant_record,id);r.identities.emplace("subject_uuid",Id(subject));
    if(target)r.identities.emplace("target_uuid",Id(target));
    r.attributes.emplace("grant_class","privilege");r.attributes.emplace("subject_kind",std::move(kind));
    r.attributes.emplace("right",std::move(right));r.attributes.emplace("effect",deny?"deny":"allow");return r;
  }
  GraphData(){
    rows={Principal(),Group(202),Role(206),Group(207),Role(208),Role(209),
        Member(210,200,"principal",202,"group"),Member(211,202,"group",206,"role"),
        Member(212,206,"role",207,"group"),Member(213,207,"group",208,"role"),Member(214,202,"group",208,"role"),
        Grant(220,200,"principal","CONNECT"),Grant(221,208,"role","SELECT",false,240),
        Grant(222,207,"group","SELECT",true,241),Grant(223,202,"group","UPDATE"),Grant(224,206,"role","UPDATE",true,240)};
    rows[1].attributes.erase("active");rows[2].attributes.erase("active");rows[5].attributes["active"]=false;
    Build();
  }
  void Build(){
    payloads.clear();records.clear();payloads.reserve(rows.size());records.reserve(rows.size());
    for(std::size_t n=0;n<rows.size();++n){
      auto metadata=SecurityMetadata(rows[n],n).metadata;
      Check(!catalog::ValidateCatalogMetadataVersionView(catalog::BorrowCatalogMetadataVersion(metadata)),"genuine graph common metadata framing");
      const auto encoded=catalog::EncodeCatalogSecurityRecord(rows[n]);Check(encoded.ok(),"genuine graph native scalar/UUID codec");payloads.push_back(encoded.bytes);
      const auto& bytes=payloads.back();const auto decoded=catalog::DecodeCatalogSecurityRecordView(rows[n].kind,
          {reinterpret_cast<const char*>(bytes.data()),bytes.size()});Check(decoded.ok(),"genuine graph native borrowed record");
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
authorization::DurableAuthorizationState LegacyGraph(const GraphData& graph){
  authorization::DurableAuthorizationState s;const auto& b=*graph.observation.binding;
  s.authority_uuid=b.security_authority_uuid;s.security_context_generation=graph.observation.principal->security_context_generation;
  s.security_epoch=b.security_epoch;s.policy_epoch=b.policy_epoch;s.catalog_generation_id=b.catalog_generation;
  for(std::size_t n=0;n<graph.rows.size();++n){const auto& row=graph.rows[n];const auto& native=graph.records[n];
    const bool active=!native.deleted&&native.lifecycle==catalog::CatalogObjectLifecycle::active&&native.status==catalog::CatalogObjectStatus::active&&
        row.Boolean("active")!=false&&row.Boolean("created_disabled")!=true;
    const auto id=row.Identity(catalog::CatalogSecurityPrimaryIdentityName(row.kind));
    if(row.kind==catalog::CatalogRecordKind::user_account)s.principals.push_back({id,"principal",active,s.security_epoch});
    else if(row.kind==catalog::CatalogRecordKind::role_account){s.roles.push_back({id,active,s.security_epoch});
      if(id==db::kCanonicalSysarchRoleIdentity&&row.Boolean("engine_owned")==true&&row.Boolean("immutable")==true&&row.Attribute("authority_class")=="engine_owned_sysarch")s.engine_owned_sysarch_role_uuid=id;}
    else if(row.kind==catalog::CatalogRecordKind::group_account)s.groups.push_back({id,active,s.security_epoch});
    else if(row.Attribute("grant_class")=="privilege")s.grants.push_back({id,row.Identity("subject_uuid"),std::string(*row.Attribute("subject_kind")),row.Identity("target_uuid"),std::string(*row.Attribute("right")),row.Attribute("effect")=="deny",active,s.security_epoch});
    else s.memberships.push_back({row.Identity("member_uuid"),std::string(*row.Attribute("member_kind")),row.Identity("parent_uuid"),std::string(*row.Attribute("parent_kind")),active,s.security_epoch});
  }return s;
}
void Parity(const GraphData& data,const ProjectionResult& result){
  auto state=LegacyGraph(data);const auto legacy=authorization::MaterializeDurableAuthorizationContext(state,{Id(200),state.security_epoch,state.policy_epoch,state.catalog_generation_id});
  Check(legacy.ok&&result.ok(),"native and existing owning materializers both accept genuine graph");
  Check(legacy.context.effective_subjects.size()==result.projection.subjects().size()&&legacy.context.grants.size()==result.projection.grants().size(),"exact closure/grant cardinality parity");
  for(const auto& subject:result.projection.subjects())Check(std::any_of(legacy.context.effective_subjects.begin(),legacy.context.effective_subjects.end(),[&](const auto& old){return old.subject_uuid==subject.subject_uuid&&old.subject_kind==KindName(subject.kind);}),"exact binary typed subject parity");
  for(const auto& grant:result.projection.grants())Check(std::any_of(legacy.context.grants.begin(),legacy.context.grants.end(),[&](const auto& old){return old.grant_uuid==grant.grant_uuid&&old.subject_uuid==grant.subject_uuid&&old.subject_kind==KindName(grant.subject_kind)&&old.target_uuid==grant.target_uuid&&old.right==grant.right&&old.deny==grant.deny&&old.security_epoch==grant.security_epoch;}),"exact durable binary grant/effect/target parity");
  authorization::EngineRequestContext request;request.security_context_present=true;request.principal_uuid=Id(200);request.security_epoch=state.security_epoch;request.catalog_generation_id=state.catalog_generation_id;
  for(const auto& right:authorization::KnownSecurityRights())for(const auto target:{Uuid{},Id(240),Id(241),Id(242)}){
    const auto old=authorization::EvaluateMaterializedAuthorization(request,legacy.context,right,target);
    Check(old.authorized==result.projection.Allows(right,target),"all registered rights/global/target explicit-deny parity");
  }
}
void GraphSemantics(){
  const auto bytes=authorization::NativeRuntimeAuthorizationWorkspaceBytes(projection_limits);LeafMemoryFixture memory(bytes);
  auto prepared=authorization::PrepareNativeRuntimeAuthorizationProjection(projection_limits,memory.memory,memory.binding);Check(prepared.ok(),"governed graph preparation");
  GraphData graph;const auto original_payloads=graph.payloads;
  auto result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(graph.payloads==original_payloads,"native security/credential attribute payload bytes remain immutable during projection");
  Check(result.projection.subjects().size()==5&&result.projection.memberships().size()==5&&result.projection.grants().size()==5,
      "transitive mixed group-role diamond is deduplicated without discarding actual membership lineage");
  Check(result.projection.Allows("CONNECT")&&result.projection.Allows("SELECT",Id(240))&&!result.projection.Allows("SELECT",Id(241))&&
      !result.projection.Allows("SELECT",Id(242))&&result.projection.Allows("UPDATE",Id(242))&&!result.projection.Allows("UPDATE",Id(240)),
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
  graph.rows.push_back(GraphData::Member(225,208,"role",202,"group"));graph.Build();reject(ProjectionError::membership_cycle);
  auto legacy_cycle=authorization::MaterializeDurableAuthorizationContext(LegacyGraph(graph),{Id(200),1,31,1});Check(!legacy_cycle.ok,"existing materializer also rejects actual reachable cycle");
  graph.rows.pop_back();graph.rows.push_back(GraphData::Member(225,209,"role",209,"role"));graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(result.projection.subjects().size()==5,"unreachable inactive self cycle grants nothing and does not deny unrelated principal");
  graph.rows.pop_back();graph.rows[2].attributes["active"]=false;graph.Build();reject(ProjectionError::subject_missing);
  graph.rows[2].attributes.erase("active");graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Check(result.ok(),"omitted imported active flag is NOT automatically denied");
  graph.rows[2].attributes["created_disabled"]=true;graph.Build();reject(ProjectionError::subject_missing);graph.rows[2].attributes.erase("created_disabled");graph.Build();
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
    for(const auto n:{"security_context_generation","security_generation","policy_generation"})graph.rows[0].attributes[n]=std::numeric_limits<u64>::max();
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
  graph.rows[6].attributes["effect"]="deny";graph.Build();reject(ProjectionError::malformed_membership);graph.rows[6].attributes.erase("effect");graph.Build();
  graph.rows[6].identities["principal_uuid"]=Id(202);graph.Build();reject(ProjectionError::malformed_membership);graph.rows[6].identities.erase("principal_uuid");graph.Build();
  graph.rows[11].attributes["effect"]="unknown";graph.Build();reject(ProjectionError::malformed_grant);graph.rows[11].attributes["effect"]="allow";
  graph.rows[11].attributes["right"]="MADE_UP_PERMISSION";graph.Build();reject(ProjectionError::unknown_right);graph.rows[11].attributes["right"]="CONNECT";graph.Build();
  // Exact structural type validation cannot confuse malformed active text with
  // an omitted optional active flag, even on an otherwise unreachable record.
  graph.records[5].record.attributes[9]=std::string_view("true");reject(ProjectionError::invalid_record);graph.Build();
  graph.records[5].record.attributes[13]=std::string_view("\xc0\xaf",2);reject(ProjectionError::invalid_record);graph.Build();
  graph.records[5].record.identities[0]=Id(241);reject(ProjectionError::invalid_record);graph.Build();
  result=NoHeapProject(graph.observation,prepared.workspace);Check(result.ok(),"valid result before overlap admission");
  graph.records[5].record.attributes[13]=result.projection.grants()[0].right;
  reject(ProjectionError::input_overlap);graph.Build();
  graph.records[0].record.identities[0]=Id(201);reject(ProjectionError::invalid_record);graph.Build();
  graph.records[0].row_uuid=graph.records[0].object_uuid;reject(ProjectionError::invalid_record);graph.Build();
  graph.rows[11].attributes["active"]=false;graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(!result.projection.Allows("CONNECT"),"retired/inactive grants cannot authorize");
  // Name/code alone, and even caller attributes on a different role identity,
  // never attach the engine-owned bootstrap bundle.
  graph.rows[4].attributes["role_code"]="ROLE_SYSARCH";graph.rows[4].attributes["role_name"]="SYSARCH";
  graph.rows[4].attributes["engine_owned"]=true;graph.rows[4].attributes["immutable"]=true;graph.rows[4].attributes["authority_class"]="engine_owned_sysarch";
  graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Check(result.ok()&&result.projection.engine_owned_bootstrap_role_uuid().is_nil()&&!result.projection.Allows("DELETE"),"forged display bootstrap role cannot elevate");
  graph.rows[4].identities["role_uuid"]=db::kCanonicalSysarchRoleIdentity;
  graph.rows[4].attributes["identity_authority"]="uuid";graph.rows[4].attributes["create_time_only"]=true;
  for(auto& r:graph.rows)if(r.Identity("parent_uuid")==Id(208))r.identities["parent_uuid"]=db::kCanonicalSysarchRoleIdentity;
  graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);Parity(graph,result);
  Check(result.projection.engine_owned_bootstrap_role_uuid()==db::kCanonicalSysarchRoleIdentity&&result.projection.Allows("DELETE")&&
      !result.projection.Allows("UPDATE",Id(240)),"actual canonical immutable reachable role bundle still obeys explicit denial");
  graph.rows[4].attributes["immutable"]=false;graph.Build();result=NoHeapProject(graph.observation,prepared.workspace);
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
    const auto exact_work=result.work_performed;prepared.workspace={};
    auto limited=projection_limits;limited.maximum_work=exact_work;prepared=prepare(limited);
    Check(prepared.ok()&&NoHeapProject(graph.observation,prepared.workspace).ok(),"exact measured work boundary accepted without off-by-one");
    prepared.workspace={};limited.maximum_work=exact_work-1;prepared=prepare(limited);Check(prepared.ok(),"one short work preparation");
    ProjectionEmpty(NoHeapProject(graph.observation,prepared.workspace),ProjectionError::work_limit);
  }
  {
    const auto& rights=authorization::KnownSecurityRights();
    Check(rights.size()>1,"actual registered vocabulary has distinct first/last positions");
    const auto first=*rights.begin(),last=*rights.rbegin();
    // This is a separately measured full valid projection, not an internal
    // selector oracle. Only one genuine encoded grant's right changes.
    auto p=prepare(projection_limits);Check(p.ok(),"vocabulary accounting preparation");
    graph.rows[10].attributes["right"]=first;graph.Build();
    const auto first_result=NoHeapProject(graph.observation,p.workspace);Check(first_result.ok(),"first actual registered right projects");
    graph.rows[10].attributes["right"]=last;graph.Build();
    const auto last_result=NoHeapProject(graph.observation,p.workspace);Check(last_result.ok(),"last actual registered right projects");
    Check(last_result.work_performed+first.size()==first_result.work_performed+last.size()+rights.size()-1,
        "every actual vocabulary candidate comparison charged in addition to exact source text inspection bytes");
    const auto exact=last_result.work_performed;p.workspace={};
    auto l=projection_limits;l.maximum_work=exact;p=prepare(l);
    Check(p.ok()&&NoHeapProject(graph.observation,p.workspace).ok(),"last vocabulary candidate exact total work bound succeeds");
    p.workspace={};--l.maximum_work;p=prepare(l);Check(p.ok(),"one-short complete vocabulary work preparation");
    ProjectionEmpty(NoHeapProject(graph.observation,p.workspace),ProjectionError::work_limit);
    graph.rows[10].attributes["right"]="CONNECT";graph.Build();
  }
  {auto l=projection_limits;l.maximum_records=graph.records.size()-1;l.maximum_depth=1;auto p=prepare(l);Check(p.ok(),"smaller record capacity");ProjectionEmpty(NoHeapProject(graph.observation,p.workspace),ProjectionError::resource_exhausted);}
  {auto l=projection_limits;l.maximum_depth=4;auto p=prepare(l);Check(p.ok(),"shallower DFS capacity");ProjectionEmpty(NoHeapProject(graph.observation,p.workspace),ProjectionError::depth_limit);}
  {auto l=projection_limits;l.maximum_depth=5;auto p=prepare(l);Check(p.ok()&&NoHeapProject(graph.observation,p.workspace).ok(),"exact mixed graph depth admitted");}
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
  ProjectionBounds();ProjectionPreparationFaults();GraphSemantics();
  for(unsigned profile=0;profile<5;++profile){RealNativeProjection(profile,false);RealNativeProjection(profile,true);}
  RealNativeProjection(0,false,true);
  std::cout<<"PASS native runtime authorization projection "<<checks<<" checks; projection is not authentication/activation/BEGIN\n";return 0;
}catch(const std::exception& e){allocation_budget=-1;std::cerr<<"FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
