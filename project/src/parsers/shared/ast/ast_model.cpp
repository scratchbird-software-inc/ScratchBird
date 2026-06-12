// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast_model.hpp"

#include <sstream>

namespace scratchbird::parser::ast {
namespace {

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

}  // namespace

std::string ToString(ParserMode value) {
  switch (value) {
    case ParserMode::kNativeSbsql: return "native_sbsql";
    case ParserMode::kReference: return "reference";
    case ParserMode::kMeta: return "meta";
    case ParserMode::kToolLocal: return "tool_local";
    case ParserMode::kRefusalOnly: return "refusal_only";
  }
  return "unknown";
}

std::string ToString(ReferenceMode value) {
  switch (value) {
    case ReferenceMode::kNone: return "none";
    case ReferenceMode::kFirebird: return "firebird";
    case ReferenceMode::kPostgreSql: return "postgresql";
    case ReferenceMode::kMySqlMariaDb: return "mysql_mariadb";
    case ReferenceMode::kSqlite: return "sqlite";
    case ReferenceMode::kDuckDb: return "duckdb";
    case ReferenceMode::kOther: return "other";
  }
  return "unknown";
}

std::string ToString(AstFamily value) {
  switch (value) {
    case AstFamily::kShowIdentity: return "ShowIdentityAst";
    case AstFamily::kRefusal: return "RefusalAst";
  }
  return "unknown";
}

std::string ToString(ShowIdentityKind value) {
  switch (value) {
    case ShowIdentityKind::kVersion: return "version";
    case ShowIdentityKind::kDatabase: return "database";
    case ShowIdentityKind::kSystem: return "system";
    case ShowIdentityKind::kCapabilities: return "capabilities";
  }
  return "unknown";
}

ShowIdentityAst MakeShowIdentityAst(ShowIdentityKind show_kind,
                                    std::string raw_command_form,
                                    std::string surface_key_candidate,
                                    SourceRange source_range) {
  ShowIdentityAst ast;
  ast.header.ast_format_version = 1;
  ast.header.parser_mode = ParserMode::kNativeSbsql;
  ast.header.reference_mode = ReferenceMode::kNone;
  ast.header.family = AstFamily::kShowIdentity;
  ast.header.command_family_candidate = "sbsql.identity_session";
  ast.header.surface_key_candidate = std::move(surface_key_candidate);
  ast.header.source_range = source_range;
  ast.header.token_spans.push_back(source_range);
  ast.show_kind = show_kind;
  ast.raw_command_form = std::move(raw_command_form);
  return ast;
}

std::string SerializeToJson(const ShowIdentityAst& ast) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"ast_format_version\": " << ast.header.ast_format_version << ",\n";
  out << "  \"ast_node\": \"" << JsonEscape(ToString(ast.header.family)) << "\",\n";
  out << "  \"parser_mode\": \"" << JsonEscape(ToString(ast.header.parser_mode)) << "\",\n";
  out << "  \"reference_mode\": \"" << JsonEscape(ToString(ast.header.reference_mode)) << "\",\n";
  out << "  \"command_family_candidate\": \"" << JsonEscape(ast.header.command_family_candidate) << "\",\n";
  out << "  \"surface_key_candidate\": \"" << JsonEscape(ast.header.surface_key_candidate) << "\",\n";
  out << "  \"source_encoding\": \"" << JsonEscape(ast.header.source_encoding) << "\",\n";
  out << "  \"source_range\": {\"start_byte\": " << ast.header.source_range.start_byte
      << ", \"end_byte\": " << ast.header.source_range.end_byte << "},\n";
  out << "  \"fields\": {\n";
  out << "    \"show_kind\": \"" << JsonEscape(ToString(ast.show_kind)) << "\",\n";
  out << "    \"raw_command_form\": \"" << JsonEscape(ast.raw_command_form) << "\"\n";
  out << "  }\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::parser::ast
