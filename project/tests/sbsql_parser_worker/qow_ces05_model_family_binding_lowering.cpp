// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace scratchbird::parser::sbsql {
std::uint64_t Rcp073DocumentFrontdoorProofMaskForTest();
}

namespace {

std::string Uuid(const std::uint64_t value) {
  char buffer[37];
  std::snprintf(buffer, sizeof(buffer), "00000000-0000-7000-8000-%012llx",
                static_cast<unsigned long long>(value));
  return buffer;
}

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-CES05-DOCUMENT-PARSER: " << detail << '\n';
  return condition;
}

bool HasDiagnostic(const sbsql::MessageVectorSet& messages,
                   const std::string_view diagnostic) {
  return std::ranges::any_of(messages.diagnostics, [&](const auto& entry) {
    return entry.code == diagnostic;
  });
}

bool HasOperand(const sbsql::SblrEnvelope& envelope,
                const std::string_view type, const std::string_view name,
                const std::string_view value = {}) {
  return std::ranges::any_of(envelope.operands, [&](const auto& operand) {
    return operand.type == type && operand.name == name &&
           (value.empty() || operand.value == value);
  });
}

sbsql::ParserConfig Config() {
  sbsql::ParserConfig config;
  config.parser_uuid = Uuid(700);
  config.bundle_contract_id = "sbp_sbsql@qow-ces05-document-v1";
  config.build_id = "qow-ces05-document-v1";
  return config;
}

sbsql::SessionContext Session() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = Uuid(701);
  session.connection_uuid = Uuid(702);
  session.database_uuid = Uuid(703);
  session.dialect_profile_uuid = Uuid(704);
  session.catalog_epoch = 7;
  session.security_policy_epoch = 7;
  session.descriptor_epoch = 7;
  return session;
}

void SetEngineAuthority(sbsql::NativeRelationalBindingContext* context) {
  auto& authority = context->engine_statement_authority;
  authority.statement_uuid = context->statement_uuid;
  authority.transaction_uuid = context->owning_transaction_uuid;
  authority.statement_snapshot_uuid = context->statement_snapshot_uuid;
  authority.statement_metadata_snapshot_uuid =
      context->statement_metadata_snapshot_uuid;
  authority.catalog_epoch_uuid = context->catalog_epoch_uuid;
  authority.local_transaction_id = context->local_transaction_id;
  authority.snapshot_visible_through_local_transaction_id =
      context->snapshot_visible_through_local_transaction_id;
}

std::uint32_t DescriptorFor(const sbsql::NativeExpressionAstNode& expression) {
  if (expression.literal_kind == sbsql::NativeLiteralAstKind::kString) return 3;
  if (expression.expression_kind ==
          sbsql::NativeExpressionAstKind::kBinary &&
      (expression.operator_name == "=" || expression.operator_name == "<>" ||
       expression.operator_name == "<" || expression.operator_name == "<=" ||
       expression.operator_name == ">" || expression.operator_name == ">=")) {
    return 4;
  }
  return 2;
}

sbsql::NativeRelationalBindingContext ContextFor(
    const sbsql::NativeRelationalAstDocument& ast, const bool unnest) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = Uuid(710);
  context.catalog_epoch_uuid = Uuid(711);
  context.security_context_uuid = Uuid(712);
  context.statement_uuid = Uuid(713);
  context.owning_transaction_uuid = Uuid(714);
  context.statement_snapshot_uuid = Uuid(715);
  context.statement_metadata_snapshot_uuid = Uuid(716);
  context.local_transaction_id = 17;
  context.snapshot_visible_through_local_transaction_id = 18;
  SetEngineAuthority(&context);
  context.descriptors = {
      {1, Uuid(721), Uuid(731), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
      {2, Uuid(722), Uuid(732), sbsql::BoundNullability::kNullable,
       std::nullopt, std::nullopt, {}},
      {3, Uuid(723), Uuid(733), sbsql::BoundNullability::kNullable,
       Uuid(741), std::nullopt, {}},
      {4, Uuid(724), Uuid(734), sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
  };
  if (unnest) {
    // The document root and element output share the engine JSON type UUID;
    // the path literal and arithmetic children retain text/numeric types.
    context.descriptors = {
        {2, Uuid(722), Uuid(738), sbsql::BoundNullability::kNonNull,
         std::nullopt, std::nullopt, {}},
        {3, Uuid(723), Uuid(738), sbsql::BoundNullability::kNullable,
         std::nullopt, std::nullopt, {}},
        {4, Uuid(724), Uuid(733), sbsql::BoundNullability::kNullable,
         Uuid(741), std::nullopt, {}},
    };
  }

  const bool wildcard = std::ranges::any_of(ast.expressions, [](const auto& e) {
    return e.expression_kind == sbsql::NativeExpressionAstKind::kWildcard;
  });
  const std::size_t wildcard_count = wildcard ? (unnest ? 1 : 3) : 0;
  std::uint32_t next_expression = 101;
  if (wildcard) {
    for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
      const auto descriptor_id = unnest ? 3 : static_cast<std::uint32_t>(ordinal + 1);
      context.expressions.push_back(
          {next_expression++, descriptor_id, std::nullopt,
           unnest ? std::nullopt
                  : std::optional<std::string>(Uuid(750 + ordinal))});
    }
  }
  for (const auto& expression : ast.expressions) {
    if (expression.expression_kind == sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    sbsql::NativeExpressionBindingInput input;
    input.expression_id = next_expression++;
    if (unnest && ast.catalog_relation_sources.front()
                      .model_document_expression_id ==
                      expression.expression_id) {
      input.descriptor_id = 2;
    } else if (unnest &&
               expression.literal_kind ==
                   sbsql::NativeLiteralAstKind::kString) {
      input.descriptor_id = 4;
    } else if (unnest) {
      input.descriptor_id = 5;
    } else {
      input.descriptor_id = DescriptorFor(expression);
    }
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kFunctionCall &&
        expression.operator_name != "DOCUMENT_PATH") {
      input.function_uuid = Uuid(770 + expression.expression_id);
    }
    if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kIdentifier &&
        !unnest) {
      const auto& source = ast.catalog_relation_sources.front();
      const bool source_alias =
          expression.qualified_identifier.size() == 1 &&
          source.alias.has_value() &&
          expression.qualified_identifier.front().spelling ==
              source.alias->spelling &&
          expression.qualified_identifier.front().quoted ==
              source.alias->quoted;
      input.bound_name_uuid =
          source_alias ? Uuid(740) : Uuid(780 + expression.expression_id);
    }
    context.expressions.push_back(std::move(input));
  }

  const auto& relation = ast.relations.front();
  const auto& source_ast = ast.catalog_relation_sources.front();
  context.relations.push_back(
      {relation.relation_id,
       source_ast.model_operation_id == "DOCUMENT_UNNEST"
           ? "sblr.model-expand.document-unnest.v1"
           : (source_ast.model_operation_id == "DOCUMENT_PATH"
                  ? "sblr.model-source.document-path.v1"
                  : "sblr.model-source.document-find.v1")});
  if (wildcard) {
    for (std::size_t ordinal = 0; ordinal < wildcard_count; ++ordinal) {
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1),
           static_cast<std::uint32_t>(101 + ordinal),
           unnest ? "element" : std::array<const char*, 3>{"row_uuid", "join_key", "payload"}[ordinal],
           unnest ? 3u : static_cast<std::uint32_t>(ordinal + 1), true,
           static_cast<std::uint32_t>(ordinal), relation.relation_id});
    }
  } else {
    for (std::size_t ordinal = 0;
         ordinal < relation.output_expression_ids.size(); ++ordinal) {
      const auto ast_id = relation.output_expression_ids[ordinal];
      std::size_t preceding = 0;
      for (const auto& expression : ast.expressions) {
        if (expression.expression_kind ==
            sbsql::NativeExpressionAstKind::kWildcard) continue;
        if (expression.expression_id == ast_id) break;
        ++preceding;
      }
      const auto& binding = context.expressions[wildcard_count + preceding];
      context.outputs.push_back(
          {static_cast<std::uint32_t>(ordinal + 1), binding.expression_id,
           "document_value_" + std::to_string(ordinal + 1),
           binding.descriptor_id, true, static_cast<std::uint32_t>(ordinal),
           relation.relation_id});
    }
  }

  if (!unnest) {
    sbsql::NativeCatalogRelationBindingInput source;
    source.source_id = 1;
    source.resolution_state =
        sbsql::NativeCatalogRelationResolutionState::kBound;
    source.object_uuid = Uuid(740);
    source.resolved_object_type = "document_collection";
    source.resolved_schema_uuid = Uuid(742);
    source.parent_object_uuid = Uuid(743);
    source.catalog_generation_id = 7;
    source.security_epoch = 7;
    source.resource_epoch = 7;
    source.columns = {{0, Uuid(750), 1, "row_uuid"},
                      {1, Uuid(751), 2, "join_key"},
                      {2, Uuid(752), 3, "payload"}};
    context.catalog_relations.push_back(std::move(source));
  }
  return context;
}

sbsql::BoundStatement Bind(const sbsql::CstDocument& cst,
                           const sbsql::AstDocument& ast,
                           const bool unnest) {
  const auto context = ContextFor(ast.native_relational, unnest);
  return sbsql::BindAst(ast, cst, Config(), Session(), {}, &context);
}

bool SourcePathGrammarBindingLowering() {
  const auto cst = sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_SOURCE(app.document_fixture) AS d "
      "WHERE DOCUMENT_PATH(d, '$.join_key') >= 1 + 0;");
  const auto ast = sbsql::BuildAst(cst);
  bool passed = true;
  passed &= Require(ast.native_relational.status ==
                            sbsql::NativeRelationalParseStatus::kAccepted &&
                        ast.native_relational.catalog_relation_sources.size() == 1,
                    "DOCUMENT_SOURCE/DOCUMENT_PATH grammar was not accepted");
  if (!passed) return false;
  const auto& source = ast.native_relational.catalog_relation_sources.front();
  passed &= Require(source.source_kind ==
                            sbsql::NativeRelationSourceAstKind::kDocument &&
                        source.model_family_id == "document" &&
                        source.model_operation_id == "DOCUMENT_PATH" &&
                        source.qualified_name.size() == 2 &&
                        source.alias.has_value() && source.alias_is_explicit &&
                        source.model_path_expression_id.has_value() &&
                        source.model_value_expression_id.has_value() &&
                        source.model_comparison_operator == ">=",
                    "document source AST identity or composed RHS drifted");
  passed &= Require(
      ast.requires_name_resolution && !ast.produces_sblr &&
          ast.native_relational.model_object_resolution_requests.size() == 1 &&
          ast.native_relational.model_object_resolution_requests.front()
                  .source_id == source.source_id &&
          ast.native_relational.model_object_resolution_requests.front()
                  .model_family_id == "document" &&
          ast.native_relational.model_object_resolution_requests.front()
                  .object_class == "document_collection" &&
          ast.native_relational.model_object_resolution_requests.front()
                  .qualified_name.size() == 2,
      "collection source did not publish one exact resolution description");
  auto bound = Bind(cst, ast, false);
  passed &= Require(bound.bound && bound.native_relational.bound &&
                        bound.resolved_object_uuids ==
                            std::vector<std::string>{Uuid(740)} &&
                        bound.native_relational.catalog_relation_sources.front()
                                .object_uuid == Uuid(740),
                    "qualified collection did not bind to its UUID");
  if (!bound.bound) return false;
  passed &= Require(
      bound.native_relational.relations.size() == 1 &&
          bound.native_relational.relations.front().semantic_variant_id ==
              "sblr.model-source.document-path.v1" &&
          std::ranges::any_of(
              bound.native_relational.expressions, [](const auto& expression) {
                return expression.expression_kind ==
                           sbsql::NativeExpressionAstKind::kFunctionCall &&
                       !expression.bound_function_uuid.has_value();
              }),
      "DOCUMENT_PATH operation identity drifted into callable-registry identity");
  const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  passed &= Require(!lowered.messages.has_errors() && verified.admitted &&
                        HasOperand(lowered, "relational_node_binding_v1", "1") &&
                        std::ranges::any_of(lowered.operands, [](const auto& op) {
                          return op.type == "relational_node_binding_v1" &&
                                 op.value.find("53424c525f4d4f44454c5f534f555243455f5631") == 0;
                        }),
                    "DOCUMENT_PATH did not lower as SBLR_MODEL_SOURCE_V1");

  auto ambiguous_context = ContextFor(ast.native_relational, false);
  ambiguous_context.catalog_relations.push_back(
      ambiguous_context.catalog_relations.front());
  const auto ambiguous = sbsql::BindAst(ast, cst, Config(), Session(), {},
                                         &ambiguous_context);
  passed &= Require(!ambiguous.bound &&
                        HasDiagnostic(ambiguous.messages,
                                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
                    "ambiguous document source did not fail closed");

  auto semantic_substitution = ContextFor(ast.native_relational, false);
  semantic_substitution.relations.front().semantic_variant_id =
      "catalog.relation-source.v1";
  const auto substituted = sbsql::BindAst(
      ast, cst, Config(), Session(), {}, &semantic_substitution);
  passed &= Require(!substituted.bound &&
                        HasDiagnostic(substituted.messages,
                                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
                    "generic relation semantic substituted for document source");

  auto authority_mismatch = ContextFor(ast.native_relational, false);
  authority_mismatch.engine_statement_authority.statement_snapshot_uuid =
      Uuid(799);
  const auto unauthorized = sbsql::BindAst(
      ast, cst, Config(), Session(), {}, &authority_mismatch);
  passed &= Require(!unauthorized.bound &&
                        HasDiagnostic(unauthorized.messages,
                                      "QOW-DIAG-BOUNDAST-SCOPE"),
                    "mismatched MGA statement authority was accepted");

  auto lossy = bound;
  lossy.native_relational.expressions.back().child_expression_ids.push_back(9999);
  const auto lossy_lowered = sbsql::LowerToSblr(lossy, cst, Session());
  passed &= Require(HasDiagnostic(lossy_lowered.messages,
                                  "SB_MODEL_BINDING_INCOMPLETE_V1"),
                    "lossy expression lineage was lowered");
  return passed;
}

bool UnnestGrammarBindingLowering() {
  const auto cst = sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_UNNEST(DOCUMENT '{\"items\":[3,1,2]}', '$.items[*]') "
      "AS item;");
  const auto ast = sbsql::BuildAst(cst);
  bool passed = Require(
      ast.native_relational.status ==
              sbsql::NativeRelationalParseStatus::kAccepted &&
          ast.native_relational.catalog_relation_sources.size() == 1 &&
          ast.native_relational.catalog_relation_sources.front()
                  .model_operation_id == "DOCUMENT_UNNEST" &&
          ast.native_relational.catalog_relation_sources.front()
                  .model_document_expression_id.has_value() &&
          ast.native_relational.catalog_relation_sources.front()
                  .model_wildcard_path,
      "expression-backed DOCUMENT_UNNEST grammar drifted");
  passed &= Require(!ast.requires_name_resolution && ast.produces_sblr &&
                        ast.native_relational
                            .model_object_resolution_requests.empty(),
                    "DOCUMENT_UNNEST requested or invented catalog resolution");
  if (!passed) return false;
  const auto bound = Bind(cst, ast, true);
  passed &= Require(bound.bound && bound.resolved_object_uuids.empty() &&
                        bound.native_relational.catalog_relation_sources.front()
                                .object_uuid.empty() &&
                        bound.native_relational.catalog_relation_sources.front()
                                .resolved_object_type == "document_expression",
                    "DOCUMENT_UNNEST invented catalog identity");
  if (!bound.bound) return false;
  const auto& bound_source =
      bound.native_relational.catalog_relation_sources.front();
  const auto& bound_relation = bound.native_relational.relations.front();
  const auto bound_expression = [&](const std::uint32_t expression_id) {
    return std::ranges::find_if(
        bound.native_relational.expressions, [&](const auto& expression) {
          return expression.expression_id == expression_id;
        });
  };
  const auto root = bound_expression(bound_relation.output_expression_ids.front());
  const auto document =
      bound_expression(*bound_source.model_document_expression_id);
  const auto path = bound_expression(*bound_source.model_path_expression_id);
  const auto descriptor = [&](const std::uint32_t descriptor_id) {
    return std::ranges::find_if(
        bound.native_relational.descriptors, [&](const auto& candidate) {
          return candidate.descriptor_id == descriptor_id;
        });
  };
  passed &= Require(
      bound_relation.output_expression_ids.size() == 1 &&
          bound_relation.bound_expression_ids ==
              bound_relation.output_expression_ids &&
          root != bound.native_relational.expressions.end() &&
          document != bound.native_relational.expressions.end() &&
          path != bound.native_relational.expressions.end() &&
          root->expression_kind ==
              sbsql::NativeExpressionAstKind::kFunctionCall &&
          root->canonical_operator_name == "DOCUMENT_UNNEST" &&
          !root->bound_function_uuid.has_value() &&
          root->child_expression_ids ==
              std::vector<std::uint32_t>{
                  *bound_source.model_document_expression_id,
                  *bound_source.model_path_expression_id} &&
          path->expression_kind == sbsql::NativeExpressionAstKind::kLiteral &&
          path->literal_kind == sbsql::NativeLiteralAstKind::kString &&
          descriptor(root->result_descriptor_id)->type_uuid ==
              descriptor(document->result_descriptor_id)->type_uuid,
      "DOCUMENT_UNNEST did not preserve its typed root and ordered operands");
  const auto lowered = sbsql::LowerToSblr(bound, cst, Session());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);
  passed &= Require(!lowered.messages.has_errors() && verified.admitted &&
                        std::ranges::any_of(lowered.operands, [](const auto& op) {
                          return op.type == "relational_node_binding_v1" &&
                                 op.value.find("53424c525f4d4f44454c5f455850414e445f5631") == 0;
                        }),
                    "DOCUMENT_UNNEST did not lower as SBLR_MODEL_EXPAND_V1");

  auto descriptor_substitution = ContextFor(ast.native_relational, true);
  std::size_t binding_index = 1;
  for (const auto& expression : ast.native_relational.expressions) {
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kWildcard) {
      continue;
    }
    if (expression.expression_id ==
        *ast.native_relational.catalog_relation_sources.front()
             .model_document_expression_id) {
      descriptor_substitution.expressions[binding_index].descriptor_id = 4;
      break;
    }
    ++binding_index;
  }
  const auto substituted = sbsql::BindAst(
      ast, cst, Config(), Session(), {}, &descriptor_substitution);
  passed &= Require(
      !substituted.bound &&
          HasDiagnostic(substituted.messages, "SB_MODEL_BINDING_INCOMPLETE_V1"),
      "numeric/text descriptor kind substituted for DOCUMENT_UNNEST JSON root");

  const auto expect_lowering_refusal = [&](auto mutation,
                                            const std::string_view detail) {
    auto invalid = bound;
    mutation(invalid.native_relational);
    const auto invalid_lowered = sbsql::LowerToSblr(invalid, cst, Session());
    return Require(
        HasDiagnostic(invalid_lowered.messages,
                      "SB_MODEL_BINDING_INCOMPLETE_V1"),
        detail);
  };
  const auto root_id = bound_relation.output_expression_ids.front();
  const auto document_id = *bound_source.model_document_expression_id;
  const auto path_id = *bound_source.model_path_expression_id;
  passed &= expect_lowering_refusal(
      [=](auto& native) {
        const auto invalid_root = std::ranges::find_if(
            native.expressions, [&](const auto& expression) {
              return expression.expression_id == root_id;
            });
        std::swap(invalid_root->child_expression_ids[0],
                  invalid_root->child_expression_ids[1]);
      },
      "DOCUMENT_UNNEST reversed operand order was lowered");
  passed &= expect_lowering_refusal(
      [=](auto& native) {
        const auto invalid_root = std::ranges::find_if(
            native.expressions, [&](const auto& expression) {
              return expression.expression_id == root_id;
            });
        invalid_root->child_expression_ids = {document_id, document_id};
      },
      "DOCUMENT_UNNEST duplicate operand identity was lowered");
  passed &= expect_lowering_refusal(
      [=](auto& native) {
        const auto invalid_path = std::ranges::find_if(
            native.expressions, [&](const auto& expression) {
              return expression.expression_id == path_id;
            });
        invalid_path->literal_kind = sbsql::NativeLiteralAstKind::kNumeric;
      },
      "DOCUMENT_UNNEST non-string path kind was lowered");
  passed &= expect_lowering_refusal(
      [](auto& native) {
        auto orphan = native.expressions.front();
        orphan.expression_id = 9999;
        orphan.child_expression_ids.clear();
        native.expressions.push_back(std::move(orphan));
      },
      "DOCUMENT_UNNEST unreachable expression operand was lowered");
  return passed;
}

bool ExactRefusals() {
  const auto donor = sbsql::BuildAst(
      sbsql::BuildCst("SELECT * FROM MONGO_PIPELINE('{ $match: {} }');"));
  const auto write = sbsql::BuildAst(sbsql::BuildCst(
      "UPDATE DOCUMENT_SOURCE(app.document_fixture) SET payload = 'x';"));
  const auto alias = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_SOURCE(app.document_fixture) d "
      "WHERE DOCUMENT_PATH(other, '$.x') = 1;"));
  const auto untyped_path = sbsql::BuildAst(sbsql::BuildCst(
      "SELECT * FROM DOCUMENT_UNNEST(document_parameter, dynamic_path);"));
  bool passed = true;
  passed &= Require(HasDiagnostic(
                        donor.native_relational.messages,
                        "SB_MODEL_GRAMMAR_DONOR_TEXT_REFUSED_V1"),
                    "opaque donor pipeline was not refused exactly");
  passed &= Require(HasDiagnostic(write.native_relational.messages,
                                  "SB_MODEL_QUERY_WRITE_REFUSED_V1"),
                    "document query write was not refused exactly");
  passed &= Require(alias.native_relational.status ==
                        sbsql::NativeRelationalParseStatus::kRefused,
                    "mismatched document alias was accepted");
  passed &= Require(untyped_path.native_relational.status ==
                        sbsql::NativeRelationalParseStatus::kRefused,
                    "untyped DOCUMENT_UNNEST path was accepted");
  return passed;
}

bool WireFrontdoorProjectionCohort() {
  constexpr std::uint64_t kExpectedMask = (1ull << 22) - 1;
  const auto mask = sbsql::Rcp073DocumentFrontdoorProofMaskForTest();
  return Require(mask == kExpectedMask,
                 "wire front-door projection/refusal mask was incomplete: " +
                     std::to_string(mask));
}

}  // namespace

int main() {
  bool passed = true;
  passed &= SourcePathGrammarBindingLowering();
  passed &= UnnestGrammarBindingLowering();
  passed &= ExactRefusals();
  passed &= WireFrontdoorProjectionCohort();
  return passed ? 0 : 1;
}
