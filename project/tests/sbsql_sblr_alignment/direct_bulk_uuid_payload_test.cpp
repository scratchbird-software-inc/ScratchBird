// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0
#include "dml/direct_bulk_typed_row_codec.hpp"
#include <iostream>
#include <stdexcept>
namespace api = scratchbird::engine::internal_api;
namespace bulk = api::dml::detail;
namespace dt = scratchbird::core::datatypes;
void Check(bool ok) { if (!ok) throw std::runtime_error("binary UUID payload contract failed"); }
int main() {
  try {
    for (auto type : {dt::CanonicalTypeId::uuid, dt::CanonicalTypeId::enum_value}) {
      api::EngineTypedValue value;
      value.descriptor.canonical_type_name = dt::CanonicalTypeName(type);
      value.binary_value = {0, '\n', '|', '\t', 0xff, 0, 0x70, 0, 0x80, 0, 1, 2, 3, 4, 5, 6};
      std::vector<unsigned char> output = {0xee};
      Check(bulk::DirectPackTypedPayload(type, value, &output));
      Check(output == value.binary_value);
      bulk::DirectFixedWidthPayloadValidationColumnPlan column;
      column.column_name = "identity"; column.canonical_type_name = value.descriptor.canonical_type_name;
      column.target_type = type; column.inline_fixed = true; column.inline_bytes = 16;
      api::EngineRowValue row; row.fields = {{"identity", value}};
      Check(bulk::DirectFixedWidthTypedPayloadFailure(row, {column}).empty());
      for (std::size_t length : {0u, 1u, 15u, 17u, 36u}) {
        row.fields[0].second.binary_value.resize(length, 0x7f);
        output = {0xee};
        Check(!bulk::DirectPackTypedPayload(type, row.fields[0].second, &output));
        Check(output == std::vector<unsigned char>{0xee});
        Check(!bulk::DirectFixedWidthTypedPayloadFailure(row, {column}).empty());
      }
      for (bool with_binary : {false, true}) {
        row.fields[0].second = value;
        if (!with_binary) row.fields[0].second.binary_value.clear();
        row.fields[0].second.encoded_value = "018f7a10-1280-7000-8000-000000000105";
        output = {0xee};
        Check(!bulk::DirectPackTypedPayload(type, row.fields[0].second, &output));
        Check(output == std::vector<unsigned char>{0xee});
        Check(!bulk::DirectFixedWidthTypedPayloadFailure(row, {column}).empty());
      }
      // UUID values may be nil or use a user-data UUID version. This boundary
      // validates representation, not engine-issued identity authority.
      value.binary_value.assign(16, 0);
      Check(bulk::DirectPackTypedPayload(type, value, &output));
      Check(output == value.binary_value);
    }
    std::cout << "PASS binary-only direct bulk UUID payloads\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
