// Copyright (c) 2026 ScratchBird Software Inc.
// SPDX-License-Identifier: MPL-2.0

// CSC-TEST-002334: exact malformed SBVN/reference/binding contract.
#include "engine/sblr/sblr_variable_runtime.hpp"
#include "hash_digest.hpp"

#include <cstdlib>
#include <iostream>
#include <algorithm>
#include <initializer_list>
#include <string_view>

namespace sblr = scratchbird::engine::sblr;
namespace {
unsigned checks = 0;
void Require(bool value, const char* message) {
  ++checks;
  if (!value) { std::cerr << message << " (check " << checks << ")\n"; std::exit(EXIT_FAILURE); }
}
using Id = sblr::SblrVariableUuidV1;
Id Identity(std::uint8_t suffix) {
  return {0x01,0xa0,0x7c,0x0a,0x0d,0x5c,0x70,0,0x80,0,0xff,0,0,0,0,suffix};
}
template<class T, class Encode>
void CheckUuidFields(const T& source, std::initializer_list<Id T::*> fields, Encode encode) {
  auto valid = source;
  Require(!encode(valid).empty(), "valid UUID fixture did not encode");
  for (const auto member : fields) {
    for (const unsigned offset : {6u, 8u}) for (unsigned octet = 0; octet < 256; ++octet) {
      auto value = source;
      (value.*member)[offset] = static_cast<std::uint8_t>(octet);
      const bool admitted = offset == 6 ? (octet >> 4) == 7 : (octet >> 6) == 2;
      Require(encode(value).empty() != admitted,
              "system UUID encoder admitted invalid version/variant or rejected payload bits");
    }
    auto nil = source; (nil.*member).fill(0);
    Require(encode(nil).empty(), "nil system identity admitted");
  }
}
template<class Decode>
void CheckUuidDecoding(const std::vector<std::uint8_t>& bytes,
                      std::initializer_list<unsigned> offsets, Decode decode) {
  Require(decode(bytes), "valid UUID fixture did not decode");
  for (const auto field : offsets) {
    for (unsigned version = 0; version < 16; ++version) if (version != 7) {
      auto changed = bytes; changed[field + 6] = static_cast<std::uint8_t>(version << 4);
      Require(!decode(changed), "system UUID decoder admitted another UUID version");
    }
    for (unsigned variant = 0; variant < 4; ++variant) if (variant != 2) {
      auto changed = bytes; changed[field + 8] = static_cast<std::uint8_t>(variant << 6);
      Require(!decode(changed), "system UUID decoder admitted another UUID variant");
    }
  }
}
template<class T, class Encode, class Decode>
void CheckUuidFrame(T fixture, std::initializer_list<unsigned> offsets, Encode encode, Decode decode) {
  const auto bytes = encode(fixture);
  CheckUuidDecoding(bytes, offsets, [&](const auto& input) {
    auto output = fixture;
    std::string detail;
    const bool ok = decode(input.data(), input.size(), &output, &detail);
    Require(encode(output) == (ok ? input : bytes),
            "UUID decoding changed bytes or published a rejected destination");
    return ok;
  });
}
void CheckOtherVariableCarriers(const sblr::SblrVariableNodeV1& node) {
  using B = sblr::SblrVariableFrameBeginRequestV1;
  B begin;
  begin.operation_uuid = Identity(41); begin.transaction_uuid = Identity(42);
  begin.expires_after_ns = 1000;
  begin.demands.push_back({1,1,0,1,3,{}});
  begin.demands[0].declaration_token_sha256[0] = 1;
  const auto encode_begin = [](auto& v) { return sblr::EncodeSblrVariableFrameBeginRequestV1(&v); };
  CheckUuidFields(begin, {&B::operation_uuid, &B::transaction_uuid}, encode_begin);
  CheckUuidFrame(begin, {16,32}, encode_begin, sblr::DecodeSblrVariableFrameBeginRequestV1);

  using C = sblr::SblrVariableFrameBeginResultV1;
  C created;
  created.operation_uuid = begin.operation_uuid;
  created.public_coordination_uuid = Identity(43);
  created.scope_uuid = node.scope_uuid; created.frame_uuid = node.frame_uuid;
  created.scope_generation = created.frame_generation = created.registry_generation = created.coordinator_generation = 1;
  sblr::SblrVariableFrameMappingV1 mapping;
  mapping.declaration_occurrence_id = 1; mapping.mutability = 1; mapping.value_state = 3;
  mapping.variable_descriptor_uuid = node.variable_descriptor_uuid;
  mapping.datatype_descriptor_uuid = node.datatype_descriptor_uuid;
  mapping.datatype_type_uuid = Identity(44);
  mapping.variable_descriptor_generation = mapping.datatype_descriptor_generation = mapping.value_generation = 1;
  created.mappings = {mapping};
  const auto encode_created = [](auto& v) { return sblr::EncodeSblrVariableFrameBeginResultV1(&v); };
  CheckUuidFields(created, {&C::public_coordination_uuid, &C::operation_uuid, &C::scope_uuid, &C::frame_uuid}, encode_created);
  using M = sblr::SblrVariableFrameMappingV1;
  CheckUuidFields(mapping, {&M::variable_descriptor_uuid, &M::datatype_descriptor_uuid, &M::datatype_type_uuid}, [&](const auto& m) {
    auto c = created; c.mappings = {m}; return encode_created(c);
  });
  CheckUuidFrame(created, {16,32,48,72,168,192,216}, encode_created, sblr::DecodeSblrVariableFrameBeginResultV1);

  using X = sblr::SblrVariableFrameCloseRequestV1;
  X close{created.public_coordination_uuid, begin.operation_uuid, 1, 0};
  CheckUuidFields(close, {&X::public_coordination_uuid, &X::operation_uuid}, sblr::EncodeSblrVariableFrameCloseRequestV1);
  CheckUuidFrame(close, {16,32}, sblr::EncodeSblrVariableFrameCloseRequestV1, sblr::DecodeSblrVariableFrameCloseRequestV1);
  using Z = sblr::SblrVariableFrameCloseResultV1;
  Z closed{created.public_coordination_uuid, 2, 3};
  CheckUuidFields(closed, {&Z::public_coordination_uuid}, sblr::EncodeSblrVariableFrameCloseResultV1);
  CheckUuidFrame(closed, {16}, sblr::EncodeSblrVariableFrameCloseResultV1, sblr::DecodeSblrVariableFrameCloseResultV1);

  using Q = sblr::SblrVariableNegotiateRequestV1;
  Q negotiate;
  negotiate.preliminary_receipt_uuid = Identity(45);
  negotiate.scope_uuid = node.scope_uuid; negotiate.frame_uuid = node.frame_uuid;
  negotiate.scope_generation = negotiate.frame_generation = 1;
  negotiate.demands.push_back({1,1,0,node.scope_uuid});
  const auto encode_negotiate = [](auto& v) {
    // Scope is repeated by the demand. Change both copies when varying a
    // valid identity so this tests UUID admission, not a scope mismatch.
    for (auto& demand : v.demands) demand.scope_uuid = v.scope_uuid;
    return sblr::EncodeSblrVariableNegotiateRequestV1(&v);
  };
  CheckUuidFields(negotiate, {&Q::preliminary_receipt_uuid, &Q::scope_uuid, &Q::frame_uuid}, encode_negotiate);
  CheckUuidFrame(negotiate, {16,32,56,144}, encode_negotiate, sblr::DecodeSblrVariableNegotiateRequestV1);
  using G = sblr::SblrVariableNegotiateResultV1;
  G negotiated;
  negotiated.preliminary_receipt_uuid = negotiate.preliminary_receipt_uuid;
  negotiated.scope_uuid = node.scope_uuid; negotiated.frame_uuid = node.frame_uuid;
  negotiated.scope_generation = negotiated.frame_generation = negotiated.registry_generation = 1;
  negotiated.registry_snapshot_uuid = Identity(46);
  sblr::SblrVariableMappingV1 m;
  m.occurrence_id = 1; m.mutability = 1; m.value_state = 3;
  m.variable_descriptor_uuid = node.variable_descriptor_uuid;
  m.datatype_descriptor_uuid = node.datatype_descriptor_uuid;
  m.datatype_type_uuid = mapping.datatype_type_uuid;
  m.variable_descriptor_generation = m.datatype_descriptor_generation = m.value_generation = 1;
  negotiated.mappings = {m};
  const auto encode_negotiated = [](auto& v) { return sblr::EncodeSblrVariableNegotiateResultV1(&v); };
  CheckUuidFields(negotiated, {&G::preliminary_receipt_uuid, &G::scope_uuid, &G::frame_uuid, &G::registry_snapshot_uuid}, encode_negotiated);
  using K = sblr::SblrVariableMappingV1;
  CheckUuidFields(m, {&K::variable_descriptor_uuid, &K::datatype_descriptor_uuid, &K::datatype_type_uuid}, [&](const auto& row) {
    auto g = negotiated; g.mappings = {row}; return encode_negotiated(g);
  });
  CheckUuidFrame(negotiated, {16,32,56,120,160,184,208}, encode_negotiated, sblr::DecodeSblrVariableNegotiateResultV1);

  using F = sblr::SblrVariableFinalizeRequestV1;
  F finalize;
  finalize.preliminary_receipt_uuid = negotiate.preliminary_receipt_uuid;
  finalize.scope_uuid = node.scope_uuid; finalize.frame_uuid = node.frame_uuid;
  finalize.scope_generation = finalize.frame_generation = finalize.registry_generation = 1;
  finalize.canonical_sbvn = sblr::EncodeSblrVariableNodeTableV1({{node}});
  constexpr std::string_view domain = "ScratchBird.SblrVariableNodeTable.V1";
  std::vector<std::uint8_t> hash_input(domain.begin(), domain.end());
  hash_input.insert(hash_input.end(), finalize.canonical_sbvn.begin(), finalize.canonical_sbvn.end());
  const auto digest = scratchbird::core::hash::ComputeSha256Digest(hash_input);
  Require(digest.ok(), "finalize fixture hash failed");
  finalize.sbvn_sha256 = digest.digest;
  CheckUuidFields(finalize, {&F::preliminary_receipt_uuid, &F::scope_uuid, &F::frame_uuid}, sblr::EncodeSblrVariableFinalizeRequestV1);
  CheckUuidFrame(finalize, {16,32,56}, sblr::EncodeSblrVariableFinalizeRequestV1, sblr::DecodeSblrVariableFinalizeRequestV1);
  using A = sblr::SblrVariableAdmissionV1;
  A admitted;
  admitted.final_receipt_uuid = Identity(47); admitted.admission_token_uuid = Identity(48);
  admitted.scope_uuid = node.scope_uuid; admitted.frame_uuid = node.frame_uuid;
  admitted.registry_snapshot_uuid = negotiated.registry_snapshot_uuid;
  admitted.scope_generation = admitted.frame_generation = admitted.registry_generation = admitted.executor_availability_generation = 1;
  admitted.expires_at_monotonic_ns = 1000;
  const auto encode_admitted = [](auto& v) { return sblr::EncodeSblrVariableAdmissionV1(&v); };
  CheckUuidFields(admitted, {&A::final_receipt_uuid, &A::admission_token_uuid, &A::scope_uuid, &A::frame_uuid, &A::registry_snapshot_uuid}, encode_admitted);
  CheckUuidFrame(admitted, {16,32,48,72,96}, encode_admitted, sblr::DecodeSblrVariableAdmissionV1);
}
}

int main() {
  sblr::SblrVariableNodeV1 node;
  node.node_id = 7;
  node.parent_operand_ordinal = 1;
  node.scope_uuid = Identity(1);
  node.scope_generation = 2;
  node.frame_uuid = Identity(3);
  node.frame_generation = 4;
  node.variable_descriptor_uuid = Identity(5);
  node.variable_descriptor_generation = 6;
  node.datatype_descriptor_uuid = Identity(7);
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
  binding.execution_uuid = Identity(11);
  binding.statement_receipt_uuid = Identity(12);
  binding.variable_final_receipt_uuid = Identity(13);
  binding.admission_token_uuid = Identity(14);
  binding.scope_uuid = node.scope_uuid;
  binding.scope_generation = node.scope_generation;
  binding.frame_uuid = node.frame_uuid;
  binding.frame_generation = node.frame_generation;
  binding.registry_snapshot_uuid = Identity(15);
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
  assignment.preliminary_receipt_uuid=Identity(21);
  assignment.public_coordination_uuid=Identity(22);
  assignment.operation_uuid=Identity(23);
  assignment.scope_uuid=node.scope_uuid;assignment.scope_generation=2;
  assignment.frame_uuid=node.frame_uuid;assignment.frame_generation=4;
  assignment.registry_snapshot_uuid=Identity(25);assignment.registry_generation=6;
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
  using N = sblr::SblrVariableNodeV1;
  CheckUuidFields(node, {&N::scope_uuid, &N::frame_uuid, &N::variable_descriptor_uuid,
                        &N::datatype_descriptor_uuid}, [](const auto& n) {
    return sblr::EncodeSblrVariableNodeTableV1({{n}});
  });
  CheckUuidDecoding(sbvn, {48,72,96,120}, [](const auto& b) {
    return sblr::DecodeSblrVariableNodeTableV1(b.data(), b.size()).ok;
  });
  using R = sblr::SblrVariableNodeReferenceV1;
  CheckUuidFields(reference, {&R::scope_uuid, &R::frame_uuid, &R::variable_descriptor_uuid},
                  sblr::EncodeSblrVariableNodeReferenceV1);
  CheckUuidDecoding(encoded_reference, {48,72,96}, [&](const auto& b) {
    auto output = reference;
    const bool ok = sblr::DecodeSblrVariableNodeReferenceV1(b.data(), b.size(), &output);
    if (!ok) Require(sblr::EncodeSblrVariableNodeReferenceV1(output) == encoded_reference,
                     "reference refusal changed destination");
    return ok;
  });
  using E = sblr::SblrVariableExecutionBindingV1;
  CheckUuidFields(binding, {&E::execution_uuid, &E::statement_receipt_uuid,
      &E::variable_final_receipt_uuid, &E::admission_token_uuid, &E::scope_uuid,
      &E::frame_uuid, &E::registry_snapshot_uuid}, sblr::EncodeSblrVariableExecutionBindingV1);
  CheckUuidDecoding(sbve, {16,32,48,64,80,104,128}, [&](const auto& b) {
    auto output = binding;
    const bool ok = sblr::DecodeSblrVariableExecutionBindingV1(b.data(), b.size(), &output, &detail);
    if (!ok) Require(sblr::EncodeSblrVariableExecutionBindingV1(output) == sbve,
                     "execution binding refusal changed destination");
    return ok;
  });
  using A = sblr::SblrVariableAssignmentRequestV1;
  const auto encode_assignment = [](auto& a) { return sblr::EncodeSblrVariableAssignmentRequestV1(&a); };
  CheckUuidFields(assignment, {&A::preliminary_receipt_uuid, &A::public_coordination_uuid,
      &A::operation_uuid, &A::scope_uuid, &A::frame_uuid, &A::registry_snapshot_uuid}, encode_assignment);
  using V = sblr::SblrVariableAssignmentRecordV1;
  CheckUuidFields(value, {&V::variable_descriptor_uuid, &V::datatype_descriptor_uuid}, [&](const auto& v) {
    auto a = assignment; a.assignments = {v}; return encode_assignment(a);
  });
  CheckUuidDecoding(sbvy, {16,32,48,64,88,112,192,224}, [&](const auto& b) {
    auto output = assignment;
    const bool ok = sblr::DecodeSblrVariableAssignmentRequestV1(b.data(), b.size(), &output, &detail);
    if (!ok) Require(encode_assignment(output) == sbvy, "assignment refusal changed destination");
    return ok;
  });
  using W = sblr::SblrVariableAssignmentResultV1;
  CheckUuidFields(assignment_result, {&W::preliminary_receipt_uuid, &W::public_coordination_uuid,
      &W::scope_uuid, &W::frame_uuid}, [](auto& v) { return sblr::EncodeSblrVariableAssignmentResultV1(&v); });
  using O = sblr::SblrVariableAssignmentResultRecordV1;
  CheckUuidFields(assignment_result.results[0], {&O::variable_descriptor_uuid}, [&](const auto& row) {
    auto result = assignment_result; result.results = {row};
    return sblr::EncodeSblrVariableAssignmentResultV1(&result);
  });
  CheckUuidFrame(assignment_result, {16,32,48,72,168}, [](auto& v) {
    return sblr::EncodeSblrVariableAssignmentResultV1(&v);
  }, sblr::DecodeSblrVariableAssignmentResultV1);
  CheckOtherVariableCarriers(node);
  // UUIDs as user data retain ALL bits, independently of identity-slot rules.
  for (unsigned version = 0; version < 16; ++version) for (unsigned variant = 0; variant < 4; ++variant) {
    auto data = Identity(31);
    data[6] = static_cast<std::uint8_t>(version << 4);
    data[8] = static_cast<std::uint8_t>(variant << 6);
    auto a = assignment;
    a.assignments[0].canonical_value_bytes.assign(data.begin(), data.end());
    const auto bytes = encode_assignment(a);
    A decoded;
    Require(!bytes.empty() && sblr::DecodeSblrVariableAssignmentRequestV1(bytes.data(), bytes.size(), &decoded, &detail) &&
            decoded.assignments[0].canonical_value_bytes == a.assignments[0].canonical_value_bytes,
            "user UUID bytes were normalized or restricted by system identity rules");
  }
  for (const auto octet : {std::uint8_t(0), std::uint8_t(255)}) {
    auto a = assignment; a.assignments[0].canonical_value_bytes.assign(16, octet);
    const auto bytes = encode_assignment(a);
    A decoded;
    Require(!bytes.empty() && sblr::DecodeSblrVariableAssignmentRequestV1(bytes.data(), bytes.size(), &decoded, &detail) &&
            decoded.assignments[0].canonical_value_bytes == a.assignments[0].canonical_value_bytes,
            "nil or maximum UUID user value refused");
  }
  std::cout << "variable runtime checks=" << checks << '\n';
  return EXIT_SUCCESS;
}
