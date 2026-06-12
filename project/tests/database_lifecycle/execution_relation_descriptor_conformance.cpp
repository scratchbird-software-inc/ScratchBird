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
  descriptor.descriptor_epoch = 9;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::character;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ResultColumnDescriptor Column(std::uint32_t ordinal,
                                      std::uint8_t seed,
                                      std::string_view name,
                                      bool nullable) {
  return {ordinal,
          Descriptor(seed, name),
          std::string(name),
          std::string(name),
          std::string(name),
          nullable};
}

engine::ExecutionRelationDescriptor ValidRelation(
    engine::ExecutionRelationKind kind) {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(0x80);
  relation.descriptor_epoch = 11;
  relation.relation_kind = kind;
  relation.stable_name = "edr015.relation";
  relation.columns.push_back(Column(0, 0x10, "id", false));
  relation.columns.push_back(Column(1, 0x30, "payload", true));
  relation.snapshot_uuid = Uuid(0xa0);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(0xb0);
  relation.memory_policy_uuid = Uuid(0xc0);
  relation.memory_policy_epoch = 3;
  if (kind == engine::ExecutionRelationKind::remote_fragment) {
    relation.coordinator_fragment_uuid = Uuid(0xd0);
    relation.worker_fragment_uuid = Uuid(0xe0);
    relation.fragment_ordinal = 4;
  }
  return relation;
}

void RequireStatus(const engine::ExecutionRelationDescriptor& relation,
                   engine::ExecutionRelationDescriptorStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionRelationDescriptor(relation);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-015 relation descriptor validation status mismatch");
}

void TestValidRelationKinds() {
  const engine::ExecutionRelationKind kinds[] = {
      engine::ExecutionRelationKind::cursor,
      engine::ExecutionRelationKind::rowset,
      engine::ExecutionRelationKind::table_value,
      engine::ExecutionRelationKind::result_channel,
      engine::ExecutionRelationKind::remote_fragment};
  for (const auto kind : kinds) {
    const auto result =
        engine::ValidateExecutionRelationDescriptor(ValidRelation(kind));
    Require(result.ok(), "EDR-015 valid relation descriptor was rejected");
  }
}

void TestDescriptorIdentityFailures() {
  auto relation = ValidRelation(engine::ExecutionRelationKind::cursor);
  relation.relation_descriptor_uuid = {};
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
      "EDR-015 accepted relation descriptor without UUID");

  relation = ValidRelation(engine::ExecutionRelationKind::cursor);
  relation.descriptor_epoch = 0;
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::descriptor_epoch_required,
      "EDR-015 accepted relation descriptor without epoch");

  relation = ValidRelation(engine::ExecutionRelationKind::cursor);
  relation.stable_name.clear();
  RequireStatus(relation,
                engine::ExecutionRelationDescriptorStatus::stable_name_required,
                "EDR-015 accepted relation descriptor without stable name");

  relation = ValidRelation(engine::ExecutionRelationKind::cursor);
  relation.descriptor_authoritative = false;
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::descriptor_not_authoritative,
      "EDR-015 accepted non-authoritative relation descriptor");

  relation = ValidRelation(engine::ExecutionRelationKind::cursor);
  relation.parser_independent = false;
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::descriptor_parser_dependent,
      "EDR-015 accepted parser-dependent relation descriptor");

  relation = ValidRelation(engine::ExecutionRelationKind::cursor);
  relation.relation_kind = static_cast<engine::ExecutionRelationKind>(0xff);
  RequireStatus(relation,
                engine::ExecutionRelationDescriptorStatus::relation_kind_invalid,
                "EDR-015 accepted invalid relation kind");
}

void TestColumnFailures() {
  auto relation = ValidRelation(engine::ExecutionRelationKind::rowset);
  relation.columns.clear();
  RequireStatus(relation,
                engine::ExecutionRelationDescriptorStatus::columns_required,
                "EDR-015 accepted relation descriptor without columns");

  relation = ValidRelation(engine::ExecutionRelationKind::rowset);
  relation.columns[1].ordinal = 7;
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::column_ordinal_mismatch,
      "EDR-015 accepted non-canonical relation column ordinal");

  relation = ValidRelation(engine::ExecutionRelationKind::rowset);
  relation.columns.front().descriptor.descriptor_uuid = {};
  const auto invalid_column =
      engine::ValidateExecutionRelationDescriptor(relation);
  Require(!invalid_column.ok(),
          "EDR-015 accepted invalid relation column descriptor");
  Require(invalid_column.status ==
              engine::ExecutionRelationDescriptorStatus::column_descriptor_invalid,
          "EDR-015 invalid relation column status mismatch");
  Require(invalid_column.column_descriptor_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_uuid,
          "EDR-015 invalid column descriptor detail was not preserved");

  relation = ValidRelation(engine::ExecutionRelationKind::rowset);
  relation.columns.front().reference_rendering_name.clear();
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::
          column_rendering_metadata_required,
      "EDR-015 accepted relation column without rendering metadata");
}

void TestPolicyAndRemoteFailures() {
  auto relation = ValidRelation(engine::ExecutionRelationKind::table_value);
  relation.snapshot_uuid = {};
  RequireStatus(relation,
                engine::ExecutionRelationDescriptorStatus::snapshot_uuid_required,
                "EDR-015 accepted relation descriptor without snapshot UUID");

  relation = ValidRelation(engine::ExecutionRelationKind::table_value);
  relation.security_policy_uuid = {};
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::security_policy_uuid_required,
      "EDR-015 accepted relation descriptor without required security policy");

  relation = ValidRelation(engine::ExecutionRelationKind::table_value);
  relation.memory_policy_uuid = {};
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::memory_policy_uuid_required,
      "EDR-015 accepted relation descriptor without memory policy UUID");

  relation = ValidRelation(engine::ExecutionRelationKind::table_value);
  relation.memory_policy_epoch = 0;
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::memory_policy_epoch_required,
      "EDR-015 accepted relation descriptor without memory policy epoch");

  relation = ValidRelation(engine::ExecutionRelationKind::remote_fragment);
  relation.worker_fragment_uuid = {};
  RequireStatus(
      relation,
      engine::ExecutionRelationDescriptorStatus::remote_fragment_uuid_required,
      "EDR-015 accepted remote fragment without worker UUID");
}

}  // namespace

int main() {
  TestValidRelationKinds();
  TestDescriptorIdentityFailures();
  TestColumnFailures();
  TestPolicyAndRemoteFailures();
  return EXIT_SUCCESS;
}
