// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
void TestNativeBlobInputBoundary() {
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
      published==1 && SelectCount(fixture,context)==3,
      "actual native BLOB input lost row counts/identity publication/visibility");
  Commit(context);
  const auto recovered=ReadPhysicalPage(fixture,fixture.physical_page);
  Require(recovered.visible_rows.size()==3,"native BLOB committed physical reopen lost rows");
  for(std::size_t r=0;r<expected.size();++r) {
    const auto row=std::find_if(recovered.visible_rows.begin(),recovered.visible_rows.end(),
        [&](const auto& stored){return stored.row_uuid.value==inserted.row_uuids[r];});
    Require(row!=recovered.visible_rows.end(),"native BLOB reopen lost actual row identity");
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
