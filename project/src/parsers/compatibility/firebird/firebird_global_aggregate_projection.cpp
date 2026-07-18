// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_global_aggregate_projection.hpp"

#include "firebird_dialect.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace scratchbird::parser::firebird {
namespace {

constexpr std::string_view kNeutralProjectionMarker =
    "sblr.global_aggregate_projection.v1";
constexpr std::string_view kNeutralCountResultDescriptorKind = "scalar";
constexpr std::string_view kNeutralCountResultCanonicalType = "int64";
constexpr std::string_view kNeutralCountResultEncodedDescriptor =
    "canonical=int64;precision=64;scale=0;nullable=false";
constexpr std::string_view kNeutralAvgIntegerResultDescriptorKind = "scalar";
constexpr std::string_view kNeutralAvgIntegerResultCanonicalType = "int64";
constexpr std::string_view kNeutralAvgIntegerResultEncodedDescriptor =
    "canonical=int64;precision=64;scale=0;nullable=true";
constexpr std::string_view kNeutralAvgRealResultDescriptorKind = "scalar";
constexpr std::string_view kNeutralAvgRealResultCanonicalType = "real64";
constexpr std::string_view kNeutralAvgRealResultEncodedDescriptor =
    "canonical=real64;precision=64;nullable=true";
constexpr std::string_view kNeutralExpressionLiteralDescriptorKind = "scalar";
constexpr std::string_view kNeutralExpressionLiteralCanonicalType = "int32";
constexpr std::string_view kNeutralExpressionLiteralEncodedDescriptor =
    "canonical=int32;precision=32;scale=0;nullable=false";
constexpr std::string_view kNeutralExpressionResultDescriptorKind = "scalar";
constexpr std::string_view kNeutralExpressionResultCanonicalType = "int64";
constexpr std::string_view kNeutralExpressionResultEncodedDescriptor =
    "canonical=int64;precision=64;scale=0;nullable=true";

struct Identifier {
  std::string value;
  bool quoted{false};
};

std::string Trim(std::string_view value) {
  std::size_t begin = 0;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }
  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1])) != 0) {
    --end;
  }
  return std::string(value.substr(begin, end - begin));
}

std::string StripTerminator(std::string_view sql) {
  std::string out = Trim(sql);
  while (!out.empty() && out.back() == ';') {
    out.pop_back();
    out = Trim(out);
  }
  return out;
}

void SkipWhitespace(std::string_view text, std::size_t* offset) {
  if (offset == nullptr) return;
  while (*offset < text.size()) {
    if (std::isspace(static_cast<unsigned char>(text[*offset])) != 0) {
      ++*offset;
      continue;
    }
    if (*offset + 1 < text.size() && text[*offset] == '-' &&
        text[*offset + 1] == '-') {
      *offset += 2;
      while (*offset < text.size() && text[*offset] != '\n' &&
             text[*offset] != '\r') {
        ++*offset;
      }
      continue;
    }
    if (*offset + 1 < text.size() && text[*offset] == '/' &&
        text[*offset + 1] == '*') {
      *offset += 2;
      while (*offset + 1 < text.size() &&
             !(text[*offset] == '*' && text[*offset + 1] == '/')) {
        ++*offset;
      }
      if (*offset + 1 >= text.size()) {
        *offset = text.size();
        return;
      }
      *offset += 2;
      continue;
    }
    break;
  }
}

bool IdentifierCharacter(char ch) {
  const unsigned char value = static_cast<unsigned char>(ch);
  return std::isalnum(value) != 0 || ch == '_' || ch == '$';
}

bool ConsumeKeyword(std::string_view text,
                    std::size_t* offset,
                    std::string_view keyword) {
  if (offset == nullptr) return false;
  SkipWhitespace(text, offset);
  if (*offset + keyword.size() > text.size()) return false;
  for (std::size_t index = 0; index < keyword.size(); ++index) {
    if (std::toupper(static_cast<unsigned char>(text[*offset + index])) !=
        std::toupper(static_cast<unsigned char>(keyword[index]))) {
      return false;
    }
  }
  const std::size_t end = *offset + keyword.size();
  if ((*offset != 0 && IdentifierCharacter(text[*offset - 1])) ||
      (end < text.size() && IdentifierCharacter(text[end]))) {
    return false;
  }
  *offset = end;
  return true;
}

bool ConsumeCharacter(std::string_view text,
                      std::size_t* offset,
                      char expected) {
  if (offset == nullptr) return false;
  SkipWhitespace(text, offset);
  if (*offset >= text.size() || text[*offset] != expected) return false;
  ++*offset;
  return true;
}

std::optional<Identifier> ReadIdentifier(std::string_view text,
                                         std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  if (*offset >= text.size()) return std::nullopt;
  if (text[*offset] == '"') {
    Identifier identifier;
    identifier.quoted = true;
    ++*offset;
    while (*offset < text.size()) {
      const char ch = text[*offset];
      ++*offset;
      if (ch != '"') {
        identifier.value.push_back(ch);
        continue;
      }
      if (*offset < text.size() && text[*offset] == '"') {
        identifier.value.push_back('"');
        ++*offset;
        continue;
      }
      if (identifier.value.empty()) return std::nullopt;
      return identifier;
    }
    return std::nullopt;
  }

  const std::size_t begin = *offset;
  while (*offset < text.size()) {
    const char ch = text[*offset];
    if (!IdentifierCharacter(ch) && ch != '.') break;
    ++*offset;
  }
  if (*offset == begin) return std::nullopt;
  Identifier identifier;
  identifier.value =
      ToUpperAscii(text.substr(begin, *offset - begin));
  return identifier;
}

std::optional<std::size_t> FindTopLevelKeyword(std::string_view text,
                                               std::string_view keyword,
                                               std::size_t begin = 0) {
  const std::string upper = ToUpperAscii(text);
  const std::string expected = ToUpperAscii(keyword);
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  bool line_comment = false;
  bool block_comment = false;
  for (std::size_t index = begin;
       index + expected.size() <= text.size(); ++index) {
    const char ch = text[index];
    if (line_comment) {
      if (ch == '\n' || ch == '\r') line_comment = false;
      continue;
    }
    if (block_comment) {
      if (ch == '*' && index + 1 < text.size() && text[index + 1] == '/') {
        block_comment = false;
        ++index;
      }
      continue;
    }
    if (single_quoted) {
      if (ch == '\'' && index + 1 < text.size() && text[index + 1] == '\'') {
        ++index;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && index + 1 < text.size() && text[index + 1] == '"') {
        ++index;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
      continue;
    }
    if (ch == '"') {
      double_quoted = true;
      continue;
    }
    if (ch == '-' && index + 1 < text.size() && text[index + 1] == '-') {
      line_comment = true;
      ++index;
      continue;
    }
    if (ch == '/' && index + 1 < text.size() && text[index + 1] == '*') {
      block_comment = true;
      ++index;
      continue;
    }
    if (ch == '(') {
      ++depth;
      continue;
    }
    if (ch == ')' && depth != 0) {
      --depth;
      continue;
    }
    if (depth != 0 || upper.compare(index, expected.size(), expected) != 0) {
      continue;
    }
    const bool left = index == 0 || !IdentifierCharacter(text[index - 1]);
    const std::size_t end = index + expected.size();
    const bool right = end == text.size() || !IdentifierCharacter(text[end]);
    if (left && right) return index;
  }
  return std::nullopt;
}

std::vector<std::string> SplitTopLevel(std::string_view text) {
  std::vector<std::string> items;
  std::size_t begin = 0;
  std::size_t depth = 0;
  bool single_quoted = false;
  bool double_quoted = false;
  for (std::size_t index = 0; index < text.size(); ++index) {
    const char ch = text[index];
    if (single_quoted) {
      if (ch == '\'' && index + 1 < text.size() && text[index + 1] == '\'') {
        ++index;
      } else if (ch == '\'') {
        single_quoted = false;
      }
      continue;
    }
    if (double_quoted) {
      if (ch == '"' && index + 1 < text.size() && text[index + 1] == '"') {
        ++index;
      } else if (ch == '"') {
        double_quoted = false;
      }
      continue;
    }
    if (ch == '\'') {
      single_quoted = true;
    } else if (ch == '"') {
      double_quoted = true;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')' && depth != 0) {
      --depth;
    } else if (ch == ',' && depth == 0) {
      items.push_back(Trim(text.substr(begin, index - begin)));
      begin = index + 1;
    }
  }
  items.push_back(Trim(text.substr(begin)));
  return items;
}

bool SameIdentifier(const Identifier& left, const Identifier& right) {
  return left.quoted == right.quoted && left.value == right.value;
}

std::optional<FirebirdGlobalCountProjectionItem> ParseCountItem(
    std::string_view text) {
  std::size_t offset = 0;
  if (!ConsumeKeyword(text, &offset, "COUNT") ||
      !ConsumeCharacter(text, &offset, '(')) {
    return std::nullopt;
  }

  FirebirdGlobalCountProjectionItem item;
  item.aggregate_function_uuid =
      std::string(kFirebirdCanonicalCountAggregateUuid);
  SkipWhitespace(text, &offset);
  if (offset < text.size() && text[offset] == '*') {
    ++offset;
    item.operation = FirebirdGlobalCountProjectionOperation::kCountStar;
  } else {
    const bool distinct = ConsumeKeyword(text, &offset, "DISTINCT");
    const auto source = ReadIdentifier(text, &offset);
    if (!source || source->value.find('.') != std::string::npos) {
      return std::nullopt;
    }
    item.operation = distinct
                         ? FirebirdGlobalCountProjectionOperation::
                               kCountDistinctField
                         : FirebirdGlobalCountProjectionOperation::
                               kCountNonNullField;
    item.source_column = source->value;
    item.source_column_quoted = source->quoted;
  }
  if (!ConsumeCharacter(text, &offset, ')') ||
      !ConsumeKeyword(text, &offset, "AS")) {
    return std::nullopt;
  }
  const auto alias = ReadIdentifier(text, &offset);
  if (!alias || alias->value.find('.') != std::string::npos) {
    return std::nullopt;
  }
  SkipWhitespace(text, &offset);
  if (offset != text.size()) return std::nullopt;
  item.output_alias = alias->value;
  return item;
}

bool StartsWithCountCall(std::string_view projection) {
  std::size_t offset = 0;
  if (!ConsumeKeyword(projection, &offset, "COUNT")) return false;
  SkipWhitespace(projection, &offset);
  return offset < projection.size() && projection[offset] == '(';
}

std::optional<FirebirdGlobalAvgProjectionItem> ParseAvgItem(
    std::string_view text) {
  std::size_t offset = 0;
  if (!ConsumeKeyword(text, &offset, "AVG") ||
      !ConsumeCharacter(text, &offset, '(')) {
    return std::nullopt;
  }

  FirebirdGlobalAvgProjectionItem item;
  item.aggregate_function_uuid = std::string(kFirebirdCanonicalAvgAggregateUuid);
  const bool distinct = ConsumeKeyword(text, &offset, "DISTINCT");
  const auto source = ReadIdentifier(text, &offset);
  if (!source || source->value.find('.') != std::string::npos ||
      !ConsumeCharacter(text, &offset, ')')) {
    return std::nullopt;
  }
  item.operation = distinct
                       ? FirebirdGlobalAvgProjectionOperation::kAvgDistinctField
                       : FirebirdGlobalAvgProjectionOperation::kAvgField;
  item.source_column = source->value;
  item.source_column_quoted = source->quoted;

  if (ConsumeKeyword(text, &offset, "AS")) {
    const auto alias = ReadIdentifier(text, &offset);
    if (!alias || alias->value.find('.') != std::string::npos) {
      return std::nullopt;
    }
    item.output_alias = alias->value;
  } else {
    item.output_alias = "AVG";
  }
  SkipWhitespace(text, &offset);
  if (offset != text.size()) return std::nullopt;
  return item;
}

bool StartsWithAvgCall(std::string_view projection) {
  std::size_t offset = 0;
  if (!ConsumeKeyword(projection, &offset, "AVG")) return false;
  SkipWhitespace(projection, &offset);
  return offset < projection.size() && projection[offset] == '(';
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  static constexpr char kHex[] = "0123456789abcdef";
  for (const unsigned char ch : value) {
    switch (ch) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (ch < 0x20) {
          escaped += "\\u00";
          escaped.push_back(kHex[(ch >> 4u) & 0x0fu]);
          escaped.push_back(kHex[ch & 0x0fu]);
        } else {
          escaped.push_back(static_cast<char>(ch));
        }
    }
  }
  return escaped;
}

std::string HexEncode(std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2u);
  for (const unsigned char byte : value) {
    encoded.push_back(kHex[(byte >> 4u) & 0x0fu]);
    encoded.push_back(kHex[byte & 0x0fu]);
  }
  return encoded;
}

bool HexDecode(std::string_view value, std::string* decoded) {
  if (decoded == nullptr || value.size() % 2u != 0) return false;
  const auto digit = [](unsigned char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  std::string out;
  out.reserve(value.size() / 2u);
  for (std::size_t offset = 0; offset < value.size(); offset += 2u) {
    const int high = digit(static_cast<unsigned char>(value[offset]));
    const int low = digit(static_cast<unsigned char>(value[offset + 1u]));
    if (high < 0 || low < 0) return false;
    out.push_back(static_cast<char>((high << 4) | low));
  }
  *decoded = std::move(out);
  return true;
}

std::vector<std::string_view> SplitPacket(std::string_view packet) {
  std::vector<std::string_view> parts;
  std::size_t begin = 0;
  while (begin <= packet.size()) {
    const auto end = packet.find('|', begin);
    parts.push_back(packet.substr(
        begin,
        end == std::string_view::npos ? packet.size() - begin : end - begin));
    if (end == std::string_view::npos) break;
    begin = end + 1u;
  }
  return parts;
}

bool ParseStrictU64(std::string_view value, std::uint64_t* parsed) {
  if (parsed == nullptr || value.empty() ||
      (value.size() > 1 && value.front() == '0')) {
    return false;
  }
  std::uint64_t out = 0;
  for (const unsigned char ch : value) {
    if (!std::isdigit(ch)) return false;
    const auto digit = static_cast<std::uint64_t>(ch - '0');
    if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
      return false;
    }
    out = out * 10u + digit;
  }
  *parsed = out;
  return true;
}

std::optional<std::int32_t> ReadInt32Literal(std::string_view text,
                                             std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  const std::size_t begin = *offset;
  bool negative = false;
  if (*offset < text.size() && text[*offset] == '-') {
    negative = true;
    ++*offset;
  }
  const std::size_t digits_begin = *offset;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
  if (*offset == digits_begin ||
      (*offset - digits_begin > 1 && text[digits_begin] == '0')) {
    *offset = begin;
    return std::nullopt;
  }
  std::uint64_t magnitude = 0;
  for (std::size_t index = digits_begin; index < *offset; ++index) {
    const auto digit = static_cast<std::uint64_t>(text[index] - '0');
    if (magnitude >
        (static_cast<std::uint64_t>(
             std::numeric_limits<std::int32_t>::max()) +
         (negative ? 1u : 0u) - digit) /
            10u) {
      *offset = begin;
      return std::nullopt;
    }
    magnitude = magnitude * 10u + digit;
  }
  if (negative && magnitude == 0) {
    *offset = begin;
    return std::nullopt;
  }
  if (negative &&
      magnitude == static_cast<std::uint64_t>(
                       std::numeric_limits<std::int32_t>::max()) +
                       1u) {
    return std::numeric_limits<std::int32_t>::min();
  }
  const auto positive = static_cast<std::int32_t>(magnitude);
  return negative ? -positive : positive;
}

void RejectBinding(FirebirdBoundGlobalCountProjection* binding,
                   std::string code,
                   std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.global_aggregate_projection"));
}

void RejectBinding(FirebirdBoundGlobalAvgProjection* binding,
                   std::string code,
                   std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.global_aggregate_projection"));
}

void RejectBinding(FirebirdBoundGlobalAggregateViewCreate* binding,
                   std::string code,
                   std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.global_aggregate_view"));
}

void RejectBinding(FirebirdBoundGlobalAggregateViewSelect* binding,
                   std::string code,
                   std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.global_aggregate_view"));
}

bool CompleteColumnDescriptor(
    const ipc::PublicRelationColumnDescriptor& column) {
  return !column.column_uuid.empty() && !column.canonical_name_key.empty() &&
         !column.type_descriptor_uuid.empty() &&
         !column.type_descriptor_kind.empty() &&
         !column.canonical_type_name.empty() &&
         !column.encoded_type_descriptor.empty();
}

FirebirdGlobalAvgResultKind AvgResultKindForDescriptor(
    const ipc::PublicRelationColumnDescriptor& column) {
  const std::string type = ToUpperAscii(Trim(column.canonical_type_name));
  if (type == "INTEGER" || type == "INT" || type == "INT32" ||
      type == "BIGINT" || type == "INT64") {
    return FirebirdGlobalAvgResultKind::kNullableInt64;
  }
  if (type == "DOUBLE" || type == "DOUBLE PRECISION" ||
      type == "DOUBLE_PRECISION" || type == "REAL64") {
    return FirebirdGlobalAvgResultKind::kNullableReal64;
  }
  return FirebirdGlobalAvgResultKind::kUnsupported;
}

bool FirebirdInt32Descriptor(
    const ipc::PublicRelationColumnDescriptor& column) {
  const std::string type = ToUpperAscii(Trim(column.canonical_type_name));
  return type == "INTEGER" || type == "INT" || type == "INT32";
}

}  // namespace

FirebirdGlobalCountProjectionRoute ParseFirebirdGlobalCountProjectionRoute(
    std::string_view firebird_sql) {
  FirebirdGlobalCountProjectionRoute route;
  const std::string sql = StripTerminator(firebird_sql);

  std::size_t select_end = 0;
  if (!ConsumeKeyword(sql, &select_end, "SELECT")) return route;
  const auto from = FindTopLevelKeyword(sql, "FROM", select_end);
  if (!from) return route;
  const auto projection_items = SplitTopLevel(
      std::string_view(sql).substr(select_end, *from - select_end));
  route.attempted = projection_items.size() > 1 &&
                    std::any_of(projection_items.begin(),
                                projection_items.end(),
                                [](const std::string& item) {
                                  return StartsWithCountCall(item);
                                });
  if (!route.attempted || projection_items.size() != 3) return route;

  route.items.reserve(3);
  for (const auto& projection : projection_items) {
    auto item = ParseCountItem(projection);
    if (!item) {
      route.items.clear();
      return route;
    }
    route.items.push_back(std::move(*item));
  }
  if (route.items[0].operation !=
          FirebirdGlobalCountProjectionOperation::kCountStar ||
      route.items[1].operation !=
          FirebirdGlobalCountProjectionOperation::kCountNonNullField ||
      route.items[2].operation !=
          FirebirdGlobalCountProjectionOperation::kCountDistinctField) {
    route.items.clear();
    return route;
  }
  const Identifier second{route.items[1].source_column,
                          route.items[1].source_column_quoted};
  const Identifier third{route.items[2].source_column,
                         route.items[2].source_column_quoted};
  if (!SameIdentifier(second, third)) {
    route.items.clear();
    return route;
  }
  for (std::size_t left = 0; left < route.items.size(); ++left) {
    for (std::size_t right = left + 1; right < route.items.size(); ++right) {
      if (route.items[left].output_alias == route.items[right].output_alias) {
        route.items.clear();
        return route;
      }
    }
  }

  std::size_t source_offset = *from + std::string_view("FROM").size();
  const auto source = ReadIdentifier(sql, &source_offset);
  SkipWhitespace(sql, &source_offset);
  if (!source || source->value.empty() || source_offset != sql.size()) {
    route.items.clear();
    return route;
  }
  route.source_relation = source->value;
  route.source_relation_quoted = source->quoted;
  route.valid = true;
  return route;
}

FirebirdGlobalAvgProjectionRoute ParseFirebirdGlobalAvgProjectionRoute(
    std::string_view firebird_sql) {
  FirebirdGlobalAvgProjectionRoute route;
  const std::string sql = StripTerminator(firebird_sql);

  std::size_t select_end = 0;
  if (!ConsumeKeyword(sql, &select_end, "SELECT")) return route;
  const auto from = FindTopLevelKeyword(sql, "FROM", select_end);
  if (!from) return route;
  const auto projection_items = SplitTopLevel(
      std::string_view(sql).substr(select_end, *from - select_end));
  route.attempted = std::any_of(
      projection_items.begin(), projection_items.end(),
      [](const std::string& item) { return StartsWithAvgCall(item); });
  if (!route.attempted || projection_items.size() != 1) return route;

  auto item = ParseAvgItem(projection_items.front());
  if (!item) return route;
  route.item = std::move(*item);

  std::size_t source_offset = *from + std::string_view("FROM").size();
  const auto source = ReadIdentifier(sql, &source_offset);
  SkipWhitespace(sql, &source_offset);
  if (!source || source->value.empty() || source_offset != sql.size()) {
    route.item = {};
    return route;
  }
  route.source_relation = source->value;
  route.source_relation_quoted = source->quoted;
  route.valid = true;
  return route;
}

FirebirdBoundGlobalCountProjection BindFirebirdGlobalCountProjection(
    const FirebirdGlobalCountProjectionRoute& route,
    const ipc::PublicNameResolutionResult& resolved_relation) {
  FirebirdBoundGlobalCountProjection binding;
  binding.route = route;
  if (!route.recognized()) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.SHAPE_UNSUPPORTED",
                  "The standalone Firebird COUNT projection binder supports only the exact three-output global COUNT shape.");
    return binding;
  }
  for (const auto& item : route.items) {
    if (item.aggregate_function_uuid !=
        kFirebirdCanonicalCountAggregateUuid) {
      RejectBinding(&binding, "FIREBIRD.AGGREGATE.FUNCTION_UUID_INVALID",
                    "The Firebird COUNT item is not bound to the canonical aggregate-registry UUID.");
      return binding;
    }
  }

  const auto& descriptor = resolved_relation.relation_descriptor;
  if (!resolved_relation.resolved || resolved_relation.object_uuid.empty() ||
      !descriptor.present || descriptor.descriptor_uuid.empty() ||
      descriptor.relation_uuid != resolved_relation.object_uuid ||
      descriptor.descriptor_generation == 0 ||
      descriptor.validated_resource_epoch == 0 || descriptor.columns.empty()) {
    binding.messages = resolved_relation.messages;
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.RELATION_DESCRIPTOR_REQUIRED",
                  "The exact engine-owned relation descriptor is required to bind the Firebird COUNT projection.");
    return binding;
  }

  const auto& requested = route.items[1];
  const ipc::PublicRelationColumnDescriptor* match = nullptr;
  for (const auto& column : descriptor.columns) {
    const bool matches = requested.source_column_quoted
                             ? column.canonical_name_key ==
                                   requested.source_column
                             : ToUpperAscii(column.canonical_name_key) ==
                                   ToUpperAscii(requested.source_column);
    if (!matches) continue;
    if (match != nullptr) {
      RejectBinding(&binding, "FIREBIRD.AGGREGATE.COLUMN_AMBIGUOUS",
                    "The persisted relation descriptor contains an ambiguous COUNT field binding.");
      return binding;
    }
    match = &column;
  }
  if (match == nullptr) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.COLUMN_NOT_FOUND",
                  "The COUNT field is absent from the persisted relation descriptor.");
    return binding;
  }
  if (!CompleteColumnDescriptor(*match)) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.COLUMN_DESCRIPTOR_REQUIRED",
                  "The COUNT field requires a complete engine-owned type descriptor and UUID.");
    return binding;
  }

  binding.relation_uuid = resolved_relation.object_uuid;
  binding.relation_descriptor_uuid = descriptor.descriptor_uuid;
  binding.relation_descriptor_generation = descriptor.descriptor_generation;
  binding.validated_resource_epoch = descriptor.validated_resource_epoch;
  binding.source_column = *match;
  binding.accepted = true;
  return binding;
}

FirebirdBoundGlobalAvgProjection BindFirebirdGlobalAvgProjection(
    const FirebirdGlobalAvgProjectionRoute& route,
    const ipc::PublicNameResolutionResult& resolved_relation) {
  FirebirdBoundGlobalAvgProjection binding;
  binding.route = route;
  if (!route.recognized()) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.SHAPE_UNSUPPORTED",
                  "The standalone Firebird AVG binder supports only one direct-relation AVG([DISTINCT] field) projection.");
    return binding;
  }
  if (route.item.aggregate_function_uuid !=
      kFirebirdCanonicalAvgAggregateUuid) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.FUNCTION_UUID_INVALID",
                  "The Firebird AVG item is not bound to the canonical aggregate-registry UUID.");
    return binding;
  }

  const auto& descriptor = resolved_relation.relation_descriptor;
  if (!resolved_relation.resolved || resolved_relation.object_uuid.empty() ||
      !descriptor.present || descriptor.descriptor_uuid.empty() ||
      descriptor.relation_uuid != resolved_relation.object_uuid ||
      descriptor.descriptor_generation == 0 ||
      descriptor.validated_resource_epoch == 0 || descriptor.columns.empty()) {
    binding.messages = resolved_relation.messages;
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.RELATION_DESCRIPTOR_REQUIRED",
                  "The exact engine-owned relation descriptor is required to bind the Firebird AVG projection.");
    return binding;
  }

  const ipc::PublicRelationColumnDescriptor* match = nullptr;
  for (const auto& column : descriptor.columns) {
    const bool matches = route.item.source_column_quoted
                             ? column.canonical_name_key ==
                                   route.item.source_column
                             : ToUpperAscii(column.canonical_name_key) ==
                                   ToUpperAscii(route.item.source_column);
    if (!matches) continue;
    if (match != nullptr) {
      RejectBinding(&binding, "FIREBIRD.AGGREGATE.COLUMN_AMBIGUOUS",
                    "The persisted relation descriptor contains an ambiguous AVG field binding.");
      return binding;
    }
    match = &column;
  }
  if (match == nullptr) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.COLUMN_NOT_FOUND",
                  "The AVG field is absent from the persisted relation descriptor.");
    return binding;
  }
  if (!CompleteColumnDescriptor(*match)) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.COLUMN_DESCRIPTOR_REQUIRED",
                  "The AVG field requires a complete engine-owned type descriptor and UUID.");
    return binding;
  }
  binding.result_kind = AvgResultKindForDescriptor(*match);
  if (binding.result_kind == FirebirdGlobalAvgResultKind::kUnsupported) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE.AVG_TYPE_UNSUPPORTED",
                  "The direct Firebird AVG route admits only int32, int64, and real64 source descriptors.");
    return binding;
  }

  binding.relation_uuid = resolved_relation.object_uuid;
  binding.relation_descriptor_uuid = descriptor.descriptor_uuid;
  binding.relation_descriptor_generation = descriptor.descriptor_generation;
  binding.validated_resource_epoch = descriptor.validated_resource_epoch;
  binding.source_column = *match;
  binding.accepted = true;
  return binding;
}

std::string EncodeFirebirdGlobalCountProjectionEnvelope(
    const FirebirdBoundGlobalCountProjection& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      binding.relation_uuid.empty() ||
      binding.relation_descriptor_uuid.empty() ||
      binding.relation_descriptor_generation == 0 ||
      !CompleteColumnDescriptor(binding.source_column)) {
    return {};
  }

  std::ostringstream out;
  out << "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"dml.select_rows\","
         "\"opcode\":\"SBLR_DML_SELECT_ROWS\","
         "\"operation_family\":\"sblr.query.relational.v3\","
         "\"sblr_operation_family\":\"sblr.query.relational.v3\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":true,"
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\","
      << "\"target_object_uuid\":\"" << EscapeJson(binding.relation_uuid)
      << "\",\"target_object_kind\":\"table\","
      << "\"source_uuid\":\"" << EscapeJson(binding.relation_uuid)
      << "\",\"source_kind\":\"table\","
      << "\"dml_surface_variant\":\"global_aggregate_projection_v1\","
      << "\"result_projection\":\"" << kNeutralProjectionMarker << "\","
      << "\"aggregate_function\":\""
      << kFirebirdCanonicalCountAggregateUuid << "\","
      << "\"projection_count\":\"3\"";

  for (std::size_t index = 0; index < binding.route.items.size(); ++index) {
    const auto& item = binding.route.items[index];
    const bool field_operation =
        item.operation != FirebirdGlobalCountProjectionOperation::kCountStar;
    std::ostringstream packed;
    packed << "gag1|" << HexEncode(item.aggregate_function_uuid) << '|'
           << static_cast<unsigned>(item.operation) << '|'
           << HexEncode(item.output_alias) << '|'
           << HexEncode(binding.relation_uuid) << '|'
           << HexEncode(binding.relation_descriptor_uuid) << '|'
           << binding.relation_descriptor_generation << '|'
           << HexEncode(field_operation
                            ? std::string_view(
                                  binding.source_column.column_uuid)
                            : std::string_view{})
           << '|'
           << HexEncode(field_operation
                            ? std::string_view(
                                  binding.source_column.type_descriptor_uuid)
                            : std::string_view{})
           << '|'
           << HexEncode(field_operation
                            ? std::string_view(
                                  binding.source_column.type_descriptor_kind)
                            : std::string_view{})
           << '|'
           << HexEncode(field_operation
                            ? std::string_view(
                                  binding.source_column.canonical_type_name)
                            : std::string_view{})
           << '|'
           << HexEncode(field_operation
                            ? std::string_view(
                                  binding.source_column.encoded_type_descriptor)
                            : std::string_view{})
           << '|' << HexEncode(kNeutralCountResultDescriptorKind) << '|'
           << HexEncode(kNeutralCountResultCanonicalType) << '|'
           << HexEncode(kNeutralCountResultEncodedDescriptor);
    out << ",\"projection_" << index << "\":\""
        << EscapeJson(packed.str()) << "\"";
  }
  out << '}';
  return out.str();
}

std::string EncodeFirebirdGlobalAvgProjectionEnvelope(
    const FirebirdBoundGlobalAvgProjection& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      binding.relation_uuid.empty() ||
      binding.relation_descriptor_uuid.empty() ||
      binding.relation_descriptor_generation == 0 ||
      !CompleteColumnDescriptor(binding.source_column) ||
      binding.result_kind == FirebirdGlobalAvgResultKind::kUnsupported) {
    return {};
  }

  const bool integer_result =
      binding.result_kind == FirebirdGlobalAvgResultKind::kNullableInt64;
  const std::string_view result_kind =
      integer_result ? kNeutralAvgIntegerResultDescriptorKind
                     : kNeutralAvgRealResultDescriptorKind;
  const std::string_view result_type =
      integer_result ? kNeutralAvgIntegerResultCanonicalType
                     : kNeutralAvgRealResultCanonicalType;
  const std::string_view result_descriptor =
      integer_result ? kNeutralAvgIntegerResultEncodedDescriptor
                     : kNeutralAvgRealResultEncodedDescriptor;

  std::ostringstream packed;
  packed << "gag1|"
         << HexEncode(binding.route.item.aggregate_function_uuid) << '|'
         << static_cast<unsigned>(binding.route.item.operation) << '|'
         << HexEncode(binding.route.item.output_alias) << '|'
         << HexEncode(binding.relation_uuid) << '|'
         << HexEncode(binding.relation_descriptor_uuid) << '|'
         << binding.relation_descriptor_generation << '|'
         << HexEncode(binding.source_column.column_uuid) << '|'
         << HexEncode(binding.source_column.type_descriptor_uuid) << '|'
         << HexEncode(binding.source_column.type_descriptor_kind) << '|'
         << HexEncode(binding.source_column.canonical_type_name) << '|'
         << HexEncode(binding.source_column.encoded_type_descriptor) << '|'
         << HexEncode(result_kind) << '|'
         << HexEncode(result_type) << '|'
         << HexEncode(result_descriptor);

  std::ostringstream out;
  out << "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"dml.select_rows\","
         "\"opcode\":\"SBLR_DML_SELECT_ROWS\","
         "\"operation_family\":\"sblr.query.relational.v3\","
         "\"sblr_operation_family\":\"sblr.query.relational.v3\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":true,"
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\","
      << "\"target_object_uuid\":\"" << EscapeJson(binding.relation_uuid)
      << "\",\"target_object_kind\":\"table\","
      << "\"source_uuid\":\"" << EscapeJson(binding.relation_uuid)
      << "\",\"source_kind\":\"table\","
      << "\"dml_surface_variant\":\"global_aggregate_projection_v1\","
      << "\"result_projection\":\"" << kNeutralProjectionMarker << "\","
      << "\"aggregate_function\":\""
      << kFirebirdCanonicalAvgAggregateUuid << "\","
      << "\"projection_count\":\"1\","
      << "\"projection_0\":\"" << EscapeJson(packed.str()) << "\"}";
  return out.str();
}

FirebirdGlobalAggregateViewCreateRoute
ParseFirebirdGlobalAggregateViewCreateRoute(std::string_view firebird_sql) {
  FirebirdGlobalAggregateViewCreateRoute route;
  const std::string sql = StripTerminator(firebird_sql);
  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "CREATE")) return route;

  std::size_t after_create = offset;
  if (ConsumeKeyword(sql, &after_create, "OR")) {
    route.create_or_alter = true;
    if (!ConsumeKeyword(sql, &after_create, "ALTER")) return route;
    offset = after_create;
  }
  if (!ConsumeKeyword(sql, &offset, "VIEW")) return route;

  // Establish bounded AVG-view intent before validating the target identifier.
  // This keeps qualified or otherwise unsupported definitions out of the
  // legacy metadata overlay: every attempted AVG view is either lowered by
  // this standalone parser or rejected here.
  if (const auto as_keyword = FindTopLevelKeyword(sql, "AS", offset)) {
    std::size_t intent_offset = *as_keyword;
    if (ConsumeKeyword(sql, &intent_offset, "AS") &&
        ConsumeKeyword(sql, &intent_offset, "SELECT")) {
      route.attempted = StartsWithAvgCall(
          Trim(std::string_view(sql).substr(intent_offset)));
    }
  }
  const auto view = ReadIdentifier(sql, &offset);
  if (!view || view->value.find('.') != std::string::npos ||
      !ConsumeKeyword(sql, &offset, "AS") ||
      !ConsumeKeyword(sql, &offset, "SELECT")) {
    return route;
  }

  route.attempted = route.attempted || StartsWithAvgCall(
      Trim(std::string_view(sql).substr(offset)));
  const auto from = FindTopLevelKeyword(sql, "FROM", offset);
  if (!from) return route;
  const std::string projection =
      Trim(std::string_view(sql).substr(offset, *from - offset));
  route.attempted = route.attempted || StartsWithAvgCall(projection);
  if (!route.attempted) return route;

  std::size_t projection_offset = 0;
  if (!ConsumeKeyword(projection, &projection_offset, "AVG") ||
      !ConsumeCharacter(projection, &projection_offset, '(')) {
    return route;
  }
  const auto literal = ReadInt32Literal(projection, &projection_offset);
  if (!literal || !ConsumeCharacter(projection, &projection_offset, '*')) {
    return route;
  }
  const auto column = ReadIdentifier(projection, &projection_offset);
  if (!column || column->value.find('.') != std::string::npos ||
      !ConsumeCharacter(projection, &projection_offset, ')') ||
      !ConsumeKeyword(projection, &projection_offset, "AS")) {
    return route;
  }
  const auto alias = ReadIdentifier(projection, &projection_offset);
  SkipWhitespace(projection, &projection_offset);
  if (!alias || alias->value.find('.') != std::string::npos ||
      projection_offset != projection.size()) {
    return route;
  }

  std::size_t source_offset = *from + std::string_view("FROM").size();
  const auto source = ReadIdentifier(sql, &source_offset);
  SkipWhitespace(sql, &source_offset);
  if (!source || source->value.find('.') != std::string::npos ||
      source_offset != sql.size()) {
    return route;
  }

  route.view_name = view->value;
  route.view_name_quoted = view->quoted;
  route.source_relation = source->value;
  route.source_relation_quoted = source->quoted;
  route.source_column = column->value;
  route.source_column_quoted = column->quoted;
  route.int32_literal = *literal;
  route.result_alias = alias->value;
  route.valid = true;
  return route;
}

FirebirdGlobalAggregateViewSelectRoute
ParseFirebirdGlobalAggregateViewSelectRoute(std::string_view firebird_sql) {
  FirebirdGlobalAggregateViewSelectRoute route;
  const std::string sql = StripTerminator(firebird_sql);
  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "SELECT")) return route;
  if (!ConsumeCharacter(sql, &offset, '*')) return route;
  route.attempted = true;
  if (!ConsumeKeyword(sql, &offset, "FROM")) return route;
  const auto view = ReadIdentifier(sql, &offset);
  SkipWhitespace(sql, &offset);
  if (!view || view->value.find('.') != std::string::npos ||
      offset != sql.size()) {
    return route;
  }
  route.view_name = view->value;
  route.view_name_quoted = view->quoted;
  route.valid = true;
  return route;
}

FirebirdBoundGlobalAggregateViewCreate BindFirebirdGlobalAggregateViewCreate(
    const FirebirdGlobalAggregateViewCreateRoute& route,
    const ipc::PublicNameResolutionResult& resolved_schema,
    const ipc::PublicNameResolutionResult& resolved_relation) {
  FirebirdBoundGlobalAggregateViewCreate binding;
  binding.route = route;
  if (!route.recognized()) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE_VIEW.SHAPE_UNSUPPORTED",
                  "The standalone Firebird aggregate-view binder supports only the bounded AVG(int32_literal * int32_field) shape.");
    return binding;
  }
  if (!resolved_schema.resolved || resolved_schema.object_uuid.empty() ||
      ToUpperAscii(resolved_schema.object_class) != "SCHEMA") {
    binding.messages = resolved_schema.messages;
    RejectBinding(&binding, "FIREBIRD.AGGREGATE_VIEW.SCHEMA_REQUIRED",
                  "The exact engine-owned target schema resolution is required.");
    return binding;
  }

  const auto& descriptor = resolved_relation.relation_descriptor;
  if (!resolved_relation.resolved || resolved_relation.object_uuid.empty() ||
      ToUpperAscii(resolved_relation.object_class) != "TABLE" ||
      !descriptor.present || descriptor.descriptor_uuid.empty() ||
      descriptor.relation_uuid != resolved_relation.object_uuid ||
      descriptor.descriptor_generation == 0 ||
      descriptor.validated_resource_epoch == 0 || descriptor.columns.empty()) {
    binding.messages = resolved_relation.messages;
    RejectBinding(&binding,
                  "FIREBIRD.AGGREGATE_VIEW.RELATION_DESCRIPTOR_REQUIRED",
                  "The exact engine-owned source relation descriptor is required.");
    return binding;
  }

  const ipc::PublicRelationColumnDescriptor* match = nullptr;
  for (const auto& column : descriptor.columns) {
    const bool matches = route.source_column_quoted
                             ? column.canonical_name_key == route.source_column
                             : ToUpperAscii(column.canonical_name_key) ==
                                   ToUpperAscii(route.source_column);
    if (!matches) continue;
    if (match != nullptr) {
      RejectBinding(&binding,
                    "FIREBIRD.AGGREGATE_VIEW.COLUMN_AMBIGUOUS",
                    "The source descriptor contains an ambiguous aggregate-view column binding.");
      return binding;
    }
    match = &column;
  }
  if (match == nullptr || !CompleteColumnDescriptor(*match)) {
    RejectBinding(&binding,
                  "FIREBIRD.AGGREGATE_VIEW.COLUMN_DESCRIPTOR_REQUIRED",
                  "A complete engine-owned source column descriptor is required.");
    return binding;
  }
  if (!FirebirdInt32Descriptor(*match)) {
    RejectBinding(&binding,
                  "FIREBIRD.AGGREGATE_VIEW.SOURCE_TYPE_UNSUPPORTED",
                  "The bounded aggregate-view expression requires an int32 source descriptor.");
    return binding;
  }

  binding.schema_uuid = resolved_schema.object_uuid;
  binding.relation_uuid = resolved_relation.object_uuid;
  binding.relation_descriptor_uuid = descriptor.descriptor_uuid;
  binding.relation_descriptor_generation = descriptor.descriptor_generation;
  binding.source_column = *match;
  binding.accepted = true;
  return binding;
}

FirebirdBoundGlobalAggregateViewSelect BindFirebirdGlobalAggregateViewSelect(
    const FirebirdGlobalAggregateViewSelectRoute& route,
    const ipc::PublicNameResolutionResult& resolved_view) {
  FirebirdBoundGlobalAggregateViewSelect binding;
  binding.route = route;
  if (!route.recognized()) {
    RejectBinding(&binding, "FIREBIRD.AGGREGATE_VIEW.SELECT_SHAPE_UNSUPPORTED",
                  "The standalone Firebird aggregate-view selector supports only SELECT * FROM view.");
    return binding;
  }
  if (!resolved_view.resolved || resolved_view.object_uuid.empty() ||
      ToUpperAscii(resolved_view.object_class) != "VIEW") {
    binding.messages = resolved_view.messages;
    RejectBinding(&binding, "FIREBIRD.AGGREGATE_VIEW.VIEW_REQUIRED",
                  "The exact engine-owned view resolution is required.");
    return binding;
  }

  const auto parts = SplitPacket(resolved_view.resolution_detail);
  if (parts.size() != 11 || parts[0] != "gavs1") {
    RejectBinding(&binding,
                  "FIREBIRD.AGGREGATE_VIEW.SEMANTIC_DESCRIPTOR_REQUIRED",
                  "The resolved view does not expose the bounded aggregate-view semantic descriptor.");
    return binding;
  }
  std::string marker;
  std::string descriptor_uuid;
  std::string descriptor_kind;
  std::string canonical_type;
  std::string encoded_descriptor;
  std::string result_alias;
  std::string result_kind;
  std::string result_type;
  std::string result_descriptor;
  std::uint64_t generation = 0;
  if (!HexDecode(parts[1], &marker) ||
      !HexDecode(parts[2], &descriptor_uuid) ||
      !ParseStrictU64(parts[3], &generation) || generation == 0 ||
      !HexDecode(parts[4], &descriptor_kind) ||
      !HexDecode(parts[5], &canonical_type) ||
      !HexDecode(parts[6], &encoded_descriptor) ||
      !HexDecode(parts[7], &result_alias) ||
      !HexDecode(parts[8], &result_kind) ||
      !HexDecode(parts[9], &result_type) ||
      !HexDecode(parts[10], &result_descriptor) ||
      marker != kFirebirdGlobalAggregateViewMarkerV1 ||
      descriptor_uuid.empty() || descriptor_kind != "global_aggregate_view" ||
      canonical_type != kFirebirdGlobalAggregateViewMarkerV1 ||
      result_alias.empty() ||
      result_kind != kNeutralAvgIntegerResultDescriptorKind ||
      result_type != kNeutralAvgIntegerResultCanonicalType ||
      result_descriptor != kNeutralAvgIntegerResultEncodedDescriptor) {
    RejectBinding(&binding,
                  "FIREBIRD.AGGREGATE_VIEW.SEMANTIC_DESCRIPTOR_INVALID",
                  "The resolved aggregate-view semantic descriptor is malformed.");
    return binding;
  }

  const std::string expected_descriptor =
      std::string("marker=") +
      std::string(kFirebirdGlobalAggregateViewMarkerV1) +
      ";view_uuid=" + resolved_view.object_uuid +
      ";view_descriptor_generation=" + std::to_string(generation) +
      ";result_alias=" + result_alias +
      ";result_type=int64;result_nullable=true";
  if (encoded_descriptor != expected_descriptor) {
    RejectBinding(&binding,
                  "FIREBIRD.AGGREGATE_VIEW.SEMANTIC_DESCRIPTOR_STALE",
                  "The resolved aggregate-view semantic descriptor does not match the selected view identity and generation.");
    return binding;
  }

  binding.view_uuid = resolved_view.object_uuid;
  binding.view_descriptor_uuid = std::move(descriptor_uuid);
  binding.view_descriptor_generation = generation;
  binding.result_alias = std::move(result_alias);
  binding.result_kind = FirebirdGlobalAvgResultKind::kNullableInt64;
  binding.semantic_transport = resolved_view.resolution_detail;
  binding.accepted = true;
  return binding;
}

std::string EncodeFirebirdGlobalAggregateViewCreateEnvelope(
    const FirebirdBoundGlobalAggregateViewCreate& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      binding.schema_uuid.empty() || binding.relation_uuid.empty() ||
      binding.relation_descriptor_uuid.empty() ||
      binding.relation_descriptor_generation == 0 ||
      !CompleteColumnDescriptor(binding.source_column)) {
    return {};
  }

  std::ostringstream packed;
  packed << "gavc1|" << (binding.route.create_or_alter ? '1' : '0') << '|'
         << HexEncode(binding.relation_uuid) << '|'
         << HexEncode(binding.relation_descriptor_uuid) << '|'
         << binding.relation_descriptor_generation << '|'
         << HexEncode(binding.source_column.column_uuid) << '|'
         << HexEncode(binding.source_column.type_descriptor_uuid) << '|'
         << HexEncode(binding.source_column.type_descriptor_kind) << '|'
         << HexEncode(binding.source_column.canonical_type_name) << '|'
         << HexEncode(binding.source_column.encoded_type_descriptor) << '|'
         << HexEncode(kFirebirdGlobalAggregateViewInt32MultiplyV1) << '|'
         << HexEncode(kNeutralExpressionLiteralDescriptorKind) << '|'
         << HexEncode(kNeutralExpressionLiteralCanonicalType) << '|'
         << HexEncode(kNeutralExpressionLiteralEncodedDescriptor) << '|'
         << HexEncode(std::to_string(binding.route.int32_literal)) << '|'
         << HexEncode(kNeutralExpressionResultDescriptorKind) << '|'
         << HexEncode(kNeutralExpressionResultCanonicalType) << '|'
         << HexEncode(kNeutralExpressionResultEncodedDescriptor) << '|'
         << HexEncode(kFirebirdCanonicalAvgAggregateUuid) << '|'
         << HexEncode(binding.route.result_alias) << '|'
         << HexEncode(kNeutralAvgIntegerResultDescriptorKind) << '|'
         << HexEncode(kNeutralAvgIntegerResultCanonicalType) << '|'
         << HexEncode(kNeutralAvgIntegerResultEncodedDescriptor);

  std::ostringstream out;
  out << "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"ddl.create_view\","
         "\"opcode\":\"SBLR_DDL_CREATE_VIEW\","
         "\"operation_family\":\"sblr.catalog.mutation.v3\","
         "\"sblr_operation_family\":\"sblr.catalog.mutation.v3\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":true,"
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\","
      << "\"target_schema_uuid\":\"" << EscapeJson(binding.schema_uuid)
      << "\",\"view_name\":\"" << EscapeJson(binding.route.view_name)
      << "\",\"view_query_shape\":\""
      << kFirebirdGlobalAggregateViewMarkerV1
      << "\",\"view_source_uuid\":\""
      << EscapeJson(binding.relation_uuid)
      << "\",\"view_projection_count\":\"1\","
      << "\"view_projection_0\":\"" << EscapeJson(packed.str())
      << "\"}";
  return out.str();
}

std::string EncodeFirebirdGlobalAggregateViewSelectEnvelope(
    const FirebirdBoundGlobalAggregateViewSelect& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      binding.view_uuid.empty() || binding.view_descriptor_uuid.empty() ||
      binding.view_descriptor_generation == 0 ||
      binding.result_alias.empty() || binding.semantic_transport.empty() ||
      binding.result_kind != FirebirdGlobalAvgResultKind::kNullableInt64) {
    return {};
  }

  std::ostringstream out;
  out << "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"dml.select_rows\","
         "\"opcode\":\"SBLR_DML_SELECT_ROWS\","
         "\"operation_family\":\"sblr.query.relational.v3\","
         "\"sblr_operation_family\":\"sblr.query.relational.v3\","
         "\"result_shape\":\"engine.api.result.v1\","
         "\"diagnostic_shape\":\"engine.diagnostic.v1\","
         "\"parser_resolved_names_to_uuids\":true,"
         "\"requires_security_context\":true,"
         "\"requires_transaction_context\":true,"
         "\"requires_cluster_authority\":false,"
         "\"contains_sql_text\":false,"
         "\"identifier_profile_uuid\":\"firebird_v5\","
         "\"source_dialect\":\"firebird\","
      << "\"target_object_uuid\":\"" << EscapeJson(binding.view_uuid)
      << "\",\"target_object_kind\":\"view\","
      << "\"source_uuid\":\"" << EscapeJson(binding.view_uuid)
      << "\",\"source_kind\":\"view\","
      << "\"result_projection\":\""
      << kFirebirdGlobalAggregateViewMarkerV1
      << "\",\"projection_count\":\"1\","
      << "\"projection_0\":\""
      << EscapeJson(binding.semantic_transport) << "\"}";
  return out.str();
}

std::string_view FirebirdGlobalCountProjectionOperationName(
    FirebirdGlobalCountProjectionOperation operation) {
  switch (operation) {
    case FirebirdGlobalCountProjectionOperation::kCountStar:
      return "count_star";
    case FirebirdGlobalCountProjectionOperation::kCountNonNullField:
      return "count_non_null_field";
    case FirebirdGlobalCountProjectionOperation::kCountDistinctField:
      return "count_distinct_field";
    case FirebirdGlobalCountProjectionOperation::kUnsupported:
      break;
  }
  return "unsupported";
}

std::string_view FirebirdGlobalAvgProjectionOperationName(
    FirebirdGlobalAvgProjectionOperation operation) {
  switch (operation) {
    case FirebirdGlobalAvgProjectionOperation::kAvgField:
      return "avg_field";
    case FirebirdGlobalAvgProjectionOperation::kAvgDistinctField:
      return "avg_distinct_field";
    case FirebirdGlobalAvgProjectionOperation::kUnsupported:
      break;
  }
  return "unsupported";
}

}  // namespace scratchbird::parser::firebird
