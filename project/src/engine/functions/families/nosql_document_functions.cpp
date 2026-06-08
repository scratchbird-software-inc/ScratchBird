// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "families/nosql_document_functions.hpp"

#include "common/function_result_helpers.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::functions {
namespace {

std::string Trim(std::string value) {
  value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char ch) { return !std::isspace(ch); }));
  value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char ch) { return !std::isspace(ch); }).base(), value.end());
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

std::string JsonTypeOf(const std::string& raw) {
  const std::string value = Trim(raw);
  if (value.empty()) return "unknown";
  if (value == "null") return "null";
  if (value == "true" || value == "false") return "boolean";
  if (value.front() == '{' && value.back() == '}') return "object";
  if (value.front() == '[' && value.back() == ']') return "array";
  if (value.front() == '"' && value.back() == '"') return "string";
  if (std::isdigit(static_cast<unsigned char>(value.front())) || value.front() == '-') return "number";
  return "unknown";
}

bool JsonLooksValid(const std::string& value) {
  return JsonTypeOf(value) != "unknown";
}

bool DescriptorAcceptsDocument(const std::string& descriptor_id) {
  return descriptor_id == "json" || descriptor_id == "json_document" || descriptor_id == "document" ||
         descriptor_id == "bson" || descriptor_id == "variant" || descriptor_id == "xml" ||
         descriptor_id == "xml_document";
}

std::string LowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

std::string NamedArgumentText(const FunctionCallRequest& request, std::string_view name) {
  for (std::size_t i = 2; i < request.arguments.size(); ++i) {
    if (LowerAscii(request.arguments[i].name) == name) return LowerAscii(Trim(ValueAsText(request.arguments[i].value)));
  }
  return {};
}

std::string JsonEscape(std::string value) {
  std::string out = "\"";
  for (const char ch : value) {
    if (ch == '\\' || ch == '"') out.push_back('\\');
    out.push_back(ch);
  }
  out.push_back('"');
  return out;
}

std::string ExtractJsonObjectField(const std::string& document, const std::string& path);

std::optional<std::string> ParseJsonStringToken(const std::string& document, std::size_t* cursor) {
  if (*cursor >= document.size() || document[*cursor] != '"') return std::nullopt;
  ++(*cursor);
  std::string out;
  while (*cursor < document.size()) {
    const char ch = document[(*cursor)++];
    if (ch == '"') return out;
    if (ch != '\\') {
      out.push_back(ch);
      continue;
    }
    if (*cursor >= document.size()) return std::nullopt;
    const char escaped = document[(*cursor)++];
    switch (escaped) {
      case '"':
      case '\\':
      case '/': out.push_back(escaped); break;
      case 'b': out.push_back('\b'); break;
      case 'f': out.push_back('\f'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case 't': out.push_back('\t'); break;
      default: return std::nullopt;
    }
  }
  return std::nullopt;
}

std::optional<std::size_t> FindJsonValueEnd(const std::string& document, std::size_t cursor) {
  while (cursor < document.size() && std::isspace(static_cast<unsigned char>(document[cursor]))) ++cursor;
  if (cursor >= document.size()) return std::nullopt;
  bool in_string = false;
  int depth = 0;
  for (std::size_t i = cursor; i < document.size(); ++i) {
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
    if (ch == '{' || ch == '[') {
      ++depth;
      continue;
    }
    if (ch == '}' || ch == ']') {
      if (depth == 0) return i;
      --depth;
      continue;
    }
    if (depth == 0 && ch == ',') return i;
  }
  return document.size();
}

struct JsonObjectEntry {
  std::string key;
  std::string value;
};

bool ParseTopLevelJsonObject(const std::string& document, std::vector<JsonObjectEntry>* entries) {
  const std::string normalized = Trim(document);
  if (normalized.size() < 2 || normalized.front() != '{' || normalized.back() != '}') return false;
  std::size_t cursor = 1;
  while (cursor < normalized.size() - 1) {
    while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor]))) ++cursor;
    if (cursor < normalized.size() && normalized[cursor] == '}') return true;
    auto key = ParseJsonStringToken(normalized, &cursor);
    if (!key.has_value()) return false;
    while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor]))) ++cursor;
    if (cursor >= normalized.size() || normalized[cursor] != ':') return false;
    ++cursor;
    const auto value_end = FindJsonValueEnd(normalized, cursor);
    if (!value_end.has_value()) return false;
    entries->push_back({std::move(*key), Trim(normalized.substr(cursor, *value_end - cursor))});
    cursor = *value_end;
    while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor]))) ++cursor;
    if (cursor < normalized.size() && normalized[cursor] == ',') {
      ++cursor;
      continue;
    }
    if (cursor < normalized.size() && normalized[cursor] == '}') return true;
  }
  return true;
}

bool SplitTopLevelJsonArray(const std::string& document, std::vector<std::string>* elements) {
  const std::string normalized = Trim(document);
  if (normalized.size() < 2 || normalized.front() != '[' || normalized.back() != ']') return false;
  std::size_t cursor = 1;
  while (cursor < normalized.size() - 1) {
    while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor]))) ++cursor;
    if (cursor < normalized.size() && normalized[cursor] == ']') return true;
    const auto value_end = FindJsonValueEnd(normalized, cursor);
    if (!value_end.has_value()) return false;
    elements->push_back(Trim(normalized.substr(cursor, *value_end - cursor)));
    cursor = *value_end;
    while (cursor < normalized.size() && std::isspace(static_cast<unsigned char>(normalized[cursor]))) ++cursor;
    if (cursor < normalized.size() && normalized[cursor] == ',') {
      ++cursor;
      continue;
    }
    if (cursor < normalized.size() && normalized[cursor] == ']') return true;
  }
  return true;
}

bool ParseJsonTextArray(const std::string& document, std::vector<std::string>* elements) {
  std::vector<std::string> raw_elements;
  if (!SplitTopLevelJsonArray(document, &raw_elements)) return false;
  for (const auto& raw : raw_elements) {
    const std::string value = Trim(raw);
    if (value == "null") {
      elements->push_back(std::string());
      continue;
    }
    if (!value.empty() && value.front() == '"') {
      std::size_t cursor = 0;
      auto parsed = ParseJsonStringToken(value, &cursor);
      if (!parsed.has_value()) return false;
      while (cursor < value.size() && std::isspace(static_cast<unsigned char>(value[cursor]))) ++cursor;
      if (cursor != value.size()) return false;
      elements->push_back(std::move(*parsed));
      continue;
    }
    elements->push_back(value);
  }
  return true;
}

std::string JsonObjectKeysArray(const std::string& document, bool* ok) {
  std::vector<JsonObjectEntry> entries;
  if (!ParseTopLevelJsonObject(document, &entries)) {
    *ok = false;
    return {};
  }
  *ok = true;
  std::string out = "[";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += JsonEscape(entries[i].key);
  }
  out.push_back(']');
  return out;
}

std::string JsonArrayElementsArray(const std::string& document, bool text_mode, bool* ok) {
  std::vector<std::string> elements;
  if (!SplitTopLevelJsonArray(document, &elements)) {
    *ok = false;
    return {};
  }
  *ok = true;
  std::string out = "[";
  for (std::size_t i = 0; i < elements.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += text_mode ? JsonEscape(Trim(elements[i])) : Trim(elements[i]);
  }
  out.push_back(']');
  return out;
}

std::string JsonEachArray(const std::string& document, bool text_mode, bool* ok) {
  std::vector<JsonObjectEntry> entries;
  if (!ParseTopLevelJsonObject(document, &entries)) {
    *ok = false;
    return {};
  }
  *ok = true;
  std::string out = "[";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += "{\"key\":";
    out += JsonEscape(entries[i].key);
    out += ",\"value\":";
    out += text_mode ? JsonEscape(entries[i].value) : entries[i].value;
    out += "}";
  }
  out.push_back(']');
  return out;
}

std::string JsonStripNulls(const std::string& document, bool* ok) {
  std::vector<JsonObjectEntry> entries;
  if (!ParseTopLevelJsonObject(document, &entries)) {
    *ok = false;
    return {};
  }
  *ok = true;
  std::string out = "{";
  bool first = true;
  for (const auto& entry : entries) {
    if (Trim(entry.value) == "null") continue;
    if (!first) out.push_back(',');
    first = false;
    out += JsonEscape(entry.key);
    out.push_back(':');
    out += entry.value;
  }
  out.push_back('}');
  return out;
}

std::string JsonPretty(const std::string& document) {
  const std::string normalized = Trim(document);
  std::string out;
  int indent = 0;
  bool in_string = false;
  auto newline = [&]() {
    out.push_back('\n');
    out.append(static_cast<std::size_t>(std::max(indent, 0)) * 2u, ' ');
  };
  for (std::size_t i = 0; i < normalized.size(); ++i) {
    const char ch = normalized[i];
    if (in_string) {
      out.push_back(ch);
      if (ch == '\\' && i + 1 < normalized.size()) {
        out.push_back(normalized[++i]);
      } else if (ch == '"') {
        in_string = false;
      }
      continue;
    }
    if (ch == '"') {
      in_string = true;
      out.push_back(ch);
    } else if (ch == '{' || ch == '[') {
      out.push_back(ch);
      ++indent;
      newline();
    } else if (ch == '}' || ch == ']') {
      --indent;
      newline();
      out.push_back(ch);
    } else if (ch == ',') {
      out.push_back(ch);
      newline();
    } else if (ch == ':') {
      out += ": ";
    } else if (!std::isspace(static_cast<unsigned char>(ch))) {
      out.push_back(ch);
    }
  }
  return out;
}

std::string JsonPathQueryArray(const std::string& document, const std::string& path) {
  const std::string extracted = ExtractJsonObjectField(document, path);
  if (extracted.empty()) return "[]";
  return "[" + extracted + "]";
}

std::string JsonLiteralFromValue(const scratchbird::engine::sblr::SblrValue& value) {
  if (IsSqlNull(value)) return "null";
  if (value.descriptor_id == "boolean") {
    return (value.has_int64_value && value.int64_value != 0) || LowerAscii(ValueAsText(value)) == "true" ? "true" : "false";
  }
  if (value.has_int64_value || value.has_uint64_value || value.has_real64_value ||
      DescriptorAcceptsDocument(value.descriptor_id)) {
    return ValueAsText(value);
  }
  return JsonEscape(ValueAsText(value));
}

std::string ExtractJsonObjectField(const std::string& document, const std::string& path) {
  if (path == "$") return document;
  if (!StartsWith(path, "$.")) return {};
  const std::string key = "\"" + path.substr(2) + "\"";
  const auto key_pos = document.find(key);
  if (key_pos == std::string::npos) return {};
  const auto colon_pos = document.find(':', key_pos + key.size());
  if (colon_pos == std::string::npos) return {};
  std::size_t pos = colon_pos + 1;
  while (pos < document.size() && std::isspace(static_cast<unsigned char>(document[pos]))) ++pos;
  if (pos >= document.size()) return {};
  if (document[pos] == '"') {
    const auto end = document.find('"', pos + 1);
    return end == std::string::npos ? std::string() : document.substr(pos, end - pos + 1);
  }
  std::size_t end = pos;
  int depth = 0;
  while (end < document.size()) {
    const char ch = document[end];
    if (ch == '{' || ch == '[') ++depth;
    if (ch == '}' || ch == ']') {
      if (depth == 0) break;
      --depth;
    }
    if (depth == 0 && ch == ',') break;
    ++end;
  }
  return Trim(document.substr(pos, end - pos));
}

std::size_t JsonArrayLength(const std::string& document, bool* ok) {
  const std::string normalized = Trim(document);
  if (normalized.size() < 2 || normalized.front() != '[' || normalized.back() != ']') {
    *ok = false;
    return 0;
  }
  *ok = true;
  const std::string body = Trim(normalized.substr(1, normalized.size() - 2));
  if (body.empty()) return 0;
  std::size_t count = 1;
  int depth = 0;
  bool in_string = false;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const char ch = body[i];
    if (ch == '"' && (i == 0 || body[i - 1] != '\\')) in_string = !in_string;
    if (in_string) continue;
    if (ch == '{' || ch == '[') ++depth;
    if (ch == '}' || ch == ']') --depth;
    if (ch == ',' && depth == 0) ++count;
  }
  return count;
}

std::string JsonBuildArray(const std::vector<FunctionArgument>& arguments) {
  std::string out = "[";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) out += ",";
    out += JsonLiteralFromValue(arguments[i].value);
  }
  out += "]";
  return out;
}

std::string JsonBuildObject(const std::vector<FunctionArgument>& arguments, bool* ok) {
  if ((arguments.size() % 2) != 0) {
    *ok = false;
    return {};
  }
  *ok = true;
  std::string out = "{";
  for (std::size_t i = 0; i < arguments.size(); i += 2) {
    if (i != 0) out += ",";
    out += JsonEscape(ValueAsText(arguments[i].value));
    out += ":";
    out += JsonLiteralFromValue(arguments[i + 1].value);
  }
  out += "}";
  return out;
}

bool JsonControlBool(const scratchbird::engine::sblr::SblrValue& value, bool* ok);

std::string JsonObjectFromTextArrays(const std::vector<FunctionArgument>& arguments, bool* ok) {
  if (arguments.empty() || arguments.size() > 2) {
    *ok = false;
    return {};
  }
  if (IsSqlNull(arguments[0].value) || (arguments.size() == 2 && IsSqlNull(arguments[1].value))) {
    *ok = true;
    return "null";
  }
  std::vector<std::string> keys;
  if (!ParseJsonTextArray(ValueAsText(arguments[0].value), &keys)) {
    *ok = false;
    return {};
  }
  std::vector<std::string> values;
  if (arguments.size() == 1) {
    if ((keys.size() % 2) != 0) {
      *ok = false;
      return {};
    }
    for (std::size_t i = 1; i < keys.size(); i += 2) values.push_back(keys[i]);
    std::vector<std::string> paired_keys;
    for (std::size_t i = 0; i < keys.size(); i += 2) paired_keys.push_back(keys[i]);
    keys = std::move(paired_keys);
  } else if (!ParseJsonTextArray(ValueAsText(arguments[1].value), &values) || keys.size() != values.size()) {
    *ok = false;
    return {};
  }
  *ok = true;
  std::string out = "{";
  for (std::size_t i = 0; i < keys.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += JsonEscape(keys[i]);
    out.push_back(':');
    out += JsonEscape(values[i]);
  }
  out.push_back('}');
  return out;
}

bool JsonDescriptorLooksArrayLike(const std::string& descriptor_id) {
  const std::string normalized = LowerAscii(descriptor_id);
  return normalized == "array" || normalized == "json_array" || normalized == "text_array" ||
         normalized == "character_array" || normalized.find("array") != std::string::npos ||
         normalized.find("list") != std::string::npos;
}

std::optional<bool> JsonOptionalPrettyFlag(const std::vector<FunctionArgument>& arguments) {
  if (arguments.empty()) return false;
  const auto& last = arguments.back();
  const std::string name = LowerAscii(Trim(last.name));
  if (arguments.size() < 2 || (name != "pretty" && last.value.descriptor_id != "boolean")) return false;
  if (IsSqlNull(last.value)) return std::nullopt;
  bool ok = false;
  const bool pretty = JsonControlBool(last.value, &ok);
  if (!ok) return std::nullopt;
  return pretty;
}

std::string JsonArrayToJson(const std::vector<FunctionArgument>& arguments, bool pretty, bool* ok) {
  if (arguments.empty() || arguments.size() > 2 || IsSqlNull(arguments[0].value)) {
    *ok = !arguments.empty() && arguments.size() <= 2;
    return "null";
  }
  const std::string input = Trim(ValueAsText(arguments[0].value));
  std::string document;
  if (JsonTypeOf(input) == "array") {
    document = input;
  } else if (JsonDescriptorLooksArrayLike(arguments[0].value.descriptor_id)) {
    std::vector<std::string> elements;
    if (!ParseJsonTextArray(input, &elements)) {
      *ok = false;
      return {};
    }
    document = "[";
    for (std::size_t i = 0; i < elements.size(); ++i) {
      if (i != 0) document.push_back(',');
      document += JsonEscape(elements[i]);
    }
    document.push_back(']');
  } else {
    document = "[" + JsonLiteralFromValue(arguments[0].value) + "]";
  }
  *ok = true;
  return pretty ? JsonPretty(document) : document;
}

std::string JsonRowToJson(const std::vector<FunctionArgument>& arguments, bool pretty, bool* ok) {
  if (arguments.empty()) {
    *ok = false;
    return {};
  }
  const std::size_t field_count = (arguments.size() >= 2 &&
                                   (LowerAscii(Trim(arguments.back().name)) == "pretty" ||
                                    arguments.back().value.descriptor_id == "boolean"))
                                      ? arguments.size() - 1
                                      : arguments.size();
  if (field_count == 0 || IsSqlNull(arguments[0].value)) {
    *ok = true;
    return "null";
  }
  std::string document;
  const std::string first_text = Trim(ValueAsText(arguments[0].value));
  if (field_count == 1 && JsonTypeOf(first_text) == "object") {
    document = first_text;
  } else {
    document = "{";
    for (std::size_t i = 0; i < field_count; ++i) {
      if (i != 0) document.push_back(',');
      const std::string name = Trim(arguments[i].name).empty() ? "f" + std::to_string(i + 1)
                                                               : Trim(arguments[i].name);
      document += JsonEscape(name);
      document.push_back(':');
      document += JsonLiteralFromValue(arguments[i].value);
    }
    document.push_back('}');
  }
  *ok = true;
  return pretty ? JsonPretty(document) : document;
}

std::string JsonAggArray(const std::vector<FunctionArgument>& arguments) {
  std::string out = "[";
  for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += JsonLiteralFromValue(arguments[i].value);
  }
  out.push_back(']');
  return out;
}

std::string RemoveJsonObjectField(const std::string& document, const std::string& path);

bool JsonObjectFieldExists(const std::string& document, const std::string& path) {
  if (!StartsWith(path, "$.") || JsonTypeOf(document) != "object") return false;
  const std::string quoted_key = "\"" + path.substr(2) + "\"";
  return document.find(quoted_key) != std::string::npos;
}

std::string MutateJsonObjectField(const std::string& document,
                                  const std::string& path,
                                  const std::string& value,
                                  bool create_missing,
                                  bool replace_existing) {
  if (!StartsWith(path, "$.") || JsonTypeOf(document) != "object") return document;
  const std::string key = path.substr(2);
  const std::string quoted_key = "\"" + key + "\"";
  if (JsonObjectFieldExists(document, path)) {
    if (!replace_existing) return document;
    const std::string without_key = RemoveJsonObjectField(document, path);
    return without_key.empty() ? document : MutateJsonObjectField(without_key, path, value, true, false);
  }
  if (!create_missing) return document;
  std::string normalized = Trim(document);
  if (normalized.size() <= 2) return "{" + quoted_key + ":" + value + "}";
  normalized.pop_back();
  normalized += "," + quoted_key + ":" + value + "}";
  return normalized;
}

std::string RemoveJsonObjectField(const std::string& document, const std::string& path) {
  if (!StartsWith(path, "$.") || JsonTypeOf(document) != "object") return document;
  const std::string key = "\"" + path.substr(2) + "\"";
  const auto key_pos = document.find(key);
  if (key_pos == std::string::npos) return document;
  std::size_t start = key_pos;
  while (start > 0 && std::isspace(static_cast<unsigned char>(document[start - 1]))) --start;
  if (start > 0 && document[start - 1] == ',') --start;
  const auto colon_pos = document.find(':', key_pos + key.size());
  if (colon_pos == std::string::npos) return document;
  std::size_t end = colon_pos + 1;
  int depth = 0;
  bool in_string = false;
  while (end < document.size()) {
    const char ch = document[end];
    if (ch == '"' && (end == 0 || document[end - 1] != '\\')) in_string = !in_string;
    if (!in_string && (ch == '{' || ch == '[')) ++depth;
    if (!in_string && (ch == '}' || ch == ']')) {
      if (depth == 0) break;
      --depth;
    }
    if (!in_string && depth == 0 && ch == ',') {
      ++end;
      break;
    }
    ++end;
  }
  std::string out = document.substr(0, start) + document.substr(end);
  if (out == "{,}" || out == "{ }") return "{}";
  return out;
}

bool JsonControlBool(const scratchbird::engine::sblr::SblrValue& value, bool* ok) {
  if (value.has_int64_value) {
    *ok = true;
    return value.int64_value != 0;
  }
  const std::string text = LowerAscii(ValueAsText(value));
  if (text == "true" || text == "1") {
    *ok = true;
    return true;
  }
  if (text == "false" || text == "0") {
    *ok = true;
    return false;
  }
  *ok = false;
  return false;
}

constexpr std::size_t kXmlMaxScalarBytes = 1024u * 1024u;
constexpr std::size_t kXmlMaxArguments = 1024u;

bool IsXmlForestId(const std::string& id) {
  return IdIs(id, {"sb.xml.forest", "XMLFOREST", "xmlforest", "XMLFOREST(expr[ASname],...)",
                   "SBSQL-0C16676374C8", "SBSQL-6C89436D2254"});
}

bool IsXmlCastId(const std::string& id) {
  return IdIs(id, {"sb.xml.cast", "XMLCAST", "xmlcast", "XMLCAST(exprAStype)",
                   "XMLCAST(expr AS type)", "SBSQL-2BBA1DA50B23", "SBSQL-0C8A8486F751"});
}

bool IsXmlExistsId(const std::string& id) {
  return IdIs(id, {"sb.xml.exists", "XMLEXISTS", "xmlexists", "XMLEXISTS(xq[namespaces][PASSING...])",
                   "SBSQL-104DD993AED4", "SBSQL-EEA4907830CB"});
}

bool IsXmlAttributesId(const std::string& id) {
  return IdIs(id, {"sb.xml.attributes", "XMLATTRIBUTES", "xmlattributes",
                   "XMLATTRIBUTES(expr[ASname],...)", "SBSQL-1FD7CBD0921F", "SBSQL-E2022718464C"});
}

bool IsXmlConcatId(const std::string& id) {
  return IdIs(id, {"sb.xml.concat", "XMLCONCAT", "xmlconcat", "XMLCONCAT(expr_list)",
                   "SBSQL-934D2E7C0508", "SBSQL-2B38A69D425B"});
}

bool IsXmlCommentId(const std::string& id) {
  return IdIs(id, {"sb.xml.comment", "XMLCOMMENT", "xmlcomment", "XMLCOMMENT(text)",
                   "SBSQL-4F494D9A6610", "SBSQL-7881C81BBBE8"});
}

bool IsXmlPiId(const std::string& id) {
  return IdIs(id, {"sb.xml.pi", "XMLPI", "xmlpi", "XMLPI(NAMEtarget[,content])",
                   "SBSQL-DC75730A32EA", "SBSQL-51E09D00A979"});
}

bool IsXmlRootId(const std::string& id) {
  return IdIs(id, {"sb.xml.root", "XMLROOT", "xmlroot", "XMLROOT(expr,VERSIONver[,STANDALONE...])",
                   "SBSQL-52CC2FA7719D", "SBSQL-A31D3F4A9E77"});
}

bool IsXmlElementId(const std::string& id) {
  return IdIs(id, {"sb.xml.element", "XMLELEMENT", "xmlelement",
                   "XMLELEMENT(NAMEname[,namespaces][,attrs][,content_list])",
                   "SBSQL-54EBF8EDE58A"});
}

bool IsXmlAggId(const std::string& id) {
  return IdIs(id, {"sb.xml.agg", "XMLAGG", "xmlagg", "XMLAGG(expr[ORDERBY...])",
                   "SBSQL-5702FA6BF536", "SBSQL-94785A48EF57"});
}

bool IsXmlTableId(const std::string& id) {
  return IdIs(id, {"sb.xml.table", "XMLTABLE", "xmltable",
                   "XMLTABLE([namespaces,]xq[PASSING...]COLUMNS(...))",
                   "SBSQL-F0C5F1661298", "SBSQL-796CAD6CD56E"});
}

bool IsXmlDocumentId(const std::string& id) {
  return IdIs(id, {"sb.xml.document", "XMLDOCUMENT", "xmldocument", "xml",
                   "XMLDOCUMENT(expr)", "SBSQL-253585ABE51D", "SBSQL-5753A90D2A1C",
                   "SBSQL-663D565ADA02"});
}

bool IsXmlNamespacesId(const std::string& id) {
  return IdIs(id, {"sb.xml.namespaces", "XMLNAMESPACES", "xmlnamespaces",
                   "XMLNAMESPACES(decl_list)", "SBSQL-9D96355276FC", "SBSQL-4F9AE84DDF5A"});
}

bool IsXmlParseId(const std::string& id) {
  return IdIs(id, {"sb.xml.parse", "XMLPARSE", "xmlparse",
                   "XMLPARSE(DOCUMENT|CONTENTexpr[PRESERVE|STRIPWHITESPACE])",
                   "SBSQL-965B96256EB3", "SBSQL-F48761720168"});
}

bool IsXmlQueryId(const std::string& id) {
  return IdIs(id, {"sb.xml.query", "XMLQUERY", "xmlquery",
                   "XMLQUERY(xq[namespaces][PASSING...][RETURNING...][ONEMPTY])",
                   "SBSQL-B9BD61883168", "SBSQL-04FE00443530"});
}

bool IsXmlSerializeId(const std::string& id) {
  return IdIs(id, {"sb.xml.serialize", "XMLSERIALIZE", "xmlserialize",
                   "XMLSERIALIZE(DOCUMENT|CONTENT|SEQUENCEexprAStype[...])",
                   "SBSQL-24C067DA97B0", "SBSQL-C9809EF23816"});
}

bool IsXmlTextId(const std::string& id) {
  return IdIs(id, {"sb.xml.text", "XMLTEXT", "xmltext", "XMLTEXT(text)",
                   "SBSQL-82BBA556D880", "SBSQL-D53A57E7DD0B"});
}

bool IsXmlValidateId(const std::string& id) {
  return IdIs(id, {"sb.xml.validate", "XMLVALIDATE", "xmlvalidate",
                   "XMLVALIDATE(DOCUMENT|CONTENT|SEQUENCEexpr[ACCORDINGTOXMLSCHEMA...])",
                   "SBSQL-666EAE033CFC", "SBSQL-B4880446510E"});
}

bool IsXmlNsId(const std::string& id) {
  return IdIs(id, {"sb.xml.ns", "xml.ns", "SBSQL-2ABE2825F6A1"});
}

bool IsXmlAttrsId(const std::string& id) {
  return IdIs(id, {"sb.xml.attrs", "xml.attrs", "SBSQL-5F496C39F6E8"});
}

bool IsXmlFunctionId(const std::string& id) {
  return id.rfind("sb.xml.", 0) == 0 || IsXmlForestId(id) || IsXmlCastId(id) || IsXmlExistsId(id) ||
         IsXmlAttributesId(id) || IsXmlConcatId(id) || IsXmlCommentId(id) || IsXmlPiId(id) ||
         IsXmlRootId(id) || IsXmlElementId(id) || IsXmlAggId(id) || IsXmlTableId(id) ||
         IsXmlDocumentId(id) || IsXmlNamespacesId(id) || IsXmlParseId(id) || IsXmlQueryId(id) ||
         IsXmlSerializeId(id) || IsXmlTextId(id) || IsXmlValidateId(id) || IsXmlNsId(id) ||
         IsXmlAttrsId(id);
}

bool AppendBounded(std::string* out, std::string_view value);

bool IsJsonTableId(const std::string& id) {
  return IdIs(id, {"sb.json.table", "JSON_TABLE", "json_table",
                   "JSON_TABLE(doc,path[PASSING...]COLUMNS(...)[ONERROR])",
                   "JSON_TABLE(document,jsonpathCOLUMNS(...))",
                   "SBSQL-E4C08DADB61A", "SBSQL-433AC9801679", "SBSQL-22963E18DC40"});
}

bool IsJsonArrayToJsonId(const std::string& id) {
  return IdIs(id, {"sb.json.array_to_json", "array_to_json", "array_to_json(array[,pretty])",
                   "SBSQL-2866302407B6", "SBSQL-579AE2ED91B2"});
}

bool IsJsonRowToJsonId(const std::string& id) {
  return IdIs(id, {"sb.json.row_to_json", "row_to_json", "row_to_json(row[,pretty])",
                   "SBSQL-EA3286F7FED5", "SBSQL-1E99FF5633C4"});
}

bool IsJsonbAggId(const std::string& id) {
  return IdIs(id, {"sb.json.jsonb_agg", "jsonb_agg", "jsonb_agg(expr)",
                   "SBSQL-5F35CBE51FA4", "SBSQL-F9F64D586108"});
}

std::string JsonTableDescriptor(const FunctionCallRequest& request, bool* ok) {
  if (request.arguments.empty() || request.arguments.size() > kXmlMaxArguments) {
    *ok = false;
    return {};
  }
  const std::string document = ValueAsText(request.arguments[0].value);
  const std::string path = request.arguments.size() > 1 ? ValueAsText(request.arguments[1].value) : "$";
  std::size_t column_count = 0;
  std::size_t passing_count = 0;
  for (std::size_t i = 2; i < request.arguments.size(); ++i) {
    const std::string name = LowerAscii(Trim(request.arguments[i].name));
    const std::string value = LowerAscii(Trim(ValueAsText(request.arguments[i].value)));
    if (name.find("column") != std::string::npos || value.find("column") != std::string::npos) {
      ++column_count;
    } else {
      ++passing_count;
    }
  }
  std::string out;
  *ok =
      AppendBounded(&out, "{\"function\":\"JSON_TABLE\",\"document_descriptor\":") &&
      AppendBounded(&out, JsonEscape(request.arguments[0].value.descriptor_id)) &&
      AppendBounded(&out, ",\"path\":") &&
      AppendBounded(&out, JsonEscape(path)) &&
      AppendBounded(&out, ",\"document_bytes\":") &&
      AppendBounded(&out, std::to_string(document.size())) &&
      AppendBounded(&out, ",\"document_nonempty\":") &&
      AppendBounded(&out, Trim(document).empty() ? "false" : "true") &&
      AppendBounded(&out, ",\"passing_argument_count\":") &&
      AppendBounded(&out, std::to_string(passing_count)) &&
      AppendBounded(&out, ",\"column_count\":") &&
      AppendBounded(&out, std::to_string(column_count)) &&
      AppendBounded(&out, ",\"result\":\"descriptor\"}");
  return out;
}

bool XmlArgumentsWithinBounds(const FunctionCallRequest& request, std::string* detail) {
  if (request.arguments.size() > kXmlMaxArguments) {
    *detail = "xml scalar helper argument count exceeds bounded in-core limit";
    return false;
  }
  for (const auto& argument : request.arguments) {
    const std::string text = ValueAsText(argument.value);
    if (text.size() > kXmlMaxScalarBytes) {
      *detail = "xml scalar helper input exceeds bounded in-core byte limit";
      return false;
    }
  }
  return true;
}

bool AppendBounded(std::string* out, std::string_view value) {
  if (out->size() > kXmlMaxScalarBytes || value.size() > kXmlMaxScalarBytes - out->size()) return false;
  out->append(value.data(), value.size());
  return true;
}

bool XmlTextContentIsSafe(const std::string& value) {
  for (const unsigned char ch : value) {
    if (ch < 0x20 && ch != '\t' && ch != '\n' && ch != '\r') return false;
  }
  return true;
}

bool AppendXmlEscaped(std::string* out, const std::string& value, bool attribute) {
  if (!XmlTextContentIsSafe(value)) return false;
  for (const char ch : value) {
    switch (ch) {
      case '&':
        if (!AppendBounded(out, "&amp;")) return false;
        break;
      case '<':
        if (!AppendBounded(out, "&lt;")) return false;
        break;
      case '>':
        if (!AppendBounded(out, "&gt;")) return false;
        break;
      case '"':
        if (!AppendBounded(out, attribute ? "&quot;" : "\"")) return false;
        break;
      case '\'':
        if (!AppendBounded(out, attribute ? "&apos;" : "'")) return false;
        break;
      default:
        if (!AppendBounded(out, std::string_view(&ch, 1))) return false;
        break;
    }
  }
  return true;
}

bool IsXmlDescriptor(const std::string& descriptor_id) {
  const std::string normalized = LowerAscii(descriptor_id);
  return normalized == "xml" || normalized == "xml_document";
}

bool AppendXmlValueContent(std::string* out, const scratchbird::engine::sblr::SblrValue& value) {
  const std::string text = ValueAsText(value);
  if (IsXmlDescriptor(value.descriptor_id)) {
    return XmlTextContentIsSafe(text) && AppendBounded(out, text);
  }
  return AppendXmlEscaped(out, text, false);
}

bool IsXmlNameStartChar(char ch) {
  const unsigned char uch = static_cast<unsigned char>(ch);
  return std::isalpha(uch) || ch == '_' || ch == ':';
}

bool IsXmlNameChar(char ch) {
  const unsigned char uch = static_cast<unsigned char>(ch);
  return IsXmlNameStartChar(ch) || std::isdigit(uch) || ch == '-' || ch == '.';
}

bool IsValidXmlName(const std::string& name) {
  if (name.empty() || name.size() > 256 || !IsXmlNameStartChar(name.front())) return false;
  for (const char ch : name) {
    if (!IsXmlNameChar(ch)) return false;
  }
  return true;
}

std::string XmlNameForArgument(const FunctionArgument& argument, std::size_t index, const std::string& prefix) {
  std::string name = Trim(argument.name);
  if (name.empty()) name = prefix + std::to_string(index + 1);
  return name;
}

bool AppendXmlAttributePair(std::string* out, const std::string& name, const scratchbird::engine::sblr::SblrValue& value) {
  if (!IsValidXmlName(name)) return false;
  if (!AppendBounded(out, " ")) return false;
  if (!AppendBounded(out, name)) return false;
  if (!AppendBounded(out, "=\"")) return false;
  if (!AppendXmlEscaped(out, ValueAsText(value), true)) return false;
  return AppendBounded(out, "\"");
}

bool IsRecognizedXmlEntityAt(const std::string& value, std::size_t cursor) {
  return value.compare(cursor, 5, "&amp;") == 0 || value.compare(cursor, 4, "&lt;") == 0 ||
         value.compare(cursor, 4, "&gt;") == 0 || value.compare(cursor, 6, "&quot;") == 0 ||
         value.compare(cursor, 6, "&apos;") == 0;
}

bool IsSafeXmlAttributeFragment(const std::string& fragment) {
  std::size_t cursor = 0;
  bool saw_attribute = false;
  while (cursor < fragment.size()) {
    while (cursor < fragment.size() && std::isspace(static_cast<unsigned char>(fragment[cursor]))) ++cursor;
    if (cursor >= fragment.size()) break;
    if (!IsXmlNameStartChar(fragment[cursor])) return false;
    ++cursor;
    while (cursor < fragment.size() && IsXmlNameChar(fragment[cursor])) ++cursor;
    while (cursor < fragment.size() && std::isspace(static_cast<unsigned char>(fragment[cursor]))) ++cursor;
    if (cursor >= fragment.size() || fragment[cursor] != '=') return false;
    ++cursor;
    while (cursor < fragment.size() && std::isspace(static_cast<unsigned char>(fragment[cursor]))) ++cursor;
    if (cursor >= fragment.size() || fragment[cursor] != '"') return false;
    ++cursor;
    while (cursor < fragment.size() && fragment[cursor] != '"') {
      const char ch = fragment[cursor];
      if (ch == '<' || ch == '>' || (static_cast<unsigned char>(ch) < 0x20 && ch != '\t' && ch != '\n' && ch != '\r')) {
        return false;
      }
      if (ch == '&' && !IsRecognizedXmlEntityAt(fragment, cursor)) return false;
      ++cursor;
    }
    if (cursor >= fragment.size() || fragment[cursor] != '"') return false;
    ++cursor;
    saw_attribute = true;
  }
  return saw_attribute;
}

std::string XmlPathTerminalToken(std::string query) {
  query = Trim(std::move(query));
  if (query == "$") return query;
  if (query.size() >= 2 && ((query.front() == '"' && query.back() == '"') ||
                            (query.front() == '\'' && query.back() == '\''))) {
    query = query.substr(1, query.size() - 2);
  }
  std::size_t end = query.size();
  while (end > 0 && !IsXmlNameChar(query[end - 1])) --end;
  std::size_t start = end;
  while (start > 0 && IsXmlNameChar(query[start - 1])) --start;
  if (start < end) return query.substr(start, end - start);
  return query;
}

std::optional<std::string> XmlStandaloneValue(const scratchbird::engine::sblr::SblrValue& value) {
  const std::string normalized = LowerAscii(Trim(ValueAsText(value)));
  if (normalized == "yes" || normalized == "true" || normalized == "1") return std::string("yes");
  if (normalized == "no" || normalized == "false" || normalized == "0") return std::string("no");
  return std::nullopt;
}

bool IsSafeXmlDeclarationValue(const std::string& value) {
  if (value.empty() || value.size() > 32) return false;
  for (const char ch : value) {
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (!std::isalnum(uch) && ch != '.' && ch != '_' && ch != '-') return false;
  }
  return true;
}

std::optional<std::size_t> FindXmlTagEnd(const std::string& value, std::size_t cursor) {
  char quote = '\0';
  for (std::size_t i = cursor; i < value.size(); ++i) {
    const char ch = value[i];
    if (quote != '\0') {
      if (ch == quote) quote = '\0';
      continue;
    }
    if (ch == '"' || ch == '\'') {
      quote = ch;
      continue;
    }
    if (ch == '>') return i;
  }
  return std::nullopt;
}

bool XmlEntityRefsAreSafe(const std::string& value) {
  for (std::size_t cursor = 0; cursor < value.size(); ++cursor) {
    if (value[cursor] == '&' && !IsRecognizedXmlEntityAt(value, cursor)) return false;
  }
  return true;
}

bool XmlTextSpanIsSafe(const std::string& value) {
  return XmlTextContentIsSafe(value) && XmlEntityRefsAreSafe(value);
}

bool XmlMarkupLooksWellFormed(const std::string& input,
                              bool require_document_root,
                              std::string* root_name,
                              std::string* detail) {
  const std::string value = Trim(input);
  if (value.empty()) {
    *detail = "xml input must not be empty";
    return false;
  }
  if (!XmlTextContentIsSafe(value)) {
    *detail = "xml input contains unsafe control characters";
    return false;
  }
  if (value.find("<!DOCTYPE") != std::string::npos || value.find("<!ENTITY") != std::string::npos ||
      value.find("<!doctype") != std::string::npos || value.find("<!entity") != std::string::npos) {
    *detail = "xml input cannot contain DTD or entity declarations";
    return false;
  }

  std::vector<std::string> stack;
  std::size_t root_count = 0;
  bool inside_root = false;
  for (std::size_t cursor = 0; cursor < value.size();) {
    const std::size_t tag = value.find('<', cursor);
    const std::string text = tag == std::string::npos ? value.substr(cursor) : value.substr(cursor, tag - cursor);
    if (!XmlTextSpanIsSafe(text)) {
      *detail = "xml text content contains unsafe entity references";
      return false;
    }
    if (require_document_root && !inside_root && !Trim(text).empty()) {
      *detail = "xml document text cannot appear outside the root element";
      return false;
    }
    if (tag == std::string::npos) break;
    if (value.compare(tag, 4, "<!--") == 0) {
      const auto end = value.find("-->", tag + 4);
      if (end == std::string::npos) {
        *detail = "xml comment is not closed";
        return false;
      }
      cursor = end + 3;
      continue;
    }
    if (value.compare(tag, 2, "<?") == 0) {
      const auto end = value.find("?>", tag + 2);
      if (end == std::string::npos) {
        *detail = "xml processing instruction is not closed";
        return false;
      }
      cursor = end + 2;
      continue;
    }
    const auto tag_end = FindXmlTagEnd(value, tag + 1);
    if (!tag_end.has_value()) {
      *detail = "xml tag is not closed";
      return false;
    }
    if (tag + 1 >= *tag_end) {
      *detail = "xml tag is empty";
      return false;
    }
    if (value[tag + 1] == '/') {
      std::size_t name_begin = tag + 2;
      std::size_t name_end = name_begin;
      while (name_end < *tag_end && IsXmlNameChar(value[name_end])) ++name_end;
      const std::string name = value.substr(name_begin, name_end - name_begin);
      if (!IsValidXmlName(name) || stack.empty() || stack.back() != name) {
        *detail = "xml closing tag does not match the open element stack";
        return false;
      }
      while (name_end < *tag_end && std::isspace(static_cast<unsigned char>(value[name_end]))) ++name_end;
      if (name_end != *tag_end) {
        *detail = "xml closing tag contains unexpected content";
        return false;
      }
      stack.pop_back();
      inside_root = !stack.empty();
      cursor = *tag_end + 1;
      continue;
    }

    std::size_t name_begin = tag + 1;
    std::size_t name_end = name_begin;
    while (name_end < *tag_end && IsXmlNameChar(value[name_end])) ++name_end;
    const std::string name = value.substr(name_begin, name_end - name_begin);
    if (!IsValidXmlName(name)) {
      *detail = "xml element name is not valid";
      return false;
    }
    std::string attrs = Trim(value.substr(name_end, *tag_end - name_end));
    bool self_closing = false;
    if (!attrs.empty() && attrs.back() == '/') {
      self_closing = true;
      attrs = Trim(attrs.substr(0, attrs.size() - 1));
    }
    if (!attrs.empty() && !IsSafeXmlAttributeFragment(attrs)) {
      *detail = "xml attribute fragment is not safe";
      return false;
    }
    if (stack.empty()) {
      ++root_count;
      if (root_name != nullptr && root_name->empty()) *root_name = name;
      if (require_document_root && root_count > 1) {
        *detail = "xml document must contain one root element";
        return false;
      }
    }
    if (!self_closing) {
      stack.push_back(name);
      if (stack.size() > 128) {
        *detail = "xml nesting exceeds bounded in-core depth";
        return false;
      }
      inside_root = true;
    }
    cursor = *tag_end + 1;
  }
  if (!stack.empty()) {
    *detail = "xml document has unclosed elements";
    return false;
  }
  if (require_document_root && root_count != 1) {
    *detail = "xml document must contain exactly one root element";
    return false;
  }
  return true;
}

bool XmlModeIsContent(std::string mode) {
  mode = LowerAscii(Trim(std::move(mode)));
  return mode == "content" || mode == "sequence";
}

std::string XmlFindByTagName(const std::string& document, const std::string& tag_name) {
  if (!IsValidXmlName(tag_name)) return {};
  const std::string open_prefix = "<" + tag_name;
  const std::string close = "</" + tag_name + ">";
  std::size_t pos = 0;
  while ((pos = document.find(open_prefix, pos)) != std::string::npos) {
    const std::size_t after_name = pos + open_prefix.size();
    if (after_name < document.size() &&
        !std::isspace(static_cast<unsigned char>(document[after_name])) &&
        document[after_name] != '>' && document[after_name] != '/') {
      pos = after_name;
      continue;
    }
    const auto tag_end = FindXmlTagEnd(document, after_name);
    if (!tag_end.has_value()) return {};
    if (*tag_end > pos && document[*tag_end - 1] == '/') return document.substr(pos, *tag_end - pos + 1);
    const auto close_pos = document.find(close, *tag_end + 1);
    if (close_pos == std::string::npos) return {};
    return document.substr(pos, close_pos + close.size() - pos);
  }
  return {};
}

std::string XmlQuerySubset(const std::string& query, const std::string& document) {
  const std::string normalized_query = Trim(query);
  std::string root_name;
  std::string detail;
  if (!XmlMarkupLooksWellFormed(document, true, &root_name, &detail)) return {};
  if (normalized_query == "$" || normalized_query == "/" || normalized_query == root_name ||
      normalized_query == "/" + root_name) {
    return document;
  }
  if (StartsWith(normalized_query, "//")) return XmlFindByTagName(document, normalized_query.substr(2));
  if (StartsWith(normalized_query, "/")) return XmlFindByTagName(document, normalized_query.substr(1));
  return XmlFindByTagName(document, normalized_query);
}

bool AppendXmlNamespacePair(std::string* out, const std::string& prefix, const std::string& uri) {
  if (!XmlTextContentIsSafe(uri) || uri.find('<') != std::string::npos ||
      uri.find('>') != std::string::npos || !XmlEntityRefsAreSafe(uri)) {
    return false;
  }
  if (prefix.empty() || LowerAscii(prefix) == "default") {
    if (!AppendBounded(out, " xmlns=\"")) return false;
  } else {
    if (!IsValidXmlName(prefix) || prefix.find(':') != std::string::npos ||
        LowerAscii(prefix) == "xml") {
      return false;
    }
    if (!AppendBounded(out, " xmlns:") || !AppendBounded(out, prefix) || !AppendBounded(out, "=\"")) return false;
  }
  return AppendXmlEscaped(out, uri, true) && AppendBounded(out, "\"");
}

FunctionCallResult DispatchXmlFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  std::string bounds_detail;
  if (!XmlArgumentsWithinBounds(request, &bounds_detail)) return RefuseFunctionInvalidInput(request, std::move(bounds_detail));

  if (IsXmlDocumentId(id) || IsXmlParseId(id) || IsXmlValidateId(id)) {
    if (request.arguments.empty()) {
      return RefuseFunctionInvalidInput(
          request, IsXmlDocumentId(id) ? "xmldocument expects an XML expression"
                                       : (IsXmlParseId(id) ? "xmlparse expects an XML expression"
                                                          : "xmlvalidate expects an XML expression"));
    }
    if (request.arguments.size() > 4) {
      return RefuseFunctionInvalidInput(request, "xml document helper expects at most four bounded arguments");
    }
    std::size_t document_index = 0;
    bool require_document_root = true;
    if (request.arguments.size() >= 2) {
      const std::string mode = LowerAscii(Trim(ValueAsText(request.arguments[0].value)));
      if (mode == "document" || mode == "content" || mode == "sequence") {
        document_index = 1;
        require_document_root = !XmlModeIsContent(mode);
      }
    }
    if (document_index >= request.arguments.size()) {
      return RefuseFunctionInvalidInput(request, "xml document helper mode must be followed by an XML expression");
    }
    if (IsSqlNull(request.arguments[document_index].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("xml_document")});
    }
    const std::string text = Trim(ValueAsText(request.arguments[document_index].value));
    std::string detail;
    std::string root;
    if (!XmlMarkupLooksWellFormed(text, require_document_root, &root, &detail)) {
      return RefuseFunctionInvalidInput(request, detail);
    }
    return MakeFunctionSuccess(request, {MakeTextValue("xml_document", text)});
  }

  if (IsXmlTextId(id)) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "xmltext expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("xml")});
    std::string out;
    if (!AppendXmlEscaped(&out, ValueAsText(request.arguments[0].value), false)) {
      return RefuseFunctionInvalidInput(request, "xmltext input is unsafe or output exceeds bounded limit");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("xml", out)});
  }

  if (IsXmlSerializeId(id)) {
    if (request.arguments.empty() || request.arguments.size() > 4) {
      return RefuseFunctionInvalidInput(request, "xmlserialize expects an XML expression and optional mode/type arguments");
    }
    std::size_t document_index = 0;
    if (request.arguments.size() >= 2) {
      const std::string mode = LowerAscii(Trim(ValueAsText(request.arguments[0].value)));
      if (mode == "document" || mode == "content" || mode == "sequence") document_index = 1;
    }
    if (document_index >= request.arguments.size()) {
      return RefuseFunctionInvalidInput(request, "xmlserialize mode must be followed by an XML expression");
    }
    if (IsSqlNull(request.arguments[document_index].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    const std::string text = ValueAsText(request.arguments[document_index].value);
    if (!XmlTextContentIsSafe(text)) return RefuseFunctionInvalidInput(request, "xmlserialize input contains unsafe XML content");
    return MakeFunctionSuccess(request, {MakeTextValue("character", text)});
  }

  if (IsXmlQueryId(id)) {
    if (request.arguments.size() < 2 || request.arguments.size() > 5) {
      return RefuseFunctionInvalidInput(request, "xmlquery expects query and XML document arguments");
    }
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) {
      return MakeFunctionSuccess(request, {MakeNullValue("xml_document")});
    }
    const std::string query = ValueAsText(request.arguments[0].value);
    const std::string document = Trim(ValueAsText(request.arguments[1].value));
    std::string detail;
    std::string root;
    if (!XmlMarkupLooksWellFormed(document, true, &root, &detail)) {
      return RefuseFunctionInvalidInput(request, detail);
    }
    const std::string match = XmlQuerySubset(query, document);
    return MakeFunctionSuccess(request, {match.empty() ? MakeNullValue("xml_document")
                                                       : MakeTextValue("xml_document", match)});
  }

  if (IsXmlNamespacesId(id) || IsXmlNsId(id)) {
    if (request.arguments.empty() || (request.arguments.size() % 2) != 0) {
      return RefuseFunctionInvalidInput(request, "xmlnamespaces expects prefix/uri pairs");
    }
    std::string out;
    bool saw_value = false;
    for (std::size_t i = 0; i < request.arguments.size(); i += 2) {
      if (IsSqlNull(request.arguments[i].value) || IsSqlNull(request.arguments[i + 1].value)) continue;
      const std::string prefix = Trim(ValueAsText(request.arguments[i].value));
      const std::string uri = ValueAsText(request.arguments[i + 1].value);
      if (!AppendXmlNamespacePair(&out, prefix, uri)) {
        return RefuseFunctionInvalidInput(request, "xml namespace declaration is not safe");
      }
      saw_value = true;
    }
    return MakeFunctionSuccess(request, {saw_value ? MakeTextValue("xml", out) : MakeNullValue("xml")});
  }

  if (IsXmlAttrsId(id)) {
    if (request.arguments.empty() || (request.arguments.size() % 2) != 0) {
      return RefuseFunctionInvalidInput(request, "xml.attrs expects name/value pairs");
    }
    std::string out;
    bool saw_value = false;
    for (std::size_t i = 0; i < request.arguments.size(); i += 2) {
      if (IsSqlNull(request.arguments[i].value) || IsSqlNull(request.arguments[i + 1].value)) continue;
      const std::string name = Trim(ValueAsText(request.arguments[i].value));
      if (!AppendXmlAttributePair(&out, name, request.arguments[i + 1].value)) {
        return RefuseFunctionInvalidInput(request, "xml.attrs name/value is invalid or output exceeds bounded limit");
      }
      saw_value = true;
    }
    return MakeFunctionSuccess(request, {saw_value ? MakeTextValue("xml", out) : MakeNullValue("xml")});
  }

  if (IsXmlForestId(id)) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "xmlforest expects at least one expression");
    std::string out;
    bool saw_value = false;
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
      const auto& argument = request.arguments[i];
      if (IsSqlNull(argument.value)) continue;
      const std::string name = XmlNameForArgument(argument, i, "expr");
      if (!IsValidXmlName(name)) return RefuseFunctionInvalidInput(request, "xmlforest expression name is not a valid XML name");
      if (!AppendBounded(&out, "<") || !AppendBounded(&out, name) || !AppendBounded(&out, ">") ||
          !AppendXmlValueContent(&out, argument.value) || !AppendBounded(&out, "</") ||
          !AppendBounded(&out, name) || !AppendBounded(&out, ">")) {
        return RefuseFunctionInvalidInput(request, "xmlforest output exceeds bounded in-core byte limit or contains unsafe content");
      }
      saw_value = true;
    }
    return MakeFunctionSuccess(request, {saw_value ? MakeTextValue("xml_document", out) : MakeNullValue("xml_document")});
  }

  if (IsXmlCastId(id)) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "xmlcast expects expression and optional target type descriptor");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("xml_document")});
    const std::string text = ValueAsText(request.arguments[0].value);
    if (!XmlTextContentIsSafe(text)) return RefuseFunctionInvalidInput(request, "xmlcast input contains unsafe XML content");
    return MakeFunctionSuccess(request, {MakeTextValue("xml_document", text)});
  }

  if (IsXmlExistsId(id)) {
    if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, "xmlexists expects query and document arguments");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    std::string document;
    for (std::size_t i = 1; i < request.arguments.size(); ++i) {
      if (IsSqlNull(request.arguments[i].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
      if (!AppendBounded(&document, ValueAsText(request.arguments[i].value))) {
        return RefuseFunctionInvalidInput(request, "xmlexists document input exceeds bounded in-core byte limit");
      }
    }
    const std::string query = Trim(ValueAsText(request.arguments[0].value));
    const std::string token = XmlPathTerminalToken(query);
    const bool exists = token == "$" ? !Trim(document).empty()
                                     : (document.find(query) != std::string::npos ||
                                        (!token.empty() && document.find(token) != std::string::npos));
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", exists ? 1 : 0)});
  }

  if (IsXmlAttributesId(id)) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "xmlattributes expects at least one expression");
    std::string out;
    bool saw_value = false;
    for (std::size_t i = 0; i < request.arguments.size(); ++i) {
      const auto& argument = request.arguments[i];
      if (IsSqlNull(argument.value)) continue;
      const std::string name = XmlNameForArgument(argument, i, "attr");
      if (!AppendXmlAttributePair(&out, name, argument.value)) {
        return RefuseFunctionInvalidInput(request, "xmlattributes name/value is invalid or output exceeds bounded limit");
      }
      saw_value = true;
    }
    return MakeFunctionSuccess(request, {saw_value ? MakeTextValue("xml", out) : MakeNullValue("xml")});
  }

  if (IsXmlConcatId(id) || IsXmlAggId(id)) {
    if (request.arguments.empty()) {
      return RefuseFunctionInvalidInput(request, IsXmlConcatId(id) ? "xmlconcat expects at least one expression"
                                                                  : "xmlagg expects at least one expression");
    }
    std::string out;
    bool saw_value = false;
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) continue;
      if (!AppendXmlValueContent(&out, argument.value)) {
        return RefuseFunctionInvalidInput(request, "xml concatenation output exceeds bounded limit or contains unsafe content");
      }
      saw_value = true;
    }
    return MakeFunctionSuccess(request, {saw_value ? MakeTextValue("xml_document", out) : MakeNullValue("xml_document")});
  }

  if (IsXmlCommentId(id)) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "xmlcomment expects exactly one text argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("xml")});
    const std::string text = ValueAsText(request.arguments[0].value);
    if (!XmlTextContentIsSafe(text) || text.find("--") != std::string::npos) {
      return RefuseFunctionInvalidInput(request, "xmlcomment text contains unsafe XML comment content");
    }
    std::string out;
    if (!AppendBounded(&out, "<!--") || !AppendBounded(&out, text) || !AppendBounded(&out, "-->")) {
      return RefuseFunctionInvalidInput(request, "xmlcomment output exceeds bounded in-core byte limit");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("xml", out)});
  }

  if (IsXmlPiId(id)) {
    if (request.arguments.size() != 1 && request.arguments.size() != 2) {
      return RefuseFunctionInvalidInput(request, "xmlpi expects target and optional content");
    }
    if (IsSqlNull(request.arguments[0].value)) return RefuseFunctionInvalidInput(request, "xmlpi target is required");
    const std::string target = Trim(ValueAsText(request.arguments[0].value));
    if (!IsValidXmlName(target) || LowerAscii(target) == "xml") return RefuseFunctionInvalidInput(request, "xmlpi target is not a valid XML PI target");
    std::string content;
    if (request.arguments.size() == 2 && !IsSqlNull(request.arguments[1].value)) content = ValueAsText(request.arguments[1].value);
    if (!XmlTextContentIsSafe(content) || content.find("?>") != std::string::npos) {
      return RefuseFunctionInvalidInput(request, "xmlpi content contains unsafe XML PI content");
    }
    std::string out;
    if (!AppendBounded(&out, "<?") || !AppendBounded(&out, target) ||
        (!content.empty() && (!AppendBounded(&out, " ") || !AppendBounded(&out, content))) ||
        !AppendBounded(&out, "?>")) {
      return RefuseFunctionInvalidInput(request, "xmlpi output exceeds bounded in-core byte limit");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("xml", out)});
  }

  if (IsXmlRootId(id)) {
    if (request.arguments.empty() || request.arguments.size() > 3) {
      return RefuseFunctionInvalidInput(request, "xmlroot expects expression, optional version, and optional standalone flag");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("xml_document")});
    std::string version = "1.0";
    if (request.arguments.size() >= 2) {
      if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("xml_document")});
      version = Trim(ValueAsText(request.arguments[1].value));
    }
    if (!IsSafeXmlDeclarationValue(version)) return RefuseFunctionInvalidInput(request, "xmlroot version is not safe for XML declaration");
    std::optional<std::string> standalone;
    if (request.arguments.size() == 3) {
      if (IsSqlNull(request.arguments[2].value)) return MakeFunctionSuccess(request, {MakeNullValue("xml_document")});
      standalone = XmlStandaloneValue(request.arguments[2].value);
      if (!standalone.has_value()) return RefuseFunctionInvalidInput(request, "xmlroot standalone flag must be yes/no or boolean-compatible");
    }
    std::string out;
    if (!AppendBounded(&out, "<?xml version=\"") || !AppendBounded(&out, version) || !AppendBounded(&out, "\"") ||
        (standalone.has_value() && (!AppendBounded(&out, " standalone=\"") || !AppendBounded(&out, *standalone) ||
                                    !AppendBounded(&out, "\""))) ||
        !AppendBounded(&out, "?>") || !AppendXmlValueContent(&out, request.arguments[0].value)) {
      return RefuseFunctionInvalidInput(request, "xmlroot output exceeds bounded limit or contains unsafe content");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("xml_document", out)});
  }

  if (IsXmlElementId(id)) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "xmlelement expects an element name");
    if (IsSqlNull(request.arguments[0].value)) return RefuseFunctionInvalidInput(request, "xmlelement name is required");
    const std::string element_name = Trim(ValueAsText(request.arguments[0].value));
    if (!IsValidXmlName(element_name)) return RefuseFunctionInvalidInput(request, "xmlelement name is not a valid XML name");
    std::string attributes;
    std::string content;
    bool saw_content = false;
    for (std::size_t i = 1; i < request.arguments.size(); ++i) {
      const auto& argument = request.arguments[i];
      if (IsSqlNull(argument.value)) continue;
      const std::string text = ValueAsText(argument.value);
      const std::string argument_name = LowerAscii(Trim(argument.name));
      const bool named_attribute_fragment = !saw_content && (argument_name == "attrs" || argument_name == "attributes");
      const bool inferred_attribute_fragment = !saw_content && StartsWith(text, " ") && IsSafeXmlAttributeFragment(text);
      if (named_attribute_fragment || inferred_attribute_fragment) {
        if (!IsSafeXmlAttributeFragment(text)) {
          return RefuseFunctionInvalidInput(request, "xmlelement attribute fragment is not a safe XML attribute fragment");
        }
        if (!AppendBounded(&attributes, text)) {
          return RefuseFunctionInvalidInput(request, "xmlelement attribute fragment exceeds bounded in-core byte limit");
        }
        continue;
      }
      saw_content = true;
      if (!AppendXmlValueContent(&content, argument.value)) {
        return RefuseFunctionInvalidInput(request, "xmlelement content exceeds bounded limit or contains unsafe content");
      }
    }
    std::string out;
    if (!AppendBounded(&out, "<") || !AppendBounded(&out, element_name) || !AppendBounded(&out, attributes)) {
      return RefuseFunctionInvalidInput(request, "xmlelement output exceeds bounded in-core byte limit");
    }
    if (content.empty()) {
      if (!AppendBounded(&out, "/>")) return RefuseFunctionInvalidInput(request, "xmlelement output exceeds bounded in-core byte limit");
    } else if (!AppendBounded(&out, ">") || !AppendBounded(&out, content) || !AppendBounded(&out, "</") ||
               !AppendBounded(&out, element_name) || !AppendBounded(&out, ">")) {
      return RefuseFunctionInvalidInput(request, "xmlelement output exceeds bounded in-core byte limit");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("xml_document", out)});
  }

  if (IsXmlTableId(id)) {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "xmltable expects at least a query expression");
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    }
    const std::string query = ValueAsText(request.arguments[0].value);
    const std::string document = request.arguments.size() > 1 ? ValueAsText(request.arguments[1].value) : std::string();
    std::string out;
    if (!AppendBounded(&out, "{\"function\":\"XMLTABLE\",\"query\":") ||
        !AppendBounded(&out, JsonEscape(query)) ||
        !AppendBounded(&out, ",\"document_descriptor\":") ||
        !AppendBounded(&out, request.arguments.size() > 1 ? JsonEscape(request.arguments[1].value.descriptor_id)
                                                          : JsonEscape(std::string())) ||
        !AppendBounded(&out, ",\"document_bytes\":") ||
        !AppendBounded(&out, std::to_string(document.size())) ||
        !AppendBounded(&out, ",\"document_nonempty\":") ||
        !AppendBounded(&out, Trim(document).empty() ? "false" : "true") ||
        !AppendBounded(&out, ",\"passing_argument_count\":") ||
        !AppendBounded(&out, std::to_string(request.arguments.size() > 1 ? request.arguments.size() - 1 : 0)) ||
        !AppendBounded(&out, ",\"result\":\"descriptor\"}")) {
      return RefuseFunctionInvalidInput(request, "xmltable descriptor output exceeds bounded in-core byte limit");
    }
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", out)});
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_XML_FUNCTION_UNHANDLED",
                                      "xml function id is not handled by the activated XML scalar surface");
}

}  // namespace

bool IsNoSqlDocumentFunction(const FunctionCallRequest& request) {
  return request.context.function_id.rfind("nosql.document.", 0) == 0 ||
         request.context.function_id.rfind("sb.fn.nosql.document.", 0) == 0 ||
         IsXmlFunctionId(request.context.function_id) ||
         // sb.json.* canonicals from the SBsql surface registry's
         // builtin-expression-registry binding flow through the nosql.document
         // family dispatch where the existing JsonTypeOf / ExtractJsonObjectField
         // helpers handle the implementation.
         request.context.function_id.rfind("sb.json.", 0) == 0 ||
         IsJsonTableId(request.context.function_id) ||
         IsJsonArrayToJsonId(request.context.function_id) ||
         IsJsonRowToJsonId(request.context.function_id) ||
         IsJsonbAggId(request.context.function_id);
}

FunctionCallResult DispatchNoSqlDocumentFunction(const FunctionCallRequest& request) {
  const auto& id = request.context.function_id;
  if (IsXmlFunctionId(id)) return DispatchXmlFunction(request);

  if (IsJsonTableId(id)) {
    if (request.arguments.empty() || request.arguments.size() > kXmlMaxArguments) {
      return RefuseFunctionInvalidInput(request, "json_table expects document, optional path, and bounded descriptor arguments");
    }
    for (const auto& argument : request.arguments) {
      if (IsSqlNull(argument.value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
      if (ValueAsText(argument.value).size() > kXmlMaxScalarBytes) {
        return RefuseFunctionInvalidInput(request, "json_table descriptor input exceeds bounded in-core byte limit");
      }
    }
    bool ok = false;
    const std::string descriptor = JsonTableDescriptor(request, &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "json_table descriptor output exceeds bounded in-core byte limit");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", descriptor)});
  }

  if (id == "nosql.document.validate" || id == "sb.fn.nosql.document.sb_json_valid" ||
      id == "sb.fn.nosql.document.sb_json_exists" || id == "JSON_VALID") {
    if (request.arguments.empty()) return RefuseFunctionInvalidInput(request, "json validation expects a document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", JsonLooksValid(ValueAsText(request.arguments[0].value)) ? 1 : 0)});
  }

  if (IdIs(id, {"nosql.document.type", "sb.fn.nosql.document.sb_json_type",
                "sb.json.typeof", "sb.json.jsonb_typeof", "JSON_TYPE", "json_type",
                "json_type(document)", "jsonb_typeof", "jsonb_typeof(document)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "json_type expects a document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", JsonTypeOf(ValueAsText(request.arguments[0].value)))});
  }

  if (IdIs(id, {"nosql.document.get", "nosql.document.query", "sb.fn.nosql.document.sb_json_get",
                "sb.json.extract", "sb.json.value", "sb.json.query", "JSON_EXTRACT", "JSON_VALUE",
                "JSON_QUERY", "json_value", "json_value(document,jsonpath...)",
                "JSON_VALUE(doc,path[PASSING...][RETURNING...][ONEMPTY][ONERROR])",
                "json_query", "json_query(document,jsonpath...)", "sb.json.jsonb_path_query",
                "sb.json.jsonb_path_query_first", "jsonb_path_query",
                "jsonb_path_query(document,jsonpath[,vars[,silent]])", "jsonb_path_query_first"})) {
    if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, "json_get expects document and path");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    const std::string extracted = ExtractJsonObjectField(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value));
    if (extracted.empty()) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    if (id == "sb.json.value" && NamedArgumentText(request, "quotes") == "omit" &&
        extracted.size() >= 2 && extracted.front() == '"' && extracted.back() == '"') {
      return MakeFunctionSuccess(request, {MakeTextValue("character", extracted.substr(1, extracted.size() - 2))});
    }
    if (id == "sb.json.query" && NamedArgumentText(request, "wrapper").find("wrapper") != std::string::npos) {
      return MakeFunctionSuccess(request, {MakeTextValue("json_document", "[" + extracted + "]")});
    }
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", extracted)});
  }

  if (IdIs(id, {"nosql.document.exists", "sb.fn.nosql.document.sb_json_path_exists", "sb.json.exists",
                "JSON_CONTAINS_PATH", "JSON_EXISTS", "JSON_EXISTS(doc,path[PASSING...][ONERROR])",
                "json_exists", "json_exists(document,jsonpath)", "sb.json.jsonb_path_exists",
                "jsonb_path_exists", "jsonb_path_exists(document,jsonpath[,vars])"})) {
    if (request.arguments.size() < 2) return RefuseFunctionInvalidInput(request, "json_exists expects document and path");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    const std::string extracted = ExtractJsonObjectField(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value));
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", extracted.empty() ? 0 : 1)});
  }

  if (IdIs(id, {"sb.json.jsonb_path_match", "jsonb_path_match", "jsonb_path_match(document,jsonpath[,vars])"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "jsonb_path_match expects document and path");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("boolean")});
    const std::string extracted = Trim(ExtractJsonObjectField(ValueAsText(request.arguments[0].value), ValueAsText(request.arguments[1].value)));
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", extracted == "true" ? 1 : 0)});
  }

  if (IdIs(id, {"sb.json.jsonb_path_query_array", "jsonb_path_query_array"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "jsonb_path_query_array expects document and path");
    if (IsSqlNull(request.arguments[0].value) || IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", JsonPathQueryArray(ValueAsText(request.arguments[0].value),
                                                                                           ValueAsText(request.arguments[1].value)))});
  }

  if (IdIs(id, {"nosql.document.put", "nosql.document.update", "sb.fn.nosql.document.sb_json_set",
                "sb.json.set", "sb.json.replace", "sb.json.insert", "sb.json.jsonb_set",
                "JSON_SET", "json_set", "json_set(document,path,value)",
                "json_replace", "json_replace(document,path,value)",
                "json_insert", "json_insert(document,path,value)",
                "jsonb_set", "jsonb_set(document,path,value[,create_missing])",
                "sb.json.jsonb_insert", "jsonb_insert", "jsonb_insert(document,path,value[,insert_after])"})) {
    const bool is_jsonb_set = IdIs(id, {"sb.json.jsonb_set", "jsonb_set", "jsonb_set(document,path,value[,create_missing])"});
    const bool is_jsonb_insert = IdIs(id, {"sb.json.jsonb_insert", "jsonb_insert", "jsonb_insert(document,path,value[,insert_after])"});
    const bool accepts_optional_fourth = is_jsonb_set || is_jsonb_insert;
    if ((!accepts_optional_fourth && request.arguments.size() != 3) ||
        (accepts_optional_fourth && request.arguments.size() != 3 && request.arguments.size() != 4)) {
      return RefuseFunctionInvalidInput(request, is_jsonb_set ? "jsonb_set expects document, path, value, and optional create_missing"
                                                             : "json_set expects document, path, and value");
    }
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    bool create_missing = true;
    if (is_jsonb_set && request.arguments.size() == 4) {
      if (IsSqlNull(request.arguments[3].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
      bool ok = false;
      create_missing = JsonControlBool(request.arguments[3].value, &ok);
      if (!ok) return RefuseFunctionInvalidInput(request, "jsonb_set create_missing must be boolean-compatible");
    }
    const std::string value = JsonLiteralFromValue(request.arguments[2].value);
    if (IdIs(id, {"sb.json.replace", "json_replace", "json_replace(document,path,value)"})) create_missing = false;
    const bool replace_existing = !IdIs(id, {"sb.json.insert", "json_insert", "json_insert(document,path,value)",
                                             "sb.json.jsonb_insert", "jsonb_insert",
                                             "jsonb_insert(document,path,value[,insert_after])"});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", MutateJsonObjectField(ValueAsText(request.arguments[0].value),
                                                                                              ValueAsText(request.arguments[1].value),
                                                                                              value,
                                                                                              create_missing,
                                                                                              replace_existing))});
  }

  if (IdIs(id, {"nosql.document.remove", "nosql.document.delete", "sb.fn.nosql.document.sb_json_remove",
                "sb.json.remove", "JSON_REMOVE", "json_remove", "json_remove(document,path)"})) {
    if (request.arguments.size() != 2) return RefuseFunctionInvalidInput(request, "json_remove expects document and path");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    if (IsSqlNull(request.arguments[1].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", RemoveJsonObjectField(ValueAsText(request.arguments[0].value),
                                                                                              ValueAsText(request.arguments[1].value)))});
  }

  if (IdIs(id, {"sb.json.array_length", "sb.json.jsonb_array_length", "json_array_length", "json_array_length(document)",
                "jsonb_array_length", "jsonb_array_length(document)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "json_array_length expects one array document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("uint64")});
    bool ok = false;
    const std::size_t length = JsonArrayLength(ValueAsText(request.arguments[0].value), &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "json_array_length expects a JSON array document");
    return MakeFunctionSuccess(request, {MakeUint64Value("uint64", static_cast<std::uint64_t>(length))});
  }

  if (IdIs(id, {"sb.json.build_array", "sb.json.jsonb_build_array", "json_build_array", "json_build_array(args...)",
                "jsonb_build_array"})) {
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", JsonBuildArray(request.arguments))});
  }

  if (IsJsonArrayToJsonId(id)) {
    const auto pretty = JsonOptionalPrettyFlag(request.arguments);
    if (!pretty.has_value()) return RefuseFunctionInvalidInput(request, "array_to_json pretty flag must be boolean-compatible");
    bool ok = false;
    const std::string document = JsonArrayToJson(request.arguments, *pretty, &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "array_to_json expects an array/document argument and optional pretty flag");
    if (document == "null") return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", document)});
  }

  if (IdIs(id, {"sb.json.build_object", "sb.json.jsonb_build_object", "json_build_object", "json_build_object(k1,v1,...)",
                "jsonb_build_object", "sb.json.object", "sb.json.object_text_array",
                "sb.json.jsonb_object", "json_object", "json_object(text[][,text[]])", "jsonb_object",
                "SBSQL-4DBBCD45F15C"})) {
    bool ok = false;
    const bool text_array_variant =
        !request.arguments.empty() && request.arguments.size() <= 2 &&
        JsonTypeOf(ValueAsText(request.arguments[0].value)) == "array";
    const std::string document = text_array_variant ? JsonObjectFromTextArrays(request.arguments, &ok)
                                                    : JsonBuildObject(request.arguments, &ok);
    if (!ok) {
      return RefuseFunctionInvalidInput(
          request,
          text_array_variant ? "json_object text-array form expects one even-length text array or matching key/value text arrays"
                             : "json_build_object expects an even number of key/value arguments");
    }
    if (document == "null") return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", document)});
  }

  if (IsJsonbAggId(id)) {
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", JsonAggArray(request.arguments))});
  }

  if (IdIs(id, {"sb.json.object_keys", "sb.json.jsonb_object_keys", "json_object_keys",
                "json_object_keys(document)", "jsonb_object_keys", "jsonb_object_keys(document)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "json_object_keys expects one object document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    bool ok = false;
    const std::string keys = JsonObjectKeysArray(ValueAsText(request.arguments[0].value), &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "json_object_keys expects a JSON object document");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", keys)});
  }

  if (IdIs(id, {"sb.json.array_elements", "json_array_elements", "json_array_elements(document)",
                "sb.json.array_elements_text", "json_array_elements_text"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "json_array_elements expects one array document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    bool ok = false;
    const bool text_mode = IdIs(id, {"sb.json.array_elements_text", "json_array_elements_text"});
    const std::string elements = JsonArrayElementsArray(ValueAsText(request.arguments[0].value), text_mode, &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "json_array_elements expects a JSON array document");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", elements)});
  }

  if (IdIs(id, {"sb.json.each", "sb.fn.nosql.document.sb_json_each", "json_each", "json_each(document)",
                "sb.json.each_text", "json_each_text", "json_each_text(document)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "json_each expects one object document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    bool ok = false;
    const bool text_mode = IdIs(id, {"sb.json.each_text", "json_each_text", "json_each_text(document)"});
    const std::string rows = JsonEachArray(ValueAsText(request.arguments[0].value), text_mode, &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "json_each expects a JSON object document");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", rows)});
  }

  if (IdIs(id, {"sb.json.jsonb_strip_nulls", "jsonb_strip_nulls", "jsonb_strip_nulls(document)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "jsonb_strip_nulls expects one object document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    bool ok = false;
    const std::string stripped = JsonStripNulls(ValueAsText(request.arguments[0].value), &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "jsonb_strip_nulls expects a JSON object document");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", stripped)});
  }

  if (IdIs(id, {"sb.json.jsonb_pretty", "jsonb_pretty", "jsonb_pretty(document)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "jsonb_pretty expects one document argument");
    if (IsSqlNull(request.arguments[0].value)) return MakeFunctionSuccess(request, {MakeNullValue("character")});
    return MakeFunctionSuccess(request, {MakeTextValue("character", JsonPretty(ValueAsText(request.arguments[0].value)))});
  }

  if (IsJsonRowToJsonId(id)) {
    const auto pretty = JsonOptionalPrettyFlag(request.arguments);
    if (!pretty.has_value()) return RefuseFunctionInvalidInput(request, "row_to_json pretty flag must be boolean-compatible");
    bool ok = false;
    const std::string document = JsonRowToJson(request.arguments, *pretty, &ok);
    if (!ok) return RefuseFunctionInvalidInput(request, "row_to_json expects a row/object argument and optional pretty flag");
    if (document == "null") return MakeFunctionSuccess(request, {MakeNullValue("json_document")});
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", document)});
  }

  if (IdIs(id, {"sb.json.to_json", "sb.json.to_jsonb", "to_json", "to_json(any)", "to_jsonb", "to_jsonb(any)"})) {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "to_json expects exactly one argument");
    return MakeFunctionSuccess(request, {MakeTextValue("json_document", JsonLiteralFromValue(request.arguments[0].value))});
  }

  if (id == "nosql.document.descriptor_accepts") {
    if (request.arguments.size() != 1) return RefuseFunctionInvalidInput(request, "document descriptor_accepts expects descriptor id");
    return MakeFunctionSuccess(request, {MakeInt64Value("boolean", DescriptorAcceptsDocument(ValueAsText(request.arguments[0].value)) ? 1 : 0)});
  }

  return RefuseFunctionWithDiagnostic(request,
                                      scratchbird::engine::sblr::SblrStatusCode::unsupported_feature,
                                      "SB_DIAG_NOSQL_DOCUMENT_FUNCTION_UNHANDLED",
                                      "document function id is not handled by the activated JSON scalar surface");
}

}  // namespace scratchbird::engine::functions
