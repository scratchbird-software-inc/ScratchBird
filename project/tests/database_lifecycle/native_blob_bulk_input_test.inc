// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
void TestPhysicalBulkVersionIdentityProvenance() {
  namespace bulk=api::dml::detail;
  for (const platform::u32 profile : {8192u,16384u,32768u,65536u,131072u}) {
    auto fixture=MakeInt64IndexFixture("physical_version_provenance",1960,"int64",false,{},profile);
    auto context=Begin(fixture,"physical-version-provenance");
    api::dml::DirectPhysicalBulkAppendRequest issuer;
    issuer.context=context;issuer.target_table.uuid=fixture.table_uuid;
    const auto page=(std::filesystem::file_size(fixture.database_path)+profile-1)/profile+4;
    fixture.physical_page=page;
    const auto request_for=[&](bulk::DirectBulkUuidBatch& source, std::uint64_t destination,
                              std::uint32_t first_slot=1) {
      db::PhysicalMgaCowMutationBatchRequest batch;
      batch.version_identity_batch=&source.physical_versions;
      for (std::size_t i=0;i<source.row_uuids.size();++i) {
        db::PhysicalMgaCowMutationRequest row;
        row.database_path=fixture.database_path.string();row.relation_uuid=RelationUuid(fixture);
        row.row_uuid={platform::UuidKind::row,source.row_uuids[i]};
        row.transaction_uuid={platform::UuidKind::transaction,context.transaction_uuid};
        row.existing_local_transaction_id=scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id);
        row.use_existing_transaction=true;row.page_number=destination;row.stable_slot_id=first_slot+i;
        scratchbird::storage::page::RowDataCell cell;
        cell.column_ordinal=1;cell.value.type_id=dt::CanonicalTypeId::int64;
        cell.value.payload.assign(8,0);cell.value.payload[0]=i+1;
        row.cells.push_back(std::move(cell));batch.mutations.push_back(std::move(row));
      }
      return batch;
    };
    for (unsigned mutation=0;mutation<13;++mutation) {
      auto source=bulk::BuildDirectBulkUuidBatch(issuer,3);
      auto request=request_for(source,page);
      db::PhysicalMgaCowVersionBatch unbound;
      switch(mutation) {
        case 0: request.version_identity_batch=&unbound;break;
        case 1: request.mutations.front().relation_uuid.value.bytes.back()^=1;break;
        case 2: request.mutations.front().row_uuid.value.bytes.back()^=1;break;
        case 3: request.mutations.back().transaction_uuid.value.bytes.back()^=1;break;
        case 4: ++request.mutations.back().existing_local_transaction_id.value;break;
        case 5: request.mutations.back().use_existing_transaction=false;break;
        case 6: request.mutations.pop_back();break;
        case 7: std::swap(request.mutations.front(),request.mutations.back());break;
        case 8: source.version_uuids.front()=NewIdentity(platform::UuidKind::row,1970);break;
        case 9: source.row_uuids.front()=NewIdentity(platform::UuidKind::row,1971);
                request.mutations.front().row_uuid.value=source.row_uuids.front();break;
        case 10: request.mutations.front().kind=db::PhysicalMgaCowMutationKind::update;break;
        case 11: request.mutations.front().kind=db::PhysicalMgaCowMutationKind::delete_row;break;
        case 12: {
          auto foreign=issuer;foreign.context.database_uuid=NewIdentity(platform::UuidKind::database,1972);
          source=bulk::BuildDirectBulkUuidBatch(foreign,3);request=request_for(source,page);break;
        }
      }
      const auto bytes=std::filesystem::file_size(fixture.database_path);
      const auto refused=db::WritePhysicalMgaCowUnpublishedMutationBatch(std::move(request));
      Require(!refused.ok() && refused.row_receipts.empty() && refused.pages_written==0 &&
          std::filesystem::file_size(fixture.database_path)==bytes,
          "physical writer accepted substituted/default/foreign pre-issued identity provenance or wrote pages");
    }
    {
      auto failed_source=bulk::BuildDirectBulkUuidBatch(issuer,3);
      auto failed_request=request_for(failed_source,page);
      failed_request.mutations.front().cells.front().value.payload.pop_back();
      const auto bytes=std::filesystem::file_size(fixture.database_path);
      const auto refused=db::WritePhysicalMgaCowUnpublishedMutationBatch(failed_request);
      Require(!refused.ok() && refused.row_receipts.empty() && refused.pages_written==0 &&
          std::filesystem::file_size(fixture.database_path)==bytes,
          "malformed physical value published a consumed identity cohort");
      failed_request.mutations.front().cells.front().value.payload.push_back(0);
      const auto retry=db::WritePhysicalMgaCowUnpublishedMutationBatch(failed_request);
      Require(!retry.ok() && retry.row_receipts.empty() && retry.pages_written==0 &&
          std::filesystem::file_size(fixture.database_path)==bytes,
          "post-claim physical failure silently rearmed its issued identity cohort");
    }
    auto source=bulk::BuildDirectBulkUuidBatch(issuer,3);
    auto request=request_for(source,page);
    const auto staged=db::WritePhysicalMgaCowUnpublishedMutationBatch(request);
    Require(staged.ok() && staged.row_receipts.size()==3 && staged.written_rows==3,
        "actual sealed native version cohort failed physical staging");
    for (std::size_t i=0;i<3;++i) {
      const auto& receipt=staged.row_receipts[i];
      Require(receipt.version_uuid==source.version_uuids[i] && receipt.row_uuid.value==source.row_uuids[i] &&
          receipt.row_version==1 && receipt.database_uuid.value==fixture.database_uuid &&
          receipt.creator.transaction_uuid.value==context.transaction_uuid,
          "physical receipt substituted the actual pre-issued binary version identity");
    }
    // The same source is consumed even if its request projection is copied or
    // the enclosing owning batch is moved. Neither permits a second write.
    auto moved=std::move(source);request.version_identity_batch=&moved.physical_versions;
    for(auto& row:request.mutations) row.page_number=page+1;
    const auto bytes=std::filesystem::file_size(fixture.database_path);
    const auto replay=db::WritePhysicalMgaCowUnpublishedMutationBatch(request);
    Require(!replay.ok() && replay.row_receipts.empty() && replay.pages_written==0 &&
        std::filesystem::file_size(fixture.database_path)==bytes,
        "moving an already consumed native identity cohort rearmed it");
    auto concurrent=bulk::BuildDirectBulkUuidBatch(issuer,3);
    // Append into the existing page: physical page-generation changes must
    // preserve previously issued version identities and immutable ordinals.
    const auto concurrent_request=request_for(concurrent,page,4);
    auto first=std::async(std::launch::async,[&]{return db::WritePhysicalMgaCowUnpublishedMutationBatch(concurrent_request);});
    auto second=std::async(std::launch::async,[&]{return db::WritePhysicalMgaCowUnpublishedMutationBatch(concurrent_request);});
    const auto a=first.get(),b=second.get();
    Require(a.ok()!=b.ok() && (a.ok()?b.row_receipts.empty():a.row_receipts.empty()) &&
        (a.ok()?a.row_receipts.size():b.row_receipts.size())==3,
        "copied concurrent requests consumed a native identity cohort more than once");
    Commit(context);
    const auto reopened=ReadPhysicalPage(fixture,page);
    Require(reopened.visible_rows.size()==6,"committed native version cohorts failed actual physical reopen");
    for(const auto* identity : {&moved,&concurrent}) for(std::size_t i=0;i<3;++i) {
      const auto found=std::find_if(reopened.visible_rows.begin(),reopened.visible_rows.end(),
          [&](const auto& row){return row.row_uuid.value==identity->row_uuids[i];});
      Require(found!=reopened.visible_rows.end() && found->version_uuid==identity->version_uuids[i] &&
          found->row_version==1,"page-generation change altered a sealed version UUID or immutable chain ordinal");
    }
  }
}

void TestNativePhysicalFailureCannotFallback() {
#ifdef SCRATCHBIRD_CDP_NATIVE_ROW_IO_FAULT_TEST
  for(const auto size:{8192u,16384u,32768u,65536u,131072u}) {
    auto fixture=MakeInt64IndexFixture("native_optional_physical_failure",1970,"int64",false,{},size);
    // Reuse actual provisioned storage only after confirmed MGA rollback, not
    // a reset of observations or an unresolved-operation retry. Both real I/O
    // failures retain independent transactions/identities and full assertions.
    for(bool partial:{false,true}) {
    auto context=Begin(fixture,"native-optional-physical-failure");
    auto request=NativeRequest(fixture,context,{Int64IndexedRow(1),Int64IndexedRow(2),Int64IndexedRow(3)});
    for(auto& option:request.option_envelopes)
      if(option=="physical_mga_cow.required=true")option="physical_mga_cow.required=false";
    request.option_envelopes.push_back("physical_mga_cow.rows_per_page=1");
    struct stat file{};
    Require(::stat(fixture.database_path.c_str(),&file)==0,"native physical failure node identity unavailable");
    native_row_io_device=file.st_dev;native_row_io_inode=file.st_ino;
    native_row_io_offset.store(static_cast<off_t>((fixture.physical_page+1)*size),std::memory_order_release);
    struct Disarm {~Disarm(){native_row_io_fault.store(0,std::memory_order_release);}} disarm;
    native_row_io_fault.store(partial?2:1,std::memory_order_release);
    const auto refused=api::EngineExecuteNativeBulkIngest(request);
    Require(native_row_io_fault.load(std::memory_order_acquire)==0,
        "native optional physical write failure was not exercised");
    Require(!refused.ok&&!refused.diagnostics.empty(),
        "unresolved native publication became an optional-storage success");
    const auto evidence=[&](std::string_view kind,std::string_view value) {
      return HasEvidence(refused.evidence,kind,value);
    };
    Require(!evidence("direct_physical_bulk_row_page_writer","fallback")&&
        evidence("direct_physical_bulk_row_page_write_calls","2")&&
        evidence("direct_physical_bulk_row_page_complete_writes","1")&&
        evidence("direct_physical_bulk_row_page_write_bytes_observed",std::to_string(size+(partial?size/2:0))),
        "native failure lost exact partial-I/O observations or claimed a fallback");
    const auto inventory=db::AcquireStrongLocalTransactionInventorySnapshot(context.database_path);
    Require(inventory.ok(),"native failed-batch inventory could not be reconciled");
    const auto transaction=scratchbird::transaction::mga::LookupLocalTransaction(inventory.snapshot->inventory,
        scratchbird::transaction::mga::MakeLocalTransactionId(context.local_transaction_id));
    Require(transaction.ok()&&(transaction.entry.rollback_only||
        transaction.entry.state==scratchbird::transaction::mga::TransactionState::rolled_back),
        "native partial write lost the actual noncommittable MGA fence");
    api::EngineCommitTransactionRequest commit;commit.context=context;
    Require(!api::EngineCommitTransaction(commit).ok,"native failed physical prefix could be committed");
    if(transaction.entry.state!=scratchbird::transaction::mga::TransactionState::rolled_back)
      Rollback(context);
    auto reader=Begin(fixture,"native-failed-prefix-reader");
    Require(SelectCount(fixture,reader)==0,"native failed physical prefix escaped as visible rows");
    Rollback(reader);
    // NativeRequest selects a fresh component destination beyond the current
    // actual file end. Never overwrite an old prefix or new inventory pages.
    fixture.physical_page=0;
    }
  }
#endif
}

void TestNativeBlobInputBoundary() {
  TestNativePhysicalFailureCannotFallback();
  TestPhysicalBulkVersionIdentityProvenance();
  auto fixture=MakeDescriptorPayloadFixture("native_blob_inputs",1910);
  auto context=Begin(fixture,"native-blob-input-boundary");
  const auto& types=DescriptorPayloadTypeNames();
  const auto type=std::find(types.begin(),types.end(),"blob");
  Require(type!=types.end(),"native BLOB fixture type absent");
  const auto field=static_cast<std::size_t>(type-types.begin());
  auto expected=DescriptorPayloadRows();
  expected[1].fields[field].second.binary_value.clear(); // empty VALUE, not NULL
  auto null_row=DescriptorPayloadRow(3);
  null_row.fields[field].second.binary_value.clear();
  null_row.fields[field].second.setState(api::EngineValueState::sql_null);
  expected.push_back(std::move(null_row));
  auto request=NativeRequest(fixture,context,expected);
  auto foreign=NativeRequest(fixture,context,expected);
  Require(request.native_blob_operands.size()==3 && request.blob_sources.size()==3,
      "native BLOB fixture did not retain three actual opaque statement inputs");
  unsigned published=0;
  request.before_row_publication=[&](auto,auto,auto) {
    ++published;
    return api::MakeEngineApiDiagnostic("SB_ENGINE_API_OK","engine.api.ok",{},false);
  };
  for(unsigned mutation=0;mutation<21;++mutation) {
    auto bad=request;
    api::StatementBlobRowValueV3 unbound;
    switch(mutation) {
      case 0: bad.native_blob_operands.clear(); break;
      case 1: bad.native_blob_operands.insert(bad.native_blob_operands.begin(),bad.native_blob_operands.front()); break;
      case 2: std::reverse(bad.native_blob_operands.begin(),bad.native_blob_operands.end()); break;
      case 3: bad.native_blob_operands.back().row_index=expected.size(); break;
      case 4: bad.native_blob_operands.back().field_index=expected.back().fields.size(); break;
      case 5: bad.native_blob_operands.front().source=nullptr; break;
      case 6: bad.native_blob_operands.front().column_uuid.bytes.back()^=1; break;
      case 7: ++bad.native_blob_operands.front().relation_generation; break;
      case 8: ++bad.native_blob_operands.front().column_generation; break;
      case 9: bad.canonical_rows.front().fields[field].second.binary_value={0}; break;
      case 10: bad.canonical_rows.front().fields[field].second.encoded_value="not a native BLOB"; break;
      case 11: bad.canonical_rows.front().fields[field].second.setState(api::EngineValueState::missing); break;
      case 12: ++bad.canonical_rows.front().fields[field].second.descriptor.datatype_cohort.catalog_generation; break;
      case 13: bad.canonical_rows.front().fields[field].second.descriptor.charset_uuid=fixture.table_uuid; break;
      case 14: bad.canonical_rows.front().fields[field].second.descriptor.encoded_descriptor+=";unknown=0"; break;
      case 15: bad.native_blob_operands.front().source=foreign.native_blob_operands.front().source; break;
      case 16: bad.canonical_rows.front().fields[field].second.setState(api::EngineValueState::sql_null); break;
      case 17: bad.native_blob_operands.front().source=&unbound; break;
      case 18: {
        Require(field!=0,"native BLOB extra operand fixture needs another column");
        auto extra=bad.native_blob_operands.front();extra.field_index=0;
        bad.native_blob_operands.insert(bad.native_blob_operands.begin(),extra);
        break;
      }
      case 19:
        bad.canonical_rows.back().fields[field].second.state=api::EngineValueState::value;
        bad.canonical_rows.back().fields[field].second.is_null=true;
        break;
      case 20: bad.native_row_packet.present=true;break;
    }
    const auto refused=api::EngineExecuteNativeBulkIngest(bad);
    if(refused.ok || refused.diagnostics.empty())std::cerr<<"native BLOB bad input mutation="<<mutation<<'\n';
    Require(!refused.ok && !refused.diagnostics.empty() && refused.diagnostics.front().error &&
        !refused.diagnostics.front().code.empty() && refused.accepted_rows==0 && refused.inserted_rows==0 &&
        refused.row_uuids.empty() && published==0 && SelectCount(fixture,context)==0,
        "native BLOB malformed/stale/foreign input published rows or reached identity publication");
    if(mutation==0)Require(refused.diagnostics.front().code=="BLOB.V1_V2_REFUSED",
        "missing actual native BLOB owner lost exact legacy refusal");
  }
  {
    auto large=DescriptorPayloadRow(4);
    large.fields[field].second.binary_value.resize(8*1024*1024,0xa5);
    auto small=DescriptorPayloadRow(5);
    small.fields[field].second.binary_value.clear();
    api::StatementBlobInputBoundsV3 bounds;
    bounds.operation=SB_BLOB_OP_READ_V3;bounds.resident_bytes=20*1024*1024;
    bounds.valid_for_ns=600'000'000'000ULL;bounds.retain_tickets=4;bounds.access_tickets=128;
    bounds.maximum_work.fill(64*1024*1024);bounds.scratch_bytes=8192;bounds.window_bytes=4096;
    bounds.relative_timeout_ns=60'000'000'000ULL;
    auto windowed=NativeRequest(fixture,context,{small,large},&bounds);
    const auto prototypes=windowed.canonical_rows;
    const auto operands=windowed.native_blob_operands;
    windowed.canonical_rows.clear();windowed.native_blob_operands.clear();
    for(std::size_t r=0;r<5;++r) {
      // The large owner appears LAST. Averaging placeholder or source sizes
      // would incorrectly admit all five rows into one copy window.
      const auto which=r==4?1u:0u;
      windowed.canonical_rows.push_back(prototypes[which]);
      windowed.canonical_rows.back().requested_row_uuid=NewIdentity(platform::UuidKind::row,2000+r);
      auto binding=operands[which];binding.row_index=r;windowed.native_blob_operands.push_back(binding);
    }
    windowed.estimated_row_count=5;
    std::size_t admitted_window=0;
    windowed.before_row_publication=[&](auto rows,auto,auto) {
      admitted_window=rows.size();
      // Real owned callback refusal before row copying, not a success stub.
      return api::MakeInvalidRequestDiagnostic("native_blob_window_fixture","intentional_before_row_copy_refusal");
    };
    const auto refused=api::EngineExecuteNativeBulkIngest(windowed);
    Require(!refused.ok && admitted_window==3 && SelectCount(fixture,context)==0,
        "native BLOB window sizing ignored actual sealed payload extent or retained failed-window rows");
  }
  // Cancel the real source AFTER initial origin/profile/lifetime admission and
  // row identity allocation, before row copying. The synchronous unwind must
  // retain the actual typed native cause through the bulk savepoint rollback.
  {
    auto failing=NativeRequest(fixture,context,expected);
    std::optional<api::RetainedStatementCancellationV1> cancelled;
    Require(api::RetainStatementCancellationV1(failing.statement_receipt(),failing.context.session_uuid.bytes,
        failing.context.statement_receipt_uuid.bytes,&cancelled)==SB_ENGINE_STATUS_OK && cancelled.has_value(),
        "native BLOB cancellation test could not retain its actual statement signal");
    failing.before_row_publication=[&](auto,auto,auto) {
      Require(cancelled->Request(),"native BLOB actual statement cancellation request failed");
      return api::MakeEngineApiDiagnostic("SB_ENGINE_API_OK","engine.api.ok",{},false);
    };
    const auto failure=api::EngineExecuteNativeBulkIngest(failing);
    Require(!failure.ok && failure.native_blob_lifetime_diagnostic &&
        failure.diagnostics.front().code=="PROCESS.CANCELLED" && SelectCount(fixture,context)==0,
        "native BLOB late cancellation lost its actual cause or retained visible rows");
    auto copied=failure;
    const auto& native=*copied.native_blob_lifetime_diagnostic;
    Require(!native.lifetime.ok() && native.row_index==0 && native.field_index==field &&
        native.lifetime.diagnostic_code=="PROCESS.CANCELLED" &&
        native.lifetime.fact.fact_class==dt::BlobLifetimeFactClassV3::cancellation &&
        native.lifetime.fact.diagnostic==dt::BlobLifetimeDiagnosticCodeV3::process_cancelled &&
        native.lifetime.diagnostic_code.data()!=failure.diagnostics.front().code.data(),
        "native BLOB typed diagnostic was flattened, relabelled or borrowed from its renderer");
  }
  const auto inserted=api::EngineExecuteNativeBulkIngest(request);
  RequireOk(inserted,"actual native BLOB input bulk mutation failed");
  Require(inserted.inserted_rows==3 && inserted.accepted_rows==3 && inserted.row_uuids.size()==3 &&
      inserted.row_version_uuids.size()==3 &&
      published==1 && SelectCount(fixture,context)==3,
      "actual native BLOB input lost row counts/identity publication/visibility");
  Commit(context);
  const auto logical=api::LoadMgaRelationStoreState(BaseContext(fixture,"native-blob-version-reopen"));
  Require(logical.ok,"native BLOB committed logical versions could not be reopened");
  const auto recovered=ReadPhysicalPage(fixture,fixture.physical_page);
  Require(recovered.visible_rows.size()==3,"native BLOB committed physical reopen lost rows");
  for(std::size_t r=0;r<expected.size();++r) {
    const auto row=std::find_if(recovered.visible_rows.begin(),recovered.visible_rows.end(),
        [&](const auto& stored){return stored.row_uuid.value==inserted.row_uuids[r];});
    Require(row!=recovered.visible_rows.end(),"native BLOB reopen lost actual row identity");
    Require(row->version_uuid==inserted.row_version_uuids[r] && row->row_version==1,
        "native BLOB physical reopen has a different version identity than native DML publication");
    const auto logical_row=std::find_if(logical.state.row_versions.begin(),logical.state.row_versions.end(),
        [&](const auto& stored){return stored.row_uuid==inserted.row_uuids[r];});
    Require(logical_row!=logical.state.row_versions.end() && logical_row->version_uuid==row->version_uuid,
        "native BLOB logical and physical MGA reopen disagree on the true version identity");
    const auto cell=std::find_if(row->cells.begin(),row->cells.end(),
        [&](const auto& stored){return stored.column_ordinal==field+1;});
    const auto& golden=expected[r].fields[field].second;
    Require(cell!=row->cells.end() && !cell->external_value && cell->value.type_id==dt::CanonicalTypeId::blob &&
        cell->value.is_null==golden.isSqlNull() && cell->value.payload==golden.binary_value,
        "native BLOB persistence changed exact logical octets/empty VALUE/NULL under native column receiver");
  }
  {
    auto metadata=Begin(fixture,"native-blob-btree-refusal");
    const auto index=DescriptorPayloadUniqueIndex(fixture,metadata,DescriptorPayloadColumnName(field),1930);
    Require(!api::AppendMgaIndexMetadata(metadata,index).error,
        "native BLOB ordered refusal fixture could not stage its structural index definition");
    auto attempt=NativeRequest(fixture,metadata,{DescriptorPayloadRow(5)});
    unsigned reached_publication=0;
    attempt.before_row_publication=[&](auto,auto,auto) {
      ++reached_publication;
      return api::MakeEngineApiDiagnostic("SB_ENGINE_API_OK","engine.api.ok",{},false);
    };
    const auto refused=api::EngineExecuteNativeBulkIngest(attempt);
    RequireDiagnostic(refused,"BLOB.ORDERING_REFUSED",{},"native BLOB acquired a B-tree key through raw input admission");
    Require(reached_publication==0 && SelectCount(fixture,metadata)==3 &&
        std::any_of(refused.diagnostics.front().identity_fields.begin(),refused.diagnostics.front().identity_fields.end(),
            [&](const auto& item){return item.first=="index_uuid" && item.second==index.index_uuid;}),
        "native BLOB B-tree refusal lost actual index identity or changed rows");
    Rollback(metadata); // structural refusal fixture is not a built index
  }
  std::cout<<"native_blob_input=passed malformed_stale_foreign_origin_live_cancellation_exact_commit_reopen\n";
}
