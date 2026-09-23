// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/crud_support/crud_store.cpp"
#include "../support/binary_uuid_fixture.hpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <chrono>
namespace api = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
auto Id(unsigned n) { return scratchbird::tests::FixtureUuid(1192,n); }
int main() {
  auto identity=Id(1); identity.bytes[10]=0; identity.bytes[15]=255;
  api::CrudIndexRecord index;
  index.index_uuid=identity; index.table_uuid=Id(2); index.column_name="key";
  index.profile="btree"; index.family="btree"; index.key_envelopes={"key"};
  index.unique=true;
  const auto encoded=api::MakeCrudIndexCreateEventV2(7,index);
  std::vector<std::string> fields;
  Check(api::DecodeMgaMetadataFields(encoded,&fields) && api::ValidateCrudBinaryEvent(fields));
  Check(fields[3]==api::MetadataUuidBytes(identity) && fields[3].size()==16);
  Check(api::CrudBinaryIdentity(fields[3])==identity);
  auto bad=fields; bad[3]="019f0000-0000-7000-8000-000000000001";
  Check(!api::ValidateCrudBinaryEvent(bad));
  bad=fields;bad[2]="07";Check(!api::ValidateCrudBinaryEvent(bad));
  bad=fields;bad.push_back("unknown");Check(!api::ValidateCrudBinaryEvent(bad));
  Check(!api::ValidateCrudBinaryEvent({"SBCRUD2","TX_BEGIN","7","legacy"}));
  Check(api::ValidateCrudBinaryEvent({"SBCRUD2","TX_BEGIN","7",api::MetadataUuidBytes(Id(3))}));
  const auto locator=api::MakeCrudLargeValueLocator(identity,"digest",123);
  api::EngineUuid decoded;std::string digest;std::uint64_t size=0;
  Check(api::ParseCrudLargeValueLocator(locator,&decoded,&digest,&size));
  Check(decoded==identity && digest=="digest" && size==123);
  Check(!api::ParseCrudLargeValueLocator("@SB_OVERFLOW_V1:legacy:digest:123",&decoded,&digest,&size));
  for(std::size_t n=0;n<locator.size();++n)
    Check(!api::ParseCrudLargeValueLocator(locator.substr(0,n),&decoded,&digest,&size));
  const auto path=std::filesystem::temp_directory_path()/
      ("sb_crud_native_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  const auto write=[&](const std::string& bytes) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out.write(bytes.data(),bytes.size());Check(static_cast<bool>(out));
  };
  std::vector<std::vector<std::string>> records;
  write(encoded+encoded);
  Check(api::ReadCrudBinaryEvents(path.string(),&records) && records.size()==2 && records[1]==fields);
  write(encoded+encoded.substr(0,encoded.size()-1));
  Check(!api::ReadCrudBinaryEvents(path.string(),&records) && records.size()==2);
  auto tampered=encoded;tampered[30]^=1;write(tampered);
  Check(!api::ReadCrudBinaryEvents(path.string(),&records));
  write("SBCRUD1\tINDEX_CREATE\t7\tlegacy\n");
  Check(!api::ReadCrudBinaryEvents(path.string(),&records));
  std::filesystem::remove(path);
}
