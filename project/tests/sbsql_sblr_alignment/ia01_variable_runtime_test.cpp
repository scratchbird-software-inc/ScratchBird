// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// CSC-TEST-002334: exact malformed SBVN/reference/binding contract.
#include "engine/sblr/sblr_variable_runtime.hpp"

#include <cstdlib>
#include <iostream>

namespace sblr = scratchbird::engine::sblr;
namespace {
void Require(bool value, const char* message) {
  if (!value) { std::cerr << message << '\n'; std::exit(EXIT_FAILURE); }
}
}

int main() {
  sblr::SblrVariableNodeV1 node;
  node.node_id = 7;
  node.parent_operand_ordinal = 1;
  node.scope_uuid[0] = 1;
  node.scope_generation = 2;
  node.frame_uuid[0] = 3;
  node.frame_generation = 4;
  node.variable_descriptor_uuid[0] = 5;
  node.variable_descriptor_generation = 6;
  node.datatype_descriptor_uuid[0] = 7;
  node.datatype_descriptor_generation = 8;
  node.value_generation = 9;
  node.value_state_policy = 1;
  sblr::SblrVariableNodeTableV1 table;
  table.nodes.push_back(node);
  const auto sbvn = sblr::EncodeSblrVariableNodeTableV1(table);
  Require(sbvn.size() == 192, "SBVN exact extent");
  const auto decoded = sblr::DecodeSblrVariableNodeTableV1(
      sbvn.data(), sbvn.size());
  Require(decoded.ok && decoded.canonical_bytes == sbvn, "SBVN roundtrip");

  for (const auto offset : {0u, 4u, 6u, 8u, 12u, 16u, 24u, 32u, 152u,
                            172u, 185u, 188u}) {
    auto malformed = sbvn;
    malformed[offset] ^= 1;
    if (sblr::DecodeSblrVariableNodeTableV1(
            malformed.data(), malformed.size()).ok) {
      std::cerr << "malformed SBVN admitted at offset " << offset << '\n';
      return EXIT_FAILURE;
    }
  }
  auto trailing = sbvn;
  trailing.push_back(0);
  Require(!sblr::DecodeSblrVariableNodeTableV1(
               trailing.data(), trailing.size()).ok,
          "SBVN trailing byte admitted");

  sblr::SblrVariableNodeReferenceV1 reference;
  reference.occurrence_ordinal = 1;
  reference.node_id = node.node_id;
  reference.table_sha256[0] = 1;
  reference.scope_uuid = node.scope_uuid;
  reference.scope_generation = node.scope_generation;
  reference.frame_uuid = node.frame_uuid;
  reference.frame_generation = node.frame_generation;
  reference.variable_descriptor_uuid = node.variable_descriptor_uuid;
  reference.variable_descriptor_generation =
      node.variable_descriptor_generation;
  reference.value_generation = node.value_generation;
  const auto encoded_reference =
      sblr::EncodeSblrVariableNodeReferenceV1(reference);
  Require(encoded_reference.size() == 136, "SBVN reference exact extent");
  sblr::SblrVariableNodeReferenceV1 decoded_reference;
  Require(sblr::DecodeSblrVariableNodeReferenceV1(
              encoded_reference.data(), encoded_reference.size(),
              &decoded_reference),
          "SBVN reference roundtrip");
  auto malformed_reference = encoded_reference;
  malformed_reference[128] = 1;
  Require(!sblr::DecodeSblrVariableNodeReferenceV1(
               malformed_reference.data(), malformed_reference.size(),
               &decoded_reference),
          "SBVN reference reserved bytes admitted");

  sblr::SblrVariableExecutionBindingV1 binding;
  binding.execution_uuid[0] = 1;
  binding.statement_receipt_uuid[0] = 2;
  binding.variable_final_receipt_uuid[0] = 3;
  binding.admission_token_uuid[0] = 4;
  binding.scope_uuid = node.scope_uuid;
  binding.scope_generation = node.scope_generation;
  binding.frame_uuid = node.frame_uuid;
  binding.frame_generation = node.frame_generation;
  binding.registry_snapshot_uuid[0] = 5;
  binding.registry_generation = 6;
  binding.executor_availability_generation = 7;
  binding.binding_sha256[0] = 8;
  const auto sbve = sblr::EncodeSblrVariableExecutionBindingV1(binding);
  Require(sbve.size() == 192, "SBVE exact extent");
  sblr::SblrVariableExecutionBindingV1 decoded_binding;
  std::string detail;
  Require(sblr::DecodeSblrVariableExecutionBindingV1(
              sbve.data(), sbve.size(), &decoded_binding, &detail),
          "SBVE roundtrip");
  auto malformed_binding = sbve;
  malformed_binding[12] = 1;
  Require(!sblr::DecodeSblrVariableExecutionBindingV1(
               malformed_binding.data(), malformed_binding.size(),
               &decoded_binding, &detail),
          "SBVE reserved flags admitted");

  sblr::SblrVariableAssignmentRequestV1 assignment;
  assignment.preliminary_receipt_uuid[0]=1;
  assignment.public_coordination_uuid[0]=2;
  assignment.operation_uuid[0]=3;
  assignment.scope_uuid=node.scope_uuid;assignment.scope_generation=2;
  assignment.frame_uuid=node.frame_uuid;assignment.frame_generation=4;
  assignment.registry_snapshot_uuid[0]=5;assignment.registry_generation=6;
  sblr::SblrVariableAssignmentRecordV1 value;
  value.assignment_occurrence_id=1;value.variable_ordinal=0;
  value.variable_descriptor_uuid=node.variable_descriptor_uuid;
  value.variable_descriptor_generation=6;value.expected_value_generation=9;
  value.datatype_descriptor_uuid=node.datatype_descriptor_uuid;
  value.datatype_descriptor_generation=8;value.value_state=1;
  value.canonical_value_bytes={1,0,0,0,0,0,0,0};
  assignment.assignments.push_back(value);
  const auto sbvy=sblr::EncodeSblrVariableAssignmentRequestV1(&assignment);
  Require(sbvy.size()==304,"SBVY exact extent");
  sblr::SblrVariableAssignmentRequestV1 decoded_assignment;
  Require(sblr::DecodeSblrVariableAssignmentRequestV1(sbvy.data(),sbvy.size(),
      &decoded_assignment,&detail),"SBVY roundtrip");
  auto malformed_sbvy=sbvy;malformed_sbvy[140]=119;
  Require(!sblr::DecodeSblrVariableAssignmentRequestV1(malformed_sbvy.data(),
      malformed_sbvy.size(),&decoded_assignment,&detail),"SBVY record size admitted");

  sblr::SblrVariableAssignmentResultV1 assignment_result;
  assignment_result.preliminary_receipt_uuid=assignment.preliminary_receipt_uuid;
  assignment_result.public_coordination_uuid=assignment.public_coordination_uuid;
  assignment_result.scope_uuid=assignment.scope_uuid;assignment_result.scope_generation=2;
  assignment_result.frame_uuid=assignment.frame_uuid;assignment_result.frame_generation=4;
  assignment_result.new_registry_generation=7;
  assignment_result.results.push_back({1,0,value.variable_descriptor_uuid,6,10,7});
  const auto sbvw=sblr::EncodeSblrVariableAssignmentResultV1(&assignment_result);
  Require(sbvw.size()==208,"SBVW exact extent");
  sblr::SblrVariableAssignmentResultV1 decoded_result;
  Require(sblr::DecodeSblrVariableAssignmentResultV1(sbvw.data(),sbvw.size(),
      &decoded_result,&detail),"SBVW roundtrip");
  return EXIT_SUCCESS;
}
