// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::parser::ast {

// AST is syntax evidence only. It is not catalog, security, transaction,
// cluster, storage, or executor authority.

enum class ParserMode {
  kNativeSbsql,
  kReference,
  kMeta,
  kToolLocal,
  kRefusalOnly,
};

enum class ReferenceMode {
  kNone,
  kFirebird,
  kPostgreSql,
  kMySqlMariaDb,
  kSqlite,
  kDuckDb,
  kOther,
};

enum class AstFamily {
  kShowIdentity,
  kRefusal,
};

enum class ShowIdentityKind {
  kVersion,
  kDatabase,
  kSystem,
  kCapabilities,
};

struct SourceRange {
  std::uint32_t start_byte = 0;
  std::uint32_t end_byte = 0;
};

struct AstHeader {
  std::uint32_t ast_format_version = 1;
  ParserMode parser_mode = ParserMode::kNativeSbsql;
  ReferenceMode reference_mode = ReferenceMode::kNone;
  AstFamily family = AstFamily::kRefusal;
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string registry_snapshot_uuid;
  std::string command_family_candidate;
  std::string surface_key_candidate;
  std::string source_encoding = "utf8";
  SourceRange source_range;
  std::vector<SourceRange> token_spans;
  std::string diagnostic_context_id;
};

struct ShowIdentityAst {
  AstHeader header;
  ShowIdentityKind show_kind = ShowIdentityKind::kVersion;
  std::string raw_command_form;
};

std::string ToString(ParserMode value);
std::string ToString(ReferenceMode value);
std::string ToString(AstFamily value);
std::string ToString(ShowIdentityKind value);

ShowIdentityAst MakeShowIdentityAst(ShowIdentityKind show_kind,
                                    std::string raw_command_form,
                                    std::string surface_key_candidate,
                                    SourceRange source_range);

std::string SerializeToJson(const ShowIdentityAst& ast);

}  // namespace scratchbird::parser::ast
