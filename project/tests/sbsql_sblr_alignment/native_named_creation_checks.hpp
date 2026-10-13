// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Actual native storage fixture; no namespace/profile/security grant asserted.
namespace {
void NativeNamedCreationChecks(const db::NativeCreationCatalogSeed& supplied) {
  namespace c=scratchbird::core::catalog;
  using E=c::CatalogNamedCreationError;
  const auto request=Request();const auto example=MetricSeed(request);
  struct Names {
    c::CatalogMetadataVersion vector_metadata;
    c::CatalogNameVector vector;
    std::array<c::CatalogMetadataVersion,2> metadata;
    std::array<c::CatalogNameEntry,2> entries;
    std::array<c::CatalogCreationNameEntryInput,2> input_entries;
  };
  std::vector<Names> names(supplied[0].size());
  std::vector<c::CatalogNamedCreationInput> inputs;
  for(unsigned at=0;at<names.size();++at) {
    const auto& definition=supplied[0][at].metadata;auto& n=names[at];
    n.vector_metadata=example[1][0].metadata;
    n.vector_metadata.record.header.object_uuid=definition.name_vector_uuid;
    n.vector_metadata.record.header.row_uuid={UuidKind::row,Id(18000+3*at)};
    n.vector_metadata.record.header.parent_uuid=definition.record.header.object_uuid;
    n.vector_metadata.default_name_uuid=definition.default_name_uuid;
    n.vector_metadata.name_vector_uuid=definition.name_vector_uuid;
    n.vector_metadata.security_policy_uuid=definition.security_policy_uuid;
    n.vector=std::get<c::CatalogNameVector>(*example[1][0].name);
    n.vector.name_vector_uuid=definition.name_vector_uuid;n.vector.object_uuid=definition.record.header.object_uuid;
    n.vector.default_name_entry_uuid=definition.default_name_uuid;n.vector.security_policy_uuid=definition.security_policy_uuid;
    n.vector.object_class=definition.object_subtype;
    for(unsigned alias=0;alias<2;++alias) {
      auto& m=n.metadata[alias];auto& e=n.entries[alias];
      m=example[1][1].metadata;m.record.header.row_uuid={UuidKind::row,Id(18001+3*at+alias)};
      m.record.header.object_uuid=alias?scratchbird::core::platform::TypedUuid{UuidKind::object,Id(19000+at)}:definition.default_name_uuid;
      m.record.header.parent_uuid=definition.name_vector_uuid;
      m.default_name_uuid=m.record.header.object_uuid;m.name_vector_uuid=definition.name_vector_uuid;
      m.security_policy_uuid=definition.security_policy_uuid;
      e=std::get<c::CatalogNameEntry>(*example[1][1].name);
      e.name_entry_uuid=m.record.header.object_uuid;e.name_vector_uuid=definition.name_vector_uuid;
      e.object_uuid=definition.record.header.object_uuid;e.object_class=definition.object_subtype;
      e.security_policy_uuid=definition.security_policy_uuid;e.language_tag=alias?"de-DE":"en-US";
      e.name_class=alias?c::CatalogNameClass::alias:c::CatalogNameClass::primary;
      e.raw_name_text=e.display_name=(alias?"de_":"en_")+definition.object_subtype+"_"+std::to_string(at);
      e.default_for_object=!alias;e.default_for_language=true;
      // Supplied fixture profile-derived material, not production admission.
      e.normalized_lookup_key.assign(e.raw_name_text.begin(),e.raw_name_text.end());
      n.input_entries[alias]={&m,&e};
    }
    inputs.push_back({&definition,&n.vector_metadata,&n.vector,n.input_entries});
  }
  const c::CatalogNamedCreationLimits limits{4*inputs.size(),4*1024*1024};
  const auto assemble=[&](const auto& values,c::CatalogNamedCreationLimits bound) {
    return c::AssembleLocalNamedCreationRecords(request.bootstrap.database_uuid,
        request.creator.transaction_uuid,"en-US",values,bound);
  };
  const auto made=assemble(inputs,limits);
  Check(made.ok()&&made.records->definitions.size()==28&&made.records->names.size()==84,
      "all seven metric families have complete supplied default and localized alias names");
  const auto exact=c::CatalogNamedCreationLimits{limits.maximum_records,made.records->encoded_bytes};
  Check(assemble(inputs,exact).ok(),"exact complete encoded extent accepts the whole named package");
  const auto bytes_short=assemble(inputs,{limits.maximum_records,exact.maximum_encoded_bytes-1});
  const auto rows_short=assemble(inputs,{limits.maximum_records-1,exact.maximum_encoded_bytes});
  Check(bytes_short.error==E::resource_exhausted&&!bytes_short.records&&
      rows_short.error==E::resource_exhausted&&!rows_short.records,"one-short byte/record ceilings expose no partial package");
  {
    auto scoped=inputs;auto alias=names.back();
    for(unsigned i=0;i<2;++i)alias.input_entries[i]={&alias.metadata[i],&alias.entries[i]};
    alias.metadata[1].security_policy_uuid={UuidKind::object,Id(35000)};
    alias.metadata[1].owning_schema_uuid={UuidKind::schema,Id(35002)};
    alias.entries[1].security_policy_uuid=alias.metadata[1].security_policy_uuid;
    alias.entries[1].parent_schema_uuid=alias.metadata[1].owning_schema_uuid;
    alias.entries[1].scope_uuid={UuidKind::object,Id(35001)};
    scoped.back().entries=alias.input_entries;
    const auto retained=assemble(scoped,limits);
    Check(retained.ok()&&std::get<c::CatalogNameEntry>(*retained.records->names.back().name).scope_uuid.value==Id(35001),
        "supplied aliases retain independent scope/disclosure policy without inferring permission");
  }
  for(unsigned fault=0;fault<12;++fault) {
    constexpr std::array expected{E::invalid_request,E::invalid_request,E::invalid_names,E::invalid_names,
      E::invalid_names,E::invalid_names,E::invalid_names,E::duplicate_identity,E::duplicate_identity,
      E::invalid_metadata,E::invalid_metadata,E::duplicate_identity};
    auto bad=inputs;auto altered=names.back();auto definition=*bad.back().definition;
    for(unsigned i=0;i<2;++i)altered.input_entries[i]={&altered.metadata[i],&altered.entries[i]};
    bad.back()={&definition,&altered.vector_metadata,&altered.vector,altered.input_entries};
    if(fault==0)bad.back().definition=nullptr;
    if(fault==1)bad.back().entries={};
    if(fault==2)altered.vector.default_language_tag="fr-FR";
    if(fault==3)altered.entries[0].default_for_object=false;
    if(fault==4)altered.entries[1].default_for_object=true;
    if(fault==5)altered.entries[1].object_uuid.value=Id(30000);
    if(fault==6)altered.vector.object_uuid.value=Id(30000);
    if(fault==7)altered.metadata[1].record.header.row_uuid=altered.metadata[0].record.header.row_uuid;
    if(fault==8)altered.metadata[1].record.header.row_uuid.value=definition.record.header.object_uuid.value;
    if(fault==9)definition.creator_local_transaction_id=2;
    if(fault==10)altered.metadata[1].record.payload="unbound resident name";
    if(fault==11)bad.back()=bad.front(); // Repeated borrowed pointers are still duplicate objects.
    Arm();allocations=0;allocation_fault=1;count_allocations=true;
    const auto refused=assemble(bad,limits);count_allocations=false;allocation_fault=0;Disarm();
    Check(!refused.ok()&&!refused.records&&refused.error==expected[fault]&&allocations==0,
        "malformed final object/name/identity rejects before any owning allocation");
  }
  for(unsigned fault=0;fault<7;++fault) {
    auto database=request.bootstrap.database_uuid;auto transaction=request.creator.transaction_uuid;
    auto bound=limits;std::string_view language="en-US";std::span<const c::CatalogNamedCreationInput> values=inputs;
    if(fault==0)database.bytes[6]=0x41;
    if(fault==1)transaction.kind=UuidKind::row;
    if(fault==2)database=transaction.value;
    if(fault==3)bound.maximum_records=0;
    if(fault==4)bound.maximum_encoded_bytes=0;
    if(fault==5)language={};
    if(fault==6)values={};
    const auto refused=c::AssembleLocalNamedCreationRecords(database,transaction,language,values,bound);
    Check(refused.error==E::invalid_request&&!refused.records,"invalid creation context and missing limits are exact refusals");
  }
  bool completed=false;unsigned sites=0;
  for(unsigned at=1;at<4096;++at) {
    allocations=0;allocation_fault=at;count_allocations=true;
    const auto result=assemble(inputs,limits);count_allocations=false;allocation_fault=0;
    if(allocations<at) {Check(result.ok(),"named package allocation recovery succeeds");completed=true;break;}
    ++sites;Check(!result.ok()&&!result.records&&result.error==E::resource_exhausted,
        "every named package allocation failure is atomic with exact resource cause");
  }
  Check(completed&&sites>100,"complete named package allocation sweep exercises every real copy");
  // Cross the stack/scratch boundary with another complete language alias.
  // Large-cohort identity sorting and every scratch/output allocation are real.
  std::vector<c::CatalogMetadataVersion> extra_metadata(names.size());
  std::vector<c::CatalogNameEntry> extra_entries(names.size());
  std::vector<std::array<c::CatalogCreationNameEntryInput,3>> extra_inputs(names.size());
  auto large_inputs=inputs;
  for(unsigned i=0;i<names.size();++i) {
    auto& m=extra_metadata[i];auto& e=extra_entries[i];
    m=names[i].metadata[1];m.record.header.row_uuid={UuidKind::row,Id(22000+i)};
    m.record.header.object_uuid=m.default_name_uuid={UuidKind::object,Id(23000+i)};
    e=names[i].entries[1];e.name_entry_uuid=m.record.header.object_uuid;e.language_tag="fr-FR";
    e.display_name=e.raw_name_text="fr_"+std::to_string(i);
    e.normalized_lookup_key.assign(e.raw_name_text.begin(),e.raw_name_text.end());
    extra_inputs[i]={names[i].input_entries[0],names[i].input_entries[1],{&m,&e}};
    large_inputs[i].entries=extra_inputs[i];
  }
  const c::CatalogNamedCreationLimits large_limits{5*inputs.size(),limits.maximum_encoded_bytes};
  const auto large=assemble(large_inputs,large_limits);
  Check(large.ok()&&large.records->names.size()==112,"large native identity scratch preserves every language alias");
  auto duplicate=large_inputs;duplicate.back()=duplicate.front();
  const auto duplicate_result=assemble(duplicate,large_limits);
  Check(duplicate_result.error==E::duplicate_identity&&!duplicate_result.records,
      "large scratch path rejects repeated borrowed object/name identities");
  completed=false;unsigned large_sites=0;
  for(unsigned at=1;at<4096;++at) {
    allocations=0;allocation_fault=at;count_allocations=true;
    const auto result=assemble(large_inputs,large_limits);count_allocations=false;allocation_fault=0;
    if(allocations<at) {Check(result.ok(),"large named package allocation recovery succeeds");completed=true;break;}
    ++large_sites;Check(!result.ok()&&!result.records&&result.error==E::resource_exhausted,
        "every large scratch/output allocation failure exposes no partial records");
  }
  Check(completed&&large_sites>sites,"large package sweep includes identity scratch and all extra names");
  db::NativeCreationCatalogSeed seed;seed[0]=made.records->definitions;seed[1]=made.records->names;
  Fixture fixture;
  for(unsigned profile=0;profile<5;++profile) {
    const auto r=Request(profile,128);const auto size=r.bootstrap.page_size_bytes,budget=256*size;
    auto indexes=IndexedConstructionBindings();auto name_index=*indexes[0];
    name_index.relation_uuid=Id(21000);name_index.dependencies.index_uuid=Id(21001);
    indexes[1]=name_index;
    disk::FileDevice d;const auto path=fixture.Next();
    Check(d.Open(path.string(),disk::FileOpenMode::create_new).ok(),"named package actual owned file");
    const auto created=db::InitializeIndexedNativeCreationWorkspaceOnOpenDevice(d,r,seed,indexes,budget,*fixture.issuer);
    Check(created.ok()&&created.receipt->catalog_rows[0]==28&&created.receipt->catalog_rows[1]==84,
        "complete definitions and all supplied names actually publish on every page profile");
    Check(d.Close().ok()&&d.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"named package actual fresh reopen");
    const std::vector<disk::NativeFilespaceDevice> devices{{r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid,&d}};
    const auto definitions=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
        created.receipt->checkpoint,2,1,{indexes[0]->relation_uuid,indexes[0]->dependencies},budget);
    const auto localized=db::ReadNativeCommittedCatalogVersionsFromOpenDevices(r.bootstrap.database_uuid,devices,
        created.receipt->checkpoint,2,2,{indexes[1]->relation_uuid,indexes[1]->dependencies},budget);
    Check(definitions.ok()&&definitions.rows.size()==28&&localized.ok()&&localized.rows.size()==84,
        "real indexed catalog read retains the entire named definition set");
    for(const auto& row:localized.rows) {
      const auto original=std::find_if(seed[1].begin(),seed[1].end(),[&](const auto& value) {
        return value.metadata.record.header.object_uuid.value==row.metadata.record.header.object_uuid.value;
      });
      Check(original!=seed[1].end()&&row.name_payload&&
          c::CatalogNamePayloadPreservesIdentity(*original->name,*row.name_payload),"resident names retain actual original identities");
      const auto payload=[](const auto& value) {
        return std::visit([](const auto& name) {
          if constexpr(std::is_same_v<std::decay_t<decltype(name)>,c::CatalogNameVector>)return c::EncodeCatalogNameVector(name);
          else return c::EncodeCatalogNameEntry(name);
        },value);
      };
      const auto expected_payload=payload(*original->name),actual_payload=payload(*row.name_payload);
      Check(expected_payload.ok()&&actual_payload.ok()&&expected_payload.bytes==actual_payload.bytes,
          "exact supplied alias language/display/lookup/profile/policy fields survive storage");
    }
    const auto zero=disk::ReadFilespacePageZeroFromOpenDevice(d);Check(zero.ok(),"actual named checkpoint slots");
    const auto second=std::find_if(zero.record->roots.begin(),zero.record->roots.end(),[](const auto& root){return root.kind==19;});
    Check(second!=zero.record->roots.end(),"actual second selector declaration");
    const std::vector<byte> blank(size,0);
    Check(d.WriteAt(second->page_number*size,blank.data(),blank.size()).ok()&&d.Sync().ok(),"durable named package selector interruption");
    Check(d.Close().ok()&&d.Open(path.string(),disk::FileOpenMode::open_existing).ok(),"named recovery fresh owner");
    const auto recovered=db::RecoverIndexedNativeCreationWorkspaceSelectionOnOpenDevice(d,
        {r.bootstrap.database_uuid,r.bootstrap.filespace_uuid,r.bootstrap.page_size_profile_uuid},
        r.operation_uuid,seed,indexes,budget);
    Check(recovered.ok()&&recovered.repaired_slots==2&&recovered.receipt->catalog_rows[1]==84,
        "original named package recovers with both actual indexed catalog relations");
    Check(d.Close().ok(),"release named package actual owner");
  }
  std::cout<<"named creation allocation sites="<<sites<<" large_sites="<<large_sites<<'\n';
}
} // namespace
