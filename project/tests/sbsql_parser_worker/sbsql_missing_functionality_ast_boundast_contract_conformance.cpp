// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast_model.hpp"
#include "bound_ast_model.hpp"
#include "sbsql_v3_ast_catalog.hpp"
#include "sbsql_v3_binding_catalog.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace ast = scratchbird::parser::ast;
namespace bound = scratchbird::parser::bound_ast;
namespace v3_ast = scratchbird::parser::sbsql_v3_ast;
namespace v3_binding = scratchbird::parser::sbsql_v3_binding;

namespace {

bool Require(bool condition, std::string_view message) {
  if (condition) return true;
  std::cerr << message << "\n";
  return false;
}

bool Contains(const std::vector<std::string>& values, std::string_view needle) {
  return std::find(values.begin(), values.end(), needle) != values.end();
}

std::string BoundNameFromAst(std::string_view ast_node) {
  if (ast_node.size() < 3 || ast_node.substr(ast_node.size() - 3) != "Ast") {
    return "BoundRefusal";
  }
  return "Bound" + std::string(ast_node.substr(0, ast_node.size() - 3));
}

struct ContractCase {
  std::string_view family;
  std::string_view ast_node;
  ast::AstFamily ast_family;
  std::string_view required_right;
  std::vector<std::string_view> fields;
};

bool ValidateAcceptedFamily(const ContractCase& item) {
  bool ok = true;
  const v3_ast::SourceRange range{0, 32};
  const std::vector<v3_ast::SourceRange> spans{{0, 6}, {7, 16}, {17, 32}};
  auto node = v3_ast::MakeAstCatalogNode(std::string(item.family),
                                         "SBSQL-MISS-002",
                                         "missing_functionality_family_contract",
                                         range,
                                         spans,
                                         "contract probe only");
  std::vector<std::string> errors;
  ok &= Require(v3_ast::ValidateAstCatalogNode(node, &errors),
                std::string("AST catalog node did not validate for ") +
                    std::string(item.family));
  for (const auto& error : errors) {
    std::cerr << item.family << ": " << error << "\n";
  }
  ok &= Require(node.ast_node == item.ast_node,
                std::string("AST node mismatch for ") + std::string(item.family));
  ok &= Require(node.bound_ast_node == BoundNameFromAst(item.ast_node),
                std::string("BoundAST node mismatch for ") + std::string(item.family));
  ok &= Require(!node.raw_command_engine_authority,
                "raw SQL command text was marked as engine authority");
  ok &= Require(node.names_must_bind_to_uuid_before_engine,
                "name-to-UUID binding is not mandatory before engine admission");
  ok &= Require(node.descriptors_must_bind_before_engine,
                "descriptor binding is not mandatory before engine admission");
  for (const auto field : item.fields) {
    ok &= Require(Contains(node.required_fields, field),
                  std::string("required AST field missing for ") +
                      std::string(item.family) + ": " + std::string(field));
  }

  auto profile = v3_binding::BindingProfileForCommandFamily(item.family);
  std::vector<std::string> binding_errors;
  ok &= Require(v3_binding::ValidateBindingProfile(profile, &binding_errors),
                std::string("binding profile did not validate for ") +
                    std::string(item.family));
  for (const auto& error : binding_errors) {
    std::cerr << item.family << ": " << error << "\n";
  }
  ok &= Require(profile.command_family == item.family,
                std::string("binding profile family mismatch for ") +
                    std::string(item.family));
  ok &= Require(profile.required_right == item.required_right,
                std::string("binding right mismatch for ") + std::string(item.family));
  ok &= Require(profile.engine_security_recheck_required,
                "engine security recheck is not required");
  ok &= Require(profile.engine_transaction_recheck_required,
                "engine transaction recheck is not required");
  ok &= Require(!profile.cluster_authority_required,
                std::string("non-cluster family requires cluster authority: ") +
                    std::string(item.family));

  const bound::BindingContext context{
      "018f0000-0000-7000-8000-000000000001",
      "018f0000-0000-7000-8000-000000000002",
      "catalog-epoch-1",
      "registry-snapshot-1",
      "public_node"};
  const auto evidence = bound::MakeBoundStatementFamilyEvidence(
      item.ast_family,
      "SBSQL-MISS-002",
      std::string(item.family),
      profile.required_right,
      profile.scope_mode,
      profile.descriptor_binding_profile,
      "sblr.contract." + std::string(item.family),
      "rs.contract." + std::string(item.family),
      context);
  ok &= Require(evidence.header.command_family == item.family,
                std::string("generic BoundAST family evidence mismatch for ") +
                    std::string(item.family));
  ok &= Require(evidence.header.required_right == item.required_right,
                std::string("generic BoundAST right evidence mismatch for ") +
                    std::string(item.family));
  ok &= Require(evidence.ast_family == item.ast_family,
                std::string("generic BoundAST AST family mismatch for ") +
                    std::string(item.family));
  ok &= Require(!bound::SerializeToJson(evidence).empty(),
                "generic BoundAST evidence serialization is empty");
  return ok;
}

bool ValidateUnknownRefusal() {
  bool ok = true;
  auto node = v3_ast::MakeAstCatalogNode("sbsql.unallocated",
                                         "SBSQL-MISS-002",
                                         "unknown_family",
                                         {0, 1},
                                         {{0, 1}},
                                         "unknown");
  std::vector<std::string> errors;
  ok &= Require(!v3_ast::ValidateAstCatalogNode(node, &errors),
                "unknown family unexpectedly validated");
  ok &= Require(node.ast_node == "RefusalAst", "unknown family did not map to RefusalAst");
  ok &= Require(node.bound_ast_node == "BoundRefusal",
                "unknown family did not map to BoundRefusal");

  auto profile = v3_binding::BindingProfileForCommandFamily("sbsql.unallocated");
  std::vector<std::string> binding_errors;
  ok &= Require(!v3_binding::ValidateBindingProfile(profile, &binding_errors),
                "unknown binding profile unexpectedly validated");
  return ok;
}

bool ValidateRawTextAuthorityRejection() {
  auto node = v3_ast::MakeAstCatalogNode("sbsql.dml_upsert_variants",
                                         "SBSQL-MISS-002",
                                         "raw_text_authority_negative_case",
                                         {0, 4},
                                         {{0, 4}},
                                         "INSERT");
  node.raw_command_engine_authority = true;
  std::vector<std::string> errors;
  return Require(!v3_ast::ValidateAstCatalogNode(node, &errors),
                 "raw command engine authority was accepted");
}

}  // namespace

int main() {
  const std::vector<ContractCase> cases = {
      {"sbsql.migration_management", "MigrationManagementAst",
       ast::AstFamily::kMigrationManagement, "MIGRATE_DATABASE",
       {"migration_action", "policy_gate", "capability_descriptor"}},
      {"sbsql.temporal_bitemporal", "TemporalPeriodSpecAst",
       ast::AstFamily::kTemporalBitemporal, "TEMPORAL_HISTORY_READ_OR_ADMIN",
       {"period_descriptor", "time_axis", "mga_snapshot_ref"}},
      {"sbsql.structured_types", "StructuredTypeAst",
       ast::AstFamily::kStructuredType, "TYPE_DDL",
       {"type_family", "constructor_descriptor", "serialization_profile"}},
      {"sbsql.ddl_catalog_gaps", "DdlCatalogAst",
       ast::AstFamily::kDdlCatalog, "CATALOG_DDL",
       {"catalog_object_kind", "lifecycle_transition", "mga_root_mutation_profile"}},
      {"sbsql.dml_upsert_variants", "DmlAst",
       ast::AstFamily::kDml, "DATA_MUTATION",
       {"dml_action", "excluded_scope", "returning_clause"}},
      {"sbsql.bulk_import_export", "BulkImportExportAst",
       ast::AstFamily::kBulkImportExport, "BULK_IMPORT_EXPORT",
       {"format_descriptor", "stream_frame_descriptor", "reject_policy"}},
      {"sbsql.native_system_variables", "SystemVariableReferenceAst",
       ast::AstFamily::kSystemVariable, "SESSION_VARIABLE_READ",
       {"canonical_variable_id", "session_scope", "result_descriptor"}},
      {"sbsql.last_day_builtin", "TemporalLastDayBuiltinAst",
       ast::AstFamily::kSystemVariable, "EXPRESSION_EVALUATE",
       {"function_id", "calendar_policy", "null_semantics"}},
      {"sbsql.acceleration_management", "AccelerationManagementAst",
       ast::AstFamily::kAcceleration, "ACCELERATION_INSPECT_OR_CONTROL",
       {"acceleration_family", "profile_ref", "cache_invalidation_epoch"}},
      {"sbsql.transaction_lock_compatibility", "TransactionLockCompatibilityAst",
       ast::AstFamily::kTransactionControl, "TRANSACTION_CONTROL",
       {"lock_action", "compatibility_policy", "mga_visibility_impact"}},
  };

  bool ok = true;
  for (const auto& item : cases) ok &= ValidateAcceptedFamily(item);
  ok &= ValidateUnknownRefusal();
  ok &= ValidateRawTextAuthorityRejection();
  if (!ok) return 1;
  std::cout << "SBSQL missing-functionality AST/BoundAST contract passed\n";
  return 0;
}
