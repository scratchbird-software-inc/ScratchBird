// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "dml/direct_bulk_typed_row_codec.hpp"
#include "datatype_binary.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>

namespace api = scratchbird::engine::internal_api;
namespace codec = api::dml::detail;
namespace dt = scratchbird::core::datatypes;

void Require(bool condition, const char* message) {
  if (!condition) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}

template<class Operation> void RequireRefusal(Operation operation) {
  try { operation(); }
  catch (const std::invalid_argument&) { return; }
  Require(false, "unbound or malformed NULL was silently normalized");
}

int main() {
  for (const auto* name : {"int16", "uint16", "int32", "int64", "uuid", "character", "binary"}) {
    const auto expected = dt::CanonicalTypeIdFromStableName(name);
    Require(expected != dt::CanonicalTypeId::unknown, "test type was not resolved");
    api::InsertRowEncoderPlan plan;
    api::InsertRowEncoderColumnPlan column;
    column.column_name = "value";
    column.canonical_type_name = name;
    plan.columns.push_back(column);
    api::EngineTypedValue value;
    // A bound target, not the incoming NULL occurrence's descriptor, controls
    // the physical type. Type conversion admission occurs before this adapter.
    value.descriptor.canonical_type_name = "binary";
    value.setState(api::EngineValueState::sql_null);
    api::EngineRowValue row;
    row.fields.emplace_back("value", value);
    const auto optimized = codec::BuildDirectFixedWidthPayloadValidationPlan(plan, &row);
    const auto verify = [&](const auto& cell) {
      Require(cell.column_ordinal == 1 && cell.value.type_id == expected && cell.value.is_null &&
                  cell.value.payload.empty() && !cell.value.payload_is_toast_reference,
              "NULL lost bound type, ordinal or clean value state");
      const auto encoded = dt::EncodeDatatypeBinaryValue(cell.value);
      Require(encoded.ok(), "typed NULL could not be encoded by existing datatype codec");
      const auto decoded = dt::DecodeDatatypeBinaryValue(encoded.encoded);
      Require(decoded.ok() && decoded.value.type_id == expected && decoded.value.is_null &&
                  decoded.value.payload.empty() && !decoded.value.payload_is_toast_reference,
              "typed NULL binary round trip changed type or state");
    };
    for (const auto* selected : {static_cast<const decltype(optimized)*>(nullptr), &optimized}) {
      const auto cells = codec::DirectPhysicalCellsFromTypedInputRow(row, plan, selected);
      Require(cells.cells.size() == 1 && cells.null_cells == 1 && cells.typed_binary_cells == 0,
              "NULL cell count changed");
      verify(cells.cells.front());
      auto malformed = row;
      malformed.fields.front().second.binary_value = {0};
      RequireRefusal([&] { codec::DirectPhysicalCellsFromTypedInputRow(malformed, plan, selected); });
      malformed.fields.front().second.binary_value.clear();
      malformed.fields.front().second.encoded_value = "unexpected";
      RequireRefusal([&] { codec::DirectPhysicalCellsFromTypedInputRow(malformed, plan, selected); });
    }
    const auto retained = api::RowValuePairs(row);
    const auto cells = codec::DirectPhysicalCells(retained, &plan);
    Require(cells.size() == 1, "retained NULL cell missing");
    verify(cells.front());
    RequireRefusal([&] { codec::DirectPhysicalCells(retained, nullptr); });
    auto missing = plan;
    missing.columns.front().column_name = "another_column";
    RequireRefusal([&] { codec::DirectPhysicalCells(retained, &missing); });
    auto unbound = plan;
    unbound.columns.front().canonical_type_name = "unknown";
    RequireRefusal([&] { codec::DirectPhysicalCellsFromTypedInputRow(row, unbound); });
    RequireRefusal([&] { codec::DirectPhysicalCells(retained, &unbound); });
    const auto unbound_optimized = codec::BuildDirectFixedWidthPayloadValidationPlan(unbound, &row);
    Require(unbound_optimized.front().target_type == dt::CanonicalTypeId::unknown,
            "NULL occurrence inferred a missing column binding");
    RequireRefusal([&] { codec::DirectPhysicalCellsFromTypedInputRow(row, unbound, &unbound_optimized); });
  }
}
