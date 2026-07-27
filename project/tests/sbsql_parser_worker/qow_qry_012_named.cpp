// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "descriptor_value_runtime.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace exec = scratchbird::engine::executor;
namespace api = scratchbird::engine::internal_api;
namespace dt = scratchbird::core::datatypes;

namespace {

constexpr std::string_view kCollationUuid =
    "019f0000-0000-7400-8000-000000002701";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-QRY-012-NAMED-V1: " << detail << '\n';
  }
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& type,
                                 const std::string& encoded) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = type;
  descriptor.encoded_descriptor = encoded;
  return descriptor;
}

api::EngineTypedValue Value(const api::EngineDescriptor& descriptor,
                            const std::string& encoded) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.encoded_value = encoded;
  value.state = api::EngineValueState::value;
  return value;
}

dt::DatatypeTextSeedAuthority CollationSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow.named.join.seed";
  seed.seed_pack_version = "1";
  seed.charset_name = "utf8";
  seed.collation_name = "qow_named_join_ci";
  seed.collation_case_insensitive = true;
  return seed;
}

exec::CanonicalCompositeJoinKeyTerm NameTerm() {
  exec::CanonicalCompositeJoinKeyTerm term;
  term.left_column = 1;
  term.left_expression_descriptor_id = 2702;
  term.right_column = 1;
  term.right_expression_descriptor_id = 2705;
  term.collation_uuid = kCollationUuid;
  term.resource_epoch = 71;
  term.collation_epoch = 72;
  term.text_seed = CollationSeed();
  return term;
}

exec::CanonicalNamedJoinRequest Request(
    const exec::CanonicalNamedJoinForm form) {
  const auto left_id = Descriptor(
      "019f0000-0000-7200-8000-000000002711", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002712;"
      "nullability=non_null");
  const auto left_name = Descriptor(
      "019f0000-0000-7200-8000-000000002713", "text",
      "type_uuid=019f0000-0000-7300-8000-000000002714;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto left_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002715", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002716;"
      "nullability=non_null");
  const auto right_id = Descriptor(
      "019f0000-0000-7200-8000-000000002717", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002718;"
      "nullability=non_null");
  const auto right_name = Descriptor(
      "019f0000-0000-7200-8000-000000002719", "text",
      "type_uuid=019f0000-0000-7300-8000-000000002720;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto right_payload = Descriptor(
      "019f0000-0000-7200-8000-000000002721", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002722;"
      "nullability=non_null");
  const auto result_id = Descriptor(
      "019f0000-0000-7200-8000-000000002723", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000002724;"
      "nullability=nullable");
  const auto result_name = Descriptor(
      "019f0000-0000-7200-8000-000000002725", "text",
      "type_uuid=019f0000-0000-7300-8000-000000002726;"
      "nullability=nullable;collation_uuid=" +
          std::string(kCollationUuid));

  exec::CanonicalNamedJoinRequest request;
  auto& key = request.key_request;
  key.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000002727";
  key.physical_dag.root_physical_node_id = 2713;
  key.physical_dag.local_transaction_id = 2715;
  key.physical_dag.statement_snapshot_id = 2716;
  key.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000002731"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000002732"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000002733"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000002734"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000002735"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000002736"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000002737"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000002738"},
  };
  key.physical_dag.nodes = {
      {.physical_node_id = 2711,
       .relational_node_id = 2711,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.named-left.typed.v1",
       .output_descriptor_ids = {2701, 2702, 2703},
       .causal_counter_id = 27101},
      {.physical_node_id = 2712,
       .relational_node_id = 2712,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.named-right.typed.v1",
       .output_descriptor_ids = {2704, 2705, 2706},
       .causal_counter_id = 27102},
      {.physical_node_id = 2713,
       .relational_node_id = 2713,
       .node_kind = exec::PhysicalNodeKind::kJoin,
       .implementation_id = "join.named-key.v1",
       .input_physical_node_ids = {2711, 2712},
       .output_descriptor_ids = {2701, 2702, 2703, 2704, 2705, 2706},
       .causal_counter_id = 27103},
  };
  key.selected_physical_node_id = 2713;
  key.left_batch = exec::MakeDescriptorBatch(
      {{"id", left_id, false, 2701},
       {"name", left_name, false, 2702},
       {"left_payload", left_payload, false, 2703}},
      {{{Value(left_id, "1"), Value(left_name, "A"),
         Value(left_payload, "10")}},
       {{Value(left_id, "2"), Value(left_name, "B"),
         Value(left_payload, "11")}},
       {{Value(left_id, "3"), Value(left_name, "C"),
         Value(left_payload, "12")}}});
  key.right_batch = exec::MakeDescriptorBatch(
      {{"id", right_id, false, 2704},
       {"name", right_name, false, 2705},
       {"right_payload", right_payload, false, 2706}},
      {{{Value(right_id, "1"), Value(right_name, "a"),
         Value(right_payload, "20")}},
       {{Value(right_id, "2"), Value(right_name, "X"),
         Value(right_payload, "21")}},
       {{Value(right_id, "4"), Value(right_name, "C"),
         Value(right_payload, "22")}}});

  const exec::CanonicalCompositeJoinKeyTerm id_term = {
      .left_column = 0,
      .left_expression_descriptor_id = 2701,
      .right_column = 0,
      .right_expression_descriptor_id = 2704,
  };
  const exec::CanonicalNamedJoinBinding id_binding = {
      .normalized_name = "id",
      .key_term = id_term,
      .result_column = {"id", result_id, true, 2707},
  };
  request.form = form;
  request.binding_evidence_uuid =
      "019f0000-0000-7200-8000-000000002739";
  if (form == exec::CanonicalNamedJoinForm::kUsing) {
    key.key_terms = {id_term};
    request.bindings = {id_binding};
  } else {
    const auto name_term = NameTerm();
    key.key_terms = {id_term, name_term};
    request.bindings = {
        id_binding,
        {.normalized_name = "name",
         .key_term = name_term,
         .result_column = {"name", result_name, true, 2708}},
    };
  }

  request.projection_dag = key.physical_dag;
  request.projection_dag.root_physical_node_id = 2714;
  const std::vector<std::uint32_t> projection_ids =
      form == exec::CanonicalNamedJoinForm::kUsing
          ? std::vector<std::uint32_t>{2707, 2702, 2703, 2705, 2706}
          : std::vector<std::uint32_t>{2707, 2708, 2703, 2706};
  request.projection_dag.nodes.push_back(
      {.physical_node_id = 2714,
       .relational_node_id = 2714,
       .node_kind = exec::PhysicalNodeKind::kProject,
       .implementation_id =
           form == exec::CanonicalNamedJoinForm::kUsing
               ? "join.using-projection.v1"
               : "join.natural-projection.v1",
       .input_physical_node_ids = {2713},
       .output_descriptor_ids = projection_ids,
       .causal_counter_id = 27104});
  request.selected_projection_node_id = 2714;
  return request;
}

bool Encoded(const api::EngineTypedValue& value,
             const std::string_view expected) {
  return value.state == api::EngineValueState::value && !value.is_null &&
         value.encoded_value == expected;
}

bool IsNull(const api::EngineTypedValue& value) {
  return value.state == api::EngineValueState::sql_null && value.is_null &&
         value.encoded_value.empty() && value.binary_value.empty();
}

// QOW-TEST-QRY-012-NAMED-V1
bool ValidateNamedJoin() {
  bool passed = true;
  auto result =
      exec::ExecuteCanonicalNamedJoin(Request(exec::CanonicalNamedJoinForm::kUsing));
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalNamedJoinForm::kUsing &&
          result.binding_count == 1 && result.matched_pair_count == 2 &&
          result.output_batch.columns.size() == 5 &&
          result.output_batch.rows.size() == 2 &&
          result.output_batch.columns[0].descriptor_id == 2707 &&
          result.output_batch.columns[1].descriptor_id == 2702 &&
          result.output_batch.columns[3].descriptor_id == 2705 &&
          Encoded(result.output_batch.rows[0].values[0], "1") &&
          Encoded(result.output_batch.rows[0].values[1], "A") &&
          Encoded(result.output_batch.rows[0].values[3], "a") &&
          Encoded(result.output_batch.rows[1].values[0], "2") &&
          Encoded(result.output_batch.rows[1].values[3], "X") &&
          result.executed_join_node_id == 2713 &&
          result.executed_projection_node_id == 2714,
      "USING join did not coalesce its key or retain non-key columns");

  auto request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok && result.matched_pair_count == 2 &&
          result.unmatched_left_row_count == 1 &&
          result.unmatched_right_row_count == 1 &&
          result.output_batch.rows.size() == 4 &&
          Encoded(result.output_batch.rows[2].values[0], "3") &&
          IsNull(result.output_batch.rows[2].values[4]) &&
          Encoded(result.output_batch.rows[3].values[0], "4") &&
          IsNull(result.output_batch.rows[3].values[2]) &&
          Encoded(result.output_batch.rows[3].values[4], "22"),
      "USING FULL OUTER did not coalesce unmatched key values");

  request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(
      result.diagnostic.ok &&
          result.form == exec::CanonicalNamedJoinForm::kNatural &&
          result.binding_count == 2 && result.matched_pair_count == 1 &&
          result.unmatched_left_row_count == 2 &&
          result.unmatched_right_row_count == 2 &&
          result.output_batch.columns.size() == 4 &&
          result.output_batch.rows.size() == 5 &&
          Encoded(result.output_batch.rows[0].values[0], "1") &&
          Encoded(result.output_batch.rows[0].values[1], "A") &&
          Encoded(result.output_batch.rows[0].values[2], "10") &&
          Encoded(result.output_batch.rows[0].values[3], "20") &&
          Encoded(result.output_batch.rows[3].values[0], "2") &&
          Encoded(result.output_batch.rows[3].values[1], "X") &&
          IsNull(result.output_batch.rows[3].values[2]) &&
          Encoded(result.output_batch.rows[3].values[3], "21"),
      "NATURAL FULL OUTER did not bind all common columns in left order");

  request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.bindings.pop_back();
  request.key_request.key_terms.pop_back();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "NATURAL join omitted a common column binding");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.bindings.push_back(request.bindings.front());
  request.key_request.key_terms.push_back(
      request.key_request.key_terms.front());
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "USING join accepted a duplicate named binding");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.binding_evidence_uuid.clear();
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "named join accepted missing binder evidence");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.projection_dag.nodes.back().implementation_id =
      "join.natural-projection.v1";
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "named join accepted projection implementation drift");

  request = Request(exec::CanonicalNamedJoinForm::kNatural);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kFullOuter;
  request.maximum_output_rows = 4;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "named outer join exceeded its output bound");

  request = Request(exec::CanonicalNamedJoinForm::kUsing);
  request.join_kind = exec::CanonicalAcceptedJoinKind::kCross;
  result = exec::ExecuteCanonicalNamedJoin(request);
  passed &= Require(!result.diagnostic.ok,
                    "CROSS join entered the named-key lowering route");
  return passed;
}

}  // namespace

int main() {
  return ValidateNamedJoin() ? EXIT_SUCCESS : EXIT_FAILURE;
}
