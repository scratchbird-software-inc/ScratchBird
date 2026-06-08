// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "result_batch_transfer.hpp"

#include <sstream>
#include <utility>

namespace scratchbird::wire {
namespace {

using scratchbird::core::platform::DiagnosticArgument;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::engine::executor::FinalizeVectorizedResultBatch;
using scratchbird::engine::executor::ResultBatchColumn;
using scratchbird::engine::executor::ResultBatchLayoutKindName;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

DiagnosticRecord MakeDiagnostic(Status status,
                                std::string diagnostic_code,
                                std::string message_key,
                                std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  diagnostic.source_component = "wire.result_batch_transfer";
  diagnostic.remediation_hint = std::move(detail);
  if (!diagnostic.remediation_hint.empty()) {
    diagnostic.arguments.push_back(
        DiagnosticArgument{"detail", diagnostic.remediation_hint});
  }
  return diagnostic;
}

ResultBatchTransferResult Refuse(std::string reason) {
  ResultBatchTransferResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_RESULT_BATCH_TRANSFER.INVALID_BATCH",
                     "result_batch_transfer.invalid_batch", reason);
  result.refusal_reasons.push_back(reason);
  result.evidence.push_back("transfer_descriptor.version=" +
                            std::to_string(kResultBatchTransferDescriptorVersion));
  result.evidence.push_back("transfer_descriptor.data_transport_only=true");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

std::string CountEvidence(std::string key, u64 value) {
  return std::move(key) + "=" + std::to_string(value);
}

u64 EncodedBytesFromChildren(
    const std::vector<ResultBatchColumnTransferDescriptor>& children) {
  u64 bytes = 0;
  for (const auto& child : children) {
    bytes += child.encoded_byte_count;
  }
  return bytes;
}

u64 StringBytes(const std::vector<std::string>& values) {
  u64 bytes = 0;
  for (const auto& value : values) {
    bytes += static_cast<u64>(value.size());
  }
  return bytes;
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

ResultBatchColumnTransferDescriptor DescribeColumn(const ResultBatchColumn& column) {
  ResultBatchColumnTransferDescriptor descriptor;
  descriptor.name = column.name;
  descriptor.layout = column.layout;
  descriptor.row_count = column.row_count;
  descriptor.null_count = NullCount(column.validity_bitmap, column.row_count);
  descriptor.validity_byte_count = column.validity_bitmap.size();
  descriptor.fixed_width_bytes = column.fixed_width_bytes;
  descriptor.offset_count = column.offsets.size();
  descriptor.dictionary_value_count = column.dictionary_values.size();
  descriptor.run_count = column.run_ends.size();
  descriptor.child_count = column.children.size();
  descriptor.list_value_count =
      !column.offsets.empty() ? column.offsets.back() : 0;
  for (const auto& child : column.children) {
    descriptor.children.push_back(DescribeColumn(child));
  }
  switch (column.layout) {
    case scratchbird::engine::executor::ResultBatchLayoutKind::fixed_width:
      descriptor.encoded_byte_count =
          descriptor.validity_byte_count +
          static_cast<u64>(column.fixed_width_data.size());
      break;
    case scratchbird::engine::executor::ResultBatchLayoutKind::variable_width:
      descriptor.encoded_byte_count =
          descriptor.validity_byte_count +
          static_cast<u64>(column.offsets.size() * sizeof(u64)) +
          static_cast<u64>(column.variable_data.size());
      break;
    case scratchbird::engine::executor::ResultBatchLayoutKind::dictionary:
      descriptor.encoded_byte_count =
          descriptor.validity_byte_count +
          static_cast<u64>(column.dictionary_ids.size() * sizeof(u32)) +
          StringBytes(column.dictionary_values);
      break;
    case scratchbird::engine::executor::ResultBatchLayoutKind::run_end:
      descriptor.encoded_byte_count =
          descriptor.validity_byte_count +
          static_cast<u64>(column.run_ends.size() * sizeof(u64)) +
          StringBytes(column.run_values);
      break;
    case scratchbird::engine::executor::ResultBatchLayoutKind::struct_view:
      descriptor.encoded_byte_count =
          descriptor.validity_byte_count + EncodedBytesFromChildren(descriptor.children);
      break;
    case scratchbird::engine::executor::ResultBatchLayoutKind::list_view:
      descriptor.encoded_byte_count =
          descriptor.validity_byte_count +
          static_cast<u64>(column.offsets.size() * sizeof(u64)) +
          EncodedBytesFromChildren(descriptor.children);
      break;
    case scratchbird::engine::executor::ResultBatchLayoutKind::unknown:
      break;
  }
  return descriptor;
}

void AppendDescriptorText(const ResultBatchColumnTransferDescriptor& column,
                          std::ostringstream* out) {
  *out << "column=" << column.name
       << ";layout=" << ResultBatchLayoutKindName(column.layout)
       << ";rows=" << column.row_count << ";nulls=" << column.null_count
       << ";bytes=" << column.encoded_byte_count
       << ";validity=" << column.validity_byte_count
       << ";dictionary=" << column.dictionary_value_count
       << ";runs=" << column.run_count << ";children=" << column.child_count
       << ";list_values=" << column.list_value_count << '\n';
  for (const auto& child : column.children) {
    AppendDescriptorText(child, out);
  }
}

}  // namespace

ResultBatchTransferResult BuildResultBatchTransferDescriptor(
    const VectorizedResultBatch& batch) {
  if (batch.columns.empty()) {
    return Refuse("batch_has_no_columns");
  }
  if (batch.column_diagnostics.size() < batch.columns.size()) {
    return Refuse("batch_missing_executor_column_diagnostics");
  }
  auto revalidated = FinalizeVectorizedResultBatch(batch.row_count, batch.columns);
  if (!revalidated.ok()) {
    const auto reason = revalidated.refusal_reasons.empty()
                            ? std::string("unknown")
                            : revalidated.refusal_reasons.front();
    return Refuse("batch_revalidation_failed:" + reason);
  }
  const auto& checked_batch = revalidated.batch;

  ResultBatchTransferResult result;
  result.status = OkStatus();
  result.descriptor.version = kResultBatchTransferDescriptorVersion;
  result.descriptor.row_count = checked_batch.row_count;
  result.descriptor.column_count = checked_batch.columns.size();
  result.evidence.push_back("transfer_descriptor.version=" +
                            std::to_string(result.descriptor.version));
  result.evidence.push_back("transfer_descriptor.data_transport_only=true");
  result.evidence.push_back("transfer_descriptor.row_order_preserved=true");
  result.evidence.push_back(CountEvidence("transfer_descriptor.row_count",
                                          checked_batch.row_count));
  result.evidence.push_back(CountEvidence("transfer_descriptor.column_count",
                                          result.descriptor.column_count));

  for (const auto& column : checked_batch.columns) {
    auto descriptor = DescribeColumn(column);
    result.descriptor.encoded_byte_count += descriptor.encoded_byte_count;
    result.evidence.push_back("transfer_descriptor.column.layout_kind=" +
                              std::string(ResultBatchLayoutKindName(column.layout)));
    result.evidence.push_back(CountEvidence(
        "transfer_descriptor.column.encoded_byte_count",
        descriptor.encoded_byte_count));
    result.evidence.push_back(CountEvidence(
        "transfer_descriptor.column.null_count", descriptor.null_count));
    result.evidence.push_back(CountEvidence(
        "transfer_descriptor.column.dictionary_value_count",
        descriptor.dictionary_value_count));
    result.evidence.push_back(CountEvidence(
        "transfer_descriptor.column.run_count", descriptor.run_count));
    result.evidence.push_back(CountEvidence(
        "transfer_descriptor.column.child_count", descriptor.child_count));
    result.evidence.push_back(CountEvidence(
        "transfer_descriptor.column.list_value_count",
        descriptor.list_value_count));
    result.descriptor.columns.push_back(std::move(descriptor));
  }
  result.evidence.push_back(CountEvidence("transfer_descriptor.encoded_byte_count",
                                          result.descriptor.encoded_byte_count));
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_RESULT_BATCH_TRANSFER.OK",
                     "result_batch_transfer.ok",
                     "result_batch_transfer_descriptor_ready");
  return result;
}

std::vector<std::uint8_t> SerializeResultBatchTransferDescriptor(
    const ResultBatchTransferDescriptor& descriptor) {
  std::ostringstream out;
  out << "SB_RESULT_BATCH_TRANSFER_V" << descriptor.version << '\n';
  out << "rows=" << descriptor.row_count
      << ";columns=" << descriptor.column_count
      << ";bytes=" << descriptor.encoded_byte_count << '\n';
  for (const auto& column : descriptor.columns) {
    AppendDescriptorText(column, &out);
  }
  const auto text = out.str();
  return {text.begin(), text.end()};
}

const char* ResultBatchTransferDescriptorFieldVersion() {
  return "transfer_descriptor.version";
}

}  // namespace scratchbird::wire
