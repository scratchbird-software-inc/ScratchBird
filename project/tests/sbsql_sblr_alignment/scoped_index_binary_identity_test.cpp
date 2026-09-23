// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "mga_relation_store/mga_scoped_index_codec.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
namespace a = scratchbird::engine::internal_api;
static void Check(bool value, std::source_location where = std::source_location::current()) {
  if (!value) { std::cerr << "check failed at line " << where.line() << '\n'; std::abort(); }
}
static std::string Unhex(std::string_view hex) {
  std::string out;
  auto nibble=[](char c) { return c <= '9' ? c-'0' : c-'a'+10; };
  for (std::size_t n=0;n<hex.size();n+=2) out.push_back(static_cast<char>((nibble(hex[n])<<4)|nibble(hex[n+1])));
  return out;
}
static auto Bytes(const std::string& s) {
  return std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(s.data()),s.size());
}
static a::EngineUuid Id(unsigned n) {
  a::EngineUuid out;
  out.bytes = {1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)};
  return out;
}
int main() {
  a::MgaExactIndexEntryAppendBatch batch;
  batch.table_uuid=Id(1);batch.index.table_uuid=Id(1);batch.index.index_uuid=Id(2);
  batch.index.column_name="id";batch.index.family="btree";
  batch.entries.push_back({std::string("k\0x",3),std::string("\0p",2),Id(3),Id(4)});
  const auto golden=Unhex("53424d4942494e31020000000100000000000000090000000000000011000000000000000190000000007000800000000000000101900000000070008000000000000002020000006964050000006274726565050000006578616374030000006b00780200000000700190000000007000800000000000000301900000000070008000000000000004");
  const auto legacy=Unhex("53424d4942494e31010000000100000000000000090000000000000011000000000000002400000030313930303030302d303030302d373030302d383030302d3030303030303030303030312400000030313930303030302d303030302d373030302d383030302d303030303030303030303032020000006964050000006274726565050000006578616374030000006b00780200000000702400000030313930303030302d303030302d373030302d383030302d3030303030303030303030332400000030313930303030302d303030302d373030302d383030302d303030303030303030303034");
  std::string encoded;
  Check(a::AppendScopedExactIndexBinaryBatch(&encoded,batch,9,17));
  Check(encoded==golden && encoded.size()==137);
  std::string general_encoded;
  Check(a::AppendScopedIndexEntryBinaryRecord(&general_encoded,9,17,Id(2),Id(1),
      "id","btree","exact",std::string("k\0x",3),std::string("\0p",2),Id(3),Id(4)));
  Check(general_encoded==golden);
  Check(!a::AppendScopedIndexEntryBinaryRecord(&general_encoded,9,17,{},Id(1),
      "id","btree","exact","k","p",Id(3),Id(4)));
  Check(general_encoded==golden);
  for (const auto& data : {golden,golden+golden}) {
    std::vector<a::CrudIndexEntryRecord> rows;
    Check(a::DecodeScopedIndexBinaryBytes(Bytes(data),&rows));
    Check(rows.size()==(data.size()>golden.size()?2u:1u));
    for (const auto& row:rows) {
      Check(row.table_uuid==Id(1)&&row.index_uuid==Id(2)&&row.row_uuid==Id(3)&&row.version_uuid==Id(4));
      Check(row.creator_tx==9&&row.event_sequence==17&&row.sequence==17);
      Check(row.key_value==std::string("k\0x",3)&&row.payload_value==std::string("\0p",2));
    }
  }
  for (const auto& data : {golden,legacy}) {
    for (std::size_t size=1;size<data.size();++size) {
      std::vector<a::CrudIndexEntryRecord> rows(1);rows.front().table_uuid=Id(9);
      Check(!a::DecodeScopedIndexBinaryBytes(Bytes(data).first(size),&rows));
      Check(rows.size()==1&&rows.front().table_uuid==Id(9));
    }
  }
  for (unsigned field=0;field<4;++field) for(unsigned bit=0;bit<128;++bit) {
    auto changed=batch;
    auto* value=field==0?&changed.table_uuid:field==1?&changed.index.index_uuid:
                field==2?&changed.entries[0].row_uuid:&changed.entries[0].version_uuid;
    value->bytes[bit/8]^=1u<<(bit%8);
    if(field==0) changed.index.table_uuid=changed.table_uuid;
    std::string out="prefix";
    const bool valid=scratchbird::core::uuid::IsEngineIdentityUuid(*value);
    Check(a::AppendScopedExactIndexBinaryBatch(&out,changed,9,17)==valid);
    if(!valid) {Check(out=="prefix");continue;}
    std::vector<a::CrudIndexEntryRecord> rows;
    Check(a::DecodeScopedIndexBinaryBytes(Bytes(out).subspan(6),&rows)&&rows.size()==1);
    const auto actual=field==0?rows[0].table_uuid:field==1?rows[0].index_uuid:field==2?rows[0].row_uuid:rows[0].version_uuid;
    Check(actual==*value);
  }
  auto Reject=[&](std::string data) {
    std::vector<a::CrudIndexEntryRecord> rows(1);rows[0].row_uuid=Id(8);
    Check(!a::DecodeScopedIndexBinaryBytes(Bytes(data),&rows));
    Check(rows.size()==1&&rows[0].row_uuid==Id(8));
  };
  auto bad=golden;bad[8]=3;Reject(bad);
  bad=golden;bad[10]=1;Reject(bad);
  bad=golden;for(unsigned i=12;i<20;++i)bad[i]=static_cast<char>(255);Reject(bad);
  bad=golden;for(unsigned i=36;i<52;++i)bad[i]=0;Reject(bad);
  bad=legacy;bad[40]='x';Reject(bad);
  Reject(legacy);Reject(golden+legacy);Reject(legacy+golden);
  Reject(golden+"x");Reject(golden+legacy.substr(0,legacy.size()-1));
  std::string out="prefix";auto invalid=batch;invalid.index.table_uuid=Id(8);
  Check(!a::AppendScopedExactIndexBinaryBatch(&out,invalid,9,17)&&out=="prefix");
  invalid=batch;invalid.entries.push_back(invalid.entries.front());
  Check(!a::AppendScopedExactIndexBinaryBatch(&out,invalid,9,UINT64_MAX)&&out=="prefix");
  invalid=batch;invalid.entries.front().version_uuid={};
  Check(!a::AppendScopedExactIndexBinaryBatch(&out,invalid,9,17)&&out=="prefix");
  Check(!a::AppendScopedExactIndexBinaryBatch(nullptr,batch,9,17));
  Check(!a::DecodeScopedIndexBinaryBytes(Bytes(golden),nullptr));
  std::cout<<"scoped index binary16 and TEXT rejection passed\n";
}
