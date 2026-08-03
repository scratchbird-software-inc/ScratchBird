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
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace scratchbird::engine::sblr {
SblrDispatchResult DispatchTextualRelationalQueryForContractTest(
    SblrDispatchRequest request);
}

namespace sbsql = scratchbird::parser::sbsql;
namespace sblr = scratchbird::engine::sblr;
namespace api = scratchbird::engine::internal_api;

namespace {

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

sblr::SblrOperationEnvelope EngineQueryEnvelope(
    const sbsql::SblrEnvelope& lowered) {
  auto envelope = sblr::MakeSblrEnvelope(
      lowered.operation_id, lowered.sblr_opcode, lowered.trace_key);
  envelope.result_shape = lowered.result_shape_key;
  envelope.requires_transaction_context = true;
  envelope.operands.reserve(lowered.operands.size());
  for (const auto& operand : lowered.operands) {
    envelope.operands.push_back(
        {operand.type, operand.name, operand.value});
  }
  return envelope;
}

bool HasParserDiagnostic(const sbsql::MessageVectorSet& messages,
                         const std::string_view code) {
  return std::ranges::any_of(
      messages.diagnostics, [code](const auto& diagnostic) {
        return diagnostic.code == code;
      });
}

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  return std::ranges::any_of(
      result.api_result.diagnostics, [code](const auto& diagnostic) {
        return diagnostic.code == code;
      });
}

void SetEngineStatementAuthority(
    sbsql::NativeRelationalBindingContext* context) {
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

sbsql::NativeRelationalBindingContext ScalarBindingContext() {
  sbsql::NativeRelationalBindingContext context;
  context.bound_ast_uuid = "019f0000-0000-7000-8000-000000000601";
  context.catalog_epoch_uuid = "019f0000-0000-7100-8000-000000000602";
  context.security_context_uuid = "019f0000-0000-7110-8000-000000000602";
  context.statement_uuid = "019f0000-0000-7120-8000-000000000610";
  context.owning_transaction_uuid = "019f0000-0000-7130-8000-000000000611";
  context.statement_snapshot_uuid = "019f0000-0000-7140-8000-000000000612";
  context.statement_metadata_snapshot_uuid =
      "019f0000-0000-7150-8000-000000000613";
  context.local_transaction_id = 601;
  context.snapshot_visible_through_local_transaction_id = 602;
  SetEngineStatementAuthority(&context);
  context.descriptors = {
      {1,
       "019f0000-0000-7200-8000-000000000603",
       "019f0000-0000-7300-8000-000000000604",
       sbsql::BoundNullability::kNullable,
       std::nullopt,
       std::nullopt,
       {}},
      {2,
       "019f0000-0000-7200-8000-000000000605",
       "019f0000-0000-7300-8000-000000000606",
       sbsql::BoundNullability::kNullable,
       "019f0000-0000-7400-8000-000000000607",
       std::nullopt,
       {}}};
  for (std::uint32_t expression_id = 1; expression_id <= 10;
       ++expression_id) {
    const std::uint32_t descriptor_id =
        expression_id == 4 || expression_id == 5 || expression_id == 9 ||
                expression_id == 10
            ? 2
            : 1;
    context.expressions.push_back(
        {expression_id, descriptor_id, std::nullopt, std::nullopt});
  }
  context.expressions[4].function_uuid =
      "019f0000-0000-7500-8000-000000000608";
  context.outputs = {
      {1, 3, "computed_number", 1, true, 0},
      {2, 5, "computed_text", 2, true, 1},
  };
  return context;
}

sbsql::ParserConfig ParserConfigForTest() {
  sbsql::ParserConfig config;
  config.parser_uuid = "019f0000-0000-7500-8000-000000000609";
  config.bundle_contract_id = "sbp_sbsql@qow-qry-006-v1";
  config.build_id = "qow-qry-006-v1";
  return config;
}

sbsql::SessionContext SessionForTest() {
  sbsql::SessionContext session;
  session.authenticated = true;
  session.session_uuid = "019f0000-0000-7600-8000-00000000060a";
  session.connection_uuid = "019f0000-0000-7600-8000-00000000060b";
  session.database_uuid = "019f0000-0000-7600-8000-00000000060c";
  session.dialect_profile_uuid = "019f0000-0000-7600-8000-00000000060d";
  session.catalog_epoch = 17;
  session.security_policy_epoch = 19;
  session.descriptor_epoch = 23;
  return session;
}

sbsql::BoundStatement BoundScalarValues(const sbsql::CstDocument& cst) {
  const auto ast = sbsql::BuildAst(cst);
  const auto context = ScalarBindingContext();
  return sbsql::BindAst(ast, cst, ParserConfigForTest(), SessionForTest(),
                        {}, &context);
}

bool HasOperand(const sbsql::SblrEnvelope& envelope,
                const std::string_view type,
                const std::string_view name,
                const std::string_view value) {
  return std::ranges::any_of(envelope.operands, [&](const auto& operand) {
    return operand.type == type && operand.name == name &&
           operand.value == value;
  });
}

api::EngineRequestContext ScalarEngineContext() {
  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000000610";
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000000611";
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000000612";
  context.catalog_epoch_uuid.canonical =
      "019f0000-0000-7100-8000-000000000602";
  context.local_transaction_id = 601;
  context.snapshot_visible_through_local_transaction_id = 602;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000000613";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7110-8000-000000000602";
  context.catalog_generation_id = 602;
  context.security_epoch = 603;
  context.resource_epoch = 604;
  context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006001";
  context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006002";
  context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006003";
  context.optimizer_route_epoch = 605;
  context.optimizer_route_generation = 606;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000;
  context.optimizer_spill_allowed = true;
  context.current_monotonic_ns = "602000";
  context.authorization_context.security_epoch = 603;
  context.authorization_context.policy_epoch = 604;
  context.authorization_context.catalog_generation_id = 602;
  return context;
}

bool ValidateComposableScalarLowering() {
  const auto cst = sbsql::BuildCst(
      "VALUES (1 + 2, lower('A')), (? IS NULL, -3);");
  const auto bound = BoundScalarValues(cst);
  if (!bound.bound) {
    std::cerr << "typed scalar bind refused; expressions="
              << bound.native_relational.expressions.size() << '\n';
    for (const auto& diagnostic : bound.messages.diagnostics) {
      std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    }
    return false;
  }
  const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());
  const auto unrelated_cst =
      sbsql::BuildCst("SELECT ROW_NUMBER() OVER (ORDER BY ignored);");
  const auto lowered_without_shape_input =
      sbsql::LowerToSblr(bound, unrelated_cst, SessionForTest());
  const auto verified = sbsql::VerifySblrEnvelope(lowered);

  bool passed = true;
  passed &= Require(bound.bound && !bound.messages.has_errors(),
                    "typed scalar composition did not bind");
  passed &= Require(!lowered.messages.has_errors() &&
                        verified.admitted && !verified.messages.has_errors(),
                    "typed scalar composition did not lower and verify");
  passed &= Require(lowered.payload == lowered_without_shape_input.payload,
                    "canonical lowering depended on SQL/CST shape input");
  passed &= Require(
      HasOperand(lowered, "relational_expression_v1", "3",
                 "6|1,2|1|-|-|-|2b|-") &&
          HasOperand(lowered, "relational_expression_v1", "5",
                     "4|4|2|019f0000-0000-7500-8000-000000000608|-|-|-|-") &&
          HasOperand(lowered, "relational_expression_v1", "6",
                     "2|-|1|-|-|-|-|3f") &&
          HasOperand(lowered, "relational_expression_v1", "8",
                     "6|6,7|1|-|-|-|4953|-") &&
          HasOperand(lowered, "relational_expression_v1", "10",
                     "5|9|2|-|-|-|2d|-") &&
          HasOperand(lowered, "relational_values_row_v1", "1", "3,5") &&
          HasOperand(lowered, "relational_values_row_v1", "2", "8,10") &&
          HasOperand(lowered, "relational_node_v1", "1",
                     "13|0|-|1,2|1,2"),
      "typed scalar/operator/row composition records differ");
  passed &= Require(
      lowered.operands.size() > 10 &&
          lowered.operands[2].type == "uuid" &&
          lowered.operands[2].name == "relational_catalog_epoch_uuid" &&
          lowered.operands[2].value ==
              "019f0000-0000-7100-8000-000000000602" &&
          lowered.operands[4].type == "uuid" &&
          lowered.operands[4].name == "relational_statement_uuid" &&
          lowered.operands[4].value ==
              "019f0000-0000-7120-8000-000000000610" &&
          lowered.operands[5].type == "uuid" &&
          lowered.operands[5].name ==
              "relational_owning_transaction_uuid" &&
          lowered.operands[5].value ==
              "019f0000-0000-7130-8000-000000000611" &&
          lowered.operands[6].type == "uuid" &&
          lowered.operands[6].name ==
              "relational_statement_snapshot_uuid" &&
          lowered.operands[6].value ==
              "019f0000-0000-7140-8000-000000000612" &&
          lowered.operands[7].type == "uuid" &&
          lowered.operands[7].name ==
              "relational_statement_metadata_snapshot_uuid" &&
          lowered.operands[7].value ==
              "019f0000-0000-7150-8000-000000000613" &&
          lowered.operands[8].type == "uint64" &&
          lowered.operands[8].name ==
              "relational_local_transaction_id" &&
          lowered.operands[8].value == "601" &&
          lowered.operands[9].type == "uint64" &&
          lowered.operands[9].name ==
              "relational_snapshot_visible_through_local_transaction_id" &&
          lowered.operands[9].value == "602",
      "scalar lowering changed statement-context type, order, identity, or width");
  passed &= Require(
      lowered.payload.find("VALUES (") == std::string::npos &&
          lowered.payload.find("lower(") == std::string::npos &&
          lowered.payload.find("ROW_NUMBER") == std::string::npos &&
          lowered.payload.find("query.plan_operation") == std::string::npos,
      "canonical typed lowering leaked SQL or selected a shape-analyzer route");

  api::EngineRequestContext context;
  context.security_context_present = true;
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000000610";
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000000611";
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000000612";
  context.catalog_epoch_uuid.canonical =
      "019f0000-0000-7100-8000-000000000602";
  context.local_transaction_id = 601;
  context.snapshot_visible_through_local_transaction_id = 602;
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000000613";
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical =
      "019f0000-0000-7110-8000-000000000602";
  context.catalog_generation_id = 602;
  context.security_epoch = 603;
  context.resource_epoch = 604;
  context.optimizer_capability_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006001";
  context.optimizer_resource_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006002";
  context.optimizer_route_snapshot_uuid.canonical =
      "019f0000-0000-7200-8000-000000006003";
  context.optimizer_route_epoch = 605;
  context.optimizer_route_generation = 606;
  context.optimizer_memory_budget_bytes = 64 * 1024 * 1024;
  context.optimizer_maximum_candidate_count = 131072;
  context.optimizer_maximum_memo_groups = 131072;
  context.optimizer_maximum_search_steps = 1048576;
  context.optimizer_maximum_planning_time_ns = 5'000'000'000;
  context.optimizer_spill_allowed = true;
  context.current_monotonic_ns = "602000";
  context.authorization_context.security_epoch = 603;
  context.authorization_context.policy_epoch = 604;
  context.authorization_context.catalog_generation_id = 602;
  const auto dispatched = sblr::DispatchTextualRelationalQueryForContractTest(
      {std::move(context), EngineQueryEnvelope(lowered), {}});
  passed &= Require(
      dispatched.envelope_validated && dispatched.accepted &&
          dispatched.dispatched_to_api &&
          dispatched.logical_graph_populated &&
          dispatched.logical_properties_populated &&
          dispatched.optimizer_admitted &&
          dispatched.optimizer_admission_stage_count == 8 &&
          dispatched.logical_node_count == 1 &&
          dispatched.logical_property_count == 0 &&
          !dispatched.optimizer_selected &&
          !dispatched.physical_dag_published &&
          !dispatched.physical_dag_executed &&
          !dispatched.runtime_actuals_attached &&
          !dispatched.canonical_result_published &&
          dispatched.physical_node_count == 0 &&
          dispatched.canonical_result_bytes.empty() &&
          !dispatched.api_result.ok &&
          HasApiDiagnostic(
              dispatched,
              "QOW-DIAG-RELATIONAL-LIVE-VALUES-PAYLOAD-V1"),
      "composed VALUES expressions did not fail closed before plan publication");
  return passed;
}

bool ValidateCompositionRefusal() {
  const auto cst = sbsql::BuildCst(
      "VALUES (1 + 2, lower('A')), (? IS NULL, -3);");

  auto missing_operator = BoundScalarValues(cst);
  if (!missing_operator.bound ||
      missing_operator.native_relational.expressions.size() < 3) {
    return Require(false, "typed scalar refusal fixture did not bind");
  }
  missing_operator.native_relational.expressions[2]
      .canonical_operator_name.reset();
  const auto operator_refusal =
      sbsql::LowerToSblr(missing_operator, cst, SessionForTest());

  auto missing_row = BoundScalarValues(cst);
  missing_row.native_relational.relations[0].values_row_ids.pop_back();
  const auto row_refusal =
      sbsql::LowerToSblr(missing_row, cst, SessionForTest());

  bool passed = true;
  passed &= Require(
      operator_refusal.payload.empty() &&
          HasParserDiagnostic(operator_refusal.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "operator expression with omitted typed identity was lowered");
  passed &= Require(
      row_refusal.payload.empty() &&
          HasParserDiagnostic(row_refusal.messages,
                              "SBLR.PLAN_TREE.INVALID_HANDLE"),
      "VALUES composition with omitted row membership was lowered");
  return passed;
}

bool ValidateScalarStatementContextRefusal() {
  const auto cst = sbsql::BuildCst(
      "VALUES (1 + 2, lower('A')), (? IS NULL, -3);");
  const auto lowered =
      sbsql::LowerToSblr(BoundScalarValues(cst), cst, SessionForTest());
  const auto refused_before_publication = [&](api::EngineRequestContext context) {
    const auto result = sblr::DispatchTextualRelationalQueryForContractTest(
        {std::move(context), EngineQueryEnvelope(lowered), {}});
    return !result.envelope_validated && !result.accepted &&
           !result.dispatched_to_api && !result.logical_graph_populated &&
           !result.logical_properties_populated &&
           result.logical_node_count == 0 &&
           result.logical_property_count == 0 && !result.optimizer_admitted &&
           !result.optimizer_selected && !result.physical_dag_published &&
           !result.physical_dag_executed && !result.runtime_actuals_attached &&
           !result.canonical_result_published && !result.api_result.ok &&
           result.physical_node_count == 0 &&
           result.canonical_result_column_count == 0 &&
           result.canonical_result_row_count == 0 &&
           result.selected_plan_uuid.empty() &&
           result.canonical_result_bytes.empty() &&
           result.diagnostics.size() == 1 &&
           HasApiDiagnostic(result,
                            "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1");
  };

  bool passed = true;
  auto context = ScalarEngineContext();
  context.statement_uuid.canonical =
      "019f0000-0000-7120-8000-000000000699";
  passed &= Require(refused_before_publication(std::move(context)),
                    "statement mismatch published scalar query evidence");
  context = ScalarEngineContext();
  context.transaction_uuid.canonical =
      "019f0000-0000-7130-8000-000000000699";
  passed &= Require(refused_before_publication(std::move(context)),
                    "transaction mismatch published scalar query evidence");
  context = ScalarEngineContext();
  context.statement_snapshot_uuid.canonical =
      "019f0000-0000-7140-8000-000000000699";
  passed &= Require(refused_before_publication(std::move(context)),
                    "data snapshot mismatch published scalar query evidence");
  context = ScalarEngineContext();
  context.statement_metadata_snapshot_uuid.canonical =
      "019f0000-0000-7150-8000-000000000699";
  passed &= Require(refused_before_publication(std::move(context)),
                    "metadata snapshot mismatch published scalar query evidence");
  context = ScalarEngineContext();
  context.catalog_epoch_uuid.canonical =
      "019f0000-0000-7100-8000-000000000699";
  passed &= Require(refused_before_publication(std::move(context)),
                    "catalog mismatch published scalar query evidence");
  context = ScalarEngineContext();
  ++context.local_transaction_id;
  passed &= Require(refused_before_publication(std::move(context)),
                    "local transaction mismatch published scalar query evidence");
  context = ScalarEngineContext();
  ++context.snapshot_visible_through_local_transaction_id;
  passed &= Require(refused_before_publication(std::move(context)),
                    "visibility mismatch published scalar query evidence");
  return passed;
}

bool ValidateTemporalTableSourceRefusal() {
  struct RefusalCase {
    std::string sql;
    sbsql::NativeTemporalTableAxis axis;
    sbsql::NativeTemporalTableForm form;
  };
  const std::vector<RefusalCase> cases = {
      {"SELECT * FROM account_history FOR SYSTEM_TIME AS OF "
       "TIMESTAMP '2026-07-26T00:00:00Z';",
       sbsql::NativeTemporalTableAxis::kSystemTime,
       sbsql::NativeTemporalTableForm::kAsOf},
      {"SELECT * FROM account_history FOR VALID_TIME BETWEEN "
       "DATE '2026-01-01' AND DATE '2026-12-31';",
       sbsql::NativeTemporalTableAxis::kValidTime,
       sbsql::NativeTemporalTableForm::kBetween},
      {"WITH requested AS (SELECT * FROM account_history FOR ALL VALID_TIME) "
       "SELECT * FROM requested;",
       sbsql::NativeTemporalTableAxis::kValidTime,
       sbsql::NativeTemporalTableForm::kAll},
      {"SELECT * FROM account_history h JOIN audit_history a "
       "FOR SYSTEM TIME FROM TIMESTAMP '2026-01-01T00:00:00Z' "
       "TO TIMESTAMP '2026-02-01T00:00:00Z' ON h.id = a.id;",
       sbsql::NativeTemporalTableAxis::kSystemTime,
       sbsql::NativeTemporalTableForm::kFromTo},
  };

  bool passed = true;
  for (const auto& refusal_case : cases) {
    const auto cst = sbsql::BuildCst(refusal_case.sql);
    const auto native = sbsql::ParseNativeRelationalAst(cst);
    const auto ast = sbsql::BuildAst(cst);
    const auto bound = sbsql::BindAst(ast, cst, ParserConfigForTest(),
                                      SessionForTest(), {});
    const auto lowered = sbsql::LowerToSblr(bound, cst, SessionForTest());

    passed &= Require(
        native.status == sbsql::NativeRelationalParseStatus::kRefused &&
            native.temporal_table_source_refusal.has_value() &&
            native.temporal_table_source_refusal->axis == refusal_case.axis &&
            native.temporal_table_source_refusal->form == refusal_case.form &&
            native.relations.empty() && native.values_rows.empty() &&
            native.expressions.empty() &&
            HasParserDiagnostic(
                native.messages,
                "QOW-DIAG-QRY-006-TEMPORAL-REFUSAL-V1"),
        "temporal source did not produce the typed parser-profile refusal");
    passed &= Require(
        ast.family == sbsql::StatementFamily::kQuery &&
            ast.exact_refusal_required && !ast.produces_sblr &&
            ast.diagnostic_key ==
                "QOW-DIAG-QRY-006-TEMPORAL-REFUSAL-V1" &&
            !bound.bound && lowered.payload.empty() &&
            HasParserDiagnostic(
                lowered.messages,
                "QOW-DIAG-QRY-006-TEMPORAL-REFUSAL-V1"),
        "temporal source crossed the AST, binding, or SBLR refusal boundary");
  }

  const auto temporal_literal =
      sbsql::ParseNativeRelationalAst(sbsql::BuildCst(
          "VALUES (TIMESTAMP '2026-07-26T00:00:00Z');"));
  passed &= Require(
      temporal_literal.accepted() &&
          !temporal_literal.temporal_table_source_refusal.has_value() &&
          temporal_literal.expressions.size() == 1 &&
          temporal_literal.expressions.front().literal_kind ==
              sbsql::NativeLiteralAstKind::kTemporal,
      "ordinary temporal scalar literal was confused with a table source");

  const auto ordinary_select = sbsql::BuildAst(
      sbsql::BuildCst("SELECT system_time FROM account_history;"));
  passed &= Require(
      !ordinary_select.native_relational.recognized() &&
          !ordinary_select.exact_refusal_required &&
          !HasParserDiagnostic(
              ordinary_select.messages,
              "QOW-DIAG-QRY-006-TEMPORAL-REFUSAL-V1"),
      "ordinary temporal-named projection was confused with a table source");
  return passed;
}

}  // namespace

// QOW-ROUTE-STAGE-QRY-006-V1
// QOW-TEST-QRY-006-V1
// QOW-TEST-QRY-006-TEMPORAL-REFUSAL-V1
int main() {
  bool passed = true;
  passed &= ValidateComposableScalarLowering();
  passed &= ValidateCompositionRefusal();
  passed &= ValidateScalarStatementContextRefusal();
  passed &= ValidateTemporalTableSourceRefusal();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
