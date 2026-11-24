// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "wire/native_result_vector.hpp"

#include "datatype_catalog_manifest.hpp"
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
  if (fields.size() != 4 + 4 * columns + columns * rows) return invalid("field_count");
  if (column_names.size() != columns) return Refuse("SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "name_count");
  for (const auto& name : column_names)
    if (name.empty()) return Refuse("SBSQL.NATIVE_RESULT.COLUMN_NAME_MISSING", "empty_name");

  // ---- the type of each column, from its descriptor UUIDs through the compiled cohort identity table ----
  for (std::uint64_t c = 0; c < columns; ++c) {
    const auto base = 4 + 4 * c;
    const auto index = Index(static_cast<std::size_t>(c));
    const auto& descriptor = fields[base];
    const auto& datatype = fields[base + 1];
    const auto& datatype_descriptor = fields[base + 2];
    const auto& generation = fields[base + 3];
    if (descriptor.name != "column_descriptor[" + index + "]" || descriptor.kind != pr::Kind::uuid ||
        datatype.name != "column_datatype[" + index + "]" || datatype.kind != pr::Kind::uuid ||
        datatype_descriptor.name != "column_datatype_descriptor[" + index + "]" || datatype_descriptor.kind != pr::Kind::uuid ||
        generation.name != "column_datatype_generation[" + index + "]" || generation.kind != pr::Kind::unsigned_integer)
      return invalid("column_" + index);
    // The compiled table holds one row per (snapshot, generation) of the same identity; the vector names the identity (type, descriptor,
    // descriptor generation) and the receipt's snapshot already fixed which row the engine used. Every row that matches the identity must
    // be the int64 codec; an identity that matches rows of different codecs, or none, is refused (never a nearest match).
    bool matched = false;
    bool all_int64 = true;
    for (const auto& entry : dt::CurrentDatatypeTypeCodecIdentityRowsV3()) {
      const auto& row = entry.legacy_fields;
      if (std::string_view(reinterpret_cast<const char*>(row.type_uuid.bytes.data()), 16) != datatype.value ||
          std::string_view(reinterpret_cast<const char*>(row.descriptor_uuid.bytes.data()), 16) != datatype_descriptor.value ||
          row.descriptor_generation != Le64(generation.value))
        continue;
      matched = true;
      all_int64 = all_int64 && row.codec_id == "datatype.int64.le.v1";
    }
    if (!matched) return Refuse("SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "unknown_type");
    if (!all_int64) return Refuse("SBSQL.NATIVE_RESULT.TYPE_NOT_BUILT", "type_codec");
  }

  // ---- the rows: value[r.c], row-major, each a signed 64-bit carrier ----
  std::vector<std::string> row_records;
  std::vector<std::string> row_metadata;
  std::string metadata;
  for (std::uint64_t c = 0; c < columns; ++c) {
    if (c != 0) metadata += ';';
    metadata += column_names[static_cast<std::size_t>(c)] + ":int64:not_null";
  }
  const auto values_base = 4 + 4 * columns;
  for (std::uint64_t r = 0; r < rows; ++r) {
    std::vector<pr::Field> row_fields;
    for (std::uint64_t c = 0; c < columns; ++c) {
      const auto& value = fields[values_base + r * columns + c];
      if (value.name != "value[" + Index(static_cast<std::size_t>(r)) + "." + Index(static_cast<std::size_t>(c)) + "]" ||
          value.kind != pr::Kind::signed_integer || value.value.size() != 8)
        return Refuse("SBSQL.NATIVE_RESULT.VALUE_INVALID", "value_" + Index(static_cast<std::size_t>(r)) + "_" + Index(static_cast<std::size_t>(c)));
      row_fields.push_back({column_names[static_cast<std::size_t>(c)], pr::Kind::signed_integer, value.value});
    }
    std::string record;
    if (!pr::Encode(row_fields, &record)) return Refuse("SBSQL.NATIVE_RESULT.VECTOR_INVALID", "row_encode");
    row_records.push_back(std::move(record));
    row_metadata.push_back(metadata);
  }

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
