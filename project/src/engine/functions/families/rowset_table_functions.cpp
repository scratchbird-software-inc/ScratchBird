// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/rowset_table_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

constexpr std::size_t kDescriptorBytesLimit = 1U << 20;
constexpr std::size_t kGeneratedRowsLimit = 10000;

std::string Trim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
}

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

bool IdIs(const std::string& id, std::initializer_list<std::string_view> candidates) {
  for (const auto candidate : candidates) {
    if (id == candidate) return true;
  }
  return false;
}

std::string JsonEscape(std::string value) {
  std::string out = "\"";
  for (const char ch : value) {
    switch (ch) {
      case '\\':
      case '"':
        out.push_back('\\');
        out.push_back(ch);
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  out.push_back('"');
  return out;
}

bool LooksJsonScalar(const std::string& value) {
  const std::string text = Trim(value);
  if (text.empty()) return false;
  if ((text.front() == '[' && text.back() == ']') ||
      (text.front() == '{' && text.back() == '}') ||
      (text.front() == '"' && text.back() == '"')) {
    return true;
  }
  if (text == "null" || text == "true" || text == "false") return true;
  if (std::isdigit(static_cast<unsigned char>(text.front())) || text.front() == '-') return true;
  return false;
}

std::string JsonScalarFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (IsSqlNull(value)) return "null";
  if (value.has_int64_value) return std::to_string(value.int64_value);
  if (!value.encoded_value.empty() &&
      (value.descriptor_id == "array" || value.descriptor_id == "json_document" ||
       value.descriptor_id == "rowset" || value.descriptor_id == "table_value") &&
      LooksJsonScalar(value.encoded_value)) {
    return Trim(value.encoded_value);
  }
  return JsonEscape(ValueAsText(value));
}

std::optional<std::string> TopLevelArrayValue(const std::string& document,
                                              std::size_t open_bracket) {
  if (open_bracket >= document.size() || document[open_bracket] != '[') return std::nullopt;
  bool in_string = false;
  int depth = 0;
  for (std::size_t i = open_bracket; i < document.size(); ++i) {
    const char ch = document[i];
    if (in_string) {
      if (ch == '\\') {
        ++i;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '[') {
      ++depth;
      continue;
    }
    if (ch == ']') {
      --depth;
      if (depth == 0) return document.substr(open_bracket, i - open_bracket + 1);
    }
  }
  return std::nullopt;
}

std::optional<std::string> JsonArrayForKey(const std::string& document,
                                           std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = document.find(needle);
  if (key_pos == std::string::npos) return std::nullopt;
  const auto colon = document.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  return TopLevelArrayValue(document, cursor);
}

bool SplitTopLevelJsonArray(const std::string& document, std::vector<std::string>* elements) {
  const std::string text = Trim(document);
  if (text.size() < 2 || text.front() != '[' || text.back() != ']') return false;
  bool in_string = false;
  int depth = 0;
  std::size_t element_start = 1;
  for (std::size_t i = 1; i < text.size() - 1; ++i) {
    const char ch = text[i];
    if (in_string) {
      if (ch == '\\') {
        ++i;
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      continue;
    }
    if (ch == '[' || ch == '{') {
      ++depth;
      continue;
    }
    if (ch == ']' || ch == '}') {
      if (depth == 0) return false;
      --depth;
      continue;
    }
    if (ch == ',' && depth == 0) {
      elements->push_back(Trim(text.substr(element_start, i - element_start)));
      element_start = i + 1;
    }
  }
  const std::string last = Trim(text.substr(element_start, text.size() - 1 - element_start));
  if (!last.empty()) elements->push_back(last);
  return true;
}

std::optional<std::string> DescriptorRows(const std::string& descriptor,
                                          std::string_view expected_kind) {
  const std::string text = Trim(descriptor);
  if (text.size() > kDescriptorBytesLimit || text.empty()) return std::nullopt;
  const std::string kind = "\"kind\":\"" + std::string(expected_kind) + "\"";
  if (text.find(kind) == std::string::npos) return std::nullopt;
  return JsonArrayForKey(text, "rows");
}

std::optional<std::size_t> DescriptorRowCount(const std::string& descriptor,
                                              std::string_view expected_kind) {
  auto rows = DescriptorRows(descriptor, expected_kind);
  if (!rows.has_value()) return std::nullopt;
  std::vector<std::string> row_values;
  if (!SplitTopLevelJsonArray(*rows, &row_values)) return std::nullopt;
  return row_values.size();
}

std::string JsonArrayFromArguments(const FunctionCallRequest& request,
                                   std::size_t first_argument) {
  std::string out = "[";
  for (std::size_t i = first_argument; i < request.arguments.size(); ++i) {
    if (i != first_argument) out += ",";
    out += JsonScalarFromValue(request.arguments[i].value);
  }
  out += "]";
  return out;
}

std::string MakeDescriptor(std::string_view kind,
                           std::string row_shape_json,
                           std::string rows_json) {
  std::vector<std::string> row_values;
  const std::size_t row_count = SplitTopLevelJsonArray(rows_json, &row_values) ? row_values.size() : 0;
  std::string out = "{\"kind\":";
  out += JsonEscape(std::string(kind));
  out += ",\"row_shape\":";
  out += row_shape_json;
  out += ",\"rows\":";
  out += rows_json;
  out += ",\"row_count\":";
  out += std::to_string(row_count);
  out += "}";
  return out;
}

std::string RowShapeArgumentOrDefault(const FunctionCallRequest& request) {
  if (request.arguments.empty() || IsSqlNull(request.arguments[0].value)) return "[]";
  const std::string row_shape = Trim(ValueAsText(request.arguments[0].value));
  if (!row_shape.empty() && row_shape.front() == '[' && row_shape.back() == ']') return row_shape;
  return JsonEscape(row_shape);
}

std::string DescriptorRowShapeOrDefault(const std::string& descriptor) {
  auto row_shape = JsonArrayForKey(descriptor, "row_shape");
  return row_shape.value_or("[]");
}

FunctionCallResult NewDescriptor(const FunctionCallRequest& request,
                                 std::string_view kind) {
  if (request.arguments.size() > 1) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_new expects optional row shape");
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document", MakeDescriptor(kind, RowShapeArgumentOrDefault(request), "[]"))});
}

FunctionCallResult AppendDescriptorRow(const FunctionCallRequest& request,
                                       std::string_view kind) {
  if (request.arguments.size() < 2) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_append expects descriptor and row values");
  }
  if (IsSqlNull(request.arguments[0].value)) {
    return MakeFunctionSuccess(request, {MakeNullValue(std::string(kind))});
  }
  auto rows = DescriptorRows(ValueAsText(request.arguments[0].value), kind);
  if (!rows.has_value()) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_append expects a valid descriptor");
  }
  std::vector<std::string> row_values;
  if (!SplitTopLevelJsonArray(*rows, &row_values)) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_append rows payload is malformed");
  }
  if (kind == std::string_view("table_value") && request.arguments.size() == 2) {
    const std::string row = Trim(ValueAsText(request.arguments[1].value));
    std::vector<std::string> row_fields;
    if (!SplitTopLevelJsonArray(row, &row_fields)) {
      return RefuseFunctionInvalidInput(request, "table_value_append row must be an array descriptor");
    }
    row_values.push_back(row);
  } else {
    row_values.push_back(JsonArrayFromArguments(request, 1));
  }
  std::string rows_json = "[";
  for (std::size_t i = 0; i < row_values.size(); ++i) {
    if (i != 0) rows_json += ",";
    rows_json += row_values[i];
  }
  rows_json += "]";
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document", MakeDescriptor(kind, DescriptorRowShapeOrDefault(ValueAsText(request.arguments[0].value)), rows_json))});
}

FunctionCallResult SizeDescriptor(const FunctionCallRequest& request,
                                  std::string_view kind) {
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_size expects one descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("int64")});
  auto count = DescriptorRowCount(ValueAsText(request.arguments[0].value), kind);
  if (!count.has_value()) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_size expects a valid descriptor");
  }
  return MakeFunctionSuccess(request, {MakeInt64Value("int64", static_cast<std::int64_t>(*count))});
}

FunctionCallResult DescriptorToArray(const FunctionCallRequest& request,
                                     std::string_view kind) {
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_to_array expects one descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  auto rows = DescriptorRows(ValueAsText(request.arguments[0].value), kind);
  if (!rows.has_value()) {
    return RefuseFunctionInvalidInput(request, std::string(kind) + "_to_array expects a valid descriptor");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", *rows)});
}

bool IntArg(const FunctionCallRequest& request, std::size_t index, std::int64_t* out) {
  if (index >= request.arguments.size() || IsSqlNull(request.arguments[index].value) ||
      !request.arguments[index].value.has_int64_value) {
    return false;
  }
  *out = request.arguments[index].value.int64_value;
  return true;
}

FunctionCallResult GenerateSeries(const FunctionCallRequest& request) {
  if (request.arguments.size() < 2 || request.arguments.size() > 3) {
    return RefuseFunctionInvalidInput(request, "generate_series expects start, stop, and optional step");
  }
  std::int64_t start = 0;
  std::int64_t stop = 0;
  std::int64_t step = 1;
  if (!IntArg(request, 0, &start) || !IntArg(request, 1, &stop)) {
    return RefuseFunctionInvalidInput(request, "generate_series start and stop must be int64");
  }
  if (request.arguments.size() == 3 && !IntArg(request, 2, &step)) {
    return RefuseFunctionInvalidInput(request, "generate_series step must be int64");
  }
  if (step == 0) return RefuseFunctionInvalidInput(request, "generate_series step must not be zero");
  std::vector<std::int64_t> values;
  for (std::int64_t value = start;
       step > 0 ? value <= stop : value >= stop;
       value += step) {
    values.push_back(value);
    if (values.size() > kGeneratedRowsLimit) {
      return RefuseFunctionInvalidInput(request, "generate_series exceeds bounded row limit");
    }
    if ((step > 0 && value > stop - step) || (step < 0 && value < stop - step)) break;
  }
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += std::to_string(values[i]);
  }
  out += "]";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

FunctionCallResult UnnestArray(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "unnest expects one array");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  const std::string array_text = Trim(ValueAsText(request.arguments[0].value));
  std::vector<std::string> elements;
  if (!SplitTopLevelJsonArray(array_text, &elements)) {
    return RefuseFunctionInvalidInput(request, "unnest expects an array descriptor");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", array_text)});
}

FunctionCallResult SetOfGeneric(const FunctionCallRequest& request) {
  std::string columns = "[";
  for (std::size_t i = 0; i < request.arguments.size(); ++i) {
    if (i != 0) columns += ",";
    columns += JsonEscape(request.arguments[i].name.empty() ? "arg" + std::to_string(i) : request.arguments[i].name);
  }
  columns += "]";
  std::string out = "{\"kind\":\"setof\",\"columns\":";
  out += columns;
  out += ",\"rows\":[";
  out += JsonArrayFromArguments(request, 0);
  out += "],\"row_count\":1}";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

FunctionCallResult SetOfKeyTextValue(const FunctionCallRequest& request,
                                     bool document_value) {
  if (request.arguments.size() != 2) {
    return RefuseFunctionInvalidInput(request, "setof key/value form expects key and value");
  }
  if (IsSqlNull(request.arguments[0].value)) {
    return RefuseFunctionInvalidInput(request, "setof key must not be NULL");
  }
  const std::string key = ValueAsText(request.arguments[0].value);
  const std::string value = IsSqlNull(request.arguments[1].value)
                                ? "null"
                                : (document_value && LooksJsonScalar(ValueAsText(request.arguments[1].value))
                                       ? Trim(ValueAsText(request.arguments[1].value))
                                       : JsonEscape(ValueAsText(request.arguments[1].value)));
  std::string out = "{\"kind\":\"setof\",\"columns\":[\"key\",\"value\"],\"rows\":[[";
  out += JsonEscape(key);
  out += ",";
  out += value;
  out += "]],\"row_count\":1}";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

FunctionCallResult ElementOfMultiset(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "element expects one multiset");
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  std::vector<std::string> elements;
  if (!SplitTopLevelJsonArray(ValueAsText(request.arguments[0].value), &elements)) {
    return RefuseFunctionInvalidInput(request, "element expects an array-backed multiset");
  }
  if (elements.empty()) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  if (elements.size() != 1) {
    return RefuseFunctionInvalidInput(request, "element expects a singleton multiset");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", elements.front())});
}

FunctionCallResult FusionMultiset(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[]")});
  std::vector<std::string> fused;
  for (const auto& argument : request.arguments) {
    if (IsSqlNull(argument.value)) continue;
    std::vector<std::string> elements;
    if (!SplitTopLevelJsonArray(ValueAsText(argument.value), &elements)) {
      return RefuseFunctionInvalidInput(request, "fusion expects array-backed multisets");
    }
    fused.insert(fused.end(), elements.begin(), elements.end());
    if (fused.size() > kGeneratedRowsLimit) {
      return RefuseFunctionInvalidInput(request, "fusion exceeds bounded element limit");
    }
  }
  std::string out = "[";
  for (std::size_t i = 0; i < fused.size(); ++i) {
    if (i != 0) out += ",";
    out += fused[i];
  }
  out += "]";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

FunctionCallResult IntersectionMultiset(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[]")});
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  std::vector<std::string> intersection;
  if (!SplitTopLevelJsonArray(ValueAsText(request.arguments[0].value), &intersection)) {
    return RefuseFunctionInvalidInput(request, "intersection expects array-backed multisets");
  }
  for (std::size_t i = 1; i < request.arguments.size(); ++i) {
    if (IsSqlNull(request.arguments[i].value)) {
      intersection.clear();
      break;
    }
    std::vector<std::string> right_values;
    if (!SplitTopLevelJsonArray(ValueAsText(request.arguments[i].value), &right_values)) {
      return RefuseFunctionInvalidInput(request, "intersection expects array-backed multisets");
    }
    std::vector<std::string> next;
    for (const auto& value : intersection) {
      auto found = std::find(right_values.begin(), right_values.end(), value);
      if (found != right_values.end()) {
        next.push_back(value);
        right_values.erase(found);
      }
    }
    intersection = std::move(next);
  }
  std::string out = "[";
  for (std::size_t i = 0; i < intersection.size(); ++i) {
    if (i != 0) out += ",";
    out += intersection[i];
  }
  out += "]";
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", std::move(out))});
}

}  // namespace

bool IsRowsetTableFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  return request.context.package_name == "rowset.table" ||
         StartsWith(id, "sb.rowset.") ||
         StartsWith(id, "sb.table_value.") ||
         StartsWith(id, "sb.setof.") ||
         StartsWith(id, "sb.multiset.");
}

FunctionCallResult DispatchRowsetTableFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;

  if (IdIs(id, {"sb.rowset.rowset"})) return NewDescriptor(request, "rowset");
  if (IdIs(id, {"sb.rowset.new"})) return NewDescriptor(request, "rowset");
  if (IdIs(id, {"sb.rowset.append"})) return AppendDescriptorRow(request, "rowset");
  if (IdIs(id, {"sb.rowset.size"})) return SizeDescriptor(request, "rowset");
  if (IdIs(id, {"sb.rowset.to_array"})) return DescriptorToArray(request, "rowset");
  if (IdIs(id, {"sb.rowset.unnest"})) return UnnestArray(request);
  if (IdIs(id, {"sb.rowset.generate_series"})) return GenerateSeries(request);

  if (IdIs(id, {"sb.table_value.value"})) return NewDescriptor(request, "table_value");
  if (IdIs(id, {"sb.table_value.new"})) return NewDescriptor(request, "table_value");
  if (IdIs(id, {"sb.table_value.append"})) return AppendDescriptorRow(request, "table_value");

  if (IdIs(id, {"sb.setof.generic"})) return SetOfGeneric(request);
  if (IdIs(id, {"sb.setof.key_text_value_text"})) return SetOfKeyTextValue(request, false);
  if (IdIs(id, {"sb.setof.key_text_value_document"})) return SetOfKeyTextValue(request, true);

  if (IdIs(id, {"sb.multiset.element"})) return ElementOfMultiset(request);
  if (IdIs(id, {"sb.multiset.fusion"})) return FusionMultiset(request);
  if (IdIs(id, {"sb.multiset.intersection"})) return IntersectionMultiset(request);

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_NOT_IMPLEMENTED",
                                      "rowset/table function is registered but has no implementation route");
}

}  // namespace scratchbird::engine::functions
