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
  descriptor.descriptor_epoch = 22;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ExecutionRelationDescriptor Relation(engine::ExecutionRelationKind kind,
                                             std::uint8_t seed,
                                             std::string_view name) {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(seed);
  relation.descriptor_epoch = 22;
  relation.relation_kind = kind;
  relation.stable_name = std::string(name);
  relation.columns.push_back(
      {0, Descriptor(static_cast<std::uint8_t>(seed + 1), "payload"),
       "payload", "payload", "payload", true});
  relation.snapshot_uuid = Uuid(static_cast<std::uint8_t>(seed + 2));
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(static_cast<std::uint8_t>(seed + 3));
  relation.memory_policy_uuid = Uuid(static_cast<std::uint8_t>(seed + 4));
  relation.memory_policy_epoch = 22;
  if (kind == engine::ExecutionRelationKind::remote_fragment) {
    relation.coordinator_fragment_uuid =
        Uuid(static_cast<std::uint8_t>(seed + 5));
    relation.worker_fragment_uuid = Uuid(static_cast<std::uint8_t>(seed + 6));
  }
  return relation;
}

engine::ExecutionOptimizerProducerDescriptor ValidProducer() {
  engine::ExecutionOptimizerProducerDescriptor producer;
  producer.optimizer_producer_uuid = Uuid(0x20);
  producer.descriptor_epoch = 22;
  producer.stable_name = "edr022.optimizer.producer";
  producer.producer_kind = engine::ExecutionOptimizerProducerKind::table_value;
  producer.relation_descriptor =
      Relation(engine::ExecutionRelationKind::table_value, 0x30,
               "edr022.table.value");
  producer.owner_transaction_uuid = Uuid(0x40);
  producer.snapshot_uuid = Uuid(0x50);
  producer.security_policy_uuid = Uuid(0x60);
  return producer;
}

engine::ExecutionOptimizerAccessPlan ValidPlan(
    engine::ExecutionOptimizerConsumerMode mode =
        engine::ExecutionOptimizerConsumerMode::single_pass) {
  engine::ExecutionOptimizerAccessPlan plan;
  plan.plan_uuid = Uuid(0x70);
  plan.optimizer_epoch = 22;
  plan.consumer_mode = mode;
  if (mode == engine::ExecutionOptimizerConsumerMode::rescan) {
    plan.rescan_count = 1;
  }
  if (mode == engine::ExecutionOptimizerConsumerMode::duplicate) {
    plan.duplicate_count = 2;
  }
  if (mode == engine::ExecutionOptimizerConsumerMode::cache) {
    plan.uses_descriptor_bound_cache_key = true;
  }
  if (mode == engine::ExecutionOptimizerConsumerMode::parallel_fanout) {
    plan.parallel_fanout = true;
  }
  return plan;
}

engine::ExecutionTableValue ValidTableValue() {
  engine::ExecutionTableValue value;
  value.table_value_uuid = Uuid(0x80);
  value.relation_descriptor =
      Relation(engine::ExecutionRelationKind::table_value, 0x90,
               "edr022.table.value.adapter");
  value.producer_kind = engine::ExecutionTableValueProducerKind::routine;
  value.producer_uuid = Uuid(0xa0);
  value.owner_transaction_uuid = Uuid(0xb0);
  value.snapshot_uuid = Uuid(0xc0);
  value.security_policy_uuid = Uuid(0xd0);
  return value;
}

void RequireStatus(
    const engine::ExecutionOptimizerProducerDescriptor& producer,
    const engine::ExecutionOptimizerAccessPlan& plan,
    engine::ExecutionOptimizerPlanAdmissionStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateExecutionOptimizerPlanAdmission(producer, plan);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-022 optimizer plan admission status mismatch");
}

void RequireProducerStatus(
    const engine::ExecutionOptimizerProducerDescriptor& producer,
    engine::ExecutionOptimizerPlanAdmissionStatus expected,
    std::string_view message) {
  const auto result =
      engine::ValidateExecutionOptimizerProducerDescriptor(producer);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-022 optimizer producer status mismatch");
}

void TestValidAdmissionProfiles() {
  Require(engine::ValidateExecutionOptimizerPlanAdmission(ValidProducer(),
                                                          ValidPlan())
              .ok(),
          "EDR-022 rejected valid single-pass optimizer producer");

  auto producer = ValidProducer();
  producer.rewindability = engine::ExecutionProducerRewindability::forward_only;
  producer.determinism = engine::ExecutionProducerDeterminism::nondeterministic;
  producer.side_effect_profile =
      engine::ExecutionProducerSideEffectProfile::side_effecting;
  auto plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::rescan);
  plan.uses_materialized_spool = true;
  Require(engine::ValidateExecutionOptimizerPlanAdmission(producer, plan).ok(),
          "EDR-022 rejected materialized rescan of forward-only producer");

  producer = ValidProducer();
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::cache);
  Require(engine::ValidateExecutionOptimizerPlanAdmission(producer, plan).ok(),
          "EDR-022 rejected descriptor-bound cache for snapshot producer");

  auto table_value = ValidTableValue();
  table_value.rewindable = false;
  table_value.deterministic = false;
  table_value.side_effecting = true;
  producer =
      engine::ExecutionOptimizerProducerDescriptorFromTableValue(table_value);
  Require(producer.producer_kind ==
              engine::ExecutionOptimizerProducerKind::table_value,
          "EDR-022 table value adapter lost producer kind");
  Require(producer.rewindability ==
              engine::ExecutionProducerRewindability::forward_only,
          "EDR-022 table value adapter lost forward-only profile");
  Require(producer.determinism ==
              engine::ExecutionProducerDeterminism::nondeterministic,
          "EDR-022 table value adapter lost determinism profile");
  Require(producer.side_effect_profile ==
              engine::ExecutionProducerSideEffectProfile::side_effecting,
          "EDR-022 table value adapter lost side-effect profile");
  plan = ValidPlan();
  Require(engine::ValidateExecutionOptimizerPlanAdmission(producer, plan).ok(),
          "EDR-022 rejected adapted table value for single-pass plan");
}

void TestProducerIdentityFailures() {
  auto producer = ValidProducer();
  producer.optimizer_producer_uuid = {};
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::producer_uuid_required,
      "EDR-022 accepted producer without UUID");

  producer = ValidProducer();
  producer.descriptor_epoch = 0;
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::descriptor_epoch_required,
      "EDR-022 accepted producer without descriptor epoch");

  producer = ValidProducer();
  producer.stable_name.clear();
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::stable_name_required,
      "EDR-022 accepted producer without stable name");

  producer = ValidProducer();
  producer.descriptor_authoritative = false;
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::descriptor_not_authoritative,
      "EDR-022 accepted non-authoritative producer descriptor");

  producer = ValidProducer();
  producer.parser_independent = false;
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::descriptor_parser_dependent,
      "EDR-022 accepted parser-dependent producer descriptor");
}

void TestProducerRelationAndContextFailures() {
  auto producer = ValidProducer();
  producer.producer_kind =
      static_cast<engine::ExecutionOptimizerProducerKind>(0xff);
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::producer_kind_invalid,
      "EDR-022 accepted invalid optimizer producer kind");

  producer = ValidProducer();
  producer.relation_descriptor.relation_descriptor_uuid = {};
  const auto relation_result =
      engine::ValidateExecutionOptimizerProducerDescriptor(producer);
  Require(!relation_result.ok(),
          "EDR-022 accepted invalid producer relation descriptor");
  Require(relation_result.status ==
              engine::ExecutionOptimizerPlanAdmissionStatus::
                  relation_descriptor_invalid,
          "EDR-022 relation descriptor status mismatch");
  Require(relation_result.relation_status ==
              engine::ExecutionRelationDescriptorStatus::
                  descriptor_uuid_required,
          "EDR-022 relation diagnostic was not preserved");

  producer = ValidProducer();
  producer.relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::rowset;
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::relation_kind_invalid,
      "EDR-022 accepted mismatched optimizer relation kind");

  producer = ValidProducer();
  producer.owner_transaction_uuid = {};
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          owner_transaction_uuid_required,
      "EDR-022 accepted producer without owner transaction UUID");

  producer = ValidProducer();
  producer.snapshot_uuid = {};
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::snapshot_uuid_required,
      "EDR-022 accepted producer without snapshot UUID");

  producer = ValidProducer();
  producer.security_policy_uuid = {};
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          security_policy_uuid_required,
      "EDR-022 accepted producer without security policy UUID");
}

void TestProducerProfileEnumFailures() {
  auto producer = ValidProducer();
  producer.rewindability =
      static_cast<engine::ExecutionProducerRewindability>(0xff);
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::rewindability_invalid,
      "EDR-022 accepted invalid rewindability profile");

  producer = ValidProducer();
  producer.determinism =
      static_cast<engine::ExecutionProducerDeterminism>(0xff);
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::determinism_invalid,
      "EDR-022 accepted invalid determinism profile");

  producer = ValidProducer();
  producer.side_effect_profile =
      static_cast<engine::ExecutionProducerSideEffectProfile>(0xff);
  RequireProducerStatus(
      producer,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          side_effect_profile_invalid,
      "EDR-022 accepted invalid side-effect profile");
}

void TestPlanIdentityFailures() {
  auto plan = ValidPlan();
  plan.plan_uuid = {};
  RequireStatus(ValidProducer(), plan,
                engine::ExecutionOptimizerPlanAdmissionStatus::
                    plan_uuid_required,
                "EDR-022 accepted optimizer plan without UUID");

  plan = ValidPlan();
  plan.optimizer_epoch = 0;
  RequireStatus(ValidProducer(), plan,
                engine::ExecutionOptimizerPlanAdmissionStatus::
                    optimizer_epoch_required,
                "EDR-022 accepted optimizer plan without epoch");

  plan = ValidPlan();
  plan.consumer_mode =
      static_cast<engine::ExecutionOptimizerConsumerMode>(0xff);
  RequireStatus(ValidProducer(), plan,
                engine::ExecutionOptimizerPlanAdmissionStatus::
                    consumer_mode_invalid,
                "EDR-022 accepted optimizer plan with invalid consumer mode");

  plan = ValidPlan();
  plan.executor_contract_acknowledged = false;
  RequireStatus(ValidProducer(), plan,
                engine::ExecutionOptimizerPlanAdmissionStatus::
                    executor_contract_required,
                "EDR-022 accepted plan without executor safety contract");
}

void TestRescanAdmissionFailures() {
  auto producer = ValidProducer();
  producer.rewindability = engine::ExecutionProducerRewindability::forward_only;
  auto plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::rescan);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          non_rewindable_rescan_refused,
      "EDR-022 accepted non-rewindable producer for unsafe rescan");

  producer = ValidProducer();
  producer.determinism =
      engine::ExecutionProducerDeterminism::transaction_volatile;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::rescan);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          nondeterministic_rescan_refused,
      "EDR-022 accepted volatile producer for unsafe rescan");

  producer = ValidProducer();
  producer.materialization_allowed = false;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::rescan);
  plan.uses_materialized_spool = true;
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::materialization_not_allowed,
      "EDR-022 accepted materialization when producer disallows it");
}

void TestDuplicateAndCacheAdmissionFailures() {
  auto producer = ValidProducer();
  producer.side_effect_profile =
      engine::ExecutionProducerSideEffectProfile::side_effecting;
  auto plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::duplicate);
  plan.duplicates_producer_execution = true;
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          side_effecting_duplicate_refused,
      "EDR-022 accepted duplicate execution of side-effecting producer");

  producer = ValidProducer();
  producer.determinism = engine::ExecutionProducerDeterminism::nondeterministic;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::duplicate);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          nondeterministic_duplicate_refused,
      "EDR-022 accepted duplicate output of nondeterministic producer");

  producer = ValidProducer();
  producer.rewindability = engine::ExecutionProducerRewindability::forward_only;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::duplicate);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          non_rewindable_duplicate_refused,
      "EDR-022 accepted duplicate output of forward-only producer");

  producer = ValidProducer();
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::cache);
  plan.uses_descriptor_bound_cache_key = false;
  RequireStatus(producer, plan,
                engine::ExecutionOptimizerPlanAdmissionStatus::
                    cache_requires_descriptor_bound_key,
                "EDR-022 accepted cache without descriptor-bound key");

  producer = ValidProducer();
  producer.cache_result_allowed = false;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::cache);
  RequireStatus(producer, plan,
                engine::ExecutionOptimizerPlanAdmissionStatus::cache_not_allowed,
                "EDR-022 accepted cache when producer disallows it");

  producer = ValidProducer();
  producer.determinism =
      engine::ExecutionProducerDeterminism::statement_deterministic;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::cache);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          cache_requires_snapshot_determinism,
      "EDR-022 accepted cache without snapshot determinism");

  producer = ValidProducer();
  producer.side_effect_profile =
      engine::ExecutionProducerSideEffectProfile::external_effect;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::cache);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          side_effecting_cache_refused,
      "EDR-022 accepted cache for side-effecting producer");
}

void TestParallelFanoutAdmissionFailures() {
  auto producer = ValidProducer();
  producer.parallel_fanout_allowed = false;
  auto plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::parallel_fanout);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::parallel_fanout_not_allowed,
      "EDR-022 accepted parallel fanout when producer disallows it");

  producer = ValidProducer();
  producer.side_effect_profile =
      engine::ExecutionProducerSideEffectProfile::side_effecting;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::parallel_fanout);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          side_effecting_parallel_refused,
      "EDR-022 accepted parallel fanout for side-effecting producer");

  producer = ValidProducer();
  producer.rewindability = engine::ExecutionProducerRewindability::forward_only;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::parallel_fanout);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          parallel_requires_materialized_or_rewindable,
      "EDR-022 accepted parallel fanout for forward-only producer");

  producer = ValidProducer();
  producer.determinism =
      engine::ExecutionProducerDeterminism::transaction_volatile;
  plan = ValidPlan(engine::ExecutionOptimizerConsumerMode::parallel_fanout);
  RequireStatus(
      producer, plan,
      engine::ExecutionOptimizerPlanAdmissionStatus::
          parallel_requires_snapshot_determinism,
      "EDR-022 accepted parallel fanout for volatile producer");
}

}  // namespace

int main() {
  TestValidAdmissionProfiles();
  TestProducerIdentityFailures();
  TestProducerRelationAndContextFailures();
  TestProducerProfileEnumFailures();
  TestPlanIdentityFailures();
  TestRescanAdmissionFailures();
  TestDuplicateAndCacheAdmissionFailures();
  TestParallelFanoutAdmissionFailures();
  return EXIT_SUCCESS;
}
