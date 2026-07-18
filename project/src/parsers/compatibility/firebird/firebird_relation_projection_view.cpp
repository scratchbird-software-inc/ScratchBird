// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_relation_projection_view.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <optional>
#include <sstream>
#include <utility>

namespace scratchbird::parser::firebird {
namespace {

struct Identifier {
  std::string value;
  bool quoted{false};
};

bool ContainsSqlComment(std::string_view text) {
  bool quoted_identifier = false;
  bool string_literal = false;
  for (std::size_t index = 0; index + 1u < text.size(); ++index) {
    const char ch = text[index];
    if (string_literal) {
      if (ch == '\'' && text[index + 1u] == '\'') {
        ++index;
      } else if (ch == '\'') {
        string_literal = false;
      }
      continue;
    }
    if (quoted_identifier) {
      if (ch == '"' && text[index + 1u] == '"') {
        ++index;
      } else if (ch == '"') {
        quoted_identifier = false;
      }
      continue;
    }
    if (ch == '\'') {
      string_literal = true;
      continue;
    }
    if (ch == '"') {
      quoted_identifier = true;
      continue;
    }
    if ((ch == '-' && text[index + 1u] == '-') ||
        (ch == '/' && text[index + 1u] == '*')) {
      return true;
    }
  }
  return false;
}

std::string StripSqlComments(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool quoted_identifier = false;
  bool string_literal = false;
  for (std::size_t index = 0; index < text.size();) {
    const char ch = text[index];
    if (string_literal) {
      out.push_back(ch);
      ++index;
      if (ch == '\'' && index < text.size() && text[index] == '\'') {
        out.push_back(text[index++]);
      } else if (ch == '\'') {
        string_literal = false;
      }
      continue;
    }
    if (quoted_identifier) {
      out.push_back(ch);
      ++index;
      if (ch == '"' && index < text.size() && text[index] == '"') {
        out.push_back(text[index++]);
      } else if (ch == '"') {
        quoted_identifier = false;
      }
      continue;
    }
    if (ch == '\'') {
      string_literal = true;
      out.push_back(ch);
      ++index;
      continue;
    }
    if (ch == '"') {
      quoted_identifier = true;
      out.push_back(ch);
      ++index;
      continue;
    }
    if (index + 1u < text.size() && ch == '-' &&
        text[index + 1u] == '-') {
      out.push_back(' ');
      index += 2u;
      while (index < text.size() && text[index] != '\n' &&
             text[index] != '\r') {
        ++index;
      }
      continue;
    }
    if (index + 1u < text.size() && ch == '/' &&
        text[index + 1u] == '*') {
      out.push_back(' ');
      index += 2u;
      while (index + 1u < text.size() &&
             !(text[index] == '*' && text[index + 1u] == '/')) {
        ++index;
      }
      if (index + 1u < text.size()) index += 2u;
      continue;
    }
    out.push_back(ch);
    ++index;
  }
  return out;
}

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

std::string UpperAscii(std::string_view value) {
  std::string out(value);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return out;
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

bool SafeUnquotedOutputName(std::string_view value) {
  if (value.empty() || value.size() > 63u || value.find('.') != value.npos ||
      std::isdigit(static_cast<unsigned char>(value.front())) != 0 ||
      UpperAscii(value) != value) {
    return false;
  }
  for (const char ch : value) {
    if (!IdentifierCharacter(ch)) return false;
  }
  return true;
}

bool CanonicalNonzeroUuid(std::string_view value) {
  if (value.size() != 36u || value[8] != '-' || value[13] != '-' ||
      value[18] != '-' || value[23] != '-') {
    return false;
  }
  bool nonzero = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (index == 8u || index == 13u || index == 18u || index == 23u) {
      continue;
    }
    const char ch = value[index];
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
    nonzero = nonzero || ch != '0';
  }
  return nonzero;
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
  Identifier result;
  if (text[*offset] == '"') {
    result.quoted = true;
    ++*offset;
    bool closed = false;
    while (*offset < text.size()) {
      const char ch = text[*offset];
      ++*offset;
      if (ch != '"') {
        result.value.push_back(ch);
        continue;
      }
      if (*offset < text.size() && text[*offset] == '"') {
        result.value.push_back('"');
        ++*offset;
        continue;
      }
      closed = true;
      break;
    }
    if (!closed || result.value.empty()) return std::nullopt;
    return result;
  }
  const std::size_t begin = *offset;
  if (!IdentifierCharacter(text[*offset]) ||
      std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    return std::nullopt;
  }
  while (*offset < text.size() && IdentifierCharacter(text[*offset])) {
    ++*offset;
  }
  result.value = UpperAscii(text.substr(begin, *offset - begin));
  return result;
}

std::optional<std::size_t> FindTopLevelKeyword(std::string_view text,
                                               std::string_view keyword,
                                               std::size_t offset) {
  std::size_t depth = 0;
  bool quoted_identifier = false;
  bool string_literal = false;
  for (std::size_t index = offset; index < text.size(); ++index) {
    const char ch = text[index];
    if (string_literal) {
      if (ch == '\'' && index + 1 < text.size() && text[index + 1] == '\'') {
        ++index;
      } else if (ch == '\'') {
        string_literal = false;
      }
      continue;
    }
    if (quoted_identifier) {
      if (ch == '"' && index + 1 < text.size() && text[index + 1] == '"') {
        ++index;
      } else if (ch == '"') {
        quoted_identifier = false;
      }
      continue;
    }
    if (ch == '\'') {
      string_literal = true;
      continue;
    }
    if (ch == '"') {
      quoted_identifier = true;
      continue;
    }
    if (ch == '(') {
      ++depth;
      continue;
    }
    if (ch == ')') {
      if (depth == 0) return std::nullopt;
      --depth;
      continue;
    }
    if (depth != 0 || index + keyword.size() > text.size()) continue;
    const bool left_boundary =
        index == 0 || !IdentifierCharacter(text[index - 1]);
    const bool right_boundary =
        index + keyword.size() == text.size() ||
        !IdentifierCharacter(text[index + keyword.size()]);
    if (!left_boundary || !right_boundary) continue;
    bool matches = true;
    for (std::size_t part = 0; part < keyword.size(); ++part) {
      if (std::toupper(static_cast<unsigned char>(text[index + part])) !=
          std::toupper(static_cast<unsigned char>(keyword[part]))) {
        matches = false;
        break;
      }
    }
    if (matches) return index;
  }
  return std::nullopt;
}

std::vector<std::string> SplitTopLevel(std::string_view value) {
  std::vector<std::string> parts;
  std::size_t begin = 0;
  std::size_t depth = 0;
  bool quoted_identifier = false;
  bool string_literal = false;
  for (std::size_t index = 0; index < value.size(); ++index) {
    const char ch = value[index];
    if (string_literal) {
      if (ch == '\'' && index + 1 < value.size() && value[index + 1] == '\'') {
        ++index;
      } else if (ch == '\'') {
        string_literal = false;
      }
      continue;
    }
    if (quoted_identifier) {
      if (ch == '"' && index + 1 < value.size() && value[index + 1] == '"') {
        ++index;
      } else if (ch == '"') {
        quoted_identifier = false;
      }
      continue;
    }
    if (ch == '\'') {
      string_literal = true;
    } else if (ch == '"') {
      quoted_identifier = true;
    } else if (ch == '(') {
      ++depth;
    } else if (ch == ')' && depth != 0) {
      --depth;
    } else if (ch == ',' && depth == 0) {
      parts.push_back(Trim(value.substr(begin, index - begin)));
      begin = index + 1;
    }
  }
  parts.push_back(Trim(value.substr(begin)));
  return parts;
}

std::optional<std::int32_t> ReadInt32(std::string_view text,
                                      std::size_t* offset) {
  if (offset == nullptr) return std::nullopt;
  SkipWhitespace(text, offset);
  const std::size_t begin = *offset;
  if (*offset < text.size() &&
      (text[*offset] == '+' || text[*offset] == '-')) {
    ++*offset;
  }
  const std::size_t digits = *offset;
  while (*offset < text.size() &&
         std::isdigit(static_cast<unsigned char>(text[*offset])) != 0) {
    ++*offset;
  }
  if (digits == *offset) {
    *offset = begin;
    return std::nullopt;
  }
  const bool negative = text[begin] == '-';
  const std::size_t magnitude_begin =
      text[begin] == '+' || text[begin] == '-' ? begin + 1u : begin;
  std::uint64_t magnitude = 0;
  const auto source = text.substr(magnitude_begin, *offset - magnitude_begin);
  const auto [end, error] = std::from_chars(
      source.data(), source.data() + source.size(), magnitude);
  const std::uint64_t limit =
      negative
          ? static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) +
                1u
          : static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max());
  if (error != std::errc{} || end != source.data() + source.size() ||
      magnitude > limit) {
    return std::nullopt;
  }
  const std::int64_t parsed = negative
                                  ? -static_cast<std::int64_t>(magnitude)
                                  : static_cast<std::int64_t>(magnitude);
  return static_cast<std::int32_t>(parsed);
}

bool ParseAliasedIdentifier(std::string_view text,
                            Identifier* source,
                            std::optional<Identifier>* alias) {
  if (source == nullptr || alias == nullptr) return false;
  std::size_t offset = 0;
  const auto parsed = ReadIdentifier(text, &offset);
  if (!parsed || parsed->value.find('.') != std::string::npos) return false;
  *source = *parsed;
  std::size_t alias_offset = offset;
  if (ConsumeKeyword(text, &alias_offset, "AS")) {
    const auto parsed_alias = ReadIdentifier(text, &alias_offset);
    SkipWhitespace(text, &alias_offset);
    if (!parsed_alias || alias_offset != text.size()) return false;
    *alias = *parsed_alias;
    return true;
  }
  SkipWhitespace(text, &offset);
  return offset == text.size();
}

bool ParseAliasedInt32(std::string_view text,
                       std::int32_t* literal,
                       std::optional<Identifier>* alias) {
  if (literal == nullptr || alias == nullptr) return false;
  std::size_t offset = 0;
  const auto parsed = ReadInt32(text, &offset);
  if (!parsed) return false;
  *literal = *parsed;
  std::size_t alias_offset = offset;
  if (ConsumeKeyword(text, &alias_offset, "AS")) {
    const auto parsed_alias = ReadIdentifier(text, &alias_offset);
    SkipWhitespace(text, &alias_offset);
    if (!parsed_alias || alias_offset != text.size()) return false;
    *alias = *parsed_alias;
    return true;
  }
  SkipWhitespace(text, &offset);
  return offset == text.size();
}

bool CompleteColumnDescriptor(
    const ipc::PublicRelationColumnDescriptor& column) {
  return !column.column_uuid.empty() && !column.canonical_name_key.empty() &&
         !column.type_descriptor_uuid.empty() &&
         !column.type_descriptor_kind.empty() &&
         !column.canonical_type_name.empty() &&
         !column.encoded_type_descriptor.empty();
}

bool Int32Descriptor(const ipc::PublicRelationColumnDescriptor& column) {
  const std::string type = UpperAscii(Trim(column.canonical_type_name));
  return type == "INTEGER" || type == "INT" || type == "INT32";
}

bool Int32Descriptor(std::string_view kind, std::string_view type) {
  const std::string canonical = UpperAscii(Trim(type));
  return !kind.empty() &&
         (canonical == "INTEGER" || canonical == "INT" ||
          canonical == "INT32");
}

void Reject(FirebirdBoundRelationProjectionViewCreate* binding,
            std::string code,
            std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.relation_projection_view"));
}

void Reject(FirebirdBoundRelationProjectionViewSelect* binding,
            std::string code,
            std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.relation_projection_view"));
}

void Reject(FirebirdBoundRelationProjectionViewCreateV2* binding,
            std::string code,
            std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.relation_projection_view.v2"));
}

void Reject(FirebirdBoundRelationProjectionViewDeleteV2* binding,
            std::string code,
            std::string detail) {
  if (binding == nullptr) return;
  binding->accepted = false;
  binding->messages.diagnostics.push_back(ipc::MakeDiagnostic(
      std::move(code), "ERROR", std::move(detail),
      "sbp_firebird.relation_projection_view.v2"));
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

bool HexDecode(std::string_view encoded, std::string* value) {
  if (value == nullptr || encoded.size() % 2u != 0u) return false;
  auto nibble = [](char ch) -> int {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  };
  value->clear();
  value->reserve(encoded.size() / 2u);
  for (std::size_t index = 0; index < encoded.size(); index += 2u) {
    const int high = nibble(encoded[index]);
    const int low = nibble(encoded[index + 1u]);
    if (high < 0 || low < 0) return false;
    value->push_back(static_cast<char>((high << 4) | low));
  }
  return true;
}

std::vector<std::string> SplitPacket(std::string_view value) {
  std::vector<std::string> parts;
  std::size_t offset = 0;
  while (offset <= value.size()) {
    const auto next = value.find('|', offset);
    if (next == std::string_view::npos) {
      parts.emplace_back(value.substr(offset));
      break;
    }
    parts.emplace_back(value.substr(offset, next - offset));
    offset = next + 1u;
  }
  return parts;
}

bool ParseStrictU64(std::string_view value, std::uint64_t* parsed) {
  if (parsed == nullptr || value.empty() ||
      (value.size() > 1u && value.front() == '0')) {
    return false;
  }
  std::uint64_t next = 0;
  const auto [end, error] =
      std::from_chars(value.data(), value.data() + value.size(), next);
  if (error != std::errc{} || end != value.data() + value.size()) return false;
  *parsed = next;
  return true;
}

std::string EscapeJson(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const unsigned char byte : value) {
    switch (byte) {
      case '"': escaped += "\\\""; break;
      case '\\': escaped += "\\\\"; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (byte < 0x20u) return {};
        escaped.push_back(static_cast<char>(byte));
        break;
    }
  }
  return escaped;
}

std::string PackCreateProjection(
    const FirebirdBoundRelationProjectionViewCreate& binding,
    std::uint32_t ordinal) {
  const bool source = ordinal == 0;
  const std::string output_name =
      source ? binding.route.source_output_name
             : binding.route.literal_output_name;
  const std::string expression_kind =
      source ? "source_column" : "typed_int32_literal";
  const std::string source_column_uuid =
      source ? binding.source_column.column_uuid : std::string{};
  const std::string source_descriptor_uuid =
      source ? binding.source_column.type_descriptor_uuid : std::string{};
  const std::string descriptor_kind =
      source ? binding.source_column.type_descriptor_kind
             : std::string(kFirebirdRelationProjectionViewInt32Kind);
  const std::string canonical_type =
      source ? binding.source_column.canonical_type_name
             : std::string(kFirebirdRelationProjectionViewInt32Type);
  const std::string encoded_descriptor =
      source ? binding.source_column.encoded_type_descriptor
             : std::string(
                   kFirebirdRelationProjectionViewInt32NotNullDescriptor);
  const bool nullable = source && binding.source_column.nullable;
  const std::string value =
      source ? std::string{} : std::to_string(binding.route.literal_value);

  std::ostringstream packed;
  packed << kFirebirdRelationProjectionViewCreatePacketV1 << "|0|"
         << ordinal << '|' << HexEncode(binding.relation_uuid) << '|'
         << HexEncode(binding.relation_descriptor_uuid) << '|'
         << binding.relation_descriptor_generation << '|'
         << binding.validated_resource_epoch << '|'
         << HexEncode(output_name) << '|' << HexEncode(expression_kind) << '|'
         << HexEncode(source_column_uuid) << '|'
         << HexEncode(source_descriptor_uuid) << '|'
         << HexEncode(descriptor_kind) << '|'
         << HexEncode(canonical_type) << '|'
         << HexEncode(encoded_descriptor) << '|' << (nullable ? '1' : '0')
         << '|' << HexEncode(value);
  return packed.str();
}

bool SafeV2OutputName(std::string_view value, bool quoted) {
  if (value.empty() || value.size() > 63u || value.find('.') != value.npos) {
    return false;
  }
  if (!quoted) return SafeUnquotedOutputName(value);
  for (const unsigned char ch : value) {
    if (ch < 0x20u || ch == 0x7fu) return false;
  }
  return true;
}

std::string PackCreateProjectionV2(
    const FirebirdBoundRelationProjectionViewCreateV2& binding) {
  std::ostringstream packed;
  packed << kFirebirdRelationProjectionViewCreatePacketV2 << "|0|0|"
         << HexEncode(binding.relation_uuid) << '|'
         << HexEncode(binding.relation_descriptor_uuid) << '|'
         << binding.relation_descriptor_generation << '|'
         << binding.validated_resource_epoch << '|'
         << HexEncode(binding.route.output_name) << '|'
         << HexEncode(binding.route.source_column) << '|'
         << HexEncode(binding.source_column.column_uuid) << '|'
         << HexEncode(binding.source_column.type_descriptor_uuid) << '|'
         << HexEncode(binding.source_column.type_descriptor_kind) << '|'
         << HexEncode(binding.source_column.canonical_type_name) << '|'
         << HexEncode(binding.source_column.encoded_type_descriptor) << '|'
         << (binding.source_column.nullable ? '1' : '0') << '|'
         << HexEncode(std::string_view{});
  return packed.str();
}

}  // namespace

FirebirdRelationProjectionViewCreateRoute
ParseFirebirdRelationProjectionViewCreateRoute(std::string_view firebird_sql) {
  FirebirdRelationProjectionViewCreateRoute route;
  if (ContainsSqlComment(firebird_sql)) {
    auto classified = ParseFirebirdRelationProjectionViewCreateRoute(
        StripSqlComments(firebird_sql));
    if (classified.attempted) {
      classified.valid = false;
      return classified;
    }
    return route;
  }
  const std::string sql = StripTerminator(firebird_sql);
  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "CREATE") ||
      !ConsumeKeyword(sql, &offset, "VIEW")) {
    return route;
  }
  const auto view = ReadIdentifier(sql, &offset);
  if (!view || view->value.find('.') != std::string::npos) return route;

  std::vector<Identifier> explicit_names;
  std::size_t list_offset = offset;
  if (ConsumeCharacter(sql, &list_offset, '(')) {
    while (true) {
      const auto name = ReadIdentifier(sql, &list_offset);
      if (!name || name->value.find('.') != std::string::npos) return route;
      explicit_names.push_back(*name);
      if (ConsumeCharacter(sql, &list_offset, ')')) break;
      if (!ConsumeCharacter(sql, &list_offset, ',')) return route;
    }
    offset = list_offset;
  }
  if (!ConsumeKeyword(sql, &offset, "AS") ||
      !ConsumeKeyword(sql, &offset, "SELECT")) {
    return route;
  }
  const auto from = FindTopLevelKeyword(sql, "FROM", offset);
  if (!from) return route;
  const auto projections =
      SplitTopLevel(std::string_view(sql).substr(offset, *from - offset));
  // Establish bounded intent without claiming unrelated CREATE VIEW shapes.
  if (projections.size() != 2u) return route;
  std::size_t literal_probe = 0;
  route.attempted = ReadInt32(projections[1], &literal_probe).has_value();
  if (!route.attempted) return route;
  // This bounded route deliberately refuses comments.  Its small scanner is
  // self-contained and must never let comment text redirect the exact shape
  // into the parser-local legacy view overlay.
  if (view->quoted) return route;

  Identifier source_column;
  std::optional<Identifier> source_alias;
  std::int32_t literal = 0;
  std::optional<Identifier> literal_alias;
  if (!ParseAliasedIdentifier(projections[0], &source_column, &source_alias) ||
      !ParseAliasedInt32(projections[1], &literal, &literal_alias)) {
    return route;
  }
  if (literal != 5) return route;
  if (!explicit_names.empty() && explicit_names.size() != 2u) return route;
  if (explicit_names.empty() && !literal_alias) return route;
  // Quoted output identities require a wider canonical-name/presentation-name
  // descriptor than this first V1 packet.  Reject instead of losing quote
  // semantics or silently folding the name.
  if ((!explicit_names.empty() &&
       (explicit_names[0].quoted || explicit_names[1].quoted)) ||
      (source_alias && source_alias->quoted) ||
      (literal_alias && literal_alias->quoted) ||
      (explicit_names.empty() && !source_alias && source_column.quoted)) {
    return route;
  }

  std::size_t source_offset = *from + std::string_view("FROM").size();
  const auto source = ReadIdentifier(sql, &source_offset);
  SkipWhitespace(sql, &source_offset);
  if (!source || source->value.find('.') != std::string::npos ||
      source_offset != sql.size()) {
    return route;
  }

  const std::string source_name =
      explicit_names.empty()
          ? (source_alias ? source_alias->value : source_column.value)
          : explicit_names[0].value;
  const std::string literal_name =
      explicit_names.empty() ? literal_alias->value : explicit_names[1].value;
  if (source_name.empty() || literal_name.empty() ||
      !SafeUnquotedOutputName(source_name) ||
      !SafeUnquotedOutputName(literal_name) ||
      UpperAscii(source_name) == UpperAscii(literal_name)) {
    return route;
  }

  route.view_name = view->value;
  route.view_name_quoted = view->quoted;
  route.explicit_output_names = !explicit_names.empty();
  route.source_relation = source->value;
  route.source_relation_quoted = source->quoted;
  route.source_column = source_column.value;
  route.source_column_quoted = source_column.quoted;
  route.source_output_name = source_name;
  route.literal_output_name = literal_name;
  route.literal_value = literal;
  route.valid = true;
  return route;
}

FirebirdBoundRelationProjectionViewCreate
BindFirebirdRelationProjectionViewCreate(
    const FirebirdRelationProjectionViewCreateRoute& route,
    const ipc::PublicNameResolutionResult& resolved_schema,
    const ipc::PublicNameResolutionResult& resolved_relation) {
  FirebirdBoundRelationProjectionViewCreate binding;
  binding.route = route;
  if (!route.recognized()) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SHAPE_UNSUPPORTED",
           "The standalone Firebird relation-view binder requires one int32 source column followed by one int32 literal.");
    return binding;
  }
  if (!resolved_schema.resolved || resolved_schema.object_uuid.empty() ||
      !CanonicalNonzeroUuid(resolved_schema.object_uuid) ||
      UpperAscii(resolved_schema.object_class) != "SCHEMA") {
    binding.messages = resolved_schema.messages;
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SCHEMA_REQUIRED",
           "The exact engine-owned target schema resolution is required.");
    return binding;
  }
  const auto& descriptor = resolved_relation.relation_descriptor;
  if (!resolved_relation.resolved || resolved_relation.object_uuid.empty() ||
      !CanonicalNonzeroUuid(resolved_relation.object_uuid) ||
      UpperAscii(resolved_relation.object_class) != "TABLE" ||
      !descriptor.present || descriptor.descriptor_uuid.empty() ||
      !CanonicalNonzeroUuid(descriptor.descriptor_uuid) ||
      descriptor.relation_uuid != resolved_relation.object_uuid ||
      descriptor.descriptor_uuid == descriptor.relation_uuid ||
      descriptor.descriptor_generation == 0 ||
      descriptor.validated_resource_epoch == 0 || descriptor.columns.empty()) {
    binding.messages = resolved_relation.messages;
    Reject(&binding, "FIREBIRD.RELATION_VIEW.RELATION_DESCRIPTOR_REQUIRED",
           "The exact engine-owned source relation descriptor is required.");
    return binding;
  }

  const ipc::PublicRelationColumnDescriptor* match = nullptr;
  for (const auto& column : descriptor.columns) {
    const bool matches = route.source_column_quoted
                             ? column.canonical_name_key == route.source_column
                             : UpperAscii(column.canonical_name_key) ==
                                   UpperAscii(route.source_column);
    if (!matches) continue;
    if (match != nullptr) {
      Reject(&binding, "FIREBIRD.RELATION_VIEW.COLUMN_AMBIGUOUS",
             "The source descriptor contains an ambiguous relation-view column binding.");
      return binding;
    }
    match = &column;
  }
  if (match == nullptr || !CompleteColumnDescriptor(*match)) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.COLUMN_DESCRIPTOR_REQUIRED",
           "A complete engine-owned source column descriptor is required.");
    return binding;
  }
  if (!CanonicalNonzeroUuid(match->column_uuid) ||
      !CanonicalNonzeroUuid(match->type_descriptor_uuid) ||
      match->column_uuid == match->type_descriptor_uuid ||
      match->column_uuid == descriptor.relation_uuid ||
      match->column_uuid == descriptor.descriptor_uuid ||
      match->type_descriptor_uuid == descriptor.relation_uuid ||
      match->type_descriptor_uuid == descriptor.descriptor_uuid) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.COLUMN_IDENTITY_INVALID",
           "The source column and type descriptor must expose distinct canonical engine UUID identities.");
    return binding;
  }
  if (!Int32Descriptor(*match)) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SOURCE_TYPE_UNSUPPORTED",
           "The bounded relation-view source column must have an int32 descriptor.");
    return binding;
  }

  binding.schema_uuid = resolved_schema.object_uuid;
  binding.relation_uuid = resolved_relation.object_uuid;
  binding.relation_descriptor_uuid = descriptor.descriptor_uuid;
  binding.relation_descriptor_generation = descriptor.descriptor_generation;
  binding.validated_resource_epoch = descriptor.validated_resource_epoch;
  binding.source_column = *match;
  binding.accepted = true;
  return binding;
}

std::string EncodeFirebirdRelationProjectionViewCreateEnvelope(
    const FirebirdBoundRelationProjectionViewCreate& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      binding.schema_uuid.empty() || binding.relation_uuid.empty() ||
      binding.relation_descriptor_uuid.empty() ||
      binding.relation_descriptor_generation == 0 ||
      binding.validated_resource_epoch == 0 ||
      !CompleteColumnDescriptor(binding.source_column) ||
      !Int32Descriptor(binding.source_column)) {
    return {};
  }
  const std::string projection0 = PackCreateProjection(binding, 0);
  const std::string projection1 = PackCreateProjection(binding, 1);
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
      << kFirebirdRelationProjectionViewMarkerV1
      << "\",\"view_source_uuid\":\""
      << EscapeJson(binding.relation_uuid)
      << "\",\"view_projection_count\":\"2\","
      << "\"view_projection_0\":\"" << EscapeJson(projection0)
      << "\",\"view_projection_1\":\"" << EscapeJson(projection1)
      << "\"}";
  return out.str();
}

FirebirdRelationProjectionViewSelectRoute
ParseFirebirdRelationProjectionViewSelectRoute(std::string_view firebird_sql) {
  FirebirdRelationProjectionViewSelectRoute route;
  if (ContainsSqlComment(firebird_sql)) {
    auto classified = ParseFirebirdRelationProjectionViewSelectRoute(
        StripSqlComments(firebird_sql));
    if (classified.attempted) {
      classified.valid = false;
      return classified;
    }
    return route;
  }
  const std::string sql = StripTerminator(firebird_sql);
  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "SELECT") ||
      !ConsumeCharacter(sql, &offset, '*')) {
    return route;
  }
  route.attempted = true;
  if (!ConsumeKeyword(sql, &offset, "FROM")) return route;
  const auto view = ReadIdentifier(sql, &offset);
  SkipWhitespace(sql, &offset);
  if (!view || view->quoted || view->value.find('.') != std::string::npos ||
      offset != sql.size()) {
    return route;
  }
  route.view_name = view->value;
  route.view_name_quoted = view->quoted;
  route.valid = true;
  return route;
}

FirebirdBoundRelationProjectionViewSelect
BindFirebirdRelationProjectionViewSelect(
    const FirebirdRelationProjectionViewSelectRoute& route,
    const ipc::PublicNameResolutionResult& resolved_view) {
  FirebirdBoundRelationProjectionViewSelect binding;
  binding.route = route;
  if (!route.recognized()) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SELECT_SHAPE_UNSUPPORTED",
           "The standalone Firebird relation-view selector supports only SELECT * FROM view.");
    return binding;
  }
  if (!resolved_view.resolved || resolved_view.object_uuid.empty() ||
      !CanonicalNonzeroUuid(resolved_view.object_uuid) ||
      UpperAscii(resolved_view.object_class) != "VIEW") {
    binding.messages = resolved_view.messages;
    Reject(&binding, "FIREBIRD.RELATION_VIEW.VIEW_REQUIRED",
           "The exact engine-owned relation-view resolution is required.");
    return binding;
  }

  const auto parts = SplitPacket(resolved_view.resolution_detail);
  constexpr std::size_t kHeaderParts = 5;
  constexpr std::size_t kOutputParts = 8;
  constexpr std::size_t kOutputCount = 2;
  if (parts.size() != kHeaderParts + kOutputCount * kOutputParts ||
      parts[0] != kFirebirdRelationProjectionViewSelectPacketV1) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SEMANTIC_DESCRIPTOR_REQUIRED",
           "The resolved view does not expose the bounded relation-projection semantic descriptor.");
    return binding;
  }
  std::string marker;
  std::string descriptor_uuid;
  std::uint64_t generation = 0;
  std::uint64_t output_count = 0;
  if (!HexDecode(parts[1], &marker) ||
      !HexDecode(parts[2], &descriptor_uuid) ||
      !ParseStrictU64(parts[3], &generation) || generation == 0 ||
      !ParseStrictU64(parts[4], &output_count) || output_count != kOutputCount ||
      marker != kFirebirdRelationProjectionViewMarkerV1 ||
      !CanonicalNonzeroUuid(descriptor_uuid) ||
      descriptor_uuid == resolved_view.object_uuid) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SEMANTIC_DESCRIPTOR_INVALID",
           "The relation-view semantic descriptor header is malformed.");
    return binding;
  }

  std::vector<FirebirdRelationProjectionViewOutputDescriptor> outputs;
  outputs.reserve(kOutputCount);
  for (std::uint32_t ordinal = 0; ordinal < kOutputCount; ++ordinal) {
    const std::size_t base = kHeaderParts + ordinal * kOutputParts;
    std::uint64_t encoded_ordinal = 0;
    FirebirdRelationProjectionViewOutputDescriptor output;
    std::string nullable;
    if (!ParseStrictU64(parts[base], &encoded_ordinal) ||
        encoded_ordinal != ordinal ||
        !HexDecode(parts[base + 1], &output.name) ||
        !SafeUnquotedOutputName(output.name) ||
        !HexDecode(parts[base + 2], &output.output_column_uuid) ||
        !CanonicalNonzeroUuid(output.output_column_uuid) ||
        !HexDecode(parts[base + 3], &output.type_descriptor_uuid) ||
        !CanonicalNonzeroUuid(output.type_descriptor_uuid) ||
        output.type_descriptor_uuid == output.output_column_uuid ||
        output.output_column_uuid == resolved_view.object_uuid ||
        output.output_column_uuid == descriptor_uuid ||
        output.type_descriptor_uuid == resolved_view.object_uuid ||
        output.type_descriptor_uuid == descriptor_uuid ||
        !HexDecode(parts[base + 4], &output.descriptor_kind) ||
        !HexDecode(parts[base + 5], &output.canonical_type_name) ||
        !HexDecode(parts[base + 6], &output.encoded_type_descriptor) ||
        output.encoded_type_descriptor.empty() ||
        !HexDecode(parts[base + 7], &nullable) ||
        (nullable != "0" && nullable != "1") ||
        !Int32Descriptor(output.descriptor_kind,
                         output.canonical_type_name)) {
      Reject(&binding, "FIREBIRD.RELATION_VIEW.SEMANTIC_DESCRIPTOR_INVALID",
             "A relation-view semantic output descriptor is malformed.");
      return binding;
    }
    output.ordinal = ordinal;
    output.nullable = nullable == "1";
    outputs.push_back(std::move(output));
  }
  if (UpperAscii(outputs[0].name) == UpperAscii(outputs[1].name) ||
      outputs[0].output_column_uuid == outputs[1].output_column_uuid ||
      outputs[0].type_descriptor_uuid == outputs[1].type_descriptor_uuid ||
      outputs[0].output_column_uuid == outputs[1].type_descriptor_uuid ||
      outputs[1].output_column_uuid == outputs[0].type_descriptor_uuid ||
      outputs[1].nullable) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.SEMANTIC_DESCRIPTOR_INVALID",
           "The relation-view semantic outputs are duplicate or the literal output is nullable.");
    return binding;
  }

  binding.view_uuid = resolved_view.object_uuid;
  binding.view_descriptor_uuid = std::move(descriptor_uuid);
  binding.view_descriptor_generation = generation;
  binding.outputs = std::move(outputs);
  binding.semantic_transport = resolved_view.resolution_detail;
  binding.accepted = true;
  return binding;
}

std::string EncodeFirebirdRelationProjectionViewSelectEnvelope(
    const FirebirdBoundRelationProjectionViewSelect& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      binding.view_uuid.empty() || binding.view_descriptor_uuid.empty() ||
      binding.view_descriptor_generation == 0 || binding.outputs.size() != 2u ||
      binding.semantic_transport.empty()) {
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
      << kFirebirdRelationProjectionViewMarkerV1
      << "\",\"projection_count\":\"1\",\"projection_0\":\""
      << EscapeJson(binding.semantic_transport) << "\"}";
  return out.str();
}

FirebirdRelationProjectionViewCreateV2Route
ParseFirebirdRelationProjectionViewCreateV2Route(
    std::string_view firebird_sql) {
  FirebirdRelationProjectionViewCreateV2Route route;
  if (ContainsSqlComment(firebird_sql)) {
    auto classified = ParseFirebirdRelationProjectionViewCreateV2Route(
        StripSqlComments(firebird_sql));
    if (classified.attempted) {
      classified.valid = false;
      return classified;
    }
    return route;
  }

  const std::string sql = StripTerminator(firebird_sql);
  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "CREATE") ||
      !ConsumeKeyword(sql, &offset, "VIEW")) {
    return route;
  }
  const auto view = ReadIdentifier(sql, &offset);
  if (!view || view->value.find('.') != std::string::npos) return route;

  std::vector<Identifier> explicit_outputs;
  bool explicit_output_list = false;
  std::size_t output_offset = offset;
  if (ConsumeCharacter(sql, &output_offset, '(')) {
    explicit_output_list = true;
    while (true) {
      const auto output = ReadIdentifier(sql, &output_offset);
      if (!output || output->value.find('.') != std::string::npos) {
        return route;
      }
      explicit_outputs.push_back(*output);
      if (ConsumeCharacter(sql, &output_offset, ')')) break;
      if (!ConsumeCharacter(sql, &output_offset, ',')) return route;
    }
    offset = output_offset;
  }
  if (!ConsumeKeyword(sql, &offset, "AS") ||
      !ConsumeKeyword(sql, &offset, "SELECT")) {
    return route;
  }
  const auto from = FindTopLevelKeyword(sql, "FROM", offset);
  if (!from) return route;
  const auto projections =
      SplitTopLevel(std::string_view(sql).substr(offset, *from - offset));
  if (projections.size() != 1u) return route;

  Identifier source_column;
  std::optional<Identifier> source_alias;
  if (!ParseAliasedIdentifier(projections.front(), &source_column,
                              &source_alias)) {
    return route;
  }
  // A single direct identifier establishes the bounded V2 intent.  From this
  // point malformed tails fail closed instead of falling into a legacy view
  // overlay or a sibling parser family.
  route.attempted = true;
  if (source_alias ||
      source_column.value.find('.') != std::string::npos) {
    return route;
  }
  if (explicit_output_list && explicit_outputs.size() != 1u) return route;

  std::size_t source_offset = *from + std::string_view("FROM").size();
  const auto source = ReadIdentifier(sql, &source_offset);
  SkipWhitespace(sql, &source_offset);
  if (!source || source->value.find('.') != std::string::npos ||
      source_offset != sql.size()) {
    return route;
  }

  const Identifier output =
      explicit_output_list ? explicit_outputs.front() : source_column;
  if (view->quoted || source->quoted || source_column.quoted || output.quoted ||
      !SafeV2OutputName(output.value, false)) {
    return route;
  }

  route.view_name = view->value;
  route.view_name_quoted = view->quoted;
  route.explicit_output_name = explicit_output_list;
  route.output_name = output.value;
  route.source_relation = source->value;
  route.source_relation_quoted = source->quoted;
  route.source_column = source_column.value;
  route.source_column_quoted = source_column.quoted;
  route.valid = true;
  return route;
}

FirebirdBoundRelationProjectionViewCreateV2
BindFirebirdRelationProjectionViewCreateV2(
    const FirebirdRelationProjectionViewCreateV2Route& route,
    const ipc::PublicNameResolutionResult& resolved_schema,
    const ipc::PublicNameResolutionResult& resolved_relation) {
  FirebirdBoundRelationProjectionViewCreateV2 binding;
  binding.route = route;
  if (!route.recognized()) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.V2.CREATE_SHAPE_UNSUPPORTED",
           "The standalone Firebird V2 relation-view binder requires one direct int32 source column from one table.");
    return binding;
  }
  if (!resolved_schema.resolved ||
      !CanonicalNonzeroUuid(resolved_schema.object_uuid) ||
      UpperAscii(resolved_schema.object_class) != "SCHEMA") {
    binding.messages = resolved_schema.messages;
    Reject(&binding, "FIREBIRD.RELATION_VIEW.V2.SCHEMA_REQUIRED",
           "The exact engine-owned target schema resolution is required.");
    return binding;
  }

  const auto& descriptor = resolved_relation.relation_descriptor;
  if (!resolved_relation.resolved ||
      !CanonicalNonzeroUuid(resolved_relation.object_uuid) ||
      UpperAscii(resolved_relation.object_class) != "TABLE" ||
      !descriptor.present ||
      !CanonicalNonzeroUuid(descriptor.descriptor_uuid) ||
      descriptor.relation_uuid != resolved_relation.object_uuid ||
      descriptor.descriptor_uuid == descriptor.relation_uuid ||
      descriptor.descriptor_generation == 0 ||
      descriptor.validated_resource_epoch == 0 || descriptor.columns.empty()) {
    binding.messages = resolved_relation.messages;
    Reject(&binding,
           "FIREBIRD.RELATION_VIEW.V2.RELATION_DESCRIPTOR_REQUIRED",
           "The exact engine-owned source table descriptor is required.");
    return binding;
  }

  const ipc::PublicRelationColumnDescriptor* match = nullptr;
  for (const auto& column : descriptor.columns) {
    const bool matches = route.source_column_quoted
                             ? column.canonical_name_key == route.source_column
                             : UpperAscii(column.canonical_name_key) ==
                                   UpperAscii(route.source_column);
    if (!matches) continue;
    if (match != nullptr) {
      Reject(&binding, "FIREBIRD.RELATION_VIEW.V2.COLUMN_AMBIGUOUS",
             "The public source descriptor contains an ambiguous column binding.");
      return binding;
    }
    match = &column;
  }
  if (match == nullptr || !CompleteColumnDescriptor(*match) ||
      !CanonicalNonzeroUuid(match->column_uuid) ||
      !CanonicalNonzeroUuid(match->type_descriptor_uuid) ||
      match->column_uuid == match->type_descriptor_uuid ||
      match->column_uuid == descriptor.relation_uuid ||
      match->column_uuid == descriptor.descriptor_uuid ||
      match->type_descriptor_uuid == descriptor.relation_uuid ||
      match->type_descriptor_uuid == descriptor.descriptor_uuid ||
      resolved_schema.object_uuid == descriptor.relation_uuid ||
      resolved_schema.object_uuid == descriptor.descriptor_uuid ||
      resolved_schema.object_uuid == match->column_uuid ||
      resolved_schema.object_uuid == match->type_descriptor_uuid) {
    Reject(&binding,
           "FIREBIRD.RELATION_VIEW.V2.COLUMN_DESCRIPTOR_REQUIRED",
           "A complete, distinct engine-owned source column and type descriptor is required.");
    return binding;
  }
  if (!Int32Descriptor(*match)) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.V2.SOURCE_TYPE_UNSUPPORTED",
           "The bounded V2 relation-view source column must have an int32 descriptor.");
    return binding;
  }

  binding.schema_uuid = resolved_schema.object_uuid;
  binding.relation_uuid = resolved_relation.object_uuid;
  binding.relation_descriptor_uuid = descriptor.descriptor_uuid;
  binding.relation_descriptor_generation = descriptor.descriptor_generation;
  binding.validated_resource_epoch = descriptor.validated_resource_epoch;
  binding.source_column = *match;
  binding.accepted = true;
  return binding;
}

std::string EncodeFirebirdRelationProjectionViewCreateV2Envelope(
    const FirebirdBoundRelationProjectionViewCreateV2& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      !CanonicalNonzeroUuid(binding.schema_uuid) ||
      !CanonicalNonzeroUuid(binding.relation_uuid) ||
      !CanonicalNonzeroUuid(binding.relation_descriptor_uuid) ||
      !CanonicalNonzeroUuid(binding.source_column.column_uuid) ||
      !CanonicalNonzeroUuid(binding.source_column.type_descriptor_uuid) ||
      binding.relation_descriptor_generation == 0 ||
      binding.validated_resource_epoch == 0 ||
      !CompleteColumnDescriptor(binding.source_column) ||
      !Int32Descriptor(binding.source_column) ||
      !SafeV2OutputName(binding.route.output_name, false) ||
      binding.route.view_name_quoted || binding.route.source_relation_quoted ||
      binding.route.source_column_quoted ||
      UpperAscii(binding.source_column.canonical_name_key) !=
          UpperAscii(binding.route.source_column) ||
      binding.schema_uuid == binding.relation_uuid ||
      binding.schema_uuid == binding.relation_descriptor_uuid ||
      binding.schema_uuid == binding.source_column.column_uuid ||
      binding.schema_uuid == binding.source_column.type_descriptor_uuid ||
      binding.relation_uuid == binding.relation_descriptor_uuid ||
      binding.relation_uuid == binding.source_column.column_uuid ||
      binding.relation_uuid == binding.source_column.type_descriptor_uuid ||
      binding.relation_descriptor_uuid == binding.source_column.column_uuid ||
      binding.relation_descriptor_uuid ==
          binding.source_column.type_descriptor_uuid ||
      binding.source_column.column_uuid ==
          binding.source_column.type_descriptor_uuid) {
    return {};
  }
  const std::string projection = PackCreateProjectionV2(binding);
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
      << kFirebirdRelationProjectionViewMarkerV2
      << "\",\"view_source_uuid\":\""
      << EscapeJson(binding.relation_uuid)
      << "\",\"view_projection_count\":\"1\","
      << "\"view_projection_0\":\"" << EscapeJson(projection) << "\"}";
  return out.str();
}

FirebirdRelationProjectionViewDeleteV2Route
ParseFirebirdRelationProjectionViewDeleteV2Route(
    std::string_view firebird_sql) {
  FirebirdRelationProjectionViewDeleteV2Route route;
  if (ContainsSqlComment(firebird_sql)) {
    auto classified = ParseFirebirdRelationProjectionViewDeleteV2Route(
        StripSqlComments(firebird_sql));
    if (classified.attempted) {
      classified.valid = false;
      return classified;
    }
    return route;
  }

  const std::string sql = StripTerminator(firebird_sql);
  std::size_t offset = 0;
  if (!ConsumeKeyword(sql, &offset, "DELETE") ||
      !ConsumeKeyword(sql, &offset, "FROM")) {
    return route;
  }
  const auto view = ReadIdentifier(sql, &offset);
  if (!view || view->value.find('.') != std::string::npos ||
      !ConsumeKeyword(sql, &offset, "WHERE")) {
    return route;
  }
  const auto output = ReadIdentifier(sql, &offset);
  if (!output || output->value.find('.') != std::string::npos ||
      !ConsumeCharacter(sql, &offset, '=')) {
    return route;
  }
  route.attempted = true;
  const auto value = ReadInt32(sql, &offset);
  SkipWhitespace(sql, &offset);
  if (!value || offset != sql.size() || view->quoted || output->quoted ||
      !SafeV2OutputName(output->value, false)) {
    return route;
  }

  route.view_name = view->value;
  route.view_name_quoted = view->quoted;
  route.output_name = output->value;
  route.output_name_quoted = output->quoted;
  route.predicate_value = *value;
  route.valid = true;
  return route;
}

FirebirdBoundRelationProjectionViewDeleteV2
BindFirebirdRelationProjectionViewDeleteV2(
    const FirebirdRelationProjectionViewDeleteV2Route& route,
    const ipc::PublicNameResolutionResult& resolved_view) {
  FirebirdBoundRelationProjectionViewDeleteV2 binding;
  binding.route = route;
  if (!route.recognized()) {
    Reject(&binding, "FIREBIRD.RELATION_VIEW.V2.DELETE_SHAPE_UNSUPPORTED",
           "The standalone Firebird V2 view-delete binder requires DELETE FROM view WHERE output = int32_literal.");
    return binding;
  }
  if (!resolved_view.resolved ||
      !CanonicalNonzeroUuid(resolved_view.object_uuid) ||
      UpperAscii(resolved_view.object_class) != "VIEW") {
    binding.messages = resolved_view.messages;
    Reject(&binding, "FIREBIRD.RELATION_VIEW.V2.VIEW_REQUIRED",
           "The exact public engine-owned view resolution is required.");
    return binding;
  }

  const auto parts = SplitPacket(resolved_view.resolution_detail);
  constexpr std::size_t kPacketParts = 13;
  if (parts.size() != kPacketParts ||
      parts[0] != kFirebirdRelationProjectionViewDeletePacketV2) {
    Reject(&binding,
           "FIREBIRD.RELATION_VIEW.V2.DELETE_DESCRIPTOR_REQUIRED",
           "The resolved view does not expose the source-opaque rpvd2 descriptor.");
    return binding;
  }

  std::string marker;
  std::string descriptor_uuid;
  std::uint64_t generation = 0;
  std::uint64_t output_count = 0;
  std::uint64_t ordinal = 0;
  FirebirdRelationProjectionViewOutputDescriptor output;
  std::string nullable;
  if (!HexDecode(parts[1], &marker) ||
      marker != kFirebirdRelationProjectionViewMarkerV2 ||
      !HexDecode(parts[2], &descriptor_uuid) ||
      !CanonicalNonzeroUuid(descriptor_uuid) ||
      descriptor_uuid == resolved_view.object_uuid ||
      !ParseStrictU64(parts[3], &generation) || generation == 0 ||
      !ParseStrictU64(parts[4], &output_count) || output_count != 1u ||
      !ParseStrictU64(parts[5], &ordinal) || ordinal != 0u ||
      !HexDecode(parts[6], &output.name) ||
      !SafeV2OutputName(output.name, false) ||
      !HexDecode(parts[7], &output.output_column_uuid) ||
      !CanonicalNonzeroUuid(output.output_column_uuid) ||
      !HexDecode(parts[8], &output.type_descriptor_uuid) ||
      !CanonicalNonzeroUuid(output.type_descriptor_uuid) ||
      !HexDecode(parts[9], &output.descriptor_kind) ||
      !HexDecode(parts[10], &output.canonical_type_name) ||
      !HexDecode(parts[11], &output.encoded_type_descriptor) ||
      output.encoded_type_descriptor.empty() ||
      !HexDecode(parts[12], &nullable) ||
      (nullable != "0" && nullable != "1") ||
      !Int32Descriptor(output.descriptor_kind,
                       output.canonical_type_name) ||
      output.output_column_uuid == resolved_view.object_uuid ||
      output.output_column_uuid == descriptor_uuid ||
      output.type_descriptor_uuid == resolved_view.object_uuid ||
      output.type_descriptor_uuid == descriptor_uuid ||
      output.type_descriptor_uuid == output.output_column_uuid) {
    Reject(&binding,
           "FIREBIRD.RELATION_VIEW.V2.DELETE_DESCRIPTOR_INVALID",
           "The public rpvd2 view/output/type descriptor is malformed.");
    return binding;
  }
  const bool name_matches =
      route.output_name_quoted
          ? output.name == route.output_name
          : UpperAscii(output.name) == UpperAscii(route.output_name);
  if (!name_matches) {
    Reject(&binding,
           "FIREBIRD.RELATION_VIEW.V2.DELETE_OUTPUT_NOT_FOUND",
           "The predicate output does not match the sole public V2 view output.");
    return binding;
  }

  output.ordinal = 0;
  output.nullable = nullable == "1";
  binding.view_uuid = resolved_view.object_uuid;
  binding.view_descriptor_uuid = std::move(descriptor_uuid);
  binding.view_descriptor_generation = generation;
  binding.output = std::move(output);
  binding.semantic_transport = resolved_view.resolution_detail;
  binding.accepted = true;
  return binding;
}

std::string EncodeFirebirdRelationProjectionViewDeleteV2Envelope(
    const FirebirdBoundRelationProjectionViewDeleteV2& binding) {
  if (!binding.accepted || !binding.route.recognized() ||
      !CanonicalNonzeroUuid(binding.view_uuid) ||
      !CanonicalNonzeroUuid(binding.view_descriptor_uuid) ||
      binding.view_descriptor_generation == 0 ||
      binding.semantic_transport.empty() ||
      !binding.semantic_transport.starts_with(
          std::string(kFirebirdRelationProjectionViewDeletePacketV2) + "|") ||
      !CanonicalNonzeroUuid(binding.output.output_column_uuid) ||
      !CanonicalNonzeroUuid(binding.output.type_descriptor_uuid) ||
      binding.view_uuid == binding.view_descriptor_uuid ||
      binding.view_uuid == binding.output.output_column_uuid ||
      binding.view_uuid == binding.output.type_descriptor_uuid ||
      binding.view_descriptor_uuid == binding.output.output_column_uuid ||
      binding.view_descriptor_uuid == binding.output.type_descriptor_uuid ||
      binding.output.output_column_uuid == binding.output.type_descriptor_uuid ||
      !SafeV2OutputName(binding.output.name, false) ||
      UpperAscii(binding.output.name) !=
          UpperAscii(binding.route.output_name) ||
      !Int32Descriptor(binding.output.descriptor_kind,
                       binding.output.canonical_type_name)) {
    return {};
  }
  std::ostringstream out;
  out << "{\"envelope\":\"SBLRExecutionEnvelope.v3\","
         "\"envelope_major\":3,"
         "\"sblr_version\":\"sblr_v3\","
         "\"operation_id\":\"dml.delete_rows\","
         "\"opcode\":\"SBLR_DML_DELETE_ROWS\","
         "\"operation_family\":\"sblr.dml.delete.v3\","
         "\"sblr_operation_family\":\"sblr.dml.delete.v3\","
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
      << "\"dml_surface_variant\":\""
      << kFirebirdRelationProjectionViewMarkerV2
      << "\",\"projection_count\":\"1\",\"projection_0\":\""
      << EscapeJson(binding.semantic_transport)
      << "\",\"predicate_kind\":\"column_equals\","
      << "\"predicate_column\":\"" << EscapeJson(binding.output.name)
      << "\",\"predicate_value\":\""
      << binding.route.predicate_value
      << "\",\"predicate_value_type\":\"int32\"}";
  return out.str();
}

}  // namespace scratchbird::parser::firebird
