// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "../../src/engine/internal_api/management/memory_management_api.cpp"
#include <cstdlib>
#include <iostream>
#include <source_location>
#include <unistd.h>
namespace a = scratchbird::engine::internal_api;
void Check(bool ok, std::source_location at = std::source_location::current()) {
  if (!ok) { std::cerr << "failure at " << at.line() << '\n'; std::abort(); }
}
a::EngineUuid Id(unsigned n) {
  return a::EngineUuid{{1,144,0,0,0,0,112,0,128,0,0,0,0,0,0,static_cast<std::uint8_t>(n)}};
}
int main() {
  const auto path = std::filesystem::temp_directory_path() /
      ("sb-memory-binary-" + std::to_string(getpid()));
  auto key = a::EncodeMgaMetadataFields({a::kObjectResidencyCatalogName,
      a::MetadataUuidBytes(Id(1)), a::MetadataUuidBytes(Id(2))});
  auto ids = a::EncodeMetadataPairs({{"filespace_uuid", a::MetadataUuidBytes(Id(3))}});
  auto record = a::EncodeMgaMetadataFields({"memory.catalog.v2", key, ids, "generation=1"});
  std::string decoded_key;
  Check(a::ValidateMemoryCatalogRecord(record, &decoded_key) && decoded_key == key);
  auto bad_key = a::EncodeMgaMetadataFields({a::kObjectResidencyCatalogName,
      "01900000-0000-7000-8000-000000000001", a::MetadataUuidBytes(Id(2))});
  Check(!a::ValidateMemoryCatalogRecord(a::EncodeMgaMetadataFields({"memory.catalog.v2", bad_key, ids, "generation=1"})));
  auto bad_ids = a::EncodeMetadataPairs({{"filespace_uuid", "01900000-0000-7000-8000-000000000003"}});
  Check(!a::ValidateMemoryCatalogRecord(a::EncodeMgaMetadataFields({"memory.catalog.v2", key, bad_ids, "generation=1"})));
  Check(!a::ValidateMemoryCatalogRecord(a::EncodeMgaMetadataFields({"memory.catalog.v2", key, a::EncodeMetadataPairs({}), "generation=1"})));
  const auto write = [&](const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
    Check(bool(out));
  };
  write(record + record);
  std::vector<std::string> records;
  Check(a::ReadCatalogLines(path, "test", &records).code.empty() && records.size() == 2);
  auto corrupt = record; corrupt[24] ^= 1; write(corrupt);
  Check(!a::ReadCatalogLines(path, "test", &records).code.empty() && records.size() == 2);
  write(record.substr(0, record.size() - 1));
  Check(!a::ReadCatalogLines(path, "test", &records).code.empty());
  write("format=ScratchBirdMemoryPolicyCatalog|version=1\n");
  Check(!a::ReadCatalogLines(path, "test", &records).code.empty());
  std::filesystem::remove(path);
}
