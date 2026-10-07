// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Included by the real-file genesis fixture. Metadata references here are
// component inputs, not proof of catalog owner/name/security admission.
#include "catalog_metric_binding.hpp"
#include "catalog_metric_visibility_policy.hpp"
#include "native_creation_workspace_recovery.hpp"
#include "native_creation_catalog_input.hpp"
namespace {
db::NativeCreationCatalogSeed MetricSeed(const db::NativeFilespaceInitializationRequest& request) {
  namespace c=scratchbird::core::catalog;
  namespace m=scratchbird::core::metrics;
  c::CatalogMetricDescriptor d;
  d.definition.family="fixture_construction_measurement";
  d.definition.namespace_path="sys.metrics.storage";
  d.definition.help="Storage component definition fixture; not an emitted observation.";
  d.definition.producer_owner="construction_fixture";
  d.definition.security_family="OBS_METRICS_READ_DATABASE";
  d.definition.type=m::MetricType::gauge;d.definition.unit=m::MetricUnit::bytes;
  d.definition.value_type=m::MetricScalarType::uint64;
  d.definition.labels={{"database_uuid",true,false,m::MetricLabelType::system_uuid}};
  d.binding.metric_uuid=Id(500);d.binding.descriptor_generation=1;
  d.binding.label_schema_uuid=Id(501);d.binding.label_schema_generation=1;
  d.binding.retention_policy_uuid=Id(502);d.binding.retention_policy_generation=1;
  d.binding.visibility_policy_uuid=Id(503);d.binding.visibility_policy_generation=1;
  d.origin_transaction_uuid=request.creator.transaction_uuid;d.origin_local_transaction_id=1;
  c::CatalogMetricLabelSchema l;
  l.label_schema_uuid=Id(501);l.generation=1;l.labels=d.definition.labels;
  l.origin_transaction_uuid=d.origin_transaction_uuid;l.origin_local_transaction_id=1;
  c::CatalogMetricRetentionPolicy r;
  r.policy.policy_uuid=Id(502);r.policy.generation=1;r.policy.policy_name="fixture_retention";
  r.origin_transaction_uuid=d.origin_transaction_uuid;r.origin_local_transaction_id=1;
  c::CatalogMetricVisibilityPolicy v;
  v.policy_uuid=Id(503);v.generation=1;v.database_uuid=request.bootstrap.database_uuid;
  v.metric_uuid=Id(500);v.read_right=d.definition.security_family;
  v.origin_transaction_uuid=d.origin_transaction_uuid;v.origin_local_transaction_id=1;
  db::NativeCreationCatalogSeed seed;
  const auto add=[&](unsigned index,c::CatalogRecordKind kind,const char* subtype,const auto& encoded) {
    Check(encoded.ok(),"actual metric dependency codec");
    c::CatalogMetadataVersion meta;
    meta.record.header.kind=kind;meta.record.header.row_uuid={UuidKind::row,Id(600+index)};
    meta.record.header.object_uuid={UuidKind::object,Id(500+index)};
    meta.record.header.parent_uuid={UuidKind::object,Id(700)};
    meta.record.payload.assign(encoded.bytes.begin(),encoded.bytes.end());
    meta.owning_schema_uuid={UuidKind::schema,Id(700)};meta.owner_uuid={UuidKind::principal,Id(701)};
    meta.audit_uuid={UuidKind::object,Id(702)};
    meta.default_name_uuid={UuidKind::object,Id(710+index)};meta.name_vector_uuid={UuidKind::object,Id(720+index)};
    meta.security_policy_uuid={UuidKind::object,Id(503)};
    meta.definition_version=meta.schema_epoch=meta.security_epoch=meta.catalog_generation=1;
    meta.dependency_generation=meta.invalidation_generation=1;
    meta.creator_transaction_uuid=request.creator.transaction_uuid;meta.creator_local_transaction_id=1;
    meta.lifecycle=c::CatalogObjectLifecycle::active;meta.status=c::CatalogObjectStatus::active;
    meta.object_subtype=subtype;meta.retention_class="catalog_history";meta.trace_search_key="NATIVE-CREATION-CATALOG-FIXTURE";
    Check(c::EncodeCatalogMetadataVersion(meta).ok(),"complete common native metadata");
    seed[0].push_back({std::move(meta),{}});
  };
  add(0,c::CatalogRecordKind::metric_descriptor,"metric_descriptor",c::EncodeCatalogMetricDescriptor(d));
  add(1,c::CatalogRecordKind::metric_label_schema,"metric_label_schema",c::EncodeCatalogMetricLabelSchema(l));
  add(2,c::CatalogRecordKind::policy,"metric_retention",c::EncodeCatalogMetricRetentionPolicy(r));
  add(3,c::CatalogRecordKind::policy,"metric_visibility",c::EncodeCatalogMetricVisibilityPolicy(v));
  auto names=seed[0][0].metadata;
  names.record.header.kind=c::CatalogRecordKind::localized_name;names.record.payload.clear();
  names.record.header.row_uuid={UuidKind::row,Id(620)};
  names.record.header.object_uuid={UuidKind::object,Id(720)};
  names.record.header.parent_uuid={UuidKind::object,Id(500)};
  names.object_subtype="name_vector";names.name_vector_uuid=names.record.header.object_uuid;
  c::CatalogNameVector vector;
  vector.name_vector_uuid=names.name_vector_uuid;vector.object_uuid=names.record.header.parent_uuid;
  vector.object_class="metric_descriptor";vector.owning_schema_uuid=names.owning_schema_uuid;
  vector.default_language_tag="en-US";vector.default_name_entry_uuid=names.default_name_uuid;
  vector.name_collision_policy_uuid={UuidKind::object,Id(740)};vector.catalog_generation_id=1;
  vector.security_policy_uuid=names.security_policy_uuid;vector.lifecycle_state=c::CatalogNameLifecycle::active;
  seed[1].push_back({names,vector});
  names.record.header.row_uuid={UuidKind::row,Id(621)};
  names.record.header.object_uuid=names.default_name_uuid;
  names.record.header.parent_uuid=names.name_vector_uuid;
  names.object_subtype="name_entry";names.resource_epoch=1;
  c::CatalogNameEntry entry;
  entry.name_entry_uuid=names.record.header.object_uuid;entry.name_vector_uuid=names.name_vector_uuid;
  entry.object_uuid={UuidKind::object,Id(500)};entry.object_class="metric_descriptor";
  entry.scope_uuid={UuidKind::object,Id(700)};entry.parent_schema_uuid=names.owning_schema_uuid;
  entry.language_tag="en-US";entry.dialect_profile_uuid={UuidKind::object,Id(741)};
  entry.identifier_profile_uuid={UuidKind::object,Id(742)};
  entry.raw_name_text=entry.display_name="fixture_construction_measurement";
  entry.catalog_generation_id=1;entry.created_transaction_uuid=request.creator.transaction_uuid;
  entry.security_policy_uuid=names.security_policy_uuid;entry.resource_epoch=entry.name_resolution_epoch=1;
  entry.lifecycle_state=c::CatalogNameLifecycle::active;
  seed[1].push_back({names,entry});
  return seed;
}
void PopulatedWorkspaceChecks() {
  namespace c=scratchbird::core::catalog;
  Fixture fixture;
  for(unsigned profile=0;profile<5;++profile) {
    const auto request=Request(profile);const auto seed=MetricSeed(request);
    page_bytes=request.bootstrap.page_size_bytes;total_pages=request.total_pages;selector_page=17;
    const auto budget=256*page_bytes;const auto path=fixture.Next();disk::FileDevice device;
    Check(device.Open(path.string(),disk::FileOpenMode::create_new).ok(),"populated workspace owned open");
    const auto created=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(device,request,seed,budget,*fixture.issuer);
    if(!created.ok())std::cerr<<"populated error="<<static_cast<int>(created.error)<<" checkpoint="<<static_cast<int>(created.checkpoint_error)<<'\n';
    Check(created.ok()&&created.receipt->catalog_rows[0]==4,"real populated native construction");
    Check(device.Close().ok(),"close populated owner");
    Check(device.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"reopen populated owner");
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(device);
    Check(zero.ok()&&zero.record->bootstrap.lifecycle_state==7,"population never grants serving state");
    const std::vector<disk::NativeFilespaceDevice> devices{{request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid,&device}};
    const auto selected=db::ReadNativeBoundCheckpointSelectionFromOpenDevices(request.bootstrap.database_uuid,devices,request.bootstrap.filespace_uuid,budget);
    Check(selected.ok(),"actual reopened durable selector");
    const auto& cp=*selected.checkpoint_inventory.checkpoint;
    const disk::FilespaceRootReference checkpoint{9,0x300,cp.header.filespace_uuid,cp.header.page_number,cp.header.page_generation,cp.header.page_size_profile_uuid,cp.object_uuid};
    const auto rows=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(request.bootstrap.database_uuid,devices,checkpoint,2,1,{created.receipt->relations[0].object_uuid,{}},budget);
    Check(rows.ok()&&rows.rows.size()==4,"committed native metadata read after reopen");
    std::vector<c::CatalogMetricRowView> views;
    for(const auto& row:rows.rows) {
      Check(!row.provisional&&row.metadata.creator_transaction_uuid.value==request.creator.transaction_uuid.value,"actual committed creator binding");
      const auto original=std::find_if(seed[0].begin(),seed[0].end(),[&](const auto& r){return r.metadata.record.header.object_uuid.value==row.metadata.record.header.object_uuid.value;});
      Check(original!=seed[0].end()&&c::EncodeCatalogMetadataVersion(original->metadata).bytes==c::EncodeCatalogMetadataVersion(row.metadata).bytes,"all metadata bytes preserved");
      views.push_back({&row.metadata,row.provisional});
    }
    const auto binding=c::ResolveLocalCatalogMetricBindings(views,Id(500),1);
    Check(binding.ok()&&binding.binding->metric.labels->label_schema_uuid==Id(501)&&binding.binding->metric.retention.policy.policy_uuid==Id(502),"reopened exact dependency graph");
    const auto visibility=c::DecodeCatalogMetricVisibilityPolicy(binding.binding->metric.visibility_policy.record.payload);
    Check(visibility.ok()&&visibility.record->database_uuid==request.bootstrap.database_uuid&&visibility.record->metric_uuid==Id(500),"real typed visibility dependency");
    Check(!c::ResolveLocalCatalogMetricBindings(views,Id(500),2).ok(),"stale generation refused");
    const auto names=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(request.bootstrap.database_uuid,devices,checkpoint,2,2,{created.receipt->relations[1].object_uuid,{}},budget);
    Check(names.ok()&&names.rows.size()==2&&created.receipt->catalog_rows[1]==2,"native resident names survive reopen");
    for(const auto& row:names.rows)Check(row.name_payload.has_value()&&!row.provisional,"physical name envelope bound to actual version and location");
    const disk::FilespaceBootstrapBinding bootstrap_binding{request.bootstrap.database_uuid,request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid};
    const auto unchanged=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(device,bootstrap_binding,request.operation_uuid,seed,budget);
    Check(unchanged.ok()&&!unchanged.repaired_slots&&unchanged.receipt->catalog_rows[0]==4,"populated recovery validates unchanged selected graph");
    std::vector<byte> zero_selector(page_bytes,0);
    Check(device.WriteAt(18*page_bytes,zero_selector.data(),zero_selector.size()).ok()&&device.Sync().ok(),"simulate interrupted second selector");
    auto changed_seed=seed;changed_seed[0][0].metadata.trace_search_key="different_creation_input";
    Arm();const auto mismatch=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(device,bootstrap_binding,request.operation_uuid,changed_seed,budget);Disarm();
    Check(!mismatch.ok()&&!calls[write_call],"changed seed cannot repair or overwrite original operation");
    Arm();const auto empty_refused=db::RecoverNativeCreationWorkspaceSelectionOnOpenDevice(device,bootstrap_binding,request.operation_uuid,budget);Disarm();
    Check(!empty_refused.ok()&&!calls[write_call],"empty-workspace recovery never accepts populated input");
    const auto recovered=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(device,bootstrap_binding,request.operation_uuid,seed,budget);
    Check(recovered.ok()&&recovered.repaired_slots==2&&recovered.receipt->catalog_rows[1]==2,"exact populated construction recovers second selector bit only");
    {
      const auto crash_path=fixture.Next();const auto child=fork();Check(child>=0,"populated selector-cut fork");
      if(child==0) {
        fixture.ResetIssuer();disk::FileDevice crashed;
        if(!crashed.Open(crash_path.string(),disk::FileOpenMode::create_new).ok())_exit(88);
        Arm();stop_before_second_selector=true;
        db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(crashed,request,seed,budget,*fixture.issuer);
        _exit(89);
      }
      int status=0;Check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==86,
        "actual process exited only after first durable selector and before second write");
      disk::FileDevice crashed;Check(crashed.Open(crash_path.string(),disk::FileOpenMode::open_existing).ok(),"crash recovery reacquires physical ownership");
      const auto recovery=db::RecoverPopulatedNativeCreationWorkspaceSelectionOnOpenDevice(crashed,bootstrap_binding,request.operation_uuid,seed,budget);
      Check(recovery.ok()&&recovery.repaired_slots==2&&recovery.receipt->catalog_rows[0]==4,"populated process-crash recovery preserves metric definitions");
      const std::vector<disk::NativeFilespaceDevice> recovered_devices{{request.bootstrap.filespace_uuid,request.bootstrap.page_size_profile_uuid,&crashed}};
      const auto read=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(request.bootstrap.database_uuid,recovered_devices,
        recovery.receipt->checkpoint,2,1,{recovery.receipt->relations[0].object_uuid,{}},budget);
      Check(read.ok()&&read.rows.size()==4,"actual recovered committed metric catalog read");
    }
    for(const auto& relation:seed)for(const auto& record:relation) {
      count_allocations=true;allocations=0;allocation_fault=1;
      const auto measured=db::NativeCreationCatalogRowBytes(record);
      count_allocations=false;allocation_fault=0;
      Check(measured.has_value()&&allocations==0,"size preflight allocates no memory");
    }
    for(unsigned fault=0;fault<11;++fault) {
      auto bad=seed;const auto badpath=fixture.Next();disk::FileDevice target;
      Check(target.Open(badpath.string(),disk::FileOpenMode::create_new).ok(),"invalid populated fixture open");
      if(fault==0)bad[0][0].metadata.creator_local_transaction_id=2;
      if(fault==1)bad[0][0].metadata.creator_transaction_uuid.value=Id(999);
      if(fault==2)bad[1].push_back(bad[0][0]);
      if(fault==3)bad[0][0].metadata.definition_version=2;
      if(fault==4)bad[0][0].metadata.record.payload="bad";
      if(fault==5)bad[0][0].metadata.record.header.object_uuid.value=request.bootstrap.database_uuid;
      if(fault==6)bad[0][0].metadata.trace_search_key=std::string(page_bytes*2,'x');
      if(fault==7)bad[1][0].name.reset();
      if(fault==8)bad[1][0].metadata.record.payload="prebound payload is forbidden";
      if(fault==9)std::get<c::CatalogNameEntry>(*bad[1][1].name).created_transaction_uuid.value=Id(999);
      if(fault==10) {
        auto& meta=bad[0][0].metadata;meta.record.header.kind=c::CatalogRecordKind::policy;
        meta.object_subtype="opaque_oversize_fixture";meta.record.payload=std::string(page_bytes-96,'x');
        Check(c::EncodeCatalogMetadataVersion(meta).ok(),"oversize fixture is valid metadata, not a malformed definition");
      }
      Arm();const auto refused=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(target,request,bad,budget,*fixture.issuer);Disarm();
      Check(!refused.ok()&&!refused.receipt&&!calls[write_call]&&target.Size().size_bytes==0,"invalid batch cannot publish prefix or write bytes");
      if(fault==10)Check(refused.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded,"valid no-fit has exact capacity disposition");
    }
    {
      db::NativeCreationCatalogSeed exact;
      auto row=seed[0][0];row.metadata.record.header.kind=c::CatalogRecordKind::policy;
      row.metadata.object_subtype="opaque_exact_fit_fixture";row.metadata.record.payload="x";
      const auto initial=db::NativeCreationCatalogRowBytes(row);Check(initial.has_value(),"exact capacity measurement");
      const auto available=page_bytes-128-32-page::kRowDataPageBodyHeaderBytes;
      row.metadata.record.payload.resize(available-*initial+1,'x');exact[0].push_back(row);
      disk::FileDevice target;Check(target.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"exact fit owned open");
      const auto fits=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(target,request,exact,budget,*fixture.issuer);
      Check(fits.ok()&&fits.receipt->catalog_rows[0]==1,"exact physical leaf capacity succeeds");
      exact[0][0].metadata.record.payload.push_back('x');disk::FileDevice overflow;
      Check(overflow.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"one byte overflow owned open");
      Arm();const auto rejected=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(overflow,request,exact,budget,*fixture.issuer);Disarm();
      Check(rejected.error==db::NativeCreationWorkspaceError::catalog_capacity_exceeded&&!calls[write_call]&&!overflow.Size().size_bytes,"one byte over physical capacity refuses before I/O");
    }
    for(const auto kind:{write_call,sync_call,read_call}) {
      disk::FileDevice target;Check(target.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"populated fault owned open");
      Arm(kind,1);const auto failed=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(target,request,seed,budget,*fixture.issuer);Disarm();
      Check(!failed.ok()&&!failed.receipt&&calls[kind]>=1&&target.is_open(),"physical failure returns no successful populated receipt");
    }
    {
      disk::FileDevice target;Check(target.Open(fixture.Next().string(),disk::FileOpenMode::create_new).ok(),"reference collision fixture");
      zero_entropy=true;const auto predicted=fixture.issuer->Issue(UuidKind::object);
      Check(predicted.ok(),"predict deterministic test-only issuer identity");fixture.ResetIssuer();
      auto collision_seed=seed;collision_seed[0][0].metadata.audit_uuid=*predicted.value;
      Arm();const auto collision=db::InitializePopulatedNativeCreationWorkspaceOnOpenDevice(target,request,collision_seed,budget,*fixture.issuer);
      Disarm();zero_entropy=false;
      Check(!collision.ok()&&collision.error==db::NativeCreationWorkspaceError::identity_failure&&!calls[write_call]&&!target.Size().size_bytes,
        "generated page-owner identity cannot collide with supplied audit reference");
    }
  }
  std::error_code cleanup_error;fs::remove_all(fixture.root,cleanup_error);
  Check(!cleanup_error&&!fs::exists(fixture.root),"all generated populated construction databases removed");
}
}
