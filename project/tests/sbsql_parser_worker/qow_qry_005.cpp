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
#include <string_view>

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

bool HasApiDiagnostic(const sblr::SblrDispatchResult& result,
                      const std::string_view code) {
  return std::ranges::any_of(
      result.api_result.diagnostics, [code](const auto& diagnostic) {
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
      !dispatched.api_result.ok &&
          HasApiDiagnostic(
              dispatched,
              "QOW-DIAG-RELATIONAL-PHYSICAL-DISPATCH-PENDING"),
      "structural lowering synthesized completion or bypassed the pending seam");
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
// QOW-TEST-QRY-005-V1
int main() {
  bool passed = true;
  passed &= ValidateCanonicalLoweringAndDispatch();
  passed &= ValidateFailClosedLowering();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
