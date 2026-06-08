// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::engine::internal_api::EngineDescriptor;
using scratchbird::engine::internal_api::EngineTypedValue;

DescriptorRuntimeDiagnostic OkDiagnostic() {
  return {};
}

DescriptorRuntimeDiagnostic ErrorDiagnostic(std::string code,
                                            std::string detail = {},
                                            std::size_t row = 0,
                                            std::size_t column = 0) {
  DescriptorRuntimeDiagnostic diagnostic;
  diagnostic.ok = false;
  diagnostic.diagnostic_code = std::move(code);
  diagnostic.detail = std::move(detail);
  diagnostic.row_index = row;
  diagnostic.column_index = column;
  return diagnostic;
}

std::string LowerAscii(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

bool IsInt64Type(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return type == "int64" || type == "bigint" || type == "integer64" || type == "sb.int64";
}

bool IsBoolType(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return type == "bool" || type == "boolean" || type == "sb.boolean";
}

bool IsReal64Type(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return type == "real64" || type == "double" || type == "float8" || type == "sb.real64";
}

bool IsTextType(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return type == "text" || type == "varchar" || type == "string" || type == "sb.text" ||
         type == "timestamp" || type == "timestamp_tz" || type == "date" || type == "time";
}

bool IsBinaryType(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return type == "blob" || type == "binary" || type == "bytes";
}

bool IsUuidType(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return type == "uuid" || type == "uuidv7";
}

bool IsOpaqueEncodedType(const EngineDescriptor& descriptor) {
  const std::string type = LowerAscii(descriptor.canonical_type_name);
  return IsBinaryType(descriptor) || IsUuidType(descriptor) ||
         type == "vector" || type == "document" || type == "json" || type == "graph" || type == "search";
}

bool IsKnownScalarType(const EngineDescriptor& descriptor) {
  return IsInt64Type(descriptor) || IsBoolType(descriptor) || IsReal64Type(descriptor) || IsTextType(descriptor) ||
         IsOpaqueEncodedType(descriptor);
}

bool ParseInt64Strict(const std::string& text, std::int64_t* out) {
  if (out == nullptr || text.empty()) { return false; }
  try {
    std::size_t pos = 0;
    const auto parsed = std::stoll(text, &pos, 10);
    if (pos != text.size()) { return false; }
    *out = static_cast<std::int64_t>(parsed);
    return true;
  } catch (...) {
    return false;
  }
}

bool ParseReal64Strict(const std::string& text, double* out) {
  if (out == nullptr || text.empty()) { return false; }
  char* end = nullptr;
  const double parsed = std::strtod(text.c_str(), &end);
  if (end == nullptr || end == text.c_str() || *end != '\0' || !std::isfinite(parsed)) {
    return false;
  }
  *out = parsed;
  return true;
}

std::string FormatReal64(double value) {
  std::ostringstream out;
  out << std::setprecision(17) << value;
  return out.str();
}

bool ParseFixedWidthNumber(const std::string& text, std::size_t offset, std::size_t width, std::int64_t* out) {
  if (out == nullptr || offset + width > text.size()) { return false; }
  std::int64_t parsed = 0;
  for (std::size_t i = 0; i < width; ++i) {
    const char c = text[offset + i];
    if (c < '0' || c > '9') { return false; }
    parsed = (parsed * 10) + static_cast<std::int64_t>(c - '0');
  }
  *out = parsed;
  return true;
}

void SetDiagnostic(DescriptorRuntimeDiagnostic* out, DescriptorRuntimeDiagnostic diagnostic) {
  if (out != nullptr) { *out = std::move(diagnostic); }
}

std::vector<std::string> RowKey(const DescriptorTuple& tuple) {
  std::vector<std::string> key;
  key.reserve(tuple.values.size());
  for (const auto& value : tuple.values) {
    key.push_back(value.descriptor.canonical_type_name + ":" + (value.is_null ? "<NULL>" : value.encoded_value));
  }
  return key;
}

bool SameDescriptorShape(const DescriptorBatch& left, const DescriptorBatch& right) {
  if (left.columns.size() != right.columns.size()) { return false; }
  for (std::size_t i = 0; i < left.columns.size(); ++i) {
    if (left.columns[i].stable_name != right.columns[i].stable_name ||
        left.columns[i].nullable != right.columns[i].nullable ||
        !DescriptorMatches(left.columns[i].descriptor, right.columns[i].descriptor)) {
      return false;
    }
  }
  return true;
}

bool DescriptorFamiliesEqual(const EngineDescriptor& left, const EngineDescriptor& right) {
  return (IsInt64Type(left) && IsInt64Type(right)) ||
         (IsReal64Type(left) && IsReal64Type(right)) ||
         (IsTextType(left) && IsTextType(right)) ||
         (IsBoolType(left) && IsBoolType(right));
}

std::optional<std::string> EqualityKeyForValue(const EngineTypedValue& value,
                                               const EngineDescriptor& descriptor,
                                               std::size_t row,
                                               std::size_t column,
                                               DescriptorRuntimeDiagnostic* diagnostic) {
  if (value.is_null) { return std::nullopt; }
  if (IsInt64Type(descriptor)) {
    std::int64_t parsed = 0;
    if (!ParseInt64Strict(value.encoded_value, &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value, row, column));
      return std::nullopt;
    }
    return "i:" + std::to_string(parsed);
  }
  if (IsReal64Type(descriptor)) {
    double parsed = 0.0;
    if (!ParseReal64Strict(value.encoded_value, &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value, row, column));
      return std::nullopt;
    }
    return "r:" + FormatReal64(parsed);
  }
  if (IsBoolType(descriptor)) {
    const std::string text = LowerAscii(value.encoded_value);
    if (text == "true" || text == "1") return "b:1";
    if (text == "false" || text == "0") return "b:0";
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_BOOL_DECODE_FAILED", value.encoded_value, row, column));
    return std::nullopt;
  }
  if (IsTextType(descriptor)) { return "t:" + value.encoded_value; }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED",
                                            descriptor.canonical_type_name,
                                            row,
                                            column));
  return std::nullopt;
}

bool DescriptorValueGreaterThan(const EngineTypedValue& value,
                                const EngineTypedValue& bound,
                                const EngineDescriptor& descriptor,
                                std::size_t row,
                                std::size_t column,
                                DescriptorRuntimeDiagnostic* diagnostic,
                                bool* out) {
  if (out == nullptr) return false;
  *out = false;
  if (value.is_null || bound.is_null) return true;
  if (IsInt64Type(descriptor)) {
    std::int64_t lhs = 0;
    std::int64_t rhs = 0;
    if (!ParseInt64Strict(value.encoded_value, &lhs) ||
        !ParseInt64Strict(bound.encoded_value, &rhs)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value, row, column));
      return false;
    }
    *out = lhs > rhs;
    return true;
  }
  if (IsReal64Type(descriptor)) {
    double lhs = 0.0;
    double rhs = 0.0;
    if (!ParseReal64Strict(value.encoded_value, &lhs) ||
        !ParseReal64Strict(bound.encoded_value, &rhs)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value, row, column));
      return false;
    }
    *out = lhs > rhs;
    return true;
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED",
                                            descriptor.canonical_type_name,
                                            row,
                                            column));
  return false;
}

}  // namespace

EngineDescriptor MakeExecutorDescriptor(std::string canonical_type_name, std::string encoded_descriptor) {
  EngineDescriptor descriptor;
  descriptor.descriptor_kind = "executor.scalar";
  descriptor.canonical_type_name = std::move(canonical_type_name);
  descriptor.encoded_descriptor = encoded_descriptor.empty()
                                      ? "canonical_type=" + descriptor.canonical_type_name
                                      : std::move(encoded_descriptor);
  return descriptor;
}

EngineTypedValue MakeExecutorValue(const EngineDescriptor& descriptor,
                                   std::string encoded_value,
                                   bool is_null) {
  EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = std::move(encoded_value);
  value.is_null = is_null;
  return value;
}

DescriptorBatch MakeDescriptorBatch(std::vector<ExecutorColumnDescriptor> columns,
                                    std::vector<DescriptorTuple> rows) {
  DescriptorBatch batch;
  batch.columns = std::move(columns);
  batch.rows = std::move(rows);
  return batch;
}

std::string DescriptorFingerprint(const std::vector<ExecutorColumnDescriptor>& columns) {
  std::ostringstream out;
  for (std::size_t i = 0; i < columns.size(); ++i) {
    if (i != 0) { out << '|'; }
    out << columns[i].stable_name << ':'
        << columns[i].descriptor.descriptor_kind << ':'
        << columns[i].descriptor.canonical_type_name << ':'
        << columns[i].descriptor.encoded_descriptor << ':'
        << (columns[i].nullable ? 'N' : 'R');
  }
  return out.str();
}

bool DescriptorMatches(const EngineDescriptor& expected, const EngineDescriptor& actual) {
  if (!expected.descriptor_uuid.canonical.empty() || !actual.descriptor_uuid.canonical.empty()) {
    return expected.descriptor_uuid.canonical == actual.descriptor_uuid.canonical;
  }
  if (!expected.encoded_descriptor.empty() || !actual.encoded_descriptor.empty()) {
    return expected.encoded_descriptor == actual.encoded_descriptor;
  }
  return LowerAscii(expected.canonical_type_name) == LowerAscii(actual.canonical_type_name);
}

DescriptorRuntimeDiagnostic ValidateDescriptorBatch(const DescriptorBatch& batch) {
  if (batch.columns.empty()) {
    return ErrorDiagnostic("SB_EXECUTOR_DESCRIPTOR_REQUIRED", "column descriptor vector is empty");
  }
  for (std::size_t column = 0; column < batch.columns.size(); ++column) {
    const auto& descriptor = batch.columns[column].descriptor;
    if (descriptor.canonical_type_name.empty() || descriptor.descriptor_kind.empty()) {
      return ErrorDiagnostic("SB_EXECUTOR_DESCRIPTOR_INVALID", "descriptor kind and canonical type are required", 0, column);
    }
    if (!IsKnownScalarType(descriptor)) {
      return ErrorDiagnostic("SB_EXECUTOR_DESCRIPTOR_TYPE_UNSUPPORTED", descriptor.canonical_type_name, 0, column);
    }
  }
  for (std::size_t row = 0; row < batch.rows.size(); ++row) {
    if (batch.rows[row].values.size() != batch.columns.size()) {
      return ErrorDiagnostic("SB_EXECUTOR_ROW_WIDTH_MISMATCH", "row width does not match descriptor width", row, 0);
    }
    for (std::size_t column = 0; column < batch.columns.size(); ++column) {
      const auto& value = batch.rows[row].values[column];
      const auto& expected = batch.columns[column];
      if (value.is_null) {
        if (!expected.nullable) {
          return ErrorDiagnostic("SB_EXECUTOR_NULL_NOT_ALLOWED", expected.stable_name, row, column);
        }
        continue;
      }
      if (!DescriptorMatches(expected.descriptor, value.descriptor)) {
        return ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", expected.stable_name, row, column);
      }
      if (IsInt64Type(expected.descriptor)) {
        std::int64_t ignored = 0;
        if (!ParseInt64Strict(value.encoded_value, &ignored)) {
          return ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value, row, column);
        }
      } else if (IsBoolType(expected.descriptor)) {
        const std::string text = LowerAscii(value.encoded_value);
        if (text != "true" && text != "false" && text != "1" && text != "0") {
          return ErrorDiagnostic("SB_EXECUTOR_BOOL_DECODE_FAILED", value.encoded_value, row, column);
        }
      } else if (IsReal64Type(expected.descriptor)) {
        double ignored = 0.0;
        if (!ParseReal64Strict(value.encoded_value, &ignored)) {
          return ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value, row, column);
        }
      }
    }
  }
  return OkDiagnostic();
}

std::optional<std::size_t> FindColumnByStableName(const DescriptorBatch& batch, const std::string& stable_name) {
  for (std::size_t i = 0; i < batch.columns.size(); ++i) {
    if (batch.columns[i].stable_name == stable_name) { return i; }
  }
  return std::nullopt;
}

DescriptorBatch ProjectDescriptorBatch(const DescriptorBatch& input, const std::vector<std::size_t>& columns) {
  DescriptorBatch output;
  for (const auto column : columns) {
    if (column < input.columns.size()) { output.columns.push_back(input.columns[column]); }
  }
  output.rows.reserve(input.rows.size());
  for (const auto& row : input.rows) {
    DescriptorTuple projected;
    for (const auto column : columns) {
      if (column < row.values.size()) { projected.values.push_back(row.values[column]); }
    }
    output.rows.push_back(std::move(projected));
  }
  return output;
}

DescriptorBatch FilterDescriptorInt64GreaterThan(const DescriptorBatch& input,
                                                 std::size_t column,
                                                 std::int64_t threshold,
                                                 DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "filter column out of range", 0, column));
    return {};
  }
  if (!IsInt64Type(input.columns[column].descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED", input.columns[column].stable_name, 0, column));
    return {};
  }
  DescriptorBatch output;
  output.columns = input.columns;
  for (std::size_t row_index = 0; row_index < input.rows.size(); ++row_index) {
    const auto decoded = DecodeInt64Value(input.rows[row_index].values[column]);
    if (!decoded.ok()) {
      auto diag = decoded.diagnostic;
      diag.row_index = row_index;
      diag.column_index = column;
      SetDiagnostic(diagnostic, std::move(diag));
      return {};
    }
    if (decoded.value > threshold) { output.rows.push_back(input.rows[row_index]); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch FilterDescriptorBatchByComparison(
    const DescriptorBatch& input,
    std::size_t column,
    DescriptorComparisonOperator op,
    const EngineTypedValue& bound_value,
    DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "filter column out of range", 0, column));
    return {};
  }
  const auto& descriptor = input.columns[column].descriptor;
  if (IsOpaqueEncodedType(descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED", input.columns[column].stable_name, 0, column));
    return {};
  }
  if (!bound_value.is_null && !DescriptorFamiliesEqual(descriptor, bound_value.descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", input.columns[column].stable_name, 0, column));
    return {};
  }

  std::optional<std::string> bound_key;
  if (op == DescriptorComparisonOperator::kEqual) {
    bound_key = EqualityKeyForValue(bound_value, descriptor, 0, column, diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
  } else if (!IsInt64Type(descriptor) && !IsReal64Type(descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_FILTER_TYPE_UNSUPPORTED", input.columns[column].stable_name, 0, column));
    return {};
  }

  DescriptorBatch output;
  output.columns = input.columns;
  for (std::size_t row_index = 0; row_index < input.rows.size(); ++row_index) {
    bool matches = false;
    if (op == DescriptorComparisonOperator::kEqual) {
      const auto key = EqualityKeyForValue(input.rows[row_index].values[column],
                                           descriptor,
                                           row_index,
                                           column,
                                           diagnostic);
      if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
      matches = key && bound_key && *key == *bound_key;
    } else {
      if (!DescriptorValueGreaterThan(input.rows[row_index].values[column],
                                      bound_value,
                                      descriptor,
                                      row_index,
                                      column,
                                      diagnostic,
                                      &matches)) {
        return {};
      }
    }
    if (matches) { output.rows.push_back(input.rows[row_index]); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch SortDescriptorBatchByColumn(const DescriptorBatch& input,
                                            std::size_t column,
                                            bool ascending,
                                            DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "sort column out of range", 0, column));
    return {};
  }
  DescriptorBatch output = input;
  if (IsInt64Type(input.columns[column].descriptor)) {
    std::stable_sort(output.rows.begin(), output.rows.end(), [&](const auto& lhs, const auto& rhs) {
      const auto l = DecodeInt64Value(lhs.values[column]);
      const auto r = DecodeInt64Value(rhs.values[column]);
      return ascending ? l.value < r.value : l.value > r.value;
    });
  } else {
    std::stable_sort(output.rows.begin(), output.rows.end(), [&](const auto& lhs, const auto& rhs) {
      const std::string& l = lhs.values[column].encoded_value;
      const std::string& r = rhs.values[column].encoded_value;
      return ascending ? l < r : l > r;
    });
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch LimitOffsetDescriptorBatch(const DescriptorBatch& input,
                                           std::size_t limit,
                                           std::size_t offset) {
  DescriptorBatch output;
  output.columns = input.columns;
  if (offset >= input.rows.size()) { return output; }
  const auto end = std::min(input.rows.size(), offset + limit);
  for (std::size_t i = offset; i < end; ++i) {
    output.rows.push_back(input.rows[i]);
  }
  return output;
}

DescriptorBatch SetUnionDistinctDescriptorBatch(const DescriptorBatch& left,
                                                const DescriptorBatch& right,
                                                DescriptorRuntimeDiagnostic* diagnostic) {
  if (!SameDescriptorShape(left, right)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SETOP_DESCRIPTOR_MISMATCH"));
    return {};
  }
  DescriptorBatch output;
  output.columns = left.columns;
  std::set<std::vector<std::string>> seen;
  for (const auto& row : left.rows) {
    if (seen.insert(RowKey(row)).second) { output.rows.push_back(row); }
  }
  for (const auto& row : right.rows) {
    if (seen.insert(RowKey(row)).second) { output.rows.push_back(row); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch SetIntersectDistinctDescriptorBatch(const DescriptorBatch& left,
                                                    const DescriptorBatch& right,
                                                    DescriptorRuntimeDiagnostic* diagnostic) {
  if (!SameDescriptorShape(left, right)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SETOP_DESCRIPTOR_MISMATCH"));
    return {};
  }
  DescriptorBatch output;
  output.columns = left.columns;
  std::set<std::vector<std::string>> right_keys;
  for (const auto& row : right.rows) { right_keys.insert(RowKey(row)); }
  std::set<std::vector<std::string>> emitted;
  for (const auto& row : left.rows) {
    const auto key = RowKey(row);
    if (right_keys.count(key) != 0 && emitted.insert(key).second) { output.rows.push_back(row); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch SetExceptDistinctDescriptorBatch(const DescriptorBatch& left,
                                                 const DescriptorBatch& right,
                                                 DescriptorRuntimeDiagnostic* diagnostic) {
  if (!SameDescriptorShape(left, right)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SETOP_DESCRIPTOR_MISMATCH"));
    return {};
  }
  DescriptorBatch output;
  output.columns = left.columns;
  std::set<std::vector<std::string>> right_keys;
  for (const auto& row : right.rows) { right_keys.insert(RowKey(row)); }
  std::set<std::vector<std::string>> emitted;
  for (const auto& row : left.rows) {
    const auto key = RowKey(row);
    if (right_keys.count(key) == 0 && emitted.insert(key).second) { output.rows.push_back(row); }
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch JoinDescriptorBatchesOnInt64(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_valid = ValidateDescriptorBatch(left);
  if (!left_valid.ok) {
    SetDiagnostic(diagnostic, left_valid);
    return {};
  }
  const auto right_valid = ValidateDescriptorBatch(right);
  if (!right_valid.ok) {
    SetDiagnostic(diagnostic, right_valid);
    return {};
  }
  if (left_column >= left.columns.size() || right_column >= right.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "join column out of range"));
    return {};
  }
  if (!IsInt64Type(left.columns[left_column].descriptor) || !IsInt64Type(right.columns[right_column].descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_JOIN_TYPE_UNSUPPORTED", "join keys must be int64"));
    return {};
  }

  DescriptorBatch output;
  output.columns.reserve(left.columns.size() + right.columns.size());
  output.columns.insert(output.columns.end(), left.columns.begin(), left.columns.end());
  output.columns.insert(output.columns.end(), right.columns.begin(), right.columns.end());

  for (std::size_t left_row = 0; left_row < left.rows.size(); ++left_row) {
    if (left.rows[left_row].values[left_column].is_null) { continue; }
    const auto left_key = DecodeInt64Value(left.rows[left_row].values[left_column]);
    if (!left_key.ok()) {
      auto diag = left_key.diagnostic;
      diag.row_index = left_row;
      diag.column_index = left_column;
      SetDiagnostic(diagnostic, std::move(diag));
      return {};
    }
    for (std::size_t right_row = 0; right_row < right.rows.size(); ++right_row) {
      if (right.rows[right_row].values[right_column].is_null) { continue; }
      const auto right_key = DecodeInt64Value(right.rows[right_row].values[right_column]);
      if (!right_key.ok()) {
        auto diag = right_key.diagnostic;
        diag.row_index = right_row;
        diag.column_index = right_column;
        SetDiagnostic(diagnostic, std::move(diag));
        return {};
      }
      if (left_key.value != right_key.value) { continue; }
      DescriptorTuple joined;
      joined.values.reserve(left.rows[left_row].values.size() + right.rows[right_row].values.size());
      joined.values.insert(joined.values.end(), left.rows[left_row].values.begin(), left.rows[left_row].values.end());
      joined.values.insert(joined.values.end(), right.rows[right_row].values.begin(), right.rows[right_row].values.end());
      output.rows.push_back(std::move(joined));
    }
  }

  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch JoinDescriptorBatchesOnEqual(const DescriptorBatch& left,
                                             const DescriptorBatch& right,
                                             std::size_t left_column,
                                             std::size_t right_column,
                                             DescriptorRuntimeDiagnostic* diagnostic) {
  const auto left_valid = ValidateDescriptorBatch(left);
  if (!left_valid.ok) {
    SetDiagnostic(diagnostic, left_valid);
    return {};
  }
  const auto right_valid = ValidateDescriptorBatch(right);
  if (!right_valid.ok) {
    SetDiagnostic(diagnostic, right_valid);
    return {};
  }
  if (left_column >= left.columns.size() || right_column >= right.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "join column out of range"));
    return {};
  }
  const auto& left_descriptor = left.columns[left_column].descriptor;
  const auto& right_descriptor = right.columns[right_column].descriptor;
  if (IsOpaqueEncodedType(left_descriptor) || IsOpaqueEncodedType(right_descriptor) ||
      !DescriptorFamiliesEqual(left_descriptor, right_descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_JOIN_TYPE_UNSUPPORTED", "join keys must be comparable core scalar descriptors"));
    return {};
  }

  DescriptorBatch output;
  output.columns.reserve(left.columns.size() + right.columns.size());
  output.columns.insert(output.columns.end(), left.columns.begin(), left.columns.end());
  output.columns.insert(output.columns.end(), right.columns.begin(), right.columns.end());

  std::multimap<std::string, const DescriptorTuple*> right_index;
  for (std::size_t right_row = 0; right_row < right.rows.size(); ++right_row) {
    const auto key = EqualityKeyForValue(right.rows[right_row].values[right_column],
                                         right_descriptor,
                                         right_row,
                                         right_column,
                                         diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
    if (key) { right_index.emplace(*key, &right.rows[right_row]); }
  }
  for (std::size_t left_row = 0; left_row < left.rows.size(); ++left_row) {
    const auto key = EqualityKeyForValue(left.rows[left_row].values[left_column],
                                         left_descriptor,
                                         left_row,
                                         left_column,
                                         diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
    if (!key) { continue; }
    const auto range = right_index.equal_range(*key);
    for (auto it = range.first; it != range.second; ++it) {
      DescriptorTuple joined;
      joined.values.reserve(left.rows[left_row].values.size() + it->second->values.size());
      joined.values.insert(joined.values.end(), left.rows[left_row].values.begin(), left.rows[left_row].values.end());
      joined.values.insert(joined.values.end(), it->second->values.begin(), it->second->values.end());
      output.rows.push_back(std::move(joined));
    }
  }

  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch AggregateDescriptorCountByInt64(const DescriptorBatch& input,
                                                std::size_t group_column,
                                                std::string count_stable_name,
                                                DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (group_column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "aggregate group column out of range", 0, group_column));
    return {};
  }
  if (!IsInt64Type(input.columns[group_column].descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_AGGREGATE_TYPE_UNSUPPORTED", input.columns[group_column].stable_name, 0, group_column));
    return {};
  }

  std::map<std::int64_t, std::int64_t> counts;
  for (std::size_t row = 0; row < input.rows.size(); ++row) {
    if (input.rows[row].values[group_column].is_null) { continue; }
    const auto decoded = DecodeInt64Value(input.rows[row].values[group_column]);
    if (!decoded.ok()) {
      auto diag = decoded.diagnostic;
      diag.row_index = row;
      diag.column_index = group_column;
      SetDiagnostic(diagnostic, std::move(diag));
      return {};
    }
    ++counts[decoded.value];
  }

  DescriptorBatch output;
  output.columns = {input.columns[group_column], {std::move(count_stable_name), MakeExecutorDescriptor("int64"), false}};
  output.rows.reserve(counts.size());
  for (const auto& [group_value, count] : counts) {
    output.rows.push_back({{EncodeInt64Value(group_value), EncodeInt64Value(count)}});
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch AggregateDescriptorCountByKey(const DescriptorBatch& input,
                                              std::size_t group_column,
                                              std::string count_stable_name,
                                              DescriptorRuntimeDiagnostic* diagnostic) {
  const auto valid = ValidateDescriptorBatch(input);
  if (!valid.ok) {
    SetDiagnostic(diagnostic, valid);
    return {};
  }
  if (group_column >= input.columns.size()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_COLUMN_OUT_OF_RANGE", "aggregate group column out of range", 0, group_column));
    return {};
  }
  const auto& descriptor = input.columns[group_column].descriptor;
  if (IsOpaqueEncodedType(descriptor) ||
      (!IsInt64Type(descriptor) && !IsTextType(descriptor) &&
       !IsReal64Type(descriptor) && !IsBoolType(descriptor))) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_AGGREGATE_TYPE_UNSUPPORTED", input.columns[group_column].stable_name, 0, group_column));
    return {};
  }

  struct CountState {
    EngineTypedValue representative;
    std::int64_t count = 0;
  };
  std::map<std::string, CountState> counts;
  for (std::size_t row = 0; row < input.rows.size(); ++row) {
    const auto key = EqualityKeyForValue(input.rows[row].values[group_column],
                                         descriptor,
                                         row,
                                         group_column,
                                         diagnostic);
    if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
    if (!key) { continue; }
    auto& state = counts[*key];
    if (state.count == 0) { state.representative = input.rows[row].values[group_column]; }
    ++state.count;
  }

  DescriptorBatch output;
  output.columns = {input.columns[group_column], {std::move(count_stable_name), MakeExecutorDescriptor("int64"), false}};
  output.rows.reserve(counts.size());
  for (const auto& [key, state] : counts) {
    (void)key;
    output.rows.push_back({{state.representative, EncodeInt64Value(state.count)}});
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return output;
}

DescriptorBatch WindowDescriptorRowNumberByInt64(const DescriptorBatch& input,
                                                 std::size_t order_column,
                                                 std::string row_number_stable_name,
                                                 bool ascending,
                                                 DescriptorRuntimeDiagnostic* diagnostic) {
  auto sorted = SortDescriptorBatchByColumn(input, order_column, ascending, diagnostic);
  if (diagnostic != nullptr && !diagnostic->ok) { return {}; }
  sorted.columns.push_back({std::move(row_number_stable_name), MakeExecutorDescriptor("int64"), false});
  for (std::size_t row = 0; row < sorted.rows.size(); ++row) {
    sorted.rows[row].values.push_back(EncodeInt64Value(static_cast<std::int64_t>(row + 1)));
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return sorted;
}

EngineTypedValue EvaluateDescriptorExpression(DescriptorExpressionOperator op,
                                              const EngineTypedValue& left,
                                              const EngineTypedValue& right,
                                              DescriptorRuntimeDiagnostic* diagnostic) {
  switch (op) {
    case DescriptorExpressionOperator::kInt64Add:
    case DescriptorExpressionOperator::kInt64Subtract:
    case DescriptorExpressionOperator::kInt64Multiply:
    case DescriptorExpressionOperator::kInt64Divide:
    case DescriptorExpressionOperator::kInt64Equal:
    case DescriptorExpressionOperator::kInt64GreaterThan: {
      const auto l = DecodeInt64Value(left);
      if (!l.ok()) {
        SetDiagnostic(diagnostic, l.diagnostic);
        return {};
      }
      const auto r = DecodeInt64Value(right);
      if (!r.ok()) {
        SetDiagnostic(diagnostic, r.diagnostic);
        return {};
      }
      switch (op) {
        case DescriptorExpressionOperator::kInt64Add:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(l.value + r.value);
        case DescriptorExpressionOperator::kInt64Subtract:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(l.value - r.value);
        case DescriptorExpressionOperator::kInt64Multiply:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(l.value * r.value);
        case DescriptorExpressionOperator::kInt64Divide:
          if (r.value == 0) {
            SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DIVIDE_BY_ZERO", "int64 divide by zero"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeInt64Value(l.value / r.value);
        case DescriptorExpressionOperator::kInt64Equal:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value == r.value);
        case DescriptorExpressionOperator::kInt64GreaterThan:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value > r.value);
        default:
          break;
      }
      break;
    }
    case DescriptorExpressionOperator::kBoolAnd:
    case DescriptorExpressionOperator::kBoolOr: {
      const auto l = DecodeBoolValue(left);
      if (!l.ok()) {
        SetDiagnostic(diagnostic, l.diagnostic);
        return {};
      }
      const auto r = DecodeBoolValue(right);
      if (!r.ok()) {
        SetDiagnostic(diagnostic, r.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return EncodeBoolValue(op == DescriptorExpressionOperator::kBoolAnd ? (l.value && r.value) : (l.value || r.value));
    }
    case DescriptorExpressionOperator::kReal64Add:
    case DescriptorExpressionOperator::kReal64Subtract:
    case DescriptorExpressionOperator::kReal64Multiply:
    case DescriptorExpressionOperator::kReal64Divide:
    case DescriptorExpressionOperator::kReal64Equal:
    case DescriptorExpressionOperator::kReal64GreaterThan: {
      const auto l = DecodeReal64Value(left);
      if (!l.ok()) {
        SetDiagnostic(diagnostic, l.diagnostic);
        return {};
      }
      const auto r = DecodeReal64Value(right);
      if (!r.ok()) {
        SetDiagnostic(diagnostic, r.diagnostic);
        return {};
      }
      switch (op) {
        case DescriptorExpressionOperator::kReal64Add:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeReal64Value(l.value + r.value);
        case DescriptorExpressionOperator::kReal64Subtract:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeReal64Value(l.value - r.value);
        case DescriptorExpressionOperator::kReal64Multiply:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeReal64Value(l.value * r.value);
        case DescriptorExpressionOperator::kReal64Divide:
          if (r.value == 0.0) {
            SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DIVIDE_BY_ZERO", "real64 divide by zero"));
            return {};
          }
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeReal64Value(l.value / r.value);
        case DescriptorExpressionOperator::kReal64Equal:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value == r.value);
        case DescriptorExpressionOperator::kReal64GreaterThan:
          SetDiagnostic(diagnostic, OkDiagnostic());
          return EncodeBoolValue(l.value > r.value);
        default:
          break;
      }
      break;
    }
    case DescriptorExpressionOperator::kTextConcat:
      if (left.is_null || right.is_null) {
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("text"), {}, true);
      }
      if (!IsTextType(left.descriptor) || !IsTextType(right.descriptor)) {
        SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH",
                                                  left.descriptor.canonical_type_name + "," + right.descriptor.canonical_type_name));
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return EncodeTextValue(left.encoded_value + right.encoded_value);
    case DescriptorExpressionOperator::kTextEqual:
      if (left.is_null || right.is_null) {
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("boolean"), {}, true);
      }
      if (!IsTextType(left.descriptor) || !IsTextType(right.descriptor)) {
        SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH",
                                                  left.descriptor.canonical_type_name + "," + right.descriptor.canonical_type_name));
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return EncodeBoolValue(left.encoded_value == right.encoded_value);
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXPRESSION_UNSUPPORTED", "descriptor expression operator unsupported"));
  return {};
}

EngineTypedValue EvaluateDescriptorCoalesce(const std::vector<EngineTypedValue>& values,
                                            DescriptorRuntimeDiagnostic* diagnostic) {
  if (values.empty()) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_SPECIAL_FORM_ARGUMENT_REQUIRED", "coalesce requires at least one argument"));
    return {};
  }
  for (const auto& value : values) {
    if (!value.is_null) {
      SetDiagnostic(diagnostic, OkDiagnostic());
      return value;
    }
  }
  EngineTypedValue null_value = values.front();
  null_value.is_null = true;
  null_value.encoded_value.clear();
  SetDiagnostic(diagnostic, OkDiagnostic());
  return null_value;
}

EngineTypedValue CastDescriptorValue(const EngineTypedValue& value,
                                     const EngineDescriptor& target_descriptor,
                                     DescriptorRuntimeDiagnostic* diagnostic) {
  if (value.is_null) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, {}, true);
  }
  if (!IsKnownScalarType(target_descriptor)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_TARGET_UNSUPPORTED", target_descriptor.canonical_type_name));
    return {};
  }
  if (DescriptorMatches(target_descriptor, value.descriptor)) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsInt64Type(target_descriptor) && IsInt64Type(value.descriptor)) {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, std::to_string(decoded.value), false);
  }
  if (IsBoolType(target_descriptor) && IsBoolType(value.descriptor)) {
    const auto decoded = DecodeBoolValue(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, decoded.value ? "true" : "false", false);
  }
  if (IsReal64Type(target_descriptor) && IsReal64Type(value.descriptor)) {
    const auto decoded = DecodeReal64Value(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, FormatReal64(decoded.value), false);
  }
  if (IsTextType(target_descriptor)) {
    if (IsInt64Type(value.descriptor)) {
      const auto decoded = DecodeInt64Value(value);
      if (!decoded.ok()) {
        SetDiagnostic(diagnostic, decoded.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, std::to_string(decoded.value), false);
    }
    if (IsBoolType(value.descriptor)) {
      const auto decoded = DecodeBoolValue(value);
      if (!decoded.ok()) {
        SetDiagnostic(diagnostic, decoded.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, decoded.value ? "true" : "false", false);
    }
    if (IsReal64Type(value.descriptor)) {
      const auto decoded = DecodeReal64Value(value);
      if (!decoded.ok()) {
        SetDiagnostic(diagnostic, decoded.diagnostic);
        return {};
      }
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, FormatReal64(decoded.value), false);
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsOpaqueEncodedType(target_descriptor) && IsTextType(value.descriptor)) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsTextType(target_descriptor) && IsOpaqueEncodedType(value.descriptor)) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, value.encoded_value, false);
  }
  if (IsInt64Type(target_descriptor) && IsTextType(value.descriptor)) {
    std::int64_t parsed = 0;
    if (!ParseInt64Strict(value.encoded_value, &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED", value.encoded_value));
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, std::to_string(parsed), false);
  }
  if (IsReal64Type(target_descriptor) && IsInt64Type(value.descriptor)) {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) {
      SetDiagnostic(diagnostic, decoded.diagnostic);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, FormatReal64(static_cast<double>(decoded.value)), false);
  }
  if (IsReal64Type(target_descriptor) && IsTextType(value.descriptor)) {
    double parsed = 0.0;
    if (!ParseReal64Strict(value.encoded_value, &parsed)) {
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED", value.encoded_value));
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(target_descriptor, FormatReal64(parsed), false);
  }
  if (IsBoolType(target_descriptor) && IsTextType(value.descriptor)) {
    const std::string text = LowerAscii(value.encoded_value);
    if (text == "true" || text == "1") {
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, "true", false);
    }
    if (text == "false" || text == "0") {
      SetDiagnostic(diagnostic, OkDiagnostic());
      return MakeExecutorValue(target_descriptor, "false", false);
    }
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_FAILED", value.encoded_value));
    return {};
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_CAST_UNSUPPORTED", value.descriptor.canonical_type_name + "->" + target_descriptor.canonical_type_name));
  return {};
}

EngineTypedValue ExtractDescriptorField(const EngineTypedValue& value,
                                        const std::string& field_name,
                                        DescriptorRuntimeDiagnostic* diagnostic) {
  if (value.is_null) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(MakeExecutorDescriptor("int64"), {}, true);
  }
  if (!IsTextType(value.descriptor)) {
    if (IsBinaryType(value.descriptor)) {
      const std::string field = LowerAscii(field_name);
      if (field == "octet_length" || field == "length") {
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("uint64"), std::to_string(value.encoded_value.size()), false);
      }
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FIELD_UNSUPPORTED", field_name));
      return {};
    }
    if (IsUuidType(value.descriptor)) {
      const std::string field = LowerAscii(field_name);
      if (field == "version" && value.encoded_value.size() == 36) {
        SetDiagnostic(diagnostic, OkDiagnostic());
        return MakeExecutorValue(MakeExecutorDescriptor("uint8"), std::string(1, value.encoded_value[14]), false);
      }
      SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FIELD_UNSUPPORTED", field_name));
      return {};
    }
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_TYPE_UNSUPPORTED", value.descriptor.canonical_type_name));
    return {};
  }
  const std::string field = LowerAscii(field_name);
  if (field == "character_length" || field == "length" || field == "octet_length") {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(MakeExecutorDescriptor("uint64"), std::to_string(value.encoded_value.size()), false);
  }
  std::size_t offset = 0;
  std::size_t width = 0;
  if (field == "year") {
    offset = 0;
    width = 4;
  } else if (field == "month") {
    offset = 5;
    width = 2;
  } else if (field == "day") {
    offset = 8;
    width = 2;
  } else if (field == "hour") {
    offset = 11;
    width = 2;
  } else if (field == "minute") {
    offset = 14;
    width = 2;
  } else if (field == "second") {
    offset = 17;
    width = 2;
  } else {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FIELD_UNSUPPORTED", field_name));
    return {};
  }
  std::int64_t parsed = 0;
  if (!ParseFixedWidthNumber(value.encoded_value, offset, width, &parsed)) {
    SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_EXTRACT_FAILED", value.encoded_value));
    return {};
  }
  SetDiagnostic(diagnostic, OkDiagnostic());
  return EncodeInt64Value(parsed);
}

void SetDescriptorRuntimeVariable(DescriptorRuntimeSetScope* scope,
                                  std::string stable_name,
                                  EngineTypedValue value) {
  if (scope == nullptr) { return; }
  for (auto& variable : scope->variables) {
    if (variable.stable_name == stable_name) {
      variable.value = std::move(value);
      return;
    }
  }
  scope->variables.push_back({std::move(stable_name), std::move(value)});
}

std::optional<EngineTypedValue> GetDescriptorRuntimeVariable(const DescriptorRuntimeSetScope& scope,
                                                             const std::string& stable_name) {
  for (const auto& variable : scope.variables) {
    if (variable.stable_name == stable_name) { return variable.value; }
  }
  return std::nullopt;
}

DescriptorRuntimeDiagnostic ValidateDescriptorDomainValue(const DescriptorDomainPolicy& policy,
                                                          const EngineTypedValue& value) {
  if (policy.domain_stable_name.empty()) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_POLICY_INVALID", "domain stable name is required");
  }
  if (policy.base_descriptor.canonical_type_name.empty() || policy.base_descriptor.descriptor_kind.empty()) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_POLICY_INVALID", "domain base descriptor is required");
  }
  if (value.is_null) {
    return policy.nullable ? OkDiagnostic()
                           : ErrorDiagnostic("SB_EXECUTOR_DOMAIN_NULL_NOT_ALLOWED", policy.domain_stable_name);
  }
  if (!DescriptorMatches(policy.base_descriptor, value.descriptor)) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_DESCRIPTOR_MISMATCH", policy.domain_stable_name);
  }
  if (IsInt64Type(policy.base_descriptor)) {
    const auto decoded = DecodeInt64Value(value);
    if (!decoded.ok()) { return decoded.diagnostic; }
    if (policy.min_int64.has_value() && decoded.value < *policy.min_int64) {
      return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_MIN_VIOLATION", policy.domain_stable_name);
    }
    if (policy.max_int64.has_value() && decoded.value > *policy.max_int64) {
      return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_MAX_VIOLATION", policy.domain_stable_name);
    }
  }
  if (IsTextType(policy.base_descriptor) && policy.max_text_bytes.has_value() &&
      value.encoded_value.size() > *policy.max_text_bytes) {
    return ErrorDiagnostic("SB_EXECUTOR_DOMAIN_TEXT_LENGTH_VIOLATION", policy.domain_stable_name);
  }
  return OkDiagnostic();
}

EngineTypedValue ApplyDescriptorDomainMask(const DescriptorDomainPolicy& policy,
                                           const EngineTypedValue& value,
                                           const std::string& security_token,
                                           DescriptorRuntimeDiagnostic* diagnostic) {
  const auto validation = ValidateDescriptorDomainValue(policy, value);
  if (!validation.ok) {
    SetDiagnostic(diagnostic, validation);
    return {};
  }
  if (policy.required_security_token.empty() || policy.required_security_token == security_token ||
      policy.mask_kind == DescriptorDomainMaskKind::kNone) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return value;
  }
  if (policy.mask_kind == DescriptorDomainMaskKind::kNull) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(policy.base_descriptor, {}, true);
  }
  if (policy.mask_kind == DescriptorDomainMaskKind::kFixedText) {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(policy.base_descriptor, policy.fixed_mask_text, false);
  }
  if (policy.mask_kind == DescriptorDomainMaskKind::kRevealLast4) {
    std::string masked = value.encoded_value;
    if (masked.size() <= 4) {
      masked.assign(masked.size(), '*');
    } else {
      masked.replace(0, masked.size() - 4, masked.size() - 4, '*');
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return MakeExecutorValue(policy.base_descriptor, std::move(masked), false);
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DOMAIN_MASK_UNSUPPORTED", policy.domain_stable_name));
  return {};
}

EngineTypedValue EvaluateDescriptorDomainMethod(const DescriptorDomainPolicy& policy,
                                                const std::string& method_name,
                                                const EngineTypedValue& value,
                                                const std::string& security_token,
                                                DescriptorRuntimeDiagnostic* diagnostic) {
  const std::string method = LowerAscii(method_name);
  if (method == "validate") {
    const auto validation = ValidateDescriptorDomainValue(policy, value);
    SetDiagnostic(diagnostic, OkDiagnostic());
    return EncodeBoolValue(validation.ok);
  }
  if (method == "mask") {
    return ApplyDescriptorDomainMask(policy, value, security_token, diagnostic);
  }
  if (method == "is_visible") {
    const auto validation = ValidateDescriptorDomainValue(policy, value);
    if (!validation.ok) {
      SetDiagnostic(diagnostic, validation);
      return {};
    }
    SetDiagnostic(diagnostic, OkDiagnostic());
    return EncodeBoolValue(policy.required_security_token.empty() || policy.required_security_token == security_token);
  }
  if (method == "base_type_name") {
    SetDiagnostic(diagnostic, OkDiagnostic());
    return EncodeTextValue(policy.base_descriptor.canonical_type_name);
  }
  SetDiagnostic(diagnostic, ErrorDiagnostic("SB_EXECUTOR_DOMAIN_METHOD_UNKNOWN", method_name));
  return {};
}

Int64DecodeResult DecodeInt64Value(const EngineTypedValue& value) {
  Int64DecodeResult result;
  if (value.is_null) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_NULL_VALUE", "int64 decode received NULL");
    return result;
  }
  if (!IsInt64Type(value.descriptor)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", value.descriptor.canonical_type_name);
    return result;
  }
  if (!ParseInt64Strict(value.encoded_value, &result.value)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_INT64_DECODE_FAILED", value.encoded_value);
    return result;
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

BoolDecodeResult DecodeBoolValue(const EngineTypedValue& value) {
  BoolDecodeResult result;
  if (value.is_null) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_NULL_VALUE", "bool decode received NULL");
    return result;
  }
  if (!IsBoolType(value.descriptor)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", value.descriptor.canonical_type_name);
    return result;
  }
  const std::string text = LowerAscii(value.encoded_value);
  if (text == "true" || text == "1") {
    result.value = true;
  } else if (text == "false" || text == "0") {
    result.value = false;
  } else {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_BOOL_DECODE_FAILED", value.encoded_value);
    return result;
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

Real64DecodeResult DecodeReal64Value(const EngineTypedValue& value) {
  Real64DecodeResult result;
  if (value.is_null) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_NULL_VALUE", "real64 decode received NULL");
    return result;
  }
  if (!IsReal64Type(value.descriptor)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_VALUE_DESCRIPTOR_MISMATCH", value.descriptor.canonical_type_name);
    return result;
  }
  if (!ParseReal64Strict(value.encoded_value, &result.value)) {
    result.diagnostic = ErrorDiagnostic("SB_EXECUTOR_REAL64_DECODE_FAILED", value.encoded_value);
    return result;
  }
  result.diagnostic = OkDiagnostic();
  return result;
}

EngineTypedValue EncodeInt64Value(std::int64_t value) {
  return MakeExecutorValue(MakeExecutorDescriptor("int64"), std::to_string(value), false);
}

EngineTypedValue EncodeBoolValue(bool value) {
  return MakeExecutorValue(MakeExecutorDescriptor("boolean"), value ? "true" : "false", false);
}

EngineTypedValue EncodeReal64Value(double value) {
  return MakeExecutorValue(MakeExecutorDescriptor("real64"), FormatReal64(value), false);
}

EngineTypedValue EncodeTextValue(std::string value) {
  return MakeExecutorValue(MakeExecutorDescriptor("text"), std::move(value), false);
}

}  // namespace scratchbird::engine::executor
