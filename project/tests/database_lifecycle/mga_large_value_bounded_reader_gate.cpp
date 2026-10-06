// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// Companion-reader component evidence only. Caller row visibility is supplied
// here; real MGA selection/rollback/restart is covered by the server route gate.
#include "../support/binary_uuid_fixture.hpp"
#include "../support/owned_temp_directory.hpp"
#include "mga_relation_store/mga_large_value_store.hpp"
#include "mga_relation_store/mga_large_value_codec.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "mga_relation_store/mga_heap_runtime_support.hpp"
#include "dml/direct_bulk_typed_row_codec.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>

namespace api = scratchbird::engine::internal_api;
int main() try {
  unsigned checks = 0, failures = 0;
  const auto expect = [&](bool condition, const char* detail) {
    ++checks; if (!condition) { ++failures; std::cerr << detail << '\n'; }
  };
  scratchbird::tests::OwnedTempDirectory directory;
  const auto root = directory.path();
  api::EngineRequestContext context;
  context.database_path = (root / "component.sbdb").string();
  context.local_transaction_id = 1;
  api::CrudRowVersionRecord row;
  row.creator_tx = 1;
  row.event_sequence = 1;
  row.sequence = 1;
  row.table_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e1");
  row.row_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e2");
  row.version_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e3");
  std::string payload(12000, 'x');
  payload[45] = '\0'; payload.replace(100, 4, "\xf0\x9f\x98\x80");
  row.values = {{"payload", payload}};
  const auto stored = api::PersistMgaLargeValuesForRow(context, row.table_uuid,
      row.row_uuid, row.version_uuid, false, &row.values, nullptr);
  expect(!stored.error && api::RowsContainLargeValueLocators({row}), "fixture did not persist overflow");
  expect(row.values[0].second.state == api::EngineValueState::lob_handle,
         "overflow allocation did not publish explicit LOB_HANDLE state");
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
  // Independently encode general revision 8: no payload spelling implies state.
  auto mixed = row;
  mixed.values.push_back({"literal_locator", row.values[0].second.bytes});
  mixed.values.push_back({"legacy_marker", "SBMGA_LARGE_VALUE:ordinary-data"});
  mixed.values.push_back({"null", api::CrudStoredValue::SqlNull()});
  mixed.values.push_back({"empty", ""});
  // This component exercises opaque retained binary cells. NULL still needs
  // an explicit bound column type; its type must not be inferred from bytes.
  api::InsertRowEncoderPlan plan;
  for (const auto& field : mixed.values) {
    api::InsertRowEncoderColumnPlan column;
    column.column_name = field.first;
    column.canonical_type_name = "binary";
    plan.columns.push_back(std::move(column));
  }
  const auto physical = api::dml::detail::DirectPhysicalCells(mixed.values, &plan);
  expect(physical.size() == mixed.values.size() &&
         physical[0].value.payload_is_toast_reference &&
         !physical[1].value.payload_is_toast_reference &&
         !physical[2].value.payload_is_toast_reference &&
         physical[3].value.is_null &&
         physical[3].value.type_id == scratchbird::core::datatypes::CanonicalTypeId::binary &&
         physical[3].value.payload.empty() && !physical[4].value.is_null,
         "physical cells inferred LOB or NULL state from payload spelling");
  bool unbound_null_refused = false;
  try {
    (void)api::dml::detail::DirectPhysicalCells(mixed.values, nullptr);
  } catch (const std::invalid_argument&) { unbound_null_refused = true; }
  expect(unbound_null_refused, "physical NULL admitted without a bound column type");
  for (std::size_t i = 0; i < physical.size(); ++i) {
    const auto encoded = scratchbird::core::datatypes::EncodeDatatypeBinaryValue(physical[i].value);
    const auto decoded = scratchbird::core::datatypes::DecodeDatatypeBinaryValue(encoded.encoded);
    if (!encoded.ok() || !decoded.ok())
      std::cerr << "physical_cell=" << i << " encode=" << encoded.diagnostic.message_key
                << " decode=" << decoded.diagnostic.message_key << '\n';
    expect(encoded.ok() && decoded.ok() &&
           decoded.value.payload_is_toast_reference == (i == 0) &&
           decoded.value.is_null == (i == 3) &&
           decoded.value.payload == physical[i].value.payload,
           "physical binary cell lost exact locator/value/null state");
  }
  for (const auto malformed : {std::string("SBMGLV02"), std::string(40, '\0')}) {
    bool refused = false;
    try {
      (void)api::dml::detail::DirectPhysicalCells(
          {{"payload", {api::EngineValueState::lob_handle, malformed}}}, &plan);
    } catch (const std::invalid_argument&) { refused = true; }
    expect(refused, "physical cell admitted malformed LOB identity");
  }
  std::string oracle = "SBMRBIN1";
  const auto number = [&](std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
      oracle.push_back(static_cast<char>(value >> (8 * i)));
  };
  const auto blob = [&](const std::string& value) {
    number(value.size(), 4); oracle += value;
  };
  const auto identity = [&](const api::EngineUuid& id) {
    oracle.append(reinterpret_cast<const char*>(id.bytes.data()), id.bytes.size());
  };
  number(8, 2); number(0, 2); number(mixed.values.size(), 4); number(1, 8);
  for (const auto& field : mixed.values) blob(field.first);
  for (const auto& field : mixed.values) blob("text");
  number(mixed.creator_tx, 8); number(mixed.event_sequence, 8);
  number(mixed.previous_sequence, 8); number(mixed.deleted, 1);
  identity(mixed.table_uuid); identity(mixed.row_uuid); identity(mixed.version_uuid);
  identity(mixed.previous_version_uuid); identity(mixed.temporary_session_uuid);
  const auto state_offset = oracle.size();
  for (const auto& field : mixed.values) number(static_cast<unsigned>(field.second.state), 1);
  for (const auto& field : mixed.values)
    if (!field.second.isSqlNull()) blob(field.second.bytes);
  expect(api::BuildRowVersionStoreLine(mixed) == oracle,
         "general retained row differs from independent state-bearing frame");
  const auto decode = [&](const std::string& bytes, std::vector<api::CrudRowVersionRecord>* out) {
    api::ScopedRelationSummary summary;
    return api::DecodeScopedRowBinaryBytes({bytes.begin(), bytes.end()}, out, &summary);
  };
  std::vector<api::CrudRowVersionRecord> decoded;
  expect(decode(oracle, &decoded) && decoded.size() == 1 && decoded[0].values == mixed.values,
         "general durable row lost LOB versus present/NULL state");
  // A general record owns three u64 fields, one flag, and five binary16
  // identities before its state vector. Reject impossible row counts before
  // reserving row storage, not only when the later per-field read fails.
  const auto row_start = state_offset - 105;
  for (std::size_t remaining = 0; remaining < 105 + mixed.values.size(); ++remaining) {
    const auto truncated = oracle.substr(0, row_start + remaining);
    std::vector<api::CrudRowVersionRecord> untouched;
    const auto capacity = untouched.capacity();
    api::ScopedRelationSummary summary;
    api::BoundedScopedRowReadControl control;
    control.maximum_row_versions = 1;
    expect(!api::DecodeScopedRowBinaryBytes(
               {truncated.begin(), truncated.end()}, &untouched, &summary, &control) &&
               summary.malformed && !summary.trusted && untouched.empty() &&
               untouched.capacity() == capacity &&
               control.refusal_detail == "heap_read_row_count_exceeds_segment",
           "truncated general prefix reached row allocation or escaped early refusal");
  }
  for (const unsigned invalid : {2u, 3u, 4u, 5u, 7u, 8u, 255u}) {
    auto corrupt_state = oracle;
    corrupt_state[state_offset] = static_cast<char>(invalid);
    decoded.clear();
    expect(!decode(corrupt_state, &decoded) && decoded.empty(),
           "general durable row accepted unresolved state or published partial rows");
  }
  auto legacy_general = oracle;
  legacy_general[8] = 5;
  decoded.clear();
  expect(!decode(legacy_general, &decoded),
         "old general format was guessed into state-bearing rows");
  auto literals = mixed;
  literals.values.erase(literals.values.begin());
  expect(!api::RowsContainLargeValueLocators({literals}),
         "ordinary locator-shaped data acquired LOB state");
  auto literal_rows = std::vector<api::CrudRowVersionRecord>{literals};
  api::BoundedScopedRowReadControl literal_control;
  literal_control.maximum_bytes = limit;
  literal_control.maximum_memory_bytes = limit;
  api::HeapReadRuntimeObservation literal_observation;
  literal_control.runtime_observation = &literal_observation;
  expect(!api::ExpandVisibleMgaLargeValuesBounded(context, &literal_rows,
             &literal_control, 1024, visible).error &&
             literal_rows[0].values == literals.values &&
             literal_observation.storage_bytes_read == 0,
         "literal locator bytes triggered companion I/O or changed data");
  run(original, true, limit, limit, false);
  run(original, false, 10, limit, false);
  run(original, false, limit, 1024, false);
  run(original, false, limit, limit, true);
  run(original, false, limit, limit, false, true);
  {
    auto invisible_rows = std::vector<api::CrudRowVersionRecord>{row};
    api::BoundedScopedRowReadControl control;
    control.maximum_bytes = limit;
    control.maximum_memory_bytes = limit;
    const auto refused = api::ExpandVisibleMgaLargeValuesBounded(context, &invisible_rows,
        &control, 1024, [](std::uint64_t) { return false; });
    expect(refused.error && invisible_rows[0].values == row.values,
           "invisible overflow creator was admitted or partially materialized");
  }
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
  expect(row.values[0].second.valid() &&
             api::ReadMgaLargeValueLocator(row.values[0].second.bytes, &overflow_uuid, &checksum, &logical_size),
         "fixture overflow locator did not preserve native identity");
  const auto reclaimed = api::EncodeMgaMetadataFields(
      {"SBMGL002", "LARGE_VALUE_RECLAIMED", "1", api::MetadataUuidBytes(overflow_uuid),
       api::MetadataUuidBytes(row.table_uuid), api::MetadataUuidBytes(row.row_uuid),
       api::MetadataUuidBytes(row.version_uuid), "payload", "test"});
  expect(!reclaimed.empty(), "reclaim fixture binary encoding failed");
  run(original + reclaimed, false, limit, limit, false);
  write(original);
  auto forced = row;
  forced.version_uuid = scratchbird::tests::FixtureUuidLiteral("019f2100-0000-7000-8000-0000000002e4");
  const auto literal_locator = row.values[0].second.bytes;
  forced.values = {{"payload", literal_locator}};
  const auto forced_store = api::PersistMgaLargeValuesForRow(context, forced.table_uuid,
      forced.row_uuid, forced.version_uuid, true, &forced.values, nullptr);
  expect(!forced_store.error &&
             forced.values[0].second.state == api::EngineValueState::lob_handle &&
             forced.values[0].second.bytes != literal_locator,
         "forced overflow interpreted present locator bytes instead of storing them");
  auto forced_rows = std::vector<api::CrudRowVersionRecord>{forced};
  api::BoundedScopedRowReadControl forced_control;
  forced_control.maximum_bytes = limit;
  forced_control.maximum_memory_bytes = limit;
  expect(!api::ExpandVisibleMgaLargeValuesBounded(context, &forced_rows,
             &forced_control, 1024, visible).error &&
             forced_rows[0].values[0].second == api::CrudStoredValue(literal_locator),
         "forced overflow did not materialize exact present locator-shaped bytes");
  std::cout << "checks=" << checks << " failures=" << failures << " artifacts=" << root << '\n';
  directory.Cleanup();
  return failures ? 1 : 0;
} catch (const std::exception& error) {
  std::cerr << error.what() << '\n';
  return 1;
}
