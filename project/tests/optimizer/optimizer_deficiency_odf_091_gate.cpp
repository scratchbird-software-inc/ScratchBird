// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "result_batch_transfer.hpp"
#include "vectorized_result_batch.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace wire = scratchbird::wire;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "finality_authority=true", "visibility_authority=true",
          "parser_execution_authority=true", "provider_authority=true",
          "client_authority=true", "recovery_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-091 evidence leaked forbidden documentation or authority token");
    }
  }
}

std::vector<std::uint8_t> Bytes(std::string_view text) {
  return {text.begin(), text.end()};
}

std::vector<std::uint8_t> FixedBytes(std::size_t count) {
  std::vector<std::uint8_t> data(count);
  for (std::size_t i = 0; i < count; ++i) {
    data[i] = static_cast<std::uint8_t>(i + 1u);
  }
  return data;
}

exec::VectorizedResultBatchResult BuildCompleteBatch() {
  exec::VectorizedResultBatchBuilder builder(4);
  builder.AddColumn(exec::MakeFixedWidthResultBatchColumn(
      "id", 4, 4, FixedBytes(16), exec::MakeResultBatchValidityBitmap(4)));
  builder.AddColumn(exec::MakeVariableWidthResultBatchColumn(
      "name", 4, {0, 3, 7, 7, 10}, Bytes("redbluenil"),
      exec::MakeResultBatchValidityBitmap(4, {2})));
  builder.AddColumn(exec::MakeDictionaryResultBatchColumn(
      "status", 4, {0, 1, 0, 2}, {"new", "seen", "done"},
      exec::MakeResultBatchValidityBitmap(4)));
  builder.AddColumn(exec::MakeRunEndResultBatchColumn(
      "segment", 4, {2, 4}, {"north", "south"},
      exec::MakeResultBatchValidityBitmap(4)));

  std::vector<exec::ResultBatchColumn> struct_children;
  struct_children.push_back(exec::MakeFixedWidthResultBatchColumn(
      "metrics.count", 4, 2, FixedBytes(8),
      exec::MakeResultBatchValidityBitmap(4)));
  struct_children.push_back(exec::MakeVariableWidthResultBatchColumn(
      "metrics.label", 4, {0, 1, 3, 6, 10}, Bytes("abcdefqrst"),
      exec::MakeResultBatchValidityBitmap(4)));
  builder.AddColumn(exec::MakeStructViewResultBatchColumn(
      "metrics", 4, std::move(struct_children),
      exec::MakeResultBatchValidityBitmap(4)));

  auto list_child = exec::MakeVariableWidthResultBatchColumn(
      "tags.value", 5, {0, 1, 2, 3, 4, 5}, Bytes("abcde"),
      exec::MakeResultBatchValidityBitmap(5));
  builder.AddColumn(exec::MakeListViewResultBatchColumn(
      "tags", 4, {0, 2, 2, 3, 5}, std::move(list_child),
      exec::MakeResultBatchValidityBitmap(4, {1})));

  return builder.Finalize();
}

void CompleteBatchAndTransferDescriptor() {
  auto result = BuildCompleteBatch();
  Require(result.ok(), "ODF-091 complete vectorized result batch did not finalize");
  Require(result.batch.row_count == 4, "ODF-091 batch row count changed");
  Require(result.batch.columns.size() == 6, "ODF-091 batch column count changed");
  Require(result.batch.column_diagnostics.size() >= 9,
          "ODF-091 nested column diagnostics missing");
  Require(EvidenceHas(result.evidence, "result_batch.data_transport_only=true"),
          "ODF-091 transport-only evidence missing");
  Require(EvidenceHas(result.evidence, "result_batch.row_order_preserved=true"),
          "ODF-091 row order evidence missing");
  Require(EvidenceHas(result.evidence, "column.layout_kind=fixed_width"),
          "ODF-091 fixed-width evidence missing");
  Require(EvidenceHas(result.evidence, "column.layout_kind=variable_width"),
          "ODF-091 variable-width evidence missing");
  Require(EvidenceHas(result.evidence, "column.layout_kind=dictionary"),
          "ODF-091 dictionary evidence missing");
  Require(EvidenceHas(result.evidence, "column.layout_kind=run_end"),
          "ODF-091 run-end evidence missing");
  Require(EvidenceHas(result.evidence, "column.layout_kind=struct_view"),
          "ODF-091 struct evidence missing");
  Require(EvidenceHas(result.evidence, "column.layout_kind=list_view"),
          "ODF-091 list evidence missing");
  Require(EvidenceHas(result.evidence, "column.null_count=1"),
          "ODF-091 null-count evidence missing");
  Require(EvidenceHas(result.evidence, "column.dictionary_value_count=3"),
          "ODF-091 dictionary metadata evidence missing");
  Require(EvidenceHas(result.evidence, "column.run_count=2"),
          "ODF-091 run-end metadata evidence missing");
  Require(EvidenceHas(result.evidence, "column.child_count=1"),
          "ODF-091 child metadata evidence missing");
  Require(EvidenceHas(result.evidence, "column.list_value_count=5"),
          "ODF-091 list metadata evidence missing");
  RequireEvidenceHygiene(result.evidence);

  auto descriptor = wire::BuildResultBatchTransferDescriptor(result.batch);
  Require(descriptor.ok(), "ODF-091 wire transfer descriptor failed");
  Require(descriptor.descriptor.version ==
              wire::kResultBatchTransferDescriptorVersion,
          "ODF-091 transfer descriptor version changed");
  Require(descriptor.descriptor.row_count == 4,
          "ODF-091 transfer descriptor row count changed");
  Require(descriptor.descriptor.column_count == 6,
          "ODF-091 transfer descriptor column count changed");
  Require(descriptor.descriptor.encoded_byte_count > 0,
          "ODF-091 transfer descriptor encoded byte count missing");
  Require(EvidenceHas(descriptor.evidence, "transfer_descriptor.version=1"),
          "ODF-091 transfer descriptor version evidence missing");
  Require(EvidenceHas(descriptor.evidence,
                      "transfer_descriptor.column.layout_kind=list_view"),
          "ODF-091 transfer descriptor layout evidence missing");
  Require(EvidenceHas(descriptor.evidence,
                      "transfer_descriptor.column.encoded_byte_count="),
          "ODF-091 transfer descriptor byte evidence missing");
  RequireEvidenceHygiene(descriptor.evidence);

  const auto bytes =
      wire::SerializeResultBatchTransferDescriptor(descriptor.descriptor);
  const std::string serialized(bytes.begin(), bytes.end());
  Require(serialized.find("SB_RESULT_BATCH_TRANSFER_V1") != std::string::npos,
          "ODF-091 serialized transfer header missing");
  Require(serialized.find("layout=list_view") != std::string::npos,
          "ODF-091 serialized list layout missing");
}

void RequireRefusal(const exec::VectorizedResultBatchResult& result,
                    std::string_view diagnostic,
                    std::string_view reason) {
  Require(!result.ok() && result.fail_closed,
          "ODF-091 corrupt batch was not refused fail-closed");
  Require(result.diagnostic.diagnostic_code == diagnostic,
          "ODF-091 diagnostic code changed");
  Require(EvidenceHas(result.evidence, "fallback_refusal_reason="),
          "ODF-091 fallback/refusal evidence missing");
  Require(EvidenceHas(result.evidence, reason),
          "ODF-091 exact refusal reason missing");
  RequireEvidenceHygiene(result.evidence);
}

void CorruptInputsFailClosed() {
  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeFixedWidthResultBatchColumn("bad_validity", 4, 4,
                                                    FixedBytes(16), {0xff})}),
      "SB_RESULT_BATCH.INVALID_VALIDITY_BITMAP",
      "validity_bitmap_padding_bits_set");

  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeVariableWidthResultBatchColumn(
                 "bad_offsets", 4, {0, 2, 1, 4, 5}, Bytes("abcde"),
                 exec::MakeResultBatchValidityBitmap(4))}),
      "SB_RESULT_BATCH.CORRUPT_OFFSETS", "variable_offsets_not_monotonic");

  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeDictionaryResultBatchColumn(
                 "bad_dictionary", 4, {0, 1, 4, 2}, {"a", "b", "c"},
                 exec::MakeResultBatchValidityBitmap(4))}),
      "SB_RESULT_BATCH.DICTIONARY_ID_INVALID",
      "dictionary_id_out_of_range");

  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeRunEndResultBatchColumn(
                 "bad_runs", 4, {2, 2}, {"a", "b"},
                 exec::MakeResultBatchValidityBitmap(4))}),
      "SB_RESULT_BATCH.RUN_END_INVALID", "run_end_range_malformed");

  std::vector<exec::ResultBatchColumn> bad_struct_children;
  bad_struct_children.push_back(exec::MakeFixedWidthResultBatchColumn(
      "child", 3, 4, FixedBytes(12), exec::MakeResultBatchValidityBitmap(3)));
  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeStructViewResultBatchColumn(
                 "bad_struct", 4, std::move(bad_struct_children),
                 exec::MakeResultBatchValidityBitmap(4))}),
      "SB_RESULT_BATCH.STRUCT_CHILD_MISMATCH",
      "struct_child_row_count_mismatch");

  auto child = exec::MakeFixedWidthResultBatchColumn(
      "list_child", 3, 1, FixedBytes(3), exec::MakeResultBatchValidityBitmap(3));
  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeListViewResultBatchColumn(
                 "bad_list", 4, {0, 1, 2, 3, 4}, std::move(child),
                 exec::MakeResultBatchValidityBitmap(4))}),
      "SB_RESULT_BATCH.CORRUPT_OFFSETS", "list_terminal_offset_mismatch");

  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          4, {exec::MakeFixedWidthResultBatchColumn(
                 "bad_rows", 3, 4, FixedBytes(12),
                 exec::MakeResultBatchValidityBitmap(3))}),
      "SB_RESULT_BATCH.ROW_COUNT_MISMATCH", "column_row_count_mismatch");
}

void TransferDescriptorRefusesUnfinalizedBatch() {
  exec::VectorizedResultBatch unfinalized;
  unfinalized.row_count = 4;
  unfinalized.columns.push_back(exec::MakeFixedWidthResultBatchColumn(
      "id", 4, 4, FixedBytes(16), exec::MakeResultBatchValidityBitmap(4)));

  auto refused = wire::BuildResultBatchTransferDescriptor(unfinalized);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-091 transfer descriptor accepted unfinalized batch");
  Require(refused.diagnostic.diagnostic_code ==
              "SB_RESULT_BATCH_TRANSFER.INVALID_BATCH",
          "ODF-091 transfer descriptor refusal diagnostic changed");
  Require(EvidenceHas(refused.evidence,
                      "fallback_refusal_reason=batch_missing_executor_column_diagnostics"),
          "ODF-091 transfer descriptor refusal reason missing");
  RequireEvidenceHygiene(refused.evidence);
}

void TransferDescriptorRevalidatesMutableBatch() {
  auto result = BuildCompleteBatch();
  Require(result.ok(), "ODF-091 mutable batch fixture did not finalize");
  result.batch.columns.front().fixed_width_data.pop_back();

  auto refused = wire::BuildResultBatchTransferDescriptor(result.batch);
  Require(!refused.ok() && refused.fail_closed,
          "ODF-091 transfer descriptor accepted stale mutated batch");
  Require(EvidenceHas(refused.evidence,
                      "fallback_refusal_reason=batch_revalidation_failed:fixed_width_data_size_mismatch"),
          "ODF-091 stale batch revalidation reason missing");
  RequireEvidenceHygiene(refused.evidence);
}

void HugeRowCountFailsClosedBeforeBitmapScan() {
  RequireRefusal(
      exec::FinalizeVectorizedResultBatch(
          std::numeric_limits<exec::u64>::max(),
          {exec::MakeFixedWidthResultBatchColumn(
              "huge", std::numeric_limits<exec::u64>::max(), 1, {}, {})}),
      "SB_RESULT_BATCH.INVALID_VALIDITY_BITMAP",
      "validity_bitmap_size_mismatch");
}

void DuplicateNestedNamesUseColumnLocalMetadata() {
  exec::VectorizedResultBatchBuilder builder(2);
  std::vector<exec::ResultBatchColumn> left_children;
  left_children.push_back(exec::MakeFixedWidthResultBatchColumn(
      "value", 2, 1, FixedBytes(2), exec::MakeResultBatchValidityBitmap(2)));
  builder.AddColumn(exec::MakeStructViewResultBatchColumn(
      "left", 2, std::move(left_children),
      exec::MakeResultBatchValidityBitmap(2)));

  std::vector<exec::ResultBatchColumn> right_children;
  right_children.push_back(exec::MakeFixedWidthResultBatchColumn(
      "value", 2, 4, FixedBytes(8), exec::MakeResultBatchValidityBitmap(2)));
  builder.AddColumn(exec::MakeStructViewResultBatchColumn(
      "right", 2, std::move(right_children),
      exec::MakeResultBatchValidityBitmap(2)));

  auto result = builder.Finalize();
  Require(result.ok(), "ODF-091 duplicate nested-name batch failed");
  auto descriptor = wire::BuildResultBatchTransferDescriptor(result.batch);
  Require(descriptor.ok(),
          "ODF-091 duplicate nested-name descriptor failed");
  Require(descriptor.descriptor.columns.size() == 2,
          "ODF-091 duplicate nested-name descriptor column count changed");
  Require(descriptor.descriptor.columns[0].children.size() == 1 &&
              descriptor.descriptor.columns[1].children.size() == 1,
          "ODF-091 duplicate nested-name children missing");
  Require(descriptor.descriptor.columns[0].children[0].encoded_byte_count == 3,
          "ODF-091 left duplicate child metadata was not column-local");
  Require(descriptor.descriptor.columns[1].children[0].encoded_byte_count == 9,
          "ODF-091 right duplicate child metadata was not column-local");
}

}  // namespace

int main() {
  CompleteBatchAndTransferDescriptor();
  CorruptInputsFailClosed();
  TransferDescriptorRefusesUnfinalizedBatch();
  TransferDescriptorRevalidatesMutableBatch();
  HugeRowCountFailsClosedBeforeBitmapScan();
  DuplicateNestedNamesUseColumnLocalMetadata();
  return EXIT_SUCCESS;
}
