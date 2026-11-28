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
#include "engine/sblr/sblr_literal_runtime.hpp"
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
namespace sblr = scratchbird::engine::sblr;

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

// The engine's documented layout: operation_id, result_kind, column_count, row_count, per column SIX descriptor fields (the identity triple and generation, then the declared
// decimal precision and scale: 0, 0 for a non-decimal column), then value[r.c]. OLD (before 2026-12-25, SB_NATIVE_VALUES_DECIMAL_SCALE): four fields per column.
std::vector<pr::Field> Vector(const std::vector<std::vector<std::int64_t>>& rows, std::size_t columns, const dt::DatatypeTypeCodecIdentityRowV1& row,
                              std::uint64_t column_precision = 0, std::uint64_t column_scale = 0) {
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
    fields.push_back({"column_precision[" + i + "]", pr::Kind::unsigned_integer, U64(column_precision)});
    fields.push_back({"column_scale[" + i + "]", pr::Kind::unsigned_integer, U64(column_scale)});
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


  // ---- S5: the native transaction boundary vector (COMMIT / ROLLBACK): strict decode, nothing guessed ----
  {
    const auto u64 = [](std::uint64_t v) { std::string s(8, '\0'); for (int i = 0; i < 8; ++i) s[i] = static_cast<char>(v >> (8 * i)); return s; };
    const std::string ended(16, '\x11'), fresh(16, '\x22');
    const auto make = [&](const std::string& finality, std::uint64_t ended_order, std::uint64_t new_order) {
      return std::vector<pr::Field>{{"operation_id", pr::Kind::text, "engine.op.txn_commit"},
                                    {"result_kind", pr::Kind::text, "native_transaction_boundary"},
                                    {"ended_order", pr::Kind::unsigned_integer, u64(ended_order)},
                                    {"ended_uuid", pr::Kind::uuid, ended},
                                    {"finality", pr::Kind::text, finality},
                                    {"new_order", pr::Kind::unsigned_integer, u64(new_order)},
                                    {"new_uuid", pr::Kind::uuid, fresh}};
    };
    sbsql::NativeTransactionBoundary boundary;
    using S = sbsql::NativeBoundaryParseStatus;
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("committed", 5, 6)), true, &boundary) == S::parsed && boundary.committed &&
              boundary.ended_order == 5 && boundary.new_order == 6 && boundary.ended_uuid == ended && boundary.new_uuid == fresh,
          "a commit boundary decodes to its orders, UUIDs and finality");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("rolled_back", 5, 6)), false, &boundary) == S::parsed && !boundary.committed,
          "a rollback boundary decodes");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("rolled_back", 5, 6)), true, nullptr) == S::refused,
          "finality that contradicts the statement (rolled_back for a COMMIT) is refused");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("committed", 5, 6)), false, nullptr) == S::refused,
          "finality that contradicts the statement (committed for a ROLLBACK) is refused");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("COMMITTED", 5, 6)), true, nullptr) == S::refused, "an unknown finality token is refused");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("committed", 5, 5)), true, nullptr) == S::refused, "a replacement that is not a later order is refused");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(make("committed", 0, 1)), true, nullptr) == S::refused, "an ended order of 0 is refused");
    auto extra = make("committed", 5, 6);
    extra.push_back({"message", pr::Kind::text, "committed"});
    Check(sbsql::ParseNativeTransactionBoundary(Encode(extra), true, nullptr) == S::refused, "an extra (prose) field is refused");
    auto renamed = make("committed", 5, 6);
    renamed[5].name = "next_order";
    Check(sbsql::ParseNativeTransactionBoundary(Encode(renamed), true, nullptr) == S::refused, "a renamed field is refused");
    Check(sbsql::ParseNativeTransactionBoundary("", true, nullptr) == S::not_native_boundary, "an empty payload is not a boundary");
    Check(sbsql::ParseNativeTransactionBoundary(Encode(std::vector<pr::Field>{{"operation_id", pr::Kind::text, "x"}, {"result_kind", pr::Kind::text, "bound_projection"}}), true, nullptr) == S::not_native_boundary,
          "another result_kind is not a boundary");
  }

  // ---- SB_NATIVE_VALUES_DECIMAL_LITERAL (2026-12-24): an exact DECIMAL column. The value is the cohort's own 24-byte base-1e9 codec body (Kind::bytes, never text),
  // produced HERE by the engine's own codec (EncodeSblrLiteralExactDecimalV1) from a lexical; the parser decodes it with the same codec and renders the codec's
  // canonical fixed-point spelling. Round trip: lexical -> engine codec bytes -> vector -> parser conversion -> client text. Sign, scale, zero and the maximum
  // precision (38) are each covered; the header bytes (sign bit + scale, precision) are asserted against constants written out here, not read from the codec. ----
  {
    const dt::DatatypeTypeCodecIdentityRowV1* decimal = nullptr;
    for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3())
      if (entry.legacy_fields.codec_id == "datatype.decimal.base1e9.le.v1" && entry.legacy_fields.canonical_name == "decimal") { decimal = &entry.legacy_fields; break; }
    Check(decimal != nullptr, "the compiled cohort has the exact-decimal identity (datatype.decimal.base1e9.le.v1)");
    if (decimal) {
      const auto decimal_vector = [&](const std::string& body, pr::Kind kind, std::uint64_t column_precision = 2, std::uint64_t column_scale = 1) {
        auto fields = Vector({{0}}, 1, *decimal, column_precision, column_scale);
        fields.back() = {"value[0.0]", kind, body};
        return fields;
      };
      struct Case { const char* lexical; const char* shown; unsigned sign_and_scale; unsigned precision; std::uint64_t column_precision; std::uint64_t column_scale; };
      // the last two columns are the column's DECLARED precision and scale (the written shape): the parser pads the fraction to the declared scale
      const Case cases[] = {
          {"1.5", "1.5", 1, 2, 2, 1}, {"-1.5", "-1.5", 0x81, 2, 2, 1}, {"0.1", "0.1", 1, 1, 1, 1}, {"-0.1", "-0.1", 0x81, 1, 1, 1}, {"0", "0", 0, 1, 1, 0},
          {"123.456", "123.456", 3, 6, 6, 3}, {"-9223372036854775809", "-9223372036854775809", 0x80, 19, 19, 0}, {"9223372036854775808", "9223372036854775808", 0, 19, 19, 0},
          {"99999999999999999999999999999999999999", "99999999999999999999999999999999999999", 0, 38, 38, 0},
          {"0.00000000000000000000000000000000000001", "0.00000000000000000000000000000000000001", 38, 38, 38, 38},
          {"1e2", "100", 0, 3, 3, 0}, {"5e-1", "0.5", 1, 1, 1, 1},
          // OLD (2026-12-24): {"0.10", "0.1", 1, 1} and {"-0.000", "0", 0, 1}: the display was the codec's canonical spelling. NEW (2026-12-25, owner Q1): the value bytes are still canonical
          // (trailing fractional zeros and a negative zero dropped, pinned by ia01_literal_runtime_test) and the DISPLAY follows the column's declared scale
          {"0.10", "0.10", 1, 1, 2, 2}, {"-0.000", "0.000", 0, 1, 3, 3}, {"1.5", "1.500", 1, 2, 4, 3}, {"100", "100.00", 0, 3, 5, 2}, {"-1", "-1.0", 0x80, 1, 20, 1}};
      for (const auto& c : cases) {
        const auto encoded = sblr::EncodeSblrLiteralExactDecimalV1(c.lexical);
        Check(encoded.ok, std::string("the engine codec accepts ") + c.lexical);
        if (!encoded.ok) continue;
        const std::string body(reinterpret_cast<const char*>(encoded.canonical_bytes.data()), encoded.canonical_bytes.size());
        Check(body.size() == 24 && static_cast<unsigned char>(body[0]) == c.sign_and_scale && static_cast<unsigned char>(body[1]) == c.precision,
              std::string(c.lexical) + ": header byte 0 (sign bit | scale) and byte 1 (precision) are the constants written here");
        const auto converted = sbsql::ConvertNativeScalarProjectionVector(Encode(decimal_vector(body, pr::Kind::bytes, c.column_precision, c.column_scale)), {"d"});
        Check(converted.status == sbsql::NativeVectorConversionStatus::converted, std::string(c.lexical) + " converts: " + converted.refusal_code + " " + converted.refusal_detail);
        const auto rows = Decode(converted.payload);
        Check(rows.ok && rows.rows.size() == 1 && rows.rows[0].size() == 1 && rows.rows[0][0] && *rows.rows[0][0] == c.shown,
              std::string(c.lexical) + ": the client row is the fixed-point spelling `" + c.shown + "` at the column's declared scale");
        Check(converted.payload.find("d:decimal:not_null") != std::string::npos, std::string(c.lexical) + ": the parser's row metadata names the column type decimal");
        // the same body through a signed_integer / text carrier or reversed is NOT a decimal value
        Check(sbsql::ConvertNativeScalarProjectionVector(Encode(decimal_vector(body, pr::Kind::text)), {"d"}).refusal_code == "SBSQL.NATIVE_RESULT.VALUE_INVALID",
              std::string(c.lexical) + ": a text carrier for a decimal column is refused (VALUE_INVALID)");
      }
      {
        // the declared shape is checked, never trusted: a value that does not fit it, and a shape that is not a decimal shape, are refused
        const auto body_of = [&](const char* lexical) {
          const auto e = sblr::EncodeSblrLiteralExactDecimalV1(lexical);
          return std::string(reinterpret_cast<const char*>(e.canonical_bytes.data()), e.canonical_bytes.size());
        };
        const auto refusal = [&](const std::string& body, std::uint64_t p, std::uint64_t sc) {
          const auto r = sbsql::ConvertNativeScalarProjectionVector(Encode(decimal_vector(body, pr::Kind::bytes, p, sc)), {"d"});
          return r.status == sbsql::NativeVectorConversionStatus::refused ? r.refusal_code + "/" + r.refusal_detail : std::string("(not refused)");
        };
        Check(refusal(body_of("1.5"), 1, 0) == "SBSQL.NATIVE_RESULT.VALUE_INVALID/value_0_0", "1.5 does not fit decimal(1,0) (scale): refused");
        Check(refusal(body_of("12.5"), 2, 1) == "SBSQL.NATIVE_RESULT.VALUE_INVALID/value_0_0", "12.5 does not fit decimal(2,1) (integer digits): refused");
        Check(refusal(body_of("0.25"), 2, 1) == "SBSQL.NATIVE_RESULT.VALUE_INVALID/value_0_0", "0.25 does not fit decimal(2,1) (scale): refused");
        Check(refusal(body_of("0.5"), 1, 1) == "(not refused)", "0.5 fits decimal(1,1) (a pure fraction has no integer digits)");
        Check(refusal(body_of("0"), 1, 1) == "(not refused)", "zero fits decimal(1,1)");
        Check(refusal(body_of("1.5"), 0, 0) == "SBSQL.NATIVE_RESULT.VECTOR_INVALID/column_shape_0", "a decimal column declaring precision 0 is refused");
        Check(refusal(body_of("1.5"), 39, 1) == "SBSQL.NATIVE_RESULT.VECTOR_INVALID/column_shape_0", "a decimal column declaring precision 39 is refused");
        Check(refusal(body_of("1.5"), 2, 3) == "SBSQL.NATIVE_RESULT.VECTOR_INVALID/column_shape_0", "a decimal column declaring scale > precision is refused");
        auto int_with_shape = Vector({{1}}, 1, *int64, 2, 1);
        Check(sbsql::ConvertNativeScalarProjectionVector(Encode(int_with_shape), {"i"}).refusal_code == "SBSQL.NATIVE_RESULT.VECTOR_INVALID", "an int64 column declaring a decimal shape is refused");
        auto short_layout = Vector({{1}}, 1, *int64);
        short_layout.erase(short_layout.begin() + 8, short_layout.begin() + 10);  // the OLD four-field-per-column layout
        Check(sbsql::ConvertNativeScalarProjectionVector(Encode(short_layout), {"i"}).refusal_code == "SBSQL.NATIVE_RESULT.VECTOR_INVALID", "the OLD four-field column layout is refused (field_count)");
      }
      const auto good = sblr::EncodeSblrLiteralExactDecimalV1("-1.5");
      const std::string body(reinterpret_cast<const char*>(good.canonical_bytes.data()), good.canonical_bytes.size());
      const auto code_of = [&](const std::string& value) {
        const auto r = sbsql::ConvertNativeScalarProjectionVector(Encode(decimal_vector(value, pr::Kind::bytes)), {"d"});
        return r.status == sbsql::NativeVectorConversionStatus::refused ? r.refusal_code : std::string("(not refused)");
      };
      Check(code_of(std::string(body.rbegin(), body.rend())) == "SBSQL.NATIVE_RESULT.VALUE_INVALID", "the codec body reversed is refused (VALUE_INVALID), never rendered");
      Check(code_of(body.substr(0, 23)) == "SBSQL.NATIVE_RESULT.VALUE_INVALID", "a 23-byte body is refused");
      Check(code_of(body + std::string(1, '\0')) == "SBSQL.NATIVE_RESULT.VALUE_INVALID", "a 25-byte body is refused");
      auto flipped = body;
      flipped[0] = static_cast<char>(static_cast<unsigned char>(flipped[0]) ^ 0x80);  // sign flipped: still canonical (-1.5 -> 1.5), so it decodes: the carrier IS the sign
      Check(sbsql::ConvertNativeScalarProjectionVector(Encode(decimal_vector(flipped, pr::Kind::bytes)), {"d"}).status == sbsql::NativeVectorConversionStatus::converted &&
                *Decode(sbsql::ConvertNativeScalarProjectionVector(Encode(decimal_vector(flipped, pr::Kind::bytes)), {"d"}).payload).rows[0][0] == "1.5",
            "the sign bit is the sign: flipping it in the body gives 1.5 (so a vector test must pin the sign, which route_b does)");
      auto noncanonical = body;
      noncanonical[1] = 9;  // precision byte disagrees with the coefficient
      Check(code_of(noncanonical) == "SBSQL.NATIVE_RESULT.VALUE_INVALID", "a non-canonical header (precision) is refused");
      // a decimal body under the int64 identity (and the int64 carrier under the decimal identity) is refused
      Check(sbsql::ConvertNativeScalarProjectionVector(Encode([&] { auto f = Vector({{0}}, 1, *int64); f.back() = {"value[0.0]", pr::Kind::bytes, body}; return f; }()), {"d"}).refusal_code ==
                "SBSQL.NATIVE_RESULT.VALUE_INVALID", "a decimal body under the int64 identity is refused");
      Check(sbsql::ConvertNativeScalarProjectionVector(Encode(Vector({{1}}, 1, *decimal, 2, 1)), {"d"}).refusal_code == "SBSQL.NATIVE_RESULT.VALUE_INVALID",
            "an int64 carrier under the decimal identity is refused");
      // two columns: int64 and decimal in ONE row, each with its own identity (the group is four fields per column either way)
      auto mixed = Vector({{7, 0}}, 2, *int64);
      for (auto& f : mixed) {
        if (f.name == "column_datatype[1]") f.value = Bytes16(decimal->type_uuid);
        if (f.name == "column_datatype_descriptor[1]") f.value = Bytes16(decimal->descriptor_uuid);
        if (f.name == "column_datatype_generation[1]") f.value = U64(decimal->descriptor_generation);
        if (f.name == "column_precision[1]") f.value = U64(2);
        if (f.name == "column_scale[1]") f.value = U64(1);
        if (f.name == "value[0.1]") f = {"value[0.1]", pr::Kind::bytes, body};
      }
      const auto mixed_converted = sbsql::ConvertNativeScalarProjectionVector(Encode(mixed), {"a", "b"});
      const auto mixed_rows = Decode(mixed_converted.payload);
      Check(mixed_converted.status == sbsql::NativeVectorConversionStatus::converted && mixed_rows.ok && *mixed_rows.rows[0][0] == "7" && *mixed_rows.rows[0][1] == "-1.5" &&
                mixed_converted.payload.find("a:int64:not_null;b:decimal:not_null") != std::string::npos,
            "an int64 column and a decimal column in one row each render by their own identity");
    }
  }

  // ---- the engine's decimal fit rules (SB_NATIVE_VALUES_DECIMAL_UNIFICATION / _SCALE, 2026-12-25): the functions the engine's VALUES validation sites, its typed-literal evaluation and its
  // vector encoder call. A shape FITS a column when scale <= the column's and integer digits <= the column's integer digits; a canonical body FITS when its VALUE does (zero has no integer digits). ----
  {
    const auto body = [&](const char* lexical) {
      const auto e = sblr::EncodeSblrLiteralExactDecimalV1(lexical);
      return std::string(reinterpret_cast<const char*>(e.canonical_bytes.data()), e.canonical_bytes.size());
    };
    const auto bytes_fit = [&](const char* lexical, std::uint32_t p, std::uint32_t sc) {
      const auto b = body(lexical);
      return sblr::SblrDecimalBodyFitsColumnV1(reinterpret_cast<const std::uint8_t*>(b.data()), b.size(), p, sc);
    };
    struct BodyCase { const char* lexical; std::uint32_t p, s; bool fits; };
    for (const auto& c : {BodyCase{"1.5", 2, 1, true}, BodyCase{"1.5", 3, 2, true}, BodyCase{"1.5", 1, 0, false}, BodyCase{"12.5", 2, 1, false}, BodyCase{"12.5", 3, 1, true},
                          BodyCase{"0.25", 2, 1, false}, BodyCase{"0.25", 2, 2, true}, BodyCase{"0.5", 1, 1, true}, BodyCase{"0", 1, 1, true}, BodyCase{"0", 1, 0, true},
                          BodyCase{"-0.000", 3, 3, true}, BodyCase{"100", 3, 0, true}, BodyCase{"100", 2, 0, false}, BodyCase{"100", 21, 2, true}, BodyCase{"100", 5, 3, false},
                          BodyCase{"99999999999999999999999999999999999999", 38, 0, true}, BodyCase{"99999999999999999999999999999999999999", 38, 1, false},
                          BodyCase{"0.00000000000000000000000000000000000001", 38, 38, true}, BodyCase{"1.5", 0, 0, false}, BodyCase{"1.5", 39, 1, false}, BodyCase{"1.5", 2, 3, false}})
      Check(bytes_fit(c.lexical, c.p, c.s) == c.fits, std::string("body ") + c.lexical + " in decimal(" + std::to_string(c.p) + "," + std::to_string(c.s) + ") " + (c.fits ? "fits" : "does not fit"));
    const auto b = body("1.5");
    Check(!sblr::SblrDecimalBodyFitsColumnV1(reinterpret_cast<const std::uint8_t*>(b.data()), 23, 2, 1) && !sblr::SblrDecimalBodyFitsColumnV1(nullptr, 24, 2, 1),
          "a truncated or absent body never fits");
    using O = std::optional<std::uint32_t>;
    struct ShapeCase { O lp, ls, cp, cs; bool fits; };
    for (const auto& c : {ShapeCase{2, 1, 3, 2, true}, ShapeCase{3, 2, 3, 2, true}, ShapeCase{3, 2, 2, 1, false}, ShapeCase{2, 1, 2, 1, true}, ShapeCase{3, 1, 3, 2, false}, ShapeCase{4, 1, 3, 2, false},
                          ShapeCase{1, 1, 2, 2, true}, ShapeCase{2, 2, 2, 1, false}, ShapeCase{std::nullopt, std::nullopt, std::nullopt, std::nullopt, true}, ShapeCase{2, 1, std::nullopt, std::nullopt, false},
                          ShapeCase{std::nullopt, std::nullopt, 2, 1, false}, ShapeCase{1, 2, 3, 2, false}})
      Check(sblr::SblrDecimalShapeFitsColumnV1(c.lp, c.ls, c.cp, c.cs) == c.fits, "shape fit rule case");
  }

  std::cout << "native_result_vector_parse: " << checks << " checks, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
