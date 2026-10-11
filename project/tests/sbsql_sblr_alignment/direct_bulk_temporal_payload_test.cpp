// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_typed_row_codec.hpp"
#include "datatype_type_codec_identity_v3.hpp"
#include "datatype_time.hpp"
#include "datatype_timestamp.hpp"
#include <iostream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace bulk = api::dml::detail;
namespace dt = scratchbird::core::datatypes;
unsigned checks = 0;
void Check(bool value, const char* message) {
  ++checks;
  if (!value) throw std::runtime_error(message);
}
api::EngineTypedValue Value(dt::CanonicalTypeId type, unsigned generation, std::uint64_t bits,
                          std::uint32_t fraction = 0) {
  api::EngineTypedValue value;
  const auto cohort = generation == 10 ? dt::kDatatypeCohortV10 : dt::kDatatypeCohortV11;
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
    const auto& row = entry.legacy_fields;
    if (row.catalog_snapshot_uuid != cohort || row.canonical_binary_type_code != static_cast<unsigned>(type)) continue;
    auto& descriptor = value.descriptor;
    descriptor.descriptor_uuid = descriptor.datatype_descriptor_uuid = row.descriptor_uuid;
    descriptor.datatype_descriptor_generation = row.descriptor_generation;
    descriptor.type_uuid = row.type_uuid;
    descriptor.datatype_cohort = {cohort, generation, generation};
    descriptor.descriptor_kind = "scalar";
    descriptor.canonical_type_name = type == dt::CanonicalTypeId::date ? "date" :
        type == dt::CanonicalTypeId::time ? "time" : "timestamp";
    descriptor.encoded_descriptor = "nullability=nullable";
    for (unsigned i = 0; i < (type == dt::CanonicalTypeId::date ? 4u : 8u); ++i)
      value.binary_value.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
    if (type == dt::CanonicalTypeId::timestamp) {
      for (unsigned i = 0; i < 4; ++i)
        value.binary_value.push_back(static_cast<std::uint8_t>(fraction >> (8*i)));
      value.binary_value.resize(16, 0);
    }
    return value;
  }
  throw std::runtime_error("explicit temporal fixture row unavailable");
}
api::InsertRowEncoderPlan Plan(const api::EngineTypedValue& value) {
  api::InsertRowEncoderPlan plan;
  api::InsertRowEncoderColumnPlan column;
  column.column_name = "v";
  column.canonical_type_name = value.descriptor.canonical_type_name;
  plan.columns.push_back(column);
  return plan;
}
void Refuses(dt::CanonicalTypeId type, const api::EngineTypedValue& value) {
  Check(!bulk::DirectValidateNativeTemporalValue(value, type), "invalid temporal carrier admitted");
  std::vector<std::uint8_t> output{0x5a};
  Check(!bulk::DirectPackTypedPayload(type, value, &output) && output == std::vector<std::uint8_t>{0x5a},
        "temporal packing refusal changed output");
  bool refused = false;
  try { (void)bulk::DirectStoredValueForColumn(value, type); } catch (const std::invalid_argument&) { refused = true; }
  Check(refused, "retained temporal path admitted invalid carrier");
  const auto plan = Plan(value);
  api::EngineRowValue row; row.fields.push_back({"v", value});
  const auto validation = bulk::BuildDirectFixedWidthPayloadValidationPlan(plan, &row);
  Check(!bulk::DirectFixedWidthTypedPayloadFailure(row, validation).empty(), "fixed-width fast path bypassed temporal validation");
  for (const auto* selected : {static_cast<const std::vector<bulk::DirectFixedWidthPayloadValidationColumnPlan>*>(nullptr), &validation}) {
    refused = false;
    try { (void)bulk::DirectPhysicalCellsFromTypedInputRow(row, plan, selected); } catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "physical temporal path admitted invalid carrier");
  }
}
void Run(dt::CanonicalTypeId type, unsigned generation) {
  const std::vector<std::uint64_t> cases = type == dt::CanonicalTypeId::date ?
      std::vector<std::uint64_t>{0, 1, 0xffffffffu, 0x80000000u, 0x7fffffffu} :
      type == dt::CanonicalTypeId::time ?
      std::vector<std::uint64_t>{0, 1, 255, 256, dt::kTimeMaximumNanosecondsV3} :
      std::vector<std::uint64_t>{0, 1, 255, 256, ~std::uint64_t{0},
          static_cast<std::uint64_t>(dt::kTimestampMinimumCivilSecondV3),
          static_cast<std::uint64_t>(dt::kTimestampMaximumCivilSecondV3)};
  for (const auto bits : cases) {
    const auto value = Value(type, generation, bits);
    for (const bool null : {false, true}) {
      auto typed = value;
      if (null) { typed.state = api::EngineValueState::sql_null; typed.binary_value.clear(); }
      Check(bulk::DirectValidateNativeTemporalValue(typed, type), "exact native temporal refused");
      const auto stored = bulk::DirectStoredValueForColumn(typed, type);
      Check(stored.isSqlNull() == null && stored.bytes == std::string(typed.binary_value.begin(), typed.binary_value.end()),
            "retained temporal bytes changed");
      api::EngineRowValue row; row.fields.push_back({"v", typed});
      const auto plan = Plan(typed);
      const auto validation = bulk::BuildDirectFixedWidthPayloadValidationPlan(plan, &row);
      Check(bulk::DirectFixedWidthTypedPayloadFailure(row, validation).empty(), "valid temporal fixed-width admission failed");
      for (const auto* selected : {static_cast<const std::vector<bulk::DirectFixedWidthPayloadValidationColumnPlan>*>(nullptr), &validation}) {
        const auto physical = bulk::DirectPhysicalCellsFromTypedInputRow(row, plan, selected);
        Check(physical.cells.size() == 1 && physical.cells[0].value.type_id == type &&
            physical.cells[0].value.is_null == null && physical.cells[0].value.payload == typed.binary_value,
            "physical and retained temporal carriers diverged");
      }
      auto destination = typed.descriptor;
      Check(bulk::DirectValidateNativeTemporalValue(typed, type, &destination, true), "exact destination refused");
      destination.encoded_descriptor = "nullability=non_null";
      Check(bulk::DirectValidateNativeTemporalValue(typed, type, &destination, false) == !null,
            "destination nullability not enforced independently of source nullability");
      destination.datatype_cohort = generation == 10 ? api::EngineDatatypeCohort{dt::kDatatypeCohortV11,11,11} :
          api::EngineDatatypeCohort{dt::kDatatypeCohortV10,10,10};
      Check(!bulk::DirectValidateNativeTemporalValue(typed, type, &destination, false), "cross-cohort source relabeled");
      for (unsigned mutation = 0; mutation < 9; ++mutation) {
        destination = typed.descriptor;
        switch (mutation) {
          case 0: destination.datatype_descriptor_uuid.bytes[0] ^= 1; break;
          case 1: ++destination.datatype_descriptor_generation; break;
          case 2: destination.type_uuid.bytes[0] ^= 1; break;
          case 3: ++destination.datatype_cohort.catalog_generation; break;
          case 4: ++destination.datatype_cohort.registry_generation; break;
          case 5: destination.encoded_descriptor += ";precision=1"; break;
          case 6: destination.encoded_descriptor += ";unknown=1"; break;
          case 7: destination.charset_uuid = destination.type_uuid; break;
          case 8: destination.collation_uuid = destination.type_uuid; break;
        }
        api::EngineApiDiagnostic diagnostic;
        Check(!bulk::DirectValidateNativeTemporalValue(typed, type, &destination, true, &diagnostic) &&
                  diagnostic.error && !diagnostic.code.empty(),
              "invalid receiving descriptor admitted or diagnostic lost");
      }
      for (const bool source_nullable : {false, true}) {
        auto source = typed;
        source.descriptor.encoded_descriptor = source_nullable ? "nullability=nullable" : "nullability=non_null";
        for (const bool target_nullable : {false, true}) {
          destination = typed.descriptor;
          destination.encoded_descriptor = target_nullable ? "nullability=nullable" : "nullability=non_null";
          Check(bulk::DirectValidateNativeTemporalValue(source, type, &destination, target_nullable) ==
              (!null || (source_nullable && target_nullable)), "independent source/destination NULL contract changed");
        }
      }
    }
  }
  for (unsigned mutation = 0; mutation < 15; ++mutation) {
    auto value = Value(type, generation, 1);
    switch (mutation) {
      case 0: value.encoded_value = type == dt::CanonicalTypeId::date ? "1970-01-01" : "00:00:01"; value.binary_value.clear(); break;
      case 1: value.encoded_value = "ambiguous"; break;
      case 2: value.binary_value.pop_back(); break;
      case 3: value.binary_value.push_back(0); break;
      case 4: value.descriptor.datatype_cohort = {}; break;
      case 5: ++value.descriptor.datatype_cohort.registry_generation; break;
      case 6: value.descriptor.datatype_descriptor_uuid.bytes[0] ^= 1; break;
      case 7: ++value.descriptor.datatype_descriptor_generation; break;
      case 8: value.descriptor.type_uuid.bytes[0] ^= 1; break;
      case 9: value.descriptor.encoded_descriptor += ";precision=1"; break;
      case 10: value.descriptor.charset_uuid = value.descriptor.type_uuid; break;
      case 11: value.state = api::EngineValueState::sql_null; break;
      case 12: value.state = api::EngineValueState::missing; value.binary_value.clear(); break;
      case 13: value.descriptor.encoded_descriptor += ";unknown=1"; break;
      case 14: value.descriptor.collation_uuid = value.descriptor.type_uuid; break;
    }
    Refuses(type, value);
  }
  if (type == dt::CanonicalTypeId::time) {
    Refuses(type, Value(type, generation, dt::kTimeMaximumNanosecondsV3 + 1));
    Refuses(type, Value(type, generation, ~std::uint64_t{0}));
    api::EngineApiDiagnostic diagnostic;
    Check(!bulk::DirectValidateNativeTemporalValue(
        Value(type, generation, dt::kTimeMaximumNanosecondsV3 + 1), type, nullptr, true, &diagnostic) &&
        diagnostic.native_source && diagnostic.native_source->datatype_cause &&
        !diagnostic.native_source->datatype_cause->status.ok() &&
        diagnostic.native_source->datatype_cause->diagnostic_code == diagnostic.code &&
        diagnostic.native_source->datatype_cause->detail == diagnostic.detail,
        "native TIME component failure lost its typed cause");
  }
  if (type == dt::CanonicalTypeId::timestamp) {
    for (const auto* text : {"1970-01-01T00:00:00", "1970-01-01 01:00:00+01:00", "1970-01-01T00:00:00Z"}) {
      auto value = Value(type, generation, 0);
      value.binary_value.clear(); value.encoded_value = text;
      Refuses(type, value);
    }
    for (const auto fraction : {0u, 1u, 999999999u}) {
      for (const auto seconds : {dt::kTimestampMinimumCivilSecondV3, std::int64_t{-1},
          std::int64_t{0}, dt::kTimestampMaximumCivilSecondV3}) {
        const auto value = Value(type, generation, static_cast<std::uint64_t>(seconds), fraction);
        std::vector<std::uint8_t> payload;
        Check(bulk::DirectPackTypedPayload(type, value, &payload) && payload == value.binary_value,
              "TIMESTAMP fraction/extrema native bytes changed");
      }
    }
    for (const auto bits : {static_cast<std::uint64_t>(dt::kTimestampMinimumCivilSecondV3-1),
                           static_cast<std::uint64_t>(dt::kTimestampMaximumCivilSecondV3+1)})
      Refuses(type, Value(type, generation, bits));
    Refuses(type, Value(type, generation, 0, 1000000000));
    for (unsigned i = 12; i < 16; ++i) {
      auto value = Value(type, generation, 0); value.binary_value[i] = 1; Refuses(type, value);
    }
    api::EngineApiDiagnostic diagnostic;
    Check(!bulk::DirectValidateNativeTemporalValue(Value(type, generation, 0, 1000000000), type,
        nullptr, true, &diagnostic) && diagnostic.native_source && diagnostic.native_source->datatype_cause &&
        diagnostic.native_source->datatype_cause->diagnostic_code == diagnostic.code &&
        diagnostic.native_source->datatype_cause->detail == diagnostic.detail,
        "TIMESTAMP canonical failure lost typed diagnostic");
  }
}
void RetainedColumnBindings() {
  for (const unsigned generation : {10u, 11u}) {
    const auto date = Value(dt::CanonicalTypeId::date, generation, 0xffffffffu);
    const auto time = Value(dt::CanonicalTypeId::time, generation, dt::kTimeMaximumNanosecondsV3);
    const auto timestamp = Value(dt::CanonicalTypeId::timestamp, generation, ~std::uint64_t{0}, 999999999);
    auto plan = Plan(date);
    plan.columns[0].column_name = "day";
    auto middle = plan.columns[0]; middle.column_name = "opaque"; middle.canonical_type_name = "binary";
    auto last = plan.columns[0]; last.column_name = "clock"; last.canonical_type_name = "time";
    plan.columns.push_back(middle); plan.columns.push_back(last);
    auto stamp_column = last; stamp_column.column_name = "stamp"; stamp_column.canonical_type_name = "timestamp";
    plan.columns.push_back(stamp_column);
    const auto day = bulk::DirectStoredValueForColumn(date, dt::CanonicalTypeId::date);
    const auto clock = bulk::DirectStoredValueForColumn(time, dt::CanonicalTypeId::time);
    const auto stamp = bulk::DirectStoredValueForColumn(timestamp, dt::CanonicalTypeId::timestamp);
    for (const bool null : {false, true}) {
      const auto day_value = null ? api::CrudStoredValue::SqlNull() : day;
      const auto clock_value = null ? api::CrudStoredValue::SqlNull() : clock;
      const auto stamp_value = null ? api::CrudStoredValue::SqlNull() : stamp;
      // Retained input can be reordered or sparse. Its position is not the
      // physical column ordinal and must not select a different receiver.
      const api::CrudValueFields reordered{{"clock", clock_value}, {"day", day_value}};
      const auto cells = bulk::DirectPhysicalCells(reordered, &plan);
      Check(cells.size() == 2 && cells[0].column_ordinal == 3 && cells[1].column_ordinal == 1,
            "retained temporal ordinals followed input order");
      Check(cells[0].value.type_id == dt::CanonicalTypeId::time &&
                cells[1].value.type_id == dt::CanonicalTypeId::date &&
                cells[0].value.is_null == null && cells[1].value.is_null == null &&
                cells[0].value.payload == (null ? std::vector<std::uint8_t>{} : time.binary_value) &&
                cells[1].value.payload == (null ? std::vector<std::uint8_t>{} : date.binary_value),
            "retained temporal type, state or canonical payload changed");
      const auto sparse = bulk::DirectPhysicalCells({{"clock", clock_value}}, &plan);
      Check(sparse.size() == 1 && sparse[0].column_ordinal == 3 &&
                sparse[0].value.type_id == dt::CanonicalTypeId::time,
            "sparse retained temporal binding lost");
      bool refused = false;
      try { (void)bulk::DirectPhysicalCells({{"missing", clock_value}}, &plan); }
      catch (const std::invalid_argument&) { refused = true; }
      Check(refused, "missing retained receiving column accepted");
      for (bool sparse_timestamp : {false, true}) {
        const api::CrudValueFields fields = sparse_timestamp ? api::CrudValueFields{{"stamp", stamp_value}} :
            api::CrudValueFields{{"stamp", stamp_value}, {"day", day_value}, {"clock", clock_value}};
        const auto result = bulk::DirectPhysicalCells(fields, &plan);
        Check(result.size() == (sparse_timestamp ? 1 : 3) && result[0].column_ordinal == 4 &&
              result[0].value.type_id == dt::CanonicalTypeId::timestamp && result[0].value.is_null == null &&
              result[0].value.payload == (null ? std::vector<std::uint8_t>{} : timestamp.binary_value),
              "retained TIMESTAMP reordered/sparse/NULL binding lost");
      }
    }
    const std::string bytes{"\xff\0\x80", 3};
    const auto opaque = bulk::DirectPhysicalCells({{"opaque", api::CrudStoredValue(bytes)}}, &plan);
    Check(opaque.size() == 1 && opaque[0].column_ordinal == 2 &&
              opaque[0].value.type_id == dt::CanonicalTypeId::binary &&
              opaque[0].value.payload == std::vector<std::uint8_t>({255, 0, 128}),
          "opaque retained binary bytes were translated");
    bool refused = false;
    try { (void)bulk::DirectPhysicalCells({{"day", api::CrudStoredValue::SqlNull()}}, nullptr); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "profileless retained NULL accepted");
    auto oversized = plan;
    oversized.columns.resize(65536);
    oversized.columns.back() = last;
    oversized.columns.back().column_name = "overflow";
    refused = false;
    try { (void)bulk::DirectPhysicalCells({{"overflow", clock}}, &oversized); }
    catch (const std::invalid_argument&) { refused = true; }
    Check(refused, "retained physical ordinal truncated");
  }
}
int main() {
  for (const auto type : {dt::CanonicalTypeId::date, dt::CanonicalTypeId::time, dt::CanonicalTypeId::timestamp})
    for (const unsigned generation : {10u, 11u}) Run(type, generation);
  RetainedColumnBindings();
  std::cout << "direct_bulk_temporal_payload checks=" << checks << '\n';
}
