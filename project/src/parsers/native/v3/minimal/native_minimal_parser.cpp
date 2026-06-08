// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "native_minimal_parser.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>

namespace scratchbird::parser::native_v3 {
namespace {

using scratchbird::parser::ast::MakeShowIdentityAst;
using scratchbird::parser::ast::ShowIdentityKind;
using scratchbird::parser::ast::SourceRange;

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\b': out << "\\b"; break;
      case '\f': out << "\\f"; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default:
        if (ch < 0x20) {
          constexpr char kHex[] = "0123456789abcdef";
          out << "\\u00" << kHex[(ch >> 4) & 0x0f] << kHex[ch & 0x0f];
        } else {
          out << ch;
        }
    }
  }
  return out.str();
}

bool IsAsciiSpace(char ch) {
  return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == '\f';
}

std::string TrimOptionalSemicolon(std::string_view input, SourceRange& range) {
  std::size_t begin = 0;
  std::size_t end = input.size();
  while (begin < end && IsAsciiSpace(input[begin])) ++begin;
  while (end > begin && IsAsciiSpace(input[end - 1])) --end;
  if (end > begin && input[end - 1] == ';') {
    --end;
    while (end > begin && IsAsciiSpace(input[end - 1])) --end;
  }
  range.start_byte = static_cast<std::uint32_t>(begin);
  range.end_byte = static_cast<std::uint32_t>(end);
  return std::string(input.substr(begin, end - begin));
}

std::string UpperAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return value;
}

ParseResult Error(std::string code, std::string message, SourceRange range) {
  return ParseResult{ParseDiagnostic{std::move(code), std::move(message), range}};
}

}  // namespace

bool ParseResult::ok() const {
  return std::holds_alternative<scratchbird::parser::ast::ShowIdentityAst>(value);
}

ParseResult ParseMinimalIdentityShow(std::string_view command_text) {
  SourceRange range;
  const std::string trimmed = TrimOptionalSemicolon(command_text, range);
  const std::string normalized = UpperAscii(trimmed);

  if (trimmed.empty()) {
    return Error("SBP_EMPTY_COMMAND", "empty command", range);
  }

  if (normalized == "SHOW VERSION") {
    return ParseResult{MakeShowIdentityAst(ShowIdentityKind::kVersion, trimmed,
                                           "show.version", range)};
  }

  if (normalized == "SHOW DATABASE") {
    return ParseResult{MakeShowIdentityAst(ShowIdentityKind::kDatabase, trimmed,
                                           "show.database", range)};
  }

  return Error("SBP_UNSUPPORTED_VERTICAL_SLICE_COMMAND",
               "initial vertical slice supports only SHOW VERSION and SHOW DATABASE",
               range);
}

std::string SerializeDiagnosticToJson(const ParseDiagnostic& diagnostic) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"diagnostic_code\": \"" << JsonEscape(diagnostic.code) << "\",\n";
  out << "  \"phase\": \"parser_grammar\",\n";
  out << "  \"message\": \"" << JsonEscape(diagnostic.message) << "\",\n";
  out << "  \"source_range\": {\"start_byte\": " << diagnostic.source_range.start_byte
      << ", \"end_byte\": " << diagnostic.source_range.end_byte << "}\n";
  out << "}\n";
  return out.str();
}

std::string SerializeParseResultToJson(const ParseResult& result) {
  if (const auto* ast = std::get_if<scratchbird::parser::ast::ShowIdentityAst>(&result.value)) {
    return scratchbird::parser::ast::SerializeToJson(*ast);
  }
  return SerializeDiagnosticToJson(std::get<ParseDiagnostic>(result.value));
}

}  // namespace scratchbird::parser::native_v3
