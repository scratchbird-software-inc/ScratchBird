// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "lowering/lowering.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sbsql = scratchbird::parser::sbsql;

namespace {

// QOW-TEST-SBLR-VERIFIER-V1

constexpr std::string_view kDescriptorUuid =
    "019f0000-0000-7200-8000-000000000401";
constexpr std::string_view kTypeUuid =
    "019f0000-0000-7300-8000-000000000402";

bool Require(const bool condition, const std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

std::string Hex(const std::string_view value) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string encoded;
  encoded.reserve(value.size() * 2);
  for (const unsigned char ch : value) {
    encoded.push_back(kDigits[ch >> 4]);
    encoded.push_back(kDigits[ch & 0x0f]);
  }
  return encoded;
}

std::string JoinHandles(const std::vector<std::uint32_t>& handles) {
  if (handles.empty()) return "-";
  std::string encoded;
  for (const auto handle : handles) {
    if (!encoded.empty()) encoded.push_back(',');
    encoded += std::to_string(handle);
  }
  return encoded;
}

bool HasDiagnostic(const sbsql::SblrVerifierResult& result,
                   const std::string_view code,
                   const std::string_view field_id = {}) {
  return std::ranges::any_of(
      result.messages.diagnostics, [&](const auto& diagnostic) {
        if (diagnostic.code != code) return false;
        if (field_id.empty()) return true;
        return std::ranges::any_of(diagnostic.fields, [&](const auto& field) {
          return field.name == "field_id" && field.value == field_id;
        });
      });
}

sbsql::SblrEnvelope BaseEnvelope(const std::uint32_t root_node_id) {
  sbsql::SblrEnvelope envelope;
  envelope.operation_family = "sblr.query.relational.v3";
  envelope.envelope_version = 3;
  envelope.statement_hash = 401;
  envelope.surface_key = "qow.sblr.verifier.v1";
  envelope.command_family = "query";
  envelope.operation_id = "query.execute";
  envelope.sblr_operation_key = "sblr.query.relational.v3";
  envelope.sblr_opcode = "SBLR_QUERY_EXECUTE";
  envelope.engine_api_operation_id = "query.execute";
  envelope.result_shape_key = "query_execute_result";
  envelope.diagnostic_shape_key = "engine.diagnostic.v1";
  envelope.resource_contract_key = "resource.contract.query_read";
  envelope.required_rights = {"right.read"};
  envelope.required_authority_steps = {
      "authority.engine.query_execute_api_required",
      "authority.server.transaction_context_required",
      "authority.engine.mga_transaction_context_required",
      "authority.parser.no_security_authorization",
      "authority.parser.no_storage_or_finality",
      "authority.parser.no_sql_text_execution",
  };
  envelope.payload = "canonical_binary_transport_deferred_to_qow_ct_003";
  envelope.operands = {
      {"uint16", "relational_wire_version", "2"},
      {"uuid", "relational_bound_sblr_tree_uuid",
       "019f0000-0000-7000-8000-000000000400"},
      {"uuid", "relational_catalog_epoch_uuid",
       "019f0000-0000-7100-8000-000000000403"},
      {"uuid", "relational_security_context_uuid",
       "019f0000-0000-7110-8000-000000000404"},
      {"uuid", "relational_statement_uuid",
       "019f0000-0000-7120-8000-000000000405"},
      {"uuid", "relational_owning_transaction_uuid",
       "019f0000-0000-7130-8000-000000000406"},
      {"uuid", "relational_statement_snapshot_uuid",
       "019f0000-0000-7140-8000-000000000407"},
      {"uuid", "relational_statement_metadata_snapshot_uuid",
       "019f0000-0000-7150-8000-000000000408"},
      {"uint64", "relational_local_transaction_id", "401"},
      {"uint64",
       "relational_snapshot_visible_through_local_transaction_id", "0"},
      {"uint32", "relational_root_node_id", std::to_string(root_node_id)},
      {"relational_descriptor_v1", "1",
       std::string(kDescriptorUuid) + "|" + std::string(kTypeUuid) +
           "|1|-|-|-|-|-"},
      {"relational_expression_v1", "1", "1|-|1|-|-|1|-|31"},
  };
  return envelope;
}

void AddOutput(sbsql::SblrEnvelope* envelope, const std::uint32_t output_id,
               const std::uint32_t node_id,
               const std::uint32_t expression_id = 1,
               const std::uint32_t descriptor_id = 1) {
  envelope->operands.push_back(
      {"relational_output_v1", std::to_string(output_id),
       std::to_string(node_id) + "|" + std::to_string(expression_id) + "|" +
           std::to_string(descriptor_id) + "|1|0|" +
           Hex("column_" + std::to_string(node_id))});
}

void AddNode(sbsql::SblrEnvelope* envelope, const std::uint32_t node_id,
             const std::uint32_t kind,
             const std::vector<std::uint32_t>& inputs,
             const bool shareable = false,
             const std::string_view rows = "-",
             const std::string_view bound_expressions = "1") {
  envelope->operands.push_back(
      {"relational_node_v1", std::to_string(node_id),
       std::to_string(kind) + "|" + (shareable ? "1" : "0") + "|" +
           JoinHandles(inputs) + "|1|" + std::string(rows)});
  envelope->operands.push_back(
      {"relational_node_binding_v1", std::to_string(node_id),
       Hex("relational.contract-node.v1") + "|" +
           std::string(bound_expressions) + "|-|-|-"});
}

sbsql::SblrEnvelope ValuesEnvelope() {
  auto envelope = BaseEnvelope(1);
  AddOutput(&envelope, 1, 1);
  envelope.operands.push_back({"relational_values_row_v1", "1", "1"});
  AddNode(&envelope, 1, 13, {}, false, "1");
  return envelope;
}

sbsql::SblrEnvelope ScanEnvelope() {
  auto envelope = BaseEnvelope(1);
  AddOutput(&envelope, 1, 1);
  AddNode(&envelope, 1, 1, {});
  return envelope;
}

sbsql::SblrEnvelope ChainEnvelope(const std::uint32_t depth) {
  auto envelope = BaseEnvelope(depth);
  for (std::uint32_t node_id = 1; node_id <= depth; ++node_id) {
    AddOutput(&envelope, node_id, node_id);
  }
  for (std::uint32_t node_id = 1; node_id <= depth; ++node_id) {
    AddNode(&envelope, node_id, node_id == 1 ? 1 : 3,
            node_id == 1 ? std::vector<std::uint32_t>{}
                         : std::vector<std::uint32_t>{node_id - 1});
  }
  return envelope;
}

sbsql::SblrEnvelope WideEnvelope(const std::uint32_t fanout) {
  const auto root_id = fanout + 1;
  auto envelope = BaseEnvelope(root_id);
  for (std::uint32_t node_id = 1; node_id <= root_id; ++node_id) {
    AddOutput(&envelope, node_id, node_id);
  }
  std::vector<std::uint32_t> inputs;
  inputs.reserve(fanout);
  for (std::uint32_t node_id = 1; node_id <= fanout; ++node_id) {
    AddNode(&envelope, node_id, 1, {});
    inputs.push_back(node_id);
  }
  AddNode(&envelope, root_id, 9, inputs);
  return envelope;
}

sbsql::SblrEnvelope ExpressionChainEnvelope(const std::uint32_t depth) {
  auto envelope = BaseEnvelope(1);
  for (std::uint32_t expression_id = 2; expression_id <= depth;
       ++expression_id) {
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(expression_id),
         "7|" + std::to_string(expression_id - 1) + "|1|-|-|-|-|-"});
  }
  AddOutput(&envelope, 1, 1, depth);
  AddNode(&envelope, 1, 3, {}, false, "-", std::to_string(depth));
  return envelope;
}

sbsql::SblrEnvelope ExpressionFanoutEnvelope(const std::uint32_t fanout) {
  auto envelope = BaseEnvelope(1);
  std::vector<std::uint32_t> children(fanout, 1);
  envelope.operands.push_back(
      {"relational_expression_v1", "2",
       "4|" + JoinHandles(children) +
           "|1|019f0000-0000-7500-8000-000000000410|-|-|-|-"});
  AddOutput(&envelope, 1, 1, 2);
  AddNode(&envelope, 1, 3, {}, false, "-", "2");
  return envelope;
}

sbsql::SblrEnvelope SharedExpressionEnvelope() {
  auto envelope = BaseEnvelope(1);
  envelope.operands.push_back(
      {"relational_expression_v1", "2", "7|1|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "3", "7|1|1|-|-|-|-|-"});
  envelope.operands.push_back(
      {"relational_expression_v1", "4",
       "4|2,3|1|019f0000-0000-7500-8000-000000000411|-|-|-|-"});
  AddOutput(&envelope, 1, 1, 4);
  AddNode(&envelope, 1, 3, {}, false, "-", "4");
  return envelope;
}

sbsql::SblrEnvelope SharedDescendantExpressionDepthEnvelope() {
  auto envelope = BaseEnvelope(1);
  envelope.operands.back().value = "7|2|1|-|-|-|-|-";
  envelope.operands.push_back(
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"});
  for (std::uint32_t expression_id = 3; expression_id <= 256;
       ++expression_id) {
    const auto child_id = expression_id == 3 ? 1 : expression_id - 1;
    envelope.operands.push_back(
        {"relational_expression_v1", std::to_string(expression_id),
         "7|" + std::to_string(child_id) + "|1|-|-|-|-|-"});
  }
  envelope.operands.push_back(
      {"relational_expression_v1", "257",
       "4|1,256|1|019f0000-0000-7500-8000-000000000414|-|-|-|-"});
  AddOutput(&envelope, 1, 1, 257);
  AddNode(&envelope, 1, 3, {}, false, "-", "257");
  return envelope;
}

sbsql::SblrEnvelope SharedEnvelope() {
  auto envelope = BaseEnvelope(4);
  for (std::uint32_t node_id = 1; node_id <= 4; ++node_id) {
    AddOutput(&envelope, node_id, node_id);
  }
  AddNode(&envelope, 1, 1, {}, true);
  AddNode(&envelope, 2, 2, {1});
  AddNode(&envelope, 3, 3, {1});
  AddNode(&envelope, 4, 9, {2, 3});
  return envelope;
}

sbsql::SblrEnvelope SharedDescendantRelationDepthEnvelope() {
  constexpr std::uint32_t kRootNodeId = 257;
  auto envelope = BaseEnvelope(kRootNodeId);
  for (std::uint32_t node_id = 1; node_id <= kRootNodeId; ++node_id) {
    AddOutput(&envelope, node_id, node_id);
  }
  AddNode(&envelope, 1, 3, {2}, true);
  AddNode(&envelope, 2, 1, {});
  for (std::uint32_t node_id = 3; node_id <= 256; ++node_id) {
    AddNode(&envelope, node_id, 3,
            {node_id == 3 ? 1 : node_id - 1});
  }
  AddNode(&envelope, kRootNodeId, 9, {1, 256});
  return envelope;
}

sbsql::SblrOperand* FindOperand(sbsql::SblrEnvelope* envelope,
                                const std::string_view type,
                                const std::string_view name) {
  const auto found = std::ranges::find_if(
      envelope->operands, [&](const auto& operand) {
        return operand.type == type && operand.name == name;
      });
  return found == envelope->operands.end() ? nullptr : &*found;
}

void RemoveOperand(sbsql::SblrEnvelope* envelope,
                   const std::string_view type,
                   const std::string_view name) {
  std::erase_if(envelope->operands, [&](const auto& operand) {
    return operand.type == type && operand.name == name;
  });
}

bool ValidateAcceptedForms() {
  const auto values = sbsql::VerifySblrEnvelope(ValuesEnvelope());
  const auto scan = sbsql::VerifySblrEnvelope(ScanEnvelope());
  const auto shared = sbsql::VerifySblrEnvelope(SharedEnvelope());
  const auto deep = sbsql::VerifySblrEnvelope(ChainEnvelope(256));
  const auto wide = sbsql::VerifySblrEnvelope(WideEnvelope(1024));
  const auto expression_deep =
      sbsql::VerifySblrEnvelope(ExpressionChainEnvelope(256));
  const auto expression_wide =
      sbsql::VerifySblrEnvelope(ExpressionFanoutEnvelope(1024));
  const auto shared_expression =
      sbsql::VerifySblrEnvelope(SharedExpressionEnvelope());

  bool passed = true;
  passed &= Require(values.admitted && values.messages.diagnostics.empty(),
                    "canonical VALUES graph was refused");
  passed &= Require(values.validated_relational_node_count == 1 &&
                        values.validated_relational_expression_count == 1 &&
                        values.validated_relational_record_count == 4,
                    "canonical VALUES verification evidence differs");
  passed &= Require(scan.admitted && scan.validated_relational_node_count == 1,
                    "legal non-VALUES graph was refused");
  passed &= Require(shared.admitted &&
                        shared.validated_relational_node_count == 4 &&
                        shared.maximum_observed_relational_depth == 3,
                    "declared shared relational DAG was refused");
  passed &= Require(deep.admitted &&
                        deep.maximum_observed_relational_depth == 256,
                    "exact relational depth boundary was refused");
  passed &= Require(wide.admitted &&
                        wide.validated_relational_node_count == 1025 &&
                        wide.maximum_observed_relational_depth == 2,
                    "exact relational fanout boundary was refused");
  passed &= Require(expression_deep.admitted &&
                        expression_deep.maximum_observed_expression_depth ==
                            256,
                    "exact expression depth boundary was refused");
  passed &= Require(expression_wide.admitted &&
                        expression_wide.validated_relational_expression_count ==
                            2,
                    "exact expression fanout boundary was refused");
  passed &= Require(shared_expression.admitted &&
                        shared_expression
                                .validated_relational_expression_count == 4 &&
                        shared_expression.maximum_observed_expression_depth ==
                            3,
                    "stable shared-child expression graph was refused");
  return passed;
}

bool ValidateEnvelopeAndRecordRefusals() {
  auto root = ValuesEnvelope();
  root.sblr_opcode = "SBLR_QUERY_PLAN_OPERATION";
  const auto root_result = sbsql::VerifySblrEnvelope(root);

  auto version = ValuesEnvelope();
  FindOperand(&version, "uint16", "relational_wire_version")->value = "3";
  const auto version_result = sbsql::VerifySblrEnvelope(version);

  auto duplicate_version = ValuesEnvelope();
  duplicate_version.operands.push_back(
      {"uint16", "relational_wire_version", "2"});
  const auto duplicate_version_result =
      sbsql::VerifySblrEnvelope(duplicate_version);

  auto pre_correction_v2 = ValuesEnvelope();
  pre_correction_v2.operands.erase(pre_correction_v2.operands.begin() + 4,
                                   pre_correction_v2.operands.begin() + 10);
  const auto pre_correction_v2_result =
      sbsql::VerifySblrEnvelope(pre_correction_v2);

  auto missing_statement = ValuesEnvelope();
  RemoveOperand(&missing_statement, "uuid", "relational_statement_uuid");
  const auto missing_statement_result =
      sbsql::VerifySblrEnvelope(missing_statement);

  auto duplicate_statement = ValuesEnvelope();
  duplicate_statement.operands.push_back(
      *FindOperand(&duplicate_statement, "uuid", "relational_statement_uuid"));
  const auto duplicate_statement_result =
      sbsql::VerifySblrEnvelope(duplicate_statement);

  auto nil_statement = ValuesEnvelope();
  FindOperand(&nil_statement, "uuid", "relational_statement_uuid")->value =
      "00000000-0000-0000-0000-000000000000";
  const auto nil_statement_result =
      sbsql::VerifySblrEnvelope(nil_statement);

  auto swapped_statement_authority = ValuesEnvelope();
  std::swap(swapped_statement_authority.operands[4],
            swapped_statement_authority.operands[5]);
  const auto swapped_statement_authority_result =
      sbsql::VerifySblrEnvelope(swapped_statement_authority);

  auto narrowed_local_transaction = ValuesEnvelope();
  FindOperand(&narrowed_local_transaction, "uint64",
              "relational_local_transaction_id")
      ->type = "uint32";
  const auto narrowed_local_transaction_result =
      sbsql::VerifySblrEnvelope(narrowed_local_transaction);

  auto malformed_highwater = ValuesEnvelope();
  FindOperand(
      &malformed_highwater, "uint64",
      "relational_snapshot_visible_through_local_transaction_id")
      ->value = "01";
  const auto malformed_highwater_result =
      sbsql::VerifySblrEnvelope(malformed_highwater);

  auto scope = ValuesEnvelope();
  FindOperand(&scope, "uuid", "relational_catalog_epoch_uuid")->value =
      "NOT-A-CANONICAL-UUID";
  const auto scope_result = sbsql::VerifySblrEnvelope(scope);

  auto unknown = ValuesEnvelope();
  unknown.operands.push_back({"text", "silent_default", "forbidden"});
  const auto unknown_result = sbsql::VerifySblrEnvelope(unknown);

  auto malformed = ValuesEnvelope();
  FindOperand(&malformed, "relational_expression_v1", "1")->value =
      "1|-|1|-|-|1|-|not_hex";
  const auto malformed_result = sbsql::VerifySblrEnvelope(malformed);

  auto duplicate_descriptor = ValuesEnvelope();
  duplicate_descriptor.operands.push_back(
      *FindOperand(&duplicate_descriptor, "relational_descriptor_v1", "1"));
  const auto duplicate_descriptor_result =
      sbsql::VerifySblrEnvelope(duplicate_descriptor);

  auto missing_binding = ValuesEnvelope();
  RemoveOperand(&missing_binding, "relational_node_binding_v1", "1");
  const auto missing_binding_result =
      sbsql::VerifySblrEnvelope(missing_binding);

  auto out_of_order_binding = ValuesEnvelope();
  const auto binding =
      *FindOperand(&out_of_order_binding, "relational_node_binding_v1", "1");
  RemoveOperand(&out_of_order_binding, "relational_node_binding_v1", "1");
  const auto node_position = std::ranges::find_if(
      out_of_order_binding.operands, [](const auto& operand) {
        return operand.type == "relational_node_v1" && operand.name == "1";
      });
  out_of_order_binding.operands.insert(node_position, binding);
  const auto out_of_order_binding_result =
      sbsql::VerifySblrEnvelope(out_of_order_binding);

  auto duplicate_binding = ValuesEnvelope();
  duplicate_binding.operands.push_back(
      *FindOperand(&duplicate_binding, "relational_node_binding_v1", "1"));
  const auto duplicate_binding_result =
      sbsql::VerifySblrEnvelope(duplicate_binding);

  auto missing_root = ValuesEnvelope();
  RemoveOperand(&missing_root, "uint32", "relational_root_node_id");
  const auto missing_root_result = sbsql::VerifySblrEnvelope(missing_root);

  auto duplicate_output = ValuesEnvelope();
  duplicate_output.operands.push_back(
      *FindOperand(&duplicate_output, "relational_output_v1", "1"));
  const auto duplicate_output_result =
      sbsql::VerifySblrEnvelope(duplicate_output);

  auto duplicate_row = ValuesEnvelope();
  duplicate_row.operands.push_back(
      *FindOperand(&duplicate_row, "relational_values_row_v1", "1"));
  const auto duplicate_row_result =
      sbsql::VerifySblrEnvelope(duplicate_row);

  bool passed = true;
  passed &= Require(!root_result.admitted &&
                        HasDiagnostic(root_result,
                                      "QOW-DIAG-RELATIONAL-ROOT-NONCANONICAL",
                                      "package_root"),
                    "noncanonical query root diagnostic differs");
  passed &= Require(!version_result.admitted &&
                        HasDiagnostic(version_result,
                                      "SBLR.PLAN_TREE.INVALID_VERSION",
                                      "wire_version"),
                    "unsupported relational version was accepted");
  passed &= Require(!duplicate_version_result.admitted &&
                        HasDiagnostic(duplicate_version_result,
                                      "SBLR.PLAN_TREE.INVALID_VERSION",
                                      "wire_version"),
                    "duplicate relational version was accepted");
  passed &= Require(
      !pre_correction_v2_result.admitted &&
          HasDiagnostic(pre_correction_v2_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1"),
      "pre-correction wire-v2 statement authority defaulted ambiently");
  passed &= Require(
      !missing_statement_result.admitted &&
          HasDiagnostic(missing_statement_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1"),
      "missing statement identity was accepted");
  passed &= Require(
      !duplicate_statement_result.admitted &&
          HasDiagnostic(duplicate_statement_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1",
                        "statement_uuid"),
      "duplicate statement identity was accepted");
  passed &= Require(
      !nil_statement_result.admitted &&
          HasDiagnostic(nil_statement_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1",
                        "statement_uuid"),
      "nil statement identity was accepted");
  passed &= Require(
      !swapped_statement_authority_result.admitted &&
          HasDiagnostic(swapped_statement_authority_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1"),
      "swapped statement and transaction identities were accepted");
  passed &= Require(
      !narrowed_local_transaction_result.admitted &&
          HasDiagnostic(narrowed_local_transaction_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1"),
      "narrowed local transaction identity was accepted");
  passed &= Require(
      !malformed_highwater_result.admitted &&
          HasDiagnostic(malformed_highwater_result,
                        "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1",
                        "snapshot_visible_through_local_transaction_id"),
      "noncanonical visibility high-water was accepted");
  passed &= Require(!scope_result.admitted &&
                        HasDiagnostic(scope_result,
                                      "QOW-DIAG-LOGICAL-GRAPH-BOUNDARY-V1",
                                      "catalog_epoch_uuid"),
                    "malformed typed planning scope was accepted");
  passed &= Require(!unknown_result.admitted &&
                        HasDiagnostic(unknown_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "unknown_operand"),
                    "unknown query operand was accepted");
  passed &= Require(!malformed_result.admitted &&
                        HasDiagnostic(malformed_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "expression_record"),
                    "malformed typed scalar record was accepted");
  passed &= Require(!duplicate_descriptor_result.admitted &&
                        HasDiagnostic(duplicate_descriptor_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "descriptor_record"),
                    "duplicate descriptor identity was accepted");
  passed &= Require(!missing_binding_result.admitted &&
                        HasDiagnostic(missing_binding_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "node_id_or_kind"),
                    "node without semantic binding was accepted");
  passed &= Require(!out_of_order_binding_result.admitted &&
                        HasDiagnostic(out_of_order_binding_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "node_binding_record"),
                    "out-of-order node binding was accepted");
  passed &= Require(!duplicate_binding_result.admitted &&
                        HasDiagnostic(duplicate_binding_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "node_binding_record"),
                    "duplicate node binding was accepted");
  passed &= Require(!missing_root_result.admitted &&
                        HasDiagnostic(missing_root_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "root_node_id"),
                    "missing relational root was accepted");
  passed &= Require(!duplicate_output_result.admitted &&
                        HasDiagnostic(duplicate_output_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "output_record"),
                    "duplicate output handle was accepted");
  passed &= Require(!duplicate_row_result.admitted &&
                        HasDiagnostic(duplicate_row_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "values_row_record"),
                    "duplicate VALUES row handle was accepted");
  return passed;
}

bool ValidateGraphRefusals() {
  auto dangling_node = ChainEnvelope(2);
  FindOperand(&dangling_node, "relational_node_v1", "2")->value =
      "3|0|99|1|-";
  const auto dangling_node_result =
      sbsql::VerifySblrEnvelope(dangling_node);

  auto cycle = ChainEnvelope(2);
  FindOperand(&cycle, "relational_node_v1", "1")->value = "1|0|2|1|-";
  const auto cycle_result = sbsql::VerifySblrEnvelope(cycle);

  auto undeclared_share = SharedEnvelope();
  FindOperand(&undeclared_share, "relational_node_v1", "1")->value =
      "1|0|-|1|-";
  const auto undeclared_share_result =
      sbsql::VerifySblrEnvelope(undeclared_share);

  auto duplicate_input = ChainEnvelope(2);
  FindOperand(&duplicate_input, "relational_node_v1", "2")->value =
      "3|0|1,1|1|-";
  const auto duplicate_input_result =
      sbsql::VerifySblrEnvelope(duplicate_input);

  auto orphan_node = ScanEnvelope();
  AddOutput(&orphan_node, 2, 2);
  AddNode(&orphan_node, 2, 1, {});
  const auto orphan_node_result = sbsql::VerifySblrEnvelope(orphan_node);

  auto expression_cycle = ScanEnvelope();
  FindOperand(&expression_cycle, "relational_expression_v1", "1")->value =
      "5|1|1|-|-|-|2d|-";
  const auto expression_cycle_result =
      sbsql::VerifySblrEnvelope(expression_cycle);

  auto orphan_expression = ScanEnvelope();
  orphan_expression.operands.push_back(
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"});
  const auto orphan_expression_result =
      sbsql::VerifySblrEnvelope(orphan_expression);

  auto orphan_descriptor = ScanEnvelope();
  orphan_descriptor.operands.push_back(
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7200-8000-000000000405|"
       "019f0000-0000-7300-8000-000000000406|1|-|-|-|-|-"});
  const auto orphan_descriptor_result =
      sbsql::VerifySblrEnvelope(orphan_descriptor);

  auto mismatch = ScanEnvelope();
  mismatch.operands.push_back(
      {"relational_descriptor_v1", "2",
       "019f0000-0000-7200-8000-000000000407|"
       "019f0000-0000-7300-8000-000000000408|1|-|-|-|-|-"});
  FindOperand(&mismatch, "relational_output_v1", "1")->value =
      "1|1|2|1|0|636f6c756d6e5f31";
  FindOperand(&mismatch, "relational_node_v1", "1")->value = "1|0|-|2|-";
  const auto mismatch_result = sbsql::VerifySblrEnvelope(mismatch);

  auto dangling_output = ScanEnvelope();
  FindOperand(&dangling_output, "relational_output_v1", "1")->value =
      "99|1|1|1|0|636f6c756d6e5f31";
  const auto dangling_output_result =
      sbsql::VerifySblrEnvelope(dangling_output);

  auto property_dependency = ScanEnvelope();
  property_dependency.operands.push_back(
      {"relational_property_v1",
       "019f0000-0000-7400-8000-000000000412",
       "1|1|-|-|019f0000-0000-7400-8000-000000000413|-"});
  const auto property_dependency_result =
      sbsql::VerifySblrEnvelope(property_dependency);

  auto grouping_owner = ScanEnvelope();
  grouping_owner.operands.push_back(
      {"relational_grouping_set_v1", "0", "1|1"});
  const auto grouping_owner_result =
      sbsql::VerifySblrEnvelope(grouping_owner);

  auto grouping_member = ScanEnvelope();
  FindOperand(&grouping_member, "relational_node_v1", "1")->value =
      "5|0|-|1|-";
  grouping_member.operands.push_back(
      {"relational_grouping_set_v1", "0", "1|99"});
  const auto grouping_member_result =
      sbsql::VerifySblrEnvelope(grouping_member);

  auto grouping_order = ScanEnvelope();
  grouping_order.operands.push_back(
      {"relational_expression_v1", "2", "1|-|1|-|-|1|-|32"});
  FindOperand(&grouping_order, "relational_node_v1", "1")->value =
      "5|0|-|1|-";
  FindOperand(&grouping_order, "relational_node_binding_v1", "1")->value =
      Hex("relational.contract-node.v1") + "|1,2|-|-|-";
  grouping_order.operands.push_back(
      {"relational_grouping_set_v1", "0", "1|2,1"});
  const auto grouping_order_result =
      sbsql::VerifySblrEnvelope(grouping_order);

  bool passed = true;
  passed &= Require(!dangling_node_result.admitted &&
                        HasDiagnostic(dangling_node_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "input_node_ids"),
                    "dangling relational edge was accepted");
  passed &= Require(!cycle_result.admitted &&
                        HasDiagnostic(cycle_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "cycle"),
                    "relational cycle was accepted");
  passed &= Require(!undeclared_share_result.admitted &&
                        HasDiagnostic(undeclared_share_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "shareable"),
                    "undeclared shared relational node was accepted");
  passed &= Require(!duplicate_input_result.admitted &&
                        HasDiagnostic(duplicate_input_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "input_node_ids"),
                    "duplicate input handle was interpreted as sharing");
  passed &= Require(!orphan_node_result.admitted &&
                        HasDiagnostic(orphan_node_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "orphan_node"),
                    "orphan relational node was accepted");
  passed &= Require(!expression_cycle_result.admitted &&
                        HasDiagnostic(expression_cycle_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "expression_cycle"),
                    "scalar expression cycle was accepted");
  passed &= Require(!orphan_expression_result.admitted &&
                        HasDiagnostic(orphan_expression_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "orphan_expression"),
                    "orphan scalar expression was accepted");
  passed &= Require(!orphan_descriptor_result.admitted &&
                        HasDiagnostic(orphan_descriptor_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "orphan_descriptor"),
                    "orphan descriptor was accepted");
  passed &= Require(!mismatch_result.admitted &&
                        HasDiagnostic(mismatch_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "output_record"),
                    "expression/output descriptor mismatch was accepted");
  passed &= Require(!dangling_output_result.admitted &&
                        HasDiagnostic(dangling_output_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "output_record"),
                    "output owned by a dangling node was accepted");
  passed &= Require(
      !property_dependency_result.admitted &&
          HasDiagnostic(property_dependency_result,
                        "QOW-DIAG-LOGICAL-PROPERTY-DEPENDENCY-V1",
                        "unknown_property_dependency"),
      "dangling logical-property dependency was accepted");
  passed &= Require(!grouping_owner_result.admitted &&
                        HasDiagnostic(grouping_owner_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "grouping_set_record"),
                    "grouping set on a non-aggregate node was accepted");
  passed &= Require(!grouping_member_result.admitted &&
                        HasDiagnostic(grouping_member_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "grouping_set_expression_ids"),
                    "dangling grouping-set member was accepted");
  passed &= Require(!grouping_order_result.admitted &&
                        HasDiagnostic(grouping_order_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "grouping_set_expression_order"),
                    "noncanonical grouping-set expression order was accepted");
  return passed;
}

bool ValidateTypedAndLimitRefusals() {
  auto nullability = ValuesEnvelope();
  FindOperand(&nullability, "relational_descriptor_v1", "1")->value =
      std::string(kDescriptorUuid) + "|" + std::string(kTypeUuid) +
      "|0|-|-|-|-|-";
  const auto nullability_result = sbsql::VerifySblrEnvelope(nullability);

  auto node_kind = ValuesEnvelope();
  FindOperand(&node_kind, "relational_node_v1", "1")->value =
      "18|0|-|1|1";
  const auto node_kind_result = sbsql::VerifySblrEnvelope(node_kind);

  auto property = ScanEnvelope();
  property.operands.push_back(
      {"relational_property_v1",
       "019f0000-0000-7400-8000-000000000409", "0|1|-|-|-|-"});
  const auto property_result = sbsql::VerifySblrEnvelope(property);

  const auto depth_result = sbsql::VerifySblrEnvelope(ChainEnvelope(257));
  const auto fanout_result = sbsql::VerifySblrEnvelope(WideEnvelope(1025));
  const auto expression_depth_result =
      sbsql::VerifySblrEnvelope(ExpressionChainEnvelope(257));
  const auto expression_fanout_result =
      sbsql::VerifySblrEnvelope(ExpressionFanoutEnvelope(1025));
  const auto shared_relation_depth_result = sbsql::VerifySblrEnvelope(
      SharedDescendantRelationDepthEnvelope());
  const auto shared_expression_depth_result = sbsql::VerifySblrEnvelope(
      SharedDescendantExpressionDepthEnvelope());

  auto operand_bytes = ValuesEnvelope();
  operand_bytes.operands.push_back(
      {"text", "oversized_operand", std::string(65537, 'x')});
  const auto operand_bytes_result =
      sbsql::VerifySblrEnvelope(operand_bytes);

  auto authority = ValuesEnvelope();
  authority.parser_executes_sql = true;
  const auto authority_result = sbsql::VerifySblrEnvelope(authority);

  bool passed = true;
  passed &= Require(!nullability_result.admitted &&
                        HasDiagnostic(nullability_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "descriptor_record"),
                    "invalid descriptor nullability was accepted");
  passed &= Require(!node_kind_result.admitted &&
                        HasDiagnostic(node_kind_result,
                                      "SBLR.PLAN_TREE.INVALID_HANDLE",
                                      "node_id_or_kind"),
                    "unknown relational node kind was accepted");
  passed &= Require(!property_result.admitted &&
                        HasDiagnostic(property_result,
                                      "QOW-DIAG-LOGICAL-PROPERTY-IDENTITY-V1",
                                      "property_record"),
                    "invalid logical property identity was accepted");
  passed &= Require(!depth_result.admitted &&
                        HasDiagnostic(depth_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "maximum_depth"),
                    "relational depth overrun was accepted");
  passed &= Require(!fanout_result.admitted &&
                        HasDiagnostic(fanout_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "node_fanout"),
                    "relational fanout overrun was accepted");
  passed &= Require(!expression_depth_result.admitted &&
                        HasDiagnostic(expression_depth_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "expression_maximum_depth"),
                    "expression depth overrun was accepted");
  passed &= Require(!expression_fanout_result.admitted &&
                        HasDiagnostic(expression_fanout_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "expression_fanout"),
                    "expression fanout overrun was accepted");
  passed &= Require(!shared_relation_depth_result.admitted &&
                        HasDiagnostic(shared_relation_depth_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "maximum_depth"),
                    "shared descendant concealed relational depth overrun");
  passed &= Require(!shared_expression_depth_result.admitted &&
                        HasDiagnostic(shared_expression_depth_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "expression_maximum_depth"),
                    "shared child concealed expression depth overrun");
  passed &= Require(!operand_bytes_result.admitted &&
                        HasDiagnostic(operand_bytes_result,
                                      "SBLR.PLAN_TREE.RESOURCE_LIMIT",
                                      "operand_bytes"),
                    "relational operand byte overrun was accepted");
  passed &= Require(!authority_result.admitted &&
                        HasDiagnostic(
                            authority_result,
                            "SBSQL.SBLR.QUERY_EXECUTE_AUTHORITY_INVALID"),
                    "parser-side query authority bypass was accepted");
  return passed;
}

}  // namespace

// QOW-CTEST-SBLR-VERIFIER-V1
int main() {
  bool passed = true;
  passed &= ValidateAcceptedForms();
  passed &= ValidateEnvelopeAndRecordRefusals();
  passed &= ValidateGraphRefusals();
  passed &= ValidateTypedAndLimitRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
