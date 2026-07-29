// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "binder/binder.hpp"

#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace scratchbird::parser::sbsql {
namespace {

bool RequiresDescriptorAuthority(const AstDocument& ast) {
  return ast.statement_binding_contract_key == "binder.statement.public_authority_required" ||
         ast.native_relational.recognized() ||
         ast.requires_name_resolution;
}

bool RequiresSecurityAuthority(const AstDocument& ast) {
  return ast.statement_parser_category == "security" ||
         ast.statement_parser_category == "migration" ||
         ast.statement_parser_category == "bridge" ||
         ast.family == StatementFamily::kBridge ||
         ast.statement_binding_contract_key == "binder.statement.cluster_profile_gate";
}

bool RequiresTransactionAuthority(const AstDocument& ast) {
  return ast.statement_parser_category == "transaction" ||
         ast.statement_parser_category == "migration" ||
         ast.statement_binding_contract_key == "binder.statement.transaction_context";
}

bool TokenTextIs(const Token& token, std::string_view expected) {
  return ToUpperAscii(token.text) == expected;
}

bool IsSourceFreeCteRoute(const CstDocument& cst) {
  std::vector<const Token*> tokens;
  for (const auto& token : cst.tokens) {
    if (IsTriviaToken(token)) continue;
    tokens.push_back(&token);
  }
  if (tokens.empty()) return false;
  std::size_t first = 0;
  if (TokenTextIs(*tokens[first], "EXPLAIN")) {
    ++first;
    if (first >= tokens.size()) return false;
  }
  if (TokenTextIs(*tokens[first], "WITH")) return true;
  if (!TokenTextIs(*tokens[first], "SELECT")) return false;
  for (std::size_t index = first + 1; index + 2 < tokens.size(); ++index) {
    if (TokenTextIs(*tokens[index], "FROM") &&
        tokens[index + 1]->text == "(" &&
        TokenTextIs(*tokens[index + 2], "WITH")) {
      return true;
    }
  }
  return false;
}

bool IsQualifiedNamePartToken(const Token& token) {
  return token.kind == TokenKind::kIdentifier || token.kind == TokenKind::kKeyword;
}

bool ConsumeEngineOwnedProjectionPath(const std::vector<const Token*>& tokens,
                                      std::size_t* index) {
  if (index == nullptr) return false;
  std::size_t cursor = *index;
  std::vector<std::string> parts;
  bool expect_part = true;
  while (cursor < tokens.size()) {
    const auto& token = *tokens[cursor];
    if (expect_part) {
      if (!IsQualifiedNamePartToken(token)) break;
      parts.push_back(ToUpperAscii(token.text));
      expect_part = false;
      ++cursor;
      continue;
    }
    if (token.text == ".") {
      expect_part = true;
      ++cursor;
      continue;
    }
    break;
  }
  if (parts.size() < 2 || expect_part) return false;
  bool engine_owned = parts.front() == "SYS" ||
                      parts.front() == "INFORMATION" ||
                      parts.front() == "EMULATED";
  for (std::size_t part = 1; part < parts.size(); ++part) {
    engine_owned = engine_owned || parts[part] == "SYS" ||
                   parts[part] == "INFORMATION" ||
                   parts[part] == "EMULATED";
  }
  if (!engine_owned) return false;
  *index = cursor;
  return true;
}

bool IsSourceFreeCatalogProjectionCountRoute(const CstDocument& cst) {
  std::vector<const Token*> tokens;
  for (const auto& token : cst.tokens) {
    if (IsTriviaToken(token)) continue;
    tokens.push_back(&token);
  }
  if (tokens.empty() || !TokenTextIs(*tokens.front(), "SELECT")) return false;

  std::size_t from_index = tokens.size();
  int paren_depth = 0;
  bool saw_count_projection = false;
  for (std::size_t index = 1; index < tokens.size(); ++index) {
    if (tokens[index]->text == "(") {
      ++paren_depth;
    } else if (tokens[index]->text == ")" && paren_depth > 0) {
      --paren_depth;
    }
    if (paren_depth == 0 && TokenTextIs(*tokens[index], "FROM")) {
      from_index = index;
      break;
    }
    if (TokenTextIs(*tokens[index], "COUNT")) {
      saw_count_projection = true;
    }
  }
  if (!saw_count_projection || from_index == tokens.size()) return false;

  std::size_t relation_index = from_index + 1;
  return ConsumeEngineOwnedProjectionPath(tokens, &relation_index);
}

std::string ResultShapeFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "result.shape.rowset";
  }
  if (ast.family == StatementFamily::kShow || ast.family == StatementFamily::kObservability) {
    return "result.shape.management_report";
  }
  if (ast.family == StatementFamily::kCall) return "result.shape.routine_result";
  return "result.shape.command_status";
}

std::string ResourceContractFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "resource.contract.query_read";
  }
  if (ast.family == StatementFamily::kInsert || ast.family == StatementFamily::kUpdate ||
      ast.family == StatementFamily::kDelete || ast.family == StatementFamily::kMerge ||
      ast.family == StatementFamily::kUpsert) {
    return "resource.contract.dml_write";
  }
  if (ast.family == StatementFamily::kCatalog || ast.family == StatementFamily::kSecurity ||
      ast.family == StatementFamily::kStorageManagement ||
      ast.family == StatementFamily::kMigration ||
      ast.family == StatementFamily::kBridge) {
    return "resource.contract.metadata_mutation";
  }
  return "resource.contract.control";
}

std::string RequiredRightFor(const AstDocument& ast) {
  if (ast.family == StatementFamily::kQuery || ast.family == StatementFamily::kValues) {
    return "right.read";
  }
  if (ast.family == StatementFamily::kInsert || ast.family == StatementFamily::kUpdate ||
      ast.family == StatementFamily::kDelete || ast.family == StatementFamily::kMerge ||
      ast.family == StatementFamily::kUpsert) {
    return "right.write";
  }
  if (ast.family == StatementFamily::kCatalog) return "right.catalog_mutate";
  if (ast.family == StatementFamily::kSecurity) return "right.security_admin";
  if (ast.family == StatementFamily::kTransaction) return "right.transaction_control";
  if (ast.family == StatementFamily::kMigration) return "right.migrate_database";
  if (ast.family == StatementFamily::kBridge) return "right.bridge.use";
  if (ast.family == StatementFamily::kShow || ast.family == StatementFamily::kObservability) {
    return "right.observe";
  }
  return "right.execute";
}

void PopulateAuthorityMetadata(BoundStatement* bound, const AstDocument& ast) {
  bound->statement_surface_id = ast.statement_surface_id;
  bound->statement_surface_name = ast.statement_surface_name;
  bound->statement_parser_category = ast.statement_parser_category;
  bound->parser_handler_key = ast.parser_handler_key;
  bound->binding_contract_key = ast.statement_binding_contract_key;
  bound->admission_contract_key = ast.statement_admission_contract_key;
  bound->behavior_descriptor_key = ast.statement_behavior_descriptor_key;
  bound->diagnostic_key = ast.diagnostic_key;
  bound->requires_name_resolution = ast.requires_name_resolution;
  bound->requires_descriptor_authority = RequiresDescriptorAuthority(ast);
  bound->requires_security_authority = RequiresSecurityAuthority(ast);
  bound->requires_transaction_authority = RequiresTransactionAuthority(ast);
  bound->requires_cluster_profile = ast.requires_cluster_profile;
  bound->exact_refusal_required = ast.exact_refusal_required;
  bound->command_family = ast.statement_kind;
  bound->surface_key = ast.statement_surface_id.empty() ? ast.registry_family
                                                        : ast.statement_surface_id;
  bound->sblr_operation_key = ast.operation_family;
  bound->result_shape_key = ResultShapeFor(ast);
  bound->diagnostic_shape_key = ast.diagnostic_key.empty() ? "diagnostic.canonical_message_vector"
                                                           : ast.diagnostic_key;
  bound->resource_contract_key = ResourceContractFor(ast);
  bound->conformance_case_key = ast.statement_surface_id.empty()
                                    ? "conformance.unclassified_statement"
                                    : "conformance." + ast.statement_surface_id;
  bound->trace_key = "trace.bound_ast." + std::to_string(ast.source_hash);
  bound->edition_gate_result = "edition_gate.not_evaluated_parser_binder";
  bound->profile_gate_result = bound->requires_cluster_profile
                                   ? "profile_gate.cluster_required"
                                   : "profile_gate.public_or_default";
  bound->granted_scope = "granted_scope.pending_server_authority";
  bound->required_rights.push_back(RequiredRightFor(ast));
  if (bound->requires_descriptor_authority) {
    bound->descriptor_refs.push_back("descriptor.pending_server_or_engine_authority");
  }
  if (bound->requires_security_authority) {
    bound->policy_refs.push_back("policy.pending_server_security_authority");
  }

  bound->name_resolution_authority_key =
      bound->requires_name_resolution ? "authority.server.resolve_name_registry_public"
                                      : "authority.not_required.parser_syntax_only";
  bound->descriptor_authority_key =
      bound->requires_descriptor_authority ? "authority.engine.descriptor_context_required"
                                           : "authority.not_required.parser_syntax_only";
  bound->security_authority_key =
      bound->requires_security_authority ? "authority.server.security_policy_context_required"
                                         : "authority.not_required.parser_syntax_only";
  bound->transaction_authority_key =
      bound->requires_transaction_authority ? "authority.server.transaction_context_required"
                                            : "authority.not_required.parser_syntax_only";

  bound->required_authority_steps.push_back("authority.parser.syntax_evidence_only");
  if (!bound->statement_surface_id.empty()) {
    bound->required_authority_steps.push_back("authority.parser.surface_descriptor_candidate");
  }
  if (bound->requires_name_resolution) {
    bound->required_authority_steps.push_back(bound->name_resolution_authority_key);
  }
  if (bound->requires_descriptor_authority) {
    bound->required_authority_steps.push_back(bound->descriptor_authority_key);
  }
  if (bound->requires_security_authority) {
    bound->required_authority_steps.push_back(bound->security_authority_key);
  }
  if (bound->requires_transaction_authority) {
    bound->required_authority_steps.push_back(bound->transaction_authority_key);
  }
  if (bound->requires_cluster_profile) {
    bound->required_authority_steps.push_back("authority.cluster.profile_gate_required");
  }
}

void AddBoundAstDiagnostic(BoundNativeRelationalDocument* document,
                           std::string code,
                           std::string message,
                           std::vector<Field> fields = {}) {
  if (document->messages.has_errors()) return;
  document->messages.diagnostics.push_back(MakeDiagnostic(
      std::move(code), "ERROR", std::move(message), "sbp_sbsql.native_binder",
      std::move(fields)));
}

bool LooksLikeUuidV7(const std::string_view value) {
  return LooksLikeCanonicalUuid(value) && value[14] == '7';
}

std::string_view ExpectedAggregateSemanticVariant(
    const NativeAggregateGroupingForm grouping_form,
    const NativeAggregateProjectionForm projection_form) {
  if (grouping_form == NativeAggregateGroupingForm::kSimple) {
    if (projection_form == NativeAggregateProjectionForm::kKeyCountSum) {
      return "aggregate.grouped-int64-key-count-sum.v1";
    }
    if (projection_form == NativeAggregateProjectionForm::kKeysCountSum) {
      return "aggregate.grouped-int64-keys-count-sum.v1";
    }
    return {};
  }
  const bool projects_grouping_metadata =
      projection_form ==
      NativeAggregateProjectionForm::kKeysCountSumGrouping;
  if (projection_form != NativeAggregateProjectionForm::kKeysCountSum &&
      !projects_grouping_metadata) {
    return {};
  }
  switch (grouping_form) {
    case NativeAggregateGroupingForm::kGroupingSets:
      return projects_grouping_metadata
                 ? "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1"
                 : "aggregate.grouping-sets-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kRollup:
      return projects_grouping_metadata
                 ? "aggregate.rollup-int64-keys-count-sum-grouping.v1"
                 : "aggregate.rollup-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kCube:
      return projects_grouping_metadata
                 ? "aggregate.cube-int64-keys-count-sum-grouping.v1"
                 : "aggregate.cube-int64-keys-count-sum.v1";
    case NativeAggregateGroupingForm::kSimple:
    case NativeAggregateGroupingForm::kNone:
      return {};
  }
  return {};
}

BoundNativeRelationalDocument RefusedBoundAst(
    BoundNativeRelationalDocument document) {
  document.bound = false;
  document.bound_ast_uuid.clear();
  document.security_context_uuid.clear();
  document.root_relation_id = 0;
  document.root_scope_id = 0;
  document.descriptors.clear();
  document.expressions.clear();
  document.values_rows.clear();
  document.grouping_sets.clear();
  document.outputs.clear();
  document.relations.clear();
  document.scopes.clear();
  return document;
}

} // namespace

// QOW-SOURCE-QRY-001-BINDING-V1
BoundNativeRelationalDocument BindNativeRelationalAst(
    const NativeRelationalAstDocument& ast,
    const NativeRelationalBindingContext& context) {
  BoundNativeRelationalDocument bound;
  if (!ast.accepted()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "only an accepted typed relational AST can be bound");
    return RefusedBoundAst(std::move(bound));
  }
  if (!LooksLikeUuidV7(context.bound_ast_uuid)) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-SCOPE",
                          "binding requires a non-null UUIDv7 BoundAST identity");
    return RefusedBoundAst(std::move(bound));
  }
  if (!LooksLikeCanonicalUuid(context.catalog_epoch_uuid)) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-SCOPE",
                          "binding requires an engine-supplied catalog epoch UUID");
    return RefusedBoundAst(std::move(bound));
  }
  if (!LooksLikeCanonicalUuid(context.security_context_uuid)) {
    AddBoundAstDiagnostic(
        &bound, "QOW-DIAG-BOUNDAST-SCOPE",
        "binding requires an engine-supplied security context UUID");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, const NativeDescriptorBindingInput*>
      descriptor_by_id;
  std::unordered_set<std::string> descriptor_uuids;
  for (const auto& descriptor : context.descriptors) {
    if (descriptor.descriptor_id == 0 ||
        !LooksLikeCanonicalUuid(descriptor.descriptor_uuid) ||
        !LooksLikeCanonicalUuid(descriptor.type_uuid) ||
        (descriptor.collation_uuid.has_value() &&
         !LooksLikeCanonicalUuid(*descriptor.collation_uuid)) ||
        (descriptor.width_precision_scale.scale.has_value() &&
         (!descriptor.width_precision_scale.precision.has_value() ||
          *descriptor.width_precision_scale.scale >
              *descriptor.width_precision_scale.precision))) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                            "descriptor binding contains an invalid typed field");
      return RefusedBoundAst(std::move(bound));
    }
    if (!descriptor_by_id.emplace(descriptor.descriptor_id, &descriptor).second ||
        !descriptor_uuids.emplace(descriptor.descriptor_uuid).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                            "descriptor IDs and UUID handles must be unique");
      return RefusedBoundAst(std::move(bound));
    }
  }
  if (descriptor_by_id.empty()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                          "typed relational binding requires descriptor handles");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, const NativeExpressionBindingInput*>
      expression_binding_by_id;
  for (const auto& expression_binding : context.expressions) {
    if (expression_binding.expression_id == 0 ||
        expression_binding.descriptor_id == 0 ||
        descriptor_by_id.find(expression_binding.descriptor_id) ==
            descriptor_by_id.end() ||
        !expression_binding_by_id
             .emplace(expression_binding.expression_id, &expression_binding)
             .second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "expression binding contains a missing or duplicate handle");
      return RefusedBoundAst(std::move(bound));
    }
  }

  std::unordered_map<std::uint32_t, const NativeExpressionAstNode*> ast_expression_by_id;
  for (const auto& expression : ast.expressions) {
    if (expression.expression_id == 0 ||
        !ast_expression_by_id.emplace(expression.expression_id, &expression).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "typed AST expression handles are invalid");
      return RefusedBoundAst(std::move(bound));
    }
  }
  if (expression_binding_by_id.size() != ast_expression_by_id.size()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                          "every typed AST expression requires exactly one binding");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, std::uint8_t> expression_visit_state;
  std::function<bool(std::uint32_t)> visit_expression =
      [&](const std::uint32_t expression_id) {
        const auto state = expression_visit_state[expression_id];
        if (state == 1) return false;
        if (state == 2) return true;
        const auto expression_iterator = ast_expression_by_id.find(expression_id);
        if (expression_iterator == ast_expression_by_id.end()) return false;
        expression_visit_state[expression_id] = 1;
        for (const auto child_id : expression_iterator->second->child_expression_ids) {
          if (child_id == 0 || !visit_expression(child_id)) return false;
        }
        expression_visit_state[expression_id] = 2;
        return true;
      };
  for (const auto& [expression_id, expression] : ast_expression_by_id) {
    (void)expression;
    if (!visit_expression(expression_id)) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "expression graph contains a cycle or dangling child");
      return RefusedBoundAst(std::move(bound));
    }
  }

  std::unordered_set<std::uint32_t> used_descriptor_ids;
  bound.expressions.reserve(ast.expressions.size());
  for (const auto& expression : ast.expressions) {
    const auto binding_iterator =
        expression_binding_by_id.find(expression.expression_id);
    if (binding_iterator == expression_binding_by_id.end()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "typed AST expression is missing its descriptor binding");
      return RefusedBoundAst(std::move(bound));
    }
    const auto& expression_binding = *binding_iterator->second;
    const bool function_call =
        expression.expression_kind == NativeExpressionAstKind::kFunctionCall;
    const bool identifier =
        expression.expression_kind == NativeExpressionAstKind::kIdentifier;
    if (function_call != expression_binding.function_uuid.has_value() ||
        (expression_binding.function_uuid.has_value() &&
         !LooksLikeCanonicalUuid(*expression_binding.function_uuid))) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "function UUID state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    if (identifier != expression_binding.bound_name_uuid.has_value() ||
        (expression_binding.bound_name_uuid.has_value() &&
         !LooksLikeCanonicalUuid(*expression_binding.bound_name_uuid))) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "identifier UUID state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    const bool literal =
        expression.expression_kind == NativeExpressionAstKind::kLiteral;
    if (literal != expression.literal_kind.has_value()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "literal kind state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    const bool operator_expression =
        expression.expression_kind == NativeExpressionAstKind::kUnary ||
        expression.expression_kind == NativeExpressionAstKind::kBinary;
    if ((operator_expression && expression.operator_name.empty()) ||
        (!operator_expression && !function_call &&
         !expression.operator_name.empty())) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                            "operator identity state does not match the expression kind");
      return RefusedBoundAst(std::move(bound));
    }
    for (const auto child_id : expression.child_expression_ids) {
      if (child_id == 0 || ast_expression_by_id.find(child_id) == ast_expression_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-EXPRESSION",
                              "expression contains a dangling child handle");
        return RefusedBoundAst(std::move(bound));
      }
    }

    BoundExpressionAstRecord record;
    record.expression_id = expression.expression_id;
    record.expression_kind = expression.expression_kind;
    record.literal_kind = expression.literal_kind;
    record.child_expression_ids = expression.child_expression_ids;
    record.result_descriptor_id = expression_binding.descriptor_id;
    record.bound_function_uuid = expression_binding.function_uuid;
    record.bound_name_uuid = expression_binding.bound_name_uuid;
    if (operator_expression) {
      record.canonical_operator_name = expression.operator_name;
    }
    if (expression.expression_kind == NativeExpressionAstKind::kLiteral ||
        expression.expression_kind == NativeExpressionAstKind::kParameter) {
      record.literal_or_parameter_ref = expression.spelling;
    }
    used_descriptor_ids.insert(record.result_descriptor_id);
    bound.expressions.push_back(std::move(record));
  }

  if (ast.values_rows.empty() || ast.relations.empty() ||
      ast.root_relation_id == 0) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "typed relational AST requires a reachable VALUES-backed root");
    return RefusedBoundAst(std::move(bound));
  }
  std::unordered_set<std::uint32_t> values_row_ids;
  const auto& first_row = ast.values_rows.front();
  const auto values_arity = first_row.expression_ids.size();
  if (values_arity == 0) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "VALUES rows require at least one typed expression handle");
    return RefusedBoundAst(std::move(bound));
  }
  bound.values_rows.reserve(ast.values_rows.size());
  for (const auto& row : ast.values_rows) {
    if (row.row_id == 0 || !values_row_ids.insert(row.row_id).second ||
        row.expression_ids.size() != values_arity) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "VALUES row handles and arity must be exact");
      return RefusedBoundAst(std::move(bound));
    }
    for (const auto expression_id : row.expression_ids) {
      if (expression_id == 0 ||
          ast_expression_by_id.find(expression_id) == ast_expression_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "VALUES row contains a dangling expression handle");
        return RefusedBoundAst(std::move(bound));
      }
    }
    bound.values_rows.push_back({row.row_id, row.expression_ids});
  }

  std::unordered_map<std::uint32_t, const NativeRelationAstNode*>
      ast_relation_by_id;
  for (const auto& relation : ast.relations) {
    if (relation.relation_id == 0 ||
        !ast_relation_by_id.emplace(relation.relation_id, &relation).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "typed relation handles must be nonzero and unique");
      return RefusedBoundAst(std::move(bound));
    }
  }
  const auto root_relation = ast_relation_by_id.find(ast.root_relation_id);
  if (root_relation == ast_relation_by_id.end() ||
      ast.relations.size() < 1 || ast.relations.size() > 3) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "native relation graph is outside the bounded profile");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_map<std::uint32_t, const NativeRelationBindingInput*>
      relation_binding_by_id;
  for (const auto& relation_binding : context.relations) {
    if (relation_binding.relation_id == 0 ||
        relation_binding.semantic_variant_id.empty() ||
        !ast_relation_by_id.contains(relation_binding.relation_id) ||
        !relation_binding_by_id
             .emplace(relation_binding.relation_id, &relation_binding)
             .second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "relation semantic binding is missing or contradictory");
      return RefusedBoundAst(std::move(bound));
    }
  }

  std::size_t expected_output_count = 0;
  const NativeRelationAstNode* values_relation_ast = nullptr;
  const NativeRelationAstNode* aggregate_relation_ast = nullptr;
  const NativeRelationAstNode* filter_relation_ast = nullptr;
  bound.relations.reserve(ast.relations.size());
  for (const auto& relation : ast.relations) {
    for (const auto input_id : relation.input_relation_ids) {
      if (input_id == 0 || input_id == relation.relation_id ||
          !ast_relation_by_id.contains(input_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation graph contains a dangling or cyclic input");
        return RefusedBoundAst(std::move(bound));
      }
    }
    for (const auto expression_id : relation.output_expression_ids) {
      if (!ast_expression_by_id.contains(expression_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation output contains a dangling expression");
        return RefusedBoundAst(std::move(bound));
      }
    }
    for (const auto expression_id : relation.predicate_expression_ids) {
      if (!ast_expression_by_id.contains(expression_id)) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "relation predicate contains a dangling expression");
        return RefusedBoundAst(std::move(bound));
      }
    }

    BoundRelationAstRecord record;
    record.relation_id = relation.relation_id;
    record.relation_kind = relation.relation_kind;
    record.aggregate_grouping_form = relation.aggregate_grouping_form;
    record.aggregate_projection_form = relation.aggregate_projection_form;
    record.input_relation_ids = relation.input_relation_ids;
    record.values_row_ids = relation.values_row_ids;
    record.output_expression_ids = relation.output_expression_ids;
    record.grouping_key_expression_ids = relation.grouping_key_expression_ids;
    record.aggregate_expression_ids = relation.aggregate_expression_ids;
    record.predicate_expression_ids = relation.predicate_expression_ids;
    record.bound_object_uuid = std::nullopt;
    record.lateral = false;

    if (relation.relation_kind == NativeRelationAstKind::kValues) {
      if (values_relation_ast != nullptr || !relation.input_relation_ids.empty() ||
          relation.values_row_ids.size() != bound.values_rows.size() ||
          relation.output_expression_ids != first_row.expression_ids ||
          relation.aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          relation.aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !relation.grouping_key_expression_ids.empty() ||
          !relation.aggregate_expression_ids.empty() ||
          !relation.predicate_expression_ids.empty()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed VALUES relation graph is not canonical");
        return RefusedBoundAst(std::move(bound));
      }
      for (std::size_t index = 0; index < relation.values_row_ids.size(); ++index) {
        if (relation.values_row_ids[index] != bound.values_rows[index].row_id) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "VALUES relation row handles are not canonical");
          return RefusedBoundAst(std::move(bound));
        }
      }
      values_relation_ast = &relation;
      record.semantic_variant_id = "values.literal-table.v1";
      for (const auto& row : bound.values_rows) {
        record.bound_expression_ids.insert(record.bound_expression_ids.end(),
                                           row.expression_ids.begin(),
                                           row.expression_ids.end());
      }
    } else if (relation.relation_kind == NativeRelationAstKind::kAggregate) {
      const bool projects_key_count_sum =
          relation.aggregate_projection_form ==
          NativeAggregateProjectionForm::kKeyCountSum;
      const bool projects_keys_count_sum =
          relation.aggregate_projection_form ==
          NativeAggregateProjectionForm::kKeysCountSum;
      const bool projects_grouping_metadata =
          relation.aggregate_projection_form ==
          NativeAggregateProjectionForm::kKeysCountSumGrouping;
      const bool one_key_profile =
          relation.aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          projects_key_count_sum;
      const bool two_key_profile =
          (relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kSimple &&
           projects_keys_count_sum) ||
          ((relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kGroupingSets ||
           relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kRollup ||
           relation.aggregate_grouping_form ==
               NativeAggregateGroupingForm::kCube) &&
           (projects_keys_count_sum || projects_grouping_metadata));
      const std::size_t expected_output_count =
          one_key_profile ? 3 : (projects_grouping_metadata ? 7 : 4);
      bool output_shape_matches = false;
      if (one_key_profile &&
          relation.grouping_key_expression_ids.size() == 1 &&
          relation.aggregate_expression_ids.size() == 2 &&
          relation.output_expression_ids.size() == 3) {
        output_shape_matches =
            relation.output_expression_ids[0] ==
                relation.grouping_key_expression_ids[0] &&
            relation.output_expression_ids[1] ==
                relation.aggregate_expression_ids[0] &&
            relation.output_expression_ids[2] ==
                relation.aggregate_expression_ids[1];
      } else if (two_key_profile &&
                 relation.grouping_key_expression_ids.size() == 2 &&
                 relation.aggregate_expression_ids.size() == 2 &&
                 relation.output_expression_ids.size() ==
                     expected_output_count) {
        output_shape_matches =
            relation.output_expression_ids[0] ==
                relation.grouping_key_expression_ids[0] &&
            relation.output_expression_ids[1] ==
                relation.grouping_key_expression_ids[1] &&
            relation.output_expression_ids[2] ==
                relation.aggregate_expression_ids[0] &&
            relation.output_expression_ids[3] ==
                relation.aggregate_expression_ids[1];
      }
      if (aggregate_relation_ast != nullptr ||
          relation.input_relation_ids.size() != 1 ||
          !relation.values_row_ids.empty() ||
          !relation.predicate_expression_ids.empty() ||
          (!one_key_profile && !two_key_profile) ||
          relation.output_expression_ids.size() != expected_output_count ||
          !output_shape_matches) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed aggregate relation is outside the bounded profile");
        return RefusedBoundAst(std::move(bound));
      }
      if (projects_grouping_metadata) {
        const auto grouping_a =
            ast_expression_by_id.find(relation.output_expression_ids[4]);
        const auto grouping_b =
            ast_expression_by_id.find(relation.output_expression_ids[5]);
        const auto grouping_id =
            ast_expression_by_id.find(relation.output_expression_ids[6]);
        const auto metadata_descriptor_is_exact =
            [&](const auto expression) {
              if (expression == ast_expression_by_id.end()) return false;
              const auto binding =
                  expression_binding_by_id.find(expression->first);
              if (binding == expression_binding_by_id.end()) return false;
              const auto descriptor =
                  descriptor_by_id.find(binding->second->descriptor_id);
              return descriptor != descriptor_by_id.end() &&
                     !binding->second->function_uuid.has_value() &&
                     !binding->second->bound_name_uuid.has_value() &&
                     descriptor->second->nullability ==
                         BoundNullability::kNonNull &&
                     !descriptor->second->collation_uuid.has_value() &&
                     !descriptor->second->timezone_profile_id.has_value() &&
                     !descriptor->second->width_precision_scale.width
                          .has_value() &&
                     !descriptor->second->width_precision_scale.precision
                          .has_value() &&
                     !descriptor->second->width_precision_scale.scale
                          .has_value();
            };
        if (grouping_a == ast_expression_by_id.end() ||
            grouping_b == ast_expression_by_id.end() ||
            grouping_id == ast_expression_by_id.end() ||
            grouping_a->second->expression_kind !=
                NativeExpressionAstKind::kUnary ||
            grouping_a->second->operator_name != "grouping" ||
            grouping_a->second->child_expression_ids !=
                std::vector<std::uint32_t>{
                    relation.grouping_key_expression_ids[0]} ||
            grouping_b->second->expression_kind !=
                NativeExpressionAstKind::kUnary ||
            grouping_b->second->operator_name != "grouping" ||
            grouping_b->second->child_expression_ids !=
                std::vector<std::uint32_t>{
                    relation.grouping_key_expression_ids[1]} ||
            grouping_id->second->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            grouping_id->second->operator_name != "grouping_id" ||
            grouping_id->second->child_expression_ids !=
                relation.grouping_key_expression_ids ||
            !metadata_descriptor_is_exact(grouping_a) ||
            !metadata_descriptor_is_exact(grouping_b) ||
            !metadata_descriptor_is_exact(grouping_id)) {
          AddBoundAstDiagnostic(
              &bound, "QOW-DIAG-BOUNDAST-RELATION",
              "grouping metadata projections must bind the two ordered keys and non-null descriptors exactly");
          return RefusedBoundAst(std::move(bound));
        }
      }
      const auto semantic_binding =
          relation_binding_by_id.find(relation.relation_id);
      if (semantic_binding == relation_binding_by_id.end()) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed aggregate requires an authoritative semantic binding");
        return RefusedBoundAst(std::move(bound));
      }
      const auto expected_semantic =
          ExpectedAggregateSemanticVariant(
              relation.aggregate_grouping_form,
              relation.aggregate_projection_form);
      if (expected_semantic.empty() ||
          semantic_binding->second->semantic_variant_id != expected_semantic) {
        AddBoundAstDiagnostic(
            &bound, "QOW-DIAG-BOUNDAST-RELATION",
            "aggregate semantic binding contradicts the parsed grouping form");
        return RefusedBoundAst(std::move(bound));
      }
      aggregate_relation_ast = &relation;
      record.semantic_variant_id =
          semantic_binding->second->semantic_variant_id;
      record.bound_expression_ids = relation.output_expression_ids;
    } else if (relation.relation_kind == NativeRelationAstKind::kFilter) {
      const auto semantic_binding =
          relation_binding_by_id.find(relation.relation_id);
      const NativeExpressionAstNode* predicate = nullptr;
      const NativeExpressionAstNode* boolean_root = nullptr;
      const NativeExpressionAstNode* count_comparison = nullptr;
      const NativeExpressionAstNode* sum_comparison = nullptr;
      const NativeExpressionAstNode* having_count = nullptr;
      const NativeExpressionAstNode* count_threshold = nullptr;
      const NativeExpressionAstNode* having_sum = nullptr;
      const NativeExpressionAstNode* sum_threshold = nullptr;
      const NativeExpressionAstNode* having_argument = nullptr;
      const NativeExpressionAstNode* projected_count = nullptr;
      const NativeExpressionAstNode* projected_sum = nullptr;
      const NativeExpressionAstNode* projected_argument = nullptr;
      if (relation.predicate_expression_ids.size() == 1) {
        predicate = ast_expression_by_id.at(
            relation.predicate_expression_ids.front());
        if (predicate->expression_kind == NativeExpressionAstKind::kBinary &&
            predicate->operator_name == ">" &&
            predicate->child_expression_ids.size() == 2) {
          sum_comparison = predicate;
        } else if (predicate->expression_kind ==
                       NativeExpressionAstKind::kUnary &&
                   predicate->operator_name == "NOT" &&
                   predicate->child_expression_ids.size() == 1) {
          const auto* operand = ast_expression_by_id.at(
              predicate->child_expression_ids.front());
          if (operand->expression_kind == NativeExpressionAstKind::kBinary &&
              (operand->operator_name == "AND" ||
               operand->operator_name == "OR") &&
              operand->child_expression_ids.size() == 2) {
            boolean_root = operand;
            count_comparison = ast_expression_by_id.at(
                operand->child_expression_ids[0]);
            sum_comparison = ast_expression_by_id.at(
                operand->child_expression_ids[1]);
          } else {
            const auto* function =
                operand->expression_kind == NativeExpressionAstKind::kBinary &&
                        operand->operator_name == ">" &&
                        operand->child_expression_ids.size() == 2
                    ? ast_expression_by_id.at(
                          operand->child_expression_ids.front())
                    : nullptr;
            if (function != nullptr &&
                function->expression_kind ==
                    NativeExpressionAstKind::kFunctionCall &&
                ToUpperAscii(function->operator_name) == "COUNT") {
              count_comparison = operand;
            } else {
              sum_comparison = operand;
            }
          }
        } else if (predicate->expression_kind ==
                       NativeExpressionAstKind::kBinary &&
                   (predicate->operator_name == "AND" ||
                    predicate->operator_name == "OR") &&
                   predicate->child_expression_ids.size() == 2) {
          boolean_root = predicate;
          count_comparison =
              ast_expression_by_id.at(predicate->child_expression_ids[0]);
          sum_comparison =
              ast_expression_by_id.at(predicate->child_expression_ids[1]);
        }
        if (count_comparison != nullptr &&
            count_comparison->expression_kind ==
                NativeExpressionAstKind::kBinary &&
            count_comparison->operator_name == ">" &&
            count_comparison->child_expression_ids.size() == 2) {
          having_count = ast_expression_by_id.at(
              count_comparison->child_expression_ids[0]);
          count_threshold = ast_expression_by_id.at(
              count_comparison->child_expression_ids[1]);
        }
        if (sum_comparison != nullptr &&
            sum_comparison->expression_kind ==
                NativeExpressionAstKind::kBinary &&
            sum_comparison->operator_name == ">" &&
            sum_comparison->child_expression_ids.size() == 2) {
          having_sum = ast_expression_by_id.at(
              sum_comparison->child_expression_ids[0]);
          sum_threshold = ast_expression_by_id.at(
              sum_comparison->child_expression_ids[1]);
          if (having_sum->child_expression_ids.size() == 1) {
            having_argument = ast_expression_by_id.at(
                having_sum->child_expression_ids.front());
          }
        }
      }
      if (aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_expression_ids.size() == 2) {
        projected_count = ast_expression_by_id.at(
            aggregate_relation_ast->aggregate_expression_ids[0]);
        projected_sum = ast_expression_by_id.at(
            aggregate_relation_ast->aggregate_expression_ids[1]);
        if (projected_sum->child_expression_ids.size() == 1) {
          projected_argument = ast_expression_by_id.at(
              projected_sum->child_expression_ids.front());
        }
      }
      const auto binding_for = [&](const NativeExpressionAstNode* expression) {
        return expression == nullptr
                   ? expression_binding_by_id.end()
                   : expression_binding_by_id.find(expression->expression_id);
      };
      const auto descriptor_is = [&](const NativeExpressionAstNode* expression,
                                     const BoundNullability nullability) {
        const auto binding = binding_for(expression);
        return binding != expression_binding_by_id.end() &&
               descriptor_by_id.at(binding->second->descriptor_id)
                       ->nullability == nullability;
      };
      const auto descriptor_is_unqualified =
          [&](const NativeExpressionAstNode* expression,
              const BoundNullability nullability) {
            const auto binding = binding_for(expression);
            if (binding == expression_binding_by_id.end()) return false;
            const auto descriptor =
                descriptor_by_id.at(binding->second->descriptor_id);
            return descriptor->nullability == nullability &&
                   !descriptor->collation_uuid.has_value() &&
                   !descriptor->timezone_profile_id.has_value() &&
                   !descriptor->width_precision_scale.width.has_value() &&
                   !descriptor->width_precision_scale.precision.has_value() &&
                   !descriptor->width_precision_scale.scale.has_value();
          };
      const auto predicate_binding = binding_for(predicate);
      const auto boolean_root_binding = binding_for(boolean_root);
      const auto count_comparison_binding = binding_for(count_comparison);
      const auto sum_comparison_binding = binding_for(sum_comparison);
      const auto having_count_binding = binding_for(having_count);
      const auto projected_count_binding = binding_for(projected_count);
      const auto having_sum_binding = binding_for(having_sum);
      const auto projected_sum_binding = binding_for(projected_sum);
      const auto having_argument_binding = binding_for(having_argument);
      const auto projected_argument_binding = binding_for(projected_argument);
      const bool simple_sum_profile =
          predicate != nullptr && sum_comparison == predicate;
      const bool count_sum_and_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root == predicate && predicate->operator_name == "AND";
      const bool count_sum_or_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root == predicate && predicate->operator_name == "OR";
      const bool not_count_sum_and_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root != nullptr && boolean_root != predicate &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "AND";
      const bool not_count_sum_or_profile =
          count_comparison != nullptr && predicate != nullptr &&
          boolean_root != nullptr && boolean_root != predicate &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{boolean_root->expression_id} &&
          boolean_root->operator_name == "OR";
      const bool not_count_sum_boolean_profile =
          not_count_sum_and_profile || not_count_sum_or_profile;
      const bool count_sum_boolean_profile =
          count_sum_and_profile || count_sum_or_profile ||
          not_count_sum_boolean_profile;
      const bool not_sum_profile =
          predicate != nullptr && sum_comparison != nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{sum_comparison->expression_id};
      const bool not_count_profile =
          predicate != nullptr && count_comparison != nullptr &&
          boolean_root == nullptr &&
          predicate->expression_kind == NativeExpressionAstKind::kUnary &&
          predicate->operator_name == "NOT" &&
          predicate->child_expression_ids ==
              std::vector<std::uint32_t>{count_comparison->expression_id};
      const bool not_sum_descriptors_are_exact =
          !not_sum_profile ||
          (descriptor_is_unqualified(predicate,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_comparison,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_threshold,
                                     BoundNullability::kNonNull));
      const bool not_count_sum_boolean_descriptors_are_exact =
          !not_count_sum_boolean_profile ||
          (descriptor_is_unqualified(predicate,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(boolean_root,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_comparison,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(sum_comparison,
                                      BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_threshold,
                                      BoundNullability::kNonNull) &&
           descriptor_is_unqualified(sum_threshold,
                                      BoundNullability::kNonNull));
      const bool not_count_descriptors_are_exact =
          !not_count_profile ||
          (descriptor_is_unqualified(predicate,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_comparison,
                                     BoundNullability::kNullable) &&
           descriptor_is_unqualified(count_threshold,
                                     BoundNullability::kNonNull) &&
           descriptor_is_unqualified(having_count,
                                     BoundNullability::kNonNull) &&
           descriptor_is_unqualified(projected_count,
                                     BoundNullability::kNonNull));
      const std::string_view expected_filter_semantic =
          not_count_sum_or_profile
              ? "filter.having-not-count-sum-or-gt-int64-literals.v1"
              : not_count_sum_and_profile
              ? "filter.having-not-count-sum-and-gt-int64-literals.v1"
              : not_count_profile
              ? "filter.having-not-count-gt-int64-literal.v1"
              : not_sum_profile
              ? "filter.having-not-sum-gt-int64-literal.v1"
              : count_sum_or_profile
              ? "filter.having-count-sum-or-gt-int64-literals.v1"
              : (count_sum_and_profile
                     ? "filter.having-count-sum-and-gt-int64-literals.v1"
                     : "filter.having-sum-gt-int64-literal.v1");
      // QOW-SOURCE-QRY-001-BINDING-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_one_key_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeyCountSum;
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_two_key_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_grouping_sets_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_grouping_sets_metadata_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_rollup_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_rollup_metadata_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_cube_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-COUNT-SUM-OR-GT-V1
      const bool admitted_cube_metadata_or_having =
          count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-SUM-GT-V1
      const bool admitted_two_key_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-COUNT-GT-V1
      const bool admitted_two_key_not_count_having =
          not_count_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_two_key_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-NOT-COUNT-SUM-OR-GT-V1
      const bool admitted_two_key_not_count_sum_or_having =
          not_count_sum_or_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_grouping_sets_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_grouping_sets_metadata_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_rollup_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_rollup_metadata_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_cube_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-NOT-COUNT-SUM-AND-GT-V1
      const bool admitted_cube_metadata_not_count_sum_and_having =
          not_count_sum_and_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-NOT-SUM-GT-V1
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool admitted_grouping_sets_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      const bool admitted_grouping_sets_metadata_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.size() == 4 &&
          ast.grouping_sets[0].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[0].ordinal == 0 &&
          ast.grouping_sets[0].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1]} &&
          ast.grouping_sets[1].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[1].ordinal == 1 &&
          ast.grouping_sets[1].expression_ids.empty() &&
          ast.grouping_sets[2].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[2].ordinal == 2 &&
          ast.grouping_sets[2].expression_ids ==
              std::vector<std::uint32_t>{
                  aggregate_relation_ast->grouping_key_expression_ids[1],
                  aggregate_relation_ast->grouping_key_expression_ids[0]} &&
          ast.grouping_sets[3].relation_id ==
              aggregate_relation_ast->relation_id &&
          ast.grouping_sets[3].ordinal == 3 &&
          ast.grouping_sets[3].expression_ids ==
              ast.grouping_sets[0].expression_ids;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-NOT-SUM-GT-V1
      const bool admitted_rollup_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool admitted_rollup_metadata_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-NOT-SUM-GT-V1
      const bool admitted_cube_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-NOT-SUM-GT-V1
      const bool admitted_cube_metadata_not_sum_having =
          not_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation_ast->grouping_key_expression_ids.size() == 2 &&
          ast.grouping_sets.empty();
      // QOW-SOURCE-QRY-001-BINDING-TWO-KEY-HAVING-SUM-GT-V1
      const bool admitted_simple_having =
          (simple_sum_profile ||
           (count_sum_and_profile && aggregate_relation_ast != nullptr &&
            aggregate_relation_ast->aggregate_projection_form ==
                NativeAggregateProjectionForm::kKeyCountSum)) &&
          aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kSimple &&
          (aggregate_relation_ast->aggregate_projection_form ==
               NativeAggregateProjectionForm::kKeyCountSum ||
           aggregate_relation_ast->aggregate_projection_form ==
               NativeAggregateProjectionForm::kKeysCountSum);
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-HAVING-SUM-GT-V1
      const bool admitted_grouping_sets_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-SUM-GT-V1
      const bool admitted_grouping_sets_metadata_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-HAVING-SUM-GT-V1
      const bool admitted_rollup_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-SUM-GT-V1
      const bool admitted_rollup_metadata_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kRollup &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      // QOW-SOURCE-QRY-001-BINDING-CUBE-HAVING-SUM-GT-V1
      const bool admitted_cube_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSum;
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-SUM-GT-V1
      const bool admitted_cube_metadata_sum_having =
          simple_sum_profile && aggregate_relation_ast != nullptr &&
          aggregate_relation_ast->aggregate_grouping_form ==
              NativeAggregateGroupingForm::kCube &&
          aggregate_relation_ast->aggregate_projection_form ==
              NativeAggregateProjectionForm::kKeysCountSumGrouping;
      // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-V1
      // QOW-SOURCE-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-V1
      const bool admitted_multi_key_boolean_having =
          count_sum_and_profile && aggregate_relation_ast != nullptr &&
          (aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kSimple ||
           aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kGroupingSets ||
           aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kRollup ||
           aggregate_relation_ast->aggregate_grouping_form ==
               NativeAggregateGroupingForm::kCube) &&
          (aggregate_relation_ast->aggregate_projection_form ==
               NativeAggregateProjectionForm::kKeysCountSum ||
           ((aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kGroupingSets ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kRollup ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kCube) &&
            aggregate_relation_ast->aggregate_projection_form ==
                NativeAggregateProjectionForm::kKeysCountSumGrouping));
      const auto metadata_not_sum_outputs_are_exact = [&] {
        if (!admitted_grouping_sets_metadata_not_count_sum_and_having &&
            !admitted_rollup_metadata_not_count_sum_and_having &&
            !admitted_cube_metadata_not_count_sum_and_having &&
            !admitted_grouping_sets_metadata_not_sum_having &&
            !admitted_rollup_metadata_not_sum_having &&
            !admitted_cube_metadata_not_sum_having) {
          return true;
        }
        constexpr std::array<std::string_view, 7> kOutputNames = {
            "key_a",      "key_b",     "row_count", "total_amount",
            "grouping_a", "grouping_b", "grouping_id"};
        for (const auto [relation_id, first_output_id] :
             {std::pair{aggregate_relation_ast->relation_id, 4U},
              std::pair{relation.relation_id, 11U}}) {
          for (std::size_t ordinal = 0; ordinal < kOutputNames.size();
               ++ordinal) {
            const auto output = std::ranges::find_if(
                context.outputs, [&](const auto& candidate) {
                  return candidate.relation_id == relation_id &&
                         candidate.ordinal == ordinal;
                });
            if (output == context.outputs.end() ||
                output->output_id != first_output_id + ordinal ||
                output->output_name_utf8 != kOutputNames[ordinal]) {
              return false;
            }
          }
        }
        return true;
      };
      const auto not_count_sum_and_outputs_are_exact = [&] {
        if (!admitted_two_key_not_count_having &&
            !admitted_two_key_not_count_sum_and_having &&
            !admitted_two_key_not_count_sum_or_having &&
            !admitted_grouping_sets_not_count_sum_and_having &&
            !admitted_rollup_not_count_sum_and_having &&
            !admitted_cube_not_count_sum_and_having) {
          return true;
        }
        constexpr std::array<std::string_view, 4> kOutputNames = {
            "key_a", "key_b", "row_count", "total_amount"};
        for (const auto [relation_id, first_output_id] :
             {std::pair{aggregate_relation_ast->relation_id, 4U},
              std::pair{relation.relation_id, 8U}}) {
          for (std::size_t ordinal = 0; ordinal < kOutputNames.size();
               ++ordinal) {
            const auto output = std::ranges::find_if(
                context.outputs, [&](const auto& candidate) {
                  return candidate.relation_id == relation_id &&
                         candidate.ordinal == ordinal;
                });
            if (output == context.outputs.end() ||
                output->output_id != first_output_id + ordinal ||
                output->output_name_utf8 != kOutputNames[ordinal] ||
                !output->visible) {
              return false;
            }
          }
        }
        return true;
      };
      if (filter_relation_ast != nullptr || aggregate_relation_ast == nullptr ||
          (!admitted_one_key_or_having && !admitted_two_key_or_having &&
           !admitted_grouping_sets_or_having &&
           !admitted_grouping_sets_metadata_or_having &&
           !admitted_rollup_or_having &&
           !admitted_rollup_metadata_or_having &&
           !admitted_cube_or_having &&
           !admitted_cube_metadata_or_having &&
           !admitted_two_key_not_sum_having &&
           !admitted_two_key_not_count_having &&
           !admitted_two_key_not_count_sum_and_having &&
           !admitted_two_key_not_count_sum_or_having &&
           !admitted_grouping_sets_not_count_sum_and_having &&
           !admitted_grouping_sets_metadata_not_count_sum_and_having &&
           !admitted_rollup_not_count_sum_and_having &&
           !admitted_rollup_metadata_not_count_sum_and_having &&
           !admitted_cube_not_count_sum_and_having &&
           !admitted_cube_metadata_not_count_sum_and_having &&
           !admitted_grouping_sets_not_sum_having &&
           !admitted_grouping_sets_metadata_not_sum_having &&
           !admitted_rollup_not_sum_having &&
           !admitted_rollup_metadata_not_sum_having &&
           !admitted_cube_not_sum_having &&
           !admitted_cube_metadata_not_sum_having &&
           !admitted_simple_having &&
           !admitted_grouping_sets_sum_having &&
           !admitted_grouping_sets_metadata_sum_having &&
           !admitted_rollup_sum_having &&
           !admitted_rollup_metadata_sum_having &&
           !admitted_cube_sum_having &&
           !admitted_cube_metadata_sum_having &&
           !admitted_multi_key_boolean_having) ||
          ((admitted_grouping_sets_metadata_not_count_sum_and_having ||
            admitted_rollup_metadata_not_count_sum_and_having ||
            admitted_cube_metadata_not_count_sum_and_having ||
            admitted_grouping_sets_metadata_not_sum_having ||
            admitted_rollup_metadata_not_sum_having ||
            admitted_cube_not_sum_having ||
            admitted_cube_metadata_not_sum_having) &&
           std::ranges::any_of(context.outputs,
                               [](const auto& output) {
                                 return !output.visible;
                               })) ||
          !metadata_not_sum_outputs_are_exact() ||
          !not_count_sum_and_outputs_are_exact() ||
          relation.relation_id != ast.root_relation_id ||
          relation.input_relation_ids !=
              std::vector<std::uint32_t>{aggregate_relation_ast->relation_id} ||
          !relation.values_row_ids.empty() ||
          relation.output_expression_ids !=
              aggregate_relation_ast->output_expression_ids ||
          relation.aggregate_grouping_form !=
              NativeAggregateGroupingForm::kNone ||
          relation.aggregate_projection_form !=
              NativeAggregateProjectionForm::kNone ||
          !relation.grouping_key_expression_ids.empty() ||
          !relation.aggregate_expression_ids.empty() || predicate == nullptr ||
          predicate_binding == expression_binding_by_id.end() ||
          (!not_count_profile &&
           (sum_comparison == nullptr || having_sum == nullptr ||
            sum_threshold == nullptr ||
            sum_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            sum_comparison->operator_name != ">" ||
            sum_comparison->child_expression_ids.size() != 2 ||
            having_sum->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_sum->operator_name) != "SUM" ||
            having_sum->child_expression_ids.size() != 1 ||
            having_argument == nullptr || projected_sum == nullptr ||
            projected_argument == nullptr ||
            having_argument->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            projected_argument->expression_kind !=
                NativeExpressionAstKind::kIdentifier ||
            sum_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            sum_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !sum_threshold->child_expression_ids.empty() ||
            having_sum_binding == expression_binding_by_id.end() ||
            projected_sum_binding == expression_binding_by_id.end() ||
            having_argument_binding == expression_binding_by_id.end() ||
            projected_argument_binding == expression_binding_by_id.end() ||
            having_sum_binding->second->function_uuid !=
                "019de5fc-2400-72e4-8549-82b2eef5a777" ||
            projected_sum_binding->second->function_uuid !=
                having_sum_binding->second->function_uuid ||
            having_sum_binding->second->descriptor_id !=
                projected_sum_binding->second->descriptor_id ||
            having_argument_binding->second->bound_name_uuid !=
                projected_argument_binding->second->bound_name_uuid ||
            having_argument_binding->second->descriptor_id !=
                projected_argument_binding->second->descriptor_id ||
            !descriptor_is(sum_comparison, BoundNullability::kNullable) ||
            !descriptor_is(sum_threshold, BoundNullability::kNonNull))) ||
          !descriptor_is(predicate, BoundNullability::kNullable) ||
          !not_sum_descriptors_are_exact ||
          !not_count_descriptors_are_exact ||
          !not_count_sum_boolean_descriptors_are_exact ||
          (not_sum_profile &&
           (predicate->expression_kind != NativeExpressionAstKind::kUnary ||
            predicate->operator_name != "NOT" ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{sum_comparison->expression_id} ||
            predicate_binding->second->descriptor_id !=
                sum_comparison_binding->second->descriptor_id)) ||
          (not_count_profile &&
           (count_comparison == nullptr || having_count == nullptr ||
            count_threshold == nullptr || projected_count == nullptr ||
            count_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            count_comparison->operator_name != ">" ||
            count_comparison->child_expression_ids !=
                std::vector<std::uint32_t>{having_count->expression_id,
                                           count_threshold->expression_id} ||
            having_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_count->operator_name) != "COUNT" ||
            !having_count->child_expression_ids.empty() ||
            projected_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(projected_count->operator_name) != "COUNT" ||
            !projected_count->child_expression_ids.empty() ||
            count_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            count_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !count_threshold->child_expression_ids.empty() ||
            count_comparison_binding == expression_binding_by_id.end() ||
            having_count_binding == expression_binding_by_id.end() ||
            projected_count_binding == expression_binding_by_id.end() ||
            having_count_binding->second->function_uuid !=
                "019de5fc-2400-784a-9aec-371f8b95b7ea" ||
            projected_count_binding->second->function_uuid !=
                having_count_binding->second->function_uuid ||
            having_count_binding->second->descriptor_id !=
                projected_count_binding->second->descriptor_id ||
            predicate->child_expression_ids !=
                std::vector<std::uint32_t>{count_comparison->expression_id} ||
            predicate_binding->second->descriptor_id !=
                count_comparison_binding->second->descriptor_id)) ||
          (count_sum_boolean_profile &&
           (boolean_root == nullptr ||
            boolean_root_binding == expression_binding_by_id.end() ||
            boolean_root->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            boolean_root->operator_name !=
                ((count_sum_or_profile || not_count_sum_or_profile)
                     ? "OR"
                     : "AND") ||
            boolean_root->child_expression_ids !=
                std::vector<std::uint32_t>{count_comparison->expression_id,
                                           sum_comparison->expression_id} ||
            !descriptor_is(boolean_root, BoundNullability::kNullable) ||
            count_comparison->expression_kind !=
                NativeExpressionAstKind::kBinary ||
            count_comparison->operator_name != ">" ||
            count_comparison->child_expression_ids.size() != 2 ||
            having_count == nullptr || count_threshold == nullptr ||
            projected_count == nullptr ||
            having_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(having_count->operator_name) != "COUNT" ||
            !having_count->child_expression_ids.empty() ||
            projected_count->expression_kind !=
                NativeExpressionAstKind::kFunctionCall ||
            ToUpperAscii(projected_count->operator_name) != "COUNT" ||
            !projected_count->child_expression_ids.empty() ||
            count_threshold->expression_kind !=
                NativeExpressionAstKind::kLiteral ||
            count_threshold->literal_kind != NativeLiteralAstKind::kNumeric ||
            !count_threshold->child_expression_ids.empty() ||
            having_count_binding == expression_binding_by_id.end() ||
            projected_count_binding == expression_binding_by_id.end() ||
            having_count_binding->second->function_uuid !=
                "019de5fc-2400-784a-9aec-371f8b95b7ea" ||
            projected_count_binding->second->function_uuid !=
                having_count_binding->second->function_uuid ||
            having_count_binding->second->descriptor_id !=
                projected_count_binding->second->descriptor_id ||
            !descriptor_is(count_comparison,
                           BoundNullability::kNullable) ||
            !descriptor_is(count_threshold, BoundNullability::kNonNull) ||
            boolean_root_binding->second->descriptor_id !=
                count_comparison_binding->second->descriptor_id ||
            boolean_root_binding->second->descriptor_id !=
                sum_comparison_binding->second->descriptor_id ||
            (!not_count_sum_boolean_profile && boolean_root != predicate) ||
            (not_count_sum_boolean_profile &&
             (predicate->expression_kind !=
                  NativeExpressionAstKind::kUnary ||
              predicate->operator_name != "NOT" ||
              predicate->child_expression_ids !=
                  std::vector<std::uint32_t>{boolean_root->expression_id} ||
              predicate_binding->second->descriptor_id !=
                  boolean_root_binding->second->descriptor_id)))) ||
          semantic_binding == relation_binding_by_id.end() ||
          semantic_binding->second->semantic_variant_id !=
              expected_filter_semantic) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "typed HAVING filter is outside the bounded profile");
        return RefusedBoundAst(std::move(bound));
      }
      filter_relation_ast = &relation;
      record.semantic_variant_id =
          semantic_binding->second->semantic_variant_id;
      record.bound_expression_ids = relation.predicate_expression_ids;
    }
    expected_output_count += relation.output_expression_ids.size();
    bound.relations.push_back(std::move(record));
  }
  if (values_relation_ast == nullptr ||
      (aggregate_relation_ast == nullptr && ast.relations.size() != 1) ||
      (aggregate_relation_ast != nullptr &&
       ((filter_relation_ast == nullptr &&
         (ast.relations.size() != 2 ||
          ast.root_relation_id != aggregate_relation_ast->relation_id)) ||
        (filter_relation_ast != nullptr &&
         (ast.relations.size() != 3 ||
          ast.root_relation_id != filter_relation_ast->relation_id ||
          filter_relation_ast->input_relation_ids.front() !=
              aggregate_relation_ast->relation_id)) ||
        aggregate_relation_ast->input_relation_ids.front() !=
            values_relation_ast->relation_id)) ||
      relation_binding_by_id.size() !=
          (aggregate_relation_ast == nullptr
               ? 0
               : (filter_relation_ast == nullptr ? 1 : 2))) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "native VALUES/aggregate graph is not canonical");
    return RefusedBoundAst(std::move(bound));
  }

  if (context.outputs.size() != expected_output_count) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                          "output bindings must cover every relation output");
    return RefusedBoundAst(std::move(bound));
  }
  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_map<std::uint32_t, std::unordered_set<std::uint32_t>>
      output_ordinals_by_relation;
  bound.outputs.reserve(context.outputs.size());
  for (const auto& output : context.outputs) {
    const auto relation_id =
        output.relation_id == 0 && ast.relations.size() == 1
            ? ast.root_relation_id
            : output.relation_id;
    const auto relation = ast_relation_by_id.find(relation_id);
    if (relation == ast_relation_by_id.end() || output.output_id == 0 ||
        output.ordinal >= relation->second->output_expression_ids.size() ||
        relation->second->output_expression_ids[output.ordinal] !=
            output.expression_id ||
        !output_ids.insert(output.output_id).second ||
        !output_ordinals_by_relation[relation_id]
             .insert(output.ordinal)
             .second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                            "output relation, ID, ordinal, and expression must be exact");
      return RefusedBoundAst(std::move(bound));
    }
    const auto expression_binding =
        expression_binding_by_id.find(output.expression_id);
    if (expression_binding == expression_binding_by_id.end() ||
        expression_binding->second->descriptor_id != output.descriptor_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                            "output descriptor does not match its bound expression");
      return RefusedBoundAst(std::move(bound));
    }
    BoundOutputAstRecord record;
    record.output_id = output.output_id;
    record.relation_id = relation_id;
    record.expression_id = output.expression_id;
    record.output_name_utf8 = output.output_name_utf8;
    record.descriptor_id = output.descriptor_id;
    record.visible = output.visible;
    record.ordinal = output.ordinal;
    used_descriptor_ids.insert(record.descriptor_id);
    bound.outputs.push_back(std::move(record));
  }
  std::ranges::sort(bound.outputs, [](const auto& left, const auto& right) {
    return left.relation_id != right.relation_id
               ? left.relation_id < right.relation_id
               : left.ordinal < right.ordinal;
  });

  if (aggregate_relation_ast == nullptr) {
    if (!ast.grouping_sets.empty()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "VALUES leaf cannot own grouping sets");
      return RefusedBoundAst(std::move(bound));
    }
  } else if (aggregate_relation_ast->aggregate_grouping_form ==
             NativeAggregateGroupingForm::kGroupingSets) {
    // QOW-SOURCE-QRY-001-BINDING-GROUPING-SETS-V1
    if (ast.grouping_sets.empty()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "GROUPING SETS aggregate requires typed set records");
      return RefusedBoundAst(std::move(bound));
    }
    bound.grouping_sets.reserve(ast.grouping_sets.size());
    for (std::size_t ordinal = 0; ordinal < ast.grouping_sets.size(); ++ordinal) {
      const auto& grouping_set = ast.grouping_sets[ordinal];
      if (grouping_set.relation_id != aggregate_relation_ast->relation_id ||
          grouping_set.ordinal != ordinal) {
        AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                              "grouping-set relation and ordinals must be exact");
        return RefusedBoundAst(std::move(bound));
      }
      std::vector<std::pair<std::size_t, std::uint32_t>> canonical_members;
      std::unordered_set<std::uint32_t> members;
      for (const auto expression_id : grouping_set.expression_ids) {
        const auto key = std::ranges::find(
            aggregate_relation_ast->grouping_key_expression_ids,
            expression_id);
        if (key == aggregate_relation_ast->grouping_key_expression_ids.end() ||
            !members.insert(expression_id).second) {
          AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                                "grouping-set member must be a unique bound key");
          return RefusedBoundAst(std::move(bound));
        }
        canonical_members.emplace_back(
            static_cast<std::size_t>(std::distance(
                aggregate_relation_ast->grouping_key_expression_ids.begin(),
                key)),
            expression_id);
      }
      std::ranges::sort(canonical_members);
      BoundGroupingSetAstRecord record;
      record.relation_id = grouping_set.relation_id;
      record.ordinal = grouping_set.ordinal;
      for (const auto& [key_ordinal, expression_id] : canonical_members) {
        (void)key_ordinal;
        record.expression_ids.push_back(expression_id);
      }
      bound.grouping_sets.push_back(std::move(record));
    }
  } else if (aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kSimple ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kRollup ||
             aggregate_relation_ast->aggregate_grouping_form ==
                 NativeAggregateGroupingForm::kCube) {
    // QOW-SOURCE-QRY-001-BINDING-FIXED-GROUPING-FORM-V1
    if (!ast.grouping_sets.empty()) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "fixed ordinary/ROLLUP/CUBE aggregate cannot own arbitrary grouping-set records");
      return RefusedBoundAst(std::move(bound));
    }
  } else {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "typed aggregate grouping form is not supported");
    return RefusedBoundAst(std::move(bound));
  }

  if (used_descriptor_ids.size() != descriptor_by_id.size()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-DESCRIPTOR",
                          "binding context contains an unused descriptor handle");
    return RefusedBoundAst(std::move(bound));
  }
  bound.descriptors.reserve(context.descriptors.size());
  for (const auto& descriptor : context.descriptors) {
    BoundDescriptorAstRecord record;
    record.descriptor_id = descriptor.descriptor_id;
    record.descriptor_uuid = descriptor.descriptor_uuid;
    record.type_uuid = descriptor.type_uuid;
    record.nullability = descriptor.nullability;
    record.collation_uuid = descriptor.collation_uuid;
    record.timezone_profile_id = descriptor.timezone_profile_id;
    record.width_precision_scale = descriptor.width_precision_scale;
    bound.descriptors.push_back(std::move(record));
  }
  std::ranges::sort(bound.descriptors,
                    [](const auto& left, const auto& right) {
                      return left.descriptor_id < right.descriptor_id;
                    });

  BoundScopeAstRecord scope;
  scope.scope_id = 1;
  scope.parent_scope_id = std::nullopt;
  scope.visible_relation_ids = {ast.root_relation_id};
  for (const auto& output : bound.outputs) {
    if (output.relation_id == ast.root_relation_id && output.visible) {
      scope.visible_projection_ids.push_back(output.output_id);
    }
  }
  std::ranges::sort(scope.visible_projection_ids);
  scope.catalog_epoch_uuid = context.catalog_epoch_uuid;
  bound.scopes.push_back(std::move(scope));

  bound.bound_ast_uuid = context.bound_ast_uuid;
  bound.security_context_uuid = context.security_context_uuid;
  bound.root_relation_id = ast.root_relation_id;
  bound.root_scope_id = 1;
  bound.bound = true;
  return bound;
}

BoundStatement BindAst(const AstDocument& ast,
                       const CstDocument& cst,
                       const ParserConfig& config,
                       const SessionContext& session,
                       const std::vector<std::string>& resolved_object_uuids,
                       const NativeRelationalBindingContext* native_binding_context) {
  BoundStatement bound;
  bound.parser_api_major = config.parser_api_major;
  bound.protocol_version = config.protocol_version;
  bound.parser_package_uuid = config.parser_uuid;
  bound.parser_package_version = config.bundle_contract_id;
  bound.parser_build_id = config.build_id;
  bound.command_registry_snapshot_uuid = "sbsql-generated-registry.v1";
  bound.session_uuid = session.session_uuid;
  bound.connection_uuid = session.connection_uuid;
  bound.database_uuid = session.database_uuid;
  bound.dialect_profile_uuid = session.dialect_profile_uuid;
  bound.catalog_epoch = session.catalog_epoch;
  bound.security_policy_epoch = session.security_policy_epoch;
  bound.descriptor_epoch = session.descriptor_epoch;
  bound.transaction_context = session.transaction_context;
  bound.registry_family = ast.registry_family;
  bound.operation_family = ast.operation_family;
  bound.statement_hash = Fnv1a64(cst.source);
  bound.native_relational_recognized = ast.native_relational.recognized();
  bound.messages = ast.messages;
  PopulateAuthorityMetadata(&bound, ast);
  if (bound.messages.has_errors()) return bound;
  if (!session.authenticated && ast.family != StatementFamily::kUnknown) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.AUTH.REQUIRED", "ERROR", "statement binding requires an authenticated server session",
        "sbp_sbsql.binder"));
    return bound;
  }
  if (bound.requires_cluster_profile) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.CLUSTER.AUTHORITY_REQUIRED", "ERROR",
        "cluster-private statement binding requires a cluster profile authority context",
        "sbp_sbsql.binder",
        {{"statement_surface_id", bound.statement_surface_id},
         {"authority", "authority.cluster.profile_gate_required"}}));
    return bound;
  }
  if (bound.exact_refusal_required && bound.behavior_descriptor_key.find("fail_closed") != std::string::npos) {
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.STATEMENT.EXACT_REFUSAL_REQUIRED", "ERROR",
        "statement binding requires exact refusal before SBLR lowering",
        "sbp_sbsql.binder",
        {{"statement_surface_id", bound.statement_surface_id},
         {"diagnostic_key", bound.diagnostic_key}}));
    return bound;
  }
  if (ast.native_relational.recognized()) {
    if (native_binding_context == nullptr || session.catalog_epoch == 0 ||
        session.descriptor_epoch == 0) {
      bound.messages.diagnostics.push_back(MakeDiagnostic(
          "QOW-DIAG-BOUNDAST-SCOPE", "ERROR",
          "typed relational binding requires engine-supplied descriptor and epoch context",
          "sbp_sbsql.native_binder"));
      return bound;
    }
    bound.native_relational =
        BindNativeRelationalAst(ast.native_relational, *native_binding_context);
    bound.messages.diagnostics.insert(
        bound.messages.diagnostics.end(),
        bound.native_relational.messages.diagnostics.begin(),
        bound.native_relational.messages.diagnostics.end());
    if (bound.messages.has_errors()) return bound;
    bound.descriptor_refs.clear();
    for (const auto& descriptor : bound.native_relational.descriptors) {
      bound.descriptor_refs.push_back(descriptor.descriptor_uuid);
    }
    bound.bound = bound.native_relational.bound;
    return bound;
  }
  if (ast.requires_name_resolution) {
    if (!resolved_object_uuids.empty()) {
      bound.resolved_object_uuids = resolved_object_uuids;
      bound.bound = true;
      return bound;
    }
    if (IsSourceFreeCteRoute(cst) || IsSourceFreeCatalogProjectionCountRoute(cst)) {
      bound.bound = true;
      return bound;
    }
    if (config.server_endpoint.empty()) {
      bound.messages.diagnostics.push_back(MakeDiagnostic(
          "SBSQL.NAME_RESOLUTION.SERVER_ENDPOINT_REQUIRED", "ERROR",
          "object-name binding requires ResolveNameRegistryPublic through sb_server IPC",
          "sbp_sbsql.binder"));
      return bound;
    }
    bound.messages.diagnostics.push_back(MakeDiagnostic(
        "SBSQL.NAME_RESOLUTION.PUBLIC_RESOLVER_REQUIRED", "ERROR",
        "public name resolution must be performed by sb_server before this statement can lower to final SBLR",
        "sbp_sbsql.binder"));
    return bound;
  }
  bound.bound = true;
  return bound;
}

} // namespace scratchbird::parser::sbsql
