// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sbsql_v3_ast_catalog.hpp"

#include <map>
#include <sstream>
#include <string_view>
#include <utility>

namespace scratchbird::parser::sbsql_v3_ast {
namespace {

const std::map<std::string_view, std::string_view>& AstMap() {
  static const std::map<std::string_view, std::string_view> kMap = {
      {"sbsql.identity_session", "IdentitySessionAst"},
      {"sbsql.transaction", "TransactionControlAst"},
      {"sbsql.query_dml", "QueryDmlAst"},
      {"sbsql.ddl_schema_tree", "SchemaTreeDdlAst"},
      {"sbsql.ddl_database_storage", "DatabaseStorageDdlAst"},
      {"sbsql.ddl_table_index_domain", "TableIndexDomainDdlAst"},
      {"sbsql.ddl_routine_udr", "RoutineUdrDdlAst"},
      {"sbsql.security_dcl", "SecurityDclAst"},
      {"sbsql.policy", "PolicyAst"},
      {"sbsql.observability", "ObservabilityAst"},
      {"sbsql.management", "ManagementAst"},
      {"sbsql.acceleration", "AccelerationAst"},
      {"sbsql.archive_replication_migration", "ArchiveReplicationMigrationAst"},
      {"sbsql.private_cluster", "PrivateClusterAst"},
  };
  return kMap;
}

std::string JsonEscape(std::string_view input) {
  std::ostringstream out;
  for (const unsigned char ch : input) {
    switch (ch) {
      case '\\': out << "\\\\"; break;
      case '"': out << "\\\""; break;
      case '\n': out << "\\n"; break;
      case '\r': out << "\\r"; break;
      case '\t': out << "\\t"; break;
      default: out << ch;
    }
  }
  return out.str();
}

}  // namespace

std::string AstNodeForCommandFamily(std::string_view command_family) {
  const auto found = AstMap().find(command_family);
  if (found == AstMap().end()) return "RefusalAst";
  return std::string(found->second);
}

std::string BoundAstNodeForCommandFamily(std::string_view command_family) {
  const std::string ast = AstNodeForCommandFamily(command_family);
  if (ast == "RefusalAst") return "BoundRefusal";
  return "Bound" + ast.substr(0, ast.size() - 3);
}

std::vector<std::string> RequiredFieldsForCommandFamily(std::string_view command_family) {
  if (command_family == "sbsql.identity_session") return {"show_target", "session_scope", "context_key"};
  if (command_family == "sbsql.transaction") return {"transaction_action", "isolation_level", "read_only", "savepoint_name", "transaction_options"};
  if (command_family == "sbsql.query_dml") return {"query_kind", "target_relation", "projection_tree", "predicate_tree", "returning_clause", "query_modifiers"};
  if (command_family == "sbsql.ddl_schema_tree") return {"ddl_action", "schema_path", "object_name", "localized_names", "schema_tree_options"};
  if (command_family == "sbsql.ddl_database_storage") return {"ddl_action", "database_ref", "filespace_ref", "page_profile", "storage_options"};
  if (command_family == "sbsql.ddl_table_index_domain") return {"ddl_action", "object_ref", "columns", "constraints", "index_definition", "domain_descriptor", "storage_profile"};
  if (command_family == "sbsql.ddl_routine_udr") return {"ddl_action", "routine_ref", "parameters", "return_descriptor", "sblr_body_ref", "udr_package_ref"};
  if (command_family == "sbsql.security_dcl") return {"security_action", "principal_ref", "grant_payload", "auth_source", "membership_options"};
  if (command_family == "sbsql.policy") return {"policy_action", "policy_ref", "policy_kind", "target_object_ref", "policy_payload"};
  if (command_family == "sbsql.observability") return {"show_target", "scope_mode", "filter_predicate", "metrics_family", "explain_options"};
  if (command_family == "sbsql.management") return {"management_target", "management_action", "target_ref", "control_options", "inspect_scope"};
  if (command_family == "sbsql.acceleration") return {"acceleration_family", "acceleration_action", "target_ref", "profile_ref", "capability_options"};
  if (command_family == "sbsql.archive_replication_migration") return {"operation_kind", "target_ref", "policy_ref", "lineage_options", "migration_options"};
  if (command_family == "sbsql.private_cluster") return {"cluster_target", "cluster_action", "cluster_ref", "epoch_ref", "decision_ref", "route_ref"};
  return {};
}

AstCatalogNode MakeAstCatalogNode(std::string command_family,
                                   std::string surface_key,
                                   std::string grammar_rule,
                                   SourceRange source_range,
                                   std::vector<SourceRange> token_spans,
                                   std::string raw_command_evidence) {
  AstCatalogNode node;
  node.header.command_family = std::move(command_family);
  node.header.surface_key = std::move(surface_key);
  node.header.grammar_rule = std::move(grammar_rule);
  node.header.source_range = source_range;
  node.header.token_spans = std::move(token_spans);
  node.ast_node = AstNodeForCommandFamily(node.header.command_family);
  node.bound_ast_node = BoundAstNodeForCommandFamily(node.header.command_family);
  node.required_fields = RequiredFieldsForCommandFamily(node.header.command_family);
  node.raw_command_evidence = std::move(raw_command_evidence);
  return node;
}

bool ValidateAstCatalogNode(const AstCatalogNode& node, std::vector<std::string>* errors) {
  if (node.header.ast_format_version != 1) errors->push_back("ast_format_version_invalid");
  if (node.header.parser_mode != "native_sbsql_v3") errors->push_back("parser_mode_invalid");
  if (node.header.command_family.empty()) errors->push_back("command_family_missing");
  if (node.header.surface_key.empty()) errors->push_back("surface_key_missing");
  if (node.header.grammar_rule.empty()) errors->push_back("grammar_rule_missing");
  if (node.header.source_encoding != "utf8") errors->push_back("source_encoding_invalid");
  if (node.header.source_range.end_byte < node.header.source_range.start_byte) errors->push_back("source_range_invalid");
  if (node.header.token_spans.empty()) errors->push_back("token_spans_missing");
  if (node.ast_node.empty() || node.ast_node == "RefusalAst") errors->push_back("ast_node_missing");
  if (node.bound_ast_node.empty() || node.bound_ast_node == "BoundRefusal") errors->push_back("bound_ast_node_missing");
  if (node.required_fields.empty()) errors->push_back("required_fields_missing");
  if (node.raw_command_engine_authority) errors->push_back("raw_command_engine_authority_forbidden");
  if (!node.names_must_bind_to_uuid_before_engine) errors->push_back("uuid_binding_required");
  if (!node.descriptors_must_bind_before_engine) errors->push_back("descriptor_binding_required");
  return errors->empty();
}

std::string SerializeAstCatalogNodeToJson(const AstCatalogNode& node) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"ast_format_version\": " << node.header.ast_format_version << ",\n";
  out << "  \"parser_mode\": \"" << JsonEscape(node.header.parser_mode) << "\",\n";
  out << "  \"command_family\": \"" << JsonEscape(node.header.command_family) << "\",\n";
  out << "  \"surface_key\": \"" << JsonEscape(node.header.surface_key) << "\",\n";
  out << "  \"grammar_rule\": \"" << JsonEscape(node.header.grammar_rule) << "\",\n";
  out << "  \"ast_node\": \"" << JsonEscape(node.ast_node) << "\",\n";
  out << "  \"bound_ast_node\": \"" << JsonEscape(node.bound_ast_node) << "\",\n";
  out << "  \"source_range\": {\"start_byte\": " << node.header.source_range.start_byte << ", \"end_byte\": " << node.header.source_range.end_byte << "},\n";
  out << "  \"token_span_count\": " << node.header.token_spans.size() << ",\n";
  out << "  \"raw_command_engine_authority\": " << (node.raw_command_engine_authority ? "true" : "false") << ",\n";
  out << "  \"required_field_count\": " << node.required_fields.size() << "\n";
  out << "}\n";
  return out.str();
}

}  // namespace scratchbird::parser::sbsql_v3_ast
