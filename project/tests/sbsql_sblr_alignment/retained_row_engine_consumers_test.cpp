// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "crud_support/crud_store.hpp"
#include "mga_relation_store/mga_row_codec.hpp"
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
    FramedVectorScoring();
    std::cout << "PASS retained engine index, predicate and result-state consumers\n";
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << '\n';
    return 1;
  }
}
