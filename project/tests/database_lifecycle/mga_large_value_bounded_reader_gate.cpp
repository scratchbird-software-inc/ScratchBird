// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Companion-reader component evidence only. Caller row visibility is supplied
// here; real MGA selection/rollback/restart is covered by the server route gate.
#include "../support/binary_uuid_fixture.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_large_value_codec.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace api = scratchbird::engine::internal_api;
int main() {
  unsigned checks = 0, failures = 0;
  const auto expect = [&](bool condition, const char* detail) {
    ++checks; if (!condition) { ++failures; std::cerr << detail << '\n'; }
  };
  const auto root = std::filesystem::temp_directory_path() /
      ("sb_i8_large_reader_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(root);
  api::EngineRequestContext context;
  context.database_path = (root / "component.sbdb").string();
  context.local_transaction_id = 1;
  api::CrudRowVersionRecord row;
  row.creator_tx = 1;
  row.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e1");
  row.row_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e2");
  row.version_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e3");
  std::string payload(12000, 'x');
  payload[45] = '\0'; payload.replace(100, 4, "\xf0\x9f\x98\x80");
  row.values = {{"payload", payload}};
  const auto stored = api::PersistMgaLargeValuesForRow(context, row.table_uuid,
      row.row_uuid, row.version_uuid, false, &row.values, nullptr);
  expect(!stored.error && api::RowsContainLargeValueLocators({row}), "fixture did not persist overflow");
  if (stored.error || failures) return 1;
  const auto path = context.database_path + ".sb.mga_large_values";
  std::ifstream file(path, std::ios::binary);
  const std::string original{std::istreambuf_iterator<char>(file), {}};
  file.close();
  const auto write = [&](const std::string& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), bytes.size());
  };
  const std::function<bool(std::uint64_t)> visible = [](std::uint64_t tx) { return tx == 1; };
  const auto run = [&](std::string bytes, bool success, std::uint64_t byte_limit,
                       std::uint64_t memory_limit, bool cancel, bool wrong_owner = false) {
    write(bytes);
    auto rows = std::vector<api::CrudRowVersionRecord>{row};
    if (wrong_owner) rows[0].row_uuid = row.version_uuid;
    const auto before = rows[0].values;
    api::BoundedScopedRowReadControl control;
    control.maximum_bytes = byte_limit;
    control.maximum_memory_bytes = memory_limit;
    const std::function<bool()> cancellation = [=] { return cancel; };
    control.cancellation_requested = &cancellation;
    api::HeapReadRuntimeObservation observation;
    control.runtime_observation = &observation;
    const auto result = api::ExpandVisibleMgaLargeValuesBounded(context, &rows, &control, 1024, visible);
    expect(result.error != success, "bounded reader disposition differs");
    expect(success ? rows[0].values[0].second == payload : rows[0].values == before,
           "bounded reader changed exact bytes or partially replaced refused rows");
    if (success) expect(observation.storage_bytes_read == bytes.size(), "companion I/O accounting differs");
  };
  constexpr std::uint64_t limit = 16 * 1024 * 1024;
  run(original, true, limit, limit, false);
  run(original, false, 10, limit, false);
  run(original, false, limit, 1024, false);
  run(original, false, limit, limit, true);
  run(original, false, limit, limit, false, true);
  run(original.substr(0, original.size() - 1), false, limit, limit, false);
  std::vector<std::string> frames;
  expect(api::DecodeMgaMetadataStream(
      {reinterpret_cast<const std::uint8_t*>(original.data()), original.size()}, &frames) && frames.size() > 1,
      "fixture large values are not complete binary metadata frames");
  if (failures) return 1;
  run(frames.front() + original, false, limit, limit, false);
  std::size_t chunk_index = 0;
  std::vector<std::string> chunk_fields;
  for (; chunk_index < frames.size(); ++chunk_index) {
    expect(api::DecodeMgaMetadataFields(frames[chunk_index], &chunk_fields), "fixture metadata decode failed");
    if (chunk_fields.size() == 7 && chunk_fields[1] == "LARGE_VALUE_CHUNK") break;
  }
  expect(chunk_index < frames.size() && !chunk_fields[5].empty(), "fixture contains no nonempty chunk");
  if (failures) return 1;
  const auto joined = [](const std::vector<std::string>& records) {
    std::string bytes;
    for (const auto& record : records) bytes += record;
    return bytes;
  };
  auto duplicate = frames;
  duplicate.insert(duplicate.begin() + chunk_index, frames[chunk_index]);
  run(joined(duplicate), false, limit, limit, false);
  auto corrupt = frames;
  chunk_fields[5][0] ^= 1;
  corrupt[chunk_index] = api::EncodeMgaMetadataFields(chunk_fields);
  expect(!corrupt[chunk_index].empty(), "corrupt payload fixture encoding failed");
  run(joined(corrupt), false, limit, limit, false);
  auto checksum_corrupt = original;
  checksum_corrupt.back() ^= 1;
  run(checksum_corrupt, false, limit, limit, false);
  api::EngineUuid overflow_uuid;
  std::uint64_t checksum = 0, logical_size = 0;
  expect(api::ReadMgaLargeValueLocator(row.values[0].second, &overflow_uuid, &checksum, &logical_size),
         "fixture overflow locator did not preserve native identity");
  const auto reclaimed = api::EncodeMgaMetadataFields(
      {"SBMGL002", "LARGE_VALUE_RECLAIMED", "1", api::MetadataUuidBytes(overflow_uuid),
       api::MetadataUuidBytes(row.table_uuid), api::MetadataUuidBytes(row.row_uuid),
       api::MetadataUuidBytes(row.version_uuid), "payload", "test"});
  expect(!reclaimed.empty(), "reclaim fixture binary encoding failed");
  run(original + reclaimed, false, limit, limit, false);
  std::cout << "checks=" << checks << " failures=" << failures << " artifacts=" << root << '\n';
  if (!failures) std::filesystem::remove_all(root);
  return failures ? 1 : 0;
}
