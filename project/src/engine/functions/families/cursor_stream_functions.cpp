// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/cursor_stream_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

constexpr std::size_t kDescriptorBytesLimit = 1U << 20;

bool StartsWith(const std::string& value, const std::string& prefix) {
  return value.rfind(prefix, 0) == 0;
}

std::string Trim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) {
    return !std::isspace(ch);
  }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) {
    return !std::isspace(ch);
  }).base(), value.end());
  return value;
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

bool IdIs(const std::string& id, std::string_view candidate) {
  return id == candidate;
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

std::optional<std::string> JsonStringForKey(const std::string& document,
                                            std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = document.find(needle);
  if (key_pos == std::string::npos) return std::nullopt;
  const auto colon = document.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  if (cursor >= document.size() || document[cursor] != '"') return std::nullopt;
  ++cursor;
  std::string out;
  bool escaping = false;
  for (; cursor < document.size(); ++cursor) {
    const char ch = document[cursor];
    if (escaping) {
      switch (ch) {
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        default:
          out.push_back(ch);
          break;
      }
      escaping = false;
      continue;
    }
    if (ch == '\\') {
      escaping = true;
      continue;
    }
    if (ch == '"') return out;
    out.push_back(ch);
  }
  return std::nullopt;
}

std::optional<std::int64_t> JsonIntForKey(const std::string& document,
                                          std::string_view key) {
  const std::string needle = "\"" + std::string(key) + "\"";
  const auto key_pos = document.find(needle);
  if (key_pos == std::string::npos) return std::nullopt;
  const auto colon = document.find(':', key_pos + needle.size());
  if (colon == std::string::npos) return std::nullopt;
  std::size_t cursor = colon + 1;
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  std::size_t end = cursor;
  if (end < document.size() && document[end] == '-') ++end;
  while (end < document.size() && std::isdigit(static_cast<unsigned char>(document[end]))) ++end;
  if (end == cursor) return std::nullopt;
  try {
    return std::stoll(document.substr(cursor, end - cursor));
  } catch (...) {
    return std::nullopt;
  }
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

std::size_t RowCountFromRows(const std::string& rows_json) {
  std::vector<std::string> rows;
  return SplitTopLevelJsonArray(rows_json, &rows) ? rows.size() : 0;
}

bool HasKind(const std::string& document, std::string_view kind) {
  if (document.size() > kDescriptorBytesLimit) return false;
  return document.find("\"kind\":\"" + std::string(kind) + "\"") != std::string::npos;
}

std::optional<std::string> RowsForKind(const std::string& document, std::string_view kind) {
  const std::string text = Trim(document);
  if (!HasKind(text, kind)) return std::nullopt;
  return JsonArrayForKey(text, "rows");
}

std::string RowShapeOrDefault(const std::string& document) {
  return JsonArrayForKey(document, "row_shape").value_or("[]");
}

std::string MakeCursorDescriptor(std::string handle_id,
                                 std::string state,
                                 std::int64_t position,
                                 std::string lifetime_class,
                                 std::string holdability,
                                 std::string scrollability,
                                 std::string row_shape,
                                 std::string rows) {
  std::string out = "{\"kind\":\"cursor\",\"handle_id\":";
  out += JsonEscape(std::move(handle_id));
  out += ",\"state\":";
  out += JsonEscape(std::move(state));
  out += ",\"position\":";
  out += std::to_string(position);
  out += ",\"lifetime_class\":";
  out += JsonEscape(std::move(lifetime_class));
  out += ",\"holdability\":";
  out += JsonEscape(std::move(holdability));
  out += ",\"scrollability\":";
  out += JsonEscape(std::move(scrollability));
  out += ",\"row_shape\":";
  out += std::move(row_shape);
  out += ",\"rows\":";
  out += rows;
  out += ",\"row_count\":";
  out += std::to_string(RowCountFromRows(rows));
  out += "}";
  return out;
}

std::string MakeDefaultCursorDescriptor() {
  return MakeCursorDescriptor("cursor:inline", "open", 0, "statement", "without_hold",
                              "forward_only", "[]", "[]");
}

std::string MakeClosedCursorDescriptor(std::string cursor_descriptor) {
  const std::string text = Trim(std::move(cursor_descriptor));
  return MakeCursorDescriptor(JsonStringForKey(text, "handle_id").value_or("cursor:inline"),
                              "closed",
                              JsonIntForKey(text, "position").value_or(0),
                              JsonStringForKey(text, "lifetime_class").value_or("statement"),
                              JsonStringForKey(text, "holdability").value_or("without_hold"),
                              JsonStringForKey(text, "scrollability").value_or("forward_only"),
                              RowShapeOrDefault(text),
                              JsonArrayForKey(text, "rows").value_or("[]"));
}

std::string MakeStreamDescriptor(std::string handle_id, std::string state, std::string rows) {
  std::string out = "{\"kind\":\"stream\",\"handle_id\":";
  out += JsonEscape(std::move(handle_id));
  out += ",\"state\":";
  out += JsonEscape(std::move(state));
  out += ",\"rows\":";
  out += rows;
  out += ",\"row_count\":";
  out += std::to_string(RowCountFromRows(rows));
  out += "}";
  return out;
}

std::string MakeRowsetDescriptor(std::string rows) {
  std::string out = "{\"kind\":\"rowset\",\"row_shape\":[],\"rows\":";
  out += rows;
  out += ",\"row_count\":";
  out += std::to_string(RowCountFromRows(rows));
  out += "}";
  return out;
}

std::string MakeLocatorDescriptor(const std::string& cursor_descriptor) {
  const std::string handle_id = JsonStringForKey(cursor_descriptor, "handle_id").value_or("cursor:inline");
  const auto position = JsonIntForKey(cursor_descriptor, "position").value_or(0);
  std::string out = "{\"kind\":\"locator\",\"locator_id\":";
  out += JsonEscape(handle_id + ":" + std::to_string(position));
  out += ",\"source_handle_kind\":\"cursor\",\"position\":";
  out += std::to_string(position);
  out += ",\"valid\":true}";
  return out;
}

scratchbird::engine::sblr::SblrValue MakeBoolValue(bool input) {
  scratchbird::engine::sblr::SblrValue value;
  value.descriptor_id = "boolean";
  value.payload_kind = scratchbird::engine::sblr::SblrValuePayloadKind::boolean;
  value.int64_value = input ? 1 : 0;
  value.has_int64_value = true;
  value.encoded_value = input ? "true" : "false";
  value.text_value = value.encoded_value;
  value.is_null = false;
  return value;
}

std::string CursorDescriptorArgumentOrDefault(const FunctionCallRequest& request) {
  if (request.arguments.empty() || IsSqlNull(request.arguments[0].value)) return MakeDefaultCursorDescriptor();
  return ValueAsText(request.arguments[0].value);
}

FunctionCallResult CursorOpen(const FunctionCallRequest& request) {
  if (request.arguments.size() > 1) {
    return RefuseFunctionInvalidInput(request, "cursor_open expects optional select descriptor");
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document",
                     MakeCursorDescriptor("cursor:open", "open", 0, "statement",
                                          "without_hold", "forward_only", "[]", "[]"))});
}

FunctionCallResult CursorClose(const FunctionCallRequest& request) {
  if (request.arguments.size() > 1) {
    return RefuseFunctionInvalidInput(request, "cursor_close expects optional cursor descriptor");
  }
  const std::string cursor_descriptor = CursorDescriptorArgumentOrDefault(request);
  if (!HasKind(cursor_descriptor, "cursor")) {
    return RefuseFunctionInvalidInput(request, "cursor_close expects a cursor descriptor");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeClosedCursorDescriptor(cursor_descriptor))});
}

FunctionCallResult CursorTextAttribute(const FunctionCallRequest& request,
                                       std::string_view attribute,
                                       std::string_view default_value) {
  if (request.arguments.size() > 1) {
    return RefuseFunctionInvalidInput(request, std::string(attribute) + " expects optional cursor descriptor");
  }
  const std::string cursor_descriptor = CursorDescriptorArgumentOrDefault(request);
  if (!HasKind(cursor_descriptor, "cursor")) {
    return RefuseFunctionInvalidInput(request, std::string(attribute) + " expects a cursor descriptor");
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("character", JsonStringForKey(cursor_descriptor, attribute).value_or(std::string(default_value)))});
}

FunctionCallResult CursorPosition(const FunctionCallRequest& request) {
  if (request.arguments.size() > 1) {
    return RefuseFunctionInvalidInput(request, "cursor_position expects optional cursor descriptor");
  }
  const std::string cursor_descriptor = CursorDescriptorArgumentOrDefault(request);
  if (!HasKind(cursor_descriptor, "cursor")) {
    return RefuseFunctionInvalidInput(request, "cursor_position expects a cursor descriptor");
  }
  return MakeFunctionSuccess(
      request,
      {MakeInt64Value("int64", JsonIntForKey(cursor_descriptor, "position").value_or(0))});
}

FunctionCallResult CursorActive(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeBoolValue(false)});
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "cursor_active expects optional cursor name or descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeBoolValue(false)});
  const std::string value = ValueAsText(request.arguments[0].value);
  if (HasKind(value, "cursor")) {
    return MakeFunctionSuccess(
        request,
        {MakeBoolValue(JsonStringForKey(value, "state").value_or("closed") == "open")});
  }
  return MakeFunctionSuccess(request, {MakeBoolValue(false)});
}

FunctionCallResult HandleKind(const FunctionCallRequest& request) {
  if (request.arguments.empty()) return MakeFunctionSuccess(request, {MakeTextValue("character", "none")});
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "handle_kind expects one handle descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
  const std::string descriptor = ValueAsText(request.arguments[0].value);
  for (const std::string_view kind : {"cursor", "rowset", "table_value", "stream", "locator"}) {
    if (HasKind(descriptor, kind)) return MakeFunctionSuccess(request, {MakeTextValue("character", std::string(kind))});
  }
  return RefuseFunctionInvalidInput(request, "handle_kind expects a known handle descriptor");
}

FunctionCallResult RowsetToCursor(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "rowset_to_cursor expects one rowset descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  const std::string rowset = ValueAsText(request.arguments[0].value);
  auto rows = RowsForKind(rowset, "rowset");
  if (!rows.has_value()) {
    return RefuseFunctionInvalidInput(request, "rowset_to_cursor expects a rowset descriptor");
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document",
                     MakeCursorDescriptor("cursor:rowset", "open", 0, "statement",
                                          "without_hold", "forward_only", RowShapeOrDefault(rowset), *rows))});
}

FunctionCallResult TableValueToCursor(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "table_value_to_cursor expects one table_value descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  const std::string table_value = ValueAsText(request.arguments[0].value);
  auto rows = RowsForKind(table_value, "table_value");
  if (!rows.has_value()) {
    return RefuseFunctionInvalidInput(request, "table_value_to_cursor expects a table_value descriptor");
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document",
                     MakeCursorDescriptor("cursor:table_value", "open", 0, "statement",
                                          "without_hold", "forward_only", RowShapeOrDefault(table_value), *rows))});
}

FunctionCallResult CursorToRowset(const FunctionCallRequest& request) {
  if (request.arguments.empty() || request.arguments.size() > 2) {
    return RefuseFunctionInvalidInput(request, "cursor_to_rowset expects cursor descriptor and optional max_rows");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  const std::string cursor = ValueAsText(request.arguments[0].value);
  auto rows = RowsForKind(cursor, "cursor");
  if (!rows.has_value()) {
    return RefuseFunctionInvalidInput(request, "cursor_to_rowset expects a cursor descriptor");
  }
  if (request.arguments.size() == 2 &&
      (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value < 0)) {
    return RefuseFunctionInvalidInput(request, "cursor_to_rowset max_rows must be non-negative int64");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeRowsetDescriptor(*rows))});
}

FunctionCallResult StreamToRowset(const FunctionCallRequest& request) {
  if (request.arguments.empty() || request.arguments.size() > 2) {
    return RefuseFunctionInvalidInput(request, "stream_to_rowset expects stream descriptor and optional max_rows");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  const std::string stream = ValueAsText(request.arguments[0].value);
  auto rows = RowsForKind(stream, "stream");
  if (!rows.has_value()) {
    return RefuseFunctionInvalidInput(request, "stream_to_rowset expects a stream descriptor");
  }
  if (request.arguments.size() == 2 &&
      (!request.arguments[1].value.has_int64_value || request.arguments[1].value.int64_value < 0)) {
    return RefuseFunctionInvalidInput(request, "stream_to_rowset max_rows must be non-negative int64");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeRowsetDescriptor(*rows))});
}

FunctionCallResult StreamClose(const FunctionCallRequest& request) {
  if (request.arguments.size() > 1) {
    return RefuseFunctionInvalidInput(request, "stream_close expects optional stream descriptor");
  }
  std::string rows = "[]";
  if (!request.arguments.empty() && !IsSqlNull(request.arguments[0].value)) {
    const std::string stream = ValueAsText(request.arguments[0].value);
    auto parsed_rows = RowsForKind(stream, "stream");
    if (!parsed_rows.has_value()) {
      return RefuseFunctionInvalidInput(request, "stream_close expects a stream descriptor");
    }
    rows = *parsed_rows;
  }
  return MakeFunctionSuccess(
      request,
      {MakeTextValue("json_document", MakeStreamDescriptor("stream:inline", "closed", rows))});
}

FunctionCallResult CurrentRowLocator(const FunctionCallRequest& request) {
  if (request.arguments.size() != 1) {
    return RefuseFunctionInvalidInput(request, "current_row_locator expects one cursor descriptor");
  }
  if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
  const std::string cursor = ValueAsText(request.arguments[0].value);
  if (!HasKind(cursor, "cursor")) {
    return RefuseFunctionInvalidInput(request, "current_row_locator expects a cursor descriptor");
  }
  return MakeFunctionSuccess(request, {MakeTextValue("json_document", MakeLocatorDescriptor(cursor))});
}

}  // namespace

bool IsCursorStreamFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  return request.context.package_name == "cursor.stream" ||
         StartsWith(id, "sb.cursor.") ||
         StartsWith(id, "sb.stream.") ||
         StartsWith(id, "sb.handle.");
}

FunctionCallResult DispatchCursorStreamFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (IdIs(id, "sb.cursor.open")) return CursorOpen(request);
  if (IdIs(id, "sb.cursor.close")) return CursorClose(request);
  if (IdIs(id, "sb.cursor.state")) return CursorTextAttribute(request, "state", "open");
  if (IdIs(id, "sb.cursor.position")) return CursorPosition(request);
  if (IdIs(id, "sb.cursor.lifetime_class")) return CursorTextAttribute(request, "lifetime_class", "statement");
  if (IdIs(id, "sb.cursor.holdability")) return CursorTextAttribute(request, "holdability", "without_hold");
  if (IdIs(id, "sb.cursor.scrollability")) return CursorTextAttribute(request, "scrollability", "forward_only");
  if (IdIs(id, "sb.cursor.active")) return CursorActive(request);
  if (IdIs(id, "sb.cursor.current_row_locator")) return CurrentRowLocator(request);
  if (IdIs(id, "sb.cursor.to_rowset")) return CursorToRowset(request);
  if (IdIs(id, "sb.cursor.rowset_to_cursor")) return RowsetToCursor(request);
  if (IdIs(id, "sb.cursor.table_value_to_cursor")) return TableValueToCursor(request);
  if (IdIs(id, "sb.stream.close")) return StreamClose(request);
  if (IdIs(id, "sb.stream.to_rowset")) return StreamToRowset(request);
  if (IdIs(id, "sb.handle.kind")) return HandleKind(request);

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_FUNCTION_NOT_IMPLEMENTED",
                                      "cursor/stream function is registered but has no implementation route");
}

}  // namespace scratchbird::engine::functions
