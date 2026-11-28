// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/native_result_vector.hpp"

#include "datatype_catalog_manifest.hpp"
#include "engine/sblr/sblr_literal_runtime.hpp"
#include "wire/public_result_packet.hpp"

#include <cstdint>
#include <string_view>

namespace scratchbird::parser::sbsql {
namespace {
namespace pr = scratchbird::wire::public_result;

std::uint64_t Le64(std::string_view bytes) {
  std::uint64_t value = 0;
  for (std::size_t i = 0; i < 8 && i < bytes.size(); ++i)
    value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[i])) << (8 * i);
  return value;
}

NativeVectorConversion Refuse(std::string code, std::string detail) {
  NativeVectorConversion out;
  out.status = NativeVectorConversionStatus::refused;
  out.refusal_code = std::move(code);
  out.refusal_detail = std::move(detail);
  return out;
}

bool IsNativeVector(const std::vector<pr::Field>& fields) {
  return fields.size() >= 2 && fields[1].name == "result_kind" && fields[1].kind == pr::Kind::text &&
         fields[1].value == "native_scalar_projection_vector";
}

std::string Index(std::size_t value) { return std::to_string(value); }
}  // namespace

NativeVectorConversion ConvertNativeScalarProjectionVector(std::string_view engine_payload,
                                                           const std::vector<std::string>& column_names) {
  namespace dt = scratchbird::core::datatypes;
  NativeVectorConversion none;
  std::vector<pr::Field> fields;
  if (!engine_payload.starts_with(pr::kMagic) || !pr::Decode(engine_payload, &fields) || !IsNativeVector(fields)) return none;

  // ---- the fixed layout, strictly: operation_id, result_kind, column_count, row_count, 4 fields per column, then the values ----
  const auto invalid = [](std::string detail) { return Refuse("SBSQL.NATIVE_RESULT.VECTOR_INVALID", std::move(detail)); };
  if (fields.size() < 4 || fields[0].name != "operation_id" || fields[0].kind != pr::Kind::text ||
      fields[2].name != "column_count" || fields[2].kind != pr::Kind::unsigned_integer ||
      fields[3].name != "row_count" || fields[3].kind != pr::Kind::unsigned_integer)
    return invalid("header");
  const auto columns = Le64(fields[2].value);
  const auto rows = Le64(fields[3].value);
  if (columns == 0 || columns > 4096 || rows > 1048576) return invalid("extent");
  // SB_NATIVE_VALUES_DECIMAL_SCALE (2026-12-25): SIX fields per column (OLD: four): the identity triple + generation, then column_precision and column_scale
  // SB_NATIVE_VALUES_NULL_CELLS (2026-12-26; owner 2026-11-28 N1): SEVEN fields per column (the seventh is column_nullability), and the cell fields are DATA-DEPENDENT: a not-null column
  // has one value field per cell; a nullable column has a state field per cell and a value field only for a non-NULL cell. The cell fields are therefore walked sequentially below and the
  // walk must land EXACTLY on the last field (too few and too many fields are both refused). Here only the minimum is checked.
  if (fields.size() < 4 + 7 * columns) return invalid("field_count");
  if (column_names.size() != columns) return Refuse("SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "name_count");
  for (const auto& name : column_names)
    if (name.empty()) return Refuse("SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "empty_name");

  // ---- the type of each column, from its descriptor UUIDs through the compiled cohort identity table ----
  std::vector<bool> column_is_boolean;
  std::vector<bool> column_nullable;  // the column's DECLARED nullability (the vector's column_nullability field)
  std::vector<std::pair<std::uint32_t, std::uint32_t>> column_shape;  // declared decimal (precision, scale) per column; (0, 0) for a non-decimal column
  std::vector<bool> column_is_decimal;  // SB_NATIVE_VALUES_DECIMAL_LITERAL (2026-12-24): the decimal codec identity (datatype.decimal.base1e9.le.v1)
  for (std::uint64_t c = 0; c < columns; ++c) {
    const auto base = 4 + 7 * c;
    const auto index = Index(static_cast<std::size_t>(c));
    const auto& descriptor = fields[base];
    const auto& datatype = fields[base + 1];
    const auto& datatype_descriptor = fields[base + 2];
    const auto& generation = fields[base + 3];
    if (descriptor.name != "column_descriptor[" + index + "]" || descriptor.kind != pr::Kind::uuid ||
        datatype.name != "column_datatype[" + index + "]" || datatype.kind != pr::Kind::uuid ||
        datatype_descriptor.name != "column_datatype_descriptor[" + index + "]" || datatype_descriptor.kind != pr::Kind::uuid ||
        generation.name != "column_datatype_generation[" + index + "]" || generation.kind != pr::Kind::unsigned_integer ||
        fields[base + 4].name != "column_precision[" + index + "]" || fields[base + 4].kind != pr::Kind::unsigned_integer || fields[base + 4].value.size() != 8 ||
        fields[base + 5].name != "column_scale[" + index + "]" || fields[base + 5].kind != pr::Kind::unsigned_integer || fields[base + 5].value.size() != 8 ||
        fields[base + 6].name != "column_nullability[" + index + "]" || fields[base + 6].kind != pr::Kind::unsigned_integer || fields[base + 6].value.size() != 8)
      return invalid("column_" + index);
    const auto declared_nullability = Le64(fields[base + 6].value);
    if (declared_nullability > 1) return invalid("column_nullability_" + index);
    const auto declared_precision = Le64(fields[base + 4].value);
    const auto declared_scale = Le64(fields[base + 5].value);
    // The compiled table holds one row per (snapshot, generation) of the same identity; the vector names the identity (type, descriptor,
    // descriptor generation) and the receipt's snapshot already fixed which row the engine used. Every row that matches the identity must
    // be the int64 codec; an identity that matches rows of different codecs, or none, is refused (never a nearest match).
    // SB_NATIVE_VALUES_BOOLEAN_LITERAL (2026-12-13): two codecs are built, int64 and boolean (datatype.boolean.u8.v1); the column's kind is the codec of the
    // identity rows it matches (every matching row must agree).
    bool matched = false;
    bool all_int64 = true, all_boolean = true, all_decimal = true;
    for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
      const auto& row = entry.legacy_fields;
      if (std::string_view(reinterpret_cast<const char*>(row.type_uuid.bytes.data()), 16) != datatype.value ||
          std::string_view(reinterpret_cast<const char*>(row.descriptor_uuid.bytes.data()), 16) != datatype_descriptor.value ||
          row.descriptor_generation != Le64(generation.value))
        continue;
      matched = true;
      all_int64 = all_int64 && row.codec_id == "datatype.int64.le.v1";
      all_boolean = all_boolean && row.codec_id == "datatype.boolean.u8.v1";
      all_decimal = all_decimal && row.codec_id == "datatype.decimal.base1e9.le.v1";
    }
    if (!matched) return Refuse("SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "unknown_type");
    if (!all_int64 && !all_boolean && !all_decimal) return Refuse("SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "type_codec");
    // a decimal column declares 1 <= precision <= 38 and scale <= precision; every other column declares (0, 0)
    if (all_decimal ? (declared_precision == 0 || declared_precision > 38 || declared_scale > declared_precision) : (declared_precision != 0 || declared_scale != 0))
      return Refuse("SBSQL.NATIVE_RESULT.VECTOR_INVALID", "column_shape_" + index);
    column_shape.push_back({static_cast<std::uint32_t>(declared_precision), static_cast<std::uint32_t>(declared_scale)});
    column_nullable.push_back(declared_nullability == 1);
    column_is_boolean.push_back(all_boolean);
    column_is_decimal.push_back(all_decimal);
  }

  // ---- the rows: value[r.c], row-major, each a signed 64-bit carrier ----
  std::vector<std::string> row_records;
  std::vector<std::string> row_metadata;
  // SB_NATIVE_VALUES_NULL_CELLS: the cell fields, walked sequentially (their count depends on the data). `at` is the next unread field.
  std::size_t at = static_cast<std::size_t>(4 + 7 * columns);
  for (std::uint64_t r = 0; r < rows; ++r) {
    std::vector<pr::Field> row_fields;
    std::string row_meta;
    for (std::uint64_t c = 0; c < columns; ++c) {
      const std::string cell = Index(static_cast<std::size_t>(r)) + "." + Index(static_cast<std::size_t>(c));
      const bool nullable_column = column_nullable[static_cast<std::size_t>(c)];
      bool null_cell = false;
      if (nullable_column) {
        // state[r.c]: u64 LE8, 0 = a value field follows, 1 = SQL NULL and NO value field follows
        if (at >= fields.size()) return invalid("field_count");
        const auto& state = fields[at++];
        if (state.name != "state[" + cell + "]" || state.kind != pr::Kind::unsigned_integer || state.value.size() != 8 || Le64(state.value) > 1)
          return Refuse("SBSQL.NATIVE_RESULT.VALUE_INVALID", "state_" + Index(static_cast<std::size_t>(r)) + "_" + Index(static_cast<std::size_t>(c)));
        null_cell = Le64(state.value) == 1;
      }
      if (c != 0) row_meta += ';';
      row_meta += column_names[static_cast<std::size_t>(c)] +
                  (column_is_boolean[static_cast<std::size_t>(c)] ? ":boolean" : column_is_decimal[static_cast<std::size_t>(c)] ? ":decimal" : ":int64") +
                  (null_cell ? ":null" : ":not_null");
      if (null_cell) {
        // a NULL cell: the parser's own existing NULL carrier (an EMPTY text cell with the `null` state in the row metadata, which the client decodes to NULL)
        row_fields.push_back({column_names[static_cast<std::size_t>(c)], pr::Kind::text, std::string()});
        continue;
      }
      if (at >= fields.size()) return invalid("field_count");
      const auto& value = fields[at++];
      const bool boolean_column = column_is_boolean[static_cast<std::size_t>(c)];
      const bool decimal_column = column_is_decimal[static_cast<std::size_t>(c)];
      if (decimal_column) {
        // an exact decimal value is the cohort's 24-byte codec body (Kind::bytes); the engine's own codec decodes it (sign, scale, precision are in
        // its header) and the PARSER renders it: the codec's canonical fixed-point spelling (scale kept: 0.10 stays 0.10), never engine text.
        const auto decoded = value.value.size() == scratchbird::engine::sblr::kSblrLiteralExactDecimalBytes
            ? scratchbird::engine::sblr::DecodeSblrLiteralExactDecimalV1(reinterpret_cast<const std::uint8_t*>(value.value.data()), value.value.size())
            : scratchbird::engine::sblr::SblrLiteralExactDecimalCodecResultV1{};
        // SB_NATIVE_VALUES_DECIMAL_SCALE (2026-12-25; owner 2026-11-28 Q1): the display scale is the COLUMN's declared scale (the value bytes are canonical, no trailing
        // fractional zeros): the parser pads the fraction to it (`0.10` shows 0.10, `0.0` shows 0.0). A value that does not fit the declared precision and scale is refused.
        const auto& shape = column_shape[static_cast<std::size_t>(c)];
        if (value.name != "value[" + Index(static_cast<std::size_t>(r)) + "." + Index(static_cast<std::size_t>(c)) + "]" ||
            value.kind != pr::Kind::bytes || !decoded.ok ||
            !scratchbird::engine::sblr::SblrDecimalBodyFitsColumnV1(reinterpret_cast<const std::uint8_t*>(value.value.data()), value.value.size(), shape.first, shape.second))
          return Refuse("SBSQL.NATIVE_RESULT.VALUE_INVALID", "value_" + Index(static_cast<std::size_t>(r)) + "_" + Index(static_cast<std::size_t>(c)));
        std::string shown = decoded.canonical_lexical;
        if (shape.second != 0) {
          const auto point = shown.find('.');
          const std::size_t have = point == std::string::npos ? 0 : shown.size() - point - 1;
          if (point == std::string::npos) shown += '.';
          shown.append(shape.second - have, '0');
        }
        row_fields.push_back({column_names[static_cast<std::size_t>(c)], pr::Kind::text, std::move(shown)});
        continue;
      }
      // an int64 value is a signed 64-bit carrier; a boolean value is ONE byte, 0 or 1 (the engine's vector carrier, Kind::bytes)
      if (value.name != "value[" + Index(static_cast<std::size_t>(r)) + "." + Index(static_cast<std::size_t>(c)) + "]" ||
          (boolean_column ? (value.kind != pr::Kind::bytes || value.value.size() != 1 || static_cast<unsigned char>(value.value[0]) > 1)
                          : (value.kind != pr::Kind::signed_integer || value.value.size() != 8)))
        return Refuse("SBSQL.NATIVE_RESULT.VALUE_INVALID", "value_" + Index(static_cast<std::size_t>(r)) + "_" + Index(static_cast<std::size_t>(c)));
      // The parser renders the value for the client: an int64 stays a signed 64-bit carrier; a boolean becomes the text `true` / `false` (the forms the
      // client's own typing recognizes as a boolean column; a display choice, the parser's business: the engine sent one byte 0 or 1).
      if (boolean_column)
        row_fields.push_back({column_names[static_cast<std::size_t>(c)], pr::Kind::text, value.value[0] == '\0' ? "false" : "true"});
      else
        row_fields.push_back({column_names[static_cast<std::size_t>(c)], pr::Kind::signed_integer, value.value});
    }
    std::string record;
    if (!pr::Encode(row_fields, &record)) return Refuse("SBSQL.NATIVE_RESULT.VECTOR_INVALID", "row_encode");
    row_records.push_back(std::move(record));
    row_metadata.push_back(std::move(row_meta));
  }

  if (at != fields.size()) return invalid("field_count");

  // ---- the legacy-shaped packet: operation_id, result_kind, row_count, then row[i] and row_meta[i] per row ----
  std::vector<pr::Field> out_fields;
  out_fields.push_back({"operation_id", pr::Kind::text, fields[0].value});
  out_fields.push_back({"result_kind", pr::Kind::text, "scalar_projection_rows"});
  out_fields.push_back({"row_count", pr::Kind::text, std::to_string(rows)});
  for (std::size_t r = 0; r < row_records.size(); ++r) {
    out_fields.push_back({"row[" + Index(r) + "]", pr::Kind::row, std::move(row_records[r])});
    out_fields.push_back({"row_meta[" + Index(r) + "]", pr::Kind::text, std::move(row_metadata[r])});
  }
  NativeVectorConversion out;
  if (!pr::Encode(out_fields, &out.payload)) return Refuse("SBSQL.NATIVE_RESULT.VECTOR_INVALID", "encode");
  out.status = NativeVectorConversionStatus::converted;
  return out;
}

NativeBoundaryParseStatus ParseNativeTransactionBoundary(std::string_view engine_payload, bool expect_commit,
                                                         NativeTransactionBoundary* out) {
  std::vector<pr::Field> fields;
  if (!engine_payload.starts_with(pr::kMagic) || !pr::Decode(engine_payload, &fields) || fields.size() < 2 ||
      fields[1].name != "result_kind" || fields[1].kind != pr::Kind::text || fields[1].value != "native_transaction_boundary")
    return NativeBoundaryParseStatus::not_native_boundary;
  if (fields.size() != 7 || fields[0].name != "operation_id" || fields[2].name != "ended_order" ||
      fields[2].kind != pr::Kind::unsigned_integer || fields[3].name != "ended_uuid" || fields[3].kind != pr::Kind::uuid ||
      fields[4].name != "finality" || fields[4].kind != pr::Kind::text || fields[5].name != "new_order" ||
      fields[5].kind != pr::Kind::unsigned_integer || fields[6].name != "new_uuid" || fields[6].kind != pr::Kind::uuid)
    return NativeBoundaryParseStatus::refused;
  const bool committed = fields[4].value == "committed";
  if ((!committed && fields[4].value != "rolled_back") || committed != expect_commit) return NativeBoundaryParseStatus::refused;
  NativeTransactionBoundary parsed;
  parsed.ended_order = Le64(fields[2].value);
  parsed.ended_uuid = fields[3].value;
  parsed.committed = committed;
  parsed.new_order = Le64(fields[5].value);
  parsed.new_uuid = fields[6].value;
  if (parsed.ended_order == 0 || parsed.new_order <= parsed.ended_order) return NativeBoundaryParseStatus::refused;
  if (out != nullptr) *out = std::move(parsed);
  return NativeBoundaryParseStatus::parsed;
}

}  // namespace scratchbird::parser::sbsql
