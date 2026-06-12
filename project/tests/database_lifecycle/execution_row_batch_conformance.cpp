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

engine::ExecutionTypeDescriptor Descriptor(std::uint8_t seed) {
  engine::ExecutionTypeDescriptor descriptor;
  for (std::size_t index = 0; index < 16; ++index) {
    descriptor.descriptor_uuid.bytes[index] = static_cast<std::uint8_t>(seed + index);
  }
  descriptor.descriptor_epoch = 7;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::binary;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = "edr_row_descriptor";
  return descriptor;
}

engine::ExecutionRowBatch ValidBatch() {
  engine::ExecutionRowBatch batch;
  batch.batch_sequence = 42;
  batch.first_row_ordinal = 100;
  batch.max_rows = 16;
  batch.row_shape.columns.push_back({0, false});
  batch.row_shape.columns.push_back({1, true});

  batch.data_packet.descriptor_table.push_back(Descriptor(0x10));
  batch.data_packet.descriptor_table.push_back(Descriptor(0x30));
  batch.data_packet.payload_area = {0x01, 0x02, 0x03};
  batch.data_packet.slot_table.push_back({0, engine::ExecutionValueState::value, 0, 1});
  batch.data_packet.slot_table.push_back({1, engine::ExecutionValueState::sql_null, 0, 0});
  batch.data_packet.slot_table.push_back({0, engine::ExecutionValueState::value, 1, 1});
  batch.data_packet.slot_table.push_back({1, engine::ExecutionValueState::value, 2, 1});

  engine::ExecutionRow first;
  first.slot_indexes = {0, 1};
  first.value_state_bitmap = {engine::ExecutionValueState::value,
                              engine::ExecutionValueState::sql_null};
  first.null_bitmap = {false, true};
  batch.rows.push_back(first);

  engine::ExecutionRow second;
  second.slot_indexes = {2, 3};
  second.value_state_bitmap = {engine::ExecutionValueState::value,
                               engine::ExecutionValueState::value};
  second.null_bitmap = {false, false};
  batch.rows.push_back(second);
  return batch;
}

void RequireStatus(const engine::ExecutionRowBatch& batch,
                   engine::ExecutionRowBatchStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionRowBatch(batch);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-005 row batch validation status mismatch");
}

void TestValidExecutionRowBatch() {
  const auto result = engine::ValidateExecutionRowBatch(ValidBatch());
  Require(result.ok(), "EDR-005 valid execution row batch was rejected");

  engine::ExecutionRowBatch empty;
  Require(engine::ValidateExecutionRowBatch(empty).ok(),
          "EDR-005 empty row batch should validate");
}

void TestExecutionRowBatchEnvelopeFailures() {
  auto batch = ValidBatch();
  batch.data_packet.descriptor_table.front().descriptor_epoch = 0;
  const auto packet_result = engine::ValidateExecutionRowBatch(batch);
  Require(!packet_result.ok(),
          "EDR-005 accepted row batch with invalid data packet");
  Require(packet_result.status == engine::ExecutionRowBatchStatus::invalid_data_packet,
          "EDR-005 invalid packet status mismatch");
  Require(packet_result.packet_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-005 invalid packet diagnostic was not preserved");

  batch = ValidBatch();
  batch.max_rows = 0;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::max_rows_zero,
                "EDR-005 accepted zero max rows");

  batch = ValidBatch();
  batch.max_rows = engine::kExecutionRowBatchHardMaxRows + 1;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::max_rows_exceeds_limit,
                "EDR-005 accepted max rows above hard limit");

  batch = ValidBatch();
  batch.max_rows = 1;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_count_exceeds_limit,
                "EDR-005 accepted row count above max rows");
}

void TestExecutionRowBatchShapeFailures() {
  auto batch = ValidBatch();
  batch.row_shape.columns.clear();
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_shape_required,
                "EDR-005 accepted rows without row shape");

  batch = ValidBatch();
  batch.row_shape.columns.front().descriptor_index = 99;
  RequireStatus(batch,
                engine::ExecutionRowBatchStatus::row_shape_descriptor_out_of_range,
                "EDR-005 accepted row shape with invalid descriptor index");

  batch = ValidBatch();
  batch.rows.front().slot_indexes.pop_back();
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_width_mismatch,
                "EDR-005 accepted row width mismatch");
}

void TestExecutionRowBatchSlotAndBitmapFailures() {
  auto batch = ValidBatch();
  batch.rows.front().slot_indexes.front() = 99;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_slot_index_out_of_range,
                "EDR-005 accepted invalid row slot index");

  batch = ValidBatch();
  batch.rows.front().slot_indexes.front() = 1;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_slot_descriptor_mismatch,
                "EDR-005 accepted slot descriptor mismatch");

  batch = ValidBatch();
  batch.rows.front().value_state_bitmap.front() =
      static_cast<engine::ExecutionValueState>(0xff);
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_invalid_value_state,
                "EDR-005 accepted invalid row value-state bitmap entry");

  batch = ValidBatch();
  batch.rows.front().value_state_bitmap.front() = engine::ExecutionValueState::error;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_state_bitmap_mismatch,
                "EDR-005 accepted row value-state bitmap mismatch");

  batch = ValidBatch();
  batch.rows.front().null_bitmap.front() = true;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_null_bitmap_mismatch,
                "EDR-005 accepted row null bitmap mismatch");

  batch = ValidBatch();
  batch.row_shape.columns[1].nullable = false;
  RequireStatus(batch, engine::ExecutionRowBatchStatus::row_non_nullable_null,
                "EDR-005 accepted SQL null in non-nullable column");
}

}  // namespace

int main() {
  TestValidExecutionRowBatch();
  TestExecutionRowBatchEnvelopeFailures();
  TestExecutionRowBatchShapeFailures();
  TestExecutionRowBatchSlotAndBitmapFailures();
  return EXIT_SUCCESS;
}
