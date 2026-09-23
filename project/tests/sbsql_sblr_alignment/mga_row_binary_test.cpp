// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/mga_relation_store/mga_row_codec.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
a::EngineUuid Id(unsigned value) {
  return a::EngineUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(value)}};
}
int main() {
  a::CrudRowVersionRecord row;
  row.table_uuid=Id(1);row.row_uuid=Id(2);row.version_uuid=Id(3);row.creator_tx=7;
  row.previous_version_uuid=Id(4);row.previous_sequence=9;row.temporary_session_uuid=Id(5);
  row.values={{"opaque",std::string("data\0\t\n;=",10)}};
  const auto bytes=a::BuildRowVersionStoreLine(row);
  Check(!bytes.empty());
  auto decode=[](const std::string& value,std::vector<a::CrudRowVersionRecord>* rows) {
    a::ScopedRelationSummary summary;
    return a::DecodeScopedRowBinaryBytes({value.begin(),value.end()},rows,&summary);
  };
  std::vector<a::CrudRowVersionRecord> decoded;
  Check(decode(bytes,&decoded)&&decoded.size()==1);
  Check(decoded[0].table_uuid==row.table_uuid&&decoded[0].row_uuid==row.row_uuid&&
        decoded[0].version_uuid==row.version_uuid&&decoded[0].previous_version_uuid==row.previous_version_uuid&&
        decoded[0].temporary_session_uuid==row.temporary_session_uuid&&decoded[0].values==row.values);
  for(std::size_t n=1;n<bytes.size();++n) {
    auto retained=decoded;
    Check(!decode(bytes.substr(0,n),&retained)&&retained.size()==decoded.size()&&retained[0].row_uuid==row.row_uuid);
  }
  for(unsigned version=1;version<=4;++version) {
    auto old=bytes;old[8]=static_cast<char>(version);old[9]=0;
    std::vector<a::CrudRowVersionRecord> out;Check(!decode(old,&out)&&out.empty());
  }
  auto tombstone=row;tombstone.deleted=true;tombstone.values.clear();
  tombstone.temporary_session_uuid={};tombstone.previous_version_uuid={};
  std::vector<a::CrudRowVersionRecord> removed;
  Check(decode(a::BuildRowVersionStoreLine(tombstone),&removed)&&removed.size()==1&&removed[0].deleted&&removed[0].values.empty());
  a::EngineTypedValue uuid;uuid.descriptor.canonical_type_name="uuid";
  uuid.binary_value.assign(row.row_uuid.bytes.begin(),row.row_uuid.bytes.end());
  a::EngineRowValue typed;typed.fields={{"id",uuid}};
  const std::vector<std::string> order{"id"};
  std::string compact;
  Check(a::AppendScopedRowIdentityBinaryBatch(&compact,{row},row.table_uuid,{},std::span<const a::EngineRowValue>(&typed,1),order,7,11));
  std::vector<a::CrudRowVersionRecord> compact_rows;
  Check(decode(compact,&compact_rows)&&compact_rows.size()==1&&compact_rows[0].event_sequence==11&&compact_rows[0].temporary_session_uuid.is_nil());
  Check(compact_rows[0].values[0].second==std::string(reinterpret_cast<const char*>(row.row_uuid.bytes.data()),16));
  std::string payload;
  uuid.binary_value.clear();uuid.encoded_value="01900000-0000-7000-8000-000000000002";
  Check(!a::ScopedRowBinaryCanonicalPayload(uuid,&payload));
  uuid.encoded_value.clear();uuid.binary_value.resize(15);
  Check(!a::ScopedRowBinaryCanonicalPayload(uuid,&payload));
}
