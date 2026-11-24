// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0
//
// SEARCH_KEY: SB_NATIVE_RESULT_VECTOR_PARSER_TEST (S4h part 1; owner Q310).
//
// The SBsql parser side of a native result: ConvertNativeScalarProjectionVector turns the engine's native_scalar_projection_vector (values
// and descriptor UUIDs, NO names) into the client-visible row, taking the column NAME from the statement's own alias and the TYPE from the
// descriptor UUIDs through the compiled datatype cohort. The vectors here are built byte for byte to the layout the engine documents
// (public_abi.cpp, SB_NATIVE_RESULT_MESSAGE_VECTOR); native_select_one_test feeds the REAL engine's vector through the same converter.
#include "datatype_catalog_manifest.hpp"
#include "wire/native_result_vector.hpp"
#include "wire/public_result_packet.hpp"
#include "wire/sbsql_test_wire.hpp"

#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace dt = scratchbird::core::datatypes;
namespace pr = scratchbird::wire::public_result;
namespace sbsql = scratchbird::parser::sbsql;

namespace {
int checks = 0;
int failures = 0;
void Check(bool ok, const std::string& why) {
  ++checks;
  if (!ok) {
    ++failures;
    std::cerr << "FAIL: " << why << '\n';
  }
}
std::string U64(std::uint64_t value) {
  std::string out(8, '\0');
  for (unsigned i = 0; i < 8; ++i) out[i] = static_cast<char>(value >> (i * 8));
  return out;
}
std::string Bytes16(const scratchbird::core::platform::Uuid& id) {
  return std::string(reinterpret_cast<const char*>(id.bytes.data()), 16);
}
const dt::DatatypeTypeCodecIdentityRowV1* Int64Row() {
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3())
    if (entry.legacy_fields.codec_id == "datatype.int64.le.v1" && entry.legacy_fields.canonical_name == "bigint") return &entry.legacy_fields;
  return nullptr;
}
const dt::DatatypeTypeCodecIdentityRowV1* NonInt64Row() {
  for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3())
    if (entry.legacy_fields.codec_id != "datatype.int64.le.v1") return &entry.legacy_fields;
  return nullptr;
}

// The engine's documented layout: operation_id, result_kind, column_count, row_count, per column four descriptor fields, then value[r.c].
std::vector<pr::Field> Vector(const std::vector<std::vector<std::int64_t>>& rows, std::size_t columns, const dt::DatatypeTypeCodecIdentityRowV1& row) {
  std::vector<pr::Field> fields;
  fields.push_back({"operation_id", pr::Kind::text, "query.evaluate_projection"});
  fields.push_back({"result_kind", pr::Kind::text, "native_scalar_projection_vector"});
  fields.push_back({"column_count", pr::Kind::unsigned_integer, U64(columns)});
  fields.push_back({"row_count", pr::Kind::unsigned_integer, U64(rows.size())});
  for (std::size_t c = 0; c < columns; ++c) {
    const auto i = std::to_string(c);
    std::string descriptor(16, '\0');
    descriptor[6] = 0x70;
    descriptor[8] = static_cast<char>(0x80);
    descriptor[15] = static_cast<char>(c + 1);
    fields.push_back({"column_descriptor[" + i + "]", pr::Kind::uuid, descriptor});
    fields.push_back({"column_datatype[" + i + "]", pr::Kind::uuid, Bytes16(row.type_uuid)});
    fields.push_back({"column_datatype_descriptor[" + i + "]", pr::Kind::uuid, Bytes16(row.descriptor_uuid)});
    fields.push_back({"column_datatype_generation[" + i + "]", pr::Kind::unsigned_integer, U64(row.descriptor_generation)});
  }
  for (std::size_t r = 0; r < rows.size(); ++r)
    for (std::size_t c = 0; c < columns; ++c)
      fields.push_back({"value[" + std::to_string(r) + "." + std::to_string(c) + "]", pr::Kind::signed_integer, U64(static_cast<std::uint64_t>(rows[r][c]))});
  return fields;
}
std::string Encode(const std::vector<pr::Field>& fields) {
  std::string out;
  Check(pr::Encode(fields, &out), "encode a fixture packet");
  return out;
}
// The packet the LEGACY engine rendered for the same rows (api_result_payload: row[i] a row record, row_meta[i] the type metadata).
std::string LegacyPacket(const std::vector<std::string>& names, const std::vector<std::vector<std::int64_t>>& rows) {
  std::vector<pr::Field> out;
  out.push_back({"operation_id", pr::Kind::text, "query.evaluate_projection"});
  out.push_back({"result_kind", pr::Kind::text, "scalar_projection_rows"});
  out.push_back({"row_count", pr::Kind::text, std::to_string(rows.size())});
  std::string metadata;
  for (std::size_t c = 0; c < names.size(); ++c) metadata += (c ? ";" : "") + names[c] + ":int64:not_null";
  for (std::size_t r = 0; r < rows.size(); ++r) {
    std::vector<pr::Field> row;
    for (std::size_t c = 0; c < names.size(); ++c) row.push_back({names[c], pr::Kind::signed_integer, U64(static_cast<std::uint64_t>(rows[r][c]))});
    std::string record;
    Check(pr::Encode(row, &record), "encode a legacy row record");
    out.push_back({"row[" + std::to_string(r) + "]", pr::Kind::row, record});
    out.push_back({"row_meta[" + std::to_string(r) + "]", pr::Kind::text, metadata});
  }
  return Encode(out);
}

struct Rows {
  bool ok = false;
  std::vector<std::uint32_t> oids;
  std::vector<std::vector<std::optional<std::string>>> rows;
};
Rows Decode(const std::string& packet) {
  Rows out;
  out.ok = sbsql::DecodePublicResultRowsForTest(packet, &out.oids, &out.rows);
  return out;
}
}  // namespace

int main() {
  const auto* int64 = Int64Row();
  const auto* other = NonInt64Row();
  Check(int64 != nullptr && other != nullptr, "the compiled cohort has the int64 identity and a non-int64 identity");
  if (!int64 || !other) return 1;

  // ---- `SELECT 1 AS one` and `SELECT 2 AS two`: one column, one row ----
  for (const std::int64_t value : {1, 2}) {
    const std::string alias = value == 1 ? "one" : "two";
    const auto native = Encode(Vector({{value}}, 1, *int64));
    const auto converted = sbsql::ConvertNativeScalarProjectionVector(native, {alias});
    Check(converted.status == sbsql::NativeVectorConversionStatus::converted, "value " + std::to_string(value) + " converts: " + converted.refusal_code + " " + converted.refusal_detail);
    Check(converted.payload == LegacyPacket({alias}, {{value}}), "value " + std::to_string(value) + ": the converted packet is byte-identical to the legacy engine's packet for the same rows");
    const auto rows = Decode(converted.payload);
    Check(rows.ok && rows.rows.size() == 1 && rows.rows[0].size() == 1 && rows.rows[0][0] && *rows.rows[0][0] == std::to_string(value),
          "value " + std::to_string(value) + ": the client row is the value");
    Check(rows.oids.size() == 1, "one client-visible column type");
  }

  // ---- the NAME is the parser's alias, never the engine's ----
  {
    const auto native = Encode(Vector({{7}}, 1, *int64));
    const auto a = sbsql::ConvertNativeScalarProjectionVector(native, {"alpha"});
    const auto b = sbsql::ConvertNativeScalarProjectionVector(native, {"beta"});
    Check(a.status == sbsql::NativeVectorConversionStatus::converted && b.status == sbsql::NativeVectorConversionStatus::converted &&
              a.payload != b.payload && a.payload == LegacyPacket({"alpha"}, {{7}}) && b.payload == LegacyPacket({"beta"}, {{7}}),
          "the same engine vector yields the name the parser supplies");
    // the engine sending a name of its own is a malformed vector (the layout has no name field), not a hint
    auto with_name = Vector({{7}}, 1, *int64);
    with_name.insert(with_name.begin() + 4, {"column_name[0]", pr::Kind::text, "engine_picked"});
    const auto refused = sbsql::ConvertNativeScalarProjectionVector(Encode(with_name), {"alpha"});
    Check(refused.status == sbsql::NativeVectorConversionStatus::refused && refused.refusal_code == "SBSQL.NATIVE_RESULT.VECTOR_INVALID",
          "an engine-supplied name field is refused, never used");
    Check(refused.payload.find("engine_picked") == std::string::npos, "and nothing of it reaches the client packet");
  }

  // ---- two columns, and two rows ----
  {
    const auto native = Encode(Vector({{1, 2}}, 2, *int64));
    const auto converted = sbsql::ConvertNativeScalarProjectionVector(native, {"one", "two"});
    Check(converted.status == sbsql::NativeVectorConversionStatus::converted && converted.payload == LegacyPacket({"one", "two"}, {{1, 2}}),
          "two columns: byte-identical to the legacy packet");
    const auto rows = Decode(converted.payload);
    Check(rows.ok && rows.rows.size() == 1 && rows.rows[0].size() == 2 && *rows.rows[0][0] == "1" && *rows.rows[0][1] == "2" && rows.oids.size() == 2,
          "two columns: the client row is (1, 2)");
    const auto two_rows = sbsql::ConvertNativeScalarProjectionVector(Encode(Vector({{1, 2}, {3, -4}}, 2, *int64)), {"a", "b"});
    const auto decoded = Decode(two_rows.payload);
    Check(two_rows.status == sbsql::NativeVectorConversionStatus::converted && decoded.ok && decoded.rows.size() == 2 && *decoded.rows[1][0] == "3" && *decoded.rows[1][1] == "-4",
          "two rows with a negative value decode in order");
  }

  // ---- refused shapes surface the parser's specific code (a machine token), no engine prose ----
  {
    const auto good = Vector({{1}}, 1, *int64);
    const auto code_of = [&](const std::vector<pr::Field>& fields, const std::vector<std::string>& names) {
      const auto r = sbsql::ConvertNativeScalarProjectionVector(Encode(fields), names);
      return r.status == sbsql::NativeVectorConversionStatus::refused ? r.refusal_code : std::string("(not refused)");
    };
    Check(code_of(Vector({{1}}, 1, *other), {"one"}) == "SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "a non-int64 datatype cohort row: type not built");
    auto unknown = good;
    unknown[5].value = std::string(16, '\x11');
    Check(code_of(unknown, {"one"}) == "SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "an unknown type uuid: type not built (never guessed)");
    auto wrong_generation = good;
    wrong_generation[7].value = U64(999999);
    Check(code_of(wrong_generation, {"one"}) == "SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "a wrong descriptor generation: type not built");
    auto text_value = good;
    text_value.back() = {"value[0.0]", pr::Kind::text, "1"};
    Check(code_of(text_value, {"one"}) == "SBSQL.NATIVE_RESULT.VALUE_INVALID", "a text carrier where an int64 is required: value invalid");
    auto short_value = good;
    short_value.pop_back();
    Check(code_of(short_value, {"one"}) == "SBSQL.NATIVE_RESULT.VECTOR_INVALID", "a missing value: vector invalid");
    auto misnamed = good;
    misnamed.back().name = "value[0.1]";
    Check(code_of(misnamed, {"one"}) == "SBSQL.NATIVE_RESULT.VALUE_INVALID", "a value out of its position: value invalid");
    Check(code_of(good, {}) == "SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "no alias from the parser: name missing (no name is invented)");
    Check(code_of(good, {"one", "two"}) == "SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "an alias count that differs from the column count: name missing");
    Check(code_of(good, {""}) == "SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "an empty alias: name missing");
    auto zero_columns = Vector({}, 0, *int64);
    Check(code_of(zero_columns, {}) == "SBSQL.NATIVE_RESULT.VECTOR_INVALID", "zero columns: vector invalid");
  }

  // ---- a legacy packet, an engine error packet and plain text are NOT native vectors and pass through untouched ----
  {
    const auto legacy = LegacyPacket({"one"}, {{1}});
    const auto r = sbsql::ConvertNativeScalarProjectionVector(legacy, {"one"});
    Check(r.status == sbsql::NativeVectorConversionStatus::not_native_vector && r.payload.empty(), "a legacy rendered packet is not converted");
    Check(Decode(legacy).ok && *Decode(legacy).rows[0][0] == "1", "the legacy rendered rows still decode unchanged");
    Check(sbsql::ConvertNativeScalarProjectionVector("", {"one"}).status == sbsql::NativeVectorConversionStatus::not_native_vector, "an empty payload is left alone");
    Check(sbsql::ConvertNativeScalarProjectionVector("row[0]=one=1\n", {"one"}).status == sbsql::NativeVectorConversionStatus::not_native_vector, "a text payload is left alone");
    std::vector<pr::Field> other_kind = {{"operation_id", pr::Kind::text, "query.evaluate_projection"}, {"result_kind", pr::Kind::text, "bound_projection"}};
    Check(sbsql::ConvertNativeScalarProjectionVector(Encode(other_kind), {"one"}).status == sbsql::NativeVectorConversionStatus::not_native_vector,
          "another result_kind is left alone");
  }

  std::cout << "native_result_vector_parse: " << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
