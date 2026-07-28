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

std::string EncodeHex(const std::string_view value) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kHex[ch >> 4]);
    encoded.push_back(kHex[ch & 0x0f]);
  }
  return encoded;
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

sbsql::NativeRelationalBindingContext GroupedAggregateBindingContext(
    const sbsql::NativeRelationalAstDocument& ast,
    const std::string_view semantic_variant_id) {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-0000000005a1";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-0000000005a2";
  context.security_context_uuid = "019f0000-0000-7110-8000-0000000005a2";
  const auto aggregate_relation = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind ==
               sbsql::NativeRelationAstKind::kAggregate;
      });
  const bool one_key_grouping =
      aggregate_relation != ast.relations.end() &&
      aggregate_relation->aggregate_projection_form ==
          sbsql::NativeAggregateProjectionForm::kKeyCountSum;
  const auto filter_relation = std::ranges::find_if(
      ast.relations, [](const auto& relation) {
        return relation.relation_kind ==
               sbsql::NativeRelationAstKind::kFilter;
      });
  const bool has_having_filter = filter_relation != ast.relations.end();
  const bool has_count_sum_and_having =
      has_having_filter &&
      filter_relation->predicate_expression_ids.size() == 1 &&
      ast.expressions[filter_relation->predicate_expression_ids.front() - 1]
              .operator_name == "AND";
  context.descriptors = {
      {1, "019f0000-0000-7200-8000-0000000005a3",
       "019f0000-0000-7300-8000-0000000005a4",
       sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}},
      {2, "019f0000-0000-7200-8000-0000000005a5",
       "019f0000-0000-7300-8000-0000000005a6",
       sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}},
      {3, "019f0000-0000-7200-8000-0000000005a7",
       "019f0000-0000-7300-8000-0000000005a8",
       one_key_grouping ? sbsql::BoundNullability::kNonNull
                        : sbsql::BoundNullability::kNullable,
       std::nullopt, std::nullopt, {}},
      {4, "019f0000-0000-7200-8000-0000000005a9",
       "019f0000-0000-7300-8000-0000000005aa",
       one_key_grouping ? sbsql::BoundNullability::kNullable
                        : sbsql::BoundNullability::kNonNull,
       std::nullopt, std::nullopt, {}},
  };
  if (!one_key_grouping) {
    context.descriptors.push_back(
        {5, "019f0000-0000-7200-8000-0000000005ab",
         "019f0000-0000-7300-8000-0000000005ac",
         sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}});
  }
  if (has_having_filter) {
    context.descriptors.insert(
        context.descriptors.end(),
        {{9, "019f0000-0000-7200-8000-0000000005b6",
          "019f0000-0000-7300-8000-0000000005a6",
          sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {}},
         {10, "019f0000-0000-7200-8000-0000000005b7",
          "019f0000-0000-7300-8000-0000000005b8",
          sbsql::BoundNullability::kNullable, std::nullopt, std::nullopt, {}}});
  }
  const bool projects_grouping_metadata =
      aggregate_relation != ast.relations.end() &&
      aggregate_relation->aggregate_projection_form ==
          sbsql::NativeAggregateProjectionForm::kKeysCountSumGrouping;
  if (projects_grouping_metadata) {
    context.descriptors.insert(
        context.descriptors.end(),
        {{6, "019f0000-0000-7200-8000-0000000005b0",
          "019f0000-0000-7300-8000-0000000005b1",
          sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {}},
         {7, "019f0000-0000-7200-8000-0000000005b2",
          "019f0000-0000-7300-8000-0000000005b3",
          sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {}},
         {8, "019f0000-0000-7200-8000-0000000005b4",
          "019f0000-0000-7300-8000-0000000005b5",
          sbsql::BoundNullability::kNonNull, std::nullopt, std::nullopt, {}}});
  }

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
        descriptor_id = one_key_grouping ? 2 : 3;
        bound_name_uuid = "019f0000-0000-7500-8000-0000000005af";
      }
    } else if (expression.expression_kind ==
               sbsql::NativeExpressionAstKind::kFunctionCall) {
      if (expression.operator_name == "COUNT") {
        descriptor_id = one_key_grouping ? 3 : 4;
        function_uuid = "019de5fc-2400-784a-9aec-371f8b95b7ea";
      } else if (expression.operator_name == "SUM") {
        descriptor_id = one_key_grouping ? 4 : 5;
        function_uuid = "019de5fc-2400-72e4-8549-82b2eef5a777";
      }
    } else if (projects_grouping_metadata &&
               expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kUnary &&
               expression.operator_name == "grouping" &&
               expression.child_expression_ids.size() == 1) {
      descriptor_id =
          expression.child_expression_ids[0] ==
                  aggregate_relation->grouping_key_expression_ids[0]
              ? 6
              : 7;
    } else if (projects_grouping_metadata &&
               expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kBinary &&
               expression.operator_name == "grouping_id") {
      descriptor_id = 8;
    } else if (has_having_filter &&
               expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kLiteral &&
               descriptor_id == 0) {
      descriptor_id = 9;
    } else if (has_having_filter &&
               expression.expression_kind ==
                   sbsql::NativeExpressionAstKind::kBinary &&
               (expression.operator_name == ">" ||
                expression.operator_name == "AND")) {
      descriptor_id = 10;
    }
    context.expressions.push_back({expression.expression_id, descriptor_id,
                                   std::move(function_uuid),
                                   std::move(bound_name_uuid)});
    descriptor_by_expression[expression.expression_id] = descriptor_id;
  }

  std::uint32_t output_id = 1;
  for (const auto& relation : ast.relations) {
    const std::array<std::string_view, 7> aggregate_names = {
        "key_a", "key_b", "row_count", "total_amount",
        "grouping_a", "grouping_b", "grouping_id"};
    const std::array<std::string_view, 3> values_names = {
        "key_a", "key_b", "amount"};
    const std::array<std::string_view, 3> simple_aggregate_names = {
        "key_a", "row_count", "total_amount"};
    const std::array<std::string_view, 2> simple_values_names = {
        "key_a", "amount"};
    for (std::size_t ordinal = 0;
         ordinal < relation.output_expression_ids.size(); ++ordinal) {
      const auto expression_id = relation.output_expression_ids[ordinal];
      const auto output_name =
          one_key_grouping
              ? (relation.relation_kind ==
                         sbsql::NativeRelationAstKind::kValues
                     ? simple_values_names[ordinal]
                     : simple_aggregate_names[ordinal])
              : (relation.relation_kind ==
                         sbsql::NativeRelationAstKind::kValues
                     ? values_names[ordinal]
                     : aggregate_names[ordinal]);
      context.outputs.push_back(
          {output_id++, expression_id, std::string(output_name),
           descriptor_by_expression.at(expression_id), true,
           static_cast<std::uint32_t>(ordinal), relation.relation_id});
    }
  }
  if (aggregate_relation != ast.relations.end()) {
    context.relations.push_back(
        {aggregate_relation->relation_id, std::string(semantic_variant_id)});
  }
  if (has_having_filter) {
    context.relations.push_back(
        {filter_relation->relation_id,
         has_count_sum_and_having
             ? "filter.having-count-sum-and-gt-int64-literals.v1"
             : "filter.having-sum-gt-int64-literal.v1"});
  }
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

bool ValidateSimpleGroupByParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10), (1,20), (2,5), (NULL,7), (2,NULL), "
      "(1,5)) AS input(key_a,amount) GROUP BY key_a;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouped-int64-key-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.family == sbsql::StatementFamily::kQuery &&
          ast.native_relational.relations.size() == 2 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kSimple &&
          ast.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeyCountSum &&
          ast.native_relational.relations[1]
                  .grouping_key_expression_ids.size() == 1 &&
          ast.native_relational.relations[1]
                  .aggregate_expression_ids.size() == 2 &&
          ast.native_relational.relations[1]
                  .output_expression_ids.size() == 3 &&
          ast.native_relational.grouping_sets.empty(),
      "native parser did not retain the exact ordinary one-key GROUP BY form");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 2 &&
          bound.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kSimple &&
          bound.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeyCountSum &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.grouped-int64-key-count-sum.v1" &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not retain ordinary GROUP BY semantic authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 2) {
    return false;
  }

  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2|1,2,3,4,5,6") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,3,4|-") &&
          has_operand(
              "relational_node_binding_v1", "2",
              EncodeHex("aggregate.grouped-int64-key-count-sum.v1") +
                  "|1,2,4|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("SELECT key_a") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos,
      "ordinary GROUP BY did not lower to the exact canonical wire-v2 DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto has_group = [&](const std::string_view key,
                             const std::string_view count,
                             const std::string_view sum) {
    return std::ranges::any_of(
        dispatched.api_result.result_shape.rows, [&](const auto& row) {
          return row.fields.size() == 3 &&
                 row.fields[0].second.encoded_value == key &&
                 row.fields[1].second.encoded_value == count &&
                 row.fields[2].second.encoded_value == sum;
        });
  };
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
          dispatched.canonical_result_column_count == 3 &&
          dispatched.canonical_result_row_count == 3 &&
          dispatched.api_result.result_shape.rows.size() == 3 &&
          has_group("1", "3", "35") && has_group("2", "2", "5"),
      "parser-produced ordinary GROUP BY did not complete the live engine spine");

  const auto malformed_cst = sbsql::BuildCst(
      "SELECT key_a, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10)) AS input(key_a,amount) GROUP BY amount;");
  const auto malformed_ast = sbsql::BuildAst(malformed_cst);

  auto mismatched_context = context;
  mismatched_context.relations[0].semantic_variant_id =
      "aggregate.grouping-sets-int64-keys-count-sum.v1";
  const auto mismatched_semantic = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &mismatched_context);

  auto injected_set_ast = ast;
  injected_set_ast.native_relational.grouping_sets.push_back(
      {2, 0,
       injected_set_ast.native_relational.relations[1]
           .grouping_key_expression_ids,
       {}});
  const auto injected_set_bound = sbsql::BindAst(
      injected_set_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);

  auto projection_drift_bound = bound;
  projection_drift_bound.native_relational.relations[1]
      .aggregate_projection_form =
      sbsql::NativeAggregateProjectionForm::kKeysCountSum;
  const auto refused_lowering = sbsql::LowerToSblr(
      projection_drift_bound, cst, SessionForTest());

  passed &= Require(
      malformed_ast.native_relational.recognized() &&
          !malformed_ast.native_relational.accepted() &&
          malformed_ast.native_relational.root_relation_id == 0 &&
          malformed_ast.native_relational.relations.empty() &&
          malformed_ast.native_relational.expressions.empty() &&
          HasParserDiagnostic(malformed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !mismatched_semantic.bound &&
          mismatched_semantic.messages.has_errors() &&
          !injected_set_bound.bound &&
          injected_set_bound.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "ordinary GROUP BY syntax, semantic, set, or projection drift did not fail closed");
  return passed;
}

bool ValidateHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10), (1,20), (2,5), (NULL,7), (2,NULL), "
      "(1,5)) AS input(key_a,amount) GROUP BY key_a "
      "HAVING SUM(amount) > 6;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouped-int64-key-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          ast.native_relational.relations.size() == 3 &&
          ast.native_relational.relations[2].relation_kind ==
              sbsql::NativeRelationAstKind::kFilter &&
          ast.native_relational.relations[2].input_relation_ids ==
              std::vector<std::uint32_t>{2} &&
          ast.native_relational.relations[2].output_expression_ids ==
              ast.native_relational.relations[1].output_expression_ids &&
          ast.native_relational.relations[2]
                  .predicate_expression_ids.size() == 1,
      "native parser did not retain HAVING as a typed FILTER relation");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.root_relation_id == 3 &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.grouped-int64-key-count-sum.v1" &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-sum-gt-int64-literal.v1" &&
          bound.native_relational.relations[2].bound_expression_ids ==
              bound.native_relational.relations[2]
                  .predicate_expression_ids,
      "native binder did not preserve aggregate and HAVING authority separately");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3) {
    return false;
  }

  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2|1,2,3,4,5,6") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,3,4|-") &&
          has_operand("relational_node_v1", "3", "2|0|2|1,3,4|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex("filter.having-sum-gt-int64-literal.v1") + "|" +
                  std::to_string(predicate_id) + "|-|-|-") &&
          lowered.payload.find("HAVING SUM") == std::string::npos,
      "HAVING did not lower to the exact canonical three-node DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto has_value_group = [&](const std::string_view key,
                                   const std::string_view count,
                                   const std::string_view sum) {
    return std::ranges::any_of(
        dispatched.api_result.result_shape.rows, [&](const auto& row) {
          return row.fields.size() == 3 &&
                 row.fields[0].second.state == api::EngineValueState::value &&
                 row.fields[0].second.encoded_value == key &&
                 row.fields[1].second.encoded_value == count &&
                 row.fields[2].second.encoded_value == sum;
        });
  };
  const auto has_null_group = std::ranges::any_of(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 3 &&
               row.fields[0].second.state ==
                   api::EngineValueState::sql_null &&
               row.fields[1].second.encoded_value == "1" &&
               row.fields[2].second.encoded_value == "7";
      });
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 3 &&
          dispatched.canonical_result_row_count == 2 &&
          has_value_group("1", "3", "35") && has_null_group,
      "parser-produced HAVING did not filter grouped output on the live spine");

  const auto unsupported_cst = sbsql::BuildCst(
      "SELECT key_a, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10)) AS input(key_a,amount) GROUP BY key_a "
      "HAVING SUM(amount) >= 6;");
  const auto unsupported_ast = sbsql::BuildAst(unsupported_cst);

  auto semantic_drift_context = context;
  semantic_drift_context.relations.back().semantic_variant_id =
      "filter.where.v1";
  const auto semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &semantic_drift_context);

  const auto ast_predicate = std::ranges::find_if(
      ast.native_relational.expressions, [&](const auto& expression) {
        return expression.expression_id == predicate_id;
      });
  auto function_drift_context = context;
  const auto function_drift = std::ranges::find_if(
      function_drift_context.expressions, [&](const auto& binding) {
        return binding.expression_id ==
               ast_predicate->child_expression_ids.front();
      });
  function_drift->function_uuid =
      "019de5fc-2400-784a-9aec-371f8b95b7ea";
  const auto function_identity_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &function_drift_context);

  auto operator_drift_bound = bound;
  const auto predicate = std::ranges::find_if(
      operator_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == predicate_id;
      });
  predicate->canonical_operator_name = "<";
  const auto refused_lowering = sbsql::LowerToSblr(
      operator_drift_bound, cst, SessionForTest());

  passed &= Require(
      unsupported_ast.native_relational.recognized() &&
          !unsupported_ast.native_relational.accepted() &&
          HasParserDiagnostic(unsupported_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !semantic_drift.bound && semantic_drift.messages.has_errors() &&
          !function_identity_drift.bound &&
          function_identity_drift.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "HAVING syntax, semantic, function, or operator drift did not fail closed");
  return passed;
}

bool ValidateBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10), (1,20), (2,5), (NULL,7), (2,NULL), "
      "(1,5), (3,NULL)) AS input(key_a,amount) GROUP BY key_a "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouped-int64-key-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  const auto filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  const auto* predicate =
      filter_relation != nullptr &&
              filter_relation->predicate_expression_ids.size() == 1
          ? &ast.native_relational.expressions[
                filter_relation->predicate_expression_ids.front() - 1]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() && filter_relation != nullptr &&
          ast.native_relational.root_relation_id == 3 &&
          filter_relation->relation_kind ==
              sbsql::NativeRelationAstKind::kFilter &&
          predicate != nullptr &&
          predicate->expression_kind ==
              sbsql::NativeExpressionAstKind::kBinary &&
          predicate->operator_name == "AND" &&
          predicate->child_expression_ids.size() == 2,
      "native parser did not retain ordered COUNT/SUM Boolean HAVING");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].bound_expression_ids ==
              bound.native_relational.relations[2].predicate_expression_ids,
      "native binder did not preserve Boolean HAVING authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3 || predicate == nullptr) {
    return false;
  }

  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2|1,2,3,4,5,6,7") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,3,4|-") &&
          has_operand("relational_node_v1", "3", "2|0|2|1,3,4|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "Boolean HAVING did not lower to the exact canonical three-node DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const bool only_expected_group =
      dispatched.api_result.result_shape.rows.size() == 1 &&
      dispatched.api_result.result_shape.rows.front().fields.size() == 3 &&
      dispatched.api_result.result_shape.rows.front().fields[0].second.state ==
          api::EngineValueState::value &&
      dispatched.api_result.result_shape.rows.front()
              .fields[0]
              .second.encoded_value == "1" &&
      dispatched.api_result.result_shape.rows.front()
              .fields[1]
              .second.encoded_value == "3" &&
      dispatched.api_result.result_shape.rows.front()
              .fields[2]
              .second.encoded_value == "35";
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 3 &&
          dispatched.canonical_result_row_count == 1 && only_expected_group,
      "parser-produced Boolean HAVING did not execute COUNT/SUM AND on the live spine");

  const auto unsupported_cst = sbsql::BuildCst(
      "SELECT key_a, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10)) AS input(key_a,amount) GROUP BY key_a "
      "HAVING COUNT(*) > 1 OR SUM(amount) > 6;");
  const auto unsupported_ast = sbsql::BuildAst(unsupported_cst);

  auto semantic_drift_context = context;
  semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &semantic_drift_context);

  auto count_identity_drift_context = context;
  const auto count_comparison =
      ast.native_relational.expressions[predicate->child_expression_ids[0] - 1];
  const auto having_count_id = count_comparison.child_expression_ids[0];
  const auto count_identity_drift = std::ranges::find_if(
      count_identity_drift_context.expressions, [&](const auto& binding) {
        return binding.expression_id == having_count_id;
      });
  count_identity_drift->function_uuid =
      "019de5fc-2400-72e4-8549-82b2eef5a777";
  const auto count_function_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &count_identity_drift_context);

  auto descriptor_drift_context = context;
  const auto count_comparison_binding = std::ranges::find_if(
      descriptor_drift_context.expressions, [&](const auto& binding) {
        return binding.expression_id ==
               predicate->child_expression_ids.front();
      });
  count_comparison_binding->descriptor_id = 9;
  const auto descriptor_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &descriptor_drift_context);

  auto operator_drift_bound = bound;
  const auto bound_predicate = std::ranges::find_if(
      operator_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == predicate_id;
      });
  bound_predicate->canonical_operator_name = "OR";
  const auto refused_lowering = sbsql::LowerToSblr(
      operator_drift_bound, cst, SessionForTest());

  passed &= Require(
      unsupported_ast.native_relational.recognized() &&
          !unsupported_ast.native_relational.accepted() &&
          HasParserDiagnostic(unsupported_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !semantic_drift.bound && semantic_drift.messages.has_errors() &&
          !count_function_drift.bound &&
          count_function_drift.messages.has_errors() &&
          !descriptor_drift.bound && descriptor_drift.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "Boolean HAVING syntax, semantic, identity, descriptor, or operator drift did not fail closed");
  return passed;
}

bool ValidateTwoKeyBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL)) "
      "AS input(key_a,key_b,amount) GROUP BY key_a,key_b "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 4;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouped-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  const auto* predicate =
      filter_relation != nullptr &&
              filter_relation->predicate_expression_ids.size() == 1
          ? &ast.native_relational.expressions[
                filter_relation->predicate_expression_ids.front() - 1]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() && filter_relation != nullptr &&
          ast.native_relational.root_relation_id == 3 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kSimple &&
          ast.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSum &&
          ast.native_relational.relations[1]
                  .grouping_key_expression_ids.size() == 2 &&
          filter_relation->relation_kind ==
              sbsql::NativeRelationAstKind::kFilter &&
          predicate != nullptr &&
          predicate->expression_kind ==
              sbsql::NativeExpressionAstKind::kBinary &&
          predicate->operator_name == "AND" &&
          predicate->child_expression_ids.size() == 2,
      "native parser did not retain ordered Boolean HAVING over two grouping keys");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.grouped-int64-keys-count-sum.v1" &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              4,
      "native binder did not preserve two-key Boolean HAVING authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3 || predicate == nullptr) {
    return false;
  }

  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand("relational_node_v1", "3", "2|0|2|1,2,4,5|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "two-key Boolean HAVING did not lower to the exact canonical three-node DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const bool only_expected_group =
      dispatched.api_result.result_shape.rows.size() == 1 &&
      dispatched.api_result.result_shape.rows.front().fields.size() == 4 &&
      dispatched.api_result.result_shape.rows.front()
              .fields[0]
              .second.encoded_value == "1" &&
      dispatched.api_result.result_shape.rows.front()
              .fields[1]
              .second.encoded_value == "10" &&
      dispatched.api_result.result_shape.rows.front()
              .fields[2]
              .second.encoded_value == "2" &&
      dispatched.api_result.result_shape.rows.front()
              .fields[3]
              .second.encoded_value == "5";
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 4 &&
          dispatched.canonical_result_row_count == 1 && only_expected_group,
      "parser-produced two-key Boolean HAVING did not complete the live engine spine");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY key_a,key_b HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  const auto reversed_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY key_a,key_b "
      "HAVING SUM(amount) > 4 AND COUNT(*) > 1;");
  const auto reversed_ast = sbsql::BuildAst(reversed_cst);

  auto semantic_drift_context = context;
  semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &semantic_drift_context);

  auto aggregate_drift_context = context;
  aggregate_drift_context.relations.front().semantic_variant_id =
      "aggregate.grouped-int64-key-count-sum.v1";
  const auto aggregate_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_drift_context);

  auto count_identity_drift_context = context;
  const auto& count_comparison =
      ast.native_relational.expressions[predicate->child_expression_ids[0] - 1];
  const auto having_count_id = count_comparison.child_expression_ids[0];
  const auto count_identity_drift = std::ranges::find_if(
      count_identity_drift_context.expressions, [&](const auto& binding) {
        return binding.expression_id == having_count_id;
      });
  count_identity_drift->function_uuid =
      "019de5fc-2400-72e4-8549-82b2eef5a777";
  const auto count_function_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &count_identity_drift_context);

  auto operator_drift_bound = bound;
  const auto bound_predicate = std::ranges::find_if(
      operator_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == predicate_id;
      });
  bound_predicate->canonical_operator_name = "OR";
  const auto refused_lowering = sbsql::LowerToSblr(
      operator_drift_bound, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          reversed_ast.native_relational.recognized() &&
          !reversed_ast.native_relational.accepted() &&
          HasParserDiagnostic(reversed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !semantic_drift.bound && semantic_drift.messages.has_errors() &&
          !aggregate_drift.bound && aggregate_drift.messages.has_errors() &&
          !count_function_drift.bound &&
          count_function_drift.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "two-key Boolean HAVING syntax, semantic, aggregate, identity, or operator drift did not fail closed");
  return passed;
}

bool ValidateSimpleTwoKeyGroupByParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL)) AS input(key_a,key_b,amount) "
      "GROUP BY key_a,key_b;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouped-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.family == sbsql::StatementFamily::kQuery &&
          ast.native_relational.relations.size() == 2 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kSimple &&
          ast.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSum &&
          ast.native_relational.relations[1]
                  .grouping_key_expression_ids.size() == 2 &&
          ast.native_relational.relations[1]
                  .aggregate_expression_ids.size() == 2 &&
          ast.native_relational.relations[1]
                  .output_expression_ids.size() == 4 &&
          ast.native_relational.grouping_sets.empty(),
      "native parser did not retain the exact ordinary two-key GROUP BY form");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 2 &&
          bound.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kSimple &&
          bound.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSum &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.grouped-int64-keys-count-sum.v1" &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not retain two-key GROUP BY semantic authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 2) {
    return false;
  }

  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand(
              "relational_node_binding_v1", "2",
              EncodeHex("aggregate.grouped-int64-keys-count-sum.v1") +
                  "|1,2,3,5|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("SELECT key_a") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos,
      "two-key GROUP BY did not lower to the exact canonical wire-v2 DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto has_group = [&](const std::string_view key_a,
                             const std::string_view key_b,
                             const std::string_view count,
                             const std::string_view sum) {
    return std::ranges::any_of(
        dispatched.api_result.result_shape.rows, [&](const auto& row) {
          return row.fields.size() == 4 &&
                 row.fields[0].second.encoded_value == key_a &&
                 row.fields[1].second.encoded_value == key_b &&
                 row.fields[2].second.encoded_value == count &&
                 row.fields[3].second.encoded_value == sum;
        });
  };
  const auto has_null_key_group = [&](const std::size_t null_key_ordinal,
                                      const std::string_view other_key,
                                      const std::string_view sum) {
    return std::ranges::any_of(
        dispatched.api_result.result_shape.rows, [&](const auto& row) {
          const auto other_key_ordinal = null_key_ordinal == 0 ? 1U : 0U;
          return row.fields.size() == 4 &&
                 row.fields[null_key_ordinal].second.is_null &&
                 !row.fields[other_key_ordinal].second.is_null &&
                 row.fields[other_key_ordinal].second.encoded_value ==
                     other_key &&
                 row.fields[2].second.encoded_value == "1" &&
                 row.fields[3].second.encoded_value == sum;
        });
  };
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
          dispatched.canonical_result_row_count == 5 &&
          dispatched.api_result.result_shape.rows.size() == 5 &&
          has_group("1", "10", "2", "5") &&
          has_group("1", "20", "1", "7") &&
          has_group("2", "10", "1", "4") &&
          has_null_key_group(0, "10", "8") &&
          has_null_key_group(1, "1", "3"),
      "parser-produced two-key GROUP BY did not complete the live engine spine");

  const auto malformed_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY key_a,amount;");
  const auto malformed_ast = sbsql::BuildAst(malformed_cst);

  auto mismatched_context = context;
  mismatched_context.relations[0].semantic_variant_id =
      "aggregate.grouped-int64-key-count-sum.v1";
  const auto mismatched_semantic = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &mismatched_context);

  auto injected_set_ast = ast;
  injected_set_ast.native_relational.grouping_sets.push_back(
      {2, 0,
       injected_set_ast.native_relational.relations[1]
           .grouping_key_expression_ids,
       {}});
  const auto injected_set_bound = sbsql::BindAst(
      injected_set_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);

  auto projection_drift_bound = bound;
  projection_drift_bound.native_relational.relations[1]
      .aggregate_projection_form =
      sbsql::NativeAggregateProjectionForm::kKeyCountSum;
  const auto refused_lowering = sbsql::LowerToSblr(
      projection_drift_bound, cst, SessionForTest());

  passed &= Require(
      malformed_ast.native_relational.recognized() &&
          !malformed_ast.native_relational.accepted() &&
          malformed_ast.native_relational.root_relation_id == 0 &&
          malformed_ast.native_relational.relations.empty() &&
          malformed_ast.native_relational.expressions.empty() &&
          HasParserDiagnostic(malformed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !mismatched_semantic.bound &&
          mismatched_semantic.messages.has_errors() &&
          !injected_set_bound.bound &&
          injected_set_bound.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "two-key GROUP BY syntax, semantic, set, or projection drift did not fail closed");
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
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouping-sets-int64-keys-count-sum.v1");
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

bool ValidateGroupingSetsBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL)) "
      "AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_b), (), (key_b,key_a), (key_b)) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.grouping-sets-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  const auto* predicate =
      filter_relation != nullptr &&
              filter_relation->predicate_expression_ids.size() == 1
          ? &ast.native_relational.expressions[
                filter_relation->predicate_expression_ids.front() - 1]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          ast.native_relational.relations.size() == 3 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kGroupingSets &&
          ast.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSum &&
          ast.native_relational.grouping_sets.size() == 4 &&
          ast.native_relational.grouping_sets[3].expression_ids ==
              ast.native_relational.grouping_sets[0].expression_ids &&
          filter_relation != nullptr && predicate != nullptr &&
          predicate->expression_kind ==
              sbsql::NativeExpressionAstKind::kBinary &&
          predicate->operator_name == "AND" &&
          predicate->child_expression_ids.size() == 2,
      "native parser did not retain Boolean HAVING over ordered/repeated grouping sets");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.grouping-sets-int64-keys-count-sum.v1" &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.grouping_sets.size() == 4 &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              4,
      "native binder did not preserve grouping-set Boolean HAVING authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3 || predicate == nullptr) {
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
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7,8") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand("relational_node_v1", "3", "2|0|2|1,2,4,5|-") &&
          has_operand("relational_grouping_set_v1", "0",
                      "2|" + std::to_string(key_b)) &&
          has_operand("relational_grouping_set_v1", "1", "2|-") &&
          has_operand("relational_grouping_set_v1", "2",
                      "2|" + std::to_string(key_a) + "," +
                          std::to_string(key_b)) &&
          has_operand("relational_grouping_set_v1", "3",
                      "2|" + std::to_string(key_b)) &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "grouping-set Boolean HAVING did not lower to the exact canonical DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto matching_key_b_subtotals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 && row.fields[0].second.is_null &&
               !row.fields[1].second.is_null &&
               row.fields[1].second.encoded_value == "10" &&
               row.fields[2].second.encoded_value == "4" &&
               row.fields[3].second.encoded_value == "17";
      });
  const auto matching_grand_totals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 && row.fields[0].second.is_null &&
               row.fields[1].second.is_null &&
               row.fields[2].second.encoded_value == "8" &&
               row.fields[3].second.encoded_value == "27";
      });
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 4 &&
          dispatched.canonical_result_row_count == 3 &&
          dispatched.api_result.result_shape.rows.size() == 3 &&
          matching_key_b_subtotals == 2 && matching_grand_totals == 1,
      "grouping-set Boolean HAVING did not preserve repeated set identity and SQL truth");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_a,key_b), ()) "
      "HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  auto filter_semantic_drift_context = context;
  filter_semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto filter_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &filter_semantic_drift_context);
  auto aggregate_semantic_drift_context = context;
  aggregate_semantic_drift_context.relations.front().semantic_variant_id =
      "aggregate.grouped-int64-keys-count-sum.v1";
  const auto aggregate_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_semantic_drift_context);

  auto grouping_ordinal_drift = bound;
  grouping_ordinal_drift.native_relational.grouping_sets[1].ordinal = 0;
  const auto refused_lowering = sbsql::LowerToSblr(
      grouping_ordinal_drift, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !filter_semantic_drift.bound &&
          filter_semantic_drift.messages.has_errors() &&
          !aggregate_semantic_drift.bound &&
          aggregate_semantic_drift.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "grouping-set HAVING syntax, form, semantic, or ordinal drift did not fail closed");
  return passed;
}

bool ValidateGroupingSetsGroupingMetadataBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kPreHavingSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL), "
      "(NULL,10,1)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_b), (), (key_b,key_a), (key_b));";
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL), "
      "(NULL,10,1)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_b), (), (key_b,key_a), (key_b)) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  constexpr std::string_view kAggregateSemantic =
      "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1";

  const auto pre_cst = sbsql::BuildCst(std::string(kPreHavingSql));
  const auto pre_ast = sbsql::BuildAst(pre_cst);
  const auto pre_context = GroupedAggregateBindingContext(
      pre_ast.native_relational, kAggregateSemantic);
  const auto pre_bound = sbsql::BindAst(
      pre_ast, pre_cst, ParserConfigForTest(), SessionForTest(), {},
      &pre_context);
  const auto pre_lowered =
      sbsql::LowerToSblr(pre_bound, pre_cst, SessionForTest());
  const auto pre_dispatched = sblr::DecodeAndDispatchSblrOperation(
      pre_lowered.payload, GroupingSetsEngineContext());

  using ExpectedRow =
      std::array<std::optional<std::string_view>, 7>;
  const auto count_rows = [](const auto& rows, const ExpectedRow& expected) {
    return std::ranges::count_if(rows, [&](const auto& row) {
      if (row.fields.size() != expected.size()) return false;
      for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        const auto& value = row.fields[ordinal].second;
        if (expected[ordinal].has_value()) {
          if (value.is_null ||
              value.encoded_value != *expected[ordinal]) {
            return false;
          }
        } else if (!value.is_null) {
          return false;
        }
      }
      return true;
    });
  };

  bool passed = true;
  const auto& pre_rows = pre_dispatched.api_result.result_shape.rows;
  passed &= Require(
      pre_dispatched.envelope_validated && pre_dispatched.accepted &&
          pre_dispatched.dispatched_to_api &&
          pre_dispatched.physical_dag_executed &&
          pre_dispatched.canonical_result_published &&
          pre_dispatched.api_result.ok &&
          pre_dispatched.logical_node_count == 2 &&
          pre_dispatched.physical_node_count == 2 &&
          pre_dispatched.canonical_result_column_count == 7 &&
          pre_dispatched.canonical_result_row_count == 15 &&
          pre_rows.size() == 15 &&
          count_rows(pre_rows,
                     {std::nullopt, "10", "5", "18", "1", "0", "2"}) ==
              2 &&
          count_rows(pre_rows,
                     {std::nullopt, "20", "1", "7", "1", "0", "2"}) ==
              2 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "1", "3", "1", "0",
                      "2"}) == 2 &&
          count_rows(pre_rows,
                     {std::nullopt, "30", "2", std::nullopt, "1", "0",
                      "2"}) == 2 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "9", "28", "1", "1",
                      "3"}) == 1 &&
          count_rows(pre_rows, {"1", "10", "2", "5", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows, {"1", "20", "1", "7", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {"1", std::nullopt, "1", "3", "0", "0", "0"}) == 1 &&
          count_rows(pre_rows, {"2", "10", "1", "4", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {std::nullopt, "10", "2", "9", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {"3", "30", "2", std::nullopt, "0", "0", "0"}) == 1,
      "pre-HAVING GROUPING SETS metadata rows did not retain all 15 exact group identities");

  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational, kAggregateSemantic);
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  const auto* aggregate_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[1]
          : nullptr;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          aggregate_relation != nullptr && filter_relation != nullptr &&
          aggregate_relation->aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kGroupingSets &&
          aggregate_relation->aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation->output_expression_ids.size() == 7 &&
          filter_relation->output_expression_ids ==
              aggregate_relation->output_expression_ids &&
          ast.native_relational.grouping_sets.size() == 4 &&
          ast.native_relational.grouping_sets[3].expression_ids ==
              ast.native_relational.grouping_sets[0].expression_ids,
      "native parser did not retain metadata and repeated-set identity through HAVING");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              kAggregateSemantic &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              7 &&
          bound.native_relational.relations[2].output_expression_ids ==
              bound.native_relational.relations[1].output_expression_ids,
      "native binder did not preserve all seven metadata HAVING outputs");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3) {
    return false;
  }

  const auto& aggregate = bound.native_relational.relations[1];
  const auto key_a = aggregate.grouping_key_expression_ids[0];
  const auto key_b = aggregate.grouping_key_expression_ids[1];
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7,8,9") &&
          has_operand("relational_node_v1", "2",
                      "5|0|1|1,2,4,5,6,7,8|-") &&
          has_operand("relational_node_v1", "3",
                      "2|0|2|1,2,4,5,6,7,8|-") &&
          has_operand("relational_grouping_set_v1", "0",
                      "2|" + std::to_string(key_b)) &&
          has_operand("relational_grouping_set_v1", "1", "2|-") &&
          has_operand("relational_grouping_set_v1", "2",
                      "2|" + std::to_string(key_a) + "," +
                          std::to_string(key_b)) &&
          has_operand("relational_grouping_set_v1", "3",
                      "2|" + std::to_string(key_b)) &&
          has_operand("relational_node_binding_v1", "2",
                      EncodeHex(kAggregateSemantic) +
                          "|1,2,3,5,6,7,8|-|-|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "grouping metadata HAVING did not lower to the exact seven-column canonical DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto& rows = dispatched.api_result.result_shape.rows;
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 7 &&
          dispatched.canonical_result_row_count == 4 && rows.size() == 4 &&
          count_rows(rows,
                     {std::nullopt, "10", "5", "18", "1", "0", "2"}) ==
              2 &&
          count_rows(rows,
                     {std::nullopt, std::nullopt, "9", "28", "1", "1",
                      "3"}) == 1 &&
          count_rows(rows,
                     {std::nullopt, "10", "2", "9", "0", "0", "0"}) ==
              1 &&
          count_rows(rows, {"1", "10", "2", "5", "0", "0", "0"}) ==
              0 &&
          count_rows(rows,
                     {"3", "30", "2", std::nullopt, "0", "0", "0"}) == 0,
      "grouping metadata HAVING did not preserve repeated, structural-NULL, actual-NULL, FALSE, and UNKNOWN identities");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_a,key_b), ()) "
      "HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  const auto order_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_b,key_a) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY GROUPING SETS ((key_a,key_b), ()) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 4;");
  const auto order_ast = sbsql::BuildAst(order_cst);

  auto aggregate_semantic_drift_context = context;
  aggregate_semantic_drift_context.relations.front().semantic_variant_id =
      "aggregate.grouping-sets-int64-keys-count-sum.v1";
  const auto aggregate_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_semantic_drift_context);
  auto filter_semantic_drift_context = context;
  filter_semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto filter_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &filter_semantic_drift_context);

  auto key_drift_ast = ast;
  const auto& drift_aggregate = key_drift_ast.native_relational.relations[1];
  const auto grouping_a_id = drift_aggregate.output_expression_ids[4];
  const auto grouping_a = std::ranges::find_if(
      key_drift_ast.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_a_id;
      });
  grouping_a->child_expression_ids = {
      drift_aggregate.grouping_key_expression_ids[1]};
  const auto key_drift_bound = sbsql::BindAst(
      key_drift_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);

  auto order_drift_bound = bound;
  const auto grouping_id_id =
      order_drift_bound.native_relational.relations[1]
          .output_expression_ids[6];
  const auto grouping_id = std::ranges::find_if(
      order_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_id_id;
      });
  std::ranges::reverse(grouping_id->child_expression_ids);
  const auto refused_lowering = sbsql::LowerToSblr(
      order_drift_bound, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          order_ast.native_relational.recognized() &&
          !order_ast.native_relational.accepted() &&
          HasParserDiagnostic(order_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !aggregate_semantic_drift.bound &&
          aggregate_semantic_drift.messages.has_errors() &&
          !filter_semantic_drift.bound &&
          filter_semantic_drift.messages.has_errors() &&
          !key_drift_bound.bound && key_drift_bound.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "grouping metadata HAVING parser, semantic, key/order, or lowering drift did not fail closed");
  return passed;
}

bool ValidateRollupParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b);";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.rollup-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.family == sbsql::StatementFamily::kQuery &&
          ast.native_relational.relations.size() == 2 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kRollup &&
          ast.native_relational.grouping_sets.empty(),
      "native parser did not retain the exact two-key ROLLUP form");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 2 &&
          bound.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kRollup &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.rollup-int64-keys-count-sum.v1" &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not preserve ROLLUP with its authoritative semantic binding");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 2) {
    return false;
  }
  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand(
              "relational_node_binding_v1", "2",
              "6167677265676174652e726f6c6c75702d696e7436342d6b6579732d"
              "636f756e742d73756d2e7631|1,2,3,5|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("SELECT key_a") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos,
      "typed ROLLUP lowering did not emit the fixed-profile canonical wire-v2 DAG");

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
          dispatched.canonical_result_row_count == 9 &&
          dispatched.api_result.result_shape.rows.size() == 9,
      "parser-produced ROLLUP DAG did not complete the live engine spine");

  const auto malformed_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,);" );
  const auto malformed_ast = sbsql::BuildAst(malformed_cst);
  passed &= Require(
      malformed_ast.native_relational.recognized() &&
          !malformed_ast.native_relational.accepted() &&
          malformed_ast.native_relational.root_relation_id == 0 &&
          malformed_ast.native_relational.relations.empty() &&
          malformed_ast.native_relational.grouping_sets.empty() &&
          HasParserDiagnostic(malformed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED"),
      "malformed ROLLUP syntax retained a partial native AST");

  auto contradictory_ast = ast;
  contradictory_ast.native_relational.grouping_sets.push_back(
      {2, 0,
       contradictory_ast.native_relational.relations[1]
           .grouping_key_expression_ids,
       {}});
  const auto contradictory_bound = sbsql::BindAst(
      contradictory_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);
  auto mismatched_semantic_context = context;
  mismatched_semantic_context.relations[0].semantic_variant_id =
      "aggregate.cube-int64-keys-count-sum.v1";
  const auto mismatched_semantic = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &mismatched_semantic_context);
  auto contradictory_lowering = bound;
  contradictory_lowering.native_relational.grouping_sets.push_back(
      {2, 0,
       contradictory_lowering.native_relational.relations[1]
           .grouping_key_expression_ids});
  const auto refused_lowering = sbsql::LowerToSblr(
      contradictory_lowering, cst, SessionForTest());
  passed &= Require(
      !contradictory_bound.bound &&
          contradictory_bound.messages.has_errors() &&
          !mismatched_semantic.bound &&
          mismatched_semantic.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "fixed ROLLUP accepted a contradictory semantic or grouping-set payload");
  return passed;
}

bool ValidateRollupBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL)) "
      "AS input(key_a,key_b,amount) GROUP BY ROLLUP(key_a,key_b) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.rollup-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  const auto* predicate =
      filter_relation != nullptr &&
              filter_relation->predicate_expression_ids.size() == 1
          ? &ast.native_relational.expressions[
                filter_relation->predicate_expression_ids.front() - 1]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          ast.native_relational.relations.size() == 3 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kRollup &&
          ast.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSum &&
          ast.native_relational.grouping_sets.empty() &&
          filter_relation != nullptr && predicate != nullptr &&
          predicate->expression_kind ==
              sbsql::NativeExpressionAstKind::kBinary &&
          predicate->operator_name == "AND" &&
          predicate->child_expression_ids.size() == 2,
      "native parser did not retain ordered Boolean HAVING over ROLLUP");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.rollup-int64-keys-count-sum.v1" &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              4 &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not preserve ROLLUP Boolean HAVING authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3 || predicate == nullptr) {
    return false;
  }

  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7,8") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand("relational_node_v1", "3", "2|0|2|1,2,4,5|-") &&
          has_operand(
              "relational_node_binding_v1", "2",
              EncodeHex("aggregate.rollup-int64-keys-count-sum.v1") +
                  "|1,2,3,5|-|-|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "ROLLUP Boolean HAVING did not lower to the exact canonical DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto matching_key_a_subtotals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 &&
               !row.fields[0].second.is_null &&
               row.fields[0].second.encoded_value == "1" &&
               row.fields[1].second.is_null &&
               row.fields[2].second.encoded_value == "4" &&
               row.fields[3].second.encoded_value == "15";
      });
  const auto matching_grand_totals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 && row.fields[0].second.is_null &&
               row.fields[1].second.is_null &&
               row.fields[2].second.encoded_value == "8" &&
               row.fields[3].second.encoded_value == "27";
      });
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 4 &&
          dispatched.canonical_result_row_count == 2 &&
          dispatched.api_result.result_shape.rows.size() == 2 &&
          matching_key_a_subtotals == 1 && matching_grand_totals == 1,
      "ROLLUP Boolean HAVING did not preserve subtotal/grand-total SQL truth");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b) HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  auto filter_semantic_drift_context = context;
  filter_semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto filter_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &filter_semantic_drift_context);
  auto aggregate_semantic_drift_context = context;
  aggregate_semantic_drift_context.relations.front().semantic_variant_id =
      "aggregate.cube-int64-keys-count-sum.v1";
  const auto aggregate_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_semantic_drift_context);

  auto bound_semantic_drift = bound;
  bound_semantic_drift.native_relational.relations[1].semantic_variant_id =
      "aggregate.cube-int64-keys-count-sum.v1";
  const auto refused_lowering = sbsql::LowerToSblr(
      bound_semantic_drift, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !filter_semantic_drift.bound &&
          filter_semantic_drift.messages.has_errors() &&
          !aggregate_semantic_drift.bound &&
          aggregate_semantic_drift.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "ROLLUP HAVING syntax, form, semantic, or lowering drift did not fail closed");
  return passed;
}

bool ValidateRollupGroupingMetadataBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kPreHavingSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL), "
      "(NULL,10,1)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b);";
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL), "
      "(NULL,10,1)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  constexpr std::string_view kAggregateSemantic =
      "aggregate.rollup-int64-keys-count-sum-grouping.v1";

  const auto pre_cst = sbsql::BuildCst(std::string(kPreHavingSql));
  const auto pre_ast = sbsql::BuildAst(pre_cst);
  const auto pre_context = GroupedAggregateBindingContext(
      pre_ast.native_relational, kAggregateSemantic);
  const auto pre_bound = sbsql::BindAst(
      pre_ast, pre_cst, ParserConfigForTest(), SessionForTest(), {},
      &pre_context);
  const auto pre_lowered =
      sbsql::LowerToSblr(pre_bound, pre_cst, SessionForTest());
  const auto pre_dispatched = sblr::DecodeAndDispatchSblrOperation(
      pre_lowered.payload, GroupingSetsEngineContext());

  using ExpectedRow =
      std::array<std::optional<std::string_view>, 7>;
  const auto count_rows = [](const auto& rows, const ExpectedRow& expected) {
    return std::ranges::count_if(rows, [&](const auto& row) {
      if (row.fields.size() != expected.size()) return false;
      for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        const auto& value = row.fields[ordinal].second;
        if (expected[ordinal].has_value()) {
          if (value.is_null || value.encoded_value != *expected[ordinal]) {
            return false;
          }
        } else if (!value.is_null) {
          return false;
        }
      }
      return true;
    });
  };

  bool passed = true;
  const auto& pre_rows = pre_dispatched.api_result.result_shape.rows;
  passed &= Require(
      pre_dispatched.envelope_validated && pre_dispatched.accepted &&
          pre_dispatched.dispatched_to_api &&
          pre_dispatched.physical_dag_executed &&
          pre_dispatched.canonical_result_published &&
          pre_dispatched.api_result.ok &&
          pre_dispatched.logical_node_count == 2 &&
          pre_dispatched.physical_node_count == 2 &&
          pre_dispatched.canonical_result_column_count == 7 &&
          pre_dispatched.canonical_result_row_count == 11 &&
          pre_rows.size() == 11 &&
          count_rows(pre_rows, {"1", "10", "2", "5", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows, {"1", "20", "1", "7", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {"1", std::nullopt, "1", "3", "0", "0", "0"}) == 1 &&
          count_rows(pre_rows, {"2", "10", "1", "4", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {std::nullopt, "10", "2", "9", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {"3", "30", "2", std::nullopt, "0", "0", "0"}) == 1 &&
          count_rows(pre_rows,
                     {"1", std::nullopt, "4", "15", "0", "1", "1"}) == 1 &&
          count_rows(pre_rows,
                     {"2", std::nullopt, "1", "4", "0", "1", "1"}) == 1 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "2", "9", "0", "1",
                      "1"}) == 1 &&
          count_rows(pre_rows,
                     {"3", std::nullopt, "2", std::nullopt, "0", "1",
                      "1"}) == 1 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "9", "28", "1", "1",
                      "3"}) == 1,
      "pre-HAVING ROLLUP metadata rows did not retain all 11 exact group identities");

  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational, kAggregateSemantic);
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  const auto* aggregate_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[1]
          : nullptr;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          aggregate_relation != nullptr && filter_relation != nullptr &&
          aggregate_relation->aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kRollup &&
          aggregate_relation->aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation->output_expression_ids.size() == 7 &&
          filter_relation->output_expression_ids ==
              aggregate_relation->output_expression_ids &&
          ast.native_relational.grouping_sets.empty(),
      "native parser did not preserve fixed ROLLUP metadata through HAVING");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              kAggregateSemantic &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              7 &&
          bound.native_relational.relations[2].output_expression_ids ==
              bound.native_relational.relations[1].output_expression_ids &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not preserve all seven ROLLUP HAVING outputs");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3) {
    return false;
  }

  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7,8,9") &&
          has_operand("relational_node_v1", "2",
                      "5|0|1|1,2,4,5,6,7,8|-") &&
          has_operand("relational_node_v1", "3",
                      "2|0|2|1,2,4,5,6,7,8|-") &&
          has_operand("relational_node_binding_v1", "2",
                      EncodeHex(kAggregateSemantic) +
                          "|1,2,3,5,6,7,8|-|-|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "ROLLUP metadata HAVING did not lower to the exact fixed seven-column DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto& rows = dispatched.api_result.result_shape.rows;
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 7 &&
          dispatched.canonical_result_row_count == 4 && rows.size() == 4 &&
          count_rows(rows,
                     {std::nullopt, "10", "2", "9", "0", "0", "0"}) ==
              1 &&
          count_rows(rows,
                     {"1", std::nullopt, "4", "15", "0", "1", "1"}) == 1 &&
          count_rows(rows,
                     {std::nullopt, std::nullopt, "2", "9", "0", "1",
                      "1"}) == 1 &&
          count_rows(rows,
                     {std::nullopt, std::nullopt, "9", "28", "1", "1",
                      "3"}) == 1 &&
          count_rows(rows, {"1", "10", "2", "5", "0", "0", "0"}) ==
              0 &&
          count_rows(rows,
                     {"3", "30", "2", std::nullopt, "0", "0", "0"}) == 0,
      "ROLLUP metadata HAVING did not preserve exact detail, subtotal, grand-total, FALSE, and UNKNOWN identities");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b) HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  const auto order_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_b,key_a) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 4;");
  const auto order_ast = sbsql::BuildAst(order_cst);

  auto aggregate_semantic_drift_context = context;
  aggregate_semantic_drift_context.relations.front().semantic_variant_id =
      "aggregate.rollup-int64-keys-count-sum.v1";
  const auto aggregate_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_semantic_drift_context);
  auto filter_semantic_drift_context = context;
  filter_semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto filter_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &filter_semantic_drift_context);

  auto key_drift_ast = ast;
  const auto& drift_aggregate = key_drift_ast.native_relational.relations[1];
  const auto grouping_a_id = drift_aggregate.output_expression_ids[4];
  const auto grouping_a = std::ranges::find_if(
      key_drift_ast.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_a_id;
      });
  grouping_a->child_expression_ids = {
      drift_aggregate.grouping_key_expression_ids[1]};
  const auto key_drift_bound = sbsql::BindAst(
      key_drift_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);

  auto order_drift_bound = bound;
  const auto grouping_id_id =
      order_drift_bound.native_relational.relations[1]
          .output_expression_ids[6];
  const auto grouping_id = std::ranges::find_if(
      order_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_id_id;
      });
  std::ranges::reverse(grouping_id->child_expression_ids);
  const auto refused_lowering = sbsql::LowerToSblr(
      order_drift_bound, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          order_ast.native_relational.recognized() &&
          !order_ast.native_relational.accepted() &&
          HasParserDiagnostic(order_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !aggregate_semantic_drift.bound &&
          aggregate_semantic_drift.messages.has_errors() &&
          !filter_semantic_drift.bound &&
          filter_semantic_drift.messages.has_errors() &&
          !key_drift_bound.bound && key_drift_bound.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "ROLLUP metadata HAVING parser, semantic, key/order, lineage, or lowering drift did not fail closed");
  return passed;
}

bool ValidateCubeParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,key_b);";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.cube-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.family == sbsql::StatementFamily::kQuery &&
          ast.native_relational.relations.size() == 2 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kCube &&
          ast.native_relational.grouping_sets.empty(),
      "native parser did not retain the exact two-key CUBE form");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 2 &&
          bound.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kCube &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.cube-int64-keys-count-sum.v1" &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not preserve CUBE with its authoritative semantic binding");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 2) {
    return false;
  }
  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand(
              "relational_node_binding_v1", "2",
              "6167677265676174652e637562652d696e7436342d6b6579732d636f"
              "756e742d73756d2e7631|1,2,3,5|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("SELECT key_a") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos,
      "typed CUBE lowering did not emit the fixed-profile canonical wire-v2 DAG");

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
      "parser-produced CUBE DAG did not complete the live engine spine");

  const auto malformed_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,);" );
  const auto malformed_ast = sbsql::BuildAst(malformed_cst);
  passed &= Require(
      malformed_ast.native_relational.recognized() &&
          !malformed_ast.native_relational.accepted() &&
          malformed_ast.native_relational.root_relation_id == 0 &&
          malformed_ast.native_relational.relations.empty() &&
          malformed_ast.native_relational.grouping_sets.empty() &&
          HasParserDiagnostic(malformed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED"),
      "malformed CUBE syntax retained a partial native AST");

  auto contradictory_ast = ast;
  contradictory_ast.native_relational.grouping_sets.push_back(
      {2, 0,
       contradictory_ast.native_relational.relations[1]
           .grouping_key_expression_ids,
       {}});
  const auto contradictory_bound = sbsql::BindAst(
      contradictory_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);
  auto mismatched_semantic_context = context;
  mismatched_semantic_context.relations[0].semantic_variant_id =
      "aggregate.rollup-int64-keys-count-sum.v1";
  const auto mismatched_semantic = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &mismatched_semantic_context);
  auto contradictory_lowering = bound;
  contradictory_lowering.native_relational.grouping_sets.push_back(
      {2, 0,
       contradictory_lowering.native_relational.relations[1]
           .grouping_key_expression_ids});
  const auto refused_lowering = sbsql::LowerToSblr(
      contradictory_lowering, cst, SessionForTest());
  passed &= Require(
      !contradictory_bound.bound &&
          contradictory_bound.messages.has_errors() &&
          !mismatched_semantic.bound &&
          mismatched_semantic.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "fixed CUBE accepted a contradictory semantic or grouping-set payload");
  return passed;
}

bool ValidateCubeBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL)) "
      "AS input(key_a,key_b,amount) GROUP BY CUBE(key_a,key_b) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.cube-int64-keys-count-sum.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  const auto* predicate =
      filter_relation != nullptr &&
              filter_relation->predicate_expression_ids.size() == 1
          ? &ast.native_relational.expressions[
                filter_relation->predicate_expression_ids.front() - 1]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          ast.native_relational.relations.size() == 3 &&
          ast.native_relational.relations[1].aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kCube &&
          ast.native_relational.relations[1].aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSum &&
          ast.native_relational.grouping_sets.empty() &&
          filter_relation != nullptr && predicate != nullptr &&
          predicate->expression_kind ==
              sbsql::NativeExpressionAstKind::kBinary &&
          predicate->operator_name == "AND" &&
          predicate->child_expression_ids.size() == 2,
      "native parser did not retain ordered Boolean HAVING over CUBE");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              "aggregate.cube-int64-keys-count-sum.v1" &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              4 &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not preserve CUBE Boolean HAVING authority");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3 || predicate == nullptr) {
    return false;
  }

  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7,8") &&
          has_operand("relational_node_v1", "2", "5|0|1|1,2,4,5|-") &&
          has_operand("relational_node_v1", "3", "2|0|2|1,2,4,5|-") &&
          has_operand(
              "relational_node_binding_v1", "2",
              EncodeHex("aggregate.cube-int64-keys-count-sum.v1") +
                  "|1,2,3,5|-|-|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "CUBE Boolean HAVING did not lower to the exact canonical DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto matching_key_a_subtotals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 &&
               !row.fields[0].second.is_null &&
               row.fields[0].second.encoded_value == "1" &&
               row.fields[1].second.is_null &&
               row.fields[2].second.encoded_value == "4" &&
               row.fields[3].second.encoded_value == "15";
      });
  const auto matching_key_b_subtotals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 && row.fields[0].second.is_null &&
               !row.fields[1].second.is_null &&
               row.fields[1].second.encoded_value == "10" &&
               row.fields[2].second.encoded_value == "4" &&
               row.fields[3].second.encoded_value == "17";
      });
  const auto matching_grand_totals = std::ranges::count_if(
      dispatched.api_result.result_shape.rows, [](const auto& row) {
        return row.fields.size() == 4 && row.fields[0].second.is_null &&
               row.fields[1].second.is_null &&
               row.fields[2].second.encoded_value == "8" &&
               row.fields[3].second.encoded_value == "27";
      });
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 4 &&
          dispatched.canonical_result_row_count == 3 &&
          dispatched.api_result.result_shape.rows.size() == 3 &&
          matching_key_a_subtotals == 1 &&
          matching_key_b_subtotals == 1 && matching_grand_totals == 1,
      "CUBE Boolean HAVING did not preserve both subtotals and grand-total SQL truth");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,key_b) HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  auto filter_semantic_drift_context = context;
  filter_semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto filter_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &filter_semantic_drift_context);
  auto aggregate_semantic_drift_context = context;
  aggregate_semantic_drift_context.relations.front().semantic_variant_id =
      "aggregate.rollup-int64-keys-count-sum.v1";
  const auto aggregate_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_semantic_drift_context);

  auto bound_semantic_drift = bound;
  bound_semantic_drift.native_relational.relations[1].semantic_variant_id =
      "aggregate.rollup-int64-keys-count-sum.v1";
  const auto refused_lowering = sbsql::LowerToSblr(
      bound_semantic_drift, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !filter_semantic_drift.bound &&
          filter_semantic_drift.messages.has_errors() &&
          !aggregate_semantic_drift.bound &&
          aggregate_semantic_drift.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "CUBE HAVING syntax, semantic, or lowering drift did not fail closed");
  return passed;
}

bool ValidateCubeGroupingMetadataBooleanHavingParserBindingLoweringAndDispatch() {
  constexpr std::string_view kPreHavingSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL), "
      "(NULL,10,1)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,key_b);";
  constexpr std::string_view kSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
      "(NULL,10,8), (1,10,NULL), (3,30,NULL), (3,30,NULL), "
      "(NULL,10,1)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,key_b) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 6;";
  constexpr std::string_view kAggregateSemantic =
      "aggregate.cube-int64-keys-count-sum-grouping.v1";

  const auto pre_cst = sbsql::BuildCst(std::string(kPreHavingSql));
  const auto pre_ast = sbsql::BuildAst(pre_cst);
  const auto pre_context = GroupedAggregateBindingContext(
      pre_ast.native_relational, kAggregateSemantic);
  const auto pre_bound = sbsql::BindAst(
      pre_ast, pre_cst, ParserConfigForTest(), SessionForTest(), {},
      &pre_context);
  const auto pre_lowered =
      sbsql::LowerToSblr(pre_bound, pre_cst, SessionForTest());
  const auto pre_dispatched = sblr::DecodeAndDispatchSblrOperation(
      pre_lowered.payload, GroupingSetsEngineContext());

  using ExpectedRow =
      std::array<std::optional<std::string_view>, 7>;
  const auto count_rows = [](const auto& rows, const ExpectedRow& expected) {
    return std::ranges::count_if(rows, [&](const auto& row) {
      if (row.fields.size() != expected.size()) return false;
      for (std::size_t ordinal = 0; ordinal < expected.size(); ++ordinal) {
        const auto& value = row.fields[ordinal].second;
        if (expected[ordinal].has_value()) {
          if (value.is_null || value.encoded_value != *expected[ordinal]) {
            return false;
          }
        } else if (!value.is_null) {
          return false;
        }
      }
      return true;
    });
  };

  bool passed = true;
  const auto& pre_rows = pre_dispatched.api_result.result_shape.rows;
  passed &= Require(
      pre_dispatched.envelope_validated && pre_dispatched.accepted &&
          pre_dispatched.dispatched_to_api &&
          pre_dispatched.physical_dag_executed &&
          pre_dispatched.canonical_result_published &&
          pre_dispatched.api_result.ok &&
          pre_dispatched.logical_node_count == 2 &&
          pre_dispatched.physical_node_count == 2 &&
          pre_dispatched.canonical_result_column_count == 7 &&
          pre_dispatched.canonical_result_row_count == 15 &&
          pre_rows.size() == 15 &&
          count_rows(pre_rows, {"1", "10", "2", "5", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows, {"1", "20", "1", "7", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {"1", std::nullopt, "1", "3", "0", "0", "0"}) == 1 &&
          count_rows(pre_rows, {"2", "10", "1", "4", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {std::nullopt, "10", "2", "9", "0", "0", "0"}) ==
              1 &&
          count_rows(pre_rows,
                     {"3", "30", "2", std::nullopt, "0", "0", "0"}) == 1 &&
          count_rows(pre_rows,
                     {"1", std::nullopt, "4", "15", "0", "1", "1"}) == 1 &&
          count_rows(pre_rows,
                     {"2", std::nullopt, "1", "4", "0", "1", "1"}) == 1 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "2", "9", "0", "1",
                      "1"}) == 1 &&
          count_rows(pre_rows,
                     {"3", std::nullopt, "2", std::nullopt, "0", "1",
                      "1"}) == 1 &&
          count_rows(pre_rows,
                     {std::nullopt, "10", "5", "18", "1", "0", "2"}) ==
              1 &&
          count_rows(pre_rows,
                     {std::nullopt, "20", "1", "7", "1", "0", "2"}) ==
              1 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "1", "3", "1", "0",
                      "2"}) == 1 &&
          count_rows(pre_rows,
                     {std::nullopt, "30", "2", std::nullopt, "1", "0",
                      "2"}) == 1 &&
          count_rows(pre_rows,
                     {std::nullopt, std::nullopt, "9", "28", "1", "1",
                      "3"}) == 1,
      "pre-HAVING CUBE metadata rows did not retain all 15 exact group identities");

  const auto cst = sbsql::BuildCst(std::string(kSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational, kAggregateSemantic);
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  const auto* aggregate_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[1]
          : nullptr;
  const auto* filter_relation =
      ast.native_relational.relations.size() == 3
          ? &ast.native_relational.relations[2]
          : nullptr;
  passed &= Require(
      ast.native_relational.accepted() &&
          ast.native_relational.root_relation_id == 3 &&
          aggregate_relation != nullptr && filter_relation != nullptr &&
          aggregate_relation->aggregate_grouping_form ==
              sbsql::NativeAggregateGroupingForm::kCube &&
          aggregate_relation->aggregate_projection_form ==
              sbsql::NativeAggregateProjectionForm::kKeysCountSumGrouping &&
          aggregate_relation->output_expression_ids.size() == 7 &&
          filter_relation->output_expression_ids ==
              aggregate_relation->output_expression_ids &&
          ast.native_relational.grouping_sets.empty(),
      "native parser did not preserve fixed CUBE metadata through HAVING");
  passed &= Require(
      bound.bound && bound.native_relational.bound &&
          bound.native_relational.relations.size() == 3 &&
          bound.native_relational.relations[1].semantic_variant_id ==
              kAggregateSemantic &&
          bound.native_relational.relations[2].semantic_variant_id ==
              "filter.having-count-sum-and-gt-int64-literals.v1" &&
          bound.native_relational.relations[2].output_expression_ids.size() ==
              7 &&
          bound.native_relational.relations[2].output_expression_ids ==
              bound.native_relational.relations[1].output_expression_ids &&
          bound.native_relational.grouping_sets.empty(),
      "native binder did not preserve all seven CUBE HAVING outputs");
  if (!bound.native_relational.bound ||
      bound.native_relational.relations.size() != 3) {
    return false;
  }

  const auto predicate_id =
      bound.native_relational.relations[2].bound_expression_ids.front();
  const auto has_operand_type = [&](const std::string_view type) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type;
    });
  };
  const auto has_operand = [&](const std::string_view type,
                               const std::string_view name,
                               const std::string_view value) {
    return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
      return operand.type == type && operand.name == name &&
             operand.value == value;
    });
  };
  passed &= Require(
      !lowered.messages.has_errors() && verified.admitted &&
          !verified.messages.has_errors() &&
          has_operand("uint32", "relational_root_node_id", "3") &&
          has_operand("relational_node_v1", "1",
                      "13|0|-|1,2,3|1,2,3,4,5,6,7,8,9") &&
          has_operand("relational_node_v1", "2",
                      "5|0|1|1,2,4,5,6,7,8|-") &&
          has_operand("relational_node_v1", "3",
                      "2|0|2|1,2,4,5,6,7,8|-") &&
          has_operand("relational_node_binding_v1", "2",
                      EncodeHex(kAggregateSemantic) +
                          "|1,2,3,5,6,7,8|-|-|-") &&
          has_operand(
              "relational_node_binding_v1", "3",
              EncodeHex(
                  "filter.having-count-sum-and-gt-int64-literals.v1") +
                  "|" + std::to_string(predicate_id) + "|-|-|-") &&
          !has_operand_type("relational_grouping_set_v1") &&
          lowered.payload.find("HAVING COUNT") == std::string::npos,
      "CUBE metadata HAVING did not lower to the exact fixed seven-column DAG");

  const auto dispatched = sblr::DecodeAndDispatchSblrOperation(
      lowered.payload, GroupingSetsEngineContext());
  const auto& rows = dispatched.api_result.result_shape.rows;
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
          dispatched.logical_node_count == 3 &&
          dispatched.physical_node_count == 3 &&
          dispatched.canonical_result_column_count == 7 &&
          dispatched.canonical_result_row_count == 5 && rows.size() == 5 &&
          count_rows(rows,
                     {std::nullopt, "10", "2", "9", "0", "0", "0"}) ==
              1 &&
          count_rows(rows,
                     {"1", std::nullopt, "4", "15", "0", "1", "1"}) == 1 &&
          count_rows(rows,
                     {std::nullopt, std::nullopt, "2", "9", "0", "1",
                      "1"}) == 1 &&
          count_rows(rows,
                     {std::nullopt, "10", "5", "18", "1", "0", "2"}) ==
              1 &&
          count_rows(rows,
                     {std::nullopt, std::nullopt, "9", "28", "1", "1",
                      "3"}) == 1 &&
          count_rows(rows, {"1", "10", "2", "5", "0", "0", "0"}) ==
              0 &&
          count_rows(rows,
                     {"3", "30", "2", std::nullopt, "0", "0", "0"}) == 0,
      "CUBE metadata HAVING did not preserve exact detail, both subtotal axes, grand-total, FALSE, and UNKNOWN identities");

  const auto sum_only_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,key_b) HAVING SUM(amount) > 4;");
  const auto sum_only_ast = sbsql::BuildAst(sum_only_cst);
  const auto order_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), GROUPING(key_a), "
      "GROUPING(key_b), GROUPING_ID(key_b,key_a) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY CUBE(key_a,key_b) "
      "HAVING COUNT(*) > 1 AND SUM(amount) > 4;");
  const auto order_ast = sbsql::BuildAst(order_cst);

  auto aggregate_semantic_drift_context = context;
  aggregate_semantic_drift_context.relations.front().semantic_variant_id =
      "aggregate.cube-int64-keys-count-sum.v1";
  const auto aggregate_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &aggregate_semantic_drift_context);
  auto filter_semantic_drift_context = context;
  filter_semantic_drift_context.relations.back().semantic_variant_id =
      "filter.having-sum-gt-int64-literal.v1";
  const auto filter_semantic_drift = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &filter_semantic_drift_context);

  auto key_drift_ast = ast;
  const auto& drift_aggregate = key_drift_ast.native_relational.relations[1];
  const auto grouping_a_id = drift_aggregate.output_expression_ids[4];
  const auto grouping_a = std::ranges::find_if(
      key_drift_ast.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_a_id;
      });
  grouping_a->child_expression_ids = {
      drift_aggregate.grouping_key_expression_ids[1]};
  const auto key_drift_bound = sbsql::BindAst(
      key_drift_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);

  auto order_drift_bound = bound;
  const auto grouping_id_id =
      order_drift_bound.native_relational.relations[1]
          .output_expression_ids[6];
  const auto grouping_id = std::ranges::find_if(
      order_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_id_id;
      });
  std::ranges::reverse(grouping_id->child_expression_ids);
  const auto refused_lowering = sbsql::LowerToSblr(
      order_drift_bound, cst, SessionForTest());

  passed &= Require(
      sum_only_ast.native_relational.recognized() &&
          !sum_only_ast.native_relational.accepted() &&
          HasParserDiagnostic(sum_only_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          order_ast.native_relational.recognized() &&
          !order_ast.native_relational.accepted() &&
          HasParserDiagnostic(order_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !aggregate_semantic_drift.bound &&
          aggregate_semantic_drift.messages.has_errors() &&
          !filter_semantic_drift.bound &&
          filter_semantic_drift.messages.has_errors() &&
          !key_drift_bound.bound && key_drift_bound.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "CUBE metadata HAVING parser, semantic, key/order, lineage, or lowering drift did not fail closed");
  return passed;
}

bool ValidateGroupingMetadataParserBindingLoweringAndDispatch() {
  const auto validate_form = [](
                                 const std::string_view grouping_clause,
                                 const sbsql::NativeAggregateGroupingForm
                                     expected_grouping_form,
                                 const std::string_view semantic_variant,
                                 const std::size_t expected_row_count,
                                 const bool expects_grouping_set_records) {
    const std::string sql =
        "SELECT key_a, key_b, COUNT(*), SUM(amount), "
        "GROUPING(key_a), GROUPING(key_b), GROUPING_ID(key_a,key_b) "
        "FROM (VALUES (1,10,5), (1,20,7), (1,NULL,3), (2,10,4), "
        "(NULL,10,8), (1,10,NULL)) AS input(key_a,key_b,amount) GROUP BY " +
        std::string(grouping_clause) + ";";
    const auto cst = sbsql::BuildCst(sql);
    const auto ast = sbsql::BuildAst(cst);

    bool passed = true;
    const bool ast_shape =
        ast.native_relational.accepted() &&
        ast.native_relational.relations.size() == 2 &&
        ast.native_relational.relations[1].aggregate_grouping_form ==
            expected_grouping_form &&
        ast.native_relational.relations[1].aggregate_projection_form ==
            sbsql::NativeAggregateProjectionForm::kKeysCountSumGrouping &&
        ast.native_relational.relations[1].output_expression_ids.size() == 7 &&
        (ast.native_relational.grouping_sets.empty() !=
         expects_grouping_set_records);
    passed &= Require(
        ast_shape,
        "native parser did not retain the exact grouping metadata projection form");
    if (!ast_shape) return false;

    const auto& aggregate = ast.native_relational.relations[1];
    const auto grouping_a = std::ranges::find_if(
        ast.native_relational.expressions, [&](const auto& expression) {
          return expression.expression_id == aggregate.output_expression_ids[4];
        });
    const auto grouping_b = std::ranges::find_if(
        ast.native_relational.expressions, [&](const auto& expression) {
          return expression.expression_id == aggregate.output_expression_ids[5];
        });
    const auto grouping_id = std::ranges::find_if(
        ast.native_relational.expressions, [&](const auto& expression) {
          return expression.expression_id == aggregate.output_expression_ids[6];
        });
    passed &= Require(
        grouping_a != ast.native_relational.expressions.end() &&
            grouping_b != ast.native_relational.expressions.end() &&
            grouping_id != ast.native_relational.expressions.end() &&
            grouping_a->expression_kind ==
                sbsql::NativeExpressionAstKind::kUnary &&
            grouping_a->operator_name == "grouping" &&
            grouping_a->child_expression_ids ==
                std::vector<std::uint32_t>{
                    aggregate.grouping_key_expression_ids[0]} &&
            grouping_b->expression_kind ==
                sbsql::NativeExpressionAstKind::kUnary &&
            grouping_b->operator_name == "grouping" &&
            grouping_b->child_expression_ids ==
                std::vector<std::uint32_t>{
                    aggregate.grouping_key_expression_ids[1]} &&
            grouping_id->expression_kind ==
                sbsql::NativeExpressionAstKind::kBinary &&
            grouping_id->operator_name == "grouping_id" &&
            grouping_id->child_expression_ids ==
                aggregate.grouping_key_expression_ids,
        "GROUPING/GROUPING_ID did not retain exact ordered key lineage");

    const auto context = GroupedAggregateBindingContext(
        ast.native_relational, semantic_variant);
    const auto bound = sbsql::BindAst(
        ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);
    const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
    const auto verified = sbsql::VerifySblrEnvelope(lowered);
    passed &= Require(
        bound.bound && bound.native_relational.bound &&
            bound.native_relational.relations.size() == 2 &&
            bound.native_relational.relations[1].aggregate_projection_form ==
                sbsql::NativeAggregateProjectionForm::kKeysCountSumGrouping &&
            bound.native_relational.relations[1].semantic_variant_id ==
                semantic_variant,
        "native binder did not retain the authoritative grouping metadata profile");
    if (!bound.native_relational.bound) return false;

    const auto has_operand_type = [&](const std::string_view type) {
      return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
        return operand.type == type;
      });
    };
    const auto has_operand = [&](const std::string_view type,
                                 const std::string_view name,
                                 const std::string_view value) {
      return std::ranges::any_of(lowered.operands, [&](const auto& operand) {
        return operand.type == type && operand.name == name &&
               operand.value == value;
      });
    };
    passed &= Require(
        !lowered.messages.has_errors() && verified.admitted &&
            !verified.messages.has_errors() &&
            has_operand("relational_node_v1", "2",
                        "5|0|1|1,2,4,5,6,7,8|-") &&
            has_operand("relational_node_binding_v1", "2",
                        EncodeHex(semantic_variant) +
                            "|1,2,3,5,6,7,8|-|-|-") &&
            has_operand("relational_expression_v1", "6",
                        "5|1|6|-|-|-|67726f7570696e67|-") &&
            has_operand("relational_expression_v1", "7",
                        "5|2|7|-|-|-|67726f7570696e67|-") &&
            has_operand("relational_expression_v1", "8",
                        "6|1,2|8|-|-|-|67726f7570696e675f6964|-") &&
            (has_operand_type("relational_grouping_set_v1") ==
             expects_grouping_set_records) &&
            lowered.payload.find("SELECT key_a") == std::string::npos &&
            lowered.payload.find("query.plan_operation") == std::string::npos,
        "grouping metadata did not lower to the exact canonical wire-v2 form");

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
            dispatched.canonical_result_published &&
            dispatched.api_result.ok &&
            dispatched.logical_node_count == 2 &&
            dispatched.physical_node_count == 2 &&
            dispatched.canonical_result_column_count == 7 &&
            dispatched.canonical_result_row_count == expected_row_count &&
            dispatched.api_result.result_shape.rows.size() ==
                expected_row_count &&
            dispatched.api_result.result_shape.columns.size() == 7 &&
            dispatched.api_result.result_shape.columns[4]
                    .canonical_type_name == "int64" &&
            dispatched.api_result.result_shape.columns[5]
                    .canonical_type_name == "int64" &&
            dispatched.api_result.result_shape.columns[6]
                    .canonical_type_name == "int64",
        "parser-produced grouping metadata profile did not complete the live engine spine");
    return passed;
  };

  bool passed = true;
  passed &= validate_form(
      "GROUPING SETS ((key_b), (), (key_b,key_a), (key_b))",
      sbsql::NativeAggregateGroupingForm::kGroupingSets,
      "aggregate.grouping-sets-int64-keys-count-sum-grouping.v1", 12,
      true);
  passed &= validate_form(
      "ROLLUP(key_a,key_b)",
      sbsql::NativeAggregateGroupingForm::kRollup,
      "aggregate.rollup-int64-keys-count-sum-grouping.v1", 9, false);
  passed &= validate_form(
      "CUBE(key_a,key_b)", sbsql::NativeAggregateGroupingForm::kCube,
      "aggregate.cube-int64-keys-count-sum-grouping.v1", 12, false);

  constexpr std::string_view kValidSql =
      "SELECT key_a, key_b, COUNT(*), SUM(amount), "
      "GROUPING(key_a), GROUPING(key_b), GROUPING_ID(key_a,key_b) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b);";
  const auto cst = sbsql::BuildCst(std::string(kValidSql));
  const auto ast = sbsql::BuildAst(cst);
  const auto context = GroupedAggregateBindingContext(
      ast.native_relational,
      "aggregate.rollup-int64-keys-count-sum-grouping.v1");
  const auto bound = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {}, &context);

  const auto malformed_cst = sbsql::BuildCst(
      "SELECT key_a, key_b, COUNT(*), SUM(amount), "
      "GROUPING(key_a), GROUPING(key_b), GROUPING_ID(key_b,key_a) "
      "FROM (VALUES (1,10,5)) AS input(key_a,key_b,amount) "
      "GROUP BY ROLLUP(key_a,key_b);" );
  const auto malformed_ast = sbsql::BuildAst(malformed_cst);

  auto mismatched_context = context;
  mismatched_context.relations[0].semantic_variant_id =
      "aggregate.rollup-int64-keys-count-sum.v1";
  const auto mismatched_semantic = sbsql::BindAst(
      ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &mismatched_context);

  auto key_drift_ast = ast;
  const auto& drift_aggregate = key_drift_ast.native_relational.relations[1];
  const auto grouping_a_id = drift_aggregate.output_expression_ids[4];
  const auto grouping_a = std::ranges::find_if(
      key_drift_ast.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_a_id;
      });
  grouping_a->child_expression_ids = {
      drift_aggregate.grouping_key_expression_ids[1]};
  const auto key_drift_bound = sbsql::BindAst(
      key_drift_ast, cst, ParserConfigForTest(), SessionForTest(), {},
      &context);

  auto operator_drift_bound = bound;
  const auto bound_grouping_a = std::ranges::find_if(
      operator_drift_bound.native_relational.expressions,
      [&](const auto& expression) {
        return expression.expression_id == grouping_a_id;
      });
  bound_grouping_a->canonical_operator_name = "grouping_id";
  const auto refused_lowering = sbsql::LowerToSblr(
      operator_drift_bound, cst, SessionForTest());

  passed &= Require(
      malformed_ast.native_relational.recognized() &&
          !malformed_ast.native_relational.accepted() &&
          malformed_ast.native_relational.relations.empty() &&
          malformed_ast.native_relational.expressions.empty() &&
          HasParserDiagnostic(malformed_ast.messages,
                              "QOW-DIAG-QRY-001-AST-MALFORMED") &&
          !mismatched_semantic.bound &&
          mismatched_semantic.messages.has_errors() &&
          !key_drift_bound.bound && key_drift_bound.messages.has_errors() &&
          refused_lowering.payload.empty() &&
          HasParserDiagnostic(refused_lowering.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "grouping metadata syntax, semantic, key, or operator drift did not fail closed");
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
// QOW-TEST-QRY-001-SIMPLE-GROUP-BY-V1
// QOW-TEST-QRY-001-BINDING-SIMPLE-GROUP-BY-V1
// QOW-TEST-QRY-001-HAVING-SUM-GT-V1
// QOW-TEST-QRY-001-BINDING-HAVING-SUM-GT-V1
// QOW-TEST-QRY-001-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-BINDING-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-TWO-KEY-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-BINDING-TWO-KEY-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-SIMPLE-TWO-KEY-GROUP-BY-V1
// QOW-TEST-QRY-001-BINDING-SIMPLE-TWO-KEY-GROUP-BY-V1
// QOW-TEST-QRY-001-GROUPING-SETS-V1
// QOW-TEST-QRY-001-BINDING-GROUPING-SETS-V1
// QOW-TEST-QRY-001-GROUPING-SETS-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-BINDING-GROUPING-SETS-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-GROUPING-SETS-GROUPING-METADATA-HAVING-V1
// QOW-TEST-QRY-001-BINDING-GROUPING-SETS-GROUPING-METADATA-HAVING-V1
// QOW-TEST-QRY-001-ROLLUP-V1
// QOW-TEST-QRY-001-BINDING-ROLLUP-V1
// QOW-TEST-QRY-001-ROLLUP-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-BINDING-ROLLUP-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-ROLLUP-GROUPING-METADATA-HAVING-V1
// QOW-TEST-QRY-001-BINDING-ROLLUP-GROUPING-METADATA-HAVING-V1
// QOW-TEST-QRY-001-CUBE-V1
// QOW-TEST-QRY-001-BINDING-CUBE-V1
// QOW-TEST-QRY-001-CUBE-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-BINDING-CUBE-HAVING-COUNT-SUM-AND-GT-V1
// QOW-TEST-QRY-001-CUBE-GROUPING-METADATA-HAVING-V1
// QOW-TEST-QRY-001-BINDING-CUBE-GROUPING-METADATA-HAVING-V1
// QOW-TEST-QRY-001-GROUPING-METADATA-V1
// QOW-TEST-QRY-001-BINDING-GROUPING-METADATA-V1
// QOW-TEST-QRY-005-V1
int main() {
  bool passed = true;
  passed &= ValidateCanonicalLoweringAndDispatch();
  passed &= ValidateSimpleGroupByParserBindingLoweringAndDispatch();
  passed &= ValidateHavingParserBindingLoweringAndDispatch();
  passed &= ValidateBooleanHavingParserBindingLoweringAndDispatch();
  passed &= ValidateTwoKeyBooleanHavingParserBindingLoweringAndDispatch();
  passed &= ValidateSimpleTwoKeyGroupByParserBindingLoweringAndDispatch();
  passed &= ValidateGroupingSetsParserBindingLoweringAndDispatch();
  passed &=
      ValidateGroupingSetsBooleanHavingParserBindingLoweringAndDispatch();
  passed &=
      ValidateGroupingSetsGroupingMetadataBooleanHavingParserBindingLoweringAndDispatch();
  passed &= ValidateRollupParserBindingLoweringAndDispatch();
  passed &= ValidateRollupBooleanHavingParserBindingLoweringAndDispatch();
  passed &=
      ValidateRollupGroupingMetadataBooleanHavingParserBindingLoweringAndDispatch();
  passed &= ValidateCubeParserBindingLoweringAndDispatch();
  passed &= ValidateCubeBooleanHavingParserBindingLoweringAndDispatch();
  passed &=
      ValidateCubeGroupingMetadataBooleanHavingParserBindingLoweringAndDispatch();
  passed &= ValidateGroupingMetadataParserBindingLoweringAndDispatch();
  passed &= ValidateFailClosedLowering();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
