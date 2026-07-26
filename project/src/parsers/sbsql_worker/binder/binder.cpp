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

  if (ast.values_rows.empty()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "VALUES relation has no typed row handles");
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
  if (context.outputs.size() != first_row.expression_ids.size()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                          "VALUES output bindings must match the row arity");
    return RefusedBoundAst(std::move(bound));
  }

  std::unordered_set<std::uint32_t> output_ids;
  std::unordered_set<std::uint32_t> output_ordinals;
  bound.outputs.reserve(context.outputs.size());
  for (const auto& output : context.outputs) {
    if (output.output_id == 0 || output.ordinal >= first_row.expression_ids.size() ||
        first_row.expression_ids[output.ordinal] != output.expression_id ||
        !output_ids.insert(output.output_id).second ||
        !output_ordinals.insert(output.ordinal).second) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-OUTPUT",
                            "output IDs, ordinals, and expression handles must be exact");
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
    record.expression_id = output.expression_id;
    record.output_name_utf8 = output.output_name_utf8;
    record.descriptor_id = output.descriptor_id;
    record.visible = output.visible;
    record.ordinal = output.ordinal;
    used_descriptor_ids.insert(record.descriptor_id);
    bound.outputs.push_back(std::move(record));
  }
  std::ranges::sort(bound.outputs,
                    [](const auto& left, const auto& right) {
                      return left.ordinal < right.ordinal;
                    });

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

  if (ast.relations.size() != 1 || ast.root_relation_id == 0 ||
      ast.relations.front().relation_id != ast.root_relation_id ||
      ast.relations.front().relation_kind != NativeRelationAstKind::kValues ||
      !ast.relations.front().input_relation_ids.empty() ||
      ast.relations.front().values_row_ids.size() != bound.values_rows.size()) {
    AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                          "typed VALUES relation graph is not canonical");
    return RefusedBoundAst(std::move(bound));
  }
  BoundRelationAstRecord relation;
  relation.relation_id = ast.root_relation_id;
  relation.relation_kind = NativeRelationAstKind::kValues;
  relation.input_relation_ids = ast.relations.front().input_relation_ids;
  relation.values_row_ids = ast.relations.front().values_row_ids;
  for (std::size_t index = 0; index < relation.values_row_ids.size(); ++index) {
    if (relation.values_row_ids[index] != bound.values_rows[index].row_id) {
      AddBoundAstDiagnostic(&bound, "QOW-DIAG-BOUNDAST-RELATION",
                            "VALUES relation row handles are not canonical");
      return RefusedBoundAst(std::move(bound));
    }
  }
  relation.bound_object_uuid = std::nullopt;
  relation.lateral = false;
  bound.relations.push_back(std::move(relation));

  BoundScopeAstRecord scope;
  scope.scope_id = 1;
  scope.parent_scope_id = std::nullopt;
  scope.visible_relation_ids = {ast.root_relation_id};
  for (const auto& output : bound.outputs) {
    if (output.visible) scope.visible_projection_ids.push_back(output.output_id);
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
