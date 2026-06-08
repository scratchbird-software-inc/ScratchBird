// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "direct_binary_result_frame.hpp"

#include "compression_policy.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace scratchbird::wire {
namespace {

namespace index = scratchbird::core::index;

using scratchbird::core::platform::DiagnosticArgument;
namespace platform = scratchbird::core::platform;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
using scratchbird::engine::executor::FinalizeVectorizedResultBatch;
using scratchbird::engine::executor::ResultBatchColumn;
using scratchbird::engine::executor::ResultBatchLayoutKindName;

constexpr std::array<std::uint8_t, 8> kMagic{{'S', 'B', 'D', 'B',
                                              'F', 'R', 'M', '1'}};
constexpr u32 kHeaderSize = 64;

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
  diagnostic.source_component = "wire.direct_binary_result_frame";
  diagnostic.remediation_hint = std::move(detail);
  if (!diagnostic.remediation_hint.empty()) {
    diagnostic.arguments.push_back(
        DiagnosticArgument{"detail", diagnostic.remediation_hint});
  }
  return diagnostic;
}

DirectBinaryResultFrameResult Refuse(std::string reason) {
  DirectBinaryResultFrameResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic = MakeDiagnostic(
      result.status, "SB_DIRECT_BINARY_RESULT_FRAME.INVALID",
      "direct_binary_result_frame.invalid", reason);
  result.refusal_reasons.push_back(reason);
  result.evidence.push_back("direct_binary_frame.version=" +
                            std::to_string(kDirectBinaryResultFrameVersion));
  result.evidence.push_back("direct_binary_frame.data_transport_only=true");
  result.evidence.push_back("direct_binary_frame.row_order_preserved=true");
  result.evidence.push_back("direct_binary_frame.finality_authority=false");
  result.evidence.push_back("direct_binary_frame.visibility_authority=false");
  result.evidence.push_back("direct_binary_frame.parser_authority=false");
  result.evidence.push_back("direct_binary_frame.client_authority=false");
  result.evidence.push_back("direct_binary_frame.provider_authority=false");
  result.evidence.push_back("direct_binary_frame.security_authority=false");
  result.evidence.push_back("direct_binary_frame.mga_authority=false");
  result.evidence.push_back("direct_binary_frame.wal_authority=false");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  return result;
}

platform::RuntimeCompatibilityDescriptor DirectBinaryFrameCompatibility(
    platform::RuntimeCompatibilityDescriptor descriptor = {}) {
  const bool explicit_descriptor = !descriptor.route_id.empty();
  if (descriptor.route_id.empty()) {
    descriptor = platform::CurrentRuntimeCompatibilityDescriptor(
        "wire.direct_binary_result_frame");
  }
  descriptor.route_id = descriptor.route_id.empty()
                            ? "wire.direct_binary_result_frame"
                            : descriptor.route_id;
  descriptor.source_component = "wire.direct_binary_result_frame";
  descriptor.required_endian = platform::RuntimeEndian::little;
  if (!explicit_descriptor ||
      descriptor.provider_endian == platform::RuntimeEndian::unknown) {
    descriptor.provider_endian = platform::RuntimeEndian::little;
  }
  if (descriptor.required_alignment == 0) {
    descriptor.required_alignment = 8;
  }
  if (descriptor.provider_alignment == 0) {
    descriptor.provider_alignment = 8;
  }
  descriptor.deterministic_scalar_fallback_required = false;
  descriptor.deterministic_scalar_fallback_available = false;
  descriptor.fail_closed_on_mismatch = true;
  return descriptor;
}

void AppendCompatibilityEvidence(
    DirectBinaryResultFrameResult* result,
    const platform::RuntimeCompatibilityResult& compatibility) {
  result->evidence.insert(result->evidence.end(),
                          compatibility.evidence.begin(),
                          compatibility.evidence.end());
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

void SetBit(std::vector<std::uint8_t>* bitmap, u64 ordinal) {
  (*bitmap)[static_cast<std::size_t>(ordinal / 8u)] |=
      static_cast<std::uint8_t>(1u << (ordinal % 8u));
}

std::vector<std::uint8_t> SliceBitmapRange(
    const std::vector<std::uint8_t>& bitmap,
    u64 start_row,
    u64 row_count) {
  if (bitmap.empty()) {
    return {};
  }
  u64 byte_count = 0;
  if (!BitmapByteCount(row_count, &byte_count)) {
    return {};
  }
  std::vector<std::uint8_t> out(static_cast<std::size_t>(byte_count), 0u);
  for (u64 row = 0; row < row_count; ++row) {
    if (BitSet(bitmap, start_row + row)) {
      SetBit(&out, row);
    }
  }
  return out;
}

u64 ClearBitCount(const std::vector<std::uint8_t>& bitmap, u64 row_count) {
  u64 count = 0;
  for (u64 row = 0; row < row_count; ++row) {
    if (!BitSet(bitmap, row)) {
      ++count;
    }
  }
  return count;
}

u64 SetBitCount(const std::vector<std::uint8_t>& bitmap, u64 row_count) {
  u64 count = 0;
  for (u64 row = 0; row < row_count; ++row) {
    if (BitSet(bitmap, row)) {
      ++count;
    }
  }
  return count;
}

bool BitmapPaddingValid(const std::vector<std::uint8_t>& bitmap,
                        u64 row_count) {
  if (row_count % 8u == 0 || bitmap.empty()) {
    return true;
  }
  const std::uint8_t used_bits =
      static_cast<std::uint8_t>((1u << (row_count % 8u)) - 1u);
  return (bitmap.back() & ~used_bits) == 0;
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xffu));
  out->push_back(static_cast<std::uint8_t>((value >> 8u) & 0xffu));
}

void AppendU32(std::vector<std::uint8_t>* out, u32 value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

void AppendU64(std::vector<std::uint8_t>* out, u64 value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out->push_back(static_cast<std::uint8_t>((value >> shift) & 0xffu));
  }
}

bool ReadU16(const std::vector<std::uint8_t>& bytes,
             std::size_t* offset,
             std::uint16_t* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < 2u) {
    return false;
  }
  *value = static_cast<std::uint16_t>(bytes[*offset]) |
           static_cast<std::uint16_t>(bytes[*offset + 1u] << 8u);
  *offset += 2u;
  return true;
}

bool ReadU32(const std::vector<std::uint8_t>& bytes,
             std::size_t* offset,
             u32* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < 4u) {
    return false;
  }
  u32 result = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    result |= static_cast<u32>(bytes[(*offset)++]) << shift;
  }
  *value = result;
  return true;
}

bool ReadU64(const std::vector<std::uint8_t>& bytes,
             std::size_t* offset,
             u64* value) {
  if (*offset > bytes.size() || bytes.size() - *offset < 8u) {
    return false;
  }
  u64 result = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    result |= static_cast<u64>(bytes[(*offset)++]) << shift;
  }
  *value = result;
  return true;
}

void AppendBytes(std::vector<std::uint8_t>* out,
                 const std::vector<std::uint8_t>& bytes) {
  out->insert(out->end(), bytes.begin(), bytes.end());
}

void AppendStringPayload(std::vector<std::uint8_t>* out,
                         std::string_view value) {
  AppendU64(out, static_cast<u64>(value.size()));
  out->insert(out->end(), value.begin(), value.end());
}

bool AddRange(std::vector<std::uint8_t>* payload,
              const std::vector<std::uint8_t>& bytes,
              u64* offset,
              u64* length) {
  *offset = static_cast<u64>(payload->size());
  *length = static_cast<u64>(bytes.size());
  if (*offset > std::numeric_limits<u64>::max() - *length) {
    return false;
  }
  AppendBytes(payload, bytes);
  return true;
}

std::vector<std::uint8_t> NormalizedRedactionBitmap(
    const ResultBatchColumn& column) {
  if (!column.redaction_bitmap.empty()) {
    return column.redaction_bitmap;
  }
  u64 byte_count = 0;
  if (!BitmapByteCount(column.row_count, &byte_count)) {
    return {};
  }
  return std::vector<std::uint8_t>(static_cast<std::size_t>(byte_count), 0u);
}

std::vector<std::uint8_t> FixedWidthPayload(const ResultBatchColumn& column,
                                            const std::vector<std::uint8_t>& redaction) {
  auto data = column.fixed_width_data;
  const auto width = static_cast<std::size_t>(column.fixed_width_bytes);
  for (u64 row = 0; row < column.row_count; ++row) {
    if (BitSet(redaction, row)) {
      auto begin = data.begin() + static_cast<std::ptrdiff_t>(row * width);
      std::fill(begin, begin + static_cast<std::ptrdiff_t>(width), 0u);
    }
  }
  return data;
}

std::vector<std::uint8_t> OffsetDataPayload(
    const ResultBatchColumn& column,
    const std::vector<std::uint8_t>& redaction) {
  std::vector<std::uint8_t> out;
  for (const auto offset : column.offsets) {
    AppendU64(&out, offset);
  }
  auto data = column.variable_data;
  for (u64 row = 0; row < column.row_count; ++row) {
    if (!BitSet(redaction, row)) {
      continue;
    }
    const auto begin = static_cast<std::size_t>(column.offsets[row]);
    const auto end = static_cast<std::size_t>(column.offsets[row + 1u]);
    std::fill(data.begin() + static_cast<std::ptrdiff_t>(begin),
              data.begin() + static_cast<std::ptrdiff_t>(end), 0u);
  }
  AppendBytes(&out, data);
  return out;
}

std::vector<std::uint8_t> DictionaryPayload(
    const ResultBatchColumn& column,
    const std::vector<std::uint8_t>& redaction) {
  std::set<u32> visible_ids;
  for (u64 row = 0; row < column.row_count; ++row) {
    if (!BitSet(redaction, row)) {
      visible_ids.insert(column.dictionary_ids[static_cast<std::size_t>(row)]);
    }
  }

  std::vector<std::uint8_t> out;
  for (u64 row = 0; row < column.row_count; ++row) {
    const u32 id = BitSet(redaction, row)
                       ? 0u
                       : column.dictionary_ids[static_cast<std::size_t>(row)];
    AppendU32(&out, id);
  }
  for (u32 id = 0; id < column.dictionary_values.size(); ++id) {
    if (visible_ids.count(id) == 0u) {
      AppendStringPayload(&out, {});
    } else {
      AppendStringPayload(&out, column.dictionary_values[id]);
    }
  }
  return out;
}

std::vector<std::uint8_t> RunEndPayload(
    const ResultBatchColumn& column,
    const std::vector<std::uint8_t>& redaction) {
  std::vector<std::uint8_t> out;
  for (const auto run_end : column.run_ends) {
    AppendU64(&out, run_end);
  }
  u64 run_begin = 0;
  for (std::size_t run = 0; run < column.run_ends.size(); ++run) {
    bool visible = false;
    for (u64 row = run_begin; row < column.run_ends[run]; ++row) {
      visible = visible || !BitSet(redaction, row);
    }
    AppendStringPayload(&out, visible ? std::string_view(column.run_values[run])
                                      : std::string_view());
    run_begin = column.run_ends[run];
  }
  return out;
}

bool SliceColumnRows(const ResultBatchColumn& column,
                     u64 start_row,
                     u64 row_count,
                     ResultBatchColumn* out,
                     std::string* reason) {
  if (start_row > column.row_count || row_count > column.row_count - start_row) {
    *reason = "window_row_range_exceeds_column";
    return false;
  }
  out->name = column.name;
  out->layout = column.layout;
  out->row_count = row_count;
  out->validity_bitmap =
      SliceBitmapRange(column.validity_bitmap, start_row, row_count);
  out->redaction_bitmap =
      SliceBitmapRange(column.redaction_bitmap, start_row, row_count);
  switch (column.layout) {
    case ResultBatchLayoutKind::fixed_width: {
      out->fixed_width_bytes = column.fixed_width_bytes;
      const auto width = static_cast<std::size_t>(column.fixed_width_bytes);
      const auto begin = static_cast<std::size_t>(start_row) * width;
      const auto length = static_cast<std::size_t>(row_count) * width;
      if (begin > column.fixed_width_data.size() ||
          length > column.fixed_width_data.size() - begin) {
        *reason = "window_fixed_width_range_exceeds_payload";
        return false;
      }
      out->fixed_width_data.assign(
          column.fixed_width_data.begin() + static_cast<std::ptrdiff_t>(begin),
          column.fixed_width_data.begin() +
              static_cast<std::ptrdiff_t>(begin + length));
      break;
    }
    case ResultBatchLayoutKind::variable_width: {
      const auto start = static_cast<std::size_t>(column.offsets[start_row]);
      const auto end =
          static_cast<std::size_t>(column.offsets[start_row + row_count]);
      if (start > column.variable_data.size() ||
          end > column.variable_data.size() || end < start) {
        *reason = "window_variable_width_range_exceeds_payload";
        return false;
      }
      out->offsets.reserve(static_cast<std::size_t>(row_count + 1u));
      for (u64 row = 0; row <= row_count; ++row) {
        out->offsets.push_back(column.offsets[start_row + row] -
                               column.offsets[start_row]);
      }
      out->variable_data.assign(
          column.variable_data.begin() + static_cast<std::ptrdiff_t>(start),
          column.variable_data.begin() + static_cast<std::ptrdiff_t>(end));
      break;
    }
    case ResultBatchLayoutKind::dictionary: {
      const auto begin = static_cast<std::size_t>(start_row);
      const auto length = static_cast<std::size_t>(row_count);
      if (begin > column.dictionary_ids.size() ||
          length > column.dictionary_ids.size() - begin) {
        *reason = "window_dictionary_range_exceeds_payload";
        return false;
      }
      out->dictionary_ids.assign(
          column.dictionary_ids.begin() + static_cast<std::ptrdiff_t>(begin),
          column.dictionary_ids.begin() +
              static_cast<std::ptrdiff_t>(begin + length));
      out->dictionary_values = column.dictionary_values;
      break;
    }
    case ResultBatchLayoutKind::run_end: {
      u64 run_begin = 0;
      u64 window_rows = 0;
      for (std::size_t run = 0; run < column.run_ends.size(); ++run) {
        const u64 run_end = column.run_ends[run];
        const u64 overlap_begin = std::max(run_begin, start_row);
        const u64 overlap_end = std::min(run_end, start_row + row_count);
        if (overlap_begin < overlap_end) {
          window_rows += overlap_end - overlap_begin;
          out->run_ends.push_back(window_rows);
          out->run_values.push_back(column.run_values[run]);
        }
        run_begin = run_end;
      }
      if (row_count != 0 && out->run_ends.empty()) {
        *reason = "window_run_end_range_has_no_runs";
        return false;
      }
      break;
    }
    case ResultBatchLayoutKind::struct_view:
      for (const auto& child : column.children) {
        ResultBatchColumn sliced_child;
        if (!SliceColumnRows(child, start_row, row_count, &sliced_child,
                             reason)) {
          return false;
        }
        out->children.push_back(std::move(sliced_child));
      }
      break;
    case ResultBatchLayoutKind::list_view: {
      if (column.children.size() != 1u) {
        *reason = "window_list_view_requires_single_child";
        return false;
      }
      const u64 child_start = column.offsets[start_row];
      const u64 child_end = column.offsets[start_row + row_count];
      out->offsets.reserve(static_cast<std::size_t>(row_count + 1u));
      for (u64 row = 0; row <= row_count; ++row) {
        out->offsets.push_back(column.offsets[start_row + row] - child_start);
      }
      ResultBatchColumn child;
      if (!SliceColumnRows(column.children.front(), child_start,
                           child_end - child_start, &child, reason)) {
        return false;
      }
      out->children.push_back(std::move(child));
      break;
    }
    case ResultBatchLayoutKind::unknown:
      *reason = "window_unsupported_layout_kind";
      return false;
  }
  return true;
}

scratchbird::engine::executor::VectorizedResultBatchResult SliceBatchRows(
    const VectorizedResultBatch& batch,
    u64 start_row,
    u64 row_count,
    std::string* reason) {
  std::vector<ResultBatchColumn> columns;
  columns.reserve(batch.columns.size());
  for (const auto& column : batch.columns) {
    ResultBatchColumn sliced;
    if (!SliceColumnRows(column, start_row, row_count, &sliced, reason)) {
      scratchbird::engine::executor::VectorizedResultBatchResult result;
      result.status = RefusalStatus();
      result.fail_closed = true;
      result.refusal_reasons.push_back(*reason);
      return result;
    }
    columns.push_back(std::move(sliced));
  }
  return FinalizeVectorizedResultBatch(row_count, std::move(columns));
}

DirectBinaryResultFrameWindowResult WindowRefuse(
    std::string diagnostic_code,
    std::string reason,
    const DirectBinaryResultFrameWindowPolicy& policy) {
  DirectBinaryResultFrameWindowResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.frame_sequence = policy.frame_sequence;
  result.start_row = policy.start_row;
  result.refusal_reasons.push_back(reason);
  result.cancelled = reason == "server_side_cancellation_requested";
  result.backpressure = reason == "client_credit_unavailable" ||
                        reason == "client_credit_insufficient_for_frame";
  result.evidence.push_back("ORH_ACTUAL_FRAME_BYTE_ACCOUNTING");
  result.evidence.push_back("direct_binary_frame.windowed=true");
  result.evidence.push_back("direct_binary_frame.actual_byte_accounting=true");
  result.evidence.push_back("direct_binary_frame.row_boundary_stop=true");
  result.evidence.push_back("direct_binary_frame.full_materialization_required=false");
  result.evidence.push_back("direct_binary_frame.frame_sequence=" +
                            std::to_string(policy.frame_sequence));
  result.evidence.push_back("direct_binary_frame.actual_byte_probe_count=0");
  result.evidence.push_back("fallback_refusal_reason=" + reason);
  result.evidence.push_back("fail_closed=true");
  result.diagnostic =
      MakeDiagnostic(result.status, std::move(diagnostic_code),
                     "direct_binary_result_frame.window_refused",
                     std::move(reason));
  return result;
}

DirectBinaryResultFrameResult DescribeAndAppendColumn(
    const ResultBatchColumn& column,
    std::vector<std::uint8_t>* payload,
    DirectBinaryResultFrameColumnLayout* layout);

DirectBinaryResultFrameResult DescribeChildren(
    const ResultBatchColumn& column,
    std::vector<std::uint8_t>* payload,
    DirectBinaryResultFrameColumnLayout* layout) {
  for (const auto& child : column.children) {
    DirectBinaryResultFrameColumnLayout child_layout;
    auto child_result = DescribeAndAppendColumn(child, payload, &child_layout);
    if (!child_result.ok()) {
      return child_result;
    }
    layout->children.push_back(std::move(child_layout));
  }
  return DirectBinaryResultFrameResult{OkStatus(), false, {}, {}, {}, {}};
}

DirectBinaryResultFrameResult DescribeAndAppendColumn(
    const ResultBatchColumn& column,
    std::vector<std::uint8_t>* payload,
    DirectBinaryResultFrameColumnLayout* layout) {
  u64 expected_bitmap_size = 0;
  if (!BitmapByteCount(column.row_count, &expected_bitmap_size)) {
    return Refuse("bitmap_size_overflow");
  }
  const auto redaction = NormalizedRedactionBitmap(column);
  if (static_cast<u64>(column.validity_bitmap.size()) != expected_bitmap_size) {
    return Refuse("null_bitmap_size_mismatch");
  }
  if (static_cast<u64>(redaction.size()) != expected_bitmap_size) {
    return Refuse("redaction_bitmap_size_mismatch");
  }
  if (!BitmapPaddingValid(column.validity_bitmap, column.row_count)) {
    return Refuse("null_bitmap_padding_bits_set");
  }
  if (!BitmapPaddingValid(redaction, column.row_count)) {
    return Refuse("redaction_bitmap_padding_bits_set");
  }

  layout->name = column.name;
  layout->layout = column.layout;
  layout->row_count = column.row_count;
  layout->null_count = ClearBitCount(column.validity_bitmap, column.row_count);
  layout->redaction_count = SetBitCount(redaction, column.row_count);
  layout->fixed_width_bytes = column.fixed_width_bytes;
  layout->offset_count = column.offsets.size();
  layout->dictionary_value_count = column.dictionary_values.size();
  layout->run_count = column.run_ends.size();
  layout->child_count = column.children.size();
  layout->list_value_count =
      !column.offsets.empty() ? column.offsets.back() : 0;
  if (!AddRange(payload, column.validity_bitmap, &layout->validity_bitmap_offset,
                &layout->validity_bitmap_length) ||
      !AddRange(payload, redaction, &layout->redaction_bitmap_offset,
                &layout->redaction_bitmap_length)) {
    return Refuse("payload_size_overflow");
  }

  std::vector<std::uint8_t> column_payload;
  switch (column.layout) {
    case ResultBatchLayoutKind::fixed_width:
      column_payload = FixedWidthPayload(column, redaction);
      break;
    case ResultBatchLayoutKind::variable_width:
      column_payload = OffsetDataPayload(column, redaction);
      break;
    case ResultBatchLayoutKind::dictionary:
      column_payload = DictionaryPayload(column, redaction);
      break;
    case ResultBatchLayoutKind::run_end:
      column_payload = RunEndPayload(column, redaction);
      break;
    case ResultBatchLayoutKind::struct_view: {
      auto children = DescribeChildren(column, payload, layout);
      if (!children.ok()) {
        return children;
      }
      break;
    }
    case ResultBatchLayoutKind::list_view: {
      for (const auto offset : column.offsets) {
        AppendU64(&column_payload, offset);
      }
      auto children = DescribeChildren(column, payload, layout);
      if (!children.ok()) {
        return children;
      }
      break;
    }
    case ResultBatchLayoutKind::unknown:
      return Refuse("unsupported_layout_kind");
  }

  if (!AddRange(payload, column_payload, &layout->payload_offset,
                &layout->payload_length)) {
    return Refuse("payload_size_overflow");
  }
  return DirectBinaryResultFrameResult{OkStatus(), false, {}, {}, {}, {}};
}

void AppendColumnLayoutDescriptor(
    const DirectBinaryResultFrameColumnLayout& column,
    std::vector<std::uint8_t>* out) {
  AppendU16(out, static_cast<std::uint16_t>(column.name.size()));
  out->insert(out->end(), column.name.begin(), column.name.end());
  AppendU32(out, static_cast<u32>(column.layout));
  AppendU64(out, column.row_count);
  AppendU64(out, column.null_count);
  AppendU64(out, column.redaction_count);
  AppendU64(out, column.validity_bitmap_offset);
  AppendU64(out, column.validity_bitmap_length);
  AppendU64(out, column.redaction_bitmap_offset);
  AppendU64(out, column.redaction_bitmap_length);
  AppendU64(out, column.payload_offset);
  AppendU64(out, column.payload_length);
  AppendU64(out, column.fixed_width_bytes);
  AppendU64(out, column.offset_count);
  AppendU64(out, column.dictionary_value_count);
  AppendU64(out, column.run_count);
  AppendU64(out, static_cast<u64>(column.children.size()));
  AppendU64(out, column.list_value_count);
  for (const auto& child : column.children) {
    AppendColumnLayoutDescriptor(child, out);
  }
}

std::vector<std::uint8_t> SerializeDescriptor(
    const DirectBinaryResultFrameDescriptor& descriptor) {
  std::vector<std::uint8_t> out;
  AppendU32(&out, descriptor.version);
  AppendU64(&out, descriptor.row_count);
  AppendU64(&out, descriptor.column_count);
  AppendU64(&out, descriptor.payload_length);
  for (const auto& column : descriptor.columns) {
    AppendColumnLayoutDescriptor(column, &out);
  }
  return out;
}

void AppendHeader(std::vector<std::uint8_t>* out,
                  u32 version,
                  u64 descriptor_length,
                  u64 payload_length,
                  u64 row_count,
                  u64 column_count) {
  out->insert(out->end(), kMagic.begin(), kMagic.end());
  AppendU32(out, version);
  AppendU32(out, kHeaderSize);
  AppendU64(out, kHeaderSize);
  AppendU64(out, descriptor_length);
  AppendU64(out, kHeaderSize + descriptor_length);
  AppendU64(out, payload_length);
  AppendU64(out, row_count);
  AppendU64(out, column_count);
}

bool ReadName(const std::vector<std::uint8_t>& descriptor,
              std::size_t* offset,
              std::string* name) {
  std::uint16_t length = 0;
  if (!ReadU16(descriptor, offset, &length) || *offset > descriptor.size() ||
      descriptor.size() - *offset < length) {
    return false;
  }
  name->assign(reinterpret_cast<const char*>(descriptor.data() + *offset),
               length);
  *offset += length;
  return !name->empty();
}

bool KnownLayout(u32 layout) {
  return layout >= static_cast<u32>(ResultBatchLayoutKind::fixed_width) &&
         layout <= static_cast<u32>(ResultBatchLayoutKind::list_view);
}

bool RangeInside(u64 offset, u64 length, u64 total) {
  return offset <= total && length <= total - offset;
}

struct PayloadRange {
  u64 offset = 0;
  u64 length = 0;
};

bool AddPayloadRange(std::vector<PayloadRange>* ranges,
                     u64 offset,
                     u64 length,
                     u64 total) {
  if (!RangeInside(offset, length, total)) {
    return false;
  }
  if (length == 0) {
    return true;
  }
  ranges->push_back({offset, length});
  return true;
}

bool CollectPayloadRanges(const DirectBinaryResultFrameColumnLayout& column,
                          u64 payload_size,
                          std::vector<PayloadRange>* ranges) {
  if (!AddPayloadRange(ranges, column.validity_bitmap_offset,
                       column.validity_bitmap_length, payload_size) ||
      !AddPayloadRange(ranges, column.redaction_bitmap_offset,
                       column.redaction_bitmap_length, payload_size) ||
      !AddPayloadRange(ranges, column.payload_offset, column.payload_length,
                       payload_size)) {
    return false;
  }
  for (const auto& child : column.children) {
    if (!CollectPayloadRanges(child, payload_size, ranges)) {
      return false;
    }
  }
  return true;
}

bool PayloadRangesExactlyCoverFramePayload(u64 payload_size,
                                           std::vector<PayloadRange> ranges) {
  if (payload_size == 0) {
    return ranges.empty();
  }
  std::sort(ranges.begin(), ranges.end(),
            [](const PayloadRange& left, const PayloadRange& right) {
              if (left.offset != right.offset) {
                return left.offset < right.offset;
              }
              return left.length < right.length;
            });
  u64 expected_offset = 0;
  for (const auto& range : ranges) {
    if (range.offset != expected_offset ||
        range.length > payload_size - expected_offset) {
      return false;
    }
    expected_offset += range.length;
  }
  return expected_offset == payload_size;
}

bool ReadColumnLayoutDescriptor(
    const std::vector<std::uint8_t>& descriptor,
    std::size_t* offset,
    DirectBinaryResultFrameColumnLayout* column) {
  u32 layout = 0;
  if (!ReadName(descriptor, offset, &column->name) ||
      !ReadU32(descriptor, offset, &layout) || !KnownLayout(layout) ||
      !ReadU64(descriptor, offset, &column->row_count) ||
      !ReadU64(descriptor, offset, &column->null_count) ||
      !ReadU64(descriptor, offset, &column->redaction_count) ||
      !ReadU64(descriptor, offset, &column->validity_bitmap_offset) ||
      !ReadU64(descriptor, offset, &column->validity_bitmap_length) ||
      !ReadU64(descriptor, offset, &column->redaction_bitmap_offset) ||
      !ReadU64(descriptor, offset, &column->redaction_bitmap_length) ||
      !ReadU64(descriptor, offset, &column->payload_offset) ||
      !ReadU64(descriptor, offset, &column->payload_length) ||
      !ReadU64(descriptor, offset, &column->fixed_width_bytes) ||
      !ReadU64(descriptor, offset, &column->offset_count) ||
      !ReadU64(descriptor, offset, &column->dictionary_value_count) ||
      !ReadU64(descriptor, offset, &column->run_count) ||
      !ReadU64(descriptor, offset, &column->child_count) ||
      !ReadU64(descriptor, offset, &column->list_value_count)) {
    return false;
  }
  column->layout = static_cast<ResultBatchLayoutKind>(layout);
  for (u64 i = 0; i < column->child_count; ++i) {
    DirectBinaryResultFrameColumnLayout child;
    if (!ReadColumnLayoutDescriptor(descriptor, offset, &child)) {
      return false;
    }
    column->children.push_back(std::move(child));
  }
  return column->children.size() == column->child_count;
}

std::vector<std::uint8_t> Slice(const std::vector<std::uint8_t>& data,
                                u64 offset,
                                u64 length) {
  const auto begin = data.begin() + static_cast<std::ptrdiff_t>(offset);
  return {begin, begin + static_cast<std::ptrdiff_t>(length)};
}

bool ParseU64Array(const std::vector<std::uint8_t>& bytes,
                   std::size_t count,
                   std::vector<u64>* values) {
  if (count > bytes.size() / sizeof(u64)) {
    return false;
  }
  std::size_t offset = 0;
  for (std::size_t i = 0; i < count; ++i) {
    u64 value = 0;
    if (!ReadU64(bytes, &offset, &value)) {
      return false;
    }
    values->push_back(value);
  }
  return true;
}

DirectBinaryResultFrameResult ValidateColumnLayout(
    const DirectBinaryResultFrameColumnLayout& column,
    const std::vector<std::uint8_t>& payload,
    u64 parent_row_count,
    bool parent_is_list_child) {
  if (!parent_is_list_child && column.row_count != parent_row_count) {
    return Refuse("descriptor_mismatch:column_row_count_mismatch");
  }
  u64 bitmap_size = 0;
  if (!BitmapByteCount(column.row_count, &bitmap_size)) {
    return Refuse("descriptor_mismatch:bitmap_size_overflow");
  }
  if (column.validity_bitmap_length != bitmap_size) {
    return Refuse("null_bitmap_size_mismatch");
  }
  if (column.redaction_bitmap_length != bitmap_size) {
    return Refuse("redaction_bitmap_size_mismatch");
  }
  if (!RangeInside(column.validity_bitmap_offset, column.validity_bitmap_length,
                   payload.size()) ||
      !RangeInside(column.redaction_bitmap_offset, column.redaction_bitmap_length,
                   payload.size()) ||
      !RangeInside(column.payload_offset, column.payload_length,
                   payload.size())) {
    return Refuse("payload_size_mismatch");
  }
  const auto validity =
      Slice(payload, column.validity_bitmap_offset, column.validity_bitmap_length);
  const auto redaction =
      Slice(payload, column.redaction_bitmap_offset, column.redaction_bitmap_length);
  if (!BitmapPaddingValid(validity, column.row_count)) {
    return Refuse("null_bitmap_padding_bits_set");
  }
  if (!BitmapPaddingValid(redaction, column.row_count)) {
    return Refuse("redaction_bitmap_padding_bits_set");
  }
  if (ClearBitCount(validity, column.row_count) != column.null_count) {
    return Refuse("descriptor_mismatch:null_count_mismatch");
  }
  if (SetBitCount(redaction, column.row_count) != column.redaction_count) {
    return Refuse("descriptor_mismatch:redaction_count_mismatch");
  }

  const auto column_payload =
      Slice(payload, column.payload_offset, column.payload_length);
  switch (column.layout) {
    case ResultBatchLayoutKind::fixed_width:
      if (column.fixed_width_bytes == 0 ||
          column.row_count >
              std::numeric_limits<u64>::max() / column.fixed_width_bytes ||
          column.payload_length != column.row_count * column.fixed_width_bytes) {
        return Refuse("payload_size_mismatch");
      }
      break;
    case ResultBatchLayoutKind::variable_width: {
      if (column.offset_count != column.row_count + 1u ||
          column.offset_count > column_payload.size() / sizeof(u64)) {
        return Refuse("descriptor_mismatch:offset_count_mismatch");
      }
      std::vector<u64> offsets;
      if (!ParseU64Array(column_payload,
                         static_cast<std::size_t>(column.offset_count),
                         &offsets)) {
        return Refuse("malformed_frame");
      }
      for (std::size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] < offsets[i - 1u]) {
          return Refuse("descriptor_mismatch:offsets_not_monotonic");
        }
      }
      const auto data_bytes =
          column_payload.size() - (offsets.size() * sizeof(u64));
      if (offsets.empty() || offsets.front() != 0 ||
          offsets.back() != static_cast<u64>(data_bytes)) {
        return Refuse("payload_size_mismatch");
      }
      break;
    }
    case ResultBatchLayoutKind::dictionary: {
      if (column.payload_length < column.row_count * sizeof(u32)) {
        return Refuse("payload_size_mismatch");
      }
      std::size_t offset = 0;
      for (u64 row = 0; row < column.row_count; ++row) {
        u32 id = 0;
        if (!ReadU32(column_payload, &offset, &id) ||
            id >= column.dictionary_value_count) {
          return Refuse("descriptor_mismatch:dictionary_id_out_of_range");
        }
      }
      for (u64 i = 0; i < column.dictionary_value_count; ++i) {
        u64 length = 0;
        if (!ReadU64(column_payload, &offset, &length) ||
            offset > column_payload.size() ||
            length > column_payload.size() - offset) {
          return Refuse("payload_size_mismatch");
        }
        offset += static_cast<std::size_t>(length);
      }
      if (offset != column_payload.size()) {
        return Refuse("payload_size_mismatch");
      }
      break;
    }
    case ResultBatchLayoutKind::run_end: {
      if (column.run_count == 0 ||
          column.payload_length < column.run_count * sizeof(u64)) {
        return Refuse("payload_size_mismatch");
      }
      std::size_t offset = 0;
      u64 previous = 0;
      for (u64 i = 0; i < column.run_count; ++i) {
        u64 run_end = 0;
        if (!ReadU64(column_payload, &offset, &run_end) ||
            run_end <= previous || run_end > column.row_count) {
          return Refuse("descriptor_mismatch:run_end_malformed");
        }
        previous = run_end;
      }
      if (previous != column.row_count) {
        return Refuse("descriptor_mismatch:run_end_terminal_mismatch");
      }
      for (u64 i = 0; i < column.run_count; ++i) {
        u64 length = 0;
        if (!ReadU64(column_payload, &offset, &length) ||
            offset > column_payload.size() ||
            length > column_payload.size() - offset) {
          return Refuse("payload_size_mismatch");
        }
        offset += static_cast<std::size_t>(length);
      }
      if (offset != column_payload.size()) {
        return Refuse("payload_size_mismatch");
      }
      break;
    }
    case ResultBatchLayoutKind::struct_view:
      if (column.children.empty() || column.payload_length != 0) {
        return Refuse("descriptor_mismatch:struct_layout_mismatch");
      }
      break;
    case ResultBatchLayoutKind::list_view: {
      if (column.children.size() != 1u ||
          column.offset_count != column.row_count + 1u ||
          column.payload_length != column.offset_count * sizeof(u64)) {
        return Refuse("descriptor_mismatch:list_layout_mismatch");
      }
      std::vector<u64> offsets;
      if (!ParseU64Array(column_payload,
                         static_cast<std::size_t>(column.offset_count),
                         &offsets) ||
          offsets.empty() || offsets.front() != 0) {
        return Refuse("descriptor_mismatch:list_offsets_malformed");
      }
      for (std::size_t i = 1; i < offsets.size(); ++i) {
        if (offsets[i] < offsets[i - 1u]) {
          return Refuse("descriptor_mismatch:list_offsets_not_monotonic");
        }
      }
      if (offsets.back() != column.children.front().row_count ||
          offsets.back() != column.list_value_count) {
        return Refuse("descriptor_mismatch:list_terminal_offset_mismatch");
      }
      break;
    }
    case ResultBatchLayoutKind::unknown:
      return Refuse("unsupported_layout_kind");
  }

  for (const auto& child : column.children) {
    auto child_result = ValidateColumnLayout(
        child, payload,
        column.layout == ResultBatchLayoutKind::list_view ? child.row_count
                                                          : column.row_count,
        column.layout == ResultBatchLayoutKind::list_view);
    if (!child_result.ok()) {
      return child_result;
    }
  }
  return DirectBinaryResultFrameResult{OkStatus(), false, {}, {}, {}, {}};
}

}  // namespace

DirectBinaryResultFrameResult BuildDirectBinaryResultFrame(
    const VectorizedResultBatch& batch) {
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      DirectBinaryFrameCompatibility());
  if (!compatibility.ok) {
    auto result =
        Refuse("runtime_compatibility:" + compatibility.diagnostic_code);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }
  auto transfer = BuildResultBatchTransferDescriptor(batch);
  if (!transfer.ok()) {
    const auto reason = transfer.refusal_reasons.empty()
                            ? std::string("invalid_transfer_descriptor")
                            : transfer.refusal_reasons.front();
    return Refuse("batch_transfer_descriptor_failed:" + reason);
  }
  auto revalidated = FinalizeVectorizedResultBatch(batch.row_count, batch.columns);
  if (!revalidated.ok()) {
    const auto reason = revalidated.refusal_reasons.empty()
                            ? std::string("unknown")
                            : revalidated.refusal_reasons.front();
    return Refuse("corrupt_batch:" + reason);
  }

  std::vector<std::uint8_t> payload;
  DirectBinaryResultFrameDescriptor descriptor;
  descriptor.version = kDirectBinaryResultFrameVersion;
  descriptor.row_count = revalidated.batch.row_count;
  descriptor.column_count = revalidated.batch.columns.size();

  for (const auto& column : revalidated.batch.columns) {
    DirectBinaryResultFrameColumnLayout layout;
    auto described = DescribeAndAppendColumn(column, &payload, &layout);
    if (!described.ok()) {
      return described;
    }
    descriptor.columns.push_back(std::move(layout));
  }
  descriptor.payload_length = payload.size();

  const auto descriptor_bytes = SerializeDescriptor(descriptor);
  std::vector<std::uint8_t> frame_bytes;
  AppendHeader(&frame_bytes, descriptor.version, descriptor_bytes.size(),
               payload.size(), descriptor.row_count, descriptor.column_count);
  AppendBytes(&frame_bytes, descriptor_bytes);
  AppendBytes(&frame_bytes, payload);

  DirectBinaryResultFrameResult result;
  result.status = OkStatus();
  result.frame.descriptor = std::move(descriptor);
  result.frame.bytes = std::move(frame_bytes);
  result.evidence.push_back("direct_binary_frame.version=" +
                            std::to_string(kDirectBinaryResultFrameVersion));
  result.evidence.push_back("direct_binary_frame.data_transport_only=true");
  result.evidence.push_back("direct_binary_frame.row_order_preserved=true");
  result.evidence.push_back("direct_binary_frame.row_object_formatting=false");
  result.evidence.push_back("direct_binary_frame.finality_authority=false");
  result.evidence.push_back("direct_binary_frame.visibility_authority=false");
  result.evidence.push_back("direct_binary_frame.parser_authority=false");
  result.evidence.push_back("direct_binary_frame.client_authority=false");
  result.evidence.push_back("direct_binary_frame.provider_authority=false");
  result.evidence.push_back("direct_binary_frame.security_authority=false");
  result.evidence.push_back("direct_binary_frame.mga_authority=false");
  result.evidence.push_back("direct_binary_frame.wal_authority=false");
  AppendCompatibilityEvidence(&result, compatibility);
  result.evidence.push_back(
      CountEvidence("direct_binary_frame.row_count", result.frame.descriptor.row_count));
  result.evidence.push_back(CountEvidence("direct_binary_frame.column_count",
                                          result.frame.descriptor.column_count));
  result.evidence.push_back(CountEvidence("direct_binary_frame.payload_length",
                                          result.frame.descriptor.payload_length));
  for (const auto& column : result.frame.descriptor.columns) {
    result.evidence.push_back("direct_binary_frame.column.layout_kind=" +
                              std::string(ResultBatchLayoutKindName(column.layout)));
    result.evidence.push_back(CountEvidence(
        "direct_binary_frame.column.redaction_count", column.redaction_count));
  }
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_DIRECT_BINARY_RESULT_FRAME.OK",
                     "direct_binary_result_frame.ok",
                     "direct_binary_result_frame_ready");
  return result;
}

DirectBinaryResultFrameWindowResult BuildDirectBinaryResultFrameWindow(
    const VectorizedResultBatch& batch,
    const DirectBinaryResultFrameWindowPolicy& policy) {
  if (policy.cancellation_requested) {
    DirectBinaryResultFrameWindowResult result;
    result.status = OkStatus();
    result.frame_sequence = policy.frame_sequence;
    result.start_row = policy.start_row;
    result.cancelled = true;
    result.ordering_preserved = policy.require_ordered_output;
    result.evidence.push_back("ORH_ACTUAL_FRAME_BYTE_ACCOUNTING");
    result.evidence.push_back("direct_binary_frame.windowed=true");
    result.evidence.push_back("direct_binary_frame.actual_byte_accounting=true");
    result.evidence.push_back("direct_binary_frame.row_boundary_stop=true");
    result.evidence.push_back("direct_binary_frame.full_materialization_required=false");
    result.evidence.push_back("direct_binary_frame.frame_sequence=" +
                              std::to_string(policy.frame_sequence));
    result.evidence.push_back("direct_binary_frame.cancellation_observed_before_build=true");
    result.evidence.push_back("direct_binary_frame.cancelled=true");
    result.diagnostic =
        MakeDiagnostic(result.status, "SB_DIRECT_BINARY_RESULT_FRAME.CANCELLED",
                       "direct_binary_result_frame.window_cancelled",
                       "server_side_cancellation_requested");
    return result;
  }
  if (policy.client_credit_rows == 0 || policy.client_credit_bytes == 0) {
    return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.BACKPRESSURE",
                        "client_credit_unavailable", policy);
  }
  if (policy.requested_rows == 0 || policy.max_rows == 0 ||
      policy.max_frame_bytes == 0) {
    return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.INVALID_WINDOW_POLICY",
                        "nonzero_requested_rows_max_rows_and_frame_bytes_required",
                        policy);
  }
  auto revalidated = FinalizeVectorizedResultBatch(batch.row_count, batch.columns);
  if (!revalidated.ok()) {
    const auto reason = revalidated.refusal_reasons.empty()
                            ? std::string("unknown")
                            : revalidated.refusal_reasons.front();
    return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.INVALID_BATCH",
                        "batch_revalidation_failed:" + reason, policy);
  }
  const auto& checked_batch = revalidated.batch;
  if (policy.start_row > checked_batch.row_count) {
    return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.INVALID_WINDOW_POLICY",
                        "start_row_exceeds_batch_rows", policy);
  }
  const u64 remaining_rows = checked_batch.row_count - policy.start_row;
  const u64 bounded_rows = std::min({policy.requested_rows,
                                     policy.max_rows,
                                     policy.client_credit_rows,
                                     remaining_rows});
  if (bounded_rows == 0) {
    return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.BACKPRESSURE",
                        "client_credit_unavailable", policy);
  }

  const u64 byte_limit =
      std::min(policy.max_frame_bytes, policy.client_credit_bytes);
  DirectBinaryResultFrameResult accepted_frame;
  DirectBinaryResultFrameResult one_row_frame;
  bool one_row_probed = false;
  u64 accepted_rows = 0;
  u64 probe_count = 0;
  u64 low = 1;
  u64 high = bounded_rows;
  while (low <= high) {
    const u64 rows = low + ((high - low) / 2u);
    ++probe_count;
    std::string slice_reason;
    auto sliced = SliceBatchRows(checked_batch, policy.start_row, rows,
                                 &slice_reason);
    if (!sliced.ok()) {
      return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.WINDOW_SLICE_FAILED",
                          slice_reason.empty() ? "window_slice_failed"
                                               : slice_reason,
                          policy);
    }
    auto frame = BuildDirectBinaryResultFrame(sliced.batch);
    if (!frame.ok()) {
      const auto reason = frame.refusal_reasons.empty()
                              ? frame.diagnostic.diagnostic_code
                              : frame.refusal_reasons.front();
      return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.WINDOW_BUILD_FAILED",
                          "window_frame_build_failed:" + reason, policy);
    }
    const auto actual_bytes = static_cast<u64>(frame.frame.bytes.size());
    if (rows == 1) {
      one_row_probed = true;
      one_row_frame = frame;
    }
    if (actual_bytes <= byte_limit) {
      accepted_rows = rows;
      accepted_frame = std::move(frame);
      low = rows + 1u;
    } else {
      if (rows == 1) {
        break;
      }
      high = rows - 1u;
    }
  }

  if (accepted_rows == 0 || !accepted_frame.ok()) {
    if (!one_row_probed) {
      ++probe_count;
      std::string slice_reason;
      auto sliced = SliceBatchRows(checked_batch, policy.start_row, 1,
                                   &slice_reason);
      if (!sliced.ok()) {
        return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.WINDOW_SLICE_FAILED",
                            slice_reason.empty() ? "window_slice_failed"
                                                 : slice_reason,
                            policy);
      }
      one_row_frame = BuildDirectBinaryResultFrame(sliced.batch);
      if (!one_row_frame.ok()) {
        const auto reason = one_row_frame.refusal_reasons.empty()
                                ? one_row_frame.diagnostic.diagnostic_code
                                : one_row_frame.refusal_reasons.front();
        return WindowRefuse("SB_DIRECT_BINARY_RESULT_FRAME.WINDOW_BUILD_FAILED",
                            "window_frame_build_failed:" + reason, policy);
      }
    }
    auto refused = WindowRefuse(
        "SB_DIRECT_BINARY_RESULT_FRAME.ROW_EXCEEDS_FRAME_OR_CREDIT",
        static_cast<u64>(one_row_frame.frame.bytes.size()) >
                policy.client_credit_bytes
            ? "client_credit_insufficient_for_frame"
            : "single_row_exceeds_frame_byte_limit",
        policy);
    refused.actual_frame_bytes =
        static_cast<u64>(one_row_frame.frame.bytes.size());
    refused.evidence.push_back("direct_binary_frame.actual_byte_probe_count=" +
                               std::to_string(probe_count));
    refused.evidence.push_back("direct_binary_frame.one_row_actual_bytes=" +
                               std::to_string(refused.actual_frame_bytes));
    return refused;
  }

  DirectBinaryResultFrameWindowResult result;
  result.status = OkStatus();
  result.frame = std::move(accepted_frame.frame);
  result.frame_sequence = policy.frame_sequence;
  result.start_row = policy.start_row;
  result.row_count = accepted_rows;
  result.next_start_row = policy.start_row + accepted_rows;
  result.actual_frame_bytes = static_cast<u64>(result.frame.bytes.size());
  result.continuation_required = result.next_start_row < checked_batch.row_count;
  result.ordering_preserved = policy.require_ordered_output;
  result.evidence = std::move(accepted_frame.evidence);
  result.evidence.push_back("ORH_ACTUAL_FRAME_BYTE_ACCOUNTING");
  result.evidence.push_back("direct_binary_frame.windowed=true");
  result.evidence.push_back("direct_binary_frame.actual_byte_accounting=true");
  result.evidence.push_back("direct_binary_frame.row_boundary_stop=true");
  result.evidence.push_back("direct_binary_frame.full_materialization_required=false");
  result.evidence.push_back("direct_binary_frame.frame_sequence=" +
                            std::to_string(result.frame_sequence));
  result.evidence.push_back("direct_binary_frame.window_start_row=" +
                            std::to_string(result.start_row));
  result.evidence.push_back("direct_binary_frame.window_row_count=" +
                            std::to_string(result.row_count));
  result.evidence.push_back("direct_binary_frame.window_next_start_row=" +
                            std::to_string(result.next_start_row));
  result.evidence.push_back("direct_binary_frame.actual_frame_bytes=" +
                            std::to_string(result.actual_frame_bytes));
  result.evidence.push_back("direct_binary_frame.actual_byte_probe_count=" +
                            std::to_string(probe_count));
  result.evidence.push_back("direct_binary_frame.max_frame_bytes=" +
                            std::to_string(policy.max_frame_bytes));
  result.evidence.push_back("direct_binary_frame.client_credit_bytes=" +
                            std::to_string(policy.client_credit_bytes));
  result.evidence.push_back("direct_binary_frame.client_credit_rows=" +
                            std::to_string(policy.client_credit_rows));
  result.evidence.push_back("direct_binary_frame.continuation_required=" +
                            std::string(result.continuation_required ? "true"
                                                                     : "false"));
  result.evidence.push_back("direct_binary_frame.ordering_preserved=" +
                            std::string(result.ordering_preserved ? "true"
                                                                  : "false"));
  result.diagnostic =
      MakeDiagnostic(result.status,
                     "SB_DIRECT_BINARY_RESULT_FRAME.WINDOW_READY",
                     "direct_binary_result_frame.window_ready",
                     "direct_binary_result_frame_window_ready");
  return result;
}

DirectBinaryResultFrameResult ParseDirectBinaryResultFrame(
    const std::vector<std::uint8_t>& bytes) {
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      DirectBinaryFrameCompatibility());
  if (!compatibility.ok) {
    auto result =
        Refuse("runtime_compatibility:" + compatibility.diagnostic_code);
    AppendCompatibilityEvidence(&result, compatibility);
    return result;
  }
  if (bytes.size() < kHeaderSize) {
    return Refuse("malformed_truncated_frame");
  }
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
    return Refuse("malformed_frame_magic");
  }

  std::size_t offset = kMagic.size();
  u32 version = 0;
  u32 header_size = 0;
  u64 descriptor_offset = 0;
  u64 descriptor_length = 0;
  u64 payload_offset = 0;
  u64 payload_length = 0;
  u64 row_count = 0;
  u64 column_count = 0;
  if (!ReadU32(bytes, &offset, &version) ||
      !ReadU32(bytes, &offset, &header_size) ||
      !ReadU64(bytes, &offset, &descriptor_offset) ||
      !ReadU64(bytes, &offset, &descriptor_length) ||
      !ReadU64(bytes, &offset, &payload_offset) ||
      !ReadU64(bytes, &offset, &payload_length) ||
      !ReadU64(bytes, &offset, &row_count) ||
      !ReadU64(bytes, &offset, &column_count)) {
    return Refuse("malformed_truncated_frame");
  }
  if (version != kDirectBinaryResultFrameVersion) {
    return Refuse("unsupported_frame_version");
  }
  if (header_size != kHeaderSize || descriptor_offset != kHeaderSize ||
      !RangeInside(descriptor_offset, descriptor_length, bytes.size()) ||
      !RangeInside(payload_offset, payload_length, bytes.size()) ||
      payload_offset != descriptor_offset + descriptor_length ||
      bytes.size() != payload_offset + payload_length) {
    return Refuse("payload_size_mismatch");
  }

  const auto descriptor_bytes = Slice(bytes, descriptor_offset, descriptor_length);
  const auto payload = Slice(bytes, payload_offset, payload_length);
  std::size_t descriptor_read = 0;
  DirectBinaryResultFrameDescriptor descriptor;
  u32 descriptor_version = 0;
  if (!ReadU32(descriptor_bytes, &descriptor_read, &descriptor_version) ||
      !ReadU64(descriptor_bytes, &descriptor_read, &descriptor.row_count) ||
      !ReadU64(descriptor_bytes, &descriptor_read, &descriptor.column_count) ||
      !ReadU64(descriptor_bytes, &descriptor_read, &descriptor.payload_length)) {
    return Refuse("malformed_truncated_frame");
  }
  descriptor.version = descriptor_version;
  if (descriptor.version != version || descriptor.row_count != row_count ||
      descriptor.column_count != column_count ||
      descriptor.payload_length != payload_length) {
    return Refuse("descriptor_mismatch:header_descriptor_mismatch");
  }
  for (u64 i = 0; i < descriptor.column_count; ++i) {
    DirectBinaryResultFrameColumnLayout column;
    if (!ReadColumnLayoutDescriptor(descriptor_bytes, &descriptor_read, &column)) {
      return Refuse("descriptor_mismatch:malformed_column_layout");
    }
    descriptor.columns.push_back(std::move(column));
  }
  if (descriptor_read != descriptor_bytes.size()) {
    return Refuse("descriptor_mismatch:trailing_descriptor_bytes");
  }
  if (descriptor.columns.size() != descriptor.column_count) {
    return Refuse("descriptor_mismatch:column_count_mismatch");
  }
  for (const auto& column : descriptor.columns) {
    auto validated =
        ValidateColumnLayout(column, payload, descriptor.row_count, false);
    if (!validated.ok()) {
      return validated;
    }
  }
  std::vector<PayloadRange> payload_ranges;
  for (const auto& column : descriptor.columns) {
    if (!CollectPayloadRanges(column, payload.size(), &payload_ranges)) {
      return Refuse("payload_size_mismatch");
    }
  }
  if (!PayloadRangesExactlyCoverFramePayload(payload.size(), payload_ranges)) {
    return Refuse("payload_size_mismatch");
  }

  DirectBinaryResultFrameResult result;
  result.status = OkStatus();
  result.frame.descriptor = std::move(descriptor);
  result.frame.bytes = bytes;
  result.evidence.push_back("direct_binary_frame.version=" +
                            std::to_string(kDirectBinaryResultFrameVersion));
  result.evidence.push_back("direct_binary_frame.parsed=true");
  result.evidence.push_back("direct_binary_frame.data_transport_only=true");
  result.evidence.push_back("direct_binary_frame.row_order_preserved=true");
  result.evidence.push_back("direct_binary_frame.row_object_formatting=false");
  result.evidence.push_back("direct_binary_frame.finality_authority=false");
  result.evidence.push_back("direct_binary_frame.visibility_authority=false");
  result.evidence.push_back("direct_binary_frame.parser_authority=false");
  result.evidence.push_back("direct_binary_frame.client_authority=false");
  result.evidence.push_back("direct_binary_frame.provider_authority=false");
  result.evidence.push_back("direct_binary_frame.security_authority=false");
  result.evidence.push_back("direct_binary_frame.mga_authority=false");
  result.evidence.push_back("direct_binary_frame.wal_authority=false");
  AppendCompatibilityEvidence(&result, compatibility);
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_DIRECT_BINARY_RESULT_FRAME.OK",
                     "direct_binary_result_frame.ok",
                     "direct_binary_result_frame_parsed");
  return result;
}

DirectBinaryResultFrameResult ValidateDirectBinaryResultFrameRuntimeCompatibility(
    const platform::RuntimeCompatibilityDescriptor& descriptor) {
  const auto compatibility = platform::NegotiateRuntimeCompatibility(
      DirectBinaryFrameCompatibility(descriptor));
  if (compatibility.ok) {
    DirectBinaryResultFrameResult result;
    result.status = OkStatus();
    AppendCompatibilityEvidence(&result, compatibility);
    result.diagnostic =
        MakeDiagnostic(result.status, "SB_DIRECT_BINARY_RESULT_FRAME.OK",
                       "direct_binary_result_frame.runtime_compatible",
                       "runtime_compatibility_admitted");
    return result;
  }
  auto result =
      Refuse("runtime_compatibility:" + compatibility.diagnostic_code);
  AppendCompatibilityEvidence(&result, compatibility);
  return result;
}

DirectBinaryResultFrameResult ValidateDirectBinaryResultFrameEvidenceClaims(
    const std::vector<std::string>& evidence_claims) {
  for (const auto& item : evidence_claims) {
    for (const auto forbidden :
         {"finality_authority=true", "visibility_authority=true",
          "parser_authority=true", "parser_execution_authority=true",
          "client_authority=true", "provider_authority=true",
          "security_authority=true", "mga_authority=true",
          "wal_authority=true", "row_object_formatting=true",
          "row_object_fallback=true"}) {
      if (item.find(forbidden) != std::string::npos) {
        std::string key = forbidden;
        const auto suffix = key.find("=true");
        if (suffix != std::string::npos) {
          key.erase(suffix);
        }
        return Refuse("forbidden_authority_or_row_object_claim:" + key);
      }
    }
  }
  DirectBinaryResultFrameResult result;
  result.status = OkStatus();
  result.evidence.push_back("direct_binary_frame.evidence_claims_valid=true");
  result.evidence.push_back("direct_binary_frame.data_transport_only=true");
  result.evidence.push_back("direct_binary_frame.row_object_formatting=false");
  result.diagnostic =
      MakeDiagnostic(result.status, "SB_DIRECT_BINARY_RESULT_FRAME.OK",
                     "direct_binary_result_frame.ok",
                     "direct_binary_result_frame_evidence_claims_valid");
  return result;
}

const char* DirectBinaryResultFrameFieldVersion() {
  return "direct_binary_frame.version";
}

const char* DirectBinaryResultFrameCompressionPolicyFamilyName() {
  return index::CompressionFamilyName(
      index::CompressionFamily::kBinaryResultFrame);
}

std::vector<std::string> DirectBinaryResultFrameCompressionPolicyEvidence() {
  return {
      std::string(index::kCompressionPolicyByFamilySearchKey),
      std::string("compression_family=") +
          DirectBinaryResultFrameCompressionPolicyFamilyName(),
      "compression_adapter=direct_binary_result_frame",
      "compression_metadata_only=true",
      "direct_binary_frame.finality_authority=false",
      "direct_binary_frame.visibility_authority=false",
      "direct_binary_frame.parser_authority=false",
  };
}

scratchbird::engine::optimizer::BenchmarkResultFastPathEvidence
BuildBenchmarkResultFastPathEvidenceFromWireResult(
    const BinaryResultFastPathObservation& observation) {
  namespace opt = scratchbird::engine::optimizer;

  opt::BenchmarkResultFastPathEvidence evidence;
  evidence.route_kind = observation.route_kind;
  evidence.statement_family = observation.statement_family;
  evidence.benchmark_clean_candidate = observation.benchmark_clean_candidate;
  evidence.disabled_or_fallback = observation.disabled_or_fallback;
  evidence.disabled_reason = observation.disabled_reason;
  evidence.parser_or_cache_executes_sql =
      observation.parser_or_cache_executes_sql;
  evidence.parser_or_cache_owns_transaction_finality =
      observation.parser_or_cache_owns_transaction_finality;
  evidence.transaction_authority = observation.transaction_authority;
  evidence.runtime_evidence = observation.runtime_evidence;
  evidence.result_contract_hash = observation.result_contract_hash;
  evidence.diagnostic_code =
      observation.diagnostic_code.empty()
          ? "SB_ORH_BINARY_RESULT_FAST_PATH.WIRE_ROUTE_OBSERVED"
          : observation.diagnostic_code;

  const auto* frame = observation.frame_result;
  const bool frame_ok = frame != nullptr && frame->ok();
  const auto& policy = observation.instrumentation_policy;
  evidence.binary_or_equivalent_frame_selected =
      frame_ok && !observation.disabled_or_fallback;
  if (evidence.binary_or_equivalent_frame_selected) {
    evidence.frame_kind = DirectBinaryResultFrameCompressionPolicyFamilyName();
    evidence.frame_version = std::to_string(frame->frame.descriptor.version);
  }
  evidence.equivalent_result_materialization =
      frame_ok && observation.equivalent_result_materialization;
  evidence.exact_diagnostics_preserved =
      frame != nullptr && !frame->diagnostic.diagnostic_code.empty();
  evidence.nonessential_evidence_suppressed_during_timing =
      policy.benchmark_clean_eligible && !policy.route_phase_timing_enabled &&
      !policy.append_page_index_timing_enabled &&
      !policy.agent_cpu_thread_counters_enabled &&
      !policy.support_bundle_summary_enabled &&
      !policy.hot_path_string_formatting_enabled;
  evidence.support_evidence_available_outside_timed_path =
      observation.support_evidence_available_outside_timed_path;
  evidence.timed_path_text_rendering_suppressed =
      !policy.hot_path_string_formatting_enabled;

  if (evidence.disabled_reason.empty()) {
    if (observation.disabled_or_fallback) {
      evidence.disabled_reason = "binary result fast path disabled by route";
    } else if (!frame_ok) {
      evidence.disabled_reason =
          frame == nullptr ? "binary result frame was not built"
                           : frame->diagnostic.diagnostic_code;
    } else if (!policy.benchmark_clean_eligible) {
      evidence.disabled_reason =
          "instrumentation policy is not benchmark-clean eligible";
    } else if (!observation.runtime_evidence.runtime_consumed) {
      evidence.disabled_reason =
          "route evidence did not prove runtime consumption";
    }
  }
  return evidence;
}

}  // namespace scratchbird::wire
