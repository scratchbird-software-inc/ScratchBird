// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
// Representation-only fixtures. No caller-built metadata is live DML authority.
void BlobReceiverConstruction() {
  namespace page = scratchbird::storage::page;
  dt::DatatypeStorageIdentityV3 identity;
  Check(dt::LookupDatatypeStorageIdentityV3(dt::kBlobV11ReceiptUuid,11,11,
      scratchbird::core::platform::Uuid{{0x01,0x9d,0,0,0,0,0x70,0,0x80,0,0,0,0,0,0xd8,0x30}},1,&identity),
      "BLOB receiver exact V11 identity absent");
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid = identity.descriptor_uuid;
  descriptor.descriptor_uuid.bytes.back() ^= 0x20; // independent occurrence
  descriptor.datatype_descriptor_uuid = identity.descriptor_uuid;
  descriptor.datatype_descriptor_generation = identity.descriptor_generation;
  descriptor.type_uuid = identity.type_uuid;
  descriptor.datatype_cohort = {dt::kBlobV11ReceiptUuid,11,11};
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = "blob";
  auto column = descriptor.descriptor_uuid;
  column.bytes.back() ^= 0x10;
  for (const bool nullable : {false,true}) {
    descriptor.encoded_descriptor = nullable ? "nullability=nullable" : "nullability=non_null";
    page::RowDataProfileColumnBinding original;
    api::EngineApiDiagnostic diagnostic;
    Check(bulk::DirectBuildBlobPageColumn(descriptor,identity,7,nullable,column,23,&original,&diagnostic),
        "native BLOB column receiver refused exact binding");
    Check(original.blob && dt::ValidateBlobProfileHandleV3(*original.blob).ok() &&
        original.column_ordinal==7 && original.null_allowed==nullable &&
        original.column_uuid==column && original.column_generation==23 &&
        !original.date && !original.time && !original.timestamp && !original.interval && !original.bit_string,
        "native BLOB column receiver lost exact identity/generation/profile");
    // Feed the actual engine-produced receiving profile to the native row
    // codec. These are stored bytes, deliberately not a live value fixture.
    for (unsigned state=0;state<4;++state) {
      if(state==1 && !nullable) continue;
      page::RowDataPageBody body;
      auto id=column;
      const auto distinct=[&](unsigned suffix) { auto next=id; next.bytes.back()=suffix; return next; };
      body.relation_uuid={scratchbird::core::platform::UuidKind::object,distinct(0x61)};
      body.segment_id=1; body.segment_generation=2;
      body.page_number=21; body.page_generation=7; body.compaction_generation=3;
      page::RowDataRecord row;
      row.row_uuid={scratchbird::core::platform::UuidKind::row,distinct(0x62)};
      row.version_uuid=distinct(0x63);
      row.transaction_uuid={scratchbird::core::platform::UuidKind::transaction,distinct(0x64)};
      row.local_transaction_id=1; row.storage_generation=1;
      row.stable_slot_id=1; row.internal_row_ordinal=1;
      page::RowDataCell cell; cell.column_ordinal=7;
      if(state==3) cell.external_value=page::RowExternalValueLocator{distinct(0x65),123,12000};
      else {
        cell.value.type_id=dt::CanonicalTypeId::blob; cell.value.is_null=state==1;
        if(state==2) cell.value.payload={0,255,'S','B','B','L','O','B','0','3',0,0x80};
      }
      row.cells.push_back(cell); body.rows.push_back(std::move(row));
      const page::RowDataProfileReceiver receiving{body.relation_uuid,{&original,1},31};
      const auto encoded=page::BuildRowDataPageBody(body,8192,receiving);
      Check(encoded.ok(),"engine BLOB receiver could not encode native stored representation");
      const auto decoded=page::ParseRowDataPageBody(encoded.serialized,21,receiving);
      Check(decoded.ok() && decoded.body.rows.size()==1 && decoded.body.rows[0].cells.size()==1 &&
          decoded.body.rows[0].cells[0].external_value==cell.external_value &&
          decoded.body.rows[0].cells[0].value.is_null==cell.value.is_null &&
          decoded.body.rows[0].cells[0].value.payload==cell.value.payload,
          "engine BLOB receiver changed logical octets,empty,NULL or locator");
      auto stale=original; ++stale.column_generation;
      auto wrong=receiving; wrong.columns={&stale,1};
      Check(!page::ParseRowDataPageBody(encoded.serialized,21,wrong).ok(),
          "stored BLOB reopened under wrong actual column generation");
      Check(!page::ParseRowDataPageBody(encoded.serialized,21).ok(),
          "engine BLOB receiver enabled an unbound physical reopen");
    }
    const auto unchanged = [&](const auto& output) {
      return output.blob==original.blob && output.column_uuid==original.column_uuid &&
          output.column_generation==original.column_generation && output.column_ordinal==original.column_ordinal &&
          output.null_allowed==original.null_allowed && output.date==original.date &&
          output.time==original.time && output.timestamp==original.timestamp &&
          output.interval==original.interval && output.bit_string==original.bit_string;
    };
    auto output=original;
    for (unsigned mutation=0;mutation<33;++mutation) {
      auto invalid=descriptor;
      auto invalid_identity=identity;
      auto invalid_column=column;
      std::uint64_t generation=23;
      std::uint16_t ordinal=9;
      switch(mutation) {
        case 0: invalid.descriptor_uuid={}; break;
        case 1: invalid.descriptor_kind="row"; break;
        case 2: invalid.datatype_descriptor_uuid.bytes.back()^=1; break;
        case 3: ++invalid.datatype_descriptor_generation; break;
        case 4: invalid.type_uuid.bytes.back()^=1; break;
        case 5: invalid.datatype_cohort.catalog_snapshot_uuid.bytes.back()^=1; break;
        case 6: ++invalid.datatype_cohort.catalog_generation; break;
        case 7: ++invalid.datatype_cohort.registry_generation; break;
        case 8: invalid.charset_uuid=column; break;
        case 9: invalid.collation_uuid=column; break;
        case 10: invalid.encoded_descriptor+=";precision=0"; break;
        case 11: invalid.encoded_descriptor+=";length=0"; break;
        case 12: invalid.encoded_descriptor+=";unknown=0"; break;
        case 13: invalid.encoded_descriptor+=";codec_generation=2"; break;
        case 14: invalid.encoded_descriptor+=";canonical=integer"; break;
        case 15: invalid.encoded_descriptor+=";nullable=true"; break;
        case 16: invalid.encoded_descriptor+=";not_null="+std::string(nullable?"true":"false"); break;
        case 17: invalid.encoded_descriptor="nullability=bad"; break;
        case 18: invalid_column={}; break;
        case 19: invalid_column.bytes[6]=0x40; break;
        case 20: generation=0; break;
        case 21: ordinal=0; break;
        case 22: invalid_identity.descriptor_uuid.bytes.back()^=1; break;
        case 23: ++invalid_identity.descriptor_generation; break;
        case 24: invalid_identity.type_uuid.bytes.back()^=1; break;
        case 25: invalid_identity.type_id=dt::CanonicalTypeId::binary; break;
        case 26: invalid_identity.codec.reset(); break;
        case 27: ++invalid_identity.codec->native_fields.canonical_value_maximum_bytes; break;
        case 28: ++invalid_identity.codec->legacy_fields.codec_generation; break;
        case 29: invalid.datatype_cohort={dt::kDatatypeCohortV10,10,10}; break;
        case 30: invalid.encoded_descriptor+=";codec_id=unknown"; break;
        case 31: invalid.encoded_descriptor+=";scale=0"; break;
        case 32: invalid.encoded_descriptor+=";container_rank=0"; break;
      }
      output=original;
      diagnostic={};
      Check(!bulk::DirectBuildBlobPageColumn(invalid,invalid_identity,ordinal,nullable,
          invalid_column,generation,&output,&diagnostic) && unchanged(output) &&
          diagnostic.error && diagnostic.code=="DATATYPE.DESCRIPTOR.INVALID",
          "invalid BLOB receiver replaced output or lost diagnostic");
    }
    output=original;
    Check(!bulk::DirectBuildBlobPageColumn(descriptor,identity,9,!nullable,column,23,&output,&diagnostic) && unchanged(output),
        "BLOB receiving nullability contradiction ignored");
    Check(!bulk::DirectBuildBlobPageColumn(descriptor,identity,9,nullable,column,23,nullptr,&diagnostic),
        "BLOB receiver null output admitted");
    // The receiver has no label-based type dispatch. Actual UUID/codec binding
    // remains unchanged by a presentation alias or absent redundant nullable.
    auto alias=descriptor;
    alias.canonical_type_name="localized_blob_alias";
    alias.encoded_descriptor="canonical=localized_blob_alias";
    Check(bulk::DirectBuildBlobPageColumn(alias,identity,65535,nullable,column,29,&output,&diagnostic) &&
        output.column_ordinal==65535 && output.column_generation==29 && output.null_allowed==nullable,
        "native BLOB receiver inferred identity from display label or lost typed nullability");
    std::size_t allocations=0;
    allocation_counter=&allocations;
    const bool counted=bulk::DirectBuildBlobPageColumn(descriptor,identity,9,nullable,column,29,&output,&diagnostic);
    allocation_counter=nullptr;
    Check(counted && allocations>0 && allocations<1000,"BLOB receiver allocation sweep empty/unbounded");
    for (std::size_t position=0;position<allocations;++position) {
      output=original; diagnostic={};
      const auto before=allocation_faults;
      allocation_failure_after=static_cast<long>(position);
      bool accepted=false,threw=false;
      try { accepted=bulk::DirectBuildBlobPageColumn(descriptor,identity,9,nullable,column,29,&output,&diagnostic); }
      catch (...) { threw=true; }
      allocation_failure_after=-1;
      Check(allocation_faults==before+1 && !accepted && !threw && unchanged(output) &&
          diagnostic.error && diagnostic.code=="RESOURCE.BUDGET_EXCEEDED",
          "BLOB receiver allocation failure lost atomicity or resource diagnostic");
      Check(bulk::DirectBuildBlobPageColumn(descriptor,identity,9,nullable,column,29,&output,&diagnostic) &&
          output.column_generation==29,"BLOB receiver fresh retry failed");
    }
    // The explicit stored-representation converter is not live source
    // admission. Check its state, payload and locator handling independently.
    for(unsigned state=0;state<4;++state) {
      api::EngineTypedValue typed;typed.descriptor=descriptor;
      api::CrudStoredValue stored;
      if(state==1) {typed.setState(api::EngineValueState::sql_null);stored=api::CrudStoredValue::SqlNull();}
      if(state==2)stored=api::CrudStoredValue(std::string("\0\xff\x80",3));
      if(state==3) {
        const auto locator=page::EncodeRowExternalValueLocator({column,123,12000});
        Check(locator.has_value(),"stored BLOB locator fixture invalid");
        stored={api::EngineValueState::lob_handle,std::string(locator->begin(),locator->end())};
      }
      api::EngineRowValue row;row.fields.push_back({"v",typed});
      const auto encoder=Plan(typed);
      api::CrudValueFields values{{"v",stored}};
      const auto physical=bulk::DirectPhysicalCellsFromTypedInputRow(row,encoder,nullptr,&values);
      Check(physical.cells.size()==1 && physical.cells[0].column_ordinal==1 &&
          physical.null_cells==(state==1?1u:0u) && physical.typed_binary_cells==(state==1?0u:1u) &&
          (state==1 || physical.typed_cell_counts.at(dt::CanonicalTypeId::blob)==1),
          "stored BLOB conversion shape/state/type accounting drifted");
      if(state==3)Check(physical.cells[0].external_value.has_value() &&
          physical.cells[0].external_value->logical_bytes==12000,"stored BLOB lost exact external locator");
      else Check(physical.cells[0].value.type_id==dt::CanonicalTypeId::blob &&
          physical.cells[0].value.is_null==typed.isSqlNull() &&
          std::string(physical.cells[0].value.payload.begin(),physical.cells[0].value.payload.end())==stored.bytes,
          "stored BLOB conversion changed exact bytes/empty/NULL");
      for(unsigned mutation=0;mutation<7;++mutation) {
        auto bad_row=row;auto bad_values=values;
        if(mutation==0)bad_values.clear();
        if(mutation==1)bad_values.push_back(bad_values.front());
        if(mutation==2)bad_values.front().second={api::EngineValueState::missing,{}};
        if(mutation==3)bad_row.fields.front().second.binary_value={0};
        if(mutation==4)bad_row.fields.front().second.encoded_value="shadow";
        if(mutation==5)bad_values.front().second={api::EngineValueState::lob_handle,std::string(24,'\0')};
        if(mutation==6)bad_row.fields.front().second.setState(typed.isSqlNull()?api::EngineValueState::value:api::EngineValueState::sql_null);
        bool refused=false;
        try{(void)bulk::DirectPhysicalCellsFromTypedInputRow(bad_row,encoder,nullptr,&bad_values);}
        catch(const std::invalid_argument&){refused=true;}
        Check(refused,"stored BLOB conversion admitted dirty/ambiguous/mismatched or legacy locator state");
      }
    }
    // Representation admission never enables a caller's raw BLOB as a live
    // generic V1/V2 value, including empty VALUE or contextual SQL NULL.
    for (unsigned state=0;state<3;++state) {
      api::EngineTypedValue generic;
      generic.descriptor=descriptor;
      if(state==1) generic.state=api::EngineValueState::sql_null;
      if(state==2) generic.binary_value={0,255,0x80,0};
      std::vector<scratchbird::core::platform::byte> packed{0x5a};
      Check(!bulk::DirectPackTypedPayload(dt::CanonicalTypeId::blob,generic,&packed) &&
          packed==std::vector<scratchbird::core::platform::byte>{0x5a},
          "BLOB receiver enabled generic V1/V2 or modified refused output");
      bool refused=false;
      try { (void)bulk::DirectStoredValueForColumn(generic,dt::CanonicalTypeId::blob); }
      catch(const std::invalid_argument&) { refused=true; }
      Check(refused,"generic BLOB became retained row data without native input admission");
      api::EngineRowValue row; row.fields.push_back({"v",generic});
      const auto encoder=Plan(generic);
      const auto plan=bulk::BuildDirectFixedWidthPayloadValidationPlan(encoder,&row);
      for (const auto* selected : {static_cast<const std::vector<bulk::DirectFixedWidthPayloadValidationColumnPlan>*>(nullptr),&plan}) {
        refused=false;
        try { (void)bulk::DirectPhysicalCellsFromTypedInputRow(row,encoder,selected); }
        catch(const std::invalid_argument&) { refused=true; }
        Check(refused,"generic BLOB became physical row data without native input admission");
      }
    }
  }
}
