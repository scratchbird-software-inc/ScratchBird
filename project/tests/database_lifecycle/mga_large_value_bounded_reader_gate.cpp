// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Companion-reader component evidence only. Caller row visibility is supplied
// here; real MGA selection/rollback/restart is covered by the server route gate.
#include "mga_relation_store/mga_large_value_store.hpp"
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
  row.table_uuid = "019f2100-0000-7000-8000-0000000002e1";
  row.row_uuid = "019f2100-0000-7000-8000-0000000002e2";
  row.version_uuid = "019f2100-0000-7000-8000-0000000002e3";
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
  const auto header_end = original.find('\n');
  run(original.substr(0, header_end + 1) + original, false, limit, limit, false);
  const auto first_chunk_end = original.find('\n', header_end + 1);
  run(original.substr(0, first_chunk_end + 1) + original.substr(header_end + 1),
      false, limit, limit, false);
  auto corrupt = original;
  const auto chunk = corrupt.find("LARGE_VALUE_CHUNK");
  auto value = corrupt.find('\t', chunk);
  for (int field = 0; field < 3; ++field) value = corrupt.find('\t', value + 1);
  corrupt[value + 1] = corrupt[value + 1] == '0' ? '1' : '0';
  run(corrupt, false, limit, limit, false);
  const auto uuid_begin = row.values[0].second.find(':') + 1;
  const auto overflow_uuid = row.values[0].second.substr(uuid_begin, 36);
  run(original + "SBMGA1\tLARGE_VALUE_RECLAIMED\t1\t" + overflow_uuid + "\t" +
      row.table_uuid + "\t" + row.row_uuid + "\t" + row.version_uuid + "\tpayload\ttest\n",
      false, limit, limit, false);
  std::cout << "checks=" << checks << " failures=" << failures << " artifacts=" << root << '\n';
  if (!failures) std::filesystem::remove_all(root);
  return failures ? 1 : 0;
}
