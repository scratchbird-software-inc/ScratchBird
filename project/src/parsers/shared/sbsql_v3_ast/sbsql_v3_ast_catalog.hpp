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

namespace scratchbird::parser::sbsql_v3_ast {

struct SourceRange {
  std::uint32_t start_byte = 0;
  std::uint32_t end_byte = 0;
};

struct AstCatalogHeader {
  std::uint32_t ast_format_version = 1;
  std::string parser_mode = "native_sbsql_v3";
  std::string command_family;
  std::string surface_key;
  std::string grammar_rule;
  std::string parser_package_uuid;
  std::string parser_package_version;
  std::string registry_snapshot_uuid;
  std::string source_encoding = "utf8";
  SourceRange source_range;
  std::vector<SourceRange> token_spans;
  std::string diagnostic_context_id;
};

struct AstCatalogNode {
  AstCatalogHeader header;
  std::string ast_node;
  std::string bound_ast_node;
  std::vector<std::string> required_fields;
  std::string raw_command_evidence;
  bool raw_command_engine_authority = false;
  bool names_must_bind_to_uuid_before_engine = true;
  bool descriptors_must_bind_before_engine = true;
};

std::string AstNodeForCommandFamily(std::string_view command_family);
std::string BoundAstNodeForCommandFamily(std::string_view command_family);
std::vector<std::string> RequiredFieldsForCommandFamily(std::string_view command_family);
AstCatalogNode MakeAstCatalogNode(std::string command_family,
                                   std::string surface_key,
                                   std::string grammar_rule,
                                   SourceRange source_range,
                                   std::vector<SourceRange> token_spans,
                                   std::string raw_command_evidence = {});
bool ValidateAstCatalogNode(const AstCatalogNode& node, std::vector<std::string>* errors);
std::string SerializeAstCatalogNodeToJson(const AstCatalogNode& node);

}  // namespace scratchbird::parser::sbsql_v3_ast
