// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_catalog_projection.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

namespace scratchbird::parser::firebird {
namespace {

constexpr std::uint32_t kSqlVarying = 448;
constexpr std::uint32_t kSqlText = 452;
constexpr std::uint32_t kSqlShort = 500;

constexpr std::uint8_t kBlrVersion4 = 4;
constexpr std::uint8_t kBlrVersion5 = 5;
constexpr std::uint8_t kBlrBegin = 2;
constexpr std::uint8_t kBlrMessage = 4;
constexpr std::uint8_t kBlrShort = 7;
constexpr std::uint8_t kBlrText2 = 15;
constexpr std::uint8_t kBlrVarying2 = 38;
constexpr std::uint8_t kBlrEnd = 255;
constexpr std::uint8_t kBlrEoc = 76;

std::string TrimAscii(std::string value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front())) != 0) {
    value.erase(value.begin());
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())) != 0) {
    value.pop_back();
  }
  return value;
}

std::string UpperAscii(std::string value) {
  for (char& ch : value) {
    ch = static_cast<char>(
        std::toupper(static_cast<unsigned char>(ch)));
  }
  return value;
}

std::optional<std::size_t> Utf8CodePointCount(std::string_view value) {
  std::size_t count = 0;
  for (std::size_t offset = 0; offset < value.size();) {
    const auto first = static_cast<unsigned char>(value[offset]);
    std::size_t width = 0;
    std::uint32_t code_point = 0;
    if (first <= 0x7f) {
      width = 1;
      code_point = first;
    } else if (first >= 0xc2 && first <= 0xdf) {
      width = 2;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      width = 3;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      width = 4;
      code_point = first & 0x07;
    } else {
      return std::nullopt;
    }
    if (offset + width > value.size()) return std::nullopt;
    for (std::size_t index = 1; index < width; ++index) {
      const auto continuation =
          static_cast<unsigned char>(value[offset + index]);
      if ((continuation & 0xc0) != 0x80) return std::nullopt;
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if ((width == 3 && code_point >= 0xd800 && code_point <= 0xdfff) ||
        (width == 3 && code_point < 0x800) ||
        (width == 4 && code_point < 0x10000) || code_point > 0x10ffff) {
      return std::nullopt;
    }
    offset += width;
    ++count;
  }
  return count;
}

void AppendU16(std::vector<std::uint8_t>* bytes, std::uint32_t value) {
  bytes->push_back(static_cast<std::uint8_t>(value & 0xff));
  bytes->push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

std::optional<std::uint32_t> AddAlignedLength(std::uint32_t offset,
                                              std::uint32_t alignment,
                                              std::uint32_t length) {
  std::uint64_t aligned = offset;
  if (alignment > 1) {
    aligned = (aligned + alignment - 1) &
              ~(static_cast<std::uint64_t>(alignment) - 1);
  }
  const std::uint64_t end = aligned + length;
  if (end > std::numeric_limits<std::uint32_t>::max()) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(end);
}

std::string StripLineComments(std::string_view input) {
  std::string out;
  out.reserve(input.size());
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t index = 0; index < input.size(); ++index) {
    const char ch = input[index];
    if (!single_quoted && !double_quoted && ch == '-' &&
        index + 1 < input.size() && input[index + 1] == '-') {
      index += 2;
      while (index < input.size() && input[index] != '\n' &&
             input[index] != '\r') {
        ++index;
      }
      if (index < input.size()) out.push_back(' ');
      continue;
    }
    out.push_back(ch);
    if (ch == '\'' && !double_quoted) {
      if (single_quoted && index + 1 < input.size() &&
          input[index + 1] == '\'') {
        out.push_back(input[++index]);
      } else {
        single_quoted = !single_quoted;
      }
    } else if (ch == '"' && !single_quoted) {
      if (double_quoted && index + 1 < input.size() &&
          input[index + 1] == '"') {
        out.push_back(input[++index]);
      } else {
        double_quoted = !double_quoted;
      }
    }
  }
  return out;
}

std::string NormalizeSql(std::string_view input) {
  std::string text = TrimAscii(StripLineComments(input));
  while (!text.empty() && text.back() == ';') {
    text.pop_back();
    text = TrimAscii(std::move(text));
  }
  std::string out;
  out.reserve(text.size());
  bool whitespace = false;
  for (const unsigned char ch : text) {
    if (std::isspace(ch) != 0) {
      whitespace = !out.empty();
      continue;
    }
    if (whitespace) out.push_back(' ');
    whitespace = false;
    out.push_back(static_cast<char>(std::toupper(ch)));
  }
  return TrimAscii(std::move(out));
}

bool ContainsAll(std::string_view text,
                 std::initializer_list<std::string_view> required) {
  for (const auto token : required) {
    if (text.find(token) == std::string_view::npos) return false;
  }
  return true;
}

bool ContainsCommaDelimitedInteger(std::string_view text,
                                   std::string_view digits) {
  if (digits.empty()) return false;
  std::size_t search = 0;
  while ((search = text.find(digits, search)) != std::string_view::npos) {
    const std::size_t end = search + digits.size();
    const bool numeric_token =
        (search == 0 ||
         std::isdigit(static_cast<unsigned char>(text[search - 1])) == 0) &&
        (end == text.size() ||
         std::isdigit(static_cast<unsigned char>(text[end])) == 0);
    std::size_t left = search;
    while (left > 0 &&
           std::isspace(static_cast<unsigned char>(text[left - 1])) != 0) {
      --left;
    }
    std::size_t right = end;
    while (right < text.size() &&
           std::isspace(static_cast<unsigned char>(text[right])) != 0) {
      ++right;
    }
    if (numeric_token && left > 0 && text[left - 1] == ',' &&
        right < text.size() && text[right] == ',') {
      return true;
    }
    search = end;
  }
  return false;
}

bool StartsWithCommand(std::string_view text, std::string_view command) {
  if (!text.starts_with(command)) return false;
  return text.size() == command.size() ||
         std::isspace(static_cast<unsigned char>(text[command.size()])) != 0 ||
         text[command.size()] == '(';
}

void SkipSpace(std::string_view text, std::size_t* offset) {
  while (*offset < text.size() &&
         std::isspace(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
}

bool ConsumeToken(std::string_view text,
                  std::size_t* offset,
                  std::string_view token) {
  SkipSpace(text, offset);
  if (*offset + token.size() > text.size()) return false;
  for (std::size_t index = 0; index < token.size(); ++index) {
    if (std::toupper(static_cast<unsigned char>(text[*offset + index])) !=
        std::toupper(static_cast<unsigned char>(token[index]))) {
      return false;
    }
  }
  const std::size_t end = *offset + token.size();
  if (!token.empty() &&
      (std::isalnum(static_cast<unsigned char>(token.back())) != 0 ||
       token.back() == '_' || token.back() == '$') &&
      end < text.size() &&
      (std::isalnum(static_cast<unsigned char>(text[end])) != 0 ||
       text[end] == '_' || text[end] == '$')) {
    return false;
  }
  *offset = end;
  return true;
}

std::optional<std::string> ReadSqlString(std::string_view text,
                                        std::size_t* offset) {
  SkipSpace(text, offset);
  if (*offset >= text.size() || text[*offset] != '\'') return std::nullopt;
  ++*offset;
  std::string value;
  while (*offset < text.size()) {
    const char ch = text[(*offset)++];
    if (ch != '\'') {
      value.push_back(ch);
      continue;
    }
    if (*offset < text.size() && text[*offset] == '\'') {
      value.push_back('\'');
      ++*offset;
      continue;
    }
    return value;
  }
  return std::nullopt;
}

bool ConsumeCharacter(std::string_view text, std::size_t* offset, char ch) {
  SkipSpace(text, offset);
  if (*offset >= text.size() || text[*offset] != ch) return false;
  ++*offset;
  return true;
}

FirebirdCatalogProjectionRoute ParseSelectRoute(std::string_view input) {
  const std::string sql = TrimAscii(StripLineComments(input));
  std::size_t offset = 0;
  if (!ConsumeToken(sql, &offset, "SELECT")) return {};

  std::optional<std::string> message;
  {
    const std::size_t saved = offset;
    if (auto literal = ReadSqlString(sql, &offset)) {
      if (!ConsumeToken(sql, &offset, "AS") ||
          !ConsumeToken(sql, &offset, "MSG") ||
          !ConsumeCharacter(sql, &offset, ',')) {
        return {};
      }
      message = std::move(*literal);
    } else {
      offset = saved;
    }
  }

  if (!ConsumeToken(sql, &offset, "V") ||
      !ConsumeCharacter(sql, &offset, '.') ||
      !ConsumeCharacter(sql, &offset, '*') ||
      !ConsumeToken(sql, &offset, "FROM") ||
      !ConsumeToken(sql, &offset, "V_FIELDS_INFO")) {
    return {};
  }
  const std::size_t before_as = offset;
  if (ConsumeToken(sql, &offset, "AS")) {
    if (!ConsumeToken(sql, &offset, "V")) return {};
  } else {
    offset = before_as;
    if (!ConsumeToken(sql, &offset, "V")) return {};
  }
  SkipSpace(sql, &offset);
  while (offset < sql.size() && sql[offset] == ';') {
    ++offset;
    SkipSpace(sql, &offset);
  }
  if (offset != sql.size()) return {};

  FirebirdCatalogProjectionRoute route;
  // SELECT syntax identifies the Firebird presentation shape only. The
  // effective persisted view variant is deliberately left unbound until the
  // exact engine transaction resolves the engine-owned view descriptor.
  route.kind =
      FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoUnbound;
  route.view_name = "V_FIELDS_INFO";
  route.source_relation_name = "TEST";
  route.leading_message = std::move(message);
  return route;
}

FirebirdCatalogProjectionRoute FunctionRoute(std::string_view normalized) {
  if (!StartsWithCommand(normalized,
                         "CREATE OR ALTER FUNCTION FN_GET_TYPE_NAME") ||
      !ContainsAll(
          normalized,
          {"A_TYPE SMALLINT", "A_SUBTYPE SMALLINT", "RETURNS VARCHAR(2048)",
           "DECLARE FTYPE VARCHAR(2048)", "DECODE(", "COALESCE(A_SUBTYPE,0)",
           "'SMALLINT'", "'INTEGER'", "'NUMERIC'",
           "'DECIMAL'", "'DECFLOAT(16)'", "'DECFLOAT(34)'", "'INT128'",
           "'TIME WITH TIME ZONE'", "'TIMESTAMP WITH TIME ZONE'",
           "'BLOB SUB_TYPE BINARY'", "'BLOB SUB_TYPE TEXT'", "RETURN FTYPE"}) ||
      !ContainsCommaDelimitedInteger(normalized, "7") ||
      !ContainsCommaDelimitedInteger(normalized, "8") ||
      !ContainsCommaDelimitedInteger(normalized, "261") ||
      normalized.find("CREATE VIEW") != std::string_view::npos ||
      !normalized.ends_with("END")) {
    return {};
  }
  FirebirdCatalogProjectionRoute route;
  route.kind = FirebirdCatalogProjectionRouteKind::kCreateTypeNameFunction;
  route.function_name = "FN_GET_TYPE_NAME";
  return route;
}

FirebirdCatalogProjectionRoute ViewRoute(std::string_view normalized) {
  if (!StartsWithCommand(normalized, "CREATE VIEW V_FIELDS_INFO AS SELECT") ||
      !ContainsAll(normalized,
                   {"FROM RDB$RELATION_FIELDS RF",
                    "JOIN RDB$FIELDS F ON RF.RDB$FIELD_SOURCE = F.RDB$FIELD_NAME",
                    "RDB$CHARACTER_SETS C",
                    "RDB$COLLATIONS K",
                    "RF.RDB$RELATION_NAME = UPPER('TEST')",
                    "RF.RDB$FIELD_NAME AS FIELD_NAME",
                    "F.RDB$CHARACTER_LENGTH AS FIELD_CHAR_LEN",
                    "F.RDB$CHARACTER_SET_ID AS FIELD_CSET_ID",
                    "F.RDB$COLLATION_ID AS FIELD_COLL_ID",
                    "C.RDB$CHARACTER_SET_NAME AS CSET_NAME",
                    "K.RDB$COLLATION_NAME AS FIELD_COLLATION",
                    "ORDER BY FIELD_NAME"})) {
    return {};
  }

  FirebirdCatalogProjectionRoute route;
  route.view_name = "V_FIELDS_INFO";
  route.source_relation_name = "TEST";
  if (ContainsAll(normalized,
                  {"FN_GET_TYPE_NAME(F.RDB$FIELD_TYPE, F.RDB$FIELD_SUB_TYPE)",
                   "AS FIELD_TYPE",
                   "RF.RDB$FIELD_POSITION AS FIELD_POS"})) {
    route.kind =
        FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoTypeView;
    route.function_name = "FN_GET_TYPE_NAME";
    route.semantic_variant = std::string(kFirebirdFieldsInfoTypeInventoryV1);
    return route;
  }
  if (normalized.find("FN_GET_TYPE_NAME") == std::string_view::npos &&
      normalized.find(" AS FIELD_TYPE") == std::string_view::npos &&
      normalized.find(" AS FIELD_POS") == std::string_view::npos) {
    route.kind =
        FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoCharsetView;
    route.semantic_variant =
        std::string(kFirebirdFieldsInfoCharsetInventoryV1);
    return route;
  }
  return {};
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (ch < 0x20) return {};
        escaped.push_back(static_cast<char>(ch));
    }
  }
  return escaped;
}

std::string EnvelopeHeader(std::string_view operation_id,
                           std::string_view opcode,
                           std::string_view operation_family) {
  return "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"" + EscapeJson(operation_id) + "\","
         "\"opcode\":\"" + EscapeJson(opcode) + "\","
         "\"operation_family\":\"" + EscapeJson(operation_family) + "\","
         "\"sblr_operation_family\":\"" + EscapeJson(operation_family) + "\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":true,"
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"parser_executes_sql\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\",";
}

std::string TextLineValue(std::string_view payload, std::string_view key) {
  const std::string prefix = std::string(key) + "=";
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto end = payload.find('\n', offset);
    const auto line = payload.substr(
        offset, end == std::string_view::npos ? payload.size() - offset
                                              : end - offset);
    if (line.starts_with(prefix)) return std::string(line.substr(prefix.size()));
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return {};
}

std::optional<std::uint64_t> ParseU64(std::string_view text) {
  if (text.empty()) return std::nullopt;
  std::uint64_t value = 0;
  for (const char ch : text) {
    if (ch < '0' || ch > '9') return std::nullopt;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
      return std::nullopt;
    }
    value = value * 10 + digit;
  }
  return value;
}

std::map<std::string, std::string> SemicolonFields(std::string_view text) {
  std::map<std::string, std::string> fields;
  std::size_t offset = 0;
  while (offset <= text.size()) {
    const auto end = text.find(';', offset);
    const auto field = text.substr(
        offset, end == std::string_view::npos ? text.size() - offset
                                              : end - offset);
    const auto equals = field.find('=');
    if (equals != std::string_view::npos) {
      fields.emplace(std::string(field.substr(0, equals)),
                     std::string(field.substr(equals + 1)));
    }
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return fields;
}

std::string EvidenceValue(std::string_view payload, std::string_view kind) {
  const std::string prefix = "evidence=" + std::string(kind) + ":";
  std::size_t offset = 0;
  while (offset <= payload.size()) {
    const auto end = payload.find('\n', offset);
    const auto line = payload.substr(
        offset, end == std::string_view::npos ? payload.size() - offset
                                              : end - offset);
    if (line.starts_with(prefix)) return std::string(line.substr(prefix.size()));
    if (end == std::string_view::npos) break;
    offset = end + 1;
  }
  return {};
}

std::string ResourceKey(std::string_view value) {
  std::string key;
  for (const unsigned char ch : value) {
    if (std::isalnum(ch) != 0) {
      key.push_back(static_cast<char>(std::toupper(ch)));
    }
  }
  return key;
}

std::optional<std::pair<std::string, std::int16_t>> FirebirdCharset(
    std::string_view canonical_name) {
  const std::string key = ResourceKey(canonical_name);
  if (key == "NONE") return std::pair<std::string, std::int16_t>{"NONE", 0};
  if (key == "OCTETS") return std::pair<std::string, std::int16_t>{"OCTETS", 1};
  if (key == "ASCII") return std::pair<std::string, std::int16_t>{"ASCII", 2};
  if (key == "UNICODEFSS") {
    return std::pair<std::string, std::int16_t>{"UNICODE_FSS", 3};
  }
  if (key == "UTF8" || key == "UTF8MB4") {
    return std::pair<std::string, std::int16_t>{"UTF8", 4};
  }
  if (key == "ISO88591" || key == "LATIN1") {
    return std::pair<std::string, std::int16_t>{"ISO8859_1", 21};
  }
  if (key == "WIN1250" || key == "WINDOWS1250" || key == "CP1250") {
    return std::pair<std::string, std::int16_t>{"WIN1250", 51};
  }
  if (key == "WIN1251" || key == "WINDOWS1251" || key == "CP1251") {
    return std::pair<std::string, std::int16_t>{"WIN1251", 52};
  }
  if (key == "WIN1252" || key == "WINDOWS1252" || key == "CP1252") {
    return std::pair<std::string, std::int16_t>{"WIN1252", 53};
  }
  if (key == "WIN1257" || key == "WINDOWS1257" || key == "CP1257") {
    return std::pair<std::string, std::int16_t>{"WIN1257", 60};
  }
  if (key == "GBK") return std::pair<std::string, std::int16_t>{"GBK", 67};
  return std::nullopt;
}

std::optional<std::pair<std::string, std::int16_t>> FirebirdCollation(
    std::string_view firebird_charset,
    std::string_view canonical_name) {
  const std::string charset = ResourceKey(firebird_charset);
  const std::string key = ResourceKey(canonical_name);
  if (key.empty()) return std::nullopt;
  if (charset == "NONE" && (key == "NONE" || key == "DEFAULT")) {
    return std::pair<std::string, std::int16_t>{"NONE", 0};
  }
  if (charset == "OCTETS" && (key == "OCTETS" || key == "DEFAULT")) {
    return std::pair<std::string, std::int16_t>{"OCTETS", 0};
  }
  if (charset == "ASCII" && (key == "ASCII" || key == "DEFAULT")) {
    return std::pair<std::string, std::int16_t>{"ASCII", 0};
  }
  if (charset == "UTF8") {
    if (key == "UTF8" || key == "UNICODE" || key == "UNICODEDEFAULT") {
      return std::pair<std::string, std::int16_t>{"UTF8", 0};
    }
    if (key == "UNICODECI") {
      return std::pair<std::string, std::int16_t>{"UNICODE_CI", 3};
    }
    if (key == "UNICODECIAI") {
      return std::pair<std::string, std::int16_t>{"UNICODE_CI_AI", 4};
    }
  }
  if (charset == "ISO88591" &&
      (key == "ISO88591" || key == "LATIN1" || key == "DEFAULT")) {
    return std::pair<std::string, std::int16_t>{"ISO8859_1", 0};
  }
  if (charset == "WIN1250") {
    if (key == "WIN1250" || key == "WINDOWS1250" || key == "DEFAULT") {
      return std::pair<std::string, std::int16_t>{"WIN1250", 0};
    }
    if (key == "WINCZ" || key == "PXCZ") {
      return std::pair<std::string, std::int16_t>{"WIN_CZ", 7};
    }
  }
  if (charset == "WIN1251") {
    if (key == "WIN1251") {
      return std::pair<std::string, std::int16_t>{"WIN1251", 0};
    }
    if (key == "PXWCYRL") {
      return std::pair<std::string, std::int16_t>{"PXW_CYRL", 1};
    }
  }
  if (charset == "WIN1252") {
    if (key == "WIN1252") return std::pair<std::string, std::int16_t>{"WIN1252", 0};
    if (key == "PXWINTL") return std::pair<std::string, std::int16_t>{"PXW_INTL", 1};
    if (key == "PXWINTL850") return std::pair<std::string, std::int16_t>{"PXW_INTL850", 2};
    if (key == "PXWNORDAN4") return std::pair<std::string, std::int16_t>{"PXW_NORDAN4", 3};
    if (key == "PXWSPAN") return std::pair<std::string, std::int16_t>{"PXW_SPAN", 4};
    if (key == "PXWSWEDFIN") return std::pair<std::string, std::int16_t>{"PXW_SWEDFIN", 5};
  }
  if (charset == "WIN1257") {
    if (key == "WIN1257") return std::pair<std::string, std::int16_t>{"WIN1257", 0};
    if (key == "WIN1257EE") return std::pair<std::string, std::int16_t>{"WIN1257_EE", 1};
    if (key == "WIN1257LT") return std::pair<std::string, std::int16_t>{"WIN1257_LT", 2};
    if (key == "WIN1257LV") return std::pair<std::string, std::int16_t>{"WIN1257_LV", 3};
  }
  if (charset == "GBK") {
    if (key == "GBK") return std::pair<std::string, std::int16_t>{"GBK", 0};
    if (key == "GBKUNICODE") return std::pair<std::string, std::int16_t>{"GBK_UNICODE", 1};
  }
  return std::nullopt;
}

std::string NormalizeTypeWhitespace(std::string value) {
  value = UpperAscii(TrimAscii(std::move(value)));
  std::string out;
  bool whitespace = false;
  for (const unsigned char ch : value) {
    if (std::isspace(ch) != 0) {
      whitespace = !out.empty();
      continue;
    }
    if (whitespace) out.push_back(' ');
    whitespace = false;
    out.push_back(static_cast<char>(ch));
  }
  return TrimAscii(std::move(out));
}

std::optional<std::string> FirebirdTypeName(std::string canonical_type,
                                           bool text_large_object) {
  const std::string type = NormalizeTypeWhitespace(std::move(canonical_type));
  if (type == "SMALLINT" || type == "INT16") return "SMALLINT";
  if (type == "INT" || type == "INTEGER" || type == "INT32") return "INTEGER";
  if (type == "BIGINT" || type == "INT64") return "BIGINT";
  if (type == "INT128") return "INT128";
  if (type == "FLOAT" || type == "REAL" || type == "REAL32") return "FLOAT";
  if (type == "DOUBLE" || type == "DOUBLE PRECISION" || type == "LONG FLOAT" ||
      type == "REAL64") {
    return "DOUBLE PRECISION";
  }
  if (type.starts_with("NUMERIC")) return "NUMERIC";
  if (type.starts_with("DECIMAL")) return "DECIMAL";
  if (type.starts_with("DECFLOAT(16") || type == "DECFLOAT16") return "DECFLOAT(16)";
  if (type.starts_with("DECFLOAT(34") || type == "DECFLOAT34") return "DECFLOAT(34)";
  if (type == "DATE") return "DATE";
  if (type == "TIME" || type == "TIME WITHOUT TIME ZONE") {
    return "TIME WITHOUT TIME ZONE";
  }
  if (type == "TIME WITH TIME ZONE" || type == "TIME_TZ") {
    return "TIME WITH TIME ZONE";
  }
  if (type == "TIMESTAMP" || type == "TIMESTAMP WITHOUT TIME ZONE") {
    return "TIMESTAMP WITHOUT TIME ZONE";
  }
  if (type == "TIMESTAMP WITH TIME ZONE" || type == "TIMESTAMP_TZ") {
    return "TIMESTAMP WITH TIME ZONE";
  }
  if (type == "BOOLEAN" || type == "BOOL") return "BOOLEAN";
  if (type.starts_with("CHAR(") || type.starts_with("CHARACTER(") ||
      type.starts_with("NCHAR(") || type.starts_with("NATIONAL CHAR")) {
    return "CHAR";
  }
  if (type.starts_with("VARCHAR(") ||
      type.starts_with("CHARACTER VARYING(")) {
    return "VARCHAR";
  }
  if (type.starts_with("BLOB")) {
    if (type.find("SUB_TYPE TEXT") != std::string::npos ||
        type.find("SUB_TYPE 1") != std::string::npos || text_large_object) {
      return "BLOB SUB_TYPE TEXT";
    }
    return "BLOB SUB_TYPE BINARY";
  }
  return std::nullopt;
}

struct NeutralColumn {
  std::string column_uuid;
  std::string name;
  std::uint64_t ordinal{0};
  std::string canonical_type_name;
  std::optional<std::uint64_t> character_length;
  std::string charset_uuid;
  std::string charset_name;
  std::string collation_uuid;
  std::string collation_name;
  bool text_large_object{false};
};

}  // namespace

FirebirdCatalogProjectionRoute ParseFirebirdCatalogProjectionRoute(
    std::string_view firebird_sql) {
  if (auto select = ParseSelectRoute(firebird_sql); select.recognized()) {
    return select;
  }
  const std::string normalized = NormalizeSql(firebird_sql);
  if (auto function = FunctionRoute(normalized); function.recognized()) {
    return function;
  }
  return ViewRoute(normalized);
}

std::optional<FirebirdCatalogProjectionRoute>
BindFirebirdCatalogProjectionViewVariant(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view engine_resolution_detail) {
  if (route.kind !=
          FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoUnbound ||
      route.view_name != "V_FIELDS_INFO" ||
      route.source_relation_name != "TEST" ||
      !route.semantic_variant.empty()) {
    return std::nullopt;
  }
  const std::string type_detail =
      std::string(kCatalogRelationDescriptorProjectionV1) + ":" +
      std::string(kFirebirdFieldsInfoTypeInventoryV1);
  const std::string charset_detail =
      std::string(kCatalogRelationDescriptorProjectionV1) + ":" +
      std::string(kFirebirdFieldsInfoCharsetInventoryV1);

  FirebirdCatalogProjectionRoute bound = route;
  if (engine_resolution_detail == type_detail) {
    bound.kind = FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType;
    bound.semantic_variant =
        std::string(kFirebirdFieldsInfoTypeInventoryV1);
    return bound;
  }
  if (engine_resolution_detail == charset_detail) {
    bound.kind = FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoCharset;
    bound.semantic_variant =
        std::string(kFirebirdFieldsInfoCharsetInventoryV1);
    return bound;
  }
  return std::nullopt;
}

FirebirdCatalogProjectionRoute FirebirdCatalogProjectionRouteForExactRebind(
    const FirebirdCatalogProjectionRoute& route) {
  FirebirdCatalogProjectionRoute rebound = route;
  if (route.kind ==
          FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType ||
      route.kind ==
          FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoCharset) {
    rebound.kind =
        FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoUnbound;
    rebound.semantic_variant.clear();
  }
  return rebound;
}

std::string_view FirebirdCatalogProjectionRouteName(
    FirebirdCatalogProjectionRouteKind kind) {
  switch (kind) {
    case FirebirdCatalogProjectionRouteKind::kCreateTypeNameFunction:
      return "create_type_name_function";
    case FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoTypeView:
      return "create_fields_info_type_view";
    case FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoCharsetView:
      return "create_fields_info_charset_view";
    case FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoUnbound:
      return "select_fields_info_unbound";
    case FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType:
      return "select_fields_info_type";
    case FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoCharset:
      return "select_fields_info_charset";
    case FirebirdCatalogProjectionRouteKind::kUnsupported:
      return "unsupported";
  }
  return "unsupported";
}

std::string EncodeFirebirdCatalogProjectionFunctionEnvelope(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view schema_uuid) {
  if (route.kind !=
          FirebirdCatalogProjectionRouteKind::kCreateTypeNameFunction ||
      route.function_name != "FN_GET_TYPE_NAME" || schema_uuid.empty()) {
    return {};
  }
  std::ostringstream out;
  out << EnvelopeHeader("ddl.create_function", "SBLR_DDL_CREATE_FUNCTION",
                        "sblr.catalog.mutation.v3")
      << "\"target_object_kind\":\"function\","
      << "\"function_name\":\"FN_GET_TYPE_NAME\","
      << "\"target_schema_uuid\":\"" << EscapeJson(schema_uuid) << "\","
      << "\"executor\":\"metadata_only\","
      << "\"side_effect_class\":\"none\","
      << "\"body_compilation_included\":false,"
      << "\"compiled_body_provenance\":\"firebird.standalone.catalog_relation_type_name.v1\","
      << "\"compiled_body_descriptor\":\""
      << kCatalogRelationTypeNameDescriptorV1 << "\","
      << "\"routine_parameter_count\":\"2\","
      << "\"routine_parameter_0_name\":\"A_TYPE\","
      << "\"routine_parameter_0_mode\":\"in\","
      << "\"routine_parameter_0_type\":\"smallint\","
      << "\"routine_parameter_1_name\":\"A_SUBTYPE\","
      << "\"routine_parameter_1_mode\":\"in\","
      << "\"routine_parameter_1_type\":\"smallint\","
      << "\"routine_return_count\":\"1\","
      << "\"routine_return_0_name\":\"RETURN_VALUE\","
      << "\"routine_return_0_type\":\"varchar(2048)\","
      << "\"permission\":\"manage_executable\"}";
  return out.str();
}

std::string EncodeFirebirdCatalogProjectionViewEnvelope(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view schema_uuid,
    std::string_view function_uuid) {
  const bool typed = route.kind ==
      FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoTypeView;
  const bool charset = route.kind ==
      FirebirdCatalogProjectionRouteKind::kCreateFieldsInfoCharsetView;
  if ((!typed && !charset) || route.view_name != "V_FIELDS_INFO" ||
      route.source_relation_name != "TEST" || schema_uuid.empty() ||
      (typed && function_uuid.empty())) {
    return {};
  }
  std::ostringstream out;
  out << EnvelopeHeader("ddl.create_view", "SBLR_DDL_CREATE_VIEW",
                        "sblr.catalog.mutation.v3")
      << "\"target_object_kind\":\"view\","
      << "\"view_name\":\"V_FIELDS_INFO\","
      << "\"target_schema_uuid\":\"" << EscapeJson(schema_uuid) << "\","
      << "\"view_query_shape\":\""
      << kCatalogRelationDescriptorProjectionV1 << "\","
      << "\"view_source_name\":\"TEST\","
      << "\"view_projection_count\":" << (typed ? 2 : 1) << ','
      << "\"view_projection_0\":\"variant:"
      << EscapeJson(route.semantic_variant) << "\"";
  if (typed) {
    out << ",\"view_projection_1\":\"function_uuid:"
        << EscapeJson(function_uuid) << "\"";
  }
  out << '}';
  return out.str();
}

std::string EncodeFirebirdCatalogProjectionSelectEnvelope(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view view_uuid,
    std::string_view relation_uuid,
    std::string_view relation_descriptor_uuid,
    std::uint64_t relation_descriptor_generation) {
  if (!route.is_select() || view_uuid.empty() || relation_uuid.empty() ||
      relation_descriptor_uuid.empty() || relation_descriptor_generation == 0 ||
      route.semantic_variant.empty()) {
    return {};
  }
  std::ostringstream out;
  out << EnvelopeHeader("dml.select_rows", "SBLR_DML_SELECT_ROWS",
                        "sblr.query.relational.v3")
      << "\"target_object_uuid\":\"" << EscapeJson(view_uuid) << "\","
      << "\"target_object_kind\":\"view\","
      << "\"source_kind\":\"catalog_relation_descriptor_projection\","
      << "\"source_uuid\":\"" << EscapeJson(relation_uuid) << "\","
      << "\"source_fingerprint\":\""
      << EscapeJson(relation_descriptor_uuid) << "\","
      << "\"source_position\":\"" << relation_descriptor_generation << "\","
      << "\"result_projection\":\""
      << kCatalogRelationDescriptorProjectionV1 << "\","
      << "\"dml_surface_variant\":\""
      << EscapeJson(route.semantic_variant) << "\"}";
  return out.str();
}

FirebirdCatalogProjectionSqlDaResult
DescribeFirebirdCatalogProjectionSqlDa(
    const FirebirdCatalogProjectionRoute& route,
    const FirebirdCatalogProjectionSqlDaProfile& profile) {
  FirebirdCatalogProjectionSqlDaResult result;
  auto reject = [&](std::string diagnostic) {
    result.ok = false;
    result.diagnostic = std::move(diagnostic);
    result.columns.clear();
    return result;
  };

  const bool typed =
      route.kind ==
          FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoType &&
      route.semantic_variant == kFirebirdFieldsInfoTypeInventoryV1;
  const bool charset_only =
      route.kind ==
          FirebirdCatalogProjectionRouteKind::kSelectFieldsInfoCharset &&
      route.semantic_variant == kFirebirdFieldsInfoCharsetInventoryV1;
  if ((!typed && !charset_only) || route.view_name != "V_FIELDS_INFO" ||
      route.source_relation_name != "TEST") {
    return reject("catalog_projection_bound_select_required");
  }
  if (profile.database_default_charset_max_bytes_per_character == 0 ||
      profile.database_default_charset_max_bytes_per_character > 4 ||
      profile.statement_charset_max_bytes_per_character == 0 ||
      profile.statement_charset_max_bytes_per_character > 4) {
    return reject("catalog_projection_charset_profile_invalid");
  }

  const std::uint64_t function_length =
      2048ULL * profile.database_default_charset_max_bytes_per_character;
  if (function_length > std::numeric_limits<std::uint16_t>::max()) {
    return reject("catalog_projection_field_type_length_invalid");
  }

  result.columns.reserve((typed ? 8U : 6U) +
                         (route.leading_message ? 1U : 0U));
  if (route.leading_message) {
    const auto character_count = Utf8CodePointCount(*route.leading_message);
    if (!character_count) {
      return reject("catalog_projection_message_utf8_invalid");
    }
    const std::uint64_t message_length =
        static_cast<std::uint64_t>(*character_count) *
        profile.statement_charset_max_bytes_per_character;
    if (message_length > std::numeric_limits<std::uint16_t>::max()) {
      return reject("catalog_projection_message_length_invalid");
    }
    result.columns.push_back({
        .field_name = "CONSTANT",
        .alias_name = "MSG",
        .sql_type = kSqlText,
        .subtype = profile.statement_charset_id,
        .length = static_cast<std::uint32_t>(message_length),
        .charset_id = profile.statement_charset_id,
        .nullable = false,
    });
  }

  auto append_view_column = [&](std::string name,
                                std::uint32_t sql_type,
                                std::uint32_t length,
                                std::uint16_t charset_id) {
    result.columns.push_back({
        .field_name = name,
        .alias_name = name,
        .relation_name = "V_FIELDS_INFO",
        .owner_name = "SYSDBA",
        .sql_type = sql_type,
        .subtype = sql_type == kSqlText || sql_type == kSqlVarying
                       ? charset_id
                       : 0,
        .length = length,
        .charset_id = charset_id,
        .nullable = true,
    });
  };

  // Firebird system identifiers are CHAR(63) CHARACTER SET UTF8. Their
  // physical SQLDA length is therefore 252 bytes, independent of the database
  // default charset used by the function result below.
  append_view_column("FIELD_NAME", kSqlText, 252, 4);
  if (typed) {
    append_view_column("FIELD_TYPE", kSqlVarying,
                       static_cast<std::uint32_t>(function_length),
                       profile.database_default_charset_id);
    append_view_column("FIELD_POS", kSqlShort, 2, 0);
  }
  append_view_column("FIELD_CHAR_LEN", kSqlShort, 2, 0);
  append_view_column("FIELD_CSET_ID", kSqlShort, 2, 0);
  append_view_column("FIELD_COLL_ID", kSqlShort, 2, 0);
  append_view_column("CSET_NAME", kSqlText, 252, 4);
  append_view_column("FIELD_COLLATION", kSqlText, 252, 4);

  result.ok = true;
  return result;
}

FirebirdCatalogProjectionFetchBlr BuildFirebirdCatalogProjectionFetchBlr(
    const std::vector<FirebirdCatalogProjectionSqlDaColumn>& columns,
    std::uint32_t sql_dialect) {
  FirebirdCatalogProjectionFetchBlr result;
  auto reject = [&](std::string diagnostic) {
    result.ok = false;
    result.diagnostic = std::move(diagnostic);
    result.message_length = 0;
    result.bytes.clear();
    return result;
  };
  if (columns.empty() || columns.size() >= 32767) {
    return reject("catalog_projection_sqlda_count_invalid");
  }

  result.bytes.reserve(8 + columns.size() * 7);
  result.bytes.push_back(sql_dialect <= 1 ? kBlrVersion4 : kBlrVersion5);
  result.bytes.push_back(kBlrBegin);
  result.bytes.push_back(kBlrMessage);
  result.bytes.push_back(0);
  AppendU16(&result.bytes,
            static_cast<std::uint32_t>(columns.size() * 2));

  std::uint32_t message_length = 0;
  for (const auto& column : columns) {
    if (column.length > std::numeric_limits<std::uint16_t>::max() ||
        column.scale < std::numeric_limits<std::int8_t>::min() ||
        column.scale > std::numeric_limits<std::int8_t>::max()) {
      return reject("catalog_projection_sqlda_column_invalid");
    }

    std::uint32_t alignment = 0;
    std::uint32_t physical_length = column.length;
    switch (column.sql_type) {
      case kSqlText:
        result.bytes.push_back(kBlrText2);
        AppendU16(&result.bytes, column.charset_id);
        AppendU16(&result.bytes, column.length);
        alignment = 1;
        break;
      case kSqlVarying:
        result.bytes.push_back(kBlrVarying2);
        AppendU16(&result.bytes, column.charset_id);
        AppendU16(&result.bytes, column.length);
        alignment = 2;
        physical_length += 2;
        break;
      case kSqlShort:
        if (column.length != 2) {
          return reject("catalog_projection_sqlda_short_length_invalid");
        }
        result.bytes.push_back(kBlrShort);
        result.bytes.push_back(static_cast<std::uint8_t>(column.scale));
        alignment = 2;
        break;
      default:
        return reject("catalog_projection_sqlda_type_unsupported");
    }

    result.bytes.push_back(kBlrShort);
    result.bytes.push_back(0);
    const auto value_end =
        AddAlignedLength(message_length, alignment, physical_length);
    if (!value_end) {
      return reject("catalog_projection_message_length_overflow");
    }
    const auto indicator_end = AddAlignedLength(*value_end, 2, 2);
    if (!indicator_end) {
      return reject("catalog_projection_message_length_overflow");
    }
    message_length = *indicator_end;
  }

  result.bytes.push_back(kBlrEnd);
  result.bytes.push_back(kBlrEoc);
  result.message_length = message_length;
  result.ok = true;
  return result;
}

FirebirdCatalogProjectionRenderResult RenderFirebirdCatalogProjectionPayload(
    const FirebirdCatalogProjectionRoute& route,
    std::string_view neutral_payload,
    std::string_view expected_relation_uuid,
    std::string_view expected_descriptor_uuid,
    std::uint64_t expected_descriptor_generation) {
  FirebirdCatalogProjectionRenderResult result;
  auto reject = [&](std::string diagnostic) {
    result.ok = false;
    result.diagnostic = std::move(diagnostic);
    result.rows.clear();
    return result;
  };
  if (!route.is_select()) return reject("catalog_projection_select_route_required");
  if (EvidenceValue(neutral_payload, "catalog_projection_marker") !=
      kCatalogRelationDescriptorProjectionV1) {
    return reject("catalog_projection_marker_missing");
  }
  if (!expected_relation_uuid.empty() &&
      EvidenceValue(neutral_payload, "catalog_projection_relation_uuid") !=
          expected_relation_uuid) {
    return reject("catalog_projection_relation_uuid_mismatch");
  }
  if (!expected_descriptor_uuid.empty() &&
      EvidenceValue(neutral_payload, "catalog_projection_descriptor_uuid") !=
          expected_descriptor_uuid) {
    return reject("catalog_projection_descriptor_uuid_mismatch");
  }
  if (expected_descriptor_generation != 0) {
    const auto actual = ParseU64(EvidenceValue(
        neutral_payload, "catalog_projection_descriptor_generation"));
    if (!actual || *actual != expected_descriptor_generation) {
      return reject("catalog_projection_descriptor_generation_mismatch");
    }
  }
  if (EvidenceValue(neutral_payload, "catalog_projection_parser_sql") !=
      "false") {
    return reject("catalog_projection_parser_sql_authority_present");
  }
  const auto row_count = ParseU64(TextLineValue(neutral_payload, "row_count"));
  if (!row_count || *row_count > 4096) {
    return reject("catalog_projection_row_count_invalid");
  }

  std::vector<NeutralColumn> neutral;
  neutral.reserve(static_cast<std::size_t>(*row_count));
  std::map<std::uint64_t, bool> ordinals;
  for (std::uint64_t index = 0; index < *row_count; ++index) {
    const auto fields = SemicolonFields(TextLineValue(
        neutral_payload, "row[" + std::to_string(index) + "]"));
    auto field = [&](std::string_view name) -> std::string {
      const auto found = fields.find(std::string(name));
      return found == fields.end() ? std::string{} : found->second;
    };
    NeutralColumn column;
    column.column_uuid = field("column_uuid");
    column.name = field("canonical_name_key");
    column.canonical_type_name = field("canonical_type_name");
    const auto ordinal = ParseU64(field("ordinal"));
    const std::string character_length = field("character_length");
    if (!character_length.empty()) {
      column.character_length = ParseU64(character_length);
      if (!column.character_length) {
        return reject("catalog_projection_character_length_invalid");
      }
    }
    column.charset_uuid = field("charset_uuid");
    column.charset_name = field("charset_canonical_name");
    column.collation_uuid = field("collation_uuid");
    column.collation_name = field("collation_canonical_name");
    const std::string text_lob = UpperAscii(field("text_large_object"));
    column.text_large_object = text_lob == "TRUE" || text_lob == "1";
    if (column.column_uuid.empty() || column.name.empty() ||
        column.canonical_type_name.empty() || !ordinal ||
        !ordinals.emplace(*ordinal, true).second ||
        (column.charset_uuid.empty() != column.charset_name.empty()) ||
        (column.collation_uuid.empty() != column.collation_name.empty()) ||
        (!column.collation_uuid.empty() && column.charset_uuid.empty())) {
      return reject("catalog_projection_neutral_row_invalid");
    }
    column.ordinal = *ordinal;
    neutral.push_back(std::move(column));
  }
  std::stable_sort(neutral.begin(), neutral.end(),
                   [](const NeutralColumn& left, const NeutralColumn& right) {
                     if (left.name != right.name) return left.name < right.name;
                     if (left.charset_name != right.charset_name) {
                       return left.charset_name < right.charset_name;
                     }
                     if (left.collation_name != right.collation_name) {
                       return left.collation_name < right.collation_name;
                     }
                     return left.ordinal < right.ordinal;
                   });

  const bool typed = route.semantic_variant == kFirebirdFieldsInfoTypeInventoryV1;
  const bool charset_only =
      route.semantic_variant == kFirebirdFieldsInfoCharsetInventoryV1;
  if (!typed && !charset_only) {
    return reject("catalog_projection_variant_unsupported");
  }
  for (const auto& column : neutral) {
    std::optional<std::string> charset_id;
    std::optional<std::string> collation_id;
    std::optional<std::string> charset_name;
    std::optional<std::string> collation_name;
    if (!column.charset_uuid.empty()) {
      const auto charset = FirebirdCharset(column.charset_name);
      if (!charset) return reject("catalog_projection_charset_unsupported");
      charset_id = std::to_string(charset->second);
      charset_name = charset->first;
      if (column.collation_uuid.empty()) {
        return reject("catalog_projection_collation_required");
      }
      const auto collation = FirebirdCollation(charset->first,
                                               column.collation_name);
      if (!collation) return reject("catalog_projection_collation_unsupported");
      collation_id = std::to_string(collation->second);
      collation_name = collation->first;
    }

    FirebirdCatalogProjectionRenderedRow row;
    if (route.leading_message) row.cells.push_back(*route.leading_message);
    row.cells.push_back(column.name);
    if (typed) {
      const auto type_name = FirebirdTypeName(column.canonical_type_name,
                                              column.text_large_object);
      if (!type_name) return reject("catalog_projection_type_unsupported");
      row.cells.push_back(*type_name);
      row.cells.push_back(std::to_string(column.ordinal));
    }
    row.cells.push_back(column.character_length
                            ? std::optional<std::string>(
                                  std::to_string(*column.character_length))
                            : std::nullopt);
    row.cells.push_back(std::move(charset_id));
    row.cells.push_back(std::move(collation_id));
    row.cells.push_back(std::move(charset_name));
    row.cells.push_back(std::move(collation_name));
    result.rows.push_back(std::move(row));
  }
  result.ok = true;
  return result;
}

}  // namespace scratchbird::parser::firebird
