// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/query/narrow_query_profile_source.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
namespace a=scratchbird::engine::internal_api;
namespace w=scratchbird::wire;
static void Check(bool value) { if (!value) std::abort(); }
int main() {
  auto id=scratchbird::tests::FixtureUuid(1031,1); id.bytes[9]=0xff;
  Check(a::NativeUuid(id.bytes)==id);
  a::SourceState source;
  a::BoundColumn column;column.column_uuid=id;column.ordinal=3;
  source.columns.push_back(column);
  Check(a::FindColumnIndex(source,id.bytes,3)==0);
  Check(!a::FindColumnIndex(source,id.bytes,4));
  auto other=id;other.bytes[15]^=1;
  Check(!a::FindColumnIndex(source,other.bytes,3));
  a::datatypes::DatatypeTypeCodecIdentityRowV1 datatype;
  datatype.descriptor_uuid=id;datatype.type_uuid=other;
  datatype.descriptor_generation=2;datatype.type_generation=3;
  datatype.canonical_binary_type_code=4;datatype.codec_id="codec";
  datatype.codec_version=5;datatype.codec_generation=6;datatype.canonical_value_bytes=8;
  w::NarrowQueryOutputOccurrence output;
  output.datatype_descriptor_uuid=id.bytes;output.datatype_type_uuid=other.bytes;
  output.datatype_descriptor_generation=2;output.datatype_type_generation=3;
  output.datatype_binary_type_code=4;output.codec_id="codec";
  output.codec_version=5;output.codec_generation=6;output.canonical_value_bytes=8;
  output.null_encoding=1;output.nullability=1;
  Check(a::OutputMatchesDatatype(output,datatype,true));
  output.datatype_type_uuid=id.bytes;
  Check(!a::OutputMatchesDatatype(output,datatype,true));
  a::SourceRow row;row.row_uuid=id;row.version_uuid=other;
  std::uint64_t dynamic_bytes=0;
  Check(a::RowMemory(row,&dynamic_bytes) && dynamic_bytes==0);
  const auto diagnostic=a::Diagnostic("code","key",id);
  Check(diagnostic.detail.empty() && diagnostic.identity_fields.size()==1);
  Check(diagnostic.identity_fields.front().second==id);
}
