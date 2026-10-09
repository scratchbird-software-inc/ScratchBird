// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
#include "dml/insert_batch.hpp"
#include <iostream>
#include <stdexcept>
namespace api = scratchbird::engine::internal_api;
namespace {
void Check(bool condition, const char* detail) {
  if (!condition) throw std::runtime_error(detail);
}
void RetainedStateConsumers() {
  const auto logical_key = [](const std::vector<api::CrudStoredValue>& values) {
    std::string encoded = "SBCLKEY2";
    const auto number = [&](std::size_t value) {
      for (unsigned shift = 0; shift < 32; shift += 8)
        encoded.push_back(static_cast<char>((value >> shift) & 255));
    };
    number(values.size());
    for (const auto& value : values) {
      encoded.push_back(static_cast<char>(value.state));
      number(value.bytes.size()); encoded += value.bytes;
    }
    return encoded;
  };
  api::CrudIndexRecord index;
  index.column_name = "v";
  index.family = api::kCrudIndexFamilyBtree;
  api::CrudRowVersionRecord row;
  for (const auto& value : std::vector<api::CrudStoredValue>{
           "", "<NULL>", "<DEFAULT>", std::string(16, '\0'), api::CrudStoredValue::SqlNull()}) {
    row.values = {{"v", value}};
    Check(api::CrudIndexKeysForValues(index, row.values) ==
              std::vector<std::string>{logical_key({value})},
          "engine scalar index projection lost value state or bytes");
    api::EnginePredicateEnvelope predicate;
    predicate.canonical_predicate_envelope = "v";
    predicate.predicate_kind = "columns_all_null";
    Check(api::CrudRowMatchesPredicate(row, predicate) == value.isSqlNull(),
          "NULL predicate inferred state from payload");
    predicate.predicate_kind = "columns_all_not_null";
    Check(api::CrudRowMatchesPredicate(row, predicate) == value.isPresent(),
          "NOT NULL predicate inferred state from payload");
    api::EngineTypedValue bound;
    bound.descriptor.canonical_type_name = "binary";
    bound.setState(value.state);
    bound.binary_value.assign(value.bytes.begin(), value.bytes.end());
    predicate.bound_values = {bound};
    predicate.predicate_kind = "column_equals";
    Check(api::CrudRowMatchesPredicate(row, predicate) == value.isPresent(),
          "binary equality lost empty/marker bytes or made NULL equal NULL");
    const auto shape = api::CrudRowsToResultShape({row});
    Check(shape.rows.size() == 1 && shape.rows[0].fields.size() == 1 &&
              shape.rows[0].fields[0].second.state == value.state &&
              shape.rows[0].fields[0].second.encoded_value == value.bytes,
          "retained row publication lost state or bytes");
  }
  index.key_envelopes = {"v", "other"};
  row.values = {{"v", "<NULL>"}, {"other", api::CrudStoredValue::SqlNull()}};
  Check(api::CrudIndexKeysForValues(index, row.values) ==
            std::vector<std::string>{logical_key({"<NULL>", api::CrudStoredValue::SqlNull()})},
        "compound engine projection conflated literal marker and NULL");
  for (const auto state : {api::EngineValueState::missing, api::EngineValueState::default_requested,
                           api::EngineValueState::unknown}) {
    row.values = {{"v", {state, {}}}};
    api::EnginePredicateEnvelope predicate;
    predicate.canonical_predicate_envelope = "v";
    for (const auto kind : {"columns_all_null", "columns_all_not_null"}) {
      predicate.predicate_kind = kind;
      Check(!api::CrudRowMatchesPredicate(row, predicate), "unresolved cell matched NULL predicate");
    }
  }
}

void RowCodecStateAdmission() {
  api::CrudRowVersionRecord row;
  row.creator_tx = 1;
  row.table_uuid.bytes = {1, 144, 10, 9, 0, 124, 112, 0, 128, 0, 0, 0, 0, 0, 0, 1};
  row.row_uuid = row.table_uuid; row.row_uuid.bytes[15] = 2;
  row.version_uuid = row.table_uuid; row.version_uuid.bytes[15] = 3;
  api::EngineTypedValue value;
  value.descriptor.canonical_type_name = "binary";
  api::EngineRowValue typed;
  typed.fields = {{"v", value}};
  const std::vector<std::string> order{"v"};
  const auto append = [&](std::string& output) {
    return api::AppendScopedRowBinaryBatch(&output, {row},
        std::span<const api::EngineRowValue>(&typed, 1), order, 1);
  };
  for (const auto state : {api::EngineValueState::value, api::EngineValueState::sql_null}) {
    typed.fields[0].second.setState(state);
    std::string output;
    Check(append(output) && !output.empty(), "native row codec rejected resolved empty/NULL cell");
  }
  for (unsigned tag = 1; tag < 256; ++tag) {
    auto& cell = typed.fields[0].second;
    cell.setState(static_cast<api::EngineValueState>(tag));
    cell.encoded_value = "<NULL>";
    std::string output = "unchanged";
    Check(!append(output) && output == "unchanged", "native row codec accepted unresolved state or NULL payload");
    cell.encoded_value.clear();
    if (tag != 1) {
      Check(!append(output) && output == "unchanged", "native row codec silently resolved a payload-free state");
    }
  }
}

void PreparedInsertStorageAdmission() {
  api::InsertBatchContext context;
  api::InsertRowEncoderColumnPlan first, second;
  first.column_name = "a"; second.column_name = "b";
  context.row_encoder_plan.columns = {first, second};
  api::PreparedInsertRow row;
  const auto admits = [&] {
    return !api::ValidatePreparedInsertStorageShape(context, row).error;
  };
  for (const auto& value : std::vector<api::CrudStoredValue>{
           "", "<NULL>", "<DEFAULT>", std::string(16, '\0'), api::CrudStoredValue::SqlNull()}) {
    row.values = {{"b", value}, {"a", "unchanged"}};
    const auto before = row.values;
    Check(admits() && row.values == before,
          "prepared insert rejected/reinterpreted resolved payload or reordered fields");
  }
  for (unsigned tag = 1; tag < 256; ++tag) {
    row.values = {{"a", "ok"}, {"b", {static_cast<api::EngineValueState>(tag), "<NULL>"}}};
    Check(!admits(), "prepared insert admitted a non-value state with marker payload");
    row.values[1].second.bytes.clear();
    Check(admits() == (tag == 1), "prepared insert admitted unresolved payload-free state");
  }
  row.values = {{"a", "ok"}};
  Check(!admits(), "prepared insert admitted missing column");
  row.values.push_back({"a", "duplicate"});
  Check(!admits(), "prepared insert admitted duplicate column");
  row.values[1].first = "unbound";
  Check(!admits(), "prepared insert admitted unbound column");
  row.values[1].first = "b";
  row.values.push_back({"extra", "extra"});
  Check(!admits(), "prepared insert admitted extra column");
}

void NativeScalarStorageRoundTrips() {
  api::CrudRowVersionRecord identity;
  identity.creator_tx = 7;
  identity.table_uuid.bytes = {1, 144, 10, 9, 0, 124, 112, 0, 128, 0, 0, 0, 0, 0, 0, 1};
  identity.row_uuid = identity.table_uuid; identity.row_uuid.bytes[15] = 2;
  identity.version_uuid = identity.table_uuid; identity.version_uuid.bytes[15] = 3;
  const std::vector<std::string> order{"v"};
  struct Scalar { const char* name; std::uint8_t tag; std::size_t width; };
  for (const auto scalar : {Scalar{"boolean", 3, 1}, Scalar{"int32", 4, 4},
                           Scalar{"int64", 2, 8}, Scalar{"uint64", 5, 8},
                           Scalar{"real64", 6, 8}, Scalar{"binary", 7, 16},
                           Scalar{"uuid", 0, 16}, Scalar{"text", 1, 4}}) {
    for (unsigned pattern = 0; pattern < 5; ++pattern) {
      std::string bytes(scalar.width, '\0');
      if (pattern == 1) bytes.back() = static_cast<char>(0x80); // minimum / negative zero
      if (pattern == 2) { bytes.assign(scalar.width, static_cast<char>(0xff)); bytes.back() = 0x7f; }
      if (pattern == 3) bytes.assign(scalar.width, static_cast<char>(0xff));
      if (pattern == 4) bytes.assign(scalar.width, '3'); // digits are still native bits
      if (scalar.tag == 3) bytes[0] = static_cast<char>(pattern % 2);
      api::EngineTypedValue value;
      value.descriptor.canonical_type_name = scalar.name;
      value.binary_value.assign(bytes.begin(), bytes.end());
      for (bool null : {false, true}) {
        auto cell = value;
        if (null) { cell.binary_value.clear(); cell.setState(api::EngineValueState::sql_null); }
        const api::EngineRowValue typed{ {}, {{"v", cell}} };
        for (unsigned version : {6u, 7u, 8u}) {
          if (version == 7 && scalar.tag == 0) continue; // native packet has no UUID tag
          auto row = identity;
          std::string output;
          if (version == 6) {
            Check(api::AppendScopedRowIdentityBinaryBatch(&output, {row}, row.table_uuid, {},
                      std::span<const api::EngineRowValue>(&typed, 1), order, 7, 11),
                  "compact scalar append failed");
          } else if (version == 7) {
            api::EngineNativeRowPacketFrame frame;
            frame.present = true; frame.version = 2; frame.row_count = 1; frame.column_count = 1;
            frame.field_order = order; frame.column_type_tags = {scalar.tag}; frame.row_offsets = {0};
            frame.packet_bytes.push_back(null ? 1 : 0);
            if (!null) {
              if (scalar.tag == 1 || scalar.tag == 7) {
                for (unsigned byte = 0; byte < 4; ++byte)
                  frame.packet_bytes.push_back(static_cast<std::uint8_t>(bytes.size() >> (8 * byte)));
              }
              frame.packet_bytes.insert(frame.packet_bytes.end(), bytes.begin(), bytes.end());
            }
            frame.row_sizes = {static_cast<std::uint32_t>(frame.packet_bytes.size())};
            Check(api::AppendScopedRowIdentityNativePacketBatch(&output, {row}, row.table_uuid, {}, frame, 7, 11),
                  "native packet scalar append failed");
          } else {
            row.previous_version_uuid = identity.version_uuid;
            row.previous_sequence = 10; // force full version framing
            Check(api::AppendScopedRowBinaryBatch(&output, {row},
                      std::span<const api::EngineRowValue>(&typed, 1), order, 11),
                  "general scalar append failed");
          }
          Check(static_cast<unsigned char>(output[8]) == version, "wrong storage fixture version");
          std::vector<api::CrudRowVersionRecord> decoded;
          api::ScopedRelationSummary summary;
          Check(api::DecodeScopedRowBinaryBytes({output.begin(), output.end()}, &decoded, &summary) &&
                    decoded.size() == 1 && decoded[0].values.size() == 1 &&
                    decoded[0].values[0].second.state == cell.state &&
                    decoded[0].values[0].second.bytes == (null ? std::string{} : bytes),
                "scoped row codec changed native scalar bytes/state into display text");
        }
      }
    }
  }
}

void FramedVectorScoring() {
  api::EngineUuid table;
  table.bytes = {1, 144, 10, 9, 0, 124, 112, 0, 128, 0, 0, 0, 0, 0, 0, 1};
  api::CrudIndexRecord index;
  index.creator_tx = 1; index.table_uuid = table;
  index.index_uuid = table; index.index_uuid.bytes[15] = 2;
  index.family = api::kCrudIndexFamilyVectorExact;
  index.column_name = "v";
  api::CrudState state;
  state.transactions[1] = "active";
  state.indexes = {index};
  for (unsigned ordinal = 0; ordinal != 2; ++ordinal) {
    api::CrudRowVersionRecord row;
    row.creator_tx = 1; row.event_sequence = row.sequence = ordinal + 1;
    row.table_uuid = table; row.row_uuid = table; row.row_uuid.bytes[15] = 3 + ordinal;
    row.version_uuid = row.row_uuid; row.version_uuid.bytes[15] += 2;
    // This retained scalar profile stores comma-separated components; the
    // surrounding logical index frame is not part of the vector payload.
    row.values = {{"v", ordinal == 0 ? "9,0" : "1,0"}};
    state.row_versions.push_back(row);
    const auto keys = api::CrudIndexKeysForValues(index, row.values);
    Check(keys.size() == 1, "vector projection missing");
    api::CrudIndexEntryRecord entry;
    entry.creator_tx = 1; entry.table_uuid = table; entry.index_uuid = index.index_uuid;
    entry.row_uuid = row.row_uuid; entry.version_uuid = row.version_uuid;
    entry.key_value = keys.front();
    state.index_entries.push_back(std::move(entry));
  }
  api::EnginePredicateEnvelope predicate;
  predicate.predicate_kind = "vector_exact_nearest";
  predicate.canonical_predicate_envelope = "v";
  api::EngineTypedValue query;
  query.descriptor.canonical_type_name = "vector";
  query.encoded_value = "0,0";
  predicate.bound_values = {query};
  api::EngineRequestContext context;
  context.local_transaction_id = 1;
  const auto rows = api::IndexedCrudRowsForPredicateForContext(state, table, predicate, context, 1, nullptr);
  Check(rows.size() == 1 && rows[0].row_uuid == state.row_versions[1].row_uuid,
        "vector distance consumed framing instead of the projected value");
}

}
int main() {
  try {
    RetainedStateConsumers();
    RowCodecStateAdmission();
    PreparedInsertStorageAdmission();
    NativeScalarStorageRoundTrips();
    FramedVectorScoring();
    std::cout << "PASS retained engine index, predicate and result-state consumers\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
