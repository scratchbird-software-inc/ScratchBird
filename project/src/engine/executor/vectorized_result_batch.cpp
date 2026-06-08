// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vectorized_result_batch.hpp"

#include "reservation_backed_memory_resource.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <sstream>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

std::string CountEvidence(std::string key, u64 value) {
  return std::move(key) + "=" + std::to_string(value);
}

bool BitmapByteCount(u64 row_count, u64* byte_count) {
  if (byte_count == nullptr) {
    return false;
  }
  *byte_count = (row_count / 8u) + ((row_count % 8u) == 0 ? 0u : 1u);
  return *byte_count <=
         static_cast<u64>(std::numeric_limits<std::size_t>::max());
}

bool BitSet(const std::vector<std::uint8_t>& bitmap, u64 ordinal) {
  return (bitmap[static_cast<std::size_t>(ordinal / 8u)] &
          static_cast<std::uint8_t>(1u << (ordinal % 8u))) != 0;
}

u64 NullCount(const std::vector<std::uint8_t>& bitmap, u64 row_count) {
  u64 null_count = 0;
  for (u64 row = 0; row < row_count; ++row) {
    if (!BitSet(bitmap, row)) {
      ++null_count;
    }
  }
  return null_count;
}

u64 SetBitCount(const std::vector<std::uint8_t>& bitmap, u64 row_count) {
  u64 bit_count = 0;
  for (u64 row = 0; row < row_count; ++row) {
    if (BitSet(bitmap, row)) {
      ++bit_count;
    }
  }
  return bit_count;
}

u64 StringBytes(const std::vector<std::string>& values) {
  u64 bytes = 0;
  for (const auto& value : values) {
    bytes += static_cast<u64>(value.size());
  }
  return bytes;
}

u64 ChildEncodedBytes(const std::vector<ResultBatchColumnDiagnostics>& items) {
  u64 bytes = 0;
  for (const auto& item : items) {
    bytes += item.encoded_byte_count;
  }
  return bytes;
}

u64 EstimateResultFrameScratchBytes(const ResultBatchColumn& column) {
  u64 bytes = static_cast<u64>(column.validity_bitmap.size()) +
              static_cast<u64>(column.redaction_bitmap.size()) +
              static_cast<u64>(column.fixed_width_data.size()) +
              static_cast<u64>(column.offsets.size() * sizeof(u64)) +
              static_cast<u64>(column.variable_data.size()) +
              static_cast<u64>(column.dictionary_ids.size() * sizeof(u32)) +
              StringBytes(column.dictionary_values) +
              static_cast<u64>(column.run_ends.size() * sizeof(u64)) +
              StringBytes(column.run_values);
  for (const auto& child : column.children) {
    const u64 child_bytes = EstimateResultFrameScratchBytes(child);
    bytes = child_bytes > std::numeric_limits<u64>::max() - bytes
                ? std::numeric_limits<u64>::max()
                : bytes + child_bytes;
  }
  return bytes;
}

u64 EstimateResultFrameScratchBytes(const std::vector<ResultBatchColumn>& columns) {
  u64 bytes = 0;
  for (const auto& column : columns) {
    const u64 column_bytes = EstimateResultFrameScratchBytes(column);
    bytes = column_bytes > std::numeric_limits<u64>::max() - bytes
                ? std::numeric_limits<u64>::max()
                : bytes + column_bytes;
  }
  return bytes == 0 ? 1 : bytes;
}

VectorizedResultBatchResult Refuse(std::string diagnostic_code,
                                   std::string message_key,
                                   std::string reason,
                                   u64 row_count,
                                   ResultBatchLayoutKind layout) {
  VectorizedResultBatchResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.batch.row_count = row_count;
  result.diagnostic = MakeVectorizedResultBatchDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence.push_back("result_batch.data_transport_only=true");
  result.evidence.push_back("result_batch.row_order_preserved=true");
  result.evidence.push_back(CountEvidence("result_batch.row_count", row_count));
  result.evidence.push_back("layout_kind=" +
                            std::string(ResultBatchLayoutKindName(layout)));
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

void AppendEvidence(std::vector<std::string>* out,
                    const std::vector<std::string>& values) {
  out->insert(out->end(), values.begin(), values.end());
}

VectorizedResultBatchResult ValidateValidityBitmap(
    const ResultBatchColumn& column) {
  u64 expected_size = 0;
  if (!BitmapByteCount(column.row_count, &expected_size)) {
    return Refuse("SB_RESULT_BATCH.INVALID_VALIDITY_BITMAP",
                  "result_batch.invalid_validity_bitmap",
                  "validity_bitmap_size_overflow", column.row_count,
                  column.layout);
  }
  if (static_cast<u64>(column.validity_bitmap.size()) != expected_size) {
    return Refuse("SB_RESULT_BATCH.INVALID_VALIDITY_BITMAP",
                  "result_batch.invalid_validity_bitmap",
                  "validity_bitmap_size_mismatch", column.row_count,
                  column.layout);
  }
  if (column.row_count % 8u != 0 && !column.validity_bitmap.empty()) {
    const std::uint8_t used_bits =
        static_cast<std::uint8_t>((1u << (column.row_count % 8u)) - 1u);
    const auto tail = column.validity_bitmap.back();
    if ((tail & ~used_bits) != 0) {
      return Refuse("SB_RESULT_BATCH.INVALID_VALIDITY_BITMAP",
                    "result_batch.invalid_validity_bitmap",
                    "validity_bitmap_padding_bits_set", column.row_count,
                    column.layout);
    }
  }
  VectorizedResultBatchResult ok;
  ok.status = OkStatus();
  return ok;
}

VectorizedResultBatchResult ValidateOptionalRedactionBitmap(
    const ResultBatchColumn& column) {
  if (column.redaction_bitmap.empty()) {
    return VectorizedResultBatchResult{OkStatus(), false, {}, {}, {}, {}};
  }
  u64 expected_size = 0;
  if (!BitmapByteCount(column.row_count, &expected_size)) {
    return Refuse("SB_RESULT_BATCH.INVALID_REDACTION_BITMAP",
                  "result_batch.invalid_redaction_bitmap",
                  "redaction_bitmap_size_overflow", column.row_count,
                  column.layout);
  }
  if (static_cast<u64>(column.redaction_bitmap.size()) != expected_size) {
    return Refuse("SB_RESULT_BATCH.INVALID_REDACTION_BITMAP",
                  "result_batch.invalid_redaction_bitmap",
                  "redaction_bitmap_size_mismatch", column.row_count,
                  column.layout);
  }
  if (column.row_count % 8u != 0 && !column.redaction_bitmap.empty()) {
    const std::uint8_t used_bits =
        static_cast<std::uint8_t>((1u << (column.row_count % 8u)) - 1u);
    const auto tail = column.redaction_bitmap.back();
    if ((tail & ~used_bits) != 0) {
      return Refuse("SB_RESULT_BATCH.INVALID_REDACTION_BITMAP",
                    "result_batch.invalid_redaction_bitmap",
                    "redaction_bitmap_padding_bits_set", column.row_count,
                    column.layout);
    }
  }
  return VectorizedResultBatchResult{OkStatus(), false, {}, {}, {}, {}};
}

VectorizedResultBatchResult ValidateOffsets(
    const ResultBatchColumn& column,
    u64 expected_last,
    const char* reason_prefix) {
  if (column.row_count == std::numeric_limits<u64>::max()) {
    return Refuse("SB_RESULT_BATCH.CORRUPT_OFFSETS",
                  "result_batch.corrupt_offsets",
                  std::string(reason_prefix) + "_offset_count_overflow",
                  column.row_count, column.layout);
  }
  const auto expected_count = column.row_count + 1u;
  if (expected_count >
          static_cast<u64>(std::numeric_limits<std::size_t>::max()) ||
      column.offsets.size() != static_cast<std::size_t>(expected_count)) {
    return Refuse("SB_RESULT_BATCH.CORRUPT_OFFSETS",
                  "result_batch.corrupt_offsets",
                  std::string(reason_prefix) + "_offset_count_mismatch",
                  column.row_count, column.layout);
  }
  if (column.offsets.empty() || column.offsets.front() != 0) {
    return Refuse("SB_RESULT_BATCH.CORRUPT_OFFSETS",
                  "result_batch.corrupt_offsets",
                  std::string(reason_prefix) + "_offset_zero_missing",
                  column.row_count, column.layout);
  }
  for (std::size_t i = 1; i < column.offsets.size(); ++i) {
    if (column.offsets[i] < column.offsets[i - 1]) {
      return Refuse("SB_RESULT_BATCH.CORRUPT_OFFSETS",
                    "result_batch.corrupt_offsets",
                    std::string(reason_prefix) + "_offsets_not_monotonic",
                    column.row_count, column.layout);
    }
  }
  if (column.offsets.back() != expected_last) {
    return Refuse("SB_RESULT_BATCH.CORRUPT_OFFSETS",
                  "result_batch.corrupt_offsets",
                  std::string(reason_prefix) + "_terminal_offset_mismatch",
                  column.row_count, column.layout);
  }
  VectorizedResultBatchResult ok;
  ok.status = OkStatus();
  return ok;
}

VectorizedResultBatchResult ValidateColumn(
    const ResultBatchColumn& column,
    std::vector<ResultBatchColumnDiagnostics>* diagnostics,
    std::vector<std::string>* evidence);

VectorizedResultBatchResult ValidateChildrenForStruct(
    const ResultBatchColumn& column,
    std::vector<ResultBatchColumnDiagnostics>* child_diagnostics,
    std::vector<std::string>* evidence) {
  if (column.children.empty()) {
    return Refuse("SB_RESULT_BATCH.STRUCT_CHILD_MISMATCH",
                  "result_batch.struct_child_mismatch",
                  "struct_view_requires_child_columns", column.row_count,
                  column.layout);
  }
  for (const auto& child : column.children) {
    if (child.row_count != column.row_count) {
      return Refuse("SB_RESULT_BATCH.STRUCT_CHILD_MISMATCH",
                    "result_batch.struct_child_mismatch",
                    "struct_child_row_count_mismatch", column.row_count,
                    column.layout);
    }
    auto child_result = ValidateColumn(child, child_diagnostics, evidence);
    if (!child_result.ok()) {
      return child_result;
    }
  }
  VectorizedResultBatchResult ok;
  ok.status = OkStatus();
  return ok;
}

VectorizedResultBatchResult ValidateChildrenForList(
    const ResultBatchColumn& column,
    std::vector<ResultBatchColumnDiagnostics>* child_diagnostics,
    std::vector<std::string>* evidence) {
  if (column.children.size() != 1u) {
    return Refuse("SB_RESULT_BATCH.LIST_CHILD_MISMATCH",
                  "result_batch.list_child_mismatch",
                  "list_view_requires_single_child_column", column.row_count,
                  column.layout);
  }
  const auto& child = column.children.front();
  const auto offsets = ValidateOffsets(column, child.row_count, "list");
  if (!offsets.ok()) {
    return offsets;
  }
  auto child_result = ValidateColumn(child, child_diagnostics, evidence);
  if (!child_result.ok()) {
    return child_result;
  }
  VectorizedResultBatchResult ok;
  ok.status = OkStatus();
  return ok;
}

VectorizedResultBatchResult ValidateColumn(
    const ResultBatchColumn& column,
    std::vector<ResultBatchColumnDiagnostics>* diagnostics,
    std::vector<std::string>* evidence) {
  if (column.name.empty()) {
    return Refuse("SB_RESULT_BATCH.COLUMN_NAME_REQUIRED",
                  "result_batch.column_name_required",
                  "column_name_required", column.row_count, column.layout);
  }
  if (column.layout == ResultBatchLayoutKind::unknown) {
    return Refuse("SB_RESULT_BATCH.UNSUPPORTED_LAYOUT",
                  "result_batch.unsupported_layout",
                  "unsupported_layout_kind", column.row_count, column.layout);
  }
  const auto validity = ValidateValidityBitmap(column);
  if (!validity.ok()) {
    return validity;
  }
  const auto redaction = ValidateOptionalRedactionBitmap(column);
  if (!redaction.ok()) {
    return redaction;
  }

  ResultBatchColumnDiagnostics diagnostic;
  diagnostic.name = column.name;
  diagnostic.layout = column.layout;
  diagnostic.row_count = column.row_count;
  diagnostic.null_count = NullCount(column.validity_bitmap, column.row_count);
  diagnostic.redaction_count =
      column.redaction_bitmap.empty()
          ? 0
          : SetBitCount(column.redaction_bitmap, column.row_count);
  diagnostic.validity_byte_count =
      static_cast<u64>(column.validity_bitmap.size());
  diagnostic.redaction_byte_count =
      static_cast<u64>(column.redaction_bitmap.size());

  std::vector<ResultBatchColumnDiagnostics> child_diagnostics;

  switch (column.layout) {
    case ResultBatchLayoutKind::fixed_width: {
      if (column.fixed_width_bytes == 0) {
        return Refuse("SB_RESULT_BATCH.FIXED_WIDTH_INVALID",
                      "result_batch.fixed_width_invalid",
                      "fixed_width_byte_width_required", column.row_count,
                      column.layout);
      }
      if (column.row_count >
          std::numeric_limits<u64>::max() / column.fixed_width_bytes) {
        return Refuse("SB_RESULT_BATCH.FIXED_WIDTH_INVALID",
                      "result_batch.fixed_width_invalid",
                      "fixed_width_size_overflow", column.row_count,
                      column.layout);
      }
      const auto expected = column.row_count * column.fixed_width_bytes;
      if (column.fixed_width_data.size() != expected) {
        return Refuse("SB_RESULT_BATCH.FIXED_WIDTH_INVALID",
                      "result_batch.fixed_width_invalid",
                      "fixed_width_data_size_mismatch", column.row_count,
                      column.layout);
      }
      diagnostic.encoded_byte_count =
          diagnostic.validity_byte_count +
          static_cast<u64>(column.fixed_width_data.size());
      break;
    }
    case ResultBatchLayoutKind::variable_width: {
      const auto offsets =
          ValidateOffsets(column, column.variable_data.size(), "variable");
      if (!offsets.ok()) {
        return offsets;
      }
      diagnostic.encoded_byte_count =
          diagnostic.validity_byte_count +
          static_cast<u64>(column.offsets.size() * sizeof(u64)) +
          static_cast<u64>(column.variable_data.size());
      break;
    }
    case ResultBatchLayoutKind::dictionary: {
      if (column.dictionary_ids.size() != column.row_count) {
        return Refuse("SB_RESULT_BATCH.DICTIONARY_ID_INVALID",
                      "result_batch.dictionary_id_invalid",
                      "dictionary_id_count_mismatch", column.row_count,
                      column.layout);
      }
      if (column.dictionary_values.empty()) {
        return Refuse("SB_RESULT_BATCH.DICTIONARY_ID_INVALID",
                      "result_batch.dictionary_id_invalid",
                      "dictionary_values_required", column.row_count,
                      column.layout);
      }
      for (const auto id : column.dictionary_ids) {
        if (id >= column.dictionary_values.size()) {
          return Refuse("SB_RESULT_BATCH.DICTIONARY_ID_INVALID",
                        "result_batch.dictionary_id_invalid",
                        "dictionary_id_out_of_range", column.row_count,
                        column.layout);
        }
      }
      diagnostic.dictionary_value_count =
          static_cast<u64>(column.dictionary_values.size());
      diagnostic.encoded_byte_count =
          diagnostic.validity_byte_count +
          static_cast<u64>(column.dictionary_ids.size() * sizeof(u32)) +
          StringBytes(column.dictionary_values);
      break;
    }
    case ResultBatchLayoutKind::run_end: {
      if (column.run_ends.empty() ||
          column.run_ends.size() != column.run_values.size()) {
        return Refuse("SB_RESULT_BATCH.RUN_END_INVALID",
                      "result_batch.run_end_invalid",
                      "run_end_count_mismatch", column.row_count,
                      column.layout);
      }
      u64 previous = 0;
      for (const auto run_end : column.run_ends) {
        if (run_end <= previous || run_end > column.row_count) {
          return Refuse("SB_RESULT_BATCH.RUN_END_INVALID",
                        "result_batch.run_end_invalid",
                        "run_end_range_malformed", column.row_count,
                        column.layout);
        }
        previous = run_end;
      }
      if (column.run_ends.back() != column.row_count) {
        return Refuse("SB_RESULT_BATCH.RUN_END_INVALID",
                      "result_batch.run_end_invalid",
                      "run_end_terminal_row_mismatch", column.row_count,
                      column.layout);
      }
      diagnostic.run_count = static_cast<u64>(column.run_ends.size());
      diagnostic.encoded_byte_count =
          diagnostic.validity_byte_count +
          static_cast<u64>(column.run_ends.size() * sizeof(u64)) +
          StringBytes(column.run_values);
      break;
    }
    case ResultBatchLayoutKind::struct_view: {
      auto children =
          ValidateChildrenForStruct(column, &child_diagnostics, evidence);
      if (!children.ok()) {
        return children;
      }
      diagnostic.child_count = static_cast<u64>(column.children.size());
      diagnostic.encoded_byte_count =
          diagnostic.validity_byte_count +
          ChildEncodedBytes(child_diagnostics);
      break;
    }
    case ResultBatchLayoutKind::list_view: {
      auto child = ValidateChildrenForList(column, &child_diagnostics, evidence);
      if (!child.ok()) {
        return child;
      }
      diagnostic.child_count = 1;
      diagnostic.list_value_count = column.offsets.back();
      diagnostic.encoded_byte_count =
          diagnostic.validity_byte_count +
          static_cast<u64>(column.offsets.size() * sizeof(u64)) +
          ChildEncodedBytes(child_diagnostics);
      break;
    }
    case ResultBatchLayoutKind::unknown:
      break;
  }

  diagnostics->insert(diagnostics->end(), child_diagnostics.begin(),
                      child_diagnostics.end());
  diagnostics->push_back(diagnostic);
  evidence->push_back("column.name=" + column.name);
  evidence->push_back("column.layout_kind=" +
                      std::string(ResultBatchLayoutKindName(column.layout)));
  evidence->push_back(CountEvidence("column.row_count", column.row_count));
  evidence->push_back(CountEvidence("column.null_count",
                                    diagnostic.null_count));
  evidence->push_back(CountEvidence("column.redaction_count",
                                    diagnostic.redaction_count));
  evidence->push_back(CountEvidence("column.encoded_byte_count",
                                    diagnostic.encoded_byte_count));
  evidence->push_back(CountEvidence("column.validity_byte_count",
                                    diagnostic.validity_byte_count));
  evidence->push_back(CountEvidence("column.redaction_byte_count",
                                    diagnostic.redaction_byte_count));
  evidence->push_back(CountEvidence("column.dictionary_value_count",
                                    diagnostic.dictionary_value_count));
  evidence->push_back(CountEvidence("column.run_count",
                                    diagnostic.run_count));
  evidence->push_back(CountEvidence("column.child_count",
                                    diagnostic.child_count));
  evidence->push_back(CountEvidence("column.list_value_count",
                                    diagnostic.list_value_count));
  AppendEvidence(evidence, column.children.empty() ? std::vector<std::string>{}
                                                  : std::vector<std::string>{
                                                        "column.children.validated=true"});
  return VectorizedResultBatchResult{OkStatus(), false, {}, {}, {}, {}};
}

}  // namespace

const char* ResultBatchLayoutKindName(ResultBatchLayoutKind layout) {
  switch (layout) {
    case ResultBatchLayoutKind::fixed_width:
      return "fixed_width";
    case ResultBatchLayoutKind::variable_width:
      return "variable_width";
    case ResultBatchLayoutKind::dictionary:
      return "dictionary";
    case ResultBatchLayoutKind::run_end:
      return "run_end";
    case ResultBatchLayoutKind::struct_view:
      return "struct_view";
    case ResultBatchLayoutKind::list_view:
      return "list_view";
    case ResultBatchLayoutKind::unknown:
      break;
  }
  return "unknown";
}

std::vector<std::uint8_t> MakeResultBatchValidityBitmap(
    u64 row_count,
    const std::vector<u64>& null_ordinals) {
  u64 byte_count = 0;
  if (!BitmapByteCount(row_count, &byte_count)) {
    return {};
  }
  std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(byte_count),
                                   0xffu);
  if (row_count % 8u != 0 && !bitmap.empty()) {
    bitmap.back() = static_cast<std::uint8_t>((1u << (row_count % 8u)) - 1u);
  }
  for (const auto ordinal : null_ordinals) {
    if (ordinal >= row_count) {
      continue;
    }
    bitmap[static_cast<std::size_t>(ordinal / 8u)] &=
        static_cast<std::uint8_t>(~(1u << (ordinal % 8u)));
  }
  return bitmap;
}

std::vector<std::uint8_t> MakeResultBatchRedactionBitmap(
    u64 row_count,
    const std::vector<u64>& redacted_ordinals) {
  u64 byte_count = 0;
  if (!BitmapByteCount(row_count, &byte_count)) {
    return {};
  }
  std::vector<std::uint8_t> bitmap(static_cast<std::size_t>(byte_count), 0u);
  for (const auto ordinal : redacted_ordinals) {
    if (ordinal >= row_count) {
      continue;
    }
    bitmap[static_cast<std::size_t>(ordinal / 8u)] |=
        static_cast<std::uint8_t>(1u << (ordinal % 8u));
  }
  return bitmap;
}

ResultBatchColumn MakeFixedWidthResultBatchColumn(
    std::string name,
    u64 row_count,
    u64 fixed_width_bytes,
    std::vector<std::uint8_t> data,
    std::vector<std::uint8_t> validity_bitmap) {
  ResultBatchColumn column;
  column.name = std::move(name);
  column.layout = ResultBatchLayoutKind::fixed_width;
  column.row_count = row_count;
  column.fixed_width_bytes = fixed_width_bytes;
  column.fixed_width_data = std::move(data);
  column.validity_bitmap = std::move(validity_bitmap);
  return column;
}

ResultBatchColumn MakeVariableWidthResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u64> offsets,
    std::vector<std::uint8_t> data,
    std::vector<std::uint8_t> validity_bitmap) {
  ResultBatchColumn column;
  column.name = std::move(name);
  column.layout = ResultBatchLayoutKind::variable_width;
  column.row_count = row_count;
  column.offsets = std::move(offsets);
  column.variable_data = std::move(data);
  column.validity_bitmap = std::move(validity_bitmap);
  return column;
}

ResultBatchColumn MakeDictionaryResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u32> dictionary_ids,
    std::vector<std::string> dictionary_values,
    std::vector<std::uint8_t> validity_bitmap) {
  ResultBatchColumn column;
  column.name = std::move(name);
  column.layout = ResultBatchLayoutKind::dictionary;
  column.row_count = row_count;
  column.dictionary_ids = std::move(dictionary_ids);
  column.dictionary_values = std::move(dictionary_values);
  column.validity_bitmap = std::move(validity_bitmap);
  return column;
}

ResultBatchColumn MakeRunEndResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u64> run_ends,
    std::vector<std::string> run_values,
    std::vector<std::uint8_t> validity_bitmap) {
  ResultBatchColumn column;
  column.name = std::move(name);
  column.layout = ResultBatchLayoutKind::run_end;
  column.row_count = row_count;
  column.run_ends = std::move(run_ends);
  column.run_values = std::move(run_values);
  column.validity_bitmap = std::move(validity_bitmap);
  return column;
}

ResultBatchColumn MakeStructViewResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<ResultBatchColumn> children,
    std::vector<std::uint8_t> validity_bitmap) {
  ResultBatchColumn column;
  column.name = std::move(name);
  column.layout = ResultBatchLayoutKind::struct_view;
  column.row_count = row_count;
  column.children = std::move(children);
  column.validity_bitmap = std::move(validity_bitmap);
  return column;
}

ResultBatchColumn MakeListViewResultBatchColumn(
    std::string name,
    u64 row_count,
    std::vector<u64> offsets,
    ResultBatchColumn child,
    std::vector<std::uint8_t> validity_bitmap) {
  ResultBatchColumn column;
  column.name = std::move(name);
  column.layout = ResultBatchLayoutKind::list_view;
  column.row_count = row_count;
  column.offsets = std::move(offsets);
  column.children.push_back(std::move(child));
  column.validity_bitmap = std::move(validity_bitmap);
  return column;
}

VectorizedResultBatchBuilder::VectorizedResultBatchBuilder(u64 row_count)
    : row_count_(row_count) {}

void VectorizedResultBatchBuilder::AddColumn(ResultBatchColumn column) {
  columns_.push_back(std::move(column));
}

VectorizedResultBatchResult VectorizedResultBatchBuilder::Finalize() const {
  return FinalizeVectorizedResultBatch(row_count_, columns_);
}

VectorizedResultBatchResult FinalizeVectorizedResultBatch(
    u64 row_count,
    std::vector<ResultBatchColumn> columns) {
  if (columns.empty()) {
    return Refuse("SB_RESULT_BATCH.COLUMN_REQUIRED",
                  "result_batch.column_required",
                  "result_batch_requires_columns", row_count,
                  ResultBatchLayoutKind::unknown);
  }

  std::set<std::string> names;
  std::vector<ResultBatchColumnDiagnostics> diagnostics;
  std::vector<std::string> evidence;
  evidence.push_back("result_batch.data_transport_only=true");
  evidence.push_back("result_batch.row_order_preserved=true");
  evidence.push_back("result_batch.finality_authority=false");
  evidence.push_back("result_batch.visibility_authority=false");
  evidence.push_back("result_batch.parser_execution_authority=false");
  evidence.push_back("result_batch.provider_authority=false");
  evidence.push_back("result_batch.client_authority=false");
  evidence.push_back("result_batch.recovery_authority=false");
  evidence.push_back(CountEvidence("result_batch.row_count", row_count));
  evidence.push_back(CountEvidence("result_batch.column_count",
                                   static_cast<u64>(columns.size())));

  for (const auto& column : columns) {
    if (column.row_count != row_count) {
      return Refuse("SB_RESULT_BATCH.ROW_COUNT_MISMATCH",
                    "result_batch.row_count_mismatch",
                    "column_row_count_mismatch", row_count, column.layout);
    }
    if (!names.insert(column.name).second) {
      return Refuse("SB_RESULT_BATCH.COLUMN_NAME_DUPLICATE",
                    "result_batch.column_name_duplicate",
                    "column_name_duplicate", row_count, column.layout);
    }
    auto column_result = ValidateColumn(column, &diagnostics, &evidence);
    if (!column_result.ok()) {
      return column_result;
    }
  }

  VectorizedResultBatchResult result;
  result.status = OkStatus();
  result.batch.row_count = row_count;
  result.batch.columns = std::move(columns);
  result.batch.column_diagnostics = std::move(diagnostics);
  result.batch.evidence = evidence;
  result.evidence = std::move(evidence);
  result.diagnostic = MakeVectorizedResultBatchDiagnostic(
      result.status, "SB_RESULT_BATCH.OK", "result_batch.ok",
      "vectorized_result_batch_finalized");
  return result;
}

VectorizedResultBatchResult FinalizeVectorizedResultBatchFromReservedResource(
    scratchbird::core::memory::ReservationBackedMemoryResource* resource,
    u64 row_count,
    std::vector<ResultBatchColumn> columns,
    bool engine_mga_authoritative,
    bool parser_or_donor_authority,
    bool memory_finality_or_visibility_authority,
    bool debug_or_relaxed_path) {
  if (resource == nullptr || !resource->active()) {
    return Refuse("SB_RESULT_BATCH.RESERVED_RESOURCE_REQUIRED",
                  "result_batch.reserved_resource_required",
                  "active_reservation_backed_resource_required",
                  row_count,
                  ResultBatchLayoutKind::unknown);
  }
  if (!engine_mga_authoritative || parser_or_donor_authority ||
      memory_finality_or_visibility_authority || debug_or_relaxed_path) {
    return Refuse("SB_RESULT_BATCH.RESERVED_RESOURCE_UNSAFE_AUTHORITY",
                  "result_batch.reserved_resource_unsafe_authority",
                  "engine_mga_required_and_parser_donor_memory_finality_debug_authority_refused",
                  row_count,
                  ResultBatchLayoutKind::unknown);
  }

  scratchbird::core::memory::ReservationBackedMemoryAllocationRequest allocation;
  allocation.bytes = EstimateResultFrameScratchBytes(columns);
  allocation.alignment = alignof(std::max_align_t);
  allocation.purpose = "executor.result_frame_scratch";
  const auto scratch = resource->Allocate(std::move(allocation));
  if (!scratch.ok()) {
    VectorizedResultBatchResult result;
    result.status = scratch.status;
    result.fail_closed = true;
    result.diagnostic = scratch.diagnostic;
    result.evidence.push_back("result_batch.fail_closed=true");
    result.evidence.push_back("result_batch.refused=reserved_allocation_refused");
    return result;
  }

  auto result = FinalizeVectorizedResultBatch(row_count, std::move(columns));
  result.evidence.push_back("CEIC-012_QUERY_OPERATOR_PLANNER_PARSER_ARENAS");
  result.evidence.push_back("result_batch.reserved_resource_passed=true");
  result.evidence.push_back("result_batch.after_reservation=true");
  result.evidence.push_back("result_batch.reserved_scratch_bytes=" +
                            std::to_string(scratch.bytes));
  result.evidence.push_back(
      "result_batch.authority_scope=data_transport_only_not_transaction_finality_visibility_recovery_parser_donor_benchmark_cluster_optimizer_index_or_agent_authority");
  if (result.ok()) {
    result.batch.evidence = result.evidence;
  }
  return result;
}

DiagnosticRecord MakeVectorizedResultBatchDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = "engine.executor.result_batch";
  diagnostic.remediation_hint = std::move(detail);
  if (!diagnostic.remediation_hint.empty()) {
    diagnostic.arguments.push_back(
        DiagnosticArgument{"detail", diagnostic.remediation_hint});
  }
  return diagnostic;
}

}  // namespace scratchbird::engine::executor
