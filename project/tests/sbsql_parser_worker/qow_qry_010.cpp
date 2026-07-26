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
    "019f0000-0000-7400-8000-000000001001";

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) std::cerr << "QOW-TEST-QRY-010-V1: " << detail << '\n';
  return condition;
}

api::EngineDescriptor Descriptor(const std::string& descriptor_uuid,
                                 const std::string& canonical_type,
                                 const std::string& encoded_descriptor) {
  api::EngineDescriptor descriptor;
  descriptor.descriptor_uuid.canonical = descriptor_uuid;
  descriptor.descriptor_kind = "scalar";
  descriptor.canonical_type_name = canonical_type;
  descriptor.encoded_descriptor = encoded_descriptor;
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

api::EngineTypedValue Null(const api::EngineDescriptor& descriptor) {
  api::EngineTypedValue value;
  value.descriptor = descriptor;
  value.is_null = true;
  value.state = api::EngineValueState::sql_null;
  return value;
}

dt::DatatypeTextSeedAuthority CollationSeed() {
  dt::DatatypeTextSeedAuthority seed;
  seed.active = true;
  seed.seed_pack_name = "qow.seed";
  seed.seed_pack_version = "1";
  seed.charset_name = "utf8";
  seed.collation_name = "qow_ci";
  seed.collation_case_insensitive = true;
  return seed;
}

exec::CanonicalDescriptorSortRequest Request() {
  const auto text_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001002", "text",
      "type_uuid=019f0000-0000-7300-8000-000000001003;"
      "nullability=non_null;collation_uuid=" +
          std::string(kCollationUuid));
  const auto decimal_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001004", "decimal",
      "type_uuid=019f0000-0000-7300-8000-000000001005;"
      "nullability=nullable;precision=12;scale=2");
  const auto int64_descriptor = Descriptor(
      "019f0000-0000-7200-8000-000000001006", "int64",
      "type_uuid=019f0000-0000-7300-8000-000000001007;"
      "nullability=non_null");

  exec::CanonicalDescriptorSortRequest request;
  request.physical_dag.selected_plan_uuid =
      "019f0000-0000-7200-8000-000000001008";
  request.physical_dag.root_physical_node_id = 1012;
  request.physical_dag.local_transaction_id = 1013;
  request.physical_dag.statement_snapshot_id = 1014;
  request.physical_dag.admission_evidence = {
      {exec::PhysicalAdmissionStage::kBoundRequest,
       "019f0000-0000-7200-8000-000000001011"},
      {exec::PhysicalAdmissionStage::kCatalogEpoch,
       "019f0000-0000-7200-8000-000000001012"},
      {exec::PhysicalAdmissionStage::kSecurity,
       "019f0000-0000-7200-8000-000000001013"},
      {exec::PhysicalAdmissionStage::kMgaStatementBoundary,
       "019f0000-0000-7200-8000-000000001014"},
      {exec::PhysicalAdmissionStage::kPolicyCapability,
       "019f0000-0000-7200-8000-000000001015"},
      {exec::PhysicalAdmissionStage::kResource,
       "019f0000-0000-7200-8000-000000001016"},
      {exec::PhysicalAdmissionStage::kStatisticsProvenance,
       "019f0000-0000-7200-8000-000000001017"},
      {exec::PhysicalAdmissionStage::kCanonicalRoute,
       "019f0000-0000-7200-8000-000000001018"},
  };
  request.physical_dag.nodes = {
      {.physical_node_id = 1011,
       .relational_node_id = 1011,
       .node_kind = exec::PhysicalNodeKind::kValues,
       .implementation_id = "values.typed.v1",
       .output_descriptor_ids = {1001, 1002, 1003},
       .causal_counter_id = 10101},
      {.physical_node_id = 1012,
       .relational_node_id = 1012,
       .node_kind = exec::PhysicalNodeKind::kSort,
       .implementation_id = "sort.typed.terms.v1",
       .input_physical_node_ids = {1011},
       .output_descriptor_ids = {1001, 1002, 1003},
       .causal_counter_id = 10102},
  };
  request.selected_physical_node_id = 1012;
  request.input_batch = exec::MakeDescriptorBatch(
      {{"normalized_name", text_descriptor, false, 1001},
       {"amount", decimal_descriptor, true, 1002},
       {"row_id", int64_descriptor, false, 1003}},
      {{{Value(text_descriptor, "b"), Null(decimal_descriptor),
         Value(int64_descriptor, "3")}},
       {{Value(text_descriptor, "A"), Value(decimal_descriptor, "2.00"),
         Value(int64_descriptor, "2")}},
       {{Value(text_descriptor, "a"), Value(decimal_descriptor, "2.00"),
         Value(int64_descriptor, "1")}},
       {{Value(text_descriptor, "A"), Value(decimal_descriptor, "1.00"),
         Value(int64_descriptor, "4")}},
       {{Value(text_descriptor, "b"), Value(decimal_descriptor, "5.00"),
         Value(int64_descriptor, "5")}}});

  exec::CanonicalDescriptorOrderTerm name_term;
  name_term.column = 0;
  name_term.expression_descriptor_id = 1001;
  name_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  name_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;
  name_term.collation_uuid = kCollationUuid;
  name_term.resource_epoch = 41;
  name_term.collation_epoch = 42;
  name_term.text_seed = CollationSeed();

  exec::CanonicalDescriptorOrderTerm amount_term;
  amount_term.column = 1;
  amount_term.expression_descriptor_id = 1002;
  amount_term.direction = exec::CanonicalDescriptorOrderDirection::descending;
  amount_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;

  exec::CanonicalDescriptorOrderTerm id_term;
  id_term.column = 2;
  id_term.expression_descriptor_id = 1003;
  id_term.direction = exec::CanonicalDescriptorOrderDirection::ascending;
  id_term.null_placement = exec::CanonicalDescriptorNullPlacement::last;

  request.order_terms = {name_term, amount_term, id_term};
  request.deterministic_tie_evidence_uuid =
      "019f0000-0000-7200-8000-000000001019";
  return request;
}

std::vector<std::string> RowIds(const exec::DescriptorBatch& batch) {
  std::vector<std::string> ids;
  for (const auto& row : batch.rows) {
    ids.push_back(row.values[2].encoded_value);
  }
  return ids;
}

// QOW-TEST-QRY-010-V1
bool ValidateTypedPhysicalOrdering() {
  bool passed = true;
  auto result = exec::ExecuteCanonicalDescriptorSort(Request());
  passed &= Require(result.diagnostic.ok &&
                        result.executed_physical_node_id == 1012 &&
                        result.causal_counter_id == 10102,
                    "typed physical sort node was not executable");
  passed &= Require(RowIds(result.output_batch) ==
                        std::vector<std::string>({"1", "2", "4", "5", "3"}),
                    "multi-term collation, DESC, or NULLS LAST order is wrong");
  passed &= Require(result.output_batch.columns.size() == 3 &&
                        result.output_batch.columns[0].descriptor_id == 1001 &&
                        result.output_batch.columns[2].descriptor_id == 1003,
                    "sort changed bound output descriptor handles");

  auto request = Request();
  request.order_terms.resize(2);
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(RowIds(result.output_batch) ==
                        std::vector<std::string>({"2", "1", "4", "5", "3"}),
                    "equal order keys did not retain deterministic input order");

  request = Request();
  request.input_batch.rows[1].values[1].encoded_value = "not-a-decimal";
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "malformed numeric operand produced partial sort output");

  request = Request();
  request.order_terms[0].resource_epoch = 0;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "unbound collation resource authority was accepted");

  request = Request();
  request.deterministic_tie_evidence_uuid.clear();
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "missing deterministic tie evidence was accepted");

  request = Request();
  request.order_terms[0].expression_descriptor_id = 1002;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "mismatched expression descriptor handle was accepted");

  request = Request();
  request.maximum_pair_comparisons = 24;
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(!result.diagnostic.ok && result.output_batch.rows.empty(),
                    "comparison resource limit was exceeded");

  request = Request();
  request.input_batch.rows.clear();
  result = exec::ExecuteCanonicalDescriptorSort(request);
  passed &= Require(result.diagnostic.ok && result.output_batch.rows.empty() &&
                        result.output_batch.columns.size() == 3,
                    "empty typed input lost its bound output schema");
  return passed;
}

}  // namespace

int main() {
  return ValidateTypedPhysicalOrdering() ? EXIT_SUCCESS : EXIT_FAILURE;
}
