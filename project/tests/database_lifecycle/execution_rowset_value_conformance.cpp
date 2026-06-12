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
  descriptor.descriptor_epoch = 17;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::binary;
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

engine::ExecutionRelationDescriptor RowsetRelation() {
  engine::ExecutionRelationDescriptor relation;
  relation.relation_descriptor_uuid = Uuid(0x70);
  relation.descriptor_epoch = 6;
  relation.relation_kind = engine::ExecutionRelationKind::rowset;
  relation.stable_name = "edr017.rowset.relation";
  relation.columns.push_back(Column(0, 0x10, "payload", false));
  relation.columns.push_back(Column(1, 0x30, "state", true));
  relation.snapshot_uuid = Uuid(0x80);
  relation.security_context_required = true;
  relation.security_policy_uuid = Uuid(0x90);
  relation.memory_policy_uuid = Uuid(0xa0);
  relation.memory_policy_epoch = 9;
  return relation;
}

engine::ExecutionRowBatch RowBatch(std::uint64_t sequence) {
  engine::ExecutionRowBatch batch;
  batch.batch_sequence = sequence;
  batch.first_row_ordinal = sequence;
  batch.final_batch = true;
  batch.max_rows = 8;
  batch.row_shape.columns.push_back({0, false});
  batch.row_shape.columns.push_back({1, true});
  batch.data_packet.descriptor_table.push_back(Descriptor(0x10, "payload"));
  batch.data_packet.descriptor_table.push_back(Descriptor(0x30, "state"));
  batch.data_packet.payload_area = {0x01};
  batch.data_packet.slot_table.push_back(
      {0, engine::ExecutionValueState::value, 0, 1});
  batch.data_packet.slot_table.push_back(
      {1, engine::ExecutionValueState::sql_null, 0, 0});

  batch.rows.emplace_back();
  auto& row = batch.rows.back();
  row.slot_indexes = {0, 1};
  row.value_state_bitmap = {engine::ExecutionValueState::value,
                            engine::ExecutionValueState::sql_null};
  row.null_bitmap.push_back(false);
  row.null_bitmap.push_back(true);
  return batch;
}

engine::ExecutionRowsetValue InlineRowset() {
  engine::ExecutionRowsetValue rowset;
  rowset.rowset_uuid = Uuid(0xb0);
  rowset.relation_descriptor = RowsetRelation();
  rowset.storage_kind = engine::ExecutionRowsetStorageKind::inline_rows;
  rowset.row_batches.push_back(RowBatch(0));
  rowset.owner_transaction_uuid = Uuid(0xc0);
  rowset.memory_owner_uuid = Uuid(0xd0);
  rowset.max_rows = 4;
  rowset.memory_bytes = 64;
  rowset.memory_limit_bytes = 128;
  return rowset;
}

engine::ExecutionRowsetValue SpillRowset() {
  auto rowset = InlineRowset();
  rowset.storage_kind = engine::ExecutionRowsetStorageKind::spill_handle;
  rowset.row_batches.clear();
  rowset.spill_handle_uuid = Uuid(0xe0);
  rowset.copyable = true;
  rowset.movable = true;
  return rowset;
}

void RequireStatus(const engine::ExecutionRowsetValue& rowset,
                   engine::ExecutionRowsetValueStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionRowsetValue(rowset);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-017 rowset value validation status mismatch");
}

void TestValidRowsetValues() {
  Require(engine::ValidateExecutionRowsetValue(InlineRowset()).ok(),
          "EDR-017 valid inline rowset was rejected");
  Require(engine::ValidateExecutionRowsetValue(SpillRowset()).ok(),
          "EDR-017 valid spill rowset was rejected");
}

void TestIdentityAndOwnershipFailures() {
  auto rowset = InlineRowset();
  rowset.rowset_uuid = {};
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::rowset_uuid_required,
                "EDR-017 accepted rowset without UUID");

  rowset = InlineRowset();
  rowset.relation_descriptor.relation_descriptor_uuid = {};
  const auto invalid_relation = engine::ValidateExecutionRowsetValue(rowset);
  Require(!invalid_relation.ok(),
          "EDR-017 accepted rowset with invalid relation descriptor");
  Require(invalid_relation.status ==
              engine::ExecutionRowsetValueStatus::relation_descriptor_invalid,
          "EDR-017 invalid relation status mismatch");
  Require(invalid_relation.relation_status ==
              engine::ExecutionRelationDescriptorStatus::descriptor_uuid_required,
          "EDR-017 relation descriptor diagnostic was not preserved");

  rowset = InlineRowset();
  rowset.relation_descriptor.relation_kind =
      engine::ExecutionRelationKind::cursor;
  RequireStatus(
      rowset, engine::ExecutionRowsetValueStatus::relation_descriptor_kind_invalid,
      "EDR-017 accepted non-rowset relation descriptor");

  rowset = InlineRowset();
  rowset.owner_transaction_uuid = {};
  RequireStatus(
      rowset, engine::ExecutionRowsetValueStatus::owner_transaction_uuid_required,
      "EDR-017 accepted rowset without owner transaction UUID");

  rowset = InlineRowset();
  rowset.memory_owner_uuid = {};
  RequireStatus(rowset,
                engine::ExecutionRowsetValueStatus::memory_owner_uuid_required,
                "EDR-017 accepted rowset without memory owner UUID");
}

void TestBoundsMemoryAndMobilityFailures() {
  auto rowset = InlineRowset();
  rowset.bounded = false;
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::unbounded_rowset,
                "EDR-017 accepted unbounded rowset");

  rowset = InlineRowset();
  rowset.max_rows = 0;
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::max_rows_required,
                "EDR-017 accepted rowset without max rows");

  rowset = InlineRowset();
  rowset.memory_limit_bytes = 0;
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::memory_limit_required,
                "EDR-017 accepted rowset without memory limit");

  rowset = InlineRowset();
  rowset.memory_bytes = 256;
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::memory_limit_exceeded,
                "EDR-017 accepted rowset over memory limit");

  rowset = InlineRowset();
  rowset.copyable = false;
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::not_copyable,
                "EDR-017 accepted non-copyable rowset");

  rowset = InlineRowset();
  rowset.movable = false;
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::not_movable,
                "EDR-017 accepted non-movable rowset");
}

void TestStorageAndBatchFailures() {
  auto rowset = InlineRowset();
  rowset.storage_kind = static_cast<engine::ExecutionRowsetStorageKind>(0xff);
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::storage_kind_invalid,
                "EDR-017 accepted invalid rowset storage kind");

  rowset = InlineRowset();
  rowset.row_batches.clear();
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::inline_rows_required,
                "EDR-017 accepted inline rowset without row batches");

  rowset = InlineRowset();
  rowset.row_batches.front().data_packet.descriptor_table.front().descriptor_epoch = 0;
  const auto invalid_batch = engine::ValidateExecutionRowsetValue(rowset);
  Require(!invalid_batch.ok(), "EDR-017 accepted invalid inline row batch");
  Require(invalid_batch.status ==
              engine::ExecutionRowsetValueStatus::inline_batch_invalid,
          "EDR-017 invalid inline batch status mismatch");
  Require(invalid_batch.row_batch_status ==
              engine::ExecutionRowBatchStatus::invalid_data_packet,
          "EDR-017 invalid inline batch detail was not preserved");

  rowset = InlineRowset();
  rowset.relation_descriptor.columns.pop_back();
  RequireStatus(rowset,
                engine::ExecutionRowsetValueStatus::inline_batch_shape_mismatch,
                "EDR-017 accepted rowset batch shape mismatch");

  rowset = InlineRowset();
  rowset.max_rows = 1;
  rowset.row_batches.push_back(RowBatch(1));
  RequireStatus(rowset, engine::ExecutionRowsetValueStatus::row_count_exceeds_max,
                "EDR-017 accepted row count above max row bound");

  rowset = SpillRowset();
  rowset.spill_handle_uuid = {};
  RequireStatus(rowset,
                engine::ExecutionRowsetValueStatus::spill_handle_uuid_required,
                "EDR-017 accepted spill rowset without spill handle UUID");

  rowset = SpillRowset();
  rowset.row_batches.push_back(RowBatch(0));
  RequireStatus(rowset,
                engine::ExecutionRowsetValueStatus::spill_inline_rows_forbidden,
                "EDR-017 accepted spill rowset with inline rows");
}

}  // namespace

int main() {
  TestValidRowsetValues();
  TestIdentityAndOwnershipFailures();
  TestBoundsMemoryAndMobilityFailures();
  TestStorageAndBatchFailures();
  return EXIT_SUCCESS;
}
