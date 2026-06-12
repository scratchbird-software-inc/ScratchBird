// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "scratchbird/engine/value.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace engine = scratchbird::engine;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

engine::Uuid Uuid(std::uint8_t seed) {
  engine::Uuid uuid;
  for (std::size_t index = 0; index < 16; ++index) {
    uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  return uuid;
}

engine::ExecutionTypeDescriptor Descriptor(std::uint8_t seed,
                                           std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  descriptor.descriptor_uuid = Uuid(seed);
  descriptor.descriptor_epoch = 18;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ExecutionRelationDescriptor TableRelation() {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(0x70);
  relation.descriptor_epoch = 7;
  relation.relation_kind = engine::ExecutionRelationKind::table_value;
  relation.stable_name = "edr018.table.value.relation";
  relation.columns.push_back(
      {0, Descriptor(0x10, "payload"), "payload", "payload", "payload", true});
  relation.snapshot_uuid = Uuid(0x80);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(0x90);
  relation.memory_policy_uuid = Uuid(0xa0);
  relation.memory_policy_epoch = 10;
  return relation;
}

engine::ExecutionTableValue ValidTableValue(
    engine::ExecutionTableValueProducerKind producer_kind) {
  engine::ExecutionTableValue value;
  value.table_value_uuid = Uuid(0xb0);
  value.relation_descriptor = TableRelation();
  value.producer_kind = producer_kind;
  value.producer_uuid = Uuid(0xc0);
  value.owner_transaction_uuid = Uuid(0xd0);
  value.snapshot_uuid = Uuid(0xe0);
  value.security_policy_uuid = Uuid(0xf0);
  return value;
}

void RequireStatus(const engine::ExecutionTableValue& value,
                   engine::ExecutionTableValueStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionTableValue(value);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-018 table value validation status mismatch");
}

void TestValidProducerKinds() {
  const engine::ExecutionTableValueProducerKind kinds[] = {
      engine::ExecutionTableValueProducerKind::sblr_operator,
      engine::ExecutionTableValueProducerKind::routine,
      engine::ExecutionTableValueProducerKind::udr,
      engine::ExecutionTableValueProducerKind::result_channel};
  for (const auto kind : kinds) {
    Require(engine::ValidateExecutionTableValue(ValidTableValue(kind)).ok(),
            "EDR-018 valid table value was rejected");
  }
}

void TestIdentityAndRelationFailures() {
  auto value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.table_value_uuid = {};
  RequireStatus(
      value, engine::ExecutionTableValueStatus::table_value_uuid_required,
      "EDR-018 accepted table value without UUID");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.relation_descriptor.relation_descriptor_uuid = {};
  const auto invalid_relation = engine::ValidateExecutionTableValue(value);
  Require(!invalid_relation.ok(),
          "EDR-018 accepted table value with invalid relation descriptor");
  Require(invalid_relation.status ==
              engine::ExecutionTableValueStatus::relation_descriptor_invalid,
          "EDR-018 invalid relation status mismatch");
  Require(invalid_relation.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-018 relation descriptor diagnostic was not preserved");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireStatus(
      value, engine::ExecutionTableValueStatus::relation_descriptor_kind_invalid,
      "EDR-018 accepted non-table relation descriptor");
}

void TestProducerAndPolicyFailures() {
  auto value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.producer_kind =
      static_cast<engine::ExecutionTableValueProducerKind>(0xff);
  RequireStatus(value,
                engine::ExecutionTableValueStatus::producer_kind_invalid,
                "EDR-018 accepted invalid table value producer kind");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.producer_uuid = {};
  RequireStatus(value,
                engine::ExecutionTableValueStatus::producer_uuid_required,
                "EDR-018 accepted table value without producer UUID");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.owner_transaction_uuid = {};
  RequireStatus(
      value,
      engine::ExecutionTableValueStatus::owner_transaction_uuid_required,
      "EDR-018 accepted table value without owner transaction UUID");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.snapshot_uuid = {};
  RequireStatus(value,
                engine::ExecutionTableValueStatus::snapshot_uuid_required,
                "EDR-018 accepted table value without snapshot UUID");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.security_policy_uuid = {};
  RequireStatus(
      value,
      engine::ExecutionTableValueStatus::security_policy_uuid_required,
      "EDR-018 accepted table value without security policy UUID");
}

void TestConsumerAndOptimizerFailures() {
  auto value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.consumable_by_sblr_operator = false;
  value.consumable_by_routine = false;
  value.consumable_by_udr = false;
  value.consumable_by_result_channel = false;
  RequireStatus(value,
                engine::ExecutionTableValueStatus::consumer_surface_required,
                "EDR-018 accepted table value without consumer surface");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.plan_requires_rewind = true;
  value.rewindable = false;
  RequireStatus(value,
                engine::ExecutionTableValueStatus::non_rewindable_plan_refused,
                "EDR-018 accepted non-rewindable table value for rewind plan");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.plan_requires_rewind = true;
  value.deterministic = false;
  RequireStatus(
      value,
      engine::ExecutionTableValueStatus::nondeterministic_rewind_refused,
      "EDR-018 accepted nondeterministic table value for rewind plan");

  value = ValidTableValue(engine::ExecutionTableValueProducerKind::routine);
  value.plan_may_duplicate = true;
  value.side_effecting = true;
  RequireStatus(
      value,
      engine::ExecutionTableValueStatus::side_effecting_duplicate_refused,
      "EDR-018 accepted side-effecting table value for duplicate plan");
}

}  // namespace

int main() {
  TestValidProducerKinds();
  TestIdentityAndRelationFailures();
  TestProducerAndPolicyFailures();
  TestConsumerAndOptimizerFailures();
  return EXIT_SUCCESS;
}
