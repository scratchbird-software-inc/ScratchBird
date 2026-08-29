// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/update_datatype_operator_authority_provider.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;
namespace wire = scratchbird::wire;

constexpr std::string_view kDatatypeSnapshotUuid =
    "019d0000-0000-7000-8000-00000000d701";
constexpr std::string_view kBigintDescriptorUuid =
    "019d0000-0000-7000-8000-00000000d711";
constexpr std::string_view kBigintTypeUuid =
    "019d0000-0000-7000-8000-00000000d712";

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) Fail(message);
}

wire::TypedUpdateUuid Uuid(unsigned seed) {
  wire::TypedUpdateUuid value{};
  for (std::size_t index = 0; index < value.size(); ++index) {
    value[index] = static_cast<std::uint8_t>(
        (seed * 37u + static_cast<unsigned>(index) * 19u) & 0xffu);
  }
  value[0] |= 1u;
  value[6] = static_cast<std::uint8_t>((value[6] & 0x0fu) | 0x70u);
  value[8] = static_cast<std::uint8_t>((value[8] & 0x3fu) | 0x80u);
  return value;
}

wire::TypedUpdateUuid ParseUuid(std::string_view text) {
  const auto parsed = scratchbird::core::uuid::ParseUuid(std::string(text));
  Require(parsed.ok(), "test UUID did not parse");
  wire::TypedUpdateUuid value{};
  std::copy(parsed.value.bytes.begin(), parsed.value.bytes.end(), value.begin());
  return value;
}

std::string UuidText(const wire::TypedUpdateUuid& value) {
  scratchbird::core::platform::Uuid uuid{};
  std::copy(value.begin(), value.end(), uuid.bytes.begin());
  return scratchbird::core::uuid::UuidToString(uuid);
}

wire::TypedUpdateHash VectorHash(std::span<const std::uint8_t> bytes) {
  Require(bytes.size() >= 104, "vector extent was too short");
  wire::TypedUpdateHash result{};
  std::copy_n(bytes.begin() + 72, result.size(), result.begin());
  return result;
}

wire::TypedUpdateVectorIdentity Identity(
    unsigned seed, const wire::TypedUpdateDescriptorCarrier& descriptor) {
  wire::TypedUpdateVectorIdentity result;
  result.vector_uuid = Uuid(seed);
  result.vector_generation = seed + 1;
  result.owner_descriptor_uuid = descriptor.descriptor_uuid;
  result.owner_descriptor_generation = descriptor.descriptor_generation;
  return result;
}

api::EngineRequestContext Context(bool recovery = false,
                                  bool consumer = false) {
  api::EngineRequestContext context;
  context.trust_mode = api::EngineTrustMode::server_isolated;
  context.database_path = "/tmp/sb_update_datatype_operator_provider.sdb";
  context.database_uuid.canonical = UuidText(Uuid(1));
  context.transaction_uuid.canonical = UuidText(Uuid(2));
  context.local_transaction_id = 3;
  context.statement_receipt_uuid.canonical = UuidText(Uuid(3));
  context.statement_snapshot_uuid.canonical = UuidText(Uuid(4));
  context.statement_metadata_snapshot_engine_owned = true;
  context.statement_metadata_snapshot_uuid.canonical = UuidText(Uuid(5));
  context.catalog_generation_id = 1;
  context.datatype_catalog_snapshot_uuid.canonical =
      std::string(kDatatypeSnapshotUuid);
  context.datatype_catalog_generation = 1;
  context.datatype_registry_generation = 1;
  context.security_context_present = true;
  context.authorization_context.present = true;
  context.authorization_context.authority_uuid.canonical = UuidText(Uuid(6));
  context.authorization_context.security_context_generation = 1;
  context.trace_tags.push_back(
      recovery ? "private_dml_update_rows_recovery"
               : consumer ? "private_dml_update_rows_consumer"
                          : "private_dml_update_rows_binder");
  return context;
}

struct Fixture {
  wire::TypedUpdateDescriptorCarrier descriptor;
  wire::TypedUpdateAssignmentVector assignments;
  wire::TypedUpdatePredicateVector predicate;
  std::vector<std::uint8_t> dudc;
  std::vector<std::uint8_t> duav;
  std::vector<std::uint8_t> duev;
};

Fixture MakeFixture(bool equality) {
  Fixture fixture;
  auto& descriptor = fixture.descriptor;
  descriptor.descriptor_uuid = Uuid(10);
  descriptor.descriptor_generation = 1;
  descriptor.authenticated_statement_receipt_uuid = ParseUuid(
      Context().statement_receipt_uuid.canonical);
  descriptor.structural_occurrence_id = 11;
  descriptor.operation_uuid = Uuid(12);
  descriptor.operation_generation = 1;
  descriptor.owning_transaction_uuid = ParseUuid(
      Context().transaction_uuid.canonical);
  descriptor.owning_local_transaction_id = Context().local_transaction_id;
  descriptor.statement_snapshot_uuid = ParseUuid(
      Context().statement_snapshot_uuid.canonical);
  descriptor.catalog_snapshot_uuid = ParseUuid(
      Context().statement_metadata_snapshot_uuid.canonical);
  descriptor.catalog_generation = Context().catalog_generation_id;
  descriptor.datatype_registry_generation = 1;
  descriptor.security_context_uuid = Uuid(13);
  descriptor.security_snapshot_uuid = Uuid(14);
  descriptor.security_generation = 1;
  descriptor.target_relation_uuid = Uuid(15);
  descriptor.target_relation_generation = 1;
  descriptor.target_relation_occurrence_uuid = Uuid(16);
  descriptor.target_relation_occurrence_generation = 1;

  fixture.assignments.identity = Identity(20, descriptor);
  wire::TypedUpdateAssignmentRecord assignment;
  assignment.assignment_ordinal = 1;
  assignment.assignment_occurrence_uuid = Uuid(21);
  assignment.assignment_occurrence_generation = 1;
  assignment.target_column_occurrence_uuid = Uuid(22);
  assignment.target_column_occurrence_generation = 1;
  assignment.target_column_uuid = Uuid(23);
  assignment.target_column_generation = 1;
  assignment.value_descriptor_uuid = ParseUuid(kBigintDescriptorUuid);
  assignment.value_descriptor_generation = 1;
  assignment.value_type_uuid = ParseUuid(kBigintTypeUuid);
  assignment.value_type_generation = 1;
  assignment.codec_id = "datatype.int64.le.v1";
  assignment.codec_version = 1;
  assignment.codec_generation = 1;
  assignment.value_state = wire::TypedUpdateValueState::value;
  assignment.canonical_value = {42, 0, 0, 0, 0, 0, 0, 0};
  fixture.assignments.records.push_back(assignment);

  fixture.predicate.identity = Identity(24, descriptor);
  if (equality) {
    wire::TypedUpdatePredicateRecord column;
    column.node_id = 1;
    column.node_occurrence_uuid = Uuid(25);
    column.node_occurrence_generation = 1;
    column.node_kind = wire::TypedUpdatePredicateNodeKind::column_reference;
    column.output_descriptor_uuid = assignment.value_descriptor_uuid;
    column.output_descriptor_generation = assignment.value_descriptor_generation;
    column.output_type_uuid = assignment.value_type_uuid;
    column.output_type_generation = assignment.value_type_generation;
    column.output_codec_id = assignment.codec_id;
    column.output_codec_version = assignment.codec_version;
    column.output_codec_generation = assignment.codec_generation;
    column.referenced_relation_occurrence_uuid =
        descriptor.target_relation_occurrence_uuid;
    column.referenced_relation_occurrence_generation =
        descriptor.target_relation_occurrence_generation;
    column.referenced_column_occurrence_uuid =
        assignment.target_column_occurrence_uuid;
    column.referenced_column_occurrence_generation =
        assignment.target_column_occurrence_generation;
    column.referenced_column_uuid = assignment.target_column_uuid;
    column.referenced_column_generation = assignment.target_column_generation;

    auto literal = column;
    literal.node_id = 2;
    literal.node_occurrence_uuid = Uuid(26);
    literal.node_kind = wire::TypedUpdatePredicateNodeKind::typed_literal;
    literal.referenced_relation_occurrence_uuid = {};
    literal.referenced_relation_occurrence_generation = 0;
    literal.referenced_column_occurrence_uuid = {};
    literal.referenced_column_occurrence_generation = 0;
    literal.referenced_column_uuid = {};
    literal.referenced_column_generation = 0;
    literal.value_state = wire::TypedUpdateValueState::value;
    literal.canonical_value = {42, 0, 0, 0, 0, 0, 0, 0};

    wire::TypedUpdatePredicateRecord comparison;
    comparison.node_id = 3;
    comparison.node_occurrence_uuid = Uuid(27);
    comparison.node_occurrence_generation = 1;
    comparison.node_kind = wire::TypedUpdatePredicateNodeKind::comparison;
    comparison.output_descriptor_uuid = wire::kTypedUpdateBooleanUuid;
    comparison.output_descriptor_generation = 1;
    comparison.output_type_uuid = wire::kTypedUpdateBooleanUuid;
    comparison.output_type_generation = 1;
    comparison.output_codec_id = "datatype.boolean.u8.v1";
    comparison.output_codec_version = 1;
    comparison.output_codec_generation = 1;
    comparison.left_child_node_id = 1;
    comparison.right_child_node_id = 2;
    comparison.operator_uuid = wire::kTypedUpdateEqualOperatorUuid;
    comparison.operator_generation = 1;
    fixture.predicate.records = {column, literal, comparison};
  } else {
    wire::TypedUpdatePredicateRecord predicate;
    predicate.node_id = 1;
    predicate.node_occurrence_uuid = Uuid(25);
    predicate.node_occurrence_generation = 1;
    predicate.node_kind =
        wire::TypedUpdatePredicateNodeKind::canonical_boolean_constant;
    predicate.value_state = wire::TypedUpdateValueState::value;
    predicate.output_descriptor_uuid = wire::kTypedUpdateBooleanUuid;
    predicate.output_descriptor_generation = 1;
    predicate.output_type_uuid = wire::kTypedUpdateBooleanUuid;
    predicate.output_type_generation = 1;
    predicate.output_codec_id = "datatype.boolean.u8.v1";
    predicate.output_codec_version = 1;
    predicate.output_codec_generation = 1;
    predicate.canonical_value = {1};
    fixture.predicate.records.push_back(std::move(predicate));
  }

  wire::TypedUpdateCarrierError error;
  Require(wire::EncodeTypedUpdateAssignmentVector(
              fixture.assignments, &fixture.duav, &error),
          "DUAV fixture encode failed: " + error.field + ":" + error.detail);
  Require(wire::DecodeAndValidateTypedUpdateAssignmentVector(
              fixture.duav, &fixture.assignments, &error),
          "DUAV fixture decode failed: " + error.field + ":" + error.detail);
  descriptor.assignment_vector_uuid = fixture.assignments.identity.vector_uuid;
  descriptor.assignment_vector_generation =
      fixture.assignments.identity.vector_generation;
  descriptor.assignment_count = 1;
  descriptor.assignment_vector_sha256 = VectorHash(fixture.duav);

  Require(wire::EncodeTypedUpdatePredicateVector(
              fixture.predicate, &fixture.duev, &error),
          "DUEV fixture encode failed: " + error.field + ":" + error.detail);
  Require(wire::DecodeAndValidateTypedUpdatePredicateVector(
              fixture.duev, &fixture.predicate, &error),
          "DUEV fixture decode failed: " + error.field + ":" + error.detail);
  descriptor.predicate_expression_uuid = fixture.predicate.identity.vector_uuid;
  descriptor.predicate_expression_generation =
      fixture.predicate.identity.vector_generation;
  descriptor.predicate_root_node_id = fixture.predicate.records.back().node_id;
  descriptor.predicate_node_count =
      static_cast<std::uint32_t>(fixture.predicate.records.size());
  descriptor.predicate_vector_sha256 = VectorHash(fixture.duev);

  auto bind_empty_vector = [&](auto* vector, unsigned seed, auto encoder,
                               wire::TypedUpdateUuid* uuid,
                               std::uint64_t* generation,
                               std::uint32_t* count,
                               wire::TypedUpdateHash* hash) {
    vector->identity = Identity(seed, descriptor);
    std::vector<std::uint8_t> bytes;
    Require(encoder(*vector, &bytes, &error),
            "empty authority vector encode failed: " + error.field + ":" +
                error.detail);
    *uuid = vector->identity.vector_uuid;
    *generation = vector->identity.vector_generation;
    *count = 0;
    *hash = VectorHash(bytes);
  };
  wire::TypedUpdateRowPolicyVector policies;
  wire::TypedUpdateConstraintVector constraints;
  wire::TypedUpdateTriggerVector triggers;
  bind_empty_vector(&policies, 30,
                    wire::EncodeTypedUpdateRowPolicyVector,
                    &descriptor.row_policy_set_uuid,
                    &descriptor.row_policy_set_generation,
                    &descriptor.row_policy_count,
                    &descriptor.row_policy_set_sha256);
  bind_empty_vector(&constraints, 31,
                    wire::EncodeTypedUpdateConstraintVector,
                    &descriptor.constraint_set_uuid,
                    &descriptor.constraint_set_generation,
                    &descriptor.constraint_count,
                    &descriptor.ordered_constraint_set_sha256);
  bind_empty_vector(&triggers, 32,
                    wire::EncodeTypedUpdateTriggerVector,
                    &descriptor.trigger_set_uuid,
                    &descriptor.trigger_set_generation,
                    &descriptor.trigger_count,
                    &descriptor.ordered_trigger_set_sha256);

  descriptor.deterministic_target_order_uuid = Uuid(33);
  descriptor.deterministic_target_order_generation = 1;
  descriptor.resource_budget_uuid = Uuid(34);
  descriptor.resource_budget_generation = 1;
  descriptor.recovery_token_uuid = Uuid(35);
  descriptor.recovery_generation = 1;
  descriptor.executor_availability_generation = 1;
  descriptor.builtin_operator_snapshot_uuid =
      wire::kTypedUpdateOperatorSnapshotUuid;
  descriptor.builtin_operator_registry_generation = 1;

  Require(wire::EncodeTypedUpdateDescriptor(descriptor, &fixture.dudc,
                                            &error),
          "DUDC fixture encode failed: " + error.field + ":" + error.detail);
  Require(wire::DecodeAndValidateTypedUpdateDescriptor(
              fixture.dudc, &fixture.descriptor, &error),
          "DUDC fixture decode failed: " + error.field + ":" + error.detail);
  return fixture;
}

api::EngineDmlUpdateDatatypeOperatorAuthorityCaptureResultV1 Capture(
    const Fixture& fixture, api::EngineRequestContext context = Context()) {
  api::EngineDmlUpdateDatatypeOperatorAuthorityCaptureRequestV1 request;
  request.context = std::move(context);
  request.authenticated_statement_receipt_uuid =
      request.context.statement_receipt_uuid.canonical;
  request.exact_descriptor_dudc = fixture.dudc;
  request.exact_assignment_vector_duav = fixture.duav;
  request.exact_predicate_vector_duev = fixture.duev;
  return api::CaptureDmlUpdateDatatypeOperatorAuthorityV1(request);
}

void TestPrebindingAndCanonicalTrue() {
  api::EngineDmlUpdateDatatypeOperatorBindingRequestV1 binding;
  binding.context = Context();
  const auto resolved =
      api::ResolveDmlUpdateDatatypeOperatorBindingAuthorityV1(binding);
  if (!resolved.ok) {
    std::cerr << resolved.diagnostic.code << ':'
              << resolved.diagnostic.message_key << ':'
              << resolved.diagnostic.detail << '\n';
  }
  Require(resolved.ok && resolved.datatype_snapshot_uuid == kDatatypeSnapshotUuid &&
              resolved.boolean_descriptor_uuid ==
                  UuidText(wire::kTypedUpdateBooleanUuid) &&
              resolved.boolean_type_uuid ==
                  UuidText(wire::kTypedUpdateBooleanUuid) &&
              resolved.boolean_codec_id == "datatype.boolean.u8.v1" &&
              resolved.builtin_operator_snapshot_uuid ==
                  UuidText(wire::kTypedUpdateOperatorSnapshotUuid) &&
              resolved.equality_operator_uuid.empty(),
          "pre-carrier boolean/operator authority was not live-registry issued");

  const auto fixture = MakeFixture(false);
  const auto captured = Capture(fixture);
  Require(captured.ok && captured.datatype_snapshot_handle.valid() &&
              captured.operator_snapshot_handle.valid() &&
              captured.datatypes.records.size() == 2 &&
              captured.operators.records.empty() &&
              !captured.exact_datatype_authority_dudv.empty() &&
              !captured.exact_builtin_operator_authority_duov.empty(),
          "canonical TRUE datatype authority capture failed");
  Require(!api::RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
               Context(false, true), captured)
               .error,
          "unchanged live datatype/operator handles failed revalidation");

  const auto binder_refused =
      api::RevalidateDmlUpdateDatatypeOperatorAuthorityV1(Context(), captured);
  Require(binder_refused.error &&
              binder_refused.code == "SECURITY.ACCESS_DENIED",
          "binder capability was accepted for datatype revalidation");

  auto cross_receipt = Context(false, true);
  cross_receipt.statement_receipt_uuid.canonical = UuidText(Uuid(90));
  const auto refused = api::RevalidateDmlUpdateDatatypeOperatorAuthorityV1(
      cross_receipt, captured);
  Require(refused.error && refused.code == "MGA.TRANSACTION.STALE",
          "cross-receipt datatype authority handle was accepted");

  auto recovery_context = Context(true);
  Require(!api::RevalidateRecoveredDmlUpdateDatatypeOperatorAuthorityV1(
               recovery_context, fixture.descriptor, fixture.assignments,
               fixture.predicate, captured.datatypes, captured.operators)
               .error,
          "byte-identical restart datatype/operator authority was refused");
  auto tampered = captured.datatypes;
  tampered.exact_bytes.back() ^= 0x80;
  const auto tamper_refused =
      api::RevalidateRecoveredDmlUpdateDatatypeOperatorAuthorityV1(
          recovery_context, fixture.descriptor, fixture.assignments,
          fixture.predicate, tampered, captured.operators);
  Require(tamper_refused.error && tamper_refused.code == "MGA.TRANSACTION.STALE",
          "tampered recovered DUDV authority was accepted");
}

void TestEqualityBindingAndOperatorAuthority() {
  api::EngineDmlUpdateDatatypeOperatorBindingRequestV1 binding;
  binding.context = Context();
  binding.equality_required = true;
  binding.left_descriptor_uuid = std::string(kBigintDescriptorUuid);
  binding.left_descriptor_generation = 1;
  binding.left_type_uuid = std::string(kBigintTypeUuid);
  binding.left_type_generation = 1;
  binding.right_descriptor_uuid = std::string(kBigintDescriptorUuid);
  binding.right_descriptor_generation = 1;
  binding.right_type_uuid = std::string(kBigintTypeUuid);
  binding.right_type_generation = 1;
  const auto resolved =
      api::ResolveDmlUpdateDatatypeOperatorBindingAuthorityV1(binding);
  Require(resolved.ok &&
              resolved.equality_operator_uuid ==
                  UuidText(wire::kTypedUpdateEqualOperatorUuid) &&
              resolved.equality_operator_generation == 1,
          "live bigint equality authority did not resolve");

  const auto fixture = MakeFixture(true);
  const auto captured = Capture(fixture);
  Require(captured.ok && captured.datatypes.records.size() == 2 &&
              captured.operators.records.size() == 1 &&
              captured.operators.records.front().operator_uuid ==
                  wire::kTypedUpdateEqualOperatorUuid,
          "exact equality DUOV authority capture failed");

  auto stale = Context();
  stale.datatype_registry_generation = 2;
  const auto stale_capture = Capture(fixture, stale);
  Require(!stale_capture.ok && stale_capture.diagnostic.error,
          "stale datatype registry generation was accepted");
}

}  // namespace

int main() {
  TestPrebindingAndCanonicalTrue();
  TestEqualityBindingAndOperatorAuthority();
  std::cout
      << "sbsql_dml_update_datatype_operator_authority_provider_conformance: PASS\n";
  return EXIT_SUCCESS;
}
