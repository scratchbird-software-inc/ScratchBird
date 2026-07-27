// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ast/ast.hpp"
#include "binder/binder.hpp"
#include "cst/cst.hpp"
#include "lowering/lowering.hpp"
#include "sblr_dispatch.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;
namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

bool HasParserDiagnostic(const sbsql::MessageVectorSet& messages,
                         const std::string_view code) {
  return std::ranges::any_of(
      messages.diagnostics, [code](const auto& diagnostic) {
        return diagnostic.code == code;
      });
}

sbsql::NativeRelationalBindingContext ValuesBindingContext() {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-000000000501";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000502";
  context.security_context_uuid = "019f0000-0000-7110-8000-000000000502";
  context.descriptors = {
      {1,
       "019f0000-0000-7200-8000-000000000503",
       "019f0000-0000-7300-8000-000000000504",
       sbsql::BoundNullability::kNonNull,
       std::nullopt,
       std::nullopt,
       {}},
      {2,
       "019f0000-0000-7200-8000-000000000505",
       "019f0000-0000-7300-8000-000000000506",
       sbsql::BoundNullability::kNullable,
       "019f0000-0000-7400-8000-000000000507",
       std::nullopt,
       {}}};
  context.expressions = {
      {1, 1, std::nullopt},
      {2, 2, std::nullopt},
      {3, 1, std::nullopt},
      {4, 2, std::nullopt},
  };
  context.outputs = {
      {1, 1, "column_1", 1, true, 0},
      {2, 2, "column_2", 2, true, 1},
  };
  return context;
}

sbsql::ParserConfig ParserConfigForTest() {
  sbsql::ParserConfig config;
  config.parser_uuid = "019f0000-0000-7500-8000-000000000508";
  config.bundle_contract_id = "sbp_sbsql@qow-qry-005-v1";
  config.build_id = "qow-qry-005-v1";
  return config;
}

sbsql::SessionContext SessionForTest() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7600-8000-000000000509";
  session.connection_uuid = "019f0000-0000-7600-8000-00000000050a";
  session.database_uuid = "019f0000-0000-7600-8000-00000000050b";
  session.dialect_profile_uuid = "019f0000-0000-7600-8000-00000000050c";
  session.catalog_epoch = 7;
  session.security_policy_epoch = 11;
  session.descriptor_epoch = 13;
  return session;
}

sbsql::BoundStatement BoundValues(const sbsql::CstDocument& cst) {
  const auto ast = sbsql::BuildAst(cst);
  const auto context = ValuesBindingContext();
  return sbsql::BindAst(ast, cst, ParserConfigForTest(), SessionForTest(),
                        {}, &context);
}

sbsql::NativeRelationalBindingContext GroupingSetsBindingContext(
    const sbsql::NativeRelationalAstDocument& ast) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-0000000005a1";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-0000000005a2";
  context.security_context_uuid = "019f0000-0000-7110-8000-0000000005a2";
  context.descriptors = {
      {1, "019f0000-0000-7200-8000-0000000005a3",
       "019f0000-0000-7300-8000-0000000005a4",
       sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}},
      {2, "019f0000-0000-7200-8000-0000000005a5",
       "019f0000-0000-7300-8000-0000000005a6",
       sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}},
      {3, "019f0000-0000-7200-8000-0000000005a7",
       "019f0000-0000-7300-8000-0000000005a8",
       sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}},
      {4, "019f0000-0000-7200-8000-0000000005a9",
       "019f0000-0000-7300-8000-0000000005aa",
       sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {}},
      {5, "019f0000-0000-7200-8000-0000000005ab",
       "019f0000-0000-7300-8000-0000000005ac",
       sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}},
  };

  std::unordered_map<std::uint32_t, std::uint32_t> descriptor_by_expression;
  for (const auto& row : ast.values_rows) {
    for (std::size_t ordinal = 0; ordinal < row.expression_ids.size(); ++ordinal) {
      descriptor_by_expression[row.expression_ids[ordinal]] =
          static_cast<std::uint32_t>(ordinal + 1);
    }
  }
  for (const auto& expression : ast.expressions) {
    auto descriptor_id = descriptor_by_expression[expression.expression_id];
    std::optional<std::string> function_uuid;
    std::optional<std::string> bound_name_uuid;
    if (expression.expression_kind ==
        sbsql::NativeExpressionAstKind::kIdentifier) {
      if (expression.spelling == "key_a") {
        descriptor_id = 1;
        bound_name_uuid = "019f0000-0000-7500-8000-0000000005ad";
      } else if (expression.spelling == "key_b") {
        descriptor_id = 2;
        bound_name_uuid = "019f0000-0000-7500-8000-0000000005ae";
      } else if (expression.spelling == "amount") {
        descriptor_id = 3;
        bound_name_uuid = "019f0000-0000-7500-8000-0000000005af";
      }
    } else if (expression.expression_kind ==
               sbsql::NativeExpressionAstKind::kFunctionCall) {
      if (expression.operator_name == "COUNT") {
        descriptor_id = 4;
        function_uuid = "019de5fc-2400-784a-9aec-371f8b95b7ea";
      } else if (expression.operator_name == "SUM") {
        descriptor_id = 5;
        function_uuid = "019de5fc-2400-72e4-8549-82b2eef5a777";
      }
    }
    context.expressions.push_back({expression.expression_id, descriptor_id,
                                   std::move(function_uuid),
                                   std::move(bound_name_uuid)});
    descriptor_by_expression[expression.expression_id] = descriptor_id;
  }

  std::uint32_t output_id = 1;
  for (const auto& relation : ast.relations) {
    const std::array<std::string_view, 4> aggregate_names = {
        "key_a", "key_b", "row_count", "total_amount"};
    const std::array<std::string_view, 3> values_names = {
        "key_a", "key_b", "amount"};
    for (std::size_t ordinal = 0;
         ordinal < relation.output_expression_ids.size(); ++ordinal) {
      const auto expression_id = relation.output_expression_ids[ordinal];
      const auto output_name =
          relation.relation_kind == sbsql::NativeRelationAstKind::kValues
              ? values_names[ordinal]
              : aggregate_names[ordinal];
      context.outputs.push_back(
          {output_id++, expression_id, std::string(output_name),
           descriptor_by_expression.at(expression_id), true,
           static_cast<std::uint32_t>(ordinal), relation.relation_id});
    }
  }
  context.relations.push_back(
      {ast.root_relation_id,
       "aggregate.grouping-sets-int64-keys-count-sum.v1"});
  return context;
}

api::EngineRequestContext GroupingSetsEngineContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-0000000005a2";
  context.local_transaction_id = 55;
  context.snapshot_visible_through_local_transaction_id = 53;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7100-8000-0000000005a2";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7110-8000-0000000005a2";
  context.catalog_generation_id = 552;
  context.security_epoch = 553;
  context.resource_epoch = 554;
  context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006001";
  context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006002";
  context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006003";
  context.optimizer_route_epoch = 555;
  context.optimizer_route_generation = 556;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000;
  context.optimizer_spill_allowed = true;
  context.current_monotonic_ns = "552000";
  context.authorization_context.security_epoch = 553;
  context.authorization_context.policy_epoch = 554;
  context.authorization_context.catalog_generation_id = 552;
  return context;
}

bool ValidateCanonicalLoweringAndDispatch() {
  const auto cst = sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');");
  const auto lowered =
      sbsql::LowerToSblr(BoundValues(cst), cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(!lowered.messages.has_errors(),
                    "typed VALUES lowering emitted an error");
  passed &= Require(lowered.operation_id == "query.execute" &&
                        lowered.sblr_opcode == "SBLR_QUERY_EXECUTE" &&
                        lowered.engine_api_operation_id == "query.execute",
                    "typed VALUES did not select the canonical query root");
  passed &= Require(lowered.result_shape_key == "query_execute_result",
                    "canonical query result shape differs");
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(lowered.operands.size() == 17 &&
                        lowered.operands[0].type == "uint16" &&
                        lowered.operands[0].name ==
                            "relational_wire_version" &&
                        lowered.operands[0].value == "2" &&
                        lowered.operands[1].type == "uuid" &&
                        lowered.operands[1].name ==
                            "relational_bound_sblr_tree_uuid" &&
                        lowered.operands[1].value ==
                            "019f0000-0000-7000-8000-000000000501" &&
                        lowered.operands[4].type == "uint32" &&
                        lowered.operands[4].name ==
                            "relational_root_node_id" &&
                        lowered.operands[4].value == "1" &&
                        has_operand("relational_descriptor_v1", "1",
                                    "019f0000-0000-7200-8000-000000000503|"
                                    "019f0000-0000-7300-8000-000000000504|"
                                    "1|-|-|-|-|-") &&
                        has_operand("relational_expression_v1", "1",
                                    "1|-|1|-|-|1|-|31") &&
                        has_operand("relational_values_row_v1", "1", "1,2") &&
                        has_operand("relational_values_row_v1", "2", "3,4") &&
                        has_operand("relational_node_v1", "1",
                                    "13|0|-|1,2|1,2") &&
                        has_operand(
                            "relational_node_binding_v1", "1",
                            "76616c7565732e6c69746572616c2d7461626c652e7631|"
                            "1,2,3,4|-|-|-"),
                    "typed relation handles were not structurally preserved");
  passed &= Require(
      lowered.payload.find("operation_id=query.execute\n") !=
              std::string::npos &&
          lowered.payload.find("opcode=SBLR_QUERY_EXECUTE\n") !=
              std::string::npos &&
          lowered.payload.find(
              "operand=relational_node_v1\t1\t13|0|-|1,2|1,2\n") !=
              std::string::npos &&
          lowered.payload.find("VALUES (1") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos &&
          lowered.payload.find("SBLR_QUERY_PLAN_OPERATION") ==
              std::string::npos,
      "canonical payload is missing typed structure or leaked SQL/legacy root");
  passed &= Require(verified.admitted && !verified.messages.has_errors(),
                    "parser-side canonical envelope verification failed");

  api::EngineRequestContext engine_context;
  engine_context.security_context_present = true;
  engine_context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000000502";
  engine_context.local_transaction_id = 51;
  engine_context.snapshot_visible_through_local_transaction_id = 49;
  engine_context.statement_metadata_snapshot_engine_owned = true;
  engine_context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7100-8000-000000000502";
  engine_context.authorization_context.present = true;
  engine_context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7110-8000-000000000502";
  engine_context.catalog_generation_id = 502;
  engine_context.security_epoch = 503;
  engine_context.resource_epoch = 504;
  engine_context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006001";
  engine_context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006002";
  engine_context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006003";
  engine_context.optimizer_route_epoch = 505;
  engine_context.optimizer_route_generation = 506;
  engine_context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  engine_context.optimizer_maximum_candidate_count = 131072;
  engine_context.optimizer_maximum_memo_groups = 131072;
  engine_context.optimizer_maximum_search_steps = 1048576;
  engine_context.optimizer_maximum_planning_time_ns = 5'000'000'000;
  engine_context.optimizer_spill_allowed = true;
  engine_context.current_monotonic_ns = "502000";
  engine_context.authorization_context.security_epoch = 503;
  engine_context.authorization_context.policy_epoch = 504;
  engine_context.authorization_context.catalog_generation_id = 502;
  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, std::move(engine_context));
  passed &= Require(dispatched.envelope_validated && dispatched.accepted &&
                        dispatched.dispatched_to_api &&
                        dispatched.logical_graph_populated &&
                        dispatched.logical_properties_populated &&
                        dispatched.optimizer_admitted &&
                        dispatched.optimizer_admission_stage_count == 8 &&
                        dispatched.logical_node_count == 1 &&
                        dispatched.logical_property_count == 0,
                    "lowered payload did not reach the canonical typed dispatch seam");
  passed &= Require(
      dispatched.optimizer_selected && dispatched.physical_dag_published &&
          dispatched.physical_dag_executed &&
          dispatched.runtime_actuals_attached &&
          dispatched.canonical_result_published &&
          dispatched.api_result.ok && dispatched.physical_node_count == 1 &&
          dispatched.canonical_result_column_count == 2 &&
          dispatched.canonical_result_row_count == 2 &&
          dispatched.api_result.result_shape.rows.size() == 2 &&
          dispatched.api_result.result_shape.rows[0].fields.size() == 2 &&
          dispatched.api_result.result_shape.rows[0].fields[0].second
                  .encoded_value == "1" &&
          dispatched.api_result.result_shape.rows[1].fields[1].second
                  .encoded_value == "b",
      "structurally lowered literal VALUES did not complete the live spine");
  return passed;
}

bool ValidateGroupingSetsParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_b), (), (key_b,key_a), (key_b));";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupingSetsBindingContext(ast.native_relational);
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.family == sbsql::StatementFamily::kQuery &&
          ast.native_relational.relations.size() == 2 &&
          ast.native_relational.grouping_sets.size() == 4 &&
          ast.native_relational.grouping_sets[0].expression_ids.size() == 1 &&
          ast.native_relational.grouping_sets[1].expression_ids.empty() &&
          ast.native_relational.grouping_sets[2].expression_ids.size() == 2 &&
          ast.native_relational.grouping_sets[3].expression_ids ==
              ast.native_relational.grouping_sets[0].expression_ids,
      "native parser did not retain ordered/repeated GROUPING SETS syntax");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 2 &&
          bound.native_relational.grouping_sets.size() == 4 &&
          bound.native_relational.grouping_sets[2].expression_ids ==
              bound.native_relational.relations[1]
                  .grouping_key_expression_ids,
      "native binder did not resolve and canonicalize grouping-set members");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 2) {
    return false;
  }
  const auto& aggregate = bound.native_relational.relations[1];
  const auto key_a = aggregate.grouping_key_expression_ids[0];
  const auto key_b = aggregate.grouping_key_expression_ids[1];
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  const bool canonical_grouping_lowered =
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("relational_node_v1", "1", "13|0|-|1,2,3|1,2,3,4,5,6") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand("relational_grouping_set_v1", "0",
                      "2|" + std::to_string(key_b)) &&
          has_operand("relational_grouping_set_v1", "1", "2|-") &&
          has_operand("relational_grouping_set_v1", "2",
                      "2|" + std::to_string(key_a) + "," +
                          std::to_string(key_b)) &&
          has_operand("relational_grouping_set_v1", "3",
                      "2|" + std::to_string(key_b)) &&
          lowered.payload.find("SELECT key_a") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos;
  if (!canonical_grouping_lowered) {
    for (const auto& operand : lowered.operands) {
      std::cerr << operand.type << '\t' << operand.name << '\t'
                << operand.value << '\n';
    }
    for (const auto& diagnostic : lowered.messages.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
  }
  passed &= Require(
      canonical_grouping_lowered,
      "typed GROUPING SETS lowering did not emit the canonical wire-v2 DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  passed &= Require(
      dispatched.envelope_validated && dispatched.accepted &&
          dispatched.dispatched_to_api &&
          dispatched.logical_graph_populated &&
          dispatched.logical_properties_populated &&
          dispatched.optimizer_admitted && dispatched.optimizer_selected &&
          dispatched.physical_dag_published &&
          dispatched.physical_dag_executed &&
          dispatched.runtime_actuals_attached &&
          dispatched.canonical_result_published && dispatched.api_result.ok &&
          dispatched.logical_node_count == 2 &&
          dispatched.physical_node_count == 2 &&
          dispatched.canonical_result_column_count == 4 &&
          dispatched.canonical_result_row_count == 12 &&
          dispatched.api_result.result_shape.rows.size() == 12,
      "parser-produced GROUPING SETS DAG did not complete the live engine spine");

  const auto malformed_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_b),);" );
  const auto malformed_ast = sbsql::BuildAst(malformed_cst);
  passed &= Require(
      malformed_ast.native_relational.recognized() &&
          !malformed_ast.native_relational.accepted() &&
          malformed_ast.family == sbsql::StatementFamily::kQuery &&
          malformed_ast.native_relational.root_relation_id == 0 &&
          malformed_ast.native_relational.relations.empty() &&
          malformed_ast.native_relational.grouping_sets.empty() &&
          HasParserDiagnostic(malformed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED"),
      "malformed GROUPING SETS syntax retained a partial native AST");

  auto duplicate_member_ast = ast;
  duplicate_member_ast.native_relational.grouping_sets[0]
      .expression_ids.push_back(key_b);
  const auto duplicate_member = sbsql::BindAst(
      duplicate_member_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);
  auto missing_semantic_context = context;
  missing_semantic_context.relations.clear();
  const auto missing_semantic = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &missing_semantic_context);
  passed &= Require(
      !duplicate_member.bound && duplicate_member.messages.has_errors() &&
          duplicate_member.native_relational.grouping_sets.empty() &&
          !missing_semantic.bound && missing_semantic.messages.has_errors(),
      "invalid grouping member or missing semantic binding did not fail closed");

  auto duplicate_ordinal = bound;
  duplicate_ordinal.native_relational.grouping_sets[1].ordinal = 0;
  const auto refused_lowering =
      sbsql::LowerToSblr(duplicate_ordinal, cst, SessionForTest());
  passed &= Require(
      refused_lowering.payload.empty() &&
          refused_lowering.operation_id == "query.execute" &&
          refused_lowering.sblr_opcode == "SBLR_QUERY_EXECUTE" &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "invalid grouping-set BoundAST lowered or fell back to the legacy root");
  return passed;
}

bool ValidateFailClosedLowering() {
  const auto cst = sbsql::BuildCst("VALUES (1, 'a'), (2, 'b');");

  auto dangling_root = BoundValues(cst);
  dangling_root.native_relational.root_relation_id = 99;
  const auto dangling =
      sbsql::LowerToSblr(dangling_root, cst, SessionForTest());

  auto duplicate_descriptor = BoundValues(cst);
  duplicate_descriptor.native_relational.outputs[1].descriptor_id = 1;
  const auto duplicate =
      sbsql::LowerToSblr(duplicate_descriptor, cst, SessionForTest());

  auto incomplete = BoundValues(cst);
  incomplete.native_relational.bound = false;
  const auto missing_bound_ast =
      sbsql::LowerToSblr(incomplete, cst, SessionForTest());

  bool passed = true;
  passed &= Require(
      dangling.payload.empty() && dangling.messages.has_errors() &&
          HasParserDiagnostic(dangling.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "dangling BoundAST root was lowered");
  passed &= Require(
      duplicate.payload.empty() && duplicate.messages.has_errors() &&
          HasParserDiagnostic(duplicate.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "duplicate output descriptor handle was lowered");
  passed &= Require(
      missing_bound_ast.payload.empty() && missing_bound_ast.messages.has_errors() &&
          HasParserDiagnostic(missing_bound_ast.messages,
                              "QOW-DIAG-BOUNDAST-RELATION") &&
          missing_bound_ast.operation_id == "query.execute" &&
          missing_bound_ast.sblr_opcode == "SBLR_QUERY_EXECUTE",
      "incomplete native query fell back to the legacy root");
  return passed;
}

}  // namespace

// QOW-ROUTE-STAGE-QRY-005-V1
// QOW-TEST-QRY-001-GROUPING-SETS-V1
// QOW-TEST-QRY-001-BINDING-GROUPING-SETS-V1
// QOW-TEST-QRY-005-V1
int main() {
  bool passed = true;
  passed &= ValidateCanonicalLoweringAndDispatch();
  passed &= ValidateGroupingSetsParserBindingLoweringAndDispatch();
  passed &= ValidateFailClosedLowering();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
