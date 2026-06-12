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

engine::ExecutionTypeDescriptor Descriptor(std::uint8_t seed,
                                           std::string_view name) {
  engine::ExecutionTypeDescriptor descriptor;
  for (std::size_t index = 0; index < 16; ++index) {
    descriptor.descriptor_uuid.bytes[index] =
        static_cast<std::uint8_t>(seed + index);
  }
  descriptor.descriptor_epoch = 7;
  descriptor.canonical_type_id = seed;
  descriptor.family = engine::ExecutionTypeFamily::binary;
  descriptor.width_class = engine::ExecutionTypeWidthClass::variable;
  descriptor.stable_name = std::string(name);
  return descriptor;
}

engine::ExecutionRowBatch Batch(std::uint64_t sequence, bool final_batch) {
  engine::ExecutionRowBatch batch;
  batch.batch_sequence = sequence;
  batch.first_row_ordinal = sequence == 0 ? 0 : 2;
  batch.final_batch = final_batch;
  batch.max_rows = 16;
  batch.row_shape.columns.push_back({0, false});
  batch.row_shape.columns.push_back({1, true});
  batch.data_packet.descriptor_table.push_back(Descriptor(0x10, "payload"));
  batch.data_packet.descriptor_table.push_back(Descriptor(0x30, "status"));

  if (sequence == 0) {
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
  } else {
    batch.data_packet.payload_area = {0x04, 0x05};
    batch.data_packet.slot_table.push_back({0, engine::ExecutionValueState::value, 0, 1});
    batch.data_packet.slot_table.push_back({1, engine::ExecutionValueState::value, 1, 1});

    engine::ExecutionRow row;
    row.slot_indexes = {0, 1};
    row.value_state_bitmap = {engine::ExecutionValueState::value,
                              engine::ExecutionValueState::value};
    row.null_bitmap = {false, false};
    batch.rows.push_back(row);
  }
  return batch;
}

engine::ExecutionResultEnvelope ValidEnvelope() {
  engine::ExecutionResultEnvelope envelope;
  envelope.metadata.result_shape_id = "result.shape.edr006.rowset";
  envelope.metadata.statement_uuid = "statement-edr006";
  envelope.columns.push_back({0, Descriptor(0x10, "payload"), "payload",
                              "payload", "payload", false});
  envelope.columns.push_back({1, Descriptor(0x30, "status"), "status",
                              "status", "status", true});
  envelope.row_batches.push_back(Batch(0, false));
  envelope.row_batches.push_back(Batch(1, true));
  envelope.completion.present = true;
  envelope.completion.success = true;
  envelope.completion.completion_code = "COMMAND_OK";
  envelope.completion.rows_affected = 3;
  envelope.summary.rows_produced = 3;
  envelope.summary.batch_count = 2;
  envelope.summary.final_batch_seen = true;
  envelope.diagnostics.push_back({false, "00000", "ok", "completed"});
  return envelope;
}

void RequireStatus(const engine::ExecutionResultEnvelope& envelope,
                   engine::ExecutionResultEnvelopeStatus expected,
                   std::string_view message) {
  const auto result = engine::ValidateExecutionResultEnvelope(envelope);
  Require(!result.ok(), message);
  Require(result.status == expected,
          "EDR-006 result envelope validation status mismatch");
}

void TestValidExecutionResultEnvelope() {
  const auto result = engine::ValidateExecutionResultEnvelope(ValidEnvelope());
  Require(result.ok(), "EDR-006 valid result envelope was rejected");
}

void TestExecutionResultEnvelopeMetadataAndColumnFailures() {
  auto envelope = ValidEnvelope();
  envelope.metadata.major_version = 2;
  RequireStatus(envelope, engine::ExecutionResultEnvelopeStatus::unsupported_version,
                "EDR-006 accepted unsupported result envelope version");

  envelope = ValidEnvelope();
  envelope.metadata.result_shape_id.clear();
  RequireStatus(envelope, engine::ExecutionResultEnvelopeStatus::metadata_required,
                "EDR-006 accepted result envelope without shape id");

  envelope = ValidEnvelope();
  envelope.metadata.descriptor_authoritative = false;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::metadata_not_authoritative,
                "EDR-006 accepted non-authoritative result envelope metadata");

  envelope = ValidEnvelope();
  envelope.metadata.parser_independent = false;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::metadata_parser_dependent,
                "EDR-006 accepted parser-dependent result envelope metadata");

  envelope = ValidEnvelope();
  envelope.columns.clear();
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::columns_required_for_rows,
                "EDR-006 accepted row batches without result columns");

  envelope = ValidEnvelope();
  envelope.columns[1].ordinal = 9;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::column_ordinal_mismatch,
                "EDR-006 accepted non-canonical column ordinal");

  envelope = ValidEnvelope();
  envelope.columns.front().descriptor.descriptor_uuid = {};
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::column_descriptor_invalid,
                "EDR-006 accepted invalid result column descriptor");

  envelope = ValidEnvelope();
  envelope.columns.front().reference_rendering_name.clear();
  RequireStatus(
      envelope,
      engine::ExecutionResultEnvelopeStatus::column_rendering_metadata_required,
      "EDR-006 accepted column without reference rendering metadata");
}

void TestExecutionResultEnvelopeRowBatchFailures() {
  auto envelope = ValidEnvelope();
  envelope.row_batches.front().data_packet.descriptor_table.front().descriptor_epoch = 0;
  const auto invalid_batch = engine::ValidateExecutionResultEnvelope(envelope);
  Require(!invalid_batch.ok(),
          "EDR-006 accepted result envelope with invalid row batch");
  Require(invalid_batch.status ==
              engine::ExecutionResultEnvelopeStatus::row_batch_invalid,
          "EDR-006 invalid row batch status mismatch");
  Require(invalid_batch.row_batch_status ==
              engine::ExecutionRowBatchStatus::invalid_data_packet,
          "EDR-006 invalid row batch diagnostic was not preserved");
  Require(invalid_batch.packet_status ==
              engine::ExecutionDataPacketStatus::descriptor_missing_epoch,
          "EDR-006 invalid packet diagnostic was not preserved");

  envelope = ValidEnvelope();
  envelope.row_batches[1].batch_sequence = 9;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::row_batch_sequence_mismatch,
                "EDR-006 accepted non-contiguous result batch sequence");

  envelope = ValidEnvelope();
  envelope.row_batches.front().final_batch = true;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::row_batch_final_not_last,
                "EDR-006 accepted final batch before last batch");

  envelope = ValidEnvelope();
  envelope.row_batches.front().rows.clear();
  envelope.row_batches.front().data_packet.payload_area.clear();
  envelope.row_batches.front().data_packet.slot_table.clear();
  envelope.row_batches.front().row_shape.columns.pop_back();
  envelope.summary.rows_produced = 1;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::row_batch_shape_mismatch,
                "EDR-006 accepted row batch shape mismatch");

  envelope = ValidEnvelope();
  envelope.row_batches.front().data_packet.descriptor_table.front() =
      Descriptor(0x50, "different");
  RequireStatus(
      envelope,
      engine::ExecutionResultEnvelopeStatus::row_batch_column_descriptor_mismatch,
      "EDR-006 accepted row batch column descriptor mismatch");

  envelope = ValidEnvelope();
  envelope.row_batches.front().rows.clear();
  envelope.row_batches.front().data_packet.payload_area.clear();
  envelope.row_batches.front().data_packet.slot_table.clear();
  envelope.row_batches.front().row_shape.columns.front().nullable = true;
  envelope.summary.rows_produced = 1;
  RequireStatus(
      envelope,
      engine::ExecutionResultEnvelopeStatus::row_batch_column_nullability_mismatch,
      "EDR-006 accepted row batch column nullability mismatch");
}

void TestExecutionResultEnvelopeCompletionSummaryAndDiagnosticsFailures() {
  auto envelope = ValidEnvelope();
  envelope.completion.present = false;
  RequireStatus(envelope, engine::ExecutionResultEnvelopeStatus::completion_required,
                "EDR-006 accepted result envelope without completion");

  envelope = ValidEnvelope();
  envelope.completion.completion_code.clear();
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::completion_code_required,
                "EDR-006 accepted completion without code");

  envelope = ValidEnvelope();
  envelope.completion.success = false;
  envelope.diagnostics.clear();
  RequireStatus(
      envelope,
      engine::ExecutionResultEnvelopeStatus::completion_error_without_diagnostic,
      "EDR-006 accepted failed completion without error diagnostic");

  envelope = ValidEnvelope();
  envelope.summary.rows_produced = 4;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::summary_row_count_mismatch,
                "EDR-006 accepted incorrect summary row count");

  envelope = ValidEnvelope();
  envelope.summary.batch_count = 99;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::summary_batch_count_mismatch,
                "EDR-006 accepted incorrect summary batch count");

  envelope = ValidEnvelope();
  envelope.summary.final_batch_seen = false;
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::summary_final_batch_mismatch,
                "EDR-006 accepted incorrect final-batch summary");

  envelope = ValidEnvelope();
  envelope.diagnostics.front().diagnostic_code.clear();
  RequireStatus(envelope,
                engine::ExecutionResultEnvelopeStatus::diagnostic_code_required,
                "EDR-006 accepted diagnostic without code");
}

}  // namespace

int main() {
  TestValidExecutionResultEnvelope();
  TestExecutionResultEnvelopeMetadataAndColumnFailures();
  TestExecutionResultEnvelopeRowBatchFailures();
  TestExecutionResultEnvelopeCompletionSummaryAndDiagnosticsFailures();
  return EXIT_SUCCESS;
}
